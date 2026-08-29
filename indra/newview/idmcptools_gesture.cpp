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

    // Commit-phase re-check for deferred tools: a restriction may have arrived
    // between the request and the completion. Same shape as the inline check in
    // idmcptools_manage.cpp (inventory.give).
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
        // No @showinv re-check here: this loader also serves gesture.write's
        // read-merge phase, which discloses nothing and is gated by @edit.
        // The disclosure gate lives in gesture.read's `then` (Ruling 16) —
        // unlike idmcp_wearable_loaded, whose callback is read-path only.

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

    // Point the local inventory item at the freshly-uploaded asset so a read
    // right after a write sees the new content. Deliberately duplicated from
    // idmcptools_inventory.cpp:324 — that copy is file-local, and this fork
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
                // A flags-zero wait is a no-op step, but the stock editor saves
                // one happily (llpreviewgesture.cpp:1555, all checkboxes off), so
                // refusing it here would block edits to gestures the viewer can
                // save — and break the gesture.read round-trip on them.
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

    // Serialize `g` and push it to UpdateGestureAgentInventory. Takes ownership
    // of nothing; `g` must outlive only this call.
    void idmcp_gesture_upload(LLMultiGesture* g, const LLUUID& item_id, const IDMCPCallPtr& call)
    {
        // Commit-phase re-check before the upload is enqueued: the request-phase
        // gate may have run before an async asset load, and a restriction can
        // arrive in that window.
        if (idmcp_commit_denied(call, RLV_BHVR_EDIT, "edit")) return;

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
        // Same grid-side limit the viewer's own save enforces
        // (llpreviewgesture.cpp:1097, "GestureSaveFailedTooManySteps").
        if (dp.getCurrentSize() > 1000)
        {
            idmcp_tool_err(call, IDMCP_ERR_INVALID_PARAMS,
                           "gesture is too large: serialized size " +
                           std::to_string(dp.getCurrentSize()) +
                           " exceeds the 1000-byte limit; use fewer or shorter steps");
            return;
        }
        const std::string payload(&buffer[0]);

        LLResourceUploadInfo::ptr_t info = std::make_shared<LLBufferedAssetUploadInfo>(
            item_id, LLAssetType::AT_GESTURE, payload,
            [call](LLUUID itemId, LLUUID newAssetId, LLUUID, LLSD)
            {
                // No RLV re-check here: by now the upload has landed server-side,
                // so reporting a block would falsely claim the write failed. The
                // commit gate is the pre-enqueue check above; this callback only
                // syncs the local model to what the server already holds.
                idmcp_set_item_asset(itemId, newAssetId);
                // Keep the live copy in step with what was just saved.
                // activateGestureWithAsset early-returns for a gesture that is
                // still in mActive, so it would silently leave the OLD trigger
                // and steps live. replaceGesture is the stock editor's path for
                // saving an active gesture (llpreviewgesture.cpp:1060): it stops
                // the old copy, refetches the new asset into mActive, and
                // informs the server of the new asset id.
                if (LLGestureMgr::instance().isGestureActive(itemId))
                {
                    LLGestureMgr::instance().replaceGesture(itemId, newAssetId);
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
}

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
                // A @showinv may have arrived while the asset was fetching;
                // re-check at the moment content would reach the caller. This
                // lives here rather than in idmcp_gesture_loaded because that
                // loader also serves gesture.write's read-merge phase, which
                // discloses nothing and stays gated by @edit (Ruling 16).
                if (idmcp_commit_denied(call, RLV_BHVR_SHOWINV, "showinv")) return;
                boost::json::object o = gesture_to_json(g);
                o["name"]   = g->mName;
                o["active"] = LLGestureMgr::instance().isGestureActive(item_id);
                idmcp_tool_ok(call, o);
            });
        };
        reg.add(std::move(t));
    }

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
                    // No RLV re-check: the item already exists by the time this
                    // fires, and creation was enqueued synchronously right after
                    // the request-phase gate — there is no window to guard, only
                    // a created item to report honestly.
                    boost::json::object o;
                    o["accepted"] = true;
                    o["item_id"]  = new_id.asString();
                    idmcp_tool_ok(call, o);
                }));
        };
        reg.add(std::move(t));
    }

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
                // Accept any JSON number with an integral in-range value (5.0
                // and 5 both mean 5 — serializers differ); anything else that
                // is present is an error, never a silent no-op (Ruling 8).
                if (a.contains("key"))
                {
                    const boost::json::value& kv = a.at("key");
                    const double d = kv.is_number() ? kv.to_number<double>() : -1.0;
                    if (d < 0.0 || d > 255.0 || d != (double)(S64)d)
                    {
                        idmcp_tool_err(call, IDMCP_ERR_INVALID_PARAMS,
                                       "key must be an integer 0-255");
                        return;
                    }
                    merged.mKey = (KEY)(S64)d;
                }
                if (a.contains("mask"))
                {
                    const boost::json::value& mv = a.at("mask");
                    const double d = mv.is_number() ? mv.to_number<double>() : -1.0;
                    if (d < 0.0 || d > (double)0xFFFFFFFFu || d != (double)(S64)d)
                    {
                        idmcp_tool_err(call, IDMCP_ERR_INVALID_PARAMS,
                                       "mask must be an integer 0-4294967295");
                        return;
                    }
                    merged.mMask = (MASK)(S64)d;
                }

                if (a.contains("steps"))
                {
                    if (!a.at("steps").is_array())
                    {
                        idmcp_tool_err(call, IDMCP_ERR_INVALID_PARAMS, "steps must be an array");
                        return;
                    }
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
}
