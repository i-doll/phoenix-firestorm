/**
 * @file fsgpudrivenmaterials.cpp
 * @brief GPU-driven draws for the deferred legacy material passes (FSPerfGPUDrivenMaterials)
 *
 * $LicenseInfo:firstyear=2026&license=viewerlgpl$
 * Phoenix Firestorm Viewer Source Code
 * Copyright (C) 2026, The Phoenix Firestorm Project, Inc.
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation;
 * version 2.1 of the License only.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301  USA
 *
 * The Phoenix Firestorm Project, Inc., 1831 Oakwood Drive, Fairmont, Minnesota 56031-3225 USA
 * http://www.firestormviewer.org
 * $/LicenseInfo$
 */

// The stock legacy material passes (LLDrawPoolMaterials::renderDeferred: PASS_MATERIAL, PASS_SPECMAP,
// PASS_NORMMAP, PASS_NORMSPEC and their alpha mask and emissive variants, the surfaces with a legacy normal or
// specular map, or environment shine on a material) cost, for every batch: up to four uniforms (specular
// colour, environment intensity, minimum alpha, fullbright), up to three texture binds (diffuse, normal and
// specular map), a model matrix (modelview, modelview projection and normal matrix uniforms), a texture matrix
// load and reset when the texture is animated, a vertex buffer bind and a glDrawRangeElements.
//
// With FSPerfGPUDrivenMaterials on, the static batches come out of a vertex arena instead (fsgddarena.h: a GPU
// side copy of the position, normal, texcoord 0, colour and, where the buffer has them, tangent, texcoord 1
// and texcoord 2 regions, and the 16 bit indices of their vertex buffers), and each batch is one 20 byte
// indirect draw command plus a record: which model matrices (in a storage buffer, the matrices the stock path
// would have computed, computed the same way), which texture matrix (identity, or the batch's animation), and
// which material (a second storage buffer: the specular colour, environment intensity, minimum alpha and
// fullbright value the stock loop would set, and the bindless handles of the textures it would bind).
// materialV.glsl built with FS_GDD_MATERIAL reads them and hands the material to materialF.glsl, which reads
// it in place of its uniforms and samplers; everything else in the shaders is the stock code.
//
// One GPU-driven program per stock permutation, so the passes stay as they are: the batches of each pass are
// drawn in their stock order, one glMultiDrawElementsIndirect per run of batches from the arena. A batch that
// can't come from it (rigged, empty, a texture missing or without a handle (not loaded, one or two channels,
// options pending, media), 32 bit indices, buffer mapped or lacking an attribute the program reads, arena
// full or the copy budget used up) is drawn by the stock code in its place. Rigged and blended passes stay
// entirely stock.
//
// FSPerfGPUDrivenPBRGroupRuns gives that order up here too: the stock fallbacks spread through a pass cut it
// into many runs, and every switch between a run and a stock batch costs a program bind and the inputs set up
// again. A run has no state of its own (the material of every batch is in its record), so grouped, all batches
// from the arena go out as one multi-draw and the stock fallbacks after them, in their stock order. The depth
// test decides what a G-buffer pass leaves, so only where two surfaces have exactly the same depth can a
// different one win than in the stock order.
//
// The texture handles are those of FSPerfGPUDrivenPBRBindless (fsgpudrivenpbr.cpp), looked up once per
// texture per pass through the cache FSPerfGPUDrivenLegacy keeps (fsGDDPassTextureHandle); this needs bindless
// textures, which are only used on NVIDIA.
//
// FSPerfGPUDrivenMaterialsVerify: as FSPerfGPUDrivenLegacyVerify, after the pass the batches it drew from the
// arena are drawn again the stock way with colour and depth writes off and depth test GL_LESS under a
// GL_SAMPLES_PASSED query; it also reads back a couple of arena entries per pass, checks for GL errors and
// logs statistics every 10 seconds (FSPerfGPUDrivenMaterialsStats logs only the statistics).

#include "llviewerprecompiledheaders.h"

#include "fsgpudrivenmaterials.h"

#include "fsgddarena.h"
#include "fsgpudrivenlegacy.h"
#include "fsgpudrivenpbr.h"

#include "lldrawpool.h"
#include "llframetimer.h"
#include "llgl.h"
#include "llglslshader.h"
#include "llglstates.h"
#include "llmaterial.h"
#include "llrender.h"
#include "llspatialpartition.h"
#include "llvertexbuffer.h"
#include "llviewercontrol.h"
#include "llviewershadermgr.h"
#include "llviewertexture.h"
#include "pipeline.h"

#include <cstring>
#include <sstream>
#include <unordered_map>
#include <vector>

namespace
{
    // the other passes use the same bindings; each pass binds its buffers and clears them again
    constexpr GLuint FS_GDD_MAT_MATRIX_BINDING = 5;     // shader storage binding of FSGDDMaterialMatrices
    constexpr GLuint FS_GDD_MAT_MATERIAL_BINDING = 6;   // and of FSGDDMaterialRecords
    constexpr U32 FS_GDD_MAT_PROGRAMS = LLMaterial::SHADER_COUNT; // the non rigged permutations
    // the attributes the arena can feed; a program wanting any other is not used
    constexpr U32 FS_GDD_MAT_ATTRIBS = LLVertexBuffer::MAP_VERTEX | LLVertexBuffer::MAP_NORMAL | LLVertexBuffer::MAP_TEXCOORD0 |
                                       LLVertexBuffer::MAP_COLOR | LLVertexBuffer::MAP_TANGENT | LLVertexBuffer::MAP_TEXCOORD1 |
                                       LLVertexBuffer::MAP_TEXCOORD2;

