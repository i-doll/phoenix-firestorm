/**
 * @file class1/deferred/fsShadowFadeF.glsl
 *
 * $LicenseInfo:firstyear=2026&license=viewerlgpl$
 * Phoenix Firestorm Viewer Source Code
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

// Smooth sun shadow fade (FSRenderShadowSmoothFade), used by sampleDirectionalShadow in
// shadowUtil.glsl. Compiled with the basic shaders and attached next to shadowUtil.glsl only while
// the global define FS_SHADOW_EDGE_FADE is set; its value is the fraction of the shadow range over
// which shadows fade out at its far end.

uniform vec4 shadow_clip;

// Non-zero: pick one cascade per pixel in the overlap of two cascades, by a dither, instead of
// blending both. Set by the deferred sun pass alone, and only while the shadow blur runs after it
// (it turns the dither into a smooth transition); every other shadow consumer keeps the blend.
uniform int fs_shadow_cascade_dither;

// interleaved gradient noise (Jimenez 2014), kept off 0 and 1 so the two sides of a pair below
// always pick exactly one cascade
float fsCascadeNoise()
{
    float n = fract(52.9829189 * fract(0.06711056 * gl_FragCoord.x + 0.00583715 * gl_FragCoord.y));
    return clamp(n, 0.002, 0.998);
}

// The overlap of cascades k and k+1 at a pixel: t (0..1) from k's side to k+1's. sampleDirectionalShadow
// weighs cascade k by 1 - t (fsCascadeFadeOut gets t) and cascade k+1 by 1 - (1 - t)
// (fsCascadeFadeIn gets 1 - t). Dithered, cascade k+1 takes the pixel where t >= noise and k
// elsewhere: the weights become 0 or 1 and still add up to 1.
float fsCascadeFadeOut(float t)
{
    if (fs_shadow_cascade_dither == 0 || t <= 0.0)
    {
        return t;
    }
    return t >= fsCascadeNoise() ? 1.0 : 0.0;
}

float fsCascadeFadeIn(float f)
{
    if (fs_shadow_cascade_dither == 0 || f <= 0.0)
    {
        return f;
    }
    return f > 1.0 - fsCascadeNoise() ? 1.0 : 0.0;
}

// shadow faded smoothly to lit over the last FS_SHADOW_EDGE_FADE of the shadow range, ending at
// its far end (shadow_clip.w), where the shadow maps stop; z: view space z of the lookup
float fsShadowEdgeFade(float shadow, float z)
{
    float range = shadow_clip.w;
    float len = max(range * float(FS_SHADOW_EDGE_FADE), 0.5);
    return mix(shadow, 1.0, smoothstep(range - len, range, -z));
}
