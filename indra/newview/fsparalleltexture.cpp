/**
 * @file fsparalleltexture.cpp
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

// The regular sweep of LLViewerTextureList::updateImagesFetchTextures visits 5% of the textures
// per frame (more under memory pressure) and calls updateImageDecodePriority for each. Most of its
// cost is the face loop: LLFace::calcPixelArea for faces not updated in the last 10 frames, then a
// biased virtual size per face reduced to a maximum. With FSPerfParallelTexturePriority on and a
// sweep of at least MIN_PARALLEL_TEXTURES eligible textures, prepare() does that loop up front:
//
//  1. (workers) for every eligible texture, collect the faces the serial loop would visit (same
//     1024 face cap per texture) whose pixel area is stale (gFrameCount - mLastTextureUpdate > 10).
//     Read only.
//  2. (main) merge the lists in sweep order and keep each face once, stamping mLastTextureUpdate as
//     the serial loop does. Faces are shared between textures (diffuse, normal and specular maps of
//     one face), so this is what makes every face belong to exactly one thread below.
//  3. (workers) calcPixelArea for the stale non-rigged faces, and mInFrustum. It writes only the
//     face's own cached fields and reads the camera (an LLSimpleton, no lock), the drawable's
//     transform, the face extents and, for particles, the spatial group bounds. ll_frand is
//     thread-local. Rigged faces stay on the main thread, which does them while the workers run:
//     their calcPixelArea may update the rigging info of an LLVolumeFace, and mesh volumes are
//     shared between objects.
//  4. (workers) the per-texture reduction, a copy of the face loop of updateImageDecodePriority
//     without the calcPixelArea call (every face it visits is fresh after 3), into a results array.
//     Settings and static state (sDesiredDiscardBias, sMaxVirtualSize, TextureScale*AreaFactor,
//     TextureCameraBoost) are read once on the main thread and passed in.
//
// Nothing here copies an LLPointer, touches GL, the fetcher or list membership: the workers get raw
// pointers and the sweep's entries vector holds the references. The main thread is inside run() for
// the whole parallel part, so no texture, face list, face or drawable changes meanwhile.
//
// The sweep then applies the results in its original order and time budget: updateImageDecodePriority
// takes the result instead of running its face loop and does the rest (max virtual size reset,
// updateSceneTextureStats, spot lights, lazy flush, processTextureStats) as before, then updateFetch.
//
// Differences from the serial sweep: faces of textures the serial loop would have stopped early on
// (max virtual size reached, or the time budget ran out before reaching the texture) get their pixel
// area refreshed too. That is a fresher value of the same cache, never an older one.
//
// FSPerfParallelTexturePriorityVerify: the sweep runs the original serial face loop (and applies its
// result) for every texture that has a parallel result and compares the two; step 3 is checked by
// recomputing a few faces per frame on the main thread.

#include "llviewerprecompiledheaders.h"

#include "fsparalleltexture.h"

#include "llappviewer.h"
#include "llface.h"
#include "llimagegl.h"
#include "llrender.h"
#include "lltextureentry.h"
#include "llviewercontrol.h"
#include "llviewerobject.h"
#include "llviewertexture.h"
#include "llviewertextureanim.h"
#include "llviewertexturelist.h"
#include "llvovolume.h"

#include <atomic>
#include <condition_variable>
#include <functional>
#include <mutex>
#include <thread>
#include <xmmintrin.h>
#include <emmintrin.h>

extern bool gCubeSnapshot;

namespace
{
    constexpr U32 MIN_PARALLEL_TEXTURES = 128;  // smaller sweeps are not worth waking the workers
    constexpr U32 TEXTURES_PER_CHUNK = 16;
    constexpr U32 FACES_PER_CHUNK = 32;
    constexpr U32 MAX_FACES_TO_CHECK = 1024;    // as in updateImageDecodePriority
    constexpr U32 FACE_UPDATE_FRAMES = 10;      // as in updateImageDecodePriority
    constexpr F32 BIAS_TRS_ON_SCREEN = 1.f;     // as in updateImageDecodePriority
    constexpr F32 PIXEL_AREA_UPDATE_PERIOD = 0.1f; // as in LLFace::calcPixelArea

    //-------------------------------------------------------------------------
    // Worker pool. Persistent, started on first use, never destroyed (detached threads waiting on
    // a condition variable), so nothing has to be torn down on exit. The main thread works through
    // the same job range and then waits only for jobs already taken, so the join is bounded by the
    // longest single chunk.
    //-------------------------------------------------------------------------
    class FSTexturePool
    {
    public:
        static FSTexturePool* instance()
        {
            static FSTexturePool* sPool = new FSTexturePool();
            return sPool;
        }

        U32 getWorkerCount() const { return mWorkerCount; }

        // Runs job(0..count-1) on the workers and the caller. caller_first, if set, runs on the
        // caller after the workers are woken and before it joins in on the jobs.
        void run(U32 count, const std::function<void(U32)>& job, const std::function<void()>* caller_first = nullptr)
        {
            if (count < 2 || mWorkerCount == 0)
            {
                if (caller_first)
                {
                    (*caller_first)();
                }
                for (U32 i = 0; i < count; ++i)
                {
                    job(i);
                }
                return;
            }

            mJob = &job;
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

            if (caller_first)
            {
                (*caller_first)();
            }
            work();

            {
                LL_PROFILE_ZONE_NAMED_CATEGORY_TEXTURE("fs tex priority - join");
                waitUntil([this, count] { return mDone.load() >= count; });
            }

            // Late wakers see mOpen false and leave; wait for any that got in before.
            mOpen.store(false);
            waitUntil([this] { return mActive.load() == 0; });
            mJob = nullptr;
        }

    private:
        FSTexturePool()
        {
            const U32 hw = std::thread::hardware_concurrency();
            // the caller works too; leave cores for the fetch, decode and other viewer threads
            const U32 count = hw > 3 ? llmin(hw - 2, 3U) : 1U;
            for (U32 i = 0; i < count; ++i)
            {
                try
                {
                    std::thread(&FSTexturePool::workerLoop, this).detach();
                    ++mWorkerCount;
                }
                catch (const std::exception& e)
                {
                    LL_WARNS("Texture") << "Could not start a texture priority worker: " << e.what() << LL_ENDL;
                    break;
                }
            }
            LL_INFOS("Texture") << "Texture priority workers: " << mWorkerCount << LL_ENDL;
        }

        void work()
        {
            U32 index;
            while ((index = mNext.fetch_add(1)) < mCount)
            {
                (*mJob)(index);
                if (mDone.fetch_add(1) + 1 == mCount)
                {
                    signalDone();
                }
            }
        }

        // Spins briefly (the in-flight chunks are usually short), then blocks until a worker
        // signals mDoneCV, rather than yielding a core away for the whole wait.
        template <typename Pred> void waitUntil(Pred done)
        {
            for (U32 i = 0; i < 1024; ++i)
            {
                if (done())
                {
                    return;
                }
                _mm_pause();
            }
            std::unique_lock<std::mutex> lock(mDoneMutex);
            mDoneCV.wait(lock, done);
        }

        // after the state waitUntil tests changed: taking the mutex orders this with a waiter
        // between its test and its wait, so the wake isn't lost
        void signalDone()
        {
            {
                std::lock_guard<std::mutex> lock(mDoneMutex);
            }
            mDoneCV.notify_all();
        }

        void workerLoop()
        {
            LL_PROFILER_SET_THREAD_NAME("FS Texture Priority");
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
                if (mActive.fetch_sub(1) == 1)
                {
                    signalDone();
                }
            }
        }

        U32 mWorkerCount = 0;

        std::mutex mMutex;
        std::condition_variable mCV;
        U64 mGeneration = 0;
        std::mutex mDoneMutex;
        std::condition_variable mDoneCV;    // mDone reached mCount, or mActive reached 0

        const std::function<void(U32)>* mJob = nullptr;
        U32 mCount = 0;
        U32 mCSR = 0;
        std::atomic<U32> mNext{ 0 };
        std::atomic<U32> mDone{ 0 };
        std::atomic<S32> mActive{ 0 };
        std::atomic<bool> mOpen{ false };
    };

    // Per-frame state, reused between frames to avoid allocations.
    bool sActive = false;
    std::vector<LLViewerFetchedTexture*> sTextures;      // entries[i], raw (the entries vector holds the references)
    std::vector<U8> sHasResult;                          // entries[i] has a result
    std::vector<FSTexturePriorityFaces> sResults;        // per entry
    std::vector<U32> sEligible;                          // entry indices that get a result
    // a cache line each: workers push_back into neighbouring chunks at the same time
    struct alignas(64) FSChunkFaces
    {
        std::vector<LLFace*> mFaces;
    };
    std::vector<FSChunkFaces> sChunkFaces;               // step 1 output, per texture chunk
    std::vector<LLFace*> sFaces;                         // step 3 input, non-rigged
    std::vector<LLFace*> sRiggedFaces;                   // step 3, main thread

    // Step 3 results kept for the verify mode
    struct FSFaceCheck
    {
        bool mThrottled;    // calcPixelArea returned its cached result
        bool mInFrustum;
        F32  mPixelArea;
        F32  mImportance;
    };
    std::vector<FSFaceCheck> sFaceChecks;

    // Step 1: the stale faces the serial face loop would visit for imagep.
    void collect_stale_faces(LLViewerFetchedTexture* imagep, U32 frame, std::vector<LLFace*>& out)
    {
        U32 face_count = 0;
        for (U32 i = 0; i < LLRender::NUM_TEXTURE_CHANNELS; ++i)
        {
            face_count += imagep->getNumFaces(i);
            const S32 faces_to_check = (face_count > MAX_FACES_TO_CHECK) ? 0 : imagep->getNumFaces(i);
            for (S32 fi = 0; fi < faces_to_check; ++fi)
            {
                LLFace* face = (*(imagep->getFaceList(i)))[fi];
                if (face && face->getViewerObject() && (frame - face->mLastTextureUpdate) > FACE_UPDATE_FRAMES)
                {
                    out.push_back(face);
                }
            }
        }
    }

    bool nearly_equal(F32 a, F32 b)
    {
        return a == b || fabsf(a - b) <= 1e-5f * llmax(fabsf(a), fabsf(b));
    }

    // Verify statistics, logged every 10 seconds while verify is on.
    U32 sVerifiedTextures = 0;
    U32 sTextureMismatches = 0;
    U32 sVerifiedFaces = 0;
    U32 sFaceMismatches = 0;
    U32 sLoggedMismatches = 0;
    constexpr U32 MAX_LOGGED_MISMATCHES = 20;

    void log_verify_summary()
    {
        static LLFrameTimer timer;
        if (timer.getElapsedTimeF32() < 10.f)
        {
            return;
        }
        timer.reset();
        if (sVerifiedTextures || sVerifiedFaces)
        {
            LL_INFOS("Texture") << "FSPerfParallelTexturePriorityVerify (10s): textures=" << sVerifiedTextures
                                << " texture_mismatches=" << sTextureMismatches
                                << " faces=" << sVerifiedFaces
                                << " face_mismatches=" << sFaceMismatches << LL_ENDL;
        }
        sVerifiedTextures = sTextureMismatches = sVerifiedFaces = sFaceMismatches = 0;
    }

}

// Inputs of the face loop read once on the main thread.
struct FSParallelTexture::Params
{
    F32 mDiscardBias;       // LLViewerTexture::sDesiredDiscardBias
    F32 mMaxVirtualSize;    // LLViewerFetchedTexture::sMaxVirtualSize
    F32 mScaleMin;          // TextureScaleMinAreaFactor
    F32 mScaleMax;          // TextureScaleMaxAreaFactor
    F32 mCameraBoost;       // TextureCameraBoost
    bool mSelfFullRes;      // IDSelfAvatarFullResTextures
};

// Step 4: the face loop of LLViewerTextureList::updateImageDecodePriority without the
// calcPixelArea call. Keep in step with it; the verify mode compares the two.
// static
void FSParallelTexture::reduceFaces(LLViewerFetchedTexture* imagep, const Params& p, FSTexturePriorityFaces& out)
{
    F32 max_vsize = 0.f;
    bool on_screen = false;
    U32 face_count = 0;

    // get adjusted bias based on image resolution
    LLImageGL* img = imagep->getGLTexture();
    F32 max_discard = F32(img ? img->getMaxDiscardLevel() : MAX_DISCARD_LEVEL);
    F32 bias = llclamp(max_discard - 2.f, 1.f, p.mDiscardBias);

    // convert bias into a vsize scaler
    bias = (F32) llroundf(powf(4, bias - 1.f));

    for (U32 i = 0; i < LLRender::NUM_TEXTURE_CHANNELS; ++i)
    {
        face_count += imagep->getNumFaces(i);
        S32 faces_to_check = (face_count > MAX_FACES_TO_CHECK) ? 0 : imagep->getNumFaces(i);

        for (S32 fi = 0; fi < faces_to_check; ++fi)
        {
            LLFace* face = (*(imagep->getFaceList(i)))[fi];

            if (face && face->getViewerObject())
            {
                F32 vsize = face->getPixelArea();

                on_screen |= face->mInFrustum;

                S32 te_offset = face->getTEOffset();  // offset is -1 if not inited
                LLViewerObject* objp = face->getViewerObject();
                const LLTextureEntry* te = (te_offset < 0 || te_offset >= objp->getNumTEs()) ? nullptr : objp->getTE(te_offset);
                F32 min_scale = te ? llmin(fabsf(te->getScaleS()), fabsf(te->getScaleT())) : 1.f;
                if (te && objp->getPCode() == LL_PCODE_VOLUME)
                {
                    const LLViewerTextureAnim* anim = static_cast<LLVOVolume*>(objp)->mTextureAnimp;
                    if (anim && (anim->mMode & LLTextureAnim::ON) && !(anim->mMode & (LLTextureAnim::ROTATE | LLTextureAnim::SCALE))
                        && anim->mSizeX && anim->mSizeY && (anim->mFace < 0 || anim->mFace == te_offset))
                    {
                        min_scale = llmin(1.f / anim->mSizeX, 1.f / anim->mSizeY);
                    }
                }
                min_scale = llclamp(min_scale * min_scale, p.mScaleMin, p.mScaleMax);
                vsize /= min_scale;

                if (!face->mInFrustum || p.mDiscardBias > 1.9f + face->mImportanceToCamera / 2.f)
                {
                    vsize /= bias;
                }

                if (face->mInFrustum)
                {
                    vsize *= llmax(face->mImportanceToCamera * p.mCameraBoost, 1.f);
                }

                // <ID:i.doll> [Full-res own avatar textures]
                if (p.mSelfFullRes && idIsOwnAttachment(objp))
                {
                    vsize = (F32)MAX_IMAGE_AREA;
                }
                // </ID:i.doll>

                max_vsize = llmax(max_vsize, vsize);

                if (max_vsize >= p.mMaxVirtualSize
                    && (on_screen || p.mDiscardBias <= BIAS_TRS_ON_SCREEN))
                {
                    break;
                }
            }
        }

        if (max_vsize >= p.mMaxVirtualSize
            && (on_screen || p.mDiscardBias <= BIAS_TRS_ON_SCREEN))
        {
            break;
        }
    }

    out.mMaxVSize = max_vsize;
    out.mOnScreen = on_screen;
    out.mFaceCount = face_count;
}

// Step 3 check: recompute a few parallel calcPixelArea results on the main thread.
// static
void FSParallelTexture::verifyFaces()
{
    constexpr size_t FACES_PER_FRAME = 16;
    static size_t start = 0;
    const size_t count = sFaces.size();
    if (!count)
    {
        return;
    }
    LL_PROFILE_ZONE_NAMED_CATEGORY_TEXTURE("fs tex priority - verify faces");
    start = (start + 7919) % count;
    size_t checked = 0;
    for (size_t n = 0; n < count && checked < FACES_PER_FRAME; ++n)
    {
        const size_t k = (start + n) % count;
        const FSFaceCheck& par = sFaceChecks[k];
        if (par.mThrottled)
        {
            continue;   // returned cached values; a forced recompute would legitimately differ
        }
        LLFace* face = sFaces[k];
        const F32 saved_update = face->mLastPixelAreaUpdate;
        face->mLastPixelAreaUpdate = -1.e9f;   // defeat the 0.1 s throttle
        F32 cos_angle, radius;
        const bool in_frustum = face->calcPixelArea(cos_angle, radius);
        face->mLastPixelAreaUpdate = saved_update;
        ++checked;
        ++sVerifiedFaces;
        if (in_frustum != par.mInFrustum || !nearly_equal(face->mPixelArea, par.mPixelArea)
            || !nearly_equal(face->mImportanceToCamera, par.mImportance))
        {
            ++sFaceMismatches;
            if (sLoggedMismatches < MAX_LOGGED_MISMATCHES)
            {
                ++sLoggedMismatches;
                LL_WARNS("Texture") << "FSPerfParallelTexturePriorityVerify: face pixel area mismatch"
                                    << " in_frustum " << par.mInFrustum << "/" << in_frustum
                                    << " pixel_area " << par.mPixelArea << "/" << face->mPixelArea
                                    << " importance " << par.mImportance << "/" << face->mImportanceToCamera
                                    << LL_ENDL;
            }
        }
    }
}

// static
bool FSParallelTexture::prepare(const entries_t& entries, const skip_set_t& skip)
{
    static LLCachedControl<bool> enabled(gSavedSettings, "FSPerfParallelTexturePriority", false);
    sActive = false;
    if (!enabled || gCubeSnapshot || entries.size() < MIN_PARALLEL_TEXTURES)
    {
        return false;
    }

    LL_PROFILE_ZONE_NAMED_CATEGORY_TEXTURE("fs tex priority - prepare");

    const size_t count = entries.size();
    sTextures.resize(count);
    sHasResult.assign(count, 0);
    sResults.resize(count);
    sEligible.clear();
    for (size_t i = 0; i < count; ++i)
    {
        LLViewerFetchedTexture* imagep = entries[i].get();
        sTextures[i] = imagep;
        // Same test as the sweep, plus the boost test that skips the face loop
        if (imagep->getNumRefs() > 1 && !skip.count(imagep) && imagep->getBoostLevel() < LLViewerFetchedTexture::BOOST_HIGH)
        {
            sEligible.push_back((U32)i);
        }
    }
    if (sEligible.size() < MIN_PARALLEL_TEXTURES)
    {
        return false;
    }

    FSTexturePool* pool = FSTexturePool::instance();
    const U32 frame = gFrameCount;
    const U32 texture_chunks = ((U32)sEligible.size() + TEXTURES_PER_CHUNK - 1) / TEXTURES_PER_CHUNK;

    // 1. stale faces, per texture chunk (workers, read only)
    {
        LL_PROFILE_ZONE_NAMED_CATEGORY_TEXTURE("fs tex priority - stale faces");
        if (sChunkFaces.size() < texture_chunks)
        {
            sChunkFaces.resize(texture_chunks);
        }
        std::function<void(U32)> job = [frame](U32 chunk)
        {
            std::vector<LLFace*>& out = sChunkFaces[chunk].mFaces;
            out.clear();
            const U32 end = llmin((chunk + 1) * TEXTURES_PER_CHUNK, (U32)sEligible.size());
            for (U32 e = chunk * TEXTURES_PER_CHUNK; e < end; ++e)
            {
                collect_stale_faces(sTextures[sEligible[e]], frame, out);
            }
        };
        pool->run(texture_chunks, job);
    }

    // 2. each stale face once, in sweep order, stamped as the serial loop stamps it (main)
    {
        LL_PROFILE_ZONE_NAMED_CATEGORY_TEXTURE("fs tex priority - dedupe faces");
        sFaces.clear();
        sRiggedFaces.clear();
        for (U32 chunk = 0; chunk < texture_chunks; ++chunk)
        {
            for (LLFace* face : sChunkFaces[chunk].mFaces)
            {
                if ((frame - face->mLastTextureUpdate) > FACE_UPDATE_FRAMES)
                {
                    face->mLastTextureUpdate = frame;
                    if (face->isState(LLFace::RIGGED))
                    {
                        sRiggedFaces.push_back(face);
                    }
                    else
                    {
                        sFaces.push_back(face);
                    }
                }
            }
        }
    }

    // 3. pixel areas: non-rigged faces on the workers, rigged ones on the main thread meanwhile
    const bool verify = verifying();
    {
        LL_PROFILE_ZONE_NAMED_CATEGORY_TEXTURE("fs tex priority - pixel areas");
        sFaceChecks.resize(sFaces.size());
        const F32 now = gFrameTimeSeconds;
        const U32 face_chunks = ((U32)sFaces.size() + FACES_PER_CHUNK - 1) / FACES_PER_CHUNK;
        std::function<void(U32)> job = [now, verify](U32 chunk)
        {
            LL_PROFILE_ZONE_NAMED_CATEGORY_TEXTURE("fs tex priority - pixel area chunk");
            const U32 end = llmin((chunk + 1) * FACES_PER_CHUNK, (U32)sFaces.size());
            for (U32 k = chunk * FACES_PER_CHUNK; k < end; ++k)
            {
                LLFace* face = sFaces[k];
                const bool throttled = now - face->mLastPixelAreaUpdate < PIXEL_AREA_UPDATE_PERIOD;
                F32 radius;
                F32 cos_angle_to_view_dir;
                face->mInFrustum = face->calcPixelArea(cos_angle_to_view_dir, radius);
                if (verify)
                {
                    sFaceChecks[k] = { throttled, face->mInFrustum, face->mPixelArea, face->mImportanceToCamera };
                }
            }
        };
        std::function<void()> rigged = []()
        {
            LL_PROFILE_ZONE_NAMED_CATEGORY_TEXTURE("fs tex priority - rigged pixel areas");
            for (LLFace* face : sRiggedFaces)
            {
                F32 radius;
                F32 cos_angle_to_view_dir;
                face->mInFrustum = face->calcPixelArea(cos_angle_to_view_dir, radius);
            }
        };
        pool->run(face_chunks, job, &rigged);
    }

    if (verify)
    {
        verifyFaces();
    }

    // 4. per-texture reduction (workers)
    {
        LL_PROFILE_ZONE_NAMED_CATEGORY_TEXTURE("fs tex priority - reduce");
        static LLCachedControl<F32> texture_scale_min(gSavedSettings, "TextureScaleMinAreaFactor", 0.0095f);
        static LLCachedControl<F32> texture_scale_max(gSavedSettings, "TextureScaleMaxAreaFactor", 25.f);
        static LLCachedControl<F32> texture_camera_boost(gSavedSettings, "TextureCameraBoost", 8.f);
        static LLCachedControl<bool> self_full_res(gSavedSettings, "IDSelfAvatarFullResTextures", false); // <ID:i.doll/>
        Params params;
        params.mDiscardBias = LLViewerTexture::sDesiredDiscardBias;
        params.mMaxVirtualSize = LLViewerFetchedTexture::sMaxVirtualSize;
        params.mScaleMin = texture_scale_min();
        params.mScaleMax = texture_scale_max();
        params.mCameraBoost = texture_camera_boost();
        params.mSelfFullRes = self_full_res(); // <ID:i.doll/>

        std::function<void(U32)> job = [&params](U32 chunk)
        {
            LL_PROFILE_ZONE_NAMED_CATEGORY_TEXTURE("fs tex priority - reduce chunk");
            const U32 end = llmin((chunk + 1) * TEXTURES_PER_CHUNK, (U32)sEligible.size());
            for (U32 e = chunk * TEXTURES_PER_CHUNK; e < end; ++e)
            {
                const U32 i = sEligible[e];
                reduceFaces(sTextures[i], params, sResults[i]);
            }
        };
        pool->run(texture_chunks, job);
    }

    for (U32 i : sEligible)
    {
        sHasResult[i] = 1;
    }
    sActive = true;
    return true;
}

// static
const FSTexturePriorityFaces* FSParallelTexture::result(size_t index, const LLViewerFetchedTexture* imagep)
{
    if (!sActive || index >= sTextures.size() || !sHasResult[index] || sTextures[index] != imagep)
    {
        return nullptr;
    }
    return &sResults[index];
}

// static
bool FSParallelTexture::verifying()
{
    static LLCachedControl<bool> verify(gSavedSettings, "FSPerfParallelTexturePriorityVerify", false);
    return verify;
}

// static
void FSParallelTexture::verify(const LLViewerFetchedTexture* imagep, const FSTexturePriorityFaces& parallel,
                               const FSTexturePriorityFaces& serial)
{
    if (serial.mFaceCount == U32_MAX)
    {
        return; // boosted since prepare(): the serial path skipped its face loop
    }
    ++sVerifiedTextures;
    if (parallel.mOnScreen != serial.mOnScreen || parallel.mFaceCount != serial.mFaceCount
        || !nearly_equal(parallel.mMaxVSize, serial.mMaxVSize))
    {
        ++sTextureMismatches;
        if (sLoggedMismatches < MAX_LOGGED_MISMATCHES)
        {
            ++sLoggedMismatches;
            LL_WARNS("Texture") << "FSPerfParallelTexturePriorityVerify: texture " << imagep->getID()
                                << " max_vsize " << parallel.mMaxVSize << "/" << serial.mMaxVSize
                                << " on_screen " << parallel.mOnScreen << "/" << serial.mOnScreen
                                << " faces " << parallel.mFaceCount << "/" << serial.mFaceCount
                                << " (parallel/serial)" << LL_ENDL;
        }
    }
}

// static
void FSParallelTexture::finish()
{
    if (sActive && verifying())
    {
        log_verify_summary();
    }
    sActive = false;
}
