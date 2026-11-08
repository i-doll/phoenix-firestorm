/**
 * @file fsgpudrivendraws.cpp
 * @brief GPU-driven draws, first slice: opaque shadow casters as one multi-draw (FSPerfGPUDrivenDraws)
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

// genDrawInfo gives every batch (a run of faces with one texture or one texture set) a vertex buffer of
// its own, so on the stock path every opaque shadow caster costs a buffer bind, a vertex attribute setup,
// a model matrix upload and a glDrawRangeElements, once per shadow map it reaches. Merging draws that share
// a buffer (FSPerfShadowMergedBatches) can't get below one call per buffer.
//
// With FSPerfGPUDrivenDraws on, the depth only opaque casters (the pass types renderShadow draws with
// gDeferredShadowProgram, not rigged) come out of a position arena instead: one GL buffer holding a copy of
// the positions of every caster's vertex buffer, and one holding a copy of its 16 bit indices. A buffer is
// copied in on the GPU (glCopyBufferSubData, nothing goes through the CPU) the first time it casts, and again
// whenever its content serial (LLVertexBuffer::getFSContentSerial, bumped on every upload) changes. Entries
// that no shadow pass used for a while are dropped. Each caster then is one 20 byte indirect command
// (index range offset into the arena, base vertex of its copy, and as base instance its own index) plus a
// record naming its matrix, the model view projection the stock path would have computed for it. The vertex
// shader reads the record as an instanced attribute (divisor 4, so every instance of a command reads the
// command's record), which is what gl_DrawIDARB would give without needing GL_ARB_shader_draw_parameters.
// A whole shadow pass is one glMultiDrawElementsIndirect; with FSPerfShadowSinglePass, one per set of
// cascades, each command instanced once per cascade like the stock single pass draws.
//
// Anything that can't come from the arena (32 bit indices, a buffer still mapped, the per frame copy budget
// or the arena size limit reached) stays in the list and is drawn by the stock loop. Rigged, alpha masked,
// GLTF, tree, grass, terrain and avatar casters are untouched.
//
// FSPerfGPUDrivenDrawsVerify: after each multi-draw the same batches are drawn the stock way with depth
// writes off, a small polygon offset and a GL_SAMPLES_PASSED query; any sample that passes GL_LESS is a
// spot where the stock path would have put a nearer caster than the multi-draw did, which is logged. It
// also reads back a couple of arena entries per pass and compares them with their source buffers.

#include "llviewerprecompiledheaders.h"

#include "fsgpudrivendraws.h"

#include "fsshadowsinglepass.h"
#include "lldrawpool.h"
#include "llframetimer.h"
#include "llgl.h"
#include "llglslshader.h"
#include "llrender.h"
#include "llspatialpartition.h"
#include "llvertexbuffer.h"
#include "llviewercontrol.h"

#include <algorithm>
#include <map>
#include <unordered_map>

LLGLSLShader gFSGDDShadowProgram;
LLGLSLShader gFSGDDShadowSPProgram;

namespace
{
    constexpr GLuint FS_GDD_MATRIX_BINDING = 7;     // shader storage binding of FSGDDMatrices
    constexpr GLuint FS_GDD_RECORD_DIVISOR = 4;     // instances per record step: at least the most cascades a command has
    constexpr U32 FS_GDD_VERTEX_STRIDE = 16;        // LLVertexBuffer::sTypeSize[TYPE_VERTEX]
    constexpr U32 FS_GDD_COPIES_PER_FRAME = 2048;   // vertex buffers copied into the arena per frame, at most
    constexpr U32 FS_GDD_EVICT_AFTER = 600;         // frames an entry may go unused before it is dropped
    constexpr U32 FS_GDD_VERIFY_SAMPLES = 64;       // samples a verify pass may let through before it warns

    const LLStaticHashedString sClip("fs_shadow_clip");
    const LLStaticHashedString sCascade("fs_shadow_cascade");

    struct DrawCommand  // DrawElementsIndirectCommand
    {
        GLuint  mCount;
        GLuint  mInstanceCount;
        GLuint  mFirstIndex;
        GLint   mBaseVertex;
        GLuint  mBaseInstance;
    };

    // First fit over [0, capacity) with neighbouring free ranges joined.
    class RangeAllocator
    {
    public:
        void reset()
        {
            mFree.clear();
            mCapacity = 0;
            mUsed = 0;
        }

        void grow(U32 capacity)
        {
            if (capacity > mCapacity)
            {
                insertFree(mCapacity, capacity - mCapacity);
                mCapacity = capacity;
            }
        }

        bool allocate(U32 size, U32& offset)
        {
            for (auto it = mFree.begin(); it != mFree.end(); ++it)
            {
                if (it->second >= size)
                {
                    offset = it->first;
                    const U32 rest = it->second - size;
                    mFree.erase(it);
                    if (rest)
                    {
                        mFree.emplace(offset + size, rest);
                    }
                    mUsed += size;
                    return true;
                }
            }
            return false;
        }

        void release(U32 offset, U32 size)
        {
            if (size)
            {
                mUsed -= size;
                insertFree(offset, size);
            }
        }

        U32 capacity() const    { return mCapacity; }
        U32 used() const        { return mUsed; }

    private:
        void insertFree(U32 offset, U32 size)
        {
            auto next = mFree.lower_bound(offset);
            if (next != mFree.begin())
            {
                auto prev = std::prev(next);
                if (prev->first + prev->second == offset)
                {
                    offset = prev->first;
                    size += prev->second;
                    mFree.erase(prev);
                }
            }
            if (next != mFree.end() && offset + size == next->first)
            {
                size += next->second;
                mFree.erase(next);
            }
            mFree.emplace(offset, size);
        }

        std::map<U32, U32> mFree;   // offset, size
        U32 mCapacity = 0;
        U32 mUsed = 0;
    };

    struct ArenaEntry
    {
        U32 mSerial = 0;        // LLVertexBuffer::getFSContentSerial() of the copy
        U32 mVertex = 0;        // first vertex of the copy in the vertex arena
        U32 mNumVerts = 0;
        U32 mIndex = 0;         // first index of the copy in the index arena
        U32 mNumIndices = 0;
        U32 mLastFrame = 0;
        bool mVerified = false; // FSPerfGPUDrivenDrawsVerify read it back already
    };

    // The positions and indices of the vertex buffers that cast opaque shadows. Keyed by buffer address, but
    // only ever compared, never dereferenced: a buffer freed and another allocated at its address has a new
    // content serial, so its entry is copied again.
    class Arena
    {
    public:
        ArenaEntry* find(LLVertexBuffer* vb, U32 frame);
        void releaseGL();
        void beginFrame(U32 frame);
        void bindForDraw();
        bool verifyEntry(const LLVertexBuffer* vb, ArenaEntry& entry);
        U32 vertexMB() const        { return (U32)(((U64)mVerts.used() * FS_GDD_VERTEX_STRIDE) >> 20); }
        U32 indexMB() const         { return (U32)(((U64)mIndices.used() * sizeof(U16)) >> 20); }
        U32 uploads() const         { return mUploads; }
        U32 fallbacks() const       { return mFallbacks; }
        void clearCounts()          { mUploads = 0; mFallbacks = 0; }

    private:
        bool allocate(U32 verts, U32 indices, U32 frame, U32& vertex_offset, U32& index_offset);
        void evict(U32 frame, U32 unused_frames);
        bool growBuffer(GLuint& buffer, U32 old_bytes, U32 new_bytes);

        GLuint mVB = 0;
        GLuint mIB = 0;
        RangeAllocator mVerts;      // in vertices
        RangeAllocator mIndices;    // in indices
        std::unordered_map<const LLVertexBuffer*, ArenaEntry> mEntries;
        U32 mFrame = 0;
        U32 mCopies = 0;            // this frame
        U32 mLastSweep = 0;
        U32 mFullFrame = U32_MAX;   // frame an allocation found the arena full
        U32 mUploads = 0;           // since clearCounts
        U32 mFallbacks = 0;
    };

    Arena sArena;

    // per multi-draw data, rebuilt for each call
    GLuint sIndirectBuffer = 0;
    GLuint sDrawBuffer = 0;
    GLuint sMatrixBuffer = 0;
    GLuint sQuery = 0;
    GLint sRecordAttrib[2] = { -1, -1 };   // location of fs_gdd_draw: plain, single pass
    S32 sInputsReady[2] = { 0, 0 };         // bind_inputs done: 0 not yet, 1 yes, -1 failed
    std::vector<DrawCommand> sCommands;
    std::vector<U32> sDraws;                // per command: matrix index
    std::vector<glm::mat4> sMatrices;
    std::unordered_map<const LLMatrix4*, U32> sMatrixIndex;

    // stats, logged every 10 seconds with FSPerfShadowBatchStats or FSPerfGPUDrivenDrawsVerify
    U64 sStatBatches = 0;
    U64 sStatCommands = 0;
    U64 sStatCalls = 0;
    U64 sStatLeft = 0;

    void arena_limits(U32& max_verts, U32& max_indices)
    {
        static LLCachedControl<S32> arena_mb(gSavedSettings, "FSPerfGPUDrivenDrawsArenaMB", 256);
        const U64 bytes = (U64)llclamp((S32)arena_mb, 16, 2048) << 20;
        // three quarters positions (16 bytes a vertex), a quarter indices (2 bytes each)
        max_verts = (U32)((bytes * 3 / 4) / FS_GDD_VERTEX_STRIDE);
        max_indices = (U32)((bytes / 4) / sizeof(U16));
    }
}

bool Arena::growBuffer(GLuint& buffer, U32 old_bytes, U32 new_bytes)
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

void Arena::evict(U32 frame, U32 unused_frames)
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

bool Arena::allocate(U32 verts, U32 indices, U32 frame, U32& vertex_offset, U32& index_offset)
{
    if (mFullFrame == frame)
    {   // full and nothing to drop: don't search again for every buffer this frame
        return false;
    }
    U32 max_verts = 0;
    U32 max_indices = 0;
    arena_limits(max_verts, max_indices);

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
        {   // grow, doubling, up to the limit
            const U32 want_verts = llmin(max_verts, llmax(mVerts.capacity() * 2, llmax(verts * 4, (U32)(1u << 20))));
            const U32 want_indices = llmin(max_indices, llmax(mIndices.capacity() * 2, llmax(indices * 4, (U32)(3u << 20))));
            if (want_verts > mVerts.capacity() &&
                growBuffer(mVB, mVerts.capacity() * FS_GDD_VERTEX_STRIDE, want_verts * FS_GDD_VERTEX_STRIDE))
            {
                mVerts.grow(want_verts);
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

void Arena::beginFrame(U32 frame)
{
    if (frame == mFrame)
    {
        return;
    }
    mFrame = frame;
    mCopies = 0;
    if (frame - mLastSweep >= 120)
    {   // forget the copies of buffers that stopped casting (or were freed)
        mLastSweep = frame;
        evict(frame, FS_GDD_EVICT_AFTER);
    }
}

ArenaEntry* Arena::find(LLVertexBuffer* vb, U32 frame)
{
    if (!vb || !vb->getFSGLBuffer() || !vb->getFSGLIndices() || vb->getFSIndicesType() != GL_UNSIGNED_SHORT ||
        vb->isFSMapped() || !vb->hasDataType(LLVertexBuffer::TYPE_VERTEX) || !vb->getNumVerts() || !vb->getNumIndices())
    {
        ++mFallbacks;
        return nullptr;
    }

    const U32 serial = vb->getFSContentSerial();
    auto it = mEntries.find(vb);
    if (it != mEntries.end())
    {
        ArenaEntry& entry = it->second;
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
        ++mFallbacks;
        return nullptr;
    }

    ArenaEntry entry;
    entry.mSerial = serial;
    entry.mNumVerts = vb->getNumVerts();
    entry.mNumIndices = vb->getNumIndices();
    entry.mLastFrame = frame;
    if (!allocate(entry.mNumVerts, entry.mNumIndices, frame, entry.mVertex, entry.mIndex))
    {
        LL_WARNS_ONCE("Rendering") << "FSPerfGPUDrivenDraws: the shadow position arena is full ("
                                   << vertexMB() << " MB positions, " << indexMB() << " MB indices), "
                                   << "the casters that don't fit are drawn one by one" << LL_ENDL;
        ++mFallbacks;
        return nullptr;
    }

    {
        LL_PROFILE_ZONE_NAMED_CATEGORY_PIPELINE("gdd arena copy");
        glBindBuffer(GL_COPY_READ_BUFFER, vb->getFSGLBuffer());
        glBindBuffer(GL_COPY_WRITE_BUFFER, mVB);
        glCopyBufferSubData(GL_COPY_READ_BUFFER, GL_COPY_WRITE_BUFFER, vb->getOffset(LLVertexBuffer::TYPE_VERTEX),
                            (GLintptr)entry.mVertex * FS_GDD_VERTEX_STRIDE, (GLsizeiptr)entry.mNumVerts * FS_GDD_VERTEX_STRIDE);
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

void Arena::bindForDraw()
{
    // the one vertex attribute the program has: position, location TYPE_VERTEX
    glBindBuffer(GL_ARRAY_BUFFER, mVB);
    glVertexAttribPointer(LLVertexBuffer::TYPE_VERTEX, 3, GL_FLOAT, GL_FALSE, FS_GDD_VERTEX_STRIDE, nullptr);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, mIB);
}

// FSPerfGPUDrivenDrawsVerify: read back an entry and its source buffer and compare. Stalls; debug only.
bool Arena::verifyEntry(const LLVertexBuffer* vb, ArenaEntry& entry)
{
    entry.mVerified = true;
    std::vector<U8> src(entry.mNumVerts * FS_GDD_VERTEX_STRIDE);
    std::vector<U8> dst(src.size());
    glBindBuffer(GL_COPY_READ_BUFFER, vb->getFSGLBuffer());
    glGetBufferSubData(GL_COPY_READ_BUFFER, vb->getOffset(LLVertexBuffer::TYPE_VERTEX), src.size(), src.data());
    glBindBuffer(GL_COPY_READ_BUFFER, mVB);
    glGetBufferSubData(GL_COPY_READ_BUFFER, (GLintptr)entry.mVertex * FS_GDD_VERTEX_STRIDE, dst.size(), dst.data());
    bool ok = src == dst;

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

void Arena::releaseGL()
{
    if (mVB)
    {
        glDeleteBuffers(1, &mVB);
        mVB = 0;
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

void fsGDDReleaseGL()
{
    sArena.releaseGL();
    GLuint buffers[3] = { sIndirectBuffer, sDrawBuffer, sMatrixBuffer };
    for (GLuint buffer : buffers)
    {
        if (buffer)
        {
            glDeleteBuffers(1, &buffer);
        }
    }
    sIndirectBuffer = 0;
    sDrawBuffer = 0;
    sMatrixBuffer = 0;
    if (sQuery)
    {
        glDeleteQueries(1, &sQuery);
        sQuery = 0;
    }
}

void fsGDDUnloadShaders()
{
    gFSGDDShadowProgram.unload();
    gFSGDDShadowSPProgram.unload();
    sInputsReady[0] = 0;
    sInputsReady[1] = 0;
    fsGDDReleaseGL();
}

namespace
{
    // the matrix block's binding, and where the driver put the per command record
    bool bind_inputs(LLGLSLShader& shader, GLint& record_attrib)
    {
        const GLuint matrices = glGetProgramResourceIndex(shader.mProgramObject, GL_SHADER_STORAGE_BLOCK, "FSGDDMatrices");
        record_attrib = glGetAttribLocation(shader.mProgramObject, "fs_gdd_draw");
        if (matrices == GL_INVALID_INDEX || record_attrib <= 0)
        {
            return false;
        }
        glShaderStorageBlockBinding(shader.mProgramObject, matrices, FS_GDD_MATRIX_BINDING);
        return true;
    }

    // createShader() may only submit the program (parallel shader compile); bind_inputs waits for its first use
    bool create_program(LLGLSLShader& shader, const char* name, S32 shader_level, S32 single_pass_ext)
    {
        shader.mName = name;
        shader.mShaderFiles.clear();
        shader.clearPermutations();
        shader.mShaderLevel = shader_level;
        shader.mShaderFiles.push_back(std::make_pair("deferred/fsGPUDrivenShadowV.glsl", GL_VERTEX_SHADER));
        shader.mShaderFiles.push_back(std::make_pair("deferred/shadowF.glsl", GL_FRAGMENT_SHADER));
        if (single_pass_ext > 0)
        {
            shader.addPermutation("FS_SHADOW_SINGLE_PASS", llformat("%d", single_pass_ext));
        }
        return shader.createShader();
    }
}

bool fsGDDLoadShaders(S32 shader_level, bool use_shadow)
{
    fsGDDUnloadShaders();

    static LLCachedControl<bool> enabled(gSavedSettings, "FSPerfGPUDrivenDraws", false);
    if (!enabled || !use_shadow)
    {
        return false;
    }
    if (!gGLManager.mFSHasGPUDrivenDraws)
    {
        LL_INFOS_ONCE("ShaderLoading") << "FSPerfGPUDrivenDraws: needs OpenGL 4.3 with GL_ARB_multi_draw_indirect and "
                                       << "GL_ARB_shader_storage_buffer_object (in the vertex shader); shadow casters "
                                       << "are drawn as before" << LL_ENDL;
        return false;
    }

    if (!create_program(gFSGDDShadowProgram, "FS GPU-Driven Shadow Shader", shader_level, 0))
    {
        LL_WARNS("ShaderLoading") << "FSPerfGPUDrivenDraws: the multi-draw shadow program failed to build, "
                                  << "shadow casters are drawn as before" << LL_ENDL;
        fsGDDUnloadShaders();
        return false;
    }

    // the single pass variant only when FSPerfShadowSinglePass has its own programs
    if (gFSShadowSinglePassShaders && gGLManager.mFSVertexViewportIndex > 0 &&
        !create_program(gFSGDDShadowSPProgram, "FS GPU-Driven Single Pass Shadow Shader", shader_level,
                        gGLManager.mFSVertexViewportIndex))
    {
        LL_WARNS("ShaderLoading") << "FSPerfGPUDrivenDraws: the single pass multi-draw shadow program failed to build, "
                                  << "single pass shadow casters are drawn as before" << LL_ENDL;
        gFSGDDShadowSPProgram.unload();
    }
    return true;
}

bool fsGDDShadowEnabled(bool single_pass)
{
    static LLCachedControl<bool> enabled(gSavedSettings, "FSPerfGPUDrivenDraws", false);
    if (!enabled || !gGLManager.mFSHasGPUDrivenDraws)
    {
        return false;
    }
    if (sInputsReady[single_pass ? 1 : 0] < 0)
    {
        return false;
    }
    return single_pass ? gFSGDDShadowSPProgram.isComplete() : gFSGDDShadowProgram.isComplete();
}

namespace
{
    struct Item
    {
        LLDrawInfo* mParams;
        U32         mMask;      // single pass cascade set, 1 otherwise
    };

    struct Segment
    {
        U32 mMask;
        U32 mFirst;             // first command
        U32 mCount;
    };

    std::vector<Item> sItems;

    // Binds the program; the first time, also finds its inputs. False (for good) when they aren't there.
    bool bind_program(U32 which)
    {
        LLGLSLShader& program = which ? gFSGDDShadowSPProgram : gFSGDDShadowProgram;
        program.bind();
        if (LLGLSLShader::sCurBoundShaderPtr != &program)
        {
            return false;
        }
        if (!sInputsReady[which])
        {
            sInputsReady[which] = bind_inputs(program, sRecordAttrib[which]) ? 1 : -1;
            if (sInputsReady[which] < 0)
            {
                LL_WARNS("Rendering") << "FSPerfGPUDrivenDraws: " << program.mName << " lacks its matrix block or draw "
                                      << "record attribute, shadow casters are drawn as before" << LL_ENDL;
            }
        }
        return sInputsReady[which] > 0;
    }
    std::vector<Item> sHandled;     // FSPerfGPUDrivenDrawsVerify
    std::vector<Segment> sSegments;

    U32 matrix_index(const LLMatrix4* model, const glm::mat4& proj, const glm::mat4& view)
    {
        auto it = sMatrixIndex.find(model);
        if (it != sMatrixIndex.end())
        {
            return it->second;
        }
        // as LLRenderPass::applyModelMatrix and LLRender::syncMatrices compose it
        glm::mat4 modelview = view;
        if (model)
        {
            modelview *= glm::make_mat4((const F32*)model->mMatrix);
        }
        const U32 index = (U32)sMatrices.size();
        sMatrices.push_back(proj * modelview);
        sMatrixIndex.emplace(model, index);
        return index;
    }

    U32 popcount4(U32 mask)
    {
        return (mask & 1) + ((mask >> 1) & 1) + ((mask >> 2) & 1) + ((mask >> 3) & 1);
    }

    // Turns the items (sorted by mask, model matrix, buffer, index offset, longer range first) into commands
    // and segments. Items that can't be drawn from the arena go to left, the rest (verify only) to sHandled.
    void build_commands(const std::vector<Item>& items, std::vector<Item>& left, bool single_pass, bool verify)
    {
        sCommands.clear();
        sDraws.clear();
        sMatrices.clear();
        sMatrixIndex.clear();
        sSegments.clear();
        sHandled.clear();

        const U32 frame = LLFrameTimer::getFrameCount();
        sArena.beginFrame(frame);
        const glm::mat4 proj = gGL.getProjectionMatrix();
        const glm::mat4 view = get_current_modelview();

        const size_t count = items.size();
        size_t i = 0;
        while (i < count)
        {
            const Item& first = items[i];
            LLVertexBuffer* vb = first.mParams->mVertexBuffer.get();
            const LLMatrix4* model = first.mParams->mModelMatrix;
            size_t group_end = i + 1;
            while (group_end < count && items[group_end].mMask == first.mMask &&
                   items[group_end].mParams->mModelMatrix == model && items[group_end].mParams->mVertexBuffer.get() == vb)
            {
                ++group_end;
            }

            const ArenaEntry* entry = sArena.find(vb, frame);
            bool in_range = entry != nullptr;
            for (size_t j = i; in_range && j < group_end; ++j)
            {   // always true for a valid batch; anything else is left to the stock path rather than read past the copy
                const LLDrawInfo* params = items[j].mParams;
                in_range = params->mOffset + params->mCount <= entry->mNumIndices && params->mEnd < entry->mNumVerts;
            }
            if (!in_range)
            {
                left.insert(left.end(), items.begin() + i, items.begin() + group_end);
                i = group_end;
                continue;
            }

            if (sSegments.empty() || sSegments.back().mMask != first.mMask)
            {
                sSegments.push_back({ first.mMask, (U32)sCommands.size(), 0 });
            }
            const U32 matrix = matrix_index(model, proj, view);
            const U32 instances = single_pass ? popcount4(first.mMask) : 1;

            // a run of contiguous index ranges is one command; a range inside the one just added is dropped
            U32 offset = first.mParams->mOffset;
            U32 index_end = offset + first.mParams->mCount;
            for (size_t j = i + 1; j <= group_end; ++j)
            {
                const LLDrawInfo* params = j < group_end ? items[j].mParams : nullptr;
                if (params && params->mOffset + params->mCount <= index_end)
                {
                    continue;
                }
                if (params && params->mOffset == index_end)
                {
                    index_end += params->mCount;
                    continue;
                }
                // base instance: this command's record
                sCommands.push_back({ index_end - offset, instances, entry->mIndex + offset, (GLint)entry->mVertex, (GLuint)sCommands.size() });
                sDraws.push_back(matrix);
                ++sSegments.back().mCount;
                if (params)
                {
                    offset = params->mOffset;
                    index_end = offset + params->mCount;
                }
            }
            if (verify)
            {
                sHandled.insert(sHandled.end(), items.begin() + i, items.begin() + group_end);
            }
            i = group_end;
        }
    }

    void upload(GLuint& buffer, GLenum target, const void* data, size_t bytes)
    {
        if (!buffer)
        {
            glGenBuffers(1, &buffer);
        }
        glBindBuffer(target, buffer);
        glBufferData(target, bytes, data, GL_STREAM_DRAW);  // new storage: the previous pass's draws may still read the old
    }

    // Issues the built commands, one multi-draw per segment. The program is bound by the caller.
    void draw_commands(bool single_pass)
    {
        LL_PROFILE_ZONE_NAMED_CATEGORY_PIPELINE("gdd multi-draw");
        LL_PROFILE_GPU_ZONE("gdd multi-draw");

        upload(sIndirectBuffer, GL_DRAW_INDIRECT_BUFFER, sCommands.data(), sCommands.size() * sizeof(DrawCommand));
        upload(sMatrixBuffer, GL_SHADER_STORAGE_BUFFER, sMatrices.data(), sMatrices.size() * sizeof(glm::mat4));
        glBindBufferBase(GL_SHADER_STORAGE_BUFFER, FS_GDD_MATRIX_BINDING, sMatrixBuffer);

        // the per command records, an instanced attribute stepped by the commands' base instances
        const GLuint record = (GLuint)sRecordAttrib[single_pass ? 1 : 0];
        upload(sDrawBuffer, GL_ARRAY_BUFFER, sDraws.data(), sDraws.size() * sizeof(U32));
        glEnableVertexAttribArray(record);
        glVertexAttribIPointer(record, 1, GL_UNSIGNED_INT, sizeof(U32), nullptr);
        glVertexAttribDivisor(record, FS_GDD_RECORD_DIVISOR);
        sArena.bindForDraw();

        LLGLSLShader* shader = LLGLSLShader::sCurBoundShaderPtr;
        for (const Segment& segment : sSegments)
        {
            if (single_pass)
            {
                F32 cascades[4] = { 0.f, 0.f, 0.f, 0.f };
                U32 n = 0;
                for (U32 c = 0; c < 4; ++c)
                {
                    if (segment.mMask & (1u << c))
                    {
                        cascades[n++] = (F32)c;
                    }
                }
                shader->uniform4f(sCascade, cascades[0], cascades[1], cascades[2], cascades[3]);
            }
            glMultiDrawElementsIndirect(GL_TRIANGLES, GL_UNSIGNED_SHORT,
                                        (const void*)((size_t)segment.mFirst * sizeof(DrawCommand)), (GLsizei)segment.mCount, 0);
        }

        // the program's attribute mask doesn't have this location, so LLVertexBuffer expects it off and undivided
        glVertexAttribDivisor(record, 0);
        glDisableVertexAttribArray(record);
        glBindBufferBase(GL_SHADER_STORAGE_BUFFER, FS_GDD_MATRIX_BINDING, 0);
        glBindBuffer(GL_SHADER_STORAGE_BUFFER, 0);
        glBindBuffer(GL_DRAW_INDIRECT_BUFFER, 0);
        LLVertexBuffer::unbind();   // the next setBuffer() binds and sets up its own attributes again
        STOP_GLERROR;
    }

    // FSPerfGPUDrivenDrawsVerify: the handled batches again, the stock way, with depth writes off and a GL_LESS
    // test against what the multi-draw left. The stock program for this pass must be bound.
    void verify_against_stock(bool single_pass)
    {
        if (sHandled.empty())
        {
            return;
        }
        LL_PROFILE_ZONE_NAMED_CATEGORY_PIPELINE("gdd verify");

        // arena copies against their sources, a couple per pass
        U32 checked = 0;
        for (const Item& item : sHandled)
        {
            LLVertexBuffer* vb = item.mParams->mVertexBuffer.get();
            ArenaEntry* found = sArena.find(vb, LLFrameTimer::getFrameCount());
            if (found && !found->mVerified)
            {
                if (!sArena.verifyEntry(vb, *found))
                {
                    static LLFrameTimer warn_timer;
                    if (warn_timer.getElapsedTimeF32() > 5.f)
                    {
                        warn_timer.reset();
                        LL_WARNS("Rendering") << "FSPerfGPUDrivenDrawsVerify: the arena copy of a vertex buffer ("
                                              << found->mNumVerts << " vertices, " << found->mNumIndices
                                              << " indices) differs from the buffer" << LL_ENDL;
                    }
                }
                if (++checked >= 2)
                {
                    break;
                }
            }
        }

        if (!sQuery)
        {
            glGenQueries(1, &sQuery);
        }
        GLfloat factor = 0.f;
        GLfloat units = 0.f;
        glGetFloatv(GL_POLYGON_OFFSET_FACTOR, &factor);
        glGetFloatv(GL_POLYGON_OFFSET_UNITS, &units);
        {
            LLGLDepthTest depth(GL_TRUE, GL_FALSE, GL_LESS);
            LLGLEnable offset(GL_POLYGON_OFFSET_FILL);
            glPolygonOffset(1.f, 4.f);  // only casters clearly nearer than what the multi-draw wrote count
            glBeginQuery(GL_SAMPLES_PASSED, sQuery);
            U32 mask = 0;
            for (const Item& item : sHandled)
            {
                if (single_pass && item.mMask != mask)
                {
                    mask = item.mMask;
                    F32 cascades[4] = { 0.f, 0.f, 0.f, 0.f };
                    U32 n = 0;
                    for (U32 c = 0; c < 4; ++c)
                    {
                        if (mask & (1u << c))
                        {
                            cascades[n++] = (F32)c;
                        }
                    }
                    LLGLSLShader::sCurBoundShaderPtr->uniform4f(sCascade, cascades[0], cascades[1], cascades[2], cascades[3]);
                    LLVertexBuffer::sFSInstanceCount = n;
                }
                LLDrawInfo& params = *item.mParams;
                LLRenderPass::applyModelMatrix(params.mModelMatrix);
                params.mVertexBuffer->setBuffer();
                params.mVertexBuffer->drawRange(LLRender::TRIANGLES, params.mStart, params.mEnd, params.mCount, params.mOffset);
            }
            LLVertexBuffer::sFSInstanceCount = 0;
            glEndQuery(GL_SAMPLES_PASSED);
        }
        glPolygonOffset(factor, units);
        gGL.loadMatrix(gGLModelView);
        gGLLastMatrix = NULL;

        GLuint samples = 0;
        glGetQueryObjectuiv(sQuery, GL_QUERY_RESULT, &samples);
        if (samples > FS_GDD_VERIFY_SAMPLES)
        {
            static LLFrameTimer warn_timer;
            static U32 mismatches = 0;
            ++mismatches;
            if (warn_timer.getElapsedTimeF32() > 5.f)
            {
                warn_timer.reset();
                LL_WARNS("Rendering") << "FSPerfGPUDrivenDrawsVerify: " << samples << " samples of " << sHandled.size()
                                      << " stock shadow casters are nearer than the multi-draw depth"
                                      << (single_pass ? " (single pass)" : "") << "; " << mismatches
                                      << " mismatching passes so far" << LL_ENDL;
            }
        }
    }

    void count_stats(U32 batches, U32 calls, U32 left)
    {
        static LLCachedControl<bool> batch_stats(gSavedSettings, "FSPerfShadowBatchStats", false);
        static LLCachedControl<bool> verify(gSavedSettings, "FSPerfGPUDrivenDrawsVerify", false);
        if (!batch_stats && !verify)
        {
            return;
        }
        sStatBatches += batches;
        sStatCommands += sCommands.size();
        sStatCalls += calls;
        sStatLeft += left;
        static LLFrameTimer timer;
        if (timer.getElapsedTimeF32() >= 10.f)
        {
            LL_INFOS("FSShadowBatches") << "GPU-driven shadow casters: " << sStatBatches << " batches, "
                                        << sStatCommands << " draw commands in " << sStatCalls << " multi-draw calls, "
                                        << sStatLeft << " batches left to the stock path; arena " << sArena.vertexMB()
                                        << " MB positions, " << sArena.indexMB() << " MB indices, "
                                        << sArena.uploads() << " buffer copies, " << sArena.fallbacks()
                                        << " buffers not in the arena" << LL_ENDL;
            sStatBatches = 0;
            sStatCommands = 0;
            sStatCalls = 0;
            sStatLeft = 0;
            sArena.clearCounts();
            timer.reset();
        }
    }
}

U32 fsGDDDrawShadowOpaque(std::vector<LLDrawInfo*>& batches)
{
    if (batches.empty() || !fsGDDShadowEnabled(false))
    {
        return 0;
    }
    LL_PROFILE_ZONE_SCOPED_CATEGORY_PIPELINE;
    static LLCachedControl<bool> verify(gSavedSettings, "FSPerfGPUDrivenDrawsVerify", false);

    LLGLSLShader* stock = LLGLSLShader::sCurBoundShaderPtr;
    if (!stock)
    {
        return 0;
    }

    sItems.clear();
    for (LLDrawInfo* params : batches)
    {
        sItems.push_back({ params, 1 });
    }
    std::sort(sItems.begin(), sItems.end(), [](const Item& a, const Item& b)
    {
        const LLDrawInfo* lhs = a.mParams;
        const LLDrawInfo* rhs = b.mParams;
        if (lhs->mModelMatrix != rhs->mModelMatrix)
        {
            return lhs->mModelMatrix < rhs->mModelMatrix;
        }
        if (lhs->mVertexBuffer.get() != rhs->mVertexBuffer.get())
        {
            return lhs->mVertexBuffer.get() < rhs->mVertexBuffer.get();
        }
        if (lhs->mOffset != rhs->mOffset)
        {
            return lhs->mOffset < rhs->mOffset;
        }
        return lhs->mCount > rhs->mCount;
    });

    static std::vector<Item> left;
    left.clear();
    build_commands(sItems, left, false, verify);

    const U32 total = (U32)batches.size();
    U32 calls = 0;
    if (!sCommands.empty() && !bind_program(0))
    {   // the arena copies stay for the next try; this pass goes the stock way
        stock->bind();
        return 0;
    }
    if (!sCommands.empty())
    {
        draw_commands(false);
        calls = (U32)sSegments.size();
        stock->bind();
        if (verify)
        {
            verify_against_stock(false);
        }
    }

    batches.clear();
    for (const Item& item : left)
    {
        batches.push_back(item.mParams);
    }
    gGL.loadMatrix(gGLModelView);
    gGLLastMatrix = NULL;
    count_stats(total, calls, (U32)batches.size());
    return calls;
}

U32 fsGDDDrawShadowSinglePass(std::vector<LLPipeline::FSShadowSPEntry>& list, const glm::mat4* clip)
{
    if (list.empty() || !fsGDDShadowEnabled(true))
    {
        return 0;
    }
    LL_PROFILE_ZONE_SCOPED_CATEGORY_PIPELINE;
    static LLCachedControl<bool> verify(gSavedSettings, "FSPerfGPUDrivenDrawsVerify", false);

    // already sorted by mask, model matrix, buffer and offset (fsShadowSinglePassFinish)
    sItems.clear();
    for (const LLPipeline::FSShadowSPEntry& entry : list)
    {
        sItems.push_back({ entry.mParams, entry.mMask });
    }

    static std::vector<Item> left;
    left.clear();
    build_commands(sItems, left, true, verify);

    const U32 total = (U32)list.size();
    U32 calls = 0;
    if (!sCommands.empty() && !bind_program(1))
    {   // the list is untouched, the caller draws it
        return 0;
    }
    if (!sCommands.empty())
    {
        gFSGDDShadowSPProgram.uniformMatrix4fv(sClip, 4, GL_FALSE, glm::value_ptr(clip[0]));
        draw_commands(true);
        calls = (U32)sSegments.size();
        if (verify && gFSShadowSPProgram.isComplete())
        {
            gFSShadowSPProgram.bind();
            gFSShadowSPProgram.uniformMatrix4fv(sClip, 4, GL_FALSE, glm::value_ptr(clip[0]));
            verify_against_stock(true);
        }
    }

    list.clear();
    for (const Item& item : left)
    {
        list.push_back({ item.mParams, item.mMask });
    }
    gGL.loadMatrix(gGLModelView);
    gGLLastMatrix = NULL;
    count_stats(total, calls, (U32)list.size());
    return calls;
}
