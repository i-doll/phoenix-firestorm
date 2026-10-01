/**
 * @file idmcptools_contents.cpp
 * @brief <ID> MCP server: object.contents and object.addContents (task inventory).
 *
 * Part of Five's custom Firestorm fork. Custom code carries an `ID` prefix.
 *
 * Adding follows dropInventory/dropScript without calling them: they return
 * silently while the user is mid-drag, and dropInventory switches an open
 * Build floater to its Contents tab. The drop rules come from the public
 * isInventoryDropAcceptable. Main thread only; never touches the selection.
 */

#include "llviewerprecompiledheaders.h"

#include "idmcptools_object.h"
#include "idmcptools.h"
#include "idmcpserver.h"
#include "idmcprlvgate.h"

#include "llagent.h"
#include "llinventorydefines.h"     // TASK_INVENTORY_ITEM_KEY
#include "llinventorymodel.h"       // gInventory
#include "lltimer.h"                // time_corrected
#include "lltooldraganddrop.h"      // isInventoryDropAcceptable (public static)
#include "llviewerinventory.h"
#include "llviewerobject.h"
#include "llviewerobjectlist.h"     // gObjectList
#include "rlvactions.h"
#include "rlvdefines.h"
#include "rlvlocks.h"               // gRlvAttachmentLocks

#include <algorithm>
#include <map>
#include <set>
#include <vector>

using namespace idmcp_obj;

namespace
{
    constexpr F64    LIST_LIMIT   = 15.0;   // spec: 15 s
    constexpr F64    ADD_LIMIT    = 45.0;
    constexpr F64    VERIFY_LIMIT = 20.0;
    constexpr F64    REFETCH_GAP  = 2.0;
    constexpr size_t MAX_ITEMS    = 32;

    // As edit, plus willObjectAcceptInventory's RLVa rules, reported as RLV errors.
    IDMCPGateResult rlv_add_contents(LLViewerObject* root)
    {
        IDMCPGateResult g = rlvEdit(root);
        if (!g.allowed || !IDMCPRlvGate::isEnabled() || !root) return g;
        if (gRlvAttachmentLocks.isLockedAttachment(root))
        {
            return IDMCPRlvGate::deny(RLV_BHVR_DETACH, "detach");
        }
        if (agentSittingOn(root))
        {
            if (RlvActions::hasBehaviour(RLV_BHVR_UNSIT)) return IDMCPRlvGate::deny(RLV_BHVR_UNSIT, "unsit");
            if (RlvActions::hasBehaviour(RLV_BHVR_SITTP)) return IDMCPRlvGate::deny(RLV_BHVR_SITTP, "sittp");
        }
        return g;
    }

    IDMCPGateResult gate_add_contents(const boost::json::object& args, IDMCPGatePhase)
    {
        return rlv_add_contents(findRoot(args));
    }

    // ---- object.contents ---------------------------------------------------------

    class ListJob : public Job
    {
    public:
        ListJob(const IDMCPCallPtr& call, const Target& t)
            : Job(call, LIST_LIMIT), mRootId(t.root->getID()), mPrimId(t.prim->getID()), mLink(t.link) {}

        void begin()
        {
            if (LLViewerObject* p = gObjectList.findObject(mPrimId)) mFetch.start(p);
        }

    protected:
        void tick(F64) override
        {
            LLViewerObject* p = gObjectList.findObject(mPrimId);
            if (!p || p->isDead())
            {
                fail(IDMCP_ERR_NOT_FOUND, "the object is gone (deleted, taken or out of range)");
                return;
            }
            if (!mFetch.done()) return;
            if (mFetch.failed())
            {
                fail(IDMCP_ERR_CAP_UNAVAIL, "the region couldn't send the object's contents; try again");
                return;
            }
            boost::json::array items;
            for (const LLPointer<LLViewerInventoryItem>& item : mFetch.items())
            {
                items.push_back(itemJson(item.get()));
            }
            boost::json::object out;
            out["object_id"] = mRootId.asString();
            if (mLink) out["link"] = mLink;
            out["items"] = std::move(items);
            succeed(std::move(out));
        }

        void teardown() override { mFetch.stop(); }

