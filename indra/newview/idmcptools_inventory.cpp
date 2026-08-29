/**
 * @file idmcptools_inventory.cpp
 * @brief <ID> MCP server: inventory browse/search tools.
 *
 * Part of Five's custom Firestorm fork.
 *
 * All tools are RLV-gated on @showinv. Browsing an unfetched folder defers via
 * callAfterCategoryFetch; getItem defers via a self-cleaning fetch observer.
 * Search operates over already-loaded inventory.
 */

#include "llviewerprecompiledheaders.h"

#include "idmcptools.h"
#include "idmcpserver.h"
#include "idmcprlvgate.h"

#include "llinventorymodel.h"
#include "llinventoryfunctions.h"
#include "llinventoryobserver.h"
#include "llviewerinventory.h"
#include "llappearancemgr.h"

#include "llagent.h"
#include "llviewerregion.h"
#include "llassetstorage.h"      // gAssetStorage, getInvItemAsset
#include "llhost.h"              // LLHost
#include "llfilesystem.h"        // LLFileSystem
#include "llnotecard.h"
#include "llviewerassetupload.h" // LLBufferedAssetUploadInfo, LLScriptAssetUpload

#include "llassettype.h"
#include "llfoldertype.h"
#include "llinventorytype.h"     // IT_NOTECARD / IT_LSL
#include "llpermissionsflags.h"  // PERM_*
#include "llfloaterperms.h"      // LLFloaterPerms::getNextOwnerPerms (new-item defaults)
#include "lluuid.h"              // LLUUID, LLTransactionID

#include <algorithm>
#include <cctype>
#include <sstream>
#include <vector>

namespace
{
    std::string lower(std::string s)
    {
        std::transform(s.begin(), s.end(), s.begin(),
                       [](unsigned char c){ return (char)std::tolower(c); });
        return s;
    }

    bool looks_like_uuid(const std::string& s)
    {
        return s.size() == 36 && s[8] == '-' && s[13] == '-' && s[18] == '-' && s[23] == '-';
    }

    // Parse a UUID-or-well-known-name into a folder id (null if unresolvable).
    LLUUID resolve_folder(const std::string& s)
    {
        if (looks_like_uuid(s))
        {
            return LLUUID(s);
        }
        const std::string k = lower(s);
        if (k == "root" || k == "my_inventory")   return gInventory.getRootFolderID();
        if (k == "cof" || k == "current_outfit")  return LLAppearanceMgr::instance().getCOF();
        if (k == "outfits" || k == "my_outfits")  return gInventory.findCategoryUUIDForType(LLFolderType::FT_MY_OUTFITS);
        if (k == "trash")                         return gInventory.findCategoryUUIDForType(LLFolderType::FT_TRASH);
        return LLUUID::null;
    }

    boost::json::object item_to_json(LLViewerInventoryItem* it)
    {
        boost::json::object o;
        o["id"]         = it->getUUID().asString();
        o["name"]       = it->getName();
        o["asset_type"] = LLAssetType::lookupHumanReadable(it->getType());
        o["is_link"]    = it->getIsLinkType();
        if (it->getIsLinkType())
        {
            o["linked_id"] = it->getLinkedUUID().asString();
        }
        o["worn"] = get_is_item_worn(it);
        return o;
    }

    boost::json::object cat_to_json(LLViewerInventoryCategory* cat)
    {
        boost::json::object o;
        o["id"]          = cat->getUUID().asString();
        o["name"]        = cat->getName();
        o["folder_type"] = LLFolderType::lookup(cat->getPreferredType());
        o["is_folder"]   = true;
        return o;
    }

    boost::json::value build_folder_listing(const LLUUID& folder)
    {
        boost::json::object out;
        out["folder_id"] = folder.asString();
        LLViewerInventoryCategory* fcat = gInventory.getCategory(folder);
        out["name"] = fcat ? fcat->getName() : std::string();

        LLInventoryModel::cat_array_t*  cats  = nullptr;
        LLInventoryModel::item_array_t* items = nullptr;
        gInventory.getDirectDescendentsOf(folder, cats, items);

        boost::json::array jcats;
        if (cats)
        {
            for (auto& c : *cats)  jcats.push_back(cat_to_json(c.get()));
        }
        boost::json::array jitems;
        if (items)
        {
            for (auto& i : *items) jitems.push_back(item_to_json(i.get()));
        }
        out["categories"] = std::move(jcats);
        out["items"]      = std::move(jitems);
        return out;
    }

