// Bloom: pick out the bright parts of the HDR scene at half resolution

/*[EXTRA_CODE_HERE]*/

out vec4 frag_color;

in vec2 vary_fragcoord;

uniform sampler2D diffuseRect;  // full resolution HDR scene, linear
uniform sampler2D exposureMap;
uniform vec2 src_texel;         // one texel of diffuseRect

uniform float exposure;
uniform float bloom_threshold;  // in exposed (on screen) brightness
uniform float bloom_knee;       // width of the soft transition below the threshold

float luminance(vec3 c)
{
    return dot(c, vec3(0.2126, 0.7152, 0.0722));
}

// What of this colour passes the threshold, easing in over the knee rather than switching on
vec3 brightPart(vec3 c)
{
    float brightness = max(c.r, max(c.g, c.b));
    float soft = clamp(brightness - bloom_threshold + bloom_knee, 0.0, 2.0 * bloom_knee);
    soft = soft * soft / (4.0 * bloom_knee + 1e-4);
    float contribution = max(soft, brightness - bloom_threshold) / max(brightness, 1e-4);
    return c * contribution;
}

vec3 tap(vec2 tc, float scale, inout float weight_sum)
{
    vec3 c = texture(diffuseRect, tc).rgb * scale;
    c = brightPart(c);
    // A single very bright pixel would otherwise turn into a flickering blob: weigh each tap
    // down by its own brightness, so the average leans toward the typical value
    float w = 1.0 / (1.0 + luminance(c));
    weight_sum += w;
    return c * w;
}

void main()
{
    vec2 tc = vary_fragcoord.xy;
    float scale = max(exposure * texture(exposureMap, vec2(0.5)).r, 1e-4);

    // Four bilinear taps cover the 4x4 source texels around this half resolution texel
    float weight_sum = 0.0;
    vec3 c = tap(tc + vec2(-1.0, -1.0) * src_texel, scale, weight_sum);
    c += tap(tc + vec2( 1.0, -1.0) * src_texel, scale, weight_sum);
    c += tap(tc + vec2(-1.0,  1.0) * src_texel, scale, weight_sum);
    c += tap(tc + vec2( 1.0,  1.0) * src_texel, scale, weight_sum);
    c /= max(weight_sum, 1e-4);

    // Back to scene units, it is added before exposure is applied
    frag_color = vec4(c / scale, 1.0);
}