    // everything materialV.glsl reads; tangents and the second and third texture coordinates only where the
    // buffer has them (a program that reads them turns a buffer without them down). An eighth of
    // FSPerfGPUDrivenMaterialsArenaMB for indices.
    FSGDDArena sArena("FSPerfGPUDrivenMaterials arena",
                      { LLVertexBuffer::TYPE_VERTEX, LLVertexBuffer::TYPE_NORMAL, LLVertexBuffer::TYPE_TEXCOORD0,
                        LLVertexBuffer::TYPE_COLOR, LLVertexBuffer::TYPE_TANGENT, LLVertexBuffer::TYPE_TEXCOORD1,
                        LLVertexBuffer::TYPE_TEXCOORD2 },
                      "FSPerfGPUDrivenMaterialsArenaMB", 256, 8,
                      LLVertexBuffer::MAP_TANGENT | LLVertexBuffer::MAP_TEXCOORD1 | LLVertexBuffer::MAP_TEXCOORD2);

    struct DrawCommand  // DrawElementsIndirectCommand
    {
        GLuint  mCount;
        GLuint  mInstanceCount;
        GLuint  mFirstIndex;
        GLint   mBaseVertex;
        GLuint  mBaseInstance;
    };

    struct Record       // fs_gdd_draw
    {
        U32 mMatrix;    // modelview; modelview projection and normal matrix follow
        U32 mTexMatrix;
        U32 mMaterial;
        U32 mPad;
    };

    struct Material     // FSGDDLegacyMaterial, std430
    {
        F32 mSpecular[4];   // specular_color
        F32 mParams[4];     // env_intensity, minimum_alpha, emissive_brightness, 0
        U32 mTex[8];        // handles (low, high): diffuseMap, bumpMap, specularMap, unused
    };
    static_assert(sizeof(Material) == 64, "FSGDDLegacyMaterial layout");

    struct Op
    {
        bool mStock;            // drawn the stock way (push_stock(*mParams))
        U32 mFirst;             // first command
        U32 mCount;             // commands
        LLDrawInfo* mParams;
    };

    // why a batch went the stock way
    enum EReason
    {
        R_RIGGED = 0,
        R_EMPTY,
        R_NO_TEXTURE,
        R_HANDLE,
        R_ATTRIBUTES,
        R_RANGE,
        R_ARENA,                        // + FSGDDArena::EFail
        R_COUNT = R_ARENA + FSGDDArena::FAIL_COUNT
    };
    const char* reason_name(U32 reason)
    {
        static const char* names[] = { "rigged", "empty", "no texture", "texture without handle",
                                       "attributes the program reads", "index range" };
        return reason < R_ARENA ? names[reason] : FSGDDArena::failName(reason - R_ARENA);
    }

    LLGLSLShader sPrograms[FS_GDD_MAT_PROGRAMS];    // by gDeferredMaterialProgram index
    GLint sRecordAttrib[FS_GDD_MAT_PROGRAMS];       // location of fs_gdd_draw
    S32 sInputsReady[FS_GDD_MAT_PROGRAMS] = {};     // bind_inputs done: 0 not yet, 1 yes, -1 failed

    GLuint sIndirectBuffer = 0;
    GLuint sRecordBuffer = 0;
    GLuint sMatrixBuffer = 0;
    GLuint sMaterialBuffer = 0;
    GLuint sQuery = 0;

    std::vector<Op> sOps;
    std::vector<DrawCommand> sCommands;
    std::vector<Record> sRecords;
    std::vector<glm::mat4> sMatrices;
    std::vector<Material> sMaterials;
    std::vector<LLDrawInfo*> sCommandParams;    // per command: its batch (verify, and the stock fallback)
    std::unordered_map<const LLMatrix4*, U32> sMatrixIndex;
    std::unordered_map<const LLMatrix4*, U32> sTexMatrixIndex;
    // consecutive batches mostly share their matrices: the last answers skip the map lookups
    const LLMatrix4* sLastModel = nullptr;
    U32 sLastModelIndex = U32_MAX;              // U32_MAX: none yet this pass
    const LLMatrix4* sLastTexMatrix = nullptr;
    U32 sLastTexMatrixIndex = U32_MAX;

    // The stock loop's state for the batches drawn the stock way: its uniform locations and texture units, and
    // what it last set (uniforms belong to the stock program, which the GPU-driven draws leave alone, and those
    // bind no textures, so the values stay current across them)
    struct StockState
    {
        bool mReady = false;
        GLint mIntensity = -1;
        GLint mBrightness = -1;
        GLint mMinAlpha = -1;
        GLint mSpecular = -1;
        S32 mDiffuseChannel = -1;
        S32 mSpecChannel = -1;
        S32 mNormChannel = -1;
        F32 mLastIntensity = 0.f;
        F32 mLastFullbright = 0.f;
        F32 mLastMinimumAlpha = 0.f;
        LLVector4 mLastSpecular;
        LLTexture* mLastNormalMap = nullptr;
        LLTexture* mLastSpecMap = nullptr;
        LLTexture* mLastDiffuse = nullptr;
    };
    StockState sStock;

    // stats, every 10 seconds with FSPerfGPUDrivenMaterialsVerify or FSPerfGPUDrivenMaterialsStats
    U64 sStatBatches = 0;
    U64 sStatDrawn = 0;
    U64 sStatCalls = 0;
    U64 sStatSwitches = 0;
    U64 sStatPasses = 0;
    U64 sStatMaterials = 0;
    U64 sStatReasons[R_COUNT] = {};
    U64 sStatMismatches = 0;
}

