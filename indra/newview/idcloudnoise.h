/**
 * @file idcloudnoise.h
 * @brief <ID> Volumetric clouds: noise volumes and the weather (coverage) map. Plain C++ with no
 * viewer dependencies, so tools outside the viewer can build the exact same textures.
 *
 * Part of Five's custom Firestorm fork.
 */

#ifndef ID_CLOUDNOISE_H
#define ID_CLOUDNOISE_H

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <numeric>
#include <vector>

namespace idcloudnoise
{
    constexpr int32_t BASE_DIM    = 128;
    constexpr int32_t DETAIL_DIM  = 32;
    constexpr int32_t WEATHER_DIM = 512;
    constexpr int32_t MAX_LAYERS  = 4;

    constexpr float TWO_PI  = 6.28318530718f;
    constexpr float DEG_RAD = 0.0174532925199f;

    // Same order as IDCloudType.
    enum Type : int32_t
    {
        Cumulus,
        Cumulonimbus,
        Stratus,
        Stratocumulus,
        Altocumulus,
        Cirrus
    };

    struct WeatherLayer
    {
        int32_t type = Cumulus;
        int32_t dirA = 1;
        int32_t dirB = 0;
        uint32_t seed = 0;
    };

    inline int32_t roundi(float v)
    {
        return (int32_t)std::floor(v + 0.5f);
    }

    inline uint32_t mix(uint32_t h)
    {
        h ^= h >> 16;
        h *= 0x7feb352du;
        h ^= h >> 15;
        h *= 0x846ca68bu;
        h ^= h >> 16;
        return h;
    }

    inline uint32_t hash3(int32_t x, int32_t y, int32_t z, uint32_t seed)
    {
        uint32_t h = mix(seed ^ 0x9e3779b9u);
        h = mix(h ^ (uint32_t)x);
        h = mix(h ^ (uint32_t)y);
        return mix(h ^ (uint32_t)z);
    }

    inline float unit(uint32_t h)
    {
        return (float)(h >> 8) * (1.f / 16777216.f);
    }

    inline int32_t wrapi(int32_t i, int32_t period)
    {
        const int32_t r = i % period;
        return r < 0 ? r + period : r;
    }

    inline float fade(float t)
    {
        return t * t * t * (t * (t * 6.f - 15.f) + 10.f);
    }

    inline float lerpf(float a, float b, float t)
    {
        return a + (b - a) * t;
    }

    inline float clamp01(float v)
    {
        return v < 0.f ? 0.f : (v > 1.f ? 1.f : v);
    }

    inline uint8_t toByte(float v)
    {
        return (uint8_t)(clamp01(v) * 255.f + 0.5f);
    }

    inline float grad3(uint32_t h, float x, float y, float z)
    {
        const uint32_t g = h & 15;
        const float u = g < 8 ? x : y;
        const float v = g < 4 ? y : ((g == 12 || g == 14) ? x : z);
        return ((g & 1) ? -u : u) + ((g & 2) ? -v : v);
    }

    inline float perlin3(float x, float y, float z, int32_t period, uint32_t seed)
    {
        const int32_t ix = (int32_t)std::floor(x);
        const int32_t iy = (int32_t)std::floor(y);
        const int32_t iz = (int32_t)std::floor(z);
        const float fx = x - (float)ix;
        const float fy = y - (float)iy;
        const float fz = z - (float)iz;

        float c[8];
        for (int32_t i = 0; i < 8; ++i)
        {
            const int32_t dx = i & 1;
            const int32_t dy = (i >> 1) & 1;
            const int32_t dz = (i >> 2) & 1;
            const uint32_t h = hash3(wrapi(ix + dx, period), wrapi(iy + dy, period), wrapi(iz + dz, period), seed);
            c[i] = grad3(h, fx - (float)dx, fy - (float)dy, fz - (float)dz);
        }

        const float u = fade(fx);
        const float v = fade(fy);
        const float w = fade(fz);
        const float x00 = lerpf(c[0], c[1], u);
        const float x10 = lerpf(c[2], c[3], u);
        const float x01 = lerpf(c[4], c[5], u);
        const float x11 = lerpf(c[6], c[7], u);
        return lerpf(lerpf(x00, x10, v), lerpf(x01, x11, v), w);
    }

    inline float perlinFbm3(float u, float v, float w, int32_t period, int32_t octaves, uint32_t seed)
    {
        float sum = 0.f;
        float norm = 0.f;
        float amp = 1.f;
        for (int32_t o = 0; o < octaves; ++o)
        {
            const float p = (float)period;
            sum += amp * perlin3(u * p, v * p, w * p, period, seed + (uint32_t)o);
            norm += amp;
            amp *= 0.5f;
            period *= 2;
        }
        return clamp01(sum / norm * 0.5f + 0.5f);
    }

