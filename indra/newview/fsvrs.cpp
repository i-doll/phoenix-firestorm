/**
 * @file fsvrs.cpp
 * @brief Variable rate shading of the main view's G-buffer pass (FSRenderVRS)
 *
 * $LicenseInfo:firstyear=2026&license=fsviewerlgpl$
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

// Design notes
//
// Why the G-buffer pass only. Coarse shading runs one fragment shader invocation for a 2x1,
// 1x2, 2x2 ... block and writes its outputs to every covered pixel; coverage and depth stay per
// pixel. In the opaque G-buffer pass that only flattens texture and normal detail inside the
// block, and the content-adaptive selection keeps it to tiles where that detail was too weak
// to see. Lighting, SSAO / GTAO, SSR and the rest read the G-buffer at full resolution
// afterwards and are not affected. The alpha pass, water, the post passes, shadows, probes and
// mirrors are never shaded coarsely: their targets differ in size, or their output is the
// final colour where block artefacts would show directly.
//
// Where the rates come from. The previous frame: its post-processed colour (after tone mapping,
// glow and depth of field, before anti-aliasing) and its depth. That is one frame of latency, so
// fsVRSRateF.glsl only lets a tile be one level coarser than any neighbour, and the classifier
// measures pixel differences 2 (and 4) apart, which come out the same whether the tile was
// itself shaded coarsely or not, so tiles don't flicker between rates. See the shaders.
//
// Dynamic resolution and window resizes. The image is tied to the size it was built for; a
// G-buffer of any other size renders at full rate until the next image is built at the new
// size (one frame).
//
// Measuring. With the setting on, GL_TIMESTAMP pairs (glQueryCounter, so they don't collide
// with the GL_TIME_ELAPSED queries of the profilers) time the G-buffer pass and the image
// build, read back a few frames later without stalling. Every FSRenderVRSLogInterval seconds
// the image is read back through a PBO and fence, also without stalling, and one line gives the
// share of tiles at each rate and the average G-buffer time of frames with and without VRS.

#include "llviewerprecompiledheaders.h"

#include "fsvrs.h"

#include "llgl.h"
#include "llglslshader.h"
#include "llrender.h"
#include "llrendertarget.h"
#include "llshadermgr.h"
#include "lltimer.h"
#include "llvertexbuffer.h"
#include "llviewercontrol.h"
#include "llviewershadermgr.h"
#include "pipeline.h"

#include <iomanip>
#include <sstream>

extern bool gCubeSnapshot;
extern bool gSnapshot;

#if LL_GL_FUNC_POINTER

namespace
{
    // Palette indices written by fsVRSRateF.glsl
    constexpr S32 PALETTE_SIZE = 7;
    const GLenum PALETTE[PALETTE_SIZE] = {
        GL_SHADING_RATE_1_INVOCATION_PER_PIXEL_NV,
        GL_SHADING_RATE_1_INVOCATION_PER_1X2_PIXELS_NV,
        GL_SHADING_RATE_1_INVOCATION_PER_2X1_PIXELS_NV,
        GL_SHADING_RATE_1_INVOCATION_PER_2X2_PIXELS_NV,
        GL_SHADING_RATE_1_INVOCATION_PER_2X4_PIXELS_NV,
        GL_SHADING_RATE_1_INVOCATION_PER_4X2_PIXELS_NV,
        GL_SHADING_RATE_1_INVOCATION_PER_4X4_PIXELS_NV,
    };
    const char* const RATE_NAMES[PALETTE_SIZE] = { "1x1", "1x2", "2x1", "2x2", "2x4", "4x2", "4x4" };

    constexpr S32 QUERY_SLOTS = 4;      // frames a timing may be in flight
    constexpr S32 MAX_TILE = 64;        // sanity bound on the reported texel size

    // GL_TIMESTAMP pairs read back a few frames later
    class TimerRing
    {
    public:
        void begin()
        {
            mOpen = -1;
            if (!mCreated)
            {
                for (Slot& slot : mSlots)
                {
                    glGenQueries(2, slot.mQuery);
                }
                mCreated = true;
            }
            Slot& slot = mSlots[mNext];
            if (!slot.mPending)
            {
                glQueryCounter(slot.mQuery[0], GL_TIMESTAMP);
                mOpen = mNext;
            }
        }

        void end(S32 tag)
        {
            if (mOpen < 0)
            {
                return;
            }
            Slot& slot = mSlots[mOpen];
            glQueryCounter(slot.mQuery[1], GL_TIMESTAMP);
            slot.mTag = tag;
            slot.mPending = true;
            mNext = (mOpen + 1) % QUERY_SLOTS;
            mOpen = -1;
        }

        // Adds finished timings to sum[tag] / count[tag], without waiting
        void poll(F64* sum, U32* count)
        {
            for (Slot& slot : mSlots)
            {
                if (!slot.mPending)
                {
                    continue;
                }
                GLuint available = GL_FALSE;
                glGetQueryObjectuiv(slot.mQuery[1], GL_QUERY_RESULT_AVAILABLE, &available);
                if (!available)
                {
                    continue;
                }
                GLuint64 t0 = 0;
                GLuint64 t1 = 0;
                glGetQueryObjectui64v(slot.mQuery[0], GL_QUERY_RESULT, &t0);
                glGetQueryObjectui64v(slot.mQuery[1], GL_QUERY_RESULT, &t1);
                slot.mPending = false;
                if (t1 > t0)
                {
                    sum[slot.mTag] += (F64)(t1 - t0) / 1000000.0;
                    ++count[slot.mTag];
                }
            }
        }

        void release()
        {
            if (mCreated)
            {
                for (Slot& slot : mSlots)
                {
                    glDeleteQueries(2, slot.mQuery);
                    slot = Slot();
                }
            }
            mCreated = false;
            mOpen = -1;
            mNext = 0;
        }

    private:
        struct Slot
        {
            GLuint mQuery[2] = { 0, 0 };
            S32 mTag = 0;
            bool mPending = false;
        };
        Slot mSlots[QUERY_SLOTS];
        S32 mOpen = -1;
        S32 mNext = 0;
        bool mCreated = false;
    };

    LLGLSLShader sClassifyProgram;
    LLGLSLShader sRateProgram;
    bool sShadersTried = false;
    bool sShadersOK = false;

    // Hardware limits, queried once per GL context
    bool sCapsQueried = false;
    bool sCapsOK = false;
    S32 sTexelW = 16;
    S32 sTexelH = 16;
    S32 sMaxLevel = 1;              // 2 when 4 pixel wide rates are allowed

    LLRenderTarget sLevels;         // pass 1 output, RGBA8, one texel per tile
    GLuint sRateTex = 0;            // the shading rate image, R8UI, immutable
    GLuint sRateFBO = 0;
    U32 sTilesX = 0;
    U32 sTilesY = 0;

    bool sHaveImage = false;
    U32 sBuiltWidth = 0;            // G-buffer size the image is for
    U32 sBuiltHeight = 0;
    U32 sBuiltFrame = 0;
    bool sActive = false;           // the G-buffer pass is being shaded with the image
    bool sInGBuffer = false;

    TimerRing sGBufferTimer;        // tag 1 = with VRS, 0 = full rate
    TimerRing sBuildTimer;
    F64 sGBufferMs[2] = { 0.0, 0.0 };
    U32 sGBufferFrames[2] = { 0, 0 };
    F64 sBuildMs[1] = { 0.0 };
    U32 sBuildFrames[1] = { 0 };

    GLuint sStatsPBO = 0;
    U32 sStatsBytes = 0;
    U32 sStatsTilesX = 0;
    U32 sStatsTilesY = 0;
    GLsync sStatsFence = nullptr;
    F64 sLastStats = 0.0;

    bool sAllocated = false;        // anything above needs releasing

    F64 now_seconds()
    {
        return LLTimer::getTotalSeconds().value();
    }

    bool enabled()
    {
        static LLCachedControl<bool> vrs(gSavedSettings, "FSRenderVRS", false);
        return vrs && gGLManager.mHasNVShadingRateImage && gGLManager.mHasDSA;
    }

    // Main view frames only
    bool main_view(bool for_snapshot)
    {
        return !for_snapshot && !gSnapshot && !gCubeSnapshot && gPipeline.mRT == &gPipeline.mMainRT;
    }

    bool query_caps()
    {
        if (sCapsQueried)
        {
            return sCapsOK;
        }
        sCapsQueried = true;
        sAllocated = true;

        GLint texel_w = 0;
        GLint texel_h = 0;
        GLint palette = 0;
        GLint coarse_samples = 0;
        glGetIntegerv(GL_SHADING_RATE_IMAGE_TEXEL_WIDTH_NV, &texel_w);
        glGetIntegerv(GL_SHADING_RATE_IMAGE_TEXEL_HEIGHT_NV, &texel_h);
        glGetIntegerv(GL_SHADING_RATE_IMAGE_PALETTE_SIZE_NV, &palette);
        glGetIntegerv(GL_MAX_COARSE_FRAGMENT_SAMPLES_NV, &coarse_samples);

        sTexelW = texel_w;
        sTexelH = texel_h;
        // 4x4 covers 16 samples of a single sampled target, 4x2 and 2x4 cover 8
        sMaxLevel = coarse_samples >= 16 ? 2 : 1;
        sCapsOK = texel_w > 0 && texel_h > 0 && texel_w <= MAX_TILE && texel_h <= MAX_TILE && palette >= PALETTE_SIZE && coarse_samples >= 4;

        LL_INFOS("VRS") << "Shading rate image: texel " << texel_w << "x" << texel_h << ", palette " << palette
                        << ", max coarse samples " << coarse_samples << (sCapsOK ? "" : ", not usable") << LL_ENDL;
        return sCapsOK;
    }

    bool load_shaders()
    {
        // LLViewerShaderMgr::unloadShaders() unloads every LLGLSLShader, these included: a
        // shader reload shows as built programs that are no longer complete.
        if (sShadersTried && (!sShadersOK || (sClassifyProgram.isComplete() && sRateProgram.isComplete())))
        {
            return sShadersOK;
        }
        if (LLViewerShaderMgr::instance()->mShaderLevel[LLViewerShaderMgr::SHADER_DEFERRED] <= 0)
        {
            return false; // shaders are being reloaded
        }
        sShadersTried = true;

        struct Program
        {
            LLGLSLShader* mShader;
            const char* mName;
            const char* mFile;
        };
        const Program programs[] = {
            { &sClassifyProgram, "FS VRS Classify", "deferred/fsVRSClassifyF.glsl" },
            { &sRateProgram,     "FS VRS Rate",     "deferred/fsVRSRateF.glsl" },
        };

        sShadersOK = true;
        for (const Program& program : programs)
        {
            program.mShader->mName = program.mName;
            program.mShader->mShaderFiles.clear();
            program.mShader->clearPermutations();
            program.mShader->mShaderFiles.push_back(std::make_pair("deferred/postDeferredNoTCV.glsl", GL_VERTEX_SHADER));
            program.mShader->mShaderFiles.push_back(std::make_pair(program.mFile, GL_FRAGMENT_SHADER));
            program.mShader->mShaderLevel = LLViewerShaderMgr::instance()->mShaderLevel[LLViewerShaderMgr::SHADER_DEFERRED];
            if (!program.mShader->createShader())
            {
                LL_WARNS("VRS") << program.mName << " failed to build, variable rate shading stays off" << LL_ENDL;
                sShadersOK = false;
            }
        }
        if (!sShadersOK)
        {
            sClassifyProgram.unload();
            sRateProgram.unload();
        }
        return sShadersOK;
    }

    void release_image()
    {
        sLevels.release();
        if (sRateFBO)
        {
            glDeleteFramebuffers(1, &sRateFBO);
            sRateFBO = 0;
        }
        if (sRateTex)
        {
            glDeleteTextures(1, &sRateTex);
            sRateTex = 0;
        }
        sTilesX = sTilesY = 0;
        sHaveImage = false;
    }

    bool ensure_image(U32 tiles_x, U32 tiles_y)
    {
        if (sRateTex && sTilesX == tiles_x && sTilesY == tiles_y)
        {
            return true;
        }
        release_image();
        sAllocated = true;

        if (!sLevels.allocate(tiles_x, tiles_y, GL_RGBA8))
        {
            release_image();
            return false;
        }

        // The shading rate image must be an immutable R8UI texture
        glCreateTextures(GL_TEXTURE_2D, 1, &sRateTex);
        glTextureStorage2D(sRateTex, 1, GL_R8UI, tiles_x, tiles_y);
        glTextureParameteri(sRateTex, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTextureParameteri(sRateTex, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        glCreateFramebuffers(1, &sRateFBO);
        glNamedFramebufferTexture(sRateFBO, GL_COLOR_ATTACHMENT0, sRateTex, 0);
        if (glCheckNamedFramebufferStatus(sRateFBO, GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE)
        {
            LL_WARNS_ONCE("VRS") << "Shading rate image framebuffer incomplete, variable rate shading stays off" << LL_ENDL;
            release_image();
            return false;
        }

        sTilesX = tiles_x;
        sTilesY = tiles_y;
        return true;
    }

    // One line with the share of tiles at each rate and the timings since the last line
    void log_stats(const U8* rates, U32 count)
    {
        U32 histogram[PALETTE_SIZE] = { 0 };
        for (U32 i = 0; i < count; ++i)
        {
            if (rates[i] < PALETTE_SIZE)
            {
                ++histogram[rates[i]];
            }
        }

        std::ostringstream line;
        line << std::fixed << std::setprecision(1) << "VRS " << sStatsTilesX << "x" << sStatsTilesY << " tiles of "
             << sTexelW << "x" << sTexelH << ":";
        for (S32 i = 0; i < PALETTE_SIZE; ++i)
        {
            line << " " << RATE_NAMES[i] << " " << (count ? 100.0 * histogram[i] / count : 0.0) << "%";
        }
        line << std::setprecision(3) << " | G-buffer ";
        if (sGBufferFrames[1])
        {
            line << sGBufferMs[1] / sGBufferFrames[1] << " ms (" << sGBufferFrames[1] << " VRS frames)";
        }
        else
        {
            line << "- (no VRS frames)";
        }
        if (sGBufferFrames[0])
        {
            line << ", " << sGBufferMs[0] / sGBufferFrames[0] << " ms (" << sGBufferFrames[0] << " full rate frames)";
        }
        if (sBuildFrames[0])
        {
            line << " | rate image " << sBuildMs[0] / sBuildFrames[0] << " ms";
        }
        LL_INFOS("VRS") << line.str() << LL_ENDL;

        sGBufferMs[0] = sGBufferMs[1] = 0.0;
        sGBufferFrames[0] = sGBufferFrames[1] = 0;
        sBuildMs[0] = 0.0;
        sBuildFrames[0] = 0;
    }

    // Finishes a read back started by request_stats() once its fence has passed
    void poll_stats()
    {
        if (!sStatsFence)
        {
            return;
        }
        GLenum status = glClientWaitSync(sStatsFence, 0, 0);
        if (status != GL_ALREADY_SIGNALED && status != GL_CONDITION_SATISFIED)
        {
            return;
        }
        glDeleteSync(sStatsFence);
        sStatsFence = nullptr;

        const U32 count = sStatsTilesX * sStatsTilesY;
        glBindBuffer(GL_PIXEL_PACK_BUFFER, sStatsPBO);
        const U8* rates = (const U8*)glMapBufferRange(GL_PIXEL_PACK_BUFFER, 0, count, GL_MAP_READ_BIT);
        if (rates)
        {
            log_stats(rates, count);
            glUnmapBuffer(GL_PIXEL_PACK_BUFFER);
        }
        glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);
    }

    // Copies the freshly built image into the PBO. sRateFBO must be bound.
    void request_stats()
    {
        static LLCachedControl<F32> interval(gSavedSettings, "FSRenderVRSLogInterval", 5.f);
        const F64 now = now_seconds();
        if (interval <= 0.f || sStatsFence || now - sLastStats < (F64)interval)
        {
            return;
        }
        sLastStats = now;

        const U32 bytes = sTilesX * sTilesY;
        if (!sStatsPBO)
        {
            glGenBuffers(1, &sStatsPBO);
            sStatsBytes = 0;
        }
        glBindBuffer(GL_PIXEL_PACK_BUFFER, sStatsPBO);
        if (sStatsBytes != bytes)
        {
            glBufferData(GL_PIXEL_PACK_BUFFER, bytes, nullptr, GL_STREAM_READ);
            sStatsBytes = bytes;
        }
        GLint pack_alignment = 4;
        glGetIntegerv(GL_PACK_ALIGNMENT, &pack_alignment);
        glPixelStorei(GL_PACK_ALIGNMENT, 1);
        glReadPixels(0, 0, sTilesX, sTilesY, GL_RED_INTEGER, GL_UNSIGNED_BYTE, nullptr);
        glPixelStorei(GL_PACK_ALIGNMENT, pack_alignment);
        glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);
        sStatsFence = glFenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0);
        sStatsTilesX = sTilesX;
        sStatsTilesY = sTilesY;
    }

    void release_all()
    {
        if (sActive)
        {
            glDisable(GL_SHADING_RATE_IMAGE_NV);
            fs_glBindShadingRateImageNV(0);
            sActive = false;
        }
        release_image();
        sGBufferTimer.release();
        sBuildTimer.release();
        if (sStatsFence)
        {
            glDeleteSync(sStatsFence);
            sStatsFence = nullptr;
        }
        if (sStatsPBO)
        {
            glDeleteBuffers(1, &sStatsPBO);
            sStatsPBO = 0;
        }
        sStatsBytes = 0;
        sGBufferMs[0] = sGBufferMs[1] = 0.0;
        sGBufferFrames[0] = sGBufferFrames[1] = 0;
        sBuildMs[0] = 0.0;
        sBuildFrames[0] = 0;
        sCapsQueried = false;
        sInGBuffer = false;
        sAllocated = false;
    }
}

// static
void FSVRS::beginGBuffer(bool for_snapshot)
{
    if (!enabled())
    {
        if (sAllocated)
        {
            release_all();
        }
        return;
    }
    if (!main_view(for_snapshot) || !query_caps())
    {
        return;
    }

    sGBufferTimer.poll(sGBufferMs, sGBufferFrames);
    sBuildTimer.poll(sBuildMs, sBuildFrames);
    poll_stats();

    // The image must be fresh and built for this G-buffer's size
    const LLRenderTarget& gbuffer = gPipeline.mRT->deferredScreen;
    sActive = sHaveImage && sRateTex &&
              sBuiltWidth == gbuffer.getWidth() && sBuiltHeight == gbuffer.getHeight() &&
              LLFrameTimer::getFrameCount() - sBuiltFrame <= 2;

    sInGBuffer = true;
    sGBufferTimer.begin();

    if (sActive)
    {
        fs_glBindShadingRateImageNV(sRateTex);
        fs_glShadingRateImagePaletteNV(0, 0, PALETTE_SIZE, PALETTE);
        glEnable(GL_SHADING_RATE_IMAGE_NV);
    }
}

// static
void FSVRS::endGBuffer()
{
    if (!sInGBuffer)
    {
        return;
    }
    sInGBuffer = false;

    const bool active = sActive;
    if (sActive)
    {
        glDisable(GL_SHADING_RATE_IMAGE_NV);
        fs_glBindShadingRateImageNV(0);
        sActive = false;
    }
    sGBufferTimer.end(active ? 1 : 0);
}

// static
void FSVRS::buildRateImage(LLRenderTarget* color, LLRenderTarget* depth)
{
    if (!enabled() || !main_view(false) || !color || !depth)
    {
        return;
    }
    if (!query_caps() || !load_shaders())
    {
        return;
    }

    const U32 width = color->getWidth();
    const U32 height = color->getHeight();
    if (width == 0 || height == 0 || depth->getWidth() != width || depth->getHeight() != height)
    {
        return;
    }
    const U32 tiles_x = (width + sTexelW - 1) / sTexelW;
    const U32 tiles_y = (height + sTexelH - 1) / sTexelH;
    if (!ensure_image(tiles_x, tiles_y))
    {
        return;
    }

    LL_PROFILE_GPU_ZONE("vrs rate image");
    sBuildTimer.begin();

    LLGLDisable blend(GL_BLEND);
    LLGLDisable cull(GL_CULL_FACE);
    LLGLDepthTest depth_test(GL_FALSE, GL_FALSE);
    gGL.setColorMask(true, true);

    // Quality 1 keeps nearly everything at full rate, 0 is the most aggressive
    static LLCachedControl<F32> quality(gSavedSettings, "FSRenderVRSQuality", 0.5f);
    static LLCachedControl<F32> periphery(gSavedSettings, "FSRenderVRSPeriphery", 0.f);
    const F32 threshold = lerp(0.05f, 0.005f, llclamp((F32)quality, 0.f, 1.f));

    const LLPipeline::LensDoF& dof = gPipeline.mLensDoF;
    const bool use_dof = dof.mActive && dof.mMaxCoF > 0.f && dof.mMagnification != 0.f &&
                         dof.mTanPixelAngle * -dof.mFocalDistance != 0.f;
    const glm::mat4 proj = get_current_projection();

    static LLStaticHashedString s_src_size("fs_vrs_src_size");
    static LLStaticHashedString s_tile("fs_vrs_tile");
    static LLStaticHashedString s_threshold("fs_vrs_threshold");
    static LLStaticHashedString s_periphery("fs_vrs_periphery");
    static LLStaticHashedString s_max_level("fs_vrs_max_level");
    static LLStaticHashedString s_proj("fs_vrs_proj");
    static LLStaticHashedString s_dof("fs_vrs_dof");
    static LLStaticHashedString s_dof_params("fs_vrs_dof_params");
    static LLStaticHashedString s_dof_max("fs_vrs_dof_max");
    static LLStaticHashedString s_tiles("fs_vrs_tiles");

    { // pass 1: levels per tile
        sLevels.bindTarget();
        sClassifyProgram.bind();
        sClassifyProgram.bindTexture(LLShaderMgr::DEFERRED_DIFFUSE, color, false, LLTexUnit::TFO_POINT);
        sClassifyProgram.bindTexture(LLShaderMgr::DEFERRED_DEPTH, depth, true, LLTexUnit::TFO_POINT);
        sClassifyProgram.uniform2i(s_src_size, (GLint)width, (GLint)height);
        sClassifyProgram.uniform2i(s_tile, sTexelW, sTexelH);
        sClassifyProgram.uniform1f(s_threshold, threshold);
        sClassifyProgram.uniform1f(s_periphery, llclamp((F32)periphery, 0.f, 1.f));
        sClassifyProgram.uniform1i(s_max_level, sMaxLevel);
        sClassifyProgram.uniform2f(s_proj, proj[2][2], proj[3][2]);
        sClassifyProgram.uniform1i(s_dof, use_dof ? 1 : 0);
        sClassifyProgram.uniform4f(s_dof_params, dof.mFocalDistance, dof.mBlurConstant, dof.mMagnification,
                                   dof.mTanPixelAngle * -dof.mFocalDistance);
        sClassifyProgram.uniform1f(s_dof_max, dof.mMaxCoF);

        gPipeline.mScreenTriangleVB->setBuffer();
        gPipeline.mScreenTriangleVB->drawArrays(LLRender::TRIANGLES, 0, 3);

        sClassifyProgram.unbind();
        sLevels.flush();
    }

    { // pass 2: neighbour limit and palette index, into the R8UI image
        GLint viewport[4];
        glGetIntegerv(GL_VIEWPORT, viewport);
        glBindFramebuffer(GL_FRAMEBUFFER, sRateFBO);
        glViewport(0, 0, sTilesX, sTilesY);

        sRateProgram.bind();
        sRateProgram.bindTexture(LLShaderMgr::DEFERRED_DIFFUSE, &sLevels, false, LLTexUnit::TFO_POINT);
        sRateProgram.uniform2i(s_tiles, (GLint)sTilesX, (GLint)sTilesY);

        gPipeline.mScreenTriangleVB->setBuffer();
        gPipeline.mScreenTriangleVB->drawArrays(LLRender::TRIANGLES, 0, 3);

        sRateProgram.unbind();

        request_stats();

        glBindFramebuffer(GL_FRAMEBUFFER, LLRenderTarget::sCurFBO);
        glViewport(viewport[0], viewport[1], viewport[2], viewport[3]);
    }

    // Make the writes visible to the next shading rate image lookups
    fs_glShadingRateImageBarrierNV(GL_TRUE);

    sBuildTimer.end(0);

    sHaveImage = true;
    sBuiltWidth = width;
    sBuiltHeight = height;
    sBuiltFrame = LLFrameTimer::getFrameCount();
}

// static
void FSVRS::releaseGL()
{
    if (sAllocated)
    {
        release_all();
    }
}

#else // !LL_GL_FUNC_POINTER: the extension is never available

// static
void FSVRS::beginGBuffer(bool for_snapshot)
{
}

// static
void FSVRS::endGBuffer()
{
}

// static
void FSVRS::buildRateImage(LLRenderTarget* color, LLRenderTarget* depth)
{
}

// static
void FSVRS::releaseGL()
{
}

#endif // LL_GL_FUNC_POINTER
