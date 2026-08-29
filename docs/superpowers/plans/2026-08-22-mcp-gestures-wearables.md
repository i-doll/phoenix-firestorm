# `gesture.*` / `wearable.*` MCP Tools Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Let an MCP agent author gestures and wearables — the missing step between "raw asset uploaded" and "item the avatar can wear or perform".

**Architecture:** Two new tool-area files following the existing `idmcptools_*.cpp` pattern. Gestures ride the modern capability path (`LLDataPacker` serialise → `LLBufferedAssetUploadInfo` → `UpdateGestureAgentInventory`), structurally identical to the existing `notecard.write`. Wearables have no capability, so writes go through `LLAgentWearables::saveWearable` and are therefore **worn-only**; reads work on any item via `LLWearableList::getAsset`.

**Tech Stack:** C++17, Firestorm viewer internals (`LLMultiGesture`, `LLGestureMgr`, `LLWearable`/`LLViewerWearable`, `LLAgentWearables`, `LLAvatarAppearanceDictionary`, `LLVisualParam`, `LLAssetStorage`, `LLViewerAssetUpload`), `boost::json`, the in-repo `IDMCPToolRegistry`.

**Spec:** `docs/superpowers/specs/2026-08-22-mcp-gestures-wearables-design.md`

## Global Constraints

- **No unit-test harness exists for `idmcp*` code.** No test target covers any `idmcp*` translation unit — `indra/newview/tests/` contains none, and `indra/newview/CMakeLists.txt` lists none. These tools depend on live viewer singletons (`gAgent`, `gInventory`, `gAgentAvatarp`, `gAgentWearables`, `LLGestureMgr`, `gAssetStorage`) that cannot be exercised without a running, logged-in viewer. **This plan substitutes the live-MCP verification matrix in Task 6 for the usual red-green unit cycle.** Do not fabricate a unit test that cannot run; do not skip Task 6.
- **Do not build this viewer.** The build needs special setup and belongs to Five. Per-task verification is careful reading plus that task's compile checklist; the real compile and live run happen in Task 6.
- **This plan adds two new `.cpp` files, which forces a full PCH rebuild.** That cost was accepted in the spec. Both files must be added to `indra/newview/CMakeLists.txt` (Tasks 1 and 4) or they will not link.
- **Custom code carries the `ID` prefix** (`IDMCPGestureLoad`, `idmcp_*`), per the fork convention. Never `FS`.
- **Main thread only.** Every function here runs on the viewer main thread. No locking, and never touch these globals from a coroutine.
- **`@edit` gates all four write tools** (`gesture.create`, `gesture.write`, `wearable.create`, `wearable.write`); `@showinv` gates all reads; `gesture.play` declares **no** gate because `LLGestureMgr` already enforces `@sendgesture` internally. This is a recorded decision — do not add a lock-aware check on RLV-locked worn wearables, which was considered and declined.
- **Deferred tools re-check RLV at Commit phase inline**, in the completion callback, before the side effect lands — the pattern at `idmcptools_manage.cpp:284`. Copy that shape, including `data["checkedAt"] = "commit"`.
- **Every heap callback context is deleted on all paths**, including error and failure paths. Use `std::unique_ptr` at the top of each C-style callback.
- **Commit straight to the current `bot-*-master*` branch.** No feature branches.
- Conventional-commit messages. No Co-Authored-By or "Generated with" trailers.

## File Structure

| File | Responsibility |
|---|---|
| `indra/newview/idmcptools_gesture.cpp` (create) | All seven `gesture.*` tools: the JSON⇄`LLMultiGesture` translation, the asset loader, the upload, and the `LLGestureMgr` wrappers. |
| `indra/newview/idmcptools_wearable.cpp` (create) | All three `wearable.*` tools: TE/param enumeration from the appearance dictionary, the worn and unworn read paths, and the worn-only write path. |
| `indra/newview/idmcptools.h` (modify) | Two new registration declarations. |
| `indra/newview/idmcpserver.cpp` (modify) | Two new registration calls in `initSingleton`. |
| `indra/newview/CMakeLists.txt` (modify) | Two new source entries. |
| `doc/rlva-custom-commands.md` (modify) | Note that `@edit` is extended to inventory content in this fork. |

The two tool files share no code. Each duplicates the four tiny arg-parsing helpers (`looks_like_uuid`, `arg_str`, `arg_bool`, `arg_f64`) in its own anonymous namespace, exactly as every existing `idmcptools_*.cpp` does — this fork keeps those file-local rather than hoisting them, and this plan follows the established pattern rather than starting a refactor.

---

### Task 1: Gesture file scaffold + the four non-asset tools

Establishes the file, its registration and its CMake entry, and ships the four gesture tools that need no asset I/O. Reviewable on its own: after this task the server exposes working `gesture.list`, `gesture.activate`, `gesture.deactivate` and `gesture.play`.

**Files:**
- Create: `indra/newview/idmcptools_gesture.cpp`
- Modify: `indra/newview/idmcptools.h` — add one declaration
- Modify: `indra/newview/idmcpserver.cpp:155-169` — add one registration call
- Modify: `indra/newview/CMakeLists.txt:96-109` — add one source entry

**Interfaces:**
- Consumes: `IDMCPToolRegistry`, `IDMCPTool`, `IDMCPCallPtr`, `IDMCPGateResult`, `IDMCPGatePhase` (`idmcptoolregistry.h`); `idmcp_tool_ok`, `idmcp_tool_err`, the `IDMCP_ERR_*` codes (`idmcpserver.h:77-86`); `IDMCPRlvGate::checkBehaviour`, `IDMCPRlvGate::isEnabled` (`idmcprlvgate.h`).
- Produces (Tasks 2 and 3 depend on these by exact name, in the same anonymous namespace):
  - `bool looks_like_uuid(const std::string&)`
  - `std::string arg_str(const boost::json::object&, const char*)`
  - `bool arg_bool(const boost::json::object&, const char*, bool def)`
  - `IDMCPGateResult gate_showinv(const boost::json::object&, IDMCPGatePhase)`
  - `IDMCPGateResult gate_edit(const boost::json::object&, IDMCPGatePhase)`
  - `LLViewerInventoryItem* resolve_gesture_item(const std::string& spec, const IDMCPCallPtr& call)` — errors the call and returns `nullptr` on failure
  - `void idmcp_register_gesture_tools(IDMCPToolRegistry&)` (public, declared in `idmcptools.h`)

- [ ] **Step 1: Create the file with its header comment and includes**

Create `indra/newview/idmcptools_gesture.cpp`:

```cpp
/**
 * @file idmcptools_gesture.cpp
 * @brief <ID> MCP server: gesture authoring, activation and playback tools.
 *
 * Part of Five's custom Firestorm fork.
 *
 * Gestures are the one asset type here with a modern capability path: serialize
 * an LLMultiGesture into an ASCII LLDataPacker buffer and hand it to
 * UpdateGestureAgentInventory, exactly as notecard.write does. Reads go through
 * gAssetStorage (or straight off LLGestureMgr when the gesture is already
 * active).
 *
 * Writes are gated on @edit — a fork extension of that restriction to inventory
 * content, see doc/rlva-custom-commands.md. gesture.play declares no gate
 * because LLGestureMgr::playGesture already enforces @sendgesture internally
 * (llgesturemgr.cpp:557); adding a second check here would be a path that can
 * drift out of sync with the first.
 */

#include "llviewerprecompiledheaders.h"

#include "idmcptools.h"
#include "idmcpserver.h"
#include "idmcprlvgate.h"

#include "llagent.h"
#include "llassetstorage.h"
#include "lldatapacker.h"
#include "llfilesystem.h"
#include "llfloaterperms.h"
#include "llgesturemgr.h"
#include "llinventoryfunctions.h"
#include "llinventorymodel.h"
#include "llmultigesture.h"
#include "llviewerassetupload.h"
#include "llviewerinventory.h"
#include "llviewerregion.h"

#include "rlvdefines.h"

#include "lluuid.h"

#include <functional>
#include <memory>
#include <vector>
```

- [ ] **Step 2: Add the file-local helpers**

Append to the same file. These mirror the identical helpers in `idmcptools_appearance.cpp:34-72`.

```cpp
namespace
{
    bool looks_like_uuid(const std::string& s)
    {
        return s.size() == 36 && s[8] == '-' && s[13] == '-' && s[18] == '-' && s[23] == '-';
    }

    std::string arg_str(const boost::json::object& args, const char* key)
    {
        auto it = args.find(key);
        return (it != args.end() && it->value().is_string())
                   ? std::string(it->value().as_string().c_str()) : std::string();
    }

    bool arg_bool(const boost::json::object& args, const char* key, bool def)
    {
        auto it = args.find(key);
        return (it != args.end() && it->value().is_bool()) ? it->value().as_bool() : def;
    }

    IDMCPGateResult gate_showinv(const boost::json::object&, IDMCPGatePhase)
    {
        return IDMCPRlvGate::checkBehaviour(RLV_BHVR_SHOWINV, "showinv");
    }

    // Fork extension: @edit covers inventory-content edits, not just in-world
    // object editing. Documented in doc/rlva-custom-commands.md.
    IDMCPGateResult gate_edit(const boost::json::object&, IDMCPGatePhase)
    {
        return IDMCPRlvGate::checkBehaviour(RLV_BHVR_EDIT, "edit");
    }

    // Resolve an item_id argument to a gesture item. Errors the call and returns
    // nullptr if the argument is malformed, unknown, or not a gesture.
    LLViewerInventoryItem* resolve_gesture_item(const std::string& spec, const IDMCPCallPtr& call)
    {
        if (!looks_like_uuid(spec))
        {
            idmcp_tool_err(call, IDMCP_ERR_INVALID_PARAMS, "item_id must be a UUID");
            return nullptr;
        }
        LLViewerInventoryItem* item = gInventory.getItem(LLUUID(spec));
        if (!item || item->getType() != LLAssetType::AT_GESTURE)
        {
            idmcp_tool_err(call, IDMCP_ERR_NOT_FOUND, "no such gesture");
            return nullptr;
        }
        return item;
    }
}
```

- [ ] **Step 3: Add the registration function with `gesture.list`**

