/**
 * @file pbropaqueV.glsl
 *
 * $LicenseInfo:firstyear=2022&license=viewerlgpl$
 * Second Life Viewer Source Code
 * Copyright (C) 2022, Linden Research, Inc.
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

#ifdef FS_GDD
#extension GL_ARB_shader_storage_buffer_object : require
#endif

#ifdef RIGGED_PRECISE_MATH
invariant gl_Position;
precise gl_Position;
#endif


#ifndef IS_HUD

//deferred opaque implementation

#ifdef FS_GDD
// FSPerfGPUDrivenPBR: static opaque batches drawn with glMultiDrawElementsIndirect out of a vertex arena.
// Every draw command has a record, an instanced attribute the command's base instance selects, naming
// its matrices and its material in the storage buffers below. They hold exactly the values the stock
// path puts in the uniforms of the same names for that batch, computed the same way on the CPU.
layout(std430) readonly buffer FSGDDPBRMatrices
{
    mat4 fs_gdd_matrix[];   // per model matrix: modelview, modelview projection, normal matrix; texture matrices
};

struct FSGDDMaterial
{
    vec4 transform[8];      // texture_base_color/normal/metallic_roughness/emissive_transform, two each
    vec4 factors;           // minimum_alpha, roughnessFactor, metallicFactor
    vec4 emissive;          // emissiveColor
    uvec4 tex01;            // bindless handles: diffuseMap, bumpMap
    uvec4 tex23;            // specularMap, emissiveMap
};

layout(std430) readonly buffer FSGDDPBRMaterials
{
    FSGDDMaterial fs_gdd_material[];
};

in uvec4 fs_gdd_draw;       // x: first of the three model matrices, y: texture matrix, z: material

// the material's constants (and textures) for the fragment shader, which reads them in place of its uniforms
flat out vec4 fs_gdd_factors;
flat out vec3 fs_gdd_emissive;
#ifdef FS_GDD_BINDLESS
flat out uvec4 fs_gdd_tex01;
flat out uvec4 fs_gdd_tex23;
#endif
#else
uniform mat4 modelview_matrix;

#ifdef HAS_SKIN
uniform mat4 projection_matrix;
mat3x4 getSkinBlend();
vec3 skinNormal(mat3x4 b, vec3 pos, vec3 dir, mat4 m);
vec4 skinTransformH(mat3x4 b, vec3 pos, mat4 m);
#else
uniform mat3 normal_matrix;
uniform mat4 modelview_projection_matrix;
#endif
uniform mat4 texture_matrix0;

uniform vec4[2] texture_base_color_transform;
uniform vec4[2] texture_normal_transform;
uniform vec4[2] texture_metallic_roughness_transform;
uniform vec4[2] texture_emissive_transform;
#endif

in vec3 position;
in vec4 diffuse_color;
in vec3 normal;
in vec4 tangent;
in vec2 texcoord0;

out vec2 base_color_texcoord;
out vec2 normal_texcoord;
out vec2 metallic_roughness_texcoord;
out vec2 emissive_texcoord;

out vec4 vertex_color;

out vec3 vary_tangent;
flat out float vary_sign;
out vec3 vary_normal;
out vec3 vary_position;

vec2 texture_transform(vec2 vertex_texcoord, vec4[2] khr_gltf_transform, mat4 sl_animation_transform);
vec4 tangent_space_transform(vec4 vertex_tangent, vec3 vertex_normal, vec4[2] khr_gltf_transform, mat4 sl_animation_transform);

void main()
{
#ifdef FS_GDD
    mat4 modelview_matrix = fs_gdd_matrix[fs_gdd_draw.x];
    mat4 modelview_projection_matrix = fs_gdd_matrix[fs_gdd_draw.x + 1u];
    mat3 normal_matrix = mat3(fs_gdd_matrix[fs_gdd_draw.x + 2u]);
    mat4 texture_matrix0 = fs_gdd_matrix[fs_gdd_draw.y];
    FSGDDMaterial fs_mat = fs_gdd_material[fs_gdd_draw.z];
    vec4[2] texture_base_color_transform = vec4[2](fs_mat.transform[0], fs_mat.transform[1]);
    vec4[2] texture_normal_transform = vec4[2](fs_mat.transform[2], fs_mat.transform[3]);
    vec4[2] texture_metallic_roughness_transform = vec4[2](fs_mat.transform[4], fs_mat.transform[5]);
    vec4[2] texture_emissive_transform = vec4[2](fs_mat.transform[6], fs_mat.transform[7]);
    fs_gdd_factors = fs_mat.factors;
    fs_gdd_emissive = fs_mat.emissive.rgb;
#ifdef FS_GDD_BINDLESS
    fs_gdd_tex01 = fs_mat.tex01;
    fs_gdd_tex23 = fs_mat.tex23;
#endif
#endif

#ifdef HAS_SKIN
    mat3x4 skin = getSkinBlend();
    vec3 pos = skinTransformH(skin, position.xyz, modelview_matrix).xyz;
    vary_position = pos;
    gl_Position = projection_matrix*vec4(pos,1.0);

#else
    vary_position = (modelview_matrix*vec4(position.xyz, 1.0)).xyz;
    //transform vertex
    gl_Position = modelview_projection_matrix * vec4(position.xyz, 1.0);
#endif

    base_color_texcoord = texture_transform(texcoord0, texture_base_color_transform, texture_matrix0);
    normal_texcoord = texture_transform(texcoord0, texture_normal_transform, texture_matrix0);
    metallic_roughness_texcoord = texture_transform(texcoord0, texture_metallic_roughness_transform, texture_matrix0);
    emissive_texcoord = texture_transform(texcoord0, texture_emissive_transform, texture_matrix0);

#ifdef HAS_SKIN
    vec3 n = skinNormal(skin, position.xyz, normal.xyz, modelview_matrix);
    vec3 t = skinNormal(skin, position.xyz, tangent.xyz, modelview_matrix);
#else //HAS_SKIN
    vec3 n = normal_matrix * normal;
    vec3 t = normal_matrix * tangent.xyz;
#endif

    n = normalize(n);

    vec4 transformed_tangent = tangent_space_transform(vec4(t, tangent.w), n, texture_normal_transform, texture_matrix0);
    vary_tangent = normalize(transformed_tangent.xyz);
    vary_sign = transformed_tangent.w;
    vary_normal = n;

    vertex_color = diffuse_color;
}

#else

// fullbright HUD implementation

uniform mat4 modelview_projection_matrix;

uniform mat4 texture_matrix0;

uniform vec4[2] texture_base_color_transform;
uniform vec4[2] texture_emissive_transform;

in vec3 position;
in vec4 diffuse_color;
in vec2 texcoord0;

out vec2 base_color_texcoord;
out vec2 emissive_texcoord;

out vec4 vertex_color;

vec2 texture_transform(vec2 vertex_texcoord, vec4[2] khr_gltf_transform, mat4 sl_animation_transform);

void main()
{
    //transform vertex
    gl_Position = modelview_projection_matrix * vec4(position.xyz, 1.0);

    base_color_texcoord = texture_transform(texcoord0, texture_base_color_transform, texture_matrix0);
    emissive_texcoord = texture_transform(texcoord0, texture_emissive_transform, texture_matrix0);

    vertex_color = diffuse_color;
}

#endif

