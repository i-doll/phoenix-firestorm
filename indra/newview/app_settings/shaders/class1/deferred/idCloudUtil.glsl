/**
 * @file class1/deferred/idCloudUtil.glsl
 * @brief <ID> Volumetric clouds: cloud density field and cloud shadow lookup, shared by the cloud
 * shaders and every shader that samples sun shadows.
 *
 * Part of Five's custom Firestorm fork.
 */

uniform sampler3D idCloudNoiseBase;
uniform sampler3D idCloudNoiseDetail;
uniform sampler2D idCloudWeather;

uniform int   id_layer_count;
uniform float id_layer_base[4];
uniform float id_layer_top[4];
uniform float id_layer_coverage[4];
uniform float id_layer_density[4];
uniform float id_layer_detail[4];
uniform int   id_layer_type[4];
uniform vec2  id_layer_wind[4];
uniform vec2  id_layer_wind_dir[4];

const float ID_WEATHER_TILE_M = 40960.0;
const float ID_WEATHER_TEXEL_M = 80.0;
const float ID_SIGMA_MAX = 0.04;

float idRemap(float v, float lo, float hi, float new_lo, float new_hi)
{
    return new_lo + (v - lo) * (new_hi - new_lo) / max(hi - lo, 1e-5);
}

float idWeatherChannel(vec4 w, int layer)
{
    return layer == 0 ? w.r : (layer == 1 ? w.g : (layer == 2 ? w.b : w.a));
}

// x: base noise period in metres, y: detail noise period, z: base erosion strength, w: vertical squash.
// Periods divide ID_WEATHER_TILE_M so the field stays continuous where the camera position wraps.
vec4 idCloudTypeParams(int type)
{
    if (type == 1) return vec4(5120.0, 640.0, 1.0, 0.6);   // cumulonimbus
    if (type == 2) return vec4(5120.0, 640.0, 0.3, 2.0);   // stratus
    if (type == 3) return vec4(2560.0, 320.0, 0.7, 1.5);   // stratocumulus
    if (type == 4) return vec4(1280.0, 160.0, 0.9, 1.0);   // altocumulus
    if (type == 5) return vec4(5120.0, 640.0, 0.8, 4.0);   // cirrus
    return vec4(2560.0, 320.0, 1.0, 1.0);                   // cumulus
}

float idCloudProfile(int type, float h, float cov)
{
    if (type == 1)
    {
        return smoothstep(0.0, 0.04, h) * (1.0 - smoothstep(0.9, 1.0, h));
    }
    if (type == 2)
    {
        return smoothstep(0.0, 0.25, h) * (1.0 - smoothstep(0.6, 1.0, h));
    }
    if (type == 3)
    {
        float top = mix(0.5, 0.95, cov);
        return smoothstep(0.0, 0.12, h) * (1.0 - smoothstep(top * 0.6, top, h));
    }
    if (type == 4)
    {
        return smoothstep(0.0, 0.15, h) * (1.0 - smoothstep(0.5, 0.9, h));
    }
    if (type == 5)
    {
        return smoothstep(0.0, 0.35, h) * (1.0 - smoothstep(0.55, 1.0, h));
    }
    float top = mix(0.55, 1.0, cov);
    return smoothstep(0.0, 0.08, h) * (1.0 - smoothstep(top * 0.65, top, h));
}

