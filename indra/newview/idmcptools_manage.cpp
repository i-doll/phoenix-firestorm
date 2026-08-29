/**
 * @file idmcptools_manage.cpp
 * @brief <ID> MCP server: inventory organize (create/rename/move/delete) + give.
 *
 * Part of Five's custom Firestorm fork.
 *
 * Mutations are gated by the RLVa folder-lock singleton (RlvFolderLocks) and, for
 * giving, RlvActions::canGiveInventory. delete requires an explicit confirm flag.
 * No-copy items are refused for giving (they would otherwise pop a modal and be
 * transferred away).
 */

#include "llviewerprecompiledheaders.h"

#include "idmcptools.h"
#include "idmcpserver.h"
#include "idmcprlvgate.h"

#include "llinventorymodel.h"
#include "llinventoryfunctions.h"
#include "llviewerinventory.h"
#include "llgiveinventory.h"
#include "llpermissions.h"
#include "llfoldertype.h"
#include "llagent.h"

#include "rlvlocks.h"       // RlvFolderLocks
#include "rlvactions.h"
#include "lluuid.h"

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

    LLUUID arg_uuid(const boost::json::object& args, const char* key)
    {
        const std::string s = arg_str(args, key);
        return looks_like_uuid(s) ? LLUUID(s) : LLUUID::null;
    }

    IDMCPGateResult deny(const char* label)
    {
        IDMCPGateResult r;
        r.allowed   = false;
        r.behaviour = label;
        return r;
    }

    // Resolve a folder spec (UUID or "root") to an id.
    LLUUID resolve_parent(const std::string& s)
    {
        if (looks_like_uuid(s)) return LLUUID(s);
        if (s == "root")        return gInventory.getRootFolderID();
        return LLUUID::null;
    }
}

// ---------------------------------------------------------------------------

