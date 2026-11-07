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
    bool isActive() const;
    void update();
    bool fullQuality() const;
    void release();

    void renderShadowMap();
    void render(LLRenderTarget* screen, LLRenderTarget* depth_src);
    void bindForSampling(LLGLSLShader& shader);

private:
    void cleanupSingleton() override;
    bool prerequisitesMet() const;
    void updateNoise();
    void updateWeather();

    bool ensureSceneTarget(U32 width, U32 height);
    void setDensityInputs(LLGLSLShader& shader);
    void clearDensityInputs(LLGLSLShader& shader);
    void march(LLRenderTarget& target, LLRenderTarget* depth_src, S32 steps, S32 samples);
    void composite(LLRenderTarget* depth_src);

    U32 mNoiseBase   = 0;
    U32 mNoiseDetail = 0;
    U32 mWeather     = 0;

    // Attachment 0: RGBA16F fogged in-scatter + transmittance. Attachment 1: R32F mean cloud distance.
    LLRenderTarget mSceneRT;

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
