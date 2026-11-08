/**
 * @file fsvrs.h
 * @brief Variable rate shading of the main view's G-buffer pass (FSRenderVRS)
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

#ifndef FS_VRS_H
#define FS_VRS_H

#include "stdtypes.h"

class LLRenderTarget;

// Variable rate shading through GL_NV_shading_rate_image (NVIDIA Turing and newer).
//
// At the end of each main view frame, renderFinalize() calls buildRateImage() with the
// post-processed colour and the depth of that frame. Two small passes turn them into an R8UI
// image with one texel per shading rate tile (16x16 pixels on current hardware): low contrast
// tiles, the sky and tiles blurred by depth of field get coarse rates (2x1, 1x2, 2x2, and for
// the sky and strong blur up to 4x4), anything with an edge stays at 1x1. The next frame's
// G-buffer pass, and only that pass, is shaded with it: display() wraps the main camera's
// renderGeomDeferred() in beginGBuffer() / endGBuffer().
//
// Never used for snapshots, reflection probes, mirrors, shadows, post passes, the UI or HUDs,
// nor when the G-buffer is not the size the image was built for (dynamic resolution steps,
// window resizes): such frames render at full rate.
//
// With FSRenderVRS off, or without the extension, every entry point returns after a cached
// bool read.
class FSVRS
{
public:
    // display(), around the main camera's renderGeomDeferred().
    static void beginGBuffer(bool for_snapshot);
    static void endGBuffer();

    // renderFinalize(), after depth of field: builds next frame's shading rate image.
    static void buildRateImage(LLRenderTarget* color, LLRenderTarget* depth);

    // LLPipeline::releaseGLBuffers(): frees the GL objects.
    static void releaseGL();
};

#endif // FS_VRS_H
