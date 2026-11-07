// Bloom: spread a smaller level back up onto the next larger one

/*[EXTRA_CODE_HERE]*/

// Drawn with additive blending onto the larger level, which still holds its own downsampled
// image, so every level's blur ends up summed into the top one.

out vec4 frag_color;

in vec2 vary_fragcoord;

uniform sampler2D diffuseRect;  // smaller level
uniform vec2 src_texel;         // one texel of diffuseRect

void main()
{
    vec2 tc = vary_fragcoord.xy;

    // A tent: four edge taps two texels out, four diagonal taps one texel out at double weight
    vec3 c = texture(diffuseRect, tc + vec2(-2.0,  0.0) * src_texel).rgb;
    c += texture(diffuseRect, tc + vec2( 2.0,  0.0) * src_texel).rgb;
    c += texture(diffuseRect, tc + vec2( 0.0, -2.0) * src_texel).rgb;
    c += texture(diffuseRect, tc + vec2( 0.0,  2.0) * src_texel).rgb;
    c += texture(diffuseRect, tc + vec2(-1.0, -1.0) * src_texel).rgb * 2.0;
    c += texture(diffuseRect, tc + vec2( 1.0, -1.0) * src_texel).rgb * 2.0;
    c += texture(diffuseRect, tc + vec2(-1.0,  1.0) * src_texel).rgb * 2.0;
    c += texture(diffuseRect, tc + vec2( 1.0,  1.0) * src_texel).rgb * 2.0;

    frag_color = vec4(c / 12.0, 1.0);
}
