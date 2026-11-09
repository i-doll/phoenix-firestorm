/**
 * @file fsgpudrivenlegacy.cpp
 * @brief GPU-driven draws for the deferred legacy (non PBR) opaque passes (FSPerfGPUDrivenLegacy)
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

// The stock legacy opaque passes (LLRenderPass::pushBatch for PASS_SIMPLE and PASS_FULLBRIGHT, pushMaskBatches
// for PASS_ALPHA_MASK and PASS_FULLBRIGHT_ALPHA_MASK) cost, for every batch, a model matrix (modelview,
// modelview projection and normal matrix uniforms), up to 16 texture binds (the batch's texture list, which the
// indexed texture shaders pick from by the vertex's texture index) or one bind and a texture matrix, a minimum
// alpha uniform in the masked passes, a vertex buffer bind and a glDrawRangeElements.
//
// With FSPerfGPUDrivenLegacy on, the batches come out of a vertex arena instead (fsgddarena.h: a GPU side copy
// of the position, normal, texcoord0 and colour regions and the 16 bit indices of their vertex buffers; the
// texture index is the w of the position, so it comes along; normals are optional, fullbright buffers have
// none), and each batch is one 20 byte indirect draw command plus a record: which model matrices (in a storage
// buffer, the matrices the stock path would have computed, computed the same way), which texture matrix
// (identity, or the batch's animation when pushBatch would load it), and where its textures start in a second
// storage buffer: a header (the minimum alpha pushMaskBatches would set, the texture count) and the bindless
// handle of every texture pushBatch would bind, in unit order. diffuseV.glsl / fullbrightV.glsl built with
// FS_GDD_LEGACY pick the handle the vertex's texture index selects, exactly as diffuseLookup picks a unit; the
// fragment shaders are the stock ones with diffuseLookup and minimum_alpha replaced.
//
// The texture handles are those of FSPerfGPUDrivenPBRBindless (fsgpudrivenpbr.cpp: a sampler per texture
// state, dropped before LLImageGL changes a texture), so this needs bindless textures, which are only used on
// NVIDIA; without them the passes stay as they are, since binding textures is most of what this saves.
//
// The batches are drawn in the stock order: one glMultiDrawElementsIndirect per run of batches from the arena.
// A batch that can't come from it (rigged, empty, a texture missing or without a handle (not loaded, one or
// two channels, options pending, media), 32 bit indices, buffer mapped or lacking an attribute, arena full or
// the copy budget used up) is drawn by the stock code in its place.
//
// FSPerfGPUDrivenLegacyVerify: after the pass, the batches it drew from the arena are drawn again the stock way
// with colour and depth writes off and depth test GL_LESS under a GL_SAMPLES_PASSED query: a sample passes only
// where the stock geometry is nearer than what the multi-draws left, which is logged (only where the pass
// writes depth; the fullbright passes may not). It also reads back a couple of arena entries per pass and
// compares them with their vertex buffers, checks for GL errors, and logs statistics every 10 seconds
// (FSPerfGPUDrivenLegacyStats logs only the statistics).

#include "llviewerprecompiledheaders.h"

#include "fsgpudrivenlegacy.h"

#include "fsgddarena.h"
#include "fsgpudrivenpbr.h"

#include "lldrawpool.h"
#include "llframetimer.h"
#include "llgl.h"
#include "llglslshader.h"
#include "llglstates.h"
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

LLGLSLShader gFSGDDDiffuseProgram;
LLGLSLShader gFSGDDDiffuseAlphaMaskProgram;
LLGLSLShader gFSGDDFullbrightProgram;
LLGLSLShader gFSGDDFullbrightAlphaMaskProgram;

namespace
{
    // the PBR pass uses the same bindings; each pass binds its buffers and clears them again
    constexpr GLuint FS_GDD_LEGACY_MATRIX_BINDING = 5;  // shader storage binding of FSGDDLegacyMatrices
    constexpr GLuint FS_GDD_LEGACY_TEXTURE_BINDING = 6; // and of FSGDDLegacyTextures
    // the attributes the arena can feed; a program wanting any other is not used
    constexpr U32 FS_GDD_LEGACY_ATTRIBS = LLVertexBuffer::MAP_VERTEX | LLVertexBuffer::MAP_NORMAL | LLVertexBuffer::MAP_TEXCOORD0 |
                                          LLVertexBuffer::MAP_COLOR | LLVertexBuffer::MAP_TEXTURE_INDEX;

    // everything diffuseV.glsl reads (fullbrightV.glsl: all but the normal, which fullbright buffers lack), an
    // eighth of FSPerfGPUDrivenLegacyArenaMB for indices
    FSGDDArena sArena("FSPerfGPUDrivenLegacy arena",
                      { LLVertexBuffer::TYPE_VERTEX, LLVertexBuffer::TYPE_NORMAL, LLVertexBuffer::TYPE_TEXCOORD0,
                        LLVertexBuffer::TYPE_COLOR },
                      "FSPerfGPUDrivenLegacyArenaMB", 512, 8, LLVertexBuffer::MAP_NORMAL);

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
        U32 mTextures;  // first entry in FSGDDLegacyTextures: the header, then the handles
        U32 mPad;
    };

    struct TexEntry     // uvec2, std430 stride 8
    {
        U32 mX;         // header: minimum alpha (float bits); handle: low 32 bits
        U32 mY;         // header: texture count; handle: high 32 bits
    };
    static_assert(sizeof(TexEntry) == 8, "FSGDDLegacyTextures layout");

    struct Op
    {
        bool mStock;            // pool.pushBatch(*mParams)
        U32 mFirst;             // first command
        U32 mCount;             // commands
        LLDrawInfo* mParams;
    };

    enum EPass
    {
        P_SIMPLE = 0,
        P_ALPHA_MASK,
        P_FULLBRIGHT,
        P_FULLBRIGHT_ALPHA_MASK,
        P_COUNT
    };

    struct PassInfo
    {
        U32 mType;
        LLGLSLShader* mStock;
        LLGLSLShader* mProgram;
        const char* mName;
        bool mMask;             // pushMaskBatches: minimum alpha per batch
        bool mNormals;          // the program reads normals
    };

    const PassInfo sPasses[P_COUNT] =
    {
        { LLRenderPass::PASS_SIMPLE, &gDeferredDiffuseProgram, &gFSGDDDiffuseProgram,
          "FS GPU-Driven Diffuse Shader", false, true },
        { LLRenderPass::PASS_ALPHA_MASK, &gDeferredDiffuseAlphaMaskProgram, &gFSGDDDiffuseAlphaMaskProgram,
          "FS GPU-Driven Diffuse Alpha Mask Shader", true, true },
        { LLRenderPass::PASS_FULLBRIGHT, &gDeferredFullbrightProgram, &gFSGDDFullbrightProgram,
          "FS GPU-Driven Fullbright Shader", false, false },
        { LLRenderPass::PASS_FULLBRIGHT_ALPHA_MASK, &gDeferredFullbrightAlphaMaskProgram, &gFSGDDFullbrightAlphaMaskProgram,
          "FS GPU-Driven Fullbright Alpha Mask Shader", true, false },
    };

    // why a batch went the stock way
    enum EReason
    {
        R_RIGGED = 0,
        R_EMPTY,
        R_NO_TEXTURE,
        R_HANDLE,
        R_NORMALS,
        R_RANGE,
        R_ARENA,                        // + FSGDDArena::EFail
        R_COUNT = R_ARENA + FSGDDArena::FAIL_COUNT
    };
    const char* reason_name(U32 reason)
    {
        static const char* names[] = { "rigged", "empty", "no texture", "texture without handle", "no normals", "index range" };
        return reason < R_ARENA ? names[reason] : FSGDDArena::failName(reason - R_ARENA);
    }

    GLuint sIndirectBuffer = 0;
    GLuint sRecordBuffer = 0;
    GLuint sMatrixBuffer = 0;
    GLuint sTextureBuffer = 0;
    GLuint sQuery = 0;
    GLint sRecordAttrib[P_COUNT] = { -1, -1, -1, -1 };  // location of fs_gdd_draw
    S32 sInputsReady[P_COUNT] = {};                     // bind_inputs done: 0 not yet, 1 yes, -1 failed

    std::vector<Op> sOps;
    std::vector<DrawCommand> sCommands;
    std::vector<Record> sRecords;
    std::vector<glm::mat4> sMatrices;
    std::vector<TexEntry> sTextures;
    std::vector<LLDrawInfo*> sCommandParams;    // per command: its batch (verify, and the stock fallback)
    std::unordered_map<const LLMatrix4*, U32> sMatrixIndex;
    std::unordered_map<const LLMatrix4*, U32> sTexMatrixIndex;
    // consecutive batches mostly share their matrices: the last answers skip the map lookups
    const LLMatrix4* sLastModel = nullptr;
    U32 sLastModelIndex = U32_MAX;              // U32_MAX: none yet this pass
    const LLMatrix4* sLastTexMatrix = nullptr;
    U32 sLastTexMatrixIndex = U32_MAX;
    U32 sPassSerial = 1;    // the pass being built; textures remember their handle by it (mFSGDDPass), also
                            // for FSPerfGPUDrivenMaterials (fsGDDBeginTexturePass)

    // stats, every 10 seconds with FSPerfGPUDrivenLegacyVerify or FSPerfGPUDrivenLegacyStats
    U64 sStatBatches = 0;
    U64 sStatDrawn = 0;
    U64 sStatCalls = 0;
    U64 sStatSwitches = 0;
    U64 sStatPasses = 0;
    U64 sStatReasons[R_COUNT] = {};
    U64 sStatMismatches = 0;
    U64 sStatUnverified = 0;    // passes without depth writes, where the occlusion check can't run
}

//-----------------------------------------------------------------------------
// Programs

namespace
{
    bool create_program(const PassInfo& pass)
    {
        const LLGLSLShader& stock = *pass.mStock;
        LLGLSLShader& shader = *pass.mProgram;
        shader.mName = pass.mName;
        shader.mFeatures = stock.mFeatures;
        // no indexed texture units: the vertex shader reads texture_index itself and diffuseLookup samples a handle
        shader.mFeatures.mIndexedTextureChannels = 0;
        shader.mShaderFiles = stock.mShaderFiles;
        shader.mShaderLevel = stock.mShaderLevel;
        shader.mShaderGroup = stock.mShaderGroup;
        shader.clearPermutations();
        shader.mDefines = stock.mDefines;   // the stock permutations (HAS_ALPHA_MASK, HAS_EMISSIVE, ...), then ours
        shader.addPermutation("FS_GDD_LEGACY", "1");
        return shader.createShader();
    }

    // the storage block bindings, where the driver put the per command record, and whether the arena can feed
    // every attribute the program reads
    bool bind_inputs(LLGLSLShader& shader, GLint& record_attrib)
    {
        const GLuint matrices = glGetProgramResourceIndex(shader.mProgramObject, GL_SHADER_STORAGE_BLOCK, "FSGDDLegacyMatrices");
        const GLuint textures = glGetProgramResourceIndex(shader.mProgramObject, GL_SHADER_STORAGE_BLOCK, "FSGDDLegacyTextures");
        record_attrib = glGetAttribLocation(shader.mProgramObject, "fs_gdd_draw");
        if (matrices == GL_INVALID_INDEX || textures == GL_INVALID_INDEX || record_attrib < 0 ||
            (shader.mAttributeMask & ~FS_GDD_LEGACY_ATTRIBS) || !(shader.mAttributeMask & LLVertexBuffer::MAP_TEXTURE_INDEX))
        {
            return false;
        }
        glShaderStorageBlockBinding(shader.mProgramObject, matrices, FS_GDD_LEGACY_MATRIX_BINDING);
        glShaderStorageBlockBinding(shader.mProgramObject, textures, FS_GDD_LEGACY_TEXTURE_BINDING);
        return true;
    }

    // Binds the pass's GPU-driven program; the first time, also finds its inputs. False (for good) when they
    // aren't there.
    bool bind_program(U32 pass)
    {
        LLGLSLShader& shader = *sPasses[pass].mProgram;
        shader.bind();
        if (LLGLSLShader::sCurBoundShaderPtr != &shader)
        {
            return false;
        }
        if (!sInputsReady[pass])
        {
            sInputsReady[pass] = bind_inputs(shader, sRecordAttrib[pass]) ? 1 : -1;
            if (sInputsReady[pass] < 0)
            {
                LL_WARNS("Rendering") << "FSPerfGPUDrivenLegacy: " << shader.mName << " lacks its storage blocks or draw "
                                      << "record attribute, or reads an attribute the arena doesn't hold; its batches "
                                      << "are drawn as before" << LL_ENDL;
            }
        }
        return sInputsReady[pass] > 0;
    }

    bool program_usable(U32 pass)
    {
        return sPasses[pass].mProgram->isComplete() && sInputsReady[pass] >= 0;
    }
}

void fsGDDLegacyUnloadShaders()
{
    for (U32 pass = 0; pass < P_COUNT; ++pass)
    {
        sPasses[pass].mProgram->unload();
        sInputsReady[pass] = 0;
        sRecordAttrib[pass] = -1;
    }
    fsGDDLegacyReleaseGL();
}

void fsGDDLegacyLoadShaders()
{
    fsGDDLegacyUnloadShaders();

    static LLCachedControl<bool> enabled(gSavedSettings, "FSPerfGPUDrivenLegacy", false);
    if (!enabled)
    {
        return;
    }
    if (!gGLManager.mFSHasGPUDrivenDraws)
    {
        LL_INFOS_ONCE("ShaderLoading") << "FSPerfGPUDrivenLegacy: needs OpenGL 4.3 with GL_ARB_multi_draw_indirect and "
                                       << "GL_ARB_shader_storage_buffer_object; legacy batches are drawn as before" << LL_ENDL;
        return;
    }
    GLint vs_blocks = 0;
    glGetIntegerv(GL_MAX_VERTEX_SHADER_STORAGE_BLOCKS, &vs_blocks);
    if (vs_blocks < 2)
    {
        LL_INFOS_ONCE("ShaderLoading") << "FSPerfGPUDrivenLegacy: needs two shader storage blocks in the vertex shader, "
                                       << "the driver offers " << vs_blocks << "; legacy batches are drawn as before" << LL_ENDL;
        return;
    }
    // without bindless textures every batch would still bind its textures, which is most of what this saves
    if (!fsGDDBindlessSetup())
    {
        LL_INFOS_ONCE("ShaderLoading") << "FSPerfGPUDrivenLegacy: needs bindless textures (NVIDIA GPUs only); "
                                       << "legacy batches are drawn as before" << LL_ENDL;
        return;
    }

    for (U32 pass = 0; pass < P_COUNT; ++pass)
    {
        if (sPasses[pass].mStock->mShaderFiles.empty())
        {
            continue;
        }
        if (!create_program(sPasses[pass]))
        {
            LL_WARNS("ShaderLoading") << "FSPerfGPUDrivenLegacy: " << sPasses[pass].mName << " failed to build, "
                                      << "its batches are drawn as before" << LL_ENDL;
            sPasses[pass].mProgram->unload();
        }
    }
}

void fsGDDLegacyReleaseGL()
{
    sArena.releaseGL();
    GLuint buffers[4] = { sIndirectBuffer, sRecordBuffer, sMatrixBuffer, sTextureBuffer };
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
    sTextureBuffer = 0;
    if (sQuery)
    {
        glDeleteQueries(1, &sQuery);
        sQuery = 0;
    }
}

//-----------------------------------------------------------------------------
// The per pass texture handle cache

void fsGDDBeginTexturePass()
{
    if (++sPassSerial == 0)
    {
        sPassSerial = 1; // 0 is what a texture starts with
    }
}

U64 fsGDDPassTextureHandle(LLViewerTexture* tex, U32 frame)
{
    if (tex->mFSGDDPass != sPassSerial)
    {
        tex->mFSGDDPass = sPassSerial;
        tex->mFSGDDHandle = fsGDDTextureHandle(tex, frame);
        if (tex->mFSGDDHandle)
        {
            tex->setActive();
        }
    }
    return tex->mFSGDDHandle;
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
        // as LLRenderPass::applyModelMatrix and LLRender::syncMatrices compose them
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
            return 0;   // identity, what MM_TEXTURE0 holds outside pushBatch
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

    U64 handle_of(LLViewerTexture* tex, U32 frame)
    {
        return fsGDDPassTextureHandle(tex, frame);
    }

    void push_handle(U64 handle)
    {
        sTextures.push_back({ (U32)(handle & 0xFFFFFFFFull), (U32)(handle >> 32) });
    }

    // Turns the pass's batches into ops (in the stock order) and commands.
    void build(const PassInfo& pass, LLCullResult::drawinfo_iterator begin, LLCullResult::drawinfo_iterator end)
    {
        sOps.clear();
        sCommands.clear();
        sRecords.clear();
        sMatrices.clear();
        sTextures.clear();
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

        for (LLCullResult::drawinfo_iterator i = begin; i != end; )
        {
            LLDrawInfo* pparams = *i;
            LLCullResult::increment_iterator(i, end);
            if (!pparams)
            {   // the stock loops skip these too
                continue;
            }
            LLDrawInfo& params = *pparams;
            ++sStatBatches;

            S32 reason = -1;
            const U32 tex_first = (U32)sTextures.size();
            U32 tex_matrix = 0;
            const FSGDDArenaEntry* entry = nullptr;
            if (params.mAvatar.notNull() || params.mSkinInfo.notNull())
            {
                reason = R_RIGGED;
            }
            else if (!params.mCount || params.mVertexBuffer.isNull())
            {   // pushBatch draws nothing for these; it may still bind, so it gets them
                reason = R_EMPTY;
            }
            else
            {
                // the header: the minimum alpha pushMaskBatches sets for this batch, and the texture count
                const F32 cutoff = pass.mMask ? params.mAlphaMaskCutoff : 0.f;
                U32 cutoff_bits = 0;
                memcpy(&cutoff_bits, &cutoff, sizeof(cutoff_bits));
                sTextures.push_back({ cutoff_bits, 0 });

                // the textures pushBatch(params, true, true) binds, in unit order
                if (params.mTextureList.size() > 1)
                {   // every texture of the list on its unit, the texture matrix untouched (identity)
                    for (const LLPointer<LLViewerTexture>& tex : params.mTextureList)
                    {
                        if (tex.isNull())
                        {   // its unit keeps an earlier batch's texture on the stock path
                            reason = R_NO_TEXTURE;
                            break;
                        }
                        const U64 handle = handle_of(tex.get(), frame);
                        if (!handle)
                        {
                            reason = R_HANDLE;
                            break;
                        }
                        push_handle(handle);
                    }
                }
                else if (params.mTexture.isNull())
                {   // unit 0 unbound on the stock path
                    reason = R_NO_TEXTURE;
                }
                else
                {   // one texture on unit 0, and the texture matrix if there is one
                    const U64 handle = handle_of(params.mTexture.get(), frame);
                    if (!handle)
                    {
                        reason = R_HANDLE;
                    }
                    else
                    {
                        push_handle(handle);
                        tex_matrix = texture_matrix_index(params.mTextureMatrix.get());
                    }
                }

                if (reason < 0)
                {
                    sTextures[tex_first].mY = (U32)sTextures.size() - tex_first - 1;
                    U32 fail = 0;
                    if (pass.mNormals && !(params.mVertexBuffer->getTypeMask() & LLVertexBuffer::MAP_NORMAL))
                    {   // checked before the arena, which would copy the buffer only for it to be turned down here
                        reason = R_NORMALS;
                    }
                    else if (!(entry = sArena.find(params.mVertexBuffer.get(), frame, &fail)))
                    {
                        reason = R_ARENA + fail;
                    }
                    else if (pass.mNormals && !(entry->mTypeMask & LLVertexBuffer::MAP_NORMAL))
                    {
                        reason = R_NORMALS;
                    }
                    else if (params.mOffset + params.mCount > entry->mNumIndices || params.mEnd >= entry->mNumVerts)
                    {   // never for a valid batch; anything else is left to the stock path rather than read past the copy
                        reason = R_RANGE;
                    }
                }
            }

            if (reason >= 0)
            {
                sTextures.resize(tex_first);
                ++sStatReasons[reason];
                sOps.push_back({ true, 0, 0, &params });
                continue;
            }

            ++sStatDrawn;
            const U32 command = (U32)sCommands.size();
            sCommands.push_back({ params.mCount, 1, entry->mIndex + params.mOffset, (GLint)entry->mVertex, command });
            sRecords.push_back({ matrix_index(params.mModelMatrix, proj, view), tex_matrix, tex_first, 0 });
            sCommandParams.push_back(&params);

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
    void setup_inputs(U32 pass)
    {
        // the attributes go into the global vertex array object, never a buffer's own
        LLVertexBuffer::fsUseDefaultVAO();
        glBindBufferBase(GL_SHADER_STORAGE_BUFFER, FS_GDD_LEGACY_MATRIX_BINDING, sMatrixBuffer);
        glBindBufferBase(GL_SHADER_STORAGE_BUFFER, FS_GDD_LEGACY_TEXTURE_BINDING, sTextureBuffer);
        glBindBuffer(GL_DRAW_INDIRECT_BUFFER, sIndirectBuffer);
        const GLuint record = (GLuint)sRecordAttrib[pass];
        glBindBuffer(GL_ARRAY_BUFFER, sRecordBuffer);
        glEnableVertexAttribArray(record);
        glVertexAttribIPointer(record, 4, GL_UNSIGNED_INT, sizeof(Record), nullptr);
        glVertexAttribDivisor(record, 1);
        // every attribute the program reads (bind_inputs checked the arena holds them), texture_index included
        sArena.bindForDraw(sPasses[pass].mProgram->mAttributeMask);
    }

    // undo setup_inputs: the program's attribute mask doesn't have the record's location, so LLVertexBuffer
    // expects it off and undivided
    void finish_inputs(U32 pass)
    {
        const GLuint record = (GLuint)sRecordAttrib[pass];
        glVertexAttribDivisor(record, 0);
        glDisableVertexAttribArray(record);
        glBindBufferBase(GL_SHADER_STORAGE_BUFFER, FS_GDD_LEGACY_MATRIX_BINDING, 0);
        glBindBufferBase(GL_SHADER_STORAGE_BUFFER, FS_GDD_LEGACY_TEXTURE_BINDING, 0);  // also unbinds the generic binding
        glBindBuffer(GL_DRAW_INDIRECT_BUFFER, 0);
        LLVertexBuffer::unbind();   // the next setBuffer() binds and sets up its own attributes again
    }

    // the stock way, as pushBatches / pushMaskBatches draw a batch
    void push_stock(LLRenderPass& pool, const PassInfo& pass, LLDrawInfo& params)
    {
        if (pass.mMask)
        {
            LLGLSLShader::sCurBoundShaderPtr->setMinimumAlpha(params.mAlphaMaskCutoff);
        }
        pool.pushBatch(params, true, true);
    }

    // Draws the ops. The stock program is bound on entry and on return. False if the GPU-driven program
    // couldn't be bound before anything was drawn (then nothing was).
    bool execute(LLRenderPass& pool, U32 pass_index, U32& calls, U32& switches)
    {
        LL_PROFILE_ZONE_NAMED_CATEGORY_DRAWPOOL("gdd legacy execute");
        LL_PROFILE_GPU_ZONE("gdd legacy");
        const PassInfo& pass = sPasses[pass_index];

        upload(sIndirectBuffer, GL_DRAW_INDIRECT_BUFFER, sCommands.data(), sCommands.size() * sizeof(DrawCommand));
        upload(sRecordBuffer, GL_ARRAY_BUFFER, sRecords.data(), sRecords.size() * sizeof(Record));
        upload(sMatrixBuffer, GL_SHADER_STORAGE_BUFFER, sMatrices.data(), sMatrices.size() * sizeof(glm::mat4));
        upload(sTextureBuffer, GL_SHADER_STORAGE_BUFFER, sTextures.data(), sTextures.size() * sizeof(TexEntry));
        glBindBuffer(GL_SHADER_STORAGE_BUFFER, 0);
        glBindBuffer(GL_DRAW_INDIRECT_BUFFER, 0);
        glBindBuffer(GL_ARRAY_BUFFER, 0);
        LLVertexBuffer::sGLRenderBuffer = 0;    // or the next setBuffer of the buffer it named skips binding it

        LLGLSLShader* stock = pass.mStock;
        bool gdd = false;   // the GPU-driven program is bound
        bool drew = false;

        for (const Op& op : sOps)
        {
            if (op.mStock)
            {
                if (gdd)
                {
                    finish_inputs(pass_index);
                    stock->bind();
                    gdd = false;
                    ++switches;
                }
                push_stock(pool, pass, *op.mParams);
                drew = true;
                continue;
            }

            if (!gdd)
            {
                if (!bind_program(pass_index))
                {
                    stock->bind();
                    if (!drew)
                    {
                        return false;
                    }
                    // not expected (fsGDDDrawLegacy bound the program once already); the rest of the pass goes
                    // the stock way, in order
                    LL_WARNS_ONCE("Rendering") << "FSPerfGPUDrivenLegacy: a program failed to bind mid pass" << LL_ENDL;
                    for (size_t i = &op - sOps.data(); i < sOps.size(); ++i)
                    {
                        const Op& rest = sOps[i];
                        if (rest.mStock)
                        {
                            push_stock(pool, pass, *rest.mParams);
                        }
                        else
                        {
                            for (U32 c = rest.mFirst; c < rest.mFirst + rest.mCount; ++c)
                            {
                                push_stock(pool, pass, *sCommandParams[c]);
                            }
                        }
                    }
                    return true;
                }
                ++switches;
                gdd = true;
                setup_inputs(pass_index);
            }
            glMultiDrawElementsIndirect(GL_TRIANGLES, GL_UNSIGNED_SHORT, (const void*)((size_t)op.mFirst * sizeof(DrawCommand)),
                                        (GLsizei)op.mCount, 0);
            ++calls;
            drew = true;
        }
        if (gdd)
        {
            finish_inputs(pass_index);
            stock->bind();
        }
        STOP_GLERROR;
        return true;
    }

    // FSPerfGPUDrivenLegacyVerify: the batches drawn from the arena again, the stock way, colour and depth
    // writes off, depth test GL_LESS against what the multi-draws left. The stock program must be bound.
    void verify_against_stock(LLRenderPass& pool, const PassInfo& pass)
    {
        if (sCommandParams.empty())
        {
            return;
        }
        LL_PROFILE_ZONE_NAMED_CATEGORY_DRAWPOOL("gdd legacy verify");

        GLenum error = glGetError();
        if (error != GL_NO_ERROR)
        {
            static LLFrameTimer warn_timer;
            if (warn_timer.getElapsedTimeF32() > 5.f)
            {
                warn_timer.reset();
                LL_WARNS("Rendering") << "FSPerfGPUDrivenLegacyVerify: GL error 0x" << std::hex << error << std::dec
                                      << " after the GPU-driven " << pass.mName << " pass" << LL_ENDL;
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
                        LL_WARNS("Rendering") << "FSPerfGPUDrivenLegacyVerify: the arena copy of a vertex buffer ("
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
            ++sStatUnverified;
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
                push_stock(pool, pass, *params);
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
                LL_WARNS("Rendering") << "FSPerfGPUDrivenLegacyVerify: " << samples << " samples of " << sCommandParams.size()
                                      << " stock batches (" << pass.mName << ") are nearer than the multi-draw depth; "
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
        LL_INFOS("FSGPUDrivenLegacy") << "GPU-driven legacy, last 10 s: " << sStatPasses << " passes, " << sStatBatches
                                      << " batches (" << sStatDrawn << " from the arena) in " << sStatCalls
                                      << " multi-draw calls, " << sStatSwitches << " program switches; stock path for: "
                                      << (reasons.tellp() ? reasons.str() : std::string("none")) << "; arena "
                                      << sArena.vertexMB() << " MB vertices, " << sArena.indexMB() << " MB indices, "
                                      << sArena.entries() << " buffers, " << sArena.uploads() << " copies; "
                                      << sStatMismatches << " verify mismatches, " << sStatUnverified
                                      << " passes without depth writes (occlusion check skipped)" << LL_ENDL;
        sStatPasses = sStatBatches = sStatDrawn = sStatCalls = sStatSwitches = sStatUnverified = 0;
        sArena.clearCounts();
    }
}

bool fsGDDDrawLegacy(LLRenderPass& pool, U32 type)
{
    // before any early return, so idle handles go non resident and away also while the PBR pass is off;
    // it does something every 120 frames, and is called once a frame rather than once per pass
    static U32 s_swept_frame = U32_MAX;
    const U32 sweep_frame = LLFrameTimer::getFrameCount();
    if (sweep_frame != s_swept_frame)
    {
        s_swept_frame = sweep_frame;
        fsGDDSweepHandles(sweep_frame);
    }

    static LLCachedControl<bool> enabled(gSavedSettings, "FSPerfGPUDrivenLegacy", false);
    if (!enabled || !gGLManager.mFSHasGPUDrivenDraws || !fsGDDBindlessReady() || LLPipeline::sShadowRender ||
        LLPipeline::sRenderingHUDs)
    {
        return false;
    }
    U32 pass_index = P_COUNT;
    for (U32 p = 0; p < P_COUNT; ++p)
    {
        if (sPasses[p].mType == type)
        {
            pass_index = p;
            break;
        }
    }
    if (pass_index == P_COUNT)
    {
        return false;
    }
    const PassInfo& pass = sPasses[pass_index];
    // the HUD programs and anything else bound in place of the stock one keep the stock path
    if (LLGLSLShader::sCurBoundShaderPtr != pass.mStock || !program_usable(pass_index))
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
    static LLCachedControl<bool> verify(gSavedSettings, "FSPerfGPUDrivenLegacyVerify", false);
    static LLCachedControl<bool> stats(gSavedSettings, "FSPerfGPUDrivenLegacyStats", false);

    // The first bind finds the program's inputs; do that now, so a failure leaves the whole pass to the
    // stock loop rather than part of it
    if (!sInputsReady[pass_index])
    {
        const bool ready = bind_program(pass_index);
        pass.mStock->bind();
        if (!ready)
        {
            return false;
        }
    }

    // The environment uniforms (LLEnvironment marks the programs of the shader list dirty when they change,
    // and the stock program took them when the pool bound it); ours isn't in that list, so it takes the
    // current values at its first bind in each pass
    pass.mProgram->mUniformsDirty = true;

    {
        LL_PROFILE_ZONE_NAMED_CATEGORY_DRAWPOOL("gdd legacy build");
        build(pass, begin, end);
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

    U32 calls = 0;
    U32 switches = 0;
    if (!execute(pool, pass_index, calls, switches))
    {
        return false;
    }

    if (verify)
    {
        verify_against_stock(pool, pass);
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
