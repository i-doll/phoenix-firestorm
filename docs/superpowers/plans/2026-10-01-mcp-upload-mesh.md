# `upload.mesh` Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Give MCP agents an `upload.mesh` tool that turns a `.dae`/`.gltf`/`.glb` file into a mesh inventory item, with a free dry-run fee quote before any L$ is spent.

**Architecture:** A job object (`IDMCPMeshUploadJob`) opens the stock Upload Model floater, loads the file into it, sets the floater's widgets, and calls `gMeshRepo.uploadModel` with its own fee and upload observers. An idle callback moves the job through its stages, each with a time limit. Three small `<ID>`-tagged upstream changes make this possible: friend access, an observer method that carries the server response, and a per-tool MCP timeout.

**Tech Stack:** C++17, Firestorm viewer internals (`LLFloaterModelPreview`, `LLModelPreview`, `LLMeshRepository`/`LLMeshUploadThread`, `LLWholeModelFeeObserver`/`LLWholeModelUploadObserver`, `LLFloaterReg`, `gIdleCallbacks`), `boost::json`, the in-repo `IDMCPToolRegistry`.

**Spec:** `docs/superpowers/specs/2026-10-01-mcp-upload-mesh-design.md`

## Global Constraints

- **Do not build this viewer.** The build needs special setup and belongs to the user. Each task ends with a compile-read checklist. The real compile and live run happen in Task 5.
- **No unit-test harness exists for `idmcp*` code.** No test target covers any `idmcp*` translation unit, and the job needs a logged-in viewer with a live floater. Task 5's live verification matrix replaces the red-green unit cycle. Do not invent a unit test that can't run, and do not skip Task 5.
- **Custom code carries the `ID` prefix** (`IDMCPMeshUploadJob`, `idmcp_*`). Mark every edit to upstream files with a trailing `// <ID> ...` comment, matching `llstartup.cpp:97`.
- **Main thread only**, except the two `LLWholeModelFeeObserver` methods, which `LLMeshUploadThread` calls on its own thread (`llmeshrepository.cpp:3430-3471`). Those two only write under `mReplyMutex`.
- **The job must outlive any in-flight fee or upload request.** `LLMeshUploadThread` holds a raw observer pointer after `LLHandle::get()`. Never destroy the job while a request is pending.
- **One mesh per call. No batch.** The floater is a singleton.
- **Loads always pass `force_disable_slm = true`.**
- **Tool timeout is 900 s** for `upload.mesh`. Stage limits: load 120 s, analyze 300 s, quote 60 s, upload 300 s.
- **Busy error is `IDMCP_ERR_BUSY = -32006`** with the message `upload model floater is in use`.
- Conventional-commit messages. No Co-Authored-By or "Generated with" trailers.
- Work on branch `feat/mcp-upload-mesh` (already created from `bot-7.2.3-master2`).

## Review Focus

1. **The user closes the floater mid-job.** Expect an error reply ("upload model floater was closed") within one idle tick, not a 900 s hang. Covered by Task 5, check 10.
2. **The MCP call ends (timeout or disconnect) while a fee or upload request is in flight.** Expect no crash. The job stays alive until the callback lands, and a later call no longer gets the busy error. Covered by Task 3's lifetime rules and Task 5, checks 8 and 11.
3. **`rigged.skin_weights: true` on an unrigged model.** Expect an explicit error, not a silent unrigged upload. Covered by Task 2's `configure` and Task 5, check 12.
4. **LOD or physics file whose meshes don't match the high LOD.** Expect an importer error with the floater's log lines, not a quote. Covered by Task 2's validation and Task 5, check 13.
5. **An account that can't upload mesh** (no payment info, or the IP terms weren't accepted). Expect the server's fee-request error text, not a timeout. Covered by Task 2's `setModelPhysicsFeeErrorStatus` path and Task 5, check 14.

## File Structure

