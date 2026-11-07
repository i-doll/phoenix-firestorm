/**
 * @file fsNISUpscaleF.glsl
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
 * $/LicenseInfo$
 */

// The scaler below is a port of NVScaler from the NVIDIA Image Scaling SDK v1.0.3
// (github.com/NVIDIAGameWorks/NVIDIAImageScaling, NIS/NIS_Scaler.h and NIS/NIS_Config.h),
// which carries this notice:
//
// The MIT License(MIT)
//
// Copyright(c) 2022 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
//
// Permission is hereby granted, free of charge, to any person obtaining a copy of
// this software and associated documentation files(the "Software"), to deal in
// the Software without restriction, including without limitation the rights to
// use, copy, modify, merge, publish, distribute, sublicense, and / or sell copies of
// the Software, and to permit persons to whom the Software is furnished to do so,
// subject to the following conditions :
//
// The above copyright notice and this permission notice shall be included in all
// copies or substantial portions of the Software.
//
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
// IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY, FITNESS
// FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.IN NO EVENT SHALL THE AUTHORS OR
// COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER
// IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN
// CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.

/*[EXTRA_CODE_HERE]*/

// NIS present: replaces postDeferredNoDoFF.glsl (noise variant) when the post-processed scene
// is smaller than the window and FSRenderUpscaler selects NIS. Upstream is a compute shader
// that stages the luma of a source tile and its edge map in shared memory; this is a
// per-pixel port in which every output pixel fetches its own 6x6 source luma support and
// derives the 2x2 edge map it interpolates from the middle 4x4 of it. The algorithm, the
// coefficient tables and the constants (computed in fsdynamicresolution.cpp exactly like
// NVScalerUpdateConfig in SDR mode) are unchanged. The input is display-referred, tone mapped
// and gamma corrected, as NIS expects for SDR. Dither and depth are written like the stock
// present.

out vec4 frag_color;

uniform sampler2D diffuseRect;  // post-processed scene at the internal size
uniform sampler2D depthMap;

uniform vec2 screen_res;        // internal (source) size in texels

// NISConfig, from NVScalerUpdateConfig
uniform vec4 nis_detect;        // kDetectRatio, kDetectThres, kMinContrastRatio, kRatioNorm
uniform vec4 nis_sharp_y;       // kContrastBoost, kEps, kSharpStartY, kSharpScaleY
uniform vec4 nis_sharp;         // kSharpStrengthMin, kSharpStrengthScale, kSharpLimitMin, kSharpLimitScale

in vec2 vary_fragcoord;

uniform float fs_deband;    // > 0: triangular debanding dither replaces the stock noise
vec3 fsDeband(vec3 c);      // fsDebandF.glsl

#define kDetectRatio        nis_detect.x
#define kDetectThres        nis_detect.y
#define kMinContrastRatio   nis_detect.z
#define kRatioNorm          nis_detect.w
#define kContrastBoost      nis_sharp_y.x
#define kEps                nis_sharp_y.y
#define kSharpStartY        nis_sharp_y.z
#define kSharpScaleY        nis_sharp_y.w
#define kSharpStrengthMin   nis_sharp.x
#define kSharpStrengthScale nis_sharp.y
#define kSharpLimitMin      nis_sharp.z
#define kSharpLimitScale    nis_sharp.w

#define kPhaseCount 64

