/**
 * @file idmcp_meshupload.cpp
 * @brief <ID> MCP server: upload.mesh job.
 *
 * Part of Five's custom Firestorm fork. Custom code carries an `ID` prefix.
 *
 * LLModelPreview can't run without its floater, so the job opens the stock
 * Upload Model floater, loads the file, sets the floater's widgets, then calls
 * gMeshRepo.uploadModel with its own fee/upload observers. One job at a time:
 * the floater is a singleton. An idle callback moves the job through its
 * stages, each with a time limit.
 *
 * Lifetime: sActive owns the job. The fee observer methods run on the mesh
 * upload thread, which holds a raw pointer to the job, so the job is never
 * released while a fee or upload request is in flight, even after the MCP
 * call has been answered.
 */

#include "llviewerprecompiledheaders.h"

#include "idmcp_meshupload.h"
#include "idmcpserver.h"

#include "llbutton.h"
#include "llcallbacklist.h"         // gIdleCallbacks
#include "llcombobox.h"
#include "llfloatermodelpreview.h"
#include "llfloaterreg.h"
#include "llmeshrepository.h"       // gMeshRepo
#include "llmodel.h"
#include "llmodelloader.h"
#include "llmodelpreview.h"         // lod_name[]
#include "llmutex.h"
#include "llsdutil.h"               // llsd_clone
#include "llstatusbar.h"            // gStatusBar
#include "lltimer.h"
#include "lluploadfloaterobservers.h"
#include "llviewercontrol.h"        // gSavedSettings
#include "llviewertexteditor.h"

#include <deque>
#include <memory>
#include <sstream>
#include <utility>
#include <vector>

namespace
{
    constexpr F64 LOAD_LIMIT    = 120.0;
    constexpr F64 ANALYZE_LIMIT = 300.0;
    constexpr F64 QUOTE_LIMIT   = 60.0;
    constexpr F64 UPLOAD_LIMIT  = 300.0;
    // A request that never calls back would pin the job (and the busy error)
    // forever. After this long past its stage deadline the job is parked in
    // sParked instead: leaked, never freed, so the raw pointer stays valid.
    constexpr F64 ABANDON_GRACE = 600.0;
    constexpr int SETTLE_TICKS  = 3;   // idle ticks the preview must stay idle

    // physics_lod_combo item order (floater_model_preview.xml:872-881).
    constexpr S32 PHYS_NONE = 0, PHYS_HIGH = 1, PHYS_MEDIUM = 2,
                  PHYS_LOW = 3, PHYS_LOWEST = 4, PHYS_CUBE = 5;

    S32 physics_combo_index(const std::string& k)
    {
        if (k == "high")   return PHYS_HIGH;
        if (k == "medium") return PHYS_MEDIUM;
        if (k == "low")    return PHYS_LOW;
        if (k == "lowest") return PHYS_LOWEST;
        if (k == "cube")   return PHYS_CUBE;
        return PHYS_NONE;
    }
}

bool idmcp_mesh_is_physics_keyword(const std::string& k)
{
    return k == "none" || k == "high" || k == "medium" || k == "low" || k == "lowest" || k == "cube";
}

