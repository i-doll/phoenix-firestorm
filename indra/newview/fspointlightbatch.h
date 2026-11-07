/**
 * @file fspointlightbatch.h
 * @brief Instanced drawing of deferred local point lights
 *
 * $LicenseInfo:firstyear=2026&license=fsviewerlgpl$
 * Phoenix Firestorm Viewer Source Code
 * Copyright (C) 2026, Trish
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

#ifndef FS_POINTLIGHTBATCH_H
#define FS_POINTLIGHTBATCH_H

#include "llpointer.h"
#include "v3color.h"
#include "v4math.h"

#include <vector>

class LLCamera;
class LLGLSLShader;
class LLVertexBuffer;

// Draws the deferred local point lights whose box the camera is outside of as
// instanced light volumes (FSPerfBatchedLocalLights). LLPipeline::renderDeferredLighting
// still decides which lights are drawn and how; with batching on it queues each
// box-volume point light here instead of setting five uniforms and drawing it, then
// flushes the queue once: every light goes up in one uniform buffer upload and is
// drawn in one instanced call per FS_LIGHTS_PER_DRAW lights. Lights the camera is
// inside of, spotlights and projectors keep the stock paths.
class FSPointLightBatch
{
public:
    // Lights per instanced draw: two vec4 each, filling the 16 KB uniform block every
    // GL implementation has to support.
    static constexpr U32 FS_LIGHTS_PER_DRAW = 512;
    // Uniform buffer binding point, past the ones LLGLSLShader::UniformBlock names.
    static constexpr U32 FS_UBO_BINDING = 15;

    // Start of a deferred lighting pass. True when batching is on and usable this frame
    // (the setting, the program and the uniform buffer alignment); the queue is cleared.
    bool begin();

    // Queue one point light: agent-space centre, volume half size (radius * 1.5, as the
    // stock path sends), linear colour already scaled, and falloff.
    void add(const F32* center, F32 size, const LLColor3& linear_color, F32 falloff)
    {
        mData.emplace_back(center[0], center[1], center[2], size);
        mData.emplace_back(linear_color.mV[0], linear_color.mV[1], linear_color.mV[2], falloff);
    }

    // Upload and draw everything queued since begin(). The lighting target, additive
    // blend and depth buffer are those renderDeferredLighting already has bound.
    void flush(LLCamera* camera);

    // Point the program's FSPointLights block at FS_UBO_BINDING. Called once per link.
    static void setupProgram(LLGLSLShader& shader);

    // Release the GL objects (LLPipeline::cleanup).
    void release();

private:
    std::vector<LLVector4>    mData;            // per light: centre.xyz + size, colour.rgb + falloff
    LLPointer<LLVertexBuffer> mBoxVB;           // 6 faces x 2 triangles, unindexed, [-1, 1] cube
    U32                       mUBO = 0;
    S32                       mUBOAlignment = -1; // GL_UNIFORM_BUFFER_OFFSET_ALIGNMENT, queried once

    // Per frame counts for the debug log (tag "FSBatchedLights").
    U32 mFrameLights = 0;
    U32 mFrameDraws  = 0;
    U32 mFrameGLCalls = 0;
};

extern FSPointLightBatch gFSPointLightBatch;

#endif // FS_POINTLIGHTBATCH_H
