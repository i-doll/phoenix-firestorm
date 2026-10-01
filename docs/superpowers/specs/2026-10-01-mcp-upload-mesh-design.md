# Design: `upload.mesh`

Date: 2026-10-01
Status: approved in chat, awaiting spec review

## Problem

The MCP upload tools cover images, sounds, animations and GLTF materials. Agents that
build models (in Blender scripts, or by generating `.dae`/`.glb` files directly) can't get
those models into Second Life without a person clicking through the Upload Model floater.

The tool must handle both static props (LODs and a physics shape matter) and rigged
wearables (skin weights and joint positions matter).

## Why mesh can't share `run_upload_tool`

The other upload tools hand a file to `LLNewFileResourceUploadInfo`, which encodes and
uploads it in one step. Mesh goes through a multi-stage pipeline:

1. A threaded loader (`LLDAELoader` / `LLGLTFLoader`) parses the file.
2. LOD and physics meshes are generated or loaded, and decomposition is optional.
3. The importer validates the result (`LLModelPreview::mModelNoErrors`).
4. `gMeshRepo.uploadModel(..., do_upload=false, fee_observer)` asks the server for a fee
   quote and an upload URL.
5. `gMeshRepo.uploadModel(..., do_upload=true, upload_observer)` uploads.

Stages 1–3 live in `LLModelPreview`, which dereferences its floater (`mFMP`) about 126
times. It can't run without `LLFloaterModelPreview`.

## Constraints discovered while reading the viewer

1. **The floater is a singleton.** `LLFloaterModelPreview::sInstance`, registered as
   `upload_model`. Only one mesh upload can be in flight, and an agent job must not take
   over a floater the user is working in.
2. **Loading by path skips the picker.** `LLFloaterModelPreview::loadModel(lod, filename,
   force_disable_slm)` loads a file directly. `setModelLoadedCallback` on `LLModelPreview`
   fires when a load finishes.
3. **Options live in widgets.** Upload options are read with `childGetValue` at
   calculate and upload time: `upload_textures`, `upload_skin`, `upload_joints`,
   `lock_scale_if_joint_position`, `import_scale`. LOD sources are combos
   `lod_source_{high,medium,low,lowest}` with values `Load from file` / `MeshOpt Auto` /
   `MeshOptCombine`. The physics source is `physics_lod_combo`, which `onPhysicsUseLOD`
   reads by index (`physics_high` … `physics_lowest`, presets, `physics_cube`,
   `load_from_file`). Setting a widget alone does nothing. Each set must be followed by
   the control's commit callback so the floater reacts as it would to a click.
4. **Fee and upload observers are passed by handle.** `uploadModel` takes
   `LLHandle<LLWholeModelFeeObserver>` and `LLHandle<LLWholeModelUploadObserver>`, so a
   job object can receive the results instead of the floater.
5. **The upload observer doesn't receive the new item.** `onModelUploadSuccess()` takes
   no arguments (`lluploadfloaterobservers.h:71`). The server response carrying
   `new_inventory_item` / `new_asset` is consumed in `llmeshrepository.cpp` around line
   3409 and never reaches the observer.
6. **Deferred calls time out after 30 s.** `idmcpserver.cpp:494` sets
   `DEFAULT_TOOL_TIMEOUT` after `invoke` returns, and `sweepTimeouts` then fails the call
   with `-32001`. Loading a large model plus decomposition can take minutes.
7. **Decomposition is asynchronous.** `onPhysicsStageExecute` queues `DecompRequest`s
   into `sInstance->mCurRequest`. `DecompRequest::completed()` erases each one, so the
   stage is finished when that set is empty again.

## Tool interface

```
upload.mesh {
  path       string, required. One .dae / .gltf / .glb file.
  name       string. Inventory name; defaults to the file stem.
  dest       string. Folder UUID or well-known name; default is the Objects folder.
  scale      number, default 1.0
  textures   bool, default false (embedded textures cost extra L$)
  rigged     { skin_weights: bool, joint_positions: bool,
               lock_scale_if_joint_position: bool }
             each defaults to what the floater detects in the model
  lods       { medium | low | lowest: "auto" | <file path> }   default "auto"
  physics    "none" | "high" | "medium" | "low" | "lowest" | "cube" | <file path>
             default "none" (the floater's own default; "lowest" made the server
             reject detailed meshes for degenerate physics triangles)
  analyze    bool, default false (convex hull decomposition of the physics shape)
  confirm    bool, default false
}
```

`name` and `dest` reuse `name_for` / `resolve_folder` from `idmcptools_upload.cpp`, so
behaviour matches the other upload tools.

### Dry run (no `confirm`)

Runs stages 1–4 and spends nothing. Returns:

```
{
  dry_run: true,
  confirm_hint: "re-call with \"confirm\":true to upload",
  upload_price, price_breakdown: { model, streaming, physics, instances, textures },
  land_impact,                         // resource_cost
  weights: { download, physics, server },
  triangles: { high, medium, low, lowest, physics },
  warnings: [ ... ],                   // importer log lines
  balance, affordable
}
```

### Confirmed

Runs stages 1–4 again from scratch. Skips the upload with an error if the fresh quote
exceeds the balance. Otherwise runs stage 5 and returns:

```
{ uploaded: true, item_id, asset_id, cost, land_impact }
```

### Errors

