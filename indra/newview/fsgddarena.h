/**
 * @file fsgddarena.h
 * @brief Vertex arenas for GPU-driven draws (FSPerfGPUDrivenDraws, FSPerfGPUDrivenPBR)
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

#ifndef FS_GDDARENA_H
#define FS_GDDARENA_H

#include "llvertexbuffer.h"

#include <map>
#include <set>
#include <string>
#include <unordered_map>
#include <vector>

// First fit over [0, capacity) with neighbouring free ranges joined.
class FSGDDRangeAllocator
{
public:
    void reset();
    void grow(U32 capacity);
    bool allocate(U32 size, U32& offset);
    void release(U32 offset, U32 size);
    U32 capacity() const    { return mCapacity; }
    U32 used() const        { return mUsed; }

private:
    void insertFree(U32 offset, U32 size);
    void addFree(U32 offset, U32 size);
    void removeFree(std::map<U32, U32>::iterator it);

    std::map<U32, U32> mFree;                   // offset, size: for merging neighbours
    std::set<std::pair<U32, U32>> mFreeBySize;  // size, offset: best fit without scanning
    U32 mCapacity = 0;
    U32 mUsed = 0;
};

struct FSGDDArenaEntry
{
    U32 mSerial = 0;        // LLVertexBuffer::getFSContentSerial() of the copy
    U32 mVertex = 0;        // first vertex of the copy in the vertex arena
    U32 mNumVerts = 0;
    U32 mIndex = 0;         // first index of the copy in the index arena
    U32 mNumIndices = 0;
    U32 mLastFrame = 0;
    U32 mTypeMask = 0;      // the held types this copy has (1 << LLVertexBuffer::AttributeType)
    bool mVerified = false; // a verify mode read it back already
};

// A copy, on the GPU, of some attributes and the 16 bit indices of LLVertexBuffers: one GL buffer per
// attribute (laid out like the vertex buffer's own region of that attribute, so a vertex's attributes are at
// the same index in every one) and one index buffer. A vertex buffer is copied in with glCopyBufferSubData
// the first time a pass wants it, and again whenever its content serial (LLVertexBuffer::getFSContentSerial,
// bumped on every upload) changes; entries no pass used for a while are dropped. Keyed by buffer address,
// but only ever compared, never dereferenced: a buffer freed and another allocated at its address has a new
// content serial, so its entry is copied again.
class FSGDDArena
{
public:
    enum EFail
    {
        FAIL_NO_BUFFER = 0, // no GL buffer, no vertices or indices
        FAIL_INDEX32,       // 32 bit indices
        FAIL_MAPPED,        // mapped, an upload pending
        FAIL_ATTRIBUTES,    // lacks an attribute the arena holds
        FAIL_COPY_BUDGET,   // the per frame copy budget is used up
        FAIL_FULL,          // the arena is at its size limit
        FAIL_COUNT
    };
    static const char* failName(U32 reason);

    // types: the LLVertexBuffer::AttributeType values held. budget_setting: the S32 setting with the size
    // limit in MB; index_share: the part of it for indices is 1 / index_share. optional_mask: held types
    // (1 << type) a buffer may lack; they are copied when it has them (FSGDDArenaEntry::mTypeMask says which).
    FSGDDArena(const char* name, const std::vector<U32>& types, const char* budget_setting, S32 default_mb, U32 index_share,
               U32 optional_mask = 0);

    // The entry of vb, copied in now if needed and possible; nullptr (and why in reason) otherwise.
    FSGDDArenaEntry* find(LLVertexBuffer* vb, U32 frame, U32* reason = nullptr);
    // The entry of vb if it is there and current, without copying anything.
    FSGDDArenaEntry* peek(const LLVertexBuffer* vb);
    void beginFrame(U32 frame);
    // Points the attribute locations of the held types (formats as LLVertexBuffer::setupVertexBuffer) at the
    // arena and binds the index buffer. Only the types in attrib_mask (LLVertexBuffer::MAP_*) are set up.
    // MAP_TEXTURE_INDEX reads the w of the held positions, as setupVertexBuffer does.
    void bindForDraw(U32 attrib_mask);
    // Reads back an entry and its source buffer and compares them. Stalls; verify modes only.
    bool verifyEntry(const LLVertexBuffer* vb, FSGDDArenaEntry& entry);
    void releaseGL();

    U32 vertexMB() const;
    U32 indexMB() const;
    U32 vertexStride() const    { return mStride; }
    U32 entries() const         { return (U32)mEntries.size(); }
    U32 uploads() const         { return mUploads; }
    U32 fallbacks() const       { return mFallbacks; }
    const U32* failCounts() const { return mFails; }
    void clearCounts();

private:
    void limits(U32 frame, U32& max_verts, U32& max_indices);
    bool allocate(U32 verts, U32 indices, U32 frame, U32& vertex_offset, U32& index_offset);
    void evict(U32 frame, U32 unused_frames);
    bool growBuffer(GLuint& buffer, U32 old_bytes, U32 new_bytes);
    void fail(U32 reason, U32* out);

    std::string mName;
    std::vector<U32> mTypes;
    std::vector<GLuint> mVBs;       // one per type
    U32 mTypeMask = 0;              // the types a buffer must have
    U32 mStride = 0;                // sum of the types' sizes
    std::string mBudgetSetting;
    S32 mDefaultMB = 256;
    U32 mIndexShare = 4;

    GLuint mIB = 0;
    FSGDDRangeAllocator mVerts;     // in vertices
    FSGDDRangeAllocator mIndices;   // in indices
    std::unordered_map<const LLVertexBuffer*, FSGDDArenaEntry> mEntries;
    U32 mFrame = 0;
    U32 mCopies = 0;                // this frame
    U32 mLastSweep = 0;
    U32 mFullFrame = U32_MAX;       // frame an allocation found the arena full
    U32 mEvictFrame = U32_MAX;      // frame an allocation last evicted, at most once per frame
    U32 mLimitsFrame = U32_MAX;     // frame mMaxVerts and mMaxIndices were read from the setting
    U32 mSlot = U32_MAX;            // this arena's slot in LLVertexBuffer's entry cache
    U32 mEpoch = 1;                 // moves on when entries are evicted or released (buffers' cached entries go stale)
    U32 mMaxVerts = 0;
    U32 mMaxIndices = 0;
    U32 mUploads = 0;               // since clearCounts
    U32 mFallbacks = 0;
    U32 mFails[FAIL_COUNT] = {};
};

#endif // FS_GDDARENA_H