        std::string timeoutMessage() const override { return "the object's contents didn't arrive in time; try again"; }

    private:
        LLUUID        mRootId;
        LLUUID        mPrimId;
        S32           mLink = 0;
        ContentsFetch mFetch;
    };

    void run_contents(const boost::json::object& args, const IDMCPCallPtr& call)
    {
        Target t;
        if (!resolve(args, call, t)) return;
        auto job = std::make_shared<ListJob>(call, t);
        Job::launch(job);
        job->begin();
    }

    // ---- object.addContents ------------------------------------------------------

    struct AddEntry
    {
        LLUUID             item_id;
        std::string        name;
        LLAssetType::EType type = LLAssetType::AT_NONE;
        std::string        result;   // "added", "not_found", or why it wasn't sent
        bool               sent = false;
    };

    std::map<S32, S32> count_by_type(const std::vector<LLPointer<LLViewerInventoryItem>>& items)
    {
        std::map<S32, S32> out;
        for (const LLPointer<LLViewerInventoryItem>& item : items)
        {
            ++out[(S32)item->getType()];
        }
        return out;
    }

    std::set<std::string> names_of(const std::vector<LLPointer<LLViewerInventoryItem>>& items)
    {
        std::set<std::string> out;
        for (const LLPointer<LLViewerInventoryItem>& item : items) out.insert(item->getName());
        return out;
    }

    class AddJob : public Job
    {
    public:
        AddJob(const IDMCPCallPtr& call, const Target& t, const std::vector<LLUUID>& items, bool running)
            : Job(call, ADD_LIMIT), mRootId(t.root->getID()), mPrimId(t.prim->getID()), mLink(t.link),
              mRunning(running)
        {
            for (const LLUUID& id : items)
            {
                AddEntry e;
                e.item_id = id;
                mEntries.push_back(e);
            }
        }

        void begin()
        {
            if (LLViewerObject* p = gObjectList.findObject(mPrimId)) mFetch.start(p);
        }

    protected:
        void tick(F64 now) override
        {
            LLViewerObject* prim = gObjectList.findObject(mPrimId);
            if (!prim || prim->isDead())
            {
                fail(IDMCP_ERR_NOT_FOUND, "the object is gone (deleted, taken or out of range)");
                return;
            }
            switch (mStep)
            {
            case Step::Baseline:
                if (!mFetch.done()) return;
                if (mFetch.failed())
                {
                    fail(IDMCP_ERR_CAP_UNAVAIL, "the region couldn't send the object's contents; try again");
                    return;
                }
                mBaseline = count_by_type(mFetch.items());
                mBaselineNames = names_of(mFetch.items());
                mFetch.stop();
                // Commit point: a restriction may have arrived while the contents loaded.
                if (failIfDenied(rlv_add_contents(prim->getRootEdit()))) return;
                mStep = Step::Adding;
                return;

            case Step::Adding:
                if (mNext < mEntries.size())
                {
                    const IDMCPGateResult g = rlv_add_contents(prim->getRootEdit());
                    if (!g.allowed)
                    {
                        if (!any_sent())
                        {
                            failIfDenied(g);
                            return;
                        }
                        // Some items are already out: report the rest as refused and verify.
                        for (; mNext < mEntries.size(); ++mNext)
                        {
                            mEntries[mNext].result = "refused: " + rlvErrorMessage(g);
                        }
                        return;
                    }
                    add_one(prim, mEntries[mNext++]);   // one item per frame, in order
                    return;
                }
                if (!any_sent())
                {
                    fail(IDMCP_ERR_INVALID_PARAMS, "none of the items could be added", result_data());
                    return;
                }
                mStep        = Step::Verifying;
                mVerifyUntil = std::min(now + VERIFY_LIMIT, mDeadline - 0.5);   // answer before the job deadline
                mFetchedAt   = now;
                mFetch.start(prim);
                return;

            case Step::Verifying:
                if (mFetch.done() && !mFetch.failed() && confirm(mFetch.items()))
                {
                    succeed(result_data());
                    return;
                }
                if (now >= mVerifyUntil)
                {
                    for (AddEntry& e : mEntries)
                    {
                        if (e.sent && e.result.empty()) e.result = "not_found";
                    }
                    succeed(result_data());
                    return;
                }
                if (mFetch.done() && now - mFetchedAt >= REFETCH_GAP)
                {
                    mFetchedAt = now;
                    mFetch.start(prim);
                }
                return;
            }
        }

