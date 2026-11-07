/**
 * @file fsssrhiz.h
 * @brief Scene pyramid for hierarchical-Z screen-space reflections (FSRenderSSRHiZ)
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

#ifndef FS_SSRHIZ_H
#define FS_SSRHIZ_H

#include "stdtypes.h"

class LLGLSLShader;
class LLRenderTarget;
class LLVertexBuffer;

// Once per frame, after the lit scene is copied into the SSR scene map, a mip pyramid of it is
// built: rgb the box-filtered colour (glossy cone lookups), a the nearest linear depth under each
// texel (hierarchical-Z tracing). Next frame's SSR in the lighting and alpha passes traces
// through it (class3/deferred/screenSpaceReflUtil.glsl, FS_SSR_HIZ).
//
// Everything is off and nothing is allocated unless RenderScreenSpaceReflections and
// FSRenderSSRHiZ are both on.
namespace FSSSRHiZ
{
    // Both settings on
    bool enabled();

    // Builds the two pyramid programs (call from the deferred shader load). They stay unloaded
    // while the feature is off.
    void loadShaders(S32 shader_level);

    // Rebuilds the pyramid from the SSR scene map (colour + depth)
    void build(LLRenderTarget& scene_map, LLVertexBuffer* screen_triangle);

    // Binds the pyramid and the tracing uniforms to a shader that samples the SSR scene map.
    // No-op for shaders without the FS_SSR_HIZ permutation. Until a pyramid of scene_map's size
    // exists (first frame, resize) the shader is told to use the classic march.
    void bind(LLGLSLShader& shader, const LLRenderTarget& scene_map);

    // Sends a bound shader back to the classic march, for passes that swap in a scene map the
    // pyramid wasn't built from (transparent water).
    void disable(LLGLSLShader& shader);

    void release();
}

#endif // FS_SSRHIZ_H
