/**
 * @file class3/deferred/idCloudsReprojectF.glsl
 * @brief <ID> Volumetric clouds: half-resolution history, one pixel in each 4x2 block marched per frame.
 *
 * Part of Five's custom Firestorm fork.
 */

/*[EXTRA_CODE_HERE]*/

// Same layout as idCloudsF.glsl: [0] in-scatter and transmittance, [1] cloud distance and scene distance.
out vec4 frag_data[2];

in vec2 vary_fragcoord;

// This frame's sparse march, one texel per block.
uniform sampler2D idCloudScene;
uniform sampler2D idCloudDepth;
// Last frame's half-resolution result.
uniform sampler2D idCloudHistory;
uniform sampler2D idCloudHistoryDepth;

// Current eye space to last frame's eye space, and last frame's projection.
uniform mat4  id_reproject;
uniform mat4  id_last_proj;
uniform vec2  id_march_grid;
uniform ivec2 id_march_block;
uniform ivec2 id_march_offset;

float getDepth(vec2 pos_screen);
vec4 getPositionWithDepth(vec2 pos_screen, float depth);

const float ID_MAX_DIST = 160000.0;
const float ID_NO_CLOUD = 1e8;

// Whether two scene distances belong to the same surface.
bool idSameSurface(float a, float b)
{
    return abs(a - b) <= 0.05 * max(a, b) + 1.0;
}

float idSurfaceWeight(float a, float b)
{
    float k = 1.0 + 50.0 * abs(a - b) / max(b, 1.0);
    return 1.0 / (k * k);
}

void main()
{
    ivec2 p = ivec2(gl_FragCoord.xy);
    ivec2 cell = p / id_march_block;
    ivec2 march_max = textureSize(idCloudScene, 0) - 1;
    ivec2 mc = min(cell, march_max);

    if (p - cell * id_march_block == id_march_offset)
    {
        frag_data[0] = texelFetch(idCloudScene, mc, 0);
        frag_data[1] = vec4(texelFetch(idCloudDepth, mc, 0).rg, 0.0, 1.0);
        return;
    }

    vec2 uv = (vec2(p) + 0.5) / id_march_grid;
    float depth = getDepth(uv);
    vec3 dir = normalize(getPositionWithDepth(uv, 0.5).xyz);
    float scene_t = depth >= 1.0 ? ID_MAX_DIST : length(getPositionWithDepth(uv, depth).xyz);

    // This frame's samples around the block, restricted to those that saw the same surface, so
    // neither the fallback nor the clamp carries cloud across a depth edge.
    vec4 fresh = vec4(0.0, 0.0, 0.0, 1.0);
    float fresh_dist = 1e9;
    float best_err = 1e30;
    float best_g = ID_MAX_DIST;
    vec4 lo = vec4(1e30);
    vec4 hi = vec4(-1e30);
    bool matched = false;
    for (int y = -1; y <= 1; ++y)
    {
        for (int x = -1; x <= 1; ++x)
        {
            ivec2 c = clamp(mc + ivec2(x, y), ivec2(0), march_max);
            vec4 s = texelFetch(idCloudScene, c, 0);
            vec2 sd = texelFetch(idCloudDepth, c, 0).rg;
            float err = abs(sd.g - scene_t);
            if (err < best_err)
            {
                best_err = err;
                best_g = sd.g;
                fresh = s;
                fresh_dist = sd.r;
            }
            if (idSameSurface(sd.g, scene_t))
            {
                lo = min(lo, s);
                hi = max(hi, s);
                matched = true;
            }
        }
    }
    if (!idSameSurface(best_g, scene_t) && scene_t < fresh_dist)
    {
        // Nothing nearby saw this surface and the cloud they saw lies behind it.
        fresh = vec4(0.0, 0.0, 0.0, 1.0);
        fresh_dist = 1e9;
    }

    // Reproject through the cloud seen on this surface, else the cloud this pixel showed last frame.
    float d = fresh_dist;
    if (d > ID_NO_CLOUD)
    {
        d = texelFetch(idCloudHistoryDepth, p, 0).r;
    }
    d = min(d, scene_t);

    vec4 clip = id_last_proj * (id_reproject * vec4(dir * d, 1.0));
    vec2 puv = clip.xy / max(clip.w, 1e-6) * 0.5 + 0.5;
    bool valid = clip.w > 0.0 && all(greaterThanEqual(puv, vec2(0.0))) && all(lessThanEqual(puv, vec2(1.0)));

    vec4 hist = fresh;
    float hist_dist = fresh_dist;
    if (valid)
    {
        // The scene distance this pixel had from last frame's camera. The march clips at the
        // scene, so history texels marched against another surface are left out.
        float last_scene_t = depth >= 1.0 ? ID_MAX_DIST : length((id_reproject * vec4(dir * scene_t, 1.0)).xyz);

        ivec2 hist_max = textureSize(idCloudHistory, 0) - 1;
        vec2 f = puv * id_march_grid - 0.5;
        ivec2 base = ivec2(floor(f));
        vec2 w = f - vec2(base);
        vec4 sum = vec4(0.0);
        float w_sum = 0.0;
        float best = -1.0;
        valid = false;
        for (int j = 0; j < 2; ++j)
        {
            for (int i = 0; i < 2; ++i)
            {
                ivec2 c = clamp(base + ivec2(i, j), ivec2(0), hist_max);
                vec2 hd = texelFetch(idCloudHistoryDepth, c, 0).rg;
                if (!idSameSurface(hd.g, last_scene_t))
                {
                    continue;
                }
                float bilinear = (i == 0 ? 1.0 - w.x : w.x) * (j == 0 ? 1.0 - w.y : w.y);
                float wt = (bilinear + 1e-4) * idSurfaceWeight(hd.g, last_scene_t);
                sum += texelFetch(idCloudHistory, c, 0) * wt;
                w_sum += wt;
                if (wt > best)
                {
                    best = wt;
                    hist_dist = hd.r;
                }
                valid = true;
            }
        }
        if (valid)
        {
            hist = sum / w_sum;
        }
    }

    if (!valid)
    {
        frag_data[0] = fresh;
        frag_data[1] = vec4(fresh_dist, scene_t, 0.0, 1.0);
        return;
    }

    // Under camera motion, clamp to this frame's samples of the same surface so stale history cannot linger.
    if (matched)
    {
        float motion_px = length((puv - uv) * id_march_grid);
        hist = mix(hist, clamp(hist, lo, hi), clamp(motion_px * 0.5, 0.0, 1.0));
    }

    frag_data[0] = hist;
    frag_data[1] = vec4(hist_dist, scene_t, 0.0, 1.0);
}
