# Additional Tone Mapping Options

**Date:** 2026-07-29
**Status:** Design approved; revised after adversarial review

Expand `RenderTonemapType` from 2 tone mappers to 11. The change is confined to
one GLSL file plus UI/settings metadata — **no C++ changes are required.**

> This document was revised after a four-way adversarial review. Claims that the
> review refuted have been corrected rather than softened; see *Corrections from
> review* at the end for what changed and why.

## Background: how tone mapping is wired today

The selected mapper reaches the shader as a plain `int` uniform:

- `indra/newview/pipeline.cpp:8187-8188` reads `RenderTonemapType` via
  `LLCachedControl<U32>` and calls `uniform1i(tonemap_type, ...)`.
- `indra/newview/lldrawpoolwater.cpp:281-282` does the same for the water shader.

Neither site clamps or validates the value. The shader consumes it in
`indra/newview/app_settings/shaders/class1/deferred/tonemapUtilF.glsl`, where a
`switch (tonemap_type)` appears **twice** — in `toneMap()` (line 131) and in
`toneMapNoExposure()` (line 160).

Input is linear Rec.709, post-exposure. Output is SDR, clamped to 0–1
(`tonemapUtilF.glsl:144`) and encoded downstream.

Three behaviours worth knowing before judging this feature's reach:

- **The tone mapper does not run at all when `RenderHDREnabled` is off.** In
  `renderFinalize` (`pipeline.cpp:~9215-9241`) the code calls `gammaCorrect()`
  *instead of* `tonemap()`. Vintage Mode force-sets that flag false
  (`llviewercontrol.cpp:356-359`), which is why the prefs C++ greys the combo out.
- **Water is unaffected.** `tonemapUtilF.glsl` is attached to the water program
  and the uniform is uploaded, but the only call site is commented out —
  `class3/environment/waterF.glsl:327` reads
  `//radiance = toneMapNoExposure(radiance);`. The upload at
  `lldrawpoolwater.cpp:281-282` is dead code. New mappers must still *compile*
  inside the water program, but water will not change appearance.
- **Applying a quality preset silently resets the user's choice.**
  `llfeaturemanager.cpp:633-636` does `gSavedSettings.setU32(name, recommended)`,
  and all 24 featuretable rows read `RenderTonemapType 1 1` (col2 = available,
  col3 = recommended *value*; parser at `llfeaturemanager.cpp:359-362`). So any
  quality-preset apply forces the mapper back to ACES. See *Open decision*.

Adding a mapper therefore means: one GLSL function, one `case` label, and
combo-box entries.

## Constraint: the enum is append-only

Values 0 and 1 are persisted in users' `settings.xml` and hardcoded in 24
featuretable rows (8 each in `featuretable.txt`, `_linux`, `_mac`). **Never
renumber.** New mappers append at 2 and above.

## The mapper table

| # | Label | Source | Notes |
|---|---|---|---|
| 0 | Khronos Neutral | existing | unchanged |
| 1 | ACES (Hill) | existing | label gains "(Hill)" to disambiguate from 2 |
| 2 | ACES (Narkowicz) | **already present, unused** | `tonemapUtilF.glsl:55`; zero call sites; needs only a `case` |
| 3 | Reinhard (extended) | new | ~4 lines, white point 4.0 |
| 4 | Uncharted 2 (Hable) | new | ~16 lines, W=11.2, exposure bias 2.0 |
| 5 | AgX | new | ~30 lines, Iolite minimal (sRGB-direct) |
| 6 | AgX Punchy | new | +8 lines on AgX |
| 7 | AgX Golden | new | +3 lines on Punchy |
| 8 | Gran Turismo | new | ~25 lines; Uchimura 2017 curve |
| 9 | GT7 | new | ~120 lines; port of official MIT reference |
| 10 | Lottes (AMD) | new | ~15 lines, fixed parameters |

All constants for 2–8 and 10 were verified numerically against their published
sources during review; none required correction. Lottes ships the paper's
suggested constants baked in rather than exposing its knobs as new settings
(YAGNI); exposing them is deferred.

## Architecture: extract the dispatcher

Duplicating an 11-case switch across two functions guarantees drift. Extract one
dispatcher, and make it the single place the input guard lives:

```glsl
vec3 applyTonemap(vec3 color)
{
    // Single input guard. Every pow()-based mapper below is undefined for
    // negative input (GLSL 8.2), and the existing contract comment at line 94
    // already claims non-negative input without enforcing it.
    color = max(color, vec3(0.0));

    switch (tonemap_type)
    {
    case 0:  return PBRNeutralToneMapping(color);
    case 1:  return toneMapACES_Hill(color);
    case 2:  return toneMapACES_Narkowicz(color);
    case 3:  return toneMapReinhard(color);
    case 4:  return toneMapUncharted2(color);
    case 5:  return toneMapAgX(color);
    case 6:  return toneMapAgXPunchy(color);
    case 7:  return toneMapAgXGolden(color);
    case 8:  return toneMapUchimura(color);
    case 9:  return toneMapGT7(color);
    case 10: return toneMapLottes(color);
    }
    return clamp(color, 0.0, 1.0); // unreachable via UI; free insurance
}
```

`toneMap()` then reduces to:

```glsl
vec3 toneMap(vec3 color)
{
#ifndef NO_POST
    float exp_scale = texture(exposureMap, vec2(0.5, 0.5)).r;
    float final_exposure = exposure * exp_scale;
    vec3 exposed_color = color * final_exposure;

    color = mix(exposed_color, applyTonemap(exposed_color), tonemap_mix);
    color = clamp(color, 0.0, 1.0);
#else
    color *= exposure * texture(exposureMap, vec2(0.5, 0.5)).r;
    color = clamp(color, 0.0, 1.0);
#endif
    return color;
}
```

**Behaviour-preserving.** The current code assigns `linear_input_color = color`
(line 124), never modifies it, then computes
`exposed_linear_input = linear_input_color * final_exposure` — the same single
multiply of the same operands as `exposed_color` (line 128). Bit-identical, no
reassociation hazard. `toneMapNoExposure()` reduces the same way without the
exposure terms.

The trailing `return` replaces a `default:` case. Note it is *observably* a
no-op: for an out-of-range value the old path yields `clamp(mix(e, e, t))` and
the new yields `clamp(mix(e, clamp(e), t))`, equal per-component for
`t` in [0,1] — and the `RenderTonemapMix` slider is bounded [0,1] in XUI. Keep it
as insurance against strict drivers, not as a user-visible improvement.

## NaN safety — the defect this review caught

Every mapper shipping today is a rational polynomial, which is why the pipeline
has never needed an input guard. **Six of the nine new mappers use `pow()` with a
fractional exponent, which is undefined for a negative base.** Three concrete
triggers, all verified numerically:

1. **AgX at pure black.** `agxDefaultContrastApprox(0) = -0.00232` — the
   polynomial's own constant term. A black pixel traced through the fully
   guarded path (`max(v,1e-10)` → `log2` → clamp to `MIN_EV` → normalise → poly)
   still arrives at −0.00232, then hits `pow(negative, 2.2)`. This affects
   **every black pixel in the scene**, not an edge case. An input guard does not
   fix it — the negative originates *inside* AgX.
2. **AgX via inverse-matrix off-diagonals.** `agx_mat_inv` has negative
   off-diagonals, so a sufficiently saturated post-polynomial triple can go
   negative before the EOTF even when the polynomial behaves. Real but rare: a
   2197-point input sweep found 63 negatives, of which 62 were mechanism (1) and
   exactly **1** was this. Same fix, so incidence does not change the plan.
3. **Uncharted 2 pole.** The denominator `x(Ax+B)+DF` has a real root at
   x = −0.124662; with the 2.0 exposure bias that is scene input **−0.062331**,
   giving division by ~zero → Inf.

Lottes (`pow(x, 1.6)`) and Uchimura (toe `pow(x/m, 1.28)`) are undefined for
negative input but need no epsilon at zero — `pow(0, y>0)` is defined and equals
0. Do not add one out of superstition.

**Why this must be fixed rather than tolerated:**

- `clamp()` does not sanitize NaN. GLSL makes NaN support optional and leaves
  min/max's result undefined when an operand is NaN, so `clamp(NaN,0,1)` is
  per-driver: some hardware drops the NaN and yields 0 or 1, others pass it to
  the framebuffer. Downstream passes that read it (CAS sharpening, FXAA) smear
  it into visible blobs.