    inline float worley3(float x, float y, float z, int32_t cells, uint32_t seed)
    {
        const int32_t ix = (int32_t)std::floor(x);
        const int32_t iy = (int32_t)std::floor(y);
        const int32_t iz = (int32_t)std::floor(z);
        float best = 8.f;
        for (int32_t dz = -1; dz <= 1; ++dz)
        {
            for (int32_t dy = -1; dy <= 1; ++dy)
            {
                for (int32_t dx = -1; dx <= 1; ++dx)
                {
                    const int32_t cx = ix + dx;
                    const int32_t cy = iy + dy;
                    const int32_t cz = iz + dz;
                    const uint32_t h = hash3(wrapi(cx, cells), wrapi(cy, cells), wrapi(cz, cells), seed);
                    const float px = (float)cx + unit(h) - x;
                    const float py = (float)cy + unit(mix(h ^ 0x68e31da4u)) - y;
                    const float pz = (float)cz + unit(mix(h ^ 0xb5297a4du)) - z;
                    best = std::min(best, px * px + py * py + pz * pz);
                }
            }
        }
        return 1.f - clamp01(std::sqrt(best));
    }

    inline float worleyFbm3(float u, float v, float w, int32_t cells, uint32_t seed)
    {
        const float c0 = (float)cells;
        const float c1 = c0 * 2.f;
        const float c2 = c0 * 4.f;
        return worley3(u * c0, v * c0, w * c0, cells, seed) * 0.625f
             + worley3(u * c1, v * c1, w * c1, cells * 2, seed + 1) * 0.25f
             + worley3(u * c2, v * c2, w * c2, cells * 4, seed + 2) * 0.125f;
    }

    inline float perlin2(float x, float y, int32_t px, int32_t py, uint32_t seed)
    {
        const int32_t ix = (int32_t)std::floor(x);
        const int32_t iy = (int32_t)std::floor(y);
        const float fx = x - (float)ix;
        const float fy = y - (float)iy;

        float c[4];
        for (int32_t i = 0; i < 4; ++i)
        {
            const int32_t dx = i & 1;
            const int32_t dy = (i >> 1) & 1;
            const float a = unit(hash3(wrapi(ix + dx, px), wrapi(iy + dy, py), 0, seed)) * TWO_PI;
            c[i] = std::cos(a) * (fx - (float)dx) + std::sin(a) * (fy - (float)dy);
        }

        const float u = fade(fx);
        return lerpf(lerpf(c[0], c[1], u), lerpf(c[2], c[3], u), fade(fy));
    }

    inline float worley2(float x, float y, int32_t cx_count, int32_t cy_count, uint32_t seed)
    {
        const int32_t ix = (int32_t)std::floor(x);
        const int32_t iy = (int32_t)std::floor(y);
        float best = 8.f;
        for (int32_t dy = -1; dy <= 1; ++dy)
        {
            for (int32_t dx = -1; dx <= 1; ++dx)
            {
                const int32_t cx = ix + dx;
                const int32_t cy = iy + dy;
                const uint32_t h = hash3(wrapi(cx, cx_count), wrapi(cy, cy_count), 0, seed);
                const float px = (float)cx + unit(h) - x;
                const float py = (float)cy + unit(mix(h ^ 0x68e31da4u)) - y;
                best = std::min(best, px * px + py * py);
            }
        }
        return 1.f - clamp01(std::sqrt(best));
    }

    inline bool generateBase(std::vector<uint8_t>& out, const std::atomic<bool>& cancel)
    {
        out.assign((size_t)BASE_DIM * BASE_DIM * BASE_DIM * 4, 0);
        const float inv = 1.f / (float)BASE_DIM;
        size_t i = 0;
        for (int32_t z = 0; z < BASE_DIM; ++z)
        {
            if (cancel.load(std::memory_order_relaxed))
            {
                return false;
            }
            const float w = ((float)z + 0.5f) * inv;
            for (int32_t y = 0; y < BASE_DIM; ++y)
            {
                const float v = ((float)y + 0.5f) * inv;
                for (int32_t x = 0; x < BASE_DIM; ++x)
                {
                    const float u = ((float)x + 0.5f) * inv;
                    const float wg = worleyFbm3(u, v, w, 4, 101);
                    const float perlin = perlinFbm3(u, v, w, 8, 4, 401);
                    out[i++] = toByte(wg + perlin * (1.f - wg));
                    out[i++] = toByte(wg);
                    out[i++] = toByte(worleyFbm3(u, v, w, 8, 201));
                    out[i++] = toByte(worleyFbm3(u, v, w, 16, 301));
                }
            }
        }
        return true;
    }

