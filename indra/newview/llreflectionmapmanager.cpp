/**
 * @file llreflectionmapmanager.cpp
 * @brief LLReflectionMapManager class implementation
 *
 * $LicenseInfo:firstyear=2022&license=viewerlgpl$
 * Second Life Viewer Source Code
 * Copyright (C) 2022, Linden Research, Inc.
 *
 * Alchemy Viewer Source Code
 * Copyright © 2026, Rye <rye@alchemyviewer.org>
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
 * Linden Research, Inc., 945 Battery Street, San Francisco, CA  94111  USA
 * $/LicenseInfo$
 */

#include "llviewerprecompiledheaders.h"

#include "llreflectionmapmanager.h"

#include <vector>
#include <map>      // <FS:Trish/> ProbeCull logging
#include <sstream>  // <FS:Trish/>

#include "llviewercamera.h"
#include "llspatialpartition.h"
#include "llviewerregion.h"
#include "pipeline.h"
#include "llviewershadermgr.h"
#include "llviewercontrol.h"
#include "llenvironment.h"
#include "llstartup.h"
#include "llviewermenufile.h"
#include "llnotificationsutil.h"

#if LL_WINDOWS
#pragma warning (push)
#pragma warning (disable : 4702) // compiler complains unreachable code
#endif
#define TINYEXR_USE_MINIZ 0
#include "zlib.h"
#define TINYEXR_IMPLEMENTATION
#include "tinyexr/tinyexr.h"
#if LL_WINDOWS
#pragma warning (pop)
#endif

LLPointer<LLImageGL> gEXRImage;

void load_exr(const std::string& filename)
{
    // reset reflection maps when previewing a new HDRI
    gPipeline.mReflectionMapManager.reset();
    gPipeline.mReflectionMapManager.initReflectionMaps();

    float* out; // width * height * RGBA
    int width;
    int height;
    const char* err = NULL; // or nullptr in C++11

    int ret =  LoadEXRWithLayer(&out, &width, &height, filename.c_str(), /* layername */ nullptr, &err);
    if (ret == TINYEXR_SUCCESS)
    {
        // This texture is owned by the viewer.  Do not wrap a manually
        // generated name as an external texture: doing so leaves its GL
        // lifetime outside LLImageGL and leaked previous HDRI previews.
        gEXRImage = new LLImageGL(width, height, 4, true);
        gEXRImage->setExplicitFormat(GL_RGB16F, GL_RGBA, GL_FLOAT);
        gEXRImage->setHasMipMaps(true);
        gEXRImage->setUseMipMaps(true);
        gEXRImage->setFilteringOption(LLTexUnit::TFO_TRILINEAR);

        if (!gEXRImage->createGLTexture())
        {
            free(out);
            gEXRImage = nullptr;
            return;
        }

        gGL.getTexUnit(0)->bind(gEXRImage);

        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB16F, width, height, 0, GL_RGBA, GL_FLOAT, out);
        LLImageGLMemory::alloc_tex_image(width, height, GL_RGB16F, 1);

        free(out); // release memory of image data

        glGenerateMipmap(GL_TEXTURE_2D);

        gGL.getTexUnit(0)->unbind(LLTexUnit::TT_TEXTURE);

    }
    else
    {
        LLSD notif_args;
        notif_args["WHAT"] = filename;
        notif_args["REASON"] = "Unknown";
        if (err)
        {
            notif_args["REASON"] = std::string(err);
            FreeEXRErrorMessage(err); // release memory of error message.
        }
        LLNotificationsUtil::add("CannotLoad", notif_args);
    }
}

void hdri_preview()
{
    LLFilePickerReplyThread::startPicker(
        [](const std::vector<std::string>& filenames, LLFilePicker::ELoadFilter load_filter, LLFilePicker::ESaveFilter save_filter)
        {
            if (LLAppViewer::instance()->quitRequested())
            {
                return;
            }
            if (filenames.size() > 0)
            {
                load_exr(filenames[0]);
            }
        },
        LLFilePicker::FFLOAD_HDRI,
        true);
}

extern bool gCubeSnapshot;
extern bool gTeleportDisplay;

static U32 sUpdateCount = 0;


// get the next highest power of two of v (or v if v is already a power of two)
//defined in llvertexbuffer.cpp
extern U32 nhpo2(U32 v);

static void touch_default_probe(LLReflectionMap* probe)
{
    if (LLViewerCamera::getInstance())
    {
        LLVector3 origin = LLViewerCamera::getInstance()->getOrigin();
        origin.mV[2] += 64.f;

        probe->mOrigin.load3(origin.mV);
    }
}

LLReflectionMapManager::LLReflectionMapManager()
{
    mDynamicProbeCount = LL_MAX_REFLECTION_PROBE_COUNT;
    initCubeFree();
}

void LLReflectionMapManager::initCubeFree()
{
    // start at 1 because index 0 is reserved for mDefaultProbe
    for (U32 i = 1; i < mDynamicProbeCount; ++i)
    {
        mCubeFree.push_back(i);
    }
}

// Order probes for the cube-slot handout below: placed probes first, then by distance.
//
// This order decides which probes exist in VRAM at all, so it is where artist intent has to be
// honoured. Distance alone did the opposite: mDistance is (distance to origin - radius), so a
// large influence volume reads as nearer than a small one the camera is equally far inside --
// an automatic probe covering a 16m octree node has a radius of about 13.9, and a manual probe
// on a 2m prim standing in the same spot has 1. The placed probe lost the slot every time.
//
// A probe's radius is not a statement about its importance when the octree chose it and is
// exactly that when a person did, so priority leads and distance orders within each group.
// Automatic probes are a fallback for space nobody placed a probe in; they should be what gives
// when there is not enough room for everything.
// <FS:Trish> ProbeCull logging state
namespace
{
    std::map<std::string, U32> sCullCounts;
    F32 sLastCullSummary = 0.f;

    const char* probe_type_name(const LLReflectionMap* probe)
    {
        if (probe->mViewerObject)
        {
            return "manual";
        }
        if (probe->mOrphaned)
        {
            return "orphan";
        }
        return probe->mGroup ? "auto" : "terrain";
    }
}
// </FS:Trish>

struct CompareProbeDistance
{
    bool operator()(const LLPointer<LLReflectionMap>& lhs, const LLPointer<LLReflectionMap>& rhs) const
    {
        if (lhs->mPriority != rhs->mPriority)
        {
            return lhs->mPriority > rhs->mPriority;
        }

        return lhs->mDistance < rhs->mDistance;
    }
};

// <FS:Trish> Importance-weighted update order (FSPerfProbeUpdatePriority), set once per update()
static bool sImportancePriority = false;

// How much a refresh of this probe is worth per second of age: probes the camera can see and
// probes close to it go stale visibly, an off-screen probe 100 m away hardly at all. Age is a
// factor, not a term, so a probe nobody looks at still climbs the queue and is never starved.
static F32 update_importance(LLReflectionMap* p)
{
    constexpr F32 OFFSCREEN_WEIGHT = 0.25f;
    constexpr F32 DISTANCE_FALLOFF = 32.f; // metres at which importance halves
    F32 w = p->mInView ? 1.f : OFFSCREEN_WEIGHT;
    return w / (1.f + llmax(p->mDistance, 0.f) / DISTANCE_FALLOFF);
}
// </FS:Trish>

static F32 update_score(LLReflectionMap* p)
{
    // <FS:Trish>
    if (sImportancePriority)
    {
        return (gFrameTimeSeconds - p->mLastUpdateTime) * update_importance(p);
    }
    // </FS:Trish>
    return gFrameTimeSeconds - p->mLastUpdateTime  - p->mDistance*0.1f;
}

// return true if a is higher priority for an update than b
static bool check_priority(LLReflectionMap* a, LLReflectionMap* b)
{
    if (a->mCubeIndex == -1)
    { // not a candidate for updating
        return false;
    }
    else if (b->mCubeIndex == -1)
    { // b is not a candidate for updating, a is higher priority by default
        return true;
    }
    else if (!a->mComplete && !b->mComplete)
    { //neither probe is complete, generate the placed one first, then use distance
        // <FS:Trish> With importance ordering, a missing probe on screen is filled before one off screen
        if (sImportancePriority && a->mInView != b->mInView)
        {
            return a->mInView;
        }
        // </FS:Trish>
        if (a->mPriority != b->mPriority)
        {
            return a->mPriority > b->mPriority;
        }
        return a->mDistance < b->mDistance;
    }
    else if (a->mComplete && b->mComplete)
    { //both probes are complete, use update_score metric
        return update_score(a) > update_score(b);
    }

    // a or b is not complete,
    if (sUpdateCount % 3 == 0)
    { // every third update, allow complete probes to cut in line in front of non-complete probes to avoid spammy probe generators from deadlocking scheduler (SL-20258))
        return !b->mComplete;
    }

    // prioritize incomplete probe
    return b->mComplete;
}

