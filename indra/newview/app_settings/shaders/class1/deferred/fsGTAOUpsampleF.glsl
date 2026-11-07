// Joint bilateral upsample of the half-resolution GTAO (FSRenderGTAOHalfRes)

// Linked into the deferred sun program in place of fsGTAOF.glsl when the half-resolution pass
// is on, so sunLightSSAOF.glsl's FS_GTAO hook calls this fsCalcGTAO unchanged and the result
// lands in the light map's green channel, blurred and read exactly as before.
//
// The four half-resolution texels around the pixel are weighted bilinearly, times how well each
// one's depth and geometric normal (stored by fsGTAOHalfF.glsl) match the pixel's own. A texel
// from the other side of a silhouette or a crease then carries almost no weight, so occlusion
// does not bleed across edges. If none of them matches (a thin feature that fell between the
// half-resolution samples), the closest match is taken alone.

uniform sampler2D occlusionMap; // half-resolution GTAO: r visibility, g view depth, ba normal

// Relative depth difference counted as one unit of mismatch, face-on and at grazing angles,
// where the depth changes fastest across the two-pixel footprint of a texel
#define FS_GTAO_UP_DEPTH_TOL_FACING  0.01
#define FS_GTAO_UP_DEPTH_TOL_GRAZING 0.06

vec2 fsGTAOUpSignNotZero(vec2 v)
{
    return vec2(v.x >= 0.0 ? 1.0 : -1.0, v.y >= 0.0 ? 1.0 : -1.0);
}

vec3 fsGTAOOctDecode(vec2 e)
{
    vec3 n = vec3(e, 1.0 - abs(e.x) - abs(e.y));
    if (n.z < 0.0)
    {
        n.xy = (1.0 - abs(n.yx)) * fsGTAOUpSignNotZero(n.xy);
    }
    return normalize(n);
}

// Same signature as fsGTAOF.glsl's: pos is the view-space position, norm the view-space
// geometric normal, pos_screen unused (the light map is full resolution, gl_FragCoord locates
// the pixel).
float fsCalcGTAO(vec3 pos, vec3 norm, vec2 pos_screen)
{
    ivec2 lo_max = textureSize(occlusionMap, 0) - 1;

    // Texel t covers full pixels 2t and 2t+1, so its centre sits at full coordinate 2t + 1
    vec2 c = gl_FragCoord.xy * 0.5 - 0.5;
    vec2 base_f = floor(c);
    vec2 f = c - base_f;
    ivec2 base = ivec2(base_f);

    vec3 n = normalize(norm);
    float z = max(-pos.z, 1e-4);
    float facing = clamp(dot(n, normalize(-pos)), 0.0, 1.0);
    float inv_tol = 1.0 / (z * mix(FS_GTAO_UP_DEPTH_TOL_GRAZING, FS_GTAO_UP_DEPTH_TOL_FACING, facing));

    float sum_w = 0.0;
    float sum_ao = 0.0;
    float best_score = 1e30;
    float best_ao = 1.0;

    for (int i = 0; i < 4; ++i)
    {
        ivec2 o = ivec2(i & 1, i >> 1);
        vec4 s = texelFetch(occlusionMap, clamp(base + o, ivec2(0), lo_max), 0);
        if (s.g < 0.0)
        {
            continue; // only sky under that texel
        }

        float rel = abs(s.g - z) * inv_tol;
        float nd = max(dot(fsGTAOOctDecode(s.ba), n), 0.0);
        float w_n = nd * nd;
        w_n *= w_n;
        w_n *= w_n; // nd^8

        vec2 bil = mix(vec2(1.0) - f, f, vec2(o));
        float w = bil.x * bil.y * w_n / (1.0 + rel * rel);
        sum_w += w;
        sum_ao += w * s.r;

        float score = rel + 4.0 * (1.0 - nd);
        if (score < best_score)
        {
            best_score = score;
            best_ao = s.r;
        }
    }

    return sum_w > 1e-4 ? sum_ao / sum_w : best_ao;
}
