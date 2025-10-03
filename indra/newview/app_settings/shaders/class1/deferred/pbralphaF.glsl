/**
 * @file pbralphaF.glsl
 *
 * $LicenseInfo:firstyear=2023&license=viewerlgpl$
 * Second Life Viewer Source Code
 * Copyright (C) 2023, Linden Research, Inc.
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

 // debug stub

layout(location = 0) out vec4 frag_color;
layout(location = 1) out vec4 frag_reveal;
uniform int uOITPass;
uniform int uPremultAlpha;
uniform int uOITUseMRT;

void main()
{
    vec4 out_color = vec4(1.0, 0, 0.5, 0.5);

    if (uOITPass == OIT_PASS_ACCUM)
    {
        float alpha = clamp(out_color.a, 0.0, OIT_ALPHA_MAX);
        float weight = max(OIT_WEIGHT_MIN, exp(-OIT_WEIGHT_DEPTH_SCALE * gl_FragCoord.z) * (alpha + OIT_WEIGHT_ALPHA_BIAS));
        vec3 premult = out_color.rgb * alpha;
        frag_color = vec4(premult * weight, alpha * weight);
        frag_reveal = (uOITUseMRT != 0) ? vec4(alpha) : vec4(0.0);
    }
    else if (uOITPass == OIT_PASS_REVEAL)
    {
        float alpha = clamp(out_color.a, 0.0, OIT_ALPHA_MAX);
        // Non-MRT reveal pass: write alpha into the reveal output (location 1).
        frag_reveal = vec4(alpha);
        // The other output is inactive in this pass; write zeros for clarity.
        frag_color = vec4(0.0);
    }
    else
    {
        if (uPremultAlpha != 0)
        {
            float alpha = out_color.a;
            frag_color = vec4(out_color.rgb * alpha, alpha);
        }
        else
        {
            frag_color = out_color;
        }
        frag_reveal = vec4(0.0);
    }
}
