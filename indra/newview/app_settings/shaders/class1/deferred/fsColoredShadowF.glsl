// Coloured sun shadows: the sun's transmittance through translucent casters (FSRenderColoredShadows)

// Linked into the deferred soften program next to softenLightF.glsl, only while the setting is
// on (FS_COLORED_SHADOW). softenLightF multiplies the sunlight by fsColoredShadowTransmittance.
//
// coloredShadowMapN (RGBA8, sun cascades 0 and 1 only) holds, per shadow map texel, the product
// of the transmittance of every alpha-blended caster in that texel, white where there is none.
// The caster pass is depth tested against the opaque shadow map of the same cascade (shared
// depth buffer, never written), so only translucent surfaces in front of the nearest opaque
// caster count. A receiver is itself that nearest opaque caster, so glass behind it in light
// space (further from the sun) never tints it, and glass in front of it always does. No depth
// test is needed here.
//
// Where the opaque sun shadow already darkens a pixel the tint changes nothing, so it needs no
// filtering of its own beyond the bilinear fetch.

uniform sampler2D coloredShadowMap0;
uniform sampler2D coloredShadowMap1;

// per cascade, 1 when its map was drawn this frame for the main camera; 0 in reflection probes,
// with no translucent casters in the cascade, or with shadows skipped this frame
uniform vec2 colored_shadow_valid;

uniform mat4 shadow_matrix[4];
uniform vec4 shadow_clip;

// the map's transmittance at view space position pos, white outside the map
vec3 fsColoredShadowFetch(sampler2D map, mat4 m, vec3 pos)
{
    vec4 lpos = m * vec4(pos, 1.0);
    if (lpos.w <= 0.0)
    {
        return vec3(1.0);
    }
    vec2 uv = lpos.xy / lpos.w;
    if (any(lessThan(uv, vec2(0.0))) || any(greaterThan(uv, vec2(1.0))))
    {
        return vec3(1.0);
    }
    return texture(map, uv).rgb;
}

// Sun transmittance (linear rgb, 1 = unfiltered) at view space position pos. Cascade 0 blends
// into cascade 1 over 0.75 to 1.25 times the first split, the range sampleDirectionalShadow
// blends the depth maps over, so the tint does not step where the shadow does not. Fades back
// to white across the blend from cascade 1 to 2 (the same range around the second split),
// since the far cascades have no colour map.
vec3 fsColoredShadowTransmittance(vec3 pos)
{
    float z = -pos.z;
    float far_end = shadow_clip.y * 1.25;
    if (z >= far_end || z <= 0.0)
    {
        return vec3(1.0);
    }

    // 0 in cascade 0 alone, 1 in cascade 1 alone
    float blend = clamp((z - 0.75 * shadow_clip.x) / max(0.5 * shadow_clip.x, 1.0e-3), 0.0, 1.0);

    vec3 t0 = vec3(1.0);
    vec3 t1 = vec3(1.0);
    if (blend < 1.0 && colored_shadow_valid.x > 0.5)
    {
        t0 = fsColoredShadowFetch(coloredShadowMap0, shadow_matrix[0], pos);
    }
    if (blend > 0.0 && colored_shadow_valid.y > 0.5)
    {
        t1 = fsColoredShadowFetch(coloredShadowMap1, shadow_matrix[1], pos);
    }
    vec3 t = mix(t0, t1, blend);

    float fade = clamp((far_end - z) / max(shadow_clip.y * 0.5, 1.0e-3), 0.0, 1.0);
    return mix(vec3(1.0), clamp(t, vec3(0.0), vec3(1.0)), fade);
}