    IDMCPGateResult gate_showinv(const boost::json::object&, IDMCPGatePhase)
    {
        return IDMCPRlvGate::checkBehaviour(RLV_BHVR_SHOWINV, "showinv");
    }

    // Self-cleaning single-item fetch: responds on done(), removes itself from
    // the model and deletes. The base class self-completes on its own expiry
    // timer even if the item never arrives, so there is no permanent leak.
    class IDMCPItemFetch : public LLInventoryFetchItemsObserver
    {
    public:
        IDMCPItemFetch(const uuid_vec_t& ids, IDMCPCallPtr call, LLUUID id)
            : LLInventoryFetchItemsObserver(ids), mCall(std::move(call)), mId(id) {}

        void done() override
        {
            gInventory.removeObserver(this);
            LLViewerInventoryItem* item = gInventory.getItem(mId);
            if (item)
            {
                idmcp_tool_ok(mCall, item_to_json(item));
            }
            else
            {
                idmcp_tool_err(mCall, IDMCP_ERR_NOT_FOUND, "item not found");
            }
            delete this;
        }

    private:
        IDMCPCallPtr mCall;
        LLUUID       mId;
    };

    // -----------------------------------------------------------------------
    // Search: name-substring over loaded inventory.

    class IDMCPSearchCollector : public LLInventoryCollectFunctor
    {
    public:
        explicit IDMCPSearchCollector(std::string name_lower) : mName(std::move(name_lower)) {}
        bool operator()(LLInventoryCategory* cat, LLInventoryItem* item) override
        {
            if (mName.empty())
            {
                return true;
            }
            const std::string n = item ? item->getName() : (cat ? cat->getName() : std::string());
            return lower(n).find(mName) != std::string::npos;
        }
    private:
        std::string mName;
    };

    // -----------------------------------------------------------------------
    // Notecard / LSL script content: asset download (read) + cap upload (write).

    std::string arg_str(const boost::json::object& args, const char* key)
    {
        auto it = args.find(key);
        return (it != args.end() && it->value().is_string())
                   ? std::string(it->value().as_string().c_str()) : std::string();
    }

    IDMCPGateResult gate_viewnote(const boost::json::object&, IDMCPGatePhase)
    {
        return IDMCPRlvGate::checkBehaviour(RLV_BHVR_VIEWNOTE, "viewnote");
    }

    IDMCPGateResult gate_viewscript(const boost::json::object&, IDMCPGatePhase)
    {
        return IDMCPRlvGate::checkBehaviour(RLV_BHVR_VIEWSCRIPT, "viewscript");
    }

    std::string strip_trailing_nulls(std::string s)
    {
        while (!s.empty() && s.back() == '\0') s.pop_back();
        return s;
    }