// coef_scale from NIS_Config.h: phase p uses taps 0..2 at [2p] and taps 3..5 at [2p + 1]
const vec3 kNisCoefScaler[128] = vec3[128](
    vec3(0.0000, 0.0000, 1.0000), vec3(0.0000, 0.0000, 0.0000),
    vec3(0.0029, -0.0127, 1.0000), vec3(0.0132, -0.0034, 0.0000),
    vec3(0.0063, -0.0249, 0.9985), vec3(0.0269, -0.0068, 0.0000),
    vec3(0.0088, -0.0361, 0.9956), vec3(0.0415, -0.0103, 0.0005),
    vec3(0.0117, -0.0474, 0.9932), vec3(0.0562, -0.0142, 0.0005),
    vec3(0.0142, -0.0576, 0.9897), vec3(0.0713, -0.0181, 0.0005),
    vec3(0.0166, -0.0674, 0.9844), vec3(0.0874, -0.0220, 0.0010),
    vec3(0.0186, -0.0762, 0.9785), vec3(0.1040, -0.0264, 0.0015),
    vec3(0.0205, -0.0850, 0.9727), vec3(0.1206, -0.0308, 0.0020),
    vec3(0.0225, -0.0928, 0.9648), vec3(0.1382, -0.0352, 0.0024),
    vec3(0.0239, -0.1006, 0.9575), vec3(0.1558, -0.0396, 0.0029),
    vec3(0.0254, -0.1074, 0.9487), vec3(0.1738, -0.0439, 0.0034),
    vec3(0.0264, -0.1138, 0.9390), vec3(0.1929, -0.0488, 0.0044),
    vec3(0.0278, -0.1191, 0.9282), vec3(0.2119, -0.0537, 0.0049),
    vec3(0.0288, -0.1245, 0.9170), vec3(0.2310, -0.0581, 0.0059),
    vec3(0.0293, -0.1294, 0.9058), vec3(0.2510, -0.0630, 0.0063),
    vec3(0.0303, -0.1333, 0.8926), vec3(0.2710, -0.0679, 0.0073),
    vec3(0.0308, -0.1367, 0.8789), vec3(0.2915, -0.0728, 0.0083),
    vec3(0.0308, -0.1401, 0.8657), vec3(0.3120, -0.0776, 0.0093),
    vec3(0.0313, -0.1426, 0.8506), vec3(0.3330, -0.0825, 0.0103),
    vec3(0.0313, -0.1445, 0.8354), vec3(0.3540, -0.0874, 0.0112),
    vec3(0.0313, -0.1460, 0.8193), vec3(0.3755, -0.0923, 0.0122),
    vec3(0.0313, -0.1470, 0.8022), vec3(0.3965, -0.0967, 0.0137),
    vec3(0.0308, -0.1479, 0.7856), vec3(0.4185, -0.1016, 0.0146),
    vec3(0.0303, -0.1479, 0.7681), vec3(0.4399, -0.1060, 0.0156),
    vec3(0.0298, -0.1479, 0.7505), vec3(0.4614, -0.1104, 0.0166),
    vec3(0.0293, -0.1470, 0.7314), vec3(0.4829, -0.1147, 0.0181),
    vec3(0.0288, -0.1460, 0.7119), vec3(0.5049, -0.1187, 0.0190),
    vec3(0.0278, -0.1445, 0.6929), vec3(0.5264, -0.1226, 0.0200),
    vec3(0.0273, -0.1431, 0.6724), vec3(0.5479, -0.1260, 0.0215),
    vec3(0.0264, -0.1411, 0.6528), vec3(0.5693, -0.1299, 0.0225),
    vec3(0.0254, -0.1387, 0.6323), vec3(0.5903, -0.1328, 0.0234),
    vec3(0.0244, -0.1357, 0.6113), vec3(0.6113, -0.1357, 0.0244),
    vec3(0.0234, -0.1328, 0.5903), vec3(0.6323, -0.1387, 0.0254),
    vec3(0.0225, -0.1299, 0.5693), vec3(0.6528, -0.1411, 0.0264),
    vec3(0.0215, -0.1260, 0.5479), vec3(0.6724, -0.1431, 0.0273),
    vec3(0.0200, -0.1226, 0.5264), vec3(0.6929, -0.1445, 0.0278),
    vec3(0.0190, -0.1187, 0.5049), vec3(0.7119, -0.1460, 0.0288),
    vec3(0.0181, -0.1147, 0.4829), vec3(0.7314, -0.1470, 0.0293),
    vec3(0.0166, -0.1104, 0.4614), vec3(0.7505, -0.1479, 0.0298),
    vec3(0.0156, -0.1060, 0.4399), vec3(0.7681, -0.1479, 0.0303),
    vec3(0.0146, -0.1016, 0.4185), vec3(0.7856, -0.1479, 0.0308),
    vec3(0.0137, -0.0967, 0.3965), vec3(0.8022, -0.1470, 0.0313),
    vec3(0.0122, -0.0923, 0.3755), vec3(0.8193, -0.1460, 0.0313),
    vec3(0.0112, -0.0874, 0.3540), vec3(0.8354, -0.1445, 0.0313),
    vec3(0.0103, -0.0825, 0.3330), vec3(0.8506, -0.1426, 0.0313),
    vec3(0.0093, -0.0776, 0.3120), vec3(0.8657, -0.1401, 0.0308),
    vec3(0.0083, -0.0728, 0.2915), vec3(0.8789, -0.1367, 0.0308),
    vec3(0.0073, -0.0679, 0.2710), vec3(0.8926, -0.1333, 0.0303),
    vec3(0.0063, -0.0630, 0.2510), vec3(0.9058, -0.1294, 0.0293),
    vec3(0.0059, -0.0581, 0.2310), vec3(0.9170, -0.1245, 0.0288),
    vec3(0.0049, -0.0537, 0.2119), vec3(0.9282, -0.1191, 0.0278),
    vec3(0.0044, -0.0488, 0.1929), vec3(0.9390, -0.1138, 0.0264),
    vec3(0.0034, -0.0439, 0.1738), vec3(0.9487, -0.1074, 0.0254),
    vec3(0.0029, -0.0396, 0.1558), vec3(0.9575, -0.1006, 0.0239),
    vec3(0.0024, -0.0352, 0.1382), vec3(0.9648, -0.0928, 0.0225),
    vec3(0.0020, -0.0308, 0.1206), vec3(0.9727, -0.0850, 0.0205),
    vec3(0.0015, -0.0264, 0.1040), vec3(0.9785, -0.0762, 0.0186),
    vec3(0.0010, -0.0220, 0.0874), vec3(0.9844, -0.0674, 0.0166),
    vec3(0.0005, -0.0181, 0.0713), vec3(0.9897, -0.0576, 0.0142),
    vec3(0.0005, -0.0142, 0.0562), vec3(0.9932, -0.0474, 0.0117),
    vec3(0.0005, -0.0103, 0.0415), vec3(0.9956, -0.0361, 0.0088),
    vec3(0.0000, -0.0068, 0.0269), vec3(0.9985, -0.0249, 0.0063),
    vec3(0.0000, -0.0034, 0.0132), vec3(1.0000, -0.0127, 0.0029)
);

