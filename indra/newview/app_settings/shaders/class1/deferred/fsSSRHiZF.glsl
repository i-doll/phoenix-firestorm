/**
 * @file class1/deferred/fsSSRHiZF.glsl
 *
 * Builds the screen-space reflection scene pyramid (FSRenderSSRHiZ), one mip level per pass.
 * rgb holds the scene colour, box-filtered per level for the glossy cone lookup; a holds the
 * NEAREST linear view depth (metres) the level's texel covers, for hierarchical-Z tracing.
 *
 * $LicenseInfo:firstyear=2026&license=viewerlgpl$
 * Phoenix Firestorm Viewer Source Code
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation;
 * version 2.1 of the License only.
 * $/LicenseInfo$
 */

/*[EXTRA_CODE_HERE]*/

out vec4 frag_color;

uniform sampler2D diffuseRect;

#ifdef FS_SSR_HIZ_COPY

// Level 0: the scene map's colour and depth, depth turned linear.
uniform sampler2D depthMap;
uniform mat4 ssr_inv_proj;
uniform vec2 screen_res;

void main()
{
    ivec2 px = ivec2(gl_FragCoord.xy);
    vec3 col = texelFetch(diffuseRect, px, 0).rgb;
    float d = texelFetch(depthMap, px, 0).r;
#ifdef REVERSE_Z
    float ndcz = d;
#else
    float ndcz = 2.0 * d - 1.0;
#endif
    vec4 ndc = vec4((vec2(px) + 0.5) / screen_res * 2.0 - 1.0, ndcz, 1.0);
    float z = dot(vec4(ssr_inv_proj[0].z, ssr_inv_proj[1].z, ssr_inv_proj[2].z, ssr_inv_proj[3].z), ndc);
    float w = dot(vec4(ssr_inv_proj[0].w, ssr_inv_proj[1].w, ssr_inv_proj[2].w, ssr_inv_proj[3].w), ndc);
    float lin = -z / w;
    // Half float storage: keep the far plane (or an infinite one) finite.
    frag_color = vec4(col, clamp(lin, 0.0, 60000.0));
}

#else

// Level N from level N-1. The caller restricts the texture's base and max level to N-1 while
// rendering into level N, so lod 0 here IS level N-1 and there is no feedback loop.
void main()
{
    ivec2 srcSize = textureSize(diffuseRect, 0);
    ivec2 dst = ivec2(gl_FragCoord.xy);
    ivec2 dstSize = max(srcSize / 2, ivec2(1));
    ivec2 m = srcSize - 1;
    ivec2 p = dst * 2;

    vec4 a = texelFetch(diffuseRect, min(p, m), 0);
    vec4 b = texelFetch(diffuseRect, min(p + ivec2(1, 0), m), 0);
    vec4 c = texelFetch(diffuseRect, min(p + ivec2(0, 1), m), 0);
    vec4 d = texelFetch(diffuseRect, min(p + ivec2(1, 1), m), 0);

    vec3 col = (a.rgb + b.rgb + c.rgb + d.rgb) * 0.25;
    float z = min(min(a.a, b.a), min(c.a, d.a));

    // An odd source size loses its last row or column to the halving. The last destination
    // texel takes it in, so the nearest depth stays conservative over the whole screen.
    bool extraX = (srcSize.x % 2) == 1 && dst.x == dstSize.x - 1;
    bool extraY = (srcSize.y % 2) == 1 && dst.y == dstSize.y - 1;
    if (extraX)
    {
        z = min(z, texelFetch(diffuseRect, min(p + ivec2(2, 0), m), 0).a);
        z = min(z, texelFetch(diffuseRect, min(p + ivec2(2, 1), m), 0).a);
    }
    if (extraY)
    {
        z = min(z, texelFetch(diffuseRect, min(p + ivec2(0, 2), m), 0).a);
        z = min(z, texelFetch(diffuseRect, min(p + ivec2(1, 2), m), 0).a);
    }
    if (extraX && extraY)
    {
        z = min(z, texelFetch(diffuseRect, min(p + ivec2(2, 2), m), 0).a);
    }

    frag_color = vec4(col, z);
}

#endif