    inline bool generateDetail(std::vector<uint8_t>& out, const std::atomic<bool>& cancel)
    {
        out.assign((size_t)DETAIL_DIM * DETAIL_DIM * DETAIL_DIM * 3, 0);
        const float inv = 1.f / (float)DETAIL_DIM;
        size_t i = 0;
        for (int32_t z = 0; z < DETAIL_DIM; ++z)
        {
            if (cancel.load(std::memory_order_relaxed))
            {
                return false;
            }
            const float w = ((float)z + 0.5f) * inv;
            for (int32_t y = 0; y < DETAIL_DIM; ++y)
            {
                const float v = ((float)y + 0.5f) * inv;
                for (int32_t x = 0; x < DETAIL_DIM; ++x)
                {
                    const float u = ((float)x + 0.5f) * inv;
                    out[i++] = toByte(worleyFbm3(u, v, w, 2, 501));
                    out[i++] = toByte(worleyFbm3(u, v, w, 4, 601));
                    out[i++] = toByte(worleyFbm3(u, v, w, 8, 701));
                }
            }
        }
        return true;
    }

    // f: lattice frequency across the tile; cells: how much the pattern is separate round cells
    // rather than connected blobs; warp: how far the pattern is bent, as a fraction of the tile.
    struct WeatherShape
    {
        int32_t fu = 10;
        int32_t fv = 10;
        float cells = 0.5f;
        float warp = 0.01f;
        int32_t rows = 0;
        int32_t octaves = 3;
        float patches = 0.f;
        bool smooth = false;
    };

    inline WeatherShape weatherShape(const WeatherLayer& layer)
    {
        WeatherShape w;
        int32_t f = 10;
        switch (layer.type)
        {
            case Cumulus:       f = 10; w.cells = 0.85f; w.warp = 0.01f;  w.patches = 0.5f; break;
            case Cumulonimbus:  f = 1;  w.cells = 0.85f; w.warp = 0.01f;  w.octaves = 1; break;
            case Stratus:
            {
                const float len = std::sqrt((float)(layer.dirA * layer.dirA + layer.dirB * layer.dirB));
                w.fu = std::max(1, roundi(2.f / len));
                w.fv = std::max(1, roundi(6.f / len));
                w.cells = 0.f;
                w.warp = 0.02f;
                w.smooth = true;
                return w;
            }
            case Stratocumulus: f = 24; w.cells = 0.6f;  w.warp = 0.008f; w.patches = 0.35f; break;
            case Altocumulus:
            {
                // Cells set in rows across the wind.
                const float len = std::sqrt((float)(layer.dirA * layer.dirA + layer.dirB * layer.dirB));
                w.fu = std::max(1, roundi(28.f / len));
                w.fv = std::max(1, roundi(28.f / len));
                w.cells = 0.9f;
                w.warp = 0.005f;
                w.rows = std::max(1, roundi(40.f / len));
                w.patches = 0.6f;
                return w;
            }
            case Cirrus:
            {
                const float len = std::sqrt((float)(layer.dirA * layer.dirA + layer.dirB * layer.dirB));
                w.fu = std::max(1, roundi(3.f / len));
                w.fv = std::max(1, roundi(8.f / len));
                w.cells = 0.f;
                w.warp = 0.04f;
                w.smooth = true;
                return w;
            }
            default: break;
        }
        w.fu = f;
        w.fv = f;
        return w;
    }

