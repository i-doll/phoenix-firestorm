/**
 * @file idmcptools_im.cpp
 * @brief <ID> MCP server: 1:1 instant messaging (send + receive/read).
 *
 * Part of Five's custom Firestorm fork. Custom code carries an `ID` prefix.
 *
 * Person-to-person IM (NOT group chat - that's group.sendIM, NOT public chat -
 * that's chat.*). Sending mirrors group.sendIM but on a P2P session. Receiving is
 * different from chat.listen: IMs arrive unsolicited from anyone at any time, so a
 * persistent buffer is installed once (LLIMModel::addNewMsgCallback) and collects
 * inbound P2P messages from server start; im.replies drains it (chat.replies-style
 * wait), im.getMessages reads stored history, im.getConversations lists sessions.
 * RLV @sendim gates sending; @recvim filters what lands in the buffer. Main-thread.
 */

#include "llviewerprecompiledheaders.h"

#include "idmcp.h"
#include "idmcptools.h"
#include "idmcpserver.h"
#include "idmcprlvgate.h"

#include "llimview.h"           // LLIMModel, LLIMMgr, gIMMgr, LLIMSession
#include "llinstantmessage.h"   // IM_NOTHING_SPECIAL
#include "llagent.h"            // gAgent
#include "rlvactions.h"

#include "llevents.h"           // LLEventPumps, LLTempBoundListener
#include "lltimer.h"            // LLTimer::getTotalSeconds
#include "llsd.h"
#include "lluuid.h"

#include <algorithm>
#include <map>
#include <memory>
#include <vector>

namespace
{
    // ---- arg helpers -------------------------------------------------------

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

    S32 arg_int(const boost::json::object& args, const char* key, S32 dflt)
    {
        auto it = args.find(key);
        if (it == args.end()) return dflt;
        if (it->value().is_int64())  return (S32)it->value().as_int64();
        if (it->value().is_double()) return (S32)it->value().as_double();
        return dflt;
    }

    static const size_t ID_IM_BUFFER_MAX   = 100;   // per-session inbound cap
    static const S32    ID_IM_MAX_WAIT      = 30;

    // ---- inbound buffer (always-on) ---------------------------------------

    std::map<LLUUID, boost::json::array> g_inbound;   // session_id -> messages

    struct Wait
    {
        IDMCPCallPtr call;
        LLUUID       session;      // null => any session
        F64          deadline = 0.0;
        bool         done = false;
    };
    std::vector<std::shared_ptr<Wait>> g_waits;

    bool                g_tick_on = false;
    LLTempBoundListener g_tick;
    bool                g_buffer_installed = false;
    boost::signals2::connection g_msg_conn;

    // Drain buffered inbound IMs into a JSON array. If `session` is non-null only
    // that session; otherwise all sessions (each entry carries its session_id).
    boost::json::array drain(const LLUUID& session)
    {
        boost::json::array out;
        if (session.notNull())
        {
            auto it = g_inbound.find(session);
            if (it != g_inbound.end())
            {
                out = std::move(it->second);
                it->second = boost::json::array();
            }
        }
        else
        {
            for (auto& [sid, buf] : g_inbound)
            {
                for (auto& v : buf) out.push_back(std::move(v));
                buf = boost::json::array();
            }
        }
        return out;
    }

    bool has_buffered(const LLUUID& session)
    {
        if (session.notNull())
        {
            auto it = g_inbound.find(session);
            return it != g_inbound.end() && !it->second.empty();
        }
        for (auto& [sid, buf] : g_inbound) if (!buf.empty()) return true;
        return false;
    }

    void finish_replies(const std::shared_ptr<Wait>& w)
    {
        if (w->done) return;
        w->done = true;
        boost::json::object o;
        o["replies"] = drain(w->session);
        idmcp_tool_ok(w->call, o);
    }

    void im_tick()
    {
        if (g_waits.empty()) return;
        const F64 now = LLTimer::getTotalSeconds();
        for (auto it = g_waits.begin(); it != g_waits.end(); )
        {
            auto& w = *it;
            if (!w->done && (has_buffered(w->session) || now >= w->deadline)) finish_replies(w);
            if (w->done) it = g_waits.erase(it);
            else         ++it;
        }
    }

    void ensure_tick()
    {
        if (g_tick_on) return;
        g_tick = LLEventPumps::instance().obtain("mainloop").listen(
            "idmcp_im", [](const LLSD&) -> bool { im_tick(); return false; });
        g_tick_on = true;
    }

