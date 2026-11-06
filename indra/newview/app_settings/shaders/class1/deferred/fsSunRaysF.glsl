// Sun rays: in-scattered sunlight along each view ray, shadowed by the sun shadow maps

/*[EXTRA_CODE_HERE]*/

// Written at reduced resolution into two channels: r is how much sunlight is scattered toward
// the camera along the ray to this pixel, before the light's colour, g is the distance to what
// this pixel sees. fsSunRaysCompositeF uses g to filter r back up to full resolution without
// smearing it across silhouettes, then adds it to the scene.

out vec4 frag_color;

in vec2 vary_fragcoord;

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

uniform int ray_steps;
uniform float ray_max_dist;         // meters, the march never goes past this or the shadow range
uniform float ray_density;          // scattering per meter, at the camera's height
uniform float ray_anisotropy;       // 0 scatters evenly, toward 1 concentrates around the light
uniform float ray_height_falloff;   // per meter of height above the camera, 0 for even air
uniform vec3 ray_up;                // world up, in view space

// Distance stored for the sky. Fits a half float.
const float SKY_DIST = 65000.0;

float getDepth(vec2 pos_screen);
vec4 getPositionWithDepth(vec2 pos_screen, float depth);

// <ID:VolumetricClouds>
#ifdef ID_VOLUMETRIC_CLOUDS
float idSampleCloudShadow(vec3 pos_eye);
#endif
// </ID:VolumetricClouds>

// Lit (1) or shadowed (0) at a view space position, one comparison tap in the nearest cascade
// that covers it. Beyond the last cascade counts as lit.
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
        vis = texture(shadowMap0, vec3(lpos.xy, lpos.z + shadow_bias));
    }
    else if (dist < shadow_clip.y)
    {
        lpos = shadow_matrix[1] * p;
        lpos.xyz /= lpos.w;
        vis = texture(shadowMap1, vec3(lpos.xy, lpos.z + shadow_bias));
    }
    else if (dist < shadow_clip.z)
    {
        lpos = shadow_matrix[2] * p;
        lpos.xyz /= lpos.w;
        vis = texture(shadowMap2, vec3(lpos.xy, lpos.z + shadow_bias));
    }
    else if (dist < shadow_clip.w)
    {
        lpos = shadow_matrix[3] * p;
        lpos.xyz /= lpos.w;
        vis = texture(shadowMap3, vec3(lpos.xy, lpos.z + shadow_bias));
    }
    return vis;
}

// Henyey-Greenstein, scaled so that even scattering is 1
float phase(float cos_theta, float g)
{
    float g2 = g * g;
    float denom = 1.0 + g2 - 2.0 * g * cos_theta;
    return (1.0 - g2) / (denom * sqrt(denom));
}

// Per pixel start offset (interleaved gradient noise), so neighbouring pixels sample between
// each other's steps and the composite's 3x3 filter turns banding into an even result. Fixed
// per pixel, so still frames are stable.
float stepOffset(vec2 frag)
{
    return fract(52.9829189 * fract(dot(frag, vec2(0.06711056, 0.00583715))));
}

// Air density relative to the camera's height, h meters above it. Capped below the camera so
// looking down from high up doesn't blow out.
float heightScale(float h)
{
    return exp(min(-ray_height_falloff * h, 1.3862944)); // ln 4
}

void main()
{
    vec2 tc = vary_fragcoord.xy;
    float depth = getDepth(tc);
    bool sky = depth >= 1.0;

    // The sky's far plane may be at infinity, so take the direction from a point in front of it
    vec3 pos = getPositionWithDepth(tc, sky ? 0.5 : depth).xyz;
    vec3 view_dir = normalize(pos);
    float scene_dist = sky ? SKY_DIST : min(length(pos), SKY_DIST);
    float march_dist = min(min(scene_dist, ray_max_dist), shadow_clip.w);

    if (march_dist <= 0.0)
    {
        frag_color = vec4(0.0, scene_dist, 0.0, 1.0);
        return;
    }

    // Steps are spaced quadratically: close to the camera, where shafts are large on screen,
    // they are fine, and they spread out with distance where the shadow cascades are coarse
    // anyway. Each sample at u along [0, 1] covers 2 u march_dist / steps meters.
    int steps = max(ray_steps, 1);
    float inv_steps = 1.0 / float(steps);
    float jitter = stepOffset(gl_FragCoord.xy);
    float rise = dot(view_dir, ray_up);    // meters gained in height per meter along the ray

    float scattered = 0.0;
    float optical_depth = 0.0;
    float prev_t = 0.0;
    for (int i = 0; i < steps; ++i)
    {
        float u = (float(i) + jitter) * inv_steps;
        float t = march_dist * u * u;
        float dt = 2.0 * march_dist * u * inv_steps;
        float sigma = ray_density * heightScale(t * rise);

        // Light scattered at distance t reaches the camera dimmed by the air in between
        optical_depth += sigma * (t - prev_t);
        prev_t = t;
        vec3 sample_pos = view_dir * t;
        // <ID:VolumetricClouds>
#ifdef ID_VOLUMETRIC_CLOUDS
        scattered += sunVisibility(sample_pos) * idSampleCloudShadow(sample_pos) * sigma * exp(-optical_depth) * dt;
#else
        scattered += sunVisibility(sample_pos) * sigma * exp(-optical_depth) * dt;
#endif
        // </ID:VolumetricClouds>
    }

    vec3 light_dir = (sun_up_factor == 1) ? sun_dir : moon_dir;
    float cos_theta = dot(view_dir, normalize(light_dir));

    float result = scattered * phase(cos_theta, ray_anisotropy);
    frag_color = vec4(result, scene_dist, 0.0, 1.0);
}
