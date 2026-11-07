// Coloured sun shadows: a translucent caster's transmittance (FSRenderColoredShadows)

/*[EXTRA_CODE_HERE]*/

// Drawn into a sun cascade's colour map with multiplicative blending (ZERO, SRC_COLOR) over a
// white clear, depth tested against the cascade's opaque shadow depth without writing it. Each
// alpha-blended caster in a texel multiplies the sunlight reaching past it by
//     mix(white, colour * (1 - colored_shadow_absorption), alpha)
// so clear glass passes all light, stained glass tints it by its colour in proportion to its
// opacity, and the absorption setting lets an opaque-looking blended surface also darken it.
// Legacy prims use the indexed diffuse lookup and sRGB vertex colour (as alphaF.glsl does); with
// FS_PBR_CASTER, glTF materials use the base colour map and linear base colour factor (as
// pbralphaF.glsl does).

out vec4 frag_color;

in vec4 vertex_color;
in vec2 vary_texcoord0;

#if defined(FS_PBR_CASTER)
uniform sampler2D diffuseMap;
#else
vec4 diffuseLookup(vec2 texcoord);
#endif

uniform float colored_shadow_absorption;

vec3 fsCasterSrgbToLinear(vec3 cs)
{
    vec3 low_range = cs / vec3(12.92);
    vec3 high_range = pow((cs + vec3(0.055)) / vec3(1.055), vec3(2.4));
    bvec3 lte = lessThanEqual(cs, vec3(0.04045));
    return mix(high_range, low_range, vec3(lte));
}

void main()
{
#if defined(FS_PBR_CASTER)
    vec4 tex = texture(diffuseMap, vary_texcoord0.xy);
    float alpha = tex.a * vertex_color.a;
    vec3 color = fsCasterSrgbToLinear(tex.rgb) * vertex_color.rgb;
#else
    vec4 tex = diffuseLookup(vary_texcoord0.xy);
    float alpha = tex.a * vertex_color.a;
    vec3 color = fsCasterSrgbToLinear(clamp(tex.rgb * vertex_color.rgb, vec3(0.0), vec3(1.0)));
#endif

    alpha = clamp(alpha, 0.0, 1.0);
    if (alpha < 0.004)
    { // fully clear: multiplying by white changes nothing
        discard;
    }

    vec3 filtered = clamp(color, vec3(0.0), vec3(1.0)) * (1.0 - clamp(colored_shadow_absorption, 0.0, 1.0));
    frag_color = vec4(mix(vec3(1.0), filtered, alpha), 1.0);
}
