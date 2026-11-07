/**
 * @file treeShadowV.glsl
 *
  * $LicenseInfo:firstyear=2007&license=viewerlgpl$
 * Second Life Viewer Source Code
 * Copyright (C) 2007, Linden Research, Inc.
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

uniform mat4 texture_matrix0;
uniform mat4 modelview_projection_matrix;

in vec3 position;
in vec2 texcoord0;

out vec2 vary_texcoord0;

void main()
{
    //transform vertex
#ifdef FS_SHADOW_SINGLE_PASS
    // modelview_projection_matrix holds only the model matrix here
    int cascade = int(fs_shadow_cascade[gl_InstanceID]);
    gl_Position = fs_shadow_clip[cascade] * (modelview_projection_matrix * vec4(position.xyz, 1.0));
    gl_ViewportIndex = cascade;
#else
    gl_Position = modelview_projection_matrix*vec4(position.xyz, 1.0);
#endif

    vary_texcoord0 = (texture_matrix0 * vec4(texcoord0,0,1)).xy;
}
