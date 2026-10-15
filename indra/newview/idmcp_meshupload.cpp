/**
 * @file idmcp_meshupload.cpp
 * @brief <ID> MCP server: upload.mesh job.
 *
 * Part of Five's custom Firestorm fork. Custom code carries an `ID` prefix.
 *
 * Each job owns an LLModelPreview with no floater behind it: the job is the
 * preview's host (idmodelpreviewhost.h), supplying the upload options and
 * collecting the importer log. It loads the file, applies the options, then
 * calls gMeshRepo.uploadModel with its own fee/upload observers. Several jobs
 * run at once, and the Upload Model floater stays free for the user. An idle
 * callback moves every job through its stages, each with a time limit.
 *
 * Lifetime: sJobs owns the jobs. The fee observer methods run on the mesh
 * upload thread, which holds a raw pointer to the job, so a job is never
 * released while a fee or upload request is in flight, even after the MCP
 * call has been answered. Its preview is deleted once the call is answered
 * and no model is loading (deleting it mid-load would block on the loader).
 */

#include "llviewerprecompiledheaders.h"

#include "idmcp_meshupload.h"
#include "idmcpserver.h"

#include "llagent.h"                // gAgent
#include "llcallbacklist.h"         // gIdleCallbacks
#include "llcorehttputil.h"
#include "llcoros.h"
#include "llmeshrepository.h"       // gMeshRepo
#include "llmodel.h"
#include "llmodelloader.h"
#include "llmodelpreview.h"
#include "llmutex.h"
#include "llsdutil.h"               // llsd_clone
#include "llstatusbar.h"            // gStatusBar
#include "lltimer.h"
#include "lluploadfloaterobservers.h"
#include "llviewercontrol.h"        // gSavedSettings

#include <algorithm>
#include <atomic>
#include <deque>
#include <map>
#include <memory>
#include <set>
#include <sstream>
#include <utility>
#include <vector>

namespace
{
    constexpr F64 LOAD_LIMIT    = 120.0;
    constexpr F64 ANALYZE_LIMIT = 300.0;
    constexpr F64 QUOTE_LIMIT   = 60.0;
    constexpr F64 UPLOAD_LIMIT  = 300.0;
    // A request that never calls back would pin the job forever. After this
    // long past its stage deadline the job is parked in sParked instead:
    // leaked, never freed, so the raw pointer stays valid.
    constexpr F64 ABANDON_GRACE = 600.0;
    constexpr int SETTLE_TICKS  = 3;   // idle ticks the preview must stay idle
    constexpr size_t MAX_JOBS   = 4;   // each job holds a preview avatar and every LOD of its model
    constexpr size_t MAX_LOG_LINES = 500;
    // Nothing is drawn, but the preview is a dynamic texture and needs a size.
    constexpr S32 PREVIEW_TEXTURE_SIZE = 16;

    // The physics LOD for "high".."lowest" (the floater's physics_lod_combo).
    S32 physics_lod(const std::string& k)
    {
        if (k == "high")   return LLModel::LOD_HIGH;
        if (k == "medium") return LLModel::LOD_MEDIUM;
        if (k == "low")    return LLModel::LOD_LOW;
        return LLModel::LOD_IMPOSTOR;
    }

    // English text of the floater strings the preview reports
    // (floater_model_preview.xml).
    const char* lod_status_text(const std::string& name)
    {
        if (name == "mesh_status_submesh_mismatch")  return "Levels of detail have a different number of textureable faces.";
        if (name == "mesh_status_mesh_mismatch")     return "Levels of detail have a different number of mesh instances.";
        if (name == "mesh_status_too_many_vertices") return "Level of detail has too many vertices.";
        if (name == "mesh_status_missing_lod")       return "Missing required level of detail.";
        return nullptr;
    }

    const char* physics_status_text(U32 errors)
    {
        // Same priority as LLModelPreview::updateStatusMessages.
        if (errors & LLModelPreview::PHYSICS_ERROR_TOOMANYHULLS)       return "Some models exceed the hull limit (256); try 'simplify'.";
        if (errors & LLModelPreview::PHYSICS_ERROR_TOOMANYVERTSINHULL) return "Some physical hulls exceed the vertex limit (256); try an alternative 'Analyze Method'.";
        if (errors & LLModelPreview::PHYSICS_ERROR_DEGENERATE)         return "The physics mesh is too dense or contains degenerate triangles. Use Analyze/Simplify to resolve.";
        if (errors & LLModelPreview::PHYSICS_ERROR_TOOTHIN)            return "One or more dimensions is less than 0.5m, only hull-based (analyzed) physics shapes will work correctly.";
        return nullptr;
    }

