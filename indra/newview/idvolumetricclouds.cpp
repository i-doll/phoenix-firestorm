/**
 * @file idvolumetricclouds.cpp
 * @brief <ID> Volumetric cloud renderer: noise and weather textures, gating, per-frame update.
 *
 * Part of Five's custom Firestorm fork.
 */

#include "llviewerprecompiledheaders.h"

#include "idvolumetricclouds.h"

#include "idcloudnoise.h"

#include "idvolumetriccloudsettings.h"
#include "llagent.h"
#include "llappviewer.h"
#include "lldir.h"
#include "lldrawpoolalpha.h"
#include "llenvironment.h"
#include "llfile.h"
#include "llframetimer.h"
#include "llgl.h"
#include "llglslshader.h"
#include "llimagegl.h"
#include "llrender.h"
#include "llviewercamera.h"
#include "llviewercontrol.h"
#include "llviewershadermgr.h"
#include "pipeline.h"
#include "rlvactions.h"
#include "workqueue.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <numeric>
#include <string>

extern bool gSnapshot;
extern bool gCubeSnapshot;

struct IDCloudNoiseJob
{
    std::string mDir;
    std::string mBasePath;
    std::string mDetailPath;
    std::vector<U8> mBase;
    std::vector<U8> mDetail;
    bool mOk = false;
    bool mFromCache = false;
    F32 mSeconds = 0.f;
    std::atomic<bool> mCancel{ false };
    std::atomic<bool> mDone{ false };
};

using IDCloudWeatherLayer = idcloudnoise::WeatherLayer;

struct IDCloudWeatherJob
{
    std::vector<IDCloudWeatherLayer> mLayers;
    std::vector<U32> mKey;
    std::vector<U8> mPixels;
    std::atomic<bool> mCancel{ false };
    std::atomic<bool> mDone{ false };
};

namespace
{
    using idcloudnoise::BASE_DIM;
    using idcloudnoise::DETAIL_DIM;
    using idcloudnoise::WEATHER_DIM;

    constexpr S32 FULL_STEPS   = 96;
    constexpr S32 LIVE_STEPS   = 48;
    constexpr S32 FULL_SAMPLES = 4;
    constexpr S32 FULL_LIGHT_STEPS = 6;
    constexpr S32 PROBE_STEPS = 32;
    constexpr S32 PROBE_LIGHT_STEPS = 2;
    constexpr U32 PROBE_DIVISOR = 4;
    constexpr S32 MARCH_BLOCK_X = 4;
    constexpr S32 MARCH_BLOCK_Y = 2;
    // Bayer-like order over the 4x2 block, so pixels refreshed on consecutive frames lie far apart.
    const glm::ivec2 MARCH_ORDER[MARCH_BLOCK_X * MARCH_BLOCK_Y] = {
        { 0, 0 }, { 2, 1 }, { 2, 0 }, { 0, 1 }, { 1, 0 }, { 3, 1 }, { 3, 0 }, { 1, 1 }
    };
    constexpr F64 HISTORY_MAX_JUMP_M = 64.0;
    constexpr F32 PLANET_RADIUS_M = 6360000.f;

    constexpr U32 SHADOW_RES = 1024;
    constexpr F32 SHADOW_SIZE_M = 16000.f;
    constexpr F32 SHADOW_MIN_LIGHT_RISE = 0.05f;

    // ID_FROXEL_SLICES in idCloudUtil.glsl must match FROXEL_D.
    constexpr S32 FROXEL_W = 160;
    constexpr S32 FROXEL_H = 90;
    constexpr S32 FROXEL_D = 64;
    constexpr F32 FROXEL_FAR_M = 1500.f;
    constexpr S32 FROXEL_LIGHT_STEPS = 2;

    const char* CACHE_DIR   = "volumetric_clouds";
    const char* BASE_FILE   = "noise_base_v1_128.raw";
    const char* DETAIL_FILE = "noise_detail_v1_32.raw";

    bool readCache(const std::string& path, size_t size, std::vector<U8>& out)
    {
        llifstream in(path, std::ios::in | std::ios::binary);
        if (!in.is_open())
        {
            return false;
        }
        out.resize(size);
        in.read(reinterpret_cast<char*>(out.data()), (std::streamsize)size);
        if ((size_t)in.gcount() != size || in.peek() != std::char_traits<char>::eof())
        {
            out.clear();
            return false;
        }
        return true;
    }

    void writeCache(const std::string& path, const std::vector<U8>& data)
    {
        llofstream out(path, std::ios::out | std::ios::binary | std::ios::trunc);
        if (!out.is_open())
        {
            return;
        }
        out.write(reinterpret_cast<const char*>(data.data()), (std::streamsize)data.size());
        out.close();
        if (out.fail())
        {
            LLFile::remove(path);
        }
    }

    void runNoiseJob(IDCloudNoiseJob& job)
    {
        const auto start = std::chrono::steady_clock::now();
        const size_t base_size = (size_t)BASE_DIM * BASE_DIM * BASE_DIM * 4;
        const size_t detail_size = (size_t)DETAIL_DIM * DETAIL_DIM * DETAIL_DIM * 3;

        if (readCache(job.mBasePath, base_size, job.mBase) && readCache(job.mDetailPath, detail_size, job.mDetail))
        {
            job.mFromCache = true;
            job.mOk = true;
        }
        else if (idcloudnoise::generateBase(job.mBase, job.mCancel) && idcloudnoise::generateDetail(job.mDetail, job.mCancel))
        {
            LLFile::mkdir(job.mDir);
            writeCache(job.mBasePath, job.mBase);
            writeCache(job.mDetailPath, job.mDetail);
            job.mOk = true;
        }

        job.mSeconds = std::chrono::duration<F32>(std::chrono::steady_clock::now() - start).count();
        job.mDone.store(true, std::memory_order_release);
    }


