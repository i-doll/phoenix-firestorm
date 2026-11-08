/**
 * @file fsShadowSinglePassMergeF.glsl
 * @brief Single pass sun shadow cascades: merges a cascade's cell of the shadow atlas into its shadow map
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
 *
 * The Phoenix Firestorm Project, Inc., 1831 Oakwood Drive, Fairmont, Minnesota 56031-3225 USA
 * http://www.firestormviewer.org
 * $/LicenseInfo$
 */

// Drawn over a whole sun shadow map with depth test GL_LESS and depth writes on, so the map keeps
// the nearer of its own depth (the casters drawn per cascade) and the atlas cell's (the casters
// drawn once for all cascades). The cell has the map's size, so texels match one to one.

out vec4 frag_color;

uniform sampler2D diffuseMap;   // the atlas depth, compare mode off
uniform vec2 fs_shadow_cell;    // texel offset of this cascade's cell in the atlas

void main()
{
    float depth = texelFetch(diffuseMap, ivec2(gl_FragCoord.xy) + ivec2(fs_shadow_cell), 0).r;
    if (depth >= 1.0)
    {   // nothing drawn there, the clear value can't be nearer
        discard;
    }
    gl_FragDepth = depth;
    frag_color = vec4(1.0);
}
