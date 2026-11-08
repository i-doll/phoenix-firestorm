/**
 * @file class1/deferred/fsHeightFogF.glsl
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

// Exponential height fog (FSRenderHeightFog). Compiled with the basic shaders and attached next to
// waterFogF.glsl only while the global define FS_HEIGHT_FOG is set. Used by the deferred haze pass
// (hazeF.glsl) for opaque pixels and the sky, and by applySkyAndWaterFog (waterFogF.glsl) for
// forward rendered alpha above the water.
//
// The air's density is fs_height_fog.x at the fog's base height and below it, and falls off as
// exp(-falloff * h) at h meters above it. Its integral along the straight line from the camera to
// what a pixel sees has a closed form, so the fog costs a handful of instructions per pixel.
// Heights come from waterPlane (view space, normalised), so they follow every camera the viewer
// renders from, reflection probes included, without uniforms of their own.
//
// The fog is lit like the sky's own haze (calcAtmosphericVars): the horizon colours lit by the
// ambient and the sun or moon light the atmosphere lets through, the haze share of it scattered
// toward the camera by a Henyey-Greenstein lobe around the light. Same conventions as the sun rays
// (fsSunRaysF.glsl): density per meter, anisotropy 0 for even scattering, falloff per meter.

uniform vec4 fs_height_fog;         // density per meter at the base height, height falloff per meter,
                                    // base height above the water (m), maximum opacity
uniform vec4 fs_height_fog_color;   // linear tint, then 1 to take the hue from the sky or 0 for the tint's own
uniform vec2 fs_height_fog_scatter; // anisotropy (0 even, toward 1 around the light), distance for the sky (m)

// Rooms around the camera (FSRenderHeightFogInteriors, fsfoginterior.cpp), main view only:
// distance along the ray where the fog starts (the room's radius while indoors), distance where
// the sheltered air around the camera ends, and the share of the ambient and of the sun or moon
// light that sheltered air does not get. All zero: the plain fog.
uniform vec4 fs_height_fog_interior;

uniform vec4 waterPlane;
uniform vec3 sun_dir;   // view space, toward the light
uniform vec3 moon_dir;

// The sky's settings (atmosphericsFuncs.glsl)
uniform vec3  lightnorm;
uniform vec3  sunlight_color;
uniform vec3  moonlight_color;
uniform int   sun_up_factor;
uniform vec3  ambient_color;
uniform vec3  blue_horizon;
uniform vec3  blue_density;
uniform float haze_horizon;
uniform float haze_density;
uniform float cloud_shadow;
uniform float density_multiplier;
uniform float max_y;
uniform float sky_hdr_scale;

vec3 srgb_to_linear(vec3 col);

// Integral over height of the density relative to the base: h below the base (even density
// there), (1 - exp(-k h)) / k above it.
float fsHeightFogPrimitive(float h, float k)
{
    if (h <= 0.0)
    {
        return h;
    }
    float x = k * h;
    return (x < 1e-4) ? h * (1.0 - 0.5 * x) : (1.0 - exp(-x)) / k;
}

// Opacity of the fog between heights h0 (camera) and h1 (end of the ray), dist meters apart.
// The relative density averaged along a straight ray is the average over height, the integral
// above divided by the rise. Rays that hardly rise take the density halfway along.
float fsHeightFogOpacity(float h0, float h1, float dist)
{
    float k = max(fs_height_fog.y, 0.0);
    float dh = h1 - h0;
    float mean_density;
    if (abs(dh) < 0.5)
    {
        mean_density = exp(-k * max(0.5 * (h0 + h1), 0.0));
    }
    else
    {
        mean_density = (fsHeightFogPrimitive(h1, k) - fsHeightFogPrimitive(h0, k)) / dh;
    }
    float optical_depth = fs_height_fog.x * dist * mean_density;
    return min(1.0 - exp(-optical_depth), fs_height_fog.w);
}

// Henyey-Greenstein, scaled so that even scattering is 1 (as in fsSunRaysF.glsl)
float fsHeightFogPhase(float cos_theta, float g)
{
    float g2 = g * g;
    float denom = 1.0 + g2 - 2.0 * g * cos_theta;
    // Capped: near g = 1 the peak grows without bound and blows out bloom
    return min((1.0 - g2) / (denom * sqrt(denom)), 16.0);
}

// Linear HDR colour of the light the fog scatters toward the camera along view_dir. amb_scale and
// sun_scale (0-1) dim the ambient and the sun or moon light, for air under a roof.
vec3 fsHeightFogColor(vec3 view_dir, float amb_scale, float sun_scale)
{
    // The sun or moon light through the atmosphere, as calcAtmosphericVars has it
    vec3 sunlight = (sun_up_factor == 1) ? sunlight_color : moonlight_color;
    vec3 light_atten = (blue_density + vec3(haze_density * 0.25)) * (density_multiplier * max_y);
    sunlight *= exp(-light_atten / max(1e-6, lightnorm.y));

    vec3 combined_haze = max(blue_density + vec3(haze_density), vec3(1e-6));
    vec3 blue_weight = blue_density / combined_haze;
    vec3 haze_weight = vec3(haze_density) / combined_haze;

    vec3 amb = ambient_color + (vec3(1.0) - ambient_color) * cloud_shadow * 0.5;
    vec3 cs = sunlight * (1.0 - cloud_shadow);
    amb *= clamp(amb_scale, 0.0, 1.0);
    cs *= clamp(sun_scale, 0.0, 1.0);

    vec3 light_dir = (sun_up_factor == 1) ? sun_dir : moon_dir;
    float g = clamp(fs_height_fog_scatter.x, 0.0, 0.95);
    float phase = fsHeightFogPhase(dot(view_dir, normalize(light_dir)), g);

    // The sky's haze colour with the sun's share scattered by the lobe, converted the way
    // atmosFragLighting converts the haze
    vec3 col = blue_horizon * blue_weight * (cs + amb) + haze_horizon * haze_weight * (cs * phase + amb);
    // Clamp after the conversion and by the peak channel, so a bright lobe keeps its hue.
    // 1174 is srgb_to_linear(20), the old per channel cap of 10 doubled.
    col = srgb_to_linear(col * 2.0);
    col *= min(1.0, 1174.0 / max(max(col.r, max(col.g, col.b)), 1e-6));
    col *= sky_hdr_scale;

    if (fs_height_fog_color.a > 0.5)
    {
        return col * fs_height_fog_color.rgb;
    }
    // The tint's own hue at the sky's brightness, so it still follows day and night
    return fs_height_fog_color.rgb * dot(col, vec3(0.2126, 0.7152, 0.0722));
}

// Fog toward a view space position: rgb is its colour, a its opacity. For the sky (sky true),
// pos only gives the direction and the ray runs fs_height_fog_scatter.y meters. None while the
// camera is under water, where the water fog takes over.
vec4 fsHeightFog(vec3 pos, bool sky)
{
    float eye_height = waterPlane.w; // signed distance from the camera, at the origin, to the water
    if (eye_height <= 0.0)
    {
        return vec4(0.0);
    }

    float len = length(pos);
    vec3 view_dir = pos / max(len, 1e-6);
    float dist = sky ? fs_height_fog_scatter.y : len;

    float h0 = eye_height - fs_height_fog.z;
    float rise = dot(view_dir, waterPlane.xyz);

    if (fs_height_fog_interior.y > 0.0)
    {
        // Indoors the fog starts at d0, so the room stays clear while windows and doors still
        // show the fog outside. Between d0 and d1 the air is under the roof: it gets only part
        // of the sky's and the sun's light. Beyond d1 it is lit as usual.
        float d0 = clamp(fs_height_fog_interior.x, 0.0, dist);
        float d1 = clamp(fs_height_fog_interior.y, d0, dist);
        float ha = h0 + rise * d0;
        float hb = h0 + rise * d1;
        float near_opacity = fsHeightFogOpacity(ha, hb, d1 - d0);
        float far_opacity = fsHeightFogOpacity(hb, h0 + rise * dist, dist - d1);

        vec3 near_color = fsHeightFogColor(view_dir, 1.0 - fs_height_fog_interior.z, 1.0 - fs_height_fog_interior.w);
        vec3 far_color = fsHeightFogColor(view_dir, 1.0, 1.0);

        // Near stretch over the far one, premultiplied, then held to the maximum opacity
        vec3 premul = near_color * near_opacity + far_color * far_opacity * (1.0 - near_opacity);
        float total = 1.0 - (1.0 - near_opacity) * (1.0 - far_opacity);
        float capped = min(total, fs_height_fog.w);
        if (capped <= 0.0)
        {
            return vec4(0.0);
        }
        return vec4(premul / total, capped);
    }

    float h1 = h0 + rise * dist;
    float opacity = fsHeightFogOpacity(h0, h1, dist);

    return vec4(fsHeightFogColor(view_dir, 1.0, 1.0), opacity);
}

// Forward rendered colour at view space position pos, seen through the fog
vec3 fsApplyHeightFog(vec3 pos, vec3 color)
{
    vec4 fog = fsHeightFog(pos, false);
    return mix(color, fog.rgb, fog.a);
}
