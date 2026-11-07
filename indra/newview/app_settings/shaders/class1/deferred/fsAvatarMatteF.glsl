// Avatars only snapshot: coverage from the avatars rendered over a black and over a white background

/*[EXTRA_CODE_HERE]*/

// Both inputs are linear HDR, taken before exposure and tone mapping, where blending is linear:
// over a background B a pixel is colour + (1 - coverage) * B. Over black that leaves the
// premultiplied colour, and the white frame differs from it by (1 - coverage) * white.

out vec4 frag_color;

in vec2 vary_fragcoord;

uniform sampler2D diffuseRect;      // avatars over black
uniform sampler2D altDiffuseMap;    // avatars over white
uniform float bg_level;             // the white background's level
uniform int output_alpha;           // 1: write the coverage, 0: write the un-premultiplied colour

void main()
{
    vec2 tc = vary_fragcoord.xy;
    vec3 over_black = texture(diffuseRect, tc).rgb;
    vec3 over_white = texture(altDiffuseMap, tc).rgb;

    // How much of the background shows through, per channel. Tinted glass lets the channels
    // through unevenly, the average keeps its overall transparency.
    vec3 seen = clamp((over_white - over_black) / max(bg_level, 1e-4), 0.0, 1.0);
    float alpha = 1.0 - (seen.r + seen.g + seen.b) / 3.0;

    // Half float rounding leaves the bare background a hair off 0, and solid surfaces a hair off 1
    if (alpha < 1.0 / 512.0)
    {
        alpha = 0.0;
    }
    else if (alpha > 1.0 - 1.0 / 512.0)
    {
        alpha = 1.0;
    }

    if (output_alpha != 0)
    {
        frag_color = vec4(vec3(alpha), 1.0);
        return;
    }

    vec3 color = alpha > 0.0 ? over_black / alpha : vec3(0.0);
    frag_color = vec4(max(color, vec3(0.0)), 0.0);
}