// coef_usm from NIS_Config.h: phase p uses taps 0..2 at [2p] and taps 3..5 at [2p + 1]
const vec3 kNisCoefUSM[128] = vec3[128](
    vec3(0.0000, -0.6001, 1.2002), vec3(-0.6001, 0.0000, 0.0000),
    vec3(0.0029, -0.6084, 1.1987), vec3(-0.5903, -0.0029, 0.0000),
    vec3(0.0049, -0.6147, 1.1958), vec3(-0.5791, -0.0068, 0.0005),
    vec3(0.0073, -0.6196, 1.1890), vec3(-0.5659, -0.0103, 0.0000),
    vec3(0.0093, -0.6235, 1.1802), vec3(-0.5513, -0.0151, 0.0000),
    vec3(0.0112, -0.6265, 1.1699), vec3(-0.5352, -0.0195, 0.0005),
    vec3(0.0122, -0.6270, 1.1582), vec3(-0.5181, -0.0259, 0.0005),
    vec3(0.0142, -0.6284, 1.1455), vec3(-0.5005, -0.0317, 0.0005),
    vec3(0.0156, -0.6265, 1.1274), vec3(-0.4790, -0.0386, 0.0005),
    vec3(0.0166, -0.6235, 1.1089), vec3(-0.4570, -0.0454, 0.0010),
    vec3(0.0176, -0.6187, 1.0879), vec3(-0.4346, -0.0532, 0.0010),
    vec3(0.0181, -0.6138, 1.0659), vec3(-0.4102, -0.0615, 0.0015),
    vec3(0.0190, -0.6069, 1.0405), vec3(-0.3843, -0.0698, 0.0015),
    vec3(0.0195, -0.6006, 1.0161), vec3(-0.3574, -0.0796, 0.0020),
    vec3(0.0200, -0.5928, 0.9893), vec3(-0.3286, -0.0898, 0.0024),
    vec3(0.0200, -0.5820, 0.9580), vec3(-0.2988, -0.1001, 0.0029),
    vec3(0.0200, -0.5728, 0.9292), vec3(-0.2690, -0.1104, 0.0034),
    vec3(0.0200, -0.5620, 0.8975), vec3(-0.2368, -0.1226, 0.0039),
    vec3(0.0205, -0.5498, 0.8643), vec3(-0.2046, -0.1343, 0.0044),
    vec3(0.0200, -0.5371, 0.8301), vec3(-0.1709, -0.1465, 0.0049),
    vec3(0.0195, -0.5239, 0.7944), vec3(-0.1367, -0.1587, 0.0054),
    vec3(0.0195, -0.5107, 0.7598), vec3(-0.1021, -0.1724, 0.0059),
    vec3(0.0190, -0.4966, 0.7231), vec3(-0.0649, -0.1865, 0.0063),
    vec3(0.0186, -0.4819, 0.6846), vec3(-0.0288, -0.1997, 0.0068),
    vec3(0.0186, -0.4668, 0.6460), vec3(0.0093, -0.2144, 0.0073),
    vec3(0.0176, -0.4507, 0.6055), vec3(0.0479, -0.2290, 0.0083),
    vec3(0.0171, -0.4370, 0.5693), vec3(0.0859, -0.2446, 0.0088),
    vec3(0.0161, -0.4199, 0.5283), vec3(0.1255, -0.2598, 0.0098),
    vec3(0.0161, -0.4048, 0.4883), vec3(0.1655, -0.2754, 0.0103),
    vec3(0.0151, -0.3887, 0.4497), vec3(0.2041, -0.2910, 0.0107),
    vec3(0.0142, -0.3711, 0.4072), vec3(0.2446, -0.3066, 0.0117),
    vec3(0.0137, -0.3555, 0.3672), vec3(0.2852, -0.3228, 0.0122),
    vec3(0.0132, -0.3394, 0.3262), vec3(0.3262, -0.3394, 0.0132),
    vec3(0.0122, -0.3228, 0.2852), vec3(0.3672, -0.3555, 0.0137),
    vec3(0.0117, -0.3066, 0.2446), vec3(0.4072, -0.3711, 0.0142),
    vec3(0.0107, -0.2910, 0.2041), vec3(0.4497, -0.3887, 0.0151),
    vec3(0.0103, -0.2754, 0.1655), vec3(0.4883, -0.4048, 0.0161),
    vec3(0.0098, -0.2598, 0.1255), vec3(0.5283, -0.4199, 0.0161),
    vec3(0.0088, -0.2446, 0.0859), vec3(0.5693, -0.4370, 0.0171),
    vec3(0.0083, -0.2290, 0.0479), vec3(0.6055, -0.4507, 0.0176),
    vec3(0.0073, -0.2144, 0.0093), vec3(0.6460, -0.4668, 0.0186),
    vec3(0.0068, -0.1997, -0.0288), vec3(0.6846, -0.4819, 0.0186),
    vec3(0.0063, -0.1865, -0.0649), vec3(0.7231, -0.4966, 0.0190),
    vec3(0.0059, -0.1724, -0.1021), vec3(0.7598, -0.5107, 0.0195),
    vec3(0.0054, -0.1587, -0.1367), vec3(0.7944, -0.5239, 0.0195),
    vec3(0.0049, -0.1465, -0.1709), vec3(0.8301, -0.5371, 0.0200),
    vec3(0.0044, -0.1343, -0.2046), vec3(0.8643, -0.5498, 0.0205),
    vec3(0.0039, -0.1226, -0.2368), vec3(0.8975, -0.5620, 0.0200),
    vec3(0.0034, -0.1104, -0.2690), vec3(0.9292, -0.5728, 0.0200),
    vec3(0.0029, -0.1001, -0.2988), vec3(0.9580, -0.5820, 0.0200),
    vec3(0.0024, -0.0898, -0.3286), vec3(0.9893, -0.5928, 0.0200),
    vec3(0.0020, -0.0796, -0.3574), vec3(1.0161, -0.6006, 0.0195),
    vec3(0.0015, -0.0698, -0.3843), vec3(1.0405, -0.6069, 0.0190),
    vec3(0.0015, -0.0615, -0.4102), vec3(1.0659, -0.6138, 0.0181),
    vec3(0.0010, -0.0532, -0.4346), vec3(1.0879, -0.6187, 0.0176),
    vec3(0.0010, -0.0454, -0.4570), vec3(1.1089, -0.6235, 0.0166),
    vec3(0.0005, -0.0386, -0.4790), vec3(1.1274, -0.6265, 0.0156),
    vec3(0.0005, -0.0317, -0.5005), vec3(1.1455, -0.6284, 0.0142),
    vec3(0.0005, -0.0259, -0.5181), vec3(1.1582, -0.6270, 0.0122),
    vec3(0.0005, -0.0195, -0.5352), vec3(1.1699, -0.6265, 0.0112),
    vec3(0.0000, -0.0151, -0.5513), vec3(1.1802, -0.6235, 0.0093),
    vec3(0.0000, -0.0103, -0.5659), vec3(1.1890, -0.6196, 0.0073),
    vec3(0.0005, -0.0068, -0.5791), vec3(1.1958, -0.6147, 0.0049),
    vec3(0.0000, -0.0029, -0.5903), vec3(1.1987, -0.6084, 0.0029)
);
// The 6x6 luma support, row major: P(i, j) is row i (y), column j (x), as p[i][j] upstream.
// Row and column 0 are the source texel two before floor(src).
#define P(i, j) p[(i) * 6 + (j)]