    U32 upload3D(const std::vector<U8>& data, S32 dim, GLint internal_format, GLenum format)
    {
        U32 tex = 0;
        glGenTextures(1, &tex);
        if (tex == 0)
        {
            return 0;
        }
        gGL.getTexUnit(0)->bindManual(LLTexUnit::TT_TEXTURE_3D, tex);
        glTexImage3D(GL_TEXTURE_3D, 0, internal_format, dim, dim, dim, 0, format, GL_UNSIGNED_BYTE, data.data());
        glTexParameteri(GL_TEXTURE_3D, GL_TEXTURE_WRAP_S, GL_REPEAT);
        glTexParameteri(GL_TEXTURE_3D, GL_TEXTURE_WRAP_T, GL_REPEAT);
        glTexParameteri(GL_TEXTURE_3D, GL_TEXTURE_WRAP_R, GL_REPEAT);
        glTexParameteri(GL_TEXTURE_3D, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
        glTexParameteri(GL_TEXTURE_3D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glGenerateMipmap(GL_TEXTURE_3D);
        gGL.getTexUnit(0)->unbind(LLTexUnit::TT_TEXTURE_3D);
        return tex;
    }

    U32 createFroxelTexture()
    {
        U32 tex = 0;
        glGenTextures(1, &tex);
        if (tex == 0)
        {
            return 0;
        }
        gGL.getTexUnit(0)->bindManual(LLTexUnit::TT_TEXTURE_3D, tex);
        glTexImage3D(GL_TEXTURE_3D, 0, GL_RGBA16F, FROXEL_W, FROXEL_H, FROXEL_D, 0, GL_RGBA, GL_HALF_FLOAT, nullptr);
        glTexParameteri(GL_TEXTURE_3D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_3D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_3D, GL_TEXTURE_WRAP_R, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_3D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_3D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        gGL.getTexUnit(0)->unbind(LLTexUnit::TT_TEXTURE_3D);
        return tex;
    }

    // The alpha passes bind the froxels without a matching unbind. A unit left recording a
    // deleted name would skip glBindTexture once GL hands that name out again.
    void unbindFromAllUnits(U32 tex)
    {
        for (U32 i = 0; i < LL_NUM_TEXTURE_LAYERS; ++i)
        {
            LLTexUnit* unit = gGL.getTexUnit(i);
            if (unit && unit->getCurrTexture() == tex)
            {
                unit->disable();
            }
        }
    }
}

IDVolumetricClouds::IDVolumetricClouds()
{
}


void IDVolumetricClouds::cleanupSingleton()
{
    if (mNoiseJob)
    {
        mNoiseJob->mCancel = true;
    }
    if (mWeatherJob)
    {
        mWeatherJob->mCancel = true;
    }
    mStateConnection.disconnect();
}

// static
bool IDVolumetricClouds::isSupported()
{
#if LL_LINUX
    return gGLManager.mGLVersion >= 4.09f;
#else
    return false;
#endif
}

// static
bool IDVolumetricClouds::canEdit()
{
    return isSupported() && RlvActions::canChangeEnvironment();
}

bool IDVolumetricClouds::prerequisitesMet() const
{
    static LLCachedControl<bool> enabled(gSavedSettings, "IDVolumetricCloudsEnabled", false);
    return enabled
        && canEdit()
        && !IDCloudState::instance().current().mLayers.empty();
}

bool IDVolumetricClouds::hasFogLayer() const
{
    for (const IDCloudLayer& l : IDCloudState::instance().current().mLayers)
    {
        if (l.mType == IDCloudType::Fog)
        {
            return true;
        }
    }
    return false;
}

bool IDVolumetricClouds::isActive() const
{
    return prerequisitesMet() && mNoiseBase && mNoiseDetail && mWeather
        && gIDCloudsProgram.isComplete() && gIDCloudsCompositeProgram.isComplete();
}

bool IDVolumetricClouds::fullQuality() const
{
    static LLCachedControl<bool> full_live(gSavedSettings, "IDVolumetricCloudsFullQualityLive", false);
    return gSnapshot || full_live;
}

void IDVolumetricClouds::update()
{
    // Reflection probes keep their last render until their turn to refresh comes round, which can
    // take a long time, so a cloud switch would linger in reflections without a reset.
    if (!gCubeSnapshot)
    {
        const bool active = isActive();
        if (active != mWasActive)
        {
            mWasActive = active;
            gPipeline.mReflectionMapManager.reset();
            if (!active)
            {
                for (U32 slot = 0; slot < 2; ++slot)
                {
                    mProbeMarchRT[slot].release();
                    mProbeSceneRT[slot].release();
                }
                mSceneRT.release();
                mSceneFrame = U32_MAX;
                mMarchRT.release();
                mHistoryRT[0].release();
                mHistoryRT[1].release();
                mHistoryValid = false;
                mShadowRT.release();
                mShadowReady = false;
                releaseFroxels();
            }
        }
    }

    static LLCachedControl<bool> enabled(gSavedSettings, "IDVolumetricCloudsEnabled", false);
    if (!enabled || !isSupported())
    {
        mShadowReady = false;
        return;
    }

    const U32 frame = LLFrameTimer::getFrameCount();
    if (frame != mLastFrame)
    {
        mLastFrame = frame;

        if (!mStateConnection.connected())
        {
            mStateConnection = IDCloudState::instance().mChanged.connect([this]() { mWeatherDirty = true; });
            mWeatherDirty = true;
        }

        updateNoise();
        updateWeather();

        if (isActive())
        {
            IDCloudState::instance().advance(gFrameIntervalSeconds.value());
        }
    }

    // Probe renders move the viewer camera to the probe, so the map follows the main camera only.
    if (!gCubeSnapshot && frame != mShadowFrame)
    {
        mShadowFrame = frame;
        renderShadowMap();
    }
}

void IDVolumetricClouds::updateNoise()
{
    if (mNoiseBase || mNoiseFailed)
    {
        return;
    }

    if (!mNoiseJob)
    {
        if (!prerequisitesMet())
        {
            return;
        }

        LL::WorkQueue::ptr_t queue = LL::WorkQueue::getInstance("General");
        if (!queue)
        {
            LL_WARNS("VolumetricClouds") << "No General work queue; volumetric clouds unavailable" << LL_ENDL;
            mNoiseFailed = true;
            return;
        }

        auto job = std::make_shared<IDCloudNoiseJob>();
        job->mDir = gDirUtilp->getExpandedFilename(LL_PATH_CACHE, CACHE_DIR);
        job->mBasePath = gDirUtilp->getExpandedFilename(LL_PATH_CACHE, CACHE_DIR, BASE_FILE);
        job->mDetailPath = gDirUtilp->getExpandedFilename(LL_PATH_CACHE, CACHE_DIR, DETAIL_FILE);
        if (!queue->post([job]() { runNoiseJob(*job); }))
        {
            LL_WARNS("VolumetricClouds") << "General work queue closed; volumetric clouds unavailable" << LL_ENDL;
            mNoiseFailed = true;
            return;
        }
        mNoiseJob = job;
        return;
    }

    if (!mNoiseJob->mDone.load(std::memory_order_acquire))
    {
        return;
    }

    std::shared_ptr<IDCloudNoiseJob> job = std::move(mNoiseJob);
    if (!job->mOk)
    {
        LL_WARNS("VolumetricClouds") << "Noise generation was cancelled" << LL_ENDL;
        mNoiseFailed = true;
        return;
    }

    mNoiseBase = upload3D(job->mBase, BASE_DIM, GL_RGBA8, GL_RGBA);
    mNoiseDetail = upload3D(job->mDetail, DETAIL_DIM, GL_RGB8, GL_RGB);
    if (!mNoiseBase || !mNoiseDetail)
    {
        LL_WARNS("VolumetricClouds") << "Could not create noise textures" << LL_ENDL;
        release();
        mNoiseFailed = true;
        return;
    }

    LL_INFOS("VolumetricClouds") << "Noise textures " << (job->mFromCache ? "loaded from cache" : "generated")
                                 << " in " << job->mSeconds << " s" << LL_ENDL;
}

void IDVolumetricClouds::updateWeather()
{
    if (mWeatherJob && mWeatherJob->mDone.load(std::memory_order_acquire))
    {
        std::shared_ptr<IDCloudWeatherJob> job = std::move(mWeatherJob);

        if (!mWeather)
        {
            glGenTextures(1, &mWeather);
        }
        if (mWeather)
        {
            gGL.getTexUnit(0)->bindManual(LLTexUnit::TT_TEXTURE, mWeather);
            glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, WEATHER_DIM, WEATHER_DIM, 0, GL_RGBA, GL_UNSIGNED_BYTE, job->mPixels.data());
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_REPEAT);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
            glGenerateMipmap(GL_TEXTURE_2D);
            gGL.getTexUnit(0)->unbind(LLTexUnit::TT_TEXTURE);
            mWeatherKey = job->mKey;
        }
    }

    if (mWeatherJob || !mWeatherDirty)
    {
        return;
    }
    mWeatherDirty = false;

    const IDCloudPreset& preset = IDCloudState::instance().current();
    std::vector<IDCloudWeatherLayer> layers;
    std::vector<U32> key;
    for (size_t i = 0; i < preset.mLayers.size() && i < IDCloudPreset::MAX_LAYERS; ++i)
    {
        const IDCloudWeatherLayer l = idcloudnoise::weatherLayer(i, (S32)preset.mLayers[i].mType, preset.mLayers[i].mWindDirDeg);
        key.push_back((U32)l.type | ((U32)(l.dirA + 8) << 8) | ((U32)(l.dirB + 8) << 16));
        layers.push_back(l);
    }

    if (layers.empty() || (mWeather && key == mWeatherKey))
    {
        return;
    }

    LL::WorkQueue::ptr_t queue = LL::WorkQueue::getInstance("General");
    if (!queue)
    {
        return;
    }

    auto job = std::make_shared<IDCloudWeatherJob>();
    job->mLayers = std::move(layers);
    job->mKey = std::move(key);
    if (queue->post([job]()
        {
            idcloudnoise::generateWeather(job->mLayers, job->mPixels, job->mCancel);
            job->mDone.store(true, std::memory_order_release);
        }))
    {
        mWeatherJob = job;
    }
}

void IDVolumetricClouds::release()
{
    if (mNoiseBase)
    {
        LLImageGL::deleteTextures(1, &mNoiseBase);
        mNoiseBase = 0;
    }
    if (mNoiseDetail)
    {
        LLImageGL::deleteTextures(1, &mNoiseDetail);
        mNoiseDetail = 0;
    }
    if (mWeather)
    {
        LLImageGL::deleteTextures(1, &mWeather);
        mWeather = 0;
    }
    mSceneRT.release();
    mSceneFrame = U32_MAX;
    mMarchRT.release();
    mHistoryRT[0].release();
    mHistoryRT[1].release();
    mHistoryValid = false;
    for (U32 slot = 0; slot < 2; ++slot)
    {
        mProbeMarchRT[slot].release();
        mProbeSceneRT[slot].release();
    }
    mShadowRT.release();
    mShadowReady = false;
    releaseFroxels();
    mFroxelFailed = false;
    mWeatherKey.clear();
    mWeatherDirty = true;
}

void IDVolumetricClouds::renderShadowMap()
{
    mShadowReady = false;
    if (!isActive() || !gIDCloudShadowProgram.isComplete())
    {
        return;
    }
    if (!mShadowRT.isComplete() && !mShadowRT.allocate(SHADOW_RES, SHADOW_RES, GL_RGBA16F))
    {
        LL_WARNS_ONCE("VolumetricClouds") << "Could not allocate the cloud shadow map" << LL_ENDL;
        mShadowRT.release();
        return;
    }

    const std::vector<IDCloudLayer>& layers = IDCloudState::instance().current().mLayers;
    F32 lo = F32_MAX;
    F32 hi = -F32_MAX;
    glm::vec4 layer_base(0.f);
    glm::vec4 layer_top(0.f);
    for (size_t i = 0; i < layers.size() && i < IDCloudPreset::MAX_LAYERS; ++i)
    {
        lo = llmin(lo, layers[i].mBaseM);
        hi = llmax(hi, layers[i].mTopM);
        layer_base[(glm::length_t)i] = layers[i].mBaseM;
        layer_top[(glm::length_t)i] = layers[i].mTopM;
    }
    if (hi <= lo)
    {
        return;
    }

    LLVector3 light = LLEnvironment::instance().getLightDirection();
    light.normalize();
    const F32 rise = llmax(light.mV[VZ], SHADOW_MIN_LIGHT_RISE);
    const F32 drift_x = light.mV[VX] / rise;
    const F32 drift_y = light.mV[VY] / rise;

    // Snapped to whole texels so the shadows hold still while the camera moves.
    const F64 texel = (F64)SHADOW_SIZE_M / (F64)SHADOW_RES;
    const LLVector3d cam = gAgent.getPosGlobalFromAgent(LLViewerCamera::getInstance()->getOrigin());
    const LLVector3d center(std::floor(cam.mdV[VX] / texel) * texel, std::floor(cam.mdV[VY] / texel) * texel, 0.0);
    const LLVector3 center_agent = gAgent.getPosAgentFromGlobal(center);

    const F64 wrap = (F64)IDCloudState::WIND_WRAP_M;
    F64 wx = std::fmod(center.mdV[VX], wrap);
    F64 wy = std::fmod(center.mdV[VY], wrap);
    if (wx < 0.0) wx += wrap;
    if (wy < 0.0) wy += wrap;

    // Agent space to (shadow map uv, altitude). A point maps to where the light ray through it
    // crosses sea level.
    const F32 inv_size = 1.f / SHADOW_SIZE_M;
    glm::mat4 world_to_uv(0.f);
    world_to_uv[0][0] = inv_size;
    world_to_uv[1][1] = inv_size;
    world_to_uv[2][0] = -drift_x * inv_size;
    world_to_uv[2][1] = -drift_y * inv_size;
    world_to_uv[2][2] = 1.f;
    world_to_uv[3][0] = 0.5f - center_agent.mV[VX] * inv_size;
    world_to_uv[3][1] = 0.5f - center_agent.mV[VY] * inv_size;
    world_to_uv[3][3] = 1.f;
    mShadowWorldToUV = world_to_uv;
    mShadowLayerBase = layer_base;
    mShadowLayerTop = layer_top;

    static LLStaticHashedString s_center("id_cloud_shadow_center");
    static LLStaticHashedString s_size("id_cloud_shadow_size");
    static LLStaticHashedString s_drift("id_cloud_shadow_drift");

    LLGLDepthTest depth(GL_FALSE, GL_FALSE);
    LLGLDisable cull(GL_CULL_FACE);
    LLGLDisable blend(GL_BLEND);

    LLGLSLShader& shader = gIDCloudShadowProgram;
    mShadowRT.bindTarget();
    shader.bind();
    setDensityInputs(shader);
    shader.uniform2f(s_center, (F32)wx, (F32)wy);
    shader.uniform1f(s_size, SHADOW_SIZE_M);
    shader.uniform2f(s_drift, drift_x, drift_y);

    gPipeline.mScreenTriangleVB->setBuffer();
    gPipeline.mScreenTriangleVB->drawArrays(LLRender::TRIANGLES, 0, 3);

    clearDensityInputs(shader);
    shader.unbind();
    mShadowRT.flush();
    mShadowReady = true;
}

void IDVolumetricClouds::render(LLRenderTarget* screen, LLRenderTarget* depth_src)
{
    if (gCubeSnapshot)
    {
        renderProbe(screen, depth_src);
        return;
    }

    mSceneFrame = U32_MAX;
    mFroxelFrame = U32_MAX;
    mHazePending = false;
    if (!screen || !depth_src || !isActive())
    {
        mHistoryValid = false;
        return;
    }
    llassert(LLRenderTarget::getCurrentBoundTarget() == screen);

    if (!ensureSceneTarget(screen->getWidth(), screen->getHeight()))
    {
        mHistoryValid = false;
        return;
    }

    LLGLDepthTest depth(GL_FALSE, GL_FALSE);
    LLGLDisable cull(GL_CULL_FACE);

    const bool full = fullQuality();
    if (full || !renderLive(depth_src))
    {
        mHistoryValid = false;
        march(mSceneRT, depth_src, full ? FULL_STEPS : LIVE_STEPS, full ? FULL_SAMPLES : 1, FULL_LIGHT_STEPS, true,
              glm::vec2((F32)mSceneRT.getWidth(), (F32)mSceneRT.getHeight()), glm::ivec2(1), glm::ivec2(0));
    }
    composite(mSceneRT, depth_src, LLPipeline::RenderDeferredAtmospheric);
    mSceneFrame = LLFrameTimer::getFrameCount();
    mHazePending = LLPipeline::RenderDeferredAtmospheric;

    if (hasFogLayer())
    {
        renderFroxels(depth_src);
    }
    else
    {
        releaseFroxels();
    }
}

void IDVolumetricClouds::onHazeApplied()
{
    if (!gCubeSnapshot)
    {
        mHazePending = false;
    }
}

// Never touches the live history or mSceneRT.
void IDVolumetricClouds::renderProbe(LLRenderTarget* screen, LLRenderTarget* depth_src)
{
    // Like the flat clouds, kept out of irradiance maps so moving clouds don't make ambient light pop.
    if (!gPipeline.mHeroProbeManager.isMirrorPass() && !gPipeline.mReflectionMapManager.isRadiancePass())
    {
        return;
    }
    if (!screen || !depth_src || !isActive())
    {
        return;
    }
    llassert(LLRenderTarget::getCurrentBoundTarget() == screen);

    const U32 slot = gPipeline.mHeroProbeManager.isMirrorPass() ? 1 : 0;
    if (!ensureProbeTargets(slot, screen->getWidth(), screen->getHeight()))
    {
        return;
    }

    LLGLDepthTest depth(GL_FALSE, GL_FALSE);
    LLGLDisable cull(GL_CULL_FACE);

    LLRenderTarget& scene = mProbeSceneRT[slot];
    const glm::vec2 size((F32)scene.getWidth(), (F32)scene.getHeight());
    if (gIDCloudsUpsampleProgram.isComplete())
    {
        const glm::vec2 grid = size / (F32)PROBE_DIVISOR;
        march(mProbeMarchRT[slot], depth_src, PROBE_STEPS, 1, PROBE_LIGHT_STEPS, false, grid, glm::ivec2(1), glm::ivec2(0));
        upsample(mProbeMarchRT[slot], scene, depth_src, grid);
    }
    else
    {
        march(scene, depth_src, PROBE_STEPS, 1, PROBE_LIGHT_STEPS, false, size, glm::ivec2(1), glm::ivec2(0));
    }

    // renderGeomPostDeferred skips the haze pass on probes at reflection detail 0.
    static LLCachedControl<S32> probe_level(gSavedSettings, "RenderReflectionProbeLevel", 0);
    composite(scene, depth_src, LLPipeline::RenderDeferredAtmospheric && probe_level() != 0);
}

bool IDVolumetricClouds::ensureProbeTargets(U32 slot, U32 width, U32 height)
{
    LLRenderTarget& march_rt = mProbeMarchRT[slot];
    LLRenderTarget& scene_rt = mProbeSceneRT[slot];
    const U32 march_w = llmax((width + PROBE_DIVISOR - 1) / PROBE_DIVISOR, 1u);
    const U32 march_h = llmax((height + PROBE_DIVISOR - 1) / PROBE_DIVISOR, 1u);
    if (march_rt.isComplete() && march_rt.getWidth() == march_w && march_rt.getHeight() == march_h
        && scene_rt.isComplete() && scene_rt.getWidth() == width && scene_rt.getHeight() == height)
    {
        return true;
    }

    march_rt.release();
    scene_rt.release();
    if (march_rt.allocate(march_w, march_h, GL_RGBA16F) && march_rt.addColorAttachment(GL_RG32F)
        && scene_rt.allocate(width, height, GL_RGBA16F) && scene_rt.addColorAttachment(GL_R32F))
    {
        return true;
    }
    LL_WARNS_ONCE("VolumetricClouds") << "Could not allocate the reflection cloud targets" << LL_ENDL;
    march_rt.release();
    scene_rt.release();
    return false;
}

bool IDVolumetricClouds::renderLive(LLRenderTarget* depth_src)
{
    if (!gIDCloudsReprojectProgram.isComplete() || !gIDCloudsUpsampleProgram.isComplete()
        || !ensureLiveTargets(mSceneRT.getWidth(), mSceneRT.getHeight()))
    {
        return false;
    }

    const LLVector3d camera = gAgent.getPosGlobalFromAgent(LLViewerCamera::getInstance()->getOrigin());
    const LLVector3d origin = gAgent.getPosGlobalFromAgent(LLVector3::zero);

    const glm::vec2 grid(mSceneRT.getWidth() * 0.5f, mSceneRT.getHeight() * 0.5f);
    LLRenderTarget& history = mHistoryRT[mHistoryIndex];
    LLRenderTarget& next = mHistoryRT[1 - mHistoryIndex];
    if (historyUsable(camera))
    {
        const glm::ivec2 offset = MARCH_ORDER[mTemporalIndex++ % (MARCH_BLOCK_X * MARCH_BLOCK_Y)];
        march(mMarchRT, depth_src, LIVE_STEPS, 1, FULL_LIGHT_STEPS, true, grid, glm::ivec2(MARCH_BLOCK_X, MARCH_BLOCK_Y), offset);

        // Agent coordinates jump by the change of region origin when the camera crosses a region.
        const LLVector3 shift(origin - mHistoryOrigin);
        glm::mat4 to_last_agent(1.f);
        to_last_agent[3] = glm::vec4(shift.mV[VX], shift.mV[VY], shift.mV[VZ], 1.f);
        const glm::mat4 to_last_eye = get_last_modelview() * to_last_agent * glm::inverse(get_current_modelview());
        reproject(next, history, depth_src, to_last_eye, grid, offset);
    }
    else
    {
        march(next, depth_src, LIVE_STEPS, 1, FULL_LIGHT_STEPS, true, grid, glm::ivec2(1), glm::ivec2(0));
    }
    upsample(next, mSceneRT, depth_src, grid);

    mHistoryIndex = 1 - mHistoryIndex;
    mHistoryValid = true;
    mHistoryFrame = LLFrameTimer::getFrameCount();
    mHistoryCamera = camera;
    mHistoryOrigin = origin;
    return true;
}

// The previous result is only reprojected from the frame right before this one, with the
// matrices of that frame, and only if the camera did not jump.
bool IDVolumetricClouds::historyUsable(const LLVector3d& camera) const
{
    return mHistoryValid
        && mHistoryFrame + 1 == LLFrameTimer::getFrameCount()
        && (camera - mHistoryCamera).length() <= HISTORY_MAX_JUMP_M;
}

bool IDVolumetricClouds::ensureLiveTargets(U32 width, U32 height)
{
    const U32 half_w = llmax((width + 1) / 2, 1u);
    const U32 half_h = llmax((height + 1) / 2, 1u);
    const U32 march_w = (half_w + MARCH_BLOCK_X - 1) / MARCH_BLOCK_X;
    const U32 march_h = (half_h + MARCH_BLOCK_Y - 1) / MARCH_BLOCK_Y;

    bool ready = mMarchRT.isComplete() && mMarchRT.getWidth() == march_w && mMarchRT.getHeight() == march_h;
    for (const LLRenderTarget& history : mHistoryRT)
    {
        ready = ready && history.isComplete() && history.getWidth() == half_w && history.getHeight() == half_h;
    }
    if (ready)
    {
        return true;
    }

    mHistoryValid = false;
    mMarchRT.release();
    bool ok = mMarchRT.allocate(march_w, march_h, GL_RGBA16F) && mMarchRT.addColorAttachment(GL_RG32F);
    for (LLRenderTarget& history : mHistoryRT)
    {
        history.release();
        ok = ok && history.allocate(half_w, half_h, GL_RGBA16F) && history.addColorAttachment(GL_RG32F);
    }
    if (ok)
    {
        return true;
    }

    LL_WARNS_ONCE("VolumetricClouds") << "Could not allocate the live cloud targets; rendering at full resolution" << LL_ENDL;
    mMarchRT.release();
    mHistoryRT[0].release();
    mHistoryRT[1].release();
    return false;
}

bool IDVolumetricClouds::ensureSceneTarget(U32 width, U32 height)
{
    if (mSceneRT.isComplete() && mSceneRT.getWidth() == width && mSceneRT.getHeight() == height)
    {
        return true;
    }
    mSceneRT.release();
    if (mSceneRT.allocate(width, height, GL_RGBA16F) && mSceneRT.addColorAttachment(GL_R32F))
    {
        return true;
    }
    LL_WARNS_ONCE("VolumetricClouds") << "Could not allocate the cloud render target" << LL_ENDL;
    mSceneRT.release();
    return false;
}

void IDVolumetricClouds::setDensityInputs(LLGLSLShader& shader)
{
    S32 channel = shader.enableTexture(LLShaderMgr::ID_CLOUD_NOISE_BASE, LLTexUnit::TT_TEXTURE_3D);
    if (channel > -1)
    {
        gGL.getTexUnit(channel)->bindManual(LLTexUnit::TT_TEXTURE_3D, mNoiseBase, true);
    }
    channel = shader.enableTexture(LLShaderMgr::ID_CLOUD_NOISE_DETAIL, LLTexUnit::TT_TEXTURE_3D);
    if (channel > -1)
    {
        gGL.getTexUnit(channel)->bindManual(LLTexUnit::TT_TEXTURE_3D, mNoiseDetail, true);
    }
    channel = shader.enableTexture(LLShaderMgr::ID_CLOUD_WEATHER, LLTexUnit::TT_TEXTURE);
    if (channel > -1)
    {
        gGL.getTexUnit(channel)->bindManual(LLTexUnit::TT_TEXTURE, mWeather, true);
    }

    static LLStaticHashedString s_layer_count("id_layer_count");
    static LLStaticHashedString s_layer_base("id_layer_base");
    static LLStaticHashedString s_layer_top("id_layer_top");
    static LLStaticHashedString s_layer_coverage("id_layer_coverage");
    static LLStaticHashedString s_layer_density("id_layer_density");
    static LLStaticHashedString s_layer_detail("id_layer_detail");
    static LLStaticHashedString s_layer_type("id_layer_type");
    static LLStaticHashedString s_layer_wind("id_layer_wind");
    static LLStaticHashedString s_layer_streak("id_layer_streak");
    static LLStaticHashedString s_planet_radius("id_planet_radius");
    static LLStaticHashedString s_min_step("id_min_step");
    static LLStaticHashedString s_probe_min("id_probe_min");
    static LLStaticHashedString s_camera_global("id_camera_global");

    constexpr U32 MAX = IDCloudPreset::MAX_LAYERS;
    F32 base[MAX] = {};
    F32 top[MAX] = {};
    F32 coverage[MAX] = {};
    F32 density[MAX] = {};
    F32 detail[MAX] = {};
    GLint type[MAX] = {};
    F32 wind[MAX * 2] = {};
    F32 streak[MAX * 2] = {};

    const IDCloudState& state = IDCloudState::instance();
    const std::vector<IDCloudLayer>& layers = state.current().mLayers;
    const U32 count = llmin((U32)layers.size(), MAX);
    for (U32 i = 0; i < count; ++i)
    {
        const IDCloudLayer& l = layers[i];
        base[i] = l.mBaseM;
        top[i] = l.mTopM;
        coverage[i] = l.mCoverage;
        density[i] = l.mDensity;
        detail[i] = l.mDetail;
        type[i] = (GLint)l.mType;
        const LLVector2 offset = state.windOffsetM(i);
        wind[i * 2] = offset.mV[VX];
        wind[i * 2 + 1] = offset.mV[VY];
        S32 sa = 1;
        S32 sb = 0;
        idcloudnoise::cirrusLattice(l.mWindDirDeg, sa, sb);
        streak[i * 2] = (F32)sa;
        streak[i * 2 + 1] = (F32)sb;
    }

    shader.uniform1i(s_layer_count, (GLint)count);
    shader.uniform1fv(s_layer_base, MAX, base);
    shader.uniform1fv(s_layer_top, MAX, top);
    shader.uniform1fv(s_layer_coverage, MAX, coverage);
    shader.uniform1fv(s_layer_density, MAX, density);
    shader.uniform1fv(s_layer_detail, MAX, detail);
    shader.uniform1iv(s_layer_type, MAX, type);
    shader.uniform2fv(s_layer_wind, MAX, wind);
    shader.uniform2fv(s_layer_streak, MAX, streak);
    shader.uniform1f(s_planet_radius, PLANET_RADIUS_M);
    const bool fog = hasFogLayer();
    shader.uniform1f(s_min_step, fog ? 1.f : 8.f);
    shader.uniform1f(s_probe_min, fog ? 10.f : 50.f);

    const F64 wrap = (F64)IDCloudState::WIND_WRAP_M;
    const LLVector3d cam = gAgent.getPosGlobalFromAgent(LLViewerCamera::getInstance()->getOrigin());
    F64 cx = std::fmod(cam.mdV[VX], wrap);
    F64 cy = std::fmod(cam.mdV[VY], wrap);
    if (cx < 0.0) cx += wrap;
    if (cy < 0.0) cy += wrap;
    shader.uniform3f(s_camera_global, (F32)cx, (F32)cy, (F32)cam.mdV[VZ]);
}

void IDVolumetricClouds::clearDensityInputs(LLGLSLShader& shader)
{
    S32 channel = shader.getTextureChannel(LLShaderMgr::ID_CLOUD_NOISE_BASE);
    if (channel > -1)
    {
        gGL.getTexUnit(channel)->unbind(LLTexUnit::TT_TEXTURE_3D);
        shader.disableTexture(LLShaderMgr::ID_CLOUD_NOISE_BASE, LLTexUnit::TT_TEXTURE_3D);
    }
    channel = shader.getTextureChannel(LLShaderMgr::ID_CLOUD_NOISE_DETAIL);
    if (channel > -1)
    {
        gGL.getTexUnit(channel)->unbind(LLTexUnit::TT_TEXTURE_3D);
        shader.disableTexture(LLShaderMgr::ID_CLOUD_NOISE_DETAIL, LLTexUnit::TT_TEXTURE_3D);
    }
    channel = shader.getTextureChannel(LLShaderMgr::ID_CLOUD_WEATHER);
    if (channel > -1)
    {
        gGL.getTexUnit(channel)->unbind(LLTexUnit::TT_TEXTURE);
        shader.disableTexture(LLShaderMgr::ID_CLOUD_WEATHER, LLTexUnit::TT_TEXTURE);
    }
}

void IDVolumetricClouds::march(LLRenderTarget& target, LLRenderTarget* depth_src, S32 steps, S32 samples, S32 light_steps, bool detail,
                               const glm::vec2& grid, const glm::ivec2& block, const glm::ivec2& offset)
{
    static LLStaticHashedString s_eye_to_world("id_eye_to_world");
    static LLStaticHashedString s_steps("id_steps");
    static LLStaticHashedString s_samples("id_samples");
    static LLStaticHashedString s_light_steps("id_light_steps");
    static LLStaticHashedString s_detail_noise("id_detail_noise");
    static LLStaticHashedString s_grid("id_march_grid");
    static LLStaticHashedString s_block("id_march_block");
    static LLStaticHashedString s_offset("id_march_offset");
    static LLStaticHashedString s_fog_shadows("id_fog_shadows");
    static LLStaticHashedString s_shadow_range("id_shadow_range");
    static LLStaticHashedString s_clouds_active("id_clouds_active");

    LLGLSLShader& shader = gIDCloudsProgram;
    LLGLDisable blend(GL_BLEND);

    target.bindTarget();
    gPipeline.bindDeferredShader(shader, nullptr, depth_src);
    setDensityInputs(shader);

    const glm::mat4 eye_to_world(glm::mat3(glm::inverse(get_current_modelview())));
    shader.uniformMatrix4fv(s_eye_to_world, 1, GL_FALSE, glm::value_ptr(eye_to_world));
    shader.uniform1i(s_steps, steps);
    shader.uniform1i(s_samples, samples);
    shader.uniform1i(s_light_steps, light_steps);
    shader.uniform1i(s_detail_noise, detail ? 1 : 0);
    shader.uniform2f(s_grid, grid.x, grid.y);
    shader.uniform2f(s_block, (F32)block.x, (F32)block.y);
    shader.uniform2f(s_offset, (F32)offset.x, (F32)offset.y);
    shader.uniform1i(s_fog_shadows, hasFogLayer() && LLPipeline::RenderShadowDetail > 0 ? 1 : 0);
    shader.uniform1f(s_shadow_range, gPipeline.mSunClipPlanes.mV[VW]);
    // The light march already dims the sun by cloud; the cloud shadow map would dim it twice.
    shader.uniform1i(s_clouds_active, 0);

    LLEnvironment& environment = LLEnvironment::instance();
    shader.uniform1i(LLShaderMgr::SUN_UP_FACTOR, environment.getIsSunUp() ? 1 : 0);
    shader.uniform3fv(LLShaderMgr::LIGHTNORM, 1, environment.getClampedLightNorm().mV);

    gPipeline.mScreenTriangleVB->setBuffer();
    gPipeline.mScreenTriangleVB->drawArrays(LLRender::TRIANGLES, 0, 3);

    clearDensityInputs(shader);
    gPipeline.unbindDeferredShader(shader);
    target.flush();
}

void IDVolumetricClouds::reproject(LLRenderTarget& target, LLRenderTarget& history, LLRenderTarget* depth_src,
                                   const glm::mat4& to_last_eye, const glm::vec2& grid, const glm::ivec2& offset)
{
    static LLStaticHashedString s_reproject("id_reproject");
    static LLStaticHashedString s_last_proj("id_last_proj");
    static LLStaticHashedString s_grid("id_march_grid");
    static LLStaticHashedString s_block("id_march_block");
    static LLStaticHashedString s_offset("id_march_offset");

    LLGLSLShader& shader = gIDCloudsReprojectProgram;
    LLGLDisable blend(GL_BLEND);

    target.bindTarget();
    gPipeline.bindDeferredShader(shader, nullptr, depth_src);
    shader.bindTexture(LLShaderMgr::ID_CLOUD_SCENE, &mMarchRT, false, LLTexUnit::TFO_POINT, 0);
    shader.bindTexture(LLShaderMgr::ID_CLOUD_DEPTH, &mMarchRT, false, LLTexUnit::TFO_POINT, 1);
    shader.bindTexture(LLShaderMgr::ID_CLOUD_HISTORY, &history, false, LLTexUnit::TFO_POINT, 0);
    shader.bindTexture(LLShaderMgr::ID_CLOUD_HISTORY_DEPTH, &history, false, LLTexUnit::TFO_POINT, 1);

    shader.uniformMatrix4fv(s_reproject, 1, GL_FALSE, glm::value_ptr(to_last_eye));
    shader.uniformMatrix4fv(s_last_proj, 1, GL_FALSE, glm::value_ptr(get_last_projection()));
    shader.uniform2f(s_grid, grid.x, grid.y);
    shader.uniform2i(s_block, MARCH_BLOCK_X, MARCH_BLOCK_Y);
    shader.uniform2i(s_offset, offset.x, offset.y);

    gPipeline.mScreenTriangleVB->setBuffer();
    gPipeline.mScreenTriangleVB->drawArrays(LLRender::TRIANGLES, 0, 3);

    shader.disableTexture(LLShaderMgr::ID_CLOUD_SCENE);
    shader.disableTexture(LLShaderMgr::ID_CLOUD_DEPTH);
    shader.disableTexture(LLShaderMgr::ID_CLOUD_HISTORY);
    shader.disableTexture(LLShaderMgr::ID_CLOUD_HISTORY_DEPTH);
    gPipeline.unbindDeferredShader(shader);
    target.flush();
}

void IDVolumetricClouds::upsample(LLRenderTarget& source, LLRenderTarget& dest, LLRenderTarget* depth_src, const glm::vec2& grid)
{
    static LLStaticHashedString s_grid("id_march_grid");

    LLGLSLShader& shader = gIDCloudsUpsampleProgram;
    LLGLDisable blend(GL_BLEND);

    dest.bindTarget();
    gPipeline.bindDeferredShader(shader, nullptr, depth_src);
    shader.bindTexture(LLShaderMgr::ID_CLOUD_SCENE, &source, false, LLTexUnit::TFO_POINT, 0);
    shader.bindTexture(LLShaderMgr::ID_CLOUD_DEPTH, &source, false, LLTexUnit::TFO_POINT, 1);
    shader.uniform2f(s_grid, grid.x, grid.y);

    gPipeline.mScreenTriangleVB->setBuffer();
    gPipeline.mScreenTriangleVB->drawArrays(LLRender::TRIANGLES, 0, 3);

    shader.disableTexture(LLShaderMgr::ID_CLOUD_SCENE);
    shader.disableTexture(LLShaderMgr::ID_CLOUD_DEPTH);
    gPipeline.unbindDeferredShader(shader);
    dest.flush();
}

void IDVolumetricClouds::composite(LLRenderTarget& scene, LLRenderTarget* depth_src, bool haze_compensate)
{
    static LLStaticHashedString s_haze_compensate("id_haze_compensate");

    LLGLSLShader& shader = gIDCloudsCompositeProgram;
    gPipeline.bindDeferredShader(shader, nullptr, depth_src);
    shader.bindTexture(LLShaderMgr::ID_CLOUD_SCENE, &scene, false, LLTexUnit::TFO_POINT, 0);

    LLEnvironment& environment = LLEnvironment::instance();
    shader.uniform1i(LLShaderMgr::SUN_UP_FACTOR, environment.getIsSunUp() ? 1 : 0);
    shader.uniform3fv(LLShaderMgr::LIGHTNORM, 1, environment.getClampedLightNorm().mV);
    shader.uniform4fv(LLShaderMgr::WATER_WATERPLANE, 1, LLDrawPoolAlpha::sWaterPlane.mV);
    shader.uniform1i(s_haze_compensate, haze_compensate ? 1 : 0);

    gGL.setColorMask(true, true);
    LLGLEnable blend(GL_BLEND);
    gGL.blendFunc(LLRender::BF_ONE, LLRender::BF_SOURCE_ALPHA, LLRender::BF_ZERO, LLRender::BF_SOURCE_ALPHA);

    gPipeline.mScreenTriangleVB->setBuffer();
    gPipeline.mScreenTriangleVB->drawArrays(LLRender::TRIANGLES, 0, 3);

    gGL.setSceneBlendType(LLRender::BT_ALPHA);
    shader.disableTexture(LLShaderMgr::ID_CLOUD_SCENE);
    gPipeline.unbindDeferredShader(shader);
}

bool IDVolumetricClouds::ensureFroxels()
{
    if (mFroxelFBO)
    {
        return true;
    }
    if (mFroxelFailed)
    {
        return false;
    }

    mFroxelSegments = createFroxelTexture();
    mFroxelVolume = createFroxelTexture();
    glGenFramebuffers(1, &mFroxelFBO);
    if (mFroxelSegments && mFroxelVolume && mFroxelFBO)
    {
        glBindFramebuffer(GL_FRAMEBUFFER, mFroxelFBO);
        glFramebufferTextureLayer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, mFroxelVolume, 0, 0);
        glDrawBuffer(GL_COLOR_ATTACHMENT0);
        const GLenum status = glCheckFramebufferStatus(GL_FRAMEBUFFER);
        glBindFramebuffer(GL_FRAMEBUFFER, LLRenderTarget::sCurFBO);
        if (status == GL_FRAMEBUFFER_COMPLETE)
        {
            return true;
        }
    }