// helper class to seed octree with probes
void LLReflectionMapManager::update()
{
    if (!LLPipeline::sReflectionProbesEnabled || gTeleportDisplay || LLStartUp::getStartupState() < STATE_PRECACHE)
    {
        return;
    }

    LL_PROFILE_ZONE_SCOPED_CATEGORY_DISPLAY;
    LL_PROFILE_GPU_ZONE("reflection manager update");
    llassert(!gCubeSnapshot); // assert a snapshot is not in progress
    if (LLAppViewer::instance()->logoutRequestSent())
    {
        return;
    }

    if (mPaused && gFrameTimeSeconds > mResumeTime)
    {
        resume();
    }

    mResetFade = llmin((F32)(mResetFade + gFrameIntervalSeconds * 2.f), 1.f);

    {
        U32 probe_count_temp = mDynamicProbeCount;
        if (mRenderReflectionProbeDynamicAllocation > -1)
        {
            if (mRenderReflectionProbeLevel == 0)
            {
                mDynamicProbeCount = 1;
            }
            else if (mRenderReflectionProbeLevel == 1)
            {
                mDynamicProbeCount = (U32)mProbes.size();
            }
            else if (mRenderReflectionProbeLevel == 2)
            {
                mDynamicProbeCount = llmax((U32)mProbes.size(), 128);
            }
            else
            {
                mDynamicProbeCount = 256;
            }

            if (mRenderReflectionProbeDynamicAllocation > 1)
            {
                // Round mDynamicProbeCount to the nearest increment of 16
                mDynamicProbeCount = ((mDynamicProbeCount + mRenderReflectionProbeDynamicAllocation / 2) / mRenderReflectionProbeDynamicAllocation) * 16;
                mDynamicProbeCount = llclamp(mDynamicProbeCount, 1, mRenderReflectionProbeCount);
            }
            else
            {
                mDynamicProbeCount = llclamp(mDynamicProbeCount + mRenderReflectionProbeDynamicAllocation, 1, mRenderReflectionProbeCount);
            }
        }
        else
        {
            mDynamicProbeCount = mRenderReflectionProbeCount;
        }

        mDynamicProbeCount = llmin(mDynamicProbeCount, LL_MAX_REFLECTION_PROBE_COUNT);

        if (mDynamicProbeCount != probe_count_temp)
            mResetFade = 1.f;
    }

    initReflectionMaps();

    static LLCachedControl<bool> render_hdr(gSavedSettings, "RenderHDREnabled", true);

    if (!mRenderTarget.isComplete())
    {
        U32 color_fmt = render_hdr ? GL_R11F_G11F_B10F : GL_RGB8;
        U32 targetRes = mProbeResolution * 4; // super sample
        mRenderTarget.allocate(targetRes, targetRes, color_fmt, true);
    }

    if (mMipChain.empty())
    {
        U32 res = mProbeResolution;
        U32 count = (U32)(log2((F32)res) + 0.5f);

        mMipChain.resize(count);
        for (U32 i = 0; i < count; ++i)
        {
            mMipChain[i].allocate(res, res, render_hdr ? GL_R11F_G11F_B10F : GL_RGB8);
            res /= 2;
        }
    }

    llassert(mProbes[0] == mDefaultProbe);

    LLVector4a camera_pos;
    camera_pos.load3(LLViewerCamera::instance().getOrigin().mV);

    // process kill list
    for (auto& probe : mKillList)
    {
        auto const & iter = std::find(mProbes.begin(), mProbes.end(), probe);
        if (iter != mProbes.end())
        {
            deleteProbe((U32)(iter - mProbes.begin()));
        }
    }

    mKillList.clear();

    // process create list
    for (auto& probe : mCreateList)
    {
        mProbes.push_back(probe);
    }

    mCreateList.clear();

    if (mProbes.empty())
    {
        return;
    }


    // <FS:Trish> Sky change (see invalidate())
    if (mInvalidate)
    {
        mInvalidate = false;
        for (auto& probe : mProbes)
        {
            probe->mLastUpdateTime = 0.f;
        }
        if (cullLogEnabled())
        {
            LL_INFOS("ProbeCull") << "+invalidated " << mProbes.size() << " probes (sky change), regenerating in place" << LL_ENDL;
        }
    }

    if (cullLogEnabled() && (gFrameTimeSeconds - sLastCullSummary) > 5.f)
    {
        sLastCullSummary = gFrameTimeSeconds;
        if (!sCullCounts.empty())
        {
            U32 with_slot = 0, in_view = 0, occluded = 0;
            for (auto& probe : mProbes)
            {
                with_slot += probe->mCubeIndex != -1 ? 1 : 0;
                in_view += probe->mInView ? 1 : 0;
                occluded += probe->mOccluded ? 1 : 0;
            }

            std::ostringstream counts;
            for (const auto& [reason, count] : sCullCounts)
            {
                counts << " [" << reason << ": " << count << "]";
            }
            LL_INFOS("ProbeCull") << "last 5s:" << counts.str() << " -- probes " << mProbes.size() << ", with slot " << with_slot
                << " of " << mReflectionProbeCount << ", in view " << in_view << ", occluded " << occluded << LL_ENDL;
            sCullCounts.clear();
        }
    }
    // </FS:Trish>

    // <FS:Trish> Scheduling options
    static LLCachedControl<bool> fs_update_priority(gSavedSettings, "FSPerfProbeUpdatePriority", false);
    sImportancePriority = fs_update_priority;
    // </FS:Trish>

    bool did_update = false;

    bool realtime = mRenderReflectionProbeDetail >= (S32)LLReflectionMapManager::DetailLevel::REALTIME;

    LLReflectionMap* closestDynamic = nullptr;

    LLReflectionMap* oldestProbe = nullptr;
    LLReflectionMap* oldestOccluded = nullptr;

    if (mUpdatingProbe != nullptr)
    {
        did_update = true;
        // <FS:Trish> With many probes the scheduler never runs out of "old" ones, so a face was rendered
        // every frame forever. Refreshing a probe that already has a good map can go slower; new and
        // incomplete probes, the default probe and dynamic ones keep one face per frame.
        //doProbeUpdate();
        static LLCachedControl<U32> refresh_interval(gSavedSettings, "RenderReflectionProbeRefreshFrameInterval", 3);
        const bool refreshing = mUpdatingProbe->mComplete && mUpdatingProbe != mDefaultProbe && !mUpdatingProbe->getIsDynamic();
        if (!refreshing || refresh_interval <= 1 || (LLFrameTimer::getFrameCount() % refresh_interval) == 0)
        {
            doProbeUpdate();
        }
        // </FS:Trish>
    }

    // Occlusion state only means anything for as long as the queries that produce it keep
    // running, and they stop whenever LLPipeline::sUseOcclusion drops out of its "query"
    // level -- the UseOcclusion setting, a feature-table veto, or simply turning on wireframe.
    // Nothing used to clear mOccluded when that happened, so every probe that was occluded at
    // that instant stayed occluded for the rest of the session: dropped by getReflectionMaps,
    // skipped by the neighbour packer, and unable to recover, because recovering needs a query
    // result and no query would ever run again. The one probe that escaped was whichever one
    // the camera stood inside, since that path answers false before issuing anything.
    if (LLPipeline::sUseOcclusion <= 1)
    {
        for (auto& probe : mProbes)
        {
            probe->mOccluded = false;
        }
    }

    // Make probes track the viewer objects they are attached to, then work out which automatic
    // probes a manual probe has made redundant.
    //
    // An automatic probe is a fallback for space nobody placed a probe in. Where a placed probe
    // has swallowed its whole influence volume it can no longer change a single pixel -- see
    // eclipses() for what "swallowed" has to mean for that to be true of each volume shape --
    // so the cube slot and the place in the update queue it holds are pure waste. Only probes
    // that are actually rendering get to evict anything, or a probe still generating would
    // leave a hole where the one it displaced used to be.
    {
        LL_PROFILE_ZONE_NAMED_CATEGORY_DISPLAY("rmmu - manual eclipse");

        // Slack, applied in whichever direction preserves the current state: a probe sitting on
        // the boundary of a manual probe that is moving would otherwise flip every frame, and
        // each flip costs it a full twelve-pass regeneration.
        const F32 ECLIPSE_HYSTERESIS = 0.5f;

        std::vector<LLReflectionMap*> manual_probes;

        // <FS:Trish> Every automatic probe against every manual one each frame showed up in idle profiles.
        // Probes move slowly and the hysteresis absorbs a short delay, so classify a few times a second,
        // or right away when probes come or go. Following the viewer objects stays per frame.
        constexpr F32 ECLIPSE_CHECK_INTERVAL = 0.25f;
        static F32 last_eclipse_check = -1.f;
        static size_t last_probe_count = 0;
        const bool check_eclipses = mProbes.size() != last_probe_count ||
            gFrameTimeSeconds - last_eclipse_check >= ECLIPSE_CHECK_INTERVAL || gFrameTimeSeconds < last_eclipse_check;
        if (check_eclipses)
        {
            last_eclipse_check = gFrameTimeSeconds;
            last_probe_count = mProbes.size();
        }
        // </FS:Trish>

        for (auto& probe : mProbes)
        {
            probe->syncToViewerObject();

            if (check_eclipses && // <FS:Trish/>
                probe != mDefaultProbe && probe->mViewerObject && probe->mComplete &&
                probe->mCubeIndex != -1 && probe->mFadeIn >= 1.f && probe->isRelevant())
            {
                manual_probes.push_back(probe);
            }
        }

        for (auto& probe : mProbes)
        {
            // <FS:Trish>
            if (!check_eclipses)
            {
                break;
            }
            // </FS:Trish>
            if (probe == mDefaultProbe || probe->mViewerObject)
            { // only automatic probes are ever displaced
                continue;
            }

            F32 margin = probe->mInsideManualProbe ? ECLIPSE_HYSTERESIS : -ECLIPSE_HYSTERESIS;

            bool eclipsed = false;
            for (auto* manual : manual_probes)
            {
                if (manual->eclipses(probe, margin))
                {
                    eclipsed = true;
                    break;
                }
            }

            // <FS:Trish>
            if (eclipsed != probe->mInsideManualProbe)
            {
                logCull(eclipsed ? "eclipsed by manual probe" : "+un-eclipsed (manual probe gone)", probe);
            }
            // </FS:Trish>
            probe->mInsideManualProbe = eclipsed;
        }
    }

    // Update distance to camera for all probes.
    //
    // This has to happen before the sort below, before the cube slot handout that follows it,
    // and before check_priority's tie-break -- all three consume mDistance. It used to be done
    // inside the scheduling loop further down, one frame after the sort that reads it, and only
    // for probes that loop did not skip: an irrelevant probe, or any probe at all while paused,
    // kept whatever value it last held and sorted on that. A newly created probe holds -1, which
    // is nearer than anything real.
    LLViewerCamera* camera = LLViewerCamera::getInstance(); // <FS:Trish/>
    for (auto& probe : mProbes)
    {
        // <FS:Trish> On screen! Spare the poor probe
        probe->mInView = probe == mDefaultProbe ||
            camera->sphereInFrustum(LLVector3(probe->mOrigin.getF32ptr()), probe->mRadius) != 0;
        // </FS:Trish>

        if (probe == mDefaultProbe)
        {
            // 64m for the purposes of prioritization once it has been generated once, and a
            // hard boost while it has not
            probe->mDistance = probe->mComplete ? 64.f : -4096.f;
            continue;
        }

        if (!probe->isRelevant())
        { // sorts to the very back, so the release pass below reclaims its cube slot -- that
          // pass only ever looks past mReflectionProbeCount, so a probe that stopped mattering
          // while close to the camera used to hold its slot indefinitely
            probe->mDistance = FLT_MAX;
            continue;
        }

        LLVector4a d;
        d.setSub(camera_pos, probe->mOrigin);
        probe->mDistance = d.getLength3().getF32() - probe->mRadius;
    }

    std::sort(mProbes.begin()+1, mProbes.end(), CompareProbeDistance());
    llassert(mProbes[0] == mDefaultProbe);
    llassert(mProbes[0]->mCubeArray == mTexture);
    llassert(mProbes[0]->mCubeIndex == 0);

    // make sure we're assigning cube slots to the closest probes

    // first free any cube indices for distant probes, and for probes that can no longer affect
    // anything. The second case was unreachable before: relevance had no bearing on the sort, so
    // an irrelevant probe near the camera sat inside the budget and held its slot indefinitely,
    // while still being complete enough for getReflectionMaps to keep handing it to the shader.
    // Index 0 is the default probe and is never released.
    //
    // <FS:Trish> In our world we can blow budgets, only evict a probe once another needs a slot while over budget.
    const U32 release_limit = mReflectionProbeCount + llmax(4U, mReflectionProbeCount / 8);
    // </FS:Trish>
    for (U32 i = 1; i < mProbes.size(); ++i)
    {
        LLReflectionMap* probe = mProbes[i];
        llassert(probe != nullptr);

        // <FS:Trish>
        //if (probe && probe->mCubeIndex != -1 && mUpdatingProbe != probe &&
        //    (i >= mReflectionProbeCount || !probe->isRelevant()))
        if (probe && probe->mCubeIndex != -1 && mUpdatingProbe != probe &&
            (i >= release_limit || !probe->isRelevant()))
        { // free this index
            releaseCubeIndex(probe, probe->isRelevant() ? "slot released (far past budget)" : "slot released (no longer relevant)");
        }
        // </FS:Trish>
    }

    // next distribute the free indices
    U32 count = llmin(mReflectionProbeCount, (U32)mProbes.size());

    //for (U32 i = 1; i < count && !mCubeFree.empty(); ++i)
    for (U32 i = 1; i < count; ++i) // <FS:Trish/> evictCubeIndex can make room
    {
        // find the closest probe that needs a cube index
        LLReflectionMap* probe = mProbes[i];

        // Relevance checked here as well as in the release pass above. When there are fewer
        // probes than slots every probe is inside the budget, so without this an irrelevant one
        // would be handed a slot and have it taken back on the next frame, forever.
        if (probe->mCubeIndex == -1 && probe->isRelevant())
        {
            // <FS:Trish>
            //S32 idx = allocateCubeIndex();
            //llassert(idx > 0); //if we're still in this loop, mCubeFree should not be empty and allocateCubeIndex should be returning good indices
            //probe->mCubeArray = mTexture;
            //probe->mCubeIndex = idx;
            if (mCubeFree.empty() && !evictCubeIndex(count))
            {
                break;
            }
            assignCubeIndex(probe);
            // </FS:Trish>
        }
    }

    for (unsigned int i = 0; i < mProbes.size(); ++i)
    {
        LLReflectionMap* probe = mProbes[i];
        if (probe->getNumRefs() == 1)
        { // no references held outside manager, delete this probe
            // <FS:Trish> ...unless it is an automatic probe whose octree group was just torn
            constexpr F32 ORPHAN_GRACE_SECONDS = 10.f;
            if (probe->mOrphaned &&
                (probe->mComplete || probe->mRetainedCubeIndex != -1) &&
                (gFrameTimeSeconds - probe->mOrphanTime) < ORPHAN_GRACE_SECONDS)
            {
                continue;
            }
            logCull(probe->mOrphaned ? "deleted (octree group gone, not rebuilt)" : "deleted (no longer referenced)", probe);
            // </FS:Trish>
            deleteProbe(i);
            --i;
            continue;
        }

        if (probe != mDefaultProbe &&
            (!probe->isRelevant() || mPaused))
        { // skip irrelevant probes (or all non-default probes if paused)
            continue;
        }

        if (probe->mComplete)
        {
            probe->autoAdjustOrigin();
            probe->mFadeIn = llmin((F32) (probe->mFadeIn + gFrameIntervalSeconds), 1.f);

            // Rebuild the neighbour graph once the influence volume has drifted away from the
            // one it was built against.
            //
            // The shader stops its scan at the first probe covering a pixel and then considers
            // only that probe's neighbours, which is sound only while the graph is current: two
            // probes that both contain a point necessarily intersect, so the second is reachable
            // from the first. The graph was only ever rebuilt when a probe finished a full
            // twelve-pass update, so a probe riding a moving object described where it used to
            // be, and a pixel it covered could be skipped outright because whichever probe the
            // scan started from had never heard of it. Rebuilding here also fixes the other
            // side, since the search phase writes into both lists.
            if (probe->neighborsAreStale())
            {
                updateNeighbors(probe);
            }
        }
        if (probe->mOccluded && probe->mComplete)
        {
            if (oldestOccluded == nullptr)
            {
                oldestOccluded = probe;
            }
            else if (probe->mLastUpdateTime < oldestOccluded->mLastUpdateTime)
            {
                oldestOccluded = probe;
            }
        }
        else
        {
            // <FS:Trish> Once every probe is complete the oldest one was re-rendered forever, one cube face every frame.
            // Leave complete static probes alone until they reach a minimum age, sky changes reset their update time.
            static LLCachedControl<F32> sMinRefresh(gSavedSettings, "RenderReflectionProbeMinRefresh", 10.f);
            const bool recently_refreshed = probe->mComplete && !probe->getIsDynamic() &&
                (gFrameTimeSeconds - probe->mLastUpdateTime) < sMinRefresh;
            // </FS:Trish>
            if (!did_update &&
                i < mReflectionProbeCount &&
                !probe->mOrphaned && // <FS:Trish/>
                !recently_refreshed && // <FS:Trish/>
                (oldestProbe == nullptr ||
                    check_priority(probe, oldestProbe)))
            {
               oldestProbe = probe;
            }
        }

        if (realtime &&
            closestDynamic == nullptr &&
            probe->mCubeIndex != -1 &&
            probe->getIsDynamic())
        {
            closestDynamic = probe;
        }

        if (mRenderReflectionProbeLevel == 0)
        {
            // only update default probe when coverage is set to none
            llassert(probe == mDefaultProbe);
            break;
        }
    }

    // <FS:Trish> Spread the realtime probe's faces over several frames
    static LLCachedControl<U32> fs_rt_faces(gSavedSettings, "FSPerfProbeRealtimeFacesPerFrame", 6);
    if (realtime && closestDynamic != nullptr && fs_rt_faces < 6)
    {
        updateRealtimeProbeSpread(closestDynamic, fs_rt_faces);
    }
    else
    // </FS:Trish>
    if (realtime && closestDynamic != nullptr)
    {
        LL_PROFILE_ZONE_NAMED_CATEGORY_DISPLAY("rmmu - realtime");
        // update the closest dynamic probe realtime
        // should do a full irradiance pass on "odd" frames and a radiance pass on "even" frames
        closestDynamic->autoAdjustOrigin();

        // store and override the value of "isRadiancePass" -- parts of the render pipe rely on "isRadiancePass" to set
        // lighting values etc
        bool radiance_pass = isRadiancePass();
        mRadiancePass = mRealtimeRadiancePass;
        for (U32 i = 0; i < 6; ++i)
        {
            updateProbeFace(closestDynamic, i);
        }
        mRealtimeRadiancePass = !mRealtimeRadiancePass;

        // restore "isRadiancePass"
        mRadiancePass = radiance_pass;
    }

    static LLCachedControl<F32> sUpdatePeriod(gSavedSettings, "RenderDefaultProbeUpdatePeriod", 2.f);
    if ((gFrameTimeSeconds - mDefaultProbe->mLastUpdateTime) < sUpdatePeriod)
    {
        if (mRenderReflectionProbeLevel == 0)
        { // when probes are disabled don't update the default probe more often than the prescribed update period
            oldestProbe = nullptr;
        }
    }
    else if (mRenderReflectionProbeLevel > 0)
    { // when probes are enabled don't update the default probe less often than the prescribed update period
      oldestProbe = mDefaultProbe;
    }

    // switch to updating the next oldest probe
    if (!did_update && oldestProbe != nullptr)
    {
        LLReflectionMap* probe = oldestProbe;
        llassert(probe->mCubeIndex != -1);

        probe->autoAdjustOrigin();

        sUpdateCount++;
        mUpdatingProbe = probe;
        doProbeUpdate();
    }

    if (oldestOccluded)
    {
        // as far as this occluded probe is concerned, an origin/radius update is as good as a full update
        oldestOccluded->autoAdjustOrigin();
        oldestOccluded->mLastUpdateTime = gFrameTimeSeconds;
    }
}