    // The floater registers the progress callbacks (which let a request be
    // cancelled) and reads the parameter defaults when it is built. Do the
    // same here, so analyze works even if the floater has never been opened.
    // The decomposition thread fills mStageID itself. False when there is no
    // decomposer.
    bool decomp_setup(LLPhysicsDecomp::decomp_params& defaults)
    {
        LLConvexDecomposition* cd = LLConvexDecomposition::getInstance();
        if (!cd || !gMeshRepo.mDecompThread)
        {
            return false;
        }

        const LLCDStageData* stages = nullptr;
        const S32 stage_count = cd->getStages(&stages);
        for (S32 j = 0; j < stage_count; ++j)
        {
            cd->registerCallback(j, LLPhysicsDecomp::llcdCallback);
        }

        const LLCDParam* params = nullptr;
        const S32 param_count = cd->getParameters(&params);
        for (S32 i = 0; i < param_count; ++i)
        {
            switch (params[i].mType)
            {
            case LLCDParam::LLCD_FLOAT:
                defaults[params[i].mName] = LLSD(params[i].mDefault.mFloat);
                break;
            case LLCDParam::LLCD_INTEGER:
            case LLCDParam::LLCD_ENUM:
                defaults[params[i].mName] = LLSD(params[i].mDefault.mIntOrEnumValue);
                break;
            case LLCDParam::LLCD_BOOLEAN:
                defaults[params[i].mName] = LLSD(params[i].mDefault.mBool);
                break;
            default:
                break;
            }
        }
        return true;
    }
}

bool idmcp_mesh_is_physics_keyword(const std::string& k)
{
    return k == "none" || k == "high" || k == "medium" || k == "low" || k == "lowest" || k == "cube";
}

class IDMCPMeshUploadJob
    : public LLWholeModelFeeObserver
    , public LLWholeModelUploadObserver
    , public IDModelPreviewHost
    , public std::enable_shared_from_this<IDMCPMeshUploadJob>
{
public:
    static void start(const IDMCPMeshUploadParams& p, const IDMCPCallPtr& call);

    // The preview is deliberately not deleted here: this only runs with a
    // live preview at viewer exit, when deleting textures is no longer safe.
    ~IDMCPMeshUploadJob() override = default;

    // LLWholeModelFeeObserver: called on the mesh upload thread.
    void onModelPhysicsFeeReceived(const LLSD& result, std::string upload_url) override;
    void setModelPhysicsFeeErrorStatus(S32 status, const std::string& reason, const LLSD& result) override;

    // LLWholeModelUploadObserver: called on the main thread (doOnIdleOneTime).
    void onModelUploadSuccess() override;
    void onModelUploadSuccessWithResponse(const LLSD& response) override;
    void onModelUploadFailure() override;

    // IDModelPreviewHost
    IDModelPreviewSettings getPreviewSettings() override { return mSettings; }
    void addPreviewLog(const std::string& str, bool flash) override;
    void addPreviewLog(const std::string& message, const LLSD& args, bool flash, S32 lod) override;

private:
    enum class Stage { Loading, Configuring, Analyzing, Quoting, Uploading };
    enum class Reply { Pending, Ok, Failed };   // not "None": X11 #defines None
    enum class Perm  { Pending, Granted, Denied, Failed };

    // The floater's DecompRequest reports to the floater; this one reports
    // to its job.
    class DecompRequest : public LLPhysicsDecomp::Request
    {
    public:
        DecompRequest(const std::string& stage, LLModel* mdl, const LLPhysicsDecomp::decomp_params& params,
                      const std::weak_ptr<IDMCPMeshUploadJob>& job)
            : mModel(mdl), mJob(job)
        {
            mStage    = stage;
            mDecompID = &mdl->mDecompID;
            mParams   = params;
            assignData(mdl);
        }

        // Called on the decomposition thread.
        S32 statusCallback(const char*, S32, S32) override { return mContinue; }
        // Called on the main thread.
        void completed() override;

        std::atomic<S32>   mContinue{ 1 };
        LLPointer<LLModel> mModel;
        std::weak_ptr<IDMCPMeshUploadJob> mJob;
    };

    IDMCPMeshUploadJob(const IDMCPMeshUploadParams& p, const IDMCPCallPtr& call)
        : mParams(p), mCall(call) {}

    static void onIdle(void*);
    static S32  triangles(LLModelPreview* mp, S32 lod);
    static const char* stageName(Stage s);

    void tick();
    void enter(Stage s, F64 limit);
    bool settled();
    bool requestPending();
    void requestUploadPermission();

    void loadFile(S32 lod, const std::string& path);
    void loadNext();
    bool configure();
    bool startAnalyze();
    void onDecompCompleted(DecompRequest* req);
    void cancelDecomposition();
    void sendQuote();
    void handleQuote();
    void sendUpload(const std::string& url);
    void handleUpload();
    std::string uploadBlock(int& code, boost::json::object& data) const;

    boost::json::array logLines() const;
    boost::json::array statusLines() const;
    void fail(int code, const std::string& msg, boost::json::value data = boost::json::value());
    void succeed(boost::json::object result);
    void finish();
    bool deletePreviewWhenIdle();
    void cancel();
    void release();

    static std::vector<std::shared_ptr<IDMCPMeshUploadJob>> sJobs;
    static std::vector<std::shared_ptr<IDMCPMeshUploadJob>> sParked;

    IDMCPMeshUploadParams  mParams;
    IDMCPCallPtr           mCall;
    IDModelPreviewSettings mSettings;
    bool                   mLockScale = false;   // lock_scale_if_joint_position
    LLModelPreview*        mPreview = nullptr;   // owned; see deletePreviewWhenIdle()
    std::deque<std::string> mLog;

    LLPhysicsDecomp::decomp_params        mDecompParams;
    std::set<LLPointer<DecompRequest>>    mDecompRequests;

    Perm        mPerm = Perm::Pending;
    std::string mPermError;

    Stage mStage         = Stage::Loading;
    F64   mStageDeadline = 0.0;
    int   mSettleTicks   = 0;
    bool  mFinished      = false;   // call answered; only waiting on the loader or in-flight HTTP
    std::deque<std::pair<S32, std::string>> mPendingLoads;   // (LLModel lod, path)

    S32   mQuotedPrice = 0;
    F64   mLandImpact  = 0.0;

    LLMutex     mReplyMutex;        // guards everything below
    Reply       mFeeReply = Reply::Pending;
    LLSD        mFee;
    std::string mUploadUrl;
    std::string mFeeError;
    Reply       mUploadReply = Reply::Pending;
    LLSD        mUploadResponse;
};

