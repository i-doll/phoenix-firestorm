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

void main()
{
    ivec2 p = ivec2(gl_FragCoord.xy);
    ivec2 cell = p / id_march_block;
    ivec2 march_max = textureSize(idCloudScene, 0) - 1;
    ivec2 mc = min(cell, march_max);
    vec4 fresh = texelFetch(idCloudScene, mc, 0);
    vec2 fresh_d = texelFetch(idCloudDepth, mc, 0).rg;

    if (p - cell * id_march_block == id_march_offset)
    {
        frag_data[0] = fresh;
        frag_data[1] = vec4(fresh_d, 0.0, 1.0);
        return;
    }

    vec2 uv = (vec2(p) + 0.5) / id_march_grid;
    float depth = getDepth(uv);
    float scene_t = depth >= 1.0 ? ID_MAX_DIST : length(getPositionWithDepth(uv, depth).xyz);
    vec3 dir = normalize(getPositionWithDepth(uv, 0.5).xyz);

    // Reproject through the cloud seen in this block, else the cloud this pixel showed last frame.
    float d = fresh_d.r;
    if (d > ID_NO_CLOUD)
    {
        d = texelFetch(idCloudHistoryDepth, p, 0).r;
    }
    d = min(d, scene_t);

    vec4 clip = id_last_proj * (id_reproject * vec4(dir * d, 1.0));
    vec2 puv = clip.xy / max(clip.w, 1e-6) * 0.5 + 0.5;

    bool valid = clip.w > 0.0 && all(greaterThanEqual(puv, vec2(0.0))) && all(lessThanEqual(puv, vec2(1.0)));
    vec4 hist = fresh;
    vec2 hist_d = fresh_d;
    if (valid)
    {
        ivec2 hp = min(ivec2(puv * id_march_grid), ivec2(id_march_grid) - 1);
        hist_d = texelFetch(idCloudHistoryDepth, hp, 0).rg;
        // The march clips at the scene, so history marched against different geometry is stale.
        valid = abs(hist_d.g - scene_t) <= 0.05 * max(hist_d.g, scene_t) + 1.0;
        hist = texture(idCloudHistory, puv);
    }

    if (!valid)
    {
        frag_data[0] = fresh;
        frag_data[1] = vec4(fresh_d.r, scene_t, 0.0, 1.0);
        return;
    }

    // Under camera motion, clamp to this frame's neighbouring samples so stale history cannot linger.
    vec4 lo = fresh;
    vec4 hi = fresh;
    for (int y = -1; y <= 1; ++y)
    {
        for (int x = -1; x <= 1; ++x)
        {
            vec4 s = texelFetch(idCloudScene, clamp(mc + ivec2(x, y), ivec2(0), march_max), 0);
            lo = min(lo, s);
            hi = max(hi, s);
        }
    }
    float motion_px = length((puv - uv) * id_march_grid);
    hist = mix(hist, clamp(hist, lo, hi), clamp(motion_px * 0.5, 0.0, 1.0));

    frag_data[0] = hist;
    frag_data[1] = vec4(hist_d.r, scene_t, 0.0, 1.0);
}
