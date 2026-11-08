/**
 * @file fsparallelcull.cpp
 * @brief Shadow cascade, main camera and probe face culls on worker threads (FSPerfParallelShadowCull,
 *        FSPerfParallelMainCull)
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

// Each sun shadow cascade runs updateCull (an octree walk of every partition of every region) right
// before it is drawn, one cascade after another on the main thread.
//
// With FSPerfParallelShadowCull on, the cascade loop in generateSunShadow still sets every cascade up
// as before (split, point cloud, projection, matrices, target clears of empty cascades), but instead
// of drawing a cascade it records what the draw needs (fsParallelShadowCullDefer). After the loop,
// fsParallelShadowCullFlush:
//
//  1. prepares, on the main thread, everything the culls would have changed in shared state: the water
//     clip plane of each cascade camera, the rebound of every partition's root group and the VO cache
//     partition (which only rebounds its root for shadow cameras);
//  2. walks the octrees for all cascades on worker threads, one job per cascade, region and partition.
//     A job only reads the octree and its own camera and writes its own list of groups; it does exactly
//     what LLOctreeCullShadow does with occlusion off (renderShadow turns occlusion off for its cull, so
//     checkOcclusion and markOccluder do nothing there);
//  3. commits, on the main thread and in the order the serial culls would have, what markNotCulled
//     does with each group: setVisible for the cascade's camera, the push into the cascade's cull
//     result and the visible node count;
//  4. draws the cascades in order, each with the state its draw had in the serial loop (camera id,
//     matrices, coloured cascade, minimum caster size) and renderShadow told not to cull again.
//
// The octree is not changed while the workers run: the main thread is inside the job loop or waiting
// for it, and no other thread changes the octree. All jobs finish before the first stateSort, which is
// what changes groups and drawables (rebuilds, LOD, bridges). The culls of all cascades now see the
// octree as it was before cascade 0 was drawn, where the serial culls of the later cascades saw it
// after the earlier cascades' stateSort; a group whose bounds changed in between is culled with its
// old bounds for one frame. FSPerfParallelShadowCullVerify runs the serial cull at the old place too
// and logs any difference.
//
// Not parallel: the stateSort of each cascade (it rebuilds geometry and culls inside spatial bridges,
// both main thread only), the projector shadows, the RenderShadowCullMode 1 union cull (already one
// walk) and the reflection probe sun shadows rendered by generateProbeSunShadow.

// The main camera and reflection probe face culls (FSPerfParallelMainCull) are described further down.

#include "llviewerprecompiledheaders.h"

#include "fsparallelcull.h"

#include "pipeline.h"

#include "llagent.h"
#include "llagentcamera.h"
#include "llenvironment.h"
#include "llrender.h"
#include "llspatialpartition.h"
#include "lltimer.h"
#include "llviewercamera.h"
#include "llviewercontrol.h"
#include "llviewerregion.h"
#include "llvieweroctree.h"
#include "llvocache.h"
#include "llworld.h"

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <mutex>
#include <thread>

extern bool gCubeSnapshot;

//-----------------------------------------------------------------------------
// Worker pool
//-----------------------------------------------------------------------------

namespace
{
    struct FSCullBatch
    {
        const std::function<void(U32)>* mJob = nullptr;
        U32                 mCount = 0;
        std::atomic<U32>    mNext{ 0 };
        U32                 mUsers = 0;     // workers inside drain(), guarded by the pool mutex

        void drain()
        {
            for (U32 i = mNext.fetch_add(1); i < mCount; i = mNext.fetch_add(1))
            {
                (*mJob)(i);
            }
        }
    };

    class FSCullPool
    {
    public:
        FSCullPool()
        {
            const U32 hw = std::thread::hardware_concurrency();
            // the caller works too; leave cores for the other viewer threads
            const U32 count = hw > 2 ? llclamp(hw - 2, 1U, 4U) : 1U;
            for (U32 i = 0; i < count; ++i)
            {
                try
                {
                    mThreads.emplace_back([this, i]() { workerLoop(i); });
                }
                catch (const std::exception& e)
                {
                    LL_WARNS("FSParallelCull") << "Could not start a cull worker: " << e.what() << LL_ENDL;
                    break;
                }
            }
            LL_INFOS("FSParallelCull") << "Cull workers: " << mThreads.size() << LL_ENDL;
        }

        ~FSCullPool()
        {
            {
                std::lock_guard<std::mutex> lock(mMutex);
                mQuit = true;
            }
            mWake.notify_all();
            for (std::thread& t : mThreads)
            {
                if (t.joinable())
                {
                    t.join();
                }
            }
        }

        U32 size() const { return (U32)mThreads.size(); }

        void run(U32 count, const std::function<void(U32)>& job)
        {
            FSCullBatch batch;
            batch.mJob = &job;
            batch.mCount = count;
            if (mThreads.empty() || count < 2)
            {
                batch.drain();
                return;
            }
            {
                std::lock_guard<std::mutex> lock(mMutex);
                mBatch = &batch;
                ++mGeneration;
            }
            mWake.notify_all();

            batch.drain();  // the caller works through jobs too

            // every job is taken; wait for the workers still running one (each is one octree walk)
            {
                LL_PROFILE_ZONE_NAMED_CATEGORY_PIPELINE("fs cull wait");
                std::unique_lock<std::mutex> lock(mMutex);
                mDone.wait(lock, [&batch]() { return batch.mUsers == 0; });
                mBatch = nullptr;
            }
        }

    private:
        void workerLoop(U32 index)
        {
            std::string name = "FS Cull " + std::to_string(index);
            LL_PROFILER_SET_THREAD_NAME(name.c_str());
            U64 seen = 0;
            while (true)
            {
                FSCullBatch* batch = nullptr;
                {
                    std::unique_lock<std::mutex> lock(mMutex);
                    mWake.wait(lock, [this, seen]() { return mQuit || (mBatch && mGeneration != seen); });
                    if (mQuit)
                    {
                        return;
                    }
                    seen = mGeneration;
                    batch = mBatch;
                    ++batch->mUsers;
                }

                batch->drain();

                {
                    std::lock_guard<std::mutex> lock(mMutex);
                    --batch->mUsers;
                }
                mDone.notify_all();
            }
        }

        std::vector<std::thread>    mThreads;
        std::mutex                  mMutex;
        std::condition_variable     mWake;
        std::condition_variable     mDone;
        FSCullBatch*                mBatch = nullptr;
        U64                         mGeneration = 0;
        bool                        mQuit = false;
    };

    std::unique_ptr<FSCullPool> sPool;
}

void FSParallelCull::run(U32 count, const std::function<void(U32)>& job)
{
    if (!sPool)
    {
        sPool = std::make_unique<FSCullPool>();
    }
    sPool->run(count, job);
}

U32 FSParallelCull::workerCount()
{
    return sPool ? sPool->size() : 0;
}

void FSParallelCull::shutdown()
{
    sPool.reset();
}

//-----------------------------------------------------------------------------
// Shadow cascade culls
//-----------------------------------------------------------------------------

namespace
{
    // LLOctreeCullShadow with occlusion off, for any thread: reads the octree and the camera only and
    // collects the groups markNotCulled would have been called with.
    class FSOctreeCullShadowJob : public LLViewerOctreeCull
    {
    public:
        FSOctreeCullShadowJob(LLCamera* camera, F32 min_caster_size, std::vector<LLSpatialGroup*>& out)
            : LLViewerOctreeCull(camera), mMinCasterSize(min_caster_size), mOut(out) { }

        // LLOctreeCull::earlyFail calls checkOcclusion, which returns at once with occlusion off, and
        // then fails only with occlusion on
        virtual bool earlyFail(LLViewerOctreeGroup* group) { return false; }

        virtual S32 frustumCheck(const LLViewerOctreeGroup* group)
        {
            return AABBInFrustumGroupBounds(group);
        }

        virtual S32 frustumCheckObjects(const LLViewerOctreeGroup* group)
        {
            return AABBInFrustumObjectBounds(group);
        }

        virtual void processGroup(LLViewerOctreeGroup* base_group)
        {
            LLSpatialGroup* group = (LLSpatialGroup*)base_group;
            if (LLPipeline::isShadowCasterTooSmall(group, mMinCasterSize))
            {
                return;
            }
            if (group->isEmpty())   // markNotCulled skips these
            {
                return;
            }
            mOut.push_back(group);
        }

    private:
        F32                             mMinCasterSize;
        std::vector<LLSpatialGroup*>&   mOut;
    };

    // what LLPipeline::generateSunShadow had set up for a cascade's draw
    struct FSDeferredCascade
    {
        S32         mCascade = 0;
        LLViewerCamera::eCameraID mCameraID = LLViewerCamera::CAMERA_WORLD;
        LLCamera    mCamera;
        glm::mat4   mView;
        glm::mat4   mProj;
        glm::mat4   mLastView;
        glm::mat4   mLastProj;
        F32         mMinCasterSize = 0.f;
        S32         mColoredCascade = -1;
    };

    struct FSCullJob
    {
        U32                 mDeferred = 0;      // index into sDeferred
        LLSpatialPartition* mPartition = nullptr;
        bool                mByDrawable = false;
        std::vector<LLSpatialGroup*> mGroups;
    };

    std::vector<FSDeferredCascade>  sDeferred;
    std::vector<FSCullJob>          sJobs;
    std::vector<U32>                sJobOrder;
    LLCullResult                    sResult[4];
    LLCullResult                    sVerifyResult;

    // stats for the verify log
    U32 sVerifyChecks = 0;
    U32 sVerifyMismatches = 0;
    F64 sVerifyLastLog = 0.0;
    U32 sVerifyWarnings = 0;

    // updateCull's water clip plane, which it sets on the camera it culls with
    void fs_apply_water_clip(LLCamera& camera)
    {
        if (LLPipeline::isWaterClip())
        {
            LLVector3 pnorm;
            F32 water_height = LLEnvironment::instance().getWaterHeight();
            if (LLPipeline::sUnderWaterRender)
            {
                pnorm.setVec(0, 0, 1);
            }
            else
            {
                pnorm = LLVector3(0, 0, -1);
            }
            LLPlane plane;
            plane.setVec(LLVector3(0, 0, water_height), pnorm);
            camera.setUserClipPlane(plane);
        }
        else
        {
            camera.disableUserClipPlane();
        }
    }

    void fs_list_groups(LLCullResult& result, std::vector<LLSpatialGroup*>& visible, std::vector<LLSpatialGroup*>& drawable)
    {
        visible.assign(result.beginVisibleGroups(), result.endVisibleGroups());
        drawable.assign(result.beginDrawableGroups(), result.endDrawableGroups());
    }

    // counts groups in a but not in b
    U32 fs_count_missing(std::vector<LLSpatialGroup*> a, std::vector<LLSpatialGroup*> b)
    {
        std::sort(a.begin(), a.end());
        std::sort(b.begin(), b.end());
        U32 missing = 0;
        for (LLSpatialGroup* g : a)
        {
            if (!std::binary_search(b.begin(), b.end(), g))
            {
                ++missing;
            }
        }
        return missing;
    }
}

// generateSunShadow, before the cascade loop
void LLPipeline::fsParallelShadowCullBegin(bool have_union_cull)
{
    static LLCachedControl<bool> parallel_cull(gSavedSettings, "FSPerfParallelShadowCull", false);
    sDeferred.clear();
    // the union cull of RenderShadowCullMode 1 is a single walk already; the sky is never in the shadow
    // render types, but updateCull would push it, so keep the serial path if it ever is
    mFSParallelCullActive = parallel_cull && !have_union_cull &&
                            !hasRenderType(LLPipeline::RENDER_TYPE_SKY) &&
                            !hasRenderType(LLPipeline::RENDER_TYPE_WL_SKY);
}

// generateSunShadow, where a cascade with receivers would bind its target and draw. True when the draw
// is deferred to fsParallelShadowCullFlush, the loop then goes on to the next cascade.
bool LLPipeline::fsParallelShadowCullDefer(S32 cascade, LLCamera& shadow_cam, F32 min_texel_size)
{
    if (!mFSParallelCullActive || cascade < 0 || cascade > 3)
    {
        return false;
    }

    FSDeferredCascade rec;
    rec.mCascade = cascade;
    rec.mCameraID = LLViewerCamera::sCurCameraID;
    rec.mCamera = shadow_cam;
    rec.mView = get_current_modelview();
    rec.mProj = get_current_projection();
    rec.mLastView = get_last_modelview();
    rec.mLastProj = get_last_projection();
    rec.mColoredCascade = mColoredShadowCascade;

    // the minimum caster size as generateSunShadow works it out for the draw
    static LLCachedControl<F32> min_caster_texels(gSavedSettings, "RenderShadowMinCasterTexels", 1.f);
    static LLCachedControl<F32> far_caster_scale(gSavedSettings, "FSRenderShadowFarCascadeMinCasterScale", 1.f);
    const F32 cascade_caster_scale = cascade >= 2 ? llclamp((F32)far_caster_scale, 1.f, 16.f) : 1.f;
    rec.mMinCasterSize = (!gCubeSnapshot && min_caster_texels > 0.f && llfinite(min_texel_size))
        ? llmax(min_texel_size, 0.f) * min_caster_texels * cascade_caster_scale : 0.f;

    sDeferred.push_back(rec);
    return true;
}

// generateSunShadow, after the cascade loop: cull the deferred cascades in parallel, then draw them
void LLPipeline::fsParallelShadowCullFlush()
{
    mFSParallelCullActive = false;
    if (sDeferred.empty())
    {
        return;
    }
    LL_PROFILE_ZONE_SCOPED_CATEGORY_PIPELINE;

    static LLCachedControl<bool> verify(gSavedSettings, "FSPerfParallelShadowCullVerify", false);

    // what the cascade loop left behind, put back after the deferred draws
    const LLViewerCamera::eCameraID saved_camera_id = LLViewerCamera::sCurCameraID;
    const S32 saved_colored = mColoredShadowCascade;
    const glm::mat4 saved_view = get_current_modelview();
    const glm::mat4 saved_proj = get_current_projection();
    const glm::mat4 saved_last_view = get_last_modelview();
    const glm::mat4 saved_last_proj = get_last_projection();

    // 1. main thread: the shared state updateCull changes before it walks
    U32 job_count = 0;
    {
        LL_PROFILE_ZONE_NAMED_CATEGORY_PIPELINE("fs shadow cull prepare");
        const bool avatars_only = isAvatarOnlyRender();
        const LLWorld::region_list_t& regions = LLWorld::getInstance()->getRegionList();

        for (U32 d = 0; d < sDeferred.size(); ++d)
        {
            FSDeferredCascade& rec = sDeferred[d];
            fs_apply_water_clip(rec.mCamera);
            LLViewerCamera::sCurCameraID = rec.mCameraID;

            for (LLViewerRegion* region : regions)
            {
                for (U32 i = 0; i < LLViewerRegion::NUM_PARTITIONS; i++)
                {
                    if (avatars_only &&
                        i != LLViewerRegion::PARTITION_BRIDGE &&
                        i != LLViewerRegion::PARTITION_AVATAR &&
                        i != LLViewerRegion::PARTITION_CONTROL_AV)
                    {
                        continue;
                    }
                    LLSpatialPartition* part = region->getSpatialPartition(i);
                    if (part && (LLViewerRegion::PARTITION_BRIDGE == i || hasRenderType(part->mDrawableType)))
                    {
                        // LLSpatialPartition::cull rebounds the root before walking
                        ((LLSpatialGroup*)part->mOctree->getListener(0))->rebound();

                        if (job_count >= sJobs.size())
                        {
                            sJobs.emplace_back();
                        }
                        FSCullJob& job = sJobs[job_count++];
                        job.mDeferred = d;
                        job.mPartition = part;
                        job.mByDrawable = !part->mRenderByGroup;
                        job.mGroups.clear();
                    }
                }

                // for shadow cameras this only rebounds the cache octree's root
                LLVOCachePartition* vo_part = region->getVOCachePartition();
                if (vo_part)
                {
                    vo_part->cull(rec.mCamera, false);
                }
            }
        }

        // the object partitions are the big walks: start them first
        sJobOrder.resize(job_count);
        for (U32 i = 0; i < job_count; ++i)
        {
            sJobOrder[i] = i;
        }
        std::stable_sort(sJobOrder.begin(), sJobOrder.end(), [](U32 a, U32 b)
        {
            const U32 ta = sJobs[a].mPartition->mPartitionType;
            const U32 tb = sJobs[b].mPartition->mPartitionType;
            const bool heavy_a = ta == LLViewerRegion::PARTITION_VOLUME || ta == LLViewerRegion::PARTITION_BRIDGE;
            const bool heavy_b = tb == LLViewerRegion::PARTITION_VOLUME || tb == LLViewerRegion::PARTITION_BRIDGE;
            return heavy_a && !heavy_b;
        });
    }

    // 2. worker threads (and this one): the octree walks, read only
    {
        LL_PROFILE_ZONE_NAMED_CATEGORY_PIPELINE("fs shadow cull parallel");
        FSParallelCull::run(job_count, [](U32 index)
        {
            LL_PROFILE_ZONE_NAMED_CATEGORY_PIPELINE("fs shadow cull job");
            FSCullJob& job = sJobs[sJobOrder[index]];
            FSDeferredCascade& rec = sDeferred[job.mDeferred];
            FSOctreeCullShadowJob culler(&rec.mCamera, rec.mMinCasterSize, job.mGroups);
            culler.traverse(job.mPartition->mOctree);
        });
    }

    // 3. main thread: markNotCulled's changes, in the serial order
    {
        LL_PROFILE_ZONE_NAMED_CATEGORY_PIPELINE("fs shadow cull commit");
        U32 job = 0;
        for (U32 d = 0; d < sDeferred.size(); ++d)
        {
            FSDeferredCascade& rec = sDeferred[d];
            LLCullResult& result = sResult[rec.mCascade];
            LLViewerCamera::sCurCameraID = rec.mCameraID;
            grabReferences(result);
            result.clear();
            for (; job < job_count && sJobs[job].mDeferred == d; ++job)
            {
                for (LLSpatialGroup* group : sJobs[job].mGroups)
                {
                    group->setVisible();
                    if (sJobs[job].mByDrawable)
                    {
                        result.pushDrawableGroup(group);
                    }
                    else
                    {
                        result.pushVisibleGroup(group);
                    }
                    mNumVisibleNodes++;
                }
                sJobs[job].mGroups.clear();
            }
        }
    }

    // 4. the deferred draws, in cascade order, as the loop would have made them
    std::vector<LLSpatialGroup*> par_visible, par_drawable, ser_visible, ser_drawable;
    for (FSDeferredCascade& rec : sDeferred)
    {
        const S32 j = rec.mCascade;
        LLCullResult& result = sResult[j];

        mColoredShadowCascade = rec.mColoredCascade;
        LLViewerCamera::sCurCameraID = rec.mCameraID;
        set_current_modelview(rec.mView);
        set_current_projection(rec.mProj);
        set_last_modelview(rec.mLastView);
        set_last_projection(rec.mLastProj);

        mRT->shadow[j].bindTarget();
        mRT->shadow[j].getViewport(gGLViewport);
        mRT->shadow[j].clear();

        sShadowMinCasterSize = rec.mMinCasterSize;

        if (verify)
        {   // the serial cull, where the serial loop would have run it
            LL_PROFILE_ZONE_NAMED_CATEGORY_PIPELINE("fs shadow cull verify");
            const bool saved_shadow_render = sShadowRender;
            const S32 saved_occlusion = sUseOcclusion;
            const S32 saved_nodes = mNumVisibleNodes;
            sShadowRender = true;
            sUseOcclusion = 0;
            LLCamera verify_cam = rec.mCamera;
            updateCull(verify_cam, sVerifyResult);
            sShadowRender = saved_shadow_render;
            sUseOcclusion = saved_occlusion;
            mNumVisibleNodes = saved_nodes;

            fs_list_groups(result, par_visible, par_drawable);
            fs_list_groups(sVerifyResult, ser_visible, ser_drawable);
            ++sVerifyChecks;
            if (par_visible != ser_visible || par_drawable != ser_drawable)
            {
                ++sVerifyMismatches;
                if (sVerifyWarnings < 20)
                {
                    ++sVerifyWarnings;
                    LL_WARNS("FSParallelCull") << "Shadow cascade " << j << " cull differs from the serial cull:"
                        << " by group " << par_visible.size() << " vs " << ser_visible.size()
                        << " (missing " << fs_count_missing(ser_visible, par_visible)
                        << ", extra " << fs_count_missing(par_visible, ser_visible) << "),"
                        << " by drawable " << par_drawable.size() << " vs " << ser_drawable.size()
                        << " (missing " << fs_count_missing(ser_drawable, par_drawable)
                        << ", extra " << fs_count_missing(par_drawable, ser_drawable) << ")"
                        << (sVerifyWarnings == 20 ? "; further differences only in the summary" : "") << LL_ENDL;
                }
            }
        }

        fsShadowCacheBeginCascade(j, rec.mProj, rec.mView);
        fsShadowSinglePassBeginCascade(j, rec.mProj, rec.mView, result);
        renderShadow(rec.mView, rec.mProj, rec.mCamera, result, true, false);
        fsShadowSinglePassEndCascade();
        fsShadowCacheEndCascade();
        sShadowMinCasterSize = 0.f;

        mRT->shadow[j].flush();

        if (!hasRenderDebugMask(LLPipeline::RENDER_DEBUG_SHADOW_FRUSTA) && !gCubeSnapshot)
        {
            mShadowCamera[j + 4] = rec.mCamera;
        }
    }

    if (verify)
    {
        const F64 now = LLTimer::getTotalSeconds();
        if (now - sVerifyLastLog > 10.0)
        {
            sVerifyLastLog = now;
            LL_INFOS("FSParallelCull") << "Shadow cascade cull verify: " << sVerifyChecks << " cascades checked, "
                << sVerifyMismatches << " differed from the serial cull ("
                << FSParallelCull::workerCount() << " workers)" << LL_ENDL;
            sVerifyChecks = 0;
            sVerifyMismatches = 0;
        }
    }

    sDeferred.clear();

    LLViewerCamera::sCurCameraID = saved_camera_id;
    mColoredShadowCascade = saved_colored;
    set_current_modelview(saved_view);
    set_current_projection(saved_proj);
    set_last_modelview(saved_last_view);
    set_last_projection(saved_last_proj);
}

//-----------------------------------------------------------------------------
// Main camera and reflection probe face culls (FSPerfParallelMainCull)
//-----------------------------------------------------------------------------

// updateCull for the world camera and for every reflection / hero probe cube face walks, region by
// region, the octree of each spatial partition (LLOctreeCull / LLOctreeCullNoFarClip) and then the
// region's object cache octree (LLVOCachePartition::cull). Unlike the shadow culls these walks have side
// effects on the way down: earlyFail reads back occlusion queries (checkOcclusion, GL), prunes the
// subtree of an occluded group and marks it as an occluder; processGroup (markNotCulled) sets the group
// visible, updates its distance and may queue an occlusion query; the object cache walk adds groups to
// the region's visible list or to the occluder set.
//
// What a walk reaches does not depend on any of that: the frustum tests and the traversal state (mRes)
// only read bounds and the camera, and an occlusion prune never changes which nodes a following sibling
// tests (only an only-child, flagged SKIP_FRUSTUM_CHECK, inherits mRes, and it has no siblings). So with
// FSPerfParallelMainCull on:
//
//  1. prepare, main thread, in region order: rebound each partition root (as cull does) and make a job
//     per region and partition; run LLVOCachePartition::cull with sFSDeferFrontCull set, which does all
//     its bookkeeping (pause, culled time, idle hash, back object selection) and returns 2 where it would
//     walk the front, and give that walk a job with its own camera holding the region frustum planes;
//  2. workers (and this thread): every job walks its octree with the frustum tests only, without any
//     occlusion prune, and records each node it reaches in traversal order, the end of its subtree and
//     whether processGroup would run;
//  3. commit, main thread, in the serial order (region, partition, then the object cache): replay each
//     record exactly as the serial walk would have treated that node: earlyFail (checkOcclusion, then on
//     an occluded group markOccluder and skip its subtree) and processGroup (markNotCulled, or the object
//     cache's visible group / occluder bookkeeping). Every write and every GL call happens here.
//
// The walks parallelise; occlusion reads, query bookkeeping, updateDistance and the object cache side
// effects stay serial. Walks also descend into occluded subtrees the serial cull would have skipped;
// that extra work is on the workers. The probe face culls run with occlusion off and object cache culls
// return early for them, so their commit is only the markNotCulled pass. The octree is not changed while
// the workers run: the main thread is inside the job loop, and no other thread changes octrees during
// display. The partitions of a region are rebounded before any of the region's side effects are
// committed rather than one after the other, which only matters if markNotCulled moved something (it
// does not).
//
// FSPerfParallelMainCullVerify runs the serial partition culls again after the commit, into a separate
// result, and compares the visible and drawable group lists. That second pass sees the occlusion state
// the commit left (queries already read back, groups already marked as occluders); a query whose result
// became available between the two passes can make a rare group differ. The object cache walks are not
// compared.

namespace
{
    // a node a walk reached, in the serial traversal order
    struct FSCullNode
    {
        LLViewerOctreeGroup*    mGroup;
        U32                     mEnd;       // index after this node's subtree
        bool                    mProcess;   // processGroup would run for it
    };

    // LLViewerOctreeCull::traverse without earlyFail, recording every node it reaches
    class FSOctreeRecordCull : public LLViewerOctreeCull
    {
    public:
        FSOctreeRecordCull(LLCamera* camera, std::vector<FSCullNode>& out)
            : LLViewerOctreeCull(camera), mOut(out) { }

        virtual void traverse(const OctreeNode* n)
        {
            LLViewerOctreeGroup* group = (LLViewerOctreeGroup*)n->getListener(0);
            const U32 index = (U32)mOut.size();
            mOut.push_back({ group, 0, false });

            if (mRes == 2 ||
                (mRes && group->hasState(LLViewerOctreeGroup::SKIP_FRUSTUM_CHECK)))
            {
                OctreeTraveler::traverse(n);
            }
            else
            {
                mRes = frustumCheck(group);
                if (mRes)
                {
                    OctreeTraveler::traverse(n);
                }
                mRes = 0;
            }

            mOut[index].mEnd = (U32)mOut.size();
        }

        // visit() runs first thing in OctreeTraveler::traverse, right after the push above
        virtual void processGroup(LLViewerOctreeGroup* group)
        {
            mOut.back().mProcess = true;
        }

    protected:
        std::vector<FSCullNode>& mOut;
    };

    // the frustum tests of LLOctreeCull (far clip) and LLOctreeCullNoFarClip
    class FSOctreeRecordCullSpatial : public FSOctreeRecordCull
    {
    public:
        FSOctreeRecordCullSpatial(LLCamera* camera, bool far_clip, std::vector<FSCullNode>& out)
            : FSOctreeRecordCull(camera, out), mFarClip(far_clip) { }

        virtual S32 frustumCheck(const LLViewerOctreeGroup* group)
        {
            S32 res = AABBInFrustumNoFarClipGroupBounds(group);
            if (mFarClip && res != 0)
            {
                res = llmin(res, AABBSphereIntersectGroupExtents(group));
            }
            return res;
        }

        virtual S32 frustumCheckObjects(const LLViewerOctreeGroup* group)
        {
            S32 res = AABBInFrustumNoFarClipObjectBounds(group);
            if (mFarClip && res != 0)
            {
                res = llmin(res, AABBSphereIntersectObjectExtents(group));
            }
            return res;
        }

    private:
        bool mFarClip;
    };

    // the frustum tests of LLVOCacheOctreeCull (llvocache.cpp)
    class FSOctreeRecordCullVOCache : public FSOctreeRecordCull
    {
    public:
        FSOctreeRecordCullVOCache(LLCamera* camera, const LLVector3& shift, F32 pixel_threshold, F32 near_radius,
                                  std::vector<FSCullNode>& out)
            : FSOctreeRecordCull(camera, out), mLocalShift(shift), mPixelThreshold(pixel_threshold), mNearRadius(near_radius) { }

        virtual S32 frustumCheck(const LLViewerOctreeGroup* group)
        {
            S32 res = AABBInRegionFrustumNoFarClipGroupBounds(group);
            if (res != 0)
            {
                res = llmin(res, AABBRegionSphereIntersectGroupExtents(group, mLocalShift));
            }
            return res;
        }

        virtual S32 frustumCheckObjects(const LLViewerOctreeGroup* group)
        {
            S32 res = AABBInRegionFrustumNoFarClipObjectBounds(group);
            if (res != 0)
            {
                res = llmin(res, AABBRegionSphereIntersectObjectExtents(group, mLocalShift));
            }
            if (res != 0)
            {
                const LLVector4a* exts = group->getObjectExtents();
                res = checkProjectionArea(exts[0], exts[1], mLocalShift, mPixelThreshold, mNearRadius);
            }
            return res;
        }

    private:
        LLVector3   mLocalShift;
        F32         mPixelThreshold;
        F32         mNearRadius;
    };

    struct FSMainCullJob
    {
        LLViewerRegion*         mRegion = nullptr;
        LLSpatialPartition*     mPartition = nullptr;   // a spatial partition walk, or
        LLVOCachePartition*     mVOPartition = nullptr; // an object cache front walk
        bool                    mFarClip = true;
        bool                    mVOOcclusion = false;
        F32                     mPixelThreshold = 0.f;
        F32                     mNearRadius = 0.f;
        LLVector3               mShift;
        LLCamera                mCamera;                // object cache walks: the camera with this region's planes
        std::vector<FSCullNode> mNodes;
    };

    std::vector<FSMainCullJob>  sMainJobs;
    std::vector<U32>            sMainJobOrder;
    LLCullResult                sMainVerifyResult;

    U32 sMainVerifyChecks = 0;
    U32 sMainVerifyMismatches = 0;
    F64 sMainVerifyLastLog = 0.0;
    U32 sMainVerifyWarnings = 0;

    // updateCull's partition filter for a world camera or probe face (not the HUD)
    bool fs_main_cull_partition(LLPipeline& pipeline, LLSpatialPartition* part, U32 i, bool avatars_only)
    {
        if (avatars_only &&
            i != LLViewerRegion::PARTITION_BRIDGE &&
            i != LLViewerRegion::PARTITION_AVATAR &&
            i != LLViewerRegion::PARTITION_CONTROL_AV)
        {
            return false;
        }
        return part && (LLViewerRegion::PARTITION_BRIDGE == i || pipeline.hasRenderType(part->mDrawableType));
    }
}

// updateCull, in place of its region loop. False when the serial loop should run.
bool LLPipeline::fsParallelMainCull(LLCamera& camera, LLCullResult& result, bool hud_attachments, bool avatars_only)
{
    static LLCachedControl<bool> parallel_cull(gSavedSettings, "FSPerfParallelMainCull", false);
    // shadow cameras (projectors, the union cull) use LLOctreeCullShadow and have their own parallel path
    if (!parallel_cull || hud_attachments || sShadowRender)
    {
        return false;
    }
    LL_PROFILE_ZONE_SCOPED_CATEGORY_PIPELINE;

    static LLCachedControl<bool> verify(gSavedSettings, "FSPerfParallelMainCullVerify", false);
    static LLCachedControl<bool> use_object_cache_occlusion(gSavedSettings, "UseObjectCacheOcclusion");

    const LLWorld::region_list_t& regions = LLWorld::getInstance()->getRegionList();
    // as updateCull passes it to LLVOCachePartition::cull (<FS:Beq> area search)
    const bool vo_do_occlusion = sUseOcclusion > 0 && !gAgent.getFSAreaSearchActive();
    // LLSpatialPartition::cull's choice of culler
    const bool no_far_clip = !sUseFarClip && !gCubeSnapshot;

    // 1. main thread: rebounds, the object cache bookkeeping and the job list, in the serial order
    U32 job_count = 0;
    LLViewerRegion* last_front_region = nullptr;
    {
        LL_PROFILE_ZONE_NAMED_CATEGORY_PIPELINE("fs main cull prepare");
        for (LLViewerRegion* region : regions)
        {
            for (U32 i = 0; i < LLViewerRegion::NUM_PARTITIONS; i++)
            {
                LLSpatialPartition* part = region->getSpatialPartition(i);
                if (!fs_main_cull_partition(*this, part, i, avatars_only))
                {
                    continue;
                }
                ((LLSpatialGroup*)part->mOctree->getListener(0))->rebound();

                if (job_count >= sMainJobs.size())
                {
                    sMainJobs.emplace_back();
                }
                FSMainCullJob& job = sMainJobs[job_count++];
                job.mRegion = region;
                job.mPartition = part;
                job.mVOPartition = nullptr;
                job.mFarClip = !(part->mInfiniteFarClip || no_far_clip);
                job.mNodes.clear();
            }

            LLVOCachePartition* vo_part = region->getVOCachePartition();
            if (vo_part)
            {
                LLVOCachePartition::sFSDeferFrontCull = true;
                const S32 res = vo_part->cull(camera, vo_do_occlusion);
                LLVOCachePartition::sFSDeferFrontCull = false;
                if (res == 2)
                {   // the rest of LLVOCachePartition::cull: the front walk
                    if (job_count >= sMainJobs.size())
                    {
                        sMainJobs.emplace_back();
                    }
                    FSMainCullJob& job = sMainJobs[job_count++];
                    job.mRegion = region;
                    job.mPartition = nullptr;
                    job.mVOPartition = vo_part;
                    job.mShift = region->getOriginAgent();
                    job.mCamera = camera;
                    job.mCamera.calcRegionFrustumPlanes(job.mShift, gAgentCamera.mDrawDistance);
                    job.mVOOcclusion = vo_do_occlusion && use_object_cache_occlusion;
                    job.mPixelThreshold = LLVOCacheEntry::getSquaredPixelThreshold(true);
                    job.mNearRadius = LLVOCacheEntry::sNearRadius;
                    job.mNodes.clear();
                    last_front_region = region;
                }
            }
        }

        // the big walks first
        sMainJobOrder.resize(job_count);
        for (U32 i = 0; i < job_count; ++i)
        {
            sMainJobOrder[i] = i;
        }
        std::stable_sort(sMainJobOrder.begin(), sMainJobOrder.end(), [](U32 a, U32 b)
        {
            auto heavy = [](const FSMainCullJob& job)
            {
                if (job.mVOPartition)
                {
                    return true;
                }
                const U32 type = job.mPartition->mPartitionType;
                return type == LLViewerRegion::PARTITION_VOLUME || type == LLViewerRegion::PARTITION_BRIDGE;
            };
            return heavy(sMainJobs[a]) && !heavy(sMainJobs[b]);
        });
    }

    // 2. worker threads (and this one): the walks, read only
    {
        LL_PROFILE_ZONE_NAMED_CATEGORY_PIPELINE("fs main cull parallel");
        LLCamera* cam = &camera;
        FSParallelCull::run(job_count, [cam](U32 index)
        {
            LL_PROFILE_ZONE_NAMED_CATEGORY_PIPELINE("fs main cull job");
            FSMainCullJob& job = sMainJobs[sMainJobOrder[index]];
            if (job.mVOPartition)
            {
                FSOctreeRecordCullVOCache culler(&job.mCamera, job.mShift, job.mPixelThreshold, job.mNearRadius, job.mNodes);
                culler.traverse(job.mVOPartition->mOctree);
            }
            else
            {
                FSOctreeRecordCullSpatial culler(cam, job.mFarClip, job.mNodes);
                culler.traverse(job.mPartition->mOctree);
            }
        });
    }

    // 3. main thread: the serial walks' side effects, in their order
    {
        LL_PROFILE_ZONE_NAMED_CATEGORY_PIPELINE("fs main cull commit");
        // LLOctreeCull::earlyFail does nothing in reflection renders
        const bool check_occlusion = !sReflectionRender;
        const U32 any_query_state = LLOcclusionCullingGroup::QUERY_PENDING | LLOcclusionCullingGroup::DISCARD_QUERY;

        for (U32 j = 0; j < job_count; ++j)
        {
            FSMainCullJob& job = sMainJobs[j];
            const std::vector<FSCullNode>& nodes = job.mNodes;
            const U32 count = (U32)nodes.size();

            if (job.mPartition)
            {   // LLOctreeCull::earlyFail and processGroup
                for (U32 i = 0; i < count; )
                {
                    LLSpatialGroup* group = (LLSpatialGroup*)nodes[i].mGroup;
                    if (check_occlusion)
                    {
                        // checkOcclusion changes nothing on a group with neither a pending nor a discarded query
                        if (group->isOcclusionState(any_query_state))
                        {
                            group->checkOcclusion();
                        }
                        if (group->getOctreeNode() &&
                            group->getOctreeNode()->getParent() &&
                            sUseOcclusion &&
                            group->isOcclusionState(LLSpatialGroup::OCCLUDED))
                        {
                            markOccluder(group);
                            i = nodes[i].mEnd;
                            continue;
                        }
                    }
                    if (nodes[i].mProcess)
                    {
                        markNotCulled(group, camera);
                    }
                    ++i;
                }
            }
            else
            {   // LLVOCacheOctreeCull::earlyFail and processGroup
                LLVOCachePartition* vo_part = job.mVOPartition;
                LLViewerRegion* region = job.mRegion;
                for (U32 i = 0; i < count; )
                {
                    LLOcclusionCullingGroup* group = (LLOcclusionCullingGroup*)nodes[i].mGroup;
                    const bool use_occlusion = job.mVOOcclusion && group->getOctreeNode()->getParent();
                    if (use_occlusion && !group->needsUpdate())
                    {
                        group->checkOcclusion();
                        if (group->isOcclusionState(LLOcclusionCullingGroup::OCCLUDED))
                        {
                            i = nodes[i].mEnd;
                            continue;
                        }
                    }
                    if (nodes[i].mProcess)
                    {
                        if (!use_occlusion)
                        {
                            if (region->addVisibleGroup(group))
                            {
                                group->setVisible();
                            }
                        }
                        else if (group->needsUpdate() || !group->isRecentlyVisible())
                        {
                            vo_part->addOccluders(group);
                            group->setVisible();
                        }
                        else if (group->isOcclusionState(LLOcclusionCullingGroup::QUERY_PENDING) ||
                                 group->isOcclusionState(LLOcclusionCullingGroup::ACTIVE_OCCLUSION))
                        {
                            group->setVisible();
                        }
                        else if (region->addVisibleGroup(group))
                        {
                            group->setVisible();
                        }
                    }
                    ++i;
                }
                if (!LLVOCachePartition::sNeedsOcclusionCheck)
                {
                    LLVOCachePartition::sNeedsOcclusionCheck = vo_part->fsHasOccluders();
                }
            }
            job.mNodes.clear();
        }

        // the serial object cache culls leave the camera with the last walked region's planes
        if (last_front_region)
        {
            camera.calcRegionFrustumPlanes(last_front_region->getOriginAgent(), gAgentCamera.mDrawDistance);
        }
    }

    if (verify)
    {   // the serial partition culls again, into their own result
        LL_PROFILE_ZONE_NAMED_CATEGORY_PIPELINE("fs main cull verify");
        const S32 saved_nodes = mNumVisibleNodes;
        grabReferences(sMainVerifyResult);
        sMainVerifyResult.clear();
        for (LLViewerRegion* region : regions)
        {
            for (U32 i = 0; i < LLViewerRegion::NUM_PARTITIONS; i++)
            {
                LLSpatialPartition* part = region->getSpatialPartition(i);
                if (fs_main_cull_partition(*this, part, i, avatars_only))
                {
                    part->cull(camera);
                }
            }
        }
        mNumVisibleNodes = saved_nodes;
        grabReferences(result);

        std::vector<LLSpatialGroup*> par_visible, par_drawable, ser_visible, ser_drawable;
        fs_list_groups(result, par_visible, par_drawable);
        fs_list_groups(sMainVerifyResult, ser_visible, ser_drawable);
        sMainVerifyResult.clear();
        ++sMainVerifyChecks;
        if (par_visible != ser_visible || par_drawable != ser_drawable)
        {
            ++sMainVerifyMismatches;
            if (sMainVerifyWarnings < 20)
            {
                ++sMainVerifyWarnings;
                LL_WARNS("FSParallelCull") << (gCubeSnapshot ? "Probe face" : "Main camera") << " cull differs from the serial cull:"
                    << " by group " << par_visible.size() << " vs " << ser_visible.size()
                    << " (missing " << fs_count_missing(ser_visible, par_visible)
                    << ", extra " << fs_count_missing(par_visible, ser_visible) << "),"
                    << " by drawable " << par_drawable.size() << " vs " << ser_drawable.size()
                    << " (missing " << fs_count_missing(ser_drawable, par_drawable)
                    << ", extra " << fs_count_missing(par_drawable, ser_drawable) << ")"
                    << (sMainVerifyWarnings == 20 ? "; further differences only in the summary" : "") << LL_ENDL;
            }
        }

        const F64 now = LLTimer::getTotalSeconds();
        if (now - sMainVerifyLastLog > 10.0)
        {
            sMainVerifyLastLog = now;
            LL_INFOS("FSParallelCull") << "Main/probe cull verify: " << sMainVerifyChecks << " culls checked, "
                << sMainVerifyMismatches << " differed from the serial cull ("
                << FSParallelCull::workerCount() << " workers)" << LL_ENDL;
            sMainVerifyChecks = 0;
            sMainVerifyMismatches = 0;
        }
    }

    return true;
}
