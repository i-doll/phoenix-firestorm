/**
 * @file fsDynResUpscaleF.glsl
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

/*[EXTRA_CODE_HERE]*/

// Dynamic resolution present: replaces postDeferredNoDoFF.glsl (noise variant) when the
// post-processed scene is smaller than the window. Catmull-Rom bicubic reconstruction (nine
// bilinear taps), contrast-adaptive extra sharpening in the spirit of CAS, and a clamp to the
// 2x2 source neighbourhood so neither can ring or halo. Writes depth like the stock present.

out vec4 frag_color;

uniform sampler2D diffuseRect;  // post-processed scene at the internal size, bilinear
uniform sampler2D depthMap;

uniform vec2 screen_res;            // internal (source) size in texels
uniform float fs_dynres_sharpness;  // 0 = plain Catmull-Rom, 1 = strongest

in vec2 vary_fragcoord;

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

vec3 catmullRom(vec2 uv)
{
    vec2 inv_size = 1.0 / screen_res;
    vec2 sample_pos = uv * screen_res;
    vec2 tex_pos1 = floor(sample_pos - 0.5) + 0.5;
    vec2 f = sample_pos - tex_pos1;

    vec2 w0 = f * (-0.5 + f * (1.0 - 0.5 * f));
    vec2 w1 = 1.0 + f * f * (-2.5 + 1.5 * f);
    vec2 w2 = f * (0.5 + f * (2.0 - 1.5 * f));
    vec2 w3 = f * f * (-0.5 + 0.5 * f);

    // The middle two taps share one bilinear fetch
    vec2 w12 = w1 + w2;
    vec2 offset12 = w2 / w12;

    vec2 tc0 = (tex_pos1 - 1.0) * inv_size;
    vec2 tc3 = (tex_pos1 + 2.0) * inv_size;
    vec2 tc12 = (tex_pos1 + offset12) * inv_size;

    vec3 result = vec3(0.0);
    result += texture(diffuseRect, vec2(tc0.x,  tc0.y)).rgb  * w0.x  * w0.y;
    result += texture(diffuseRect, vec2(tc12.x, tc0.y)).rgb  * w12.x * w0.y;
    result += texture(diffuseRect, vec2(tc3.x,  tc0.y)).rgb  * w3.x  * w0.y;

    result += texture(diffuseRect, vec2(tc0.x,  tc12.y)).rgb * w0.x  * w12.y;
    result += texture(diffuseRect, vec2(tc12.x, tc12.y)).rgb * w12.x * w12.y;
    result += texture(diffuseRect, vec2(tc3.x,  tc12.y)).rgb * w3.x  * w12.y;

    result += texture(diffuseRect, vec2(tc0.x,  tc3.y)).rgb  * w0.x  * w3.y;
    result += texture(diffuseRect, vec2(tc12.x, tc3.y)).rgb  * w12.x * w3.y;
    result += texture(diffuseRect, vec2(tc3.x,  tc3.y)).rgb  * w3.x  * w3.y;
    return result;
}

void main()
{
    vec2 uv = vary_fragcoord.xy;
    vec4 bilinear = texture(diffuseRect, uv);

    // The four source texels around this output pixel
    ivec2 size = ivec2(screen_res);
    ivec2 base = clamp(ivec2(floor(uv * screen_res - 0.5)), ivec2(0), size - 2);
    vec3 t00 = texelFetch(diffuseRect, base, 0).rgb;
    vec3 t10 = texelFetch(diffuseRect, base + ivec2(1, 0), 0).rgb;
    vec3 t01 = texelFetch(diffuseRect, base + ivec2(0, 1), 0).rgb;
    vec3 t11 = texelFetch(diffuseRect, base + ivec2(1, 1), 0).rgb;
    vec3 mn = min(min(t00, t10), min(t01, t11));
    vec3 mx = max(max(t00, t10), max(t01, t11));

    vec3 color = catmullRom(uv);

    // Extra sharpening along the Catmull-Rom minus bilinear direction, weighted like CAS: less
    // where the neighbourhood is already near black or white, so edges don't clip.
    vec3 lo = clamp(mn, vec3(0.0), vec3(1.0));
    vec3 hi = clamp(mx, vec3(0.0), vec3(1.0));
    vec3 amp = sqrt(clamp(min(lo, 1.0 - hi) / max(hi, vec3(1.0 / 256.0)), vec3(0.0), vec3(1.0)));
    color += (color - bilinear.rgb) * amp * fs_dynres_sharpness;

    color = clamp(color, mn, mx);

    vec2 tc = uv * screen_res * 4.0;
    vec3 seed = (color + vec3(1.0)) * vec3(tc.xy, tc.x + tc.y);
    color += vec3(noise(seed.rg), noise(seed.gb), noise(seed.rb)) * 0.003;

    // Same guard as clampHDRRange in the stock present
    color = mix(color, vec3(0.0), isnan(color));
    frag_color = vec4(clamp(color, vec3(0.0), vec3(11.2)), bilinear.a);

    gl_FragDepth = texture(depthMap, uv).r;
}
