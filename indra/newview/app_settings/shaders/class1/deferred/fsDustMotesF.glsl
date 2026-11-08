// Dust motes: draws the soft round disc of one mote. fsDustMotesV.glsl finds, culls, lights and
// sizes the motes, one point per grid cell; this only spreads its light over the point.
//
// Drawn at full resolution with additive blending over the HDR scene, after the sun rays and the
// volumetric projector light, so exposure and bloom treat both alike.

/*[EXTRA_CODE_HERE]*/

out vec4 frag_color;

in vec3 vary_color;     // the mote's light times its coverage at the centre
in float vary_sigma;    // the disc's spread in pixels
in float vary_size;     // the point's size in pixels

void main()
{
    vec2 d = (gl_PointCoord - 0.5) * vary_size;     // pixels from the mote's centre
    float cover = exp(-0.5 * dot(d, d) / (vary_sigma * vary_sigma));
    if (cover < 0.003)
    {
        discard;
    }
    frag_color = vec4(vary_color * cover, 0.0);
}
