// Dust motes: tiny specks floating in the air, seen only where sunlight or projector cones light
// them, glinting brightest when looking toward the light.
//
// The motes live in a world aligned grid of cells around the camera. A hash of each cell's global
// index decides whether it holds a mote and where, so the pattern is fixed in the world and does
// not swim as the camera moves. There is no shared wind: each mote drifts and wobbles on its own,
// smoothly in time, but never leaves its cell.
//
// One point per cell: this vertex shader finds the cell's mote, culls it (no mote, beyond the
// range, off screen, behind what the depth buffer holds, or unlit) by moving it outside the clip
// volume, lights it (one sun shadow tap, or for each projector its cone, falloff and one shadow
// tap) and sizes the point to the mote's soft disc; fsDustMotesF.glsl draws the disc. A ray march
// per pixel did the same work for millions of pixels, almost all of which held no mote.

/*[EXTRA_CODE_HERE]*/

in vec3 position;   // the cell, relative to the camera's cell (whole numbers)

out vec3 vary_color;    // the mote's light times its coverage at the centre
out float vary_sigma;   // the disc's spread in pixels
out float vary_size;    // the point's size in pixels

uniform mat4 dust_proj;             // view space to clip space

// Sun
uniform sampler2DShadow shadowMap0;
uniform sampler2DShadow shadowMap1;
uniform sampler2DShadow shadowMap2;
uniform sampler2DShadow shadowMap3;
uniform mat4 shadow_matrix[6];
uniform vec4 shadow_clip;
uniform float shadow_bias;
uniform vec3 sun_dir;       // view space, toward the light
uniform vec3 moon_dir;
uniform int sun_up_factor;

// Projectors
uniform sampler2DShadow dust_spot_shadow0;
uniform sampler2DShadow dust_spot_shadow1;
uniform sampler2DShadow dust_spot_shadow2;
uniform sampler2DShadow dust_spot_shadow3;
uniform float spot_shadow_bias;
uniform int dust_spot_count;                // 0 to 4
uniform mat4 dust_spot_proj[4];             // view space to the projector's texture space
uniform mat4 dust_spot_shadow_matrix[4];    // view space to its shadow map, [0, 1]
uniform vec4 dust_spot_center[4];           // xyz light position (view space), w range
uniform vec4 dust_spot_origin[4];           // xyz apex of the frustum (view space), w falloff
uniform vec4 dust_spot_color[4];            // rgb colour times brightness, a shadow fade (1 - fade), < 0 unshadowed

uniform int dust_sun_on;            // the sun rays ran this frame
uniform vec3 dust_sun_color;        // the sun rays' light colour times brightness

// The scene's depth, to hide motes behind surfaces
uniform sampler2D depthMap;
uniform mat4 inv_proj;

uniform mat4 dust_view_to_world;    // rotation only
uniform vec3 dust_cam_cell;         // camera position in cells, inside the base cell [0, 1)
uniform vec3 dust_cell_base;        // the base cell's global index, wrapped to [0, 65536)
uniform float dust_cell_size;       // metres
uniform float dust_range;           // metres
uniform float dust_probability;     // chance a cell holds a mote
uniform float dust_size;            // mote diameter in pixels
uniform float dust_time;            // seconds, wrapped to [0, CYCLE)
uniform float dust_turbulence;      // how far a mote wanders off its path, cells, 0 to 0.14

const float NEAR_DIST = 0.15;   // closer motes are skipped (they would sit on the lens)
const float TWO_PI = 6.2831853;
const float CYCLE = 960.0;      // seconds; every motion runs whole turns of it, so its wrap is seamless
const float MARGIN = 0.1;       // a mote stays this far (in cells) from its cell's sides

float calcLegacyDistanceAttenuation(float distance, float falloff)
{
    float dist_atten = 1.0 - clamp((distance + falloff) / (1.0 + falloff), 0.0, 1.0);
    dist_atten *= dist_atten;
    dist_atten *= 2.0;
    return dist_atten;
}

