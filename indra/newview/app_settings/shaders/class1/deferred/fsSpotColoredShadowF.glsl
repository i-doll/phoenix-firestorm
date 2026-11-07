// Coloured projector shadows: a projector's light through translucent casters
// (FSRenderColoredShadows and FSRenderColoredShadowsProjectors)

// Linked into the projector programs next to spotLightF.glsl, only while both settings are on
// (FS_SPOT_COLORED_SHADOW). spotLightF multiplies the projector's light by
// fsSpotColoredTransmittance.
//
// Each shadowed projector has an RGBA8 colour map sharing its shadow depth buffer, drawn during
// its shadow pass the way the sun cascades' maps are (fsColoredShadowCasterF.glsl): the product
// of the transmittance of every alpha-blended caster in a texel in front of the nearest opaque
// caster, white where there is none. Projectors are drawn one light at a time, so setupSpotLight
// binds that light's map next to its shadow map (same proj_shadow_matrix), white with
// fs_spot_colored_valid = 0 for a light without a map (unshadowed, nothing translucent in its
// frustum, reflection probes).

/*[EXTRA_CODE_HERE]*/

uniform sampler2D fs_spot_colored_map;
uniform float fs_spot_colored_valid;    // 1 when this light's map was drawn for its current shadow
uniform mat4 proj_shadow_matrix;        // view space to this light's shadow map, [0, 1]
uniform vec4 shadow_clip;
uniform float spot_shadow_offset;
uniform float shadow_fade;              // 1 - the slot's fade in, as the shadow itself is faded

// The projector's transmittance (linear rgb, 1 = unfiltered) at view space position pos, normal
// norm: the same normal offset and camera distance fade as the projector's shadow, and its fade
// in and out as the light enters or leaves a shadow slot.
vec3 fsSpotColoredTransmittance(vec3 pos, vec3 norm)
{
    if (fs_spot_colored_valid < 0.5)
    {
        return vec3(1.0);
    }

    pos += norm * spot_shadow_offset;

    // the camera distance fade sampleSpotShadow gives the shadow
    if (pos.z <= -shadow_clip.w)
    {
        return vec3(1.0);
    }
    float fade = 1.0 - clamp((pos.z + shadow_clip.z) / (shadow_clip.z - shadow_clip.w) * 2.0 - 1.0, 0.0, 1.0);
    fade *= 1.0 - clamp(shadow_fade, 0.0, 1.0);
    if (fade <= 0.0)
    {
        return vec3(1.0);
    }

    vec4 lpos = proj_shadow_matrix * vec4(pos, 1.0);
    if (lpos.w <= 0.0)
    {
        return vec3(1.0);
    }
    vec2 uv = lpos.xy / lpos.w;
    if (any(lessThan(uv, vec2(0.0))) || any(greaterThan(uv, vec2(1.0))))
    {
        return vec3(1.0);
    }

    vec3 t = clamp(texture(fs_spot_colored_map, uv).rgb, vec3(0.0), vec3(1.0));
    return mix(vec3(1.0), t, fade);
}