// Normalised cloud density of one layer at world_pos (x, y: global metres wrapped to the weather
// tile, z: metres above sea level). footprint_m is the size of the sampled area, for mip selection.
// cheap skips the detail erosion, for light and shadow marches.
float idCloudDensity(vec3 world_pos, int layer, float footprint_m, bool cheap)
{
    float base = id_layer_base[layer];
    float top = id_layer_top[layer];
    float h = (world_pos.z - base) / max(top - base, 1.0);
    if (h <= 0.0 || h >= 1.0)
    {
        return 0.0;
    }

    int type = id_layer_type[layer];
    vec2 xy = world_pos.xy - id_layer_wind[layer];

    float weather_lod = log2(max(footprint_m / ID_WEATHER_TEXEL_M, 1.0));
    float w = idWeatherChannel(textureLod(idCloudWeather, xy / ID_WEATHER_TILE_M, weather_lod), layer);
    float c = id_layer_coverage[layer];
    float cov = smoothstep(0.0, 0.25, w + c - 1.0);
    cov = mix(cov, 1.0, smoothstep(0.85, 1.0, c));
    if (type == 1)
    {
        cov = pow(cov, mix(1.0, 0.35, smoothstep(0.65, 0.9, h)));
    }
    if (cov <= 0.0)
    {
        return 0.0;
    }

    float profile = idCloudProfile(type, h, cov);
    if (profile <= 0.0)
    {
        return 0.0;
    }

    vec4 tp = idCloudTypeParams(type);
    vec2 stretch = vec2(1.0);
    if (type == 5)
    {
        vec2 dir = id_layer_wind_dir[layer];
        stretch = abs(dir.x) >= abs(dir.y) ? vec2(0.25, 1.0) : vec2(1.0, 0.25);
    }
    vec2 nxy = xy * stretch;

    float base_lod = log2(max(footprint_m / (tp.x / 128.0), 1.0));
    vec4 n = textureLod(idCloudNoiseBase, vec3(nxy, world_pos.z * tp.w) / tp.x, base_lod);
    float worley = n.g * 0.625 + n.b * 0.25 + n.a * 0.125;
    float shape = mix(1.0, idRemap(n.r, worley - 1.0, 1.0, 0.0, 1.0), tp.z);

    float d = idRemap(shape * profile, 1.0 - cov, 1.0, 0.0, 1.0) * cov;
    if (d <= 0.0)
    {
        return 0.0;
    }

    if (!cheap)
    {
        float detail_lod = log2(max(footprint_m / (tp.y / 32.0), 1.0));
        vec3 dn = textureLod(idCloudNoiseDetail, vec3(nxy, world_pos.z) / tp.y, detail_lod).rgb;
        float detail = dn.r * 0.625 + dn.g * 0.25 + dn.b * 0.125;
        detail = mix(detail, 1.0 - detail, clamp(h * 4.0, 0.0, 1.0));
        d = idRemap(d, detail * 0.35 * id_layer_detail[layer], 1.0, 0.0, 1.0);
    }

    return max(d, 0.0) * id_layer_density[layer];
}

float idCloudDensityAll(vec3 world_pos, float footprint_m, bool cheap)
{
    float d = 0.0;
    int count = min(id_layer_count, 4);
    for (int i = 0; i < count; ++i)
    {
        d += idCloudDensity(world_pos, i, footprint_m, cheap);
    }
    return d;
}

float idCloudSigma(float density)
{
    return density * ID_SIGMA_MAX;
}

// ---- Cloud shadow ----

uniform sampler2D idCloudShadow;
uniform mat4 id_cloud_shadow_matrix;
uniform int id_clouds_active;

// id_cloud_shadow_matrix gives the shadow map uv in xy and, in z, the height within the cloud
// column (0 at the lowest base, 1 at the highest top). The map holds the whole column's
// transmittance; only the part above the point applies.
float idSampleCloudShadow(vec3 pos_eye)
{
    if (id_clouds_active == 0)
    {
        return 1.0;
    }
    vec3 s = (id_cloud_shadow_matrix * vec4(pos_eye, 1.0)).xyz;
    float above = clamp(1.0 - s.z, 0.0, 1.0);
    if (above <= 0.0)
    {
        return 1.0;
    }
    float trans = textureLod(idCloudShadow, s.xy, 0.0).r;
    float shadow = pow(clamp(trans, 0.0, 1.0), above);
    vec2 edge = abs(s.xy - 0.5);
    return mix(shadow, 1.0, smoothstep(0.45, 0.5, max(edge.x, edge.y)));
}
