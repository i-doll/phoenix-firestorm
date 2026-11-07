/**
 * @file class3/deferred/idCloudsF.glsl
 * @brief <ID> Volumetric clouds: fullscreen ray march through the cloud layers.
 *
 * Part of Five's custom Firestorm fork.
 */

/*[EXTRA_CODE_HERE]*/

// frag_data[0]: rgb in-scattered light, already fogged by the haze in front of the cloud; a transmittance.
// frag_data[1]: r transmittance-weighted mean cloud distance in metres, 1e9 where there is no cloud.
out vec4 frag_data[2];

in vec2 vary_fragcoord;

// ---- Cloud density (shared with the cloud shadow map) ----

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

uniform float id_planet_radius;
uniform vec3  id_camera_global;

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

// Altitude above sea level of a point given relative to the camera, on a planet whose centre is
// straight below the camera. Written to keep float precision with a 6360 km radius.
float idAltitude(vec3 rel)
{
    float a = id_planet_radius + id_camera_global.z + rel.z;
    return id_camera_global.z + rel.z + dot(rel.xy, rel.xy) / (length(vec3(rel.xy, a)) + a);
}

// ---- Ray march ----

uniform mat4  id_eye_to_world;
uniform float id_pixel_angle;
uniform int   id_steps;
uniform int   id_samples;

uniform vec3  sun_dir;
uniform vec3  moon_dir;
uniform int   sun_up_factor;
uniform int   classic_mode;
uniform float sky_hdr_scale;

float getDepth(vec2 pos_screen);
vec4 getPositionWithDepth(vec2 pos_screen, float depth);
void calcAtmosphericVarsLinear(vec3 inPositionEye, vec3 norm, vec3 light_dir, out vec3 sunlit, out vec3 amblit, out vec3 additive, out vec3 atten);
vec3 srgb_to_linear(vec3 c);
vec3 clampHDRRange(vec3 color);

const float ID_MAX_DIST = 160000.0;
const float ID_FADE_START = 100000.0;
const int   ID_MAX_SEGMENTS = 8;

const vec2 ID_SS_OFFSETS[4] = vec2[4](vec2(-0.125, -0.375), vec2(0.375, -0.125), vec2(0.125, 0.375), vec2(-0.375, 0.125));

float idInterleavedGradientNoise(vec2 frag)
{
    return fract(52.9829189 * fract(dot(frag, vec2(0.06711056, 0.00583715))));
}

// Distances along the view ray (world direction with z component mu) to the sphere at altitude
// alt, sorted. The small root comes from c / q so it keeps its precision.
bool idShellHit(float alt, float mu, out float t0, out float t1)
{
    float cam = id_camera_global.z;
    float b = (id_planet_radius + cam) * mu;
    float c = (cam - alt) * (2.0 * id_planet_radius + cam + alt);
    float disc = b * b - c;
    t0 = 0.0;
    t1 = 0.0;
    if (disc < 0.0)
    {
        return false;
    }
    float s = sqrt(disc);
    float q = -(b + (b >= 0.0 ? s : -s));
    if (abs(q) < 1e-6)
    {
        return true;
    }
    float r0 = q;
    float r1 = c / q;
    t0 = min(r0, r1);
    t1 = max(r0, r1);
    return true;
}

void idAddSegment(float a, float b, float t_end, inout float seg0[ID_MAX_SEGMENTS], inout float seg1[ID_MAX_SEGMENTS], inout int n)
{
    a = max(a, 0.0);
    b = min(b, t_end);
    if (b > a && n < ID_MAX_SEGMENTS)
    {
        seg0[n] = a;
        seg1[n] = b;
        ++n;
    }
}