- `mix()` poisons even at weight 0, because `0 * NaN = NaN`. So
  `mix(exposed, NaN, tonemap_mix)` is NaN even at `tonemap_mix = 0` — the mix
  slider is not an escape hatch. By the same mechanism, Uchimura's weighted
  segment sum `T*w0 + L*w1 + S*w2` is poisoned by a NaN in any segment
  regardless of its weight; the smoothstep/step region masking is **not** a guard.

**The fix, in two places:**

- `color = max(color, vec3(0.0));` once at `applyTonemap()` entry — covers
  Uncharted 2's pole, Lottes, Uchimura, and GT7's `rgbToICtCp`.
- `val = max(val, vec3(0.0));` immediately before AgX's `pow(val, 2.2)` — covers
  triggers (1) and (2), which the input guard cannot reach.

## GLSL portability constraints

Shaders compile at versions as low as `#version 140`
(`llshadermgr.cpp:617`), and 140 is genuinely reachable for this shader: the
version choice keys purely off `gGLManager.mGLSLVersionMajor/Minor`
(`llshadermgr.cpp:569-623`) — shader class and program identity play no part.
macOS never hits it (GL4 core context), but Windows falls back to a GL 3.0
request and Linux/SDL2 sets no context version attributes at all. A GLSL 1.30
driver also receives `#version 140`.

- **`switch` on `int`** is 1.30+, below the 140 floor. Fine, and already in use.
- **`smoothstep(float,float,vec3)` / `step(float,vec3)`** genType overloads exist
  since GLSL 1.10. Fine.
- **`const` initialisers may call built-ins.** Built-in calls with constant
  arguments have been valid constant expressions since GLSL 1.20 (§4.3.3).
  Verified empirically: `const vec3 kB = pow(vec3(0.18), vec3(1.6));` compiles
  clean at `#version 140`. **Lottes needs no special handling.**
- **`const` initialisers may NOT call user-defined functions.** This is illegal
  at every desktop version. Verified: `const vec3 white_scale = partial(...)` at
  140 fails with *"global const initializers must be constant"*. **Uncharted 2's
  `white_scale` must be a plain local.**
- **`mat3(...)` is column-major.** Verify every transcribed matrix with a
  row-sum check rather than by eye: a white-preserving matrix must have
  mathematical rows summing to 1.0. `ACESInputMat` read column-major gives rows
  (0.59719, 0.35458, 0.04823) / (0.07600, 0.90834, 0.01566) / (0.02840, 0.13383,
  0.83777), each summing to 1.0000 — correct. Read row-major they sum to
  0.702/1.058/0.902, i.e. a grey-shifted white. The Iolite `agx_mat` passes the
  same test as written (rows ≈ 1.0001), confirming its arguments are already in
  GLSL column-major order and **must not be transposed again**.

## Implementations

Reference forms for mappers 3–8 and 10 — all constants verified during review:

- **Reinhard (extended):** `color * (1 + color/W²) / (1 + color)`, W = 4.0.
  Mid-grey → 0.154. Everything ≥ 4.0 per channel clips, which is what a white
  point means. The per-channel form desaturates bright saturated colours toward
  white; inherent, not an error.
- **Uncharted 2 (Hable):** A=0.15, B=0.50, C=0.10, D=0.20, E=0.02, F=0.30,
  W=11.2, exposure bias 2.0. Partial form
  `((x(Ax+CB)+DE)/(x(Ax+B)+DF)) − E/F`. `partial(11.2)=0.72513`, whiteScale
  1.3791, mid-grey → 0.128.
- **AgX:** Iolite minimal — `agx_mat` inset, `log2` over EV range
  [−12.47393, 4.026069], 6th-order contrast polynomial (15.5, −40.14, 31.96,
  −6.868, 0.4298, 0.1191, −0.00232 descending), then `agx_mat_inv` and
  `pow(val, 2.2)`. Guard `log2` input with `max(val, 1e-10)` **and** guard the
  `pow` as described above. `pow(2.2)` does not double-apply: AgX output is
  defined against a 2.2-exponent reference display, so decoding to linear here
  and re-encoding downstream is the intended round trip. Using the piecewise
  sRGB inverse instead would be bit-exact but differs only below ~0.04 encoded;
  `pow(2.2)` is what every shipping port uses.