Append after the anonymous namespace:

```cpp
// ---------------------------------------------------------------------------

void idmcp_register_gesture_tools(IDMCPToolRegistry& reg)
{
    // gesture.list -----------------------------------------------------------
    {
        IDMCPTool t;
        t.name = "gesture.list";
        t.description =
            "List the currently active gestures — the ones whose trigger words are "
            "live. Returns {gestures:[{item_id, name, trigger, playing}]}. Inactive "
            "gestures are ordinary inventory items; find them with inventory.search. "
            "Blocked by RLV @showinv.";
        t.input_schema = boost::json::parse(
            R"({"type":"object","properties":{},"additionalProperties":false})");
        t.gate = gate_showinv;
        t.invoke = [](const boost::json::object&, const IDMCPCallPtr& call)
        {
            boost::json::array out;
            for (const auto& [item_id, gesture] : LLGestureMgr::instance().getActiveGestures())
            {
                boost::json::object o;
                o["item_id"] = item_id.asString();
                // The map holds a null pointer while an activation is still
                // loading its asset. Report it as present but not yet readable.
                if (gesture)
                {
                    o["name"]    = gesture->mName;
                    o["trigger"] = gesture->mTrigger;
                    o["playing"] = gesture->mPlaying;
                }
                else
                {
                    if (LLViewerInventoryItem* item = gInventory.getItem(item_id))
                    {
                        o["name"] = item->getName();
                    }
                    o["loading"] = true;
                }
                out.push_back(std::move(o));
            }
            boost::json::object res;
            res["gestures"] = std::move(out);
            idmcp_tool_ok(call, res);
        };
        reg.add(std::move(t));
    }
}
```

- [ ] **Step 4: Add `gesture.activate` and `gesture.deactivate`**

Insert inside `idmcp_register_gesture_tools`, before its closing brace:

```cpp
    // gesture.activate -------------------------------------------------------
    {
        IDMCPTool t;
        t.name = "gesture.activate";
        t.description =
            "Activate a gesture by {\"item_id\"} so its trigger word and key binding "
            "go live. Returns {accepted, active}.";
        t.input_schema = boost::json::parse(
            R"({"type":"object","properties":{"item_id":{"type":"string"}},"required":["item_id"],"additionalProperties":false})");
        t.invoke = [](const boost::json::object& args, const IDMCPCallPtr& call)
        {
            LLViewerInventoryItem* item = resolve_gesture_item(arg_str(args, "item_id"), call);
            if (!item) return;
            LLGestureMgr::instance().activateGesture(item->getUUID());
            boost::json::object o;
            o["accepted"] = true;
            o["active"]   = LLGestureMgr::instance().isGestureActive(item->getUUID());
            idmcp_tool_ok(call, o);
        };
        reg.add(std::move(t));
    }

    // gesture.deactivate -----------------------------------------------------
    {
        IDMCPTool t;
        t.name = "gesture.deactivate";
        t.description =
            "Deactivate a gesture by {\"item_id\"} so its trigger word stops firing. "
            "Returns {accepted, active}.";
        t.input_schema = boost::json::parse(
            R"({"type":"object","properties":{"item_id":{"type":"string"}},"required":["item_id"],"additionalProperties":false})");
        t.invoke = [](const boost::json::object& args, const IDMCPCallPtr& call)
        {
            LLViewerInventoryItem* item = resolve_gesture_item(arg_str(args, "item_id"), call);
            if (!item) return;
            LLGestureMgr::instance().deactivateGesture(item->getUUID());
            boost::json::object o;
            o["accepted"] = true;
            o["active"]   = LLGestureMgr::instance().isGestureActive(item->getUUID());
            idmcp_tool_ok(call, o);
        };
        reg.add(std::move(t));
    }
```

`activateGesture` is asynchronous (it fetches the asset), so `active` may read false immediately after the call. The description does not promise otherwise, and `gesture.list` is the way to confirm.

- [ ] **Step 5: Add `gesture.play`**

Insert inside `idmcp_register_gesture_tools`, before its closing brace:

```cpp
    // gesture.play -----------------------------------------------------------
    {
        IDMCPTool t;
        t.name = "gesture.play";
        t.description =
            "Play an active gesture now, by {\"item_id\"}. The gesture must already "
            "be active (gesture.activate). Animation and chat steps run through the "
            "viewer's gesture manager, so RLV @sendgesture and chat restrictions "
            "apply as they would for a hand-triggered gesture.";
        t.input_schema = boost::json::parse(
            R"({"type":"object","properties":{"item_id":{"type":"string"}},"required":["item_id"],"additionalProperties":false})");
        // No gate: LLGestureMgr::playGesture enforces @sendgesture internally
        // (llgesturemgr.cpp:557 / RlvActions::canPlayGestures).
        t.invoke = [](const boost::json::object& args, const IDMCPCallPtr& call)
        {
            LLViewerInventoryItem* item = resolve_gesture_item(arg_str(args, "item_id"), call);
            if (!item) return;
            if (!LLGestureMgr::instance().isGestureActive(item->getUUID()))
            {
                idmcp_tool_err(call, IDMCP_ERR_INVALID_PARAMS,
                               "gesture is not active; call gesture.activate first");
                return;
            }
            LLGestureMgr::instance().playGesture(item->getUUID());
            boost::json::object o;
            o["accepted"] = true;
            idmcp_tool_ok(call, o);
        };
        reg.add(std::move(t));
    }
```

- [ ] **Step 6: Declare the registration function**

In `indra/newview/idmcptools.h`, add after the `idmcp_register_appearance_tools` line:

```cpp
void idmcp_register_gesture_tools(IDMCPToolRegistry& reg);
```

- [ ] **Step 7: Call it at server startup**

In `indra/newview/idmcpserver.cpp`, in `initSingleton`, add after the `idmcp_register_appearance_tools(mRegistry);` line at :157:

```cpp
    idmcp_register_gesture_tools(mRegistry);
```

- [ ] **Step 8: Add the file to the build**

In `indra/newview/CMakeLists.txt`, after the `idmcptools_appearance.cpp` line at :97, add:

```cmake
    idmcptools_gesture.cpp  # <ID> Embedded MCP server: gesture authoring tools
```

- [ ] **Step 9: Compile checklist (read, do not build)**

Confirm by reading:
- `LLGestureMgr` is an `LLSingleton`, so `LLGestureMgr::instance()` is correct — check `indra/newview/llgesturemgr.h`.
- `getActiveGestures()` returns `const item_map_t&` = `std::map<LLUUID, LLMultiGesture*>` — `llgesturemgr.h:59,102`.
- `mName`, `mTrigger`, `mPlaying` are public members of `LLMultiGesture` — `llmultigesture.h`.
- `activateGesture`, `deactivateGesture`, `isGestureActive`, `playGesture(const LLUUID&)` all exist — `llgesturemgr.h:71,90,96,109`.
- `RLV_BHVR_EDIT` and `RLV_BHVR_SHOWINV` are in scope via `rlvdefines.h`.
- `gate_edit` is defined but not yet referenced by any tool in this task. That is intentional — Task 3 uses it. If the compiler warns about an unused static function, leave it; the warning disappears in Task 3.

- [ ] **Step 10: Commit**

```bash
git add indra/newview/idmcptools_gesture.cpp indra/newview/idmcptools.h \
        indra/newview/idmcpserver.cpp indra/newview/CMakeLists.txt
git commit -m "feat: idmcp gesture.list/activate/deactivate/play"
```

---

### Task 2: `gesture.read`

Adds the asset loader and the `LLMultiGesture` → JSON translation. Reviewable on its own: after this task an agent can read any gesture's steps.

**Files:**
- Modify: `indra/newview/idmcptools_gesture.cpp` — add loader + serialiser to the anonymous namespace, register one tool

**Interfaces:**
- Consumes: `looks_like_uuid`, `arg_str`, `gate_showinv`, `resolve_gesture_item` from Task 1.
- Produces (Task 3 depends on both by exact name):
  - `boost::json::object gesture_to_json(LLMultiGesture* g)`
  - `void idmcp_gesture_load(const LLUUID& item_id, const IDMCPCallPtr& call, const std::function<void(LLMultiGesture*)>& then)` — invokes `then` with a gesture the callee **must not retain or delete**; on failure it errors `call` and never invokes `then`.

Task 3 additionally defines `bool idmcp_commit_denied(const IDMCPCallPtr&, ERlvBehaviour, const char*)` for its own use.

- [ ] **Step 1: Add the step serialiser**

Inside the anonymous namespace in `idmcptools_gesture.cpp`, after `resolve_gesture_item`:

```cpp
    boost::json::value step_to_json(LLGestureStep* step)
    {
        boost::json::object o;
        switch (step->getType())
        {
        case STEP_ANIMATION:
        {
            LLGestureStepAnimation* s = static_cast<LLGestureStepAnimation*>(step);
            o["type"]      = "animation";
            o["animation"] = s->mAnimName;
            o["asset_id"]  = s->mAnimAssetID.asString();
            o["action"]    = (s->mFlags & ANIM_FLAG_STOP) ? "stop" : "start";
            break;
        }
        case STEP_SOUND:
        {
            LLGestureStepSound* s = static_cast<LLGestureStepSound*>(step);
            o["type"]     = "sound";
            o["sound"]    = s->mSoundName;
            o["asset_id"] = s->mSoundAssetID.asString();
            break;
        }
        case STEP_CHAT:
        {
            LLGestureStepChat* s = static_cast<LLGestureStepChat*>(step);
            o["type"] = "chat";
            o["text"] = s->mChatText;
            break;
        }
        case STEP_WAIT:
        {
            LLGestureStepWait* s = static_cast<LLGestureStepWait*>(step);
            o["type"]            = "wait";
            o["seconds"]         = (s->mFlags & WAIT_FLAG_TIME) ? (double)s->mWaitSeconds : 0.0;
            o["for_animations"]  = (s->mFlags & WAIT_FLAG_ALL_ANIM) != 0;
            o["for_key_release"] = (s->mFlags & WAIT_FLAG_KEY_RELEASE) != 0;
            break;
        }
        default:
            o["type"] = "unknown";
            break;
        }
        return o;
    }

    boost::json::object gesture_to_json(LLMultiGesture* g)
    {
        boost::json::object o;
        o["trigger"]      = g->mTrigger;
        o["replace_with"] = g->mReplaceText;
        o["key"]          = (int)g->mKey;
        o["mask"]         = (int)g->mMask;
        boost::json::array steps;
        for (LLGestureStep* s : g->mSteps)
        {
            steps.push_back(step_to_json(s));
        }
        o["steps"] = std::move(steps);
        return o;
    }
```

