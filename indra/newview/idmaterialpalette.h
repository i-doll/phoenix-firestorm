/**
 * @file idmaterialpalette.h
 * @brief <ID:MaterialTheme> Material You colour schemes from a seed colour: a port of Google's
 * Material Color Utilities (HCT, tonal palettes, SchemeTonalSpot). Plain C++ with no viewer
 * dependencies.
 *
 * Part of Five's custom Firestorm fork.
 */

#ifndef ID_MATERIALPALETTE_H
#define ID_MATERIALPALETTE_H

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>

namespace idmaterial
{
    struct Rgb
    {
        uint8_t r, g, b;
    };

    enum class Role
    {
        Primary, OnPrimary, PrimaryContainer, OnPrimaryContainer,
        Secondary, OnSecondary, SecondaryContainer, OnSecondaryContainer,
        Tertiary, OnTertiary, TertiaryContainer, OnTertiaryContainer,
        Error, OnError, ErrorContainer, OnErrorContainer,
        Surface, OnSurface, SurfaceVariant, OnSurfaceVariant,
        SurfaceContainerLowest, SurfaceContainerLow, SurfaceContainer, SurfaceContainerHigh, SurfaceContainerHighest,
        InverseSurface, InverseOnSurface, InversePrimary,
        Outline, OutlineVariant, Shadow, Scrim,
        Count
    };

    namespace detail
    {
        constexpr double PI = 3.14159265358979323846;

        inline double linearized(uint8_t c)
        {
            double n = c / 255.0;
            return 100.0 * (n <= 0.040449936 ? n / 12.92 : std::pow((n + 0.055) / 1.055, 2.4));
        }

        inline uint8_t delinearized(double c)
        {
            double n = c / 100.0;
            double d = n <= 0.0031308 ? n * 12.92 : 1.055 * std::pow(n, 1.0 / 2.4) - 0.055;
            return (uint8_t)std::clamp((int)std::lround(d * 255.0), 0, 255);
        }

        inline double yFromLstar(double l)
        {
            double ft = (l + 16.0) / 116.0, ft3 = ft * ft * ft;
            return 100.0 * (ft3 > 216.0 / 24389.0 ? ft3 : (116.0 * ft - 16.0) / (24389.0 / 27.0));
        }

        inline double lstarFromY(double y)
        {
            double n = y / 100.0;
            double f = n > 216.0 / 24389.0 ? std::cbrt(n) : ((24389.0 / 27.0) * n + 16.0) / 116.0;
            return 116.0 * f - 16.0;
        }

        inline double sanitizeDegrees(double d)
        {
            d = std::fmod(d, 360.0);
            return d < 0.0 ? d + 360.0 : d;
        }

        struct Viewing
        {
            double n, aw, nbb, ncb, c, nc, fl, flRoot, z;
            double rgbD[3];
        };

        inline const Viewing& viewing()
        {
            static const Viewing v = [] {
                Viewing o{};
                const double white[3] = { 95.047, 100.0, 108.883 };
                double la = 200.0 / PI * yFromLstar(50.0) / 100.0;
                double rW = white[0] * 0.401288 + white[1] * 0.650173 + white[2] * -0.051461;
                double gW = white[0] * -0.250268 + white[1] * 1.204414 + white[2] * 0.045854;
                double bW = white[0] * -0.002079 + white[1] * 0.048952 + white[2] * 0.953127;
                double f = 1.0;
                o.c = 0.69;
                double d = std::clamp(f * (1.0 - (1.0 / 3.6) * std::exp((-la - 42.0) / 92.0)), 0.0, 1.0);
                o.nc = f;
                o.rgbD[0] = d * (100.0 / rW) + 1.0 - d;
                o.rgbD[1] = d * (100.0 / gW) + 1.0 - d;
                o.rgbD[2] = d * (100.0 / bW) + 1.0 - d;
                double k = 1.0 / (5.0 * la + 1.0), k4 = k * k * k * k, k4F = 1.0 - k4;
                o.fl = k4 * la + 0.1 * k4F * k4F * std::cbrt(5.0 * la);
                o.n = yFromLstar(50.0) / white[1];
                o.z = 1.48 + std::sqrt(o.n);
                o.nbb = o.ncb = 0.725 / std::pow(o.n, 0.2);
                double w[3] = { rW, gW, bW }, a[3];
                for (int i = 0; i < 3; ++i)
                {
                    double af = std::pow(o.fl * o.rgbD[i] * w[i] / 100.0, 0.42);
                    a[i] = 400.0 * af / (af + 27.13);
                }
                o.aw = (2.0 * a[0] + a[1] + 0.05 * a[2]) * o.nbb;
                o.flRoot = std::pow(o.fl, 0.25);
                return o;
            }();
            return v;
        }