// The parts of the view ray inside any layer's shells, before t_end and the ground, sorted and merged.
int idRaySegments(vec3 dir, float t_end, out float seg0[ID_MAX_SEGMENTS], out float seg1[ID_MAX_SEGMENTS])
{
    for (int i = 0; i < ID_MAX_SEGMENTS; ++i)
    {
        seg0[i] = 0.0;
        seg1[i] = 0.0;
    }

    float g0;
    float g1;
    float ground_alt = min(0.0, id_camera_global.z - 1.0);
    if (idShellHit(ground_alt, dir.z, g0, g1) && g0 > 0.0)
    {
        t_end = min(t_end, g0);
    }

    int n = 0;
    int count = min(id_layer_count, 4);
    for (int i = 0; i < count; ++i)
    {
        float o0;
        float o1;
        if (!idShellHit(id_layer_top[i], dir.z, o0, o1) || o1 <= 0.0)
        {
            continue;
        }

        float i0;
        float i1;
        if (!idShellHit(id_layer_base[i], dir.z, i0, i1) || i1 <= 0.0)
        {
            idAddSegment(o0, o1, t_end, seg0, seg1, n);
        }
        else if (i0 <= 0.0)
        {
            idAddSegment(i1, o1, t_end, seg0, seg1, n);
        }
        else
        {
            idAddSegment(o0, i0, t_end, seg0, seg1, n);
            idAddSegment(i1, o1, t_end, seg0, seg1, n);
        }
    }

    for (int i = 1; i < n; ++i)
    {
        float a = seg0[i];
        float b = seg1[i];
        int j = i - 1;
        while (j >= 0 && seg0[j] > a)
        {
            seg0[j + 1] = seg0[j];
            seg1[j + 1] = seg1[j];
            --j;
        }
        seg0[j + 1] = a;
        seg1[j + 1] = b;
    }

    int m = 0;
    for (int i = 0; i < n; ++i)
    {
        if (m > 0 && seg0[i] <= seg1[m - 1])
        {
            seg1[m - 1] = max(seg1[m - 1], seg1[i]);
        }
        else
        {
            seg0[m] = seg0[i];
            seg1[m] = seg1[i];
            ++m;
        }
    }
    return m;
}

float idHenyeyGreenstein(float cos_theta, float g)
{
    float g2 = g * g;
    float denom = 1.0 + g2 - 2.0 * g * cos_theta;
    return (1.0 - g2) / (denom * sqrt(denom));
}

float idPhase(float cos_theta, float scale)
{
    return mix(idHenyeyGreenstein(cos_theta, 0.8 * scale), idHenyeyGreenstein(cos_theta, -0.3 * scale), 0.5);
}

float idLightOpticalDepth(vec3 rel, vec3 light_w, float footprint_m)
{
    float od = 0.0;
    float dist = 0.0;
    for (int j = 0; j < 6; ++j)
    {
        float len = 30.0 * exp2(float(j));
        vec3 p = rel + light_w * (dist + 0.5 * len);
        dist += len;
        vec3 wp = vec3(id_camera_global.xy + p.xy, idAltitude(p));
        od += idCloudDensityAll(wp, max(footprint_m, len), true) * ID_SIGMA_MAX * len;
    }
    return od;
}

