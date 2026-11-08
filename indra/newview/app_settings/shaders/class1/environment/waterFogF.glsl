/**
 * @file class1\environment\waterFogF.glsl
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



uniform vec4 waterPlane;
uniform vec4 waterFogColor;
uniform float waterFogDensity;
uniform float waterFogKS;

vec3 srgb_to_linear(vec3 col);
vec3 linear_to_srgb(vec3 col);

vec3 atmosFragLighting(vec3 light, vec3 additive, vec3 atten);

#ifdef FS_HEIGHT_FOG
vec3 fsApplyHeightFog(vec3 pos, vec3 color); // fsHeightFogF.glsl
#endif

// FS: water fog variation (FSRenderWaterFogVariation). Uploaded with the other water uniforms;
// left at zero, as in programs that don't get them, it changes nothing.
uniform vec4 fs_water_fog_noise;            // x variation 0 to 1, y patches per metre (info only), w noise period in cells
uniform vec4 fs_water_fog_vtw0;              // eye space to noise space, by rows: global position in patches
uniform vec4 fs_water_fog_vtw1;              // plus the drift, wrapped at the period on the CPU
uniform vec4 fs_water_fog_vtw2;

float fsWaterFogHash(vec3 p)
{
    p = fract(p * 0.1031);
    p += dot(p, p.zyx + 31.32);
    return fract((p.x + p.y) * p.z);
}

// Value noise in [0, 1], its lattice repeating every period cells so the CPU can wrap the offset
float fsWaterFogNoise(vec3 p, float period)
{
    vec3 i = floor(p);
    vec3 f = p - i;
    f = f * f * (3.0 - 2.0 * f);
    float n000 = fsWaterFogHash(mod(i, period));
    float n100 = fsWaterFogHash(mod(i + vec3(1.0, 0.0, 0.0), period));
    float n010 = fsWaterFogHash(mod(i + vec3(0.0, 1.0, 0.0), period));
    float n110 = fsWaterFogHash(mod(i + vec3(1.0, 1.0, 0.0), period));
    float n001 = fsWaterFogHash(mod(i + vec3(0.0, 0.0, 1.0), period));
    float n101 = fsWaterFogHash(mod(i + vec3(1.0, 0.0, 1.0), period));
    float n011 = fsWaterFogHash(mod(i + vec3(0.0, 1.0, 1.0), period));
    float n111 = fsWaterFogHash(mod(i + vec3(1.0, 1.0, 1.0), period));
    return mix(mix(mix(n000, n100, f.x), mix(n010, n110, f.x), f.y),
               mix(mix(n001, n101, f.x), mix(n011, n111, f.x), f.y), f.z);
}

// How much thicker or thinner the water between the eye-space points a and b looks: murkier and
// clearer patches fixed in the world (sampled at the surface point b, so a patch stays put as
// the camera moves), drifting slowly, plus a per-pixel dither of a percent so the fog's gradient
// never steps. 1 when off.
float fsWaterFogThickness(vec3 a, vec3 b)
{
    float variation = fs_water_fog_noise.x;
    if (variation <= 0.0)
    {
        return 1.0;
    }
    vec4 p = vec4(b, 1.0);
    vec3 w = vec3(dot(fs_water_fog_vtw0, p), dot(fs_water_fog_vtw1, p), dot(fs_water_fog_vtw2, p));
    float period = max(fs_water_fog_noise.w, 1.0);
    // the second octave's scale is a whole number, so its lattice repeats with the first
    float n = 0.65 * fsWaterFogNoise(w, period) + 0.35 * fsWaterFogNoise(w * 3.0 + 17.0, period);
    float dither = fract(52.9829189 * fract(dot(gl_FragCoord.xy, vec2(0.06711056, 0.00583715)))) - 0.5;
    return max(1.0 + variation * 1.2 * (n - 0.5) + 0.02 * dither, 0.05);
}

// get a water fog color that will apply the appropriate haze to a color given
// a blend function of (ONE, SOURCE_ALPHA)
vec4 getWaterFogViewNoClip(vec3 pos)
{
    vec3 view = normalize(pos);
    //normalize view vector
    float es = -(dot(view, waterPlane.xyz));

    //find intersection point with water plane and eye vector

    //get eye depth
    float e0 = max(-waterPlane.w, 0.0);

    vec3 int_v = waterPlane.w > 0.0 ? view * waterPlane.w/es : vec3(0.0, 0.0, 0.0);

    //get object depth
    float depth = length(pos - int_v);

    //get "thickness" of water
    float l = max(depth, 0.1);
    l *= fsWaterFogThickness(int_v, pos); // <FS:Trish/> water fog variation

    float kd = waterFogDensity;
    float ks = waterFogKS;
    vec4 kc = waterFogColor;

    float F = 0.98;

    float t1 = -kd * pow(F, ks * e0);
    float t2 = kd + ks * es;
    float t3 = pow(F, t2*l) - 1.0;

    float L = pow(min(t1/t2*t3, 1.0), 1.0/1.7);

    float D = pow(0.98, l*kd);

    return vec4(srgb_to_linear(kc.rgb)*L, D);
}

vec4 getWaterFogView(vec3 pos)
{
    if (dot(pos, waterPlane.xyz) + waterPlane.w > 0.0)
    {
        return vec4(0,0,0,1);
    }

    return getWaterFogViewNoClip(pos);
}

vec4 applyWaterFogView(vec3 pos, vec4 color)
{
    vec4 fogged = getWaterFogView(pos);

    color.rgb = color.rgb * fogged.a + fogged.rgb;

    return color;
}

vec4 applyWaterFogViewLinearNoClip(vec3 pos, vec4 color)
{
    vec4 fogged = getWaterFogViewNoClip(pos);
    color.rgb *= fogged.a;
    color.rgb += fogged.rgb;
    return color;
}

vec4 applyWaterFogViewLinear(vec3 pos, vec4 color)
{
    if (dot(pos, waterPlane.xyz) + waterPlane.w > 0.0)
    {
        return color;
    }

    return applyWaterFogViewLinearNoClip(pos, color);
}

// for post deferred shaders, apply sky and water fog in a way that is consistent with
// the deferred rendering haze post effects
vec4 applySkyAndWaterFog(vec3 pos, vec3 additive, vec3 atten, vec4 color)
{
    bool eye_above_water = dot(vec3(0), waterPlane.xyz) + waterPlane.w > 0.0;
    bool obj_above_water = dot(pos.xyz, waterPlane.xyz) + waterPlane.w > 0.0;

    if (eye_above_water)
    {
        if (!obj_above_water)
        {
            color.rgb = applyWaterFogViewLinearNoClip(pos, color).rgb;
        }
        else
        {
            color.rgb = atmosFragLighting(color.rgb, additive, atten);
#ifdef FS_HEIGHT_FOG
            color.rgb = fsApplyHeightFog(pos, color.rgb); // the haze pass's height fog, for alpha
#endif
        }
    }
    else
    {
        if (obj_above_water)
        {
            color.rgb = atmosFragLighting(color.rgb, additive, atten);
        }
        else
        {
            color.rgb = applyWaterFogViewLinearNoClip(pos, color).rgb;
        }
    }

    return color;
}
