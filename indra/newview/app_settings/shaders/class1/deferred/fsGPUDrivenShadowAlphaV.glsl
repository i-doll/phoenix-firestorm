/**
 * @file fsGPUDrivenShadowAlphaV.glsl
 * @brief Alpha tested shadow casters drawn with glMultiDrawElementsIndirect (FSPerfGPUDrivenShadowAlpha)
 *
 * $LicenseInfo:firstyear=2026&license=viewerlgpl$
 * Phoenix Firestorm Viewer Source Code
 * Copyright (C) 2026, The Phoenix Firestorm Project, Inc.
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation;
 * version 2.1 of the License only.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301  USA
 *
 * The Phoenix Firestorm Project, Inc., 1831 Oakwood Drive, Fairmont, Minnesota 56031-3225 USA
 * http://www.firestormviewer.org
 * $/LicenseInfo$
 */

// The stand-in for shadowAlphaMaskV.glsl, treeShadowV.glsl and pbrShadowAlphaMaskV.glsl (not rigged) when
// the alpha tested shadow casters come out of a vertex arena in multi-draws (fsgpudrivenshadowalpha.cpp).
// Every draw command has a record, fetched as an instanced vertex attribute: the command's base instance
// points at it and the attribute divisor (4) is at least the instance count, so all instances of the command
// read the same record. It names the command's matrix (the model view projection the stock path would have
// put in modelview_projection_matrix, or for the single pass the model matrix), its texture matrix, its
// textures (the minimum alpha and texture count, then a bindless handle per texture unit the stock path would
// bind) and how the fragment shader tests it.

#extension GL_ARB_shader_storage_buffer_object : require

#ifdef FS_SHADOW_SINGLE_PASS
// Single pass sun shadow cascades (FSPerfShadowSinglePass), as in shadowAlphaMaskV.glsl: instance i draws
// the vertex into the atlas viewport of cascade fs_shadow_cascade[i].
#if FS_SHADOW_SINGLE_PASS == 2
#extension GL_NV_viewport_array2 : require
#elif FS_SHADOW_SINGLE_PASS == 3
#extension GL_AMD_vertex_shader_viewport_index : require
#else
#extension GL_ARB_shader_viewport_layer_array : require
#endif
uniform mat4 fs_shadow_clip[4];     // light projection * view of each cascade, from agent space
uniform vec4 fs_shadow_cascade;     // cascade drawn by each instance
uniform vec4 fs_shadow_width;       // shadow_target_width of each cascade
#else
uniform float shadow_target_width;
#endif

#ifdef RIGGED_PRECISE_MATH
invariant gl_Position;
precise gl_Position;
#endif

layout(std430) readonly buffer FSGDDShadowAlphaMatrices
{
    mat4 fs_gdd_matrix[];   // model view projection (single pass: model) matrices, texture matrices, GLTF base colour transforms
};

layout(std430) readonly buffer FSGDDShadowAlphaTextures
{
    uvec2 fs_gdd_tex[];     // per draw: (minimum_alpha bits, texture count), then a bindless handle per texture
};

in vec3 position;
in vec4 diffuse_color;
in vec2 texcoord0;
in int texture_index;       // the unit diffuseLookup would pick (indexedTextureV.glsl)
in uvec4 fs_gdd_draw;       // x: matrix, y: texture matrix, z: first entry in fs_gdd_tex,
                            // w: mode (bits 0-1), GLTF base colour transform matrix (bits 2-31)

out vec4 post_pos;
out float target_pos_x;
out vec4 vertex_color;
out vec2 vary_texcoord0;
flat out uvec2 fs_gdd_diffuse;
flat out float fs_gdd_minimum_alpha;
flat out uint fs_gdd_mode;

vec2 texture_transform(vec2 vertex_texcoord, vec4[2] khr_gltf_transform, mat4 sl_animation_transform);

void main()
{
    //transform vertex
    vec4 pre_pos = vec4(position.xyz, 1.0);
    vec4 pos = fs_gdd_matrix[fs_gdd_draw.x] * pre_pos;

#ifdef FS_SHADOW_SINGLE_PASS
    // the model matrix above leaves pos in agent space
    int cascade = int(fs_shadow_cascade[gl_InstanceID]);
    pos = fs_shadow_clip[cascade] * pos;
    gl_ViewportIndex = cascade;
    target_pos_x = 0.5 * (fs_shadow_width[cascade] - 1.0) * pos.x;
#else
    target_pos_x = 0.5 * (shadow_target_width - 1.0) * pos.x;
#endif

    post_pos = pos;

    gl_Position = pos;

    uint mode = fs_gdd_draw.w & 3u;
    mat4 texture_matrix0 = fs_gdd_matrix[fs_gdd_draw.y];
    if (mode == 3u)
    {   // pbrShadowAlphaMaskV.glsl: texture_base_color_transform is the first two columns of its matrix
        mat4 khr = fs_gdd_matrix[fs_gdd_draw.w >> 2u];
        vec4 texture_base_color_transform[2];
        texture_base_color_transform[0] = khr[0];
        texture_base_color_transform[1] = khr[1];
        vary_texcoord0 = texture_transform(texcoord0, texture_base_color_transform, texture_matrix0);
    }
    else
    {
        vary_texcoord0 = (texture_matrix0 * vec4(texcoord0,0,1)).xy;
    }

    uvec2 head = fs_gdd_tex[fs_gdd_draw.z];
    // an index past the draw's textures samples a unit left from an earlier draw on the stock path; the last one here
    uint index = min(uint(max(texture_index, 0)), head.y - 1u);
    fs_gdd_diffuse = fs_gdd_tex[fs_gdd_draw.z + 1u + index];
    fs_gdd_minimum_alpha = uintBitsToFloat(head.x);
    fs_gdd_mode = mode;

    vertex_color = diffuse_color;
}
