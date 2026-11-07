// Ground-truth ambient occlusion: horizon-based visibility for the sun light map's AO channel

// Jimenez, Wu, Pesce and Jarabo 2016, "Practical Real-Time Strategies for Accurate Indirect
// Occlusion". Linked as an extra fragment object into the deferred sun program when FS_GTAO is
// defined (FSRenderGTAO with RenderDeferredSSAO on); sunLightSSAOF.glsl then writes
// fsCalcGTAO in place of calcAmbientOcclusion (aoUtil.glsl).
//
// For a handful of slice directions around the view vector, march the depth buffer out to a
// world-space radius on both sides of the pixel and keep the highest horizon seen on each side.
// The two horizon angles bound the unoccluded arc of that slice, and the cosine-weighted
// integral of the arc against the normal projected into the slice has a closed form, so the
// visibility is the exact one for the heightfield the depth buffer describes rather than a
// count of samples that happened to land in front of the surface.
//
// Output is visibility in [0,1], 1 unoccluded, the same range and meaning the SSAO estimator
// writes into the green channel, so the light-map blur and the lighting pass are unchanged.
// The slice rotation and step offset are jittered per pixel for that blur to resolve. Multi-bounce is not applied here: the PBR lighting already lifts this term
// through gtaoMultiBounce (deferredUtil.glsl) with the surface albedo, and doing it twice
// would double-count.

#ifndef FS_GTAO_SLICES
#define FS_GTAO_SLICES 2
#endif

// Samples per side of each slice
#define FS_GTAO_STEPS 8

// Thickness heuristic (Jimenez 2016, section 4.3): a sample below the running horizon pulls it
// down by this fraction instead of being ignored. The depth buffer has no back faces, so every
// object looks infinitely thick; without this a thin pole or a railing occludes everything
// behind it out to the full radius.
#define FS_GTAO_THICKNESS 0.1

// Share of the radius over which an occluder's weight fades out (XeGTAO's default), so the
// occlusion has no hard edge where the search radius ends.
#define FS_GTAO_FALLOFF 0.615

#define FS_GTAO_PI      3.14159265
#define FS_GTAO_HALF_PI 1.57079633

uniform mat4 inv_proj;
uniform vec2 screen_res;
uniform float ssao_max_radius;   // RenderSSAOMaxScale: ceiling on the search radius in pixels
uniform float fs_gtao_radius;    // FSRenderGTAORadius: search radius in metres
uniform float fs_gtao_intensity; // FSRenderGTAOIntensity: exponent on the visibility

float getDepth(vec2 pos_screen);
bool isFarDepth(float d);
vec4 getPositionWithDepth(vec2 pos_screen, float depth);
float screenNoise(vec2 frag_px);

// acos to about 1e-3 radians (Lagarde), plenty for a horizon angle
float fsGTAOAcos(float x)
{
    float ax = abs(x);
    float r = (-0.156583 * ax + FS_GTAO_HALF_PI) * sqrt(max(1.0 - ax, 0.0));
    return x >= 0.0 ? r : FS_GTAO_PI - r;
}

// Fold one depth sample into the running horizon cosine of a slice side.
float fsGTAOHorizon(vec2 tc, vec3 pos, vec3 v, float low_cos, float falloff_mul, float falloff_add, float horizon_cos)
{
    // Off-screen and sky samples carry no occluder; their weight goes to zero rather than
    // branching, so the depth fetch stays in the same control flow for every pixel.
    vec2 ctc = clamp(tc, vec2(0.0), vec2(1.0));
    float d = getDepth(ctc);
    bool valid = ctc == tc && !isFarDepth(d);

    // A sky depth unprojects to infinity (a division by zero under reverse-Z), and a zero
    // weight times that is still NaN, so a dropped sample is unprojected at a finite depth.
    vec3 delta = getPositionWithDepth(ctc, valid ? d : 0.5).xyz - pos;
    float dist = max(length(delta), 1e-5);
    float sample_cos = dot(delta, v) / dist;

    float weight = valid ? clamp(dist * falloff_mul + falloff_add, 0.0, 1.0) : 0.0;
    sample_cos = mix(low_cos, sample_cos, weight);

    return sample_cos > horizon_cos ? sample_cos
                                    : max(mix(horizon_cos, sample_cos, FS_GTAO_THICKNESS), low_cos);
}