//-----------------------------------------------------------------------------
// Programs

namespace
{
    // the G-buffer permutations: not blended (DIFFUSE_ALPHA_MODE_BLEND goes to the alpha pool), not rigged
    bool wanted_program(U32 index)
    {
        return index < FS_GDD_MAT_PROGRAMS && (index & 0x3) != LLMaterial::DIFFUSE_ALPHA_MODE_BLEND;
    }

    bool create_program(U32 index)
    {
        const LLGLSLShader& stock = gDeferredMaterialProgram[index];
        LLGLSLShader& shader = sPrograms[index];
        shader.mName = llformat("FS GPU-Driven Material Shader %d", index);
        shader.mFeatures = stock.mFeatures;
        shader.mFeatures.mIndexedTextureChannels = 0;
        shader.mShaderFiles = stock.mShaderFiles;
        shader.mShaderLevel = stock.mShaderLevel;
        shader.mShaderGroup = stock.mShaderGroup;
        shader.clearPermutations();
        shader.mDefines = stock.mDefines;   // the stock permutations (HAS_NORMAL_MAP, DIFFUSE_ALPHA_MODE, ...), then ours
        shader.addPermutation("FS_GDD_MATERIAL", "1");
        return shader.createShader();
    }

    // the storage block bindings, where the driver put the per command record, and whether the arena can feed
    // every attribute the program reads
    bool bind_inputs(LLGLSLShader& shader, GLint& record_attrib)
    {
        const GLuint matrices = glGetProgramResourceIndex(shader.mProgramObject, GL_SHADER_STORAGE_BLOCK, "FSGDDMaterialMatrices");
        const GLuint materials = glGetProgramResourceIndex(shader.mProgramObject, GL_SHADER_STORAGE_BLOCK, "FSGDDMaterialRecords");
        record_attrib = glGetAttribLocation(shader.mProgramObject, "fs_gdd_draw");
        if (matrices == GL_INVALID_INDEX || materials == GL_INVALID_INDEX || record_attrib < 0 ||
            (shader.mAttributeMask & ~FS_GDD_MAT_ATTRIBS) || !(shader.mAttributeMask & LLVertexBuffer::MAP_VERTEX))
        {
            return false;
        }
        glShaderStorageBlockBinding(shader.mProgramObject, matrices, FS_GDD_MAT_MATRIX_BINDING);
        glShaderStorageBlockBinding(shader.mProgramObject, materials, FS_GDD_MAT_MATERIAL_BINDING);
        return true;
    }

    // Binds a GPU-driven program; the first time, also finds its inputs. False (for good) when they aren't there.
    bool bind_program(U32 index)
    {
        LLGLSLShader& shader = sPrograms[index];
        shader.bind();
        if (LLGLSLShader::sCurBoundShaderPtr != &shader)
        {
            return false;
        }
        if (!sInputsReady[index])
        {
            sInputsReady[index] = bind_inputs(shader, sRecordAttrib[index]) ? 1 : -1;
            if (sInputsReady[index] < 0)
            {
                LL_WARNS("Rendering") << "FSPerfGPUDrivenMaterials: " << shader.mName << " lacks its storage blocks or "
                                      << "draw record attribute, or reads an attribute the arena doesn't hold; its "
                                      << "batches are drawn as before" << LL_ENDL;
            }
        }
        return sInputsReady[index] > 0;
    }

    bool program_usable(U32 index)
    {
        return sPrograms[index].isComplete() && sInputsReady[index] >= 0;
    }
}

void fsGDDMaterialsUnloadShaders()
{
    for (U32 index = 0; index < FS_GDD_MAT_PROGRAMS; ++index)
    {
        sPrograms[index].unload();
        sInputsReady[index] = 0;
        sRecordAttrib[index] = -1;
    }
    fsGDDMaterialsReleaseGL();
}

void fsGDDMaterialsLoadShaders()
{
    fsGDDMaterialsUnloadShaders();

    static LLCachedControl<bool> enabled(gSavedSettings, "FSPerfGPUDrivenMaterials", false);
    if (!enabled)
    {
        return;
    }
    if (!gGLManager.mFSHasGPUDrivenDraws)
    {
        LL_INFOS_ONCE("ShaderLoading") << "FSPerfGPUDrivenMaterials: needs OpenGL 4.3 with GL_ARB_multi_draw_indirect and "
                                       << "GL_ARB_shader_storage_buffer_object; material batches are drawn as before" << LL_ENDL;
        return;
    }
    GLint vs_blocks = 0;
    glGetIntegerv(GL_MAX_VERTEX_SHADER_STORAGE_BLOCKS, &vs_blocks);
    if (vs_blocks < 2)
    {
        LL_INFOS_ONCE("ShaderLoading") << "FSPerfGPUDrivenMaterials: needs two shader storage blocks in the vertex shader, "
                                       << "the driver offers " << vs_blocks << "; material batches are drawn as before" << LL_ENDL;
        return;
    }
    // without bindless textures every batch would still bind its textures, which is much of what this saves
    if (!fsGDDBindlessSetup())
    {
        LL_INFOS_ONCE("ShaderLoading") << "FSPerfGPUDrivenMaterials: needs bindless textures (NVIDIA GPUs only); "
                                       << "material batches are drawn as before" << LL_ENDL;
        return;
    }

    for (U32 index = 0; index < FS_GDD_MAT_PROGRAMS; ++index)
    {
        if (!wanted_program(index) || gDeferredMaterialProgram[index].mShaderFiles.empty())
        {
            continue;
        }
        if (!create_program(index))
        {
            LL_WARNS("ShaderLoading") << "FSPerfGPUDrivenMaterials: " << sPrograms[index].mName << " failed to build, "
                                      << "its batches are drawn as before" << LL_ENDL;
            sPrograms[index].unload();
        }
    }
}

