/**
 * @file fsVRSClassifyF.glsl
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

// Variable rate shading (FSRenderVRS), pass 1 of 2. One fragment per shading rate image tile
// (normally 16x16 pixels). Reads the finished, display-encoded colour and the depth of the frame
// just rendered and decides how coarsely each axis of the tile may be shaded next frame:
// level 0 = every pixel, 1 = every 2 pixels, 2 = every 4 pixels.
//
// The error estimate uses luminance differences between pixels 2 apart (4 apart for level 2).
// Those differences come out the same whether the frame they are measured on was shaded at 1x1
// or coarser, as coarse fragments are aligned to 2x2 / 4x4 pixel blocks; so a tile that went
// coarse doesn't flip back and forth. On a linear ramp of g per pixel, e2 and e4 below are both
// g. A strong luminance step anywhere in the tile keeps it at 1x1.
//
// Output, RGBA8: r = x level / 255, g = y level / 255.

out vec4 frag_color;

uniform sampler2D diffuseRect;      // post-processed colour, display encoded
uniform sampler2D depthMap;         // the frame's depth

uniform ivec2 fs_vrs_src_size;      // size of both, in pixels
uniform ivec2 fs_vrs_tile;          // shading rate image texel size, in pixels
uniform float fs_vrs_threshold;     // per pixel luminance step allowed for 2x along an axis
uniform float fs_vrs_periphery;     // 0..1, relaxes the threshold towards the corners
uniform int fs_vrs_max_level;       // 1 when 4 pixel wide rates are not available, else 2
uniform vec2 fs_vrs_proj;           // projection [2][2] and [3][2], to get view depth
uniform int fs_vrs_dof;             // 1 when depth of field ran this frame
uniform vec4 fs_vrs_dof_params;     // focal distance (negative), blur constant, magnification, pixel length
uniform float fs_vrs_dof_max;       // max circle of confusion, pixels

float lum(ivec2 p)
{
    p = clamp(p, ivec2(0), fs_vrs_src_size - ivec2(1));
    vec3 c = clamp(texelFetch(diffuseRect, p, 0).rgb, vec3(0.0), vec3(1.0));
    return dot(c, vec3(0.299, 0.587, 0.114));
}

// Same as cofF.glsl calc_cof(), as a magnitude in pixels.
float coc_pixels(float raw_depth)
{
    float ndc_z = raw_depth * 2.0 - 1.0;
    float depth = fs_vrs_proj.y / (-ndc_z - fs_vrs_proj.x); // view space z, negative
    float sc = (depth - fs_vrs_dof_params.x) / -depth * fs_vrs_dof_params.y;
    sc /= fs_vrs_dof_params.z;
    sc /= fs_vrs_dof_params.w;
    sc *= 1.414;
    return min(abs(sc), fs_vrs_dof_max);
}

void main()
{
    ivec2 origin = ivec2(gl_FragCoord.xy) * fs_vrs_tile;
    ivec2 last = fs_vrs_src_size - ivec2(1);

    // Depth: how defocused is the tile's sharpest pixel. Sky tiles get no special case: the
    // sky and its clouds are drawn in the G-buffer pass, and distant clouds are faint enough
    // that a looser threshold or 4x4 shading turns them blocky.
    float min_coc = 1.0e10;
    for (int y = 0; y < fs_vrs_tile.y; ++y)
    {
        for (int x = 0; x < fs_vrs_tile.x; ++x)
        {
            ivec2 p = min(origin + ivec2(x, y), last);
            float d = texelFetch(depthMap, p, 0).r;
            if (fs_vrs_dof != 0)
            {
                min_coc = min(min_coc, d >= 1.0 ? fs_vrs_dof_max : coc_pixels(d));
            }
        }
    }
    if (fs_vrs_dof == 0)
    {
        min_coc = 0.0;
    }

    // Level 2 is only for strongly defocused tiles
    bool quarter = fs_vrs_max_level >= 2 && min_coc >= 4.0;

    float sx2 = 0.0;
    float sy2 = 0.0;
    float qx2 = 0.0;
    float qy2 = 0.0;
    float step_max = 0.0;
    for (int y = 0; y < fs_vrs_tile.y; ++y)
    {
        for (int x = 0; x < fs_vrs_tile.x; ++x)
        {
            ivec2 p = origin + ivec2(x, y);
            float c = lum(p);
            float dx = lum(p + ivec2(2, 0)) - c;
            float dy = lum(p + ivec2(0, 2)) - c;
            sx2 += dx * dx;
            sy2 += dy * dy;
            step_max = max(step_max, max(abs(dx), abs(dy)));
            if (quarter)
            {
                float qx = lum(p + ivec2(4, 0)) - c;
                float qy = lum(p + ivec2(0, 4)) - c;
                qx2 += qx * qx;
                qy2 += qy * qy;
            }
        }
    }

    float n = float(fs_vrs_tile.x * fs_vrs_tile.y);
    vec2 e2 = 0.5 * sqrt(vec2(sx2, sy2) / n);
    vec2 e4 = 0.25 * sqrt(vec2(qx2, qy2) / n);

    float t = fs_vrs_threshold;
    vec2 centre = (vec2(origin) + 0.5 * vec2(fs_vrs_tile)) / vec2(fs_vrs_src_size) * 2.0 - 1.0;
    float r = length(centre) * 0.70710678;
    t *= 1.0 + 4.0 * fs_vrs_periphery * smoothstep(0.5, 1.0, r);
    // A step this large is an edge: the tile stays at full rate
    float edge = min(5.0 * t, 0.25);

    ivec2 level = ivec2(0);
    if (step_max <= edge)
    {
        level.x = e2.x < t ? 1 : 0;
        level.y = e2.y < t ? 1 : 0;
        if (quarter)
        {
            // coarse shading at 4 pixels errs about twice as much as at 2
            level.x = (level.x == 1 && 2.0 * e4.x < t) ? 2 : level.x;
            level.y = (level.y == 1 && 2.0 * e4.y < t) ? 2 : level.y;
        }
    }

    // Depth of field blurs these pixels anyway
    if (min_coc >= 2.0)
    {
        level = max(level, ivec2(1));
    }
    if (quarter && min_coc >= 4.0)
    {
        level = ivec2(2);
    }

    level = min(level, ivec2(fs_vrs_max_level));
    frag_color = vec4(vec2(level) / 255.0, 0.0, 1.0);
}
