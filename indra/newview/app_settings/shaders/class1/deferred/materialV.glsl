/**
 * @file materialV.glsl
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

#ifdef FS_GDD_MATERIAL
#extension GL_ARB_shader_storage_buffer_object : require
#endif

#ifdef RIGGED_PRECISE_MATH
invariant gl_Position;
precise gl_Position;
#endif

#define DIFFUSE_ALPHA_MODE_IGNORE 0
#define DIFFUSE_ALPHA_MODE_BLEND 1
#define DIFFUSE_ALPHA_MODE_MASK 2
#define DIFFUSE_ALPHA_MODE_EMISSIVE 3

#ifdef FS_GDD_MATERIAL
// FSPerfGPUDrivenMaterials (fsgpudrivenmaterials.cpp): static batches of the legacy material passes drawn
// with glMultiDrawElementsIndirect out of a vertex arena. Every draw command has a record, an instanced
// attribute its base instance selects, naming its matrices and its material in the storage buffers below,
// which hold exactly what the stock path puts in the uniforms of the same names and binds on the texture
// units for that batch. Never skinned.
layout(std430) readonly buffer FSGDDMaterialMatrices
{
    mat4 fs_gdd_matrix[];   // per model matrix: modelview, modelview projection, normal matrix; texture matrices
};

struct FSGDDLegacyMaterial
{
    vec4 specular_color;    // specular_color
    vec4 params;            // env_intensity, minimum_alpha, emissive_brightness
    uvec4 tex01;            // bindless handles: diffuseMap, bumpMap
    uvec4 tex2;             // specularMap
};

layout(std430) readonly buffer FSGDDMaterialRecords
{
    FSGDDLegacyMaterial fs_gdd_material[];
};

in uvec4 fs_gdd_draw;       // x: first of the three model matrices, y: texture matrix, z: material

// the material for the fragment shader, which reads it in place of its uniforms and samplers
flat out vec4 fs_gdd_specular_color;
flat out vec3 fs_gdd_params;
flat out uvec4 fs_gdd_tex01;
flat out uvec2 fs_gdd_tex2;
#else
uniform mat4 modelview_matrix;
uniform mat4 modelview_projection_matrix;
#endif
uniform mat4 projection_matrix;

#ifdef HAS_SKIN
mat3x4 getSkinBlend();
vec3 skinNormal(mat3x4 b, vec3 pos, vec3 dir, mat4 m);
vec4 skinTransformH(mat3x4 b, vec3 pos, mat4 m);
#elif !defined(FS_GDD_MATERIAL)
uniform mat3 normal_matrix;
#endif

out vec3 vary_position;

#ifndef FS_GDD_MATERIAL
uniform mat4 texture_matrix0;
#endif

in vec3 position;
in vec4 diffuse_color;
in vec3 normal;
in vec2 texcoord0;


#ifdef HAS_NORMAL_MAP
in vec4 tangent;
in vec2 texcoord1;

out vec3 vary_tangent;
flat out float vary_sign;
out vec3 vary_normal;

out vec2 vary_texcoord1;
#else
out vec3 vary_normal;
#endif

#ifdef HAS_SPECULAR_MAP
in vec2 texcoord2;
out vec2 vary_texcoord2;
#endif

out vec4 vertex_color;
out vec2 vary_texcoord0;

void main()
{
#ifdef FS_GDD_MATERIAL
    mat4 modelview_matrix = fs_gdd_matrix[fs_gdd_draw.x];
    mat4 modelview_projection_matrix = fs_gdd_matrix[fs_gdd_draw.x + 1u];
    mat3 normal_matrix = mat3(fs_gdd_matrix[fs_gdd_draw.x + 2u]);
    mat4 texture_matrix0 = fs_gdd_matrix[fs_gdd_draw.y];
    FSGDDLegacyMaterial fs_gdd_mat = fs_gdd_material[fs_gdd_draw.z];
    fs_gdd_specular_color = fs_gdd_mat.specular_color;
    fs_gdd_params = fs_gdd_mat.params.xyz;
    fs_gdd_tex01 = fs_gdd_mat.tex01;
    fs_gdd_tex2 = fs_gdd_mat.tex2.xy;
#endif

#ifdef HAS_SKIN
    mat3x4 skin = getSkinBlend();
    vec3 pos = skinTransformH(skin, position.xyz, modelview_matrix).xyz;

    vary_position = pos;

    gl_Position = projection_matrix*vec4(pos,1.0);

#else
    //transform vertex
    gl_Position = modelview_projection_matrix * vec4(position.xyz, 1.0);

#endif

    vary_texcoord0 = (texture_matrix0 * vec4(texcoord0,0,1)).xy;

#ifdef HAS_NORMAL_MAP
    vary_texcoord1 = (texture_matrix0 * vec4(texcoord1,0,1)).xy;
#endif

#ifdef HAS_SPECULAR_MAP
    vary_texcoord2 = (texture_matrix0 * vec4(texcoord2,0,1)).xy;
#endif

#ifdef HAS_SKIN
    vec3 n = normalize(skinNormal(skin, position.xyz, normal.xyz, modelview_matrix));
#ifdef HAS_NORMAL_MAP
    vec3 t = normalize(skinNormal(skin, position.xyz, tangent.xyz, modelview_matrix));

    vary_tangent = t;
    vary_sign = tangent.w;
    vary_normal = n;
#else //HAS_NORMAL_MAP
    vary_normal  = n;
#endif //HAS_NORMAL_MAP
#else //HAS_SKIN
    vec3 n = normalize(normal_matrix * normal);
#ifdef HAS_NORMAL_MAP
    vec3 t = normalize(normal_matrix * tangent.xyz);

    vary_tangent = t;
    vary_sign = tangent.w;
    vary_normal = n;
#else //HAS_NORMAL_MAP
    vary_normal = n;
#endif //HAS_NORMAL_MAP
#endif //HAS_SKIN

    vertex_color = diffuse_color;

#if !defined(HAS_SKIN)
    vary_position = (modelview_matrix*vec4(position.xyz, 1.0)).xyz;
#endif
}
