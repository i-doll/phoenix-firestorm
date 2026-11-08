/**
 * @file fsparallelavatar.cpp
 * @brief Evaluates the avatar poses of a frame on worker threads
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

// What may run on a worker thread (LLCharacter::evaluateParallelMotionUpdate() and the joint
// world matrices of one avatar) and why:
// - Only motions that return true from LLMotion::canUpdateOffMainThread() (keyframe and its
//   walk/stand/fall/poser subclasses, head/eye, hand, targeting, walk/fly adjust, emote,
//   avatar physics, null) are evaluated off the main thread. An avatar with any other active
//   motion (editing, unknown future motions) is evaluated on the main thread, before the
//   workers start. The self avatar and UI avatars are never queued.
// - Those motions write only their own avatar: joints, joint states, pose blenders, visual
//   param weights and animation data. Shared data they read (keyframe curves in
//   LLKeyframeDataCache, constraint shared data, land heights, the agent's region origin,
//   object velocities, settings) is not written by anyone while the workers run, because the
//   main thread only helps with the same tasks until all of them are done.
// - LLVOAvatar::updateVisualParams() (blinks, hand poses, emotes, physics) is deferred to the
//   main thread, it applies morphs, may restart the sit motion and change the skeleton.
// - Motion deactivations (onDeactivate(), deactivate callbacks, deleting deprecated motions)
//   are deferred to the main thread.
// - LLSmoothInterpolation's interpolant cache is frozen while the workers run (cache misses
//   are computed without growing the shared vector).
// - LLJoint world matrix serials come from per thread blocks of an atomic counter, so the rigged
//   palette cache never sees a stamp twice.
// - LLPhysicsMotion's cached control is created on the main thread and its default table is
//   only looked up, never inserted into.
// - Random numbers (eye jitter, blinks) use the thread local generator of llrand.
// - LLJoint::sNumTouches/sNumUpdates are thread local debug counters; the joint debug option
//   shows the main thread's.

#include "llviewerprecompiledheaders.h"

#include "fsparallelavatar.h"

#include "llapp.h"
#include "llcriticaldamp.h"
#include "lljoint.h"
#include "llcontrolavatar.h"
#include "llviewercontrol.h"
#include "llvoavatar.h"

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <mutex>
#include <thread>
#include <unordered_set>
#include <vector>
#include <emmintrin.h>

bool FSParallelAvatar::sCollecting = false;

namespace
{
    struct PendingPose
    {
        LLPointer<LLVOAvatar> mAvatar;
        bool mAlive = true;
        bool mOffMain = false;
        U64 mFingerprint = 0;
    };

    std::vector<PendingPose> sPending;

    struct Batch
    {
        std::vector<U32> mIndices;
        bool mVerify = false;
    };

    Batch sBatch;

    // A small fixed pool. The main thread hands out a batch of tasks, works on them itself and
    // waits only for tasks already taken by a worker, so the join is bounded by one task.
    class FSPosePool
    {
    public:
        typedef void (*task_fn)(void* context, U32 index);

        ~FSPosePool()
        {
            {
                std::lock_guard<std::mutex> lock(mMutex);
                mQuit = true;
            }
            mWake.notify_all();
            for (std::thread& thread : mThreads)
            {
                if (thread.joinable())
                {
                    thread.join();
                }
            }
        }

        U32 getThreadCount()
        {
            start();
            return (U32)mThreads.size();
        }

        void run(task_fn fn, void* context, U32 count)
        {
            start();
            if (mThreads.empty() || count < 2)
            {
                for (U32 i = 0; i < count; ++i)
                {
                    fn(context, i);
                }
                return;
            }

            {
                std::lock_guard<std::mutex> lock(mMutex);
                mFn = fn;
                mContext = context;
                mCount = count;
                mNext.store(0, std::memory_order_relaxed);
                mDone.store(0, std::memory_order_relaxed);
                mOpen = true;
                ++mGeneration;
            }
            mWake.notify_all();

            work();

            {
                LL_PROFILE_ZONE_NAMED_CATEGORY_AVATAR("FSParallelAvatar wait");
                // spin briefly (the tasks in flight are usually short), then block until the last one signals
                for (U32 i = 0; i < 1024 && mDone.load(std::memory_order_acquire) < count; ++i)
                {
                    _mm_pause();
                }
                if (mDone.load(std::memory_order_acquire) < count)
                {
                    std::unique_lock<std::mutex> lock(mMutex);
                    mIdle.wait(lock, [this, count] { return mDone.load(std::memory_order_acquire) >= count; });
                }
            }

            std::unique_lock<std::mutex> lock(mMutex);
            mOpen = false;
            mIdle.wait(lock, [this] { return mActive == 0; });
        }

    private:
        void start()
        {
            if (mStarted)
            {
                return;
            }
            mStarted = true;
            U32 hardware = std::thread::hardware_concurrency();
            // Leave the main thread and one more core (the render and other worker threads) alone
            U32 count = hardware >= 4 ? llmin(hardware - 2, 6u) : 0u;
            for (U32 i = 0; i < count; ++i)
            {
                mThreads.emplace_back([this] { threadLoop(); });
            }
            LL_INFOS("FSParallelAvatar") << "Avatar pose threads: " << count << LL_ENDL;
        }

        void work()
        {
            U32 index;
            while ((index = mNext.fetch_add(1, std::memory_order_relaxed)) < mCount)
            {
                mFn(mContext, index);
                if (mDone.fetch_add(1, std::memory_order_acq_rel) + 1 == mCount)
                {   // the mutex orders this with a waiter between its test and its wait (callers don't hold it)
                    {
                        std::lock_guard<std::mutex> lock(mMutex);
                    }
                    mIdle.notify_all();
                }
            }
        }

        void threadLoop()
        {
            LL_PROFILER_SET_THREAD_NAME("FSAvatarPose");
            U64 seen = 0;
            std::unique_lock<std::mutex> lock(mMutex);
            while (true)
            {
                mWake.wait(lock, [this, &seen] { return mQuit || (mOpen && mGeneration != seen); });
                if (mQuit)
                {
                    return;
                }
                seen = mGeneration;
                ++mActive;
                lock.unlock();
                work();
                lock.lock();
                if (--mActive == 0)
                {
                    mIdle.notify_all();
                }
            }
        }

        std::vector<std::thread> mThreads;
        std::mutex mMutex;
        std::condition_variable mWake;
        std::condition_variable mIdle;
        task_fn mFn = nullptr;
        void* mContext = nullptr;
        U32 mCount = 0;
        std::atomic<U32> mNext{ 0 };
        std::atomic<U32> mDone{ 0 };
        U64 mGeneration = 0;
        U32 mActive = 0;
        bool mOpen = false;
        bool mQuit = false;
        bool mStarted = false;
    };

    FSPosePool sPool;

    U32 sVerifyLogs = 0;

    bool verify_log_allowed()
    {
        bool allowed = sVerifyLogs < 20 || (sVerifyLogs % 1000) == 0;
        ++sVerifyLogs;
        return allowed;
    }

    inline void hash_bytes(U64& hash, const void* data, size_t size)
    {
        const U8* bytes = (const U8*)data;
        for (size_t i = 0; i < size; ++i)
        {
            hash ^= bytes[i];
            hash *= 0x100000001b3ULL;
        }
    }

    void hash_joint(U64& hash, LLJoint* joint)
    {
        hash_bytes(hash, joint->getPosition().mV, sizeof(F32) * 3);
        hash_bytes(hash, joint->getRotation().mQ, sizeof(F32) * 4);
        hash_bytes(hash, joint->getScale().mV, sizeof(F32) * 3);
        hash_bytes(hash, joint->getXform()->getWorldMatrix().mMatrix, sizeof(F32) * 16);
        for (LLJoint* child : joint->mChildren)
        {
            hash_joint(hash, child);
        }
    }

    struct SavedMatrix
    {
        LLJoint* mJoint;
        LLMatrix4 mMatrix;
    };

    void save_and_dirty(std::vector<SavedMatrix>& saved, LLJoint* joint)
    {
        // Mirrors LLJoint::updateWorldMatrixChildren(): subtrees it skips are not compared
        if (!joint->mUpdateXform)
        {
            return;
        }
        saved.push_back({ joint, joint->getXform()->getWorldMatrix() });
        joint->mDirtyFlags |= LLJoint::MATRIX_DIRTY;
        for (LLJoint* child : joint->mChildren)
        {
            save_and_dirty(saved, child);
        }
    }
}

//static
void FSParallelAvatar::beginIdle()
{
    static LLCachedControl<bool> enabled(gSavedSettings, "FSPerfParallelAvatarUpdate", false);
    sCollecting = enabled && !LLApp::isExiting();
}

//static
bool FSParallelAvatar::queuePose(LLVOAvatar* avatar, bool was_sit_ground_constrained)
{
    if (sCollecting && avatar && avatar->mFSPosePending)
    {   // updateCharacter ran again this idle loop: the pose queued the first time is evaluated in endIdle,
        // don't also run the full serial motion update now
        return true;
    }
    if (!sCollecting || !avatar || avatar->isSelf() || avatar->isUIAvatar() || avatar->isDead())
    {
        return false;
    }
    // Clock, purge and loading motions on the main thread, as updateMotions(NORMAL_UPDATE) does
    if (!avatar->prepareParallelMotionUpdate())
    {
        return false;
    }
    avatar->mFSPosePending = true;
    avatar->mFSPoseWasSitGround = was_sit_ground_constrained;
    PendingPose pending;
    pending.mAvatar = avatar;
    sPending.push_back(pending);
    return true;
}

// Any thread. The part of LLVOAvatar::updateCharacter() after "update animations" that only
// touches this avatar; keep in sync with it.
//static
void FSParallelAvatar::evaluatePose(LLVOAvatar* avatar)
{
    LL_PROFILE_ZONE_SCOPED_CATEGORY_AVATAR;
    avatar->evaluateParallelMotionUpdate();

    // Special handling for sitting on ground.
    if (!avatar->getParent() && (avatar->isSitting() || avatar->mFSPoseWasSitGround))
    {
        F32 off_z = (F32)LLVector3d(avatar->getHoverOffset()).mdV[VZ];
        if (off_z != 0.0f)
        {
            LLVector3 pos = avatar->mRoot->getWorldPosition();
            pos.mV[VZ] += off_z;
            avatar->mRoot->touch();
            // SL-315
            avatar->mRoot->setWorldPosition(pos);
        }
    }

    // Update child joints as needed.
    avatar->mRoot->updateWorldMatrixChildren();
}

// Main thread. The rest of LLVOAvatar::updateCharacter(); keep in sync with it.
//static
void FSParallelAvatar::finishPose(LLVOAvatar* avatar)
{
    LL_PROFILE_ZONE_SCOPED_CATEGORY_AVATAR;
    // Deferred deactivations and visual param updates. They can move joints (poser reverting
    // joints, skeleton changes), so bring the matrices up to date again.
    if (avatar->finishParallelMotionUpdate())
    {
        avatar->mRoot->updateWorldMatrixChildren();
    }

    // update head position
    avatar->updateHeadOffset();

    // Generate footstep sounds when feet hit the ground
    avatar->updateFootstepSounds();

    // Only visible avatars are queued. System avatar mesh vertices need to be reskinned.
    avatar->mNeedsSkin = true;
}

//static
unsigned long long FSParallelAvatar::poseFingerprint(LLVOAvatar* avatar)
{
    U64 hash = 0xcbf29ce484222325ULL;
    if (avatar->mRoot)
    {
        hash_joint(hash, avatar->mRoot);
    }
    return hash;
}

// Main thread. Recomputes the avatar's joint world matrices serially from the local transforms
// the worker left and returns how many differ from what the worker computed.
//static
int FSParallelAvatar::verifyWorldMatrices(LLVOAvatar* avatar)
{
    static std::vector<SavedMatrix> saved;
    saved.clear();
    if (!avatar->mRoot)
    {
        return 0;
    }
    save_and_dirty(saved, avatar->mRoot);
    avatar->mRoot->updateWorldMatrixChildren();

    const F32 EPSILON = 1.e-4f;
    int mismatches = 0;
    for (const SavedMatrix& entry : saved)
    {
        const LLMatrix4& now = entry.mJoint->getXform()->getWorldMatrix();
        bool same = true;
        for (U32 r = 0; r < 4 && same; ++r)
        {
            for (U32 c = 0; c < 4; ++c)
            {
                if (fabsf(now.mMatrix[r][c] - entry.mMatrix.mMatrix[r][c]) > EPSILON)
                {
                    same = false;
                    break;
                }
            }
        }
        if (!same)
        {
            if (mismatches == 0 && verify_log_allowed())
            {
                LL_WARNS("FSParallelAvatar") << "Verify: joint " << entry.mJoint->getName() << " of " << avatar->getID()
                    << " world matrix differs from the serial recompute, worker translation "
                    << LLVector3(entry.mMatrix.mMatrix[3]) << " serial " << LLVector3(now.mMatrix[3]) << LL_ENDL;
            }
            ++mismatches;
        }
    }
    return mismatches;
}

//static
void FSParallelAvatar::runTask(void* context, unsigned int index)
{
    Batch* batch = (Batch*)context;
    PendingPose& pending = sPending[batch->mIndices[index]];
    evaluatePose(pending.mAvatar.get());
    if (batch->mVerify)
    {
        pending.mFingerprint = poseFingerprint(pending.mAvatar.get());
    }
}

//static
void FSParallelAvatar::endIdle()
{
    if (!sCollecting)
    {
        return;
    }
    sCollecting = false;
    if (sPending.empty())
    {
        return;
    }

    LL_PROFILE_ZONE_SCOPED_CATEGORY_AVATAR;
    static LLCachedControl<bool> verify(gSavedSettings, "FSPerfParallelAvatarUpdateVerify", false);

    sBatch.mIndices.clear();
    sBatch.mVerify = verify;

    // Motions may have been started or stopped by later objects in the idle loop, decide now
    for (U32 i = 0; i < (U32)sPending.size(); ++i)
    {
        PendingPose& pending = sPending[i];
        LLVOAvatar* avatar = pending.mAvatar.get();
        pending.mAlive = !avatar->isDead() && avatar->mRoot;
        if (!pending.mAlive)
        {
            continue;
        }
        pending.mOffMain = avatar->getMotionController().canUpdateOffMainThread();
        if (pending.mOffMain)
        {
            sBatch.mIndices.push_back(i);
        }
    }

    // Avatars with a motion that must stay on the main thread, before any worker starts
    {
        LL_PROFILE_ZONE_NAMED_CATEGORY_AVATAR("FSParallelAvatar serial");
        for (PendingPose& pending : sPending)
        {
            if (pending.mAlive && !pending.mOffMain)
            {
                evaluatePose(pending.mAvatar.get());
            }
        }
    }

    {
        LL_PROFILE_ZONE_NAMED_CATEGORY_AVATAR("FSParallelAvatar parallel");
        LLSmoothInterpolation::setCacheFrozen(true);
        sPool.run(&FSParallelAvatar::runTask, &sBatch, (U32)sBatch.mIndices.size());
        LLSmoothInterpolation::setCacheFrozen(false);
    }

    if (sBatch.mVerify)
    {
        LL_PROFILE_ZONE_NAMED_CATEGORY_AVATAR("FSParallelAvatar verify");
        U32 changed = 0;
        U32 matrix_mismatch = 0;
        for (U32 index : sBatch.mIndices)
        {
            PendingPose& pending = sPending[index];
            LLVOAvatar* avatar = pending.mAvatar.get();
            // Another task wrote this avatar's joints after its own evaluation ended
            if (poseFingerprint(avatar) != pending.mFingerprint)
            {
                ++changed;
                if (verify_log_allowed())
                {
                    LL_WARNS("FSParallelAvatar") << "Verify: joints of " << avatar->getID()
                        << " changed after their evaluation finished" << LL_ENDL;
                }
            }
            if (verifyWorldMatrices(avatar) > 0)
            {
                ++matrix_mismatch;
            }
        }
        LL_DEBUGS("FSParallelAvatar") << "Verify: " << sPending.size() << " queued, " << sBatch.mIndices.size()
            << " on workers (" << sPool.getThreadCount() << " threads), " << changed << " changed afterwards, "
            << matrix_mismatch << " world matrix mismatches" << LL_ENDL;
    }

    // Main thread part, in the original avatar order
    bool any_animesh = false;
    {
        LL_PROFILE_ZONE_NAMED_CATEGORY_AVATAR("FSParallelAvatar finish");
        for (PendingPose& pending : sPending)
        {
            pending.mAvatar->mFSPosePending = false;
            if (pending.mAlive)
            {
                finishPose(pending.mAvatar.get());
                any_animesh |= pending.mAvatar->isControlAvatar();
            }
        }
    }

    // Animesh attachments matched their root to the wearer's attachment point in updateCharacter, before
    // the wearer's queued pose was evaluated (a frame behind); match them again now that it is
    if (any_animesh)
    {
        LL_PROFILE_ZONE_NAMED_CATEGORY_AVATAR("FSParallelAvatar animesh rematch");
        std::unordered_set<const LLVOAvatar*> queued;
        for (const PendingPose& pending : sPending)
        {
            if (pending.mAlive)
            {
                queued.insert(pending.mAvatar.get());
            }
        }
        for (PendingPose& pending : sPending)
        {
            LLVOAvatar* avatar = pending.mAvatar.get();
            if (!pending.mAlive || !avatar->isControlAvatar() || avatar->isDead())
            {
                continue;
            }
            LLControlAvatar* cav = static_cast<LLControlAvatar*>(avatar);
            const LLVOAvatar* wearer = cav->getAttachedAvatar();
            if (wearer && wearer != avatar && queued.count(wearer))
            {
                cav->matchVolumeTransform();
                avatar->mRoot->updateWorldMatrixChildren();
            }
        }
    }

    sPending.clear();
}
