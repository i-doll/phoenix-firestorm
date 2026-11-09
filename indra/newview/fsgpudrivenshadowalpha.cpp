/**
 * @file fsgpudrivenshadowalpha.cpp
 * @brief GPU-driven alpha tested shadow casters: multi-draws with bindless textures (FSPerfGPUDrivenShadowAlpha)
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

// The opaque shadow casters go out as multi-draws (FSPerfGPUDrivenDraws), but the alpha tested ones still cost,
// per batch and per shadow map, a model matrix, a minimum alpha uniform, one to sixteen texture binds or a bind
// and a texture matrix, a vertex buffer bind and a glDrawRangeElements: the alpha masked, fullbright alpha
// masked and material alpha masked pools, grass and GLTF alpha masked materials of every sun cascade and
// projector, and with FSPerfShadowSinglePass the recorded alpha masked kinds in the atlas pass.
//
// With FSPerfGPUDrivenShadowAlpha on, those batches come out of a vertex arena instead (fsgddarena.h: a GPU side
// copy of the positions, whose w is the texture index, texture coordinates and colours, and the 16 bit indices
// of their vertex buffers), and each is one indirect draw command plus a record: its matrix (the model view
// projection the stock path would compute, or for the single pass the model matrix, computed the same way),
// its texture matrix (identity, the batch's animation where pushBatch or pushGLTFBatch would load it), its GLTF
// base colour transform, how the stock program would test it, and where its textures start in a storage buffer:
// the minimum alpha the stock path would set, the texture count, and the bindless handle of every texture the
// stock path would bind, in unit order (fsgpudrivenpbr.cpp's handles: a sampler per texture state, dropped
// before LLImageGL changes a texture). fsGPUDrivenShadowAlphaV.glsl picks the handle the vertex's texture index
// selects, as diffuseLookup picks a unit, and fsGPUDrivenShadowAlphaF.glsl repeats the alpha test of the stock
// fragment shader for the batch's kind: shadowAlphaMaskF.glsl (with and without the vertex alpha, the 0.05 and
// 0.88 thresholds and the dither of semi transparent texels), treeShadowF.glsl or pbrShadowAlphaMaskF.glsl.
//
// A shadow map is depth only (colour writes off, GL_LESS), so the order of the casters doesn't matter: the batches
// are collected while renderShadow walks its alpha passes, the ones that can't be taken are drawn there by the
// stock code, and the collected ones go out at the end of the alpha casters (before the coloured shadow casters,
// which test against the finished depth), one glMultiDrawElementsIndirect for the single sided casters and one
// with face culling off for double sided GLTF materials. In the single pass atlas, one per cascade set, each
// command instanced once per cascade like the stock single pass draws.
//
// Left to the stock path: rigged batches, alpha blended casters (RenderAlphaBlendShadows), anything whose
// texture has no handle (not loaded, one or two channels, options pending, media), a missing texture, GLTF
// materials that aren't alpha masked, 32 bit indices, buffers mapped or lacking texture coordinates (or
// colours, where the test reads the vertex alpha), arena full or its copy budget used up.
//
// FSPerfGPUDrivenDrawsVerify: after each multi-draw the same batches are drawn the stock way with depth writes
// off, a small polygon offset and a GL_SAMPLES_PASSED query; a sample that passes is a spot where the stock path
// would have put a nearer caster, which is logged. Statistics every 10 seconds with FSPerfShadowBatchStats.

#include "llviewerprecompiledheaders.h"

#include "fsgpudrivenshadowalpha.h"

#include "fsgddarena.h"
#include "fsgpudrivenlegacy.h"
#include "fsgpudrivenpbr.h"
#include "fsshadowsinglepass.h"

#include "lldrawpool.h"
#include "llenvironment.h"
#include "llfetchedgltfmaterial.h"
#include "llframetimer.h"
#include "llgl.h"
#include "llglslshader.h"
#include "llrender.h"
#include "llspatialpartition.h"
#include "llvertexbuffer.h"
#include "llviewercontrol.h"
#include "llviewershadermgr.h"
#include "llviewertexture.h"

#include <algorithm>
#include <cstring>
#include <sstream>
#include <unordered_map>

LLGLSLShader gFSGDDShadowAlphaProgram;
LLGLSLShader gFSGDDShadowAlphaSPProgram;

namespace
{
    // the legacy and PBR passes use the same bindings; each pass binds its buffers and clears them again
    constexpr GLuint FS_GDD_SA_MATRIX_BINDING = 5;     // shader storage binding of FSGDDShadowAlphaMatrices
    constexpr GLuint FS_GDD_SA_TEXTURE_BINDING = 6;    // and of FSGDDShadowAlphaTextures
    constexpr GLuint FS_GDD_SA_RECORD_DIVISOR = 4;     // instances per record step: at least the most cascades a command has
    constexpr U32 FS_GDD_SA_VERIFY_SAMPLES = 64;       // samples a verify pass may let through before it warns
    // the attributes the arena can feed; a program wanting any other is not used
    constexpr U32 FS_GDD_SA_ATTRIBS = LLVertexBuffer::MAP_VERTEX | LLVertexBuffer::MAP_TEXCOORD0 | LLVertexBuffer::MAP_COLOR |
                                      LLVertexBuffer::MAP_TEXTURE_INDEX;

    // record modes, as fsGPUDrivenShadowAlphaF.glsl tests them (EFSGDDShadowAlphaKind)
    constexpr U32 MODE_GLTF = FS_GDD_SA_GLTF_MASK;

    const LLStaticHashedString sClip("fs_shadow_clip");
    const LLStaticHashedString sCascade("fs_shadow_cascade");
    const LLStaticHashedString sWidth("fs_shadow_width");

    // positions (texture index in w), texture coordinates, colours where the buffer has them; an eighth of
    // FSPerfGPUDrivenShadowAlphaArenaMB for indices
    FSGDDArena sArena("FSPerfGPUDrivenShadowAlpha arena",
                      { LLVertexBuffer::TYPE_VERTEX, LLVertexBuffer::TYPE_TEXCOORD0, LLVertexBuffer::TYPE_COLOR },
                      "FSPerfGPUDrivenShadowAlphaArenaMB", 256, 8, LLVertexBuffer::MAP_COLOR);

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
        U32 mMatrix;    // model view projection (single pass: model)
        U32 mTexMatrix;
        U32 mTextures;  // first entry in FSGDDShadowAlphaTextures: the header, then the handles
        U32 mMode;      // bits 0-1: kind; bits 2-31: GLTF base colour transform (a matrix whose first two columns hold it)
    };

    struct TexEntry     // uvec2, std430 stride 8
    {
        U32 mX;         // header: minimum alpha (float bits); handle: low 32 bits
        U32 mY;         // header: texture count; handle: high 32 bits
    };
    static_assert(sizeof(TexEntry) == 8, "FSGDDShadowAlphaTextures layout");

    struct Item
    {
        LLDrawInfo* mParams;
        U32 mMask;                              // single pass cascade set, 1 otherwise
        U32 mKind;
        F32 mCutoff;                            // the minimum alpha the stock path would set
        U32 mTextures;                          // first entry in sTextures
        const LLMatrix4* mTexMatrix;            // null: identity
        const LLFetchedGLTFMaterial* mMaterial; // GLTF: base colour transform
        U32 mVertex;                            // the arena copy: first vertex
        U32 mIndex;                             // and first index
        bool mDoubleSided;                      // GLTF double sided: no face culling
    };

    struct Segment
    {
        U32 mMask;
        bool mDoubleSided;
        U32 mFirst;                             // first command
        U32 mCount;
    };

    // why a batch went the stock way
    enum EReason
    {
        R_RIGGED = 0,
        R_EMPTY,
        R_NO_TEXTURE,
        R_HANDLE,
        R_MATERIAL,
        R_COLOR,
        R_RANGE,
        R_ARENA,                                // + FSGDDArena::EFail
        R_COUNT = R_ARENA + FSGDDArena::FAIL_COUNT
    };
    const char* reason_name(U32 reason)
    {
        static const char* names[] = { "rigged", "empty", "no texture", "texture without handle", "not an alpha mask material",
                                       "no vertex colours", "index range" };
        return reason < R_ARENA ? names[reason] : FSGDDArena::failName(reason - R_ARENA);
    }

    GLuint sIndirectBuffer = 0;
    GLuint sRecordBuffer = 0;
    GLuint sMatrixBuffer = 0;
    GLuint sTextureBuffer = 0;
    GLuint sQuery = 0;
    GLint sRecordAttrib[2] = { -1, -1 };    // location of fs_gdd_draw: plain, single pass
    S32 sInputsReady[2] = { 0, 0 };         // bind_inputs done: 0 not yet, 1 yes, -1 failed

    bool sActive = false;                   // between a successful fsGDDShadowAlphaBegin and the flush
    U32 sFrame = 0;
    std::vector<Item> sItems;
    std::vector<TexEntry> sTextures;
    std::vector<DrawCommand> sCommands;
    std::vector<Record> sRecords;
    std::vector<Segment> sSegments;
    std::vector<glm::mat4> sMatrices;
    std::unordered_map<const LLMatrix4*, U32> sMatrixIndex;
    std::unordered_map<const LLMatrix4*, U32> sTexMatrixIndex;
    std::unordered_map<const LLFetchedGLTFMaterial*, U32> sTransformIndex;

    // stats, every 10 seconds with FSPerfShadowBatchStats or FSPerfGPUDrivenDrawsVerify
    U64 sStatBatches = 0;
    U64 sStatDrawn = 0;
    U64 sStatCalls = 0;
    U64 sStatPasses = 0;
    U64 sStatReasons[R_COUNT] = {};
    U64 sStatMismatches = 0;

    LLGLSLShader& program(U32 which)
    {
        return which ? gFSGDDShadowAlphaSPProgram : gFSGDDShadowAlphaProgram;
    }
}

//-----------------------------------------------------------------------------
// Programs

namespace
{
    bool create_program(LLGLSLShader& shader, const char* name, S32 shader_level, S32 single_pass_ext)
    {
        shader.mName = name;
        shader.mShaderFiles.clear();
        shader.clearPermutations();
        shader.mFeatures = LLShaderFeatures();
        // no indexed texture units: the vertex shader reads texture_index itself and the fragment shader samples a handle
        shader.mFeatures.mIndexedTextureChannels = 0;
        shader.mShaderLevel = shader_level;
        shader.mShaderFiles.push_back(std::make_pair("deferred/fsGPUDrivenShadowAlphaV.glsl", GL_VERTEX_SHADER));
        shader.mShaderFiles.push_back(std::make_pair("deferred/fsGPUDrivenShadowAlphaF.glsl", GL_FRAGMENT_SHADER));
        if (single_pass_ext > 0)
        {
            shader.addPermutation("FS_SHADOW_SINGLE_PASS", llformat("%d", single_pass_ext));
        }
        return shader.createShader();
    }

    // the storage block bindings, where the driver put the per command record, and whether the arena can feed
    // every attribute the program reads
    bool bind_inputs(LLGLSLShader& shader, GLint& record_attrib)
    {
        const GLuint matrices = glGetProgramResourceIndex(shader.mProgramObject, GL_SHADER_STORAGE_BLOCK, "FSGDDShadowAlphaMatrices");
        const GLuint textures = glGetProgramResourceIndex(shader.mProgramObject, GL_SHADER_STORAGE_BLOCK, "FSGDDShadowAlphaTextures");
        record_attrib = glGetAttribLocation(shader.mProgramObject, "fs_gdd_draw");
        if (matrices == GL_INVALID_INDEX || textures == GL_INVALID_INDEX || record_attrib < 0 ||
            (shader.mAttributeMask & ~FS_GDD_SA_ATTRIBS) || !(shader.mAttributeMask & LLVertexBuffer::MAP_VERTEX))
        {
            return false;
        }
        glShaderStorageBlockBinding(shader.mProgramObject, matrices, FS_GDD_SA_MATRIX_BINDING);
        glShaderStorageBlockBinding(shader.mProgramObject, textures, FS_GDD_SA_TEXTURE_BINDING);
        return true;
    }

    // Binds the program; the first time, also finds its inputs. False (for good) when they aren't there.
    bool bind_program(U32 which)
    {
        LLGLSLShader& shader = program(which);
        shader.bind();
        if (LLGLSLShader::sCurBoundShaderPtr != &shader)
        {
            return false;
        }
        if (!sInputsReady[which])
        {
            sInputsReady[which] = bind_inputs(shader, sRecordAttrib[which]) ? 1 : -1;
            if (sInputsReady[which] < 0)
            {
                LL_WARNS("Rendering") << "FSPerfGPUDrivenShadowAlpha: " << shader.mName << " lacks its storage blocks or draw "
                                      << "record attribute, or reads an attribute the arena doesn't hold; alpha tested "
                                      << "shadow casters are drawn as before" << LL_ENDL;
            }
        }
        return sInputsReady[which] > 0;
    }

    void rebind(LLGLSLShader* shader)
    {
        if (shader)
        {
            shader->bind();
        }
        else
        {
            LLGLSLShader::unbind();
        }
    }
}

void fsGDDShadowAlphaReleaseGL()
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
    sItems.clear();
    sTextures.clear();
    sActive = false;
}

void fsGDDShadowAlphaUnloadShaders()
{
    gFSGDDShadowAlphaProgram.unload();
    gFSGDDShadowAlphaSPProgram.unload();
    for (U32 i = 0; i < 2; ++i)
    {
        sInputsReady[i] = 0;
        sRecordAttrib[i] = -1;
    }
    fsGDDShadowAlphaReleaseGL();
}

void fsGDDShadowAlphaLoadShaders(S32 shader_level, bool use_shadow)
{
    fsGDDShadowAlphaUnloadShaders();

    static LLCachedControl<bool> enabled(gSavedSettings, "FSPerfGPUDrivenShadowAlpha", false);
    if (!enabled || !use_shadow)
    {
        return;
    }
    if (!gGLManager.mFSHasGPUDrivenDraws)
    {
        LL_INFOS_ONCE("ShaderLoading") << "FSPerfGPUDrivenShadowAlpha: needs OpenGL 4.3 with GL_ARB_multi_draw_indirect and "
                                       << "GL_ARB_shader_storage_buffer_object; alpha tested shadow casters are drawn as before" << LL_ENDL;
        return;
    }
    GLint vs_blocks = 0;
    glGetIntegerv(GL_MAX_VERTEX_SHADER_STORAGE_BLOCKS, &vs_blocks);
    if (vs_blocks < 2)
    {
        LL_INFOS_ONCE("ShaderLoading") << "FSPerfGPUDrivenShadowAlpha: needs two shader storage blocks in the vertex shader, "
                                       << "the driver offers " << vs_blocks << "; alpha tested shadow casters are drawn as before" << LL_ENDL;
        return;
    }
    // without bindless textures every caster would still bind its textures
    if (!fsGDDBindlessSetup())
    {
        LL_INFOS_ONCE("ShaderLoading") << "FSPerfGPUDrivenShadowAlpha: needs bindless textures (NVIDIA GPUs only); "
                                       << "alpha tested shadow casters are drawn as before" << LL_ENDL;
        return;
    }

    if (!create_program(gFSGDDShadowAlphaProgram, "FS GPU-Driven Shadow Alpha Shader", shader_level, 0))
    {
        LL_WARNS("ShaderLoading") << "FSPerfGPUDrivenShadowAlpha: the multi-draw alpha shadow program failed to build, "
                                  << "alpha tested shadow casters are drawn as before" << LL_ENDL;
        gFSGDDShadowAlphaProgram.unload();
    }
    // the single pass variant only when FSPerfShadowSinglePass has its own programs
    if (gFSShadowSinglePassShaders && gGLManager.mFSVertexViewportIndex > 0 &&
        !create_program(gFSGDDShadowAlphaSPProgram, "FS GPU-Driven Single Pass Shadow Alpha Shader", shader_level,
                        gGLManager.mFSVertexViewportIndex))
    {
        LL_WARNS("ShaderLoading") << "FSPerfGPUDrivenShadowAlpha: the single pass multi-draw alpha shadow program failed to "
                                  << "build, single pass alpha tested casters are drawn as before" << LL_ENDL;
        gFSGDDShadowAlphaSPProgram.unload();
    }
}

//-----------------------------------------------------------------------------
// Collecting

bool fsGDDShadowAlphaBegin(bool single_pass)
{
    sActive = false;
    sItems.clear();
    sTextures.clear();

    static LLCachedControl<bool> enabled(gSavedSettings, "FSPerfGPUDrivenShadowAlpha", false);
    if (!enabled || !gGLManager.mFSHasGPUDrivenDraws || !fsGDDBindlessReady())
    {
        return false;
    }
    const U32 which = single_pass ? 1 : 0;
    if (!program(which).isComplete() || sInputsReady[which] < 0)
    {
        return false;
    }
    if (!sInputsReady[which])
    {   // the first bind finds the program's inputs; do that now, so a failure leaves the whole pass to the stock path
        LLGLSLShader* previous = LLGLSLShader::sCurBoundShaderPtr;
        const bool ready = bind_program(which);
        rebind(previous);
        if (!ready)
        {
            return false;
        }
    }

    sFrame = LLFrameTimer::getFrameCount();
    sArena.beginFrame(sFrame);
    // idle handles go non resident and away also while the other GPU-driven passes are off; every 120 frames
    fsGDDSweepHandles(sFrame);
    fsGDDBeginTexturePass();    // the texture handle lookups of this pass (fsGDDPassTextureHandle)
    sActive = true;
    return true;
}

namespace
{
    // The texture's handle, looked up once per pass. A texture with a handle is set active as bindFast would.
    U64 handle_of(LLViewerTexture* tex)
    {
        return fsGDDPassTextureHandle(tex, sFrame);
    }

    void push_handle(U64 handle)
    {
        sTextures.push_back({ (U32)(handle & 0xFFFFFFFFull), (U32)(handle >> 32) });
    }

    // Takes a batch into the pending list if the GPU-driven program can draw it as the stock one would.
    bool collect(U32 kind, LLDrawInfo* pparams, U32 mask, F32 fixed_cutoff)
    {
        ++sStatBatches;
        if (!pparams)
        {
            return false;
        }
        LLDrawInfo& params = *pparams;
        S32 reason = -1;
        const U32 tex_first = (U32)sTextures.size();
        F32 cutoff = (fixed_cutoff < 0.f) ? params.mAlphaMaskCutoff : fixed_cutoff;
        const LLMatrix4* tex_matrix = nullptr;
        const LLFetchedGLTFMaterial* material = nullptr;
        bool double_sided = false;
        bool needs_color = false;

        if (params.mAvatar.notNull() || params.mSkinInfo.notNull())
        {
            reason = R_RIGGED;
        }
        else if (!params.mCount || params.mVertexBuffer.isNull())
        {   // the stock loops draw nothing for these
            reason = R_EMPTY;
        }
        else if (kind == FS_GDD_SA_GLTF_MASK)
        {   // LLFetchedGLTFMaterial::bind in a shadow pass: the cutoff (alpha masked materials only), the base colour
            // texture (or the media texture, or white) and its transform; pushGLTFBatch: the texture animation
            const LLFetchedGLTFMaterial* mat = params.mGLTFMaterial.get();
            if (!mat || mat->mAlphaMode != LLGLTFMaterial::ALPHA_MODE_MASK)
            {   // the stock bind would leave the minimum alpha of the batch before
                reason = R_MATERIAL;
            }
            else
            {
                LLViewerTexture* tex = params.mTexture.notNull() ? params.mTexture.get() :
                                       mat->mBaseColorTexture.notNull() ? (LLViewerTexture*)mat->mBaseColorTexture.get() :
                                       (LLViewerTexture*)LLViewerFetchedTexture::sWhiteImagep.get();
                const U64 handle = tex ? handle_of(tex) : 0;
                if (!handle)
                {
                    reason = tex ? R_HANDLE : R_NO_TEXTURE;
                }
                else
                {
                    sTextures.push_back({ 0, 1 });
                    push_handle(handle);
                    cutoff = mat->mAlphaCutoff;
                    tex_matrix = params.mTextureMatrix.get();
                    material = mat;
                    double_sided = mat->mDoubleSided;
                    needs_color = true;
                }
            }
        }
        else if ((kind == FS_GDD_SA_ALPHA_MASK || kind == FS_GDD_SA_FULLBRIGHT_MASK) && params.mTextureList.size() > 1)
        {   // pushBatch(params, true, true): every texture of the list on its unit, the texture matrix untouched (identity);
            // with a single indexed channel the stock program only ever samples the first
            sTextures.push_back({ 0, 0 });
            const size_t count = LLGLSLShader::sIndexedTextureChannels > 1 ? params.mTextureList.size() : 1;
            for (size_t i = 0; i < count; ++i)
            {
                LLViewerTexture* tex = params.mTextureList[i].get();
                if (!tex)
                {   // its unit keeps an earlier batch's texture on the stock path
                    reason = R_NO_TEXTURE;
                    break;
                }
                const U64 handle = handle_of(tex);
                if (!handle)
                {
                    reason = R_HANDLE;
                    break;
                }
                push_handle(handle);
            }
            needs_color = kind == FS_GDD_SA_ALPHA_MASK;
        }
        else if (params.mTexture.isNull())
        {   // unit 0 unbound on the stock path
            reason = R_NO_TEXTURE;
        }
        else
        {   // one texture on unit 0, and the texture matrix if there is one
            const U64 handle = handle_of(params.mTexture.get());
            if (!handle)
            {
                reason = R_HANDLE;
            }
            else
            {
                sTextures.push_back({ 0, 1 });
                push_handle(handle);
                tex_matrix = params.mTextureMatrix.get();
                needs_color = kind == FS_GDD_SA_ALPHA_MASK;
            }
        }

        const FSGDDArenaEntry* entry = nullptr;
        if (reason < 0)
        {
            U32 fail = 0;
            if (needs_color && !(params.mVertexBuffer->getTypeMask() & LLVertexBuffer::MAP_COLOR))
            {   // checked before the arena, which would copy the buffer only for it to be turned down here
                reason = R_COLOR;
            }
            else if (!(entry = sArena.find(params.mVertexBuffer.get(), sFrame, &fail)))
            {
                reason = R_ARENA + fail;
            }
            else if (needs_color && !(entry->mTypeMask & LLVertexBuffer::MAP_COLOR))
            {
                reason = R_COLOR;
            }
            else if (params.mOffset + params.mCount > entry->mNumIndices || params.mEnd >= entry->mNumVerts)
            {   // never for a valid batch; anything else is left to the stock path rather than read past the copy
                reason = R_RANGE;
            }
        }

        if (reason >= 0)
        {
            sTextures.resize(tex_first);
            ++sStatReasons[reason];
            return false;
        }

        U32 cutoff_bits = 0;
        memcpy(&cutoff_bits, &cutoff, sizeof(cutoff_bits));
        sTextures[tex_first].mX = cutoff_bits;
        sTextures[tex_first].mY = (U32)sTextures.size() - tex_first - 1;
        sItems.push_back({ pparams, mask, kind, cutoff, tex_first, tex_matrix, material, entry->mVertex, entry->mIndex, double_sided });
        ++sStatDrawn;
        return true;
    }
}

void fsGDDShadowAlphaCollect(U32 kind, std::vector<LLDrawInfo*>& batches, F32 fixed_cutoff)
{
    if (!sActive || kind >= FS_GDD_SA_KINDS)
    {
        return;
    }
    LL_PROFILE_ZONE_SCOPED_CATEGORY_PIPELINE;
    size_t out = 0;
    for (size_t i = 0; i < batches.size(); ++i)
    {
        if (!collect(kind, batches[i], 1, fixed_cutoff))
        {
            batches[out++] = batches[i];
        }
    }
    batches.resize(out);
}

void fsGDDShadowAlphaCollectSP(U32 kind, std::vector<LLPipeline::FSShadowSPEntry>& list)
{
    if (!sActive || kind >= FS_GDD_SA_KINDS)
    {
        return;
    }
    LL_PROFILE_ZONE_SCOPED_CATEGORY_PIPELINE;
    size_t out = 0;
    for (size_t i = 0; i < list.size(); ++i)
    {
        if (!collect(kind, list[i].mParams, list[i].mMask, -1.f))
        {
            list[out++] = list[i];
        }
    }
    list.resize(out);
}

//-----------------------------------------------------------------------------
// Drawing

namespace
{
    U32 matrix_index(const LLMatrix4* model, const glm::mat4& proj, const glm::mat4& view)
    {
        auto it = sMatrixIndex.find(model);
        if (it != sMatrixIndex.end())
        {
            return it->second;
        }
        // as LLRenderPass::applyModelMatrix and LLRender::syncMatrices compose it
        glm::mat4 modelview = view;
        if (model)
        {
            modelview *= glm::make_mat4((const F32*)model->mMatrix);
        }
        const U32 index = (U32)sMatrices.size();
        sMatrices.push_back(proj * modelview);
        sMatrixIndex.emplace(model, index);
        return index;
    }

    U32 texture_matrix_index(const LLMatrix4* texture_matrix)
    {
        if (!texture_matrix)
        {
            return 0;   // identity, what MM_TEXTURE0 holds outside pushBatch and pushGLTFBatch
        }
        auto it = sTexMatrixIndex.find(texture_matrix);
        if (it != sTexMatrixIndex.end())
        {
            return it->second;
        }
        const U32 index = (U32)sMatrices.size();
        sMatrices.push_back(glm::make_mat4((const F32*)texture_matrix->mMatrix));
        sTexMatrixIndex.emplace(texture_matrix, index);
        return index;
    }

    // the base colour transform LLFetchedGLTFMaterial::bind uploads (texture_base_color_transform, vec4[2]), as
    // the first two columns of a matrix
    U32 transform_index(const LLFetchedGLTFMaterial* mat)
    {
        auto it = sTransformIndex.find(mat);
        if (it != sTransformIndex.end())
        {
            return it->second;
        }
        F32 packed[LLGLTFMaterial::TextureTransform::PACK_SIZE];
        mat->mTextureTransform[LLGLTFMaterial::GLTF_TEXTURE_INFO_BASE_COLOR].getPacked(packed);
        glm::mat4 holder(0.f);
        memcpy(glm::value_ptr(holder), packed, sizeof(packed));
        const U32 index = (U32)sMatrices.size();
        sMatrices.push_back(holder);
        sTransformIndex.emplace(mat, index);
        return index;
    }

    U32 popcount4(U32 mask)
    {
        return (mask & 1) + ((mask >> 1) & 1) + ((mask >> 2) & 1) + ((mask >> 3) & 1);
    }

    void set_cascades(LLGLSLShader* shader, U32 mask)
    {
        F32 cascades[4] = { 0.f, 0.f, 0.f, 0.f };
        U32 n = 0;
        for (U32 c = 0; c < 4; ++c)
        {
            if (mask & (1u << c))
            {
                cascades[n++] = (F32)c;
            }
        }
        shader->uniform4f(sCascade, cascades[0], cascades[1], cascades[2], cascades[3]);
        LLVertexBuffer::sFSInstanceCount = n;   // the stock instanced draws; the multi-draw commands carry their own count
    }

    // Turns the pending items (sorted by face culling, then cascade set) into commands, records and segments.
    void build(bool single_pass)
    {
        std::sort(sItems.begin(), sItems.end(), [](const Item& a, const Item& b)
        {
            if (a.mDoubleSided != b.mDoubleSided)
            {
                return b.mDoubleSided;
            }
            return a.mMask < b.mMask;
        });

        sCommands.clear();
        sRecords.clear();
        sSegments.clear();
        sMatrices.clear();
        sMatrixIndex.clear();
        sTexMatrixIndex.clear();
        sTransformIndex.clear();
        sMatrices.push_back(glm::mat4(1.f));   // texture matrix 0: identity

        const glm::mat4 proj = gGL.getProjectionMatrix();
        const glm::mat4 view = get_current_modelview();
        for (const Item& item : sItems)
        {
            if (sSegments.empty() || sSegments.back().mMask != item.mMask || sSegments.back().mDoubleSided != item.mDoubleSided)
            {
                sSegments.push_back({ item.mMask, item.mDoubleSided, (U32)sCommands.size(), 0 });
            }
            const LLDrawInfo& params = *item.mParams;
            const U32 command = (U32)sCommands.size();
            const U32 instances = single_pass ? popcount4(item.mMask) : 1;
            // base instance: this command's record
            sCommands.push_back({ params.mCount, instances, item.mIndex + params.mOffset, (GLint)item.mVertex, command });
            const U32 transform = item.mKind == MODE_GLTF ? transform_index(item.mMaterial) : 0;
            sRecords.push_back({ matrix_index(params.mModelMatrix, proj, view), texture_matrix_index(item.mTexMatrix),
                                 item.mTextures, item.mKind | (transform << 2) });
            ++sSegments.back().mCount;
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

    // the arena, the record attribute and the storage buffers for the bound program
    void setup_inputs(U32 which)
    {
        // the attributes go into the global vertex array object, never a buffer's own
        LLVertexBuffer::fsUseDefaultVAO();
        glBindBufferBase(GL_SHADER_STORAGE_BUFFER, FS_GDD_SA_MATRIX_BINDING, sMatrixBuffer);
        glBindBufferBase(GL_SHADER_STORAGE_BUFFER, FS_GDD_SA_TEXTURE_BINDING, sTextureBuffer);
        glBindBuffer(GL_DRAW_INDIRECT_BUFFER, sIndirectBuffer);
        const GLuint record = (GLuint)sRecordAttrib[which];
        glBindBuffer(GL_ARRAY_BUFFER, sRecordBuffer);
        glEnableVertexAttribArray(record);
        glVertexAttribIPointer(record, 4, GL_UNSIGNED_INT, sizeof(Record), nullptr);
        glVertexAttribDivisor(record, FS_GDD_SA_RECORD_DIVISOR);
        // every attribute the program reads (bind_inputs checked the arena holds them), texture_index included
        sArena.bindForDraw(program(which).mAttributeMask);
    }

    // undo setup_inputs: the program's attribute mask doesn't have the record's location, so LLVertexBuffer
    // expects it off and undivided
    void finish_inputs(U32 which)
    {
        const GLuint record = (GLuint)sRecordAttrib[which];
        glVertexAttribDivisor(record, 0);
        glDisableVertexAttribArray(record);
        glBindBufferBase(GL_SHADER_STORAGE_BUFFER, FS_GDD_SA_MATRIX_BINDING, 0);
        glBindBufferBase(GL_SHADER_STORAGE_BUFFER, FS_GDD_SA_TEXTURE_BINDING, 0);  // also unbinds the generic binding
        glBindBuffer(GL_DRAW_INDIRECT_BUFFER, 0);
        LLVertexBuffer::unbind();   // the next setBuffer() binds and sets up its own attributes again
    }

    // As LLRenderPass::pushBatch(params, true, batch_textures) draws a batch (whose textures are there: the
    // pending ones have them all).
    void push_legacy(LLDrawInfo& params, bool batch_textures)
    {
        LLRenderPass::applyModelMatrix(params);
        bool tex_setup = false;
        if (batch_textures && params.mTextureList.size() > 1)
        {
            for (U32 i = 0; i < params.mTextureList.size(); ++i)
            {
                if (params.mTextureList[i].notNull())
                {
                    gGL.getTexUnit(i)->bindFast(params.mTextureList[i]);
                }
            }
        }
        else if (params.mTexture.notNull())
        {
            gGL.getTexUnit(0)->bindFast(params.mTexture);
            if (params.mTextureMatrix)
            {
                tex_setup = true;
                gGL.getTexUnit(0)->activate();
                gGL.matrixMode(LLRender::MM_TEXTURE);
                gGL.loadMatrix((GLfloat*)params.mTextureMatrix->mMatrix);
            }
        }
        else
        {
            gGL.getTexUnit(0)->unbindFast(LLTexUnit::TT_TEXTURE);
        }
        params.mVertexBuffer->setBuffer();
        params.mVertexBuffer->drawRange(LLRender::TRIANGLES, params.mStart, params.mEnd, params.mCount, params.mOffset);
        if (tex_setup)
        {
            gGL.matrixMode(LLRender::MM_TEXTURE0);
            gGL.loadIdentity();
            gGL.matrixMode(LLRender::MM_MODELVIEW);
        }
    }

    LLGLSLShader* stock_program(U32 kind, bool single_pass)
    {
        switch (kind)
        {
            case FS_GDD_SA_ALPHA_MASK:      return single_pass ? &gFSShadowSPAlphaMaskProgram : &gDeferredShadowAlphaMaskProgram;
            case FS_GDD_SA_FULLBRIGHT_MASK: return single_pass ? &gFSShadowSPFullbrightAlphaMaskProgram : &gDeferredShadowFullbrightAlphaMaskProgram;
            case FS_GDD_SA_TREE:            return single_pass ? &gFSShadowSPTreeProgram : &gDeferredTreeShadowProgram;
            default:                        return single_pass ? nullptr : &gDeferredShadowGLTFAlphaMaskProgram;
        }
    }

    // The pending items the stock way, with the stock programs (verify, or the multi-draw program failing to bind).
    void draw_stock(bool single_pass, U32 target_width, const glm::mat4* clip, const F32* widths)
    {
        static std::vector<const Item*> order;
        order.clear();
        for (const Item& item : sItems)
        {
            order.push_back(&item);
        }
        std::stable_sort(order.begin(), order.end(), [](const Item* a, const Item* b)
        {
            if (a->mKind != b->mKind)
            {
                return a->mKind < b->mKind;
            }
            return a->mMask < b->mMask;
        });

        const S32 sun_up = LLEnvironment::instance().getIsSunUp() ? 1 : 0;
        gGL.loadMatrix(gGLModelView);
        gGLLastMatrix = NULL;
        U32 kind = U32_MAX;
        U32 mask = 0;
        LLGLSLShader* shader = nullptr;
        LLFetchedGLTFMaterial* last_mat = nullptr;
        LLViewerTexture* last_tex = nullptr;
        for (const Item* item : order)
        {
            if (item->mKind != kind)
            {
                kind = item->mKind;
                mask = 0;
                last_mat = nullptr;
                last_tex = nullptr;
                shader = stock_program(kind, single_pass);
                if (shader)
                {
                    shader->bind();
                }
                shader = shader ? LLGLSLShader::sCurBoundShaderPtr : nullptr;
                if (shader)
                {
                    shader->uniform1i(LLShaderMgr::SUN_UP_FACTOR, sun_up);
                    if (single_pass)
                    {
                        shader->uniformMatrix4fv(sClip, 4, GL_FALSE, glm::value_ptr(clip[0]));
                        shader->uniform4fv(sWidth, 1, widths);
                        shader->uniform1f(LLShaderMgr::DEFERRED_SHADOW_TARGET_WIDTH, widths[0]);
                    }
                    else
                    {
                        shader->uniform1f(LLShaderMgr::DEFERRED_SHADOW_TARGET_WIDTH, (F32)target_width);
                    }
                }
            }
            if (!shader)
            {
                continue;
            }
            if (single_pass && item->mMask != mask)
            {
                mask = item->mMask;
                set_cascades(shader, mask);
            }
            LLDrawInfo& params = *item->mParams;
            if (kind == FS_GDD_SA_GLTF_MASK)
            {   // binds the material, which sets the minimum alpha
                LLRenderPass::pushGLTFBatch(params, last_mat, last_tex);
            }
            else
            {
                shader->setMinimumAlpha(item->mCutoff);
                push_legacy(params, kind != FS_GDD_SA_TREE);
            }
        }
        LLVertexBuffer::sFSInstanceCount = 0;
        gGL.loadMatrix(gGLModelView);
        gGLLastMatrix = NULL;
    }

    // FSPerfGPUDrivenDrawsVerify: the collected batches again, the stock way, with depth writes off and a GL_LESS
    // test against what the multi-draws left.
    void verify_against_stock(bool single_pass, U32 target_width, const glm::mat4* clip, const F32* widths)
    {
        LL_PROFILE_ZONE_NAMED_CATEGORY_PIPELINE("gdd shadow alpha verify");
        if (!sQuery)
        {
            glGenQueries(1, &sQuery);
        }
        GLfloat factor = 0.f;
        GLfloat units = 0.f;
        glGetFloatv(GL_POLYGON_OFFSET_FACTOR, &factor);
        glGetFloatv(GL_POLYGON_OFFSET_UNITS, &units);
        {
            LLGLDepthTest depth(GL_TRUE, GL_FALSE, GL_LESS);
            LLGLEnable offset(GL_POLYGON_OFFSET_FILL);
            glPolygonOffset(1.f, 4.f);  // only casters clearly nearer than what the multi-draw wrote count
            glBeginQuery(GL_SAMPLES_PASSED, sQuery);
            draw_stock(single_pass, target_width, clip, widths);
            glEndQuery(GL_SAMPLES_PASSED);
        }
        glPolygonOffset(factor, units);

        GLuint samples = 0;
        glGetQueryObjectuiv(sQuery, GL_QUERY_RESULT, &samples);
        if (samples > FS_GDD_SA_VERIFY_SAMPLES)
        {
            ++sStatMismatches;
            static LLFrameTimer warn_timer;
            if (warn_timer.getElapsedTimeF32() > 5.f)
            {
                warn_timer.reset();
                LL_WARNS("Rendering") << "FSPerfGPUDrivenShadowAlpha verify: " << samples << " samples of " << sItems.size()
                                      << " stock alpha tested shadow casters are nearer than the multi-draw depth"
                                      << (single_pass ? " (single pass)" : "") << "; " << sStatMismatches
                                      << " mismatching passes so far" << LL_ENDL;
            }
        }
    }

    void log_stats()
    {
        static LLCachedControl<bool> batch_stats(gSavedSettings, "FSPerfShadowBatchStats", false);
        static LLCachedControl<bool> verify(gSavedSettings, "FSPerfGPUDrivenDrawsVerify", false);
        static LLFrameTimer timer;
        if ((!batch_stats && !verify) || timer.getElapsedTimeF32() < 10.f)
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
        LL_INFOS("FSShadowBatches") << "GPU-driven alpha tested shadow casters, last 10 s: " << sStatPasses << " passes, "
                                    << sStatBatches << " batches (" << sStatDrawn << " from the arena) in " << sStatCalls
                                    << " multi-draw calls; stock path for: "
                                    << (reasons.tellp() ? reasons.str() : std::string("none")) << "; arena "
                                    << sArena.vertexMB() << " MB vertices, " << sArena.indexMB() << " MB indices, "
                                    << sArena.entries() << " buffers, " << sArena.uploads() << " copies; "
                                    << sStatMismatches << " verify mismatches" << LL_ENDL;
        sStatPasses = sStatBatches = sStatDrawn = sStatCalls = 0;
        sArena.clearCounts();
    }

    U32 flush(bool single_pass, U32 target_width, const glm::mat4* clip, const F32* widths)
    {
        if (!sActive)
        {
            return 0;
        }
        sActive = false;
        ++sStatPasses;
        if (sItems.empty())
        {
            log_stats();
            return 0;
        }
        LL_PROFILE_ZONE_NAMED_CATEGORY_PIPELINE("gdd shadow alpha");
        LL_PROFILE_GPU_ZONE("gdd shadow alpha");
        static LLCachedControl<bool> verify(gSavedSettings, "FSPerfGPUDrivenDrawsVerify", false);

        LLGLSLShader* previous = LLGLSLShader::sCurBoundShaderPtr;
        const U32 which = single_pass ? 1 : 0;
        build(single_pass);

        U32 calls = 0;
        if (!bind_program(which))
        {   // not expected (fsGDDShadowAlphaBegin found the inputs); the collected casters go the stock way
            LL_WARNS_ONCE("Rendering") << "FSPerfGPUDrivenShadowAlpha: the multi-draw program failed to bind" << LL_ENDL;
            draw_stock(single_pass, target_width, clip, widths);
        }
        else
        {
            upload(sIndirectBuffer, GL_DRAW_INDIRECT_BUFFER, sCommands.data(), sCommands.size() * sizeof(DrawCommand));
            upload(sRecordBuffer, GL_ARRAY_BUFFER, sRecords.data(), sRecords.size() * sizeof(Record));
            upload(sMatrixBuffer, GL_SHADER_STORAGE_BUFFER, sMatrices.data(), sMatrices.size() * sizeof(glm::mat4));
            upload(sTextureBuffer, GL_SHADER_STORAGE_BUFFER, sTextures.data(), sTextures.size() * sizeof(TexEntry));
            glBindBuffer(GL_SHADER_STORAGE_BUFFER, 0);
            glBindBuffer(GL_DRAW_INDIRECT_BUFFER, 0);
            glBindBuffer(GL_ARRAY_BUFFER, 0);
            LLVertexBuffer::sGLRenderBuffer = 0;    // or the next setBuffer of the buffer it named skips binding it

            LLGLSLShader& shader = program(which);
            if (single_pass)
            {
                shader.uniformMatrix4fv(sClip, 4, GL_FALSE, glm::value_ptr(clip[0]));
                shader.uniform4fv(sWidth, 1, widths);
            }
            else
            {
                shader.uniform1f(LLShaderMgr::DEFERRED_SHADOW_TARGET_WIDTH, (F32)target_width);
            }
            setup_inputs(which);
            for (const Segment& segment : sSegments)
            {
                LLGLDisable no_cull(segment.mDoubleSided ? GL_CULL_FACE : 0);
                if (single_pass)
                {
                    set_cascades(&shader, segment.mMask);
                }
                glMultiDrawElementsIndirect(GL_TRIANGLES, GL_UNSIGNED_SHORT,
                                            (const void*)((size_t)segment.mFirst * sizeof(DrawCommand)), (GLsizei)segment.mCount, 0);
                ++calls;
            }
            LLVertexBuffer::sFSInstanceCount = 0;
            finish_inputs(which);
            STOP_GLERROR;

            if (verify)
            {
                verify_against_stock(single_pass, target_width, clip, widths);
            }
        }

        rebind(previous);
        gGL.loadMatrix(gGLModelView);
        gGLLastMatrix = NULL;
        sStatCalls += calls;
        sItems.clear();
        sTextures.clear();
        log_stats();
        return calls;
    }
}

U32 fsGDDShadowAlphaFlush(U32 target_width)
{
    return flush(false, target_width, nullptr, nullptr);
}

U32 fsGDDShadowAlphaFlushSP(const glm::mat4* clip, const F32* widths)
{
    return flush(true, 0, clip, widths);
}