- **AgX Punchy / Golden:** applied *between* the curve and the EOTF, which is
  correct — Iolite's order is `agx → agxLook → agxEotf`, and the ASC-CDL grade is
  meant to operate in 2.2-encoded space. Punchy: power 1.35, sat 1.4. Golden:
  slope (1.0, 0.9, 0.5), power 0.8, sat 0.8. **Transcription trap:** luma is
  computed from the value *before* the pow, and reused afterwards —
  `luma + sat*(val_after_pow − luma)`.
- **Uchimura 2017:** P=1.0, a=1.0, m=0.22, l=0.4, c=1.33, b=0.0. Derived:
  l0=0.312, S0=S1=0.532 (equal because a=1 — not a bug), C2=2.13675, CP=−C2/P.
  Mid-grey → 0.179.
- **Lottes:** a=1.6, d=0.977, hdrMax=8.0, midIn=0.18, midOut=0.267. Derived
  b=1.07304, c=0.16742. Self-consistency: f(0.18)=0.26700 hits `midOut` exactly
  and f(8)=1.0 hits `hdrMax` exactly.

### GT7 (index 9)

Port of the **official reference implementation**: `gt7_tone_mapping.cpp`,
MIT © 2025 Polyphony Digital Inc., published as supplemental material to
"Physically Based Tone Mapping and Perceptual Fidelity in GT7" (SIGGRAPH **2025**
PBS course). 524 lines of C++. MIT is compatible with Firestorm's LGPL 2.1; the
**MIT copyright header must be preserved** in the shader file.

The algorithm, per the reference:

1. Convert linear Rec.709 → linear Rec.2020.
2. `rgbToICtCp()` on the input to separate luminance from chroma (via ST-2084 PQ).
3. Evaluate `GTToneMappingCurveV2` per channel → "skewed" RGB, and convert that
   to ICtCp too.
4. Scale input chroma by `chromaCurve(I / targetUcs, fadeStart, fadeEnd)`.
5. Recombine: luminance from skewed, chroma scaled → back to RGB.
6. Blend skewed vs scaled at `blendRatio`, clamp to target, apply SDR correction.
7. Convert linear Rec.2020 → linear Rec.709.

**The reference already provides SDR mode** (`initializeAsSDR()`), so no
interpretation is needed, and every SDR parameter is a compile-time constant.
Bake these as literals:

| Constant | Value |
|---|---|
| `peakIntensity` (framebuffer luminance target) | 2.5 |
| `alpha` | 0.25 |
| `midPoint` | 0.538 |
| `linearSection` | 0.444 |
| `toeStrength` | 1.28 |
| `k` | 0.7413333333333334 |
| `kA` | 2.963333333333334 |
| `kB` | −3.3733512380644313 |
| `kC` | −0.539568345323741 |
| `blendRatio` | 0.6 |
| `fadeStart` / `fadeEnd` | 0.98 / 1.16 |
| `sdrCorrectionFactor` | 0.4 |
| `framebufferLuminanceTargetUcs` | 0.6025591549907509 |
| branch point (`linearSection*peak`) | 1.11 |

**Cost:** six `inverseEotfSt2084` + three `eotfSt2084` per pixel, each two
`pow()` — roughly 18 `pow()` per pixel plus two colour-space matrices. GT7 is by
a wide margin the most expensive mapper here. Acceptable for a full-screen post
pass, but it should not be a default and the cost belongs in its tooltip.

**Validation oracle:** a Python port of the SDR path reproduces the C++
reference's own test vectors exactly — `(0.5,1.23,0.75) → (0.200,0.491,0.300)`,
`(12.3,34.3,56.9) → (1,1,1)`, `(1504.7,64.51,0.5) → (1,1,0.739)`. The GLSL port
must match the same vectors.

## Files changed