- [ ] **Step 2: Add the asset-load context and callback**

Still inside the anonymous namespace:

```cpp
    struct IDMCPGestureLoad
    {
        IDMCPCallPtr                          call;
        LLUUID                                item_id;
        std::function<void(LLMultiGesture*)>  then;
    };

    void idmcp_gesture_loaded(const LLUUID& asset_id, LLAssetType::EType type,
                              void* user_data, S32 status, LLExtStat)
    {
        std::unique_ptr<IDMCPGestureLoad> ctx(static_cast<IDMCPGestureLoad*>(user_data));
        if (!ctx) return;

        if (status != 0)
        {
            idmcp_tool_err(ctx->call, IDMCP_ERR_NOT_FOUND, "gesture asset could not be fetched");
            return;
        }

        LLFileSystem file(asset_id, type, LLFileSystem::READ);
        S32 size = file.getSize();
        if (size <= 0)
        {
            idmcp_tool_err(ctx->call, IDMCP_ERR_NOT_FOUND, "gesture asset is empty");
            return;
        }
        std::vector<char> buffer(size + 1);
        file.read((U8*)&buffer[0], size);
        buffer[size] = '\0';

        // Owned here; ctx->then must not retain it.
        std::unique_ptr<LLMultiGesture> gesture(new LLMultiGesture());
        LLDataPackerAsciiBuffer dp(&buffer[0], size + 1);
        if (!gesture->deserialize(dp))
        {
            idmcp_tool_err(ctx->call, IDMCP_ERR_NOT_FOUND, "gesture asset is malformed");
            return;
        }
        if (LLViewerInventoryItem* item = gInventory.getItem(ctx->item_id))
        {
            gesture->mName = item->getName();
        }
        ctx->then(gesture.get());
    }
```

- [ ] **Step 3: Add the loader**

Still inside the anonymous namespace:

```cpp
    // Hand `then` the gesture behind `item_id`, fetching the asset if needed.
    // The gesture is owned by the loader (or by LLGestureMgr) — `then` must
    // neither retain nor delete it. On failure this errors `call` and does not
    // invoke `then`.
    void idmcp_gesture_load(const LLUUID& item_id, const IDMCPCallPtr& call,
                            const std::function<void(LLMultiGesture*)>& then)
    {
        LLViewerInventoryItem* item = gInventory.getItem(item_id);
        if (!item)
        {
            idmcp_tool_err(call, IDMCP_ERR_NOT_FOUND, "no such gesture");
            return;
        }

        // Already active: the manager holds a live copy. A null entry means the
        // activation is still loading, so fall through to the fetch.
        const LLGestureMgr::item_map_t& active = LLGestureMgr::instance().getActiveGestures();
        auto it = active.find(item_id);
        if (it != active.end() && it->second)
        {
            then(it->second);
            return;
        }

        // A gesture item created by gesture.create has no asset yet. That is an
        // empty gesture, not an error.
        if (item->getAssetUUID().isNull())
        {
            LLMultiGesture empty;
            empty.mName = item->getName();
            then(&empty);
            return;
        }

        IDMCPGestureLoad* ctx = new IDMCPGestureLoad{ call, item_id, then };
        gAssetStorage->getAssetData(item->getAssetUUID(), LLAssetType::AT_GESTURE,
                                    idmcp_gesture_loaded, (void**)ctx, /*high_priority*/ true);
    }
```

- [ ] **Step 4: Register `gesture.read`**

Insert inside `idmcp_register_gesture_tools`, before its closing brace:

```cpp
    // gesture.read -----------------------------------------------------------
    {
        IDMCPTool t;
        t.name = "gesture.read";
        t.description =
            "Read a gesture by {\"item_id\"} (UUID). Returns {name, trigger, "
            "replace_with, key, mask, active, steps[]}. Each step is one of: "
            "{type:\"animation\", animation, asset_id, action:\"start\"|\"stop\"}, "
            "{type:\"sound\", sound, asset_id}, {type:\"chat\", text}, "
            "{type:\"wait\", seconds, for_animations, for_key_release}. A gesture "
            "with no asset yet reads as an empty step list. Blocked by RLV @showinv.";
        t.input_schema = boost::json::parse(
            R"({"type":"object","properties":{"item_id":{"type":"string"}},"required":["item_id"],"additionalProperties":false})");
        t.gate = gate_showinv;
        t.invoke = [](const boost::json::object& args, const IDMCPCallPtr& call)
        {
            LLViewerInventoryItem* item = resolve_gesture_item(arg_str(args, "item_id"), call);
            if (!item) return;
            const LLUUID item_id = item->getUUID();
            idmcp_gesture_load(item_id, call, [call, item_id](LLMultiGesture* g)
            {
                boost::json::object o = gesture_to_json(g);
                o["name"]   = g->mName;
                o["active"] = LLGestureMgr::instance().isGestureActive(item_id);
                idmcp_tool_ok(call, o);
            });
        };
        reg.add(std::move(t));
    }
```

- [ ] **Step 5: Compile checklist (read, do not build)**

Confirm by reading:
- `LLGestureStep::getType()` is non-const virtual, so `step_to_json` takes a non-const `LLGestureStep*` and `mSteps` (a `std::vector<LLGestureStep*>`) supplies one — `llmultigesture.h:137`.
- `STEP_ANIMATION`/`STEP_SOUND`/`STEP_CHAT`/`STEP_WAIT`, `ANIM_FLAG_STOP`, `WAIT_FLAG_TIME`, `WAIT_FLAG_ALL_ANIM`, `WAIT_FLAG_KEY_RELEASE` are all free names in `llmultigesture.h`.
- `~LLMultiGesture` deletes its steps (`llmultigesture.cpp`), so `std::unique_ptr<LLMultiGesture>` leaks nothing.
- `gAssetStorage->getAssetData(id, type, cb, (void**)ctx, high_priority)` matches the call at `llpreviewgesture.cpp:859` — note the `(void**)` cast, which is what that signature takes even though the callback receives it as `void*`.
- The `LLFileSystem` read shape matches `llpreviewgesture.cpp:880-890`.
- `LLGestureMgr::item_map_t` is public — `llgesturemgr.h:59`.

- [ ] **Step 6: Commit**

```bash
git add indra/newview/idmcptools_gesture.cpp
git commit -m "feat: idmcp gesture.read"
```

---

### Task 3: `gesture.create` and `gesture.write`

Adds JSON → `LLMultiGesture` parsing, inventory name resolution for animations and sounds, and the capability upload. Reviewable on its own: after this task the gesture half is complete.

**Files:**
- Modify: `indra/newview/idmcptools_gesture.cpp` — add parsers + upload to the anonymous namespace, register two tools

**Interfaces:**
- Consumes: `looks_like_uuid`, `arg_str`, `arg_bool`, `gate_edit`, `resolve_gesture_item` (Task 1); `gesture_to_json`, `idmcp_gesture_load` (Task 2).
- Produces: nothing consumed by later tasks.

- [ ] **Step 1: Add the local-item-asset helper**

`idmcp_set_item_asset` is file-local to `idmcptools_inventory.cpp:323` and is not exported. Add an equivalent inside this file's anonymous namespace, with a comment saying why the duplicate exists:

```cpp
    // Point the local inventory item at the freshly-uploaded asset so a read
    // right after a write sees the new content. Deliberately duplicated from
    // idmcptools_inventory.cpp:323 — that copy is file-local, and this fork
    // keeps these per-area helpers local rather than hoisting a shared header.
    void idmcp_set_item_asset(const LLUUID& item_id, const LLUUID& new_asset_id)
    {
        if (new_asset_id.isNull()) return;
        LLViewerInventoryItem* item = gInventory.getItem(item_id);
        if (!item) return;
        LLPointer<LLViewerInventoryItem> updated = new LLViewerInventoryItem(item);
        updated->setAssetUUID(new_asset_id);
        gInventory.updateItem(updated);
        gInventory.notifyObservers();
    }

    // Commit-phase re-check for deferred tools: a restriction may have arrived
    // between the request and the completion. Same shape as idmcptools_manage.cpp:284.
    bool idmcp_commit_denied(const IDMCPCallPtr& call, ERlvBehaviour bhvr, const char* label)
    {
        if (!IDMCPRlvGate::isEnabled()) return false;
        IDMCPGateResult g = IDMCPRlvGate::checkBehaviour(bhvr, label);
        if (g.allowed) return false;
        boost::json::object data;
        data["restriction"] = label;
        data["checkedAt"]   = "commit";
        idmcp_tool_err(call, IDMCP_ERR_RLV_RESTRICTED,
                       std::string("blocked by RLV restriction @") + label, std::move(data));
        return true;
    }
```

- [ ] **Step 2: Add inventory asset resolution by name or id**

Inside the anonymous namespace:

