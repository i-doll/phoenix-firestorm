/**
 * @file fsdynamicresolution.h
 * @brief Dynamic resolution scaling for the 3D scene (FSRenderDynamicResolution)
 *
 * $LicenseInfo:firstyear=2026&license=fsviewerlgpl$
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

#ifndef FS_DYNAMICRESOLUTION_H
#define FS_DYNAMICRESOLUTION_H

#include "stdtypes.h"

class LLGLSLShader;
class LLRenderTarget;

// When the frame rate falls below a target and the GPU is what holds it back, the main view's
// screen targets are reallocated one step smaller (100, 90, 80 ... percent of the window, down
// to a minimum), and the final present upscales the post-processed image to the window with a
// sharpening Catmull-Rom filter. When the GPU has headroom again the scale steps back up.
//
// Steps are discrete and at most one change happens per FSRenderDynamicResolutionInterval
// seconds, because every change reallocates the screen-sized targets (see the .cpp for why that
// was preferred over a sub-rectangle viewport). The UI, the shadow maps, the reflection probe
// and mirror targets and snapshots always stay at full resolution.
//
// With FSRenderDynamicResolution off every entry point returns after one cached bool read.
class FSDynamicResolution
{
public:
    // display(), once per frame right before the screen texture resize check: runs the
    // controller (which may reallocate the main screen targets) and opens the GPU timing.
    static void beginScene(bool for_snapshot);
    // render_ui(), right after renderFinalize(): closes the GPU timing of the scene.
    static void endScene();

    // allocateScreenBufferInternal(), main view only: scales the internal size.
    static void scaleMainTarget(U32& res_x, U32& res_y);
    // resizeShadowTexture(): the size the shadow maps are allocated from, unscaled.
    static void shadowBaseSize(U32& res_x, U32& res_y);

    // renderFinalize() present: the upscale program when the image is below the window size,
    // else the given fallback. setupPresent() sets its uniforms once it is bound.
    static LLGLSLShader& presentShader(LLGLSLShader& fallback, const LLRenderTarget* src);
    static void setupPresent(LLGLSLShader& shader);

    static F32 getScale();

    // Snapshots render at full resolution: the scale goes back to 1 while one of these lives.
    class SnapshotGuard
    {
    public:
        SnapshotGuard();
        ~SnapshotGuard();
        SnapshotGuard(const SnapshotGuard&) = delete;
        SnapshotGuard& operator=(const SnapshotGuard&) = delete;
    };
};

#endif // FS_DYNAMICRESOLUTION_H
