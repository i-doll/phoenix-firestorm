/**
 * @file sunLightF.glsl
 *
 * $LicenseInfo:firstyear=2007&license=viewerlgpl$
 * Second Life Viewer Source Code
 * Copyright (C) 2007, Linden Research, Inc.
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

//class 2, shadows, no SSAO

// Inputs
in vec2 vary_fragcoord;

uniform vec3 sun_dir;
uniform float shadow_bias;

vec4 getNorm(vec2 pos_screen);
vec4 getPositionWithDepth(vec2 pos_screen, float depth);
float getDepth(vec2 pos_screen);
bool isFarDepth(float d);

float sampleDirectionalShadow(vec3 pos, vec3 norm, vec2 pos_screen);
#if defined(FS_CLOUD_SHADOWS) || defined(FS_CONTACT_SHADOWS)
float fsSunShadowExtras(float shadow, vec3 pos, vec3 norm); // fsShadowExtrasF.glsl
#endif

void main()
{
    vec2 pos_screen = vary_fragcoord.xy;
    float depth     = getDepth(pos_screen);

    // Sky: every term below already resolves to fully lit out here (beyond the last shadow
    // split), so skip the normal fetch and the reconstruction.
    if (isFarDepth(depth))
    {
        frag_color = vec4(1.0);
        return;
    }

    vec4 pos        = getPositionWithDepth(pos_screen, depth);
    vec4 norm       = getNorm(pos_screen);

    vec4 col;
    col.r = sampleDirectionalShadow(pos.xyz, norm.xyz, pos_screen);
#if defined(FS_CLOUD_SHADOWS) || defined(FS_CONTACT_SHADOWS)
    col.r = fsSunShadowExtras(col.r, pos.xyz, norm.xyz);
#endif
    col.g = 1.0f;
    col.b = 1.0f; // projector shadows are sampled per light in spotLightF
    col.a = 1.0f;

    frag_color = clamp(col, vec4(0), vec4(1));
}
