/**
 * @file fsSkinSSSF.glsl
 *
 * Screen-space subsurface scattering for avatar skin (FSRenderSkinSSS).
 *
 * A separable, depth-aware blur in the style of Jimenez's separable SSS, run on avatar pixels
 * only, after the deferred sun and local lights and before alpha and tone mapping.
 *
 * What is blurred is the light, not the skin: each tap's lit colour is divided by its own
 * g-buffer albedo (linear, as the lighting used it), the resulting irradiance is blurred, and
 * the centre's albedo multiplies it back in (post-scatter texturing). Pores, freckles, makeup
 * and tattoos stay sharp; only the lighting diffuses.
 *
 * Specular highlights: the screen colour already includes them and there is no separate
 * specular buffer, so irradiance is clamped, by luminance, to a ceiling no diffuse surface can
 * exceed under the current sky (sun plus ambient) or exposure. A highlight's excess over that
 * ceiling never enters the blur, and the centre pixel keeps its own excess untouched: the blur
 * only replaces the clamped (diffuse) share of the centre, so highlights stay crisp and do not
 * smear into halos.
 *
 * Kernel: the d'Eon/Jimenez six-Gaussian skin diffusion profile, integrated per channel over
 * 15 importance-spaced taps (quadratic spacing out to 6 mm). Red reaches several millimetres,
 * green and blue stay under half a millimetre, which gives the soft red terminator and the
 * red-tinted shadow edges without softening the image. Widths are in millimetres on the
 * surface, scaled by sss_radius (1 = measured human skin), converted to pixels by distance and
 * projection.
 *
 * Which pixels: with the feature on, rigged (HAS_SKIN) and system avatar g-buffer writers bias
 * their g-buffer flag by FS_SKIN_FLAG_BIAS (see LLShaderMgr::loadShaderFile), which marks
 * "drawn as part of an avatar" without disturbing GET_GBUFFER_FLAG's 0.1 tolerance. Inside
 * that, a skin-tone weight on the albedo keeps the blur off most clothing.
 *
 * Translucency (sss_translucency > 0, vertical pass only): sunlight transmitted through thin
 * skin lit from behind (ears, fingers, nostrils, rims), after Jimenez et al. 2010, "Real-Time
 * Realistic Skin Translucency". The thickness is the distance along the light between the
 * receiver (pushed slightly inside the surface) and the nearest caster in the sun's shadow map,
 * found by bisecting comparison taps of the shadow map (cascades 0 and 1, where close-ups
 * are); it goes through the skin profile's transmittance (red survives furthest) times the sun
 * colour, the albedo and max(0.3 + dot(-N, L), 0).
 *
 * Pass 0 (horizontal): screen -> scratch. rgb = horizontally blurred clamped irradiance,
 *                      a = the centre's clamp factor (1 when nothing was clamped).
 * Pass 1 (vertical):   scratch -> screen, premultiplied blend (ONE, ONE_MINUS_SRC_ALPHA):
 *                      src = a * albedo * blurred, alpha = a * clamp factor, a = weight * strength,
 *                      which is lit + a * albedo * (blurred - clamped centre); the
 *                      translucency is added to src.
 */

/*[EXTRA_CODE_HERE]*/

out vec4 frag_color;

in vec2 vary_fragcoord;

uniform sampler2D diffuseRect;      // g-buffer albedo
uniform sampler2D altDiffuseMap;    // the lit screen (pass 0) or the scratch (pass 1)
uniform sampler2D exposureMap;

uniform vec2 screen_res;
uniform mat4 inv_proj;

uniform int sss_pass;               // 0 horizontal, 1 vertical
uniform float sss_strength;         // [0,1] how much of the scattered light replaces the lit skin
uniform float sss_radius;           // profile scale, 1 = measured human skin
uniform float sss_tint;             // 0: every channel scatters like red, 1: skin profile
uniform float sss_tone_match;       // 0: every avatar surface, 1: skin coloured albedo only
uniform float sss_max_px;           // cap on the kernel reach in pixels
uniform float exposure;             // RenderExposure, as the tone mapper uses it
uniform float sss_translucency;     // strength of the light transmitted through thin skin, 0 off

