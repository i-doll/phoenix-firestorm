// Local tone mapping: the log2 luminance of the frame at a quarter of its size. Its mips are the
// large scale lighting the tonemap pass compresses.

/*[EXTRA_CODE_HERE]*/

out vec4 frag_color;

in vec2 vary_fragcoord;

uniform sampler2D diffuseRect;  // the linear HDR frame
uniform vec2 src_texel;         // one texel of diffuseRect

void main()
{
    vec2 tc = vary_fragcoord.xy;

    // Four bilinear taps cover the 4x4 source texels under this one
    vec3 c = texture(diffuseRect, tc + vec2(-1.0, -1.0) * src_texel).rgb;
    c += texture(diffuseRect, tc + vec2( 1.0, -1.0) * src_texel).rgb;
    c += texture(diffuseRect, tc + vec2(-1.0,  1.0) * src_texel).rgb;
    c += texture(diffuseRect, tc + vec2( 1.0,  1.0) * src_texel).rgb;

    float L = dot(max(c * 0.25, vec3(0.0)), vec3(0.2126, 0.7152, 0.0722));
    frag_color = vec4(log2(max(L, 1.0e-4)), 0.0, 0.0, 1.0);
}
