/**
 * @file fsGPUDrivenShadowV.glsl
 * @brief Depth only shadow casters drawn with glMultiDrawElementsIndirect (FSPerfGPUDrivenDraws)
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

// The stand-in for shadowV.glsl when the opaque casters come out of the position arena in one
// multi-draw. Every draw command has a record, fetched as an instanced vertex attribute: the command's
// base instance points at it and the attribute divisor (4) is at least the instance count, so all
// instances of the command read the same record. The record names the command's matrix in
// fs_gdd_matrix: the model view projection the stock path would have put in modelview_projection_matrix
// for that batch, computed the same way. (gl_DrawIDARB would do the same, but needs
// GL_ARB_shader_draw_parameters; the base instance works on every OpenGL 4.3 driver.)

#extension GL_ARB_shader_storage_buffer_object : require

#ifdef FS_SHADOW_SINGLE_PASS
// Single pass sun shadow cascades (FSPerfShadowSinglePass), as in shadowV.glsl: instance i draws the
// vertex into the atlas viewport of cascade fs_shadow_cascade[i]. The matrices hold only the model matrix.
#if FS_SHADOW_SINGLE_PASS == 2
#extension GL_NV_viewport_array2 : require
#elif FS_SHADOW_SINGLE_PASS == 3
#extension GL_AMD_vertex_shader_viewport_index : require
#else
#extension GL_ARB_shader_viewport_layer_array : require
#endif
uniform mat4 fs_shadow_clip[4];     // light projection * view of each cascade, from agent space
uniform vec4 fs_shadow_cascade;     // cascade drawn by each instance
#endif

layout(std430) readonly buffer FSGDDMatrices
{
    mat4 fs_gdd_matrix[];
};

in vec3 position;
in uint fs_gdd_draw;                // per draw command: index into fs_gdd_matrix

void main()
{
    mat4 mat = fs_gdd_matrix[fs_gdd_draw];
#ifdef FS_SHADOW_SINGLE_PASS
    int cascade = int(fs_shadow_cascade[gl_InstanceID]);
    gl_Position = fs_shadow_clip[cascade] * (mat * vec4(position.xyz, 1.0));
    gl_ViewportIndex = cascade;
#else
    gl_Position = mat * vec4(position.xyz, 1.0);
#endif
}
