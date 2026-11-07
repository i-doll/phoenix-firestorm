/**
 * @file class1/deferred/idCloudUtil.glsl
 * @brief <ID> Volumetric clouds: cloud density field and cloud shadow lookup, shared by the cloud
 * shaders and every shader that samples sun shadows.
 *
 * Part of Five's custom Firestorm fork.
 */

#ifdef ID_VOLUMETRIC_CLOUDS

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
uniform vec2  id_layer_streak[4];

const float ID_WEATHER_TILE_M = 40960.0;
const float ID_WEATHER_TEXEL_M = 80.0;
const float ID_SIGMA_MAX = 0.04;

float idRemap(float v, float lo, float hi, float new_lo, float new_hi)
{
    return new_lo + (v - lo) * (new_hi - new_lo) / max(hi - lo, 1e-5);
}

// Trilinear filtering has kinks at every texel plane. A field that displaces the cloud surface by
// hundreds of metres turns those kinks into visible creases, so it is read with smoothed weights.
vec4 idTextureSmooth3D(sampler3D tex, vec3 uvw, float dim)
{
    vec3 p = uvw * dim - 0.5;
    vec3 i = floor(p);
    vec3 f = p - i;
    f = f * f * (3.0 - 2.0 * f);
    return textureLod(tex, (i + f + 0.5) / dim, 0.0);
}

// The weather map's texels are 80 m; read up close with plain bilinear filtering, a steep
// coverage threshold turns the creases between texels into flat facets.
vec4 idWeatherSample(vec2 uv, float lod)
{
    if (lod >= 1.0)
    {
        return textureLod(idCloudWeather, uv, lod);
    }
    vec2 dim = vec2(textureSize(idCloudWeather, 0));
    vec2 p = uv * dim - 0.5;
    vec2 i = floor(p);
    vec2 f = p - i;
    f = f * f * (3.0 - 2.0 * f);
    return textureLod(idCloudWeather, (i + f + 0.5) / dim, 0.0);
}

// Cubic B-spline filtering from eight trilinear fetches. Unlike plain trilinear it has no creases
// at texel planes, and unlike smoothstep weights no flat spots at texel centres, so it can drive a
// displacement of a kilometre without leaving a grid on the surface.
vec4 idTextureBSpline3D(sampler3D tex, vec3 uvw, float lod)
{
    float dim = float(textureSize(tex, int(lod)).x);
    vec3 p = uvw * dim - 0.5;
    vec3 i = floor(p);
    vec3 f = p - i;
    vec3 f2 = f * f;
    vec3 f3 = f2 * f;
    vec3 w0 = (1.0 - 3.0 * f + 3.0 * f2 - f3) / 6.0;
    vec3 w1 = (4.0 - 6.0 * f2 + 3.0 * f3) / 6.0;
    vec3 w2 = (1.0 + 3.0 * f + 3.0 * f2 - 3.0 * f3) / 6.0;
    vec3 w3 = f3 / 6.0;
    vec3 g0 = w0 + w1;
    vec3 g1 = w2 + w3;
    vec3 c0 = (i - 0.5 + w1 / g0) / dim;
    vec3 c1 = (i + 1.5 + w3 / g1) / dim;
    vec4 x00 = mix(textureLod(tex, vec3(c1.x, c0.y, c0.z), lod), textureLod(tex, vec3(c0.x, c0.y, c0.z), lod), g0.x);
    vec4 x10 = mix(textureLod(tex, vec3(c1.x, c1.y, c0.z), lod), textureLod(tex, vec3(c0.x, c1.y, c0.z), lod), g0.x);
    vec4 x01 = mix(textureLod(tex, vec3(c1.x, c0.y, c1.z), lod), textureLod(tex, vec3(c0.x, c0.y, c1.z), lod), g0.x);
    vec4 x11 = mix(textureLod(tex, vec3(c1.x, c1.y, c1.z), lod), textureLod(tex, vec3(c0.x, c1.y, c1.z), lod), g0.x);
    return mix(mix(x10, x00, g0.y), mix(x11, x01, g0.y), g1.z);
}