    // Download the item's asset and respond. For agent-inventory mutable assets
    // (scripts, notecards) the local item's asset id is legitimately null — the
    // asset changes on every save and is resolved server-side by *item id*. So we
    // pass whatever asset id we have (often null) straight through to
    // getInvItemAsset; the transfer resolves by item id and the completion
    // callback hands back the real asset uuid to read from cache. This mirrors
    // LLPreviewLSL::loadAsset, which likewise never guards on a null asset id.
    void idmcp_download_asset(const LLUUID& item_id, bool is_notecard, const IDMCPCallPtr& call)
    {
        LLViewerInventoryItem* item = gInventory.getItem(item_id);
        if (!item)
        {
            idmcp_tool_err(call, IDMCP_ERR_NOT_FOUND, "no such item");
            return;
        }
        const LLUUID asset_id = item->getAssetUUID();
        const LLAssetType::EType expect = is_notecard ? LLAssetType::AT_NOTECARD : LLAssetType::AT_LSL_TEXT;

        gAssetStorage->getInvItemAsset(
            LLHost(), gAgentID, gAgentSessionID, gAgentID, LLUUID::null,
            item_id, asset_id, expect,
            [call, is_notecard](const LLUUID& asset_uuid, LLAssetType::EType type,
                                void*, S32 status, LLExtStat)
            {
                if (status != 0)
                {
                    idmcp_tool_err(call, IDMCP_ERR_NOT_FOUND, "asset download failed");
                    return;
                }
                LLFileSystem file(asset_uuid, type, LLFileSystem::READ);
                const S32 len = file.getSize();
                std::vector<char> buffer(len + 1, 0);
                if (len > 0)
                {
                    file.read((U8*)buffer.data(), len);
                }
                buffer[len] = 0;

                boost::json::object o;
                if (is_notecard)
                {
                    LLNotecard nc;
                    std::istringstream iss(std::string(buffer.data(), len));
                    if (nc.importStream(iss))
                    {
                        o["text"]               = strip_trailing_nulls(nc.getText());
                        o["has_embedded_items"] = !nc.getItems().empty();
                    }
                    else
                    {
                        o["text"]               = strip_trailing_nulls(std::string(buffer.data(), len));
                        o["has_embedded_items"] = false;
                    }
                }
                else
                {
                    o["text"] = strip_trailing_nulls(std::string(buffer.data(), len));   // LSL is plain text
                }
                idmcp_tool_ok(call, o);
            },
            nullptr, true);
    }

    // Explicit inventory refresh: force a fresh server fetch of an item so a
    // subsequent read/operation sees the latest server state. Self-deletes.
    class IDMCPItemRefresh : public LLInventoryFetchItemsObserver
    {
    public:
        IDMCPItemRefresh(const uuid_vec_t& ids, IDMCPCallPtr call, LLUUID id)
            : LLInventoryFetchItemsObserver(ids), mCall(std::move(call)), mId(id) {}

        void done() override
        {
            gInventory.removeObserver(this);
            LLViewerInventoryItem* item = gInventory.getItem(mId);
            boost::json::object o;
            o["refreshed"] = true;
            o["has_asset"] = (item && item->getAssetUUID().notNull());
            idmcp_tool_ok(mCall, o);
            delete this;
        }

    private:
        IDMCPCallPtr mCall;
        LLUUID       mId;
    };

    // Deferred: resolve the item, then download its asset. The download resolves
    // by item id server-side, so a null local asset id (unfetched, or just
    // written) is fine — no refetch needed.
    void idmcp_read_asset(bool is_notecard, const boost::json::object& args, const IDMCPCallPtr& call)
    {
        const std::string spec = arg_str(args, "item_id");
        if (!looks_like_uuid(spec))
        {
            idmcp_tool_err(call, IDMCP_ERR_INVALID_PARAMS, "item_id must be a UUID");
            return;
        }
        LLUUID item_id(spec);
        LLViewerInventoryItem* item = gInventory.getItem(item_id);
        if (!item)
        {
            idmcp_tool_err(call, IDMCP_ERR_NOT_FOUND, "no such item");
            return;
        }
        const LLAssetType::EType expect = is_notecard ? LLAssetType::AT_NOTECARD : LLAssetType::AT_LSL_TEXT;
        if (item->getType() != expect)
        {
            idmcp_tool_err(call, IDMCP_ERR_INVALID_PARAMS,
                           is_notecard ? "item is not a notecard" : "item is not a script");
            return;
        }

        idmcp_download_asset(item_id, is_notecard, call);
    }

    // Point the local inventory item at the freshly-uploaded asset so a read
    // right after a write sees the new content. The upload path doesn't reliably
    // refresh the local model (notably for scripts), so do it explicitly.
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