```cpp
    // Resolve a step's animation/sound reference. Accepts an inventory item
    // UUID, a bare asset UUID, or an inventory item name. Returns false and
    // fills `err` on failure — including ambiguity, which lists the candidates
    // rather than silently picking one.
    bool resolve_step_asset(const std::string& spec, LLAssetType::EType at,
                            LLUUID& asset_id_out, std::string& name_out, std::string& err)
    {
        if (spec.empty())
        {
            err = "step is missing its animation/sound reference";
            return false;
        }
        if (looks_like_uuid(spec))
        {
            const LLUUID id(spec);
            if (LLViewerInventoryItem* item = gInventory.getItem(id))
            {
                if (item->getType() != at)
                {
                    err = "'" + spec + "' is not a " + std::string(LLAssetType::lookup(at));
                    return false;
                }
                asset_id_out = item->getAssetUUID();
                name_out     = item->getName();
                return true;
            }
            // Not an inventory item — treat it as a bare asset id.
            asset_id_out = id;
            name_out.clear();
            return true;
        }

        LLInventoryModel::cat_array_t  cats;
        LLInventoryModel::item_array_t items;
        LLIsType is_type(at);
        gInventory.collectDescendentsIf(gInventory.getRootFolderID(), cats, items,
                                        /*include_trash*/ false, is_type);

        std::vector<LLViewerInventoryItem*> hits;
        for (auto& i : items)
        {
            if (i->getName() == spec) hits.push_back(i.get());
        }
        if (hits.empty())
        {
            err = "no " + std::string(LLAssetType::lookup(at)) + " in inventory named '" + spec + "'";
            return false;
        }
        if (hits.size() > 1)
        {
            err = "'" + spec + "' is ambiguous (" + std::to_string(hits.size()) + " matches):";
            for (LLViewerInventoryItem* h : hits) err += " " + h->getUUID().asString();
            err += " — pass one of these UUIDs instead";
            return false;
        }
        asset_id_out = hits[0]->getAssetUUID();
        name_out     = hits[0]->getName();
        return true;
    }
```

- [ ] **Step 3: Add the JSON → steps parser**

Inside the anonymous namespace:

```cpp
    // Parse a "steps" array into freshly allocated LLGestureStep objects. On
    // failure fills `err`, deletes anything already built, and returns false —
    // so a partly-bad array never half-writes a gesture.
    bool parse_steps(const boost::json::array& arr, std::vector<LLGestureStep*>& out, std::string& err)
    {
        for (const auto& v : arr)
        {
            if (!v.is_object())
            {
                err = "each step must be an object";
                break;
            }
            const boost::json::object& s = v.as_object();
            const std::string type = arg_str(s, "type");

            if (type == "animation")
            {
                const std::string ref         = arg_str(s, "animation");
                const std::string explicit_id = arg_str(s, "asset_id");
                LLUUID      asset;
                std::string name = ref;
                if (looks_like_uuid(explicit_id))
                {
                    // Round-trip from gesture.read: trust the stored asset id and
                    // keep the stored name. Without this, editing only a gesture's
                    // trigger would fail whenever an unrelated step's animation had
                    // since been renamed, deleted, or duplicated in inventory.
                    asset = LLUUID(explicit_id);
                }
                else if (!resolve_step_asset(ref, LLAssetType::AT_ANIMATION, asset, name, err))
                {
                    break;
                }
                const std::string action = arg_str(s, "action");
                if (!action.empty() && action != "start" && action != "stop")
                {
                    err = "animation step action must be \"start\" or \"stop\"";
                    break;
                }
                LLGestureStepAnimation* step = new LLGestureStepAnimation();
                step->mAnimName    = name.empty() ? asset.asString() : name;
                step->mAnimAssetID = asset;
                step->mFlags       = (action == "stop") ? ANIM_FLAG_STOP : 0;
                out.push_back(step);
            }
            else if (type == "sound")
            {
                const std::string ref         = arg_str(s, "sound");
                const std::string explicit_id = arg_str(s, "asset_id");
                LLUUID      asset;
                std::string name = ref;
                if (looks_like_uuid(explicit_id))
                {
                    // Same round-trip guarantee as the animation branch above.
                    asset = LLUUID(explicit_id);
                }
                else if (!resolve_step_asset(ref, LLAssetType::AT_SOUND, asset, name, err))
                {
                    break;
                }
                LLGestureStepSound* step = new LLGestureStepSound();
                step->mSoundName    = name.empty() ? asset.asString() : name;
                step->mSoundAssetID = asset;
                step->mFlags        = 0;
                out.push_back(step);
            }
            else if (type == "chat")
            {
                LLGestureStepChat* step = new LLGestureStepChat();
                step->mChatText = arg_str(s, "text");
                step->mFlags    = 0;
                out.push_back(step);
            }
            else if (type == "wait")
            {
                double seconds = 0.0;
                auto sit = s.find("seconds");
                if (sit != s.end() && sit->value().is_number())
                {
                    seconds = sit->value().to_number<double>();
                }
                const bool for_anims = arg_bool(s, "for_animations", false);
                const bool for_key   = arg_bool(s, "for_key_release", false);
                if (seconds <= 0.0 && !for_anims && !for_key)
                {
                    err = "wait step needs seconds > 0, for_animations, or for_key_release";
                    break;
                }
                LLGestureStepWait* step = new LLGestureStepWait();
                step->mWaitSeconds = (F32)seconds;
                step->mFlags       = (seconds > 0.0 ? WAIT_FLAG_TIME : 0)
                                   | (for_anims    ? WAIT_FLAG_ALL_ANIM : 0)
                                   | (for_key      ? WAIT_FLAG_KEY_RELEASE : 0);
                out.push_back(step);
            }
            else
            {
                err = "unknown step type '" + type + "' (animation, sound, chat, wait)";
                break;
            }
        }

        if (!err.empty())
        {
            for (LLGestureStep* s : out) delete s;
            out.clear();
            return false;
        }
        return true;
    }
```

- [ ] **Step 4: Add the serialise-and-upload helper**

Inside the anonymous namespace:

```cpp
    // Serialize `g` and push it to UpdateGestureAgentInventory. Takes ownership
    // of nothing; `g` must outlive only this call.
    void idmcp_gesture_upload(LLMultiGesture* g, const LLUUID& item_id, const IDMCPCallPtr& call)
    {
        LLViewerRegion* region = gAgent.getRegion();
        const std::string url = region ? region->getCapability("UpdateGestureAgentInventory") : std::string();
        if (url.empty())
        {
            idmcp_tool_err(call, IDMCP_ERR_CAP_UNAVAIL,
                           "UpdateGestureAgentInventory capability unavailable");
            return;
        }

        const S32 max_size = g->getMaxSerialSize();
        std::vector<char> buffer(max_size);
        LLDataPackerAsciiBuffer dp(&buffer[0], max_size);
        if (!g->serialize(dp))
        {
            idmcp_tool_err(call, IDMCP_ERR_INVALID_PARAMS, "gesture is too large to serialize");
            return;
        }
        const std::string payload(&buffer[0]);

        const bool was_active = LLGestureMgr::instance().isGestureActive(item_id);

        LLResourceUploadInfo::ptr_t info = std::make_shared<LLBufferedAssetUploadInfo>(
            item_id, LLAssetType::AT_GESTURE, payload,
            [call, was_active](LLUUID itemId, LLUUID newAssetId, LLUUID, LLSD)
            {
                if (idmcp_commit_denied(call, RLV_BHVR_EDIT, "edit")) return;
                idmcp_set_item_asset(itemId, newAssetId);
                // Keep the live copy in step with what was just saved.
                if (was_active)
                {
                    LLGestureMgr::instance().activateGestureWithAsset(
                        itemId, newAssetId, /*inform_server*/ true, /*deactivate_similar*/ false);
                }
                boost::json::object o;
                o["accepted"]     = true;
                o["new_asset_id"] = newAssetId.asString();
                idmcp_tool_ok(call, o);
            },
            [call](LLUUID, LLUUID, LLSD, std::string reason) -> bool
            {
                idmcp_tool_err(call, IDMCP_ERR_PERMISSION, "gesture upload failed: " + reason);
                return false;
            });
        LLViewerAssetUpload::EnqueueInventoryUpload(url, info);
    }
```

The upload payload is taken as `std::string(&buffer[0])` because `LLDataPackerAsciiBuffer` writes a NUL-terminated ASCII blob into the buffer — the same assumption `llpreviewgesture.cpp:1143` makes when it passes the raw `char*` to `LLBufferedAssetUploadInfo`.

- [ ] **Step 5: Register `gesture.create`**

Insert inside `idmcp_register_gesture_tools`, before its closing brace:

```cpp
    // gesture.create ---------------------------------------------------------
    {
        IDMCPTool t;
        t.name = "gesture.create";
        t.description =
            "Create a new empty gesture. {\"parent\"} (folder UUID) and {\"name\"}. "
            "Returns the new item id; add trigger and steps with gesture.write, then "
            "gesture.activate to make it live. Blocked by RLV @edit.";
        t.input_schema = boost::json::parse(
            R"({"type":"object","properties":{"parent":{"type":"string"},"name":{"type":"string"}},"required":["parent","name"],"additionalProperties":false})");
        t.gate = gate_edit;
        t.invoke = [](const boost::json::object& args, const IDMCPCallPtr& call)
        {
            const std::string parent_spec = arg_str(args, "parent");
            const std::string name        = arg_str(args, "name");
            if (!looks_like_uuid(parent_spec) || name.empty())
            {
                idmcp_tool_err(call, IDMCP_ERR_INVALID_PARAMS,
                               "parent must be a folder UUID and name must be non-empty");
                return;
            }
            const LLUUID parent(parent_spec);
            if (!gInventory.getCategory(parent))
            {
                idmcp_tool_err(call, IDMCP_ERR_NOT_FOUND, "no such folder");
                return;
            }
            create_inventory_item(
                gAgentID, gAgentSessionID, parent, LLTransactionID::tnull,
                name, name, LLAssetType::AT_GESTURE, LLInventoryType::IT_GESTURE,
                NO_INV_SUBTYPE, LLFloaterPerms::getNextOwnerPerms("Gestures"),
                new LLBoostFuncInventoryCallback([call](const LLUUID& new_id)
                {
                    if (idmcp_commit_denied(call, RLV_BHVR_EDIT, "edit")) return;
                    boost::json::object o;
                    o["accepted"] = true;
                    o["item_id"]  = new_id.asString();
                    idmcp_tool_ok(call, o);
                }));
        };
        reg.add(std::move(t));
    }
```

- [ ] **Step 6: Register `gesture.write`**

Insert inside `idmcp_register_gesture_tools`, before its closing brace:

