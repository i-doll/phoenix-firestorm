// Sun rays: filter the reduced resolution scattering up to full size and light it

/*[EXTRA_CODE_HERE]*/

// Drawn with additive blending over the HDR scene. A 3x3 filter over the reduced resolution
// rays, each tap weighted by how close its depth is to this pixel's, so shafts don't bleed
// across silhouettes and the per pixel step offsets average out.

out vec4 frag_color;

in vec2 vary_fragcoord;

uniform sampler2D diffuseRect;  // reduced resolution scattering
uniform vec2 ray_texel;         // one texel of diffuseRect, in texture coordinates
uniform vec3 ray_color;         // linear light colour times strength

float getDepth(vec2 pos_screen);
vec4 getPositionWithDepth(vec2 pos_screen, float depth);

float viewDistance(vec2 tc)
{
    float depth = getDepth(tc);
    return depth >= 1.0 ? 1.0e6 : length(getPositionWithDepth(tc, depth).xyz);
}

void main()
{
    vec2 tc = vary_fragcoord.xy;
    float center_dist = viewDistance(tc);

    float total = 0.0;
    float weight_sum = 0.0;
    for (int y = -1; y <= 1; ++y)
    {
        for (int x = -1; x <= 1; ++x)
        {
            vec2 offset = vec2(float(x), float(y)) * ray_texel;
            float tap_dist = viewDistance(tc + offset);
            // Relative depth difference, so the same tolerance works near and far
            float rel = abs(tap_dist - center_dist) / max(center_dist, 0.1);
            float w = exp(-rel * 8.0) * ((x == 0 && y == 0) ? 2.0 : 1.0);
            total += texture(diffuseRect, tc + offset).r * w;
            weight_sum += w;
        }
    }

    float scattered = total / max(weight_sum, 1e-4);
    frag_color = vec4(ray_color * scattered, 0.0);
}
