/**
 * @file fstextureupload.cpp
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

#include "linden_common.h"

#include "fstextureupload.h"

#include "llgl.h"
#include "llthread.h"

#include <deque>

namespace
{
    // 32 MB of host-visible staging memory. The largest level that may use it
    // is half of that: a 2048x2048 RGBA (or RGB expanded to RGBA) base level.
    constexpr U32 RING_SIZE = 32u * 1024u * 1024u;
    constexpr U32 MAX_UPLOAD = RING_SIZE / 2;
    // Below this the map/fence bookkeeping costs more than the copy it saves.
    constexpr U32 MIN_UPLOAD = 32u * 1024u;
    constexpr U32 OFFSET_ALIGN = 256;
    constexpr size_t MAX_IN_FLIGHT = 512;

    struct UploadRegion
    {
        U32 mOffset;
        U32 mSize;
        GLsync mFence;
    };

    bool sEnabled = false;
    bool sFailed = false;   // creation failed or unsupported; stays off until releaseGL()
    thread_local bool sInScope = false; // only ever set on the main thread
    GLuint sBuffer = 0;
    U8* sMapped = nullptr;
    U32 sHead = 0;
    std::deque<UploadRegion> sInFlight;
    FSTextureUpload::Stats sStats;

    bool is_supported()
    {
        return gGLManager.mGLVersion >= 4.39f
#if LL_WINDOWS
            && glBufferStorage != nullptr
            && glMapBufferRange != nullptr
            && glFenceSync != nullptr
            && glClientWaitSync != nullptr
            && glDeleteSync != nullptr
#endif
            ;
    }

    bool ensure_buffer()
    {
        if (sMapped)
        {
            return true;
        }
        if (sFailed)
        {
            return false;
        }
        if (!is_supported())
        {
            LL_INFOS("Texture") << "FSPerfTextureUploadPBO: needs OpenGL 4.4 (glBufferStorage); using normal uploads" << LL_ENDL;
            sFailed = true;
            return false;
        }

        const GLbitfield flags = GL_MAP_WRITE_BIT | GL_MAP_PERSISTENT_BIT | GL_MAP_COHERENT_BIT;
        glGenBuffers(1, &sBuffer);
        glBindBuffer(GL_PIXEL_UNPACK_BUFFER, sBuffer);
        glBufferStorage(GL_PIXEL_UNPACK_BUFFER, RING_SIZE, nullptr, flags);
        sMapped = (U8*)glMapBufferRange(GL_PIXEL_UNPACK_BUFFER, 0, RING_SIZE, flags);
        glBindBuffer(GL_PIXEL_UNPACK_BUFFER, 0);

        if (!sMapped)
        {
            LL_WARNS("Texture") << "FSPerfTextureUploadPBO: could not map the upload ring; using normal uploads" << LL_ENDL;
            glDeleteBuffers(1, &sBuffer);
            sBuffer = 0;
            sFailed = true;
            return false;
        }

        sHead = 0;
        LL_INFOS("Texture") << "FSPerfTextureUploadPBO: " << (RING_SIZE >> 20) << " MB upload ring created" << LL_ENDL;
        return true;
    }

    // Frees the regions whose uploads the GPU has finished, oldest first.
    void retire()
    {
        while (!sInFlight.empty())
        {
            const UploadRegion& r = sInFlight.front();
            if (glClientWaitSync(r.mFence, 0, 0) == GL_TIMEOUT_EXPIRED)
            {
                break;
            }
            glDeleteSync(r.mFence);
            sInFlight.pop_front();
        }
        if (sInFlight.empty())
        {
            sHead = 0;
        }
    }

    // Finds room for size bytes without overwriting anything still in flight.
    bool allocate(U32 size, U32& offset)
    {
        if (sInFlight.empty())
        {
            offset = 0;
            sHead = size;
            return size <= RING_SIZE;
        }
        if (sInFlight.size() >= MAX_IN_FLIGHT)
        {
            return false;
        }

        const U32 tail = sInFlight.front().mOffset;
        const bool wrapped = sInFlight.back().mOffset < tail;
        if (!wrapped)
        {
            // In use: [tail, sHead). Free: [sHead, end) and [0, tail).
            if (sHead + size <= RING_SIZE)
            {
                offset = sHead;
                sHead += size;
                return true;
            }
            if (size <= tail)
            {
                offset = 0;
                sHead = size;
                return true;
            }
            return false;
        }

        // In use: [tail, end) and [0, sHead). Free: [sHead, tail).
        if (sHead + size <= tail)
        {
            offset = sHead;
            sHead += size;
            return true;
        }
        return false;
    }

    void copy_rgb_to_rgba(const U8* src, U8* dst, U32 pixel_count)
    {
        // Sequential 32-bit writes; the destination is write-combined memory.
        U32* out = (U32*)dst;
        for (U32 i = 0; i < pixel_count; ++i)
        {
            out[i] = (U32)src[0] | ((U32)src[1] << 8) | ((U32)src[2] << 16) | 0xFF000000u;
            src += 3;
        }
    }
}

namespace FSTextureUpload
{
    void setPBOEnabled(bool enabled)
    {
        if (sEnabled && !enabled)
        {
            releaseGL();
        }
        sEnabled = enabled;
    }

    bool isPBOEnabled()
    {
        return sEnabled;
    }

    void releaseGL()
    {
        for (const UploadRegion& r : sInFlight)
        {
            glDeleteSync(r.mFence);
        }
        sInFlight.clear();
        if (sBuffer)
        {
            if (sMapped)
            {
                glBindBuffer(GL_PIXEL_UNPACK_BUFFER, sBuffer);
                glUnmapBuffer(GL_PIXEL_UNPACK_BUFFER);
                glBindBuffer(GL_PIXEL_UNPACK_BUFFER, 0);
            }
            glDeleteBuffers(1, &sBuffer);
        }
        sBuffer = 0;
        sMapped = nullptr;
        sHead = 0;
        sFailed = false;
    }

    bool texImage2D(U32 target, S32 miplevel, S32 intformat, S32 width, S32 height,
                    U32 pixformat, U32 pixtype, const void* pixels)
    {
        if (!sEnabled || !sInScope || !pixels || pixtype != GL_UNSIGNED_BYTE || width <= 0 || height <= 0)
        {
            return false;
        }

        // Formats the driver can copy straight from the buffer. 24-bit RGB is
        // expanded to RGBA here so the driver never has to read the
        // (uncached) buffer back to swizzle it. GL_UNPACK_ALIGNMENT is 1
        // (LLRender::init) and GL_UNPACK_ROW_LENGTH 0 in setImage, so rows are
        // tightly packed.
        U32 src_components = 0;
        U32 upload_format = pixformat;
        bool expand_rgb = false;
        switch (pixformat)
        {
        case GL_RGBA:
        case GL_BGRA:
            src_components = 4;
            break;
        case GL_RGB:
            src_components = 3;
            upload_format = GL_RGBA;
            expand_rgb = true;
            break;
        case GL_RG:
            src_components = 2;
            break;
        case GL_RED:
            src_components = 1;
            break;
        default:
            return false;
        }

        const U64 pixel_count = (U64)width * (U64)height;
        const U64 upload_bytes = pixel_count * (expand_rgb ? 4 : src_components);
        if (upload_bytes < MIN_UPLOAD || upload_bytes > MAX_UPLOAD)
        {
            return false;
        }

        if (!ensure_buffer())
        {
            return false;
        }

        LL_PROFILE_ZONE_NAMED_CATEGORY_TEXTURE("glTexImage2D PBO");

        retire();

        const U32 aligned = ((U32)upload_bytes + OFFSET_ALIGN - 1) & ~(OFFSET_ALIGN - 1);
        U32 offset = 0;
        if (!allocate(aligned, offset))
        {
            ++sStats.mRingFull;
            return false;
        }

        if (expand_rgb)
        {
            copy_rgb_to_rgba((const U8*)pixels, sMapped + offset, (U32)pixel_count);
        }
        else
        {
            memcpy(sMapped + offset, pixels, (size_t)upload_bytes);
        }

        glBindBuffer(GL_PIXEL_UNPACK_BUFFER, sBuffer);
        glTexImage2D(target, miplevel, intformat, width, height, 0, upload_format, GL_UNSIGNED_BYTE, (const void*)(uintptr_t)offset);
        glBindBuffer(GL_PIXEL_UNPACK_BUFFER, 0);

        GLsync fence = glFenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0);
        if (fence)
        {
            sInFlight.push_back({ offset, aligned, fence });
        }
        else
        {
            // Can't track when the bytes are free again; make sure they are.
            glFinish();
        }

        sStats.mPBOBytes += upload_bytes;
        ++sStats.mPBOUploads;
        return true;
    }

    UploadScope::UploadScope()
        : mPrev(sInScope)
    {
        if (sEnabled && on_main_thread())
        {
            sInScope = true;
        }
    }

    UploadScope::~UploadScope()
    {
        sInScope = mPrev;
    }

    Stats takeStats()
    {
        Stats out = sStats;
        sStats = Stats();
        return out;
    }
}