        void teardown() override { mFetch.stop(); }

        std::string timeoutMessage() const override
        {
            if (mStep == Step::Baseline) return "the object's contents didn't arrive in time; try again";
            return "items were sent but the region didn't confirm them in time; check object.contents before retrying";
        }

        boost::json::object timeoutData() const override
        {
            return mStep == Step::Baseline ? boost::json::object() : result_data();
        }

    private:
        enum class Step { Baseline, Adding, Verifying };

        // The core of dropInventory/dropScript (lltooldraganddrop.cpp:1828-1885, 2064-2126).
        void add_one(LLViewerObject* prim, AddEntry& e)
        {
            LLViewerInventoryItem* item = agentItem(e.item_id);
            if (!item)
            {
                e.result = "not in your inventory";
                return;
            }
            e.name = item->getName();
            e.type = item->getType();
            if (inLibrary(item->getUUID()))
            {
                e.result = "Library items can't be added; copy the item to your inventory first";
                return;
            }
            if (!item->isFinished())
            {
                e.result = "the item hasn't finished loading; try again";
                return;
            }
            if (!LLToolDragAndDrop::isInventoryDropAcceptable(prim, item))
            {
                e.result = "refused: the object must be yours and modifiable or accept drops, the item must be "
                           "transferable or yours, and worn items can't be added";
                return;
            }
            LLPointer<LLViewerInventoryItem> copy = new LLViewerInventoryItem(item);
            const bool no_copy = !item->getPermissions().allowCopyBy(gAgent.getID());
            if (no_copy)
            {
                // The region moves a no-copy item; drop it locally the way a drag does.
                gInventory.deleteObject(item->getUUID());   // `item` is gone after this line
                gInventory.notifyObservers();
            }
            if (e.type == LLAssetType::AT_LSL_TEXT)
            {
                prim->saveScript(copy.get(), mRunning, true);
            }
            else
            {
                copy->setCreationDate(time_corrected());
                prim->updateInventory(copy.get(), TASK_INVENTORY_ITEM_KEY, true);
            }
            e.sent = true;
        }

        bool any_sent() const
        {
            return std::any_of(mEntries.begin(), mEntries.end(), [](const AddEntry& e) { return e.sent; });
        }

        // Exact name first: an entry whose name is new in the contents is added.
        // The rest are matched by asset type count, which covers a region-renamed duplicate.
        bool confirm(const std::vector<LLPointer<LLViewerInventoryItem>>& items)
        {
            const std::set<std::string> now_names = names_of(items);
            std::map<S32, S32>          fresh;
            for (const auto& kv : count_by_type(items))
            {
                auto b = mBaseline.find(kv.first);
                fresh[kv.first] = kv.second - (b == mBaseline.end() ? 0 : b->second);
            }
            std::vector<AddEntry*> rest;
            for (AddEntry& e : mEntries)
            {
                if (!e.sent) continue;
                if (now_names.count(e.name) && !mBaselineNames.count(e.name))
                {
                    e.result = "added";
                    S32& left = fresh[(S32)e.type];
                    if (left > 0) --left;
                }
                else
                {
                    rest.push_back(&e);
                }
            }
            bool all = true;
            for (AddEntry* e : rest)
            {
                S32& left = fresh[(S32)e->type];
                if (left > 0)
                {
                    e->result = "added";
                    --left;
                }
                else
                {
                    e->result.clear();
                    all = false;
                }
            }
            return all;
        }

        boost::json::object result_data() const
        {
            boost::json::array items;
            for (const AddEntry& e : mEntries)
            {
                boost::json::object r;
                r["item_id"] = e.item_id.asString();
                if (!e.name.empty()) r["name"] = e.name;
                r["result"] = e.result;
                items.push_back(std::move(r));
            }
            boost::json::object o;
            o["object_id"] = mRootId.asString();
            if (mLink) o["link"] = mLink;
            o["items"] = std::move(items);
            return o;
        }

