/**
 * @file shadowAlphaMaskV.glsl
 *
 * $LicenseInfo:firstyear=2011&license=viewerlgpl$
 * Second Life Viewer Source Code
 * Copyright (C) 2011, Linden Research, Inc.
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
 * Linden Research, Inc., 945 Battery Street, San Francisco, CA  94111  USA
 * $/LicenseInfo$
 */

#ifdef FS_SHADOW_SINGLE_PASS
// Single pass sun shadow cascades (FSPerfShadowSinglePass): instance i draws the vertex into the
// atlas viewport of cascade fs_shadow_cascade[i], transformed by that cascade's light matrices.
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

#ifdef RIGGED_PRECISE_MATH
invariant gl_Position;
precise gl_Position;
#endif

uniform mat4 texture_matrix0;
#if defined(HAS_SKIN)
uniform mat4 modelview_matrix;
uniform mat4 projection_matrix;
mat3x4 getSkinBlend();
vec4 skinTransformH(mat3x4 b, vec3 pos, mat4 m);
#else
uniform mat4 modelview_projection_matrix;
#endif

uniform float shadow_target_width;
#ifdef FS_SHADOW_SINGLE_PASS
uniform vec4 fs_shadow_width;       // shadow_target_width of each cascade
#endif

in vec3 position;
in vec4 diffuse_color;
in vec2 texcoord0;

out vec4 post_pos;
out float target_pos_x;
out vec4 vertex_color;
out vec2 vary_texcoord0;

void passTextureIndex();

void main()
{
    //transform vertex
#if defined(HAS_SKIN)
    vec4 pos = skinTransformH(getSkinBlend(), position.xyz, modelview_matrix);
#ifndef FS_SHADOW_SINGLE_PASS
    pos = projection_matrix * pos;
#endif
#else
    vec4 pre_pos = vec4(position.xyz, 1.0);
    vec4 pos = modelview_projection_matrix * pre_pos;
#endif

#ifdef FS_SHADOW_SINGLE_PASS
    // the model matrices above leave pos in agent space
    int cascade = int(fs_shadow_cascade[gl_InstanceID]);
    pos = fs_shadow_clip[cascade] * pos;
    gl_ViewportIndex = cascade;
    target_pos_x = 0.5 * (fs_shadow_width[cascade] - 1.0) * pos.x;
#else
    target_pos_x = 0.5 * (shadow_target_width - 1.0) * pos.x;
#endif

    post_pos = pos;

    gl_Position = pos;

    passTextureIndex();

    vary_texcoord0 = (texture_matrix0 * vec4(texcoord0,0,1)).xy;
    vertex_color = diffuse_color;
}
