// Lens dirt: smudges and specks on the lens that only show where bloom light falls on them.
// Drawn over the finished bloom chain with blend factors (dst colour, one), so the bloom becomes
// bloom * (1 + mask * strength). The mask is procedural and fixed to the screen.

/*[EXTRA_CODE_HERE]*/

out vec4 frag_color;

in vec2 vary_fragcoord;

uniform vec2 screen_res;        // target size, for the aspect ratio
uniform float dirt_strength;

vec4 fsDirtHash(vec2 p)
{
    vec4 q = fract(vec4(p.xyxy) * vec4(0.1031, 0.1030, 0.0973, 0.1099));
    q += dot(q, q.wzxy + 33.33);
    return fract((q.xxyz + q.yzzw) * q.zywx);
}

// Soft round specks: at most one per cell, with its own place, size and density
float fsDirtSpecks(vec2 p, float seed, float coverage)
{
    vec2 cell = floor(p);
    vec2 f = p - cell;
    float m = 0.0;
    for (int y = -1; y <= 1; ++y)
    {
        for (int x = -1; x <= 1; ++x)
        {
            vec2 o = vec2(float(x), float(y));
            vec4 h = fsDirtHash(cell + o + seed);
            if (h.w > coverage)
            {
                continue;
            }
            float radius = 0.25 + 0.5 * h.z;
            float d = length(o + 0.2 + 0.6 * h.xy - f);
            // A soft disc with a slightly denser rim, the way a dried droplet looks
            float disc = 1.0 - smoothstep(radius * 0.4, radius, d);
            float rim = smoothstep(radius * 0.55, radius * 0.85, d) * disc;
            m = max(m, (disc * 0.7 + rim * 0.3) * (0.35 + 0.65 * fract(h.w * 7.31)));
        }
    }
    return m;
}

// Smears: a short soft stroke through each chosen cell, at its own angle
float fsDirtSmears(vec2 p, float coverage)
{
    vec2 cell = floor(p);
    vec2 f = p - cell;
    float m = 0.0;
    for (int y = -1; y <= 1; ++y)
    {
        for (int x = -1; x <= 1; ++x)
        {
            vec2 o = vec2(float(x), float(y));
            vec4 h = fsDirtHash(cell + o + 71.0);
            if (h.w > coverage)
            {
                continue;
            }
            float a = h.z * 3.14159265;
            vec2 dir = vec2(cos(a), sin(a));
            vec2 r = o + 0.5 + 0.3 * (h.xy - 0.5) - f;
            float along = dot(r, dir);
            float across = dot(r, vec2(-dir.y, dir.x));
            float len = 0.6 + 0.5 * h.x;
            float width = 0.04 + 0.08 * h.y;
            float s = (1.0 - smoothstep(0.0, width, abs(across))) * (1.0 - smoothstep(len * 0.3, len, abs(along)));
            m = max(m, s * (0.3 + 0.4 * fract(h.w * 5.17)));
        }
    }
    return m;
}

void main()
{
    // Square cells whatever the window shape, measured in screen heights
    vec2 p = (vary_fragcoord - 0.5) * vec2(screen_res.x / max(screen_res.y, 1.0), 1.0);

    float mask = fsDirtSpecks(p * 6.0, 0.0, 0.45);
    mask = max(mask, fsDirtSpecks(p * 15.0, 17.0, 0.3) * 0.7);
    mask = max(mask, fsDirtSmears(p * 3.0, 0.35));

    // A little more grime toward the edges of the lens, where it is wiped least
    mask *= 0.6 + 0.4 * smoothstep(0.2, 0.8, length(p));

    frag_color = vec4(vec3(mask * dirt_strength), 0.0);
}
