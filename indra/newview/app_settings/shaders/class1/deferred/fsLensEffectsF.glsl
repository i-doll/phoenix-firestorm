// Lens effects on the finished image: chromatic aberration, lens flare and film grain

/*[EXTRA_CODE_HERE]*/

// Runs on the display referred image, after tone mapping and anti-aliasing.

out vec4 frag_color;

in vec2 vary_fragcoord;

uniform sampler2D diffuseRect;
uniform sampler2D depthMap;
uniform mat4 inv_proj;
uniform vec2 screen_res;

// Chromatic aberration: red and blue pulled apart along the line from the centre, by this many
// pixels at the corners
uniform float ca_pixels;
// Depth of field: when ca_dof is set the split also follows how far out of focus the pixel is,
// using the same circle of confusion as the depth of field pass
uniform int ca_dof;
uniform float focal_distance;
uniform float blur_constant;
uniform float tan_pixel_angle;
uniform float magnification;
uniform float max_cof;

// Lens flare
uniform float flare_strength;
uniform vec3 flare_color;
uniform vec3 sun_screen;        // xy in 0..1, z 1 when the sun is in front of the camera

// Film grain
uniform float grain_strength;
uniform float grain_size;       // in pixels
uniform float grain_seed;       // changes every frame

float viewDepth(vec2 tc)
{
    float z = texture(depthMap, tc).r * 2.0 - 1.0;
    vec4 p = inv_proj * vec4(0.0, 0.0, z, 1.0);
    return p.z / p.w;
}

// How far out of focus, 0 sharp to 1 at the largest blur the depth of field draws
float defocus(vec2 tc)
{
    float depth = viewDepth(tc);
    float sc = (depth - focal_distance) / -depth * blur_constant;
    sc /= magnification;
    float pixel_length = tan_pixel_angle * -focal_distance;
    sc = sc / pixel_length * 1.414;
    return clamp(abs(sc) / max(max_cof, 1e-4), 0.0, 1.0);
}

vec3 chromaticAberration(vec2 tc, vec3 base)
{
    vec2 from_center = tc - 0.5;
    // Grows with distance from the centre, as lateral aberration does in a real lens
    vec2 shift = from_center * 2.0 * ca_pixels / screen_res;
    if (ca_dof != 0)
    {
        shift *= defocus(tc);
    }
    if (dot(shift, shift) < 1e-10)
    {
        return base;
    }
    float r = texture(diffuseRect, tc + shift).r;
    float b = texture(diffuseRect, tc - shift).b;
    return vec3(r, base.g, b);
}

// How much of the sun's disc is not covered by geometry: depth samples on two rings around it
float sunVisibility(vec2 sun)
{
    if (sun_screen.z < 0.5)
    {
        return 0.0;
    }
    float aspect = screen_res.x / screen_res.y;
    float visible = 0.0;
    const int TAPS = 12;
    for (int i = 0; i < TAPS; ++i)
    {
        float a = float(i) / float(TAPS) * 6.2831853;
        vec2 dir = vec2(cos(a) / aspect, sin(a));
        for (int ring = 1; ring <= 2; ++ring)
        {
            vec2 p = sun + dir * 0.006 * float(ring);
            if (p.x >= 0.0 && p.x <= 1.0 && p.y >= 0.0 && p.y <= 1.0)
            {
                visible += texture(depthMap, p).r >= 1.0 ? 1.0 : 0.0;
            }
        }
    }
    visible /= float(TAPS * 2);

    // Fade out as the sun leaves the frame instead of cutting off at the edge
    vec2 outside = max(abs(sun - 0.5) - 0.5, vec2(0.0));
    return visible * clamp(1.0 - length(outside) * 8.0, 0.0, 1.0);
}

vec3 lensFlare(vec2 tc)
{
    vec2 sun = sun_screen.xy;
    float vis = sunVisibility(sun);
    if (vis <= 0.0)
    {
        return vec3(0.0);
    }

    float aspect = screen_res.x / screen_res.y;
    vec2 to_px = (tc - sun) * vec2(aspect, 1.0);
    float d_sun = length(to_px);

    // Glare around the light itself, with faint spikes from the aperture blades
    float glare = exp(-d_sun * 18.0) * 0.6 + exp(-d_sun * 4.0) * 0.08;
    float ang = atan(to_px.y, to_px.x);
    float spikes = pow(abs(cos(ang * 3.0)), 60.0) * exp(-d_sun * 6.0) * 0.25;

    vec3 flare = vec3(glare + spikes);

    // Ghosts: reflections between lens elements land on the line from the sun through the
    // centre of the frame, on both sides of it, each a soft disc with its own coating tint
    vec2 axis = vec2(0.5) - sun;
    const int GHOSTS = 5;
    float positions[GHOSTS] = float[](0.45, 0.8, 1.25, 1.6, 2.1);
    float radii[GHOSTS] = float[](0.035, 0.07, 0.025, 0.11, 0.05);
    vec3 tints[GHOSTS] = vec3[](vec3(0.9, 0.6, 1.0), vec3(0.5, 0.8, 1.0), vec3(1.0, 0.8, 0.5),
                                vec3(0.6, 1.0, 0.7), vec3(1.0, 0.6, 0.6));
    for (int i = 0; i < GHOSTS; ++i)
    {
        vec2 center = sun + axis * positions[i];
        float d = length((tc - center) * vec2(aspect, 1.0));
        float disc = 1.0 - smoothstep(radii[i] * 0.7, radii[i], d);
        flare += tints[i] * disc * 0.07;
    }

    // A faint ring around the centre of the frame, on the side facing the sun
    float ring_d = length((tc - 0.5) * vec2(aspect, 1.0));
    float ring = smoothstep(0.02, 0.0, abs(ring_d - 0.42));
    float facing = max(dot(normalize((tc - 0.5) * vec2(aspect, 1.0) + 1e-5), normalize(-axis * vec2(aspect, 1.0) + 1e-5)), 0.0);
    flare += vec3(0.7, 0.85, 1.0) * ring * facing * 0.04;

    return flare * flare_color * vis * flare_strength;
}

float hash(vec2 p)
{
    vec3 p3 = fract(vec3(p.xyx) * 0.1031);
    p3 += dot(p3, p3.yzx + 33.33);
    return fract((p3.x + p3.y) * p3.z);
}

vec3 filmGrain(vec2 tc, vec3 c)
{
    vec2 cell = floor(tc * screen_res / max(grain_size, 1.0));
    // Two hashes give a roughly bell shaped value around 0
    float n = hash(cell + grain_seed) + hash(cell * 1.7 + grain_seed * 3.1) - 1.0;
    // Film grain shows most in the midtones and least in deep shadow and full highlights
    float luma = dot(c, vec3(0.2126, 0.7152, 0.0722));
    float response = 4.0 * luma * (1.0 - luma);
    return c + n * grain_strength * (0.25 + 0.75 * response);
}

void main()
{
    vec2 tc = vary_fragcoord.xy;
    vec4 diff = texture(diffuseRect, tc);
    vec3 c = diff.rgb;

    if (ca_pixels > 0.0)
    {
        c = chromaticAberration(tc, c);
    }
    if (flare_strength > 0.0)
    {
        c += lensFlare(tc);
    }
    if (grain_strength > 0.0)
    {
        c = filmGrain(tc, c);
    }

    frag_color = vec4(clamp(c, 0.0, 1.0), diff.a);
}