void idmcp_register_manage_tools(IDMCPToolRegistry& reg)
{
    // inventory.createFolder -------------------------------------------------
    {
        IDMCPTool t;
        t.name = "inventory.createFolder";
        t.description =
            "Create a new folder. {\"parent\"} (folder UUID or \"root\") and "
            "{\"name\"}. Returns the new folder id.";
        t.input_schema = boost::json::parse(
            R"({"type":"object","properties":{"parent":{"type":"string"},"name":{"type":"string"}},"required":["parent","name"],"additionalProperties":false})");
        t.invoke = [](const boost::json::object& args, const IDMCPCallPtr& call)
        {
            const LLUUID parent = resolve_parent(arg_str(args, "parent"));
            const std::string name = arg_str(args, "name");
            if (parent.isNull() || name.empty())
            {
                idmcp_tool_err(call, IDMCP_ERR_INVALID_PARAMS, "parent and name required");
                return;
            }
            gInventory.createNewCategory(parent, LLFolderType::FT_NONE, name,
                [call](const LLUUID& new_id)
                {
                    boost::json::object o;
                    o["accepted"]  = true;
                    o["folder_id"] = new_id.asString();
                    idmcp_tool_ok(call, o);
                });
        };
        reg.add(std::move(t));
    }

    // inventory.rename -------------------------------------------------------
    {
        IDMCPTool t;
        t.name = "inventory.rename";
        t.description = "Rename an inventory item or folder. {\"id\"} and {\"name\"}.";
        t.input_schema = boost::json::parse(
            R"({"type":"object","properties":{"id":{"type":"string"},"name":{"type":"string"}},"required":["id","name"],"additionalProperties":false})");
        t.gate = [](const boost::json::object& args, IDMCPGatePhase) -> IDMCPGateResult
        {
            if (!IDMCPRlvGate::isEnabled()) return IDMCPGateResult();
            const LLUUID id = arg_uuid(args, "id");
            if (id.isNull()) return IDMCPGateResult();
            if (gInventory.getItem(id))
                return RlvFolderLocks::instance().canRenameItem(id) ? IDMCPGateResult() : deny("locked-item");
            if (gInventory.getCategory(id))
                return RlvFolderLocks::instance().canRenameFolder(id) ? IDMCPGateResult() : deny("locked-folder");
            return IDMCPGateResult();
        };
        t.invoke = [](const boost::json::object& args, const IDMCPCallPtr& call)
        {
            const LLUUID id = arg_uuid(args, "id");
            const std::string name = arg_str(args, "name");
            if (id.isNull() || name.empty())
            {
                idmcp_tool_err(call, IDMCP_ERR_INVALID_PARAMS, "id and name required");
                return;
            }
            if (gInventory.getItem(id))
            {
                LLSD updates; updates["name"] = name;
                update_inventory_item(id, updates, nullptr);
            }
            else if (gInventory.getCategory(id))
            {
                rename_category(&gInventory, id, name, nullptr);
            }
            else
            {
                idmcp_tool_err(call, IDMCP_ERR_NOT_FOUND, "no such item or folder");
                return;
            }
            boost::json::object o; o["accepted"] = true;
            idmcp_tool_ok(call, o);
        };
        reg.add(std::move(t));
    }

    // inventory.move ---------------------------------------------------------
    {
        IDMCPTool t;
        t.name = "inventory.move";
        t.description = "Move an item or folder into another folder. {\"id\"} and {\"dest\"} (folder UUID).";
        t.input_schema = boost::json::parse(
            R"({"type":"object","properties":{"id":{"type":"string"},"dest":{"type":"string"}},"required":["id","dest"],"additionalProperties":false})");
        t.gate = [](const boost::json::object& args, IDMCPGatePhase) -> IDMCPGateResult
        {
            if (!IDMCPRlvGate::isEnabled()) return IDMCPGateResult();
            const LLUUID id = arg_uuid(args, "id");
            const LLUUID dest = arg_uuid(args, "dest");
            if (id.isNull() || dest.isNull()) return IDMCPGateResult();
            if (gInventory.getItem(id))
                return RlvFolderLocks::instance().canMoveItem(id, dest) ? IDMCPGateResult() : deny("locked-item");
            if (gInventory.getCategory(id))
                return RlvFolderLocks::instance().canMoveFolder(id, dest) ? IDMCPGateResult() : deny("locked-folder");
            return IDMCPGateResult();
        };
        t.invoke = [](const boost::json::object& args, const IDMCPCallPtr& call)
        {
            const LLUUID id = arg_uuid(args, "id");
            const LLUUID dest = arg_uuid(args, "dest");
            if (id.isNull() || dest.isNull())
            {
                idmcp_tool_err(call, IDMCP_ERR_INVALID_PARAMS, "id and dest (UUIDs) required");
                return;
            }
            if (gInventory.getItem(id))
            {
                change_item_parent(id, dest);
            }
            else if (LLViewerInventoryCategory* cat = gInventory.getCategory(id))
            {
                gInventory.changeCategoryParent(cat, dest, false);
            }
            else
            {
                idmcp_tool_err(call, IDMCP_ERR_NOT_FOUND, "no such item or folder");
                return;
            }
            boost::json::object o; o["accepted"] = true;
            idmcp_tool_ok(call, o);
        };
        reg.add(std::move(t));
    }

    // inventory.delete -------------------------------------------------------
    {
        IDMCPTool t;
        t.name = "inventory.delete";
        t.description =
            "Move an item or folder to Trash. Requires {\"id\"} and {\"confirm\": true}.";
        t.input_schema = boost::json::parse(
            R"({"type":"object","properties":{"id":{"type":"string"},"confirm":{"type":"boolean"}},"required":["id","confirm"],"additionalProperties":false})");
        t.gate = [](const boost::json::object& args, IDMCPGatePhase) -> IDMCPGateResult
        {
            if (!IDMCPRlvGate::isEnabled()) return IDMCPGateResult();
            const LLUUID id = arg_uuid(args, "id");
            if (id.isNull()) return IDMCPGateResult();
            if (gInventory.getItem(id))
                return RlvFolderLocks::instance().canRemoveItem(id) ? IDMCPGateResult() : deny("locked-item");
            if (gInventory.getCategory(id))
                return RlvFolderLocks::instance().canRemoveFolder(id) ? IDMCPGateResult() : deny("locked-folder");
            return IDMCPGateResult();
        };
        t.invoke = [](const boost::json::object& args, const IDMCPCallPtr& call)
        {
            auto cit = args.find("confirm");
            const bool confirm = (cit != args.end() && cit->value().is_bool() && cit->value().as_bool());
            if (!confirm)
            {
                idmcp_tool_err(call, IDMCP_ERR_INVALID_PARAMS, "delete requires confirm:true");
                return;
            }
            const LLUUID id = arg_uuid(args, "id");
            if (gInventory.getItem(id))
            {
                remove_inventory_item(id, nullptr);
            }
            else if (gInventory.getCategory(id))
            {
                remove_inventory_category(id, nullptr);
            }
            else
            {
                idmcp_tool_err(call, IDMCP_ERR_NOT_FOUND, "no such item or folder");
                return;
            }
            boost::json::object o; o["accepted"] = true;
            idmcp_tool_ok(call, o);
        };
        reg.add(std::move(t));
    }

    // inventory.giveItem -----------------------------------------------------
    {
        IDMCPTool t;
        t.name = "inventory.giveItem";
        t.description =
            "Give a copy of an inventory item to another avatar. {\"item_id\"} and "
            "{\"to_agent\"} (UUID). No-copy items are refused. Blocked by RLV @share.";
        t.input_schema = boost::json::parse(
            R"({"type":"object","properties":{"item_id":{"type":"string"},"to_agent":{"type":"string"}},"required":["item_id","to_agent"],"additionalProperties":false})");
        t.gate = [](const boost::json::object& args, IDMCPGatePhase) -> IDMCPGateResult
        {
            if (!IDMCPRlvGate::isEnabled()) return IDMCPGateResult();
            const LLUUID to = arg_uuid(args, "to_agent");
            if (to.notNull() && !RlvActions::canGiveInventory(to))
            {
                return deny("share");
            }
            return IDMCPGateResult();
        };
        t.invoke = [](const boost::json::object& args, const IDMCPCallPtr& call)
        {
            const LLUUID item_id = arg_uuid(args, "item_id");
            const LLUUID to = arg_uuid(args, "to_agent");
            if (item_id.isNull() || to.isNull())
            {
                idmcp_tool_err(call, IDMCP_ERR_INVALID_PARAMS, "item_id and to_agent (UUIDs) required");
                return;
            }
            LLViewerInventoryItem* item = gInventory.getItem(item_id);
            if (!item)
            {
                idmcp_tool_err(call, IDMCP_ERR_NOT_FOUND, "no such item");
                return;
            }
            if (!item->getPermissions().allowCopyBy(gAgent.getID()))
            {
                idmcp_tool_err(call, IDMCP_ERR_PERMISSION,
                               "item is no-copy; refusing to give it away");
                return;
            }
            // Commit-phase re-check: a restriction may have arrived since request.
            if (IDMCPRlvGate::isEnabled() && !RlvActions::canGiveInventory(to))
            {
                boost::json::object data;
                data["restriction"] = "share";
                data["checkedAt"]   = "commit";
                idmcp_tool_err(call, IDMCP_ERR_RLV_RESTRICTED, "blocked by RLV restriction @share", std::move(data));
                return;
            }
            LLGiveInventory::doGiveInventoryItem(to, item);

            boost::json::object o; o["accepted"] = true;
            idmcp_tool_ok(call, o);
        };
        reg.add(std::move(t));
    }

    // inventory.giveFolder ---------------------------------------------------
    {
        IDMCPTool t;
        t.name = "inventory.giveFolder";
        t.description =
            "Give a folder (and its contents) to another avatar. {\"folder_id\"} "
            "and {\"to_agent\"} (UUID). Blocked by RLV @share.";
        t.input_schema = boost::json::parse(
            R"({"type":"object","properties":{"folder_id":{"type":"string"},"to_agent":{"type":"string"}},"required":["folder_id","to_agent"],"additionalProperties":false})");
        t.gate = [](const boost::json::object& args, IDMCPGatePhase) -> IDMCPGateResult
        {
            if (!IDMCPRlvGate::isEnabled()) return IDMCPGateResult();
            const LLUUID to = arg_uuid(args, "to_agent");
            if (to.notNull() && !RlvActions::canGiveInventory(to)) return deny("share");
            return IDMCPGateResult();
        };
        t.invoke = [](const boost::json::object& args, const IDMCPCallPtr& call)
        {
            const LLUUID folder_id = arg_uuid(args, "folder_id");
            const LLUUID to = arg_uuid(args, "to_agent");
            if (folder_id.isNull() || to.isNull())
            {
                idmcp_tool_err(call, IDMCP_ERR_INVALID_PARAMS, "folder_id and to_agent (UUIDs) required");
                return;
            }
            LLViewerInventoryCategory* cat = gInventory.getCategory(folder_id);
            if (!cat)
            {
                idmcp_tool_err(call, IDMCP_ERR_NOT_FOUND, "no such folder");
                return;
            }
            if (IDMCPRlvGate::isEnabled() && !RlvActions::canGiveInventory(to))
            {
                boost::json::object data;
                data["restriction"] = "share";
                data["checkedAt"]   = "commit";
                idmcp_tool_err(call, IDMCP_ERR_RLV_RESTRICTED, "blocked by RLV restriction @share", std::move(data));
                return;
            }
            LLGiveInventory::doGiveInventoryCategory(to, cat);

            boost::json::object o; o["accepted"] = true;
            idmcp_tool_ok(call, o);
        };
        reg.add(std::move(t));
    }
}
