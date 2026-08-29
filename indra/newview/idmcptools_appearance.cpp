/**
 * @file idmcptools_appearance.cpp
 * @brief <ID> MCP server: wear/attach/detach + outfit listing tools.
 *
 * Part of Five's custom Firestorm fork.
 *
 * wearItems/detachItems are RLV-gated per item via the wearable/attachment lock
 * predicates (rlvPredCanWearItem / rlvPredCanRemoveItem). The wear/detach calls
 * are fire-and-forget on the viewer's async appearance pipeline; the tool
 * acknowledges the request (a "worn" confirmation would require COF-change
 * observation, deferred to a later pass). listOutfits/getWorn are read-only,
 * gated on @showinv.
 */

#include "llviewerprecompiledheaders.h"

#include "idmcptools.h"
#include "idmcpserver.h"
#include "idmcprlvgate.h"

#include "llappearancemgr.h"
#include "llinventorymodel.h"
#include "llviewerinventory.h"
#include "llassettype.h"
#include "llfoldertype.h"

#include "rlvcommon.h"     // rlvPredCanWearItem / rlvPredCanRemoveItem
#include "rlvdefines.h"    // RLV_WEAR, RLV_BHVR_DETACH

#include "lluuid.h"

namespace
{
    bool looks_like_uuid(const std::string& s)
    {
        return s.size() == 36 && s[8] == '-' && s[13] == '-' && s[18] == '-' && s[23] == '-';
    }

    uuid_vec_t parse_uuid_array(const boost::json::object& args, const char* key)
    {
        uuid_vec_t out;
        auto it = args.find(key);
        if (it != args.end() && it->value().is_array())
        {
            for (const auto& v : it->value().as_array())
            {
                if (v.is_string())
                {
                    std::string s = v.as_string().c_str();
                    if (looks_like_uuid(s))
                    {
                        out.push_back(LLUUID(s));
                    }
                }
            }
        }
        return out;
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
        if (it != args.end() && it->value().is_bool())
        {
            return it->value().as_bool();
        }
        return def;
    }

    IDMCPGateResult gate_showinv(const boost::json::object&, IDMCPGatePhase)
    {
        return IDMCPRlvGate::checkBehaviour(RLV_BHVR_SHOWINV, "showinv");
    }

    IDMCPGateResult gate_wear(const boost::json::object& args, IDMCPGatePhase)
    {
        if (!IDMCPRlvGate::isEnabled())
        {
            return IDMCPGateResult();
        }
        for (const LLUUID& id : parse_uuid_array(args, "item_ids"))
        {
            LLViewerInventoryItem* item = gInventory.getItem(id);
            if (item && !rlvPredCanWearItem(item, RLV_WEAR))
            {
                IDMCPGateResult r;
                r.allowed   = false;
                r.behaviour = "wear";   // wearable/attachment lock
                return r;
            }
        }
        return IDMCPGateResult();
    }

    IDMCPGateResult gate_detach(const boost::json::object& args, IDMCPGatePhase)
    {
        if (!IDMCPRlvGate::isEnabled())
        {
            return IDMCPGateResult();
        }
        for (const LLUUID& id : parse_uuid_array(args, "item_ids"))
        {
            LLViewerInventoryItem* item = gInventory.getItem(id);
            if (item && !rlvPredCanRemoveItem(item))
            {
                return IDMCPRlvGate::deny(RLV_BHVR_DETACH, "detach");
            }
        }
        return IDMCPGateResult();
    }
}

// ---------------------------------------------------------------------------