```cpp
    // gesture.write ----------------------------------------------------------
    {
        IDMCPTool t;
        t.name = "gesture.write";
        t.description =
            "Update a gesture. {\"item_id\"} plus any of {\"trigger\"}, "
            "{\"replace_with\"}, {\"key\"}, {\"mask\"}, {\"steps\"}. Omitted fields "
            "keep their current value; {\"steps\"} replaces the whole list. Step "
            "animation/sound accept an inventory item name or a UUID; if a step also "
            "carries the {\"asset_id\"} that gesture.read returned, that wins, so "
            "read-modify-write round-trips exactly. Rename the gesture itself with "
            "inventory.rename. Blocked by RLV @edit.";
        t.input_schema = boost::json::parse(
            R"({"type":"object","properties":{"item_id":{"type":"string"},"trigger":{"type":"string"},"replace_with":{"type":"string"},"key":{"type":"integer"},"mask":{"type":"integer"},"steps":{"type":"array","items":{"type":"object"}}},"required":["item_id"],"additionalProperties":false})");
        t.gate = gate_edit;
        t.invoke = [](const boost::json::object& args, const IDMCPCallPtr& call)
        {
            LLViewerInventoryItem* item = resolve_gesture_item(arg_str(args, "item_id"), call);
            if (!item) return;
            const LLUUID item_id = item->getUUID();

            // Copy what we need out of `args` — the load may complete later.
            boost::json::object a = args;
            idmcp_gesture_load(item_id, call, [call, item_id, a](LLMultiGesture* current)
            {
                // Build the merged gesture. `current` is not ours to mutate:
                // when the gesture is active it is LLGestureMgr's live copy.
                LLMultiGesture merged;
                merged.mName        = current->mName;
                merged.mTrigger     = current->mTrigger;
                merged.mReplaceText = current->mReplaceText;
                merged.mKey         = current->mKey;
                merged.mMask        = current->mMask;

                if (a.contains("trigger"))      merged.mTrigger     = arg_str(a, "trigger");
                if (a.contains("replace_with")) merged.mReplaceText = arg_str(a, "replace_with");
                if (a.contains("key") && a.at("key").is_int64())
                {
                    merged.mKey = (KEY)a.at("key").as_int64();
                }
                if (a.contains("mask") && a.at("mask").is_int64())
                {
                    merged.mMask = (MASK)a.at("mask").as_int64();
                }

                if (a.contains("steps") && a.at("steps").is_array())
                {
                    std::string err;
                    if (!parse_steps(a.at("steps").as_array(), merged.mSteps, err))
                    {
                        idmcp_tool_err(call, IDMCP_ERR_INVALID_PARAMS, err);
                        return;
                    }
                }
                else
                {
                    // Keep the existing steps by re-parsing our own JSON form of
                    // them, so `merged` owns its copies and `current` is untouched.
                    boost::json::object cur = gesture_to_json(current);
                    std::string err;
                    if (!parse_steps(cur.at("steps").as_array(), merged.mSteps, err))
                    {
                        idmcp_tool_err(call, IDMCP_ERR_INVALID_PARAMS,
                                       "existing gesture steps could not be preserved: " + err);
                        return;
                    }
                }

                idmcp_gesture_upload(&merged, item_id, call);
            });
        };
        reg.add(std::move(t));
    }
```

Re-parsing the existing steps through the JSON form (rather than deep-copying `LLGestureStep` objects) keeps one code path for step construction and avoids adding a clone method to a `llcharacter` class this fork does not otherwise touch. `merged` owns its steps and its destructor frees them.

This re-parse is why `parse_steps` must honour `asset_id` over the name: `gesture_to_json` emits both, and a step whose animation has since been renamed or deleted would otherwise fail name resolution and block an edit to an unrelated field. The spec's promise that a read/write round-trip is lossless depends on this precedence.

- [ ] **Step 7: Compile checklist (read, do not build)**

Confirm by reading:
- `create_inventory_item`, `LLBoostFuncInventoryCallback`, `NO_INV_SUBTYPE` come from `llviewerinventory.h`, already included — cross-check against the identical call at `idmcptools_inventory.cpp` in `inventory.createItem`.
- `LLFloaterPerms::getNextOwnerPerms("Gestures")` — `"Gestures"` is a valid prefix, `llfloaterperms.cpp:126`.
- `LLInventoryType::IT_GESTURE` exists — `llinventorytype.h`.
- `LLIsType` is declared at `llinventoryfunctions.h:287` and takes an `LLAssetType::EType`.
- `LLGestureMgr::activateGestureWithAsset(item_id, asset_id, inform_server, deactivate_similar)` — `llgesturemgr.h:87`.
- `KEY` and `MASK` are the viewer's key typedefs, reachable through the precompiled header.
- `boost::json::object::contains` and `at` are available and already used in this tree — `idmcptools_profile.cpp:729-743`.

- [ ] **Step 8: Commit**

```bash
git add indra/newview/idmcptools_gesture.cpp
git commit -m "feat: idmcp gesture.create and gesture.write"
```

---

### Task 4: Wearable file scaffold + `wearable.read`

Establishes the second file, its registration and CMake entry, and the read path for both worn and unworn wearables.

**Files:**
- Create: `indra/newview/idmcptools_wearable.cpp`
- Modify: `indra/newview/idmcptools.h` — add one declaration
- Modify: `indra/newview/idmcpserver.cpp` — add one registration call
- Modify: `indra/newview/CMakeLists.txt` — add one source entry

**Interfaces:**
- Consumes: the same registry/server/gate surface as Task 1.
- Produces (Task 5 depends on all of these by exact name, in the same anonymous namespace):
  - `bool looks_like_uuid(const std::string&)`, `std::string arg_str(...)`, `bool arg_bool(...)`
  - `IDMCPGateResult gate_showinv(...)`, `IDMCPGateResult gate_edit(...)`
  - `std::vector<LLAvatarAppearanceDefines::ETextureIndex> tes_for_type(LLWearableType::EType)`
  - `const char* te_name(LLAvatarAppearanceDefines::ETextureIndex)`
  - `bool te_by_name(LLWearableType::EType, const std::string&, LLAvatarAppearanceDefines::ETextureIndex& out)`
  - `std::string valid_te_names(LLWearableType::EType)`
  - `bool idmcp_commit_denied(const IDMCPCallPtr&, ERlvBehaviour, const char* label)`
  - `boost::json::object wearable_to_json(LLWearable* w)`
  - `void idmcp_register_wearable_tools(IDMCPToolRegistry&)` (public)

- [ ] **Step 1: Create the file with its header comment and includes**

Create `indra/newview/idmcptools_wearable.cpp`:

```cpp
/**
 * @file idmcptools_wearable.cpp
 * @brief <ID> MCP server: wearable authoring tools (clothing and body parts).
 *
 * Part of Five's custom Firestorm fork.
 *
 * Unlike gestures, notecards and scripts, wearables have NO Update* capability
 * (see llviewerregion.cpp:3588-3597) — they still save through the legacy
 * gAssetStorage path, and every viewer entry point for that is keyed on the
 * (type, index) of a *worn* wearable. So wearable.write requires the item to be
 * worn and says so; wearable.read works on anything.
 *
 * Texture slots and tweakable params are enumerated from the appearance
 * dictionary and the wearable's own param list rather than hardcoded, so this
 * file does not have to track LLEditWearableDictionary's 35-entry table.
 *
 * Writes are gated on @edit — a fork extension of that restriction to inventory
 * content, see doc/rlva-custom-commands.md.
 */

#include "llviewerprecompiledheaders.h"

#include "idmcptools.h"
#include "idmcpserver.h"
#include "idmcprlvgate.h"

#include "llagent.h"
#include "llagentwearables.h"
#include "llavatarappearancedefines.h"
#include "llinventorymodel.h"
#include "llvisualparam.h"
#include "llviewerinventory.h"
#include "llviewertexture.h"
#include "llviewertexturelist.h"
#include "llviewerwearable.h"
#include "llvoavatarself.h"
#include "llwearablelist.h"
#include "llwearabletype.h"

#include "rlvcommon.h"     // rlvPredCanWearItem
#include "rlvdefines.h"

#include "lluuid.h"
#include "v4color.h"

#include <memory>
#include <vector>
```

- [ ] **Step 2: Add the file-local helpers**

```cpp
namespace
{
    using namespace LLAvatarAppearanceDefines;

    bool looks_like_uuid(const std::string& s)
    {
        return s.size() == 36 && s[8] == '-' && s[13] == '-' && s[18] == '-' && s[23] == '-';
    }

    std::string arg_str(const boost::json::object& args, const char* key)
    {
        auto it = args.find(key);
        return (it != args.end() && it->value().is_string())
                   ? std::string(it->value().as_string().c_str()) : std::string();
    }

    bool arg_bool(const boost::json::object& args, const char* key, bool def)
    {
        auto it = args.find(key);
        return (it != args.end() && it->value().is_bool()) ? it->value().as_bool() : def;
    }

    IDMCPGateResult gate_showinv(const boost::json::object&, IDMCPGatePhase)
    {
        return IDMCPRlvGate::checkBehaviour(RLV_BHVR_SHOWINV, "showinv");
    }

    // Fork extension: @edit covers inventory-content edits, not just in-world
    // object editing. Documented in doc/rlva-custom-commands.md.
    IDMCPGateResult gate_edit(const boost::json::object&, IDMCPGatePhase)
    {
        return IDMCPRlvGate::checkBehaviour(RLV_BHVR_EDIT, "edit");
    }

    // Commit-phase re-check for deferred tools: a restriction may have arrived
    // between the request and the completion. Same shape as idmcptools_manage.cpp:284.
    // Deliberately duplicated from idmcptools_gesture.cpp — this fork keeps these
    // per-area helpers file-local rather than hoisting a shared header.
    bool idmcp_commit_denied(const IDMCPCallPtr& call, ERlvBehaviour bhvr, const char* label)
    {
        if (!IDMCPRlvGate::isEnabled()) return false;
        IDMCPGateResult g = IDMCPRlvGate::checkBehaviour(bhvr, label);
        if (g.allowed) return false;
        boost::json::object data;
        data["restriction"] = label;
        data["checkedAt"]   = "commit";
        idmcp_tool_err(call, IDMCP_ERR_RLV_RESTRICTED,
                       std::string("blocked by RLV restriction @") + label, std::move(data));
        return true;
    }
}
```

- [ ] **Step 3: Add texture-slot enumeration**

Inside the same anonymous namespace:

