// Half-resolution GTAO (FSRenderGTAOHalfRes): one GTAO evaluation per 2x2 block of screen pixels

// Draws into a target of half the light map's width and height (rounded up), linked with
// fsGTAOF.glsl, whose maths is unchanged: the search radius and the pixel ceiling stay in
// full-resolution terms (screen_res is the full size), so the result matches the full-resolution
// pass apart from where it is evaluated. The sun program then reads this target through
// fsGTAOUpsampleF.glsl in place of fsGTAOF.glsl.
//
// Each texel evaluates GTAO at one real pixel of its 2x2 block, never a blend of them, so its
// depth and normal belong to a surface that exists. On a checkerboard the texel takes the nearest
// pixel of the block and its neighbours the farthest: where a silhouette crosses a block, both
// the foreground and the background are represented within every 2x2 texel neighbourhood, and
// the upsample finds a match on either side of the edge (Bavoil's checkerboard depth downsample).
// Sky pixels are skipped; a block of only sky is marked with a negative depth.
//
// Output: r = visibility [0,1], g = positive view depth, ba = octahedral view-space geometric
// normal. Needs a float target (RGBA16F).

/*[EXTRA_CODE_HERE]*/

out vec4 frag_color;

uniform vec2 screen_res; // full resolution of the depth buffer and the light map

float getDepth(vec2 pos_screen);
bool isFarDepth(float d);
vec4 getPositionWithDepth(vec2 pos_screen, float depth);
vec3 getGeoNorm(vec2 pos_screen);
float fsCalcGTAO(vec3 pos, vec3 norm, vec2 pos_screen);

vec2 fsGTAOSignNotZero(vec2 v)
{
    return vec2(v.x >= 0.0 ? 1.0 : -1.0, v.y >= 0.0 ? 1.0 : -1.0);
}

vec2 fsGTAOOctEncode(vec3 n)
{
    n /= abs(n.x) + abs(n.y) + abs(n.z);
    return n.z >= 0.0 ? n.xy : (1.0 - abs(n.yx)) * fsGTAOSignNotZero(n.xy);
}

void main()
{
    ivec2 lo = ivec2(gl_FragCoord.xy);
    ivec2 block = lo * 2;
    ivec2 full_max = ivec2(screen_res) - 1;
    bool want_near = ((lo.x + lo.y) & 1) == 0;

    bool found = false;
    float best_key = 0.0;
    float best_d = 0.0;
    vec2 best_tc = vec2(0.0);

    for (int i = 0; i < 4; ++i)
    {
        ivec2 px = min(block + ivec2(i & 1, i >> 1), full_max);
        vec2 tc = (vec2(px) + 0.5) / screen_res; // texel centre: the exact stored depth
        float d = getDepth(tc);
        if (isFarDepth(d))
        {
            continue;
        }
#ifdef REVERSE_Z
        float key = d;  // larger is nearer
#else
        float key = -d; // smaller is nearer
#endif
        if (!found || (want_near ? key > best_key : key < best_key))
        {
            found = true;
            best_key = key;
            best_d = d;
            best_tc = tc;
        }
    }

    if (!found)
    {
        frag_color = vec4(1.0, -1.0, 0.0, 0.0);
        return;
    }

    vec3 pos = getPositionWithDepth(best_tc, best_d).xyz;
    vec3 n = normalize(getGeoNorm(best_tc));
    float ao = fsCalcGTAO(pos, n, best_tc);
    frag_color = vec4(ao, max(-pos.z, 1e-4), fsGTAOOctEncode(n));
}
