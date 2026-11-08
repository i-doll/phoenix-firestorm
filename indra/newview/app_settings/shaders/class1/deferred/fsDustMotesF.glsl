// Dust motes: tiny specks floating in the air, seen only where sunbeams or projector cones light
// them, glinting brightest when looking toward the light.

/*[EXTRA_CODE_HERE]*/

// Drawn at full resolution with additive blending over the HDR scene, after the sun rays and the
// volumetric projector light, so exposure and bloom treat both alike.
//
// The motes live in a world aligned grid of cells around the camera. A hash of each cell's global
// index decides whether it holds a mote and where, so the pattern is fixed in the world and does
// not swim as the camera moves. There is no shared wind: each mote drifts and wobbles on its own,
// smoothly in time, but never leaves its cell. Each pixel walks its view ray through the cells (3D DDA) up to
// what it sees or the range, and stops at the first mote close enough to the ray to cover it.
// Motes are drawn a fixed size in pixels: the ray to mote distance is measured against the
// pixel's footprint at the mote. Only that one mote is lit: one sun shadow tap, or for each
// projector its cone, falloff and one shadow tap, as the volumetric passes sample them.
// Pixels where the reduced resolution volumetric passes found no lit air are skipped first, so
// in a dark room only the shafts and cones pay for the walk.

out vec4 frag_color;

in vec2 vary_fragcoord;

// Sun (as fsSunRaysF)
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

// Projectors (as fsSpotVolumetricF)
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

// What the volumetric passes left this frame, reduced resolution
uniform sampler2D dust_sun_rays;    // r: sunlight scattered along the ray, 0 where it is all shadowed
uniform sampler2D dust_spot_volume; // rgb: projector light scattered along the ray
uniform int dust_sun_on;            // the sun rays ran this frame
uniform vec3 dust_sun_color;        // the sun rays' light colour times brightness

uniform mat4 dust_view_to_world;    // rotation only
uniform vec3 dust_cam_cell;         // camera position in cells, inside the base cell [0, 1)
uniform vec3 dust_cell_base;        // the base cell's global index, wrapped to [0, 65536)
uniform float dust_cell_size;       // metres
uniform float dust_range;           // metres
uniform int dust_max_cells;         // cells the walk may visit, covers the range
uniform float dust_probability;     // chance a cell holds a mote
uniform float dust_size;            // mote diameter in pixels
uniform float dust_pixel_angle;     // radians per pixel, vertically
uniform float dust_time;            // seconds, wrapped to [0, CYCLE)
uniform float dust_turbulence;      // how far a mote wanders off its path, cells, 0 to 0.14

const int MAX_CELLS = 64;
const float NEAR_DIST = 0.15;   // closer motes are skipped (they would sit on the lens)
const float TWO_PI = 6.2831853;
const float CYCLE = 960.0;      // seconds; every motion runs whole turns of it, so its wrap is seamless
const float MARGIN = 0.1;       // a mote stays this far (in cells) from its cell's sides

float getDepth(vec2 pos_screen);
vec4 getPositionWithDepth(vec2 pos_screen, float depth);
float calcLegacyDistanceAttenuation(float distance, float falloff);

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
    return (1.0 - g2) / (denom * sqrt(denom));
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

