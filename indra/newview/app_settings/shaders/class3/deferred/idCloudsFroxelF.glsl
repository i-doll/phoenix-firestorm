/**
 * @file class3/deferred/idCloudsFroxelF.glsl
 * @brief <ID> Volumetric clouds: froxel volume that alpha surfaces read the cloud in front of them from.
 * Without ID_FROXEL_ACCUM each texel marches the segment of its view ray inside one slice; with it,
 * each texel composites the segments from the camera up to its slice.
 *
 * Part of Five's custom Firestorm fork.
 */

/*[EXTRA_CODE_HERE]*/

// rgb: in-scatter, already fogged by the haze in front of it; a: transmittance.
out vec4 frag_color;

uniform int id_froxel_slice;

vec3 clampHDRRange(vec3 color);

#if defined(ID_FROXEL_ACCUM)

uniform sampler3D idCloudFroxel;

void main()
{
    ivec2 xy = ivec2(gl_FragCoord.xy);
    vec3 scatter = vec3(0.0);
    float trans = 1.0;
    for (int k = 0; k <= id_froxel_slice; ++k)
    {
        vec4 seg = texelFetch(idCloudFroxel, ivec3(xy, k), 0);
        scatter += trans * seg.rgb;
        trans *= seg.a;
    }
    frag_color = vec4(clampHDRRange(scatter), clamp(trans, 0.0, 1.0));
}

#else

uniform vec2  id_froxel_grid;
uniform mat4  id_eye_to_world;
uniform int   id_detail_noise;
uniform vec3  id_camera_global;
uniform vec3  sun_dir;
uniform vec3  moon_dir;
uniform int   sun_up_factor;

vec4 getPositionWithDepth(vec2 pos_screen, float depth);
float idCloudDensityAll(vec3 world_pos, float footprint_m, bool cheap);
float idCloudSigma(float density);
float idFroxelSliceDist(int k);
float idAltitude(vec3 rel);
void idCloudLighting(vec3 dir_eye, vec3 up_eye, vec3 light_eye, out vec3 sunlit, out vec3 amblit);
vec3 idCloudSampleLight(vec3 rel, float alt, float ts, float sigma, float footprint, float cos_theta, vec3 light_w, vec3 light_eye,
                        vec2 tc, vec3 sunlit, vec3 amblit);
void idHaze(vec3 pos_eye, vec3 light_eye, out vec3 add, out float att);

const int ID_FROXEL_STEPS = 4;

void main()
{
    vec2 tc = gl_FragCoord.xy / id_froxel_grid;
    mat3 eye_to_world = mat3(id_eye_to_world);
    vec3 light_eye = normalize((sun_up_factor == 1) ? sun_dir : moon_dir);
    vec3 light_w = normalize(eye_to_world * light_eye);
    vec3 up_eye = vec3(eye_to_world[0].z, eye_to_world[1].z, eye_to_world[2].z);
    vec3 dir_eye = normalize(getPositionWithDepth(tc, 0.5).xyz);
    vec3 dir = normalize(eye_to_world * dir_eye);
    // Noise is read at the froxel's own footprint, so each texel holds the average over its cell.
    float pixel_angle = max(length(dFdx(dir_eye)), length(dFdy(dir_eye)));
    float cos_theta = dot(dir, light_w);

    vec3 sunlit;
    vec3 amblit;
    idCloudLighting(dir_eye, up_eye, light_eye, sunlit, amblit);

    float t0 = id_froxel_slice == 0 ? 0.0 : idFroxelSliceDist(id_froxel_slice - 1);
    float t1 = idFroxelSliceDist(id_froxel_slice);
    float ds = (t1 - t0) / float(ID_FROXEL_STEPS);

    vec3 scatter = vec3(0.0);
    float trans = 1.0;
    for (int i = 0; i < ID_FROXEL_STEPS; ++i)
    {
        float ts = t0 + (float(i) + 0.5) * ds;
        vec3 rel = dir * ts;
        float alt = idAltitude(rel);
        vec3 wp = vec3(id_camera_global.xy + rel.xy, alt);
        float footprint = max(max(ts * pixel_angle, 0.25 * ds), 1.0);
        float dens = idCloudDensityAll(wp, footprint, id_detail_noise == 0);
        if (dens <= 0.0)
        {
            continue;
        }
        float sigma = idCloudSigma(dens);
        vec3 light = idCloudSampleLight(rel, alt, ts, sigma, footprint, cos_theta, light_w, light_eye, tc, sunlit, amblit);
        float step_trans = exp(-sigma * ds);
        scatter += light * trans * (1.0 - step_trans);
        trans *= step_trans;
    }

    if (trans < 1.0)
    {
        vec3 haze_add;
        float haze_att;
        idHaze(dir_eye * (0.5 * (t0 + t1)), light_eye, haze_add, haze_att);
        scatter = scatter * haze_att + (1.0 - trans) * haze_add;
    }

    frag_color = vec4(clampHDRRange(scatter), trans);
}

#endif
