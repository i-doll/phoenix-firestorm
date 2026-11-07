/**
 * @file class2/deferred/sunLightSSAOF.glsl
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

//class 2 -- shadows and SSAO

// Inputs
in vec2 vary_fragcoord;

vec4 getPositionWithDepth(vec2 pos_screen, float depth);
float getDepth(vec2 pos_screen);
bool isFarDepth(float d);
vec4 getNorm(vec2 pos_screen);

float sampleDirectionalShadow(vec3 shadow_pos, vec3 norm, vec2 pos_screen);
#if defined(FS_CLOUD_SHADOWS) || defined(FS_CONTACT_SHADOWS)
float fsSunShadowExtras(float shadow, vec3 pos, vec3 norm); // fsShadowExtrasF.glsl
#endif
float calcAmbientOcclusion(vec4 pos, vec3 norm, vec2 pos_screen);
#ifdef FS_GTAO
float fsCalcGTAO(vec3 pos, vec3 norm, vec2 pos_screen);
#endif
vec3 getGeoNorm(vec2 pos_screen);

void main()
{
    vec2 pos_screen = vary_fragcoord.xy;
    float depth = getDepth(pos_screen);

    // Sky: the shadow terms are already fully lit beyond the last split, and the lighting pass
    // never reads the occlusion of a sky fragment -- so the eight SSAO taps are spent on nothing.
    // The blur rejects sky samples by plane distance, so neighbours do not see the difference.
    if (isFarDepth(depth))
    {
        frag_color = vec4(1.0);
        return;
    }

    vec4 pos  = getPositionWithDepth(pos_screen, depth);
    vec4 norm = getNorm(pos_screen);

    vec4 col;
    col.r = sampleDirectionalShadow(pos.xyz, norm.xyz, pos_screen);
#if defined(FS_CLOUD_SHADOWS) || defined(FS_CONTACT_SHADOWS)
    col.r = fsSunShadowExtras(col.r, pos.xyz, norm.xyz);
#endif
    // Geometric normal, not the shading one. The occlusion test compares neighbouring depths
    // against this plane, and depth knows nothing about normal-mapped detail -- testing against a
    // perturbed normal makes a flat wall occlude itself wherever its map has texture.
#ifdef FS_GTAO
    col.g = fsCalcGTAO(pos.xyz, getGeoNorm(pos_screen), pos_screen);
#else
    col.g = calcAmbientOcclusion(pos, getGeoNorm(pos_screen), pos_screen);
#endif
    col.b = 1.0f; // projector shadows are sampled per light in spotLightF
    col.a = 1.0f;

    frag_color = clamp(col, vec4(0), vec4(1));
}