void fsGDDMaterialsReleaseGL()
{
    sArena.releaseGL();
    GLuint buffers[4] = { sIndirectBuffer, sRecordBuffer, sMatrixBuffer, sMaterialBuffer };
    for (GLuint buffer : buffers)
    {
        if (buffer)
        {
            glDeleteBuffers(1, &buffer);
        }
    }
    sIndirectBuffer = 0;
    sRecordBuffer = 0;
    sMatrixBuffer = 0;
    sMaterialBuffer = 0;
    if (sQuery)
    {
        glDeleteQueries(1, &sQuery);
        sQuery = 0;
    }
}

//-----------------------------------------------------------------------------
// The pass

namespace
{
    U32 matrix_index(const LLMatrix4* model, const glm::mat4& proj, const glm::mat4& view)
    {
        if (sLastModelIndex != U32_MAX && model == sLastModel)
        {
            return sLastModelIndex;
        }
        sLastModel = model;
        auto it = sMatrixIndex.find(model);
        if (it != sMatrixIndex.end())
        {
            sLastModelIndex = it->second;
            return it->second;
        }
        // as LLRenderPass::applyModelMatrix and LLRender::syncMatrices compose them (as fsgpudrivenlegacy.cpp)
        glm::mat4 modelview = view;
        if (model)
        {
            modelview *= glm::make_mat4((const F32*)model->mMatrix);
        }
        const U32 index = (U32)sMatrices.size();
        sMatrices.push_back(modelview);
        sMatrices.push_back(proj * modelview);
        sMatrices.push_back(glm::transpose(glm::inverse(modelview)));  // the normal matrix is its upper 3x3
        sMatrixIndex.emplace(model, index);
        sLastModelIndex = index;
        return index;
    }

    U32 texture_matrix_index(const LLMatrix4* texture_matrix)
    {
        if (!texture_matrix)
        {
            return 0;   // identity, what MM_TEXTURE0 holds outside the batch
        }
        if (sLastTexMatrixIndex != U32_MAX && texture_matrix == sLastTexMatrix)
        {
            return sLastTexMatrixIndex;
        }
        sLastTexMatrix = texture_matrix;
        auto it = sTexMatrixIndex.find(texture_matrix);
        if (it != sTexMatrixIndex.end())
        {
            sLastTexMatrixIndex = it->second;
            return it->second;
        }
        const U32 index = (U32)sMatrices.size();
        sMatrices.push_back(glm::make_mat4((const F32*)texture_matrix->mMatrix));
        sTexMatrixIndex.emplace(texture_matrix, index);
        sLastTexMatrixIndex = index;
        return index;
    }

    void set_handle(Material& mat, U32 slot, U64 handle)
    {
        mat.mTex[slot * 2] = (U32)(handle & 0xFFFFFFFFull);
        mat.mTex[slot * 2 + 1] = (U32)(handle >> 32);
    }

    // The handle of a texture the stock loop binds, or the reason it can't be had
    S32 texture_of(LLViewerTexture* tex, U32 frame, U64& handle)
    {
        if (!tex)
        {   // the stock loop leaves the unit unbound, or as an earlier batch left it
            return R_NO_TEXTURE;
        }
        handle = fsGDDPassTextureHandle(tex, frame);
        return handle ? -1 : R_HANDLE;
    }

