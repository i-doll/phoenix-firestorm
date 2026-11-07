/**
 * @file class1/deferred/fsShadowExtrasF.glsl
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

// Extra terms of the deferred sun shadow, multiplied into the sun channel of the light map by
// sunLightF / sunLightSSAOF (fsSunShadowExtras). Built into the two deferred sun programs as one
// of their own files, so the program's permutation defines reach it:
//   FS_CLOUD_SHADOWS    the sky's cloud layer, projected along the light onto each pixel
//   FS_CONTACT_SHADOWS  a short depth buffer ray march towards the light (contact shadows)
// Neither runs in reflection probe captures (cube_snapshot).

uniform int cube_snapshot;
uniform vec3 sun_dir;   // view space
uniform vec3 moon_dir;  // view space
uniform int sun_up_factor;

// interleaved gradient noise (Jimenez 2014)
float fsExtrasNoise(vec2 frag)
{
    return fract(52.9829189 * fract(0.06711056 * frag.x + 0.00583715 * frag.y));
}

#if defined(FS_CLOUD_SHADOWS)
// The cloud layer of cloudsF.glsl, anchored to the world instead of the camera's sky dome: the
// dome's planar uv of a direction d is about ((d.east, -d.north) / 2) / cloud_scale, so a layer at
// height H gives each metre on it 1 / (2 H cloud_scale) of uv. Same noise textures, scroll,
// scale, coverage (cloud_shadow), densities and variance as the sky.
uniform sampler2D cloud_noise_texture;
uniform sampler2D cloud_noise_texture_next;

uniform mat4 fs_cloud_view_to_world;  // inverse modelview: view space to agent space (x east, y north, z up)
uniform vec4 fs_cloud_map;            // xy: uv1 under the camera (wrapped), z: uv per metre, w: layer height above the camera (m)
uniform vec3 fs_cloud_pos_density1;   // cloud_pos_density1 with the scroll, as the sky gets it
uniform vec3 fs_cloud_pos_density2;   // cloud_pos_density2
uniform vec4 fs_cloud_params;         // x: cloud_scale, y: cloud_variance, z: cloud_shadow (coverage), w: blend_factor
uniform vec2 fs_cloud_light_offset;   // lightnorm.xz * 0.0125: the sky's self shadow offset
uniform vec2 fs_cloud_shadow;         // x: strength 0-1 (0: off), y: softness 0-1

vec4 fsCloudNoise(vec2 uv, float bias)
{
    // blend_factor is uniform: endpoint skies need only one lookup
    if (fs_cloud_params.w <= 0.0)
    {
        return texture(cloud_noise_texture, uv, bias);
    }
    if (fs_cloud_params.w >= 1.0)
    {
        return texture(cloud_noise_texture_next, uv, bias);
    }
    return mix(texture(cloud_noise_texture, uv, bias), texture(cloud_noise_texture_next, uv, bias), fs_cloud_params.w);
}

// fraction (0..1) of the light the cloud layer lets through to view space position pos
float fsCloudShadow(vec3 pos, vec3 light_dir)
{
    float strength = fs_cloud_shadow.x;
    float cloud_scale = fs_cloud_params.x;
    if (strength <= 0.0 || cloud_scale < 0.001)
    {
        return 1.0;
    }

    mat3 rot = mat3(fs_cloud_view_to_world);
    vec3 p = rot * pos;                           // metres from the camera
    vec3 l = rot * light_dir;
    // a light near the horizon would smear the layer across the land: fade the shadows out
    float horizon = smoothstep(0.05, 0.25, l.z);
    if (horizon <= 0.0)
    {
        return 1.0;
    }

    // along the light to the layer
    vec2 q = p.xy + l.xy * ((fs_cloud_map.w - p.z) / max(l.z, 0.05));

    float softness = clamp(fs_cloud_shadow.y, 0.0, 1.0);
    float bias = softness * 3.0;                  // mip bias: softer cloud edges

    // the uv sets of cloudsV.glsl (before the scroll)
    vec2 uv1 = fs_cloud_map.xy + vec2(q.x, -q.y) * fs_cloud_map.z;
    vec2 uv2 = uv1 + fs_cloud_light_offset;
    vec2 uv3 = uv1 * 16.0;
    vec2 uv4 = uv2 * 16.0;

    // the density of cloudsF.glsl
    float cloud_variance = fs_cloud_params.y;
    float cloudDensity = 2.0 * (fs_cloud_params.z - 0.25);

    vec2 disturbance  = vec2(fsCloudNoise(uv1 / 8.0, bias).x, fsCloudNoise((uv3 + uv1) / 16.0, bias).x) * cloud_variance * (1.0 - cloud_scale * 0.25);
    vec2 disturbance2 = vec2(fsCloudNoise((uv1 + uv3) / 4.0, bias).x, fsCloudNoise((uv4 + uv2) / 8.0, bias).x) * cloud_variance * (1.0 - cloud_scale * 0.25);

    uv1 += fs_cloud_pos_density1.xy + (disturbance * 0.2);
    uv3 += fs_cloud_pos_density2.xy;

    float density_variance = min(1.0, (disturbance.x * 2.0 + disturbance.y * 2.0 + disturbance2.x + disturbance2.y) * 4.0);
    cloudDensity *= 1.0 - (density_variance * density_variance);

    float alpha1 = (fsCloudNoise(uv1, bias).x - 0.5) + (fsCloudNoise(uv3, bias).x - 0.5) * fs_cloud_pos_density2.z;
    // the sky's gain of 10 gives hard edged clouds; softness lowers it
    alpha1 = min(max(alpha1 + cloudDensity, 0.0) * mix(10.0, 3.0, softness) * fs_cloud_pos_density1.z, 1.0);
    alpha1 = 1.0 - alpha1 * alpha1;
    alpha1 = 1.0 - alpha1 * alpha1;
    alpha1 = clamp(alpha1, 0.0, 1.0);

    return 1.0 - clamp(strength, 0.0, 1.0) * horizon * alpha1;
}
#endif // FS_CLOUD_SHADOWS

#if defined(FS_CONTACT_SHADOWS)
// Screen-space contact shadows: a short march through the depth buffer from the pixel towards the
// light. Catches the small shadows the cascades are too coarse for (feet on the ground, folds,
// small props); multiplied into the cascaded shadow, so it can only darken.
uniform mat4 projection_matrix;
uniform vec2 fs_contact_shadow;      // x: ray length (m), y: strength 0-1 (0: off)

vec4 getPositionWithDepth(vec2 pos_screen, float depth);
float getDepth(vec2 pos_screen);
bool isFarDepth(float d);

#define FS_CONTACT_STEPS      14
#define FS_CONTACT_FADE_NEAR  24.0   // metres from the camera: full strength up to here
#define FS_CONTACT_FADE_FAR   48.0   // and none from here on
#define FS_CONTACT_EDGE       0.04   // screen fraction at the edges over which hits fade out
#define FS_CONTACT_THICKNESS  0.25   // metres: how deep a surface in the depth buffer is taken to be

float fsContactShadow(vec3 pos, vec3 norm, vec3 light_dir)
{
    float len = fs_contact_shadow.x;
    float dist = -pos.z;
    float strength = clamp(fs_contact_shadow.y, 0.0, 1.0) * (1.0 - smoothstep(FS_CONTACT_FADE_NEAR, FS_CONTACT_FADE_FAR, dist));
    if (strength <= 0.0 || len <= 0.0 || dot(norm, light_dir) <= 0.0)
    {
        return 1.0; // far away, off, or facing away from the light (already dark)
    }

    // depth precision falls with distance: leave the surface further and widen the hit test with it
    float bias = 0.01 + 0.002 * dist;
    float thickness = FS_CONTACT_THICKNESS + 0.01 * dist;
    float step_len = len / float(FS_CONTACT_STEPS);
    vec3 origin = pos + norm * bias;
    float t = step_len * fsExtrasNoise(gl_FragCoord.xy); // jittered start: banding becomes noise the blur removes

    for (int i = 0; i < FS_CONTACT_STEPS; ++i)
    {
        t += step_len;
        vec3 p = origin + light_dir * t;
        vec4 clip = projection_matrix * vec4(p, 1.0);
        if (clip.w <= 0.0)
        {
            break; // behind the camera
        }
        vec2 uv = clip.xy / clip.w * 0.5 + 0.5;
        if (any(lessThan(uv, vec2(0.0))) || any(greaterThan(uv, vec2(1.0))))
        {
            break; // left the screen
        }
        float d = getDepth(uv);
        if (isFarDepth(d))
        {
            continue;
        }
        // > 0: the surface the depth buffer has there is in front of the ray
        float delta = getPositionWithDepth(uv, d).z - p.z;
        if (delta > bias && delta < thickness)
        {
            vec2 edge = min(uv, vec2(1.0) - uv);
            return 1.0 - strength * smoothstep(0.0, FS_CONTACT_EDGE, min(edge.x, edge.y));
        }
    }
    return 1.0;
}
#endif // FS_CONTACT_SHADOWS

// sun shadow (light map red channel) of the pixel at view space position pos with normal norm
float fsSunShadowExtras(float shadow, vec3 pos, vec3 norm)
{
    if (cube_snapshot != 0)
    {
        return shadow; // probes keep the plain shadow: too rare for moving clouds, too coarse for contact
    }
    vec3 light_dir = normalize((sun_up_factor == 1) ? sun_dir : moon_dir);
#if defined(FS_CONTACT_SHADOWS)
    if (shadow > 0.0)
    {
        shadow *= fsContactShadow(pos, norm, light_dir);
    }
#endif
#if defined(FS_CLOUD_SHADOWS)
    if (shadow > 0.0)
    {
        shadow *= fsCloudShadow(pos, light_dir);
    }
#endif
    return shadow;
}