// The powder term darkens thin cloud seen away from the light.
float idSunScattering(float od, float cos_theta, float sigma)
{
    float powder = mix(1.0 - exp(-2.0 * sigma * 50.0), 1.0, clamp(0.5 + 0.5 * cos_theta, 0.0, 1.0));
    float a = 1.0;
    float b = 1.0;
    float c = 1.0;
    float sum = 0.0;
    for (int k = 0; k < 3; ++k)
    {
        sum += a * idPhase(cos_theta, c) * exp(-od * b);
        a *= 0.5;
        b *= 0.5;
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

void idMarch(vec3 dir, float scene_t, int steps, float jitter, vec3 light_w, vec3 sunlit, vec3 amblit,
             out vec3 scatter, out float trans, out float w_sum, out float wd_sum)
{
    scatter = vec3(0.0);
    trans = 1.0;
    w_sum = 0.0;
    wd_sum = 0.0;

    float seg0[ID_MAX_SEGMENTS];
    float seg1[ID_MAX_SEGMENTS];
    int n = idRaySegments(dir, min(scene_t, ID_MAX_DIST), seg0, seg1);

    float total = 0.0;
    for (int i = 0; i < n; ++i)
    {
        total += seg1[i] - seg0[i];
    }
    if (total <= 0.0)
    {
        return;
    }

    float cos_theta = dot(dir, light_w);
    int seg = 0;
    float seg_start = 0.0;

    // Sample spacing grows quadratically with distance into the cloud, so near cloud is sampled finely.
    for (int i = 0; i < steps; ++i)
    {
        float ua = float(i) / float(steps);
        float ub = float(i + 1) / float(steps);
        float sa = total * ua * ua;
        float sb = total * ub * ub;
        float s = mix(sa, sb, jitter);
        float ds = sb - sa;

        while (seg < n - 1 && s > seg_start + (seg1[seg] - seg0[seg]))
        {
            seg_start += seg1[seg] - seg0[seg];
            ++seg;
        }
        float t = min(seg0[seg] + (s - seg_start), seg1[seg]);

        vec3 rel = dir * t;
        float alt = idAltitude(rel);
        vec3 wp = vec3(id_camera_global.xy + rel.xy, alt);
        float footprint = max(t * id_pixel_angle, 1.0);

        float dens = idCloudDensityAll(wp, footprint, false) * (1.0 - smoothstep(ID_FADE_START, ID_MAX_DIST, t));
        if (dens <= 0.0)
        {
            continue;
        }

        float sigma = dens * ID_SIGMA_MAX;
        float od = idLightOpticalDepth(rel, light_w, footprint);
        vec3 sun = sunlit * idSunScattering(od, cos_theta, sigma);
        vec3 amb = amblit * mix(0.4, 1.0, idHeightFraction(alt));

        float step_trans = exp(-sigma * ds);
        float absorbed = trans * (1.0 - step_trans);
        scatter += (sun + amb) * absorbed;
        w_sum += absorbed;
        wd_sum += absorbed * t;
        trans *= step_trans;

        if (trans < 0.005)
        {
            break;
        }
    }
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

void main()
{
    vec2 tc = vary_fragcoord.xy;
    vec2 px = vec2(dFdx(tc.x), dFdy(tc.y));
    float depth = getDepth(tc);

    mat3 eye_to_world = mat3(id_eye_to_world);
    vec3 light_eye = normalize((sun_up_factor == 1) ? sun_dir : moon_dir);
    vec3 light_w = normalize(eye_to_world * light_eye);
    vec3 up_eye = vec3(eye_to_world[0].z, eye_to_world[1].z, eye_to_world[2].z);
    vec3 center_dir = normalize(getPositionWithDepth(tc, 0.5).xyz);

    vec3 sunlit;
    vec3 amblit;
    vec3 additive;
    vec3 atten;
    calcAtmosphericVarsLinear(center_dir * 1000.0, up_eye, light_eye, sunlit, amblit, additive, atten);
    if (classic_mode > 0)
    {
        sunlit = srgb_to_linear(sunlit * 1.35);
        amblit = srgb_to_linear(amblit);
    }

    int samples = clamp(id_samples, 1, 4);
    int steps = clamp(id_steps, 1, 256);
    float jitter = idInterleavedGradientNoise(gl_FragCoord.xy);

    vec3 scatter = vec3(0.0);
    float trans = 0.0;
    float w_sum = 0.0;
    float wd_sum = 0.0;
    for (int k = 0; k < samples; ++k)
    {
        vec2 stc = samples > 1 ? tc + ID_SS_OFFSETS[k] * px : tc;
        vec3 dir_eye = normalize(getPositionWithDepth(stc, 0.5).xyz);
        float scene_t = depth >= 1.0 ? ID_MAX_DIST : length(getPositionWithDepth(stc, depth).xyz);

        vec3 s_scatter;
        float s_trans;
        float s_w;
        float s_wd;
        idMarch(normalize(eye_to_world * dir_eye), scene_t, steps, fract(jitter + 0.25 * float(k)),
                light_w, sunlit, amblit, s_scatter, s_trans, s_w, s_wd);
        scatter += s_scatter;
        trans += s_trans;
        w_sum += s_w;
        wd_sum += s_wd;
    }
    scatter /= float(samples);
    trans /= float(samples);

    float cloud_dist = 1e9;
    if (w_sum > 1e-5)
    {
        cloud_dist = wd_sum / w_sum;
        vec3 haze_add;
        float haze_att;
        idHaze(center_dir * cloud_dist, light_eye, haze_add, haze_att);
        scatter = scatter * haze_att + (1.0 - trans) * haze_add;
    }

    frag_data[0] = vec4(clampHDRRange(scatter), clamp(trans, 0.0, 1.0));
    frag_data[1] = vec4(cloud_dist, 0.0, 0.0, 1.0);
}
