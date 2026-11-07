/**
 * @file idvolumetricclouds.h
 * @brief <ID> Volumetric cloud renderer: noise and weather textures, gating, per-frame update.
 *
 * Part of Five's custom Firestorm fork.
 */

#ifndef ID_VOLUMETRIC_CLOUDS_H
#define ID_VOLUMETRIC_CLOUDS_H

#include "llsingleton.h"

#include <boost/signals2.hpp>
#include <memory>
#include <vector>

class LLGLSLShader;
class LLRenderTarget;
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

    U32 mNoiseBase   = 0;
    U32 mNoiseDetail = 0;
    U32 mWeather     = 0;

    std::shared_ptr<IDCloudNoiseJob>   mNoiseJob;
    std::shared_ptr<IDCloudWeatherJob> mWeatherJob;
    bool mNoiseFailed = false;

    std::vector<U32> mWeatherKey;
    bool mWeatherDirty = true;

    U32 mLastFrame = U32_MAX;
    boost::signals2::scoped_connection mStateConnection;
};

#endif // ID_VOLUMETRIC_CLOUDS_H
