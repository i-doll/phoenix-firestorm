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
#include "llappearancemgr.h"
#include "llavatarappearancedefines.h"
#include "llinventorymodel.h"
#include "lllocaltextureobject.h"
#include "llvisualparam.h"
#include "llviewerinventory.h"
#include "llviewerregion.h"
#include "llviewertexture.h"
#include "llviewertexturelist.h"
#include "llviewerwearable.h"
#include "llvoavatarself.h"
#include "llwearablelist.h"
#include "llwearabletype.h"

#include "rlvdefines.h"

#include "lluuid.h"
#include "v4color.h"

#include <memory>
#include <vector>

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

    // The subset of `type`'s texture slots whose colour resolves through
    // teToColorParams — i.e. the slots setClothesColor can actually tint.
    // Computed with the same predicate wearable.write's colour check uses,
    // so message and check cannot drift apart.
    std::string tintable_te_names(LLWearableType::EType type)
    {
        std::string s;
        for (ETextureIndex te : tes_for_type(type))
        {
            U32 color_param_ids[3];
            if (!LLAvatarAppearance::teToColorParams(te, color_param_ids)) continue;
            if (!s.empty()) s += ", ";
            s += te_name(te);
        }
        return s.empty() ? std::string("(none)") : s;
    }

    // Find a tweakable param on `w` by numeric id (as a JSON key string) or by
    // its name. Returns nullptr if there is no such tweakable param.
    LLVisualParam* param_by_key(LLWearable* w, const std::string& key)
    {
        LLWearable::visual_param_vec_t plist;
        w->getVisualParams(plist);
        for (LLVisualParam* p : plist)
        {
            if (!p || !p->isTweakable()) continue;
            if (key == p->getName()) return p;
            if (key == std::to_string(p->getID())) return p;
        }
        return nullptr;
    }

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
        LLWearable::visual_param_vec_t plist;
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

    struct IDMCPWearableRead
    {
        IDMCPCallPtr call;
        LLUUID       item_id;
    };

    void idmcp_wearable_loaded(LLViewerWearable* wearable, void* user_data)
    {
        std::unique_ptr<IDMCPWearableRead> ctx(static_cast<IDMCPWearableRead*>(user_data));
        if (!ctx) return;
        // The check sits in this callback (not the caller's lambda) because it
        // serves only wearable.read's unworn fetch — there is no write path
        // through here to over-restrict. The gesture file differs: its shared
        // loader skips the check and gesture.read's `then` carries it instead
        // (Ruling 16). Don't "fix" the asymmetry back into a bug.
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
}

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
            "{accepted, item_id, wear_requested}; the wear completes "
            "asynchronously and is subject to the RLV wearable locks, so confirm "
            "with appearance.getWorn. Blocked by RLV @edit.";
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
            // createWearable silently early-returns for WT_UNIVERSAL on regions
            // without Bakes on Mesh (llagentwearables.cpp:1722) — the callback
            // never fires and the caller would only see a timeout. Same test,
            // null-hardened, so the caller gets the reason instead.
            if (type == LLWearableType::WT_UNIVERSAL &&
                (!gAgent.getRegion() || !gAgent.getRegion()->bakesOnMeshEnabled()))
            {
                idmcp_tool_err(call, IDMCP_ERR_INVALID_PARAMS,
                               "universal wearables need a region with Bakes on Mesh "
                               "enabled; this region does not support them");
                return;
            }

            // No commit-phase RLV re-check here: by callback time the item
            // already exists, so denying would report a created item as
            // blocked. The request-phase @edit gate is the real gate.
            //
            // Always create with wear=false: createWearable's own wear path
            // (wear_and_edit_cb -> requestEditingWearable) pops the appearance
            // editor and moves the camera — not acceptable from an API call.
            // The wear happens below through the same RLV-gated path
            // appearance.wearItems uses.
            LLAgentWearables::createWearable(type, false, parent,
                [call, wear, name](const LLUUID& new_id)
                {
                    // createWearable has no name parameter, so the item arrives
                    // as e.g. "New Shirt". Rename it to the requested name —
                    // same idiom as inventory.rename (idmcptools_manage.cpp).
                    LLViewerInventoryItem* item = gInventory.getItem(new_id);
                    if (item && item->getName() != name)
                    {
                        LLSD updates;
                        updates["name"] = name;
                        update_inventory_item(new_id, updates, nullptr);
                    }
                    if (wear)
                    {
                        uuid_vec_t ids{ new_id };
                        LLAppearanceMgr::instance().wearItemsOnAvatar(ids, true, false);
                    }
                    boost::json::object o;
                    o["accepted"] = true;
                    o["item_id"]  = new_id.asString();
                    // The COF round-trip has not completed at this point, so
                    // worn state cannot be reported honestly yet — and the wear
                    // is separately subject to the RLV wearable locks. The
                    // caller confirms with appearance.getWorn.
                    o["wear_requested"] = wear;
                    idmcp_tool_ok(call, o);
                });
        };
        reg.add(std::move(t));
    }

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

            // A container that is present but not an object is an error, not a
            // silent no-op — same rule as gesture.write's steps (Ruling 8).
            auto pit = args.find("params");
            if (pit != args.end() && !pit->value().is_object())
            {
                idmcp_tool_err(call, IDMCP_ERR_INVALID_PARAMS,
                               "params must be an object of {name-or-id: value}");
                return;
            }
            if (pit != args.end())
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
            if (tit != args.end() && !tit->value().is_object())
            {
                idmcp_tool_err(call, IDMCP_ERR_INVALID_PARAMS,
                               "textures must be an object of {slot: texture-uuid}");
                return;
            }
            if (tit != args.end())
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
            if (cit != args.end() && !cit->value().is_object())
            {
                idmcp_tool_err(call, IDMCP_ERR_INVALID_PARAMS,
                               "colors must be an object of {slot: [r,g,b]}");
                return;
            }
            if (cit != args.end())
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
                    // setClothesColor resolves through teToColorParams and is a
                    // silent no-op for slots with no colour params (bodypaint,
                    // alpha, tattoo, universal — wearable.read shows them as
                    // black). Reject those rather than pretend to write them.
                    U32 color_param_ids[3];
                    if (!LLAvatarAppearance::teToColorParams(te, color_param_ids))
                    {
                        idmcp_tool_err(call, IDMCP_ERR_INVALID_PARAMS,
                                       "colour slot '" + key + "' has no colour channel on this "
                                       "wearable type; tintable slots: " + tintable_te_names(type));
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
            if (!param_writes.empty())
            {
                // The editor's slider commit follows setVisualParamWeight with
                // exactly these two calls (llscrollingpanelparam.cpp:266-268).
                // Without them a shape morph saves into the wearable without
                // ever reaching the mesh — outside customize mode nothing else
                // picks it up. The colour path below needs only
                // wearableUpdated, matching llpaneleditwearable.cpp:1086.
                w->writeToAvatar(gAgentAvatarp);
                gAgentAvatarp->updateVisualParams();
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
}
