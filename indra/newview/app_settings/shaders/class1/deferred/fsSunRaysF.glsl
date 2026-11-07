// Sun rays: in-scattered sunlight along each view ray, shadowed by the sun shadow maps

/*[EXTRA_CODE_HERE]*/

// Written at reduced resolution into a single channel: how much sunlight is scattered toward
// the camera along the ray to this pixel, before the light's colour. fsSunRaysCompositeF
// filters it back up to full resolution and adds it to the scene.

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
uniform float ray_max_dist;     // meters, the march never goes past this or the shadow range
uniform float ray_density;      // scattering per meter
uniform float ray_anisotropy;   // 0 scatters evenly, toward 1 concentrates around the light

float getDepth(vec2 pos_screen);
vec4 getPositionWithDepth(vec2 pos_screen, float depth);
float idSampleCloudShadow(vec3 pos_eye); // <ID:VolumetricClouds/>

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

// Per pixel start offset, so neighbouring pixels sample between each other's steps and the
// composite's filter turns banding into an even result. Fixed per pixel, so still frames are
// stable.
float stepOffset(vec2 frag)
{
    return fract(52.9829189 * fract(dot(frag, vec2(0.06711056, 0.00583715))));
}

void main()
{
    vec2 tc = vary_fragcoord.xy;
    float depth = getDepth(tc);
    vec3 pos = getPositionWithDepth(tc, depth).xyz;

    vec3 view_dir = normalize(pos);
    float scene_dist = depth >= 1.0 ? ray_max_dist : length(pos);
    float march_dist = min(min(scene_dist, ray_max_dist), shadow_clip.w);

    if (march_dist <= 0.0)
    {
        frag_color = vec4(0.0);
        return;
    }

    int steps = max(ray_steps, 1);
    float step_len = march_dist / float(steps);
    float t = step_len * stepOffset(gl_FragCoord.xy);

    float scattered = 0.0;
    for (int i = 0; i < steps; ++i)
    {
        vec3 sample_pos = view_dir * t;
        // Light scattered at distance t reaches the camera dimmed by the same medium
        scattered += sunVisibility(sample_pos) * idSampleCloudShadow(sample_pos) * exp(-ray_density * t); // <ID:VolumetricClouds/>
        t += step_len;
    }

    vec3 light_dir = (sun_up_factor == 1) ? sun_dir : moon_dir;
    float cos_theta = dot(view_dir, normalize(light_dir));

    float result = scattered * step_len * ray_density * phase(cos_theta, ray_anisotropy);
    frag_color = vec4(result, 0.0, 0.0, 1.0);
}