    LL_WARNS_ONCE("VolumetricClouds") << "Could not create the fog froxel volume; see-through surfaces use the per-pixel cloud distance" << LL_ENDL;
    releaseFroxels();
    mFroxelFailed = true;
    return false;
}

void IDVolumetricClouds::releaseFroxels()
{
    if (mFroxelFBO)
    {
        glDeleteFramebuffers(1, &mFroxelFBO);
        mFroxelFBO = 0;
    }
    if (mFroxelSegments)
    {
        unbindFromAllUnits(mFroxelSegments);
        LLImageGL::deleteTextures(1, &mFroxelSegments);
        mFroxelSegments = 0;
    }
    if (mFroxelVolume)
    {
        unbindFromAllUnits(mFroxelVolume);
        LLImageGL::deleteTextures(1, &mFroxelVolume);
        mFroxelVolume = 0;
    }
    mFroxelFrame = U32_MAX;
}

// Draws into its own framebuffer behind LLRenderTarget's back, then rebinds the target that was
// bound before; draw buffers are framebuffer state, so only the viewport needs restoring by hand.
void IDVolumetricClouds::renderFroxels(LLRenderTarget* depth_src)
{
    if (!gIDCloudsFroxelProgram.isComplete() || !gIDCloudsFroxelAccumProgram.isComplete() || !ensureFroxels())
    {
        return;
    }

    static LLStaticHashedString s_slice("id_froxel_slice");
    static LLStaticHashedString s_far("id_froxel_far");
    static LLStaticHashedString s_grid("id_froxel_grid");
    static LLStaticHashedString s_eye_to_world("id_eye_to_world");
    static LLStaticHashedString s_light_steps("id_light_steps");
    static LLStaticHashedString s_detail_noise("id_detail_noise");
    static LLStaticHashedString s_fog_shadows("id_fog_shadows");
    static LLStaticHashedString s_shadow_range("id_shadow_range");
    static LLStaticHashedString s_clouds_active("id_clouds_active");

    LLGLDisable blend(GL_BLEND);
    gGL.flush();
    glBindFramebuffer(GL_FRAMEBUFFER, mFroxelFBO);
    glViewport(0, 0, FROXEL_W, FROXEL_H);

    {
        LLGLSLShader& shader = gIDCloudsFroxelProgram;
        gPipeline.bindDeferredShader(shader, nullptr, depth_src);
        setDensityInputs(shader);

        const glm::mat4 eye_to_world(glm::mat3(glm::inverse(get_current_modelview())));
        shader.uniformMatrix4fv(s_eye_to_world, 1, GL_FALSE, glm::value_ptr(eye_to_world));
        shader.uniform1i(s_light_steps, FROXEL_LIGHT_STEPS);
        shader.uniform1i(s_detail_noise, 1);
        shader.uniform1f(s_far, FROXEL_FAR_M);
        shader.uniform2f(s_grid, (F32)FROXEL_W, (F32)FROXEL_H);
        shader.uniform1i(s_fog_shadows, LLPipeline::RenderShadowDetail > 0 ? 1 : 0);
        shader.uniform1f(s_shadow_range, gPipeline.mSunClipPlanes.mV[VW]);
        // The light march already dims the sun by cloud; the cloud shadow map would dim it twice.
        shader.uniform1i(s_clouds_active, 0);

        LLEnvironment& environment = LLEnvironment::instance();
        shader.uniform1i(LLShaderMgr::SUN_UP_FACTOR, environment.getIsSunUp() ? 1 : 0);
        shader.uniform3fv(LLShaderMgr::LIGHTNORM, 1, environment.getClampedLightNorm().mV);

        gPipeline.mScreenTriangleVB->setBuffer();
        for (S32 k = 0; k < FROXEL_D; ++k)
        {
            glFramebufferTextureLayer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, mFroxelSegments, 0, k);
            shader.uniform1i(s_slice, k);
            gPipeline.mScreenTriangleVB->drawArrays(LLRender::TRIANGLES, 0, 3);
        }

        clearDensityInputs(shader);
        gPipeline.unbindDeferredShader(shader);
    }

    {
        LLGLSLShader& shader = gIDCloudsFroxelAccumProgram;
        gPipeline.bindDeferredShader(shader, nullptr, depth_src);
        const S32 channel = shader.enableTexture(LLShaderMgr::ID_CLOUD_FROXEL, LLTexUnit::TT_TEXTURE_3D);
        if (channel > -1)
        {
            gGL.getTexUnit(channel)->bindManual(LLTexUnit::TT_TEXTURE_3D, mFroxelSegments);
        }

        gPipeline.mScreenTriangleVB->setBuffer();
        for (S32 k = 0; k < FROXEL_D; ++k)
        {
            glFramebufferTextureLayer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, mFroxelVolume, 0, k);
            shader.uniform1i(s_slice, k);
            gPipeline.mScreenTriangleVB->drawArrays(LLRender::TRIANGLES, 0, 3);
        }

        if (channel > -1)
        {
            gGL.getTexUnit(channel)->unbind(LLTexUnit::TT_TEXTURE_3D);
            shader.disableTexture(LLShaderMgr::ID_CLOUD_FROXEL, LLTexUnit::TT_TEXTURE_3D);
        }
        gPipeline.unbindDeferredShader(shader);
    }

    gGL.flush();
    glBindFramebuffer(GL_FRAMEBUFFER, LLRenderTarget::sCurFBO);
    glViewport(0, 0, LLRenderTarget::sCurResX, LLRenderTarget::sCurResY);
    mFroxelFrame = LLFrameTimer::getFrameCount();
}

