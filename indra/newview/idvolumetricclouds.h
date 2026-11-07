/**
 * @file idvolumetricclouds.h
 * @brief <ID> Volumetric cloud renderer: noise and weather textures, gating, per-frame update.
 *
 * Part of Five's custom Firestorm fork.
 */

#ifndef ID_VOLUMETRIC_CLOUDS_H
#define ID_VOLUMETRIC_CLOUDS_H

#include "llrendertarget.h"
#include "llsingleton.h"
#include "v3dmath.h"

#include "glm/vec2.hpp"

#include <boost/signals2.hpp>
#include <memory>
#include <vector>

class LLGLSLShader;
struct IDCloudNoiseJob;
struct IDCloudWeatherJob;

class IDVolumetricClouds : public LLSingleton<IDVolumetricClouds>
{
    LLSINGLETON(IDVolumetricClouds);

public:
    static bool isSupported();
    static bool canEdit();
    bool isActive() const;
    bool hasFogLayer() const;
    void update();
    bool fullQuality() const;
    void release();

    void renderShadowMap();
    void render(LLRenderTarget* screen, LLRenderTarget* depth_src);
    void bindForSampling(LLGLSLShader& shader);
    void onHazeApplied();

private:
    void cleanupSingleton() override;
    bool prerequisitesMet() const;
    void updateNoise();
    void updateWeather();

    bool ensureSceneTarget(U32 width, U32 height);
    void setDensityInputs(LLGLSLShader& shader);
    void clearDensityInputs(LLGLSLShader& shader);
    void march(LLRenderTarget& target, LLRenderTarget* depth_src, S32 steps, S32 samples, S32 light_steps, bool detail,
               const glm::vec2& grid, const glm::ivec2& block, const glm::ivec2& offset);
    bool renderLive(LLRenderTarget* depth_src);
    bool ensureLiveTargets(U32 width, U32 height);
    bool historyUsable(const LLVector3d& camera) const;
    void reproject(LLRenderTarget& target, LLRenderTarget& history, LLRenderTarget* depth_src,
                   const glm::mat4& to_last_eye, const glm::vec2& grid, const glm::ivec2& offset);
    void upsample(LLRenderTarget& source, LLRenderTarget& dest, LLRenderTarget* depth_src, const glm::vec2& grid);
    void composite(LLRenderTarget& scene, LLRenderTarget* depth_src, bool haze_compensate);
    void renderProbe(LLRenderTarget* screen, LLRenderTarget* depth_src);
    bool ensureProbeTargets(U32 slot, U32 width, U32 height);
    bool sceneMatchesTarget() const;

    U32 mNoiseBase   = 0;
    U32 mNoiseDetail = 0;
    U32 mWeather     = 0;

    // Attachment 0: RGBA16F fogged in-scatter + transmittance. Attachment 1: R32F mean cloud distance.
    LLRenderTarget mSceneRT;
    U32 mSceneFrame = U32_MAX;
    // Composited this frame and the deferred haze pass, which fogs it again, has not run yet.
    bool mHazePending = false;

    // Live mode: one pixel per 4x2 block of the half-resolution grid is marched each frame into
    // mMarchRT, the rest is reprojected from the previous half-resolution result. Both history
    // targets have the march's two attachments, with the scene distance kept in the second's g.
    // The grid is half the screen size, so with odd sizes its texel centres sit at full-res pixel edges.
    LLRenderTarget mMarchRT;
    LLRenderTarget mHistoryRT[2];
    U32 mHistoryIndex = 0;
    bool mHistoryValid = false;
    U32 mHistoryFrame = U32_MAX;
    U32 mTemporalIndex = 0;
    LLVector3d mHistoryCamera;
    LLVector3d mHistoryOrigin;

    // Reflection probe and hero probe faces, one slot each since both can update in one frame.
    // The march runs at a quarter of the face size and is upsampled to the face size.
    LLRenderTarget mProbeMarchRT[2];
    LLRenderTarget mProbeSceneRT[2];
    bool mWasActive = false;

    // R16F sunlight transmittance through all layers, top-down around the camera.
    LLRenderTarget mShadowRT;
    glm::mat4 mShadowWorldToUV{ 1.f };
    bool mShadowReady = false;
    U32 mShadowFrame = U32_MAX;

    std::shared_ptr<IDCloudNoiseJob>   mNoiseJob;
    std::shared_ptr<IDCloudWeatherJob> mWeatherJob;
    bool mNoiseFailed = false;

    std::vector<U32> mWeatherKey;
    bool mWeatherDirty = true;

    U32 mLastFrame = U32_MAX;
    boost::signals2::scoped_connection mStateConnection;
};

#endif // ID_VOLUMETRIC_CLOUDS_H