std::vector<std::shared_ptr<IDMCPMeshUploadJob>> IDMCPMeshUploadJob::sJobs;
std::vector<std::shared_ptr<IDMCPMeshUploadJob>> IDMCPMeshUploadJob::sParked;

// ---- start ----------------------------------------------------------------

void IDMCPMeshUploadJob::start(const IDMCPMeshUploadParams& p, const IDMCPCallPtr& call)
{
    if (sJobs.size() >= MAX_JOBS)
    {
        idmcp_tool_err(call, IDMCP_ERR_BUSY,
                       llformat("%d mesh uploads are already running; retry when one finishes", (S32)sJobs.size()));
        return;
    }
    if (p.analyze && p.physics == "none")
    {
        idmcp_tool_err(call, IDMCP_ERR_INVALID_PARAMS, "analyze needs a physics shape, but physics is \"none\"");
        return;
    }
    if (p.joint_positions == 1 && p.skin_weights == 0)
    {
        idmcp_tool_err(call, IDMCP_ERR_INVALID_PARAMS, "rigged.joint_positions needs rigged.skin_weights");
        return;
    }

    std::shared_ptr<IDMCPMeshUploadJob> job(new IDMCPMeshUploadJob(p, call));
    if (p.analyze && !decomp_setup(job->mDecompParams))
    {
        idmcp_tool_err(call, IDMCP_ERR_CAP_UNAVAIL, "convex decomposition isn't available in this viewer build");
        return;
    }

    job->mSettings.name            = p.name;
    job->mSettings.import_scale    = p.scale;
    job->mSettings.upload_textures = p.textures;
    for (S32 lod = LLModel::LOD_IMPOSTOR; lod <= LLModel::LOD_MEDIUM; ++lod)
    {
        if (!p.lod_file[lod].empty())
        {
            job->mPendingLoads.emplace_back(lod, p.lod_file[lod]);
        }
    }
    if (!idmcp_mesh_is_physics_keyword(p.physics))
    {
        job->mPendingLoads.emplace_back(LLModel::LOD_PHYSICS, p.physics);
    }

    job->mPreview = new LLModelPreview(PREVIEW_TEXTURE_SIZE, PREVIEW_TEXTURE_SIZE, job.get(), nullptr);

    sJobs.push_back(job);
    if (sJobs.size() == 1)
    {
        gIdleCallbacks.addFunction(&IDMCPMeshUploadJob::onIdle, nullptr);
    }
    std::weak_ptr<IDMCPMeshUploadJob> weak = job;
    call->setCleanup([weak]
    {
        if (auto j = weak.lock()) j->cancel();
    });

    job->requestUploadPermission();
    job->loadFile(LLModel::LOD_HIGH, p.path);
    job->enter(Stage::Loading, LOAD_LIMIT);
}

void idmcp_mesh_upload_start(const IDMCPMeshUploadParams& p, const IDMCPCallPtr& call)
{
    IDMCPMeshUploadJob::start(p, call);
}

// Same request as LLFloaterModelUploadBase::requestAgentUploadPermissions.
void IDMCPMeshUploadJob::requestUploadPermission()
{
    const std::string url = gAgent.getRegionCapability("MeshUploadFlag");
    if (url.empty())
    {
        // The floater lets the upload through when the region lacks the cap.
        mPerm = Perm::Granted;
        return;
    }

    std::weak_ptr<IDMCPMeshUploadJob> weak = shared_from_this();
    LLCoros::instance().launch("IDMCPMeshUploadJob::requestUploadPermission", [weak, url]()
    {
        LLCore::HttpRequest::policy_t policy(LLCore::HttpRequest::DEFAULT_POLICY_ID);
        LLCoreHttpUtil::HttpCoroutineAdapter::ptr_t adapter =
            std::make_shared<LLCoreHttpUtil::HttpCoroutineAdapter>("MeshUploadFlag", policy);
        LLCore::HttpRequest::ptr_t request = std::make_shared<LLCore::HttpRequest>();

        LLSD result = adapter->getAndSuspend(request, url);
        LLCore::HttpStatus status =
            LLCoreHttpUtil::HttpCoroutineAdapter::getStatusFromLLSD(result[LLCoreHttpUtil::HttpCoroutineAdapter::HTTP_RESULTS]);

        std::shared_ptr<IDMCPMeshUploadJob> job = weak.lock();
        if (!job)
        {
            return;
        }
        if (!status)
        {
            job->mPerm      = Perm::Failed;
            job->mPermError = status.toString();
            return;
        }
        // Same test as LLFloaterModelPreview::onPermissionsReceived.
        const std::string upload_status = result["mesh_upload_status"].asString();
        job->mPerm = (upload_status.empty() || upload_status == "valid") ? Perm::Granted : Perm::Denied;
    });
}