    // The addNewMsgCallback slot: filter to inbound P2P and buffer it.
    void on_new_message(const LLSD& arg)
    {
        const LLUUID from_id    = arg["from_id"].asUUID();
        const LLUUID session_id = arg["session_id"].asUUID();
        if (from_id.isNull() || from_id == gAgent.getID()) return;   // drop our own echo

        auto* s = LLIMModel::getInstance()->findIMSession(session_id);
        if (!s || !s->isP2PSessionType()) return;                    // P2P only

        if (IDMCPRlvGate::isEnabled() && !RlvActions::canReceiveIM(from_id)) return;

        boost::json::object e;
        e["from_id"]    = from_id.asString();
        e["from"]       = arg["from"].asString();
        e["text"]       = arg["message"].asString();
        e["session_id"] = session_id.asString();
        e["time"]       = (double)LLTimer::getTotalSeconds();

        auto& buf = g_inbound[session_id];
        if (buf.size() >= ID_IM_BUFFER_MAX) buf.erase(buf.begin());
        buf.push_back(std::move(e));

        // Wake any im.replies waiting on this (or any) session.
        for (auto& w : g_waits)
        {
            if (!w->done && (w->session.isNull() || w->session == session_id)) finish_replies(w);
        }
    }

    void ensure_buffer()
    {
        if (g_buffer_installed) return;
        g_msg_conn = LLIMModel::getInstance()->addNewMsgCallback(
            [](const LLSD& arg) { on_new_message(arg); });
        g_buffer_installed = true;
    }

    // Resolve the target session id from avatar_id or session_id args.
    bool resolve_session(const boost::json::object& args, LLUUID& out)
    {
        const std::string sid = arg_str(args, "session_id");
        if (looks_like_uuid(sid)) { out = LLUUID(sid); return true; }
        const std::string aid = arg_str(args, "avatar_id");
        if (looks_like_uuid(aid))
        {
            out = gIMMgr->computeSessionID(IM_NOTHING_SPECIAL, LLUUID(aid));
            return true;
        }
        return false;
    }

    IDMCPGateResult gate_imsend(const boost::json::object& args, IDMCPGatePhase)
    {
        if (!IDMCPRlvGate::isEnabled()) return IDMCPGateResult();
        const std::string aid = arg_str(args, "avatar_id");
        if (looks_like_uuid(aid) && !RlvActions::canSendIM(LLUUID(aid)))
        {
            return IDMCPRlvGate::deny(RLV_BHVR_SENDIM, "sendim");
        }
        return IDMCPGateResult();
    }
}

// ---------------------------------------------------------------------------

