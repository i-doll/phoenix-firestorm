/**
 * @file postDeferredTonemap.glsl
 *
 * $LicenseInfo:firstyear=2024&license=viewerlgpl$
 * Second Life Viewer Source Code
 * Copyright (C) 2024, Linden Research, Inc.
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
 * Linden Research, Inc., 945 Battery Street, San Francisco, CA  94111  USA
 * $/LicenseInfo$
 */

/*[EXTRA_CODE_HERE]*/

uniform sampler2D exposureMap;
uniform vec2 screen_res;
in vec2 vary_fragcoord;

//===============================================================
// tone mapping taken from Khronos sample implementation
//===============================================================

// sRGB => XYZ => D65_2_D60 => AP1 => RRT_SAT
const mat3 ACESInputMat = mat3
(
    0.59719, 0.07600, 0.02840,
    0.35458, 0.90834, 0.13383,
    0.04823, 0.01566, 0.83777
);


// ODT_SAT => XYZ => D60_2_D65 => sRGB
const mat3 ACESOutputMat = mat3
(
    1.60475, -0.10208, -0.00327,
    -0.53108,  1.10813, -0.07276,
    -0.07367, -0.00605,  1.07602
);

// ACES tone map (faster approximation)
// see: https://knarkowicz.wordpress.com/2016/01/06/aces-filmic-tone-mapping-curve/
vec3 toneMapACES_Narkowicz(vec3 color)
{
    const float A = 2.51;
    const float B = 0.03;
    const float C = 2.43;
    const float D = 0.59;
    const float E = 0.14;
    return clamp((color * (A * color + B)) / (color * (C * color + D) + E), 0.0, 1.0);
}


// ACES filmic tone map approximation
// see https://github.com/TheRealMJP/BakingLab/blob/master/BakingLab/ACES.hlsl
vec3 RRTAndODTFit(vec3 color)
{
    vec3 a = color * (color + 0.0245786) - 0.000090537;
    vec3 b = color * (0.983729 * color + 0.4329510) + 0.238081;
    return a / b;
}


// tone mapping
vec3 toneMapACES_Hill(vec3 color)
{
    color = ACESInputMat * color;

    // Apply RRT and ODT
    color = RRTAndODTFit(color);

    color = ACESOutputMat * color;

    // Clamp to [0, 1]
    color = clamp(color, 0.0, 1.0);

    return color;
}

// Khronos Neutral tonemapping
// https://github.com/KhronosGroup/ToneMapping/tree/main
// Input color is non-negative and resides in the Linear Rec. 709 color space.
// Output color is also Linear Rec. 709, but in the [0, 1] range.
vec3 PBRNeutralToneMapping( vec3 color )
{
  const float startCompression = 0.8 - 0.04;
  const float desaturation = 0.15;

  float x = min(color.r, min(color.g, color.b));
  float offset = x < 0.08 ? x - 6.25 * x * x : 0.04;
  color -= offset;

  float peak = max(color.r, max(color.g, color.b));
  if (peak < startCompression) return color;

  const float d = 1. - startCompression;
  float newPeak = 1. - d * d / (peak + d - startCompression);
  color *= newPeak / peak;

  float g = 1. - 1. / (desaturation * (peak - newPeak) + 1.);
  return mix(color, newPeak * vec3(1, 1, 1), g);
}

// Extended Reinhard with a finite white point.
vec3 toneMapReinhard(vec3 color)
{
    const float white = 4.0;
    vec3 numerator = color * (1.0 + (color / (white * white)));
    return clamp(numerator / (1.0 + color), 0.0, 1.0);
}


// Uncharted 2 filmic curve.
// see http://filmicworlds.com/blog/filmic-tonemapping-operators/
vec3 uncharted2Partial(vec3 x)
{
    const float A = 0.15;
    const float B = 0.50;
    const float C = 0.10;
    const float D = 0.20;
    const float E = 0.02;
    const float F = 0.30;
    return ((x * (A * x + C * B) + D * E) / (x * (A * x + B) + D * F)) - E / F;
}

vec3 toneMapUncharted2(vec3 color)
{
    const float W = 11.2;
    const float exposure_bias = 2.0;

    // Not const: GLSL forbids user-defined function calls in const initializers.
    vec3 white_scale = vec3(1.0) / uncharted2Partial(vec3(W));

    return clamp(uncharted2Partial(color * exposure_bias) * white_scale, 0.0, 1.0);
}