void main()
{
    vec2 tc = vary_fragcoord.xy;

    // Lit air along this pixel's ray? The volumetric maps are exactly 0 where every sample was
    // in shadow or outside every cone; the bilinear tap looks at the 2x2 texels around
    bool sun_lit = false;
    if (dust_sun_on != 0)
    {
        sun_lit = texture(dust_sun_rays, tc).r > 1.0e-7;
    }
    bool spot_lit = false;
    if (dust_spot_count > 0)
    {
        vec3 v = texture(dust_spot_volume, tc).rgb;
        spot_lit = max(v.r, max(v.g, v.b)) > 1.0e-7;
    }
    if (!sun_lit && !spot_lit)
    {
        discard;
    }

    float depth = getDepth(tc);
    bool sky = depth >= 1.0;
    vec3 pos = getPositionWithDepth(tc, sky ? 0.5 : depth).xyz;
    vec3 view_dir = normalize(pos);

    // Motes in front of the sky still count: a sunbeam seen against a bright window or the sky
    // is the classic look
    float t_start = NEAR_DIST;
    float t_end = sky ? dust_range : min(length(pos), dust_range);
    if (!sun_lit)
    {
        // Projector light only: walk just the part of the ray inside their ranges
        float lo = 1.0e9;
        float hi = 0.0;
        for (int i = 0; i < 4; ++i)
        {
            if (i >= dust_spot_count)
            {
                break;
            }
            vec3 c = dust_spot_center[i].xyz;
            float b = dot(view_dir, c);
            float disc = b * b - dot(c, c) + dust_spot_center[i].w * dust_spot_center[i].w;
            if (disc > 0.0)
            {
                float h = sqrt(disc);
                lo = min(lo, b - h);
                hi = max(hi, b + h);
            }
        }
        t_start = max(t_start, lo);
        t_end = min(t_end, hi);
    }
    if (t_end <= t_start)
    {
        discard;
    }

    // 3D DDA through the cells, in the world's axes. Positions are in cells relative to the base
    // cell, so they stay small and exact however far the camera is from the grid's origin.
    float cs = dust_cell_size;
    vec3 dir = normalize(mat3(dust_view_to_world) * view_dir);
    vec3 p0 = dust_cam_cell + dir * (t_start / cs);
    ivec3 cell = ivec3(floor(p0));
    vec3 sgn = vec3(dir.x < 0.0 ? -1.0 : 1.0, dir.y < 0.0 ? -1.0 : 1.0, dir.z < 0.0 ? -1.0 : 1.0);
    ivec3 stp = ivec3(sgn);
    vec3 inv = 1.0 / max(abs(dir), vec3(1.0e-6));
    vec3 t_delta = inv * cs;                                                   // metres per cell
    vec3 t_max = t_start + (vec3(cell) + max(sgn, vec3(0.0)) - p0) * sgn * t_delta;  // next boundary
    ivec3 base = ivec3(dust_cell_base);

    // Mote shape: a soft round point dust_size pixels across, up to twice that very close, never
    // below half a pixel of spread so a moving mote fades between pixels instead of popping
    float size_far = dust_size * 0.4;

    vec3 color = vec3(0.0);
    for (int i = 0; i < MAX_CELLS; ++i)
    {
        if (i >= dust_max_cells)
        {
            break;
        }

        uvec3 h = pcg3d(uvec3(base + cell + 65536) & uvec3(0xffffu));    // kept positive, wraps with the base
        if (float(h.x >> 8u) * (1.0 / 16777216.0) < dust_probability)
        {
            // The mote's life: it fades in, drifts along its own straight path at its own speed,
            // wobbling in a little turbulence, and fades out, and a new mote starts somewhere else
            // in the cell. Each cell has its own life length (15 to 30 seconds, a whole fraction
            // of the cycle) and offset, so lives neither line up nor jump at the cycle's wrap.
            uint lives = 32u + (h.y & 31u);
            float lp = dust_time * (float(lives) / CYCLE) + unorm3(h).z;
            uint life = uint(lp) % lives;
            float u = fract(lp);
            float env = smoothstep(0.0, 0.15, u) * (1.0 - smoothstep(0.85, 1.0, u));

            uvec3 hl = pcg3d(h + uvec3(life * 2654435769u, life, ~life));
            uvec3 hl2 = pcg3d(hl);
            uvec3 hl3 = pcg3d(hl2);

            // The path, from a random start to a random end, both kept in from the cell's sides
            // by the margin and the wobble, so the mote is always found from the cells the ray
            // crosses. A slight lean downward in the ends lets dust settle on average, but many
            // motes still rise.
            float w = dust_turbulence;
            float lo = MARGIN + w;
            float width = 1.0 - 2.0 * lo;
            vec3 a = unorm3(hl);
            vec3 b = unorm3(hl2);
            a.z = mix(a.z, 1.0, 0.1);
            b.z = mix(b.z, 0.0, 0.1);
            vec3 path = lo + width * mix(a, b, u);

            // Turbulence: two incommensurate sines per axis, each its own whole number of turns
            // per cycle (a slow one of 7 to 12 seconds, a quick one of 3 to 5) and own phase.
            // A quarter of the motes circle: their slow x and y share a rate, a quarter turn
            // apart. The sum never exceeds w.
            vec3 k_slow = vec3(uvec3(80u) + (hl & uvec3(63u)));
            vec3 k_fast = vec3(uvec3(190u) + (hl2 & uvec3(127u)));
            vec3 ph_slow = vec3(hl3 & uvec3(255u)) * (1.0 / 256.0);
            vec3 ph_fast = vec3((hl3 >> 8u) & uvec3(255u)) * (1.0 / 256.0);
            if (((hl.z >> 6u) & 3u) == 0u)
            {
                k_slow.y = k_slow.x;
                ph_slow.y = ph_slow.x + 0.25;
            }
            float tc = dust_time * (1.0 / CYCLE);
            vec3 wobble = 0.7 * sin(TWO_PI * fract(k_slow * tc + ph_slow)) + 0.3 * sin(TWO_PI * fract(k_fast * tc + ph_fast));
            vec3 mote = vec3(cell) + path + w * wobble;

            vec3 rel = (mote - dust_cam_cell) * cs;     // metres from the camera
            float t = dot(rel, dir);
            if (t > t_start && t < t_end)
            {
                // Some motes are bigger flakes
                vec3 r = vec3((hl3 >> 16u) & uvec3(255u)) * (1.0 / 256.0);
                float flake = 0.7 + 0.9 * r.x * r.x * r.x;
                float footprint = t * dust_pixel_angle;     // metres per pixel at the mote
                float d2 = max(dot(rel, rel) - t * t, 0.0) / (footprint * footprint);   // pixels squared
                float size = size_far * flake * clamp(0.75 / t, 1.0, 2.0);
                float sigma = max(size, 0.45);
                float gain = size / sigma;
                float cover = env * gain * gain * exp(-0.5 * d2 / (sigma * sigma));
                if (cover > 0.003)
                {
                    vec3 p = view_dir * t;  // on this pixel's ray, within a pixel or two of the mote
                    vec3 light = vec3(0.0);
                    if (sun_lit)
                    {
                        vec3 light_dir = normalize(sun_up_factor == 1 ? sun_dir : moon_dir);
                        light += dust_sun_color * (sunVisibility(p) * glintPhase(dot(view_dir, light_dir)));
                    }
                    if (spot_lit)
                    {
                        for (int j = 0; j < 4; ++j)
                        {
                            if (j >= dust_spot_count)
                            {
                                break;
                            }
                            light += spotLight(j, p, view_dir);
                        }
                    }

                    // Twinkle: a flat flake turning slowly (2 to 8 seconds a turn, its own rate
                    // and phase) flashes twice a turn as it faces the light. Some motes are
                    // brighter than others.
                    float spin = TWO_PI * fract(float(120u + 4u * (hl3.x >> 26u)) * tc + r.z);
                    float twinkle = 0.3 + 2.0 * pow(abs(cos(spin)), 12.0);
                    float variety = 0.3 + 0.7 * r.y * r.y;

                    // Distant motes are too small to see: fade them, and to nothing at the range
                    float fade = smoothstep(NEAR_DIST, 0.45, t) / (1.0 + t * t * (1.0 / 16.0));
                    fade *= 1.0 - smoothstep(0.7 * dust_range, dust_range, t);

                    color = light * (cover * twinkle * variety * fade);
                    break;
                }
            }
        }

        float t_exit = min(t_max.x, min(t_max.y, t_max.z));
        if (t_exit >= t_end)
        {
            break;
        }
        if (t_max.x < t_max.y && t_max.x < t_max.z)
        {
            cell.x += stp.x;
            t_max.x += t_delta.x;
        }
        else if (t_max.y < t_max.z)
        {
            cell.y += stp.y;
            t_max.y += t_delta.y;
        }
        else
        {
            cell.z += stp.z;
            t_max.z += t_delta.z;
        }
    }

    if (max(color.r, max(color.g, color.b)) <= 0.0)
    {
        discard;
    }
    frag_color = vec4(color, 0.0);
}
