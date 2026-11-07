/**
 * @file class3/deferred/idCloudsCompositeF.glsl
 * @brief <ID> Volumetric clouds: premultiplied over onto the lit scene, before haze.
 *
 * Part of Five's custom Firestorm fork.
 */

/*[EXTRA_CODE_HERE]*/

// Blended with ONE, SRC_ALPHA. The haze pass runs after this one and fogs the whole pixel by the
// distance of the geometry behind the cloud, so the in-scatter is pre-compensated here: after that
// pass the pixel holds the cloud's own fogged in-scatter over the fogged scene.

out vec4 frag_color;

in vec2 vary_fragcoord;

uniform sampler2D idCloudScene;

uniform vec3  sun_dir;
uniform vec3  moon_dir;
uniform int   sun_up_factor;
uniform vec4  waterPlane;
uniform float sky_hdr_scale;
uniform int   id_haze_compensate;

float getDepth(vec2 pos_screen);
vec4 getPositionWithDepth(vec2 pos_screen, float depth);
void calcAtmosphericVarsLinear(vec3 inPositionEye, vec3 norm, vec3 light_dir, out vec3 sunlit, out vec3 amblit, out vec3 additive, out vec3 atten);
vec3 srgb_to_linear(vec3 c);

void main()
{
    vec2 tc = vary_fragcoord.xy;
    vec4 cloud = texture(idCloudScene, tc);
    if (cloud.a >= 1.0)
    {
        discard;
    }

    vec3 scatter = cloud.rgb;
    float trans = cloud.a;
    float depth = getDepth(tc);

    if (id_haze_compensate != 0 && depth < 1.0)
    {
        vec3 pos = getPositionWithDepth(tc, depth).xyz;
        if (waterPlane.w > 0.0 || dot(pos, waterPlane.xyz) + waterPlane.w > 0.0)
        {
            vec3 light_dir = (sun_up_factor == 1) ? sun_dir : moon_dir;
            vec3 sunlit;
            vec3 amblit;
            vec3 additive;
            vec3 atten;
            calcAtmosphericVarsLinear(pos, vec3(0.0, 0.0, 1.0), light_dir, sunlit, amblit, additive, atten);
            vec3 haze_add = srgb_to_linear(additive * 2.0) * sky_hdr_scale;
            scatter = (scatter - (1.0 - trans) * haze_add) / max(atten.r, 0.02);
        }
    }

    scatter = mix(scatter, vec3(0.0), isnan(scatter));
    frag_color = vec4(clamp(scatter, vec3(-11.2), vec3(11.2)), trans);
}