// ---- idle driver ----------------------------------------------------------

void IDMCPMeshUploadJob::onIdle(void*)
{
    // Copy: tick() can release jobs, which edits sJobs.
    const std::vector<std::shared_ptr<IDMCPMeshUploadJob>> jobs = sJobs;
    for (const auto& job : jobs)
    {
        job->tick();
    }
}

void IDMCPMeshUploadJob::tick()
{
    const F64 now = LLTimer::getTotalSeconds();

    if (mFinished)
    {
        if (!deletePreviewWhenIdle())
        {
            return;
        }
        if (!requestPending())
        {
            release();
        }
        else if (now > mStageDeadline + ABANDON_GRACE)
        {
            LL_WARNS("IDMCP") << "upload.mesh: request never called back; parking job" << LL_ENDL;
            sParked.push_back(shared_from_this());
            release();
        }
        return;
    }

    // A dropped connection never runs the call's cleanup, so check for it here.
    if (!mCall->connected())
    {
        cancel();
        return;
    }

    // Once sent, the upload thread works from its own copy of the instance
    // list, so the upload may still complete (and charge) after a timeout.
    if (mStage == Stage::Uploading)
    {
        if (now > mStageDeadline)
        {
            boost::json::object data;
            data["upload_sent"] = true;
            fail(IDMCP_ERR_TIMEOUT,
                 "the upload was sent but no reply arrived in time; it may still complete. Check inventory and balance before retrying",
                 std::move(data));
            return;
        }
        handleUpload();
        return;
    }

    if (now > mStageDeadline)
    {
        fail(IDMCP_ERR_TIMEOUT, std::string("timed out while ") + stageName(mStage));
        return;
    }

    LLModelPreview* mp = mPreview;
    // The loader thread sets the error state before its callback reaches the
    // main thread. Wait for that callback (it clears mModelLoader) before
    // reacting, so the preview is never deleted while it still loads.
    if (!mp->mModelLoader && mp->getLoadState() >= LLModelLoader::ERROR_PARSING)
    {
        boost::json::object data;
        data["log"] = logLines();
        fail(IDMCP_ERR_INVALID_PARAMS, "the model failed to load", std::move(data));
        return;
    }

    // The floater's draw() does this for its own preview.
    mp->update();

    switch (mStage)
    {
    case Stage::Loading:
        if (!settled()) return;
        if (!mPendingLoads.empty())
        {
            loadNext();
            return;
        }
        if (configure())
        {
            enter(Stage::Configuring, LOAD_LIMIT);
        }
        return;

    case Stage::Configuring:
        if (!settled()) return;
        if (mParams.analyze)
        {
            if (startAnalyze())
            {
                enter(Stage::Analyzing, ANALYZE_LIMIT);
            }
            return;
        }
        sendQuote();
        return;

    case Stage::Analyzing:
        if (!mDecompRequests.empty() || !settled()) return;
        sendQuote();
        return;

    case Stage::Quoting:
        handleQuote();
        return;

    case Stage::Uploading:   // handled before the preview checks
        return;
    }
}

void IDMCPMeshUploadJob::enter(Stage s, F64 limit)
{
    mStage         = s;
    mStageDeadline = LLTimer::getTotalSeconds() + limit;
    mSettleTicks   = 0;
}

// The preview is idle when nothing is loading, LOD generation has drained and
// update() has consumed the dirty flag. Require a few ticks in a row, because
// some steps (refresh, LOD queries) re-dirty it a frame later.
bool IDMCPMeshUploadJob::settled()
{
    LLModelPreview* mp = mPreview;
    const bool idle = !mp->mLoading && mp->mModelLoader == nullptr && !mp->mGenLOD && mp->mLodsQuery.empty() && !mp->mDirty
                      && (!mSettings.upload_textures || mp->areTexturesReady());
    mSettleTicks = idle ? mSettleTicks + 1 : 0;
    return mSettleTicks >= SETTLE_TICKS;
}

bool IDMCPMeshUploadJob::requestPending()
{
    LLMutexLock lock(&mReplyMutex);
    if (mStage == Stage::Quoting)   return mFeeReply == Reply::Pending;
    if (mStage == Stage::Uploading) return mUploadReply == Reply::Pending;
    return false;
}

const char* IDMCPMeshUploadJob::stageName(Stage s)
{
    switch (s)
    {
    case Stage::Loading:     return "loading the model";
    case Stage::Configuring: return "applying upload options";
    case Stage::Analyzing:   return "analyzing the physics shape";
    case Stage::Quoting:     return "waiting for the fee quote and upload permission";
    case Stage::Uploading:   return "uploading";
    }
    return "working";
}

S32 IDMCPMeshUploadJob::triangles(LLModelPreview* mp, S32 lod)
{
    S32 n = 0;
    for (const auto& mdl : mp->mModel[lod])
    {
        if (mdl.notNull()) n += mdl->getNumTriangles();
    }
    return n;
}

