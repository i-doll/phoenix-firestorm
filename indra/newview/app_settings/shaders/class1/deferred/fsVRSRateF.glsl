/**
 * @file fsVRSRateF.glsl
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

// Variable rate shading (FSRenderVRS), pass 2 of 2. Turns the per tile levels of
// fsVRSClassifyF.glsl into the R8UI shading rate image of palette indices. The image comes from
// the previous frame, so detail may have moved by up to a tile: a tile may be at most one level
// coarser, per axis, than any of its eight neighbours.
//
// Palette (set in fsvrs.cpp, width x height pixels per invocation):
//   0 1x1, 1 1x2, 2 2x1, 3 2x2, 4 2x4, 5 4x2, 6 4x4

out uvec4 frag_color;

uniform sampler2D diffuseRect;  // levels from pass 1, RGBA8, one texel per tile
uniform ivec2 fs_vrs_tiles;     // its size

ivec2 levels_at(ivec2 t)
{
    t = clamp(t, ivec2(0), fs_vrs_tiles - ivec2(1));
    return ivec2(texelFetch(diffuseRect, t, 0).rg * 255.0 + 0.5);
}

void main()
{
    ivec2 t = ivec2(gl_FragCoord.xy);
    ivec2 level = levels_at(t);
    for (int y = -1; y <= 1; ++y)
    {
        for (int x = -1; x <= 1; ++x)
        {
            level = min(level, levels_at(t + ivec2(x, y)) + ivec2(1));
        }
    }

    // There are no 4x1 / 1x4 rates
    if (level.x == 2 && level.y == 0)
    {
        level.x = 1;
    }
    if (level.y == 2 && level.x == 0)
    {
        level.y = 1;
    }

    uint index = 0u;
    if (level.x == 0)
    {
        index = level.y == 0 ? 0u : 1u;
    }
    else if (level.x == 1)
    {
        index = level.y == 0 ? 2u : (level.y == 1 ? 3u : 4u);
    }
    else
    {
        index = level.y == 1 ? 5u : 6u;
    }
    frag_color = uvec4(index, 0u, 0u, 0u);
}