// Sun shadow (cascades 0 and 1 as raw depth), for the translucency thickness
uniform sampler2DShadow shadowMap0;
uniform sampler2DShadow shadowMap1;
uniform mat4 shadow_matrix[4];
uniform vec4 shadow_clip;
uniform vec3 sun_dir;
uniform vec3 moon_dir;

// Sky (LLEnvironment), for the diffuse ceiling
uniform vec3  sunlight_color;
uniform vec3  moonlight_color;
uniform int   sun_up_factor;
uniform vec3  ambient_color;
uniform vec3  blue_density;
uniform float haze_density;
uniform float cloud_shadow;
uniform float density_multiplier;
uniform float max_y;
uniform vec3  lightnorm;
uniform float sky_sunlight_scale;
uniform float sky_ambient_scale;
uniform int   classic_mode;

vec4 getNormRaw(vec2 screenpos);
vec4 getNorm(vec2 screenpos);
vec4 getPosition(vec2 pos_screen);
vec3 linear_to_srgb(vec3 cl);
vec3 srgb_to_linear(vec3 cs);

#define FS_SKIN_FLAG_BIAS 0.05
#define FS_SKIN_TAPS 7              // per side
#define FS_SKIN_RANGE_MM 6.0        // reach of the outermost tap at sss_radius 1
#define FS_SKIN_MIN_ALBEDO 0.02     // linear; darker albedo is floored before dividing by it
#define FS_SKIN_TRANS_SHRINK 0.002  // m: the receiver is pushed this far inside the surface
#define FS_SKIN_TRANS_MM 6.0        // measured thickness (mm) per profile mm at sss_radius 1

// d'Eon/Jimenez skin profile, integrated over each tap's span: rgb = per channel weight
// (centre + 2 x sides sums to 1 per channel), a = tap offset in mm at sss_radius 1
const vec4 fs_skin_kernel[FS_SKIN_TAPS + 1] = vec4[FS_SKIN_TAPS + 1](
    vec4(0.1859, 0.3495, 0.4366, 0.0000),
    vec4(0.1482, 0.2462, 0.2510, 0.1224),
    vec4(0.1061, 0.0704, 0.0296, 0.4898),
    vec4(0.0783, 0.0080, 0.0009, 1.1020),
    vec4(0.0471, 0.0006, 0.0001, 1.9592),
    vec4(0.0193, 0.0001, 0.0000, 3.0612),
    vec4(0.0059, 0.0000, 0.0000, 4.4082),
    vec4(0.0020, 0.0000, 0.0000, 6.0000));

float fsLum(vec3 c)
{
    return dot(c, vec3(0.2126, 0.7152, 0.0722));
}

// 0: not avatar, 1: legacy avatar surface, 2: PBR avatar surface
int fsSkinSurface(float flag)
{
    if (abs(flag - (GBUFFER_FLAG_HAS_ATMOS + FS_SKIN_FLAG_BIAS)) < 0.02)
    {
        return 1;
    }
    if (abs(flag - (GBUFFER_FLAG_HAS_PBR + FS_SKIN_FLAG_BIAS)) < 0.02)
    {
        return 2;
    }
    return 0;
}

