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
// multi-draw. Every instance of every draw command has a record, fetched as an instanced vertex
// attribute with divisor 1: the command's base instance points at its first record and instance i
// reads the record after it, which is the plain base instance plus divisor 1 form every OpenGL 4.3
// driver handles (gl_DrawIDARB would need GL_ARB_shader_draw_parameters). The record names the
// matrix in fs_gdd_matrix: the model view projection the stock path would have put in
// modelview_projection_matrix for that batch, computed the same way; and, single pass, the cascade.
//
// Nothing here reads gl_InstanceID: with a non-zero base instance some drivers have offset it by the
// base instance, and divisors other than 1 combined with a base instance are rarely exercised, so
// the record carries everything the instance needs.

#extension GL_ARB_shader_storage_buffer_object : require

#ifdef FS_SHADOW_SINGLE_PASS
// Single pass sun shadow cascades (FSPerfShadowSinglePass), as in shadowV.glsl: each instance draws
// the vertex into the atlas viewport of the cascade its record names. The matrices hold only the
// model matrix.
#if FS_SHADOW_SINGLE_PASS == 2
#extension GL_NV_viewport_array2 : require
#elif FS_SHADOW_SINGLE_PASS == 3
#extension GL_AMD_vertex_shader_viewport_index : require
#else
#extension GL_ARB_shader_viewport_layer_array : require
#endif
uniform mat4 fs_shadow_clip[4];     // light projection * view of each cascade, from agent space
#endif

layout(std430) readonly buffer FSGDDMatrices
{
    mat4 fs_gdd_matrix[];
};

in vec3 position;
in uvec2 fs_gdd_draw;               // per instance of a draw command: x index into fs_gdd_matrix, y cascade

void main()
{
    mat4 mat = fs_gdd_matrix[fs_gdd_draw.x];
#ifdef FS_SHADOW_SINGLE_PASS
    int cascade = int(min(fs_gdd_draw.y, 3u));
    gl_Position = fs_shadow_clip[cascade] * (mat * vec4(position.xyz, 1.0));
    gl_ViewportIndex = cascade;
#else
    gl_Position = mat * vec4(position.xyz, 1.0);
#endif
}