float getY(vec3 rgba)
{
    return 0.2126 * rgba.x + 0.7152 * rgba.y + 0.0722 * rgba.z;
}

// GetEdgeMap(p[4][4], i, j) upstream; q is the middle 4x4 of the support, so (i, j) picks
// the 3x3 around source texel (floor + j, floor + i).
#define Q(a, b) q[((a) + i) * 4 + (b) + j]
vec4 GetEdgeMap(float q[16], int i, int j)
{
    float g_0 = abs(Q(0, 0) + Q(0, 1) + Q(0, 2) - Q(2, 0) - Q(2, 1) - Q(2, 2));
    float g_45 = abs(Q(1, 0) + Q(0, 0) + Q(0, 1) - Q(2, 1) - Q(2, 2) - Q(1, 2));
    float g_90 = abs(Q(0, 0) + Q(1, 0) + Q(2, 0) - Q(0, 2) - Q(1, 2) - Q(2, 2));
    float g_135 = abs(Q(1, 0) + Q(2, 0) + Q(2, 1) - Q(0, 1) - Q(0, 2) - Q(1, 2));

    float g_0_90_max = max(g_0, g_90);
    float g_0_90_min = min(g_0, g_90);
    float g_45_135_max = max(g_45, g_135);
    float g_45_135_min = min(g_45, g_135);

    if (g_0_90_max + g_45_135_max == 0.0)
    {
        return vec4(0.0);
    }

    float e_0_90 = min(g_0_90_max / (g_0_90_max + g_45_135_max), 1.0);
    float e_45_135 = 1.0 - e_0_90;

    bool c_0_90 = (g_0_90_max > (g_0_90_min * kDetectRatio)) && (g_0_90_max > kDetectThres) && (g_0_90_max > g_45_135_min);
    bool c_45_135 = (g_45_135_max > (g_45_135_min * kDetectRatio)) && (g_45_135_max > kDetectThres) && (g_45_135_max > g_0_90_min);
    bool c_g_0_90 = g_0_90_max == g_0;
    bool c_g_45_135 = g_45_135_max == g_45;

    float f_e_0_90 = (c_0_90 && c_45_135) ? e_0_90 : 1.0;
    float f_e_45_135 = (c_0_90 && c_45_135) ? e_45_135 : 1.0;

    float weight_0 = (c_0_90 && c_g_0_90) ? f_e_0_90 : 0.0;
    float weight_90 = (c_0_90 && !c_g_0_90) ? f_e_0_90 : 0.0;
    float weight_45 = (c_45_135 && c_g_45_135) ? f_e_45_135 : 0.0;
    float weight_135 = (c_45_135 && !c_g_45_135) ? f_e_45_135 : 0.0;

    return vec4(weight_0, weight_90, weight_45, weight_135);
}
#undef Q

