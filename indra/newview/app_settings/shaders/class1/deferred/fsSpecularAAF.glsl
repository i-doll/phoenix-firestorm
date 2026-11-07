// Specular anti-aliasing: tunable geometric roughness widening for the G-buffer writers

// Linked as an extra fragment object into the PBR opaque, alpha and terrain programs and the
// legacy material and bump programs when FS_SPECULAR_AA is defined (FSRenderSpecularAA); those
// shaders then call fsSpecularAARoughness in place of filterSpecularRoughness (globalF.glsl).

// Kaplanyan, Hill, Patney and Lefohn 2016, "Filtering Distributions of Normals for Shading
// Antialiasing", in the deferred form of Tokuyoshi and Kaplanyan 2019, "Improved Geometric
// Specular Antialiasing":
//
//   alpha'^2 = alpha^2 + min(2 * sigma^2 * (|dn/dx|^2 + |dn/dy|^2), kappa)
//
// The always-on filterSpecularRoughness in globalF.glsl is the same estimator with fixed
// Filament constants (sigma^2 0.15, kappa 0.25). This one:
//
// - takes sigma^2 (the screen-space pixel filter variance, FSRenderSpecularAAStrength) and
//   kappa (the ceiling, FSRenderSpecularAAThreshold) from the settings, as uniforms set when the
//   program is bound (LLViewerShaderMgr::updateShaderUniforms);
//
// - adds the variance a minified normal map has already averaged away inside one texel of its
//   mip chain (Toksvig 2005). Screen-space derivatives only see how the filtered normal varies
//   from one pixel to the next; a high-frequency map that the mip filter has smoothed into a
//   short, nearly constant average shows little difference between neighbours while the lobe it
//   represents is still wide. The shortening of the averaged vector measures that directly:
//   sigma^2 = (1 - |n|) / |n|, the Gaussian slope variance whose Blinn-Phong equivalent is
//   Toksvig's factor, which in GGX alpha^2 terms is an additive 2 sigma^2 like the term above.
//   Encoded normal maps are lossy and 8-bit, so a unit texel decodes up to a few percent short;
//   that much is ignored rather than turned into a roughness floor on every mirror.

// Unset, both read 0 and the filter adds nothing
uniform float fs_specular_aa_variance;  // sigma^2
uniform float fs_specular_aa_threshold; // kappa

// Shortening of a decoded normal-map texel that is codec and quantization error, not variance.
#define FS_SPECULAR_AA_TOKSVIG_DEADZONE 0.03

// perceptualRoughness: the material's, in the [0,1] space the G-buffer stores.
// n: the final shading normal, normalized (any space; only its derivatives are used).
// mapNormal: the normal-map texel as sampled and expanded to [-1,1], before normalization;
//   vec3(0,0,1) where there is no normal map.
// Derivatives: call in uniform control flow, before any discard.
float fsSpecularAARoughness(float perceptualRoughness, vec3 n, vec3 mapNormal)
{
    vec3 du = dFdx(n);
    vec3 dv = dFdy(n);
    float variance = fs_specular_aa_variance * (dot(du, du) + dot(dv, dv));

    float len = clamp(length(mapNormal), 0.05, 1.0);
    variance += max(1.0 - len - FS_SPECULAR_AA_TOKSVIG_DEADZONE, 0.0) / len;

    float kernel = min(2.0 * variance, fs_specular_aa_threshold);

    float alpha = perceptualRoughness * perceptualRoughness;
    return sqrt(sqrt(clamp(alpha * alpha + kernel, 0.0, 1.0)));
}
