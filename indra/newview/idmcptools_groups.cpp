/**
 * @file idmcptools_groups.cpp
 * @brief <ID> MCP server: group interactions (list / info / activate / chat).
 *
 * Part of Five's custom Firestorm fork.
 *
 * group.list reads the agent's group list; group.getInfo fetches group
 * properties (deferred via LLGroupMgr observer); group.activate sets the active
 * group (gated on RLV @setgroup); group.sendIM sends to a group chat session.
 */

#include "llviewerprecompiledheaders.h"

#include "idmcp.h"
#include "idmcptools.h"
#include "idmcpserver.h"
#include "idmcprlvgate.h"

#include "llagent.h"
#include "llgroupmgr.h"
#include "llgroupactions.h"
#include "llimview.h"
#include "llinstantmessage.h"
#include "llviewermessage.h"  // send_group_notice
#include "message.h"          // gMessageSystem
#include "roles_constants.h"  // GP_NOTICES_SEND
#include "rlvactions.h"
#include "llsingleton.h"
#include "lluuid.h"

#include <map>

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

    boost::json::value group_info_json(const LLUUID& id, LLGroupMgrGroupData* gd)
    {
        boost::json::object o;
        o["id"]           = id.asString();
        o["name"]         = gd->mName;
        o["charter"]      = gd->mCharter;
        o["member_count"] = gd->mMemberCount;
        return o;
    }

    // One-shot group-properties observer; coordinated teardown (mSettled) so the
    // reply path and abandon path never double-free.
    class IDMCPGroupObserver : public LLParticularGroupObserver
    {
    public:
        IDMCPGroupObserver(IDMCPCallPtr call, const LLUUID& id)
            : mCall(std::move(call)), mId(id) {}

        void changed(const LLUUID& group_id, LLGroupChange) override
        {
            if (mSettled || group_id != mId)
            {
                return;
            }
            LLGroupMgrGroupData* gd = LLGroupMgr::getInstance()->getGroupData(mId);
            unregister();
            if (gd)
            {
                idmcp_tool_ok(mCall, group_info_json(mId, gd));
            }
            else
            {
                idmcp_tool_err(mCall, IDMCP_ERR_NOT_FOUND, "group not found");
            }
            delete this;
        }

        void abandon()
        {
            if (mSettled)
            {
                return;
            }
            unregister();
            delete this;
        }

    private:
        void unregister()
        {
            mSettled = true;
            LLGroupMgr::getInstance()->removeObserver(mId, this);
        }

        IDMCPCallPtr mCall;
        LLUUID       mId;
        bool         mSettled = false;
    };
}

// ---------------------------------------------------------------------------
// Group notices: request the list over UDP and resolve the reply (forwarded
// from LLPanelGroupNotices::processGroupNoticesListReply). Keyed by group id.

class IDGroupNoticesModel : public LLSingleton<IDGroupNoticesModel>
{
    LLSINGLETON_EMPTY_CTOR(IDGroupNoticesModel);

public:
    void start(const LLUUID& group_id, const IDMCPCallPtr& call)
    {
        mPending[group_id] = call;

        gMessageSystem->newMessage("GroupNoticesListRequest");
        gMessageSystem->nextBlock("AgentData");
        gMessageSystem->addUUID("AgentID", gAgentID);
        gMessageSystem->addUUID("SessionID", gAgentSessionID);
        gMessageSystem->nextBlock("Data");
        gMessageSystem->addUUID("GroupID", group_id);
        gAgent.sendReliableMessage();

        call->setCleanup([group_id]()
        {
            if (IDGroupNoticesModel::instanceExists())
                IDGroupNoticesModel::instance().cancel(group_id);
        });
    }

    void cancel(const LLUUID& group_id) { mPending.erase(group_id); }