// ---- stages -----------------------------------------------------------------

// What LLFloaterModelPreview::loadModel does before handing the file over.
void IDMCPMeshUploadJob::loadFile(S32 lod, const std::string& path)
{
    LLModelPreview* mp = mPreview;
    mp->mLoading = true;
    if (lod == LLModel::LOD_PHYSICS)
    {
        mp->mPhysicsSearchLOD = lod;
        mp->mWarnOfUnmatchedPhyicsMeshes = false;
    }
    mp->loadModel(path, lod, true);
}

void IDMCPMeshUploadJob::loadNext()
{
    const auto [lod, path] = mPendingLoads.front();
    mPendingLoads.pop_front();
    if (lod != LLModel::LOD_PHYSICS)
    {
        mSettings.lod_source[lod] = LLModelPreview::LOD_FROM_FILE;
    }
    loadFile(lod, path);
    enter(Stage::Loading, LOAD_LIMIT);   // each file gets its own load window
}

// Applies the options the way the floater's widgets would, and marks the
// preview dirty as their commit callbacks do. Fails the call and returns
// false on a rigged option the model can't satisfy. An unset rigged option
// means "what the model contains".
bool IDMCPMeshUploadJob::configure()
{
    LLModelPreview* mp = mPreview;

    bool has_skin = false, has_joints = false;
    for (const auto& mdl : mp->mModel[LLModel::LOD_HIGH])
    {
        if (mdl.isNull()) continue;
        has_skin   = has_skin   || !mdl->mSkinWeights.empty();
        has_joints = has_joints || !mdl->mSkinInfo.mAlternateBindMatrix.empty();
    }
    if (mParams.skin_weights == 1 && !has_skin)
    {
        fail(IDMCP_ERR_INVALID_PARAMS, "rigged.skin_weights: the model has no skin weights");
        return false;
    }
    if (mParams.joint_positions == 1 && !has_joints)
    {
        fail(IDMCP_ERR_INVALID_PARAMS, "rigged.joint_positions: the model has no joint offsets");
        return false;
    }
    // The floater only offers skin weights for a rig it accepts, and joint
    // positions for a rig valid for them. Say why rather than uploading the
    // model without its rig.
    const U32 rig_flags = mp->getLegacyRigFlags();
    if (has_skin && mParams.skin_weights != 0 && rig_flags != LEGACY_RIG_OK)
    {
        const char* why = (rig_flags & LEGACY_RIG_FLAG_TOO_MANY_JOINTS) ? "the rig uses too many joints"
                        : (rig_flags & LEGACY_RIG_FLAG_UNKNOWN_JOINT)   ? "the rig uses joints the viewer doesn't know"
                        : "the rig failed validation";
        boost::json::object data;
        data["log"] = logLines();
        fail(IDMCP_ERR_INVALID_PARAMS,
             std::string("rigged.skin_weights: ") + why + "; pass \"skin_weights\":false to upload it unrigged",
             std::move(data));
        return false;
    }
    if (has_joints && mParams.skin_weights != 0 && mParams.joint_positions != 0 && !mp->isRigValidForJointPositionUpload())
    {
        boost::json::object data;
        data["log"] = logLines();
        fail(IDMCP_ERR_INVALID_PARAMS,
             "rigged.joint_positions: the rig isn't valid for joint positions; pass \"joint_positions\":false to upload without them",
             std::move(data));
        return false;
    }

    // The floater's render() enforces the same dependencies on its checkboxes:
    // joints need skin, and lock scale needs both.
    const bool want_skin   = mParams.skin_weights   < 0 ? has_skin   : mParams.skin_weights == 1;
    const bool want_joints = want_skin && (mParams.joint_positions < 0 ? has_joints : mParams.joint_positions == 1);
    mSettings.upload_skin   = want_skin;
    mSettings.upload_joints = want_joints;
    mLockScale = want_skin && want_joints && mParams.lock_scale == 1;
    mp->mDirty = true;

    // LLFloaterModelPreview::onPhysicsUseLOD. A physics file was loaded in
    // the Loading stage; "none" leaves the physics shape unset.
    if (mParams.physics == "cube")
    {
        mp->setPhysicsFromPreset(1);   // loads cube_phys.dae
    }
    else if (idmcp_mesh_is_physics_keyword(mParams.physics) && mParams.physics != "none")
    {
        mp->setPhysicsFromLOD(physics_lod(mParams.physics));
    }
    mp->refresh();
    mp->updateStatusMessages();
    return true;
}

// LLFloaterModelPreview::onPhysicsStageExecute, reporting to this job.
bool IDMCPMeshUploadJob::startAnalyze()
{
    LLModelPreview* mp = mPreview;
    if (mp->mModel[LLModel::LOD_PHYSICS].empty())
    {
        fail(IDMCP_ERR_INVALID_PARAMS, "analyze: there is no physics shape to analyze");
        return false;
    }
    // Havok names the hull stage "Analyze"; VHACD names it "Decompose".
    const auto& stages = gMeshRepo.mDecompThread->mStageID;
    const char* stage  = stages.count("Analyze") ? "Analyze"
                       : stages.count("Decompose") ? "Decompose" : nullptr;
    if (!stage)
    {
        fail(IDMCP_ERR_CAP_UNAVAIL, "convex decomposition isn't available in this viewer build");
        return false;
    }

    for (LLModel* mdl : mp->mModel[LLModel::LOD_PHYSICS])
    {
        if (!mdl) continue;
        LLPointer<DecompRequest> request = new DecompRequest(stage, mdl, mDecompParams, weak_from_this());
        mDecompRequests.insert(request);
        gMeshRepo.mDecompThread->submitRequest(request);
    }
    return true;
}