    // Turns the pass's batches into ops (in the stock order, or grouped: one op for all the commands, then the
    // stock fallbacks) and commands.
    void build(U32 index, LLCullResult::drawinfo_iterator begin, LLCullResult::drawinfo_iterator end, bool grouped)
    {
        sOps.clear();
        sCommands.clear();
        sRecords.clear();
        sMatrices.clear();
        sMaterials.clear();
        sCommandParams.clear();
        sMatrixIndex.clear();
        sTexMatrixIndex.clear();
        sLastModelIndex = U32_MAX;
        sLastTexMatrixIndex = U32_MAX;
        fsGDDBeginTexturePass();

        const U32 frame = LLFrameTimer::getFrameCount();
        sArena.beginFrame(frame);
        const glm::mat4 proj = gGL.getProjectionMatrix();
        const glm::mat4 view = get_current_modelview();
        sMatrices.push_back(glm::mat4(1.f));   // texture matrix 0: identity

        // the maps the stock loop binds: those the stock program samples (its texture channels)
        const LLGLSLShader& stock = gDeferredMaterialProgram[index];
        const bool normal_map = stock.getTextureChannel(LLShaderMgr::BUMP_MAP) > -1;
        const bool spec_map = stock.getTextureChannel(LLShaderMgr::SPECULAR_MAP) > -1;
        // the attributes the program reads that a buffer may lack
        const U32 needed = sPrograms[index].mAttributeMask & FS_GDD_MAT_ATTRIBS;

        for (LLCullResult::drawinfo_iterator i = begin; i != end; )
        {
            LLDrawInfo* pparams = *i;
            LLCullResult::increment_iterator(i, end);
            if (!pparams)
            {
                continue;
            }
            LLDrawInfo& params = *pparams;
            ++sStatBatches;

            S32 reason = -1;
            const FSGDDArenaEntry* entry = nullptr;
            Material mat = {};
            if (params.mAvatar.notNull() || params.mSkinInfo.notNull())
            {
                reason = R_RIGGED;
            }
            else if (!params.mCount || params.mVertexBuffer.isNull())
            {   // the stock loop would still bind for these; it gets them
                reason = R_EMPTY;
            }
            else
            {
                // the textures the stock loop binds for this batch
                U64 handle = 0;
                reason = texture_of(params.mTexture.get(), frame, handle);
                set_handle(mat, 0, handle);
                if (reason < 0 && normal_map)
                {
                    handle = 0;
                    reason = texture_of(params.mNormalMap.get(), frame, handle);
                    set_handle(mat, 1, handle);
                }
                if (reason < 0 && spec_map)
                {
                    handle = 0;
                    reason = texture_of(params.mSpecularMap.get(), frame, handle);
                    set_handle(mat, 2, handle);
                }

                if (reason < 0)
                {
                    U32 fail = 0;
                    if ((params.mVertexBuffer->getTypeMask() & needed) != needed)
                    {   // checked before the arena, which would copy the buffer only for it to be turned down here
                        reason = R_ATTRIBUTES;
                    }
                    else if (!(entry = sArena.find(params.mVertexBuffer.get(), frame, &fail)))
                    {
                        reason = R_ARENA + fail;
                    }
                    else if ((entry->mTypeMask & needed) != needed)
                    {
                        reason = R_ATTRIBUTES;
                    }
                    else if (params.mOffset + params.mCount > entry->mNumIndices || params.mEnd >= entry->mNumVerts)
                    {   // never for a valid batch; anything else is left to the stock path rather than read past the copy
                        reason = R_RANGE;
                    }
                }
            }

            if (reason >= 0)
            {
                ++sStatReasons[reason];
                sOps.push_back({ true, 0, 0, &params });
                continue;
            }

            // the uniforms the stock loop sets for this batch
            for (U32 c = 0; c < 4; ++c)
            {
                mat.mSpecular[c] = params.mSpecColor.mV[c];
            }
            mat.mParams[0] = params.mEnvIntensity;
            mat.mParams[1] = params.mAlphaMaskCutoff;
            mat.mParams[2] = params.mFullbright ? 1.f : 0.f;
            // consecutive batches mostly share their material
            if (sMaterials.empty() || memcmp(&sMaterials.back(), &mat, sizeof(Material)) != 0)
            {
                sMaterials.push_back(mat);
            }
            const U32 material = (U32)sMaterials.size() - 1;

            ++sStatDrawn;
            const U32 command = (U32)sCommands.size();
            sCommands.push_back({ params.mCount, 1, entry->mIndex + params.mOffset, (GLint)entry->mVertex, command });
            sRecords.push_back({ matrix_index(params.mModelMatrix, proj, view), texture_matrix_index(params.mTextureMatrix.get()),
                                 material, 0 });
            sCommandParams.push_back(&params);

            if (grouped)
            {
                continue;
            }
            Op* last = sOps.empty() ? nullptr : &sOps.back();
            if (last && !last->mStock)
            {
                ++last->mCount;
            }
            else
            {
                sOps.push_back({ false, command, 1, nullptr });
            }
        }
        if (grouped && !sCommands.empty())
        {   // the commands first, as one run
            sOps.insert(sOps.begin(), { false, 0, (U32)sCommands.size(), nullptr });
        }
        sStatMaterials += sMaterials.size();
    }

    void upload(GLuint& buffer, GLenum target, const void* data, size_t bytes)
    {
        if (!buffer)
        {
            glGenBuffers(1, &buffer);
        }
        glBindBuffer(target, buffer);
        // new storage: the previous pass's draws may still read the old; never empty
        glBufferData(target, llmax(bytes, (size_t)16), bytes ? data : nullptr, GL_STREAM_DRAW);
    }

    // the arena, the record attribute and the storage buffers for the pass's program (bound)
    void setup_inputs(U32 index)
    {
        // the attributes go into the global vertex array object, never a buffer's own
        LLVertexBuffer::fsUseDefaultVAO();
        glBindBufferBase(GL_SHADER_STORAGE_BUFFER, FS_GDD_MAT_MATRIX_BINDING, sMatrixBuffer);
        glBindBufferBase(GL_SHADER_STORAGE_BUFFER, FS_GDD_MAT_MATERIAL_BINDING, sMaterialBuffer);
        glBindBuffer(GL_DRAW_INDIRECT_BUFFER, sIndirectBuffer);
        const GLuint record = (GLuint)sRecordAttrib[index];
        glBindBuffer(GL_ARRAY_BUFFER, sRecordBuffer);
        glEnableVertexAttribArray(record);
        glVertexAttribIPointer(record, 4, GL_UNSIGNED_INT, sizeof(Record), nullptr);
        glVertexAttribDivisor(record, 1);
        // every attribute the program reads (bind_inputs checked the arena holds them, build that the batches have them)
        sArena.bindForDraw(sPrograms[index].mAttributeMask);
    }

    // undo setup_inputs: the program's attribute mask doesn't have the record's location, so LLVertexBuffer
    // expects it off and undivided
    void finish_inputs(U32 index)
    {
        const GLuint record = (GLuint)sRecordAttrib[index];
        glVertexAttribDivisor(record, 0);
        glDisableVertexAttribArray(record);
        glBindBufferBase(GL_SHADER_STORAGE_BUFFER, FS_GDD_MAT_MATRIX_BINDING, 0);
        glBindBufferBase(GL_SHADER_STORAGE_BUFFER, FS_GDD_MAT_MATERIAL_BINDING, 0);  // also unbinds the generic binding
        glBindBuffer(GL_DRAW_INDIRECT_BUFFER, 0);
        LLVertexBuffer::unbind();   // the next setBuffer() binds and sets up its own attributes again
    }

