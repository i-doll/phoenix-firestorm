/**
 * @file class1/deferred/idCloudMarchUtil.glsl
 * @brief <ID> Volumetric clouds: lighting and haze helpers shared by the cloud march and the froxel passes.
 *
 * Part of Five's custom Firestorm fork.
 */

#ifdef ID_VOLUMETRIC_CLOUDS

uniform int   id_layer_count;
uniform float id_layer_base[4];
uniform float id_layer_top[4];
uniform float id_planet_radius;
uniform vec3  id_camera_global;
uniform int   id_light_steps;
uniform float sky_hdr_scale;
uniform int   classic_mode;
uniform int   id_fog_shadows;
uniform float id_shadow_range;

float idCloudDensityAll(vec3 world_pos, float footprint_m, bool cheap);
float idCloudSigma(float density);
vec3 idEyeFromRel(vec3 rel);
#if defined(HAS_SUN_SHADOW)
float sampleDirectionalShadow(vec3 pos, vec3 norm, vec2 pos_screen);
#endif
void calcAtmosphericVarsLinear(vec3 inPositionEye, vec3 norm, vec3 light_dir, out vec3 sunlit, out vec3 amblit, out vec3 additive, out vec3 atten);
vec3 srgb_to_linear(vec3 c);

// Altitude above sea level of a point given relative to the camera, on a planet whose centre is
// straight below the camera. Written to keep float precision with a 6360 km radius.
float idAltitude(vec3 rel)
{
    float a = id_planet_radius + id_camera_global.z + rel.z;
    return id_camera_global.z + rel.z + dot(rel.xy, rel.xy) / (length(vec3(rel.xy, a)) + a);
}

float idHenyeyGreenstein(float cos_theta, float g)
{
    float g2 = g * g;
    float denom = 1.0 + g2 - 2.0 * g * cos_theta;
    return (1.0 - g2) / (denom * sqrt(denom));
}

// A strong forward lobe gives the silver lining toward the sun; the broad lobe keeps the side away
// from the sun from going flat.
float idPhase(float cos_theta, float scale)
{
    return mix(idHenyeyGreenstein(cos_theta, 0.85 * scale), idHenyeyGreenstein(cos_theta, 0.2 * scale), 0.7);
}

float idLightOpticalDepth(vec3 rel, vec3 light_w, float footprint_m)
{
    // Step lengths double each step and always add up to 4000 m, so fewer steps reach as far and a
    // tall storm still shades its own base.
    int steps = clamp(id_light_steps, 1, 6);
    float first = 4000.0 / (exp2(float(steps)) - 1.0);
    float od = 0.0;
    float dist = 0.0;
    for (int j = 0; j < steps; ++j)
    {
        float len = first * exp2(float(j));
        vec3 p = rel + light_w * (dist + 0.5 * len);
        dist += len;
        vec3 wp = vec3(id_camera_global.xy + p.xy, idAltitude(p));
        od += idCloudSigma(idCloudDensityAll(wp, max(footprint_m, len), true)) * len;
    }
    return od;
}

// How much sky light reaches a point: cloud above it blocks most of the skylight, which is what
// darkens thick bases and the shadowed sides of heaps.
float idSkyVisibility(vec3 rel, float footprint_m)
{
    const float lens[3] = float[3](60.0, 180.0, 480.0);
    float od = 0.0;
    float dist = 0.0;
    for (int j = 0; j < 3; ++j)
    {
        vec3 p = rel + vec3(0.0, 0.0, dist + 0.5 * lens[j]);
        dist += lens[j];
        vec3 wp = vec3(id_camera_global.xy + p.xy, idAltitude(p));
        od += idCloudSigma(idCloudDensityAll(wp, max(footprint_m, lens[j]), true)) * lens[j];
    }
    return exp(-0.5 * od);
}

// The powder term darkens the soft edges of dense cloud seen away from the light. It is floored so
// optically thin cloud such as cirrus stays bright.
float idSunScattering(float od, float cos_theta, float sigma)
{
    float powder = mix(0.6 + 0.4 * (1.0 - exp(-2.0 * sigma * 400.0)), 1.0, clamp(0.5 + 0.5 * cos_theta, 0.0, 1.0));
    float a = 1.0;
    float b = 1.0;
    float c = 1.0;
    float sum = 0.0;
    // Multiple scattering: each octave carries light deeper with a weaker, broader lobe, which keeps
    // cloud interiors from going flat grey behind a bright rim.
    for (int k = 0; k < 4; ++k)
    {
        sum += a * idPhase(cos_theta, c) * exp(-od * b);
        a *= 0.5;
        b *= 0.35;
        c *= 0.5;
    }
    return sum * powder;
}

float idHeightFraction(float alt)
{
    int count = min(id_layer_count, 4);
    for (int i = 0; i < count; ++i)
    {
        float base = id_layer_base[i];
        float top = id_layer_top[i];
        if (alt > base && alt < top)
        {
            return (alt - base) / max(top - base, 1.0);
        }
    }
    return 0.5;
}

// The haze the deferred haze pass would put in front of a point at pos_eye.
void idHaze(vec3 pos_eye, vec3 light_eye, out vec3 add, out float att)
{
    vec3 sunlit;
    vec3 amblit;
    vec3 additive;
    vec3 atten;
    calcAtmosphericVarsLinear(pos_eye, vec3(0.0, 0.0, 1.0), light_eye, sunlit, amblit, additive, atten);
    add = srgb_to_linear(additive * 2.0) * sky_hdr_scale;
    att = atten.r;
}

// Sun and sky light reaching the clouds seen along dir_eye.
void idCloudLighting(vec3 dir_eye, vec3 up_eye, vec3 light_eye, out vec3 sunlit, out vec3 amblit)
{
    vec3 additive;
    vec3 atten;
    calcAtmosphericVarsLinear(dir_eye * 1000.0, up_eye, light_eye, sunlit, amblit, additive, atten);
    if (classic_mode > 0)
    {
        sunlit = srgb_to_linear(sunlit * 1.35);
        amblit = srgb_to_linear(amblit);
    }
}

// Light scattered toward the camera by cloud of extinction sigma at rel, ts metres along the view ray.
vec3 idCloudSampleLight(vec3 rel, float alt, float ts, float sigma, float footprint, float cos_theta, vec3 light_w, vec3 light_eye,
                        vec2 tc, vec3 sunlit, vec3 amblit)
{
    float od = idLightOpticalDepth(rel, light_w, footprint);
    vec3 sun = sunlit * idSunScattering(od, cos_theta, sigma);
#if defined(HAS_SUN_SHADOW)
    if (id_fog_shadows == 1 && ts < id_shadow_range)
    {
        float shadow = sampleDirectionalShadow(idEyeFromRel(rel), light_eye, tc);
        sun *= mix(shadow, 1.0, smoothstep(0.9 * id_shadow_range, id_shadow_range, ts));
    }
#endif
    float hf = idHeightFraction(alt);
    vec3 amb = amblit * mix(0.4, 1.0, hf) * mix(0.5, 1.0, idSkyVisibility(rel, footprint));
    // Sunlight bounced off the ground lifts the undersides out of a flat grey slab.
    amb += sunlit * max(light_w.z, 0.0) * 0.12 * (1.0 - hf);
    return sun + amb;
}

#else

// Some drivers reject an empty compilation unit.
void idCloudMarchUtilDisabled()
{
}

#endif
