/**
 * @file fsgddarena.cpp
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

#include "llviewerprecompiledheaders.h"

#include "fsgddarena.h"

#include "llgl.h"
#include "llviewercontrol.h"

namespace
{
    constexpr U32 FS_GDD_COPIES_PER_FRAME = 2048;   // vertex buffers copied into an arena per frame, at most
    constexpr U32 FS_GDD_EVICT_AFTER = 600;         // frames an entry may go unused before it is dropped
}

//-----------------------------------------------------------------------------
// FSGDDRangeAllocator

void FSGDDRangeAllocator::reset()
{
    mFree.clear();
    mFreeBySize.clear();
    mCapacity = 0;
    mUsed = 0;
}

void FSGDDRangeAllocator::grow(U32 capacity)
{
    if (capacity > mCapacity)
    {
        insertFree(mCapacity, capacity - mCapacity);
        mCapacity = capacity;
    }
}

// Best fit: the smallest free range that holds the request, lowest offset among equal sizes.
// A first fit scan of mFree cost a few percent of the main thread once the arenas had
// fragmented into thousands of small holes.
bool FSGDDRangeAllocator::allocate(U32 size, U32& offset)
{
    auto fit = mFreeBySize.lower_bound(std::make_pair(size, 0u));
    if (fit == mFreeBySize.end())
    {
        return false;
    }
    offset = fit->second;
    const U32 rest = fit->first - size;
    removeFree(mFree.find(offset));
    if (rest)
    {
        addFree(offset + size, rest);
    }
    mUsed += size;
    return true;
}

void FSGDDRangeAllocator::addFree(U32 offset, U32 size)
{
    mFree.emplace(offset, size);
    mFreeBySize.emplace(size, offset);
}

void FSGDDRangeAllocator::removeFree(std::map<U32, U32>::iterator it)
{
    mFreeBySize.erase(std::make_pair(it->second, it->first));
    mFree.erase(it);
}

void FSGDDRangeAllocator::release(U32 offset, U32 size)
{
    if (size)
    {
        mUsed -= size;
        insertFree(offset, size);
    }
}

void FSGDDRangeAllocator::insertFree(U32 offset, U32 size)
{
    auto next = mFree.lower_bound(offset);
    if (next != mFree.begin())
    {
        auto prev = std::prev(next);
        if (prev->first + prev->second == offset)
        {
            offset = prev->first;
            size += prev->second;
            removeFree(prev);
        }
    }
    if (next != mFree.end() && offset + size == next->first)
    {
        size += next->second;
        removeFree(next);
    }
    addFree(offset, size);
}

//-----------------------------------------------------------------------------
// FSGDDArena

// static
const char* FSGDDArena::failName(U32 reason)
{
    static const char* names[FAIL_COUNT] = { "no buffer", "32 bit indices", "mapped", "missing attributes",
                                             "copy budget", "arena full" };
    return reason < FAIL_COUNT ? names[reason] : "?";
}

FSGDDArena::FSGDDArena(const char* name, const std::vector<U32>& types, const char* budget_setting, S32 default_mb, U32 index_share)
    : mName(name)
    , mTypes(types)
    , mBudgetSetting(budget_setting)
    , mDefaultMB(default_mb)
    , mIndexShare(llmax(index_share, 2u))
{
    mVBs.resize(mTypes.size(), 0);
    for (U32 type : mTypes)
    {
        mTypeMask |= 1u << type;
        mStride += LLVertexBuffer::sTypeSize[type];
    }
}

void FSGDDArena::limits(U32& max_verts, U32& max_indices) const
{
    // only when allocating, so a plain lookup of the setting this arena was given
    S32 mb = mDefaultMB;
    if (LLControlVariable* control = gSavedSettings.getControl(mBudgetSetting).get())
    {
        mb = control->getValue().asInteger();
    }
    const U64 bytes = (U64)llclamp(mb, 16, 2048) << 20;
    const U64 index_bytes = bytes / mIndexShare;
    max_verts = (U32)llmin((U64)U32_MAX / 64, (bytes - index_bytes) / mStride);
    max_indices = (U32)llmin((U64)U32_MAX / 4, index_bytes / sizeof(U16));
}

U32 FSGDDArena::vertexMB() const
{
    return (U32)(((U64)mVerts.used() * mStride) >> 20);
}

U32 FSGDDArena::indexMB() const
{
    return (U32)(((U64)mIndices.used() * sizeof(U16)) >> 20);
}

void FSGDDArena::clearCounts()
{
    mUploads = 0;
    mFallbacks = 0;
    for (U32& count : mFails)
    {
        count = 0;
    }
}

void FSGDDArena::fail(U32 reason, U32* out)
{
    ++mFallbacks;
    ++mFails[reason];
    if (out)
    {
        *out = reason;
    }
}

bool FSGDDArena::growBuffer(GLuint& buffer, U32 old_bytes, U32 new_bytes)
{
    GLuint grown = 0;
    glGenBuffers(1, &grown);
    if (!grown)
    {
        return false;
    }
    glBindBuffer(GL_COPY_WRITE_BUFFER, grown);
    glBufferData(GL_COPY_WRITE_BUFFER, new_bytes, nullptr, GL_STATIC_DRAW);
    if (glGetError() != GL_NO_ERROR)
    {
        glBindBuffer(GL_COPY_WRITE_BUFFER, 0);
        glDeleteBuffers(1, &grown);
        return false;
    }
    if (buffer && old_bytes)
    {   // keep what is in there, the entries' offsets stay valid
        glBindBuffer(GL_COPY_READ_BUFFER, buffer);
        glCopyBufferSubData(GL_COPY_READ_BUFFER, GL_COPY_WRITE_BUFFER, 0, 0, old_bytes);
        glBindBuffer(GL_COPY_READ_BUFFER, 0);
    }
    glBindBuffer(GL_COPY_WRITE_BUFFER, 0);
    if (buffer)
    {
        glDeleteBuffers(1, &buffer);
    }
    buffer = grown;
    return true;
}

void FSGDDArena::evict(U32 frame, U32 unused_frames)
{
    for (auto it = mEntries.begin(); it != mEntries.end(); )
    {
        if (frame - it->second.mLastFrame >= unused_frames)
        {
            mVerts.release(it->second.mVertex, it->second.mNumVerts);
            mIndices.release(it->second.mIndex, it->second.mNumIndices);
            it = mEntries.erase(it);
        }
        else
        {
            ++it;
        }
    }
}

bool FSGDDArena::allocate(U32 verts, U32 indices, U32 frame, U32& vertex_offset, U32& index_offset)
{
    if (mFullFrame == frame)
    {   // full and nothing to drop: don't search again for every buffer this frame
        return false;
    }
    U32 max_verts = 0;
    U32 max_indices = 0;
    limits(max_verts, max_indices);

    for (U32 attempt = 0; attempt < 3; ++attempt)
    {
        if (mVerts.allocate(verts, vertex_offset))
        {
            if (mIndices.allocate(indices, index_offset))
            {
                return true;
            }
            mVerts.release(vertex_offset, verts);
        }

        if (attempt == 0)
        {   // grow, doubling, up to the limit; every attribute buffer by the same number of vertices
            const U32 want_verts = llmin(max_verts, llmax(mVerts.capacity() * 2, llmax(verts * 4, (U32)(1u << 20))));
            const U32 want_indices = llmin(max_indices, llmax(mIndices.capacity() * 2, llmax(indices * 4, (U32)(3u << 20))));
            if (want_verts > mVerts.capacity())
            {
                bool grown = true;
                for (size_t i = 0; i < mTypes.size() && grown; ++i)
                {
                    const U32 size = LLVertexBuffer::sTypeSize[mTypes[i]];
                    grown = growBuffer(mVBs[i], mVerts.capacity() * size, want_verts * size);
                }
                if (grown)
                {
                    mVerts.grow(want_verts);
                }
                // else: the buffers that did grow hold the old range at the old offsets, still consistent
            }
            if (want_indices > mIndices.capacity() &&
                growBuffer(mIB, mIndices.capacity() * (U32)sizeof(U16), want_indices * (U32)sizeof(U16)))
            {
                mIndices.grow(want_indices);
            }
        }
        else if (attempt == 1)
        {   // at the limit: drop what no pass used in this frame or the last one (entries used this frame are
            // still referenced by commands not yet drawn)
            evict(frame, 2);
        }
    }
    mFullFrame = frame;
    return false;
}

void FSGDDArena::beginFrame(U32 frame)
{
    if (frame == mFrame)
    {
        return;
    }
    mFrame = frame;
    mCopies = 0;
    if (frame - mLastSweep >= 120)
    {   // forget the copies of buffers that stopped being drawn (or were freed)
        mLastSweep = frame;
        evict(frame, FS_GDD_EVICT_AFTER);
    }
}

FSGDDArenaEntry* FSGDDArena::peek(const LLVertexBuffer* vb)
{
    auto it = mEntries.find(vb);
    if (it == mEntries.end() || it->second.mSerial != vb->getFSContentSerial() ||
        it->second.mNumVerts != vb->getNumVerts() || it->second.mNumIndices != vb->getNumIndices())
    {
        return nullptr;
    }
    return &it->second;
}

FSGDDArenaEntry* FSGDDArena::find(LLVertexBuffer* vb, U32 frame, U32* reason)
{
    if (!vb || !vb->getFSGLBuffer() || !vb->getFSGLIndices() || !vb->getNumVerts() || !vb->getNumIndices())
    {
        fail(FAIL_NO_BUFFER, reason);
        return nullptr;
    }
    if (vb->getFSIndicesType() != GL_UNSIGNED_SHORT)
    {
        fail(FAIL_INDEX32, reason);
        return nullptr;
    }
    if (vb->isFSMapped())
    {
        fail(FAIL_MAPPED, reason);
        return nullptr;
    }
    if ((vb->getTypeMask() & mTypeMask) != mTypeMask)
    {
        fail(FAIL_ATTRIBUTES, reason);
        return nullptr;
    }

    const U32 serial = vb->getFSContentSerial();
    auto it = mEntries.find(vb);
    if (it != mEntries.end())
    {
        FSGDDArenaEntry& entry = it->second;
        if (entry.mSerial == serial && entry.mNumVerts == vb->getNumVerts() && entry.mNumIndices == vb->getNumIndices())
        {
            entry.mLastFrame = frame;
            return &entry;
        }
        // changed since it was copied (or another buffer at the same address)
        mVerts.release(entry.mVertex, entry.mNumVerts);
        mIndices.release(entry.mIndex, entry.mNumIndices);
        mEntries.erase(it);
    }

    if (mCopies >= FS_GDD_COPIES_PER_FRAME)
    {
        fail(FAIL_COPY_BUDGET, reason);
        return nullptr;
    }

    FSGDDArenaEntry entry;
    entry.mSerial = serial;
    entry.mNumVerts = vb->getNumVerts();
    entry.mNumIndices = vb->getNumIndices();
    entry.mLastFrame = frame;
    if (!allocate(entry.mNumVerts, entry.mNumIndices, frame, entry.mVertex, entry.mIndex))
    {
        LL_WARNS_ONCE("Rendering") << mName << ": the arena is full (" << vertexMB() << " MB vertices, " << indexMB()
                                   << " MB indices, limit " << mBudgetSetting << "), what doesn't fit is drawn the old way"
                                   << LL_ENDL;
        fail(FAIL_FULL, reason);
        return nullptr;
    }

    {
        LL_PROFILE_ZONE_NAMED_CATEGORY_PIPELINE("gdd arena copy");
        glBindBuffer(GL_COPY_READ_BUFFER, vb->getFSGLBuffer());
        for (size_t i = 0; i < mTypes.size(); ++i)
        {
            const LLVertexBuffer::AttributeType type = (LLVertexBuffer::AttributeType)mTypes[i];
            const U32 size = LLVertexBuffer::sTypeSize[type];
            glBindBuffer(GL_COPY_WRITE_BUFFER, mVBs[i]);
            glCopyBufferSubData(GL_COPY_READ_BUFFER, GL_COPY_WRITE_BUFFER, vb->getOffset(type),
                                (GLintptr)entry.mVertex * size, (GLsizeiptr)entry.mNumVerts * size);
        }
        glBindBuffer(GL_COPY_READ_BUFFER, vb->getFSGLIndices());
        glBindBuffer(GL_COPY_WRITE_BUFFER, mIB);
        glCopyBufferSubData(GL_COPY_READ_BUFFER, GL_COPY_WRITE_BUFFER, 0,
                            (GLintptr)entry.mIndex * sizeof(U16), (GLsizeiptr)entry.mNumIndices * sizeof(U16));
        glBindBuffer(GL_COPY_READ_BUFFER, 0);
        glBindBuffer(GL_COPY_WRITE_BUFFER, 0);
    }
    ++mCopies;
    ++mUploads;
    return &(mEntries[vb] = entry);
}

void FSGDDArena::bindForDraw(U32 attrib_mask)
{
    for (size_t i = 0; i < mTypes.size(); ++i)
    {
        const U32 type = mTypes[i];
        if (!(attrib_mask & (1u << type)))
        {
            continue;
        }
        const GLsizei stride = LLVertexBuffer::sTypeSize[type];
        glBindBuffer(GL_ARRAY_BUFFER, mVBs[i]);
        switch (type)
        {   // as LLVertexBuffer::setupVertexBuffer
        case LLVertexBuffer::TYPE_VERTEX:
        case LLVertexBuffer::TYPE_NORMAL:
            glVertexAttribPointer(type, 3, GL_FLOAT, GL_FALSE, stride, nullptr);
            break;
        case LLVertexBuffer::TYPE_TANGENT:
            glVertexAttribPointer(type, 4, GL_FLOAT, GL_FALSE, stride, nullptr);
            break;
        case LLVertexBuffer::TYPE_TEXCOORD0:
        case LLVertexBuffer::TYPE_TEXCOORD1:
        case LLVertexBuffer::TYPE_TEXCOORD2:
        case LLVertexBuffer::TYPE_TEXCOORD3:
            glVertexAttribPointer(type, 2, GL_FLOAT, GL_FALSE, stride, nullptr);
            break;
        case LLVertexBuffer::TYPE_COLOR:
        case LLVertexBuffer::TYPE_EMISSIVE:
            glVertexAttribPointer(type, 4, GL_UNSIGNED_BYTE, GL_TRUE, stride, nullptr);
            break;
        default:
            llassert(false);
            break;
        }
    }
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, mIB);
}

bool FSGDDArena::verifyEntry(const LLVertexBuffer* vb, FSGDDArenaEntry& entry)
{
    entry.mVerified = true;
    bool ok = true;
    for (size_t i = 0; i < mTypes.size() && ok; ++i)
    {
        const LLVertexBuffer::AttributeType type = (LLVertexBuffer::AttributeType)mTypes[i];
        const U32 size = LLVertexBuffer::sTypeSize[type];
        std::vector<U8> src((size_t)entry.mNumVerts * size);
        std::vector<U8> dst(src.size());
        glBindBuffer(GL_COPY_READ_BUFFER, vb->getFSGLBuffer());
        glGetBufferSubData(GL_COPY_READ_BUFFER, vb->getOffset(type), src.size(), src.data());
        glBindBuffer(GL_COPY_READ_BUFFER, mVBs[i]);
        glGetBufferSubData(GL_COPY_READ_BUFFER, (GLintptr)entry.mVertex * size, dst.size(), dst.data());
        ok = src == dst;
    }

    std::vector<U16> src_idx(entry.mNumIndices);
    std::vector<U16> dst_idx(entry.mNumIndices);
    glBindBuffer(GL_COPY_READ_BUFFER, vb->getFSGLIndices());
    glGetBufferSubData(GL_COPY_READ_BUFFER, 0, src_idx.size() * sizeof(U16), src_idx.data());
    glBindBuffer(GL_COPY_READ_BUFFER, mIB);
    glGetBufferSubData(GL_COPY_READ_BUFFER, (GLintptr)entry.mIndex * sizeof(U16), dst_idx.size() * sizeof(U16), dst_idx.data());
    glBindBuffer(GL_COPY_READ_BUFFER, 0);
    ok = ok && src_idx == dst_idx;
    U16 max_index = 0;
    for (U16 i : dst_idx)
    {
        max_index = llmax(max_index, i);
    }
    ok = ok && max_index < entry.mNumVerts;
    return ok;
}

void FSGDDArena::releaseGL()
{
    for (GLuint& buffer : mVBs)
    {
        if (buffer)
        {
            glDeleteBuffers(1, &buffer);
            buffer = 0;
        }
    }
    if (mIB)
    {
        glDeleteBuffers(1, &mIB);
        mIB = 0;
    }
    mVerts.reset();
    mIndices.reset();
    mEntries.clear();
    mCopies = 0;
    mFullFrame = U32_MAX;
}
