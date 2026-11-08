/**
 * @file fullbrightV.glsl
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

#ifdef FS_GDD_LEGACY
#extension GL_ARB_shader_storage_buffer_object : require
#endif

#ifdef RIGGED_PRECISE_MATH
invariant gl_Position;
precise gl_Position;
#endif

#ifdef FS_GDD_LEGACY
// FSPerfGPUDrivenLegacy (fsgpudrivenlegacy.cpp): static batches drawn with glMultiDrawElementsIndirect out
// of a vertex arena. Every draw command has a record, an instanced attribute its base instance selects,
// naming its matrices and its textures in the storage buffers below, which hold exactly what the stock
// path puts in the uniforms of the same names and binds on the texture units for that batch.
layout(std430) readonly buffer FSGDDLegacyMatrices
{
    mat4 fs_gdd_matrix[];   // per model matrix: modelview, modelview projection, normal matrix; texture matrices
};

layout(std430) readonly buffer FSGDDLegacyTextures
{
    uvec2 fs_gdd_tex[];     // per draw: (minimum_alpha bits, texture count), then a bindless handle per texture
};

in uvec4 fs_gdd_draw;       // x: first of the three model matrices, y: texture matrix, z: first entry in fs_gdd_tex
in int texture_index;       // the unit diffuseLookup would pick (indexedTextureV.glsl)

flat out uvec2 fs_gdd_diffuse;
flat out float fs_gdd_minimum_alpha;
#else
uniform mat4 texture_matrix0;
uniform mat4 modelview_matrix;
uniform mat4 modelview_projection_matrix;
#endif


in vec3 position;
void passTextureIndex();
in vec4 diffuse_color;
in vec2 texcoord0;

void calcAtmospherics(vec3 inPositionEye);

vec3 atmosAmbient();
vec3 atmosAffectDirectionalLight(float lightIntensity);

out vec3 vary_position;

out vec4 vertex_color;
out vec2 vary_texcoord0;

#ifdef HAS_SKIN
mat3x4 getSkinBlend();
vec4 skinTransformH(mat3x4 b, vec3 pos, mat4 m);
uniform mat4 projection_matrix;
#endif

void main()
{
#ifdef FS_GDD_LEGACY
    mat4 modelview_matrix = fs_gdd_matrix[fs_gdd_draw.x];
    mat4 modelview_projection_matrix = fs_gdd_matrix[fs_gdd_draw.x + 1u];
    mat4 texture_matrix0 = fs_gdd_matrix[fs_gdd_draw.y];
    uvec2 fs_gdd_head = fs_gdd_tex[fs_gdd_draw.z];
    // an index past the draw's textures samples a unit left from an earlier draw on the stock path; the last one here
    uint fs_gdd_index = min(uint(max(texture_index, 0)), fs_gdd_head.y - 1u);
    fs_gdd_diffuse = fs_gdd_tex[fs_gdd_draw.z + 1u + fs_gdd_index];
    fs_gdd_minimum_alpha = uintBitsToFloat(fs_gdd_head.x);
#endif

    //transform vertex
    vec4 vert = vec4(position.xyz, 1.0);
    passTextureIndex();

#ifdef HAS_SKIN
    vec4 pos = skinTransformH(getSkinBlend(), vert.xyz, modelview_matrix);
    gl_Position = projection_matrix * pos;
#else
    vec4 pos = (modelview_matrix * vert);
    gl_Position = modelview_projection_matrix*vec4(position.xyz, 1.0);
#endif

    vary_position = pos.xyz;

    vary_texcoord0 = (texture_matrix0 * vec4(texcoord0,0,1)).xy;

    calcAtmospherics(pos.xyz);

    vertex_color = diffuse_color;
}
