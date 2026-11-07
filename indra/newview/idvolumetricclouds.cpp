/**
 * @file idvolumetricclouds.cpp
 * @brief <ID> Volumetric cloud renderer: noise and weather textures, gating, per-frame update.
 *
 * Part of Five's custom Firestorm fork.
 */

#include "llviewerprecompiledheaders.h"

#include "idvolumetricclouds.h"

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

struct IDCloudWeatherLayer
{
    IDCloudType mType = IDCloudType::Cumulus;
    S32 mDirA = 1;
    S32 mDirB = 0;
    U32 mSeed = 0;
};

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
    constexpr S32 BASE_DIM    = 128;
    constexpr S32 DETAIL_DIM  = 32;
    constexpr S32 WEATHER_DIM = 512;

    constexpr S32 FULL_STEPS   = 96;
    constexpr S32 LIVE_STEPS   = 48;
    constexpr S32 FULL_SAMPLES = 4;
    constexpr F32 PLANET_RADIUS_M = 6360000.f;

    const char* CACHE_DIR   = "volumetric_clouds";
    const char* BASE_FILE   = "noise_base_v1_128.raw";
    const char* DETAIL_FILE = "noise_detail_v1_32.raw";

    inline U32 mix(U32 h)
    {
        h ^= h >> 16;
        h *= 0x7feb352du;
        h ^= h >> 15;
        h *= 0x846ca68bu;
        h ^= h >> 16;
        return h;
    }

    inline U32 hash3(S32 x, S32 y, S32 z, U32 seed)
    {
        U32 h = mix(seed ^ 0x9e3779b9u);
        h = mix(h ^ (U32)x);
        h = mix(h ^ (U32)y);
        return mix(h ^ (U32)z);
    }

    inline F32 unit(U32 h)
    {
        return (F32)(h >> 8) * (1.f / 16777216.f);
    }

    inline S32 wrapi(S32 i, S32 period)
    {
        const S32 r = i % period;
        return r < 0 ? r + period : r;
    }

    inline F32 fade(F32 t)
    {
        return t * t * t * (t * (t * 6.f - 15.f) + 10.f);
    }

    inline F32 lerpf(F32 a, F32 b, F32 t)
    {
        return a + (b - a) * t;
    }

    inline F32 clamp01(F32 v)
    {
        return v < 0.f ? 0.f : (v > 1.f ? 1.f : v);
    }

    inline U8 toByte(F32 v)
    {
        return (U8)(clamp01(v) * 255.f + 0.5f);
    }

    inline F32 grad3(U32 h, F32 x, F32 y, F32 z)
    {
        const U32 g = h & 15;
        const F32 u = g < 8 ? x : y;
        const F32 v = g < 4 ? y : ((g == 12 || g == 14) ? x : z);
        return ((g & 1) ? -u : u) + ((g & 2) ? -v : v);
    }

    F32 perlin3(F32 x, F32 y, F32 z, S32 period, U32 seed)
    {
        const S32 ix = (S32)std::floor(x);
        const S32 iy = (S32)std::floor(y);
        const S32 iz = (S32)std::floor(z);
        const F32 fx = x - (F32)ix;
        const F32 fy = y - (F32)iy;
        const F32 fz = z - (F32)iz;

        F32 c[8];
        for (S32 i = 0; i < 8; ++i)
        {
            const S32 dx = i & 1;
            const S32 dy = (i >> 1) & 1;
            const S32 dz = (i >> 2) & 1;
            const U32 h = hash3(wrapi(ix + dx, period), wrapi(iy + dy, period), wrapi(iz + dz, period), seed);
            c[i] = grad3(h, fx - (F32)dx, fy - (F32)dy, fz - (F32)dz);
        }

        const F32 u = fade(fx);
        const F32 v = fade(fy);
        const F32 w = fade(fz);
        const F32 x00 = lerpf(c[0], c[1], u);
        const F32 x10 = lerpf(c[2], c[3], u);
        const F32 x01 = lerpf(c[4], c[5], u);
        const F32 x11 = lerpf(c[6], c[7], u);
        return lerpf(lerpf(x00, x10, v), lerpf(x01, x11, v), w);
    }

    F32 perlinFbm3(F32 u, F32 v, F32 w, S32 period, S32 octaves, U32 seed)
    {
        F32 sum = 0.f;
        F32 norm = 0.f;
        F32 amp = 1.f;
        for (S32 o = 0; o < octaves; ++o)
        {
            const F32 p = (F32)period;
            sum += amp * perlin3(u * p, v * p, w * p, period, seed + (U32)o);
            norm += amp;
            amp *= 0.5f;
            period *= 2;
        }
        return clamp01(sum / norm * 0.5f + 0.5f);
    }

    F32 worley3(F32 x, F32 y, F32 z, S32 cells, U32 seed)
    {
        const S32 ix = (S32)std::floor(x);
        const S32 iy = (S32)std::floor(y);
        const S32 iz = (S32)std::floor(z);
        F32 best = 8.f;
        for (S32 dz = -1; dz <= 1; ++dz)
        {
            for (S32 dy = -1; dy <= 1; ++dy)
            {
                for (S32 dx = -1; dx <= 1; ++dx)
                {
                    const S32 cx = ix + dx;
                    const S32 cy = iy + dy;
                    const S32 cz = iz + dz;
                    const U32 h = hash3(wrapi(cx, cells), wrapi(cy, cells), wrapi(cz, cells), seed);
                    const F32 px = (F32)cx + unit(h) - x;
                    const F32 py = (F32)cy + unit(mix(h ^ 0x68e31da4u)) - y;
                    const F32 pz = (F32)cz + unit(mix(h ^ 0xb5297a4du)) - z;
                    best = llmin(best, px * px + py * py + pz * pz);
                }
            }
        }
        return 1.f - clamp01(std::sqrt(best));
    }

    F32 worleyFbm3(F32 u, F32 v, F32 w, S32 cells, U32 seed)
    {
        const F32 c0 = (F32)cells;
        const F32 c1 = c0 * 2.f;
        const F32 c2 = c0 * 4.f;
        return worley3(u * c0, v * c0, w * c0, cells, seed) * 0.625f
             + worley3(u * c1, v * c1, w * c1, cells * 2, seed + 1) * 0.25f
             + worley3(u * c2, v * c2, w * c2, cells * 4, seed + 2) * 0.125f;
    }

    F32 perlin2(F32 x, F32 y, S32 px, S32 py, U32 seed)
    {
        const S32 ix = (S32)std::floor(x);
        const S32 iy = (S32)std::floor(y);
        const F32 fx = x - (F32)ix;
        const F32 fy = y - (F32)iy;

        F32 c[4];
        for (S32 i = 0; i < 4; ++i)
        {
            const S32 dx = i & 1;
            const S32 dy = (i >> 1) & 1;
            const F32 a = unit(hash3(wrapi(ix + dx, px), wrapi(iy + dy, py), 0, seed)) * F_TWO_PI;
            c[i] = std::cos(a) * (fx - (F32)dx) + std::sin(a) * (fy - (F32)dy);
        }

        const F32 u = fade(fx);
        return lerpf(lerpf(c[0], c[1], u), lerpf(c[2], c[3], u), fade(fy));
    }

    F32 worley2(F32 x, F32 y, S32 cx_count, S32 cy_count, U32 seed)
    {
        const S32 ix = (S32)std::floor(x);
        const S32 iy = (S32)std::floor(y);
        F32 best = 8.f;
        for (S32 dy = -1; dy <= 1; ++dy)
        {
            for (S32 dx = -1; dx <= 1; ++dx)
            {
                const S32 cx = ix + dx;
                const S32 cy = iy + dy;
                const U32 h = hash3(wrapi(cx, cx_count), wrapi(cy, cy_count), 0, seed);
                const F32 px = (F32)cx + unit(h) - x;
                const F32 py = (F32)cy + unit(mix(h ^ 0x68e31da4u)) - y;
                best = llmin(best, px * px + py * py);
            }
        }
        return 1.f - clamp01(std::sqrt(best));
    }

    bool generateBase(std::vector<U8>& out, const std::atomic<bool>& cancel)
    {
        out.assign((size_t)BASE_DIM * BASE_DIM * BASE_DIM * 4, 0);
        const F32 inv = 1.f / (F32)BASE_DIM;
        size_t i = 0;
        for (S32 z = 0; z < BASE_DIM; ++z)
        {
            if (cancel.load(std::memory_order_relaxed))
            {
                return false;
            }
            const F32 w = ((F32)z + 0.5f) * inv;
            for (S32 y = 0; y < BASE_DIM; ++y)
            {
                const F32 v = ((F32)y + 0.5f) * inv;
                for (S32 x = 0; x < BASE_DIM; ++x)
                {
                    const F32 u = ((F32)x + 0.5f) * inv;
                    const F32 wg = worleyFbm3(u, v, w, 4, 101);
                    const F32 perlin = perlinFbm3(u, v, w, 8, 4, 401);
                    out[i++] = toByte(wg + perlin * (1.f - wg));
                    out[i++] = toByte(wg);
                    out[i++] = toByte(worleyFbm3(u, v, w, 8, 201));
                    out[i++] = toByte(worleyFbm3(u, v, w, 16, 301));
                }
            }
        }
        return true;
    }

    bool generateDetail(std::vector<U8>& out, const std::atomic<bool>& cancel)
    {
        out.assign((size_t)DETAIL_DIM * DETAIL_DIM * DETAIL_DIM * 3, 0);
        const F32 inv = 1.f / (F32)DETAIL_DIM;
        size_t i = 0;
        for (S32 z = 0; z < DETAIL_DIM; ++z)
        {
            if (cancel.load(std::memory_order_relaxed))
            {
                return false;
            }
            const F32 w = ((F32)z + 0.5f) * inv;
            for (S32 y = 0; y < DETAIL_DIM; ++y)
            {
                const F32 v = ((F32)y + 0.5f) * inv;
                for (S32 x = 0; x < DETAIL_DIM; ++x)
                {
                    const F32 u = ((F32)x + 0.5f) * inv;
                    out[i++] = toByte(worleyFbm3(u, v, w, 2, 501));
                    out[i++] = toByte(worleyFbm3(u, v, w, 4, 601));
                    out[i++] = toByte(worleyFbm3(u, v, w, 8, 701));
                }
            }
        }
        return true;
    }

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
        else if (generateBase(job.mBase, job.mCancel) && generateDetail(job.mDetail, job.mCancel))
        {
            LLFile::mkdir(job.mDir);
            writeCache(job.mBasePath, job.mBase);
            writeCache(job.mDetailPath, job.mDetail);
            job.mOk = true;
        }

        job.mSeconds = std::chrono::duration<F32>(std::chrono::steady_clock::now() - start).count();
        job.mDone.store(true, std::memory_order_release);
    }

    void weatherFrequencies(const IDCloudWeatherLayer& layer, S32& fu, S32& fv)
    {
        S32 f = 10;
        switch (layer.mType)
        {
            case IDCloudType::Cumulus:       f = 10; break;
            case IDCloudType::Cumulonimbus:  f = 3;  break;
            case IDCloudType::Stratus:       f = 2;  break;
            case IDCloudType::Stratocumulus: f = 16; break;
            case IDCloudType::Altocumulus:   f = 28; break;
            case IDCloudType::Cirrus:
            {
                const F32 len = std::sqrt((F32)(layer.mDirA * layer.mDirA + layer.mDirB * layer.mDirB));
                fu = llmax(1, ll_round(2.f / len));
                fv = llmax(1, ll_round(20.f / len));
                return;
            }
            default: break;
        }
        fu = f;
        fv = f;
    }

    void buildWeatherChannel(const IDCloudWeatherLayer& layer, std::vector<F32>& field)
    {
        S32 fu = 1;
        S32 fv = 1;
        weatherFrequencies(layer, fu, fv);
        const F32 a = (F32)layer.mDirA;
        const F32 b = (F32)layer.mDirB;
        const F32 inv = 1.f / (F32)WEATHER_DIM;

        size_t i = 0;
        for (S32 y = 0; y < WEATHER_DIM; ++y)
        {
            const F32 ty = ((F32)y + 0.5f) * inv;
            for (S32 x = 0; x < WEATHER_DIM; ++x)
            {
                const F32 tx = ((F32)x + 0.5f) * inv;
                const F32 su = a * tx + b * ty;
                const F32 sv = a * ty - b * tx;

                F32 perlin = 0.f;
                F32 worley = 0.f;
                F32 amp = 1.f;
                F32 norm = 0.f;
                for (S32 o = 0; o < 3; ++o)
                {
                    const S32 pu = fu << o;
                    const S32 pv = fv << o;
                    perlin += amp * perlin2(su * (F32)pu, sv * (F32)pv, pu, pv, layer.mSeed + (U32)o);
                    worley += amp * worley2(su * (F32)(pu * 2), sv * (F32)(pv * 2), pu * 2, pv * 2, layer.mSeed + 16 + (U32)o);
                    norm += amp;
                    amp *= 0.5f;
                }
                perlin = clamp01(perlin / norm + 0.5f);
                worley /= norm;
                field[i++] = worley + perlin * (1.f - worley);
            }
        }
    }

    void runWeatherJob(IDCloudWeatherJob& job)
    {
        const size_t texels = (size_t)WEATHER_DIM * WEATHER_DIM;
        job.mPixels.assign(texels * 4, 0);
        std::vector<F32> field(texels);
        std::vector<U32> order(texels);

        for (size_t c = 0; c < job.mLayers.size() && c < 4; ++c)
        {
            if (job.mCancel.load(std::memory_order_relaxed))
            {
                break;
            }
            buildWeatherChannel(job.mLayers[c], field);

            // Rank-equalise so coverage c lights up a fraction c of the sky.
            std::iota(order.begin(), order.end(), 0u);
            std::sort(order.begin(), order.end(), [&field](U32 l, U32 r) { return field[l] < field[r]; });
            for (size_t r = 0; r < texels; ++r)
            {
                job.mPixels[(size_t)order[r] * 4 + c] = (U8)((r * 255 + texels / 2) / (texels - 1));
            }
        }
        job.mDone.store(true, std::memory_order_release);
    }

    void cirrusLattice(F32 wind_dir_deg, S32& a_out, S32& b_out)
    {
        const F32 rad = wind_dir_deg * DEG_TO_RAD;
        const F32 dx = std::sin(rad);
        const F32 dy = std::cos(rad);
        F32 best = -2.f;
        a_out = 1;
        b_out = 0;
        for (S32 a = -4; a <= 4; ++a)
        {
            for (S32 b = -4; b <= 4; ++b)
            {
                if (std::gcd(std::abs(a), std::abs(b)) != 1)
                {
                    continue;
                }
                const F32 len = std::sqrt((F32)(a * a + b * b));
                const F32 d = std::fabs((F32)a * dx + (F32)b * dy) / len;
                if (d > best)
                {
                    best = d;
                    a_out = a;
                    b_out = b;
                }
            }
        }
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

bool IDVolumetricClouds::prerequisitesMet() const
{
    static LLCachedControl<bool> enabled(gSavedSettings, "IDVolumetricCloudsEnabled", false);
    return enabled
        && isSupported()
        && RlvActions::canChangeEnvironment()
        && !IDCloudState::instance().current().mLayers.empty();
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
    static LLCachedControl<bool> enabled(gSavedSettings, "IDVolumetricCloudsEnabled", false);
    if (!enabled || !isSupported())
    {
        return;
    }

    const U32 frame = LLFrameTimer::getFrameCount();
    if (frame == mLastFrame)
    {
        return;
    }
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
        IDCloudWeatherLayer l;
        l.mType = preset.mLayers[i].mType;
        l.mSeed = 0x5eed0000u + (U32)i * 7919u + (U32)l.mType;
        if (l.mType == IDCloudType::Cirrus)
        {
            cirrusLattice(preset.mLayers[i].mWindDirDeg, l.mDirA, l.mDirB);
        }
        key.push_back((U32)l.mType | ((U32)(l.mDirA + 8) << 8) | ((U32)(l.mDirB + 8) << 16));
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
    if (queue->post([job]() { runWeatherJob(*job); }))
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
    mWeatherKey.clear();
    mWeatherDirty = true;
}

void IDVolumetricClouds::renderShadowMap()
{
}

void IDVolumetricClouds::render(LLRenderTarget* screen, LLRenderTarget* depth_src)
{
    if (!screen || !depth_src || !isActive())
    {
        return;
    }
    llassert(LLRenderTarget::getCurrentBoundTarget() == screen);

    if (!ensureSceneTarget(screen->getWidth(), screen->getHeight()))
    {
        return;
    }

    LLGLDepthTest depth(GL_FALSE, GL_FALSE);
    LLGLDisable cull(GL_CULL_FACE);

    const bool full = fullQuality();
    march(mSceneRT, depth_src, full ? FULL_STEPS : LIVE_STEPS, full ? FULL_SAMPLES : 1);
    composite(depth_src);
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
    static LLStaticHashedString s_layer_wind_dir("id_layer_wind_dir");
    static LLStaticHashedString s_planet_radius("id_planet_radius");
    static LLStaticHashedString s_camera_global("id_camera_global");

    constexpr U32 MAX = IDCloudPreset::MAX_LAYERS;
    F32 base[MAX] = {};
    F32 top[MAX] = {};
    F32 coverage[MAX] = {};
    F32 density[MAX] = {};
    F32 detail[MAX] = {};
    GLint type[MAX] = {};
    F32 wind[MAX * 2] = {};
    F32 wind_dir[MAX * 2] = {};

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
        const F32 rad = l.mWindDirDeg * DEG_TO_RAD;
        wind_dir[i * 2] = std::sin(rad);
        wind_dir[i * 2 + 1] = std::cos(rad);
    }

    shader.uniform1i(s_layer_count, (GLint)count);
    shader.uniform1fv(s_layer_base, MAX, base);
    shader.uniform1fv(s_layer_top, MAX, top);
    shader.uniform1fv(s_layer_coverage, MAX, coverage);
    shader.uniform1fv(s_layer_density, MAX, density);
    shader.uniform1fv(s_layer_detail, MAX, detail);
    shader.uniform1iv(s_layer_type, MAX, type);
    shader.uniform2fv(s_layer_wind, MAX, wind);
    shader.uniform2fv(s_layer_wind_dir, MAX, wind_dir);
    shader.uniform1f(s_planet_radius, PLANET_RADIUS_M);

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

void IDVolumetricClouds::march(LLRenderTarget& target, LLRenderTarget* depth_src, S32 steps, S32 samples)
{
    static LLStaticHashedString s_eye_to_world("id_eye_to_world");
    static LLStaticHashedString s_pixel_angle("id_pixel_angle");
    static LLStaticHashedString s_steps("id_steps");
    static LLStaticHashedString s_samples("id_samples");

    LLGLSLShader& shader = gIDCloudsProgram;
    LLGLDisable blend(GL_BLEND);

    target.bindTarget();
    gPipeline.bindDeferredShader(shader, nullptr, depth_src);
    setDensityInputs(shader);

    const glm::mat4 eye_to_world(glm::mat3(glm::inverse(get_current_modelview())));
    shader.uniformMatrix4fv(s_eye_to_world, 1, GL_FALSE, glm::value_ptr(eye_to_world));
    const F32 fov = LLViewerCamera::getInstance()->getView();
    shader.uniform1f(s_pixel_angle, 2.f * std::tan(fov * 0.5f) / (F32)llmax(target.getHeight(), 1u));
    shader.uniform1i(s_steps, steps);
    shader.uniform1i(s_samples, samples);

    LLEnvironment& environment = LLEnvironment::instance();
    shader.uniform1i(LLShaderMgr::SUN_UP_FACTOR, environment.getIsSunUp() ? 1 : 0);
    shader.uniform3fv(LLShaderMgr::LIGHTNORM, 1, environment.getClampedLightNorm().mV);

    gPipeline.mScreenTriangleVB->setBuffer();
    gPipeline.mScreenTriangleVB->drawArrays(LLRender::TRIANGLES, 0, 3);

    clearDensityInputs(shader);
    gPipeline.unbindDeferredShader(shader);
    target.flush();
}

void IDVolumetricClouds::composite(LLRenderTarget* depth_src)
{
    static LLStaticHashedString s_haze_compensate("id_haze_compensate");

    LLGLSLShader& shader = gIDCloudsCompositeProgram;
    gPipeline.bindDeferredShader(shader, nullptr, depth_src);
    shader.bindTexture(LLShaderMgr::ID_CLOUD_SCENE, &mSceneRT, false, LLTexUnit::TFO_POINT, 0);

    LLEnvironment& environment = LLEnvironment::instance();
    shader.uniform1i(LLShaderMgr::SUN_UP_FACTOR, environment.getIsSunUp() ? 1 : 0);
    shader.uniform3fv(LLShaderMgr::LIGHTNORM, 1, environment.getClampedLightNorm().mV);
    shader.uniform4fv(LLShaderMgr::WATER_WATERPLANE, 1, LLDrawPoolAlpha::sWaterPlane.mV);
    shader.uniform1i(s_haze_compensate, LLPipeline::RenderDeferredAtmospheric ? 1 : 0);

    gGL.setColorMask(true, true);
    LLGLEnable blend(GL_BLEND);
    gGL.blendFunc(LLRender::BF_ONE, LLRender::BF_SOURCE_ALPHA, LLRender::BF_ZERO, LLRender::BF_SOURCE_ALPHA);

    gPipeline.mScreenTriangleVB->setBuffer();
    gPipeline.mScreenTriangleVB->drawArrays(LLRender::TRIANGLES, 0, 3);

    gGL.setSceneBlendType(LLRender::BT_ALPHA);
    shader.disableTexture(LLShaderMgr::ID_CLOUD_SCENE);
    gPipeline.unbindDeferredShader(shader);
}

void IDVolumetricClouds::bindForSampling(LLGLSLShader& shader)
{
}