float idWeatherChannel(vec4 w, int layer)
{
    return layer == 0 ? w.r : (layer == 1 ? w.g : (layer == 2 ? w.b : w.a));
}

// x: base noise period in metres, y: detail noise period, z: base erosion strength, w: vertical squash.
// Periods divide ID_WEATHER_TILE_M so the field stays continuous where the camera position wraps.
vec4 idCloudTypeParams(int type)
{
    if (type == 1) return vec4(5120.0, 640.0, 0.0, 1.0);   // cumulonimbus
    if (type == 2) return vec4(5120.0, 640.0, 0.0, 2.0);   // stratus
    if (type == 3) return vec4(1280.0, 320.0, 0.9, 1.5);   // stratocumulus
    if (type == 4) return vec4(1280.0, 160.0, 1.0, 2.0);   // altocumulus
    if (type == 5) return vec4(2560.0, 640.0, 0.0, 4.0);   // cirrus
    return vec4(1280.0, 320.0, 1.0, 1.0);                   // cumulus
}

// Extinction at a given density setting, so thin cloud types stay translucent.
float idCloudTypeExtinction(int type)
{
    if (type == 1) return 1.2;
    if (type == 2) return 0.6;
    if (type == 3) return 0.8;
    if (type == 4) return 1.4;
    if (type == 5) return 0.6;
    return 1.0;
}

// How far below the layer top a cell's dome peaks, from 0 (small cell) to 1 (large cell).
float idCloudCellTop(int type, float size)
{
    if (type == 1) return mix(0.5, 1.0, size);
    if (type == 3) return mix(0.4, 0.9, size);
    if (type == 4) return mix(0.4, 0.6, size);
    if (type == 0) return mix(0.5, 1.0, size);
    return 1.0;
}