void LLReflectionMapManager::refreshSettings()
{
    mRenderReflectionProbeDetail = gSavedSettings.getS32("RenderReflectionProbeDetail");
    mRenderReflectionProbeLevel = gSavedSettings.getS32("RenderReflectionProbeLevel");
    mRenderReflectionProbeCount = gSavedSettings.getU32("RenderReflectionProbeCount");
    mRenderReflectionProbeDynamicAllocation = gSavedSettings.getS32("RenderReflectionProbeDynamicAllocation");
    cleanupQueryPool();
}

LLReflectionMap* LLReflectionMapManager::addProbe(LLSpatialGroup* group)
{
    if (gGLManager.mGLVersion < 4.05f || !LLPipeline::sReflectionProbesEnabled)
    {
        return nullptr;
    }

    LLReflectionMap* probe = new LLReflectionMap();
    probe->mGroup = group;

    if (mDefaultProbe.isNull())
    {  //safety check to make sure default probe is always first probe added
        mDefaultProbe = new LLReflectionMap();
        mProbes.push_back(mDefaultProbe);
    }

    llassert(mProbes[0] == mDefaultProbe);

    if (group)
    {
        probe->mOrigin = group->getOctreeNode()->getCenter();
        probe->mGroupCenter = probe->mOrigin; // <FS:Trish/> autoAdjustOrigin moves mOrigin
    }

    if (gCubeSnapshot)
    { //snapshot is in progress, mProbes is being iterated over, defer insertion until next update
        mCreateList.push_back(probe);
    }
    else
    {
        mProbes.push_back(probe);
    }

    return probe;
}

U32 LLReflectionMapManager::probeCount()
{
    return mDynamicProbeCount;
}

U32 LLReflectionMapManager::probeMemory()
{
    // The SH coefficient strip is 9 x probes x RGBA16F -- tens of kilobytes, below the
    // resolution this reports in.
    return (mDynamicProbeCount * 6 * (mProbeResolution * mProbeResolution) * 4) / 1024 / 1024;
}

