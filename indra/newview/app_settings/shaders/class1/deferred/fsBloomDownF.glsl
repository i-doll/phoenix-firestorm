// Bloom: halve the resolution of the previous level, blurring as it goes

/*[EXTRA_CODE_HERE]*/

out vec4 frag_color;

in vec2 vary_fragcoord;

uniform sampler2D diffuseRect;  // previous, larger level
uniform vec2 src_texel;         // one texel of diffuseRect

void main()
{
    vec2 tc = vary_fragcoord.xy;

    // Centre plus four diagonal bilinear taps: a 5 tap kernel reaching about two source texels
    // each way, cheap enough to run on every level
    vec3 c = texture(diffuseRect, tc).rgb * 4.0;
    c += texture(diffuseRect, tc + vec2(-1.0, -1.0) * src_texel).rgb;
    c += texture(diffuseRect, tc + vec2( 1.0, -1.0) * src_texel).rgb;
    c += texture(diffuseRect, tc + vec2(-1.0,  1.0) * src_texel).rgb;
    c += texture(diffuseRect, tc + vec2( 1.0,  1.0) * src_texel).rgb;

    frag_color = vec4(c * 0.125, 1.0);
}
