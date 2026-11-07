// Debanding dither: triangular noise of about one output step, added to display encoded colour
// just before it is written to an 8 bit target, so smooth gradients (sky, fog, soft lighting)
// dither between steps instead of banding. Linked into the passes that may do that write: the
// gamma correcting tone map, CAS, the non HDR gamma correction and the present shaders.
// FSRenderDeband; the pipeline sets fs_deband to 0 for a pass whose target is 16 bit float,
// so the dither happens once, at the first 8 bit write.

uniform float fs_deband;    // dither amplitude, in output units (strength / 255), 0 = off

// Interleaved gradient noise (Jimenez 2014): well spread over the screen, so the dither reads
// as a fine even grain rather than clumps
float fsDebandIGN(vec2 p)
{
    return fract(52.9829189 * fract(dot(p, vec2(0.06711056, 0.00583715))));
}

// Uniform [0, 1) to a triangular distribution on [-1, 1]. Triangular dither of +-1 step makes
// the mean and the variance of the error independent of the signal, so there is neither
// banding nor noise that comes and goes with the brightness.
float fsDebandTriangle(float u)
{
    float n = u * 2.0 - 1.0;
    return sign(n) * (1.0 - sqrt(1.0 - abs(n)));
}

vec3 fsDeband(vec3 c)
{
    if (fs_deband <= 0.0)
    {
        return c;
    }

    // Screen locked, not animated: a fixed pattern of one step cannot be seen, an animated
    // one fizzes in dark areas at low frame rates. Each channel takes the pattern at its own
    // offset, so the noise is not all in luminance.
    vec2 p = gl_FragCoord.xy;
    vec3 n = vec3(fsDebandTriangle(fsDebandIGN(p)),
                  fsDebandTriangle(fsDebandIGN(p + vec2(47.0, 17.0))),
                  fsDebandTriangle(fsDebandIGN(p + vec2(13.0, 61.0))));

    // Within one step of black or white the amplitude shrinks, so pure black and white stay
    // exact and the clamp at either end cannot bias the average
    vec3 room = clamp(min(c, 1.0 - c) / fs_deband, 0.0, 1.0);
    return c + n * (fs_deband * room);
}