// AgX, minimal implementation.
// see https://iolite-engine.com/blog_posts/minimal_agx_implementation
// Matrices below are already in GLSL column-major constructor order.
const mat3 AgXInsetMat = mat3
(
    0.842479062253094, 0.0423282422610123, 0.0423756549057051,
    0.0784335999999992, 0.878468636469772, 0.0784336,
    0.0792237451477643, 0.0791661274605434, 0.879142973793104
);

const mat3 AgXOutsetMat = mat3
(
    1.19687900512017, -0.0528968517574562, -0.0529716355144438,
    -0.0980208811401368, 1.15190312990417, -0.0980434501171241,
    -0.0990297440797205, -0.0989611768448433, 1.15107367264116
);

vec3 agxContrastApprox(vec3 x)
{
    vec3 x2 = x * x;
    vec3 x4 = x2 * x2;

    return  15.5     * x4 * x2
          - 40.14    * x4 * x
          + 31.96    * x4
          -  6.868   * x2 * x
          +  0.4298  * x2
          +  0.1191  * x
          -  0.00232;
}

vec3 agxCurve(vec3 color)
{
    const float min_ev = -12.47393;
    const float max_ev = 4.026069;

    color = AgXInsetMat * color;
    color = clamp(log2(max(color, 1e-10)), min_ev, max_ev);
    color = (color - min_ev) / (max_ev - min_ev);

    return agxContrastApprox(color);
}

vec3 agxEotf(vec3 color)
{
    color = AgXOutsetMat * color;

    // The contrast polynomial evaluates to -0.00232 at zero and the outset
    // matrix has negative off-diagonals, so this can be negative even for
    // well-behaved input. pow() of a negative base is undefined - guard it or
    // every black pixel becomes NaN.
    return clamp(pow(max(color, 0.0), vec3(2.2)), 0.0, 1.0);
}

// ASC-CDL style grade, applied in AgX's encoded space (before the EOTF).
// Note luma is taken from the value *before* the power, matching the reference.
vec3 agxLook(vec3 color, vec3 slope, vec3 power, float sat)
{
    const vec3 lw = vec3(0.2126, 0.7152, 0.0722);

    float luma = dot(color, lw);
    color = pow(max(color * slope, 0.0), power);

    return luma + sat * (color - luma);
}

vec3 toneMapAgX(vec3 color)
{
    return agxEotf(agxCurve(color));
}

vec3 toneMapAgXPunchy(vec3 color)
{
    return agxEotf(agxLook(agxCurve(color), vec3(1.0), vec3(1.35), 1.4));
}

vec3 toneMapAgXGolden(vec3 color)
{
    return agxEotf(agxLook(agxCurve(color), vec3(1.0, 0.9, 0.5), vec3(0.8), 0.8));
}


// Gran Turismo curve (Uchimura 2017).
// see https://www.desmos.com/calculator/gslcdxvipg
vec3 toneMapUchimura(vec3 x)
{
    const float P = 1.0;   // max display brightness
    const float a = 1.0;   // contrast
    const float m = 0.22;  // linear section start
    const float l = 0.4;   // linear section length
    const float c = 1.33;  // black tightness
    const float b = 0.0;   // pedestal

    float l0 = ((P - m) * l) / a;
    float S0 = m + l0;
    float S1 = m + a * l0;
    float C2 = (a * P) / (P - S1);
    float CP = -C2 / P;

    vec3 w0 = 1.0 - smoothstep(0.0, m, x);
    vec3 w2 = step(m + l0, x);
    vec3 w1 = 1.0 - w0 - w2;

    vec3 T = m * pow(max(x, 0.0) / m, vec3(c)) + b;  // toe
    vec3 L = m + a * (x - m);                        // linear
    vec3 S = P - (P - S1) * exp(CP * (x - S0));      // shoulder

    return clamp(T * w0 + L * w1 + S * w2, 0.0, 1.0);
}