    // As LLDrawPoolMaterials::renderDeferred sets up before its loop. The stock program must be bound.
    void begin_stock(LLGLSLShader& stock)
    {
        sStock = StockState();
        sStock.mReady = true;
        sStock.mIntensity = stock.getUniformLocation(LLShaderMgr::ENVIRONMENT_INTENSITY);
        sStock.mBrightness = stock.getUniformLocation(LLShaderMgr::EMISSIVE_BRIGHTNESS);
        sStock.mMinAlpha = stock.getUniformLocation(LLShaderMgr::MINIMUM_ALPHA);
        sStock.mSpecular = stock.getUniformLocation(LLShaderMgr::SPECULAR_COLOR);
        sStock.mDiffuseChannel = stock.enableTexture(LLShaderMgr::DIFFUSE_MAP);
        sStock.mSpecChannel = stock.enableTexture(LLShaderMgr::SPECULAR_MAP);
        sStock.mNormChannel = stock.enableTexture(LLShaderMgr::BUMP_MAP);
        sStock.mLastSpecular = LLVector4(0, 0, 0, 0);

        gGL.getTexUnit(sStock.mDiffuseChannel)->unbindFast(LLTexUnit::TT_TEXTURE);
        if (sStock.mIntensity > -1)
        {
            glUniform1f(sStock.mIntensity, sStock.mLastIntensity);
        }
        if (sStock.mBrightness > -1)
        {
            glUniform1f(sStock.mBrightness, sStock.mLastFullbright);
        }
        if (sStock.mMinAlpha > -1)
        {
            glUniform1f(sStock.mMinAlpha, sStock.mLastMinimumAlpha);
        }
        if (sStock.mSpecular > -1)
        {
            glUniform4fv(sStock.mSpecular, 1, sStock.mLastSpecular.mV);
        }
    }

    // One batch the stock way, as the loop of LLDrawPoolMaterials::renderDeferred draws a non rigged one. The
    // stock program must be bound.
    void push_stock(LLGLSLShader& stock, LLDrawInfo& params)
    {
        if (!sStock.mReady)
        {
            begin_stock(stock);
        }
        StockState& s = sStock;
        if (s.mSpecular > -1 && params.mSpecColor != s.mLastSpecular)
        {
            s.mLastSpecular = params.mSpecColor;
            glUniform4fv(s.mSpecular, 1, s.mLastSpecular.mV);
        }
        if (s.mIntensity > -1 && s.mLastIntensity != params.mEnvIntensity)
        {
            s.mLastIntensity = params.mEnvIntensity;
            glUniform1f(s.mIntensity, s.mLastIntensity);
        }
        if (s.mMinAlpha > -1 && s.mLastMinimumAlpha != params.mAlphaMaskCutoff)
        {
            s.mLastMinimumAlpha = params.mAlphaMaskCutoff;
            glUniform1f(s.mMinAlpha, s.mLastMinimumAlpha);
        }
        const F32 fullbright = params.mFullbright ? 1.f : 0.f;
        if (s.mBrightness > -1 && s.mLastFullbright != fullbright)
        {
            s.mLastFullbright = fullbright;
            glUniform1f(s.mBrightness, s.mLastFullbright);
        }
        if (s.mNormChannel > -1 && params.mNormalMap != s.mLastNormalMap)
        {
            s.mLastNormalMap = params.mNormalMap;
            if (s.mLastNormalMap)
            {
                gGL.getTexUnit(s.mNormChannel)->bindFast(s.mLastNormalMap);
            }
        }
        if (s.mSpecChannel > -1 && params.mSpecularMap != s.mLastSpecMap)
        {
            s.mLastSpecMap = params.mSpecularMap;
            if (s.mLastSpecMap)
            {
                gGL.getTexUnit(s.mSpecChannel)->bindFast(s.mLastSpecMap);
            }
        }
        if (params.mTexture != s.mLastDiffuse)
        {
            s.mLastDiffuse = params.mTexture;
            if (s.mLastDiffuse)
            {
                gGL.getTexUnit(s.mDiffuseChannel)->bindFast(s.mLastDiffuse);
            }
            else
            {
                gGL.getTexUnit(s.mDiffuseChannel)->unbindFast(LLTexUnit::TT_TEXTURE);
            }
        }

        LLRenderPass::applyModelMatrix(params);

        bool tex_setup = false;
        if (params.mTextureMatrix)
        {
            gGL.getTexUnit(0)->activate();
            gGL.matrixMode(LLRender::MM_TEXTURE);
            gGL.loadMatrix((GLfloat*)params.mTextureMatrix->mMatrix);
            gPipeline.mTextureMatrixOps++;
            tex_setup = true;
        }

        params.mVertexBuffer->setBuffer();
        params.mVertexBuffer->drawRange(LLRender::TRIANGLES, params.mStart, params.mEnd, params.mCount, params.mOffset);

        if (tex_setup)
        {
            gGL.getTexUnit(0)->activate();
            gGL.loadIdentity();
            gGL.matrixMode(LLRender::MM_MODELVIEW);
        }
    }