    void handleReply(LLMessageSystem* msg)
    {
        LLUUID gid;
        msg->getUUID("AgentData", "GroupID", gid);
        auto it = mPending.find(gid);
        if (it == mPending.end()) return;
        IDMCPCallPtr call = it->second;
        mPending.erase(it);

        boost::json::array arr;
        const S32 count = msg->getNumberOfBlocks("Data");
        for (S32 i = 0; i < count; ++i)
        {
            LLUUID nid;
            std::string subject, from;
            bool has_attachment = false;
            U8 asset_type = 0;
            U32 timestamp = 0;
            msg->getUUID(  "Data", "NoticeID",      nid,            i);
            msg->getString("Data", "Subject",       subject,        i);
            msg->getString("Data", "FromName",      from,           i);
            msg->getBOOL(  "Data", "HasAttachment", has_attachment, i);
            msg->getU8(    "Data", "AssetType",     asset_type,     i);
            msg->getU32(   "Data", "Timestamp",     timestamp,      i);
            if (nid.isNull()) continue;   // the list carries a null sentinel row
            boost::json::object o;
            o["notice_id"]      = nid.asString();
            o["subject"]        = subject;
            o["from"]           = from;
            o["has_attachment"] = has_attachment;
            o["timestamp"]      = (int64_t)timestamp;
            arr.push_back(std::move(o));
        }
        boost::json::object out;
        out["notices"]  = std::move(arr);
        out["group_id"] = gid.asString();
        idmcp_tool_ok(call, out);
    }

private:
    std::map<LLUUID, IDMCPCallPtr> mPending;
};

void idmcp::onGroupNoticesListReply(LLMessageSystem* msg)
{
    if (IDGroupNoticesModel::instanceExists())
        IDGroupNoticesModel::instance().handleReply(msg);
}

// ---------------------------------------------------------------------------

