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

out vec4 frag_color;

uniform sampler2D diffuseRect;

in vec2 vary_fragcoord;

#ifdef GAMMA_CORRECT
uniform float gamma;
#endif

vec3 linear_to_srgb(vec3 cl);
vec3 srgb_to_linear(vec3 cs);
vec3 toneMap(vec3 color);

vec3 clampHDRRange(vec3 color);

#ifndef NO_POST
// Bloom, the summed levels of the bloom chain, added before exposure
uniform sampler2D bloomMap;
uniform float bloom_strength;   // 0 = off
uniform float bloom_levels;

// Colour grading
uniform int grade_enabled;
uniform mat3 grade_white_balance;   // linear, applied to the scene before exposure
uniform float grade_lift;
uniform float grade_gamma;
uniform float grade_gain;
uniform float grade_contrast;
uniform float grade_saturation;
uniform float grade_vibrance;
uniform vec3 grade_shadow_tint;
uniform vec3 grade_highlight_tint;
uniform float grade_split_balance;
uniform sampler2D altDiffuseMap;    // lookup table, size*size wide and size high
uniform float grade_lut_size;       // 0 = none
uniform float grade_lut_strength;

float gradeLuma(vec3 c)
{
    return dot(c, vec3(0.2126, 0.7152, 0.0722));
}

// The lookup table stores one size x size slice per blue step, side by side: red across a slice,
// green up it. Two bilinear reads in the neighbouring blue slices, blended.
vec3 applyLut(vec3 c)
{
    float n = grade_lut_size;
    c = clamp(c, 0.0, 1.0);
    float blue = c.b * (n - 1.0);
    float b0 = floor(blue);
    float b1 = min(b0 + 1.0, n - 1.0);
    vec2 uv = vec2((c.r * (n - 1.0) + 0.5) / (n * n), (c.g * (n - 1.0) + 0.5) / n);
    vec3 s0 = texture(altDiffuseMap, uv + vec2(b0 / n, 0.0)).rgb;
    vec3 s1 = texture(altDiffuseMap, uv + vec2(b1 / n, 0.0)).rgb;
    return mix(s0, s1, blue - b0);
}

// Display referred grading on gamma encoded colour, where the controls behave the way they do
// in photo editors
vec3 gradeDisplay(vec3 s)
{
    // Lift raises the blacks without moving white, gain scales toward white, gamma bends the
    // midtones in between
    s = s * grade_gain + grade_lift * (1.0 - s);
    s = pow(max(s, vec3(0.0)), vec3(1.0 / max(grade_gamma, 0.01)));

    s = (s - 0.5) * grade_contrast + 0.5;

    float luma = gradeLuma(s);
    s = mix(vec3(luma), s, grade_saturation);

    // Vibrance saturates muted colours more than ones that are already strong
    float chroma = max(s.r, max(s.g, s.b)) - min(s.r, min(s.g, s.b));
    s = mix(vec3(luma), s, 1.0 + grade_vibrance * (1.0 - clamp(chroma, 0.0, 1.0)));

    // Split toning: shadows and highlights each take their own tint, white leaves them alone
    float highlight = smoothstep(0.0, 1.0, clamp(luma + grade_split_balance, 0.0, 1.0));
    s *= mix(grade_shadow_tint, grade_highlight_tint, highlight);

    s = clamp(s, 0.0, 1.0);

    if (grade_lut_size > 1.0)
    {
        s = mix(s, applyLut(s), grade_lut_strength);
    }
    return s;
}
#endif

#ifdef GAMMA_CORRECT
vec3 legacyGamma(vec3 color)
{
    vec3 c = 1. - clamp(color, vec3(0.), vec3(1.));
    c = 1. - pow(c, vec3(gamma)); // s/b inverted already CPU-side

    return c;
}
#endif

void main()
{
    //this is the one of the rare spots where diffuseRect contains linear color values (not sRGB)
    vec4 diff = texture(diffuseRect, vary_fragcoord);

#ifndef NO_POST
    if (bloom_strength > 0.0)
    {
        // The chain sums every level, so divide by their count for the average blur. It holds
        // only what passed the threshold, so it adds light around bright areas.
        vec3 bloom = texture(bloomMap, vary_fragcoord).rgb / max(bloom_levels, 1.0);
        diff.rgb += bloom * bloom_strength;
    }

    if (grade_enabled != 0)
    {
        diff.rgb = max(grade_white_balance * diff.rgb, vec3(0.0));
    }

    diff.rgb = toneMap(diff.rgb);

    if (grade_enabled != 0)
    {
        diff.rgb = srgb_to_linear(gradeDisplay(linear_to_srgb(diff.rgb)));
    }
#else
    diff.rgb = clamp(diff.rgb, vec3(0.0), vec3(1.0));
#endif

#ifdef GAMMA_CORRECT
    diff.rgb = linear_to_srgb(diff.rgb);

#ifdef LEGACY_GAMMA
    diff.rgb = legacyGamma(diff.rgb);
#endif

#endif

    diff.rgb = clamp(diff.rgb, vec3(0.0), vec3(1.0)); // We should always be 0-1 past this point

    //debugExposure(diff.rgb);
    frag_color = diff;
}