```cpp
    const char* te_name(ETextureIndex te)
    {
        const LLAvatarAppearanceDictionary::TextureEntry* e =
            LLAvatarAppearance::getDictionary()->getTexture(te);
        return e ? e->mName.c_str() : "";
    }

    // The local texture slots that belong to `type`, in dictionary order. This
    // reproduces LLEditWearableDictionary::TextureCtrls (llpaneleditwearable.cpp:357)
    // without copying it, and stays correct if a slot is added upstream.
    std::vector<ETextureIndex> tes_for_type(LLWearableType::EType type)
    {
        std::vector<ETextureIndex> out;
        for (const auto& pair : LLAvatarAppearance::getDictionary()->getTextures())
        {
            const LLAvatarAppearanceDictionary::TextureEntry* e = pair.second;
            if (e && e->mIsLocalTexture && e->mWearableType == type)
            {
                out.push_back(pair.first);
            }
        }
        return out;
    }

    // Look up a texture slot on `type` by its dictionary name.
    bool te_by_name(LLWearableType::EType type, const std::string& name, ETextureIndex& out)
    {
        for (ETextureIndex te : tes_for_type(type))
        {
            if (name == te_name(te)) { out = te; return true; }
        }
        return false;
    }

    std::string valid_te_names(LLWearableType::EType type)
    {
        std::string s;
        for (ETextureIndex te : tes_for_type(type))
        {
            if (!s.empty()) s += ", ";
            s += te_name(te);
        }
        return s.empty() ? std::string("(none)") : s;
    }
```

- [ ] **Step 4: Add the wearable serialiser**

Inside the same anonymous namespace:

```cpp
    boost::json::object wearable_to_json(LLWearable* w)
    {
        boost::json::object o;
        o["type"] = LLWearableType::getInstance()->getTypeName(w->getType());
        o["name"] = w->getName();

        boost::json::object textures;
        boost::json::object colors;
        for (ETextureIndex te : tes_for_type(w->getType()))
        {
            const LLLocalTextureObject* lto = w->getLocalTextureObject((S32)te);
            if (lto && lto->getID() != IMG_DEFAULT_AVATAR)
            {
                // llpaneleditwearable.cpp:606 treats IMG_DEFAULT_AVATAR as
                // "no texture set"; report the slot as empty rather than
                // handing back a magic id the agent would try to write back.
                textures[te_name(te)] = lto->getID().asString();
            }
            const LLColor4 c = w->getClothesColor((S32)te);
            boost::json::array rgb;
            rgb.push_back(c.mV[VX]);
            rgb.push_back(c.mV[VY]);
            rgb.push_back(c.mV[VZ]);
            colors[te_name(te)] = std::move(rgb);
        }
        o["textures"] = std::move(textures);
        o["colors"]   = std::move(colors);

        boost::json::array params;
        visual_param_vec_t plist;
        w->getVisualParams(plist);
        for (LLVisualParam* p : plist)
        {
            if (!p || !p->isTweakable()) continue;
            boost::json::object jp;
            jp["id"]      = p->getID();
            jp["name"]    = p->getName();
            jp["label"]   = p->getDisplayName();
            jp["value"]   = w->getVisualParamWeight(p->getID());
            jp["min"]     = p->getMinWeight();
            jp["max"]     = p->getMaxWeight();
            jp["default"] = p->getDefaultWeight();
            params.push_back(std::move(jp));
        }
        o["params"] = std::move(params);
        return o;
    }
```

- [ ] **Step 5: Add the unworn-read context and callback**

Inside the same anonymous namespace:

```cpp
    struct IDMCPWearableRead
    {
        IDMCPCallPtr call;
        LLUUID       item_id;
    };

    void idmcp_wearable_loaded(LLViewerWearable* wearable, void* user_data)
    {
        std::unique_ptr<IDMCPWearableRead> ctx(static_cast<IDMCPWearableRead*>(user_data));
        if (!ctx) return;
        if (idmcp_commit_denied(ctx->call, RLV_BHVR_SHOWINV, "showinv")) return;
        if (!wearable)
        {
            idmcp_tool_err(ctx->call, IDMCP_ERR_NOT_FOUND, "wearable asset could not be fetched");
            return;
        }
        boost::json::object o = wearable_to_json(wearable);
        o["worn"] = false;
        idmcp_tool_ok(ctx->call, o);
    }
```

- [ ] **Step 6: Add the registration function with `wearable.read`**

Append after the anonymous namespace:

```cpp
// ---------------------------------------------------------------------------

void idmcp_register_wearable_tools(IDMCPToolRegistry& reg)
{
    // wearable.read ----------------------------------------------------------
    {
        IDMCPTool t;
        t.name = "wearable.read";
        t.description =
            "Read a wearable (clothing or body part) by {\"item_id\"} (UUID). "
            "Returns {type, name, worn, textures{slot:uuid}, colors{slot:[r,g,b]}, "
            "params[{id, name, label, value, min, max, default}]}. Slot names are "
            "the avatar texture-layer names for that wearable type. Works whether "
            "or not the item is worn. Blocked by RLV @showinv.";
        t.input_schema = boost::json::parse(
            R"({"type":"object","properties":{"item_id":{"type":"string"}},"required":["item_id"],"additionalProperties":false})");
        t.gate = gate_showinv;
        t.invoke = [](const boost::json::object& args, const IDMCPCallPtr& call)
        {
            const std::string spec = arg_str(args, "item_id");
            if (!looks_like_uuid(spec))
            {
                idmcp_tool_err(call, IDMCP_ERR_INVALID_PARAMS, "item_id must be a UUID");
                return;
            }
            const LLUUID item_id(spec);
            LLViewerInventoryItem* item = gInventory.getItem(item_id);
            if (!item ||
                (item->getType() != LLAssetType::AT_CLOTHING &&
                 item->getType() != LLAssetType::AT_BODYPART))
            {
                idmcp_tool_err(call, IDMCP_ERR_NOT_FOUND, "no such wearable");
                return;
            }

            // Worn: the live copy includes any unsaved edits, so prefer it.
            if (LLViewerWearable* worn = gAgentWearables.getWearableFromItemID(item_id))
            {
                boost::json::object o = wearable_to_json(worn);
                o["worn"] = true;
                idmcp_tool_ok(call, o);
                return;
            }

            if (item->getAssetUUID().isNull())
            {
                idmcp_tool_err(call, IDMCP_ERR_NOT_FOUND, "wearable has no asset yet");
                return;
            }
            IDMCPWearableRead* ctx = new IDMCPWearableRead{ call, item_id };
            LLWearableList::instance().getAsset(item->getAssetUUID(), item->getName(),
                                                gAgentAvatarp, item->getType(),
                                                idmcp_wearable_loaded, (void*)ctx);
        };
        reg.add(std::move(t));
    }
}
```

- [ ] **Step 7: Declare, register and build the file**

In `indra/newview/idmcptools.h`, after the `idmcp_register_gesture_tools` line added in Task 1:

```cpp
void idmcp_register_wearable_tools(IDMCPToolRegistry& reg);
```

In `indra/newview/idmcpserver.cpp`, in `initSingleton`, after the `idmcp_register_gesture_tools(mRegistry);` line added in Task 1:

```cpp
    idmcp_register_wearable_tools(mRegistry);
```

In `indra/newview/CMakeLists.txt`, after the `idmcptools_gesture.cpp` line added in Task 1:

```cmake
    idmcptools_wearable.cpp  # <ID> Embedded MCP server: wearable authoring tools
```

- [ ] **Step 8: Compile checklist (read, do not build)**

Confirm by reading:
- `LLAvatarAppearance::getDictionary()` is the accessor — `llavatarappearance.h:272`. There is no `LLAvatarAppearanceDictionary::getInstance()`.
- `LLDictionary` derives from `std::map<Index, Entry*>` (`lldictionary.h:44`), so `for (const auto& pair : ...getTextures())` yields `pair.first` = `ETextureIndex` and `pair.second` = `TextureEntry*`.
- `LLDictionaryEntry::mName` is a `const std::string` — `lldictionary.h:39`.
- `TextureEntry::mIsLocalTexture` and `mWearableType` are public — `llavatarappearancedefines.h:167-171`.
- `LLWearableType` is an `LLParamSingleton`, so `LLWearableType::getInstance()->getTypeName(type)` is correct — `llwearabletype.h:36,69`.
- `LLWearable::getLocalTextureObject(S32)`, `getClothesColor(S32)`, `getVisualParams(visual_param_vec_t&)`, `getVisualParamWeight(S32)` — `llwearable.h:107,123,118,116`.
- `LLVisualParam::isTweakable()`, `getID`, `getName`, `getDisplayName`, `getMinWeight`, `getMaxWeight`, `getDefaultWeight` — `llvisualparam.h:139-162`.
- `LLWearableList::getAsset` takes a **C-style** `void(*)(LLViewerWearable*, void*)` and a `void*` — `llwearablelist.h:51`. `idmcp_wearable_loaded` must therefore be a free function, not a lambda with captures.
- `LLLocalTextureObject::getID()` — `lllocaltextureobject.h:52`, used exactly this way at `llpaneleditwearable.cpp:606`, including the `IMG_DEFAULT_AVATAR`-means-empty convention.
- `gAgentWearables.getWearableFromItemID(item_id)` returns `LLViewerWearable*` — `llagentwearables.h:115`.

- [ ] **Step 9: Commit**

```bash
git add indra/newview/idmcptools_wearable.cpp indra/newview/idmcptools.h \
        indra/newview/idmcpserver.cpp indra/newview/CMakeLists.txt
git commit -m "feat: idmcp wearable.read"
```

---

### Task 5: `wearable.create` and `wearable.write`

Completes the wearable half. Validation runs entirely before any mutation, so one bad field cannot leave a wearable half-written.

**Files:**
- Modify: `indra/newview/idmcptools_wearable.cpp` — add the write path, register two tools

**Interfaces:**
- Consumes: everything Task 4 produced.
- Produces: nothing consumed by later tasks.

- [ ] **Step 1: Add a param lookup helper**

Inside the anonymous namespace in `idmcptools_wearable.cpp`, after `valid_te_names`:

