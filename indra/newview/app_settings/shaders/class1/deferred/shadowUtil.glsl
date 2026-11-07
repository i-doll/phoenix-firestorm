/**
 * @file class1/deferred/shadowUtil.glsl
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

uniform sampler2D   normalMap;

#if defined(SUN_SHADOW)
uniform sampler2DShadow shadowMap0;
uniform sampler2DShadow shadowMap1;
uniform sampler2DShadow shadowMap2;
uniform sampler2DShadow shadowMap3;
#endif

#if defined(SPOT_SHADOW)
uniform sampler2DShadow shadowMap4; // shadow map of the projector being drawn
#endif

uniform int cube_snapshot;

uniform vec3 sun_dir;
uniform vec3 moon_dir;
uniform vec2 shadow_res;
uniform vec2 proj_shadow_res;
uniform mat4 shadow_matrix[4];
uniform mat4 proj_shadow_matrix;
uniform vec4 shadow_clip;
uniform float shadow_bias;
uniform float shadow_offset;
uniform float spot_shadow_bias;
uniform float spot_shadow_offset;
uniform mat4 inv_proj;
uniform vec2 screen_res;
uniform int sun_up_factor;

// <ID:VolumetricClouds>
#ifdef ID_VOLUMETRIC_CLOUDS
float idSampleCloudShadow(vec3 pos_eye);
#endif
// </ID:VolumetricClouds>

// <ID:PCSS> Soft shadows whose edges widen with the distance between caster and receiver
uniform float sun_shadow_softness;   // penumbra width per meter of caster height, 0 keeps the fixed filter
uniform vec2 proj_shadow_softness;   // projector penumbra (uv) per unit of shadow map depth gap, 0 keeps the fixed filter
uniform float shadow_max_penumbra;   // widest penumbra, in shadow map texels

const int PCSS_SEARCH_TAPS = 8;
const int PCSS_FILTER_TAPS = 12;
const int PCSS_LEVELS = 2;

const vec2 pcss_disk[16] = vec2[16](
    vec2(-0.94201624, -0.39906216), vec2( 0.94558609, -0.76890725),
    vec2(-0.09418410, -0.92938870), vec2( 0.34495938,  0.29387760),
    vec2(-0.91588581,  0.45771432), vec2(-0.81544232, -0.87912464),
    vec2(-0.38277543,  0.27676845), vec2( 0.97484398,  0.75648379),
    vec2( 0.44323325, -0.97511554), vec2( 0.53742981, -0.47373420),
    vec2(-0.26496911, -0.41893023), vec2( 0.79197514,  0.19090188),
    vec2(-0.24188840,  0.99706507), vec2(-0.81409955,  0.91437590),
    vec2( 0.19984126,  0.78641367), vec2( 0.14383161, -0.14100790));

// uv_per_depth: penumbra width in uv per unit of shadow map depth between caster and receiver.
// The blocker search compares each tap at a few depths between the receiver and the light: how
// many of those come back shadowed measures how far in front of the receiver the caster is.
float pcssShadow(sampler2DShadow shadowMap, vec3 stc, vec2 uv_per_depth, vec2 texel, float base_texels)
{
    float a = 6.2831853 * fract(52.9829189 * fract(dot(gl_FragCoord.xy, vec2(0.06711056, 0.00583715))));
    mat2 rot = mat2(cos(a), sin(a), -sin(a), cos(a));

    vec2 max_uv = texel * shadow_max_penumbra;
    float range = min(stc.z, max(max_uv.x / uv_per_depth.x, max_uv.y / uv_per_depth.y));
    vec2 search = min(uv_per_depth * range, max_uv);

    float blocked = 0.0;
    float levels_shadowed = 0.0;
    for (int i = 0; i < PCSS_SEARCH_TAPS; i++)
    {
        vec2 uv = stc.xy + rot * pcss_disk[i] * search;
        float b = 1.0 - textureLod(shadowMap, vec3(uv, stc.z), 0.0);
        if (b > 0.0)
        {
            blocked += b;
            for (int k = 0; k < PCSS_LEVELS; k++)
            {
                float d = stc.z - range * (float(k) + 0.5) / float(PCSS_LEVELS);
                levels_shadowed += 1.0 - textureLod(shadowMap, vec3(uv, d), 0.0);
            }
        }
    }

    if (blocked <= 0.0)
    {
        return 1.0;
    }

    float gap = levels_shadowed / blocked * range / float(PCSS_LEVELS);
    vec2 penumbra = clamp(uv_per_depth * gap, texel * base_texels, max_uv);

    float lit = 0.0;
    for (int i = 0; i < PCSS_FILTER_TAPS; i++)
    {
        lit += textureLod(shadowMap, vec3(stc.xy + rot * pcss_disk[i] * penumbra, stc.z), 0.0);
    }
    return lit / float(PCSS_FILTER_TAPS);
}
// </ID:PCSS>

float pcfShadow(sampler2DShadow shadowMap, vec3 norm, vec4 stc, float bias_mul, vec2 pos_screen, vec3 light_dir, int cascade)
{
#if defined(SUN_SHADOW)
    float offset = shadow_bias * bias_mul;
    stc.xyz /= stc.w;

    // outside this cascade's map (e.g. a reused map after a fast camera turn) there is no
    // caster data, so treat it as lit instead of sampling the clamped border
    if (any(lessThan(stc.xy, vec2(0.0))) || any(greaterThan(stc.xy, vec2(1.0))))
    {
        return 1.0;
    }

    stc.z += offset * 2.0;
    if (cube_snapshot != 0)
    {
        return texture(shadowMap, stc.xyz);
    }

    vec2 res = vec2(textureSize(shadowMap, 0));

    // <ID:PCSS>
    if (sun_shadow_softness > 0.0)
    {
        mat4 m = shadow_matrix[cascade];
        vec2 uv_per_meter = vec2(length(vec3(m[0][0], m[1][0], m[2][0])), length(vec3(m[0][1], m[1][1], m[2][1])));
        float depth_per_meter = length(vec3(m[0][2], m[1][2], m[2][2]));
        return pcssShadow(shadowMap, stc.xyz, sun_shadow_softness * uv_per_meter / depth_per_meter, 1.0 / res, 1.5);
    }
    // </ID:PCSS>

    stc.x = floor(stc.x*res.x + fract(pos_screen.y*res.y))/res.x; // add some chaotic jitter to X sample pos according to Y to disguise the snapping going on here
    float cs = texture(shadowMap, stc.xyz);
    float shadow = cs * 4.0;
    shadow += texture(shadowMap, stc.xyz+vec3( 1.5/res.x,  0.5/res.y, 0.0));
    shadow += texture(shadowMap, stc.xyz+vec3( 0.5/res.x, -1.5/res.y, 0.0));
    shadow += texture(shadowMap, stc.xyz+vec3(-1.5/res.x, -0.5/res.y, 0.0));
    shadow += texture(shadowMap, stc.xyz+vec3(-0.5/res.x,  1.5/res.y, 0.0));
    return clamp(shadow * 0.125, 0.0, 1.0);
#else
    return 1.0;
#endif
}

float pcfSpotShadow(sampler2DShadow shadowMap, vec4 stc, float bias_scale, vec2 pos_screen)
{
#if defined(SPOT_SHADOW)
    stc.xyz /= stc.w;
    stc.z += spot_shadow_bias * bias_scale;
    if (cube_snapshot != 0)
    {
        return texture(shadowMap, stc.xyz);
    }

    // <ID:PCSS>
    if (proj_shadow_softness.y > 0.0)
    {
        return pcssShadow(shadowMap, stc.xyz, proj_shadow_softness, 1.0 / proj_shadow_res, 1.5);
    }
    // </ID:PCSS>

    stc.x = floor(proj_shadow_res.x * stc.x + fract(pos_screen.y*0.666666666)) / proj_shadow_res.x; // snap

    float cs = texture(shadowMap, stc.xyz);
    float shadow = cs;

    vec2 off = 1.0/proj_shadow_res;
    off.y *= 1.5;

    shadow += texture(shadowMap, stc.xyz+vec3(off.x*2.0, off.y, 0.0));
    shadow += texture(shadowMap, stc.xyz+vec3(off.x, -off.y, 0.0));
    shadow += texture(shadowMap, stc.xyz+vec3(-off.x, off.y, 0.0));
    shadow += texture(shadowMap, stc.xyz+vec3(-off.x*2.0, -off.y, 0.0));
    return shadow*0.2;
#else
    return 1.0;
#endif
}

float sampleDirectionalShadow(vec3 pos, vec3 norm, vec2 pos_screen)
{
#if defined(SUN_SHADOW)
    float shadow = 0.0f;
    vec3 light_dir = normalize((sun_up_factor == 1) ? sun_dir : moon_dir);

    float dp_directional_light = max(0.0, dot(norm.xyz, light_dir));
          dp_directional_light = clamp(dp_directional_light, 0.0, 1.0);

    vec3 shadow_pos = pos.xyz;

    vec3 offset = light_dir.xyz * (1.0 - dp_directional_light);

    shadow_pos += offset * shadow_offset * 2.0;

    vec4 spos = vec4(shadow_pos.xyz, 1.0);

    if (spos.z > -shadow_clip.w)
    {
        vec4 lpos;
        vec4 near_split = shadow_clip*-0.75;
        vec4 far_split = shadow_clip*-1.25;
        vec4 transition_domain = near_split-far_split;
        float weight = 0.0;

        if (spos.z < near_split.z)
        {
            lpos = shadow_matrix[3]*spos;

            float w = 1.0;
            w -= max(spos.z-far_split.z, 0.0)/transition_domain.z;
            //w = clamp(w, 0.0, 1.0);
            float contrib = pcfShadow(shadowMap3, norm, lpos, 1.0, pos_screen, light_dir, 3)*w;
            //if (contrib > 0)
            {
                shadow += contrib;
                weight += w;
            }
            shadow += max((pos.z+shadow_clip.z)/(shadow_clip.z-shadow_clip.w)*2.0-1.0, 0.0);
        }

        if (spos.z < near_split.y && spos.z > far_split.z)
        {
            lpos = shadow_matrix[2]*spos;

            float w = 1.0;
            w -= max(spos.z-far_split.y, 0.0)/transition_domain.y;
            w -= max(near_split.z-spos.z, 0.0)/transition_domain.z;
            //w = clamp(w, 0.0, 1.0);
            float contrib = pcfShadow(shadowMap2, norm, lpos, 1.0, pos_screen, light_dir, 2)*w;
            //if (contrib > 0)
            {
                shadow += contrib;
                weight += w;
            }
        }

        if (spos.z < near_split.x && spos.z > far_split.y)
        {
            lpos = shadow_matrix[1]*spos;

            float w = 1.0;
            w -= max(spos.z-far_split.x, 0.0)/transition_domain.x;
            w -= max(near_split.y-spos.z, 0.0)/transition_domain.y;
            //w = clamp(w, 0.0, 1.0);
            float contrib = pcfShadow(shadowMap1, norm, lpos, 1.0, pos_screen, light_dir, 1)*w;
            //if (contrib > 0)
            {
                shadow += contrib;
                weight += w;
            }
        }

        if (spos.z > far_split.x)
        {
            lpos = shadow_matrix[0]*spos;

            float w = 1.0;
            w -= max(near_split.x-spos.z, 0.0)/transition_domain.x;
            //w = clamp(w, 0.0, 1.0);
            float contrib = pcfShadow(shadowMap0, norm, lpos, 1.0, pos_screen, light_dir, 0)*w;
            //if (contrib > 0)
            {
                shadow += contrib;
                weight += w;
            }
        }

        shadow /= weight;
    }
    else
    {
        // <ID:VolumetricClouds>
#ifdef ID_VOLUMETRIC_CLOUDS
        return idSampleCloudShadow(pos); // lit beyond the far split...
#else
        return 1.0f; // lit beyond the far split...
#endif
        // </ID:VolumetricClouds>
    }
    //shadow = min(dp_directional_light,shadow);
    // <ID:VolumetricClouds>
#ifdef ID_VOLUMETRIC_CLOUDS
    return shadow * idSampleCloudShadow(pos);
#else
    return shadow;
#endif
    // </ID:VolumetricClouds>
#else
    return 1.0;
#endif
}

float sampleSpotShadow(vec3 pos, vec3 norm, vec2 pos_screen)
{
#if defined(SPOT_SHADOW)
    float shadow = 0.0f;
    pos += norm * spot_shadow_offset;

    vec4 spos = vec4(pos,1.0);
    if (spos.z > -shadow_clip.w)
    {
        vec4 lpos;

        vec4 near_split = shadow_clip*-0.75;
        vec4 far_split = shadow_clip*-1.25;
        vec4 transition_domain = near_split-far_split;
        float weight = 0.0;

        {
            float w = 1.0;
            w -= max(spos.z-far_split.z, 0.0)/transition_domain.z;

            lpos = proj_shadow_matrix*spos;
            shadow += pcfSpotShadow(shadowMap4, lpos, 0.8, spos.xy)*w;
            weight += w;
            shadow += max((pos.z+shadow_clip.z)/(shadow_clip.z-shadow_clip.w)*2.0-1.0, 0.0);
        }

        shadow /= weight;
    }
    else
    {
        shadow = 1.0f;
    }
    return shadow;
#else
    return 1.0;
#endif
}