        LLUUID                mRootId;
        LLUUID                mPrimId;
        S32                   mLink    = 0;
        bool                  mRunning = true;
        Step                  mStep    = Step::Baseline;
        std::vector<AddEntry> mEntries;
        size_t                mNext        = 0;
        std::map<S32, S32>    mBaseline;
        std::set<std::string> mBaselineNames;
        F64                   mVerifyUntil = 0.0;
        F64                   mFetchedAt   = 0.0;
        ContentsFetch         mFetch;
    };

    void run_add_contents(const boost::json::object& args, const IDMCPCallPtr& call)
    {
        Target t;
        if (!resolve(args, call, t)) return;
        auto it = args.find("item_ids");
        if (it == args.end() || !it->value().is_array() || it->value().as_array().empty())
        {
            idmcp_tool_err(call, IDMCP_ERR_INVALID_PARAMS, "item_ids must be a list of inventory item UUIDs");
            return;
        }
        if (it->value().as_array().size() > MAX_ITEMS)
        {
            idmcp_tool_err(call, IDMCP_ERR_INVALID_PARAMS, llformat("at most %d items per call", (S32)MAX_ITEMS));
            return;
        }
        std::vector<LLUUID> ids;
        for (const boost::json::value& v : it->value().as_array())
        {
            const std::string s = v.is_string() ? std::string(v.as_string().c_str()) : std::string();
            if (!isUuid(s))
            {
                idmcp_tool_err(call, IDMCP_ERR_INVALID_PARAMS, "item_ids must hold inventory item UUIDs");
                return;
            }
            ids.emplace_back(s);
        }
        auto job = std::make_shared<AddJob>(call, t, ids, argBool(args, "running", true));
        Job::launch(job);
        job->begin();
    }
}

// ---------------------------------------------------------------------------

void idmcp_register_contents_tools(IDMCPToolRegistry& reg)
{
    // object.contents --------------------------------------------------------------
    {
        IDMCPTool t;
        t.name = "object.contents";
        t.description =
            "List the items inside an object (its Contents tab). {\"object_id\"}*; {\"link\"} "
            "lists one prim's contents instead of the root's (link numbers from object.get). "
            "Returns {object_id, items:[{item_id, name, type, permissions}]}. The region only "
            "shows contents of objects you can modify.";
        t.input_schema = boost::json::parse(
            R"({"type":"object","properties":{"object_id":{"type":"string"},"link":{"type":"integer"}},)"
            R"("required":["object_id"],"additionalProperties":false})");
        t.timeout = 25.0;
        t.invoke  = [](const boost::json::object& args, const IDMCPCallPtr& call) { run_contents(args, call); };
        reg.add(std::move(t));
    }

    // object.addContents -----------------------------------------------------------
    {
        IDMCPTool t;
        t.name = "object.addContents";
        t.description =
            "Put inventory items into an object, as dragging them onto it would. "
            "{\"object_id\"}*, {\"item_ids\"}* (up to 32), {\"link\"} to fill one prim instead "
            "of the root, {\"running\"} for scripts (default true). The usual drop rules apply: "
            "a no-copy item moves out of your inventory, and worn items are refused. Returns "
            "{object_id, items:[{item_id, name, result}]} where result is added, not_found (sent "
            "but not seen in the contents within 20 s), or why it wasn't sent. Blocked by RLV "
            "@edit, @editobj, @editworld, @editattach, @interact, a locked attachment, or "
            "@unsit/@sittp while you sit on the object.";
        t.input_schema = boost::json::parse(
            R"({"type":"object","properties":{"object_id":{"type":"string"},"link":{"type":"integer"},)"
            R"("item_ids":{"type":"array","items":{"type":"string"},"minItems":1,"maxItems":32},)"
            R"("running":{"type":"boolean"}},"required":["object_id","item_ids"],"additionalProperties":false})");
        t.gate    = gate_add_contents;
        t.timeout = 60.0;
        t.invoke  = [](const boost::json::object& args, const IDMCPCallPtr& call) { run_add_contents(args, call); };
        reg.add(std::move(t));
    }
}