// The cloud scene belongs to this frame's main view; probes, impostors, previews and any other
// target of a different size must not read it.
bool IDVolumetricClouds::sceneMatchesTarget() const
{
    if (gCubeSnapshot || LLPipeline::sImpostorRender || mSceneFrame != LLFrameTimer::getFrameCount() || !mSceneRT.isComplete())
    {
        return false;
    }
    const LLRenderTarget* target = LLRenderTarget::getCurrentBoundTarget();
    return target && target->getWidth() == mSceneRT.getWidth() && target->getHeight() == mSceneRT.getHeight();
}

void IDVolumetricClouds::bindForSampling(LLGLSLShader& shader)
{
    static LLStaticHashedString s_active("id_clouds_active");
    static LLStaticHashedString s_matrix("id_cloud_shadow_matrix");
    static LLStaticHashedString s_layer_base("id_shadow_layer_base");
    static LLStaticHashedString s_layer_top("id_shadow_layer_top");
    static LLStaticHashedString s_scene_active("id_cloud_scene_active");
    static LLStaticHashedString s_haze_pending("id_haze_pending");
    static LLStaticHashedString s_froxel_active("id_cloud_froxel_active");
    static LLStaticHashedString s_froxel_far("id_froxel_far");

    if (LLPipeline::sRenderingHUDs || !isActive())
    {
        shader.uniform1i(s_active, 0);
        shader.uniform1i(s_scene_active, 0);
        return;
    }

    S32 channel = -1;
    if (mShadowReady)
    {
        channel = shader.enableTexture(LLShaderMgr::ID_CLOUD_SHADOW);
        if (channel > -1)
        {
            mShadowRT.bindTexture(0, channel, LLTexUnit::TFO_BILINEAR);
            gGL.getTexUnit(channel)->setTextureAddressMode(LLTexUnit::TAM_CLAMP);

            const glm::mat4 eye_to_uv = mShadowWorldToUV * glm::inverse(get_current_modelview());
            shader.uniformMatrix4fv(s_matrix, 1, GL_FALSE, glm::value_ptr(eye_to_uv));
            shader.uniform4fv(s_layer_base, 1, glm::value_ptr(mShadowLayerBase));
            shader.uniform4fv(s_layer_top, 1, glm::value_ptr(mShadowLayerTop));
        }
    }
    shader.uniform1i(s_active, channel > -1 ? 1 : 0);

    bool scene = false;
    if (shader.getTextureChannel(LLShaderMgr::ID_CLOUD_SCENE) > -1
        && shader.getTextureChannel(LLShaderMgr::ID_CLOUD_DEPTH) > -1
        && sceneMatchesTarget())
    {
        shader.bindTexture(LLShaderMgr::ID_CLOUD_SCENE, &mSceneRT, false, LLTexUnit::TFO_POINT, 0);
        shader.bindTexture(LLShaderMgr::ID_CLOUD_DEPTH, &mSceneRT, false, LLTexUnit::TFO_POINT, 1);
        // While the camera is under water the haze pass skips pixels below the water plane.
        const bool haze_pending = mHazePending && LLDrawPoolAlpha::sWaterPlane.mV[VW] > 0.f;
        shader.uniform1i(s_haze_pending, haze_pending ? 1 : 0);
        scene = true;
    }
    shader.uniform1i(s_scene_active, scene ? 1 : 0);

    bool froxel = false;
    if (scene && mFroxelFrame == LLFrameTimer::getFrameCount())
    {
        channel = shader.enableTexture(LLShaderMgr::ID_CLOUD_FROXEL, LLTexUnit::TT_TEXTURE_3D);
        if (channel > -1)
        {
            gGL.getTexUnit(channel)->bindManual(LLTexUnit::TT_TEXTURE_3D, mFroxelVolume);
            shader.uniform1f(s_froxel_far, FROXEL_FAR_M);
            froxel = true;
        }
    }
    shader.uniform1i(s_froxel_active, froxel ? 1 : 0);
}