GLuint LLReflectionMapManager::allocateQuery()
{
    if (mQueryPool.empty())
    {
        GLuint query;
        glGenQueries(1, &query);
        return query;
    }

    GLuint query = mQueryPool.front();
    mQueryPool.pop_front();
    return query;
}

void LLReflectionMapManager::recycleQuery(GLuint query)
{
    mQueryPool.push_back(query);
}

struct CompareProbeDepth
{
    bool operator()(const LLReflectionMap* lhs, const LLReflectionMap* rhs)
    {
        return lhs->mMinDepth < rhs->mMinDepth;
    }
};

void LLReflectionMapManager::getReflectionMaps(std::vector<LLReflectionMap*>& maps)
{
    LL_PROFILE_ZONE_SCOPED_CATEGORY_DISPLAY;

    LLMatrix4a modelview;
    modelview.loadu(gGLModelView);
    LLVector4a oa; // scratch space for transformed origin

    // Occlusion is measured from the main camera, so it says nothing about what a probe capture
    // rendering from somewhere else can see. llviewerdisplay stops the queries for the duration
    // of a snapshot for exactly that reason -- "don't read or write it during cube snapshots" --
    // but only the write half was ever honoured, and this is the read. Without it every capture
    // omits whatever the player happens not to be looking at, and bakes that into the probe.
    const bool honor_occlusion = !gCubeSnapshot;

    // <FS:Trish>
    const bool cull_frustum = honor_occlusion && LLViewerCamera::sCurCameraID == LLViewerCamera::CAMERA_WORLD;
    LLViewerCamera* camera = LLViewerCamera::getInstance();
    // </FS:Trish>

    U32 count = 0;
    U32 lastIdx = 0;
    for (U32 i = 0; count < maps.size() && i < mProbes.size(); ++i)
    {
        // Every probe that does not make the list gets its index cleared, not just the ones
        // without a cube slot. A probe that is occluded or not yet complete used to keep the
        // index it held on some earlier frame, and that slot now belongs to a different probe:
        // the neighbour packer rejects -1 but has no way to tell a stale index from a live one,
        // so a neighbour entry could point at an unrelated probe.
        // <FS:Trish>
        //if (mProbes[i]->mCubeIndex != -1 && mProbes[i]->mComplete &&
        //    !(honor_occlusion && mProbes[i]->mOccluded))
        if (mProbes[i]->mCubeIndex != -1 && mProbes[i]->mComplete &&
            !(honor_occlusion && mProbes[i]->mOccluded) &&
            !(cull_frustum && mProbes[i] != mDefaultProbe &&
              !camera->sphereInFrustum(LLVector3(mProbes[i]->mOrigin.getF32ptr()), mProbes[i]->mRadius)))
        // </FS:Trish>
        {
            maps[count++] = mProbes[i];
            modelview.affineTransform(mProbes[i]->mOrigin, oa);
            mProbes[i]->mMinDepth = -oa.getF32ptr()[2] - mProbes[i]->mRadius;
            mProbes[i]->mMaxDepth = -oa.getF32ptr()[2] + mProbes[i]->mRadius;
        }
        else
        {
            mProbes[i]->mProbeIndex = -1;
        }
        lastIdx = i;
    }

    // set remaining probe indices to -1
    for (U32 i = lastIdx+1; i < mProbes.size(); ++i)
    {
        mProbes[i]->mProbeIndex = -1;
    }

    if (count > 1)
    {
        std::sort(maps.begin(), maps.begin() + count, CompareProbeDepth());
    }

    for (U32 i = 0; i < count; ++i)
    {
        maps[i]->mProbeIndex = i;
    }

    // null terminate list
    if (count < maps.size())
    {
        maps[count] = nullptr;
    }
}

LLReflectionMap* LLReflectionMapManager::registerSpatialGroup(LLSpatialGroup* group)
{
    if (!group)
    {
        return nullptr;
    }
    LLSpatialPartition* part = group->getSpatialPartition();
    if (!part || part->mPartitionType != LLViewerRegion::PARTITION_VOLUME)
    {
        return nullptr;
    }
    OctreeNode* node = group->getOctreeNode();
    F32 size = node->getSize().getF32ptr()[0];
    if (size < 15.f || size > 17.f)
    {
        return nullptr;
    }

    // <FS:Trish> A group rebuilt over the same octree node
    const LLVector4a& center = node->getCenter();
    for (auto& probe : mProbes)
    {
        if (probe->mOrphaned && probe->getNumRefs() == 1)
        {
            LLVector4a delta;
            delta.setSub(probe->mGroupCenter, center);
            if (delta.getLength3().getF32() < 0.5f)
            {
                probe->mGroup = group;
                probe->mOrphaned = false;
                logCull("+orphan adopted by rebuilt group", probe);
                return probe;
            }
        }
    }
    // </FS:Trish>

    return addProbe(group);
}

LLReflectionMap* LLReflectionMapManager::registerViewerObject(LLViewerObject* vobj)
{
    if (!LLPipeline::sReflectionProbesEnabled)
    {
        return nullptr;
    }

    llassert(vobj != nullptr);

    LLReflectionMap* probe = new LLReflectionMap();
    probe->mViewerObject = vobj;
    // A placed probe, and it is one from the moment it exists. This used to be derived inside
    // autoAdjustOrigin, which is a placement pass and does not run on a probe until it has
    // already won a cube slot -- so a new manual probe carried an automatic probe's priority
    // through the sort that hands slots out, and in a region with none free it could never
    // reach the code that would have told the sort otherwise.
    probe->mPriority = 1;
    probe->mOrigin.load3(vobj->getPositionAgent().mV);

    if (gCubeSnapshot)
    { //snapshot is in progress, mProbes is being iterated over, defer insertion until next update
        mCreateList.push_back(probe);
    }
    else
    {
        mProbes.push_back(probe);
    }

    return probe;
}

S32 LLReflectionMapManager::allocateCubeIndex()
{
    if (!mCubeFree.empty())
    {
        S32 ret = mCubeFree.front();
        mCubeFree.pop_front();
        return ret;
    }

    return -1;
}

// <FS:Trish> Probe culling stability
//static
bool LLReflectionMapManager::cullLogEnabled()
{
    static LLCachedControl<bool> enabled(gSavedSettings, "FSDebugReflectionProbeCulls", false);
    return enabled;
}

//static
void LLReflectionMapManager::logCull(const char* reason, const LLReflectionMap* probe)
{
    if (!probe || !cullLogEnabled())
    {
        return;
    }

    ++sCullCounts[reason];

    if (!probe->mInView)
    { 
        return;
    }

    const F32* o = probe->mOrigin.getF32ptr();
    LL_INFOS("ProbeCull") << reason << ": " << probe_type_name(probe) << " probe " << (const void*)probe
        << " at <" << o[0] << ", " << o[1] << ", " << o[2] << "> radius " << probe->mRadius
        << " distance " << probe->mDistance << " slot " << probe->mCubeIndex
        << " complete " << probe->mComplete << LL_ENDL;
}

void LLReflectionMapManager::invalidate()
{
    mInvalidate = true;
}

void LLReflectionMapManager::clearSlotOwners()
{
    for (auto& probe : mProbes)
    {
        probe->mRetainedCubeIndex = -1;
        probe->mRetainedComplete = false;
    }
    mSlotOwner.assign(mReflectionProbeCount + 2, nullptr);
}

void LLReflectionMapManager::assignCubeIndex(LLReflectionMap* probe)
{
    S32 idx = -1;
    bool reclaimed = false;

    // Retake same slot if no one took it.
    const S32 retained = probe->mRetainedCubeIndex;
    if (retained > 0 && retained < (S32)mSlotOwner.size() && mSlotOwner[retained] == probe)
    {
        auto iter = std::find(mCubeFree.begin(), mCubeFree.end(), retained);
        if (iter != mCubeFree.end())
        {
            mCubeFree.erase(iter);
            idx = retained;
            reclaimed = true;
        }
    }

    if (idx == -1)
    {
        idx = allocateCubeIndex();
        if (idx <= 0)
        {
            return;
        }
        if (idx < (S32)mSlotOwner.size() && mSlotOwner[idx] && mSlotOwner[idx] != probe)
        { // the probe that held this slot loses what it left in it
            mSlotOwner[idx]->mRetainedCubeIndex = -1;
            mSlotOwner[idx]->mRetainedComplete = false;
        }
    }

    probe->mCubeArray = mTexture;
    probe->mCubeIndex = idx;
    if (idx < (S32)mSlotOwner.size())
    {
        mSlotOwner[idx] = probe;
    }

    if (reclaimed)
    {
        probe->mComplete = probe->mRetainedComplete;
        if (probe->mComplete)
        { // The probe was always correct, so don't fade it back in from nothing!
            probe->mFadeIn = 1.f;
        }
        logCull("+slot reclaimed", probe);
    }
    else
    {
        logCull("+slot assigned (regenerating)", probe);
    }

    probe->mRetainedCubeIndex = -1;
    probe->mRetainedComplete = false;
}

void LLReflectionMapManager::releaseCubeIndex(LLReflectionMap* probe, const char* reason)
{
    logCull(reason, probe);

    // Pushed to the back
    mCubeFree.push_back(probe->mCubeIndex);
    probe->mRetainedCubeIndex = probe->mCubeIndex;
    probe->mRetainedComplete = probe->mComplete;

    probe->mCubeArray = nullptr;
    probe->mCubeIndex = -1;
    probe->mComplete = false;
    probe->mFadeIn = 0;
    // Cleared with the rest of the render.
    probe->mOccluded = false;
    probe->mOccludedResults = 0;
}

bool LLReflectionMapManager::evictCubeIndex(U32 budget)
{
    LLReflectionMap* victim = nullptr;

    // mProbes is sorted placed-first then by distance. 
    // We most certainly never want to kill manually placed probes over all the autoprobes.
    for (U32 i = (U32)mProbes.size(); i-- > budget && i > 0; )
    {
        LLReflectionMap* probe = mProbes[i];
        if (probe->mCubeIndex == -1 || probe == mUpdatingProbe)
        {
            continue;
        }

        if (!victim)
        {
            victim = probe;
        }

        if (!probe->mInView)
        {
            victim = probe;
            break;
        }
    }

    if (!victim)
    {
        return false;
    }

    releaseCubeIndex(victim, "slot evicted (over budget)");
    return true;
}
// </FS:Trish>