// Integer hash (pcg3d, Jarzynski and Olano 2020): three independent 32 bit values
uvec3 pcg3d(uvec3 v)
{
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

vec3 unorm3(uvec3 h)
{
    return vec3(h >> 8u) * (1.0 / 16777216.0);
}

// Henyey-Greenstein, scaled so that even scattering is 1 (as in fsSunRaysF)
float phaseHG(float cos_theta, float g)
{
    float g2 = g * g;
    float denom = 1.0 + g2 - 2.0 * g * cos_theta;
    // Capped: near g = 1 the peak grows without bound and blows out bloom
    return min((1.0 - g2) / (denom * sqrt(denom)), 16.0);
}

// Dust scatters strongly forward: backlit specks blaze, side lit ones are faint
float glintPhase(float cos_theta)
{
    return 0.2 + 0.8 * phaseHG(cos_theta, 0.7);
}

// The sun rays' shadow lookup: one comparison tap in the nearest cascade, lit beyond the last
float sunVisibility(vec3 pos)
{
    float dist = -pos.z;
    vec4 p = vec4(pos, 1.0);
    vec4 lpos;
    float vis = 1.0;
    if (dist < shadow_clip.x)
    {
        lpos = shadow_matrix[0] * p;
        lpos.xyz /= lpos.w;
        vis = textureLod(shadowMap0, vec3(lpos.xy, lpos.z + shadow_bias), 0.0);
    }
    else if (dist < shadow_clip.y)
    {
        lpos = shadow_matrix[1] * p;
        lpos.xyz /= lpos.w;
        vis = textureLod(shadowMap1, vec3(lpos.xy, lpos.z + shadow_bias), 0.0);
    }
    else if (dist < shadow_clip.z)
    {
        lpos = shadow_matrix[2] * p;
        lpos.xyz /= lpos.w;
        vis = textureLod(shadowMap2, vec3(lpos.xy, lpos.z + shadow_bias), 0.0);
    }
    else if (dist < shadow_clip.w)
    {
        lpos = shadow_matrix[3] * p;
        lpos.xyz /= lpos.w;
        vis = textureLod(shadowMap3, vec3(lpos.xy, lpos.z + shadow_bias), 0.0);
    }
    return vis;
}

float spotShadow(int i, vec3 coord)
{
    if (i == 0)
    {
        return textureLod(dust_spot_shadow0, coord, 0.0);
    }
    if (i == 1)
    {
        return textureLod(dust_spot_shadow1, coord, 0.0);
    }
    if (i == 2)
    {
        return textureLod(dust_spot_shadow2, coord, 0.0);
    }
    return textureLod(dust_spot_shadow3, coord, 0.0);
}

// Projector i's light on the air at p, toward the camera (as fsSpotVolumetricF lights a step,
// without the texture)
vec3 spotLight(int i, vec3 p, vec3 view_dir)
{
    vec4 proj_tc = dust_spot_proj[i] * vec4(p, 1.0);
    if (proj_tc.z < 0.0)
    {
        return vec3(0.0);
    }
    proj_tc.xyz /= proj_tc.w;
    if (proj_tc.x <= 0.0 || proj_tc.x >= 1.0 || proj_tc.y <= 0.0 || proj_tc.y >= 1.0)
    {
        return vec3(0.0);
    }

    vec3 lv = dust_spot_center[i].xyz - p;
    float dist_atten = calcLegacyDistanceAttenuation(length(lv) / dust_spot_center[i].w, dust_spot_origin[i].w);
    if (dist_atten <= 0.0)
    {
        return vec3(0.0);
    }

    float vis = 1.0;
    if (dust_spot_color[i].a >= 0.0)
    {
        vec4 lpos = dust_spot_shadow_matrix[i] * vec4(p, 1.0);
        lpos.xyz /= lpos.w;
        vis = spotShadow(i, vec3(lpos.xy, lpos.z + spot_shadow_bias * 0.8));
        vis = clamp(vis + dust_spot_color[i].a, 0.0, 1.0);
    }

    float cos_theta = dot(view_dir, normalize(dust_spot_origin[i].xyz - p));
    return dust_spot_color[i].rgb * (dist_atten * vis * glintPhase(cos_theta));
}

// View-space depth (negative) of the scene at screen coordinate uv
float sceneViewZ(vec2 uv)
{
    float d = textureLod(depthMap, uv, 0.0).r;
#ifdef REVERSE_Z
    if (d <= 0.0)
    {
        return -1.0e9; // sky
    }
    float ndc_z = d;
#else
    if (d >= 1.0)
    {
        return -1.0e9; // sky
    }
    float ndc_z = 2.0 * d - 1.0;
#endif
    vec4 p = inv_proj * vec4(uv * 2.0 - 1.0, ndc_z, 1.0);
    return p.z / p.w;
}

void cull()
{
    gl_Position = vec4(2.0, 2.0, 2.0, 1.0); // outside the clip volume: no fragments
    gl_PointSize = 1.0;
    vary_color = vec3(0.0);
    vary_sigma = 1.0;
    vary_size = 1.0;
}

void main()
{
    float cs = dust_cell_size;
    ivec3 cell = ivec3(floor(position + 0.5));

    // Cells whose whole extent is beyond the range hold no visible mote
    vec3 centre = (vec3(cell) + 0.5 - dust_cam_cell) * cs;
    if (dot(centre, centre) > (dust_range + cs) * (dust_range + cs))
    {
        cull();
        return;
    }

    ivec3 base = ivec3(dust_cell_base);
    uvec3 h = pcg3d(uvec3(base + cell + 65536) & uvec3(0xffffu));    // kept positive, wraps with the base
    if (float(h.x >> 8u) * (1.0 / 16777216.0) >= dust_probability)
    {
        cull();
        return;
    }

    // The mote's life: it fades in, drifts along its own straight path at its own speed,
    // wobbling in a little turbulence, and fades out, and a new mote starts somewhere else in the
    // cell. Each cell has its own life length (15 to 30 seconds, a whole fraction of the cycle)
    // and offset, so lives neither line up nor jump at the cycle's wrap.
    uint lives = 32u + (h.y & 31u);
    float lp = dust_time * (float(lives) / CYCLE) + unorm3(h).z;
    uint life = uint(lp) % lives;
    float u = fract(lp);
    float env = smoothstep(0.0, 0.15, u) * (1.0 - smoothstep(0.85, 1.0, u));

    uvec3 hl = pcg3d(h + uvec3(life * 2654435769u, life, ~life));
    uvec3 hl2 = pcg3d(hl);
    uvec3 hl3 = pcg3d(hl2);

    // The path, from a random start to a random end, kept in from the cell's sides by the margin
    // and the wobble. A slight lean downward lets dust settle on average, but many motes rise.
    float w = dust_turbulence;
    float lo = MARGIN + w;
    float width = 1.0 - 2.0 * lo;
    vec3 a = unorm3(hl);
    vec3 b = unorm3(hl2);
    a.z = mix(a.z, 1.0, 0.1);
    b.z = mix(b.z, 0.0, 0.1);
    vec3 path = lo + width * mix(a, b, u);

    // Turbulence: two incommensurate sines per axis, each its own whole number of turns per
    // cycle (a slow one of 7 to 12 seconds, a quick one of 3 to 5) and own phase. A quarter of
    // the motes circle: their slow x and y share a rate, a quarter turn apart.
    vec3 k_slow = vec3(uvec3(80u) + (hl & uvec3(63u)));
    vec3 k_fast = vec3(uvec3(190u) + (hl2 & uvec3(127u)));
    vec3 ph_slow = vec3(hl3 & uvec3(255u)) * (1.0 / 256.0);
    vec3 ph_fast = vec3((hl3 >> 8u) & uvec3(255u)) * (1.0 / 256.0);
    if (((hl.z >> 6u) & 3u) == 0u)
    {
        k_slow.y = k_slow.x;
        ph_slow.y = ph_slow.x + 0.25;
    }
    float cyc = dust_time * (1.0 / CYCLE);
    vec3 wobble = 0.7 * sin(TWO_PI * fract(k_slow * cyc + ph_slow)) + 0.3 * sin(TWO_PI * fract(k_fast * cyc + ph_fast));
    vec3 rel = (vec3(cell) + path + w * wobble - dust_cam_cell) * cs;   // metres from the camera, world axes

    vec3 p = rel * mat3(dust_view_to_world);   // view space (the inverse of a rotation is its transpose)
    float t = length(p);
    if (t < NEAR_DIST || t > dust_range || -p.z < NEAR_DIST)
    {
        cull();
        return;
    }

    vec4 clip = dust_proj * vec4(p, 1.0);
    vec2 ndc = clip.xy / clip.w;
    if (any(greaterThan(abs(ndc), vec2(1.05))))
    {
        cull();
        return;
    }

    // Behind a surface
    if (sceneViewZ(ndc * 0.5 + 0.5) > p.z + 0.02)
    {
        cull();
        return;
    }

    vec3 view_dir = p / t;
    vec3 light = vec3(0.0);
    if (dust_sun_on != 0)
    {
        vec3 light_dir = normalize(sun_up_factor == 1 ? sun_dir : moon_dir);
        light += dust_sun_color * (sunVisibility(p) * glintPhase(dot(view_dir, light_dir)));
    }
    for (int j = 0; j < 4; ++j)
    {
        if (j >= dust_spot_count)
        {
            break;
        }
        light += spotLight(j, p, view_dir);
    }

    // Twinkle: a flat flake turning slowly (2 to 8 seconds a turn, its own rate and phase)
    // flashes twice a turn as it faces the light. Some motes are brighter, some are bigger flakes.
    vec3 r = vec3((hl3 >> 16u) & uvec3(255u)) * (1.0 / 256.0);
    float spin = TWO_PI * fract(float(120u + 4u * (hl3.x >> 26u)) * cyc + r.z);
    float twinkle = 0.3 + 2.0 * pow(abs(cos(spin)), 12.0);
    float variety = 0.3 + 0.7 * r.y * r.y;
    float flake = 0.7 + 0.9 * r.x * r.x * r.x;

    // Distant motes are too small to see: fade them, and to nothing at the range
    float fade = smoothstep(NEAR_DIST, 0.45, t) / (1.0 + t * t * (1.0 / 16.0));
    fade *= 1.0 - smoothstep(0.7 * dust_range, dust_range, t);

    // A soft round point dust_size pixels across, up to twice that very close, never below half
    // a pixel of spread so a moving mote fades between pixels instead of popping
    float size = dust_size * 0.4 * flake * clamp(0.75 / t, 1.0, 2.0);
    // At most 5 pixels of spread, so the disc out to three sigma (6 sigma + 1 across) fits the
    // 32 pixel point; a larger mote keeps its light (gain below) in a slightly smaller disc
    float sigma = clamp(size, 0.45, 5.0);
    float gain = size / sigma;
    vec3 color = light * (env * gain * gain * twinkle * variety * fade);
    if (max(color.r, max(color.g, color.b)) < 1.0e-4)
    {
        cull();
        return;
    }

    gl_Position = clip;
    float point = clamp(ceil(sigma * 6.0) + 1.0, 2.0, 32.0);   // the disc out to three sigma
    gl_PointSize = point;
    vary_color = color;
    vary_sigma = sigma;
    vary_size = point;
}
