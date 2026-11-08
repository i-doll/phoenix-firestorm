/**
 * @file class3/deferred/screenSpaceReflUtil.glsl
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

uniform sampler2D sceneMap;
uniform sampler2D sceneDepth;

uniform vec2 screen_res;
uniform mat4 projection_matrix;
//uniform float zNear;
//uniform float zFar;
uniform mat4 inv_proj;
uniform mat4 modelview_delta;  // should be transform from last camera space to current camera space
uniform mat4 inv_modelview_delta;

float ndcZFromScreenDepth(float d);

float random (vec2 uv)
{
    return fract(sin(dot(uv, vec2(12.9898, 78.233))) * 43758.5453123); //simple random function
}

// Based off of https://github.com/RoundedGlint585/ScreenSpaceReflection/
// A few tweaks here and there to suit our needs.

vec2 generateProjectedPosition(vec3 pos)
{
    vec4 samplePosition = projection_matrix * vec4(pos, 1.f);
    samplePosition.xy = (samplePosition.xy / samplePosition.w) * 0.5 + 0.5;
    return samplePosition.xy;
}

// Takes the point already in clip space. Projection is linear, so the march below carries its
// position and step in clip space alongside view space and never multiplies by the matrix
// inside the loop.
bool projectScreenRay(vec4 clip, out vec2 tc)
{
    tc = vec2(0);
    if (clip.w <= 0.0 || clip.z < -clip.w || clip.z > clip.w)
    {
        return false;
    }
    tc = clip.xy / clip.w * 0.5 + 0.5;
    return all(greaterThanEqual(tc, vec2(0))) && all(lessThan(tc, vec2(1)));
}

bool isBinarySearchEnabled = true;
bool isAdaptiveStepEnabled = true;
bool isExponentialStepEnabled = true;
bool debugDraw = false;

uniform float iterationCount;
uniform float rayStep;
uniform float distanceBias;
uniform float depthRejectBias;
uniform float glossySampleCount;
uniform float adaptiveStepMultiplier;
uniform float noiseSine;

float epsilon = 0.1;

// FS: hits on the negative side of this view-space plane are misses (the water sets it to its
// surface, so it only reflects what is above it). Zero, as every other shader leaves it: no clip.
// A clipped hit ends the ray (the callers break out of the march) rather than marching on past
// it: deliberate. The ray has reached something below the surface, and whatever lies behind that
// along the same ray is hidden from it, so continuing would only find wrong hits.
uniform vec4 fs_ssr_clip_plane;

bool fsSSRClipped(vec3 p)
{
    return dot(fs_ssr_clip_plane.xyz, p) + fs_ssr_clip_plane.w < 0.0;
}

float getLinearDepth(vec2 tc)
{
    float depth = texture(sceneDepth, tc).r;

    // Only view-space z is wanted, so evaluate just the z and w rows of inv_proj rather than
    // the full unprojection -- this runs once per march step.
    vec4 ndc = vec4(tc * 2.0 - 1.0, ndcZFromScreenDepth(depth), 1.0);
    float z = dot(vec4(inv_proj[0].z, inv_proj[1].z, inv_proj[2].z, inv_proj[3].z), ndc);
    float w = dot(vec4(inv_proj[0].w, inv_proj[1].w, inv_proj[2].w, inv_proj[3].w), ndc);

    return -z / w;
}

bool traceScreenRay(vec3 position, vec3 reflection, out vec4 hitColor, out float hitDepth, float depth, sampler2D textureFrame)
{
    // transform position and reflection into same coordinate frame as the sceneMap and sceneDepth
    reflection += position;
    position = (inv_modelview_delta * vec4(position, 1)).xyz;
    reflection = (inv_modelview_delta * vec4(reflection, 1)).xyz;
    reflection -= position;

    depth = -position.z;

    vec3 step = rayStep * reflection;
    vec3 marchingPosition = position + step;

    // Clip-space twins of the march state. Every update below is a scale, a sum or a midpoint,
    // all of which commute with a linear map, so these stay exactly projection_matrix times
    // their view-space counterparts (a direction carries w = 0, a point w = 1).
    vec4 clipStep = projection_matrix * vec4(step, 0.0);
    vec4 clipMarch = projection_matrix * vec4(marchingPosition, 1.0);

    float delta = 0.0;
    float depthFromScreen;
    vec2 screenPosition;
    bool hit = false;
    hitColor = vec4(0);
    hitDepth = 0.0;
    bool haveFront = false;
    bool bracketed = false;
    vec3 frontPosition = position;
    vec3 backPosition = position;
    vec4 clipFront = clipMarch - clipStep;
    vec4 clipBack = clipFront;

    int i = 0;
    if (depth > depthRejectBias)
    {
        for (; i < iterationCount && !hit; i++)
        {
            if (!projectScreenRay(clipMarch, screenPosition))
            {
                hit = false;
                break;
            }
            depthFromScreen = getLinearDepth(screenPosition);
            delta = -marchingPosition.z - depthFromScreen;

            if (depth < depthFromScreen + epsilon && depth > depthFromScreen - epsilon)
            {
                break;
            }

            if (abs(delta) < distanceBias)
            {
                if (fsSSRClipped(marchingPosition))
                {
                    break;
                }
                vec4 color = vec4(1);
                if(debugDraw)
                    color = vec4( 0.5+ sign(delta)/2,0.3,0.5- sign(delta)/2, 0);
                hitColor = texture(sceneMap, screenPosition) * color;
                hitDepth = depthFromScreen;
                hit = true;
                break;
            }
            if (isBinarySearchEnabled && delta > 0)
            {
                bracketed = haveFront;
                backPosition = marchingPosition;
                clipBack = clipMarch;
                break;
            }
            if (delta < 0.0)
            {
                haveFront = true;
                frontPosition = marchingPosition;
                clipFront = clipMarch;
            }
            if (isAdaptiveStepEnabled)
            {
                float directionSign = sign(abs(marchingPosition.z) - depthFromScreen);
                //this is sort of adapting step, should prevent lining reflection by doing sort of iterative converging
                //some implementation doing it by binary search, but I found this idea more cheaty and way easier to implement
                float stepScale = 1.0 - rayStep * max(directionSign, 0.0);
                step *= stepScale;
                clipStep *= stepScale;
                marchingPosition += step * (-directionSign);
                clipMarch += clipStep * (-directionSign);
            }
            else
            {
                marchingPosition += step;
                clipMarch += clipStep;
            }

            if (isExponentialStepEnabled)
            {
                step *= adaptiveStepMultiplier;
                clipStep *= adaptiveStepMultiplier;
            }
        }
        if (isBinarySearchEnabled && bracketed)
        {
            for(; i < iterationCount && !hit; i++)
            {
                marchingPosition = mix(frontPosition, backPosition, 0.5);
                clipMarch = mix(clipFront, clipBack, 0.5);

                if (!projectScreenRay(clipMarch, screenPosition))
                {
                    hit = false;
                    break;
                }
                depthFromScreen = getLinearDepth(screenPosition);
                delta = -marchingPosition.z - depthFromScreen;
                if (delta > 0.0)
                {
                    backPosition = marchingPosition;
                    clipBack = clipMarch;
                }
                else
                {
                    frontPosition = marchingPosition;
                    clipFront = clipMarch;
                }

                if (depth < depthFromScreen + epsilon && depth > depthFromScreen - epsilon)
                {
                    break;
                }

                if (abs(delta) < distanceBias && depthFromScreen != (depth - distanceBias))
                {
                    if (fsSSRClipped(marchingPosition))
                    {
                        break;
                    }
                    vec4 color = vec4(1);
                    if(debugDraw)
                        color = vec4( 0.5+ sign(delta)/2,0.3,0.5- sign(delta)/2, 0);
                    hitColor = texture(sceneMap, screenPosition) * color;
                    hitDepth = depthFromScreen;
                    hit = true;
                    break;
                }
            }
        }
    }

    return hit;
}

uniform vec3 POISSON3D_SAMPLES[128] = vec3[128](
    vec3(0.5433144, 0.1122154, 0.2501391),
    vec3(0.6575254, 0.721409, 0.16286),
    vec3(0.02888453, 0.05170321, 0.7573566),
    vec3(0.06635678, 0.8286457, 0.07157445),
    vec3(0.8957489, 0.4005505, 0.7916042),
    vec3(0.3423355, 0.5053263, 0.9193521),
    vec3(0.9694794, 0.9461077, 0.5406441),
    vec3(0.9975473, 0.02789414, 0.7320132),
    vec3(0.07781899, 0.3862341, 0.918594),
    vec3(0.4439073, 0.9686955, 0.4055861),
    vec3(0.9657035, 0.6624081, 0.7082613),
    vec3(0.7712346, 0.07273269, 0.3292839),
    vec3(0.2489169, 0.2550394, 0.1950516),
    vec3(0.7249326, 0.9328285, 0.3352458),
    vec3(0.6028461, 0.4424961, 0.5393377),
    vec3(0.2879795, 0.7427881, 0.6619173),
    vec3(0.3193627, 0.0486145, 0.08109283),
    vec3(0.1233155, 0.602641, 0.4378719),
    vec3(0.9800708, 0.211729, 0.6771586),
    vec3(0.4894537, 0.3319927, 0.8087631),
    vec3(0.4802743, 0.6358885, 0.814935),
    vec3(0.2692913, 0.9911493, 0.9934899),
    vec3(0.5648789, 0.8553897, 0.7784553),
    vec3(0.8497344, 0.7870212, 0.02065313),
    vec3(0.7503014, 0.2826185, 0.05412734),
    vec3(0.8045461, 0.6167251, 0.9532926),
    vec3(0.04225039, 0.2141281, 0.8678675),
    vec3(0.07116079, 0.9971236, 0.3396397),
    vec3(0.464099, 0.480959, 0.2775862),
    vec3(0.6346927, 0.31871, 0.6588384),
    vec3(0.449012, 0.8189669, 0.2736875),
    vec3(0.452929, 0.2119148, 0.672004),
    vec3(0.01506042, 0.7102436, 0.9800494),
    vec3(0.1970513, 0.4713539, 0.4644522),
    vec3(0.13715, 0.7253224, 0.5056525),
    vec3(0.9006432, 0.5335414, 0.02206874),
    vec3(0.9960898, 0.7961011, 0.01468861),
    vec3(0.3386469, 0.6337739, 0.9310676),
    vec3(0.1745718, 0.9114985, 0.1728188),
    vec3(0.6342545, 0.5721557, 0.4553517),
    vec3(0.1347412, 0.1137158, 0.7793725),
    vec3(0.3574478, 0.3448052, 0.08741581),
    vec3(0.7283059, 0.4753885, 0.2240275),
    vec3(0.8293507, 0.9971212, 0.2747005),
    vec3(0.6501846, 0.000688076, 0.7795712),
    vec3(0.01149416, 0.4930083, 0.792608),
    vec3(0.666189, 0.1875442, 0.7256873),
    vec3(0.8538797, 0.2107637, 0.1547532),
    vec3(0.5826825, 0.9750752, 0.9105834),
    vec3(0.8914346, 0.08266425, 0.5484225),
    vec3(0.4374518, 0.02987111, 0.7810078),
    vec3(0.2287418, 0.1443802, 0.1176908),
    vec3(0.2671157, 0.8929081, 0.8989366),
    vec3(0.5425819, 0.5524959, 0.6963879),
    vec3(0.3515188, 0.8304397, 0.0502702),
    vec3(0.3354864, 0.2130747, 0.141169),
    vec3(0.9729427, 0.3509927, 0.6098799),
    vec3(0.7585629, 0.7115368, 0.9099342),
    vec3(0.0140543, 0.6072157, 0.9436461),
    vec3(0.9190664, 0.8497264, 0.1643751),
    vec3(0.1538157, 0.3219983, 0.2984214),
    vec3(0.8854713, 0.2968667, 0.8511457),
    vec3(0.1910622, 0.03047311, 0.3571215),
    vec3(0.2456353, 0.5568692, 0.3530164),
    vec3(0.6927255, 0.8073994, 0.5808484),
    vec3(0.8089353, 0.8969175, 0.3427134),
    vec3(0.194477, 0.7985603, 0.8712182),
    vec3(0.7256182, 0.5653068, 0.3985921),
    vec3(0.9889427, 0.4584851, 0.8363391),
    vec3(0.5718582, 0.2127113, 0.2950557),
    vec3(0.5480209, 0.0193435, 0.2992659),
    vec3(0.6598953, 0.09478426, 0.92187),
    vec3(0.1385615, 0.2193868, 0.205245),
    vec3(0.7623423, 0.1790726, 0.1508465),
    vec3(0.7569032, 0.3773386, 0.4393887),
    vec3(0.5842971, 0.6538072, 0.5224424),
    vec3(0.9954313, 0.5763943, 0.9169143),
    vec3(0.001311183, 0.340363, 0.1488652),
    vec3(0.8167927, 0.4947158, 0.4454727),
    vec3(0.3978434, 0.7106082, 0.002727509),
    vec3(0.5459411, 0.7473233, 0.7062873),
    vec3(0.4151598, 0.5614617, 0.4748358),
    vec3(0.4440694, 0.1195122, 0.9624678),
    vec3(0.1081301, 0.4813806, 0.07047641),
    vec3(0.2402785, 0.3633997, 0.3898734),
    vec3(0.2317942, 0.6488295, 0.4221864),
    vec3(0.01145542, 0.9304277, 0.4105759),
    vec3(0.3563728, 0.9228861, 0.3282344),
    vec3(0.855314, 0.6949819, 0.3175117),
    vec3(0.730832, 0.01478493, 0.5728671),
    vec3(0.9304829, 0.02653277, 0.712552),
    vec3(0.4132186, 0.4127623, 0.6084146),
    vec3(0.7517329, 0.9978395, 0.1330464),
    vec3(0.5210338, 0.4318751, 0.9721575),
    vec3(0.02953994, 0.1375937, 0.9458942),
    vec3(0.1835506, 0.9896691, 0.7919457),
    vec3(0.3857062, 0.2682322, 0.1264563),
    vec3(0.6319699, 0.8735335, 0.04390657),
    vec3(0.5630485, 0.3339024, 0.993995),
    vec3(0.90701, 0.1512893, 0.8970422),
    vec3(0.3027443, 0.1144253, 0.1488708),
    vec3(0.9149003, 0.7382028, 0.7914025),
    vec3(0.07979286, 0.6892691, 0.2866171),
    vec3(0.7743186, 0.8046008, 0.4399814),
    vec3(0.3128662, 0.4362317, 0.6030678),
    vec3(0.1133721, 0.01605821, 0.391872),
    vec3(0.5185481, 0.9210006, 0.7889017),
    vec3(0.8217013, 0.325305, 0.1668191),
    vec3(0.8358996, 0.1449739, 0.3668382),
    vec3(0.1778213, 0.5599256, 0.1327691),
    vec3(0.06690693, 0.5508637, 0.07212365),
    vec3(0.9750564, 0.284066, 0.5727578),
    vec3(0.4350255, 0.8949825, 0.03574753),
    vec3(0.8931149, 0.9177974, 0.8123496),
    vec3(0.9055127, 0.989903, 0.813235),
    vec3(0.2897243, 0.3123978, 0.5083504),
    vec3(0.1519223, 0.3958645, 0.2640327),
    vec3(0.6840154, 0.6463035, 0.2346607),
    vec3(0.986473, 0.8714055, 0.3960275),
    vec3(0.6819352, 0.4169535, 0.8379834),
    vec3(0.9147297, 0.6144146, 0.7313942),
    vec3(0.6554981, 0.5014008, 0.9748477),
    vec3(0.9805915, 0.1318207, 0.2371372),
    vec3(0.5980836, 0.06796348, 0.9941338),
    vec3(0.6836596, 0.9917196, 0.2319056),
    vec3(0.5276511, 0.2745509, 0.5422578),
    vec3(0.829482, 0.03758276, 0.1240466),
    vec3(0.2698198, 0.0002266169, 0.3449324)
);

vec3 getPoissonSample(int i) {
    return POISSON3D_SAMPLES[i] * 2 - 1;
}

// Roughness fade for the ray march, and the glossiness at which it reaches zero.
//
// The caller that decides whether to march at all lives in another compilation unit and cannot
// see a constant declared here, so it asks for the threshold through a function. It used to
// carry its own literal instead, and the two disagreed: this fade ramped up from 0.567 while the
// gate rejected everything below 0.9, so the whole ramp was unreachable and screen-space
// reflections switched on at full strength in a single step -- a hard visible edge across any
// surface whose roughness crossed 0.1.
#define SSR_GLOSS_FADE_SLOPE 3.0
#define SSR_GLOSS_FADE_BIAS  1.7

float ssrMinGlossiness()
{
    return SSR_GLOSS_FADE_BIAS / SSR_GLOSS_FADE_SLOPE;
}

#ifdef FS_SSR_HIZ
// ---------------------------------------------------------------------------------------------
// Hierarchical-Z screen-space reflections (FSRenderSSRHiZ).
//
// ssrHiZMap is a mip pyramid of the previous frame's scene map built by fsSSRHiZF.glsl: a holds
// the nearest linear depth under each texel and rgb the box-filtered scene colour. The march runs
// in (pixel x, pixel y, 1 / linear depth), where a straight view-space ray is a straight line, and
// climbs to coarser levels while the ray stays in front of everything a cell holds, so empty space
// costs a step per cell rather than a step per pixel. At level 0 a candidate is checked against
// the exact depth: hits more than ssrThickness behind the surface (the ray passing behind a thin
// object) and hits on a surface's back face are skipped and the march goes on behind them.
//
// Each hit carries a confidence -- screen-edge distance of the HIT (not of the pixel),
// penetration depth, and how directly the ray heads back toward the camera -- which becomes the
// alpha the caller blends the reflection probe result with, so SSR fades into the probes rather
// than cutting off.
//
// ssrHiZActive is set by the pipeline only while ssrHiZMap matches sceneMap; anything else (the
// water pass, which swaps in its own scene map) keeps the classic march.
// ---------------------------------------------------------------------------------------------
uniform sampler2D ssrHiZMap;
uniform float ssrHiZActive;
uniform float ssrHiZSteps;
uniform float ssrThickness;
uniform float ssrEdgeFade;
uniform float ssrConeTrace;

// View position (previous frame) -> (pixel x, pixel y, 1 / linear depth). For a perspective
// projection clip w is the linear depth, so all three are affine in screen space.
vec3 fsSSRScreen(vec3 v, vec2 res)
{
    vec4 c = projection_matrix * vec4(v, 1.0);
    return vec3((c.xy / c.w * 0.5 + 0.5) * res, 1.0 / c.w);
}

vec3 fsSSRViewPosAt(ivec2 px, vec2 res)
{
    float d = texelFetch(sceneDepth, px, 0).r;
    vec4 p = inv_proj * vec4((vec2(px) + 0.5) / res * 2.0 - 1.0, ndcZFromScreenDepth(d), 1.0);
    return p.xyz / p.w;
}

// Normal of the surface at a hit, rebuilt from the depth buffer (the scene map has no normals),
// facing the camera. Uses the neighbour on the side with the smaller depth step so silhouettes
// don't bend it. Zero when it can't be told.
vec3 fsSSRHitNormal(ivec2 px, vec2 res, vec3 c)
{
    ivec2 m = ivec2(res) - 1;
    vec3 l = fsSSRViewPosAt(clamp(px - ivec2(1, 0), ivec2(0), m), res);
    vec3 r = fsSSRViewPosAt(clamp(px + ivec2(1, 0), ivec2(0), m), res);
    vec3 b = fsSSRViewPosAt(clamp(px - ivec2(0, 1), ivec2(0), m), res);
    vec3 t = fsSSRViewPosAt(clamp(px + ivec2(0, 1), ivec2(0), m), res);
    vec3 dx = (abs(r.z - c.z) < abs(c.z - l.z)) ? r - c : c - l;
    vec3 dy = (abs(t.z - c.z) < abs(c.z - b.z)) ? t - c : c - b;
    vec3 n = cross(dx, dy);
    float len = length(n);
    if (len < 1e-8)
    {
        return vec3(0.0);
    }
    n /= len;
    return dot(n, c) > 0.0 ? -n : n;
}

// position, dir - current-frame view space. Returns false on a miss. On a hit: hitUV in the
// scene map, confidence in [0,1], travelPx the hit's screen distance from the ray origin.
bool fsTraceHiZ(vec3 position, vec3 dir, out vec2 hitUV, out float confidence, out float travelPx)
{
    hitUV = vec2(0.0);
    confidence = 0.0;
    travelPx = 0.0;

    // Into the previous frame's camera space, like traceScreenRay.
    vec3 p0 = (inv_modelview_delta * vec4(position, 1.0)).xyz;
    vec3 d = normalize(mat3(inv_modelview_delta) * dir);

    float depth0 = -p0.z;
    if (depth0 <= max(depthRejectBias, 0.0))
    {
        return false;
    }

    vec2 res = vec2(textureSize(ssrHiZMap, 0));

    // A ray heading toward the camera is cut short of the camera plane.
    float len = 1000.0;
    if (d.z > 0.0)
    {
        len = min(len, (depth0 - 0.05) / d.z);
        if (len <= 0.0)
        {
            return false;
        }
    }

    vec3 s0 = fsSSRScreen(p0, res);
    vec3 sd = fsSSRScreen(p0 + d * len, res) - s0;

    // Parameter range that stays on screen.
    float tMax = 1.0;
    if (sd.x > 0.0) tMax = min(tMax, (res.x - s0.x) / sd.x);
    else if (sd.x < 0.0) tMax = min(tMax, -s0.x / sd.x);
    if (sd.y > 0.0) tMax = min(tMax, (res.y - s0.y) / sd.y);
    else if (sd.y < 0.0) tMax = min(tMax, -s0.y / sd.y);

    float spanPx = max(abs(sd.x), abs(sd.y));
    if (spanPx * tMax < 1.0)
    {
        return false; // the ray barely moves on screen
    }

    // Tiny components get a tiny positive stand-in: the cell boundary is then picked on the same
    // side the crossing is computed toward, so t never steps backwards.
    vec2 sdSafe = vec2(abs(sd.x) < 1e-6 ? 1e-6 : sd.x, abs(sd.y) < 1e-6 ? 1e-6 : sd.y);
    vec2 invD = 1.0 / sdSafe;
    vec2 cellStep = step(0.0, sdSafe);
    float tEps = 0.01 / spanPx; // a hundredth of a pixel
    int maxLevel = int(floor(log2(max(res.x, res.y))));
    ivec2 lastPx = ivec2(res) - 1;

    // Start just outside the origin pixel.
    vec2 tb0 = (floor(s0.xy) + cellStep - s0.xy) * invD;
    float t = min(tb0.x, tb0.y) + tEps;

    int level = 0;
    int steps = int(ssrHiZSteps);
    bool hit = false;
    vec3 h = s0;
    float pen = 0.0;

    for (int i = 0; i < steps; i++)
    {
        if (t >= tMax)
        {
            break;
        }
        vec3 p = s0 + sd * t;
        float cellSize = exp2(float(level));
        vec2 cell = floor(p.xy / cellSize);
        vec2 tb = ((cell + cellStep) * cellSize - s0.xy) * invD;
        float tExit = min(tb.x, tb.y);

        ivec2 lastTexel = textureSize(ssrHiZMap, level) - 1;
        float zNear = texelFetch(ssrHiZMap, clamp(ivec2(cell), ivec2(0), lastTexel), level).a;
        float izNear = 1.0 / max(zNear, 1e-4);
        // farthest point of the ray inside this cell (1/z shrinks with distance)
        float izFar = min(p.z, s0.z + sd.z * min(tExit, tMax));

        if (izFar > izNear)
        {
            // Entirely in front of everything in the cell: skip it and widen the search.
            t = tExit + tEps;
            level = min(level + 1, maxLevel);
            continue;
        }

        // Where the ray first reaches the cell's nearest depth.
        float tz = t;
        if (p.z > izNear)
        {
            tz = clamp((izNear - s0.z) / sd.z, t, tExit);
        }

        if (level > 0)
        {
            t = tz;
            level--;
            continue;
        }

        h = s0 + sd * tz;
        ivec2 px = clamp(ivec2(h.xy), ivec2(0), lastPx);
        float dRaw = texelFetch(sceneDepth, px, 0).r;
#ifdef REVERSE_Z
        if (dRaw <= 0.0)
#else
        if (dRaw >= 1.0)
#endif
        {
            break; // reached the sky: nothing nearer to find
        }
        vec3 sp = fsSSRViewPosAt(px, res);
        pen = 1.0 / h.z + sp.z; // ray depth - surface depth
        if (pen < ssrThickness)
        {
            vec3 n = fsSSRHitNormal(px, res, sp);
            if (dot(n, d) < 0.0 || n == vec3(0.0))
            {
                hit = true;
                break;
            }
        }
        // Behind a thin surface, or on its back face: carry on past this pixel.
        t = tExit + tEps;
    }

    if (!hit)
    {
        return false;
    }

    hitUV = h.xy / res;
    travelPx = distance(h.xy, s0.xy);

    vec2 e = min(hitUV, 1.0 - hitUV);
    float edge = ssrEdgeFade > 0.0 ? smoothstep(0.0, ssrEdgeFade, min(e.x, e.y)) : 1.0;
    float x = clamp(pen / max(ssrThickness, 1e-3), 0.0, 1.0);
    float thick = 1.0 - x * x;
    // Rays heading back toward the camera mostly find what is behind it, which the screen lacks.
    float facing = 1.0 - smoothstep(0.4, 0.9, dot(d, -normalize(p0)));
    confidence = edge * thick * facing;
    return true;
}

float fsTapScreenSpaceReflectionHiZ(vec2 tc, vec3 viewPos, vec3 n, vec3 rayDir, inout vec4 collectedColor, sampler2D source, float glossiness)
{
    collectedColor = vec4(0);

    vec3 rayDirection = normalize(rayDir);

    // The classic per-pixel fades, without its screen-edge vignette of the PIXEL: the edge fade
    // is taken at the hit instead, where the missing information actually is.
    float vignette = clamp((dot(normalize(viewPos), n) * 0.5 + 0.5) * 5.5 - 0.8, 0, 1);
    float zFar = 128.0;
    vignette *= clamp(1.0 + (viewPos.z / zFar), 0.0, 1.0);
    vignette *= clamp(glossiness * SSR_GLOSS_FADE_SLOPE - SSR_GLOSS_FADE_BIAS, 0, 1);
    if (vignette <= 0.0)
    {
        return 0.0;
    }

    float roughness = clamp(1.0 - glossiness, 0.0, 1.0);

    vec2 uv;
    float conf;
    float travel;

    if (ssrConeTrace > 0.5 || roughness <= 0.0)
    {
        // One ray; roughness widens the colour lookup instead of scattering more rays. The
        // classic path spreads its rays over a cone of tangent `roughness`, so the footprint at
        // the hit is 2 * roughness * distance, read from the pyramid level of that size.
        if (!fsTraceHiZ(viewPos, rayDirection, uv, conf, travel))
        {
            return 0.0;
        }
        if (roughness > 0.0)
        {
            float lod = log2(max(2.0 * roughness * travel, 1.0));
            collectedColor.rgb = textureLod(ssrHiZMap, uv, lod).rgb;
        }
        else
        {
            collectedColor.rgb = texture(source, uv).rgb;
        }
        collectedColor.a = conf * vignette;
        return 1.0;
    }

    // Jittered rays, as the classic path, each traced through the pyramid.
    int totalSamples = clamp(int(ceil(glossySampleCount * roughness)), 1, max(1, int(glossySampleCount)));

    vec3 axis = abs(rayDirection.z) < 0.999 ? vec3(0, 0, 1) : vec3(0, 1, 0);
    vec3 firstBasis = normalize(cross(axis, rayDirection));
    vec3 secondBasis = cross(rayDirection, firstBasis);
    vec2 sampleRotation = vec2(random(tc), random(tc + vec2(17.0, 59.0)));

    float hits = 0.0;
    float confSum = 0.0;
    for (int i = 0; i < totalSamples; i++)
    {
        vec2 sampleUV = fract(POISSON3D_SAMPLES[i].xy + sampleRotation);
        float radius = sqrt(sampleUV.x);
        float angle = 6.28318530718 * sampleUV.y;
        vec2 coeffs = radius * vec2(cos(angle), sin(angle));
        vec3 dir = normalize(rayDirection + (firstBasis * coeffs.x + secondBasis * coeffs.y) * roughness);

        if (fsTraceHiZ(viewPos, dir, uv, conf, travel))
        {
            hits += 1.0;
            confSum += conf;
            collectedColor.rgb += texture(source, uv).rgb;
        }
    }

    if (hits > 0.0)
    {
        collectedColor.rgb /= hits;
    }
    collectedColor.a = confSum / float(totalSamples) * vignette;
    return hits;
}
#endif // FS_SSR_HIZ

// rayDir - the direction to march, supplied by the caller rather than derived here. The probe
//          tap bends its lookup toward the normal as the surface roughens (the GGX dominant
//          direction), and a mirror reflect() computed here pointed somewhere else, so the two
//          reflections being mixed together came from measurably different directions on every
//          rough surface. n is still wanted separately, for the grazing-angle fade below.
float tapScreenSpaceReflection(int totalSamples, vec2 tc, vec3 viewPos, vec3 n, vec3 rayDir, inout vec4 collectedColor, sampler2D source, float glossiness)
{
#ifdef TRANSPARENT_SURFACE
collectedColor = vec4(1, 0, 1, 1);
    return 0;
#endif
#ifdef FS_SSR_HIZ
    if (ssrHiZActive > 0.5)
    {
        return fsTapScreenSpaceReflectionHiZ(tc, viewPos, n, rayDir, collectedColor, source, glossiness);
    }
#endif
    collectedColor = vec4(0);
    int hits = 0;

    float depth = -viewPos.z;

    vec3 rayDirection = normalize(rayDir);

    vec2 uv2 = tc * screen_res;
    float c = (uv2.x + uv2.y) * 0.125;
    float jitter = mod( c, 1.0);

    vec2 screenpos = 1 - abs(tc * 2 - 1);
    float vignette = clamp((abs(screenpos.x) * abs(screenpos.y)) * 16,0, 1);
    vignette *= clamp((dot(normalize(viewPos), n) * 0.5 + 0.5) * 5.5 - 0.8, 0, 1);

    float zFar = 128.0;
    vignette *= clamp(1.0+(viewPos.z/zFar), 0.0, 1.0);

    vignette *= clamp(glossiness * SSR_GLOSS_FADE_SLOPE - SSR_GLOSS_FADE_BIAS, 0, 1);

    vec4 hitpoint;

    glossiness = clamp(1.0 - glossiness, 0.0, 1.0);

    totalSamples = clamp(int(ceil(glossySampleCount * glossiness)), 1, max(1, int(glossySampleCount)));

    vec3 axis = abs(rayDirection.z) < 0.999 ? vec3(0, 0, 1) : vec3(0, 1, 0);
    vec3 firstBasis = normalize(cross(axis, rayDirection));
    vec3 secondBasis = cross(rayDirection, firstBasis);

    {
        if (vignette > 0)
        {
            // Per-pixel rotation of the Poisson set; the same for every sample, so hash once.
            vec2 sampleRotation = vec2(random(tc), random(tc + vec2(17.0, 59.0)));

            for (int i = 0; i < totalSamples; i++)
            {
                vec3 reflectionDirectionRandomized = rayDirection;
                // Mirrors and transparent SSR use zero spread. Keep the same ray without
                // computing the random disk sample (sqrt, sin and cos).
                if (glossiness > 0.0)
                {
                    vec2 sampleUV = fract(POISSON3D_SAMPLES[i].xy + sampleRotation);
                    float radius = sqrt(sampleUV.x);
                    float angle = 6.28318530718 * sampleUV.y;
                    vec2 coeffs = radius * vec2(cos(angle), sin(angle));
                    reflectionDirectionRandomized += (firstBasis * coeffs.x + secondBasis * coeffs.y) * glossiness;
                }

                //float hitDepth;

                float hitDepth;
                bool hit = traceScreenRay(viewPos, normalize(reflectionDirectionRandomized), hitpoint, hitDepth, depth, source);

                hitpoint.a = 0;

                if (hit)
                {
                    ++hits;
                    collectedColor += hitpoint;
                    collectedColor.a += 1;
                }
            }

            if (hits > 0)
            {
                collectedColor /= hits;
            }
            else
            {
                collectedColor = vec4(0);
            }
        }
    }
    float hitAlpha = hits;
    hitAlpha /= totalSamples;
    collectedColor.a = hitAlpha * vignette;
    return hits;
}