void LLReflectionMapManager::deleteProbe(U32 i)
{
    LL_PROFILE_ZONE_SCOPED_CATEGORY_DISPLAY;
    LLReflectionMap* probe = mProbes[i];

    llassert(probe != mDefaultProbe);

    if (probe->mCubeIndex != -1)
    { // mark the cube index used by this probe as being free
        mCubeFree.push_back(probe->mCubeIndex);
    }
    // <FS:Trish> 
    for (auto& owner : mSlotOwner)
    {
        if (owner == probe)
        {
            owner = nullptr;
        }
    }
    // </FS:Trish>
    if (mUpdatingProbe == probe)
    {
        mUpdatingProbe = nullptr;
        mUpdatingFace = 0;
    }
    // <FS:Trish> Its partial realtime faces belong to nobody now
    if (mRealtimeProbe == probe)
    {
        mRealtimeProbe = nullptr;
        mRealtimeFace = 0;
    }
    // </FS:Trish>

    // remove from any Neighbors lists
    for (auto& other : probe->mNeighbors)
    {
        auto const & iter = std::find(other->mNeighbors.begin(), other->mNeighbors.end(), probe);
        llassert(iter != other->mNeighbors.end());
        other->mNeighbors.erase(iter);
    }

    // Probes are distance sorted, order matters.
    mProbes.erase(mProbes.begin() + i);
}


void LLReflectionMapManager::doProbeUpdate()
{
    LL_PROFILE_ZONE_SCOPED_CATEGORY_DISPLAY;
    llassert(mUpdatingProbe != nullptr);

    updateProbeFace(mUpdatingProbe, mUpdatingFace);

    bool debug_updates = gPipeline.hasRenderDebugMask(LLPipeline::RENDER_DEBUG_PROBE_UPDATES) && mUpdatingProbe->mViewerObject;

    if (++mUpdatingFace == 6)
    {
        if (debug_updates)
        {
            mUpdatingProbe->mViewerObject->setDebugText(llformat("%.1f", (F32)gFrameTimeSeconds), LLColor4(1, 1, 1, 1));
        }
        updateNeighbors(mUpdatingProbe);
        mUpdatingFace = 0;
        if (isRadiancePass())
        {
            mUpdatingProbe->mComplete = true;
            mUpdatingProbe = nullptr;
            mRadiancePass = false;
        }
        else
        {
            mRadiancePass = true;
        }
    }
    else if (debug_updates)
    {
        mUpdatingProbe->mViewerObject->setDebugText(llformat("%.1f", (F32)gFrameTimeSeconds), LLColor4(1, 1, 0, 1));
    }
}

// Do the reflection map update render passes.
// For every 12 calls of this function, one complete reflection probe radiance map and irradiance map is generated
// First six passes render the scene with direct lighting only into a scratch space cube map at the end of the cube map array and generate
// a simple mip chain (not convolution filter).
// At the end of these passes, an irradiance map is generated for this probe and placed into the irradiance cube map array at the index for this probe
// The next six passes render the scene with both radiance and irradiance into the same scratch space cube map and generate a simple mip chain.
// At the end of these passes, a radiance map is generated for this probe and placed into the radiance cube map array at the index for this probe.
// In effect this simulates single-bounce lighting.
void LLReflectionMapManager::updateProbeFace(LLReflectionMap* probe, U32 face)
{
    LL_PROFILE_ZONE_SCOPED_CATEGORY_DISPLAY;
    LL_PROFILE_GPU_ZONE("probe update");
    // hacky hot-swap of camera specific render targets
    gPipeline.mRT = &gPipeline.mAuxillaryRT;

    mLightScale = 1.f;
    static LLCachedControl<F32> max_local_light_ambiance(gSavedSettings, "RenderReflectionProbeMaxLocalLightAmbiance", 8.f);
    if (!isRadiancePass() && probe->getAmbiance() > max_local_light_ambiance)
    {
        mLightScale = max_local_light_ambiance / probe->getAmbiance();
    }

    if (probe == mDefaultProbe)
    {
        touch_default_probe(probe);

        gPipeline.pushRenderTypeMask();

        //only render sky, water, terrain, and clouds
        gPipeline.andRenderTypeMask(LLPipeline::RENDER_TYPE_SKY, LLPipeline::RENDER_TYPE_WL_SKY,
            LLPipeline::RENDER_TYPE_WATER, LLPipeline::RENDER_TYPE_VOIDWATER, LLPipeline::RENDER_TYPE_CLOUDS, LLPipeline::RENDER_TYPE_TERRAIN, LLPipeline::END_RENDER_TYPES);

        probe->update(mRenderTarget.getWidth(), face);

        gPipeline.popRenderTypeMask();
    }
    else
    {
        llassert(mRenderReflectionProbeLevel > 0); // should never update a probe that's not the default probe if reflection coverage is none
        probe->update(mRenderTarget.getWidth(), face);
    }

    gPipeline.mRT = &gPipeline.mMainRT;

    S32 sourceIdx = mReflectionProbeCount;

    //if (probe != mUpdatingProbe)
    if (probe != mUpdatingProbe || mRealtimeUpdating) // <FS:Trish/> spread realtime faces must stay in their own scratch cube
    { // this is the "realtime" probe that's updating every frame, use the secondary scratch space channel
        sourceIdx += 1;
    }

    gGL.setColorMask(true, true);
    LLGLDepthTest depth(GL_FALSE, GL_FALSE);
    LLGLDisable cull(GL_CULL_FACE);
    LLGLDisable blend(GL_BLEND);

    // downsample to placeholder map
    {
        gGL.matrixMode(gGL.MM_MODELVIEW);
        gGL.pushMatrix();
        gGL.loadIdentity();

        gGL.matrixMode(gGL.MM_PROJECTION);
        gGL.pushMatrix();
        gGL.loadIdentity();

        gGL.flush();
        U32 res = mProbeResolution * 2;

        LLRenderTarget* screen_rt = &gPipeline.mAuxillaryRT.screen;
        static LLStaticHashedString sResScale("resScale");
        static LLStaticHashedString sDirection("direction");

        // perform a gaussian blur on the super sampled render before downsampling
        {
            gGaussianProgram.bind();
            gGaussianProgram.uniform1f(sResScale, 1.f / (mProbeResolution * 2));
            S32 diffuseChannel = gGaussianProgram.enableTexture(LLShaderMgr::DEFERRED_DIFFUSE);

            // horizontal
            gGaussianProgram.uniform2f(sDirection, 1.f, 0.f);
            gGL.getTexUnit(diffuseChannel)->bind(screen_rt);
            mRenderTarget.bindTarget();
            gPipeline.mScreenTriangleVB->setBuffer();
            gPipeline.mScreenTriangleVB->drawArrays(LLRender::TRIANGLES, 0, 3);
            mRenderTarget.flush();

            // vertical
            gGaussianProgram.uniform2f(sDirection, 0.f, 1.f);
            gGL.getTexUnit(diffuseChannel)->bind(&mRenderTarget);
            screen_rt->bindTarget();
            gPipeline.mScreenTriangleVB->setBuffer();
            gPipeline.mScreenTriangleVB->drawArrays(LLRender::TRIANGLES, 0, 3);
            screen_rt->flush();
        }


        S32 mips = (S32)(log2((F32)mProbeResolution) + 0.5f);

        gReflectionMipProgram.bind();
        S32 diffuseChannel = gReflectionMipProgram.enableTexture(LLShaderMgr::DEFERRED_DIFFUSE);

        for (int i = 0; i < mMipChain.size(); ++i)
        {
            LL_PROFILE_GPU_ZONE("probe mip");
            mMipChain[i].bindTarget();
            if (i == 0)
            {
                gGL.getTexUnit(diffuseChannel)->bind(screen_rt);
            }
            else
            {
                gGL.getTexUnit(diffuseChannel)->bind(&(mMipChain[i - 1]));
            }


            gReflectionMipProgram.uniform1f(sResScale, 1.f/(mProbeResolution*2));

            gPipeline.mScreenTriangleVB->setBuffer();
            gPipeline.mScreenTriangleVB->drawArrays(LLRender::TRIANGLES, 0, 3);

            res /= 2;

            GLint mip = i - (static_cast<GLint>(mMipChain.size()) - mips);

            if (mip >= 0)
            {
                LL_PROFILE_GPU_ZONE("probe mip copy");
                mTexture->bind(0);
                //glCopyTexSubImage3D(GL_TEXTURE_CUBE_MAP_ARRAY, mip, 0, 0, probe->mCubeIndex * 6 + face, 0, 0, res, res);
                glCopyTexSubImage3D(GL_TEXTURE_CUBE_MAP_ARRAY, mip, 0, 0, sourceIdx * 6 + face, 0, 0, res, res);
                //if (i == 0)
                //{
                    //glCopyTexSubImage3D(GL_TEXTURE_CUBE_MAP_ARRAY, mip, 0, 0, probe->mCubeIndex * 6 + face, 0, 0, res, res);
                //}
                mTexture->unbind();
            }
            mMipChain[i].flush();
        }

        gGL.popMatrix();
        gGL.matrixMode(gGL.MM_MODELVIEW);
        gGL.popMatrix();

        gGL.getTexUnit(diffuseChannel)->unbind(LLTexUnit::TT_TEXTURE);
        gReflectionMipProgram.unbind();
    }

    if (face == 5)
    {
        if (isRadiancePass())
        {
            mMipChain[0].bindTarget();

            //generate radiance map (even if this is not the irradiance map, we need the mip chain for the irradiance map)
            gRadianceGenProgram.bind();
            mVertexBuffer->setBuffer();

            static LLStaticHashedString sSourceIdx("sourceIdx");
            static LLStaticHashedString sMipLevel("mipLevel");
            static LLStaticHashedString sRoughness("roughness");
            static LLStaticHashedString sWidth("u_width");
            S32 channel = gRadianceGenProgram.enableTexture(LLShaderMgr::REFLECTION_PROBES, LLTexUnit::TT_CUBE_MAP_ARRAY);
            mTexture->bind(channel);
            gRadianceGenProgram.uniform1i(sSourceIdx, sourceIdx);
            gRadianceGenProgram.uniform1f(LLShaderMgr::REFLECTION_PROBE_MAX_LOD, mMaxProbeLOD);
            gRadianceGenProgram.uniform1f(LLShaderMgr::REFLECTION_PROBE_STRENGTH, 1.f);

            U32 res = mMipChain[0].getWidth();

            for (int i = 0; i < mMipChain.size(); ++i)
            {
                LL_PROFILE_GPU_ZONE("probe radiance gen");

                gRadianceGenProgram.uniform1f(sRoughness, (F32)i / (F32)(mMipChain.size() - 1));
                gRadianceGenProgram.uniform1f(sMipLevel, (GLfloat)i);
                gRadianceGenProgram.uniform1i(sWidth, mProbeResolution);

                for (int cf = 0; cf < 6; ++cf)
                { // for each cube face
                    LLCoordFrame frame;
                    frame.lookAt(LLVector3(0, 0, 0), LLCubeMapArray::sClipToCubeLookVecs[cf], LLCubeMapArray::sClipToCubeUpVecs[cf]);

                    F32 mat[16];
                    frame.getOpenGLRotation(mat);
                    gGL.loadMatrix(mat);

                    mVertexBuffer->drawArrays(gGL.TRIANGLE_STRIP, 0, 4);

                    glCopyTexSubImage3D(GL_TEXTURE_CUBE_MAP_ARRAY, i, 0, 0, probe->mCubeIndex * 6 + cf, 0, 0, res, res);
                }

                if (i != mMipChain.size() - 1)
                {
                    res /= 2;
                    glViewport(0, 0, res, res);
                }
            }

            gRadianceGenProgram.unbind();
            mMipChain[0].flush();
        }
        else
        {
            LL_PROFILE_GPU_ZONE("probe sh project");

            // Nine coefficients replace the 16x16x6 irradiance cubemap this used to convolve.
            // The old pass re-integrated the environment once per output texel; this integrates
            // it once, into the only nine numbers irradiance actually has.
            //
            // Integrate a mip whose faces are mSHProjectionRes, not mip 0. Averaging texels
            // before an integral does not change the integral, and the target basis cannot
            // represent anything the finer mip would add.
            S32 sh_mip = 0;
            U32 sh_res = mProbeResolution;
            while (sh_res > mSHProjectionRes && (size_t)(sh_mip + 1) < mMipChain.size())
            {
                sh_res >>= 1;
                ++sh_mip;
            }

            gSHProjectionProgram.bind();
            static LLStaticHashedString sSourceIdx("sourceIdx");
            static LLStaticHashedString sMipLevel("mipLevel");
            static LLStaticHashedString sWidth("u_width");
            S32 channel = gSHProjectionProgram.enableTexture(LLShaderMgr::REFLECTION_PROBES, LLTexUnit::TT_CUBE_MAP_ARRAY);
            mTexture->bind(channel);

            gSHProjectionProgram.uniform1i(sSourceIdx, sourceIdx);
            gSHProjectionProgram.uniform1f(sMipLevel, (GLfloat)sh_mip);
            gSHProjectionProgram.uniform1i(sWidth, (S32)sh_res);

            mSHCoeffs.bindTarget();
            // This probe owns one row and must not disturb any other, so the viewport is the
            // write mask -- the target is never cleared.
            glViewport(0, probe->mCubeIndex, LL_SH_COEFF_COUNT, 1);

            mVertexBuffer->setBuffer();
            mVertexBuffer->drawArrays(gGL.TRIANGLE_STRIP, 0, 4);

            mSHCoeffs.flush();

            gSHProjectionProgram.unbind();
        }
    }
}