float CalcLTI(float p0, float p1, float p2, float p3, float p4, float p5, int phase_index)
{
    bool selector = (phase_index <= kPhaseCount / 2);
    float sel = selector ? p0 : p3;
    float a_min = min(min(p1, p2), sel);
    float a_max = max(max(p1, p2), sel);
    sel = selector ? p2 : p5;
    float b_min = min(min(p3, p4), sel);
    float b_max = max(max(p3, p4), sel);

    float a_cont = a_max - a_min;
    float b_cont = b_max - b_min;

    float cont_ratio = max(a_cont, b_cont) / (min(a_cont, b_cont) + kEps);
    return (1.0 - clamp((cont_ratio - kMinContrastRatio) * kRatioNorm, 0.0, 1.0)) * kContrastBoost;
}

vec4 GetInterpEdgeMap(vec4 e00, vec4 e01, vec4 e10, vec4 e11, float phase_frac_x, float phase_frac_y)
{
    vec4 h0 = mix(e00, e01, phase_frac_x);
    vec4 h1 = mix(e10, e11, phase_frac_x);
    return mix(h0, h1, phase_frac_y);
}

float EvalPoly6(float pxl[6], int phase_int)
{
    phase_int = clamp(phase_int, 0, kPhaseCount - 1);
    vec3 lo = vec3(pxl[0], pxl[1], pxl[2]);
    vec3 hi = vec3(pxl[3], pxl[4], pxl[5]);
    float y = dot(kNisCoefScaler[phase_int * 2], lo) + dot(kNisCoefScaler[phase_int * 2 + 1], hi);
    float y_usm = dot(kNisCoefUSM[phase_int * 2], lo) + dot(kNisCoefUSM[phase_int * 2 + 1], hi);

    // let's compute a piece-wise ramp based on luma
    float y_scale = 1.0 - clamp((y - kSharpStartY) * kSharpScaleY, 0.0, 1.0);

    // scale the ramp to sharpen as a function of luma
    float y_sharpness = y_scale * kSharpStrengthScale + kSharpStrengthMin;

    y_usm *= y_sharpness;

    // scale the ramp to limit USM as a function of luma
    float y_sharpness_limit = (y_scale * kSharpLimitScale + kSharpLimitMin) * y;

    y_usm = min(y_sharpness_limit, max(-y_sharpness_limit, y_usm));
    // reduce ringing
    y_usm *= CalcLTI(pxl[0], pxl[1], pxl[2], pxl[3], pxl[4], pxl[5], phase_int);

    return y + y_usm;
}

