/**
 * @file class3\deferred\pointLightF.glsl
 *
 * $LicenseInfo:firstyear=2022&license=viewerlgpl$
 * Second Life Viewer Source Code
 * Copyright (C) 2022, Linden Research, Inc.
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

uniform vec3 env_mat[3];
uniform float sun_wash;

// light params
uniform vec3 color;
uniform float falloff;
uniform float size;

in vec4 vary_fragcoord;
in vec3 trans_center;

uniform vec2 screen_res;

uniform mat4 inv_proj;
uniform vec4 viewport;
uniform int classic_mode;

void calcHalfVectors(vec3 lv, vec3 n, vec3 v, out vec3 h, out vec3 l, out float nh, out float nl, out float nv, out float vh, out float lightDist);
float blinnPhongLobe(float nh, float glossiness);
void calcDiffuseSpecular(vec3 baseColor, float metallic, inout vec3 diffuseColor, inout vec3 specularColor);
vec3 pbrEnergyCompensation(vec3 specularColor, float perceptualRoughness, float nv);
vec3 clampRadiance(vec3 c);
float unpackRoughness(vec2 p);
float calcLegacyDistanceAttenuation(float distance, float falloff);
vec4 getNorm(vec2 screenpos);
vec4 getPosition(vec2 pos_screen);
vec2 getScreenXY(vec4 clip);
vec2 getScreenCoord(vec4 clip);
vec3 srgb_to_linear(vec3 c);
float getDepth(vec2 tc);

void pbrPunctual(vec3 diffuseColor, vec3 specularColor,
                    float perceptualRoughness,
                    float metallic,
                    vec3 n, // normal
                    vec3 v, // surface point to camera
                    vec3 l, // surface point to light
                    out float nl,
                    out vec3 diff,
                    out vec3 spec);

GBufferInfo getGBuffer(vec2 screenpos);

void main()
{
    vec3 final_color = vec3(0);
    vec2 tc          = getScreenCoord(vary_fragcoord);
    vec3 pos         = getPosition(tc).xyz;
    vec3 lv = trans_center.xyz - pos;
    if (size <= 0.0 || dot(lv, lv) >= size * size)
    {
        discard;
    }
    GBufferInfo gb = getGBuffer(tc);

    vec3 n = gb.normal;
    if (dot(n, lv) <= 0.0)
    {
        discard;
    }

    vec3 diffuse = gb.albedo.rgb;
    vec4 spec    = gb.specular;

    // Common half vectors calcs
    vec3  h, l, v = -normalize(pos);
    float nh, nl, nv, vh, lightDist;
    calcHalfVectors(lv, n, v, h, l, nh, nl, nv, vh, lightDist);

    float dist = lightDist / size;
    float dist_atten = calcLegacyDistanceAttenuation(dist, falloff);

    if (GET_GBUFFER_FLAG(gb.gbufferFlag, GBUFFER_FLAG_HAS_PBR))
    {
        vec3 colorEmissive = gb.emissive.rgb;
        vec3 orm = spec.rgb;
        float perceptualRoughness = unpackRoughness(spec.ga);
        float metallic = orm.b;
        vec3 baseColor = diffuse.rgb;

        // The shared split, not a copy of it. Carrying an inlined duplicate is how the
        // deferred local lights came to disagree with the sun and IBL about a dielectric's
        // diffuse albedo -- same surface, different answer depending on what was lighting it.
        vec3 diffuseColor;
        vec3 specularColor;
        calcDiffuseSpecular(baseColor, metallic, diffuseColor, specularColor);

        vec3 intensity = dist_atten * color * PUNCTUAL_LIGHT_SCALE; // see deferredUtil.glsl -- must match every other site

        float nl = 0;
        vec3 diffPunc = vec3(0);
        vec3 specPunc = vec3(0);

        pbrPunctual(diffuseColor, specularColor, perceptualRoughness, metallic, n.xyz, v, normalize(lv), nl, diffPunc, specPunc);

        final_color += intensity* clampRadiance(nl * (diffPunc + specPunc));
    }
    else
    {
        if (dot(n, l) <= 0.0)
        {
            discard;
        }

        diffuse = srgb_to_linear(diffuse);
        spec.rgb = srgb_to_linear(spec.rgb);

        float lit = nl * dist_atten;

        final_color = color.rgb*lit*diffuse;

        if (spec.a > 0.0)
        {
            lit = min(nl*6.0, 1.0) * dist_atten;

            float sa = nh;
            float fres = pow(1 - vh, 5) * 0.4+0.5;
            float gtdenom = 2 * nh;
            float gt = max(0,(min(gtdenom * nv / vh, gtdenom * nl / vh)));

            if (nh > 0.0)
            {
                float scol = fres*blinnPhongLobe(nh, spec.a)*gt/(nh*max(nl, 1e-6));
                final_color += lit*scol*color.rgb*spec.rgb;
            }
        }

        // Bounded the same way the PBR branch above is. The specular term divides by two
        // cosines that calcHalfVectors only floors at 1e-6, and the Blinn-Phong LUT carries a
        // normalization of its own on top -- at grazing angles that product runs past what a
        // half-float target can hold, and an inf here spreads to the whole frame through bloom.
        // Colour-preserving, so a highlight that hits the ceiling dims rather than changing hue.
        final_color = clampRadiance(final_color);

        if (dot(final_color, final_color) <= 0.0)
        {
            discard;
        }
    }
    float final_scale = 1.0;
    if (classic_mode > 0)
        final_scale = 0.9;
    frag_color.rgb = max(final_color * final_scale, vec3(0));
    frag_color.a = 0.0;
}