void LLReflectionMapManager::reset()
{
    mReset = true;
}

void LLReflectionMapManager::pause(F32 duration)
{
    mPaused = true;
    mResumeTime = gFrameTimeSeconds + duration;
}

void LLReflectionMapManager::resume()
{
    mPaused = false;
}

void LLReflectionMapManager::shift(const LLVector4a& offset)
{
    for (auto& probe : mProbes)
    {
        probe->mOrigin.add(offset);

        // Carried along, because a rigid translation of every probe at once leaves the neighbour
        // graph describing exactly the same geometry. Without this a region crossing would mark
        // all of them stale on the same frame and rebuild the whole graph against itself.
        if (probe->mNeighborRadius >= 0.f)
        {
            probe->mNeighborOrigin.add(offset);
        }

        probe->mGroupCenter.add(offset); // <FS:Trish/> octree nodes shift with it
    }
}

void LLReflectionMapManager::updateNeighbors(LLReflectionMap* probe)
{
    LL_PROFILE_ZONE_SCOPED_CATEGORY_DISPLAY;

    // Recorded whether or not there is a list to build. The default probe never has neighbours,
    // and without this it would report itself stale on every frame forever.
    probe->mNeighborOrigin = probe->mOrigin;
    probe->mNeighborRadius = probe->mRadius;

    if (mDefaultProbe == probe)
    {
        return;
    }

    //remove from existing neighbors
    {
        LL_PROFILE_ZONE_NAMED_CATEGORY_DISPLAY("rmmun - clear");

        for (auto& other : probe->mNeighbors)
        {
            auto const & iter = std::find(other->mNeighbors.begin(), other->mNeighbors.end(), probe);
            llassert(iter != other->mNeighbors.end()); // <--- bug davep if this ever happens, something broke badly
            other->mNeighbors.erase(iter);
        }

        probe->mNeighbors.clear();
    }

    // search for new neighbors
    if (probe->isRelevant())
    {
        LL_PROFILE_ZONE_NAMED_CATEGORY_DISPLAY("rmmun - search");
        for (auto& other : mProbes)
        {
            if (other != mDefaultProbe && other != probe)
            {
                if (other->isRelevant() && probe->intersects(other))
                {
                    probe->mNeighbors.push_back(other);
                    other->mNeighbors.push_back(probe);
                }
            }
        }
    }
}

void LLReflectionMapManager::updateUniforms()
{
    if (!LLPipeline::sReflectionProbesEnabled)
    {
        return;
    }

    LL_PROFILE_ZONE_SCOPED_CATEGORY_DISPLAY;
    LL_PROFILE_GPU_ZONE("rmmu - uniforms")


    // Have active manual probes live-track the object they're associated with, BEFORE anything
    // derives from their origin or radius.
    //
    // getReflectionMaps takes mMinDepth/mMaxDepth from these values and sorts the array on
    // mMinDepth, and refBucket -- which is the shader's lower bound for its scan -- is only
    // valid while that sort holds. Refreshing afterwards, as this used to, described where the
    // probe was in the bucket and uploaded where it is in refSphere, and a probe whose bucket
    // entry no longer covers it is one the shader scans straight past. Manual probes are the
    // only ones that get this refresh, so they were the only ones that could vanish from it.
    for (auto& probe : mProbes)
    {
        probe->syncToViewerObject();
    }

    mReflectionMaps.resize(mReflectionProbeCount);
    getReflectionMaps(mReflectionMaps);

    for (int i = 0; i < 256; ++i)
    {
        mProbeData.refBucket[i][0] = mReflectionProbeCount;
        mProbeData.refBucket[i][1] = 0;
        mProbeData.refBucket[i][2] = mReflectionProbeCount;
        mProbeData.refBucket[i][3] = mReflectionProbeCount;
    }

    // load modelview matrix into matrix 4a
    LLMatrix4a modelview;
    modelview.loadu(gGLModelView);
    LLVector4a oa; // scratch space for transformed origin

    S32 count = 0;
    U32 nc = 0; // neighbor "cursor" - index into refNeighbor to start writing the next probe's list of neighbors

    LLEnvironment& environment = LLEnvironment::instance();
    LLSettingsSky::ptr_t psky = environment.getCurrentSky();

    static LLCachedControl<bool> should_auto_adjust(gSavedSettings, "RenderSkyAutoAdjustLegacy", false);
    F32 minimum_ambiance = psky->getReflectionProbeAmbiance(should_auto_adjust);

    bool is_ambiance_pass = gCubeSnapshot && !isRadiancePass();
    F32 ambscale = is_ambiance_pass ? 0.f : 1.f;
    ambscale *= mResetFade;
    ambscale = llmax(0, ambscale);
    F32 radscale = is_ambiance_pass ? 0.5f : 1.f;
    radscale *= mResetFade;
    radscale = llmax(0, radscale);

    for (auto* refmap : mReflectionMaps)
    {
        if (refmap == nullptr)
        {
            break;
        }

        if (refmap != mDefaultProbe)
        {
            // bucket search data
            // theory of operation:
            //      1. Determine minimum and maximum depth of each influence volume and store in mDepth (done in getReflectionMaps)
            //      2. Sort by minimum depth
            //      3. Prepare a bucket for each 1m of depth out to 256m
            //      4. Store the first and one-past-last probe indices for each bucket.
            //      5. Limit the shader's depth-sorted scan to that range. Neighbor lists may
            //         still refer to any active probe, including the overflow fallback.
            unsigned int depth_min = llclamp(llfloor(refmap->mMinDepth), 0, 255);
            unsigned int depth_max = llclamp(llfloor(refmap->mMaxDepth), 0, 255);
            for (U32 i = depth_min; i <= depth_max; ++i)
            {
                if (mProbeData.refBucket[i][0] == mReflectionProbeCount)
                {
                    mProbeData.refBucket[i][0] = refmap->mProbeIndex;
                }
                mProbeData.refBucket[i][1] = refmap->mProbeIndex + 1;
            }
        }

        llassert(refmap->mProbeIndex == count);
        llassert(mReflectionMaps[refmap->mProbeIndex] == refmap);

        llassert(refmap->mCubeIndex >= 0); // should always be  true, if not, getReflectionMaps is bugged

        modelview.affineTransform(refmap->mOrigin, oa);
        mProbeData.refSphere[count].set(oa.getF32ptr());
        mProbeData.refSphere[count].mV[3] = refmap->mRadius;

        mProbeData.refIndex[count][0] = refmap->mCubeIndex;
        llassert(nc % 4 == 0);
        mProbeData.refIndex[count][1] = nc / 4;
        mProbeData.refIndex[count][3] = refmap->mPriority;

        // for objects that are reflection probes, use the volume as the influence volume of the probe
        // only possibile influence volumes are boxes and spheres, so detect boxes and treat everything else as spheres
        if (refmap->getBox(mProbeData.refBox[count]))
        { // negate priority to indicate this probe has a box influence volume
            // Negated from at least 1, because -0 and 0 are the same integer: a box probe that
            // reached here with priority 0 would be read back as an automatic SPHERE, losing
            // both its shape and its precedence. A box influence volume only exists on a probe
            // attached to a viewer object, which is a manual probe by definition.
            mProbeData.refIndex[count][3] = -llmax(mProbeData.refIndex[count][3], 1);
        }

        mProbeData.refParams[count].set(
            llmax(minimum_ambiance, refmap->getAmbiance())*ambscale, // ambiance scale
            radscale, // radiance scale
            refmap->mFadeIn, // fade in weight
            0.f); // unused

        S32 ni = nc; // neighbor ("index") - index into refNeighbor to write indices for current reflection probe's neighbors
        bool neighbors_truncated = false;
        {
            //LL_PROFILE_ZONE_NAMED_CATEGORY_DISPLAY("rmmsu - refNeighbors");
            //pack neghbor list
            const U32 max_neighbors = 64;
            U32 neighbor_count = 0;

            for (auto& neighbor : refmap->mNeighbors)
            {
                // mProbeIndex alone decides this. getReflectionMaps clears it on every probe it
                // leaves out, so it already carries occlusion, completeness and the cube slot,
                // and it is the only one of those that agrees with the list the shader will
                // actually index. Re-testing mOccluded here would disagree with it during a
                // cube snapshot, where occlusion measured from the main camera is ignored:
                // the probe would be in the scan list and unreachable as a neighbour.
                GLint idx = neighbor->mProbeIndex;
                if (idx == -1)
                {
                    continue;
                }

                if (ni >= 4096 || neighbor_count >= max_neighbors)
                {
                    neighbors_truncated = true;
                    break;
                }

                // this neighbor may be sampled
                mProbeData.refNeighbor[ni++] = idx;

                neighbor_count++;
            }
        }

        // A partial list cannot justify ending the shader's probe search. Use a negative
        // count to request a full scan without enlarging the uniform block.
        mProbeData.refIndex[count][2] = neighbors_truncated ? -1 : ni - nc;

        if (nc == ni)
        {
            //no neighbors, tag as empty
            mProbeData.refIndex[count][1] = -1;
        }
        else
        {
            // move the cursor forward
            nc = ni;
            if (nc % 4 != 0)
            { // jump to next power of 4 for compatibility with ivec4
                nc += 4 - (nc % 4);
            }
        }

        count++;
    }

#if 0
    {
        // fill in gaps in refBucket
        S32 probe_idx = mReflectionProbeCount;

        for (int i = 0; i < 256; ++i)
        {
            if (i < count)
            { // for debugging, store depth of mReflectionsMaps[i]
                rpd.refBucket[i][1] = (S32) (mReflectionMaps[i]->mDepth * 10);
            }

            if (rpd.refBucket[i][0] == mReflectionProbeCount)
            {
                rpd.refBucket[i][0] = probe_idx;
            }
            else
            {
                probe_idx = rpd.refBucket[i][0];
            }
        }
    }
#endif

    mProbeData.refmapCount = count;

    gPipeline.mHeroProbeManager.updateUniforms();

    // Get the hero data.

    mProbeData.heroBox = gPipeline.mHeroProbeManager.mHeroData.heroBox;
    mProbeData.heroSphere = gPipeline.mHeroProbeManager.mHeroData.heroSphere;
    mProbeData.heroShape  = gPipeline.mHeroProbeManager.mHeroData.heroShape;
    mProbeData.heroMipCount   = gPipeline.mHeroProbeManager.mHeroData.heroMipCount;
    mProbeData.heroProbeCount = gPipeline.mHeroProbeManager.mHeroData.heroProbeCount;

    // Copy mProbeData into the uniform buffer object.
    if (mUBO == 0)
    {
        glGenBuffers(1, &mUBO);
    }
    glBindBuffer(GL_UNIFORM_BUFFER, mUBO);
    glBufferData(GL_UNIFORM_BUFFER, sizeof(ReflectionProbeData), &mProbeData, GL_STREAM_DRAW);
    glBindBuffer(GL_UNIFORM_BUFFER, 0);

#if 0
    if (!gCubeSnapshot)
    {
        for (auto& probe : mProbes)
        {
            LLViewerObject* vobj = probe->mViewerObject;
            if (vobj)
            {
                F32 time = (F32)gFrameTimeSeconds - probe->mLastUpdateTime;
                vobj->setDebugText(llformat("%d/%d/%d/%.1f - %.1f/%.1f", probe->mCubeIndex, probe->mProbeIndex, (U32) probe->mNeighbors.size(), probe->mMinDepth, probe->mMaxDepth, time), time > 1.f ? LLColor4::white : LLColor4::green);
            }
        }
    }
#endif
}