// How much an albedo looks like skin, across the range of human tones: red leading, hue between
// red and yellow-orange, moderate saturation, not black. Smooth edges so the mask has no seams.
float fsSkinTone(vec3 c)
{
    float mx = max(c.r, max(c.g, c.b));
    float mn = min(c.r, min(c.g, c.b));
    float d = mx - mn;
    float s = d / max(mx, 1e-4);

    float h;    // degrees, -180..180 with red at 0
    if (d < 1e-4)
    {
        h = 180.0;
    }
    else if (mx == c.r)
    {
        h = 60.0 * (c.g - c.b) / d;
    }
    else if (mx == c.g)
    {
        h = 60.0 * ((c.b - c.r) / d + 2.0);
    }
    else
    {
        h = 60.0 * ((c.r - c.g) / d + 4.0);
        h = h > 180.0 ? h - 360.0 : h;
    }

    float wh = smoothstep(-25.0, -5.0, h) * (1.0 - smoothstep(45.0, 65.0, h));
    float ws = smoothstep(0.05, 0.14, s) * (1.0 - smoothstep(0.72, 0.88, s));
    float wv = smoothstep(0.04, 0.12, mx);
    return wh * ws * wv;
}

// Skin weight of a pixel in [0,1], or -1 when it is not an avatar surface. albedo: the linear
// diffuse albedo the lighting multiplied by (legacy albedo is stored sRGB, PBR linear), floored.
float fsSkinSample(vec2 tc, out vec3 albedo)
{
    albedo = vec3(1.0);
    int surface = fsSkinSurface(getNormRaw(tc).a);
    if (surface == 0)
    {
        return -1.0;
    }

    vec4 base = texture(diffuseRect, tc);
    vec3 srgb;
    float w = 1.0;
    if (surface == 2)
    {
        albedo = base.rgb;
        srgb = linear_to_srgb(base.rgb);
    }
    else
    {
        albedo = srgb_to_linear(base.rgb);
        srgb = base.rgb;
        w = 1.0 - clamp(base.a, 0.0, 1.0); // legacy alpha is the fullbright share
    }
    albedo = max(albedo, vec3(FS_SKIN_MIN_ALBEDO));
    return w * mix(1.0, fsSkinTone(srgb), clamp(sss_tone_match, 0.0, 1.0));
}

// Linear sun (or moon) colour at the ground, as calcAtmosphericVarsLinear gives the lighting
vec3 fsSkinSunlight()
{
    vec3 sun = (sun_up_factor == 1) ? sunlight_color : moonlight_color;
    vec3 light_atten = (blue_density + vec3(haze_density * 0.25)) * (density_multiplier * max_y);
    sun *= exp(-light_atten / max(1e-6, lightnorm.y));
    if (classic_mode < 1)
    {
        sun = srgb_to_linear(sun);
    }
    return sun * sky_sunlight_scale;
}

// Brightest irradiance (luminance) a diffuse surface can plausibly receive right now: full sun
// plus generous ambient, or what exposes to white, whichever is higher (the second covers
// scenes lit by local lights). Anything above it on skin is specular.
float fsSkinDiffuseCeiling()
{
    vec3 amb = ambient_color + (vec3(1.0) - ambient_color) * cloud_shadow * 0.5;
    amb = pow(max(amb, vec3(0.0)), vec3(0.9)) * 0.57;
    if (classic_mode < 1)
    {
        amb = srgb_to_linear(amb);
    }
    float sun_k = classic_mode < 1 ? 1.0 : 3.14159265; // classic PBR scales the sun by pi

    float env = fsLum(fsSkinSunlight()) * sun_k + 2.0 * fsLum(amb) * sky_ambient_scale;
    float expo = max(exposure * texture(exposureMap, vec2(0.5)).r, 1e-4);
    return 1.25 * max(env, 1.0 / expo);
}

// Irradiance clamped to the ceiling: luminance first (keeps the hue of the light), then a per
// channel cap so a floored albedo channel cannot blow up
vec3 fsSkinClamp(vec3 e, float ceiling, out float f)
{
    f = min(1.0, ceiling / max(fsLum(e), 1e-6));
    return min(e * f, vec3(3.0 * ceiling));
}