| File | Responsibility |
|---|---|
| `indra/newview/idmcp_meshupload.h` (create) | `IDMCPMeshUploadParams`, `idmcp_mesh_upload_start`, `idmcp_mesh_is_physics_keyword`. |
| `indra/newview/idmcp_meshupload.cpp` (create) | `IDMCPMeshUploadJob`: drives the floater through load → configure → analyze → quote → upload. |
| `indra/newview/idmcptools_upload.cpp` (modify) | Arg parsing and validation for `upload.mesh`, plus its registration and schema. |
| `indra/newview/idmcptoolregistry.h` (modify) | `IDMCPTool::timeout`. |
| `indra/newview/idmcpserver.h` / `.cpp` (modify) | `IDMCP_ERR_BUSY`, and use of the per-tool timeout. |
| `indra/newview/lluploadfloaterobservers.h` (modify) | `onModelUploadSuccessWithResponse`. |
| `indra/newview/llmeshrepository.cpp` (modify) | Dispatch the new observer method with the response body. |
| `indra/newview/llmodelpreview.h`, `llfloatermodelpreview.h` (modify) | `friend class IDMCPMeshUploadJob;` |
| `indra/newview/CMakeLists.txt` (modify) | New source and header entries. |
| `MCP_TOOLS.md` (modify) | Tool docs. |

---

### Task 1: MCP plumbing and the upstream observer hook

Adds the busy error, the per-tool timeout, and the observer method that carries the upload response. A reviewer can approve this on its own: existing tools behave exactly as before, and the stock floater still gets `onModelUploadSuccess()` through the default forward.

**Files:**
- Modify: `indra/newview/idmcpserver.h` (the `EIDMCPError` enum)
- Modify: `indra/newview/idmcptoolregistry.h:39-46`
- Modify: `indra/newview/idmcpserver.cpp:494`
- Modify: `indra/newview/lluploadfloaterobservers.h:65-80`
- Modify: `indra/newview/llmeshrepository.cpp:3412` and its include block