void LLReflectionMapManager::setUniforms()
{
    if (!LLPipeline::sReflectionProbesEnabled)
    {
        return;
    }

    if (mUBO == 0)
    {
        updateUniforms();
    }
    glBindBufferBase(GL_UNIFORM_BUFFER, LLGLSLShader::UB_REFLECTION_PROBES, mUBO);
}


void renderReflectionProbe(LLReflectionMap* probe)
{
    if (probe->isRelevant())
    {
        F32* po = probe->mOrigin.getF32ptr();

        //draw orange line from probe to neighbors
        gGL.flush();
        gGL.diffuseColor4f(1, 0.5f, 0, 1);
        gGL.begin(gGL.LINES);
        for (auto& neighbor : probe->mNeighbors)
        {
            if (probe->mViewerObject && neighbor->mViewerObject)
            {
                continue;
            }

            gGL.vertex3fv(po);
            gGL.vertex3fv(neighbor->mOrigin.getF32ptr());
        }
        gGL.end();
        gGL.flush();

        gGL.diffuseColor4f(1, 1, 0, 1);
        gGL.begin(gGL.LINES);
        for (auto& neighbor : probe->mNeighbors)
        {
            if (probe->mViewerObject && neighbor->mViewerObject)
            {
                gGL.vertex3fv(po);
                gGL.vertex3fv(neighbor->mOrigin.getF32ptr());
            }
        }
        gGL.end();
        gGL.flush();
    }

#if 0
    LLSpatialGroup* group = probe->mGroup;
    if (group)
    { // draw lines from corners of object aabb to reflection probe

        const LLVector4a* bounds = group->getBounds();
        LLVector4a o = bounds[0];

        gGL.flush();
        gGL.diffuseColor4f(0, 0, 1, 1);
        F32* c = o.getF32ptr();

        const F32* bc = bounds[0].getF32ptr();
        const F32* bs = bounds[1].getF32ptr();

        // daaw blue lines from corners to center of node
        gGL.begin(gGL.LINES);
        gGL.vertex3fv(c);
        gGL.vertex3f(bc[0] + bs[0], bc[1] + bs[1], bc[2] + bs[2]);
        gGL.vertex3fv(c);
        gGL.vertex3f(bc[0] - bs[0], bc[1] + bs[1], bc[2] + bs[2]);
        gGL.vertex3fv(c);
        gGL.vertex3f(bc[0] + bs[0], bc[1] - bs[1], bc[2] + bs[2]);
        gGL.vertex3fv(c);
        gGL.vertex3f(bc[0] - bs[0], bc[1] - bs[1], bc[2] + bs[2]);

        gGL.vertex3fv(c);
        gGL.vertex3f(bc[0] + bs[0], bc[1] + bs[1], bc[2] - bs[2]);
        gGL.vertex3fv(c);
        gGL.vertex3f(bc[0] - bs[0], bc[1] + bs[1], bc[2] - bs[2]);
        gGL.vertex3fv(c);
        gGL.vertex3f(bc[0] + bs[0], bc[1] - bs[1], bc[2] - bs[2]);
        gGL.vertex3fv(c);
        gGL.vertex3f(bc[0] - bs[0], bc[1] - bs[1], bc[2] - bs[2]);
        gGL.end();

        //draw yellow line from center of node to reflection probe origin
        gGL.flush();
        gGL.diffuseColor4f(1, 1, 0, 1);
        gGL.begin(gGL.LINES);
        gGL.vertex3fv(c);
        gGL.vertex3fv(po);
        gGL.end();
        gGL.flush();
    }
#endif
}

void renderReflectionProbe(LLReflectionMap* probe,
                           std::map<LLSpatialGroup*, int> group_count,
                           std::map<LLViewerObject*, int> object_count,
                           std::map<F32*, int> location_count)
{
    renderReflectionProbe(probe);
    if (!probe || !probe->isRelevant())
    {
        return;
    }

    const bool duplicate = (probe->mGroup && group_count[probe->mGroup] > 1)
                        || (probe->mViewerObject && object_count[probe->mViewerObject] > 1)
                        || location_count[probe->mOrigin.getF32ptr()] > 1;
    const bool manual = probe->mViewerObject != nullptr;
    const bool automatic = probe->mGroup != nullptr && !manual;
    gGL.diffuseColor4f(manual ? 1.f : 0.f, automatic ? 0.f : 1.f, automatic ? 1.f : 0.f, 1.f);
    glPointSize(duplicate ? 18.f : 9.f);
    gGL.begin(gGL.POINTS);
    gGL.vertex3fv(probe->mOrigin.getF32ptr());
    gGL.end();
    gGL.flush();
}

void LLReflectionMapManager::renderDebug()
{
    gDebugProgram.bind();

    for (auto& probe : mProbes)
    {
        renderReflectionProbe(probe);
    }

    gDebugProgram.unbind();
}