// pos: view-space position of the pixel; norm: view-space geometric normal (the depth buffer
// knows nothing of normal-mapped detail, so a shading normal would occlude a flat wall with its
// own texture); pos_screen: the pixel's [0,1] screen coordinate.
float fsCalcGTAO(vec3 pos, vec3 norm, vec2 pos_screen)
{
    vec3 v = normalize(-pos);
    vec3 n = normalize(norm);
    float view_depth = max(-pos.z, 1e-4);

    // World radius to screen. ndc.xy = P00 x / -z, P11 y / -z and inv_proj holds 1/P00, 1/P11.
    vec2 radius_tc = fs_gtao_radius * 0.5 / (view_depth * vec2(inv_proj[0][0], inv_proj[1][1]));
    float radius_px = radius_tc.y * screen_res.y;
    float max_px = max(ssao_max_radius, 1.0);
    float clamp_scale = min(max_px / max(radius_px, 1e-4), 1.0);
    radius_tc *= clamp_scale;
    radius_px *= clamp_scale;

    if (radius_px < 1.0)
    { // the whole search disc is inside this pixel
        return 1.0;
    }

    // Falloff on the radius actually searched once the pixel ceiling has shrunk it
    float radius = fs_gtao_radius * clamp_scale;
    float falloff_range = FS_GTAO_FALLOFF * radius;
    float falloff_mul = -1.0 / falloff_range;
    float falloff_add = (radius - falloff_range) / falloff_range + 1.0;

    // Interleaved gradient noise rotates the slices; a 4-value diagonal pattern offsets the
    // steps (3x is -x mod 4, keeping the operand non-negative). Neighbouring pixels then cover
    // different directions and distances for the blur to average.
    vec2 frag_px = gl_FragCoord.xy;
    float slice_noise = screenNoise(frag_px);
    float step_noise = 0.25 * float((int(frag_px.y) + 3 * int(frag_px.x)) & 3) + 0.125;

    // Do not sample the pixel itself
    float min_s = 1.3 / radius_px;

    float visibility = 0.0;

    for (int slice = 0; slice < FS_GTAO_SLICES; ++slice)
    {
        float phi = (float(slice) + slice_noise) * (FS_GTAO_PI / float(FS_GTAO_SLICES));
        vec2 omega = vec2(cos(phi), sin(phi));

        // The slice plane holds the view vector and this screen direction (view x/y run the
        // same way as screen x/y here).
        vec3 dir = vec3(omega, 0.0);
        vec3 ortho_dir = dir - dot(dir, v) * v;
        vec3 axis = normalize(cross(ortho_dir, v));
        vec3 proj_n = n - axis * dot(n, axis);
        float proj_n_len = length(proj_n);

        float sign_n = sign(dot(ortho_dir, proj_n));
        float cos_n = clamp(dot(proj_n, v) / max(proj_n_len, 1e-4), 0.0, 1.0);
        float n_ang = sign_n * fsGTAOAcos(cos_n);

        // Start each side at the tangent plane: anything below it cannot occlude.
        float low_cos0 = cos(n_ang + FS_GTAO_HALF_PI);
        float low_cos1 = cos(n_ang - FS_GTAO_HALF_PI);
        float horizon_cos0 = low_cos0;
        float horizon_cos1 = low_cos1;

        for (int i = 0; i < FS_GTAO_STEPS; ++i)
        {
            float s = (float(i) + step_noise) / float(FS_GTAO_STEPS);
            s = s * s + min_s; // denser near the pixel, where occluders matter most
            vec2 offset = omega * radius_tc * s;

            horizon_cos0 = fsGTAOHorizon(pos_screen + offset, pos, v, low_cos0, falloff_mul, falloff_add, horizon_cos0);
            horizon_cos1 = fsGTAOHorizon(pos_screen - offset, pos, v, low_cos1, falloff_mul, falloff_add, horizon_cos1);
        }

        // Horizon angles from the view vector, clamped to the normal's hemisphere
        float h0 = -fsGTAOAcos(horizon_cos1);
        float h1 =  fsGTAOAcos(horizon_cos0);
        h0 = n_ang + clamp(h0 - n_ang, -FS_GTAO_HALF_PI, FS_GTAO_HALF_PI);
        h1 = n_ang + clamp(h1 - n_ang, -FS_GTAO_HALF_PI, FS_GTAO_HALF_PI);

        // Cosine-weighted visible arc (Jimenez 2016, eq. 10), weighted by how much of the normal
        // lies in this slice
        float sin_n = sin(n_ang);
        float arc0 = cos_n + 2.0 * h0 * sin_n - cos(2.0 * h0 - n_ang);
        float arc1 = cos_n + 2.0 * h1 * sin_n - cos(2.0 * h1 - n_ang);
        visibility += proj_n_len * 0.25 * (arc0 + arc1);
    }

    visibility /= float(FS_GTAO_SLICES);

    return pow(clamp(visibility, 0.0, 1.0), max(fs_gtao_intensity, 0.05)); // pow(0, 0) is undefined
}