        inline void hueChroma(Rgb rgb, double& hue, double& chroma)
        {
            const Viewing& v = viewing();
            double r = linearized(rgb.r), g = linearized(rgb.g), b = linearized(rgb.b);
            double x = 0.41233895 * r + 0.35762064 * g + 0.18051042 * b;
            double y = 0.2126 * r + 0.7152 * g + 0.0722 * b;
            double z = 0.01932141 * r + 0.11916382 * g + 0.95034478 * b;
            double c[3] = { 0.401288 * x + 0.650173 * y - 0.051461 * z,
                            -0.250268 * x + 1.204414 * y + 0.045854 * z,
                            -0.002079 * x + 0.048952 * y + 0.953127 * z };
            double A[3];
            for (int i = 0; i < 3; ++i)
            {
                double dd = v.rgbD[i] * c[i];
                double af = std::pow(v.fl * std::abs(dd) / 100.0, 0.42);
                A[i] = (dd < 0 ? -1.0 : 1.0) * 400.0 * af / (af + 27.13);
            }
            double a = (11.0 * A[0] - 12.0 * A[1] + A[2]) / 11.0;
            double bb = (A[0] + A[1] - 2.0 * A[2]) / 9.0;
            double u = (20.0 * A[0] + 20.0 * A[1] + 21.0 * A[2]) / 20.0;
            double p2 = (40.0 * A[0] + 20.0 * A[1] + A[2]) / 20.0;
            hue = sanitizeDegrees(std::atan2(bb, a) * 180.0 / PI);
            double j = 100.0 * std::pow(p2 * v.nbb / v.aw, v.c * v.z);
            double huePrime = hue < 20.14 ? hue + 360.0 : hue;
            double eHue = 0.25 * (std::cos(huePrime * PI / 180.0 + 2.0) + 3.8);
            double p1 = 50000.0 / 13.0 * eHue * v.nc * v.ncb;
            double t = p1 * std::hypot(a, bb) / (u + 0.305);
            double alpha = std::pow(1.64 - std::pow(0.29, v.n), 0.73) * std::pow(t, 0.9);
            chroma = alpha * std::sqrt(j / 100.0);
        }

        inline void camToLinear(double j, double chroma, double hue, double out[3])
        {
            const Viewing& v = viewing();
            double alpha = (chroma == 0.0 || j == 0.0) ? 0.0 : chroma / std::sqrt(j / 100.0);
            double t = std::pow(alpha / std::pow(1.64 - std::pow(0.29, v.n), 0.73), 1.0 / 0.9);
            double h = hue * PI / 180.0;
            double eHue = 0.25 * (std::cos(h + 2.0) + 3.8);
            double ac = v.aw * std::pow(j / 100.0, 1.0 / v.c / v.z);
            double p1 = eHue * (50000.0 / 13.0) * v.nc * v.ncb;
            double p2 = ac / v.nbb;
            double hs = std::sin(h), hc = std::cos(h);
            double gamma = 23.0 * (p2 + 0.305) * t / (23.0 * p1 + 11.0 * t * hc + 108.0 * t * hs);
            double a = gamma * hc, b = gamma * hs;
            double A[3] = { (460.0 * p2 + 451.0 * a + 288.0 * b) / 1403.0,
                            (460.0 * p2 - 891.0 * a - 261.0 * b) / 1403.0,
                            (460.0 * p2 - 220.0 * a - 6300.0 * b) / 1403.0 };
            double F[3];
            for (int i = 0; i < 3; ++i)
            {
                double base = std::max(0.0, 27.13 * std::abs(A[i]) / (400.0 - std::abs(A[i])));
                F[i] = (A[i] < 0 ? -1.0 : 1.0) * (100.0 / v.fl) * std::pow(base, 1.0 / 0.42) / v.rgbD[i];
            }
            double x = 1.86206786 * F[0] - 1.01125463 * F[1] + 0.14918677 * F[2];
            double y = 0.38752654 * F[0] + 0.62144744 * F[1] - 0.00897398 * F[2];
            double z = -0.01584150 * F[0] - 0.03412294 * F[1] + 1.04996444 * F[2];
            out[0] = 3.2413774792388685 * x - 1.5376652402851851 * y - 0.49885366846268053 * z;
            out[1] = -0.9691452513005321 * x + 1.8758853451067872 * y + 0.04156585616912061 * z;
            out[2] = 0.05562093689691305 * x - 0.20395524564742123 * y + 1.0571799111220335 * z;
        }