**Interfaces:**
- Produces:
  - `IDMCP_ERR_BUSY` (= -32006)
  - `F64 IDMCPTool::timeout` (0 = the server's 30 s default)
  - `virtual void LLWholeModelUploadObserver::onModelUploadSuccessWithResponse(const LLSD& response)`

- [ ] **Step 1: Add the busy error code**

In `indra/newview/idmcpserver.h`, inside `enum EIDMCPError`, add after `IDMCP_ERR_CAP_UNAVAIL    = -32005,`:

```cpp
    IDMCP_ERR_BUSY           = -32006,  // a single-instance resource is in use
```

- [ ] **Step 2: Add the per-tool timeout field**

In `indra/newview/idmcptoolregistry.h`, in `struct IDMCPTool`, add after the `gate` member:

```cpp
    // Deadline for a deferred call, in seconds. 0 = the server default (30 s).
    F64 timeout = 0.0;
```

- [ ] **Step 3: Use it in the server**

In `indra/newview/idmcpserver.cpp`, replace line 494:

```cpp
            call->setDeadline(now + DEFAULT_TOOL_TIMEOUT);
```

with:

```cpp
            call->setDeadline(now + (tool->timeout > 0.0 ? tool->timeout : DEFAULT_TOOL_TIMEOUT));
```

- [ ] **Step 4: Add the observer method**

In `indra/newview/lluploadfloaterobservers.h`, in `class LLWholeModelUploadObserver`, replace:

```cpp
    virtual void onModelUploadSuccess() = 0;
```

with:

```cpp
    virtual void onModelUploadSuccess() = 0;

    // <ID> Same as onModelUploadSuccess, plus the server response
    // (new_inventory_item, new_asset). The default drops the response, so
    // existing observers are unchanged. Separate name, not an overload:
    // an overload would be hidden in subclasses (-Woverloaded-virtual).
    virtual void onModelUploadSuccessWithResponse(const LLSD& response) { onModelUploadSuccess(); }
```

- [ ] **Step 5: Dispatch it from the upload thread**

In `indra/newview/llmeshrepository.cpp`, at the `body["state"].asString() == "complete"` branch (around line 3412), replace:

```cpp
                    doOnIdleOneTime(boost::bind(&LLWholeModelUploadObserver::onModelUploadSuccess, observer));
```

with:

```cpp
                    // <ID> hand observers the response so they can read the new item/asset ids.
                    // llsd_clone: deep copy, so the main thread never shares refcounts with this one.
                    doOnIdleOneTime(boost::bind(&LLWholeModelUploadObserver::onModelUploadSuccessWithResponse, observer, llsd_clone(body)));
```

Add to the include block at the top of the file, if `llsdutil.h` isn't already included:

```cpp
#include "llsdutil.h" // <ID> llsd_clone for the upload observer response
```

- [ ] **Step 6: Compile-read checklist**

Confirm by reading: `tool` is the `const IDMCPTool*` in scope at `idmcpserver.cpp:494` (it's used at line 486 as `tool->invoke`). `LLSD` is already declared in `lluploadfloaterobservers.h` (the fee observer uses it). `LLFloaterModelPreview` doesn't override `onModelUploadSuccessWithResponse`, so it gets the default.

- [ ] **Step 7: Commit**

```bash
git add indra/newview/idmcpserver.h indra/newview/idmcptoolregistry.h indra/newview/idmcpserver.cpp indra/newview/lluploadfloaterobservers.h indra/newview/llmeshrepository.cpp
git commit -m "feat: add MCP busy error, per-tool timeout, and upload response hook"
```

---

### Task 2: Mesh upload job, dry-run path

Creates the job, which opens and drives the floater through load, configure, optional analyze, validation and the fee quote, then answers with the dry-run result. With `confirm` set, it stops after the quote and answers with an error until Task 3 lands. A reviewer can check the floater-driving logic independently of spending L$.

**Files:**
- Create: `indra/newview/idmcp_meshupload.h`
- Create: `indra/newview/idmcp_meshupload.cpp`
- Modify: `indra/newview/llmodelpreview.h:275` (friend)
- Modify: `indra/newview/llfloatermodelpreview.h:140` (friend)
- Modify: `indra/newview/CMakeLists.txt:105` and `:986`

**Interfaces:**
- Consumes: `IDMCP_ERR_BUSY` and `IDMCPTool::timeout` (Task 1), `idmcp_tool_ok`/`idmcp_tool_err` (`idmcpserver.h:148-149`), `IDMCPCall::setCleanup` (`idmcpserver.h`).
- Produces (Task 3 extends the class; Task 4 calls the free functions):
  - `struct IDMCPMeshUploadParams` (fields below)
  - `void idmcp_mesh_upload_start(const IDMCPMeshUploadParams& p, const IDMCPCallPtr& call)`
  - `bool idmcp_mesh_is_physics_keyword(const std::string& lowered)`
  - private `IDMCPMeshUploadJob` members `mQuotedPrice`, `mLandImpact`, `mUploadReply`, `mUploadResponse`, and stage `Stage::Uploading`, which Task 3 fills in

- [ ] **Step 1: Create the header**

Create `indra/newview/idmcp_meshupload.h`:

```cpp
/**
 * @file idmcp_meshupload.h
 * @brief <ID> MCP server: upload.mesh job (drives the Upload Model floater).
 *
 * Part of Five's custom Firestorm fork. Custom code carries an `ID` prefix.
 */

#ifndef ID_IDMCP_MESHUPLOAD_H
#define ID_IDMCP_MESHUPLOAD_H

#include "idmcpserver.h"   // IDMCPCallPtr
#include "lluuid.h"

#include <string>

struct IDMCPMeshUploadParams
{
    std::string path;                 // .dae / .gltf / .glb, already validated
    std::string name;                 // inventory name
    LLUUID      dest;                 // null = the Objects folder
    F32         scale           = 1.f;
    bool        textures        = false;
    int         skin_weights    = -1; // -1 = keep what the floater detected, 0 = off, 1 = on
    int         joint_positions = -1;
    int         lock_scale      = -1;
    std::string lod_file[3];          // by LLModel::LOD_IMPOSTOR..LOD_MEDIUM; empty = auto
    std::string physics = "lowest";   // a keyword (lower case) or a file path
    bool        analyze = false;
    bool        confirm = false;
};

// True for "none", "high", "medium", "low", "lowest", "cube". Expects lower case.
bool idmcp_mesh_is_physics_keyword(const std::string& lowered);

// Starts the job. Always answers `call`, possibly synchronously (busy, bad
// args). Main thread only.
void idmcp_mesh_upload_start(const IDMCPMeshUploadParams& p, const IDMCPCallPtr& call);

#endif // ID_IDMCP_MESHUPLOAD_H
```

- [ ] **Step 2: Grant the job friend access**

In `indra/newview/llmodelpreview.h`, after line 275 (`friend class LLFloaterModelPreview::DecompRequest;`), add:

```cpp
    friend class IDMCPMeshUploadJob; // <ID> MCP upload.mesh drives the preview
```

In `indra/newview/llfloatermodelpreview.h`, after line 140 (`friend class LLMeshFilePicker;`), add:

```cpp
    friend class IDMCPMeshUploadJob; // <ID> MCP upload.mesh drives the floater
```

- [ ] **Step 3: Create the job implementation**

Create `indra/newview/idmcp_meshupload.cpp`:

```cpp
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
#include "llviewertexteditor.h"

#include <deque>
#include <memory>
#include <sstream>
#include <utility>

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
    enum class Reply { None, Ok, Failed };

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

    boost::json::array logLines(LLFloaterModelPreview* fmp) const;
    void fail(int code, const std::string& msg, boost::json::value data = boost::json::value());
    void succeed(boost::json::object result);
    void finish();
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
    std::deque<std::pair<S32, std::string>> mPendingLoads;   // (LLModel lod, path)

    S32   mQuotedPrice = 0;
    F64   mLandImpact  = 0.0;

    LLMutex     mReplyMutex;        // guards everything below
    Reply       mFeeReply = Reply::None;
    LLSD        mFee;
    std::string mUploadUrl;
    std::string mFeeError;
    Reply       mUploadReply = Reply::None;
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

    LLFloaterModelPreview* fmp = floater();
    if (!fmp || !fmp->mModelPreview)
    {
        fail(IDMCP_ERR_BUSY, "upload model floater was closed");
        return;
    }
    if (now > mStageDeadline)
    {
        fail(IDMCP_ERR_TIMEOUT, std::string("timed out while ") + stageName(mStage));
        return;
    }

    LLModelPreview* mp = fmp->mModelPreview;
    if (mp->getLoadState() >= LLModelLoader::ERROR_PARSING)
    {
        boost::json::object data;
        data["log"] = logLines(fmp);
        fail(IDMCP_ERR_INVALID_PARAMS, "the model failed to load", std::move(data));
        return;
    }

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

    case Stage::Uploading:
        handleUpload();
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
    const bool idle = !mp->mLoading && !mp->mGenLOD && mp->mLodsQuery.empty() && !mp->mDirty
                      && (!mParams.textures || mp->areTexturesReady());
    mSettleTicks = idle ? mSettleTicks + 1 : 0;
    return mSettleTicks >= SETTLE_TICKS;
}

bool IDMCPMeshUploadJob::requestPending()
{
    LLMutexLock lock(&mReplyMutex);
    if (mStage == Stage::Quoting)   return mFeeReply == Reply::None;
    if (mStage == Stage::Uploading) return mUploadReply == Reply::None;
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
// rigged option the model can't satisfy.
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

    auto set_and_commit = [fmp](const char* name, const LLSD& value)
    {
        LLUICtrl* ctrl = fmp->getChild<LLUICtrl>(name);
        ctrl->setValue(value);
        ctrl->onCommit();
    };
    set_and_commit("import_scale", LLSD((F64)mParams.scale));
    set_and_commit("upload_textures", LLSD(mParams.textures));
    if (mParams.skin_weights >= 0)    set_and_commit("upload_skin", LLSD(mParams.skin_weights == 1));
    if (mParams.joint_positions >= 0) set_and_commit("upload_joints", LLSD(mParams.joint_positions == 1));
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
        fail(IDMCP_ERR_INVALID_PARAMS, "the importer rejected the model", std::move(data));
        return;
    }

    // rebuildUploadData reads the item name from description_form.
    fmp->getChild<LLUICtrl>("description_form")->setValue(mParams.name);
    mp->rebuildUploadData();

    LLFloaterModelPreview::lod_sources_map_t lod_sources;
    fmp->fillLODSourceStatistics(lod_sources);

    {
        LLMutexLock lock(&mReplyMutex);
        mFeeReply = Reply::None;
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
    if (reply == Reply::None) return;
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
        succeed(std::move(o));
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

// Task 3 replaces this body.
void IDMCPMeshUploadJob::sendUpload(LLFloaterModelPreview* fmp, const std::string& url)
{
    fail(IDMCP_ERR_CAP_UNAVAIL, "confirmed mesh upload isn't implemented yet");
}

// Task 3 replaces this body.
void IDMCPMeshUploadJob::handleUpload()
{
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
    mFeeError = llformat("fee request failed (%d): %s", status, reason.c_str());
    if (result.has("message"))
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
    if (fmp)
    {
        fmp->closeFloater(false);
    }
}

// The MCP call ended without us answering (timeout or disconnect).
void IDMCPMeshUploadJob::cancel()
{
    if (!mFinished) finish();
}

void IDMCPMeshUploadJob::release()
{
    gIdleCallbacks.deleteFunction(&IDMCPMeshUploadJob::onIdle, nullptr);
    sActive.reset();
}
```

- [ ] **Step 4: Add the files to CMake**

In `indra/newview/CMakeLists.txt`, after line 105 (`idmcptools_upload.cpp  # <ID> ...`), add:

```cmake
    idmcp_meshupload.cpp  # <ID> Embedded MCP server: upload.mesh floater job
```

After line 986 (`idmcptools.h  # <ID> ...`), add:

```cmake
    idmcp_meshupload.h  # <ID> Embedded MCP server: upload.mesh floater job
```

- [ ] **Step 5: Compile-read checklist**

Read each of these in the source to confirm:
- `LLFloaterModelPreview::lod_sources_map_t`, `fillLODSourceStatistics`, `resetUploadOptions`, `mUploadLogText` and `mCurRequest` are reachable through the new friend line (`llfloatermodelpreview.h`).
- `LLModelPreview::mModel`, `mLoading`, `mGenLOD`, `mDirty`, `mModelNoErrors`, `mUploadData` and `mPreviewScale` are reachable through the new friend line (`llmodelpreview.h`). `mLodsQuery` and `areTexturesReady()` are already public.
- `gMeshRepo.mDecompThread` and `LLPhysicsDecomp::mStageID` are public (the floater uses both at `llfloatermodelpreview.cpp:1353`).
- `LLModel::mSkinWeights` and `mSkinInfo.mAlternateBindMatrix` exist (used at `llmodelpreview.cpp:1350-1354`).
- `lod_name` is the `static const std::string[]` in `llmodelpreview.h:52`.
- `LLFloaterReg::showTypedInstance` and `instanceVisible` signatures match `llui/llfloaterreg.h:151,188`.
- `LLMutex` has a default constructor (`llcommon/llmutex.h:50`).
- The unused `fmp`/`url` parameters in the Task 3 stubs only produce warnings. If the build treats unused parameters as errors, comment the names out until Task 3.

- [ ] **Step 6: Commit**

```bash
git add indra/newview/idmcp_meshupload.h indra/newview/idmcp_meshupload.cpp indra/newview/llmodelpreview.h indra/newview/llfloatermodelpreview.h indra/newview/CMakeLists.txt
git commit -m "feat: add mesh upload job that drives the model floater to a fee quote"
```

---

### Task 3: Confirmed upload

Replaces the two stubs so `confirm:true` spends L$ and reports the new item. Kept separate so a reviewer can hold back the L$-spending path while approving the dry run.

**Files:**
- Modify: `indra/newview/idmcp_meshupload.cpp` (the `sendUpload` and `handleUpload` bodies)

**Interfaces:**
- Consumes: `onModelUploadSuccessWithResponse` (Task 1), `mQuotedPrice`, `mLandImpact`, `mUploadReply`, `mUploadResponse`, `Stage::Uploading`, `enter`, `fail`, `succeed` (Task 2).
- Produces: confirmed-call result `{ uploaded, item_id, asset_id, cost, land_impact }`.

- [ ] **Step 1: Replace `sendUpload`**

Replace the stub, including its `// Task 3 replaces this body.` comment, with:

```cpp
void IDMCPMeshUploadJob::sendUpload(LLFloaterModelPreview* fmp, const std::string& url)
{
    LLModelPreview* mp = fmp->mModelPreview;
    LLFloaterModelPreview::lod_sources_map_t lod_sources;
    fmp->fillLODSourceStatistics(lod_sources);

    {
        LLMutexLock lock(&mReplyMutex);
        mUploadReply = Reply::None;
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
```

- [ ] **Step 2: Replace `handleUpload`**

Replace the stub, including its comment, with:

```cpp
void IDMCPMeshUploadJob::handleUpload()
{
    Reply reply;
    LLSD  response;
    {
        LLMutexLock lock(&mReplyMutex);
        reply    = mUploadReply;
        response = mUploadResponse;
    }
    if (reply == Reply::None) return;
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
```

- [ ] **Step 3: Compile-read checklist**

Confirm that `LLMeshRepository::uploadModel` (`llmeshrepository.cpp:5416`) takes `upload_url` by value as its 9th argument, and the upload observer handle last. Confirm that `requestPending()` returns true in `Stage::Uploading` until a reply lands, so a timeout during upload parks the job and doesn't free it.

- [ ] **Step 4: Commit**

```bash
git add indra/newview/idmcp_meshupload.cpp
git commit -m "feat: complete confirmed mesh uploads and report the new item"
```

---

### Task 4: Register `upload.mesh` and document it

Adds argument parsing, validation, the schema and the registration, and documents the tool. After this task, agents can see and call the tool.

**Files:**
- Modify: `indra/newview/idmcptools_upload.cpp` (includes, anonymous namespace, `idmcp_register_upload_tools`)
- Modify: `MCP_TOOLS.md:192-206`

**Interfaces:**
- Consumes: `IDMCPMeshUploadParams`, `idmcp_mesh_upload_start`, `idmcp_mesh_is_physics_keyword` (Task 2), `IDMCPTool::timeout` (Task 1), and these existing file-local helpers: `lower`, `arg_str`, `arg_bool`, `arg_has`, `arg_flt`, `resolve_folder`, `name_for`.
- Produces: the registered `upload.mesh` tool.

- [ ] **Step 1: Add includes**

In `indra/newview/idmcptools_upload.cpp`, after `#include "idmcpserver.h"`, add:

```cpp
#include "idmcp_meshupload.h"       // upload.mesh job
```

After `#include "llgltfmaterial.h"` (in the "GLTF material upload" group), add:

```cpp
// Mesh upload
#include "llmodel.h"                // LLModel::LOD_* indices
```

- [ ] **Step 2: Add the arg parser**

Inside the anonymous namespace, directly before the `// Shared JSON-Schema fragment for the simple` comment, add:

```cpp
    // ---- upload.mesh -----------------------------------------------------

    void run_mesh_tool(const boost::json::object& args, const IDMCPCallPtr& call)
    {
        IDMCPMeshUploadParams p;
        p.path = arg_str(args, "path");
        if (p.path.empty())
        {
            idmcp_tool_err(call, IDMCP_ERR_INVALID_PARAMS, "path is required");
            return;
        }
        if (!gDirUtilp->fileExists(p.path))
        {
            idmcp_tool_err(call, IDMCP_ERR_INVALID_PARAMS, "file not found: " + p.path);
            return;
        }
        const std::string ext = lower(gDirUtilp->getExtension(p.path));
        if (ext != "dae" && ext != "gltf" && ext != "glb")
        {
            idmcp_tool_err(call, IDMCP_ERR_INVALID_PARAMS, "unsupported extension for upload.mesh: ." + ext);
            return;
        }

        p.name = name_for({ arg_str(args, "name") }, 0, p.path);
        const std::string dest = arg_str(args, "dest");
        p.dest = resolve_folder(dest);
        if (!dest.empty() && p.dest.isNull())
        {
            idmcp_tool_err(call, IDMCP_ERR_NOT_FOUND, "unknown dest folder: " + dest);
            return;
        }

        p.scale = arg_flt(args, "scale", 1.f);
        if (p.scale <= 0.f)
        {
            idmcp_tool_err(call, IDMCP_ERR_INVALID_PARAMS, "scale must be greater than 0");
            return;
        }
        p.textures = arg_bool(args, "textures", false);

        auto it = args.find("rigged");
        if (it != args.end() && it->value().is_object())
        {
            const boost::json::object& r = it->value().as_object();
            auto tri = [&r](const char* k) { return arg_has(r, k) ? (arg_bool(r, k) ? 1 : 0) : -1; };
            p.skin_weights    = tri("skin_weights");
            p.joint_positions = tri("joint_positions");
            p.lock_scale      = tri("lock_scale_if_joint_position");
        }

        it = args.find("lods");
        if (it != args.end() && it->value().is_object())
        {
            const boost::json::object& l = it->value().as_object();
            const std::pair<const char*, S32> slots[] = {
                { "lowest", LLModel::LOD_IMPOSTOR },
                { "low",    LLModel::LOD_LOW },
                { "medium", LLModel::LOD_MEDIUM },
            };
            for (const auto& [key, lod] : slots)
            {
                const std::string v = arg_str(l, key);
                if (v.empty() || lower(v) == "auto") continue;
                if (!gDirUtilp->fileExists(v))
                {
                    idmcp_tool_err(call, IDMCP_ERR_INVALID_PARAMS,
                                   std::string("lods.") + key + ": file not found: " + v);
                    return;
                }
                p.lod_file[lod] = v;
            }
        }

        const std::string phys = arg_str(args, "physics");
        if (!phys.empty())
        {
            const std::string k = lower(phys);
            if (idmcp_mesh_is_physics_keyword(k))
            {
                p.physics = k;
            }
            else if (gDirUtilp->fileExists(phys))
            {
                p.physics = phys;
            }
            else
            {
                idmcp_tool_err(call, IDMCP_ERR_INVALID_PARAMS,
                               "physics: not none/high/medium/low/lowest/cube and no such file: " + phys);
                return;
            }
        }

        p.analyze = arg_bool(args, "analyze", false);
        p.confirm = arg_bool(args, "confirm", false);
        idmcp_mesh_upload_start(p, call);
    }
```

- [ ] **Step 3: Add the schema**

After the `SCHEMA_ANIM` definition (still inside the anonymous namespace), add:

```cpp
    const char* SCHEMA_MESH =
        R"({"type":"object","properties":{)"
        R"("path":{"type":"string"},)"
        R"("name":{"type":"string"},)"
        R"("dest":{"type":"string"},)"
        R"("scale":{"type":"number"},)"
        R"("textures":{"type":"boolean"},)"
        R"("rigged":{"type":"object","properties":{)"
            R"("skin_weights":{"type":"boolean"},)"
            R"("joint_positions":{"type":"boolean"},)"
            R"("lock_scale_if_joint_position":{"type":"boolean"}},"additionalProperties":false},)"
        R"("lods":{"type":"object","properties":{)"
            R"("medium":{"type":"string"},)"
            R"("low":{"type":"string"},)"
            R"("lowest":{"type":"string"}},"additionalProperties":false},)"
        R"("physics":{"type":"string"},)"
        R"("analyze":{"type":"boolean"},)"
        R"("confirm":{"type":"boolean"}},"required":["path"],"additionalProperties":false})";
```

- [ ] **Step 4: Register the tool**

In `idmcp_register_upload_tools`, after the `upload.material` block (the last `reg.add(std::move(t));` at line 651 and its closing brace), add:

```cpp
    // upload.mesh ------------------------------------------------------------
    {
        IDMCPTool t;
        t.name = "upload.mesh";
        t.description =
            "Upload one mesh model (.dae/.gltf/.glb) through the viewer's Upload "
            "Model floater. {\"path\"} is required. Optional: {\"name\"}, "
            "{\"dest\"} (default = Objects), {\"scale\"}, {\"textures\"} (upload "
            "embedded textures, extra L$), {\"rigged\":{\"skin_weights\", "
            "\"joint_positions\",\"lock_scale_if_joint_position\"}} (default = "
            "whatever the model contains), {\"lods\":{\"medium\"|\"low\"|"
            "\"lowest\": \"auto\" or a file path}}, {\"physics\"}: \"none\"|"
            "\"high\"|\"medium\"|\"low\"|\"lowest\"|\"cube\"|file path (default "
            "\"lowest\"), {\"analyze\"} (convex hull decomposition). WITHOUT "
            "{\"confirm\":true} = dry run: returns the server's fee quote, land "
            "impact, weights and triangle counts, spending nothing. With "
            "{\"confirm\":true} = upload (costs L$). One mesh at a time; returns "
            "a busy error if the Upload Model floater is already open.";
        t.input_schema = boost::json::parse(SCHEMA_MESH);
        t.timeout = 900.0;
        t.invoke = [](const boost::json::object& args, const IDMCPCallPtr& call)
        { run_mesh_tool(args, call); };
        reg.add(std::move(t));
    }
```

- [ ] **Step 5: Update the file header comment**

At the top of `idmcptools_upload.cpp`, change the `@brief` line to:

```cpp
 * @brief <ID> MCP server: asset upload tools (image / sound / animation / material / mesh).
```

and add this paragraph after the paragraph that ends `...mirrors get_bulk_upload_expected_cost.`:

```cpp
 *
 * Mesh is different: it needs the Upload Model floater, so upload.mesh only
 * parses and validates its args here and hands off to idmcp_meshupload.cpp.
 * Its dry run asks the server for a fee quote rather than estimating locally.
```

- [ ] **Step 6: Document the tool**

In `MCP_TOOLS.md`, add this row after the `upload.material` row (line 199):

```markdown
| `upload.mesh` | `path`*: string<br>`name`: string<br>`dest`: string<br>`scale`: number<br>`textures`: boolean<br>`rigged`: {`skin_weights`, `joint_positions`, `lock_scale_if_joint_position`}: boolean<br>`lods`: {`medium`, `low`, `lowest`}: `"auto"` or file path<br>`physics`: string<br>`analyze`: boolean<br>`confirm`: boolean | Upload one mesh model (`.dae`/`.gltf`/`.glb`) through the Upload Model floater, which opens while the tool runs. `rigged` options default to what the model contains. `physics` is `none`, `high`, `medium`, `low`, `lowest` (default), `cube`, or a file path. `analyze` runs convex hull decomposition on the physics shape. The dry run returns the server's fee quote with land impact, weights and triangle counts per LOD. Dry-run without `confirm:true`; upload with it. |
```

In the notes list under the table (after the "Batch = one response." bullet), add:

```markdown
- **Mesh takes one file per call and can take minutes.** `upload.mesh` has a 15-minute timeout. It returns a busy error (-32006) if the Upload Model floater is already open. Closing the floater while the tool runs cancels it.
```

- [ ] **Step 7: Compile-read checklist**

Confirm that `name_for` takes `const std::vector<std::string>&` (`idmcptools_upload.cpp:302`), so the braced `{ arg_str(...) }` builds a one-element vector. Confirm that `arg_has`, `arg_bool` and `arg_str` take `const boost::json::object&`, so they work on the nested `rigged` and `lods` objects. Confirm that `run_mesh_tool` and `SCHEMA_MESH` sit inside the same anonymous namespace as `SCHEMA_SIMPLE`.

- [ ] **Step 8: Commit**

```bash
git add indra/newview/idmcptools_upload.cpp MCP_TOOLS.md
git commit -m "feat: register upload.mesh MCP tool"
```

---

### Task 5: Build and live verification (user)

The user builds and runs the viewer. The implementer prepares the test files, hands over the matrix below, and records the results. Use a region where you can rez. Run every dry run before its confirmed run.

**Files:**
- Create (scratch, not committed): `cube.dae` (a single triangulated cube), `rigged.dae` (any rigged mesh with joint offsets), `textured.glb` (any GLB with one embedded texture), `broken.dae` (`cube.dae` truncated halfway), `cube_lowest.dae` (a cube with a different mesh name, to cause a LOD mismatch).

- [ ] **Step 1: Ask the user to build**

Tell the user: "Ready to build. Please build the viewer, log in, and enable the MCP server. I'll run the checks below over MCP."

- [ ] **Step 2: Run the matrix**

| # | Call | Expected |
|---|---|---|
| 1 | `upload.mesh {path: cube.dae}` | `dry_run:true`, `upload_price` > 0, `land_impact` > 0, `triangles.high` = 12. The floater closes afterwards. Balance unchanged. |
| 2 | Same with `confirm:true` | `uploaded:true`. `inventory.getItem` on `item_id` shows an object in Objects whose asset matches `asset_id`. Balance down by `cost`. |
| 3 | `rigged.dae`, dry run | Price includes skin. Then `confirm:true`: the item is wearable and deforms with the avatar. |
| 4 | `textured.glb`, `textures:true`, dry run | `price_breakdown.textures` > 0. |
| 5 | `cube.dae`, `physics:"cube"`, `analyze:true`, dry run | Completes in under 5 min. `triangles.physics` > 0. |
| 6 | Open Build → Upload → Model by hand, then call #1 | Error -32006 `upload model floater is in use`. The hand-opened floater is untouched. |
| 7 | `broken.dae` | `the model failed to load` or `the importer rejected the model`, with `log` lines. The floater closes. |
| 8 | #5 again, then disconnect the MCP client mid-analyze | The floater closes. No item appears. Calling #1 again 5 s later works (not busy). |
| 9 | Any other deferred tool that takes > 30 s | Still times out at 30 s (the default is unchanged). |
| 10 | Start #5, close the floater by hand mid-analyze | Error `upload model floater was closed` within a second. |
| 11 | Start #2 with a fresh cube, disconnect right after the floater closes (upload in flight) | No crash. The item may still appear. A new call 30 s later is not busy. |
| 12 | `cube.dae`, `rigged:{skin_weights:true}` | Error `rigged.skin_weights: the model has no skin weights`. |
| 13 | `cube.dae`, `lods:{lowest:"cube_lowest.dae"}` | An importer error (or a warning in `warnings` if the floater accepts it). Not a hang. |
| 14 | On an account without mesh upload rights (if one is available) | A `fee request failed ...` error carrying the server's reason. |
| 15 | Upload a mesh through the floater by hand, the normal way | Works as before (Task 1's default forward). |

- [ ] **Step 3: Record results and fix**

For any failure, use superpowers:systematic-debugging before changing code. Commit each fix as `fix: ...` on this branch.
