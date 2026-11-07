/**
 * @file fstextureupload.h
 * @brief Streaming texture uploads through a persistently mapped pixel
 *        buffer object ring (FSPerfTextureUploadPBO).
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

#ifndef FS_TEXTUREUPLOAD_H
#define FS_TEXTUREUPLOAD_H

#include "stdtypes.h"

// Main-thread texture uploads normally hand glTexImage2D a pointer to client
// memory. The driver must copy (and for 24-bit RGB, usually swizzle to RGBA
// on the CPU) the whole level before the call returns, inside the frame.
//
// With the ring enabled, LLImageGL::setImage() copies each eligible level
// into a persistently mapped GL_PIXEL_UNPACK_BUFFER (expanding RGB to RGBA
// while copying, so the driver can DMA it without conversion) and issues
// glTexImage2D from the buffer offset. A fence per upload guards the bytes
// until the GPU has consumed them. When the ring is full the upload falls
// back to the old client-memory path rather than waiting, so this can never
// add a stall.
//
// Only used on the main thread, only for uncompressed 8-bit formats, and only
// on GL 4.4+ (glBufferStorage). Everything here is main-thread only.
namespace FSTextureUpload
{
    struct Stats
    {
        U64 mPBOBytes = 0;      // bytes written to the ring
        U32 mPBOUploads = 0;    // levels uploaded from the ring
        U32 mRingFull = 0;      // eligible levels that fell back because the ring was busy
    };

    // Call once per frame from the main thread with the setting value.
    // Turning it off releases the ring.
    void setPBOEnabled(bool enabled);
    bool isPBOEnabled();

    // Releases the buffer and fences (GL shutdown or context loss).
    void releaseGL();

    // Issues glTexImage2D from the ring when the upload is eligible and there
    // is room. Returns false if the caller must do the upload itself.
    bool texImage2D(U32 target, S32 miplevel, S32 intformat, S32 width, S32 height,
                    U32 pixformat, U32 pixtype, const void* pixels);

    // Uploads are only eligible inside this scope (LLImageGL::setImage), so
    // render targets, noise textures and other direct callers of
    // LLImageGL::setManualImage are untouched.
    class UploadScope
    {
    public:
        UploadScope();
        ~UploadScope();
    private:
        bool mPrev;
    };

    // Counters since the last call.
    Stats takeStats();
}

#endif // FS_TEXTUREUPLOAD_H