    inline void buildWeatherChannel(const WeatherLayer& layer, std::vector<float>& field)
    {
        const WeatherShape shape = weatherShape(layer);
        const int32_t fu = shape.fu;
        const int32_t fv = shape.fv;
        const float a = (float)layer.dirA;
        const float b = (float)layer.dirB;
        const float inv = 1.f / (float)WEATHER_DIM;

        size_t i = 0;
        for (int32_t y = 0; y < WEATHER_DIM; ++y)
        {
            const float ty0 = ((float)y + 0.5f) * inv;
            for (int32_t x = 0; x < WEATHER_DIM; ++x)
            {
                const float tx0 = ((float)x + 0.5f) * inv;
                const float tx = tx0 + shape.warp * (perlin2(tx0 * 4.f, ty0 * 4.f, 4, 4, layer.seed + 32)
                                                 + 0.5f * perlin2(tx0 * 8.f, ty0 * 8.f, 8, 8, layer.seed + 33));
                const float ty = ty0 + shape.warp * (perlin2(tx0 * 4.f, ty0 * 4.f, 4, 4, layer.seed + 34)
                                                 + 0.5f * perlin2(tx0 * 8.f, ty0 * 8.f, 8, 8, layer.seed + 35));
                const float su = a * tx + b * ty;
                const float sv = a * ty - b * tx;

                float perlin = 0.f;
                float worley = 0.f;
                float amp = 1.f;
                float norm = 0.f;
                for (int32_t o = 0; o < shape.octaves; ++o)
                {
                    const int32_t pu = fu << o;
                    const int32_t pv = fv << o;
                    perlin += amp * perlin2(su * (float)pu, sv * (float)pv, pu, pv, layer.seed + (uint32_t)o);
                    worley += amp * worley2(su * (float)(pu * 2), sv * (float)(pv * 2), pu * 2, pv * 2, layer.seed + 16 + (uint32_t)o);
                    norm += amp;
                    amp *= 0.5f;
                }
                perlin = clamp01(perlin / norm + 0.5f);
                worley /= norm;
                // Smooth types skip the cellular noise, whose round cells would cut streaks into ovals.
                float v = shape.smooth ? perlin : lerpf(worley + perlin * (1.f - worley), worley, shape.cells);
                if (shape.patches > 0.f)
                {
                    // Group the cells into banks with clear sky between them.
                    const float patch = 0.5f + perlin2(tx0 * 3.f, ty0 * 3.f, 3, 3, layer.seed + 40)
                                      + 0.5f * perlin2(tx0 * 6.f, ty0 * 6.f, 6, 6, layer.seed + 41);
                    v *= (1.f - shape.patches) + shape.patches * clamp01(patch);
                }
                if (shape.rows > 0)
                {
                    const float band = 0.5f + 0.5f * std::sin(su * (float)shape.rows * TWO_PI + 6.f * perlin);
                    v *= 0.8f + 0.2f * band;
                }
                field[i++] = v;
            }
        }
    }

    inline void cirrusLattice(float wind_dir_deg, int32_t& a_out, int32_t& b_out)
    {
        const float rad = wind_dir_deg * DEG_RAD;
        const float dx = std::sin(rad);
        const float dy = std::cos(rad);
        float best = -2.f;
        a_out = 1;
        b_out = 0;
        for (int32_t a = -2; a <= 2; ++a)
        {
            for (int32_t b = -2; b <= 2; ++b)
            {
                if (std::gcd(std::abs(a), std::abs(b)) != 1)
                {
                    continue;
                }
                const float len = std::sqrt((float)(a * a + b * b));
                const float d = ((float)a * dx + (float)b * dy) / len;
                if (d > best)
                {
                    best = d;
                    a_out = a;
                    b_out = b;
                }
            }
        }
    }

    inline bool usesStreakLattice(int32_t type)
    {
        return type == Cirrus || type == Stratus || type == Altocumulus;
    }

    inline WeatherLayer weatherLayer(size_t index, int32_t type, float wind_dir_deg)
    {
        WeatherLayer l;
        l.type = type;
        l.seed = 0x5eed0000u + (uint32_t)index * 7919u + (uint32_t)type;
        if (usesStreakLattice(type))
        {
            cirrusLattice(wind_dir_deg, l.dirA, l.dirB);
        }
        return l;
    }

    // RGBA8, one channel per layer, WEATHER_DIM squared. Returns false if cancelled.
    inline bool generateWeather(const std::vector<WeatherLayer>& layers, std::vector<uint8_t>& pixels, const std::atomic<bool>& cancel)
    {
        const size_t texels = (size_t)WEATHER_DIM * WEATHER_DIM;
        pixels.assign(texels * 4, 0);
        std::vector<float> field(texels);
        std::vector<uint32_t> order(texels);

        for (size_t c = 0; c < layers.size() && c < (size_t)MAX_LAYERS; ++c)
        {
            if (cancel.load(std::memory_order_relaxed))
            {
                return false;
            }
            buildWeatherChannel(layers[c], field);

            // Rank-equalise so coverage c lights up a fraction c of the sky.
            std::iota(order.begin(), order.end(), 0u);
            std::sort(order.begin(), order.end(), [&field](uint32_t l, uint32_t r) { return field[l] < field[r]; });
            for (size_t r = 0; r < texels; ++r)
            {
                pixels[(size_t)order[r] * 4 + c] = (uint8_t)((r * 255 + texels / 2) / (texels - 1));
            }
        }
        return true;
    }
}

#endif