    // Draws the ops. The stock program is bound on entry and on return. False if the GPU-driven program
    // couldn't be bound before anything was drawn (then nothing was).
    bool execute(U32 index, LLGLSLShader& stock, U32& calls, U32& switches)
    {
        LL_PROFILE_ZONE_NAMED_CATEGORY_DRAWPOOL("gdd materials execute");
        LL_PROFILE_GPU_ZONE("gdd materials");

        upload(sIndirectBuffer, GL_DRAW_INDIRECT_BUFFER, sCommands.data(), sCommands.size() * sizeof(DrawCommand));
        upload(sRecordBuffer, GL_ARRAY_BUFFER, sRecords.data(), sRecords.size() * sizeof(Record));
        upload(sMatrixBuffer, GL_SHADER_STORAGE_BUFFER, sMatrices.data(), sMatrices.size() * sizeof(glm::mat4));
        upload(sMaterialBuffer, GL_SHADER_STORAGE_BUFFER, sMaterials.data(), sMaterials.size() * sizeof(Material));
        glBindBuffer(GL_SHADER_STORAGE_BUFFER, 0);
        glBindBuffer(GL_DRAW_INDIRECT_BUFFER, 0);
        glBindBuffer(GL_ARRAY_BUFFER, 0);
        LLVertexBuffer::sGLRenderBuffer = 0;    // or the next setBuffer of the buffer it named skips binding it

        bool gdd = false;   // the GPU-driven program is bound
        bool drew = false;

        for (const Op& op : sOps)
        {
            if (op.mStock)
            {
                if (gdd)
                {
                    finish_inputs(index);
                    stock.bind();
                    gdd = false;
                    ++switches;
                }
                push_stock(stock, *op.mParams);
                drew = true;
                continue;
            }

            if (!gdd)
            {
                if (!bind_program(index))
                {
                    stock.bind();
                    if (!drew)
                    {
                        return false;
                    }
                    // not expected (fsGDDDrawMaterials bound the program once already); the rest of the pass goes
                    // the stock way, in order
                    LL_WARNS_ONCE("Rendering") << "FSPerfGPUDrivenMaterials: a program failed to bind mid pass" << LL_ENDL;
                    for (size_t i = &op - sOps.data(); i < sOps.size(); ++i)
                    {
                        const Op& rest = sOps[i];
                        if (rest.mStock)
                        {
                            push_stock(stock, *rest.mParams);
                        }
                        else
                        {
                            for (U32 c = rest.mFirst; c < rest.mFirst + rest.mCount; ++c)
                            {
                                push_stock(stock, *sCommandParams[c]);
                            }
                        }
                    }
                    return true;
                }
                ++switches;
                gdd = true;
                setup_inputs(index);
            }
            glMultiDrawElementsIndirect(GL_TRIANGLES, GL_UNSIGNED_SHORT, (const void*)((size_t)op.mFirst * sizeof(DrawCommand)),
                                        (GLsizei)op.mCount, 0);
            ++calls;
            drew = true;
        }
        if (gdd)
        {
            finish_inputs(index);
            stock.bind();
        }
        STOP_GLERROR;
        return true;
    }

    // FSPerfGPUDrivenMaterialsVerify: the batches drawn from the arena again, the stock way, colour and depth
    // writes off, depth test GL_LESS against what the multi-draws left. The stock program must be bound.
    void verify_against_stock(LLGLSLShader& stock)
    {
        if (sCommandParams.empty())
        {
            return;
        }
        LL_PROFILE_ZONE_NAMED_CATEGORY_DRAWPOOL("gdd materials verify");

        GLenum error = glGetError();
        if (error != GL_NO_ERROR)
        {
            static LLFrameTimer warn_timer;
            if (warn_timer.getElapsedTimeF32() > 5.f)
            {
                warn_timer.reset();
                LL_WARNS("Rendering") << "FSPerfGPUDrivenMaterialsVerify: GL error 0x" << std::hex << error << std::dec
                                      << " after the GPU-driven " << stock.mName << " pass" << LL_ENDL;
            }
        }

        // arena copies against their sources, a couple per pass
        U32 checked = 0;
        for (LLDrawInfo* params : sCommandParams)
        {
            LLVertexBuffer* vb = params->mVertexBuffer.get();
            FSGDDArenaEntry* found = sArena.peek(vb);
            if (found && !found->mVerified)
            {
                if (!sArena.verifyEntry(vb, *found))
                {
                    ++sStatMismatches;
                    static LLFrameTimer warn_timer;
                    if (warn_timer.getElapsedTimeF32() > 5.f)
                    {
                        warn_timer.reset();
                        LL_WARNS("Rendering") << "FSPerfGPUDrivenMaterialsVerify: the arena copy of a vertex buffer ("
                                              << found->mNumVerts << " vertices, " << found->mNumIndices
                                              << " indices) differs from the buffer" << LL_ENDL;
                    }
                }
                if (++checked >= 2)
                {
                    break;
                }
            }
        }

        // the occlusion check needs the multi-draws' depth in the buffer
        GLboolean depth_test = GL_FALSE;
        GLboolean depth_write = GL_FALSE;
        glGetBooleanv(GL_DEPTH_TEST, &depth_test);
        glGetBooleanv(GL_DEPTH_WRITEMASK, &depth_write);
        if (!depth_test || !depth_write)
        {
            return;
        }

        if (!sQuery)
        {
            glGenQueries(1, &sQuery);
        }
        {
            LLGLDepthTest depth(GL_TRUE, GL_FALSE, GL_LESS);
            GLboolean mask[4] = { GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE };
            glGetBooleanv(GL_COLOR_WRITEMASK, mask);
            glColorMask(GL_FALSE, GL_FALSE, GL_FALSE, GL_FALSE);    // behind gGL's back, put back below
            glBeginQuery(GL_SAMPLES_PASSED, sQuery);
            for (LLDrawInfo* params : sCommandParams)
            {
                push_stock(stock, *params);
            }
            glEndQuery(GL_SAMPLES_PASSED);
            glColorMask(mask[0], mask[1], mask[2], mask[3]);
        }

        GLuint samples = 0;
        glGetQueryObjectuiv(sQuery, GL_QUERY_RESULT, &samples);
        if (samples)
        {
            ++sStatMismatches;
            static LLFrameTimer warn_timer;
            if (warn_timer.getElapsedTimeF32() > 5.f)
            {
                warn_timer.reset();
                LL_WARNS("Rendering") << "FSPerfGPUDrivenMaterialsVerify: " << samples << " samples of " << sCommandParams.size()
                                      << " stock batches (" << stock.mName << ") are nearer than the multi-draw depth; "
                                      << sStatMismatches << " mismatches so far" << LL_ENDL;
            }
        }
    }

