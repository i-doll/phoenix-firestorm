// Sun glints: the sparse, flickering sparkles where tiny facets of the wave surface catch the
// sun, on top of the water's regular sun highlight. Linked into waterF.glsl under
// FS_WATER_GLINTS when FSRenderSunGlints is on.
//
// The surface is split into cells on the water plane. Each cell holds one facet whose tilt
// re-rolls a few times a second (at a rate of its own); it flashes when that tilt lands on the
// half vector between the view and the sun. The chance of that falls off with the angle between
// the half vector and the wave normal, so the sparkles gather into the glitter path around the
// reflection of the sun and follow the waves. Cells are sized from the pixel footprint (two
// levels blended, like mips), so a glint is always a pixel or two wide and the far water keeps
// sparkling instead of aliasing.

uniform vec3 eyeVec;            // camera position, agent space
uniform vec4 fs_glint_params;   // x intensity (0 = off), y density, z animation phase (s), w pixel angle (rad)

uvec3 fsGlintHash(uvec3 v)
{
    // pcg3d (Jarzynski and Olano 2020)
    v = v * 1664525u + 1013904223u;
    v.x += v.y * v.z;
    v.y += v.z * v.x;
    v.z += v.x * v.y;
    v ^= v >> 16u;
    v.x += v.y * v.z;
    v.y += v.z * v.x;
    v.z += v.x * v.y;
    return v;
}

vec3 fsGlintRand(vec2 cell, float a, float b)
{
    uvec3 h = fsGlintHash(uvec3(uvec2(ivec2(cell)), uint(int(a)) * 4099u + uint(b)));
    return vec3(h) * (1.0 / 4294967296.0);
}

// One level of cells of the given size: the glint coverage at w, in [0, 1]
float fsGlintLayer(vec2 w, float cell, float level, float chance, float spot, float t)
{
    vec2 g = w / cell;
    vec2 id = floor(g);
    vec2 f = g - id;

    // Each cell has its own re-roll rate and phase, so the flicker is not in step
    vec3 r0 = fsGlintRand(id, 0.0, level);
    float tt = t * (1.5 + 2.5 * r0.x) + r0.y * 16.0;
    float epoch = floor(tt);
    float life = tt - epoch;

    vec3 r1 = fsGlintRand(id, epoch, level + 64.0);
    if (r1.x >= chance)
    {
        return 0.0;
    }

    // A point somewhere inside the cell, and a quick flash over the facet's life
    vec2 c = 0.2 + 0.6 * r1.yz;
    float d = length(f - c);
    float shape = 1.0 - smoothstep(0.0, spot, d);
    float flash = sin(3.14159265 * life);
    return shape * flash * flash;
}

// Glint radiance factor for a water fragment, to be scaled by the sun's light and shadow.
// pos         eye space position
// n           eye space wave normal (unflattened)
// up          eye space normal of the water plane
// v           eye space direction to the camera
// l           eye space direction to the sun
// view_offset agent space offset of the fragment from the camera (waterV.glsl's view.xy)
float fsWaterGlint(vec3 pos, vec3 n, vec3 up, vec3 v, vec3 l, vec2 view_offset)
{
    float intensity = fs_glint_params.x;
    float nl = dot(up, l);
    if (intensity <= 0.0 || nl <= 0.0)
    {
        return 0.0;
    }

    // Spread of the facet tilts: how wide the glitter path is around the sun's reflection
    vec3 h = normalize(l + v);
    float ndh = max(dot(n, h), 1e-3);
    float tan2 = (1.0 - ndh * ndh) / (ndh * ndh);
    const float spread = 0.22;
    float lobe = exp(-tan2 / (spread * spread));
    if (lobe < 0.002)
    {
        return 0.0;
    }

    // Pixel footprint on the water plane, in metres, from the distance and the view angle
    float dist = length(pos);
    float cos_view = max(abs(dot(v, up)), 0.05);
    float px = dist * fs_glint_params.w / sqrt(cos_view);

    // Cells at least eight pixels across; the finest ones 0.12 m, closer than that glints
    // would be blobs rather than points
    float density = clamp(fs_glint_params.y, 0.1, 4.0);
    const float base_cell = 0.12;
    float lod = max(log2(px * 8.0 / base_cell), 0.0);
    float level = floor(lod);
    float blend = lod - level;
    float cell0 = base_cell * exp2(level);
    float cell1 = cell0 * 2.0;

    float chance = clamp(lobe * 0.3 * density, 0.0, 1.0);
    vec2 w = view_offset + eyeVec.xy;
    float t = fs_glint_params.z;

    // A point about a pixel and a half across, but never more than a quarter of the cell
    float g0 = fsGlintLayer(w, cell0, level, chance, clamp(1.5 * px / cell0, 0.03, 0.25), t);
    float g1 = fsGlintLayer(w, cell1, level + 1.0, chance, clamp(1.5 * px / cell1, 0.03, 0.25), t);
    float glint = mix(g0, g1, blend);

    // Water reflects more toward grazing (Schlick, F0 of water); the glints get brighter
    // toward the horizon like the real glitter path
    float vdh = clamp(dot(v, h), 0.0, 1.0);
    float fresnel = 0.02 + 0.98 * pow(1.0 - vdh, 5.0);

    // Faded in as the sun clears the horizon
    return glint * intensity * 6.0 * (0.25 + 0.75 * fresnel) * smoothstep(0.0, 0.1, nl);
}