float FilterNormal(float p[36], int phase_x_frac_int, int phase_y_frac_int)
{
    vec3 cy0 = kNisCoefScaler[phase_y_frac_int * 2];
    vec3 cy1 = kNisCoefScaler[phase_y_frac_int * 2 + 1];
    vec3 cx0 = kNisCoefScaler[phase_x_frac_int * 2];
    vec3 cx1 = kNisCoefScaler[phase_x_frac_int * 2 + 1];
    float cx[6] = float[6](cx0.x, cx0.y, cx0.z, cx1.x, cx1.y, cx1.z);
    float h_acc = 0.0;
    for (int j = 0; j < 6; ++j)
    {
        float v_acc = dot(vec3(P(0, j), P(1, j), P(2, j)), cy0) + dot(vec3(P(3, j), P(4, j), P(5, j)), cy1);
        h_acc += v_acc * cx[j];
    }
    return h_acc;
}

float AddDirFilters(float p[36], float phase_x_frac, float phase_y_frac, int phase_x_frac_int, int phase_y_frac_int, vec4 w)
{
    float f = 0.0;
    if (w.x > 0.0)
    {
        // 0 deg filter
        float interp0Deg[6];
        for (int i = 0; i < 6; ++i)
        {
            interp0Deg[i] = mix(P(i, 2), P(i, 3), phase_x_frac);
        }
        f += EvalPoly6(interp0Deg, phase_y_frac_int) * w.x;
    }
    if (w.y > 0.0)
    {
        // 90 deg filter
        float interp90Deg[6];
        for (int i = 0; i < 6; ++i)
        {
            interp90Deg[i] = mix(P(2, i), P(3, i), phase_y_frac);
        }
        f += EvalPoly6(interp90Deg, phase_x_frac_int) * w.y;
    }
    if (w.z > 0.0)
    {
        // 45 deg filter
        float pphase_b45 = 0.5 + 0.5 * (phase_x_frac - phase_y_frac);

        float temp_interp45Deg[7];
        temp_interp45Deg[1] = mix(P(2, 1), P(1, 2), pphase_b45);
        temp_interp45Deg[3] = mix(P(3, 2), P(2, 3), pphase_b45);
        temp_interp45Deg[5] = mix(P(4, 3), P(3, 4), pphase_b45);
        {
            pphase_b45 = pphase_b45 - 0.5;
            float a = (pphase_b45 >= 0.0) ? P(0, 2) : P(2, 0);
            float b = (pphase_b45 >= 0.0) ? P(1, 3) : P(3, 1);
            float c = (pphase_b45 >= 0.0) ? P(2, 4) : P(4, 2);
            float d = (pphase_b45 >= 0.0) ? P(3, 5) : P(5, 3);
            temp_interp45Deg[0] = mix(P(1, 1), a, abs(pphase_b45));
            temp_interp45Deg[2] = mix(P(2, 2), b, abs(pphase_b45));
            temp_interp45Deg[4] = mix(P(3, 3), c, abs(pphase_b45));
            temp_interp45Deg[6] = mix(P(4, 4), d, abs(pphase_b45));
        }

        float interp45Deg[6];
        float pphase_p45 = phase_x_frac + phase_y_frac;
        int shift45 = 0;
        if (pphase_p45 >= 1.0)
        {
            shift45 = 1;
            pphase_p45 = pphase_p45 - 1.0;
        }
        for (int i = 0; i < 6; i++)
        {
            interp45Deg[i] = temp_interp45Deg[i + shift45];
        }

        f += EvalPoly6(interp45Deg, int(pphase_p45 * 64.0)) * w.z;
    }
    if (w.w > 0.0)
    {
        // 135 deg filter
        float pphase_b135 = 0.5 * (phase_x_frac + phase_y_frac);

        float temp_interp135Deg[7];
        temp_interp135Deg[1] = mix(P(3, 1), P(4, 2), pphase_b135);
        temp_interp135Deg[3] = mix(P(2, 2), P(3, 3), pphase_b135);
        temp_interp135Deg[5] = mix(P(1, 3), P(2, 4), pphase_b135);
        {
            pphase_b135 = pphase_b135 - 0.5;
            float a = (pphase_b135 >= 0.0) ? P(5, 2) : P(3, 0);
            float b = (pphase_b135 >= 0.0) ? P(4, 3) : P(2, 1);
            float c = (pphase_b135 >= 0.0) ? P(3, 4) : P(1, 2);
            float d = (pphase_b135 >= 0.0) ? P(2, 5) : P(0, 3);
            temp_interp135Deg[0] = mix(P(4, 1), a, abs(pphase_b135));
            temp_interp135Deg[2] = mix(P(3, 2), b, abs(pphase_b135));
            temp_interp135Deg[4] = mix(P(2, 3), c, abs(pphase_b135));
            temp_interp135Deg[6] = mix(P(1, 4), d, abs(pphase_b135));
        }

        float interp135Deg[6];
        float pphase_p135 = 1.0 + (phase_x_frac - phase_y_frac);
        int shift135 = 0;
        if (pphase_p135 >= 1.0)
        {
            shift135 = 1;
            pphase_p135 = pphase_p135 - 1.0;
        }
        for (int i = 0; i < 6; ++i)
        {
            interp135Deg[i] = temp_interp135Deg[i + shift135];
        }

        f += EvalPoly6(interp135Deg, int(pphase_p135 * 64.0)) * w.w;
    }
    return f;
}

