/**
 * @file fsparallelgeometry.cpp
 * @brief Fills volume face vertex data on worker threads during geometry rebuilds
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

// What LLFace::getGeometryVolume touches, and how each piece is handled here:
//
//  - Vertex buffer mapping (getFooStrider -> mapVertexBuffer / mapIndexBuffer): appends to the
//    buffer's mapped region lists and the global mapped buffer list. Not thread safe. Workers
//    run with a per-thread LLVertexBuffer::FSMapRecorder, so the calls only return the pointer
//    into the CPU staging copy (allocated with the buffer, stable) and are logged; the main
//    thread replays the log in the original face order after the join, before anything can
//    call flushBuffers. The resulting region lists are the ones the serial code builds.
//  - Writes into the staging copy: each face owns [geom index, + geom count) of every attribute
//    and [indices index, + indices count) of the index buffer. The widest writes (colour and
//    emissive in 4-vertex vectors, texture coordinates in 2-vertex vectors) round up to a
//    multiple of 4 vertices; volume faces are sized with that padding (LLVOVolume::
//    updateFaceSize), and faces whose geom count is not a multiple of 4 stay serial.
//  - LLVolume::genTangents: allocates tangents in an LLVolumeFace, and LLVolumes are shared
//    between objects. Called here, on the main thread, under the same conditions the fill
//    would call it; in the worker it then finds the tangents present and returns.
//  - LLTextureEntry::getMaterialParams() returns an LLPointer copy, i.e. a non atomic reference
//    count change on a material shared by many faces. getGeometryVolume now uses
//    getMaterialParamsPtr() (same pointer, no count change).
//  - LLFace state: GLOBAL is set or cleared and TEXTURE_ANIM may be cleared. Both are done here
//    first, with the same rule, because the code between queueing and the join (registerFace)
//    reads face state; the worker then writes the same values to its own face only.
//    mTexExtents and (with the octree debug display) mLastUpdateTime / mLastMoveTime are only
//    written to the face being filled.
//  - GLTF faces that are selected build a second vertex buffer (clone + unmapBuffer, GL), and
//    unselected faces release it: such faces stay on the serial path.
//  - Emboss bump (brightness / darkness) reads the diffuse LLViewerTexture's size and discard
//    level, which texture threads can change: serial path.
//  - Animated children swap their relative transform around the fill: serial path. For the
//    rest the transforms are copied into the job.
//  - Reads only: the LLVolumeFace arrays, texture entry fields, the GLTF render material, the
//    material's transforms, the skin info bind matrix, drawable flags and matrices, the
//    object's scale, the sky's sun and moon direction. None of them change while the main
//    thread waits in the join.

#include "llviewerprecompiledheaders.h"

#include "fsparallelgeometry.h"

#include "lldrawable.h"
#include "lldrawpoolbump.h"
#include "llface.h"
#include "llspatialpartition.h"
#include "llviewercontrol.h"
#include "llviewerobject.h"
#include "llvovolume.h"
#include "llvertexbuffer.h"
#include "llvolume.h"
#include "pipeline.h"

#include <atomic>
#include <condition_variable>
#include <mutex>
#include <thread>
#include <xmmintrin.h>

int FSParallelGeometry::sDepth = 0;

namespace
{
    // Small persistent pool: the main thread hands out a range of chunk indices, wakes the
    // workers and works on the same range itself, so the wait is bounded by the longest chunk.
    // The pool object is never destroyed and its threads are detached, so nothing has to be
    // torn down on exit.
    class FSGeometryPool
    {
    public:
        typedef void (*chunk_fn_t)(void* ctx, U32 chunk);

        static FSGeometryPool* instance()
        {
            static FSGeometryPool* sPool = new FSGeometryPool();
            return sPool;
        }

        U32 getWorkerCount() const { return mWorkerCount; }

        void run(U32 count, chunk_fn_t fn, void* ctx)
        {
            mFn = fn;
            mCtx = ctx;
            mCount = count;
            mNext.store(0);
            mDone.store(0);
            mCSR = _mm_getcsr();   // same rounding / denormal handling as the main thread
            mOpen.store(true);
            {
                std::lock_guard<std::mutex> lock(mMutex);
                ++mGeneration;
            }
            mCV.notify_all();

            work();

            {
                LL_PROFILE_ZONE_NAMED_CATEGORY_VOLUME("fsgeom - join");
                while (mDone.load() < count)
                {
                    std::this_thread::yield();
                }
            }

            // Late wakers see mOpen false and leave; wait for any that got in before.
            mOpen.store(false);
            while (mActive.load() != 0)
            {
                std::this_thread::yield();
            }
        }

    private:
        FSGeometryPool()
        {
            U32 hw = std::thread::hardware_concurrency();
            mWorkerCount = (hw > 3) ? llmin(hw - 2, (U32)6) : 1;
            for (U32 i = 0; i < mWorkerCount; ++i)
            {
                std::thread(&FSGeometryPool::workerLoop, this).detach();
            }
        }

        void work()
        {
            U32 chunk;
            while ((chunk = mNext.fetch_add(1)) < mCount)
            {
                mFn(mCtx, chunk);
                mDone.fetch_add(1);
            }
        }

        void workerLoop()
        {
            LL_PROFILER_SET_THREAD_NAME("FS Geometry Fill");
            U64 seen = 0;
            while (true)
            {
                {
                    std::unique_lock<std::mutex> lock(mMutex);
                    mCV.wait(lock, [&] { return mGeneration != seen; });
                    seen = mGeneration;
                }

                // Increment before testing mOpen, the main thread clears mOpen before testing
                // mActive (both sequentially consistent): either we see it closed, or it sees us.
                mActive.fetch_add(1);
                if (mOpen.load())
                {
                    _mm_setcsr(mCSR);
                    work();
                }
                mActive.fetch_sub(1);
            }
        }

        U32 mWorkerCount = 1;

        std::mutex mMutex;
        std::condition_variable mCV;
        U64 mGeneration = 0;

        chunk_fn_t mFn = nullptr;
        void* mCtx = nullptr;
        U32 mCount = 0;
        U32 mCSR = 0;
        std::atomic<U32> mNext{ 0 };
        std::atomic<U32> mDone{ 0 };
        std::atomic<S32> mActive{ 0 };
        std::atomic<bool> mOpen{ false };
    };

    struct FSGeomJob
    {
        LLFace* mFace;
        LLPointer<LLVertexBuffer> mBuffer;
        LLVolume* mVolume;
        LLSpatialGroup* mMeshGroup;
        LLMatrix4 mMatVert;
        LLMatrix3 mMatNorm;
        S32 mTE;
        U32 mVerts;
        U16 mIndexOffset;
        bool mForce;
        bool mNoDebugAssert;
        bool mResult;
        bool mSkip;
        LLVertexBuffer::FSMapRecorder mRec;
    };

    std::vector<FSGeomJob> sJobs;
    std::vector<std::pair<LLPointer<LLDrawable>, U32>> sStateClears;
    std::vector<U32> sChunkStart;

    bool run_job(FSGeomJob& job, LLVertexBuffer::FSMapRecorder* rec)
    {
        LLVertexBuffer::fsSetThreadMapRecorder(rec);
        bool res = job.mFace->getGeometryVolume(*job.mVolume, job.mTE, job.mMatVert, job.mMatNorm,
                                                job.mIndexOffset, job.mForce, job.mNoDebugAssert);
        LLVertexBuffer::fsSetThreadMapRecorder(nullptr);
        return res;
    }

    void run_chunk(void*, U32 chunk)
    {
        LL_PROFILE_ZONE_NAMED_CATEGORY_VOLUME("fsgeom - fill chunk");
        for (U32 i = sChunkStart[chunk]; i < sChunkStart[chunk + 1]; ++i)
        {
            FSGeomJob& job = sJobs[i];
            if (!job.mSkip)
            {
                job.mRec.mCount = 0;
                job.mRec.mOverflow = false;
                job.mResult = run_job(job, &job.mRec);
            }
        }
    }

    // Byte range of the staging copy a recorded map call covers.
    void record_range(const LLVertexBuffer::FSMapRecord& r, U8*& ptr, U32& bytes)
    {
        LLVertexBuffer* vb = r.mBuffer;
        if (r.mType < 0)
        {
            U32 count = r.mCount < 0 ? vb->getNumIndices() - r.mIndex : (U32)r.mCount;
            ptr = vb->getMappedIndices() + sizeof(U16) * r.mIndex;
            bytes = (U32)sizeof(U16) * count;
        }
        else
        {
            U32 count = r.mCount < 0 ? vb->getNumVerts() - r.mIndex : (U32)r.mCount;
            U32 size = LLVertexBuffer::sTypeSize[r.mType];
            ptr = vb->getMappedData() + vb->getOffset((LLVertexBuffer::AttributeType)r.mType) + size * r.mIndex;
            bytes = size * count;
        }
    }

    U32 sVerifyMismatches = 0;
    U32 sVerifyFaces = 0;
    F64 sVerifyLastReport = 0.0;

    void report_mismatch(const FSGeomJob& job, const char* what, S32 type, U32 byte)
    {
        ++sVerifyMismatches;
        if (sVerifyMismatches <= 20 || (sVerifyMismatches % 500) == 0)
        {
            LLViewerObject* obj = job.mFace->getViewerObject();
            LL_WARNS("FSParallelGeometry") << "Verify mismatch #" << sVerifyMismatches << ": " << what
                << " object " << (obj ? obj->getID() : LLUUID::null) << " te " << job.mTE
                << " attribute " << type << " byte " << byte
                << " geom " << job.mFace->getGeomIndex() << "+" << job.mFace->getGeomCount()
                << " indices " << job.mFace->getIndicesStart() << "+" << job.mFace->getIndicesCount()
                << LL_ENDL;
        }
    }

    // Snapshot what the workers wrote, fill every face again serially (in the original
    // order), compare. The buffers end up holding the serial result.
    void verify_jobs()
    {
        LL_PROFILE_ZONE_NAMED_CATEGORY_VOLUME("fsgeom - verify");
        std::vector<std::vector<U8>> snapshots(sJobs.size());
        for (size_t i = 0; i < sJobs.size(); ++i)
        {
            const FSGeomJob& job = sJobs[i];
            if (job.mSkip || job.mRec.mOverflow)
            {
                continue;
            }
            for (U32 r = 0; r < job.mRec.mCount; ++r)
            {
                U8* ptr; U32 bytes;
                record_range(job.mRec.mRecords[r], ptr, bytes);
                snapshots[i].insert(snapshots[i].end(), ptr, ptr + bytes);
            }
        }

        LLVertexBuffer::FSMapRecorder serial_rec;
        for (size_t i = 0; i < sJobs.size(); ++i)
        {
            FSGeomJob& job = sJobs[i];
            if (job.mSkip || job.mRec.mOverflow)
            {
                continue;
            }
            serial_rec.mCount = 0;
            serial_rec.mOverflow = false;
            bool res = run_job(job, &serial_rec);
            ++sVerifyFaces;

            if (res != job.mResult)
            {
                report_mismatch(job, "return value", -2, 0);
            }
            bool same_maps = !serial_rec.mOverflow && serial_rec.mCount == job.mRec.mCount;
            for (U32 r = 0; same_maps && r < serial_rec.mCount; ++r)
            {
                const LLVertexBuffer::FSMapRecord& a = serial_rec.mRecords[r];
                const LLVertexBuffer::FSMapRecord& b = job.mRec.mRecords[r];
                same_maps = a.mBuffer == b.mBuffer && a.mType == b.mType && a.mIndex == b.mIndex && a.mCount == b.mCount;
            }
            if (!same_maps)
            {
                report_mismatch(job, "mapped ranges", -2, 0);
                job.mRec = serial_rec;
                job.mResult = res;
                continue;
            }
            job.mResult = res;
        }

        for (size_t i = 0; i < sJobs.size(); ++i)
        {
            const FSGeomJob& job = sJobs[i];
            if (job.mSkip || job.mRec.mOverflow || snapshots[i].empty())
            {
                continue;
            }
            const U8* snap = snapshots[i].data();
            size_t snap_size = snapshots[i].size();
            size_t off = 0;
            for (U32 r = 0; r < job.mRec.mCount; ++r)
            {
                U8* ptr; U32 bytes;
                record_range(job.mRec.mRecords[r], ptr, bytes);
                if (off + bytes > snap_size)
                {
                    break; // ranges changed, already reported
                }
                if (memcmp(ptr, snap + off, bytes) != 0)
                {
                    U32 b = 0;
                    while (b < bytes && ptr[b] == snap[off + b])
                    {
                        ++b;
                    }
                    report_mismatch(job, "data", job.mRec.mRecords[r].mType, b);
                    break;
                }
                off += bytes;
            }
        }

        F64 now = LLTimer::getTotalSeconds();
        if (now - sVerifyLastReport > 60.0)
        {
            sVerifyLastReport = now;
            LL_INFOS("FSParallelGeometry") << "Verify: " << sVerifyFaces << " faces checked, "
                << sVerifyMismatches << " mismatches" << LL_ENDL;
        }
    }
}

FSParallelGeometry::Scope::Scope()
    : mOpen(false)
{
    static LLCachedControl<bool> enabled(gSavedSettings, "FSPerfParallelGeometry", false);
    if (enabled)
    {
        mOpen = true;
        ++sDepth;
    }
}

FSParallelGeometry::Scope::~Scope()
{
    end();
}

void FSParallelGeometry::Scope::end()
{
    if (mOpen)
    {
        mOpen = false;
        flush();
        --sDepth;
    }
}

//static
bool FSParallelGeometry::deferFill(LLFace* facep, U16 index_offset, bool force_rebuild,
                                   bool no_debug_assert, LLSpatialGroup* mesh_group)
{
    if (!sDepth || !facep)
    {
        return false;
    }

    LLDrawable* drawablep = facep->getDrawable();
    if (!drawablep || drawablep->isState(LLDrawable::ANIMATED_CHILD))
    {
        return false;
    }

    LLVOVolume* vobj = drawablep->getVOVolume();
    LLViewerObject* face_obj = facep->getViewerObject();
    LLVolume* volume = vobj ? vobj->getVolume() : nullptr;
    LLVertexBuffer* vb = facep->getVertexBuffer();
    if (!volume || !vb || face_obj != vobj)
    {
        return false;
    }

    // Padded faces only: the vector-wide colour / texcoord writes must stay in the face's range.
    if ((facep->getGeomCount() & 0x3) != 0 || facep->getGeomCount() == 0)
    {
        return false;
    }

    S32 te = facep->getTEOffset();
    if (te < 0 || te >= volume->getNumVolumeFaces())
    {
        return false; // fill would fail and maybe log, keep that serial
    }

    const LLVolumeFace& vf = volume->getVolumeFace(te);
    S32 num_vertices = llclamp((S32)vf.mNumVertices, 0, (S32)facep->getGeomCount());
    S32 num_indices = llclamp((S32)vf.mNumIndices, 0, (S32)facep->getIndicesCount());
    if ((U32)num_indices + (U32)facep->getIndicesStart() > vb->getNumIndices() ||
        num_vertices + (U32)facep->getGeomIndex() > vb->getNumVerts())
    {
        return false;
    }

    const LLTextureEntry* tep = face_obj->getTE(te);
    if (!tep || !vb->getMappedData() || !vb->getMappedIndices())
    {
        return false;
    }

    const bool selected = tep->isSelected();
    if (selected && tep->getGLTFRenderMaterial())
    {
        return false; // builds mVertexBufferGLTF (GL)
    }
    if (!selected && facep->mVertexBufferGLTF.notNull())
    {
        return false; // releases mVertexBufferGLTF (GL)
    }

    // The rebuild flags exactly as getGeometryVolume derives them.
    const bool full_rebuild = force_rebuild || drawablep->isState(LLDrawable::REBUILD_VOLUME);
    const bool rebuild_pos = full_rebuild || drawablep->isState(LLDrawable::REBUILD_POSITION);
    const bool rebuild_tcoord = full_rebuild || drawablep->isState(LLDrawable::REBUILD_TCOORD);
    const bool rebuild_tangent = rebuild_pos && vb->hasDataType(LLVertexBuffer::TYPE_TANGENT);
    const U8 bump_code = tep->getBumpmap();

    if (rebuild_tcoord && (bump_code == BE_BRIGHTNESS || bump_code == BE_DARKNESS))
    {
        return false; // reads LLViewerTexture state
    }

    LL_PROFILE_ZONE_SCOPED_CATEGORY_VOLUME;

    // Side effects of getGeometryVolume that are not local to the face, done here.
    if ((rebuild_tcoord && (bump_code || tep->getTexGen() != LLTextureEntry::TEX_GEN_DEFAULT)) || rebuild_tangent)
    {
        face_obj->getVolume()->genTangents(te);
    }

    // Face state getGeometryVolume sets, set now because registerFace reads face state before
    // the queued fill runs. The fill writes the same values again.
    if (drawablep->isStatic())
    {
        facep->setState(LLFace::GLOBAL);
    }
    else
    {
        facep->clearState(LLFace::GLOBAL);
    }
    if (rebuild_tcoord && facep->isState(LLFace::TEXTURE_ANIM) && !vobj->mTexAnimMode)
    {
        facep->clearState(LLFace::TEXTURE_ANIM);
    }

    sJobs.emplace_back();
    FSGeomJob& job = sJobs.back();
    job.mFace = facep;
    job.mBuffer = vb;
    job.mVolume = volume;
    job.mMeshGroup = mesh_group;
    job.mMatVert = vobj->getRelativeXform();
    job.mMatNorm = vobj->getRelativeXformInvTrans();
    job.mTE = te;
    job.mVerts = facep->getGeomCount();
    job.mIndexOffset = index_offset;
    job.mForce = force_rebuild;
    job.mNoDebugAssert = no_debug_assert;
    job.mResult = false;
    job.mSkip = false;
    return true;
}

//static
void FSParallelGeometry::clearStateAfterFill(LLDrawable* drawablep, U32 state)
{
    if (sDepth)
    {
        sStateClears.emplace_back(drawablep, state);
    }
    else
    {
        drawablep->clearState(state);
    }
}

//static
void FSParallelGeometry::flush()
{
    if (sJobs.empty())
    {
        for (auto& clear : sStateClears)
        {
            clear.first->clearState(clear.second);
        }
        sStateClears.clear();
        return;
    }

    LL_PROFILE_ZONE_SCOPED_CATEGORY_VOLUME;

    static LLCachedControl<bool> verify(gSavedSettings, "FSPerfParallelGeometryVerify", false);
    static LLCachedControl<U32> min_verts(gSavedSettings, "FSPerfParallelGeometryMinVerts", 4096);

    U32 total_verts = 0;
    for (FSGeomJob& job : sJobs)
    {
        // Nothing between queueing and here may move a face to another buffer; if it ever
        // does, the queued offsets are stale and the fill must not run.
        if (job.mFace->getVertexBuffer() != job.mBuffer.get())
        {
            LL_WARNS_ONCE("FSParallelGeometry") << "Face changed vertex buffer before its queued fill, skipped" << LL_ENDL;
            job.mSkip = true;
            continue;
        }
        total_verts += job.mVerts;
    }

    FSGeometryPool* pool = (sJobs.size() >= 2 && total_verts >= (U32)min_verts) ? FSGeometryPool::instance() : nullptr;

    if (pool)
    {
        LL_PROFILE_ZONE_NAMED_CATEGORY_VOLUME("fsgeom - parallel fill");
        LL_PROFILE_ZONE_NUM(sJobs.size());
        LL_PROFILE_ZONE_NUM(total_verts);

        // Contiguous chunks of about total / (4 * threads) vertices, at least 512.
        U32 threads = pool->getWorkerCount() + 1;
        U32 grain = llmax(total_verts / (4 * threads), (U32)512);
        sChunkStart.clear();
        sChunkStart.push_back(0);
        U32 acc = 0;
        for (U32 i = 0; i < (U32)sJobs.size(); ++i)
        {
            acc += sJobs[i].mSkip ? 0 : sJobs[i].mVerts;
            if (acc >= grain)
            {
                sChunkStart.push_back(i + 1);
                acc = 0;
            }
        }
        if (sChunkStart.back() != (U32)sJobs.size())
        {
            sChunkStart.push_back((U32)sJobs.size());
        }

        LLVertexBuffer::fsSetMapRecordingActive(true);
        pool->run((U32)sChunkStart.size() - 1, run_chunk, nullptr);

        if (verify)
        {
            verify_jobs();
        }
        LLVertexBuffer::fsSetMapRecordingActive(false);

        // Register the mapped ranges, in the original face order.
        LL_PROFILE_ZONE_NAMED_CATEGORY_VOLUME("fsgeom - replay maps");
        for (FSGeomJob& job : sJobs)
        {
            if (job.mSkip)
            {
                continue;
            }
            if (job.mRec.mOverflow)
            {
                // More map calls than the recorder holds (not expected): fill again on this
                // thread without recording, which writes the same data and maps for real.
                job.mResult = run_job(job, nullptr);
                continue;
            }
            for (U32 r = 0; r < job.mRec.mCount; ++r)
            {
                LLVertexBuffer::fsReplayMap(job.mRec.mRecords[r]);
            }
        }
    }
    else
    {
        LL_PROFILE_ZONE_NAMED_CATEGORY_VOLUME("fsgeom - serial fill");
        for (FSGeomJob& job : sJobs)
        {
            if (!job.mSkip)
            {
                job.mResult = run_job(job, nullptr);
            }
        }
    }

    // Failure handling of the callers, in order.
    for (FSGeomJob& job : sJobs)
    {
        if (job.mSkip || job.mResult)
        {
            continue;
        }
        if (job.mMeshGroup)
        {
            // rebuildMesh: something's gone wrong with the vertex buffer accounting
            job.mMeshGroup->dirtyGeom();
            gPipeline.markRebuild(job.mMeshGroup);
        }
        else
        {
            LL_WARNS() << "Failed to get geometry for face!" << LL_ENDL;
        }
    }
    sJobs.clear();

    for (auto& clear : sStateClears)
    {
        clear.first->clearState(clear.second);
    }
    sStateClears.clear();
}