class IDMCPMeshUploadJob
    : public LLWholeModelFeeObserver
    , public LLWholeModelUploadObserver
{
public:
    static void start(const IDMCPMeshUploadParams& p, const IDMCPCallPtr& call);

    // LLWholeModelFeeObserver: called on the mesh upload thread.
    void onModelPhysicsFeeReceived(const LLSD& result, std::string upload_url) override;
    void setModelPhysicsFeeErrorStatus(S32 status, const std::string& reason, const LLSD& result) override;

    // LLWholeModelUploadObserver: called on the main thread (doOnIdleOneTime).
    void onModelUploadSuccess() override;
    void onModelUploadSuccessWithResponse(const LLSD& response) override;
    void onModelUploadFailure() override;

private:
    enum class Stage { Loading, Configuring, Analyzing, Quoting, Uploading };
    enum class Reply { Pending, Ok, Failed };   // not "None": X11 #defines None

    IDMCPMeshUploadJob(const IDMCPMeshUploadParams& p, const IDMCPCallPtr& call)
        : mParams(p), mCall(call) {}

    static void onIdle(void*);
    static S32  triangles(LLModelPreview* mp, S32 lod);
    static const char* stageName(Stage s);

    void tick();
    void enter(Stage s, F64 limit);
    bool settled(LLModelPreview* mp);
    bool requestPending();
    LLFloaterModelPreview* floater() const;

    void loadNext(LLFloaterModelPreview* fmp);
    bool configure(LLFloaterModelPreview* fmp);
    bool startAnalyze(LLFloaterModelPreview* fmp);
    void sendQuote(LLFloaterModelPreview* fmp);
    void handleQuote(LLFloaterModelPreview* fmp);
    void sendUpload(LLFloaterModelPreview* fmp, const std::string& url);
    void handleUpload();
    std::string uploadBlock(LLFloaterModelPreview* fmp, int& code, boost::json::object& data) const;

    boost::json::array logLines(LLFloaterModelPreview* fmp) const;
    boost::json::array statusLines(LLFloaterModelPreview* fmp) const;
    void fail(int code, const std::string& msg, boost::json::value data = boost::json::value());
    void succeed(boost::json::object result);
    void finish();
    bool closeWhenLoaderDone();
    void cancel();
    void release();

    static std::shared_ptr<IDMCPMeshUploadJob> sActive;
    static std::vector<std::shared_ptr<IDMCPMeshUploadJob>> sParked;

    IDMCPMeshUploadParams mParams;
    IDMCPCallPtr          mCall;
    LLHandle<LLFloater>   mFloater;

    Stage mStage         = Stage::Loading;
    F64   mStageDeadline = 0.0;
    int   mSettleTicks   = 0;
    bool  mFinished      = false;   // call answered; only waiting on in-flight HTTP
    bool  mClosePending  = false;   // finished, but the floater's loader is still running
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

std::shared_ptr<IDMCPMeshUploadJob>              IDMCPMeshUploadJob::sActive;
std::vector<std::shared_ptr<IDMCPMeshUploadJob>> IDMCPMeshUploadJob::sParked;

// ---- start ----------------------------------------------------------------

void IDMCPMeshUploadJob::start(const IDMCPMeshUploadParams& p, const IDMCPCallPtr& call)
{
    if (sActive || LLFloaterReg::instanceVisible("upload_model"))
    {
        idmcp_tool_err(call, IDMCP_ERR_BUSY, "upload model floater is in use");
        return;
    }
    if (p.analyze && p.physics == "none")
    {
        idmcp_tool_err(call, IDMCP_ERR_INVALID_PARAMS, "analyze needs a physics shape, but physics is \"none\"");
        return;
    }
    if (p.analyze && !gMeshRepo.mDecompThread)
    {
        idmcp_tool_err(call, IDMCP_ERR_CAP_UNAVAIL, "convex decomposition isn't available in this viewer build");
        return;
    }

    LLFloaterModelPreview* fmp = LLFloaterReg::showTypedInstance<LLFloaterModelPreview>("upload_model");
    if (!fmp)
    {
        idmcp_tool_err(call, IDMCP_ERR_CAP_UNAVAIL, "could not open the upload model floater");
        return;
    }

    std::shared_ptr<IDMCPMeshUploadJob> job(new IDMCPMeshUploadJob(p, call));
    job->mFloater = fmp->getHandle();
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

    sActive = job;
    std::weak_ptr<IDMCPMeshUploadJob> weak = job;
    call->setCleanup([weak]
    {
        if (auto j = weak.lock()) j->cancel();
    });
    gIdleCallbacks.addFunction(&IDMCPMeshUploadJob::onIdle, nullptr);

    // Same reset the floater's own "Reset" does (onReset), then load by path.
    fmp->resetUploadOptions();
    fmp->initModelPreview();
    fmp->getChild<LLUICtrl>("description_form")->setValue(p.name);
    fmp->loadModel(LLModel::LOD_HIGH, p.path, true);
    job->enter(Stage::Loading, LOAD_LIMIT);
}

void idmcp_mesh_upload_start(const IDMCPMeshUploadParams& p, const IDMCPCallPtr& call)
{
    IDMCPMeshUploadJob::start(p, call);
}

// ---- idle driver ----------------------------------------------------------

void IDMCPMeshUploadJob::onIdle(void*)
{
    // Local copy: release() resets sActive, which must not destroy the job
    // while tick() is still on the stack.
    std::shared_ptr<IDMCPMeshUploadJob> self = sActive;
    if (self) self->tick();
}

void IDMCPMeshUploadJob::tick()
{
    const F64 now = LLTimer::getTotalSeconds();

    if (mFinished)
    {
        if (mClosePending && !closeWhenLoaderDone())
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
            sParked.push_back(sActive);
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
    // list, so the floater is no longer needed, and the upload may still
    // complete (and charge) after a timeout.
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

    LLFloaterModelPreview* fmp = floater();
    // A closed floater is destroyed a frame later (mortician), so check isDead too.
    if (!fmp || fmp->isDead() || !fmp->mModelPreview)
    {
        fail(IDMCP_ERR_CAP_UNAVAIL, "upload model floater was closed");
        return;
    }
    if (now > mStageDeadline)
    {
        fail(IDMCP_ERR_TIMEOUT, std::string("timed out while ") + stageName(mStage));
        return;
    }

    LLModelPreview* mp = fmp->mModelPreview;
    // The loader thread sets the error state before its callback reaches the
    // main thread. Wait for that callback (it clears mModelLoader) before
    // reacting, or closing the floater can leave the preview with a dangling
    // loader pointer.
    if (!mp->mModelLoader && mp->getLoadState() >= LLModelLoader::ERROR_PARSING)
    {
        boost::json::object data;
        data["log"] = logLines(fmp);
        fail(IDMCP_ERR_INVALID_PARAMS, "the model failed to load", std::move(data));
        return;
    }

    // Does draw()'s work, so a minimized viewer still progresses.
    mp->update();

    switch (mStage)
    {
    case Stage::Loading:
        if (!settled(mp)) return;
        if (!mPendingLoads.empty())
        {
            loadNext(fmp);
            return;
        }
        if (configure(fmp))
        {
            enter(Stage::Configuring, LOAD_LIMIT);
        }
        return;

    case Stage::Configuring:
        if (!settled(mp)) return;
        if (mParams.analyze)
        {
            if (startAnalyze(fmp))
            {
                enter(Stage::Analyzing, ANALYZE_LIMIT);
            }
            return;
        }
        sendQuote(fmp);
        return;

    case Stage::Analyzing:
        if (!fmp->mCurRequest.empty() || !settled(mp)) return;
        sendQuote(fmp);
        return;

    case Stage::Quoting:
        handleQuote(fmp);
        return;

    case Stage::Uploading:   // handled before the floater checks
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
// the floater's draw() has consumed the dirty flag. Require a few ticks in a
// row, because some steps (refresh, LOD queries) re-dirty it a frame later.
bool IDMCPMeshUploadJob::settled(LLModelPreview* mp)
{
    const bool idle = !mp->mLoading && mp->mModelLoader == nullptr && !mp->mGenLOD && mp->mLodsQuery.empty() && !mp->mDirty
                      && (!mParams.textures || mp->areTexturesReady());
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

LLFloaterModelPreview* IDMCPMeshUploadJob::floater() const
{
    return static_cast<LLFloaterModelPreview*>(mFloater.get());
}

const char* IDMCPMeshUploadJob::stageName(Stage s)
{
    switch (s)
    {
    case Stage::Loading:     return "loading the model";
    case Stage::Configuring: return "applying upload options";
    case Stage::Analyzing:   return "analyzing the physics shape";
    case Stage::Quoting:     return "waiting for the fee quote";
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

void IDMCPMeshUploadJob::loadNext(LLFloaterModelPreview* fmp)
{
    const auto [lod, path] = mPendingLoads.front();
    mPendingLoads.pop_front();
    if (lod != LLModel::LOD_PHYSICS)
    {
        fmp->getChild<LLComboBox>("lod_source_" + lod_name[lod])->setCurrentByIndex(LLModelPreview::LOD_FROM_FILE);
    }
    fmp->loadModel(lod, path, true);
    enter(Stage::Loading, LOAD_LIMIT);   // each file gets its own load window
}

// Sets the option widgets and fires each commit callback, so the floater
// reacts as it would to a click. Fails the call and returns false on a
// rigged option the model can't satisfy. upload_skin and upload_joints are
// always written (an unset option means "what the model contains"), and
// mFirstSkinUpdate is cleared so render() never applies the user's
// FSMeshUploadAutoEnableWeights setting over them.
bool IDMCPMeshUploadJob::configure(LLFloaterModelPreview* fmp)
{
    LLModelPreview* mp = fmp->mModelPreview;

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

    mp->mFirstSkinUpdate = false;

    const bool want_skin   = mParams.skin_weights   < 0 ? has_skin   : mParams.skin_weights == 1;
    const bool want_joints = mParams.joint_positions < 0 ? has_joints : mParams.joint_positions == 1;

    auto set_and_commit = [fmp](const char* name, const LLSD& value)
    {
        LLUICtrl* ctrl = fmp->getChild<LLUICtrl>(name);
        ctrl->setValue(value);
        ctrl->onCommit();
    };
    set_and_commit("import_scale", LLSD((F64)mParams.scale));
    set_and_commit("upload_textures", LLSD(mParams.textures));
    set_and_commit("upload_skin", LLSD(want_skin));
    set_and_commit("upload_joints", LLSD(want_joints));
    if (mParams.lock_scale >= 0)      set_and_commit("lock_scale_if_joint_position", LLSD(mParams.lock_scale == 1));

    LLComboBox* phys = fmp->getChild<LLComboBox>("physics_lod_combo");
    if (!idmcp_mesh_is_physics_keyword(mParams.physics))
    {
        // The file was loaded in the Loading stage. Last item is "From file".
        phys->setCurrentByIndex(phys->getItemCount() - 1);
    }
    else if (mParams.physics == "none")
    {
        // No commit: onPhysicsUseLOD treats index 0 as a LOD index.
        phys->setCurrentByIndex(PHYS_NONE);
    }
    else
    {
        phys->setCurrentByIndex(physics_combo_index(mParams.physics));
        phys->onCommit();
    }
    return true;
}

bool IDMCPMeshUploadJob::startAnalyze(LLFloaterModelPreview* fmp)
{
    if (fmp->mModelPreview->mModel[LLModel::LOD_PHYSICS].empty())
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
    fmp->getChild<LLButton>(stage)->onCommit();
    return true;
}

void IDMCPMeshUploadJob::sendQuote(LLFloaterModelPreview* fmp)
{
    LLModelPreview* mp = fmp->mModelPreview;
    if (!mp->mModelNoErrors)
    {
        boost::json::object data;
        data["log"] = logLines(fmp);
        data["status"] = statusLines(fmp);
        fail(IDMCP_ERR_INVALID_PARAMS, "the importer rejected the model", std::move(data));
        return;
    }

    // rebuildUploadData reads the item name from description_form.
    fmp->getChild<LLUICtrl>("description_form")->setValue(mParams.name);
    mp->rebuildUploadData();
    if (mp->getLoadState() >= LLModelLoader::ERROR_PARSING)
    {
        boost::json::object data;
        data["log"] = logLines(fmp);
        data["status"] = statusLines(fmp);
        fail(IDMCP_ERR_INVALID_PARAMS, "the importer rejected the model", std::move(data));
        return;
    }

    LLFloaterModelPreview::lod_sources_map_t lod_sources;
    fmp->fillLODSourceStatistics(lod_sources);

    {
        LLMutexLock lock(&mReplyMutex);
        mFeeReply = Reply::Pending;
    }
    enter(Stage::Quoting, QUOTE_LIMIT);
    // Same arguments as LLFloaterModelPreview::onClickCalculateBtn.
    gMeshRepo.uploadModel(mp->mUploadData, lod_sources, mp->mPreviewScale,
                          fmp->childGetValue("upload_textures").asBoolean(),
                          fmp->childGetValue("upload_skin").asBoolean(),
                          fmp->childGetValue("upload_joints").asBoolean(),
                          fmp->childGetValue("lock_scale_if_joint_position").asBoolean(),
                          std::string(), mParams.dest, false,
                          getWholeModelFeeObserverHandle());
}

void IDMCPMeshUploadJob::handleQuote(LLFloaterModelPreview* fmp)
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

    const S32 price   = fee["upload_price"].asInteger();
    const S32 balance = gStatusBar ? gStatusBar->getBalance() : -1;
    mQuotedPrice = price;
    mLandImpact  = fee["resource_cost"].asReal();

    if (!mParams.confirm)
    {
        LLModelPreview* mp = fmp->mModelPreview;
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
        o["warnings"]        = logLines(fmp);
        o["balance"]         = balance;
        o["affordable"]      = (balance < 0) || (balance >= price);

        int                 block_code = 0;
        boost::json::object block_data;
        const std::string   block = uploadBlock(fmp, block_code, block_data);
        o["upload_blocked"] = !block.empty();
        if (!block.empty()) o["blocked_reason"] = block;
        succeed(std::move(o));
        return;
    }

    int                 block_code = 0;
    boost::json::object block_data;
    const std::string   block = uploadBlock(fmp, block_code, block_data);
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
    sendUpload(fmp, url);
}

// Why the floater's own Upload button would refuse this model; empty when it
// wouldn't. Sets the error code and any error data for the confirmed path.
std::string IDMCPMeshUploadJob::uploadBlock(LLFloaterModelPreview* fmp, int& code, boost::json::object& data) const
{
    if (!fmp->mHasUploadPerm)
    {
        code = IDMCP_ERR_PERMISSION;
        return "mesh upload permission isn't confirmed for this account (no payment info on file, or the permission check hasn't answered)";
    }
    // Based on the check in LLModelPreview::updateStatusMessages
    // (llmodelpreview.cpp:3484), but deliberately stricter than the floater in
    // practice: the floater re-enables Calculate/Upload regardless of physics
    // errors, while this blocks degenerate or over-limit physics shapes, which
    // the server rejects anyway. has_physics_error is a local there; it also
    // decides what physics_status_message_text shows (llmodelpreview.cpp:
    // 3393-3436). TOOTHIN (any mesh under 0.5 m on an axis, so most worn
    // items) is only a warning and doesn't block. The text shows the too-thin
    // string only when no real error is set, because those take priority.
    LLUICtrl* phys_text = fmp->getChild<LLUICtrl>("physics_status_message_text");
    const bool physics_error = phys_text->getVisible()
        && phys_text->getValue().asString() != fmp->getString("phys_status_too_thin");
    if (!gSavedSettings.getBOOL("FSIgnoreClientsideMeshValidation")
        && (!fmp->mModelPreview->mModelNoErrors || physics_error))
    {
        code           = IDMCP_ERR_INVALID_PARAMS;
        data["log"]    = logLines(fmp);
        data["status"] = statusLines(fmp);
        return "the importer blocks uploading this model (physics validation)";
    }
    return std::string();
}

void IDMCPMeshUploadJob::sendUpload(LLFloaterModelPreview* fmp, const std::string& url)
{
    LLModelPreview* mp = fmp->mModelPreview;
    LLFloaterModelPreview::lod_sources_map_t lod_sources;
    fmp->fillLODSourceStatistics(lod_sources);

    {
        LLMutexLock lock(&mReplyMutex);
        mUploadReply = Reply::Pending;
    }
    enter(Stage::Uploading, UPLOAD_LIMIT);
    // Same arguments as LLFloaterModelPreview::onUpload. mUploadData is the
    // set the quote was computed from; don't rebuild it.
    gMeshRepo.uploadModel(mp->mUploadData, lod_sources, mp->mPreviewScale,
                          fmp->childGetValue("upload_textures").asBoolean(),
                          fmp->childGetValue("upload_skin").asBoolean(),
                          fmp->childGetValue("upload_joints").asBoolean(),
                          fmp->childGetValue("lock_scale_if_joint_position").asBoolean(),
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

// ---- finishing --------------------------------------------------------------

boost::json::array IDMCPMeshUploadJob::logLines(LLFloaterModelPreview* fmp) const
{
    boost::json::array out;
    if (!fmp || !fmp->mUploadLogText) return out;

    std::istringstream in(fmp->mUploadLogText->getText());
    std::string line;
    while (std::getline(in, line))
    {
        LLStringUtil::trim(line);
        if (!line.empty()) out.emplace_back(line);
    }
    return out;
}

// The floater's LOD and physics status messages, when they have text.
boost::json::array IDMCPMeshUploadJob::statusLines(LLFloaterModelPreview* fmp) const
{
    boost::json::array out;
    if (!fmp) return out;
    for (const char* name : { "lod_status_message_text", "physics_status_message_text" })
    {
        // physics_status_message_text is hidden without being cleared.
        if (!fmp->getChildView(name)->getVisible()) continue;
        std::string text = fmp->childGetValue(name).asString();
        LLStringUtil::trim(text);
        if (!text.empty()) out.emplace_back(text);
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
    LLFloaterModelPreview* fmp = floater();
    if (fmp && !fmp->mCurRequest.empty())
    {
        LLFloaterModelPreview::onPhysicsStageCancel(nullptr, nullptr);
    }
    // Never close while the preview's loader thread is running: if its
    // callback lands after onClose, the preview keeps a pointer to the deleted
    // loader and ~LLModelPreview crashes calling shutdown() on it.
    if (fmp && !fmp->isDead() && fmp->mModelPreview && fmp->mModelPreview->mModelLoader)
    {
        mClosePending = true;
        return;
    }
    if (fmp)
    {
        fmp->closeFloater(false);
    }
}

// Closes the floater once its loader has finished. Returns true when nothing
// is left to close.
bool IDMCPMeshUploadJob::closeWhenLoaderDone()
{
    LLFloaterModelPreview* fmp = floater();
    if (fmp && !fmp->isDead() && fmp->mModelPreview && fmp->mModelPreview->mModelLoader)
    {
        return false;
    }
    if (fmp && !fmp->isDead())
    {
        fmp->closeFloater(false);
    }
    mClosePending = false;
    return true;
}

// Runs from the call cleanup (timeout) and from tick() on disconnect.
void IDMCPMeshUploadJob::cancel()
{
    if (!mFinished) finish();
}

void IDMCPMeshUploadJob::release()
{
    gIdleCallbacks.deleteFunction(&IDMCPMeshUploadJob::onIdle, nullptr);
    sActive.reset();
}