void idmcp_register_group_tools(IDMCPToolRegistry& reg)
{
    // group.list -------------------------------------------------------------
    {
        IDMCPTool t;
        t.name = "group.list";
        t.description = "List the groups you belong to, plus your active group.";
        t.input_schema = boost::json::parse(
            R"({"type":"object","properties":{},"additionalProperties":false})");
        t.invoke = [](const boost::json::object&, const IDMCPCallPtr& call)
        {
            boost::json::array arr;
            for (const LLGroupData& g : gAgent.mGroups)
            {
                boost::json::object o;
                o["id"]             = g.mID.asString();
                o["name"]           = g.mName;
                o["accept_notices"] = g.mAcceptNotices;
                arr.push_back(std::move(o));
            }
            boost::json::object out;
            out["groups"] = std::move(arr);
            out["active"] = gAgent.getGroupID().asString();
            idmcp_tool_ok(call, out);
        };
        reg.add(std::move(t));
    }

    // group.getInfo ----------------------------------------------------------
    {
        IDMCPTool t;
        t.name = "group.getInfo";
        t.description =
            "Fetch a group's details ({\"group_id\"}): name, charter, member "
            "count. Fetches from the server if not cached.";
        t.input_schema = boost::json::parse(
            R"({"type":"object","properties":{"group_id":{"type":"string"}},"required":["group_id"],"additionalProperties":false})");
        t.invoke = [](const boost::json::object& args, const IDMCPCallPtr& call)
        {
            const LLUUID gid = arg_uuid(args, "group_id");
            if (gid.isNull())
            {
                idmcp_tool_err(call, IDMCP_ERR_INVALID_PARAMS, "group_id must be a UUID");
                return;
            }
            LLGroupMgrGroupData* gd = LLGroupMgr::getInstance()->getGroupData(gid);
            if (gd && !gd->mName.empty())
            {
                idmcp_tool_ok(call, group_info_json(gid, gd));
                return;
            }
            IDMCPGroupObserver* obs = new IDMCPGroupObserver(call, gid);
            LLGroupMgr::getInstance()->addObserver(gid, obs);
            call->setCleanup([obs]() { obs->abandon(); });
            LLGroupMgr::getInstance()->sendGroupPropertiesRequest(gid);
        };
        reg.add(std::move(t));
    }

    // group.activate ---------------------------------------------------------
    {
        IDMCPTool t;
        t.name = "group.activate";
        t.description = "Set your active (tag) group to {\"group_id\"}. Blocked by RLV @setgroup.";
        t.input_schema = boost::json::parse(
            R"({"type":"object","properties":{"group_id":{"type":"string"}},"required":["group_id"],"additionalProperties":false})");
        t.gate = [](const boost::json::object&, IDMCPGatePhase) -> IDMCPGateResult
        {
            if (IDMCPRlvGate::isEnabled() && !RlvActions::canChangeActiveGroup())
            {
                IDMCPGateResult r; r.allowed = false; r.behaviour = "setgroup"; return r;
            }
            return IDMCPGateResult();
        };
        t.invoke = [](const boost::json::object& args, const IDMCPCallPtr& call)
        {
            const LLUUID gid = arg_uuid(args, "group_id");
            if (gid.isNull())
            {
                idmcp_tool_err(call, IDMCP_ERR_INVALID_PARAMS, "group_id must be a UUID");
                return;
            }
            LLGroupActions::activate(gid);
            boost::json::object o; o["accepted"] = true;
            idmcp_tool_ok(call, o);
        };
        reg.add(std::move(t));
    }

    // group.sendIM -----------------------------------------------------------
    {
        IDMCPTool t;
        t.name = "group.sendIM";
        t.description =
            "Send a message to a group's chat. {\"group_id\"} and {\"message\"}. "
            "You must be a member of the group.";
        t.input_schema = boost::json::parse(
            R"({"type":"object","properties":{"group_id":{"type":"string"},"message":{"type":"string"}},"required":["group_id","message"],"additionalProperties":false})");
        // RLV @sendim blocks group chat for a human too (canSendIM treats the
        // group id as the recipient); LLIMModel::sendMessage doesn't self-enforce,
        // so gate here to match the human path.
        t.gate = [](const boost::json::object& args, IDMCPGatePhase) -> IDMCPGateResult
        {
            if (IDMCPRlvGate::isEnabled())
            {
                const LLUUID gid = arg_uuid(args, "group_id");
                if (gid.notNull() && !RlvActions::canSendIM(gid))
                {
                    IDMCPGateResult r; r.allowed = false; r.behaviour = "sendim"; return r;
                }
            }
            return IDMCPGateResult();
        };
        t.invoke = [](const boost::json::object& args, const IDMCPCallPtr& call)
        {
            const LLUUID gid = arg_uuid(args, "group_id");
            const std::string message = arg_str(args, "message");
            if (gid.isNull() || message.empty())
            {
                idmcp_tool_err(call, IDMCP_ERR_INVALID_PARAMS, "group_id (UUID) and message required");
                return;
            }
            if (!gAgent.isInGroup(gid))
            {
                idmcp_tool_err(call, IDMCP_ERR_PERMISSION, "you are not a member of that group");
                return;
            }
            LLIMModel::sendMessage(message,
                                   gIMMgr->computeSessionID(IM_SESSION_GROUP_START, gid),
                                   gid, IM_SESSION_SEND);
            boost::json::object o; o["accepted"] = true;
            idmcp_tool_ok(call, o);
        };
        reg.add(std::move(t));
    }

    // group.getNotices -------------------------------------------------------
    {
        IDMCPTool t;
        t.name = "group.getNotices";
        t.description =
            "List a group's notices ({\"group_id\"}): notice id, subject, sender, "
            "whether it has an attachment, and timestamp.";
        t.input_schema = boost::json::parse(
            R"({"type":"object","properties":{"group_id":{"type":"string"}},"required":["group_id"],"additionalProperties":false})");
        t.invoke = [](const boost::json::object& args, const IDMCPCallPtr& call)
        {
            const LLUUID gid = arg_uuid(args, "group_id");
            if (gid.isNull())
            {
                idmcp_tool_err(call, IDMCP_ERR_INVALID_PARAMS, "group_id must be a UUID");
                return;
            }
            IDGroupNoticesModel::instance().start(gid, call);   // deferred -> SSE
        };
        reg.add(std::move(t));
    }

    // group.sendNotice -------------------------------------------------------
    {
        IDMCPTool t;
        t.name = "group.sendNotice";
        t.description =
            "Send a notice to a group. {\"group_id\"}, {\"subject\"}, {\"message\"}. "
            "Requires the group's Send Notices power.";
        t.input_schema = boost::json::parse(
            R"({"type":"object","properties":{"group_id":{"type":"string"},"subject":{"type":"string"},"message":{"type":"string"}},"required":["group_id","subject","message"],"additionalProperties":false})");
        t.invoke = [](const boost::json::object& args, const IDMCPCallPtr& call)
        {
            const LLUUID gid = arg_uuid(args, "group_id");
            const std::string subject = arg_str(args, "subject");
            const std::string message = arg_str(args, "message");
            if (gid.isNull() || subject.empty())
            {
                idmcp_tool_err(call, IDMCP_ERR_INVALID_PARAMS, "group_id (UUID) and subject required");
                return;
            }
            if (!gAgent.hasPowerInGroup(gid, GP_NOTICES_SEND))
            {
                idmcp_tool_err(call, IDMCP_ERR_PERMISSION, "you lack the Send Notices power in that group");
                return;
            }
            send_group_notice(gid, subject, message, nullptr);
            boost::json::object o; o["accepted"] = true;
            idmcp_tool_ok(call, o);
        };
        reg.add(std::move(t));
    }
}