| File | Change |
|---|---|
| `.../shaders/class1/deferred/tonemapUtilF.glsl` | 9 new functions, dispatcher refactor, NaN guards |
| `app_settings/settings.xml:14048` | comment reads "0 = Khronos Neutral, 1 = ACES"; enumerate all 11 |
| `skins/default/xui/en/panel_preferences_graphics1.xml` (combo at 1708, items 1716-1722) | combo items |
| `skins/default/xui/en/floater_preferences_graphics_advanced.xml` (combo at 1069, items 1077-1083) | combo items |
| `skins/default/xui/en/floater_phototools.xml` (combo at 2322, items 2332-2339) | combo items |
| `skins/default/xui/en/floater_phototools.xml:2340-2341` | **delete** the `Quickprefs.ShaderChanged` commit callback (full shader rebuild per selection) |
| `skins/default/xui/en/floater_phototools.xml:2324` | `enabled_control` → `RenderDisableVintageMode` so the picker greys out like the prefs panels |
| `skins/default/xui/en/floater_phototools.xml:2316, 2348` | both tooltips describe exactly two mappers and go stale (only 2348 uses the phrase "either Khronos Neutral or ACES"; 2316 has its own two-mapper prose) |
| 4 translated overlays (see below) | item 1 label → "ACES (Hill)" |

**No C++ files change.** Confirmed by exhaustive search: the only C++ touching
the setting is the two `uniform1i` sites. `llfloaterpreference.cpp:2348-2358` and
`llfloaterpreferencesgraphicsadvanced.cpp:333-343` call `getChild("TonemapType")`
only to `setEnabled(...)`. The phototools commit callback
(`quickprefs.cpp:117`) is a value-agnostic shader reload. Presets are
data-driven. `LLControlGroup` does no range validation on U32.

### UI constraints

**The dropdown list needs no layout change.** It is a floating popup, not inline
content: `LLComboBox::showList()` (`llcombobox.cpp:690-757`) calls
`fitContents(192, screen_height - 50)`, expands width to content up to
`MAX_COMBO_WIDTH = 500` (`:55`, `:706`), stacks above or below by available room,
and scrolls if too tall. 11 items ≈ 200px, drawn over the floater. The
`height="50"` panel enclosing the phototools combo does not constrain it.

**The closed combo *button* does constrain label length.** The phototools floater
is 288px wide (`floater_phototools.xml:16`), its label takes 162px at `left="5"`,
and the combo runs to `right="-5"` — roughly **110px of button**. The other two
combos are `width="150"`. This is why mapper 8 is labelled "Gran Turismo" and not
"Gran Turismo (Uchimura 2017)", which would truncate in all three panels. Keep
every label at or under the width of "Khronos Neutral", which ships today and
fits; "ACES (Narkowicz)" is within a character of it and is acceptable. **No
relayout is needed if labels respect this bound** — but the bound is a real
constraint, not a style preference.

**New items must use `name="2"` … `name="10"`.** Translation overlays and the
enable/disable logic key on the `name` attribute, not position.

**Stale value shows a stale label, not a blank.** When the bound control holds a
value with no matching item, `LLComboBox::setValue()` (`llcombobox.cpp:378-401`)
takes the `found == false` branch, sets `mLastSelectedIndex = -1`, and **never
calls `updateLabel()`** — so the button keeps whatever text it last displayed.
A preset carrying mapper 7 loaded on a build without it will confidently show
the previous label while the renderer takes the fall-through path. Note this
makes the shader-side `default:` a *render-side* safeguard only; it does nothing
for the UI, and old builds have no such default at all. Only reachable by
downgrading — appending values never affects users on 0 or 1.

**Remove the phototools commit callback.** The phototools combo alone carries
`<combo_box.commit_callback function="Quickprefs.ShaderChanged"/>`
(`floater_phototools.xml:2340-2341`), which routes via `quickprefs.cpp:117` to
`handleSetShaderChanged` (`llviewercontrol.cpp:261`) — a full shader rebuild
including `gBumpImageList.destroyGL()/restoreGL()`. This is vestigial: the mapper
is a per-frame uniform (`pipeline.cpp:8187-8188`), and neither of the other two
combos has a callback. With 2 options it was rarely hit; with 11 options users
will A/B looks rapidly and eat a multi-second stall on every flip. Deleting the
callback is an XUI-only fix.