// Transmittance of the skin profile through s mm (Jimenez et al. 2010): the same six Gaussians,
// so red carries through far more skin than green and blue
vec3 fsSkinTransmittance(float s)
{
    float d = -s * s;
    return vec3(0.233, 0.455, 0.649) * exp(d / 0.0064) +
           vec3(0.100, 0.336, 0.344) * exp(d / 0.0484) +
           vec3(0.118, 0.198, 0.000) * exp(d / 0.187) +
           vec3(0.113, 0.007, 0.007) * exp(d / 0.567) +
           vec3(0.358, 0.004, 0.000) * exp(d / 1.99) +
           vec3(0.078, 0.000, 0.000) * exp(d / 7.41);
}

// Transmittance through the skin between p (view space) and the sun along l, from one shadow
// cascade: the depth difference to the nearest caster, over the map's depth per metre along the
// light at p (right for warped maps too). The shadow maps only answer "is this depth lit", so the
// caster depth is bisected with comparison taps, down to the thickness where the profile has
// faded out. Four taps half a texel apart average out the map's stair steps. Zero when p is
// outside the cascade.
const int FS_SKIN_TRANS_STEPS = 5;

vec3 fsSkinCascadeTransmittance(sampler2DShadow depth_map, mat4 m, vec3 p, vec3 l, float scale)
{
    vec4 sp = m * vec4(p, 1.0);
    vec3 c = sp.xyz / sp.w;
    if (any(lessThan(c.xy, vec2(0.0))) || any(greaterThan(c.xy, vec2(1.0))) || c.z > 1.0)
    {
        return vec3(0.0);
    }
    vec4 sl = m * vec4(p + l * 0.1, 1.0);
    float z_per_m = max((c.z - sl.z / sl.w) / 0.1, 1e-6);
    float max_mm = 40.0 / max(scale, 1e-6);
    float z_range = max_mm * 0.001 * z_per_m;

    vec2 texel = 0.5 / vec2(textureSize(depth_map, 0));
    vec3 t = vec3(0.0);
    for (int i = 0; i < 4; ++i)
    {
        vec2 uv = c.xy + vec2((i == 0 || i == 2) ? -1.0 : 1.0, i < 2 ? -1.0 : 1.0) * texel;
        float lo = c.z - z_range;
        float hi = c.z;
        if (texture(depth_map, vec3(uv, hi)) > 0.5)
        {
            lo = hi;
        }
        else if (texture(depth_map, vec3(uv, lo)) > 0.5)
        {
            for (int k = 0; k < FS_SKIN_TRANS_STEPS; ++k)
            {
                float mid = 0.5 * (lo + hi);
                if (texture(depth_map, vec3(uv, mid)) > 0.5)
                {
                    lo = mid;
                }
                else
                {
                    hi = mid;
                }
            }
            lo = 0.5 * (lo + hi);
        }
        float thickness_mm = max(c.z - lo, 0.0) / z_per_m * 1000.0;
        t += fsSkinTransmittance(thickness_mm * scale);
    }
    return t * 0.25;
}

// Sunlight transmitted through thin skin at this pixel, before albedo
vec3 fsSkinTranslucency(vec3 pos, vec3 n)
{
    vec3 l = normalize((sun_up_factor == 1) ? sun_dir : moon_dir);
    float wrap = max(0.3 + dot(-n, l), 0.0);
    if (wrap <= 0.0)
    {
        return vec3(0.0);
    }

    vec3 p = pos - n * FS_SKIN_TRANS_SHRINK;
    float z = -p.z;
    float scale = 1.0 / (FS_SKIN_TRANS_MM * max(sss_radius, 0.05));
    vec3 t = vec3(0.0);
    if (z < shadow_clip.x)
    {
        t = fsSkinCascadeTransmittance(shadowMap0, shadow_matrix[0], p, l, scale);
    }
    else if (z < shadow_clip.y)
    {
        t = fsSkinCascadeTransmittance(shadowMap1, shadow_matrix[1], p, l, scale);
        t *= 1.0 - smoothstep(0.8 * shadow_clip.y, shadow_clip.y, z);
    }
    t = mix(vec3(t.r), t, clamp(sss_tint, 0.0, 1.0));
    return t * fsSkinSunlight() * wrap;
}