    void log_stats()
    {
        static LLFrameTimer timer;
        if (timer.getElapsedTimeF32() < 10.f)
        {
            return;
        }
        timer.reset();
        std::ostringstream reasons;
        for (U32 r = 0; r < R_COUNT; ++r)
        {
            if (sStatReasons[r])
            {
                reasons << (reasons.tellp() ? ", " : "") << reason_name(r) << " " << sStatReasons[r];
                sStatReasons[r] = 0;
            }
        }
        LL_INFOS("FSGPUDrivenMaterials") << "GPU-driven materials, last 10 s: " << sStatPasses << " passes, " << sStatBatches
                                         << " batches (" << sStatDrawn << " from the arena, " << sStatMaterials
                                         << " material records) in " << sStatCalls << " multi-draw calls, " << sStatSwitches
                                         << " program switches; stock path for: "
                                         << (reasons.tellp() ? reasons.str() : std::string("none")) << "; arena "
                                         << sArena.vertexMB() << " MB vertices, " << sArena.indexMB() << " MB indices, "
                                         << sArena.entries() << " buffers, " << sArena.uploads() << " copies; "
                                         << sStatMismatches << " verify mismatches" << LL_ENDL;
        sStatPasses = sStatBatches = sStatDrawn = sStatCalls = sStatSwitches = sStatMaterials = 0;
        sArena.clearCounts();
    }
}

bool fsGDDDrawMaterials(U32 type, LLGLSLShader& stock)
{
    // idle handles go non resident and away also while the other bindless passes are off (it does something
    // every 120 frames)
    static U32 s_swept_frame = U32_MAX;
    const U32 sweep_frame = LLFrameTimer::getFrameCount();
    if (sweep_frame != s_swept_frame)
    {
        s_swept_frame = sweep_frame;
        fsGDDSweepHandles(sweep_frame);
    }

    static LLCachedControl<bool> enabled(gSavedSettings, "FSPerfGPUDrivenMaterials", false);
    if (!enabled || !gGLManager.mFSHasGPUDrivenDraws || !fsGDDBindlessReady() || LLPipeline::sShadowRender ||
        LLPipeline::sRenderingHUDs)
    {
        return false;
    }
    // the non rigged, non blended permutations only
    U32 index = 0;
    while (index < FS_GDD_MAT_PROGRAMS && &gDeferredMaterialProgram[index] != &stock)
    {
        ++index;
    }
    if (!wanted_program(index) || LLGLSLShader::sCurBoundShaderPtr != &stock || !program_usable(index))
    {
        return false;
    }
    auto* begin = gPipeline.beginRenderMap(type);
    auto* end = gPipeline.endRenderMap(type);
    if (begin == end)
    {
        return false;
    }
    LL_PROFILE_ZONE_SCOPED_CATEGORY_DRAWPOOL;
    static LLCachedControl<bool> verify(gSavedSettings, "FSPerfGPUDrivenMaterialsVerify", false);
    static LLCachedControl<bool> stats(gSavedSettings, "FSPerfGPUDrivenMaterialsStats", false);
    static LLCachedControl<bool> group_runs(gSavedSettings, "FSPerfGPUDrivenPBRGroupRuns", false);

    // The first bind finds the program's inputs; do that now, so a failure leaves the whole pass to the
    // stock loop rather than part of it
    if (!sInputsReady[index])
    {
        const bool ready = bind_program(index);
        stock.bind();
        if (!ready)
        {
            return false;
        }
    }

    // The environment uniforms (LLEnvironment marks the programs of the shader list dirty when they change,
    // and the stock program took them when the pool bound it); ours isn't in that list, so it takes the
    // current values at its first bind in each pass
    sPrograms[index].mUniformsDirty = true;

    {
        LL_PROFILE_ZONE_NAMED_CATEGORY_DRAWPOOL("gdd materials build");
        build(index, begin, end, group_runs);
    }
    if (sCommands.empty())
    {   // nothing from the arena: the stock loop draws exactly the same
        if (verify || stats)
        {
            ++sStatPasses;
            log_stats();
        }
        return false;
    }

    sStock.mReady = false;  // the stock loop's state, set up at the first batch drawn the stock way
    U32 calls = 0;
    U32 switches = 0;
    if (!execute(index, stock, calls, switches))
    {
        return false;
    }

    if (verify)
    {
        verify_against_stock(stock);
    }
    gGL.loadMatrix(gGLModelView);
    gGLLastMatrix = NULL;

    if (verify || stats)
    {
        ++sStatPasses;
        sStatCalls += calls;
        sStatSwitches += switches;
        log_stats();
    }
    return true;
}