void idmcp_register_appearance_tools(IDMCPToolRegistry& reg)
{
    // appearance.wearItems ---------------------------------------------------
    {
        IDMCPTool t;
        t.name = "appearance.wearItems";
        t.description =
            "Wear/attach inventory items. {\"item_ids\"} is an array of item "
            "UUIDs; optional {\"replace\"} (default false) replaces what's on the "
            "same slot/point. Blocked per item by RLV wearable/attachment locks.";
        t.input_schema = boost::json::parse(
            R"({"type":"object","properties":{"item_ids":{"type":"array","items":{"type":"string"}},"replace":{"type":"boolean"}},"required":["item_ids"],"additionalProperties":false})");
        t.gate = gate_wear;
        t.invoke = [](const boost::json::object& args, const IDMCPCallPtr& call)
        {
            uuid_vec_t ids = parse_uuid_array(args, "item_ids");
            if (ids.empty())
            {
                idmcp_tool_err(call, IDMCP_ERR_INVALID_PARAMS, "item_ids required (array of UUIDs)");
                return;
            }
            const bool replace = arg_bool(args, "replace", false);
            LLAppearanceMgr::instance().wearItemsOnAvatar(ids, true, replace);

            boost::json::object out;
            out["accepted"] = true;
            out["count"]    = (int64_t)ids.size();
            idmcp_tool_ok(call, out);
        };
        reg.add(std::move(t));
    }

    // appearance.detachItems -------------------------------------------------
    {
        IDMCPTool t;
        t.name = "appearance.detachItems";
        t.description =
            "Take off / detach worn items. {\"item_ids\"} is an array of item "
            "UUIDs. Blocked per item by RLV @detach and wearable locks.";
        t.input_schema = boost::json::parse(
            R"({"type":"object","properties":{"item_ids":{"type":"array","items":{"type":"string"}}},"required":["item_ids"],"additionalProperties":false})");
        t.gate = gate_detach;
        t.invoke = [](const boost::json::object& args, const IDMCPCallPtr& call)
        {
            uuid_vec_t ids = parse_uuid_array(args, "item_ids");
            if (ids.empty())
            {
                idmcp_tool_err(call, IDMCP_ERR_INVALID_PARAMS, "item_ids required (array of UUIDs)");
                return;
            }
            LLAppearanceMgr::instance().removeItemsFromAvatar(ids);

            boost::json::object out;
            out["accepted"] = true;
            out["count"]    = (int64_t)ids.size();
            idmcp_tool_ok(call, out);
        };
        reg.add(std::move(t));
    }

    // appearance.listOutfits -------------------------------------------------
    {
        IDMCPTool t;
        t.name = "appearance.listOutfits";
        t.description = "List saved outfits (subfolders of My Outfits). Blocked by RLV @showinv.";
        t.input_schema = boost::json::parse(
            R"({"type":"object","properties":{},"additionalProperties":false})");
        t.gate = gate_showinv;
        t.invoke = [](const boost::json::object&, const IDMCPCallPtr& call)
        {
            const LLUUID outfits = gInventory.findCategoryUUIDForType(LLFolderType::FT_MY_OUTFITS);
            if (outfits.isNull())
            {
                idmcp_tool_err(call, IDMCP_ERR_NOT_FOUND, "My Outfits folder not found");
                return;
            }
            callAfterCategoryFetch(outfits, [call, outfits]()
            {
                LLInventoryModel::cat_array_t*  cats  = nullptr;
                LLInventoryModel::item_array_t* items = nullptr;
                gInventory.getDirectDescendentsOf(outfits, cats, items);

                boost::json::array arr;
                if (cats)
                {
                    for (auto& c : *cats)
                    {
                        boost::json::object o;
                        o["id"]   = c->getUUID().asString();
                        o["name"] = c->getName();
                        arr.push_back(std::move(o));
                    }
                }
                boost::json::object out;
                out["outfits"] = std::move(arr);
                idmcp_tool_ok(call, out);
            });
        };
        reg.add(std::move(t));
    }

    // appearance.getWorn -----------------------------------------------------
    {
        IDMCPTool t;
        t.name = "appearance.getWorn";
        t.description =
            "List the items currently worn/attached (the Current Outfit Folder). "
            "Blocked by RLV @showinv.";
        t.input_schema = boost::json::parse(
            R"({"type":"object","properties":{},"additionalProperties":false})");
        t.gate = gate_showinv;
        t.invoke = [](const boost::json::object&, const IDMCPCallPtr& call)
        {
            callAfterCOFFetch([call]()
            {
                const LLUUID cof = LLAppearanceMgr::instance().getCOF();
                LLInventoryModel::cat_array_t*  cats  = nullptr;
                LLInventoryModel::item_array_t* items = nullptr;
                gInventory.getDirectDescendentsOf(cof, cats, items);

                boost::json::array worn;
                if (items)
                {
                    for (auto& linkPtr : *items)
                    {
                        LLViewerInventoryItem* link = linkPtr.get();
                        const LLUUID target = link->getLinkedUUID();
                        LLViewerInventoryItem* actual = gInventory.getItem(target);
                        boost::json::object o;
                        o["id"]   = target.asString();
                        o["name"] = actual ? actual->getName() : link->getName();
                        if (actual)
                        {
                            o["asset_type"] = LLAssetType::lookupHumanReadable(actual->getType());
                        }
                        worn.push_back(std::move(o));
                    }
                }
                boost::json::object out;
                out["worn"] = std::move(worn);
                idmcp_tool_ok(call, out);
            });
        };
        reg.add(std::move(t));
    }

    // appearance.wearOutfit --------------------------------------------------
    {
        IDMCPTool t;
        t.name = "appearance.wearOutfit";
        t.description =
            "Wear a saved outfit folder. {\"folder\"} (outfit UUID) or {\"name\"} "
            "(an outfit under My Outfits); {\"mode\"} \"replace\" (default) or "
            "\"add\". Every item is checked against RLV wearable/attachment locks; "
            "a single locked item blocks the whole outfit.";
        t.input_schema = boost::json::parse(
            R"({"type":"object","properties":{"folder":{"type":"string"},"name":{"type":"string"},"mode":{"type":"string"}},"additionalProperties":false})");
        t.invoke = [](const boost::json::object& args, const IDMCPCallPtr& call)
        {
            LLUUID cat;
            const std::string folder = arg_str(args, "folder");
            if (looks_like_uuid(folder))
            {
                cat = LLUUID(folder);
            }
            else
            {
                const std::string name = arg_str(args, "name");
                if (!name.empty())
                {
                    const LLUUID outfits = gInventory.findCategoryUUIDForType(LLFolderType::FT_MY_OUTFITS);
                    cat = findDescendentCategoryIDByName(outfits, name);
                }
            }
            if (cat.isNull())
            {
                idmcp_tool_err(call, IDMCP_ERR_NOT_FOUND, "outfit folder not found (give folder UUID or name)");
                return;
            }
            const bool replace = (arg_str(args, "mode") != "add");

            callAfterCategoryFetch(cat, [call, cat, replace]()
            {
                // Per-item RLV enforcement (post-fetch, so contents are present).
                if (IDMCPRlvGate::isEnabled())
                {
                    LLInventoryModel::cat_array_t*  cats  = nullptr;
                    LLInventoryModel::item_array_t* items = nullptr;
                    gInventory.getDirectDescendentsOf(cat, cats, items);
                    if (items)
                    {
                        for (auto& linkPtr : *items)
                        {
                            LLViewerInventoryItem* link = linkPtr.get();
                            LLViewerInventoryItem* actual = gInventory.getItem(link->getLinkedUUID());
                            LLViewerInventoryItem* check = actual ? actual : link;
                            if (check && !rlvPredCanWearItem(check, RLV_WEAR))
                            {
                                boost::json::object data;
                                data["restriction"] = "wear";
                                data["item_id"]     = check->getUUID().asString();
                                data["checkedAt"]   = "commit";
                                idmcp_tool_err(call, IDMCP_ERR_RLV_RESTRICTED,
                                               "outfit contains an item blocked by RLV", std::move(data));
                                return;
                            }
                        }
                    }
                }

                if (replace) LLAppearanceMgr::instance().replaceCurrentOutfit(cat);
                else         LLAppearanceMgr::instance().addCategoryToCurrentOutfit(cat);

                boost::json::object o; o["accepted"] = true;
                idmcp_tool_ok(call, o);
            });
        };
        reg.add(std::move(t));
    }
}
