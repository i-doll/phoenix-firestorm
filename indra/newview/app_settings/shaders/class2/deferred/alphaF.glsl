/**
 * @file alphaF.glsl
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

//class2/deferred/alphaF.glsl

/*[EXTRA_CODE_HERE]*/

#define INDEXED 1
#define NON_INDEXED 2
#define NON_INDEXED_NO_COLOR 3

layout(location = 0) out vec4 frag_color;
layout(location = 1) out vec4 frag_reveal;
uniform int uOITPass;
uniform int uPremultAlpha;
uniform int uOITUseMRT;

uniform mat3 env_mat;
uniform vec3 sun_dir;
uniform vec3 moon_dir;
uniform int classic_mode;

#ifdef USE_DIFFUSE_TEX
uniform sampler2D diffuseMap;
#endif

in vec3 vary_fragcoord;
in vec3 vary_position;
in vec2 vary_texcoord0;
in vec3 vary_norm;

#ifdef USE_VERTEX_COLOR
in vec4 vertex_color; //vertex color should be treated as sRGB
#endif

uniform float minimum_alpha;

uniform mat4 proj_mat;
uniform mat4 inv_proj;
uniform vec2 screen_res;
uniform int sun_up_factor;
uniform vec4 light_position[8];
uniform vec3 light_direction[8];
uniform vec4 light_attenuation[8];
uniform vec3 light_diffuse[8];
uniform vec2 light_deferred_attenuation[8];

void waterClip(vec3 pos);

vec3 srgb_to_linear(vec3 c);
vec3 linear_to_srgb(vec3 c);

vec4 applySkyAndWaterFog(vec3 pos, vec3 additive, vec3 atten, vec4 color);
void calcAtmosphericVarsLinear(vec3 inPositionEye, vec3 norm, vec3 light_dir, out vec3 sunlit, out vec3 amblit, out vec3 atten, out vec3 additive);

#ifdef HAS_SUN_SHADOW
float sampleDirectionalShadow(vec3 pos, vec3 norm, vec2 pos_screen);
#endif

float getAmbientClamp();

void mirrorClip(vec3 pos);

// <ID:VolumetricClouds>
#ifdef ID_VOLUMETRIC_CLOUDS
vec3 idApplyClouds(vec3 color, vec3 pos_eye, vec3 haze_add, float haze_atten);
vec3 atmosFragLightingLinear(vec3 light, vec3 additive, vec3 atten);
#endif
// </ID:VolumetricClouds>

void sampleReflectionProbesLegacy(inout vec3 ambenv, inout vec3 glossenv, inout vec3 legacyenv,
        vec2 tc, vec3 pos, vec3 norm, float glossiness, float envIntensity, bool transparent, vec3 amblit_linear);

// The shared legacy punctual model, not a copy of it -- the same function the deferred legacy
// branch is built from, so a blended surface is lit like the opaque one behind it. The
// diffuse-only entry point: these objects have no specular map or colour to give one.
vec3 calcLegacyPointLightDiffuse(vec3 diffuse,
                    vec3 n, vec3 p, vec3 v,
                    vec3 lp, vec3 ld, vec3 lightColor,
                    float lightSize, float falloff, float is_pointlight);