// The part of the cell's squared radius used up at height fraction h. A cell holds cloud where
// its coverage x, about 1 - (r/R)^2, is above this, so a quadratic gives a dome.
float idCloudHeightCost(int type, float h, float top)
{
    float hh = h / top;
    if (type == 1)
    {
        // A broad mountain of cloud that narrows into a column of about half its base radius, which
        // carries on up to the anvil.
        // Towers stay nearly as broad as the base, so the anvil grows out of the storm instead of
        // sitting on a narrow stalk like a mushroom cap.
        float spread = 0.25 * smoothstep(0.55, 0.85, h);
        return max(0.3 * smoothstep(0.0, 0.6, hh) - spread, 0.08) + smoothstep(0.95, 1.0, h) + 0.6 * (1.0 - smoothstep(0.0, 0.03, h));
    }
    if (type == 2)
    {
        return 1.0 - smoothstep(0.0, 0.3, h) * (1.0 - smoothstep(0.5, 1.0, h));
    }
    if (type == 5)
    {
        return 1.0 - smoothstep(0.0, 0.4, h) * (1.0 - smoothstep(0.5, 0.9, h));
    }
    if (type == 0)
    {
        // Steep, bulging sides for most of the height, then a rounded crown.
        return pow(hh, 4.0) + 0.6 * (1.0 - smoothstep(0.0, 0.15, h));
    }
    return hh * hh + 0.6 * (1.0 - smoothstep(0.0, 0.15, h));
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
    bool feathery = type == 2 || type == 5;
    vec2 xy = world_pos.xy - id_layer_wind[layer];
    vec2 wuv = xy / ID_WEATHER_TILE_M;

    float weather_lod = log2(max(footprint_m / ID_WEATHER_TEXEL_M, 1.0));
    float c = id_layer_coverage[layer];
    float inv_c = 1.0 / max(c, 0.05);
    float overcast = smoothstep(0.85, 1.0, c);
    float x = clamp((idWeatherChannel(idWeatherSample(wuv, weather_lod), layer) + c - 1.0) * inv_c, 0.0, 1.0);
    x = mix(x, 1.0, overcast);
    // Heaped types look the map up at a position pushed around by 3D noise, so their edges can
    // bulge out past where the map itself ends. The blurred map tells whether cloud is near.
    bool heaped = type == 0 || type == 1 || type == 3 || type == 4;
    float xb = clamp((idWeatherChannel(textureLod(idCloudWeather, wuv, weather_lod + 3.0), layer) + c - 1.0) * inv_c, 0.0, 1.0);
    // Cumulus billows reach only a few hundred metres past the map's edge, so a blurred map can
    // still skip empty sky. Storm towers lean kilometres, too far for any blur to bound safely.
    float reach = (idWeatherChannel(textureLod(idCloudWeather, wuv, weather_lod + 3.0), layer) + c - 1.0) * inv_c;
    if ((type == 0 || type == 3 || type == 4) ? (reach <= -0.5 && overcast <= 0.0) : (type != 1 && x <= 0.0))
    {
        return 0.0;
    }

    // Stratus and cirrus noise is stretched along the streak direction. streak is an integer
    // lattice vector, so the rotated field still tiles with the weather map.
    vec4 tp = idCloudTypeParams(type);
    vec2 nxy = xy;
    vec2 dxy = xy;
    if (feathery)
    {
        vec2 st = id_layer_streak[layer];
        vec2 uv = vec2(dot(xy, st), dot(xy, vec2(-st.y, st.x)));
        if (type == 5)
        {
            // Bend the strands with a slow field that tiles with the weather map, so they curve
            // into hooks instead of running dead straight.
            // Bend the strands with a field that changes across them as well as along them, so
            // neighbouring fibres drift apart and together instead of sharing the same bends.
            float bend = textureLod(idCloudNoiseBase, vec3(uv.x / 20480.0, uv.y / 5120.0, 0.37), 1.0).r;
            uv.y += (bend - 0.5) * 3000.0;
        }
        float along = type == 5 ? 0.0625 : 0.5;
        nxy = uv * vec2(along, 1.0);
        dxy = uv * vec2(along, 1.0);
    }

    float base_lod = log2(max(footprint_m / (tp.x / 128.0), 1.0));
    vec4 n = textureLod(idCloudNoiseBase, vec3(nxy, world_pos.z * tp.w) / tp.x, base_lod);
    float worley = n.g * 0.625 + n.b * 0.25 + n.a * 0.125;

    float detail = 0.5;
    if (!cheap)
    {
        float detail_lod = log2(max(footprint_m / (tp.y / 32.0), 1.0));
        vec3 dn = textureLod(idCloudNoiseDetail, vec3(dxy, world_pos.z) / tp.y, detail_lod).rgb;
        detail = dn.r * 0.625 + dn.g * 0.25 + dn.b * 0.125;
    }

    if (feathery)
    {
        // Strands where the stretched noise peaks, frayed into fibres by the detail noise. Only a
        // full overcast fills the gaps between strands.
        float envelope = 1.0 - idCloudHeightCost(type, h, 1.0);
        if (type == 5)
        {
            // Cirrus: the weather bands only fade the strands in and out, so no band edge cuts them
            // into ovals. Inside, broad strands break into hair-fine fibres.
            float band = smoothstep(0.0, 0.6, x + 0.25 * c);
            // Ridges of the noise (where it crosses its middle value) are thin lines, so strands and
            // the fibres inside them stay hair-thin.
            float nr = base_lod < 1.0 ? idTextureSmooth3D(idCloudNoiseBase, vec3(nxy, world_pos.z * tp.w) / tp.x, 128.0).r : n.r;
            float strand = 1.0 - abs(nr * 2.0 - 1.0);
            float fibre = 1.0 - abs(detail * 2.0 - 1.0);
            // Only some ridges carry cloud: a second noise gathers them into separate bundles that
            // fade out along their length instead of running as continuous wood grain.
            float bundle = smoothstep(0.3, 0.55, n.g);
            float hair = strand * mix(1.0, fibre, 0.7 * id_layer_detail[layer]) * bundle;
            float lo = 0.6 - 0.35 * c;
            return smoothstep(lo, lo + 0.2, hair) * band * envelope * id_layer_density[layer] * idCloudTypeExtinction(type);
        }
        // Stratus: a blanket that thins in soft, mottled patches; the gaps open only where the
        // weather map and the noise are both low.
        float mottle = mix(worley, detail, 0.4 * id_layer_detail[layer]);
        float sheet = x + (mottle - 0.5) * 0.6 - (1.0 - c) * 0.8;
        float d = smoothstep(0.0, 0.5, sheet) * mix(0.5, 1.0, mottle) * envelope;
        return d * id_layer_density[layer] * idCloudTypeExtinction(type);
    }

    float hb = h;
    float hb_bias = 0.0;
    if (heaped)
    {
        // Billows: shift where the map is read by a fixed distance in metres, so lumps keep the
        // same size whether the map is steep or shallow there, and nudge the height too so tops
        // bubble rather than following one smooth dome.
        // Storm billows displace the surface by over a kilometre: smoothstep weights leave a grid of
        // dots and plain trilinear leaves bands, so storms use B-spline filtering.
        vec4 nw = type == 1 ? idTextureBSpline3D(idCloudNoiseBase, vec3(nxy, world_pos.z * tp.w) / tp.x, max(base_lod, 1.0))
                : (base_lod < 1.0 ? idTextureSmooth3D(idCloudNoiseBase, vec3(nxy, world_pos.z * tp.w) / tp.x, 128.0) : n);
        // Storm billows come from the smoother cellular channels: the Perlin octaves in r pit the
        // big towers with small holes.
        vec2 bw = type == 1 ? vec2(nw.g, nw.b) : vec2(nw.r, nw.g);
        vec2 warp = (bw - 0.5) * (type == 1 ? 1600.0 : (type == 3 ? 500.0 : (type == 4 ? 160.0 : 450.0)));
        if (type == 1)
        {
            // A tall storm also needs structure kilometres across: towers that lean, swell and
            // pinch with height instead of one straight column.
            vec4 big = textureLod(idCloudNoiseBase, vec3(xy, world_pos.z) / 10240.0, base_lod);
            vec4 mid = textureLod(idCloudNoiseBase, vec3(xy, world_pos.z) / 5120.0, base_lod);
            // The narrow upper column cannot take kilometre-scale leaning without breaking apart.
            float column = 1.0 - 0.75 * smoothstep(0.25, 0.55, h);
            warp = warp * mix(0.6, 1.0, column) + ((big.rg - 0.5) * 1500.0 + (mid.rg - 0.5) * 1400.0) * column;
            // Small: on the flat-topped coverage plateau of the column a larger bias shreds it.
            hb_bias = (big.b - 0.5) * 0.06;
            // The cap of the storm is smoother than the boiling towers below it, and the base is
            // flat: the billows fade in over the lowest part of the storm.
            float calm = mix(0.5, 1.0, smoothstep(0.0, 0.15, h)) * mix(1.0, 0.4, smoothstep(0.75, 0.95, h));
            warp *= calm;
            hb_bias *= calm;
            // The upper storm is blown downwind, so the anvil grows lopsided from the column; its
            // coverage comes from a blurrier map so it spreads as one canopy.
            float shear = smoothstep(0.6, 0.95, h);
            warp -= normalize(id_layer_streak[layer]) * 2000.0 * shear;
            // Separate turrets: the mid-scale noise raises some parts of the mountain above others.
            hb_bias += (mid.b - 0.5) * 0.15 * (1.0 - smoothstep(0.3, 0.6, h));
        }
        vec2 wuv_w = (xy + warp) / ID_WEATHER_TILE_M;
        float xw = clamp((idWeatherChannel(idWeatherSample(wuv_w, weather_lod), layer) + c - 1.0) * inv_c, 0.0, 1.0);
        x = mix(xw, 1.0, overcast);
        xb = clamp((idWeatherChannel(textureLod(idCloudWeather, wuv_w, weather_lod + 3.0), layer) + c - 1.0) * inv_c, 0.0, 1.0);
        if (type == 1)
        {
            x = mix(x, max(x, xb), smoothstep(0.6, 0.85, h));
        }
        // Heaps share one flat condensation level, so the height wobble fades out toward the base.
        hb = clamp(h + ((type == 1 ? nw.g : nw.a) - 0.5) * 0.25 * mix(0.35, 1.0, smoothstep(0.0, type == 1 ? 0.06 : 0.25, h)), 0.0, 1.0);
    }

    // Small cells fade out of a blurred map, so the blurred coverage tells how big the cell is.
    float size = smoothstep(0.0, 0.6, mix(xb, 1.0, overcast));

    // Billows of the low-frequency noise are as large as the dome itself, so the outline is a
    // cluster of rounded lumps rather than one smooth dome or wall.
    if (!heaped)
    {
        x += (worley - 0.5) * (type == 3 ? 0.4 : 0.8);
    }
    // Heaped types get a hard edge, so the dome does not wear a pale translucent shell.
    float edge = type == 1 ? 2.5 : (heaped ? 2.5 : 4.0);
    if (type == 3 || type == 4) edge = 3.0;
    float ch = clamp((x - idCloudHeightCost(type, hb, idCloudCellTop(type, size)) - hb_bias) * edge, 0.0, 1.0);
    if (ch <= 0.0)
    {
        return 0.0;
    }

    float shape = mix(1.0, idRemap(n.r, worley - 1.0, 1.0, 0.0, 1.0), tp.z);
    // Without erosion (storms) the remap below is a step from 0 to 1, a hard surface thinner than a
    // march step that slices into contour bands; storms ramp up with the coverage instead.
    float d = type == 1 ? ch : idRemap(shape, 1.0 - ch, 1.0, 0.0, 1.0);
    if (d <= 0.0)
    {
        return 0.0;
    }

    if (!cheap)
    {
        float wisp = mix(detail, 1.0 - detail, clamp(h * 4.0, 0.0, 1.0));
        d = idRemap(d, wisp * (type == 1 ? 0.0 : 0.45) * id_layer_detail[layer], 1.0, 0.0, 1.0);
    }

    return clamp(d, 0.0, 1.0) * id_layer_density[layer] * idCloudTypeExtinction(type);
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

// ---- Alpha composition ----

uniform sampler2D idCloudScene;
uniform sampler2D idCloudDepth;
uniform int id_cloud_scene_active;
uniform int id_haze_pending;

// rgb: in-scatter of the cloud mass in front of pos_eye, a: its transmittance. While the haze pass
// is still to run it will fog this pixel again, so the in-scatter is pre-compensated the way the
// cloud composite does it; haze_add and haze_atten are this fragment's haze colour and attenuation.
vec4 idCloudsInFront(vec3 pos_eye, vec3 haze_add, float haze_atten)
{
    if (id_cloud_scene_active == 0)
    {
        return vec4(0.0, 0.0, 0.0, 1.0);
    }
    vec4 cs = texelFetch(idCloudScene, ivec2(gl_FragCoord.xy), 0);
    float cd = texelFetch(idCloudDepth, ivec2(gl_FragCoord.xy), 0).r;
    float behind = smoothstep(cd - 50.0, cd + 50.0, length(pos_eye));
    vec3 S_f = cs.rgb * behind;  float T_f = mix(1.0, cs.a, behind);
    if (id_haze_pending != 0 && T_f < 1.0)
    {
        S_f = clamp((S_f - (1.0 - T_f) * haze_add) / max(haze_atten, 0.02), vec3(-11.2), vec3(11.2));
    }
    return vec4(S_f, T_f);
}

// The destination already holds the clouds over the opaque scene. Blended with the fragment's
// own alpha a, the result is S_f + T_f*(a*c + (1-a)*behind_scene). The result can be negative
// while the haze pass is pending.
vec3 idApplyClouds(vec3 color, vec3 pos_eye, vec3 haze_add, float haze_atten)
{
    if (id_cloud_scene_active == 0)
    {
        return color;
    }
    vec4 cloud = idCloudsInFront(pos_eye, haze_add, haze_atten);
    return color * cloud.a + cloud.rgb;
}

#else

// Some drivers reject an empty compilation unit.
void idCloudUtilDisabled()
{
}

#endif
