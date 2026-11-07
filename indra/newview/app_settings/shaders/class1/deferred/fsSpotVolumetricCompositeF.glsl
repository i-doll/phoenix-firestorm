// Volumetric projector light: filter the reduced resolution scattering up to full size

/*[EXTRA_CODE_HERE]*/

// Drawn with additive blending over the HDR scene. The joint bilateral upsample of
// fsSunRaysCompositeF, on a colour: the 3x3 reduced resolution texels around this pixel, each
// weighted by how near it is and by how close the distance it was marched to (a) is to this
// pixel's own, so light cones don't bleed across silhouettes and the per pixel step offsets of
// the march average out.

out vec4 frag_color;

in vec2 vary_fragcoord;

uniform sampler2D diffuseRect;  // reduced resolution: rgb scattered light, a distance marched to
uniform float vol_strength;

// Distance stored for the sky, as in fsSpotVolumetricF
const float SKY_DIST = 65000.0;

float getDepth(vec2 pos_screen);
vec4 getPositionWithDepth(vec2 pos_screen, float depth);

void main()
{
    vec2 tc = vary_fragcoord.xy;
    float depth = getDepth(tc);
    float center_dist = depth >= 1.0 ? SKY_DIST : min(length(getPositionWithDepth(tc, depth).xyz), SKY_DIST);

    ivec2 size = textureSize(diffuseRect, 0);
    vec2 low_pos = tc * vec2(size) - 0.5;   // in reduced resolution texel centres
    ivec2 base = ivec2(floor(low_pos + 0.5));

    vec3 total = vec3(0.0);
    float weight_sum = 0.0;
    vec3 best_value = vec3(0.0);
    float best_rel = 1.0e20;
    for (int y = -1; y <= 1; ++y)
    {
        for (int x = -1; x <= 1; ++x)
        {
            ivec2 p = clamp(base + ivec2(x, y), ivec2(0), size - ivec2(1));
            vec4 tap = texelFetch(diffuseRect, p, 0);
            vec2 d = vec2(p) - low_pos;
            // Relative distance difference, so the same tolerance works near and far
            float rel = abs(tap.a - center_dist) / max(center_dist, 0.1);
            float w = exp(-0.5 * dot(d, d) - 10.0 * rel);
            total += tap.rgb * w;
            weight_sum += w;
            if (rel < best_rel)
            {
                best_rel = rel;
                best_value = tap.rgb;
            }
        }
    }

    // A thin full resolution detail that no reduced texel saw: take the closest match
    vec3 scattered = weight_sum > 1.0e-20 ? total / weight_sum : best_value;
    frag_color = vec4(max(scattered, vec3(0.0)) * vol_strength, 0.0);
}
