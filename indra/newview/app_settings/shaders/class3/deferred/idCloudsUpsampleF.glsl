/**
 * @file class3/deferred/idCloudsUpsampleF.glsl
 * @brief <ID> Volumetric clouds: depth-aware upsample of the half-resolution clouds to full resolution.
 *
 * Part of Five's custom Firestorm fork.
 */

/*[EXTRA_CODE_HERE]*/

// Same layout as idCloudsF.glsl writes at full resolution.
out vec4 frag_data[2];

in vec2 vary_fragcoord;

// Half resolution: [0] in-scatter and transmittance, [1] r cloud distance, g scene distance.
uniform sampler2D idCloudScene;
uniform sampler2D idCloudDepth;
// Half of the full-resolution size, so half-resolution texel centres line up with odd sizes too.
uniform vec2 id_march_grid;

float getDepth(vec2 pos_screen);
vec4 getPositionWithDepth(vec2 pos_screen, float depth);

const float ID_MAX_DIST = 160000.0;

void main()
{
    vec2 tc = vary_fragcoord.xy;
    float depth = getDepth(tc);
    float scene_t = depth >= 1.0 ? ID_MAX_DIST : length(getPositionWithDepth(tc, depth).xyz);

    ivec2 size = textureSize(idCloudScene, 0);
    vec2 f = tc * id_march_grid - 0.5;
    ivec2 base = ivec2(floor(f));
    vec2 w = f - vec2(base);

    vec4 sum = vec4(0.0);
    float w_sum = 0.0;
    float best = -1.0;
    float dist = 1e9;
    for (int j = 0; j < 2; ++j)
    {
        for (int i = 0; i < 2; ++i)
        {
            ivec2 c = clamp(base + ivec2(i, j), ivec2(0), size - 1);
            vec4 s = texelFetch(idCloudScene, c, 0);
            vec2 sd = texelFetch(idCloudDepth, c, 0).rg;
            float bilinear = (i == 0 ? 1.0 - w.x : w.x) * (j == 0 ? 1.0 - w.y : w.y);
            float rel = abs(sd.g - scene_t) / max(scene_t, 1.0);
            float k = 1.0 + rel * 50.0;
            float wt = (bilinear + 1e-4) / (k * k);
            sum += s * wt;
            w_sum += wt;
            if (wt > best)
            {
                best = wt;
                dist = sd.r;
            }
        }
    }

    frag_data[0] = sum / w_sum;
    frag_data[1] = vec4(dist, 0.0, 0.0, 1.0);
}
