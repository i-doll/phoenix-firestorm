/**
 * @file fsparalleltexture.h
 * @brief Texture decode priority face pass on worker threads (FSPerfParallelTexturePriority)
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

#ifndef FS_PARALLELTEXTURE_H
#define FS_PARALLELTEXTURE_H

#include "llpointer.h"
#include "stdtypes.h"

#include <unordered_set>
#include <vector>

class LLViewerFetchedTexture;

// What the face loop of LLViewerTextureList::updateImageDecodePriority produces for one texture:
// the largest biased face virtual size, whether any face was in the frustum, and the number of
// faces counted (before the "too many faces" check).
struct FSTexturePriorityFaces
{
    F32  mMaxVSize = 0.f;
    U32  mFaceCount = 0;
    bool mOnScreen = false;
};

// Parallel face pass for the regular texture sweep in updateImagesFetchTextures.
//
// prepare() runs, before the sweep, the expensive and side-effect-free part of
// updateImageDecodePriority for every texture the sweep will visit: LLFace::calcPixelArea for the
// stale faces (each face once, on one thread) and the per-texture reduction over the face list.
// The sweep then calls updateImageDecodePriority as before, in the same order and under the same
// time budget, with the precomputed result: everything that changes textures, the fetcher or the
// list (updateSceneTextureStats, processTextureStats, updateFetch, deleteImage, raw image
// cleanup, spot light priorities) stays on the main thread.
class FSParallelTexture
{
public:
    typedef std::vector<LLPointer<LLViewerFetchedTexture> > entries_t;
    typedef std::unordered_set<LLViewerFetchedTexture*> skip_set_t;

    // Returns true when the face pass ran in parallel for this sweep. False when the setting is
    // off or the sweep is too small; then result() returns nullptr for every entry.
    static bool prepare(const entries_t& entries, const skip_set_t& skip);

    // Precomputed result for entries[index], or nullptr when it has none (not eligible when
    // prepare() ran: the caller then runs the serial face loop).
    static const FSTexturePriorityFaces* result(size_t index, const LLViewerFetchedTexture* imagep);

    // FSPerfParallelTexturePriorityVerify: the caller runs the serial face loop (which applies)
    // and hands both results here.
    static bool verifying();
    static void verify(const LLViewerFetchedTexture* imagep, const FSTexturePriorityFaces& parallel,
                       const FSTexturePriorityFaces& serial);

    // End of the sweep: drop the per-frame state.
    static void finish();

private:
    // Helpers that read private LLFace state (LLFace befriends this class).
    struct Params;
    static void reduceFaces(LLViewerFetchedTexture* imagep, const Params& p, FSTexturePriorityFaces& out);
    static void verifyFaces();
};

#endif // FS_PARALLELTEXTURE_H