**Gate the phototools combo on vintage mode.** Both prefs panels disable the
control under vintage (`llfloaterpreference.cpp:2349-2356`,
`llfloaterpreferencesgraphicsadvanced.cpp:334-341`), but phototools uses
`enabled_control="VertexShaderEnable"` (`floater_phototools.xml:2324`) and stays
enabled — an 11-way picker that does nothing, since vintage forces
`RenderHDREnabled` false (`llviewercontrol.cpp:358`) and the tone mapper is then
skipped entirely. Changing it to `enabled_control="RenderDisableVintageMode"`
keeps this XUI-only.

A tree-wide search found no stale English strings beyond the two phototools
tooltips and the `settings.xml` comment. The non-default `starlight` skin does
not override any of these three panels.

Translated skins (`fr`, `pl`, `zh`, `it`, `ja`, `de`, `ru`, `pt`) **merge**
rather than replace: `llxmlnode.cpp:573-660 updateNode()` matches children by
`name` attribute, updates matched nodes in place, leaves unmatched base children
untouched, and never appends overlay-only children. New English items therefore
survive for localized users with English labels — **new** items need no
translation work.

**But the `ACES` → `ACES (Hill)` relabel does.** Four overlays pin item 1's label
to plain "ACES", so the disambiguation would never reach those locales and the
list would show "ACES" directly above "ACES (Narkowicz)" — two entries the user
cannot tell apart:

- `fr/floater_preferences_graphics_advanced.xml:191`
- `zh/floater_preferences_graphics_advanced.xml:160`
- `zh/panel_preferences_graphics1.xml:233`
- `zh/floater_phototools.xml:253`

Four mechanical edits, no translation skill required for "(Hill)". Three
translated phototools tooltips also carry the stale "Khronos Neutral or ACES"
prose (`fr:248,251`, `pl:247,250`, `zh:248,255`); they degrade to describing two
of eleven mappers rather than breaking, so updating them is optional.

## Verification

The viewer is **not** compiled locally (project convention). Three checks cover
the realistic failure modes:

There is **no shader-compile CI** in this repo — `.github/workflows/*` build C++
only, and `.pre-commit-config.yaml:10-20` treats `.glsl` as formatting-only. The
local validator run below is the only pre-user compile gate, which raises its
importance.

1. **`glslangValidator`** (`/usr/bin/glslangValidator`). Wrap `tonemapUtilF.glsl`
   in a harness and compile at **both `#version 140` and `#version 400`**,
   injecting the precision qualifiers production uses
   (`precision mediump int; precision highp float;` — `llshadermgr.cpp:610-620`).
   Compile **both permutations, with and without `NO_POST`** (added for
   `gNoPostTonemapProgram` at `llviewershadermgr.cpp:2537`) — they take different
   paths through `toneMap()`. Also compile in the water program's configuration,
   since the file is attached there even though the call site is dead.
2. **Row-sum check on every transcribed matrix** — mathematical rows must sum to
   ~1.0, the reliable way to catch a transposition.
3. **Golden-vector test for AgX.** The row-sum check proves a matrix is
   white-preserving but a scalar sweep cannot detect a transposition, because
   grey inputs are symmetric. Push an asymmetric saturated colour (e.g.
   `(1.0, 0.1, 0.02)`) through the Python AgX and pin the result against
   published Iolite/Blender output.
4. **Python reference curves** for all mappers: monotonicity, `f(0) ≈ 0`,
   plausible mid-grey, saturation toward 1. **Plus an explicit NaN probe that
   runs pre-clamp** — during review, a first-pass harness masked the AgX defect
   because Python's `max(0.0, nan)` returns `0.0`, exactly as a naive `clamp()`
   would. Probe the raw math, not the clamped result. For GT7, assert against the
   reference's own test vectors.

### In-world review preconditions

**Without these, all 11 mappers look identical and a reviewer will report the
feature broken.** The tone mapper is bypassed under several ordinary conditions:

- **Use a PBR sky, not a legacy Windlight one.** `getTonemapMix()` returns
  **0.0** for legacy settings (`llsettingssky.cpp:2066-2074`), blending the
  mapper entirely out.
- **Reflection Probe Ambiance must be > 0.** At zero, `no_post` is forced
  (`pipeline.cpp:8148`) and the `NO_POST` permutation skips the switch outright.
- **Vintage Mode off**, since it forces `RenderHDREnabled` false
  (`llviewercontrol.cpp:358`) and `renderFinalize` then calls `gammaCorrect()`
  instead of `tonemap()` (`pipeline.cpp:9217-9239`). Requires GL > 4.05 too.
