/**
 * @file fsgpudrivenpbr.cpp
 * @brief GPU-driven draws for the deferred GLTF PBR opaque pass (FSPerfGPUDrivenPBR)
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

// The stock deferred PBR opaque pass (LLRenderPass::pushGLTFBatches) costs, for every batch, a vertex buffer
// bind and attribute setup, a model matrix (modelview, modelview projection and normal matrix uniforms), a
// texture matrix when animated, a cull face toggle for double sided materials, a glDrawRangeElements, and on
// every material change four texture binds and a dozen uniforms. genDrawInfo gives every batch its own
// vertex buffer, so nothing there can be merged.
//
// With FSPerfGPUDrivenPBR on, the batches of the pass come out of a vertex arena instead (fsgddarena.h: a GPU
// side copy of the position, normal, tangent, texcoord0 and colour regions and the 16 bit indices of their
// vertex buffers, refreshed when a buffer's content serial changes), and each batch is one 20 byte indirect
// draw command plus a record: which model matrices (in a storage buffer, the exact matrices the stock path
// would have computed, computed the same way), which texture matrix (identity or the animation's), and which
// material (the KHR texture transforms, alpha cutoff, roughness, metallic and emissive factors bind() would
// have set, and with FSPerfGPUDrivenPBRBindless the four texture handles). pbropaqueV.glsl and pbropaqueF.glsl
// built with FS_GDD read those in place of their uniforms; everything else in them is the stock code.
//
// The batches are drawn in the stock order, as runs: one glMultiDrawElementsIndirect per run of batches that
// share the face culling state and, without bindless textures, the material (whose textures are bound the
// stock way, LLFetchedGLTFMaterial::bind). With bindless textures a run only ends where a batch is double
// sided and the previous one isn't (or the other way round), or a batch that can't use handles comes. A batch
// that can't come from the arena at all (no material, rigged, empty, 32 bit indices, buffer mapped or lacking
// an attribute, arena full or the copy budget used up) is drawn by the stock code in its place, so the order,
// and with it the result where two surfaces have the same depth, is the stock one.
//
// Bindless textures: a texture with a handle is immutable (no glTexImage, no glTexParameter). The handles are
// made with glGetTextureSamplerHandleARB and a sampler object of our own per texture, set up as LLTexUnit
// would set up the texture (filter, mipmaps, wrap, anisotropy). Before LLImageGL or LLTexUnit respecify a
// texture or change its parameters they call LLImageGL::sFSTextureUnlock, which makes its handles non
// resident and deletes its samplers, which deletes the handles and makes the texture mutable again (checked
// once per GL context: if a driver keeps the texture immutable, bindless is not used). Handles are looked up
// by the texture's current GL name every pass, so a reallocated texture (a new name) gets a new handle. Only
// fetched textures with three or four components that are complete, not waiting for their options to be
// applied, and not media get handles; a batch with any other texture is drawn with bound textures.
//
// FSPerfGPUDrivenPBRVerify: after the pass, the batches it drew from the arena are drawn again the stock way
// with colour and depth writes off and depth test GL_LESS under a GL_SAMPLES_PASSED query: a sample passes
// only where the stock geometry is nearer than what the multi-draws left, which is logged. It also reads back
// a couple of arena entries per pass and compares them with their vertex buffers, checks for GL errors, and
// logs statistics every 10 seconds (FSPerfGPUDrivenPBRStats logs only the statistics).

#include "llviewerprecompiledheaders.h"

#include "fsgpudrivenpbr.h"

#include "fsglstream.h"

#include "fsgddarena.h"

#include "lldrawpool.h"
#include "llfetchedgltfmaterial.h"
#include "llframetimer.h"
#include "llgl.h"
#include "llglslshader.h"
#include "llimagegl.h"
#include "llrender.h"
#include "llspatialpartition.h"
#include "llthread.h"
#include "llvertexbuffer.h"
#include "llviewercontrol.h"
#include "llviewershadermgr.h"
#include "llviewertexture.h"
#include "pipeline.h"

#include <atomic>
#include <sstream>
#include <mutex>
#include <unordered_map>
#include <vector>

#if defined(GL_ARB_bindless_texture) && !LL_DARWIN
#define FS_GDD_BINDLESS_API 1
#else
#define FS_GDD_BINDLESS_API 0
#endif

LLGLSLShader gFSGDDPBRProgram;
LLGLSLShader gFSGDDPBRBindlessProgram;

namespace
{
    constexpr GLuint FS_GDD_PBR_MATRIX_BINDING = 5;     // shader storage binding of FSGDDPBRMatrices
    constexpr GLuint FS_GDD_PBR_MATERIAL_BINDING = 6;   // and of FSGDDPBRMaterials
    constexpr U32 FS_GDD_PBR_ATTRIBS = LLVertexBuffer::MAP_VERTEX | LLVertexBuffer::MAP_NORMAL | LLVertexBuffer::MAP_TANGENT |
                                       LLVertexBuffer::MAP_TEXCOORD0 | LLVertexBuffer::MAP_COLOR;
    constexpr U32 FS_GDD_HANDLE_IDLE_FRAMES = 300;      // frames a handle may go unused before it is made non resident
    constexpr U32 FS_GDD_HANDLE_DROP_FRAMES = 1800;     // and before it is deleted

    // everything pbropaqueV.glsl reads, an eighth of FSPerfGPUDrivenPBRArenaMB for indices
    FSGDDArena sArena("FSPerfGPUDrivenPBR arena",
                      { LLVertexBuffer::TYPE_VERTEX, LLVertexBuffer::TYPE_NORMAL, LLVertexBuffer::TYPE_TANGENT,
                        LLVertexBuffer::TYPE_TEXCOORD0, LLVertexBuffer::TYPE_COLOR },
                      "FSPerfGPUDrivenPBRArenaMB", 512, 8);

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

    struct Material     // FSGDDMaterial, std430
    {
        F32 mTransform[8][4];
        F32 mFactors[4];
        F32 mEmissive[4];
        U32 mTex[8];
    };
    static_assert(sizeof(Material) == 192, "FSGDDMaterial layout");

    enum EOpKind : U8
    {
        OP_BINDLESS = 0,    // gFSGDDPBRBindlessProgram
        OP_BOUND,           // gFSGDDPBRProgram, the material's textures bound
        OP_STOCK            // gDeferredPBROpaqueProgram, LLRenderPass::pushGLTFBatch
    };

    struct Op
    {
        EOpKind mKind;
        bool mDoubleSided;
        U32 mFirst;                     // first command
        U32 mCount;                     // commands
        LLFetchedGLTFMaterial* mMat;    // OP_BOUND
        LLViewerTexture* mTex;          // OP_BOUND: media override
        LLDrawInfo* mParams;            // OP_STOCK
    };

    // why a batch went the stock way, or (bindless on) with bound textures
    enum EReason
    {
        R_NO_MATERIAL = 0,
        R_RIGGED,
        R_EMPTY,
        R_RANGE,
        R_ARENA,                        // + FSGDDArena::EFail
        R_BOUND_MEDIA = R_ARENA + FSGDDArena::FAIL_COUNT,
        R_BOUND_TEXTURE,
        R_COUNT
    };
    const char* reason_name(U32 reason)
    {
        static const char* names[] = { "no material", "rigged", "empty", "index range" };
        if (reason < R_ARENA)
        {
            return names[reason];
        }
        if (reason < R_BOUND_MEDIA)
        {
            return FSGDDArena::failName(reason - R_ARENA);
        }
        return reason == R_BOUND_MEDIA ? "media (bound textures)" : "texture without handle (bound textures)";
    }

    GLuint sIndirectBuffer = 0;
    GLuint sRecordBuffer = 0;
    GLuint sMatrixBuffer = 0;
    GLuint sMaterialBuffer = 0;
    GLuint sQuery = 0;
    GLint sRecordAttrib[2] = { -1, -1 };    // location of fs_gdd_draw: bound, bindless
    S32 sInputsReady[2] = { 0, 0 };         // bind_inputs done: 0 not yet, 1 yes, -1 failed

    std::vector<Op> sOps;
    std::vector<DrawCommand> sCommands;
    std::vector<Record> sRecords;
    std::vector<glm::mat4> sMatrices;
    std::vector<Material> sMaterials;
    std::vector<LLDrawInfo*> sCommandParams; // per command: its batch (verify, and the stock fallback)
    std::unordered_map<const LLMatrix4*, U32> sMatrixIndex;
    std::unordered_map<const LLMatrix4*, U32> sTexMatrixIndex;
    struct MaterialInfo
    {
        U32 mIndex;
        bool mBindless;
    };
    std::unordered_map<const LLFetchedGLTFMaterial*, MaterialInfo> sMaterialIndex;

    // stats, every 10 seconds with FSPerfGPUDrivenPBRVerify or FSPerfGPUDrivenPBRStats
    U64 sStatBatches = 0;
    U64 sStatBindless = 0;
    U64 sStatBound = 0;
    U64 sStatCalls = 0;
    U64 sStatSwitches = 0;
    U64 sStatPasses = 0;
    U64 sStatReasons[R_COUNT] = {};
    U64 sStatMismatches = 0;

    LLGLSLShader& program(U32 which)
    {
        return which ? gFSGDDPBRBindlessProgram : gFSGDDPBRProgram;
    }
}

//-----------------------------------------------------------------------------
// Bindless texture handles

namespace
{
#if FS_GDD_BINDLESS_API
    PFNGLGETTEXTURESAMPLERHANDLEARBPROC sGetTextureSamplerHandle = nullptr;
    PFNGLMAKETEXTUREHANDLERESIDENTARBPROC sMakeResident = nullptr;
    PFNGLMAKETEXTUREHANDLENONRESIDENTARBPROC sMakeNonResident = nullptr;

    // The bindless entry points are loaded here, not through llglheaders.h, so FSRenderThread's
    // GL command stream doesn't proxy them: run them where the context is, in stream order
    // (runSync and recordCall call them directly when the stream isn't recording). Residency
    // changes return nothing, so they are recorded rather than waited for.
    GLuint64 fsGetHandle(GLuint texture, GLuint sampler)
    {
        GLuint64 handle = 0;
        FSGLStream::runSync([&]() { handle = sGetTextureSamplerHandle(texture, sampler); });
        return handle;
    }

    void fsMakeResidentNow(U64 handle)
    {
        sMakeResident((GLuint64)handle);
    }

    void fsMakeNonResidentNow(U64 handle)
    {
        sMakeNonResident((GLuint64)handle);
    }

    void fsMakeResident(GLuint64 handle)
    {
        FSGLStream::recordCall(&fsMakeResidentNow, (U64)handle);
    }

    void fsMakeNonResident(GLuint64 handle)
    {
        FSGLStream::recordCall(&fsMakeNonResidentNow, (U64)handle);
    }
#endif

    struct SamplerState
    {
        GLint mMag = GL_LINEAR;
        GLint mMin = GL_LINEAR;
        GLint mWrap = GL_REPEAT;
        F32 mAniso = 0.f;               // 0: not set (no anisotropic filtering support)

        bool operator==(const SamplerState& rhs) const
        {
            return mMag == rhs.mMag && mMin == rhs.mMin && mWrap == rhs.mWrap && mAniso == rhs.mAniso;
        }
    };

    struct HandleSlot
    {
        SamplerState mState;
        GLuint mSampler = 0;
        GLuint64 mHandle = 0;
        bool mResident = false;
        U32 mLastFrame = 0;
    };

    std::mutex sHandleMutex;                                    // the hook may come from the texture thread
    std::unordered_map<U32, std::vector<HandleSlot>> sHandles;  // by texture GL name
    std::atomic<U32> sHandleTextures{ 0 };                      // sHandles.size(), for the hook's fast path
    U32 sResident = 0;
    U32 sStatHandlesMade = 0;
    U32 sStatHandleFailures = 0;
    U32 sStatUnlocks = 0;
    S32 sBindlessTested = 0;                                    // 0 not yet, 1 works, -1 doesn't
    U32 sLastHandleSweep = 0;

    // As LLTexUnit::setTextureAddressModeFast and setTextureFilteringOptionFast would set the texture up.
    SamplerState sampler_state(const LLImageGL* gl)
    {
        SamplerState state;
        const LLTexUnit::eTextureFilterOptions option = gl->getFilteringOption();
        const bool mips = gl->getFSHasMipMaps();
        state.mMag = option == LLTexUnit::TFO_POINT ? GL_NEAREST : GL_LINEAR;
        if (option >= LLTexUnit::TFO_TRILINEAR && mips)
        {
            state.mMin = GL_LINEAR_MIPMAP_LINEAR;
        }
        else if (option >= LLTexUnit::TFO_BILINEAR)
        {
            state.mMin = mips ? GL_LINEAR_MIPMAP_NEAREST : GL_LINEAR;
        }
        else
        {
            state.mMin = mips ? GL_NEAREST_MIPMAP_NEAREST : GL_NEAREST;
        }
        static const GLint wrap[] = { GL_REPEAT, GL_MIRRORED_REPEAT, GL_CLAMP_TO_EDGE };
        const U32 mode = (U32)gl->getAddressMode();
        state.mWrap = mode < 3 ? wrap[mode] : GL_REPEAT;
        if (gGLManager.mHasAnisotropic)
        {
            state.mAniso = (option == LLTexUnit::TFO_ANISOTROPIC && LLRender::sAnisotropicFilteringLevel > 1.f)
                               ? llclamp(LLRender::sAnisotropicFilteringLevel, 1.f, gGLManager.mMaxAnisotropy)
                               : 1.f;
        }
        return state;
    }

    // Drops every handle of a texture: non resident, its samplers deleted (which deletes the handles).
    // sHandleMutex held, main thread.
    void drop_slots(std::vector<HandleSlot>& slots)
    {
#if FS_GDD_BINDLESS_API
        for (HandleSlot& slot : slots)
        {
            if (slot.mResident)
            {
                fsMakeNonResident(slot.mHandle);
                --sResident;
            }
            glDeleteSamplers(1, &slot.mSampler);
        }
#endif
        slots.clear();
    }

    // LLImageGL::sFSTextureUnlock: tex_name is about to be respecified or get new parameters.
    void unlock_texture(U32 tex_name)
    {
        if (!sHandleTextures.load(std::memory_order_relaxed))
        {
            return;
        }
        std::lock_guard<std::mutex> lock(sHandleMutex);
        auto it = sHandles.find(tex_name);
        if (it == sHandles.end())
        {
            return;
        }
        if (!on_main_thread())
        {   // handles are only made for textures the main thread owns; the texture thread works on new names
            LL_WARNS_ONCE("Rendering") << "FSPerfGPUDrivenPBRBindless: texture " << tex_name
                                       << " with bindless handles changed off the main thread" << LL_ENDL;
            return;
        }
        drop_slots(it->second);
        sHandles.erase(it);
        sHandleTextures.store((U32)sHandles.size(), std::memory_order_relaxed);
        ++sStatUnlocks;
    }

    void release_handles()
    {
        std::lock_guard<std::mutex> lock(sHandleMutex);
        for (auto& texture : sHandles)
        {
            drop_slots(texture.second);
        }
        sHandles.clear();
        sHandleTextures.store(0, std::memory_order_relaxed);
        sResident = 0;
    }

    // Once per GL context: does deleting the sampler of a texture/sampler handle make the texture mutable
    // again, as GL_ARB_bindless_texture says? Without that, the textures LLImageGL updates would break.
    bool bindless_self_test()
    {
#if FS_GDD_BINDLESS_API
        sGetTextureSamplerHandle = (PFNGLGETTEXTURESAMPLERHANDLEARBPROC)gGLManager.mFSBindlessProcs[0];
        sMakeResident = (PFNGLMAKETEXTUREHANDLERESIDENTARBPROC)gGLManager.mFSBindlessProcs[1];
        sMakeNonResident = (PFNGLMAKETEXTUREHANDLENONRESIDENTARBPROC)gGLManager.mFSBindlessProcs[2];
        if (!sGetTextureSamplerHandle || !sMakeResident || !sMakeNonResident)
        {
            return false;
        }
        for (U32 i = 0; i < 16 && glGetError() != GL_NO_ERROR; ++i)
        {
        }

        GLint previous = 0;
        glGetIntegerv(GL_TEXTURE_BINDING_2D, &previous);
        GLuint tex = 0;
        glGenTextures(1, &tex);
        glBindTexture(GL_TEXTURE_2D, tex);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, 4, 4, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        GLuint sampler = 0;
        glGenSamplers(1, &sampler);
        glSamplerParameteri(sampler, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        const GLuint64 handle = fsGetHandle(tex, sampler);
        bool ok = handle != 0 && glGetError() == GL_NO_ERROR;
        if (ok)
        {
            fsMakeResident(handle);
            fsMakeNonResident(handle);
            glDeleteSamplers(1, &sampler);
            sampler = 0;
            ok = glGetError() == GL_NO_ERROR;
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
            ok = ok && glGetError() == GL_NO_ERROR;
            glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, 8, 8, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
            ok = ok && glGetError() == GL_NO_ERROR;
        }
        if (sampler)
        {
            glDeleteSamplers(1, &sampler);
        }
        glBindTexture(GL_TEXTURE_2D, previous);
        glDeleteTextures(1, &tex);
        for (U32 i = 0; i < 16 && glGetError() != GL_NO_ERROR; ++i)
        {
        }
        gGL.invalidateFastBinds();
        return ok;
#else
        return false;
#endif
    }

    // The resident handle of tex as the stock path would sample it, or 0 if it can't have one.
    GLuint64 texture_handle(LLViewerTexture* tex, U32 frame)
    {
#if FS_GDD_BINDLESS_API
        if (!tex)
        {
            return 0;
        }
        const S8 type = tex->getType();
        LLImageGL* gl = tex->getGLTexture();
        if ((type != LLViewerTexture::FETCHED_TEXTURE && type != LLViewerTexture::LOD_TEXTURE) || !gl)
        {
            return 0;
        }
        const U32 name = gl->getTexName();
        if (!name || gl->getTarget() != LLTexUnit::TT_TEXTURE || gl->getComponents() < 3 || gl->getFSTexOptionsDirty())
        {   // a texture with pending options gets them at its next bind, which would drop its handle mid pass
            return 0;
        }
        const SamplerState state = sampler_state(gl);

        std::lock_guard<std::mutex> lock(sHandleMutex);
        std::vector<HandleSlot>& slots = sHandles[name];
        for (HandleSlot& slot : slots)
        {
            if (slot.mState == state)
            {
                if (!slot.mResident)
                {
                    fsMakeResident(slot.mHandle);
                    slot.mResident = true;
                    ++sResident;
                }
                slot.mLastFrame = frame;
                return slot.mHandle;
            }
        }

        HandleSlot slot;
        slot.mState = state;
        glGenSamplers(1, &slot.mSampler);
        glSamplerParameteri(slot.mSampler, GL_TEXTURE_MAG_FILTER, state.mMag);
        glSamplerParameteri(slot.mSampler, GL_TEXTURE_MIN_FILTER, state.mMin);
        glSamplerParameteri(slot.mSampler, GL_TEXTURE_WRAP_S, state.mWrap);
        glSamplerParameteri(slot.mSampler, GL_TEXTURE_WRAP_T, state.mWrap);
        if (state.mAniso > 0.f)
        {
            glSamplerParameterf(slot.mSampler, GL_TEXTURE_MAX_ANISOTROPY, state.mAniso);
        }
        // 0 (and GL_INVALID_OPERATION) for a texture that isn't complete with this sampler
        slot.mHandle = fsGetHandle(name, slot.mSampler);
        if (!slot.mHandle)
        {
            glDeleteSamplers(1, &slot.mSampler);
            for (U32 i = 0; i < 4 && glGetError() != GL_NO_ERROR; ++i)
            {
            }
            ++sStatHandleFailures;
            if (slots.empty())
            {
                sHandles.erase(name);
            }
            return 0;
        }
        fsMakeResident(slot.mHandle);
        slot.mResident = true;
        slot.mLastFrame = frame;
        ++sResident;
        ++sStatHandlesMade;
        slots.push_back(slot);
        sHandleTextures.store((U32)sHandles.size(), std::memory_order_relaxed);
        return slot.mHandle;
#else
        return 0;
#endif
    }

    // Every so often: handles not used for a while go non resident, and later away.
    void sweep_handles(U32 frame)
    {
        if (frame - sLastHandleSweep < 120)
        {
            return;
        }
        sLastHandleSweep = frame;
#if FS_GDD_BINDLESS_API
        std::lock_guard<std::mutex> lock(sHandleMutex);
        for (auto it = sHandles.begin(); it != sHandles.end(); )
        {
            std::vector<HandleSlot>& slots = it->second;
            for (size_t i = 0; i < slots.size(); )
            {
                HandleSlot& slot = slots[i];
                const U32 idle = frame - slot.mLastFrame;
                if (idle >= FS_GDD_HANDLE_DROP_FRAMES)
                {
                    std::vector<HandleSlot> one(1, slot);
                    drop_slots(one);
                    slots.erase(slots.begin() + i);
                    continue;
                }
                if (idle >= FS_GDD_HANDLE_IDLE_FRAMES && slot.mResident)
                {
                    fsMakeNonResident(slot.mHandle);
                    slot.mResident = false;
                    --sResident;
                }
                ++i;
            }
            it = slots.empty() ? sHandles.erase(it) : std::next(it);
        }
        sHandleTextures.store((U32)sHandles.size(), std::memory_order_relaxed);
#endif
    }
}

//-----------------------------------------------------------------------------
// Programs

namespace
{
    bool create_program(LLGLSLShader& shader, const char* name, bool bindless)
    {
        const LLGLSLShader& stock = gDeferredPBROpaqueProgram;
        shader.mName = name;
        shader.mFeatures = stock.mFeatures;
        shader.mShaderFiles = stock.mShaderFiles;
        shader.mShaderLevel = stock.mShaderLevel;
        shader.mShaderGroup = stock.mShaderGroup;
        shader.clearPermutations();
        shader.mDefines = stock.mDefines;   // the stock permutations (HAS_EMISSIVE, FS_SPECULAR_AA, ...), then ours
        shader.addPermutation("FS_GDD", "1");
        if (bindless)
        {
            shader.addPermutation("FS_GDD_BINDLESS", "1");
        }
        return shader.createShader();
    }

    // the storage block bindings, and where the driver put the per command record
    bool bind_inputs(LLGLSLShader& shader, GLint& record_attrib)
    {
        const GLuint matrices = glGetProgramResourceIndex(shader.mProgramObject, GL_SHADER_STORAGE_BLOCK, "FSGDDPBRMatrices");
        const GLuint materials = glGetProgramResourceIndex(shader.mProgramObject, GL_SHADER_STORAGE_BLOCK, "FSGDDPBRMaterials");
        record_attrib = glGetAttribLocation(shader.mProgramObject, "fs_gdd_draw");
        if (matrices == GL_INVALID_INDEX || materials == GL_INVALID_INDEX || record_attrib < 0)
        {
            return false;
        }
        glShaderStorageBlockBinding(shader.mProgramObject, matrices, FS_GDD_PBR_MATRIX_BINDING);
        glShaderStorageBlockBinding(shader.mProgramObject, materials, FS_GDD_PBR_MATERIAL_BINDING);
        return true;
    }

    // Binds a GPU-driven program; the first time, also finds its inputs. False (for good) when they aren't there.
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
                LL_WARNS("Rendering") << "FSPerfGPUDrivenPBR: " << shader.mName << " lacks its storage blocks or draw "
                                      << "record attribute, PBR batches are drawn as before" << LL_ENDL;
            }
        }
        return sInputsReady[which] > 0;
    }

    bool program_usable(U32 which)
    {
        return program(which).isComplete() && sInputsReady[which] >= 0;
    }
}

void fsGDDPBRUnloadShaders()
{
    gFSGDDPBRProgram.unload();
    gFSGDDPBRBindlessProgram.unload();
    sInputsReady[0] = 0;
    sInputsReady[1] = 0;
    fsGDDPBRReleaseGL();
}

void fsGDDPBRLoadShaders()
{
    fsGDDPBRUnloadShaders();

    static LLCachedControl<bool> enabled(gSavedSettings, "FSPerfGPUDrivenPBR", false);
    static LLCachedControl<bool> bindless(gSavedSettings, "FSPerfGPUDrivenPBRBindless", false);
    if (!enabled)
    {
        return;
    }
    if (!gGLManager.mFSHasGPUDrivenDraws)
    {
        LL_INFOS_ONCE("ShaderLoading") << "FSPerfGPUDrivenPBR: needs OpenGL 4.3 with GL_ARB_multi_draw_indirect and "
                                       << "GL_ARB_shader_storage_buffer_object; PBR batches are drawn as before" << LL_ENDL;
        return;
    }
    // mFSHasGPUDrivenDraws asks for one vertex shader storage block (the shadow pass); this program uses two
    GLint vs_blocks = 0;
    glGetIntegerv(GL_MAX_VERTEX_SHADER_STORAGE_BLOCKS, &vs_blocks);
    if (vs_blocks < 2)
    {
        LL_INFOS_ONCE("ShaderLoading") << "FSPerfGPUDrivenPBR: needs two shader storage blocks in the vertex shader, "
                                       << "the driver offers " << vs_blocks << "; PBR batches are drawn as before" << LL_ENDL;
        return;
    }
    if (gDeferredPBROpaqueProgram.mShaderFiles.empty())
    {
        return;
    }

    if (!create_program(gFSGDDPBRProgram, "FS GPU-Driven PBR Opaque Shader", false))
    {
        LL_WARNS("ShaderLoading") << "FSPerfGPUDrivenPBR: the multi-draw PBR program failed to build, "
                                  << "PBR batches are drawn as before" << LL_ENDL;
        fsGDDPBRUnloadShaders();
        return;
    }

    if (bindless && !gGLManager.mIsNVIDIA)
    {   // only tested on NVIDIA; elsewhere drivers may want handles uniform within a multi-draw wave,
        // which the commands of a batch don't guarantee
        LL_INFOS_ONCE("ShaderLoading") << "FSPerfGPUDrivenPBRBindless: only used on NVIDIA GPUs, "
                                       << "PBR textures are bound as before" << LL_ENDL;
    }
    else if (bindless)
    {
        if (!sBindlessTested)
        {
            sBindlessTested = bindless_self_test() ? 1 : -1;
            LL_INFOS("ShaderLoading") << "FSPerfGPUDrivenPBRBindless: bindless textures "
                                      << (sBindlessTested > 0 ? "usable" : "not usable (missing, or the driver keeps textures "
                                                                           "immutable after their handles are gone)")
                                      << LL_ENDL;
        }
        if (sBindlessTested > 0)
        {
            if (create_program(gFSGDDPBRBindlessProgram, "FS GPU-Driven PBR Opaque Bindless Shader", true))
            {
                LLImageGL::sFSTextureUnlock = &unlock_texture;  // stays set; does nothing while there are no handles
            }
            else
            {
                LL_WARNS("ShaderLoading") << "FSPerfGPUDrivenPBRBindless: the bindless program failed to build, "
                                          << "PBR textures are bound as before" << LL_ENDL;
                gFSGDDPBRBindlessProgram.unload();
            }
        }
    }
}

void fsGDDPBRReleaseGL()
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
    release_handles();
    sBindlessTested = 0;    // per GL context: tested again at the next load
}

//-----------------------------------------------------------------------------
// The pass

namespace
{
    U32 matrix_index(const LLMatrix4* model, const glm::mat4& proj, const glm::mat4& view)
    {
        auto it = sMatrixIndex.find(model);
        if (it != sMatrixIndex.end())
        {
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
        return index;
    }

    U32 texture_matrix_index(const LLMatrix4* texture_matrix)
    {
        if (!texture_matrix)
        {
            return 0;   // identity, what MM_TEXTURE0 holds outside setup_texture_matrix/teardown_texture_matrix
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

    // The material's record: what LLFetchedGLTFMaterial::bind(nullptr) sets in the deferred pass, and, when
    // bindless is wanted and every texture can have a handle, the handles of what it would bind.
    const MaterialInfo& material_index(LLFetchedGLTFMaterial* mat, bool want_bindless, U32 frame)
    {
        auto it = sMaterialIndex.find(mat);
        if (it != sMaterialIndex.end())
        {
            return it->second;
        }
        Material record = {};
        static const LLGLTFMaterial::TextureInfo infos[4] = { LLGLTFMaterial::GLTF_TEXTURE_INFO_BASE_COLOR,
                                                              LLGLTFMaterial::GLTF_TEXTURE_INFO_NORMAL,
                                                              LLGLTFMaterial::GLTF_TEXTURE_INFO_METALLIC_ROUGHNESS,
                                                              LLGLTFMaterial::GLTF_TEXTURE_INFO_EMISSIVE };
        for (U32 i = 0; i < 4; ++i)
        {
            F32 packed[8];
            mat->mTextureTransform[infos[i]].getPacked(packed);
            memcpy(record.mTransform[i * 2], packed, sizeof(packed));
        }
        record.mFactors[0] = mat->mAlphaMode == LLGLTFMaterial::ALPHA_MODE_MASK ? mat->mAlphaCutoff : -1.f;
        record.mFactors[1] = mat->mRoughnessFactor;
        record.mFactors[2] = mat->mMetallicFactor;
        record.mEmissive[0] = mat->mEmissiveColor.mV[0];
        record.mEmissive[1] = mat->mEmissiveColor.mV[1];
        record.mEmissive[2] = mat->mEmissiveColor.mV[2];

        bool bindless = want_bindless;
        if (bindless)
        {   // the textures bind() picks without a media override
            LLViewerTexture* textures[4] =
            {
                mat->mBaseColorTexture.notNull() ? (LLViewerTexture*)mat->mBaseColorTexture.get() : LLViewerFetchedTexture::sWhiteImagep.get(),
                (mat->mNormalTexture.notNull() && mat->mNormalTexture->getDiscardLevel() <= 4)
                    ? (LLViewerTexture*)mat->mNormalTexture.get() : LLViewerFetchedTexture::sFlatNormalImagep.get(),
                mat->mMetallicRoughnessTexture.notNull() ? (LLViewerTexture*)mat->mMetallicRoughnessTexture.get() : LLViewerFetchedTexture::sWhiteImagep.get(),
                mat->mEmissiveTexture.notNull() ? (LLViewerTexture*)mat->mEmissiveTexture.get() : LLViewerFetchedTexture::sWhiteImagep.get(),
            };
            for (U32 i = 0; i < 4 && bindless; ++i)
            {
                const GLuint64 handle = texture_handle(textures[i], frame);
                bindless = handle != 0;
                record.mTex[i * 2] = (U32)(handle & 0xFFFFFFFFull);
                record.mTex[i * 2 + 1] = (U32)(handle >> 32);
            }
            if (bindless)
            {
                for (LLViewerTexture* tex : textures)
                {
                    tex->setActive();   // as LLTexUnit::bindFast does, so the texture stays wanted
                    // and counts as bound this frame (isJustBound, getBoundRecently), as a real bind would
                    if (LLImageGL* gl = tex->getGLTexture())
                    {
                        gl->updateBindStats();
                    }
                }
            }
        }
        const MaterialInfo info = { (U32)sMaterials.size(), bindless };
        sMaterials.push_back(record);
        return sMaterialIndex.emplace(mat, info).first->second;
    }

    void count(U32 reason)
    {
        ++sStatReasons[reason];
    }

    // Turns the pass's batches into ops (in the stock order) and commands.
    void build(LLCullResult::drawinfo_iterator begin, LLCullResult::drawinfo_iterator end, bool bindless)
    {
        sOps.clear();
        sCommands.clear();
        sRecords.clear();
        sMatrices.clear();
        sMaterials.clear();
        sCommandParams.clear();
        sMatrixIndex.clear();
        sTexMatrixIndex.clear();
        sMaterialIndex.clear();

        const U32 frame = LLFrameTimer::getFrameCount();
        sArena.beginFrame(frame);
        const glm::mat4 proj = gGL.getProjectionMatrix();
        const glm::mat4 view = get_current_modelview();
        sMatrices.push_back(glm::mat4(1.f));   // texture matrix 0: identity

        for (LLCullResult::drawinfo_iterator i = begin; i != end; )
        {
            LLDrawInfo& params = **i;
            LLCullResult::increment_iterator(i, end);
            ++sStatBatches;

            LLFetchedGLTFMaterial* mat = params.mGLTFMaterial.get();
            S32 reason = -1;
            const FSGDDArenaEntry* entry = nullptr;
            if (!mat)
            {
                reason = R_NO_MATERIAL;
            }
            else if (params.mAvatar.notNull() || params.mSkinInfo.notNull())
            {
                reason = R_RIGGED;
            }
            else if (!params.mCount || params.mVertexBuffer.isNull())
            {
                reason = R_EMPTY;
            }
            else
            {
                U32 fail = 0;
                entry = sArena.find(params.mVertexBuffer.get(), frame, &fail);
                if (!entry)
                {
                    reason = R_ARENA + fail;
                }
                else if (params.mOffset + params.mCount > entry->mNumIndices || params.mEnd >= entry->mNumVerts)
                {   // never for a valid batch; anything else is left to the stock path rather than read past the copy
                    reason = R_RANGE;
                }
            }

            if (reason >= 0)
            {
                count(reason);
                sOps.push_back({ OP_STOCK, false, 0, 0, nullptr, nullptr, &params });
                continue;
            }

            // the media override replaces the base colour and emissive textures (bind()), so such a batch binds
            // its textures even when its material has handles
            LLViewerTexture* media = params.mTexture.get();
            const MaterialInfo& info = material_index(mat, bindless, frame);
            EOpKind kind = OP_BOUND;
            if (info.mBindless && !media)
            {
                kind = OP_BINDLESS;
                ++sStatBindless;
            }
            else
            {
                ++sStatBound;
                if (bindless)
                {
                    count(media ? R_BOUND_MEDIA : R_BOUND_TEXTURE);
                }
            }
            const bool double_sided = mat->mDoubleSided;

            const U32 command = (U32)sCommands.size();
            sCommands.push_back({ params.mCount, 1, entry->mIndex + params.mOffset, (GLint)entry->mVertex, command });
            sRecords.push_back({ matrix_index(params.mModelMatrix, proj, view), texture_matrix_index(params.mTextureMatrix.get()),
                                 info.mIndex, 0 });
            sCommandParams.push_back(&params);

            Op* last = sOps.empty() ? nullptr : &sOps.back();
            if (last && last->mKind == kind && last->mDoubleSided == double_sided &&
                (kind == OP_BINDLESS || (last->mMat == mat && last->mTex == media)))
            {
                ++last->mCount;
            }
            else
            {
                sOps.push_back({ kind, double_sided, command, 1, mat, media, nullptr });
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

    // the arena, the record attribute and the storage buffers for program `which` (bound)
    void setup_inputs(U32 which)
    {
        glBindBufferBase(GL_SHADER_STORAGE_BUFFER, FS_GDD_PBR_MATRIX_BINDING, sMatrixBuffer);
        glBindBufferBase(GL_SHADER_STORAGE_BUFFER, FS_GDD_PBR_MATERIAL_BINDING, sMaterialBuffer);
        glBindBuffer(GL_DRAW_INDIRECT_BUFFER, sIndirectBuffer);
        const GLuint record = (GLuint)sRecordAttrib[which];
        glBindBuffer(GL_ARRAY_BUFFER, sRecordBuffer);
        glEnableVertexAttribArray(record);
        glVertexAttribIPointer(record, 4, GL_UNSIGNED_INT, sizeof(Record), nullptr);
        glVertexAttribDivisor(record, 1);
        sArena.bindForDraw(FS_GDD_PBR_ATTRIBS);
    }

    // undo setup_inputs: the program's attribute mask doesn't have the record's location, so LLVertexBuffer
    // expects it off and undivided
    void finish_inputs(U32 which)
    {
        const GLuint record = (GLuint)sRecordAttrib[which];
        glVertexAttribDivisor(record, 0);
        glDisableVertexAttribArray(record);
        glBindBufferBase(GL_SHADER_STORAGE_BUFFER, FS_GDD_PBR_MATRIX_BINDING, 0);
        glBindBufferBase(GL_SHADER_STORAGE_BUFFER, FS_GDD_PBR_MATERIAL_BINDING, 0);
        glBindBuffer(GL_SHADER_STORAGE_BUFFER, 0);
        glBindBuffer(GL_DRAW_INDIRECT_BUFFER, 0);
        LLVertexBuffer::unbind();   // the next setBuffer() binds and sets up its own attributes again
    }

    // Draws the ops. The stock program is bound on entry and on return. False if a GPU-driven program
    // couldn't be bound before anything was drawn (then nothing was).
    bool execute(U32& calls, U32& switches)
    {
        LL_PROFILE_ZONE_NAMED_CATEGORY_DRAWPOOL("gdd pbr execute");
        LL_PROFILE_GPU_ZONE("gdd pbr");

        upload(sIndirectBuffer, GL_DRAW_INDIRECT_BUFFER, sCommands.data(), sCommands.size() * sizeof(DrawCommand));
        upload(sRecordBuffer, GL_ARRAY_BUFFER, sRecords.data(), sRecords.size() * sizeof(Record));
        upload(sMatrixBuffer, GL_SHADER_STORAGE_BUFFER, sMatrices.data(), sMatrices.size() * sizeof(glm::mat4));
        upload(sMaterialBuffer, GL_SHADER_STORAGE_BUFFER, sMaterials.data(), sMaterials.size() * sizeof(Material));
        glBindBuffer(GL_SHADER_STORAGE_BUFFER, 0);
        glBindBuffer(GL_DRAW_INDIRECT_BUFFER, 0);
        glBindBuffer(GL_ARRAY_BUFFER, 0);

        LLGLSLShader* stock = &gDeferredPBROpaqueProgram;
        S32 current = -1;                       // GPU-driven program bound: 0 bound textures, 1 bindless, -1 stock
        bool drew = false;
        LLFetchedGLTFMaterial* stock_mat = nullptr;     // pushGLTFBatch's material cache
        LLViewerTexture* stock_tex = nullptr;
        LLFetchedGLTFMaterial* bound_mat = nullptr;     // last material bound for gFSGDDPBRProgram
        LLViewerTexture* bound_tex = nullptr;

        for (const Op& op : sOps)
        {
            if (op.mKind == OP_STOCK)
            {
                if (current >= 0)
                {
                    finish_inputs(current);
                    stock->bind();
                    current = -1;
                    stock_mat = nullptr;    // the GPU-driven program may have bound other textures on the units
                    ++switches;
                }
                LLRenderPass::pushGLTFBatch(*op.mParams, stock_mat, stock_tex);
                drew = true;
                continue;
            }

            const S32 which = op.mKind == OP_BINDLESS ? 1 : 0;
            if (current != which)
            {
                if (current >= 0)
                {
                    finish_inputs(current);
                }
                if (!bind_program(which))
                {
                    stock->bind();
                    if (!drew)
                    {
                        return false;
                    }
                    // not expected (fsGDDDrawPBR bound both programs once already); the rest of the pass goes
                    // the stock way, in order
                    LL_WARNS_ONCE("Rendering") << "FSPerfGPUDrivenPBR: a program failed to bind mid pass" << LL_ENDL;
                    stock_mat = nullptr;
                    for (size_t i = &op - sOps.data(); i < sOps.size(); ++i)
                    {
                        const Op& rest = sOps[i];
                        if (rest.mKind == OP_STOCK)
                        {
                            LLRenderPass::pushGLTFBatch(*rest.mParams, stock_mat, stock_tex);
                        }
                        else
                        {
                            for (U32 c = rest.mFirst; c < rest.mFirst + rest.mCount; ++c)
                            {
                                LLRenderPass::pushGLTFBatch(*sCommandParams[c], stock_mat, stock_tex);
                            }
                        }
                    }
                    return true;
                }
                if (current < 0)
                {
                    ++switches;
                }
                current = which;
                setup_inputs(which);
                bound_mat = nullptr;
            }
            if (op.mKind == OP_BOUND && (op.mMat != bound_mat || op.mTex != bound_tex))
            {
                op.mMat->bind(op.mTex);
                bound_mat = op.mMat;
                bound_tex = op.mTex;
            }
            LLGLDisable cull_face(op.mDoubleSided ? GL_CULL_FACE : 0);
            glMultiDrawElementsIndirect(GL_TRIANGLES, GL_UNSIGNED_SHORT, (const void*)((size_t)op.mFirst * sizeof(DrawCommand)),
                                        (GLsizei)op.mCount, 0);
            ++calls;
            drew = true;
        }
        if (current >= 0)
        {
            finish_inputs(current);
            stock->bind();
        }
        STOP_GLERROR;
        return true;
    }

    // FSPerfGPUDrivenPBRVerify: the batches drawn from the arena again, the stock way, colour and depth writes
    // off, depth test GL_LESS against what the multi-draws left. The stock program must be bound.
    void verify_against_stock()
    {
        if (sCommandParams.empty())
        {
            return;
        }
        LL_PROFILE_ZONE_NAMED_CATEGORY_DRAWPOOL("gdd pbr verify");

        GLenum error = glGetError();
        if (error != GL_NO_ERROR)
        {
            static LLFrameTimer warn_timer;
            if (warn_timer.getElapsedTimeF32() > 5.f)
            {
                warn_timer.reset();
                LL_WARNS("Rendering") << "FSPerfGPUDrivenPBRVerify: GL error 0x" << std::hex << error << std::dec
                                      << " after the GPU-driven PBR pass" << LL_ENDL;
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
                        LL_WARNS("Rendering") << "FSPerfGPUDrivenPBRVerify: the arena copy of a vertex buffer ("
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
            LLFetchedGLTFMaterial* last_mat = nullptr;
            LLViewerTexture* last_tex = nullptr;
            for (LLDrawInfo* params : sCommandParams)
            {
                LLRenderPass::pushGLTFBatch(*params, last_mat, last_tex);
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
                LL_WARNS("Rendering") << "FSPerfGPUDrivenPBRVerify: " << samples << " samples of " << sCommandParams.size()
                                      << " stock PBR batches are nearer than the multi-draw depth; "
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
        U32 handle_textures = 0;
        {
            std::lock_guard<std::mutex> lock(sHandleMutex);
            handle_textures = (U32)sHandles.size();
        }
        LL_INFOS("FSGPUDrivenPBR") << "GPU-driven PBR, last 10 s: " << sStatPasses << " passes, " << sStatBatches
                                   << " batches (" << sStatBindless << " bindless, " << sStatBound
                                   << " bound textures) in " << sStatCalls << " multi-draw calls, "
                                   << sStatSwitches << " program switches; stock path or bound textures for: "
                                   << (reasons.tellp() ? reasons.str() : std::string("none")) << "; arena "
                                   << sArena.vertexMB() << " MB vertices, " << sArena.indexMB() << " MB indices, "
                                   << sArena.entries() << " buffers, " << sArena.uploads() << " copies; bindless "
                                   << handle_textures << " textures, " << sResident << " resident handles, "
                                   << sStatHandlesMade << " made, " << sStatHandleFailures << " failed, "
                                   << sStatUnlocks << " dropped for texture changes; " << sStatMismatches
                                   << " verify mismatches" << LL_ENDL;
        sStatPasses = sStatBatches = sStatBindless = sStatBound = sStatCalls = sStatSwitches = 0;
        sStatHandlesMade = sStatHandleFailures = sStatUnlocks = 0;
        sArena.clearCounts();
    }
}

bool fsGDDDrawPBR(U32 type)
{
    // before any early return, so idle handles go non resident and away also while no PBR batch (or no
    // bindless one) is drawn; cheap, it does something every 120 frames
    sweep_handles(LLFrameTimer::getFrameCount());

    static LLCachedControl<bool> enabled(gSavedSettings, "FSPerfGPUDrivenPBR", false);
    if (!enabled || !gGLManager.mFSHasGPUDrivenDraws || !program_usable(0) ||
        LLGLSLShader::sCurBoundShaderPtr != &gDeferredPBROpaqueProgram || LLPipeline::sShadowRender)
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
    static LLCachedControl<bool> want_bindless(gSavedSettings, "FSPerfGPUDrivenPBRBindless", false);
    static LLCachedControl<bool> verify(gSavedSettings, "FSPerfGPUDrivenPBRVerify", false);
    static LLCachedControl<bool> stats(gSavedSettings, "FSPerfGPUDrivenPBRStats", false);

    // The first bind of each program finds its inputs; do that now, so a failure leaves the whole pass
    // to the stock loop rather than part of it
    if (!sInputsReady[0])
    {
        const bool ready = bind_program(0);
        gDeferredPBROpaqueProgram.bind();
        if (!ready)
        {
            return false;
        }
    }
    bool bindless = want_bindless && sBindlessTested > 0 && program_usable(1);
    if (bindless && !sInputsReady[1])
    {
        bindless = bind_program(1);
        gDeferredPBROpaqueProgram.bind();
    }

    // The environment uniforms (LLEnvironment marks the programs of the shader list dirty when they change,
    // and the stock program took them when the pool bound it); ours aren't in that list, so they take the
    // current values at their first bind in each pass
    gFSGDDPBRProgram.mUniformsDirty = true;
    gFSGDDPBRBindlessProgram.mUniformsDirty = true;

    {
        LL_PROFILE_ZONE_NAMED_CATEGORY_DRAWPOOL("gdd pbr build");
        build(begin, end, bindless);
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
    if (!execute(calls, switches))
    {
        return false;
    }

    if (verify)
    {
        verify_against_stock();
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