void idmcp_register_im_tools(IDMCPToolRegistry& reg)
{
    ensure_buffer();   // start collecting inbound IMs immediately

    // im.send ----------------------------------------------------------------
    {
        IDMCPTool t;
        t.name = "im.send";
        t.description =
            "Send a 1:1 instant message to an avatar ({\"avatar_id\"} UUID, "
            "{\"message\"}). Private, person-to-person (not public chat, not group "
            "chat). To read their reply, poll im.replies. Blocked by RLV @sendim.";
        t.input_schema = boost::json::parse(
            R"({"type":"object","properties":{"avatar_id":{"type":"string"},"message":{"type":"string"}},"required":["avatar_id","message"],"additionalProperties":false})");
        t.gate = gate_imsend;
        t.invoke = [](const boost::json::object& args, const IDMCPCallPtr& call)
        {
            const std::string aid = arg_str(args, "avatar_id");
            const std::string message = arg_str(args, "message");
            if (!looks_like_uuid(aid) || message.empty())
            {
                idmcp_tool_err(call, IDMCP_ERR_INVALID_PARAMS, "avatar_id (UUID) and message required");
                return;
            }
            const LLUUID other(aid);
            const LLUUID session = gIMMgr->computeSessionID(IM_NOTHING_SPECIAL, other);
            LLIMModel::sendMessage(message, session, other, IM_NOTHING_SPECIAL);

            boost::json::object o;
            o["sent"]       = true;
            o["session_id"] = session.asString();
            idmcp_tool_ok(call, o);
        };
        reg.add(std::move(t));
    }

    // im.replies -------------------------------------------------------------
    {
        IDMCPTool t;
        t.name = "im.replies";
        t.description =
            "Read newly-received 1:1 IMs. Optionally filter to one correspondent "
            "with {\"avatar_id\"} or {\"session_id\"} (omit both for all P2P IMs). "
            "{\"wait_seconds\"} (max 30): if nothing is buffered, wait up to that "
            "long for the first message (good after im.send). Drains and returns "
            "{replies:[{from_id, from, text, session_id, time}]}.";
        t.input_schema = boost::json::parse(
            R"({"type":"object","properties":{"avatar_id":{"type":"string"},"session_id":{"type":"string"},"wait_seconds":{"type":"integer"}},"additionalProperties":false})");
        t.invoke = [](const boost::json::object& args, const IDMCPCallPtr& call)
        {
            LLUUID session;   // null => all
            resolve_session(args, session);

            S32 wait = arg_int(args, "wait_seconds", 0);
            if (has_buffered(session) || wait <= 0)
            {
                boost::json::object o;
                o["replies"] = drain(session);
                idmcp_tool_ok(call, o);
                return;
            }
            if (wait > ID_IM_MAX_WAIT) wait = ID_IM_MAX_WAIT;

            auto w = std::make_shared<Wait>();
            w->call     = call;
            w->session  = session;
            w->deadline = LLTimer::getTotalSeconds() + wait;
            g_waits.push_back(w);
            ensure_tick();
        };
        reg.add(std::move(t));
    }

    // im.getConversations ----------------------------------------------------
    {
        IDMCPTool t;
        t.name = "im.getConversations";
        t.description =
            "List your open 1:1 IM conversations: session_id, avatar_id (the other "
            "person), name, and num_unread. No arguments.";
        t.input_schema = boost::json::parse(
            R"({"type":"object","properties":{},"additionalProperties":false})");
        t.invoke = [](const boost::json::object&, const IDMCPCallPtr& call)
        {
            boost::json::array arr;
            for (const auto& [sid, s] : LLIMModel::getInstance()->mId2SessionMap)
            {
                if (!s || !s->isP2PSessionType()) continue;
                boost::json::object o;
                o["session_id"] = sid.asString();
                o["avatar_id"]  = s->mOtherParticipantID.asString();
                o["name"]       = s->mName;
                o["num_unread"] = s->mNumUnread;
                arr.push_back(std::move(o));
            }
            boost::json::object out;
            out["conversations"] = std::move(arr);
            idmcp_tool_ok(call, out);
        };
        reg.add(std::move(t));
    }

    // im.getMessages ---------------------------------------------------------
    {
        IDMCPTool t;
        t.name = "im.getMessages";
        t.description =
            "Read stored history for one 1:1 conversation ({\"avatar_id\"} or "
            "{\"session_id\"}, optional {\"limit\"} default 20, newest first). "
            "Read-only: does NOT clear the user's unread badge. Returns "
            "{session_id, messages:[{from, from_id, message, time, timestamp, is_history}]} "
            "(time is SL's display string; timestamp is epoch and is often 0).";
        t.input_schema = boost::json::parse(
            R"({"type":"object","properties":{"avatar_id":{"type":"string"},"session_id":{"type":"string"},"limit":{"type":"integer"}},"additionalProperties":false})");
        t.invoke = [](const boost::json::object& args, const IDMCPCallPtr& call)
        {
            LLUUID session;
            if (!resolve_session(args, session))
            {
                idmcp_tool_err(call, IDMCP_ERR_INVALID_PARAMS, "provide avatar_id or session_id (UUID)");
                return;
            }
            const S32 limit = std::max(1, arg_int(args, "limit", 20));

            std::list<LLSD> msgs;
            LLIMModel::getInstance()->getMessagesSilently(session, msgs, 0);

            boost::json::array arr;
            S32 count = 0;
            for (const auto& e : msgs)   // newest first
            {
                const LLUUID from_id = e["from_id"].asUUID();
                // @recvim hides an inbound sender's messages from a human; do the
                // same here. Never filter our own sent messages (from_id == self).
                if (IDMCPRlvGate::isEnabled() && from_id != gAgent.getID() && !RlvActions::canReceiveIM(from_id))
                    continue;
                if (count++ >= limit) break;
                boost::json::object o;
                o["from"]       = e["from"].asString();
                o["from_id"]    = from_id.asString();
                o["message"]    = e["message"].asString();
                // SL stores a display-time string ("time", always populated) plus an
                // epoch "timestamp" that is often 0 for region/history messages. Expose
                // both so the readable time survives even when the epoch is missing.
                o["time"]       = e["time"].asString();
                o["timestamp"]  = (int64_t)e["timestamp"].asInteger();
                o["is_history"] = e["is_history"].asBoolean();
                arr.push_back(std::move(o));
            }

            boost::json::object out;
            out["session_id"] = session.asString();
            out["messages"]   = std::move(arr);
            idmcp_tool_ok(call, out);
        };
        reg.add(std::move(t));
    }
}