- **Tone Mapping Mix at 1.0.**
- Snapshots taken with "no post" (`gSnapshotNoPost`) legitimately show no
  mapper — expected, not a bug.

Also left for in-world review: whether each look is *pleasant*, and GT7's cost on
real hardware. Given the environmental preconditions above, this warrants a
written testplan under the repo's existing `doc/testplans/` convention.

## Open decision

**Should the featuretable rows keep forcing `RenderTonemapType`?**
**Recommendation: delete all 24 rows.**

The setting is not in `mSkippedFeatures` — that list holds only
`RenderAnisotropic`, `RenderGamma`, `RenderVBOEnable`, `RenderFogRatio`
(`llfeaturemanager.cpp:267-270`) — so even `skipFeatures=true` does not protect
it. It is reset to ACES by the prefs quality slider
(`llfloaterpreference.cpp:2431`), both performance floaters
(`llfloaterperformance.cpp:558`, `fsfloaterperformance.cpp:897`), "Reset to
recommended settings" (`:1447`), and settings-backup restore (`:5815`).

A photographer picks AgX Golden, nudges the quality slider once, and is silently
back on ACES. That is precisely this feature's target audience hitting it
routinely, and a tone mapper is a taste choice rather than a performance knob.

Deleting the rows is **data-only**, so it preserves the "no C++ changes"
property. Because all 24 rows carry the same `1 1`, the `settings.xml` default of
1 already reproduces today's behaviour at every quality level for new users; only
the forced *overwrite* goes away. The alternative — adding it to
`mSkippedFeatures` — would require a C++ change.

Still Five's call, since it changes behaviour for existing users. Not blocking.

## Out of scope

- Exposing Lottes' contrast/shoulder parameters as settings
- Per-sky tone mapper selection (`LLSettingsSky` has `mTonemapMix` but no type)
- Re-enabling the commented-out water tone mapping call
- LUT-based tone mapping; HDR display output
- Translating the new combo labels

## Corrections from review

Four adversarial reviewers checked this spec; five substantive claims were
refuted and corrected:

1. **GT7 was wrong in every particular.** The original draft called it SIGGRAPH
   2022, claimed no canonical implementation existed, described it as a ~20-line
   per-channel/max-channel blend, and warned it would need a tuning pass. It is
   SIGGRAPH 2025; an official MIT reference exists; it is ICtCp/PQ-based and
   nits-parameterised; and it is now the best-specified mapper here.
2. **"Lottes will not compile" was false.** Built-in calls in `const`
   initialisers have been legal since GLSL 1.20. The real restriction is
   user-defined calls, which affects Uncharted 2 instead.
3. **NaN guards were missing entirely** — the most serious defect, and the one
   that would have shipped visibly broken.
4. **Water does not get the new mappers** (call site commented out); the original
   implied it would.
5. **"No translation file must be touched" was false** — once item 1 is relabelled
   "ACES (Hill)", four overlays that pin it to plain "ACES" must be updated, or
   `fr`/`zh` users see two indistinguishable "ACES" entries.
6. **Combo labels were never width-checked.** "Gran Turismo (Uchimura 2017)"
   would truncate on the ~110px phototools button; the label is now "Gran
   Turismo".
7. **A stale control value shows a stale label, not a blank** — an earlier
   revision of this spec claimed blank. `updateLabel()` is simply never called.
8. **Minor:** phototools combo items are at 2332-2339 (combo opens at 2322); only
   line 2348 carries the quoted tooltip phrase; there is no "row-major-reading
   comment" above `ACESInputMat` (that detail was invented, though the matrix is
   correctly column-major); and the `default:` case is observably a no-op rather
   than a graceful degradation.

One reviewer disagreement was resolved by direct inspection rather than by
seniority: a claim that no `RenderHDREnabled` gate exists on the tone mapping
pass is **wrong** — `pipeline.cpp:9217-9239` is an explicit
`if (hdr) { … tonemap(…) } else { gammaCorrect(…) }`.

Confirmed unchanged: all mapper constants for 2–8 and 10; the behaviour-
preserving refactor; "no C++ changes"; the featuretable count and column
semantics; and the translated-skin merge behaviour for *new* items.