// Same as LLFloaterModelPreview::DecompRequest::completed.
void IDMCPMeshUploadJob::DecompRequest::completed()
{
    if (!mContinue)
    {
        return;
    }
    mModel->setConvexHullDecomposition(mHull, mHullMesh);
    if (std::shared_ptr<IDMCPMeshUploadJob> job = mJob.lock())
    {
        job->onDecompCompleted(this);
    }
}

void IDMCPMeshUploadJob::onDecompCompleted(DecompRequest* req)
{
    mDecompRequests.erase(LLPointer<DecompRequest>(req));
    if (mPreview)
    {
        mPreview->mDirty = true;
        mPreview->refresh();
    }
}

void IDMCPMeshUploadJob::cancelDecomposition()
{
    // By value: set elements are const, and a const LLPointer only hands out
    // a const pointer.
    for (LLPointer<DecompRequest> req : mDecompRequests)
    {
        req->mContinue = 0;
    }
    mDecompRequests.clear();
}

void IDMCPMeshUploadJob::sendQuote()
{
    LLModelPreview* mp = mPreview;
    if (!mp->mModelNoErrors)
    {
        boost::json::object data;
        data["log"] = logLines();
        data["status"] = statusLines();
        fail(IDMCP_ERR_INVALID_PARAMS, "the importer rejected the model", std::move(data));
        return;
    }

    mp->rebuildUploadData();
    if (mp->getLoadState() >= LLModelLoader::ERROR_PARSING)
    {
        boost::json::object data;
        data["log"] = logLines();
        data["status"] = statusLines();
        fail(IDMCP_ERR_INVALID_PARAMS, "the importer rejected the model", std::move(data));
        return;
    }
    // The floater clamps its scale spinner to this; say so instead.
    if (mp->mMaxImportScale > 0.f && mSettings.import_scale > mp->mMaxImportScale)
    {
        boost::json::object data;
        data["max_scale"] = mp->mMaxImportScale;
        fail(IDMCP_ERR_INVALID_PARAMS,
             llformat("scale %.3f is too large for this model in this region; the most is %.3f",
                      mSettings.import_scale, mp->mMaxImportScale),
             std::move(data));
        return;
    }

    std::map<std::string, std::string> lod_sources;
    mp->fillLODSourceStatistics(lod_sources);

    {
        LLMutexLock lock(&mReplyMutex);
        mFeeReply = Reply::Pending;
    }
    enter(Stage::Quoting, QUOTE_LIMIT);
    // Same arguments as LLFloaterModelPreview::onClickCalculateBtn.
    gMeshRepo.uploadModel(mp->mUploadData, lod_sources, mp->mPreviewScale,
                          mSettings.upload_textures, mSettings.upload_skin, mSettings.upload_joints, mLockScale,
                          std::string(), mParams.dest, false,
                          getWholeModelFeeObserverHandle());
}

void IDMCPMeshUploadJob::handleQuote()
{
    Reply       reply;
    LLSD        fee;
    std::string url, error;
    {
        LLMutexLock lock(&mReplyMutex);
        reply = mFeeReply;
        fee   = mFee;
        url   = mUploadUrl;
        error = mFeeError;
    }
    if (reply == Reply::Pending) return;
    if (reply == Reply::Failed)
    {
        fail(IDMCP_ERR_CAP_UNAVAIL, error);
        return;
    }
    // uploadBlock() needs the answer; the stage deadline still applies.
    if (mPerm == Perm::Pending) return;

    const S32 price   = fee["upload_price"].asInteger();
    const S32 balance = gStatusBar ? gStatusBar->getBalance() : -1;
    mQuotedPrice = price;
    mLandImpact  = fee["resource_cost"].asReal();

    if (!mParams.confirm)
    {
        LLModelPreview* mp = mPreview;
        const LLSD& b = fee["upload_price_breakdown"];

        boost::json::object breakdown;
        breakdown["model"]     = b["model"].asInteger();
        breakdown["streaming"] = b["mesh_streaming"].asInteger();
        breakdown["physics"]   = b["mesh_physics"].asInteger();
        breakdown["instances"] = b["mesh_instance"].asInteger();
        breakdown["textures"]  = b["texture"].asInteger();

        boost::json::object weights;
        weights["download"] = fee["model_streaming_cost"].asReal();
        weights["physics"]  = fee["physics_cost"].asReal();
        weights["server"]   = fee["simulation_cost"].asReal();

        boost::json::object tris;
        tris["high"]    = triangles(mp, LLModel::LOD_HIGH);
        tris["medium"]  = triangles(mp, LLModel::LOD_MEDIUM);
        tris["low"]     = triangles(mp, LLModel::LOD_LOW);
        tris["lowest"]  = triangles(mp, LLModel::LOD_IMPOSTOR);
        tris["physics"] = triangles(mp, LLModel::LOD_PHYSICS);

        boost::json::object o;
        o["dry_run"]         = true;
        o["confirm_hint"]    = "re-call with \"confirm\":true to upload";
        o["upload_price"]    = price;
        o["price_breakdown"] = std::move(breakdown);
        o["land_impact"]     = mLandImpact;
        o["weights"]         = std::move(weights);
        o["triangles"]       = std::move(tris);
        o["warnings"]        = logLines();
        o["balance"]         = balance;
        o["affordable"]      = (balance < 0) || (balance >= price);

        int                 block_code = 0;
        boost::json::object block_data;
        const std::string   block = uploadBlock(block_code, block_data);
        o["upload_blocked"] = !block.empty();
        if (!block.empty()) o["blocked_reason"] = block;
        succeed(std::move(o));
        return;
    }

    int                 block_code = 0;
    boost::json::object block_data;
    const std::string   block = uploadBlock(block_code, block_data);
    if (!block.empty())
    {
        fail(block_code, block, std::move(block_data));
        return;
    }

    if (balance >= 0 && balance < price)
    {
        boost::json::object data;
        data["upload_price"] = price;
        data["balance"]      = balance;
        fail(IDMCP_ERR_PERMISSION, "insufficient L$ balance", std::move(data));
        return;
    }
    sendUpload(url);
}

