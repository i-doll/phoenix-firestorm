/**
 * @file class3/deferred/idCloudShadowF.glsl
 * @brief <ID> Volumetric clouds: top-down map of sunlight transmittance through each cloud layer.
 *
 * Part of Five's custom Firestorm fork.
 */

/*[EXTRA_CODE_HERE]*/

out vec4 frag_color;

in vec2 vary_fragcoord;

uniform int   id_layer_count;
uniform float id_layer_base[4];
uniform float id_layer_top[4];

uniform vec2  id_cloud_shadow_center;   // global xy of the map centre, wrapped like id_camera_global
uniform float id_cloud_shadow_size;     // metres across
uniform vec2  id_cloud_shadow_drift;    // horizontal metres toward the light per metre of height

float idCloudDensity(vec3 world_pos, int layer, float footprint_m, bool cheap);
float idCloudSigma(float density);

const int ID_SHADOW_STEPS = 32;

// Each texel is the light ray that crosses sea level at that point: the map is a parallel
// projection along the light onto the sea-level plane.
void main()
{
    vec2 xy = id_cloud_shadow_center + (vary_fragcoord - 0.5) * id_cloud_shadow_size;
    float footprint = id_cloud_shadow_size * abs(dFdx(vary_fragcoord.x));

    vec2 drift = id_cloud_shadow_drift;
    float path_per_metre = sqrt(1.0 + dot(drift, drift));

    int count = min(id_layer_count, 4);
    int steps = max(ID_SHADOW_STEPS / max(count, 1), 1);

    vec4 trans = vec4(1.0);
    for (int i = 0; i < count; ++i)
    {
        float od = 0.0;
        float base = id_layer_base[i];
        float dz = (id_layer_top[i] - base) / float(steps);
        float ds = dz * path_per_metre;
        for (int j = 0; j < steps; ++j)
        {
            float z = base + (float(j) + 0.5) * dz;
            od += idCloudSigma(idCloudDensity(vec3(xy + drift * z, z), i, footprint, true)) * ds;
        }
        trans[i] = exp(-od);
    }

    frag_color = trans;
}