```cpp
    // Find a tweakable param on `w` by numeric id (as a JSON key string) or by
    // its name. Returns nullptr if there is no such tweakable param.
    LLVisualParam* param_by_key(LLWearable* w, const std::string& key)
    {
        visual_param_vec_t plist;
        w->getVisualParams(plist);
        for (LLVisualParam* p : plist)
        {
            if (!p || !p->isTweakable()) continue;
            if (key == p->getName()) return p;
            if (key == std::to_string(p->getID())) return p;
        }
        return nullptr;
    }
```

- [ ] **Step 2: Register `wearable.create`**

Insert inside `idmcp_register_wearable_tools`, before its closing brace:

```cpp
    // wearable.create --------------------------------------------------------
    {
        IDMCPTool t;
        t.name = "wearable.create";
        t.description =
            "Create a new wearable at its default settings. {\"parent\"} (folder "
            "UUID), {\"name\"}, {\"type\"} one of shape, skin, hair, eyes, shirt, "
            "pants, shoes, socks, jacket, gloves, undershirt, underpants, skirt, "
            "alpha, tattoo, physics, universal. Optional {\"wear\"} (default true) — "
            "it must be worn before wearable.write can change it. Returns "
            "{accepted, item_id, worn}. Blocked by RLV @edit.";
        t.input_schema = boost::json::parse(
            R"({"type":"object","properties":{"parent":{"type":"string"},"name":{"type":"string"},"type":{"type":"string"},"wear":{"type":"boolean"}},"required":["parent","name","type"],"additionalProperties":false})");
        t.gate = gate_edit;
        t.invoke = [](const boost::json::object& args, const IDMCPCallPtr& call)
        {
            const std::string parent_spec = arg_str(args, "parent");
            const std::string name        = arg_str(args, "name");
            const std::string type_name   = arg_str(args, "type");
            const bool        wear        = arg_bool(args, "wear", true);

            if (!looks_like_uuid(parent_spec) || name.empty())
            {
                idmcp_tool_err(call, IDMCP_ERR_INVALID_PARAMS,
                               "parent must be a folder UUID and name must be non-empty");
                return;
            }
            const LLUUID parent(parent_spec);
            if (!gInventory.getCategory(parent))
            {
                idmcp_tool_err(call, IDMCP_ERR_NOT_FOUND, "no such folder");
                return;
            }
            const LLWearableType::EType type =
                LLWearableType::getInstance()->typeNameToType(type_name);
            if (type == LLWearableType::WT_INVALID || type == LLWearableType::WT_NONE)
            {
                idmcp_tool_err(call, IDMCP_ERR_INVALID_PARAMS,
                               "unknown wearable type '" + type_name + "'");
                return;
            }

            LLAgentWearables::createWearable(type, wear, parent,
                [call, wear](const LLUUID& new_id)
                {
                    if (idmcp_commit_denied(call, RLV_BHVR_EDIT, "edit")) return;
                    boost::json::object o;
                    o["accepted"] = true;
                    o["item_id"]  = new_id.asString();
                    // The wear is separately subject to the RLV wearable locks.
                    // A blocked wear is reported, not fatal — the item exists.
                    o["worn"] = wear && (gAgentWearables.getWearableFromItemID(new_id) != nullptr);
                    idmcp_tool_ok(call, o);
                });
        };
        reg.add(std::move(t));
    }
```

- [ ] **Step 3: Register `wearable.write`**

Insert inside `idmcp_register_wearable_tools`, before its closing brace:

```cpp
    // wearable.write ---------------------------------------------------------
    {
        IDMCPTool t;
        t.name = "wearable.write";
        t.description =
            "Change a worn wearable and save it. {\"item_id\"} plus any of "
            "{\"name\"}, {\"textures\"} {slot: texture-uuid}, {\"colors\"} "
            "{slot: [r,g,b]}, {\"params\"} {name-or-id: value}. Slot and param "
            "names come from wearable.read. The item must be WORN — the viewer has "
            "no way to save an unworn wearable; wear it first with "
            "appearance.wearItems. Values outside a param's min/max are rejected "
            "rather than clamped. Blocked by RLV @edit.";
        t.input_schema = boost::json::parse(
            R"({"type":"object","properties":{"item_id":{"type":"string"},"name":{"type":"string"},"textures":{"type":"object"},"colors":{"type":"object"},"params":{"type":"object"}},"required":["item_id"],"additionalProperties":false})");
        t.gate = gate_edit;
        t.invoke = [](const boost::json::object& args, const IDMCPCallPtr& call)
        {
            const std::string spec = arg_str(args, "item_id");
            if (!looks_like_uuid(spec))
            {
                idmcp_tool_err(call, IDMCP_ERR_INVALID_PARAMS, "item_id must be a UUID");
                return;
            }
            const LLUUID item_id(spec);

            LLViewerWearable* w = gAgentWearables.getWearableFromItemID(item_id);
            if (!w)
            {
                idmcp_tool_err(call, IDMCP_ERR_INVALID_PARAMS,
                               "wearable is not currently worn; wear it first with "
                               "appearance.wearItems");
                return;
            }
            U32 index = 0;
            if (!gAgentWearables.getWearableIndex(w, index))
            {
                idmcp_tool_err(call, IDMCP_ERR_NOT_FOUND, "worn wearable has no index");
                return;
            }
            const LLWearableType::EType type = w->getType();

            // --- Validate everything before mutating anything. ---
            std::vector<std::pair<LLVisualParam*, F32>>     param_writes;
            std::vector<std::pair<ETextureIndex, LLUUID>>   texture_writes;
            std::vector<std::pair<ETextureIndex, LLColor4>> color_writes;

            auto pit = args.find("params");
            if (pit != args.end() && pit->value().is_object())
            {
                for (const auto& kv : pit->value().as_object())
                {
                    const std::string key(kv.key());
                    if (!kv.value().is_number())
                    {
                        idmcp_tool_err(call, IDMCP_ERR_INVALID_PARAMS,
                                       "param '" + key + "' must be a number");
                        return;
                    }
                    LLVisualParam* p = param_by_key(w, key);
                    if (!p)
                    {
                        idmcp_tool_err(call, IDMCP_ERR_INVALID_PARAMS,
                                       "no tweakable param '" + key + "' on this wearable; "
                                       "use wearable.read to list them");
                        return;
                    }
                    const F32 v = (F32)kv.value().to_number<double>();
                    if (v < p->getMinWeight() || v > p->getMaxWeight())
                    {
                        idmcp_tool_err(call, IDMCP_ERR_INVALID_PARAMS,
                                       "param '" + key + "' value out of range");
                        return;
                    }
                    param_writes.emplace_back(p, v);
                }
            }

            auto tit = args.find("textures");
            if (tit != args.end() && tit->value().is_object())
            {
                for (const auto& kv : tit->value().as_object())
                {
                    const std::string key(kv.key());
                    ETextureIndex te;
                    if (!te_by_name(type, key, te))
                    {
                        idmcp_tool_err(call, IDMCP_ERR_INVALID_PARAMS,
                                       "no texture slot '" + key + "' on this wearable type; "
                                       "valid slots: " + valid_te_names(type));
                        return;
                    }
                    const std::string id = kv.value().is_string()
                                               ? std::string(kv.value().as_string().c_str())
                                               : std::string();
                    if (!looks_like_uuid(id))
                    {
                        idmcp_tool_err(call, IDMCP_ERR_INVALID_PARAMS,
                                       "texture slot '" + key + "' must be a texture UUID");
                        return;
                    }
                    texture_writes.emplace_back(te, LLUUID(id));
                }
            }

            auto cit = args.find("colors");
            if (cit != args.end() && cit->value().is_object())
            {
                for (const auto& kv : cit->value().as_object())
                {
                    const std::string key(kv.key());
                    ETextureIndex te;
                    if (!te_by_name(type, key, te))
                    {
                        idmcp_tool_err(call, IDMCP_ERR_INVALID_PARAMS,
                                       "no colour slot '" + key + "' on this wearable type; "
                                       "valid slots: " + valid_te_names(type));
                        return;
                    }
                    if (!kv.value().is_array() || kv.value().as_array().size() != 3)
                    {
                        idmcp_tool_err(call, IDMCP_ERR_INVALID_PARAMS,
                                       "colour '" + key + "' must be [r, g, b] in 0..1");
                        return;
                    }
                    F32 rgb[3];
                    const boost::json::array& a = kv.value().as_array();
                    for (int i = 0; i < 3; ++i)
                    {
                        if (!a[i].is_number())
                        {
                            idmcp_tool_err(call, IDMCP_ERR_INVALID_PARAMS,
                                           "colour '" + key + "' components must be numbers");
                            return;
                        }
                        rgb[i] = (F32)a[i].to_number<double>();
                        if (rgb[i] < 0.f || rgb[i] > 1.f)
                        {
                            idmcp_tool_err(call, IDMCP_ERR_INVALID_PARAMS,
                                           "colour '" + key + "' components must be in 0..1");
                            return;
                        }
                    }
                    color_writes.emplace_back(te, LLColor4(rgb[0], rgb[1], rgb[2], 1.f));
                }
            }

            // --- Apply. Synchronous, so no Commit-phase re-check is needed:
            // the Request-phase gate ran moments ago on this same call stack. ---
            for (const auto& [p, v] : param_writes)
            {
                w->setVisualParamWeight(p->getID(), v, false);
            }
            for (const auto& [te, color] : color_writes)
            {
                w->setClothesColor((S32)te, color, false);
            }
            for (const auto& [te, tex_id] : texture_writes)
            {
                LLViewerFetchedTexture* image = LLViewerTextureManager::getFetchedTexture(tex_id);
                if (image->getID() == IMG_DEFAULT)
                {
                    image = LLViewerTextureManager::getFetchedTexture(IMG_DEFAULT_AVATAR);
                }
                gAgentAvatarp->setLocalTexture(te, image, false, index);
            }

            gAgentAvatarp->wearableUpdated(type, false);
            gAgentWearables.saveWearable(type, index, true, arg_str(args, "name"));

            boost::json::object o;
            o["accepted"] = true;
            idmcp_tool_ok(call, o);
        };
        reg.add(std::move(t));
    }
```

- [ ] **Step 4: Compile checklist (read, do not build)**