void LLReflectionMapManager::initReflectionMaps()
{
    static LLCachedControl<U32> ref_probe_res(gSavedSettings, "RenderReflectionProbeResolution", 128U);
    U32 probe_resolution = nhpo2(llclamp(ref_probe_res(), (U32)64, (U32)512));
    // No irradiance resolution to size any more: irradiance is nine SH coefficients whatever
    // the environment looks like, so the setting that used to pick a cubemap edge length is gone.
    if (mTexture.isNull() || mReflectionProbeCount != mDynamicProbeCount || mProbeResolution != probe_resolution ||
        !mSHCoeffs.isComplete() || mReset)
    {
        if(mProbeResolution != probe_resolution)
        {
            mRenderTarget.release();
            mMipChain.clear();
        }

        gEXRImage = nullptr;
        mReset = false;
        mReflectionProbeCount = mDynamicProbeCount;
        mProbeResolution = probe_resolution;
        mMaxProbeLOD = log2f((F32)mProbeResolution) - 1.f; // number of mips - 1

        // Signed storage: the linear and quadratic bands are negative over half the sphere, so
        // none of the unsigned float formats the radiance chain uses can hold these.
        mSHCoeffs.release();
        mSHCoeffs.allocate(LL_SH_COEFF_COUNT, mReflectionProbeCount + 2, GL_RGBA16F);

        if (mTexture.isNull() ||
            mTexture->getWidth() != mProbeResolution ||
            mReflectionProbeCount + 2 != mTexture->getCount())
        {
#if 0 // Cubemap copy critically flawed and overflows
            if (mTexture)
            {
                mTexture = new LLCubeMapArray(*mTexture, mProbeResolution, mReflectionProbeCount + 2);
            }
            else
#endif
            {
                mTexture = new LLCubeMapArray();

                static LLCachedControl<bool> render_hdr(gSavedSettings, "RenderHDREnabled", true);

                // store mReflectionProbeCount+2 cube maps, final two cube maps are used for render target and radiance map generation
                // source)
                mTexture->allocate(mProbeResolution, 3, mReflectionProbeCount + 2, true, render_hdr);
            }
        }

        // reset probe state
        mUpdatingFace = 0;
        mUpdatingProbe = nullptr;
        mRadiancePass = false;
        mRealtimeRadiancePass = false;
        // <FS:Trish>
        mRealtimeProbe = nullptr;
        mRealtimeFace = 0;
        // </FS:Trish>

        // if default probe already exists, remember whether or not it's complete (SL-20498)
        bool default_complete = mDefaultProbe.isNull() ? false : mDefaultProbe->mComplete;

        for (auto& probe : mProbes)
        {
            probe->mLastUpdateTime = 0.f;
            probe->mComplete = false;
            probe->mProbeIndex = -1;
            probe->mCubeArray = nullptr;
            probe->mCubeIndex = -1;
            probe->mNeighbors.clear();
            probe->mNeighborRadius = -1.f; // list cleared, so it describes nothing
            probe->mFadeIn = 0;
            // Both of these decide whether a probe is allowed to render, so a reset that left
            // them standing could not clear a probe that was stuck on either -- and this is the
            // path a teleport, a sky change or a probe-count change goes through, which is
            // exactly where someone would expect to get their probes back.
            probe->mOccluded = false;
            probe->mInsideManualProbe = false;
            probe->mOccludedResults = 0; // <FS:Trish/>
        }

        // <FS:Trish> The cube array may have just been reallocated
        clearSlotOwners();
        if (cullLogEnabled())
        {
            LL_INFOS("ProbeCull") << "hard reset: all " << mProbes.size() << " probes dropped (" << mReflectionProbeCount << " slots)" << LL_ENDL;
        }
        // </FS:Trish>

        mCubeFree.clear();
        initCubeFree();

        if (mDefaultProbe.isNull())
        {
            llassert(mProbes.empty()); // default probe MUST be the first probe created
            mDefaultProbe = new LLReflectionMap();
            mProbes.push_back(mDefaultProbe);
        }

        llassert(mProbes[0] == mDefaultProbe);

        mDefaultProbe->mCubeIndex = 0;
        mDefaultProbe->mCubeArray = mTexture;
        mDefaultProbe->mDistance = 64.f;
        mDefaultProbe->mRadius = 4096.f;
        mDefaultProbe->mProbeIndex = 0;
        mDefaultProbe->mComplete = default_complete;

        touch_default_probe(mDefaultProbe);
    }

    if (mVertexBuffer.isNull())
    {
        U32 mask = LLVertexBuffer::MAP_VERTEX;
        LLPointer<LLVertexBuffer> buff = new LLVertexBuffer(mask);
        buff->allocateBuffer(4, 0);

        LLStrider<LLVector3> v;

        buff->getVertexStrider(v);

        // NOTE: z=-1 is load-bearing. radianceGenV/irradianceGenV use this position for BOTH
        // the clip-space gl_Position AND (rotated by modelview) the cubemap SAMPLE DIRECTION,
        // so z is the forward axis of the convolution -- do NOT flatten it. The clip-volume
        // conflict under reverse-Z (z=-1 is outside ZERO_TO_ONE) is resolved in the shaders,
        // which remap only the output clip z while keeping the direction from this raw z=-1.
        v[0] = LLVector3(-1, -1, -1);
        v[1] = LLVector3(1, -1, -1);
        v[2] = LLVector3(-1, 1, -1);
        v[3] = LLVector3(1, 1, -1);

        buff->unmapBuffer();

        mVertexBuffer = buff;
    }
}

void LLReflectionMapManager::cleanup()
{
    mVertexBuffer = nullptr;
    mRenderTarget.release();

    mMipChain.clear();

    mTexture = nullptr;
    mSHCoeffs.release();

    mProbes.clear();
    mKillList.clear();
    mCreateList.clear();

    mReflectionMaps.clear();
    mUpdatingFace = 0;

    mDefaultProbe = nullptr;
    mUpdatingProbe = nullptr;
    // <FS:Trish>
    mRealtimeProbe = nullptr;
    mRealtimeFace = 0;
    // </FS:Trish>

    if (mUBO != 0)
    {
        glDeleteBuffers(1, &mUBO);
        mUBO = 0;
    }

    cleanupQueryPool();

    // note: also called on teleport (not just shutdown), so make sure we're in a good "starting" state
    initCubeFree();
}

void LLReflectionMapManager::cleanupQueryPool()
{
    if (!mQueryPool.empty())
    {
        LL_PROFILE_ZONE_NAMED_CATEGORY_DISPLAY("cleanup query pool");
        std::vector<GLuint> queries(mQueryPool.begin(), mQueryPool.end());
        glDeleteQueries(static_cast<GLsizei>(queries.size()), queries.data());
        mQueryPool.clear();
    }
}

void LLReflectionMapManager::doOcclusion()
{
    LLVector4a eye;
    eye.load3(LLViewerCamera::instance().getOrigin().mV);

    for (auto& probe : mProbes)
    {
        if (probe.isNull() || probe == mDefaultProbe)
        {
            continue;
        }

        // A probe holding no cube data, or one relevance has switched off, cannot reach the
        // frame whatever a query says about it -- and each query costs a box draw, so this was
        // up to a couple of hundred of them per frame spent on probes with nothing to show.
        // Cleared rather than left to go stale: a probe that becomes eligible again should be
        // culled by a query taken while it had something to show, not by one taken before.
        if (probe->mCubeIndex == -1 || !probe->isRelevant())
        {
            probe->mOccluded = false;
            continue;
        }

        // <FS:Trish>
        //probe->doOcclusion(eye);
        probe->mInView = LLViewerCamera::getInstance()->sphereInFrustum(LLVector3(probe->mOrigin.getF32ptr()), probe->mRadius) != 0;
        probe->doOcclusion(eye, probe->mInView);
        // </FS:Trish>
    }
}

void LLReflectionMapManager::forceDefaultProbeAndUpdateUniforms(bool force)
{
    // Saved against the probe itself rather than against its position in mProbes. The list can
    // gain or lose entries between the two calls -- the create and kill lists are drained in
    // update(), and a probe can be dropped the moment nothing outside the manager holds it --
    // and an index-keyed restore then hands one probe's occlusion state to another. The
    // LLPointer also keeps every saved probe alive until the restore.
    static std::vector<std::pair<LLPointer<LLReflectionMap>, bool> > sSavedOcclusion;

    if (force)
    {
        llassert(sSavedOcclusion.empty());

        for (auto& probe : mProbes)
        {
            sSavedOcclusion.emplace_back(probe, probe->mOccluded);
            if (probe != mDefaultProbe)
            {
                probe->mOccluded = true;
            }
        }

        updateUniforms();
    }
    else
    {
        for (auto& saved : sSavedOcclusion)
        {
            saved.first->mOccluded = saved.second;
        }
        sSavedOcclusion.clear();
        sSavedOcclusion.shrink_to_fit();
    }
}

// <FS:Trish> Realtime probe faces spread over frames (FSPerfProbeRealtimeFacesPerFrame)
// Render the realtime probe a few faces per frame instead of all six. The faces collect in
// the realtime scratch cube, and only a finished cube is filtered into the probe's own slot
// (face 5 does that in updateProbeFace), so what the shaders sample is always a whole cube
// from one capture: there is no partly updated state to flicker.
void LLReflectionMapManager::updateRealtimeProbeSpread(LLReflectionMap* probe, U32 faces_per_frame)
{
    LL_PROFILE_ZONE_NAMED_CATEGORY_DISPLAY("rmmu - realtime spread");

    static LLVector4a sCaptureOrigin;

    // Finish a cube in progress before moving to another probe. Two dynamic probes at nearly
    // the same distance can swap places as "closest" from frame to frame, and restarting on
    // every swap would never complete a cube for either.
    if (mRealtimeFace != 0 && mRealtimeProbe && mRealtimeProbe != probe &&
        mRealtimeCubeIndex != -1 && mRealtimeProbe->mCubeIndex == mRealtimeCubeIndex &&
        mRealtimeProbe->getIsDynamic() && mRealtimeProbe->isRelevant())
    {
        probe = mRealtimeProbe;
    }

    // The scratch cube holds faces from earlier frames. They only belong together if they were
    // captured for this probe in this slot, so anything else starts the cube over.
    if (probe != mRealtimeProbe || probe->mCubeIndex != mRealtimeCubeIndex)
    {
        mRealtimeProbe = probe;
        mRealtimeCubeIndex = probe->mCubeIndex;
        mRealtimeFace = 0;
    }

    probe->autoAdjustOrigin();

    // Every face of one cube is captured from where the probe was when the cube began. A probe
    // riding a moving object would otherwise put each face at a different point and seam the
    // cube at the edges.
    if (mRealtimeFace == 0)
    {
        sCaptureOrigin = probe->mOrigin;
    }
    const LLVector4a live_origin = probe->mOrigin;
    probe->mOrigin = sCaptureOrigin;

    // store and override the value of "isRadiancePass", as the six-face path does
    const bool radiance_pass = isRadiancePass();
    mRadiancePass = mRealtimeRadiancePass;
    mRealtimeUpdating = true;

    const U32 faces = llclamp(faces_per_frame, 1U, 6U);
    for (U32 i = 0; i < faces; ++i)
    {
        updateProbeFace(probe, mRealtimeFace);
        if (++mRealtimeFace == 6)
        { // the cube is filtered; the next one is the other pass
            mRealtimeFace = 0;
            mRealtimeRadiancePass = !mRealtimeRadiancePass;
            break;
        }
    }

    mRealtimeUpdating = false;
    mRadiancePass = radiance_pass;
    probe->mOrigin = live_origin;
}
// </FS:Trish>