        inline bool solveAtChroma(double hue, double chroma, double y, double out[3])
        {
            double j = std::sqrt(y) * 11.0;
            for (int i = 0; i < 12; ++i)
            {
                camToLinear(j, chroma, hue, out);
                double fnj = 0.2126 * out[0] + 0.7152 * out[1] + 0.0722 * out[2];
                if (fnj <= 0.0)
                    return false;
                if (std::abs(fnj - y) < 0.0005)
                    break;
                j -= (fnj - y) * j / (2.0 * fnj);
            }
            for (int i = 0; i < 3; ++i)
                if (out[i] < -0.01 || out[i] > 100.01)
                    return false;
            return true;
        }

        inline Rgb fromHct(double hue, double chroma, double tone)
        {
            double y = yFromLstar(tone);
            if (chroma < 0.0001 || tone < 0.0001 || tone > 99.9999)
            {
                uint8_t g = delinearized(y);
                return { g, g, g };
            }
            hue = sanitizeDegrees(hue);
            double lin[3];
            if (!solveAtChroma(hue, chroma, y, lin))
            {
                double lo = 0.0, hi = chroma, best[3] = { y, y, y };
                for (int i = 0; i < 24; ++i)
                {
                    double mid = 0.5 * (lo + hi);
                    if (solveAtChroma(hue, mid, y, lin))
                    {
                        lo = mid;
                        std::copy(lin, lin + 3, best);
                    }
                    else
                        hi = mid;
                }
                std::copy(best, best + 3, lin);
            }
            return { delinearized(lin[0]), delinearized(lin[1]), delinearized(lin[2]) };
        }

        inline double luminance(Rgb c)
        {
            return 0.2126 * linearized(c.r) + 0.7152 * linearized(c.g) + 0.0722 * linearized(c.b);
        }
    }

    inline double contrast(Rgb a, Rgb b)
    {
        double ya = detail::luminance(a), yb = detail::luminance(b);
        return (std::max(ya, yb) + 5.0) / (std::min(ya, yb) + 5.0);
    }

    inline std::array<Rgb, (size_t)Role::Count> scheme(Rgb seed, bool dark)
    {
        double hue, chroma;
        detail::hueChroma(seed, hue, chroma);
        struct Palette
        {
            double h, c;
            Rgb operator()(double t) const { return detail::fromHct(h, c, t); }
        };
        const double tint = std::clamp((chroma - 4.0) / 8.0, 0.0, 1.0);
        const Palette p{ hue, tint * std::max(48.0, chroma) }, s{ hue, tint * 16.0 }, t{ detail::sanitizeDegrees(hue + 60.0), tint * 24.0 },
            n{ hue, tint * 6.0 }, nv{ hue, tint * 8.0 }, e{ 25.0, 84.0 };
        auto pick = [dark](double light, double darkTone) { return dark ? darkTone : light; };

        std::array<Rgb, (size_t)Role::Count> out{};
        auto set = [&out](Role r, Rgb c) { out[(size_t)r] = c; };
        auto accent = [&](const Palette& pal, Role base) {
            size_t i = (size_t)base;
            out[i] = pal(pick(40, 80));
            out[i + 1] = pal(pick(100, 20));
            out[i + 2] = pal(pick(90, 30));
            out[i + 3] = pal(pick(10, 90));
        };
        accent(p, Role::Primary);
        accent(s, Role::Secondary);
        accent(t, Role::Tertiary);
        accent(e, Role::Error);
        set(Role::Surface, n(pick(98, 6)));
        set(Role::OnSurface, n(pick(10, 90)));
        set(Role::SurfaceVariant, nv(pick(90, 30)));
        set(Role::OnSurfaceVariant, nv(pick(30, 80)));
        set(Role::SurfaceContainerLowest, n(pick(100, 4)));
        set(Role::SurfaceContainerLow, n(pick(96, 10)));
        set(Role::SurfaceContainer, n(pick(94, 12)));
        set(Role::SurfaceContainerHigh, n(pick(92, 17)));
        set(Role::SurfaceContainerHighest, n(pick(90, 22)));
        set(Role::InverseSurface, n(pick(20, 90)));
        set(Role::InverseOnSurface, n(pick(95, 20)));
        set(Role::InversePrimary, p(pick(80, 40)));
        set(Role::Outline, nv(pick(50, 60)));
        set(Role::OutlineVariant, nv(pick(80, 30)));
        set(Role::Shadow, n(0));
        set(Role::Scrim, n(0));
        return out;
    }
}

#endif