// Why the floater's own Upload button would refuse this model; empty when it
// wouldn't. Sets the error code and any error data for the confirmed path.
std::string IDMCPMeshUploadJob::uploadBlock(int& code, boost::json::object& data) const
{
    if (mPerm == Perm::Denied)
    {
        code = IDMCP_ERR_PERMISSION;
        return "this account isn't allowed to upload mesh (no payment info on file?)";
    }
    if (mPerm == Perm::Failed)
    {
        code = IDMCP_ERR_PERMISSION;
        return "the mesh upload permission check failed: " + mPermError;
    }
    // Based on the check in LLModelPreview::updateStatusMessages, but
    // deliberately stricter than the floater in practice: the floater
    // re-enables Calculate/Upload regardless of physics errors, while this
    // blocks degenerate or over-limit physics shapes, which the server
    // rejects anyway. TOOTHIN (any mesh under 0.5 m on an axis, so most worn
    // items) is only a warning and doesn't block.
    const U32 physics_error = mPreview->mPhysicsErrors
        & (LLModelPreview::PHYSICS_ERROR_DEGENERATE | LLModelPreview::PHYSICS_ERROR_TOOMANYHULLS
           | LLModelPreview::PHYSICS_ERROR_TOOMANYVERTSINHULL);
    if (!gSavedSettings.getBOOL("FSIgnoreClientsideMeshValidation")
        && (!mPreview->mModelNoErrors || physics_error))
    {
        code           = IDMCP_ERR_INVALID_PARAMS;
        data["log"]    = logLines();
        data["status"] = statusLines();
        return "the importer blocks uploading this model (physics validation)";
    }
    return std::string();
}

void IDMCPMeshUploadJob::sendUpload(const std::string& url)
{
    LLModelPreview* mp = mPreview;
    std::map<std::string, std::string> lod_sources;
    mp->fillLODSourceStatistics(lod_sources);

    {
        LLMutexLock lock(&mReplyMutex);
        mUploadReply = Reply::Pending;
    }
    enter(Stage::Uploading, UPLOAD_LIMIT);
    // Same arguments as LLFloaterModelPreview::onUpload. mUploadData is the
    // set the quote was computed from; don't rebuild it.
    gMeshRepo.uploadModel(mp->mUploadData, lod_sources, mp->mPreviewScale,
                          mSettings.upload_textures, mSettings.upload_skin, mSettings.upload_joints, mLockScale,
                          url, mParams.dest, true,
                          LLHandle<LLWholeModelFeeObserver>(), getWholeModelUploadObserverHandle());
}

void IDMCPMeshUploadJob::handleUpload()
{
    Reply reply;
    LLSD  response;
    {
        LLMutexLock lock(&mReplyMutex);
        reply    = mUploadReply;
        response = mUploadResponse;
    }
    if (reply == Reply::Pending) return;
    if (reply == Reply::Failed)
    {
        fail(IDMCP_ERR_CAP_UNAVAIL, "the server rejected the upload; the viewer shows the reason in a notification");
        return;
    }

    boost::json::object o;
    o["uploaded"]    = true;
    o["item_id"]     = response["new_inventory_item"].asUUID().asString();
    o["asset_id"]    = response["new_asset"].asUUID().asString();
    o["cost"]        = mQuotedPrice;
    o["land_impact"] = mLandImpact;
    succeed(std::move(o));
}

// ---- observers ----------------------------------------------------------------

void IDMCPMeshUploadJob::onModelPhysicsFeeReceived(const LLSD& result, std::string upload_url)
{
    LLMutexLock lock(&mReplyMutex);
    mFee       = llsd_clone(result);
    mUploadUrl = upload_url;
    mFeeReply  = Reply::Ok;
}