    void idmcp_write_notecard(const boost::json::object& args, const IDMCPCallPtr& call)
    {
        const std::string spec = arg_str(args, "item_id");
        if (!looks_like_uuid(spec))
        {
            idmcp_tool_err(call, IDMCP_ERR_INVALID_PARAMS, "item_id must be a UUID");
            return;
        }
        LLUUID item_id(spec);
        LLViewerInventoryItem* item = gInventory.getItem(item_id);
        if (!item || item->getType() != LLAssetType::AT_NOTECARD)
        {
            idmcp_tool_err(call, IDMCP_ERR_NOT_FOUND, "no such notecard");
            return;
        }
        LLViewerRegion* region = gAgent.getRegion();
        const std::string url = region ? region->getCapability("UpdateNotecardAgentInventory") : std::string();
        if (url.empty())
        {
            idmcp_tool_err(call, IDMCP_ERR_CAP_UNAVAIL, "UpdateNotecardAgentInventory capability unavailable");
            return;
        }

        LLNotecard nc;
        nc.setText(arg_str(args, "text"));
        std::ostringstream out;
        nc.exportStream(out);

        LLResourceUploadInfo::ptr_t info = std::make_shared<LLBufferedAssetUploadInfo>(
            item_id, LLAssetType::AT_NOTECARD, out.str(),
            [call](LLUUID itemId, LLUUID newAssetId, LLUUID, LLSD)
            {
                idmcp_set_item_asset(itemId, newAssetId);
                boost::json::object o;
                o["accepted"]     = true;
                o["new_asset_id"] = newAssetId.asString();
                idmcp_tool_ok(call, o);
            },
            [call](LLUUID, LLUUID, LLSD, std::string reason) -> bool
            {
                idmcp_tool_err(call, IDMCP_ERR_PERMISSION, "notecard upload failed: " + reason);
                return false;
            });
        LLViewerAssetUpload::EnqueueInventoryUpload(url, info);
    }

    void idmcp_write_script(const boost::json::object& args, const IDMCPCallPtr& call)
    {
        const std::string spec = arg_str(args, "item_id");
        if (!looks_like_uuid(spec))
        {
            idmcp_tool_err(call, IDMCP_ERR_INVALID_PARAMS, "item_id must be a UUID");
            return;
        }
        LLUUID item_id(spec);
        LLViewerInventoryItem* item = gInventory.getItem(item_id);
        if (!item || item->getType() != LLAssetType::AT_LSL_TEXT)
        {
            idmcp_tool_err(call, IDMCP_ERR_NOT_FOUND, "no such script");
            return;
        }
        LLViewerRegion* region = gAgent.getRegion();
        const std::string url = region ? region->getCapability("UpdateScriptAgent") : std::string();
        if (url.empty())
        {
            idmcp_tool_err(call, IDMCP_ERR_CAP_UNAVAIL, "UpdateScriptAgent capability unavailable");
            return;
        }

        std::string target = arg_str(args, "target");
        if (target.empty()) target = "mono";
        if (target != "lsl2" && target != "mono" && target != "luau" && target != "lsl-luau")
        {
            idmcp_tool_err(call, IDMCP_ERR_INVALID_PARAMS,
                           "target must be lsl2, mono, luau, or lsl-luau");
            return;
        }

        LLResourceUploadInfo::ptr_t info = std::make_shared<LLScriptAssetUpload>(
            item_id, target, arg_str(args, "text"),
            [call](LLUUID itemId, LLUUID newAssetId, LLUUID, LLSD response)
            {
                idmcp_set_item_asset(itemId, newAssetId);
                boost::json::object o;
                o["accepted"]     = true;
                o["new_asset_id"] = newAssetId.asString();
                if (response.has("compiled"))
                {
                    o["compiled"] = response["compiled"].asBoolean();
                }
                if (response.has("errors") && response["errors"].isArray())
                {
                    boost::json::array errs;
                    for (LLSD::array_const_iterator eit = response["errors"].beginArray();
                         eit != response["errors"].endArray(); ++eit)
                    {
                        boost::json::value ev;
                        ev = eit->asString();   // std::string -> value via operator=
                        errs.push_back(std::move(ev));
                    }
                    o["errors"] = std::move(errs);
                }
                idmcp_tool_ok(call, o);
            },
            [call](LLUUID, LLUUID, LLSD, std::string reason) -> bool
            {
                idmcp_tool_err(call, IDMCP_ERR_PERMISSION, "script upload failed: " + reason);
                return false;
            });
        LLViewerAssetUpload::EnqueueInventoryUpload(url, info);
    }
}

// ---------------------------------------------------------------------------

