// Volumetric projector light: light from one projector scattered toward the camera by the air
// inside its cone, shadowed by its shadow map when it has one and coloured by its texture.

/*[EXTRA_CODE_HERE]*/

// Drawn once per light at reduced resolution, added together: rgb is the light scattered toward
// the camera along the ray to this pixel, a is the distance to what this pixel sees (written by
// the first light only, the others leave alpha alone). fsSpotVolumetricCompositeF uses a to
// filter rgb back up to full resolution without smearing it across silhouettes, the same way
// fsSunRaysCompositeF does for the sun rays.

out vec4 frag_color;

in vec2 vary_fragcoord;

uniform sampler2DShadow shadowMap0;     // this light's shadow map, when vol_shadowed is set
uniform mat4 vol_shadow_matrix;         // view space to that map, [0, 1]
uniform int vol_shadowed;
uniform float vol_shadow_fade;          // 1 - the shadow's fade in, as spotLightF's shadow_fade
uniform float spot_shadow_bias;

uniform mat4 proj_mat;      // view space to the projector's texture space
uniform vec3 proj_n;        // projector axis, view space
uniform vec3 proj_origin;   // apex of the projector's frustum, view space
uniform vec3 center;        // the light's position, view space
uniform float size;         // the light's range
uniform float falloff;

uniform int vol_steps;
uniform float vol_density;      // scattering per metre
uniform float vol_anisotropy;   // 0 scatters evenly, toward 1 concentrates looking into the light

// Distance stored for the sky. Fits a half float.
const float SKY_DIST = 65000.0;

float getDepth(vec2 pos_screen);
vec4 getPositionWithDepth(vec2 pos_screen, float depth);
float calcLegacyDistanceAttenuation(float distance, float falloff);
vec3 getProjectedLightDiffuseColor(float light_distance, vec2 projected_uv);

// Henyey-Greenstein, scaled so that even scattering is 1 (as in fsSunRaysF)
float phase(float cos_theta, float g)
{
    float g2 = g * g;
    float denom = 1.0 + g2 - 2.0 * g * cos_theta;
    // Capped: near g = 1 the peak grows without bound and blows out bloom
    return min((1.0 - g2) / (denom * sqrt(denom)), 16.0);
}

// Per pixel start offset (interleaved gradient noise, as in fsSunRaysF): neighbouring pixels
// sample between each other's steps and the composite's filter evens it out
float stepOffset(vec2 frag)
{
    return fract(52.9829189 * fract(dot(frag, vec2(0.06711056, 0.00583715))));
}

void main()
{
    vec2 tc = vary_fragcoord.xy;
    float depth = getDepth(tc);
    bool sky = depth >= 1.0;

    vec3 pos = getPositionWithDepth(tc, sky ? 0.5 : depth).xyz;
    vec3 view_dir = normalize(pos);
    float scene_dist = sky ? SKY_DIST : min(length(pos), SKY_DIST);

    // The part of the view ray inside the light's range (a sphere around it): nothing to march
    // for most of the screen
    float b = dot(view_dir, center);
    float c = dot(center, center) - size * size;
    float disc = b * b - c;
    vec3 result = vec3(0.0);
    if (disc > 0.0)
    {
        float h = sqrt(disc);
        float t0 = max(b - h, 0.0);
        float t1 = min(b + h, scene_dist);
        if (t1 > t0)
        {
            int steps = max(vol_steps, 1);
            float dt = (t1 - t0) / float(steps);
            float jitter = stepOffset(gl_FragCoord.xy);
            for (int i = 0; i < steps; ++i)
            {
                vec3 p = view_dir * (t0 + (float(i) + jitter) * dt);

                // inside the projector's frustum, in front of it
                vec4 proj_tc = proj_mat * vec4(p, 1.0);
                if (proj_tc.z < 0.0)
                {
                    continue;
                }
                proj_tc.xyz /= proj_tc.w;
                if (proj_tc.x <= 0.0 || proj_tc.x >= 1.0 || proj_tc.y <= 0.0 || proj_tc.y >= 1.0)
                {
                    continue;
                }

                vec3 lv = center - p;
                float dist_atten = calcLegacyDistanceAttenuation(length(lv) / size, falloff);
                if (dist_atten <= 0.0)
                {
                    continue;
                }

                float vis = 1.0;
                if (vol_shadowed != 0)
                {
                    vec4 lpos = vol_shadow_matrix * vec4(p, 1.0);
                    lpos.xyz /= lpos.w;
                    vis = texture(shadowMap0, vec3(lpos.xy, lpos.z + spot_shadow_bias * 0.8));
                    vis = clamp(vis + vol_shadow_fade, 0.0, 1.0);
                }

                // colour and texture (gobo) of the light at this point, as the projector lights
                // a surface there
                vec3 light = getProjectedLightDiffuseColor(-dot(lv, proj_n), proj_tc.xy);

                float cos_theta = dot(view_dir, normalize(proj_origin - p));
                result += light * (dist_atten * vis * phase(cos_theta, vol_anisotropy));
            }
            // light scattered over the march, dimmed by the air between it and the camera
            result *= vol_density * dt * exp(-vol_density * t0);
        }
    }

    frag_color = vec4(max(result, vec3(0.0)), scene_dist);
}