void main()
{
    mirrorClip(vary_position);

    vec2 frag = vary_fragcoord.xy/vary_fragcoord.z*0.5+0.5;

    vec4 pos = vec4(vary_position, 1.0);
#ifndef IS_AVATAR_SKIN
    // clip against water plane unless this is a legacy avatar skin
    waterClip(pos.xyz);
#endif
    // Interpolation across a triangle shortens a normal that was unit-length at each vertex.
    // Every sibling forward path normalizes here, and the deferred path always reads a unit
    // normal out of the GBuffer, so leaving it raw made this the one surface whose NdotL was
    // scaled by an accident of where it sat within its triangle.
    vec3 norm = normalize(vary_norm);

    float shadow = 1.0f;

#ifdef USE_DIFFUSE_TEX
    vec4 diffuse_tap = texture(diffuseMap,vary_texcoord0.xy);
#endif

#ifdef USE_INDEXED_TEX
    vec4 diffuse_tap = diffuseLookup(vary_texcoord0.xy);
#endif

    vec4 diffuse_srgb = diffuse_tap;

#ifdef FOR_IMPOSTOR
    vec4 color;
    color.rgb = diffuse_srgb.rgb;
    color.a = 1.0;

    float final_alpha = diffuse_srgb.a * vertex_color.a;
    diffuse_srgb.rgb *= vertex_color.rgb;

    // Insure we don't pollute depth with invis pixels in impostor rendering
    //
    if (final_alpha < minimum_alpha)
    {
        discard;
    }

    color.rgb = diffuse_srgb.rgb;
    color.a = final_alpha;

#else // FOR_IMPOSTOR

    vec4 diffuse_linear = vec4(srgb_to_linear(diffuse_srgb.rgb), diffuse_srgb.a);

    vec3 light_dir = (sun_up_factor == 1) ? sun_dir: moon_dir; // TODO -- factor out "sun_up_factor" and just send in the appropriate light vector

    float final_alpha = diffuse_linear.a;

#ifdef IS_AVATAR_SKIN
    if(final_alpha < minimum_alpha)
    {
        discard;
    }
#endif

#ifdef USE_VERTEX_COLOR
    final_alpha *= vertex_color.a;

    if (final_alpha < minimum_alpha)
    { // TODO: figure out how to get invisible faces out of
        // render batches without breaking glow
        discard;
    }

    diffuse_srgb.rgb *= vertex_color.rgb;
    diffuse_linear.rgb = srgb_to_linear(diffuse_srgb.rgb);
#endif // USE_VERTEX_COLOR

    // After the alpha tests rather than ahead of them, so a fragment they reject does not pay
    // for the PCF taps. The shadow maps carry no mips, so nothing here depends on derivatives.
#ifdef HAS_SUN_SHADOW
    // <FS:Trish> shadow only enters as min(clamp(N.L), shadow), which is 0 facing away from the light
    //shadow = sampleDirectionalShadow(pos.xyz, norm.xyz, frag);
    if (dot(norm.xyz, light_dir.xyz) > 0.0)
    {
        shadow = sampleDirectionalShadow(pos.xyz, norm.xyz, frag);
    }
    // </FS:Trish>
#endif

    vec3 sunlit;
    vec3 amblit;
    vec3 additive;
    vec3 atten;

    calcAtmosphericVarsLinear(pos.xyz, norm, light_dir, sunlit, amblit, additive, atten);
    if (classic_mode > 0)
        sunlit *= 1.35;
    vec3 sunlit_linear = sunlit;
    vec3 amblit_linear = amblit;

    vec3 irradiance = amblit;
    vec3 glossenv;
    vec3 legacyenv;
    sampleReflectionProbesLegacy(irradiance, glossenv, legacyenv, frag, pos.xyz, norm.xyz, 0.0, 0.0, true, amblit_linear);


    float da = dot(norm.xyz, light_dir.xyz);
          da = clamp(da, -1.0, 1.0);

    float final_da = da;
          final_da = clamp(final_da, 0.0f, 1.0f);

    vec4 color = vec4(0.0);

    color.a   = final_alpha;

    color.rgb = irradiance;
    if (classic_mode > 0)
    {
        final_da = pow(final_da,1.2);
        vec3 sun_contrib = vec3(min(final_da, shadow));

        color.rgb = srgb_to_linear(color.rgb * 0.9 + linear_to_srgb(sun_contrib) * sunlit_linear * 0.7);
        sunlit_linear = srgb_to_linear(sunlit_linear);
    }
    else
    {
        vec3 sun_contrib = min(final_da, shadow) * sunlit_linear;
        color.rgb += sun_contrib;
    }

    color.rgb *= diffuse_linear.rgb;

    vec4 light = vec4(0,0,0,0);

    vec3 npos = normalize(-pos.xyz);

// light_deferred_attenuation carries the size/falloff pair the deferred pass reads, as the PBR
// alpha path already takes them. light_attenuation.z stays: it is the is-omni flag.
   #define LIGHT_LOOP(i) light.rgb += calcLegacyPointLightDiffuse(diffuse_linear.rgb, norm, pos.xyz, npos, light_position[i].xyz, light_direction[i].xyz, light_diffuse[i].rgb, light_deferred_attenuation[i].x, light_deferred_attenuation[i].y, light_attenuation[i].z);

    LIGHT_LOOP(1)
    LIGHT_LOOP(2)
    LIGHT_LOOP(3)
    LIGHT_LOOP(4)
    LIGHT_LOOP(5)
    LIGHT_LOOP(6)
    LIGHT_LOOP(7)

    // sum local light contrib in linear colorspace
    color.rgb += light.rgb;

    color.rgb = applySkyAndWaterFog(pos.xyz, additive, atten, color).rgb;

#endif // #else // FOR_IMPOSTOR
    float final_scale = 1;
    if (classic_mode > 0)
        final_scale = 1.1;
#ifdef IS_HUD
    color.rgb = linear_to_srgb(color.rgb);
    final_scale = 1;
#endif

    color.rgb *= final_scale;
    vec4 out_color = max(color, vec4(0));

    // <ID:VolumetricClouds>
#if defined(ID_VOLUMETRIC_CLOUDS) && !defined(FOR_IMPOSTOR) && !defined(IS_HUD)
    out_color.rgb = idApplyClouds(out_color.rgb, pos.xyz, atmosFragLightingLinear(vec3(0.0), additive, atten), atten.r);
#endif
    // </ID:VolumetricClouds>

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
    else if (uOITPass == OIT_PASS_DEPTH_MASK)
    {
        // Depth-only pre-pass: fragments that are effectively opaque write
        // depth so weighted accumulation cannot blend surfaces behind them
        // through them. Color writes are masked off by the caller.
        if (out_color.a < OIT_DEPTH_MASK_ALPHA_MIN)
        {
            discard;
        }
        frag_color = vec4(0.0);
        frag_reveal = vec4(0.0);
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