void idmcp_register_inventory_tools(IDMCPToolRegistry& reg)
{
    // inventory.getFolder ----------------------------------------------------
    {
        IDMCPTool t;
        t.name = "inventory.getFolder";
        t.description =
            "List the direct contents of an inventory folder. {\"folder\"} is a "
            "folder UUID or a well-known name: \"root\", \"cof\", \"outfits\", "
            "\"trash\". Fetches the folder if needed. Blocked by RLV @showinv.";
        t.input_schema = boost::json::parse(
            R"({"type":"object","properties":{"folder":{"type":"string"}},"required":["folder"],"additionalProperties":false})");
        t.gate = gate_showinv;
        t.invoke = [](const boost::json::object& args, const IDMCPCallPtr& call)
        {
            auto it = args.find("folder");
            std::string spec = (it != args.end() && it->value().is_string())
                                   ? std::string(it->value().as_string().c_str()) : std::string();
            LLUUID folder = resolve_folder(spec);
            if (folder.isNull())
            {
                idmcp_tool_err(call, IDMCP_ERR_NOT_FOUND, "unknown folder: " + spec);
                return;
            }
            callAfterCategoryFetch(folder, [call, folder]()
            {
                if (!gInventory.getCategory(folder))
                {
                    idmcp_tool_err(call, IDMCP_ERR_NOT_FOUND, "folder not found");
                    return;
                }
                idmcp_tool_ok(call, build_folder_listing(folder));
            });
        };
        reg.add(std::move(t));
    }

    // inventory.getItem ------------------------------------------------------
    {
        IDMCPTool t;
        t.name = "inventory.getItem";
        t.description =
            "Get details for one inventory item by {\"item_id\"} (UUID). Fetches "
            "it if not yet loaded. Blocked by RLV @showinv.";
        t.input_schema = boost::json::parse(
            R"({"type":"object","properties":{"item_id":{"type":"string"}},"required":["item_id"],"additionalProperties":false})");
        t.gate = gate_showinv;
        t.invoke = [](const boost::json::object& args, const IDMCPCallPtr& call)
        {
            auto it = args.find("item_id");
            std::string spec = (it != args.end() && it->value().is_string())
                                   ? std::string(it->value().as_string().c_str()) : std::string();
            if (!looks_like_uuid(spec))
            {
                idmcp_tool_err(call, IDMCP_ERR_INVALID_PARAMS, "item_id must be a UUID");
                return;
            }
            LLUUID id(spec);
            if (LLViewerInventoryItem* item = gInventory.getItem(id))
            {
                idmcp_tool_ok(call, item_to_json(item));
                return;
            }
            // Deferred fetch.
            uuid_vec_t ids{ id };
            IDMCPItemFetch* obs = new IDMCPItemFetch(ids, call, id);
            obs->startFetch();
            if (obs->isFinished())
            {
                LLViewerInventoryItem* item = gInventory.getItem(id);
                if (item) idmcp_tool_ok(call, item_to_json(item));
                else      idmcp_tool_err(call, IDMCP_ERR_NOT_FOUND, "item not found");
                delete obs;      // never added to the model
            }
            else
            {
                gInventory.addObserver(obs);   // done() removes + deletes
            }
        };
        reg.add(std::move(t));
    }

    // inventory.search -------------------------------------------------------
    {
        IDMCPTool t;
        t.name = "inventory.search";
        t.description =
            "Search loaded inventory by name substring. {\"name\"} required; "
            "optional {\"limit\"} (default 100, max 500). Blocked by RLV @showinv.";
        t.input_schema = boost::json::parse(
            R"({"type":"object","properties":{"name":{"type":"string"},"limit":{"type":"integer"}},"required":["name"],"additionalProperties":false})");
        t.gate = gate_showinv;
        t.invoke = [](const boost::json::object& args, const IDMCPCallPtr& call)
        {
            auto nit = args.find("name");
            std::string name = (nit != args.end() && nit->value().is_string())
                                   ? std::string(nit->value().as_string().c_str()) : std::string();
            if (name.empty())
            {
                idmcp_tool_err(call, IDMCP_ERR_INVALID_PARAMS, "name required");
                return;
            }
            std::size_t limit = 100;
            auto lit = args.find("limit");
            if (lit != args.end() && lit->value().is_int64())
            {
                long v = (long)lit->value().as_int64();
                if (v > 0) limit = std::min<std::size_t>((std::size_t)v, 500);
            }

            IDMCPSearchCollector collector(lower(name));
            LLInventoryModel::cat_array_t  cats;
            LLInventoryModel::item_array_t items;
            gInventory.collectDescendentsIf(gInventory.getRootFolderID(),
                                            cats, items, /*include_trash*/ false, collector);

            boost::json::array jitems;
            for (auto& i : items)
            {
                if (jitems.size() >= limit) break;
                jitems.push_back(item_to_json(i.get()));
            }
            boost::json::array jcats;
            for (auto& c : cats)
            {
                if (jcats.size() >= limit) break;
                jcats.push_back(cat_to_json(c.get()));
            }

            boost::json::object out;
            out["items"]      = std::move(jitems);
            out["categories"] = std::move(jcats);
            out["truncated"]  = (items.size() > limit || cats.size() > limit);
            idmcp_tool_ok(call, out);
        };
        reg.add(std::move(t));
    }

    // notecard.read ----------------------------------------------------------
    {
        IDMCPTool t;
        t.name = "notecard.read";
        t.description =
            "Read a notecard's text by {\"item_id\"} (UUID). Returns {text, "
            "has_embedded_items}. Blocked by RLV @viewnote.";
        t.input_schema = boost::json::parse(
            R"({"type":"object","properties":{"item_id":{"type":"string"}},"required":["item_id"],"additionalProperties":false})");
        t.gate = gate_viewnote;
        t.invoke = [](const boost::json::object& args, const IDMCPCallPtr& call)
        {
            idmcp_read_asset(/*is_notecard*/ true, args, call);
        };
        reg.add(std::move(t));
    }

    // notecard.write ---------------------------------------------------------
    {
        IDMCPTool t;
        t.name = "notecard.write";
        t.description =
            "Replace a notecard's text. {\"item_id\"} (UUID) and {\"text\"}. The "
            "notecard must be modifiable. Blocked by RLV @viewnote.";
        t.input_schema = boost::json::parse(
            R"({"type":"object","properties":{"item_id":{"type":"string"},"text":{"type":"string"}},"required":["item_id","text"],"additionalProperties":false})");
        t.gate = gate_viewnote;
        t.invoke = [](const boost::json::object& args, const IDMCPCallPtr& call)
        {
            idmcp_write_notecard(args, call);
        };
        reg.add(std::move(t));
    }

    // script.read ------------------------------------------------------------
    {
        IDMCPTool t;
        t.name = "script.read";
        t.description =
            "Read an LSL script's source by {\"item_id\"} (UUID). Returns {text}. "
            "Blocked by RLV @viewscript.";
        t.input_schema = boost::json::parse(
            R"({"type":"object","properties":{"item_id":{"type":"string"}},"required":["item_id"],"additionalProperties":false})");
        t.gate = gate_viewscript;
        t.invoke = [](const boost::json::object& args, const IDMCPCallPtr& call)
        {
            idmcp_read_asset(/*is_notecard*/ false, args, call);
        };
        reg.add(std::move(t));
    }

    // script.write -----------------------------------------------------------
    {
        IDMCPTool t;
        t.name = "script.write";
        t.description =
            "Replace a script's source and recompile. {\"item_id\"} (UUID), "
            "{\"text\"}, and optional {\"target\"} compiler: \"mono\" (default) and "
            "\"lsl2\" take LSL source; \"lsl-luau\" takes LSL source and compiles "
            "it to Luau; \"luau\" takes Luau source. lsl-luau/luau require region "
            "Luau support. Returns {compiled, errors?}. Blocked by RLV @viewscript.";
        t.input_schema = boost::json::parse(
            R"({"type":"object","properties":{"item_id":{"type":"string"},"text":{"type":"string"},"target":{"type":"string","enum":["mono","lsl2","luau","lsl-luau"]}},"required":["item_id","text"],"additionalProperties":false})");
        t.gate = gate_viewscript;
        t.invoke = [](const boost::json::object& args, const IDMCPCallPtr& call)
        {
            idmcp_write_script(args, call);
        };
        reg.add(std::move(t));
    }

    // inventory.createItem ---------------------------------------------------
    {
        IDMCPTool t;
        t.name = "inventory.createItem";
        t.description =
            "Create a new empty notecard or script. {\"parent\"} (folder UUID or "
            "\"root\"), {\"name\"}, {\"type\"} \"notecard\" or \"script\". Returns "
            "the new item id; add content with notecard.write / script.write.";
        t.input_schema = boost::json::parse(
            R"({"type":"object","properties":{"parent":{"type":"string"},"name":{"type":"string"},"type":{"type":"string","enum":["notecard","script"]}},"required":["parent","name","type"],"additionalProperties":false})");
        t.invoke = [](const boost::json::object& args, const IDMCPCallPtr& call)
        {
            const LLUUID parent = resolve_folder(arg_str(args, "parent"));
            const std::string name = arg_str(args, "name");
            const std::string type = arg_str(args, "type");
            if (parent.isNull() || name.empty())
            {
                idmcp_tool_err(call, IDMCP_ERR_INVALID_PARAMS, "parent and name required");
                return;
            }
            LLAssetType::EType at;
            LLInventoryType::EType it;
            // Next-owner perms as the viewer's own "New Notecard/Script" uses
            // (the user's configured defaults) — the previous MOVE|TRANSFER omitted
            // Copy, so a recipient couldn't open/read the item.
            U32 next_owner_perm;
            if (type == "notecard")   { at = LLAssetType::AT_NOTECARD;  it = LLInventoryType::IT_NOTECARD; next_owner_perm = LLFloaterPerms::getNextOwnerPerms("Notecards"); }
            else if (type == "script"){ at = LLAssetType::AT_LSL_TEXT;  it = LLInventoryType::IT_LSL;      next_owner_perm = LLFloaterPerms::getNextOwnerPerms("Scripts"); }
            else
            {
                idmcp_tool_err(call, IDMCP_ERR_INVALID_PARAMS, "type must be notecard or script");
                return;
            }

            create_inventory_item(
                gAgentID, gAgentSessionID, parent, LLTransactionID::tnull,
                name, name, at, it, NO_INV_SUBTYPE, next_owner_perm,
                new LLBoostFuncInventoryCallback([call](const LLUUID& new_id)
                {
                    boost::json::object o;
                    o["accepted"] = true;
                    o["item_id"]  = new_id.asString();
                    idmcp_tool_ok(call, o);
                }));
        };
        reg.add(std::move(t));
    }

    // inventory.refresh ------------------------------------------------------
    {
        IDMCPTool t;
        t.name = "inventory.refresh";
        t.description =
            "Force a fresh server fetch of an inventory item's metadata "
            "({\"item_id\"}) — e.g. to pick up a rename/move made elsewhere. "
            "Returns {refreshed, has_asset}. Note: scripts and notecards carry no "
            "local asset id (it's resolved server-side on read), so has_asset is "
            "false for them even though they are readable.";
        t.input_schema = boost::json::parse(
            R"({"type":"object","properties":{"item_id":{"type":"string"}},"required":["item_id"],"additionalProperties":false})");
        t.invoke = [](const boost::json::object& args, const IDMCPCallPtr& call)
        {
            const std::string spec = arg_str(args, "item_id");
            if (!looks_like_uuid(spec))
            {
                idmcp_tool_err(call, IDMCP_ERR_INVALID_PARAMS, "item_id must be a UUID");
                return;
            }
            LLUUID id(spec);
            LLViewerInventoryItem* item = gInventory.getItem(id);
            if (!item)
            {
                idmcp_tool_err(call, IDMCP_ERR_NOT_FOUND, "no such item");
                return;
            }
            item->setComplete(false);
            uuid_vec_t ids{ id };
            IDMCPItemRefresh* obs = new IDMCPItemRefresh(ids, call, id);
            obs->startFetch();
            if (obs->isFinished()) obs->done();
            else gInventory.addObserver(obs);
        };
        reg.add(std::move(t));
    }
}
