// Water caustics: sunlight focused by the waves into a moving web of bright lines on whatever
// lies under the water plane. Linked into the sun light pass (softenLightF.glsl) when
// FSRenderWaterCaustics is on; the pattern is procedural, so there is no texture to load.

uniform vec4 fs_caustic_params;     // x strength (0 = off), y cells per metre, z animation phase
uniform mat4 fs_caustic_inv_view;   // eye space to agent space

// Hash of a 2D cell index to two values in [0, 1), arithmetic only so it is stable across GPUs
vec2 fsCausticHash(vec2 p)
{
    vec3 q = fract(vec3(p.xyx) * vec3(0.1031, 0.1030, 0.0973));
    q += dot(q, q.yzx + 33.33);
    return fract((q.xx + q.yz) * q.zy);
}

// One layer: each cell holds a point circling about its centre, and the light gathers along the
// borders between neighbouring cells, where the distances to the nearest two points meet.
float fsCausticLayer(vec2 p, float t)
{
    vec2 cell = floor(p);
    vec2 f = p - cell;
    float d1 = 8.0;
    float d2 = 8.0;
    for (int y = -1; y <= 1; ++y)
    {
        for (int x = -1; x <= 1; ++x)
        {
            vec2 o = vec2(float(x), float(y));
            vec2 h = fsCausticHash(cell + o);
            vec2 r = o + 0.5 + 0.4 * sin(vec2(t) + 6.2831853 * h) - f;
            float d = dot(r, r);
            if (d < d1)
            {
                d2 = d1;
                d1 = d;
            }
            else if (d < d2)
            {
                d2 = d;
            }
        }
    }
    float c = 1.0 - clamp((sqrt(d2) - sqrt(d1)) * 2.5, 0.0, 1.0);
    return c * c * c;
}

// Caustic light on a surface, as a fraction of the direct sunlight it would get.
// pos and norm are in eye space, light_dir is the eye space direction to the sun, water_plane is
// the eye space water plane (positive above the surface).
float fsWaterCaustics(vec3 pos, vec3 norm, vec3 light_dir, vec4 water_plane)
{
    float strength = fs_caustic_params.x;
    float below = -(dot(pos, water_plane.xyz) + water_plane.w); // metres under the surface
    float sun_height = dot(light_dir, water_plane.xyz);          // sine of the sun's elevation
    float nl = dot(norm, light_dir);
    if (strength <= 0.0 || below <= 0.0 || sun_height <= 0.0 || nl <= 0.0)
    {
        return 0.0;
    }

    // Rise in over the first few centimetres so there is no seam at the waterline, then die away
    // with depth as the focus spreads out, and with a low sun as the light skims the surface
    float fade = smoothstep(0.0, 0.3, below) * exp(-0.12 * below) * smoothstep(0.05, 0.4, sun_height);
    if (fade < 0.002)
    {
        return 0.0;
    }

    vec3 wpos = (fs_caustic_inv_view * vec4(pos, 1.0)).xyz;
    vec3 wlight = mat3(fs_caustic_inv_view) * light_dir;

    // Trace back up the (refracted, so steeper) sun ray to where it entered the water, so the
    // pattern on the floor and on a wading avatar agree and slides as the sun moves
    vec2 xy = wpos.xy + wlight.xy / max(wlight.z, 0.2) * below * 0.75;

    vec2 p = xy * fs_caustic_params.y;
    float t = fs_caustic_params.z;
    float c = fsCausticLayer(p, t) * 0.6
            + fsCausticLayer(mat2(0.8, -0.6, 0.6, 0.8) * p * 1.7 + vec2(3.1, 7.7), 2.0 * t) * 0.4;

    return c * strength * fade * nl;
}