void IDMCPMeshUploadJob::setModelPhysicsFeeErrorStatus(S32 status, const std::string& reason, const LLSD& result)
{
    LLMutexLock lock(&mReplyMutex);
    // status is the packed LLCore::HttpStatus, not an HTTP code, so leave it out.
    mFeeError = "fee request failed";
    if (!reason.empty())
    {
        mFeeError += ": " + reason;
    }
    // On network failures the mesh repo copies reason into message; skip the repeat.
    if (result.has("message") && !result["message"].asString().empty()
        && result["message"].asString() != reason)
    {
        mFeeError += ": " + result["message"].asString();
    }
    mFeeReply = Reply::Failed;
}

void IDMCPMeshUploadJob::onModelUploadSuccess()
{
    onModelUploadSuccessWithResponse(LLSD());
}

void IDMCPMeshUploadJob::onModelUploadSuccessWithResponse(const LLSD& response)
{
    LLMutexLock lock(&mReplyMutex);
    mUploadResponse = response;
    mUploadReply    = Reply::Ok;
}

void IDMCPMeshUploadJob::onModelUploadFailure()
{
    LLMutexLock lock(&mReplyMutex);
    mUploadReply = Reply::Failed;
}

// ---- importer log -------------------------------------------------------------

void IDMCPMeshUploadJob::addPreviewLog(const std::string& str, bool)
{
    std::string line = str;
    LLStringUtil::trim(line);
    if (line.empty()) return;
    mLog.push_back(line);
    if (mLog.size() > MAX_LOG_LINES)
    {
        mLog.pop_front();
    }
}

// The floater turns these into its localized strings; without it, report
// the string name and its arguments.
void IDMCPMeshUploadJob::addPreviewLog(const std::string& message, const LLSD& args, bool flash, S32 lod)
{
    std::string line;
    switch (lod)
    {
    case LLModel::LOD_IMPOSTOR: line = "LOD0 "; break;
    case LLModel::LOD_LOW:      line = "LOD1 "; break;
    case LLModel::LOD_MEDIUM:   line = "LOD2 "; break;
    case LLModel::LOD_PHYSICS:  line = "PHYS "; break;
    case LLModel::LOD_HIGH:     line = "LOD3 "; break;
    default: break;
    }
    line += message;

    std::string sep = ": ";
    for (LLSD::map_const_iterator it = args.beginMap(); it != args.endMap(); ++it)
    {
        if (it->first == "Message") continue;   // loader warnings repeat their name here
        line += sep + it->first + "=" + it->second.asString();
        sep = ", ";
    }
    addPreviewLog(line, flash);
}

// ---- finishing --------------------------------------------------------------

boost::json::array IDMCPMeshUploadJob::logLines() const
{
    boost::json::array out;
    for (const std::string& line : mLog)
    {
        out.emplace_back(line);
    }
    return out;
}

// LOD and physics problems the preview found, in the floater's wording.
boost::json::array IDMCPMeshUploadJob::statusLines() const
{
    boost::json::array out;
    if (!mPreview) return out;
    for (S32 lod = LLModel::LOD_HIGH; lod >= LLModel::LOD_IMPOSTOR; --lod)
    {
        if (const char* text = lod_status_text(mPreview->mLodStatusMessage[lod]))
        {
            out.emplace_back(lod_name[lod] + ": " + text);
        }
    }
    if (const char* text = physics_status_text(mPreview->mPhysicsErrors))
    {
        out.emplace_back(std::string("physics: ") + text);
    }
    return out;
}

void IDMCPMeshUploadJob::fail(int code, const std::string& msg, boost::json::value data)
{
    if (mFinished) return;
    finish();
    idmcp_tool_err(mCall, code, msg, std::move(data));
}

void IDMCPMeshUploadJob::succeed(boost::json::object result)
{
    if (mFinished) return;
    finish();
    idmcp_tool_ok(mCall, std::move(result));
}

// Set mFinished before answering: answering runs the call's cleanup, which
// calls cancel(), which must then be a no-op.
void IDMCPMeshUploadJob::finish()
{
    mFinished = true;
    cancelDecomposition();
    // A load still running is dropped when it lands rather than processed,
    // so the preview can be deleted sooner.
    if (mPreview)
    {
        mPreview->mIgnoreLoadedCallback = true;
    }
}

// Deletes the preview once no model is loading: ~LLModelPreview would block
// the main thread until the loader thread finished. Returns true when there
// is no preview left.
bool IDMCPMeshUploadJob::deletePreviewWhenIdle()
{
    if (mPreview && mPreview->mModelLoader)
    {
        return false;
    }
    delete mPreview;
    mPreview = nullptr;
    return true;
}

// Runs from the call cleanup (timeout) and from tick() on disconnect.
void IDMCPMeshUploadJob::cancel()
{
    if (!mFinished) finish();
}

void IDMCPMeshUploadJob::release()
{
    // Keep this job alive until the function returns: the erase may drop the
    // last reference while tick() is still on the stack.
    std::shared_ptr<IDMCPMeshUploadJob> self = shared_from_this();
    sJobs.erase(std::remove(sJobs.begin(), sJobs.end(), self), sJobs.end());
    if (sJobs.empty())
    {
        gIdleCallbacks.deleteFunction(&IDMCPMeshUploadJob::onIdle, nullptr);
    }
}