// ---------------------------------------------------------------------------
// GT7 tone mapping.
//
// Ported from the sample implementation published alongside "Physically Based
// Tone Mapping and Perceptual Fidelity in GT7" (SIGGRAPH 2025 PBS course).
// SDR path only; all curve parameters are constant in SDR mode and are baked
// in below. ICtCp is used as the unified color space, per the reference default.
//
// MIT License
//
// Copyright (c) 2025 Polyphony Digital Inc.
//
// Permission is hereby granted, free of charge, to any person obtaining a copy
// of this software and associated documentation files (the "Software"), to deal
// in the Software without restriction, including without limitation the rights
// to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
// copies of the Software, and to permit persons to whom the Software is
// furnished to do so, subject to the following conditions:
//
// The above copyright notice and this permission notice shall be included in
// all copies or substantial portions of the Software.
//
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
// IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
// FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
// AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
// LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
// OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
// SOFTWARE.
// ---------------------------------------------------------------------------

const mat3 Rec709ToRec2020 = mat3
(
    0.6274, 0.0691, 0.0164,
    0.3293, 0.9195, 0.0880,
    0.0433, 0.0114, 0.8956
);

const mat3 Rec2020ToRec709 = mat3
(
    1.6605, -0.1246, -0.0182,
    -0.5876, 1.1329, -0.1006,
    -0.0728, -0.0083, 1.1187
);

// SDR constants: peak = 250nit paper white over a 100nit reference.
#define GT7_PEAK            2.5
#define GT7_MID             0.538
#define GT7_TOE             1.28
#define GT7_BRANCH          1.11    // linearSection * peak
#define GT7_KA              2.963333333333334
#define GT7_KB             -3.3733512380644313
#define GT7_KC             -0.539568345323741
#define GT7_BLEND           0.6
#define GT7_FADE_START      0.98
#define GT7_FADE_END        1.16
#define GT7_SDR_CORRECTION  0.4
#define GT7_TARGET_UCS      0.6025591549907509

vec3 gt7InverseEotfSt2084(vec3 v)
{
    const float m1 = 0.1593017578125;
    const float m2 = 78.84375;
    const float c1 = 0.8359375;
    const float c2 = 18.8515625;
    const float c3 = 18.6875;

    // frame buffer -> physical (x100) -> normalized for PQ (/10000)
    vec3 ym = pow(max(v, 0.0) * 0.01, vec3(m1));

    return exp2(m2 * (log2(c1 + c2 * ym) - log2(1.0 + c3 * ym)));
}

vec3 gt7EotfSt2084(vec3 n)
{
    const float m1 = 0.1593017578125;
    const float m2 = 78.84375;
    const float c1 = 0.8359375;
    const float c2 = 18.8515625;
    const float c3 = 18.6875;

    n = clamp(n, 0.0, 1.0);

    vec3 np = pow(n, vec3(1.0 / m2));
    vec3 l  = max(np - c1, 0.0) / (c2 - c3 * np);

    return pow(max(l, 0.0), vec3(1.0 / m1)) * 100.0;
}

vec3 gt7RgbToICtCp(vec3 rgb) // linear Rec.2020
{
    vec3 lms = vec3(dot(rgb, vec3(1688.0, 2146.0,  262.0)),
                    dot(rgb, vec3( 683.0, 2951.0,  462.0)),
                    dot(rgb, vec3(  99.0,  309.0, 3688.0))) / 4096.0;

    lms = gt7InverseEotfSt2084(lms);

    return vec3(dot(lms, vec3( 2048.0,   2048.0,    0.0)),
                dot(lms, vec3( 6610.0, -13613.0, 7003.0)),
                dot(lms, vec3(17933.0, -17390.0, -543.0))) / 4096.0;
}

vec3 gt7ICtCpToRgb(vec3 ictcp) // linear Rec.2020
{
    vec3 lms = vec3(ictcp.x + 0.00860904 * ictcp.y + 0.11103 * ictcp.z,
                    ictcp.x - 0.00860904 * ictcp.y - 0.11103 * ictcp.z,
                    ictcp.x + 0.560031  * ictcp.y - 0.320627 * ictcp.z);

    lms = gt7EotfSt2084(lms);

    return max(vec3(dot(lms, vec3( 3.43661,   -2.50645,    0.0698454)),
                    dot(lms, vec3(-0.79133,    1.9836,    -0.192271)),
                    dot(lms, vec3(-0.0259499, -0.0989137,  1.12486))), 0.0);
}

// GTToneMappingCurveV2, with a convergent exponential shoulder.
vec3 gt7Curve(vec3 x)
{
    vec3 weight_linear = smoothstep(0.0, GT7_MID, x);
    vec3 weight_toe    = 1.0 - weight_linear;

    vec3 toe      = GT7_MID * pow(max(x, 0.0) / GT7_MID, vec3(GT7_TOE));
    vec3 shoulder = GT7_KA + GT7_KB * exp(x * GT7_KC);

    return mix(shoulder, weight_toe * toe + weight_linear * x,
               step(x, vec3(GT7_BRANCH)));
}

