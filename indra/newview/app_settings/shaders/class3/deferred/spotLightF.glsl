/**
 * @file class3\deferred\spotLightF.glsl
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

uniform samplerCube environmentMap;
uniform mat4 proj_mat; //screen space to light space
uniform float proj_near; //near clip for projection
uniform vec3 proj_p; //plane projection is emitting from (in screen space)
uniform vec3 proj_n;
uniform float proj_focus; //distance from plane to begin blurring
uniform float proj_lod;  //(number of mips in proj map)
uniform float proj_range; //range between near clip and far clip plane of projection
uniform float proj_ambient_lod;
uniform float proj_ambiance;
uniform float near_clip;
uniform float far_clip;

uniform vec3 proj_origin; //origin of projection to be used for angular attenuation
uniform float sun_wash;
uniform int proj_shadow_idx;
uniform float shadow_fade;
uniform int classic_mode;

// Light params
#if defined(MULTI_SPOTLIGHT)
uniform vec3 center;
#else
in vec3 trans_center;
#endif
uniform float size;
uniform vec3 color;
uniform float falloff;

in vec4 vary_fragcoord;
uniform vec2 screen_res;

uniform mat4 inv_proj;

void calcHalfVectors(vec3 lv, vec3 n, vec3 v, out vec3 h, out vec3 l, out float nh, out float nl, out float nv, out float vh, out float lightDist);
float blinnPhongLobe(float nh, float glossiness);
void calcDiffuseSpecular(vec3 baseColor, float metallic, inout vec3 diffuseColor, inout vec3 specularColor);
vec3 pbrEnergyCompensation(vec3 specularColor, float perceptualRoughness, float nv);
vec3 clampRadiance(vec3 c);
float unpackRoughness(vec2 p);
float calcLegacyDistanceAttenuation(float distance, float falloff);
bool clipProjectedLightVars(vec3 center, vec3 pos, out float dist, out float l_dist, out vec3 lv, out vec4 proj_tc );
vec4 getNorm(vec2 screenpos);
vec3 getProjectedLightAmbiance(float amb_da, float attenuation, float lit, float nl, float noise, vec2 projected_uv);
vec3 getProjectedLightDiffuseColor(float light_distance, vec2 projected_uv );
vec2 getScreenCoord(vec4 clip);
vec3 srgb_to_linear(vec3 cs);
vec4 texture2DLodSpecular(vec2 tc, float lod);

vec4 getPosition(vec2 pos_screen);

const float M_PI = 3.14159265;

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
float sampleSpotShadow(vec3 pos, vec3 norm, vec2 pos_screen);

void main()
{
    vec3 final_color = vec3(0,0,0);
    vec2 tc          = getScreenCoord(vary_fragcoord);
    vec3 pos         = getPosition(tc).xyz;

    vec3 lv;
    vec4 proj_tc;
    float dist, l_dist;
    vec3 c;
#if defined(MULTI_SPOTLIGHT)
    c = center;
#else
    c = trans_center;
#endif

    if (clipProjectedLightVars(c, pos, dist, l_dist, lv, proj_tc))
    {
        discard;
    }

    GBufferInfo gb = getGBuffer(tc);

    vec3 n = gb.normal;

    float shadow = 1.0;

    if (proj_shadow_idx >= 0)
    {
        shadow = sampleSpotShadow(pos, n, tc);
        if (shadow_fade < -50.0) // IDRenderProjectorShadowDebug: red = shadowed, green = lit
        {
            frag_color = vec4(0.3*(1.0-shadow), 0.3*shadow, 0.0, 0.0);
            return;
        }
        shadow += shadow_fade;
        shadow = clamp(shadow, 0.0, 1.0);
    }
    else if (shadow_fade < -50.0) // IDRenderProjectorShadowDebug: blue = no shadow map
    {
        frag_color = vec4(0.0, 0.0, 0.3, 0.0);
        return;
    }

    float dist_atten = calcLegacyDistanceAttenuation(dist, falloff);
    if (dist_atten <= 0.0)
    {
        discard;
    }

    lv = proj_origin-pos.xyz;
    vec3 v = -normalize(pos);

    vec3 diffuse = gb.albedo.rgb;
    vec4 spec    = gb.specular;
    vec3 dlit    = vec3(0, 0, 0);
    vec3 slit    = vec3(0, 0, 0);

    vec3 amb_rgb = vec3(0);

    if (GET_GBUFFER_FLAG(gb.gbufferFlag, GBUFFER_FLAG_HAS_PBR))
    {
        vec3 orm = spec.rgb;
        float perceptualRoughness = unpackRoughness(spec.ga);
        float metallic = orm.b;
        vec3 baseColor = diffuse.rgb;

        vec3 diffuseColor;
        vec3 specularColor;
        calcDiffuseSpecular(baseColor, metallic, diffuseColor, specularColor);
        vec3 diffPunc = vec3(0);
        vec3 specPunc = vec3(0);
        float nl;

        // We need this additional test inside a light's frustum since a spotlight's ambiance can be applied
        if (proj_tc.x > 0.0 && proj_tc.x < 1.0
        &&  proj_tc.y > 0.0 && proj_tc.y < 1.0)
        {
            float lit = 0.0;
            float amb_da = 0.0;

            lv = normalize(lv);

            // pbrPunctual's own NdotL, so the ambiance below sees the same value either way. Only
            // a front-facing fragment uses the lobes, so the BRDF is evaluated inside the test.
            nl = clamp(dot(n.xyz, lv), 0.0, 1.0);

            if (dot(n.xyz, lv) > 0.0)
            {
                pbrPunctual(diffuseColor, specularColor, perceptualRoughness, metallic, n.xyz, v, lv, nl, diffPunc, specPunc);

                amb_da += (nl*0.5 + 0.5) * proj_ambiance;

                dlit = getProjectedLightDiffuseColor( l_dist, proj_tc.xy );

                vec3 intensity = dist_atten * dlit * PUNCTUAL_LIGHT_SCALE * shadow;

                final_color += intensity * clampRadiance(nl * (diffPunc + specPunc));
                lit = clamp(nl * dist_atten, 0.0, 1.0);
            }

            // Ambiance is scaled by proj_ambiance throughout, and most projectors leave it at zero:
            // skip the mipmapped cookie fetch then. The condition is uniform.
            if (proj_ambiance != 0.0)
            {
                amb_rgb = getProjectedLightAmbiance( amb_da, dist_atten, lit, nl, 1.0, proj_tc.xy ) * PUNCTUAL_LIGHT_SCALE * shadow;

                final_color += diffuseColor.rgb * amb_rgb;
            }
        }
    }
    else
    {
        // These legacy angular terms are independent of the PBR lighting above.
        vec3 h, l;
        float nh, nl, nv, vh, lightDist;
        calcHalfVectors(lv, n, v, h, l, nh, nl, nv, vh, lightDist);

        float envIntensity = gb.envIntensity;

        diffuse = srgb_to_linear(diffuse);
        spec.rgb = srgb_to_linear(spec.rgb);

        if (proj_tc.z > 0.0 &&
            proj_tc.x < 1.0 &&
            proj_tc.y < 1.0 &&
            proj_tc.x > 0.0 &&
            proj_tc.y > 0.0)
        {
            float amb_da = 0;
            float lit = 0.0;

            if (nl > 0.0)
            {
                lit = nl * dist_atten;

                dlit = getProjectedLightDiffuseColor( l_dist, proj_tc.xy );

                final_color = dlit*lit*diffuse*shadow;

                amb_da += (nl*0.5+0.5) * proj_ambiance;
            }

            // See the PBR branch: zero ambiance contributes exactly nothing here either (where lit
            // exceeds 1 and the clamp goes negative, nl > 0 and the facing factor is zero).
            if (proj_ambiance != 0.0)
            {
                amb_rgb = getProjectedLightAmbiance( amb_da, dist_atten, lit, nl, 1.0, proj_tc.xy ) * shadow;
                final_color += diffuse.rgb * amb_rgb * max(dot(-normalize(lv), n), 0.0);
            }
        }

        if (spec.a > 0.0)
        {
            dlit *= min(nl*6.0, 1.0) * dist_atten;

            float f = 1 - vh;
            float f2 = f * f;
            float fres = (f2 * f2 * f) * 0.4 + 0.5;

            float gtdenom = 2 * nh;
            float gt = max(0, min(gtdenom * nv / vh, gtdenom * nl / vh));

            if (nh > 0.0)
            {
                float scol = fres*blinnPhongLobe(nh, spec.a)*gt/(nh*max(nl, 1e-6));
                vec3 speccol = dlit*scol*spec.rgb*shadow;
                speccol = clamp(speccol, vec3(0), vec3(1));
                final_color += speccol;
            }
        }

        if (envIntensity > 0.0)
        {
            vec3 ref = reflect(normalize(pos), n);

            //project from point pos in direction ref to plane proj_p, proj_n
            vec3 pdelta = proj_p-pos;
            float ds = dot(ref, proj_n);

            if (ds < 0.0)
            {
                vec3 pfinal = pos + ref * dot(pdelta, proj_n)/ds;

                vec4 stc = (proj_mat * vec4(pfinal.xyz, 1.0));

                if (stc.z > 0.0)
                {
                    stc /= stc.w;

                    if (stc.x < 1.0 &&
                        stc.y < 1.0 &&
                        stc.x > 0.0 &&
                        stc.y > 0.0)
                    {
                        final_color += color.rgb * texture2DLodSpecular(stc.xy, (1 - spec.a) * (proj_lod * 0.6)).rgb * shadow * envIntensity;
                    }
                }
            }
        }
    }

    //not sure why, but this line prevents MATBUG-194
    final_color = max(final_color, vec3(0.0));
    float final_scale = 1.0;
    if (classic_mode > 0)
        final_scale = 0.9;
    //output linear
    frag_color.rgb = final_color * final_scale;
    frag_color.a = 0.0;
}
