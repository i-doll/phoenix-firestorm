/**
 * @file interface/oitCompositeF.glsl
 *
 * $LicenseInfo:firstyear=2025&license=viewerlgpl$
 * Second Life Viewer Source Code
 * Copyright (C) 2025, Linden Research, Inc.
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

in vec2 tc;

uniform sampler2D uAccum;
uniform sampler2D uReveal;

out vec4 frag_color;

void main()
{
    // The viewer enables GL_FRAMEBUFFER_SRGB for deferredScreen so the linear output below lands in sRGB space.
    // If that ever changes, convert explicitly before writing to frag_color.
    const float EPSILON = 1e-5;

    vec4 accum = texture(uAccum, tc);
    float reveal = clamp(texture(uReveal, tc).r, 0.0, 1.0);

    float weight = max(accum.a, EPSILON);
    vec3 avg_color = accum.rgb / weight;

    float alpha = clamp(1.0 - reveal, 0.0, 1.0);

    vec3 premult = avg_color * alpha;
    frag_color = vec4(premult, alpha);
}