vec3 toneMapGT7(vec3 color)
{
    vec3 rgb = max(Rec709ToRec2020 * color, 0.0);

    // Separate luminance from chroma, and tone map per channel ("skewed").
    vec3 ucs        = gt7RgbToICtCp(rgb);
    vec3 skewed     = gt7Curve(rgb);
    vec3 skewed_ucs = gt7RgbToICtCp(skewed);

    // Fade chroma out as luminance approaches the display target.
    float chroma_scale =
        1.0 - smoothstep(GT7_FADE_START, GT7_FADE_END, ucs.x / GT7_TARGET_UCS);

    vec3 scaled = gt7ICtCpToRgb(vec3(skewed_ucs.x, ucs.yz * chroma_scale));

    vec3 blended = GT7_SDR_CORRECTION *
                   min(mix(skewed, scaled, GT7_BLEND), vec3(GT7_PEAK));

    return clamp(Rec2020ToRec709 * blended, 0.0, 1.0);
}


// Lottes.
// see "Advanced Techniques and Optimization of HDR Color Pipelines", GDC 2016
vec3 toneMapLottes(vec3 x)
{
    const vec3 a      = vec3(1.6);    // contrast
    const vec3 d      = vec3(0.977);  // shoulder
    const vec3 hdrMax = vec3(8.0);
    const vec3 midIn  = vec3(0.18);
    const vec3 midOut = vec3(0.267);

    const vec3 denom = (pow(hdrMax, a * d) - pow(midIn, a * d)) * midOut;
    const vec3 b = (-pow(midIn, a) + pow(hdrMax, a) * midOut) / denom;
    const vec3 c = (pow(hdrMax, a * d) * pow(midIn, a)
                    - pow(hdrMax, a) * pow(midIn, a * d) * midOut) / denom;

    return clamp(pow(x, a) / (pow(x, a * d) * b + c), 0.0, 1.0);
}


uniform float exposure;
uniform float tonemap_mix;
uniform int tonemap_type;


vec3 applyTonemap(vec3 color)
{
    // Single input guard. Every pow()-based operator below is undefined for a
    // negative base, and nothing upstream enforces the non-negative contract
    // this file already documents.
    color = max(color, vec3(0.0));

    switch (tonemap_type)
    {
    case 0:  return PBRNeutralToneMapping(color);
    case 1:  return toneMapACES_Hill(color);
    case 2:  return toneMapACES_Narkowicz(color);
    case 3:  return toneMapReinhard(color);
    case 4:  return toneMapUncharted2(color);
    case 5:  return toneMapAgX(color);
    case 6:  return toneMapAgXPunchy(color);
    case 7:  return toneMapAgXGolden(color);
    case 8:  return toneMapUchimura(color);
    case 9:  return toneMapGT7(color);
    case 10: return toneMapLottes(color);
    }

    return clamp(color, 0.0, 1.0);
}


vec3 toneMap(vec3 color)
{
#ifndef NO_POST
    float exp_scale = texture(exposureMap, vec2(0.5,0.5)).r;
    float final_exposure = exposure * exp_scale;
    vec3 exposed_color = color * final_exposure;

    color = mix(exposed_color, applyTonemap(exposed_color), tonemap_mix);

    color = clamp(color, 0.0, 1.0);
#else
    color *= exposure * texture(exposureMap, vec2(0.5,0.5)).r;
    color = clamp(color, 0.0, 1.0);
#endif

    return color;
}


vec3 toneMapNoExposure(vec3 color)
{
#ifndef NO_POST
    color = mix(color, applyTonemap(color), tonemap_mix);

    color = clamp(color, 0.0, 1.0);
#else
     color = clamp(color, 0.0, 1.0);
#endif

    return color;
}


//===============================================================

void debugExposure(inout vec3 color)
{
    float exp_scale = texture(exposureMap, vec2(0.5,0.5)).r;
    exp_scale *= 0.5;
    if (abs(vary_fragcoord.y-exp_scale) < 0.01 && vary_fragcoord.x < 0.1)
    {
        color = vec3(1,0,0);
    }
}