void main()
{
    vec2 tc = vary_fragcoord.xy;

    vec3 albedo;
    float center_w = fsSkinSample(tc, albedo);
    if (center_w <= 0.0)
    {
        // Not avatar (-1) or not skin (0): nothing reads this pixel's blur
        if (sss_pass == 1)
        {
            discard;
        }
        frag_color = vec4(0.0, 0.0, 0.0, 1.0);
        return;
    }

    vec3 pos = getPosition(tc).xyz;
    float dist = max(-pos.z, 1e-3);

    vec3 trans = vec3(0.0);
    if (sss_pass == 1)
    {
        if (sss_translucency > 0.0)
        {
            trans = fsSkinTranslucency(pos, getNorm(tc).xyz) * albedo * (sss_translucency * center_w);
        }
        if (sss_strength <= 0.0)
        {
            // Translucency alone; the scratch was not written this frame
            frag_color = vec4(trans, 0.0);
            return;
        }
    }

    float ceiling = 0.0;
    float f_c = 1.0;
    vec3 center;
    if (sss_pass == 0)
    {
        ceiling = fsSkinDiffuseCeiling();
        center = fsSkinClamp(texture(altDiffuseMap, tc).rgb / albedo, ceiling, f_c);
    }
    else
    {
        vec4 c = texture(altDiffuseMap, tc);
        center = c.rgb;
        f_c = clamp(c.a, 1e-3, 1.0);
    }

    // Kernel reach in pixels: mm -> pixels at this distance (inv_proj[1][1] is 1 / proj[1][1])
    float px_per_mm = 0.5 * screen_res.y / (inv_proj[1][1] * dist) * 0.001;
    float reach_px = min(sss_radius * FS_SKIN_RANGE_MM * px_per_mm, sss_max_px);

    vec3 result = center;
    if (reach_px >= 1.0)
    {
        vec2 dir = (sss_pass == 0 ? vec2(1.0, 0.0) : vec2(0.0, 1.0)) / screen_res;
        float px_per_unit = reach_px / FS_SKIN_RANGE_MM;   // pixels per kernel mm
        // A tap more than about twice the reach away in depth is another surface
        float depth_reject = 1.0 / max(2.0 * (reach_px / px_per_mm) * 0.001, 1e-4);
        float tint = clamp(sss_tint, 0.0, 1.0);

        result = center * mix(vec3(fs_skin_kernel[0].r), fs_skin_kernel[0].rgb, tint);
        for (int i = 1; i <= FS_SKIN_TAPS; ++i)
        {
            vec3 k = mix(vec3(fs_skin_kernel[i].r), fs_skin_kernel[i].rgb, tint);
            float x = fs_skin_kernel[i].a * px_per_unit;
            for (int side = -1; side <= 1; side += 2)
            {
                vec2 stc = tc + dir * (x * float(side));
                vec3 s_albedo;
                float keep = clamp(fsSkinSample(stc, s_albedo), 0.0, 1.0);
                vec3 s = center;
                if (keep > 0.0)
                {
                    float dz = abs(max(-getPosition(stc).z, 1e-3) - dist);
                    keep *= 1.0 - clamp(dz * depth_reject, 0.0, 1.0);
                    vec3 c = texture(altDiffuseMap, stc).rgb;
                    if (sss_pass == 0)
                    {
                        float f;
                        c = fsSkinClamp(c / s_albedo, ceiling, f);
                    }
                    // Off the skin or across a depth step: stand in the centre, so nothing
                    // bleeds in from clothing or the background (Jimenez's follow-surface rule)
                    s = mix(center, c, keep);
                }
                result += s * k;
            }
        }
    }

    if (sss_pass == 0)
    {
        frag_color = vec4(result, f_c);
    }
    else
    {
        float a = clamp(center_w * sss_strength, 0.0, 1.0);
        frag_color = vec4(a * albedo * result + trans, a * f_c);
    }
}
