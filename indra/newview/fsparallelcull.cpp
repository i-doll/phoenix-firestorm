/**
 * @file fsparallelcull.cpp
 * @brief Sun shadow cascades culled on worker threads (FSPerfParallelShadowCull)
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

#include "llviewerprecompiledheaders.h"

#include "fsparallelcull.h"

#include "pipeline.h"

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
                    LL_WARNS("FSParallelCull") << "Could not start a shadow cull worker: " << e.what() << LL_ENDL;
                    break;
                }
            }
            LL_INFOS("FSParallelCull") << "Shadow cull workers: " << mThreads.size() << LL_ENDL;
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
                LL_PROFILE_ZONE_NAMED_CATEGORY_PIPELINE("fs shadow cull wait");
                std::unique_lock<std::mutex> lock(mMutex);
                mDone.wait(lock, [&batch]() { return batch.mUsers == 0; });
                mBatch = nullptr;
            }
        }

    private:
        void workerLoop(U32 index)
        {
            std::string name = "FS Shadow Cull " + std::to_string(index);
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