Confirm by reading:
- `LLAgentWearables::createWearable(type, wear, parent_id, created_cb)` is **static** and its callback is `std::function<void(const LLUUID&)>` — `llagentwearables.h:156`.
- `getWearableIndex(const LLWearable*, U32&)` is inherited from `LLWearableData` — `llwearabledata.h:63`.
- `saveWearable(type, index, send_update, new_name)` — the Firestorm signature has the extra `send_update` parameter — `llagentwearables.h:208`. Passing an empty `new_name` means "do not rename", per its `!new_name.empty()` guard.
- `w->setVisualParamWeight(id, v, false)` then `gAgentAvatarp->wearableUpdated(type, false)` is exactly what the editor's slider commit does — `llscrollingpanelparam.cpp:266`, `llpaneleditwearable.cpp:1056`.
- `setClothesColor(S32 te, const LLColor4&, bool upload_bake)` — `llwearable.h:126`.
- The `IMG_DEFAULT` → `IMG_DEFAULT_AVATAR` substitution and the `setLocalTexture(te, image, false, index)` call match `LLPanelEditWearable::onTexturePickerCommit`.
- `LLViewerTextureManager::getFetchedTexture` comes from `llviewertexture.h`, already included.
- Structured bindings in a range-`for` over a `std::vector<std::pair<...>>` are C++17 — fine for this tree.
- `boost::json::object` iteration yields `key_value_pair`, whose `.key()` returns a `string_view` (hence the explicit `std::string key(kv.key());`) and whose `.value()` returns the `boost::json::value`. No existing `idmcptools_*.cpp` iterates a JSON object's keys, so this is the first — it is stock boost::json, not a local idiom.

- [ ] **Step 5: Commit**

```bash
git add indra/newview/idmcptools_wearable.cpp
git commit -m "feat: idmcp wearable.create and wearable.write"
```

---

### Task 6: Document the `@edit` extension, then verify live

The code is unverifiable in this workspace. This task is the verification, and it is not optional.

**Files:**
- Modify: `doc/rlva-custom-commands.md`

- [ ] **Step 1: Confirm where the section goes**

```bash
grep -n '^#' doc/rlva-custom-commands.md
```

The doc's preamble currently says stock RLVa commands are **not** documented here, and its two top-level sections are `# Five's commands` (line 31) and `# Trish's commands` (line 163). `@edit` is a *stock* command whose behaviour this fork extends, so it fits neither — it needs its own top-level section.

- [ ] **Step 2: Widen the preamble**

Replace this sentence in the preamble:

```markdown
Reference for the RLVa commands **added by this fork** — author **Five** (Amalthea
Skydancer) and **Trish**. Stock RLVa (Kitty Barnett) and upstream Firestorm commands
are **not** documented here.
```

with:

```markdown
Reference for the RLVa commands **added by this fork** — author **Five** (Amalthea
Skydancer) and **Trish** — plus any stock command whose **behaviour** this fork
extends (see *Extended stock commands*). Stock RLVa (Kitty Barnett) and upstream
Firestorm commands are otherwise **not** documented here.
```

- [ ] **Step 3: Add the `@edit` section**

Append this as a new top-level section at the end of the file, matching the heading depth and entry style of the existing command entries:

```markdown
---

# Extended stock commands

Stock RLVa commands whose scope this fork **widens**. A restraint author working
from the upstream RLVa spec will not expect the extra behaviour documented here.

## `@edit` — extended to inventory content

- **Syntax:** `@edit=n|y` · unchanged; still `NONE_OR_EXCEPTION`.
- **Stock behaviour:** blocks editing **in-world objects** — the build tools, the
  edit floater and object selection. `RlvActions::canEdit` takes an
  `LLViewerObject*` (`rlvactions.cpp:698`) or an `ERlvCheckType` (`:668`); the
  toggle handler drives the build floater (`rlvhandler.cpp:2099`).
- **Fork extension:** additionally blocks **authoring inventory content** through
  the embedded MCP server — `gesture.create`, `gesture.write`, `wearable.create`
  and `wearable.write` return the structured RLV denial while `@edit` is active.
- **Not affected:** reads. `gesture.read`, `gesture.list` and `wearable.read`
  follow `@showinv`, as the rest of the inventory tools do. `gesture.play` is
  governed by `@sendgesture` (enforced inside `LLGestureMgr`, `llgesturemgr.cpp:557`).
- **Deliberate gap:** a worn wearable held by an RLV lock is still rewritable while
  `@edit` is off — the lock governs detaching, not content. Content protection
  requires `@edit`. This was considered and chosen, not overlooked.
- **Enforced at:** `idmcptools_gesture.cpp` and `idmcptools_wearable.cpp`
  (`gate_edit`, plus the commit-phase re-check in each deferred completion).
```

- [ ] **Step 4: Commit the doc**

```bash
git add doc/rlva-custom-commands.md
git commit -m "docs: note that @edit covers inventory content in this fork"
```

- [ ] **Step 5: Hand the build to Five**

Tell Five the branch is ready and needs a **full rebuild** — two new `.cpp` files force a PCH rebuild via autobuild+cmake. Do not attempt the build.

- [ ] **Step 6: Reconnect MCP after the rebuild**

New tools do not appear until the `firestorm` MCP server is reconnected — the tool list is cached at connect time, so a rebuild alone is not enough. Ask Five to run `/mcp` and reconnect before testing.

- [ ] **Step 7: Work the live verification matrix**

Run each with Five's viewer logged in, recording the actual response for each row:

1. `gesture.create` in a known folder → `gesture.write` with one animation step (by **name**) and one chat step → `gesture.activate` → the trigger word fires in-world, and opening the gesture in the viewer's own editor shows the same steps that were written.
2. `gesture.read` on an existing multi-step gesture round-trips: read, write the same JSON back, read again, compare — identical.
3. `gesture.write` with an animation name that matches two inventory items → error naming both UUIDs, and the gesture is unchanged.
4. `upload.image` a texture → `wearable.create` type `tattoo` with an explicit `name` → the inventory item carries **exactly that name** (not "New Tattoo") → `wearable.write` setting all three tattoo slots → the tattoo renders on the avatar.
5. `wearable.read` on the worn **shape** lists the tweakable params; `wearable.write` on `height` (or any body morph) changes the avatar **visibly and immediately** — no relog, no appearance-editor session — and the change **survives a relog**. This is the `writeToAvatar` + `updateVisualParams` fix (Ruling 15): before it, shape writes saved into the wearable without ever reaching the mesh. Pass = the morph is on screen the moment the call returns, and still there after relog.
6. `wearable.write` on an unworn wearable returns the "wear it first" error — no crash.
7. `wearable.write` with an out-of-range param value is rejected and **nothing else in the same request is applied**.
8. With `@edit=n` from a relay: `gesture.create`, `gesture.write`, `wearable.create`, `wearable.write` all return `-32011` naming the restricting object; `gesture.read` and `wearable.read` still work.
9. With `@showinv=n`: `gesture.read`, `gesture.list` and `wearable.read` return `-32011`; `gesture.play` on an already-active gesture still works.
10. With `@sendgesture=n`: `gesture.play` on an active gesture → **nothing plays in-world**. Note: the tool response may still read `accepted: true` — the block is silent inside `LLGestureMgr` and surfacing it is a deferred Task 1 minor. Pass = no in-world playback; do not fail the row on the response field.
11. `wearable.create` with `wear: true` → the appearance/customize editor does **not** open and the camera does **not** move (Ruling 14 — the old path popped the customize UI). The response carries `wear_requested`, **not** `worn` — the COF round-trip is still in flight at reply time — and `appearance.getWorn` shortly after confirms the item is actually worn.
12. `wearable.create` with `type: "universal"` on a region **without** Bakes on Mesh → an immediate error naming the Bakes-on-Mesh requirement — not a hang until the deadline sweep times out (Ruling 13: `createWearable` silently early-returns there). If no non-BoM region is reachable, run the positive half (`universal` succeeds on a BoM region) and record the negative half as environment-blocked, not passed.
13. `wearable.write` with a `colors` entry: on a worn **tattoo** or **universal**, the slot tints — these are genuinely tintable in this tree (tint params 1071-1073 etc.). The same write to a worn **bodypaint** or **alpha** slot returns the "no colour channel on this wearable type" error listing the tintable slots, and nothing in the request is applied (Rulings 9/11 — both sides route through `teToColorParams`).
14. Author a gesture in the **viewer's own editor** with a wait step whose three checkboxes are all off (a flags-zero wait) → `gesture.write` changing **only its trigger** succeeds, and the wait step survives the round-trip (Ruling 6: `parse_steps` previously rejected the exact shape the stock editor saves).
15. `gesture.write` with enough long chat steps to push the serialized size past 1000 bytes → rejected **locally** with the actual byte size in the message ("serialized size N exceeds the 1000-byte limit"), no upload attempted, gesture unchanged (Ruling 3 — the stock editor's own cap, `llpreviewgesture.cpp:1097`).
16. Take an existing gesture, **rename or delete** the inventory animation one of its steps uses → `gesture.read` → `gesture.write` changing only the trigger → succeeds, and the untouched step still points at the original asset. This is the lossless round-trip guarantee (Ruling 1): the step's `asset_id` is honoured over its stale `animation` name; name resolution is the fallback, not the authority.
17. `gesture.write` with `steps: "foo"` (present but not an array) → "steps must be an array" error and the gesture is unchanged — not silently treated as "preserve existing steps" (Ruling 8).
18. Activate a gesture with a known trigger word → `gesture.write` changing **only the trigger** to a new word — and do **not** deactivate or reactivate the gesture at any point. The old trigger **no longer fires** in-world and the new one does, immediately after the write returns; `gesture.read` right after shows the new trigger. Pass = the live `LLGestureMgr` copy was replaced in place (`replaceGesture`); the old trigger still firing after `accepted: true` means the live copy went stale — the exact bug `activateGestureWithAsset`'s early-return for still-active gestures used to cause.

- [ ] **Step 8: Fix what the matrix finds, then commit**

Any failure is a real bug, not a test-environment artifact. Fix it, re-run the affected rows, and commit with a `fix:` message. Do not mark this task complete with a failing row.
