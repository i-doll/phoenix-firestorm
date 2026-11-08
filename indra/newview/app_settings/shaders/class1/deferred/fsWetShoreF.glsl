// Wet shoreline band: surfaces just above the water line are darker and glossier, as wet sand
// and rock are, with a top edge that rises and falls with slow swells and is ragged rather than
// a straight line. Linked into the sun light pass (softenLightF.glsl) when FSRenderWetShore is
// on. Uses the region's water height, so a surface at that height away from any water (rare,
// as land sits above it) gets wet too.

uniform vec4 fs_wet_shore_params;   // x strength (0 = off), y band height in metres, z swell phase, w swell height in metres
uniform mat4 fs_wet_shore_inv_view; // eye space to agent space

// Value noise in [0, 1], arithmetic hash only so it is stable across GPUs
float fsWetShoreHash(vec2 p)
{
    vec3 q = fract(vec3(p.xyx) * 0.1031);
    q += dot(q, q.yzx + 33.33);
    return fract((q.x + q.y) * q.z);
}

float fsWetShoreNoise(vec2 p)
{
    vec2 i = floor(p);
    vec2 f = p - i;
    f = f * f * (3.0 - 2.0 * f);
    float a = fsWetShoreHash(i);
    float b = fsWetShoreHash(i + vec2(1.0, 0.0));
    float c = fsWetShoreHash(i + vec2(0.0, 1.0));
    float d = fsWetShoreHash(i + vec2(1.0, 1.0));
    return mix(mix(a, b, f.x), mix(c, d, f.x), f.y);
}

// How wet the surface at eye-space pos is, 0 to 1
float fsWetShore(vec3 pos, vec3 norm, vec4 water_plane)
{
    float strength = fs_wet_shore_params.x;
    float band = fs_wet_shore_params.y;
    if (strength <= 0.0 || band <= 0.0)
    {
        return 0.0;
    }

    float height = dot(pos, water_plane.xyz) + water_plane.w; // metres above the water
    float top_max = band + fs_wet_shore_params.w;
    if (height > top_max || height < -1.0)
    {
        return 0.0; // above any swell, or deep enough that the water hides it anyway
    }

    vec2 xy = (fs_wet_shore_inv_view * vec4(pos, 1.0)).xy;

    // Swells: two slow travelling waves across the region, so the band breathes unevenly
    float t = fs_wet_shore_params.z;
    float swell = 0.5 + 0.25 * sin(t * 0.45 + dot(xy, vec2(0.11, 0.07)))
                      + 0.25 * sin(t * 0.31 - dot(xy, vec2(0.05, 0.13)));

    // Ragged edge: the top moves by up to a third of the band with metre and decimetre noise
    float ragged = fsWetShoreNoise(xy * 0.9) * 0.7 + fsWetShoreNoise(xy * 6.0) * 0.3;
    float top = band * (0.67 + 0.33 * ragged) + fs_wet_shore_params.w * swell;

    // Fully wet near the water, drying over the upper part of the band
    float wet = 1.0 - smoothstep(top * 0.55, top, height);

    // Overhangs facing down stay drier than ground and walls
    float up = dot(norm, water_plane.xyz);
    wet *= clamp(0.75 + 0.5 * up, 0.35, 1.0);

    return clamp(wet * strength, 0.0, 1.0);
}
