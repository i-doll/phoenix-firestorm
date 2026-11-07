// Batched deferred point lights: one instance per light, drawn as its box volume

// The vertex half of gFSBatchedPointLightProgram (FSPerfBatchedLocalLights); the fragment half is
// pointLightF.glsl built with FS_BATCHED_LIGHTS, so the lighting maths is the stock point
// light's, only the light parameters arrive as flat varyings instead of uniforms.
//
// fspointlightbatch.cpp uploads every queued light once per frame and binds 512 at a time to
// the FSPointLights block; gl_InstanceID picks the light. The vertex buffer is an unindexed
// [-1,1] cube, six faces of six vertices in the order of fs_face_normal. The stock path draws
// only the three faces toward the camera (get_box_fan_indices); here every face is submitted
// and the ones the camera is not in front of are collapsed outside the clip volume, which is
// independent of winding and of the face culling state. Only lights the camera is outside the
// box of come here, as on the stock path, so at least one face survives.

uniform mat4 modelview_projection_matrix;
uniform mat4 modelview_matrix;

uniform vec3 fs_camera_origin; // agent space, like the light centres

#define FS_LIGHTS_PER_DRAW 512

layout(std140) uniform FSPointLights
{
    // [2i]   centre (agent space) and volume half size
    // [2i+1] linear colour and falloff
    vec4 fs_light_data[FS_LIGHTS_PER_DRAW * 2];
};

in vec3 position;

out vec4 vary_fragcoord;
flat out vec3 trans_center;
flat out vec4 fs_light_color;
flat out float fs_light_size;

const vec3 fs_face_normal[6] = vec3[6](vec3(-1.0, 0.0, 0.0), vec3(1.0, 0.0, 0.0),
                                       vec3(0.0, -1.0, 0.0), vec3(0.0, 1.0, 0.0),
                                       vec3(0.0, 0.0, -1.0), vec3(0.0, 0.0, 1.0));

void main()
{
    vec4 center_size = fs_light_data[gl_InstanceID * 2];
    vec4 color_falloff = fs_light_data[gl_InstanceID * 2 + 1];

    vec3 center = center_size.xyz;
    float size = center_size.w;

    trans_center = (modelview_matrix * vec4(center, 1.0)).xyz;
    fs_light_color = color_falloff;
    fs_light_size = size;

    // Camera in front of this face's plane: distance past the face along its outward normal.
    vec3 n = fs_face_normal[gl_VertexID / 6];
    if (dot(n, fs_camera_origin - center) <= size)
    {
        // All six vertices of the face land on the same point beyond the far plane:
        // zero area and clipped, nothing is rasterised.
        vary_fragcoord = vec4(0.0, 0.0, 2.0, 1.0);
        gl_Position = vary_fragcoord;
        return;
    }

    vec3 p = position * size + center;
    vec4 pos = modelview_projection_matrix * vec4(p, 1.0);
    vary_fragcoord = pos;
    gl_Position = pos;
}