// Dither noise as the stock present adds it
float hash(vec2 p) { return fract(1e4 * sin(17.0 * p.x + p.y * 0.1) * (0.1 + abs(sin(p.y * 13.0 + p.x)))); }

float noise(vec2 x)
{
    vec2 i = floor(x);
    vec2 f = fract(x);
    float a = hash(i);
    float b = hash(i + vec2(1.0, 0.0));
    float c = hash(i + vec2(0.0, 1.0));
    float d = hash(i + vec2(1.0, 1.0));
    vec2 u = f * f * (3.0 - 2.0 * f);
    return mix(a, b, u.x) + (c - a) * u.y * (1.0 - u.x) + (d - b) * u.x * u.y;
}

void main()
{
    vec2 uv = vary_fragcoord.xy;

    // Source position of this output pixel, (0.5 + dst) * kScale - 0.5 upstream
    vec2 src = uv * screen_res - 0.5;
    vec2 src_floor = floor(src);
    float fx = src.x - src_floor.x;
    float fy = src.y - src_floor.y;
    int fx_int = clamp(int(fx * float(kPhaseCount)), 0, kPhaseCount - 1);
    int fy_int = clamp(int(fy * float(kPhaseCount)), 0, kPhaseCount - 1);

    // The 6x6 luma support, texels floor - 2 .. floor + 3, clamped to the edge like the
    // linear clamp sampler upstream
    ivec2 size_max = ivec2(screen_res) - 1;
    ivec2 base = ivec2(src_floor) - 2;
    float p[36];
    for (int i = 0; i < 6; ++i)
    {
        int ty = clamp(base.y + i, 0, size_max.y);
        for (int j = 0; j < 6; ++j)
        {
            int tx = clamp(base.x + j, 0, size_max.x);
            P(i, j) = getY(texelFetch(diffuseRect, ivec2(tx, ty), 0).rgb);
        }
    }

    // Edge map of the 2x2 texels around the sample, from the middle 4x4 of the support
    float q[16];
    for (int i = 0; i < 4; ++i)
    {
        for (int j = 0; j < 4; ++j)
        {
            q[i * 4 + j] = P(i + 1, j + 1);
        }
    }
    vec4 w = GetInterpEdgeMap(GetEdgeMap(q, 0, 0), GetEdgeMap(q, 0, 1),
                              GetEdgeMap(q, 1, 0), GetEdgeMap(q, 1, 1), fx, fy);

    // weight for luma
    float baseWeight = 1.0 - w.x - w.y - w.z - w.w;

    // final luma is a weighted product of directional & normal filters
    float opY = 0.0;
    opY += FilterNormal(p, fx_int, fy_int) * baseWeight;
    opY += AddDirFilters(p, fx, fy, fx_int, fy_int, w);

    // do bilinear tap for chroma upscaling, then move it to the new luma
    vec4 op = texture(diffuseRect, uv);
    vec3 color = op.rgb + (opY - getY(op.rgb));

    if (fs_deband > 0.0)
    {
        // Triangular debanding dither (fsDebandF.glsl) in place of the stock noise
        color = fsDeband(clamp(color, vec3(0.0), vec3(1.0)));
    }
    else
    {
        vec2 tc = uv * screen_res * 4.0;
        vec3 seed = (color + vec3(1.0)) * vec3(tc.xy, tc.x + tc.y);
        color += vec3(noise(seed.rg), noise(seed.gb), noise(seed.rb)) * 0.003;
    }

    // Same guard as clampHDRRange in the stock present
    color = mix(color, vec3(0.0), isnan(color));
    frag_color = vec4(clamp(color, vec3(0.0), vec3(11.2)), op.a);

    gl_FragDepth = texture(depthMap, uv).r;
}