| Situation | Code | Message |
|---|---|---|
| Missing/unsupported `path` | INVALID_PARAMS | as for other upload tools |
| Floater already open or a job is running | new `IDMCP_ERR_BUSY = -32006` | `upload model floater is in use` |
| Importer errors (`mModelNoErrors == false`) | tool error | the floater's status text plus error log lines |
| Fee request failed | tool error | status and reason from `setModelPhysicsFeeErrorStatus` |
| Quote above balance (confirmed only) | tool error | includes price and balance |
| Stage timeout | tool error | names the stage that stalled |
| Upload failed | tool error | server reason where available |

## Architecture

### `IDMCPMeshUploadJob` (new, `idmcp_meshupload.cpp/.h`)

Inherits `LLWholeModelFeeObserver` and `LLWholeModelUploadObserver`, and is held by a
`shared_ptr` captured in the call's cleanup. A static weak pointer to the active job
handles the busy check. All work happens on the main thread and each stage starts from
the previous stage's callback:

1. **Busy check.** Fail if `LLFloaterReg::findInstance("upload_model")` is visible or a
   job is active.
2. **Open and configure.** `LLFloaterReg::showInstance("upload_model")`. Set the option
   widgets from constraint 3 and fire each commit callback.
3. **Load.** Connect `setModelLoadedCallback`, then call `loadModel(LOD_HIGH, path, true)`.
   When it fires, load each LOD given as a file path and then a physics file if one was
   given, waiting for the callback after each. Select `physics_lod_combo` by index for the
   non-file physics choices. `"none"` leaves the combo at `choose_one`.
4. **Analyze** (optional). Trigger the Decompose stage and poll `mCurRequest.empty()` on
   idle.
5. **Validate.** Check `mModelNoErrors` and collect the status and log text.
6. **Quote.** Call `rebuildUploadData()` and `fillLODSourceStatistics`, then
   `gMeshRepo.uploadModel(..., do_upload=false, getWholeModelFeeObserverHandle())`, with
   the same arguments as `onClickCalculateBtn`.
7. **Finish.** A dry run responds and closes the floater. A confirmed run checks the
   balance, calls `uploadModel(do_upload=true, ..., getWholeModelUploadObserverHandle())`
   as `onUpload` does, then responds and closes the floater.

Each wait stage has its own time limit (load 120 s, analyze 300 s, quote 60 s, upload
300 s), checked on idle. When a limit runs out the job responds with an error naming the
stage and closes the floater. If the MCP call ends early (timeout or dropped
connection), the call's cleanup cancels pending decomposition, disconnects signals and
closes the floater.

The job reads LLModelPreview members (`mModelNoErrors`, `mUploadData`, `mPreviewScale`,
triangle counts) and the floater's LOD statistics. Where these are private, use
`<ID>`-tagged `friend` declarations instead of making them public.

Rigged options follow the floater's own detection: loading a model with skin weights
turns `upload_skin` on, and loading one with joint offsets turns `upload_joints` on
(`llmodelpreview.cpp:1373-1389`). An explicit `true` on a model without that data is an
error. An explicit `false` turns the option off.

### Upstream changes (all `<ID>`-tagged)

- `lluploadfloaterobservers.h`: add
  `virtual void onModelUploadSuccessWithResponse(const LLSD& response) { onModelUploadSuccess(); }`.
  The default forwards to the existing method, so `LLFloaterModelPreview` keeps working
  unchanged. It has a new name rather than overloading `onModelUploadSuccess`, because an
  overload would be hidden in `LLFloaterModelPreview` and trip `-Woverloaded-virtual`.
- `llmeshrepository.cpp` (~line 3412): dispatch `onModelUploadSuccessWithResponse` with
  `body`.
- `idmcpserver.h`: add `IDMCP_ERR_BUSY = -32006` to `EIDMCPError`.
- `idmcptoolregistry.h`: add `F64 timeout = 0.0` to `IDMCPTool`.
  `idmcpserver.cpp:494` uses it when non-zero and falls back to `DEFAULT_TOOL_TIMEOUT`
  otherwise. `upload.mesh` sets 900 s, which covers the per-stage limits (780 s in total).

### Registration

`upload.mesh` is registered next to the other upload tools in `idmcptools_upload.cpp`
and calls into `idmcp_meshupload`. The new `.cpp` goes in `newview/CMakeLists.txt`.
`MCP_TOOLS.md` gets an entry, and the `firestorm-avatar` skill docs get one too if they
list the upload tools.

## Non-goals

- Batch uploads (`paths` / `dir`). One mesh per call.
- Hiding the floater while a job runs.
- Exposing per-LOD error thresholds, triangle limits or the decomposition tuning sliders.
  Auto LODs use the floater's defaults.
- SLM (`.slm`) reuse. Loads always pass `force_disable_slm = true`.

## Testing

The project can't be built here, so testing is a manual checklist the user runs after
building:

1. Dry run of a static cube `.dae`: returns a price, land impact and triangle counts, the
   balance doesn't change, and the floater closes.
2. Confirmed run of the same cube: the item appears in Objects (or `dest`), and `item_id`
   and `asset_id` match the inventory item.
3. Rigged `.dae` with `skin_weights` and `joint_positions`: the dry-run price includes
   them and the confirmed upload is wearable.
4. `.glb` input with `textures: true`: the textures appear in the price breakdown.
5. `physics: "cube"` and `analyze: true`: the dry run completes within the time limit.
6. Open the Upload Model floater by hand, then call the tool: it returns the busy error
   and leaves the floater untouched.
7. A malformed `.dae` returns an importer error and the floater closes.
8. Disconnect the client mid-load: the floater closes and no upload happens.
9. Run any other MCP tool afterwards to confirm it still times out at 30 s.
