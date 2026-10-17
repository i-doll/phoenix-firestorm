/**
 * @file idmcptools_manage.cpp
 * @brief <ID> MCP server: inventory organize (create/rename/move/copy/delete) + give.
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
#include "idmcptools_object.h"      // idmcp_obj::argNextOwner, maskJson

#include "llinventorydefines.h"     // LLInventoryItemFlags
#include "llinventorymodel.h"
#include "llinventoryfunctions.h"
#include "llinventorymodelbackgroundfetch.h"
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
    constexpr U32 PERM_CMT = PERM_COPY | PERM_MODIFY | PERM_TRANSFER;

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

    bool in_library(const LLUUID& id)
    {
        const LLUUID& lib = gInventory.getLibraryRootFolderID();
        return lib.notNull() && (id == lib || gInventory.isObjectDescendentOf(id, lib));
    }

    // copy_inventory_category copies only what has loaded, so a folder copy
    // waits until every subfolder has.
    bool tree_loaded(const LLUUID& cat_id)
    {
        if (!gInventory.isCategoryComplete(cat_id)) return false;
        LLInventoryModel::cat_array_t*  cats  = nullptr;
        LLInventoryModel::item_array_t* items = nullptr;
        gInventory.getDirectDescendentsOf(cat_id, cats, items);
        if (cats)
        {
            for (const LLPointer<LLViewerInventoryCategory>& cat : *cats)
            {
                if (!tree_loaded(cat->getUUID())) return false;
            }
        }
        return true;
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

    // inventory.setPermissions -----------------------------------------------
    {
        IDMCPTool t;
        t.name = "inventory.setPermissions";
        t.description =
            "Set the next-owner permissions of items in your inventory, as the item's Properties "
            "window does. {\"item_ids\"}*: one or more item UUIDs (a link sets the item it points to); "
            "{\"next_owner\"}*: {copy, modify, transfer} booleans; the next owner needs copy or "
            "transfer. Returns {items:[{item_id, name, result, next_owner}]} where result is set, "
            "unchanged, failed (the server kept the old permissions), not_found, not_loaded (load it "
            "with inventory.getItem first) or permission (you don't own it, can't transfer it, or its "
            "permissions can't be restricted). Turning copy off keeps transfer on, and bits missing "
            "from the item's base permissions stay off.";
        t.input_schema = boost::json::parse(
            R"({"type":"object","properties":{"item_ids":{"type":"array","items":{"type":"string"},"minItems":1},)"
            R"("next_owner":{"type":"object","properties":{"copy":{"type":"boolean"},"modify":{"type":"boolean"},"transfer":{"type":"boolean"}},)"
            R"("required":["copy","modify","transfer"],"additionalProperties":false}},)"
            R"("required":["item_ids","next_owner"],"additionalProperties":false})");
        t.timeout = 30.0;
        t.invoke = [](const boost::json::object& args, const IDMCPCallPtr& call)
        {
            U32 want = 0;
            if (!idmcp_obj::argNextOwner(args, call, want)) return;
            auto ids = args.find("item_ids");
            if (ids == args.end() || !ids->value().is_array() || ids->value().as_array().empty())
            {
                idmcp_tool_err(call, IDMCP_ERR_INVALID_PARAMS, "item_ids must be a non-empty array of item UUIDs");
                return;
            }

            struct Batch
            {
                IDMCPCallPtr       call;
                boost::json::array items;
                size_t             pending = 0;
            };
            struct Update
            {
                size_t                          index;
                LLPointer<LLViewerInventoryItem> item;
                U32                             expect;
            };
            auto batch  = std::make_shared<Batch>();
            batch->call = call;
            std::vector<Update> updates;

            // The checks and update of LLSidepanelItemInfo::updatePermissions.
            for (const boost::json::value& v : ids->value().as_array())
            {
                boost::json::object r;
                const std::string s = v.is_string() ? std::string(v.as_string().c_str()) : std::string();
                r["item_id"] = s;
                LLViewerInventoryItem* item = looks_like_uuid(s) ? gInventory.getItem(LLUUID(s)) : nullptr;
                if (item && item->getIsLinkType()) item = item->getLinkedItem();
                if (!item || in_library(item->getUUID()))
                {
                    r["result"] = "not_found";
                    batch->items.push_back(std::move(r));
                    continue;
                }
                r["item_id"] = item->getUUID().asString();
                r["name"]    = item->getName();
                const LLPermissions& old = item->getPermissions();
                if (!item->isFinished())
                {
                    r["result"] = "not_loaded";
                }
                else if (old.getOwner() != gAgent.getID() ||
                         !old.allowOperationBy(PERM_TRANSFER, gAgent.getID()) ||
                         LLInventoryType::cannotRestrictPermissions(item->getInventoryType()))
                {
                    r["result"] = "permission";
                }
                else
                {
                    LLPermissions perm(old);
                    perm.setNextOwnerBits(gAgent.getID(), LLUUID::null, true, want);
                    if (PERM_CMT & ~want)
                    {
                        perm.setNextOwnerBits(gAgent.getID(), LLUUID::null, false, PERM_CMT & ~want);
                    }
                    if (perm == old)
                    {
                        r["result"]     = "unchanged";
                        r["next_owner"] = idmcp_obj::maskJson(old.getMaskNextOwner());
                    }
                    else
                    {
                        LLPointer<LLViewerInventoryItem> new_item = new LLViewerInventoryItem(item);
                        new_item->setPermissions(perm);
                        if (item->getType() == LLAssetType::AT_OBJECT)
                        {
                            // Apply the new next-owner bits when the object is next rezzed.
                            new_item->setFlags(new_item->getFlags() | LLInventoryItemFlags::II_FLAGS_OBJECT_SLAM_PERM);
                        }
                        updates.push_back({ batch->items.size(), new_item,
                                            perm.getMaskNextOwner() & old.getMaskBase() });
                    }
                }
                batch->items.push_back(std::move(r));
            }

            batch->pending = updates.size();
            if (updates.empty())
            {
                boost::json::object o;
                o["items"] = batch->items;
                idmcp_tool_ok(call, o);
                return;
            }
            for (const Update& u : updates)
            {
                // AIS has already applied the server's copy of the item when this runs.
                LLPointer<LLInventoryCallback> cb = new LLBoostFuncInventoryCallback(
                    [batch, index = u.index, id = u.item->getUUID(), expect = u.expect](const LLUUID&)
                    {
                        boost::json::object& r = batch->items[index].as_object();
                        const LLViewerInventoryItem* now = gInventory.getItem(id);
                        const U32 got = now ? now->getPermissions().getMaskNextOwner() : 0;
                        r["result"] = (now && (got & PERM_CMT) == (expect & PERM_CMT)) ? "set" : "failed";
                        if (now) r["next_owner"] = idmcp_obj::maskJson(got);
                        if (--batch->pending == 0)
                        {
                            boost::json::object o;
                            o["items"] = batch->items;
                            idmcp_tool_ok(batch->call, o);
                        }
                    });
                update_inventory_item(u.item.get(), cb);
            }
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

    // inventory.copy ---------------------------------------------------------
    {
        IDMCPTool t;
        t.name = "inventory.copy";
        t.description =
            "Copy an item or folder into another folder, as Copy and Paste would. {\"id\"}* (item or "
            "folder, Library included) and {\"dest\"}* (folder UUID or \"root\"); {\"name\"} renames an "
            "item's copy. No-copy items are refused, and left behind when copying a folder. Returns "
            "{item_id} or {folder_id, skipped:[{item_id, name}]}; a folder's contents finish copying "
            "shortly after the answer. Copying into an RLV-locked folder is refused.";
        t.input_schema = boost::json::parse(
            R"({"type":"object","properties":{"id":{"type":"string"},"dest":{"type":"string"},"name":{"type":"string"}},)"
            R"("required":["id","dest"],"additionalProperties":false})");
        t.gate = [](const boost::json::object& args, IDMCPGatePhase) -> IDMCPGateResult
        {
            if (!IDMCPRlvGate::isEnabled()) return IDMCPGateResult();
            const LLUUID dest = resolve_parent(arg_str(args, "dest"));
            if (dest.notNull() &&
                RlvFolderLocks::instance().isLockedFolder(dest, RLV_LOCK_ANY,
                                                          RlvFolderLocks::ST_MASK_ANY & ~RlvFolderLocks::ST_ROOTFOLDER))
            {
                return deny("locked-folder");
            }
            return IDMCPGateResult();
        };
        t.invoke = [](const boost::json::object& args, const IDMCPCallPtr& call)
        {
            const LLUUID id   = arg_uuid(args, "id");
            const LLUUID dest = resolve_parent(arg_str(args, "dest"));
            if (id.isNull() || dest.isNull())
            {
                idmcp_tool_err(call, IDMCP_ERR_INVALID_PARAMS, "id (UUID) and dest (folder UUID or \"root\") required");
                return;
            }
            if (!gInventory.getCategory(dest) || in_library(dest))
            {
                idmcp_tool_err(call, IDMCP_ERR_NOT_FOUND, "dest must be a folder in your inventory");
                return;
            }

            if (LLViewerInventoryItem* item = gInventory.getItem(id))
            {
                LLPointer<LLInventoryCallback> cb = new LLBoostFuncInventoryCallback([call](const LLUUID& new_id)
                {
                    boost::json::object o;
                    o["accepted"] = true;
                    o["item_id"]  = new_id.asString();
                    idmcp_tool_ok(call, o);
                });
                if (item->getIsLinkType())
                {
                    link_inventory_object(dest, item->getLinkedUUID(), cb);
                    return;
                }
                if (!item->getPermissions().allowCopyBy(gAgent.getID()))
                {
                    idmcp_tool_err(call, IDMCP_ERR_PERMISSION, "item is no-copy; use inventory.move instead");
                    return;
                }
                copy_inventory_item(gAgent.getID(), item->getPermissions().getOwner(), item->getUUID(), dest,
                                    arg_str(args, "name"), cb);
                return;
            }

            LLViewerInventoryCategory* cat = gInventory.getCategory(id);
            if (!cat)
            {
                idmcp_tool_err(call, IDMCP_ERR_NOT_FOUND, "no such item or folder");
                return;
            }
            if (id == dest || gInventory.isObjectDescendentOf(dest, id))
            {
                idmcp_tool_err(call, IDMCP_ERR_INVALID_PARAMS, "can't copy a folder into itself");
                return;
            }
            if (!tree_loaded(id))
            {
                LLInventoryModelBackgroundFetch::instance().start(id, true);
                idmcp_tool_err(call, IDMCP_ERR_CAP_UNAVAIL, "the folder hasn't finished loading; try again in a few seconds");
                return;
            }
            // The same rule copy_inventory_category_content uses to leave an item behind.
            boost::json::array skipped;
            LLInventoryModel::cat_array_t  sub_cats;
            LLInventoryModel::item_array_t sub_items;
            gInventory.collectDescendents(id, sub_cats, sub_items, LLInventoryModel::EXCLUDE_TRASH);
            for (const LLPointer<LLViewerInventoryItem>& sub : sub_items)
            {
                if (!sub->getIsLinkType() &&
                    !sub->getPermissions().allowOperationBy(PERM_COPY, gAgent.getID(), gAgent.getGroupID()))
                {
                    boost::json::object s;
                    s["item_id"] = sub->getUUID().asString();
                    s["name"]    = sub->getName();
                    skipped.push_back(std::move(s));
                }
            }
            copy_inventory_category(&gInventory, cat, dest, LLUUID::null, false,
                [call, skipped](const LLUUID& new_id)
                {
                    boost::json::object o;
                    o["accepted"]  = true;
                    o["folder_id"] = new_id.asString();
                    o["skipped"]   = skipped;
                    idmcp_tool_ok(call, o);
                });
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
