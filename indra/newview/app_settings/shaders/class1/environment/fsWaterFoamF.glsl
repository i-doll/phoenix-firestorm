// Shoreline foam: a soft, moving band of foam where the water surface meets land or anything
// standing in it. Linked into the water shaders (waterF.glsl, underWaterF.glsl) when
// FSRenderWaterFoam is on and water is transparent, since it needs the scene depth behind the
// surface. The pattern is procedural, so there is no texture to load.

uniform sampler2D depthMap;     // scene depth before the water was drawn
uniform mat4 inv_proj;
uniform vec3 eyeVec;            // camera position, agent space
uniform vec4 fs_foam_params;    // x strength (0 = off), y band width in metres, z animation phase

float fsFoamHash(vec2 p)
{
    vec3 q = fract(vec3(p.xyx) * 0.1031);
    q += dot(q, q.yzx + 33.33);
    return fract((q.x + q.y) * q.z);
}

float fsFoamNoise(vec2 p)
{
    vec2 i = floor(p);
    vec2 f = p - i;
    vec2 u = f * f * (3.0 - 2.0 * f);
    float a = fsFoamHash(i);
    float b = fsFoamHash(i + vec2(1.0, 0.0));
    float c = fsFoamHash(i + vec2(0.0, 1.0));
    float d = fsFoamHash(i + vec2(1.0, 1.0));
    return mix(mix(a, b, u.x), mix(c, d, u.x), u.y);
}

// Foam cover in [0, 1] for a water surface fragment.
// screen_tc   undistorted screen position of the fragment
// pos         eye space position of the fragment
// up          eye space normal of the water plane
// view_offset agent space offset of the fragment from the camera (waterV.glsl's view.xy)
float fsWaterFoam(vec2 screen_tc, vec3 pos, vec3 up, vec2 view_offset)
{
    float strength = fs_foam_params.x;
    float width = max(fs_foam_params.y, 0.01);
    float dist = length(pos);
    if (strength <= 0.0 || dist > 256.0)
    {
        return 0.0;
    }

    float depth = texture(depthMap, screen_tc).r;
    if (depth >= 1.0)
    {
        return 0.0; // sky behind the surface
    }
    vec4 scene = inv_proj * vec4(vec3(screen_tc, depth) * 2.0 - 1.0, 1.0);
    scene.xyz /= scene.w;

    // Vertical gap between the surface and what lies behind it: the depth of the water from
    // above, the height of whatever rises out of it from below. Either way it closes to zero
    // along the line where something meets the surface.
    float gap = abs(dot(scene.xyz - pos, up));
    float band = 1.0 - clamp(gap / width, 0.0, 1.0);
    if (band <= 0.0)
    {
        return 0.0;
    }

    vec2 w = view_offset + eyeVec.xy;
    float t = fs_foam_params.z;
    float n = fsFoamNoise(w * 1.7 + vec2(0.11, 0.07) * t) * 0.6
            + fsFoamNoise(w * 4.3 - vec2(0.05, 0.13) * t) * 0.4;

    // Ridges that wash in toward the edge, broken up by the noise so the outline frays
    float ridge = 0.5 + 0.5 * sin(band * 9.0 - t * 1.3 + n * 5.0);
    float shape = band * (0.6 + 0.4 * ridge) + (n - 0.5) * 0.7;
    float foam = smoothstep(0.3, 0.7, shape) * sqrt(band);

    // Depth precision and pixel footprint cannot hold a band this thin far away
    foam *= 1.0 - smoothstep(128.0, 256.0, dist);

    return clamp(foam * strength, 0.0, 1.0);
}
