/**
 * @file fsGPUDrivenShadowAlphaF.glsl
 * @brief Alpha tested shadow casters drawn with glMultiDrawElementsIndirect (FSPerfGPUDrivenShadowAlpha)
 *
 * $LicenseInfo:firstyear=2026&license=viewerlgpl$
 * Phoenix Firestorm Viewer Source Code
 * Copyright (C) 2026, The Phoenix Firestorm Project, Inc.
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
 * The Phoenix Firestorm Project, Inc., 1831 Oakwood Drive, Fairmont, Minnesota 56031-3225 USA
 * http://www.firestormviewer.org
 * $/LicenseInfo$
 */

#extension GL_ARB_bindless_texture : require

/*[EXTRA_CODE_HERE]*/

// The alpha test of the stock shadow program each draw stands in for (fsGPUDrivenShadowAlphaV.glsl), on the
// texture the stock diffuseLookup or diffuseMap would sample, as a bindless handle:
//  mode 0: shadowAlphaMaskF.glsl
//  mode 1: shadowAlphaMaskF.glsl with IS_FULLBRIGHT (no vertex alpha)
//  mode 2: treeShadowF.glsl
//  mode 3: pbrShadowAlphaMaskF.glsl

out vec4 frag_color;

in vec4 post_pos;
in float target_pos_x;
in vec4 vertex_color;
in vec2 vary_texcoord0;
flat in uvec2 fs_gdd_diffuse;
flat in float fs_gdd_minimum_alpha;
flat in uint fs_gdd_mode;

void main()
{
    float alpha = texture(sampler2D(fs_gdd_diffuse), vary_texcoord0.xy).a;

    if (fs_gdd_mode == 3u)
    {
        alpha *= vertex_color.a;

        if (alpha < fs_gdd_minimum_alpha)
        {
            discard;
        }
    }
    else
    {
        if (alpha < fs_gdd_minimum_alpha)
        {
            discard;
        }

        if (fs_gdd_mode != 2u)
        {
            if (fs_gdd_mode == 0u)
            {
                alpha *= vertex_color.a;
            }

            if (alpha < 0.05) // treat as totally transparent
            {
                discard;
            }

            if (alpha < 0.88) // treat as semi-transparent
            {
                if (fract(0.5*floor(target_pos_x / post_pos.w )) < 0.25)
                {
                    discard;
                }
            }
        }
    }

    frag_color = vec4(1,1,1,1);
}
