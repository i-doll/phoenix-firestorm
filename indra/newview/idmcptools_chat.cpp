/**
 * @file idmcptools_chat.cpp
 * @brief <ID> MCP server: bridge-mediated in-world chat (send/listen on any channel).
 *
 * Part of Five's custom Firestorm fork.
 *
 * The viewer can only say on channel 0 / positive channels and only hears
 * channel-0 / owner-say / debug chat, so it cannot talk to scripted objects or
 * RLV relays on their (often negative) protocol channels. We route through the
 * Firestorm LSL Bridge — a worn, owned LSL *script* that can llListen/llSay on
 * ANY channel — via FSLSLBridge::{sendChatViaBridge,listenViaBridge,
 * stopListenViaBridge}. The bridge forwards heard chat back over owner-say,
 * parsed in FSLSLBridge::lslToViewer and delivered here through idmcp::onBridgeChat.
 *
 * Workflow (taught by the firestorm-chat / firestorm-rlv-relay skills):
 *   chat.listen  -> open a bounded listen on a channel (BEFORE sending)
 *   chat.send    -> say a message on a channel (RLV-gated)
 *   chat.replies -> read what was heard (optionally wait briefly for the first)
 *   chat.stopListen -> close the listen (cleanup)
 *
 * Bridge llListen slots are scarce, so cleanup is enforced at multiple layers:
 * a hard cap on the bridge, the viewer auto-sending ChatStopListen at each
 * listen's deadline (below), explicit chat.stopListen, a ChatStopListen|all on
 * MCP server stop (idmcp::stopAllBridgeListens), and the skills' discipline.
 */

#include "llviewerprecompiledheaders.h"

#include "idmcp.h"
#include "idmcptools.h"
#include "idmcpserver.h"
#include "idmcprlvgate.h"

#include "fslslbridge.h"
#include "llviewercontrol.h"    // gSavedSettings
#include "llevents.h"           // LLEventPumps, LLTempBoundListener
#include "lltimer.h"            // LLTimer::getTotalSeconds
#include "lluuid.h"

#include "rlvactions.h"
#include "rlvhandler.h"         // gRlvHandler, RLV_BHVR_*
#include "llchat.h"             // EChatType (as_avatar path)

#include <algorithm>
#include <map>
#include <memory>
#include <string>
#include <vector>

// <ID> Speak as the avatar (not via the bridge object) on channel 0 / positive
// channels. This free function has no shared header (see fsnearbychathub.cpp);
// callers declare it extern, exactly as llchatbar.cpp does.
extern void send_chat_from_viewer(std::string utf8_out_text, EChatType type, S32 channel);

namespace
{
    // ---- arg helpers -------------------------------------------------------

    bool arg_has(const boost::json::object& o, const char* key)
    {
        return o.find(key) != o.end();
    }

    bool arg_bool(const boost::json::object& o, const char* key, bool dflt)
    {
        auto it = o.find(key);
        return (it != o.end() && it->value().is_bool()) ? it->value().as_bool() : dflt;
    }

    std::string arg_str(const boost::json::object& o, const char* key)
    {
        auto it = o.find(key);
        return (it != o.end() && it->value().is_string())
                   ? std::string(it->value().as_string().c_str()) : std::string();
    }

    S32 arg_int(const boost::json::object& o, const char* key, S32 dflt)
    {
        auto it = o.find(key);
        if (it == o.end()) return dflt;
        if (it->value().is_int64())  return (S32)it->value().as_int64();
        if (it->value().is_double()) return (S32)it->value().as_double();
        if (it->value().is_string()) return (S32)atoi(it->value().as_string().c_str());
        return dflt;
    }

    bool bridge_chat_enabled()
    {
        return gSavedSettings.getBOOL("IDMCPBridgeChatEnabled");
    }

    // -1 = default listen window; hard bounds mirror the bridge.
    static const S32    ID_LISTEN_DEFAULT_SECS = 30;
    static const S32    ID_LISTEN_MAX_SECS     = 120;
    static const S32    ID_REPLIES_MAX_WAIT    = 30;
    static const size_t ID_BUFFER_MAX          = 100;   // per-channel reply cap

    // ---- state (main-thread only) -----------------------------------------

    struct IdListen
    {
        boost::json::array buffer;      // heard messages awaiting chat.replies
        F64                expiry = 0;  // when the viewer auto-sends ChatStopListen
    };
    std::map<S32, IdListen> g_listens;

    enum WaitKind { WAIT_LISTEN_ACK, WAIT_REPLIES };
    struct Wait
    {
        IDMCPCallPtr call;
        S32          channel = 0;
        F64          deadline = 0;
        WaitKind     kind = WAIT_LISTEN_ACK;
        bool         done = false;
    };
    std::vector<std::shared_ptr<Wait>> g_waits;

    bool                g_tick_on = false;
    LLTempBoundListener g_tick;

    void chat_tick();

    void ensure_tick()
    {
        if (g_tick_on) return;
        g_tick = LLEventPumps::instance().obtain("mainloop").listen(
            "idmcp_chat", [](const LLSD&) -> bool { chat_tick(); return false; });
        g_tick_on = true;
    }

    void finish_listen(const std::shared_ptr<Wait>& w, bool opened, bool confirmed, const std::string& err)
    {
        if (w->done) return;
        w->done = true;
        if (opened)
        {
            boost::json::object o;
            o["listening"] = true;
            o["channel"]   = w->channel;
            o["confirmed"] = confirmed;
            auto it = g_listens.find(w->channel);
            int expires_in = 0;
            if (it != g_listens.end())
            {
                F64 remain = it->second.expiry - (F64)LLTimer::getTotalSeconds();
                expires_in = (remain > 0.0) ? (int)remain : 0;
            }
            o["expires_in"] = expires_in;
            idmcp_tool_ok(w->call, o);
        }
        else
        {
            g_listens.erase(w->channel);
            idmcp_tool_err(w->call, IDMCP_ERR_PERMISSION,
                           err.empty() ? "bridge did not open the listen" : ("bridge refused listen: " + err));
        }
    }

    void finish_replies(const std::shared_ptr<Wait>& w)
    {
        if (w->done) return;
        w->done = true;
        boost::json::object o;
        o["channel"] = w->channel;
        auto it = g_listens.find(w->channel);
        if (it != g_listens.end())
        {
            o["listening"] = true;
            o["replies"]   = std::move(it->second.buffer);
            it->second.buffer = boost::json::array();
        }
        else
        {
            o["listening"] = false;   // listen expired/closed while waiting
            o["replies"]   = boost::json::array();
        }
        idmcp_tool_ok(w->call, o);
    }

    void chat_tick()
    {
        const F64 now = LLTimer::getTotalSeconds();

        // 1. Viewer-driven expiry: close listens whose window elapsed (primary
        //    cleanup — the bridge's own sweep is only a crash backstop).
        for (auto it = g_listens.begin(); it != g_listens.end(); )
        {
            if (now >= it->second.expiry)
            {
                FSLSLBridge::instance().stopListenViaBridge(std::to_string(it->first));
                it = g_listens.erase(it);
            }
            else
            {
                ++it;
            }
        }

        // 2. Resolve deadline-expired waits.
        for (auto it = g_waits.begin(); it != g_waits.end(); )
        {
            auto& w = *it;
            if (!w->done && now >= w->deadline)
            {
                if (w->kind == WAIT_LISTEN_ACK)
                {
                    // No ack in time; the listen almost certainly opened (bridge
                    // was reachable), so answer optimistically but unconfirmed.
                    finish_listen(w, /*opened*/ g_listens.count(w->channel) > 0, /*confirmed*/ false, "");
                }
                else
                {
                    finish_replies(w);
                }
            }
            if (w->done) it = g_waits.erase(it);
            else         ++it;
        }
    }

    // ---- RLV gate for chat.send -------------------------------------------

    IDMCPGateResult gate_chatsend(const boost::json::object& args, IDMCPGatePhase)
    {
        if (!IDMCPRlvGate::isEnabled())
        {
            return IDMCPGateResult();
        }
        const S32 channel = arg_int(args, "channel", 0);
        if (channel == 0)
        {
            if (gRlvHandler.hasBehaviour(RLV_BHVR_SENDCHAT))
            {
                return IDMCPRlvGate::deny(RLV_BHVR_SENDCHAT, "sendchat");
            }
        }
        else if (!RlvActions::canSendChannel(channel))
        {
            return IDMCPRlvGate::deny(RLV_BHVR_SENDCHANNEL, "sendchannel");
        }
        return IDMCPGateResult();
    }

    // Common preflight for the bridge-backed tools.
    bool bridge_ready(const IDMCPCallPtr& call)
    {
        if (!bridge_chat_enabled())
        {
            idmcp_tool_err(call, IDMCP_ERR_CAP_UNAVAIL,
                           "bridge chat is disabled (set IDMCPBridgeChatEnabled)");
            return false;
        }
        if (!FSLSLBridge::instance().canUseBridge())
        {
            idmcp_tool_err(call, IDMCP_ERR_CAP_UNAVAIL,
                           "the Firestorm LSL bridge is not available (enable it and wait for it to attach)");
            return false;
        }
        return true;
    }
}

// ---------------------------------------------------------------------------
// Facade impls (called from FSLSLBridge::lslToViewer and IDMCPServer::stop).

void idmcp::onBridgeChat(int channel, const LLUUID& from, const std::string& name, const std::string& text)
{
    auto it = g_listens.find((S32)channel);
    if (it == g_listens.end())
    {
        return;   // not (or no longer) listening on this channel — drop it
    }
    boost::json::object e;
    e["from_id"]   = from.asString();
    e["from_name"] = name;
    e["text"]      = text;
    e["time"]      = (double)LLTimer::getTotalSeconds();
    if (it->second.buffer.size() >= ID_BUFFER_MAX)
    {
        it->second.buffer.erase(it->second.buffer.begin());   // drop oldest
    }
    it->second.buffer.push_back(std::move(e));

    // Wake any chat.replies waiting on this channel.
    for (auto& w : g_waits)
    {
        if (!w->done && w->kind == WAIT_REPLIES && w->channel == (S32)channel)
        {
            finish_replies(w);
        }
    }
}

void idmcp::onBridgeChatStatus(bool opened, int channel, const std::string& error)
{
    for (auto& w : g_waits)
    {
        if (!w->done && w->kind == WAIT_LISTEN_ACK && w->channel == (S32)channel)
        {
            finish_listen(w, opened, /*confirmed*/ true, error);
        }
    }
}

void idmcp::stopAllBridgeListens()
{
    if (!g_listens.empty())
    {
        FSLSLBridge::instance().stopListenViaBridge("all");
        g_listens.clear();
    }
}

// ---------------------------------------------------------------------------

void idmcp_register_chat_tools(IDMCPToolRegistry& reg)
{
    // chat.listen ------------------------------------------------------------
    {
        IDMCPTool t;
        t.name = "chat.listen";
        t.description =
            "Open a listen on a chat channel via the LSL bridge so replies from "
            "scripted objects / RLV relays can be captured. Call this BEFORE "
            "chat.send if you want a reply. {\"channel\"} (integer, any channel "
            "incl. negative) and optional {\"seconds\"} (default 30, max 120). The "
            "listen auto-closes at its deadline; still call chat.stopListen when "
            "done — bridge listen slots are limited. Returns {listening, channel, "
            "expires_in}. Read what's heard with chat.replies.";
        t.input_schema = boost::json::parse(
            R"({"type":"object","properties":{"channel":{"type":"integer"},"seconds":{"type":"integer"}},"required":["channel"],"additionalProperties":false})");
        t.invoke = [](const boost::json::object& args, const IDMCPCallPtr& call)
        {
            if (!arg_has(args, "channel"))
            {
                idmcp_tool_err(call, IDMCP_ERR_INVALID_PARAMS, "channel is required");
                return;
            }
            if (!bridge_ready(call)) return;

            const S32 channel = arg_int(args, "channel", 0);
            S32 seconds = arg_int(args, "seconds", ID_LISTEN_DEFAULT_SECS);
            if (seconds <= 0)                   seconds = ID_LISTEN_DEFAULT_SECS;
            if (seconds > ID_LISTEN_MAX_SECS)   seconds = ID_LISTEN_MAX_SECS;

            // Record optimistically; the ack (or timeout) settles the call.
            g_listens[channel].expiry = LLTimer::getTotalSeconds() + seconds;
            FSLSLBridge::instance().listenViaBridge(channel, (F32)seconds);

            auto w = std::make_shared<Wait>();
            w->call     = call;
            w->channel  = channel;
            w->deadline = LLTimer::getTotalSeconds() + 5.0;   // ack window
            w->kind     = WAIT_LISTEN_ACK;
            g_waits.push_back(w);
            ensure_tick();
        };
        reg.add(std::move(t));
    }

    // chat.send --------------------------------------------------------------
    {
        IDMCPTool t;
        t.name = "chat.send";
        t.description =
            "Say a message in-world on a chat channel. By default it goes via the "
            "LSL bridge (reaches scripted objects / RLV relays on any channel, incl. "
            "negative; sent from the bridge object, not your avatar). Set "
            "{\"as_avatar\":true} to instead speak as YOUR avatar (channel 0 / "
            "positive channels only) — use this for normal public chat. "
            "{\"channel\"} (integer), {\"message\"}, optional {\"type\"} "
            "whisper|say(default)|shout (range ~10/20/100m). To get a reply from a "
            "script/relay, chat.listen on the reply channel FIRST (as_avatar is "
            "fire-and-forget, no bridge listen). RLV @sendchat / @sendchannel are "
            "enforced.";
        t.input_schema = boost::json::parse(
            R"({"type":"object","properties":{"channel":{"type":"integer"},"message":{"type":"string"},"type":{"type":"string","enum":["whisper","say","shout"]},"as_avatar":{"type":"boolean"}},"required":["channel","message"],"additionalProperties":false})");
        t.gate = gate_chatsend;
        t.invoke = [](const boost::json::object& args, const IDMCPCallPtr& call)
        {
            if (!arg_has(args, "channel"))
            {
                idmcp_tool_err(call, IDMCP_ERR_INVALID_PARAMS, "channel is required");
                return;
            }
            const std::string message = arg_str(args, "message");
            if (message.empty())
            {
                idmcp_tool_err(call, IDMCP_ERR_INVALID_PARAMS, "message is required");
                return;
            }
            std::string type = arg_str(args, "type");
            if (type.empty()) type = "say";
            if (type != "whisper" && type != "say" && type != "shout")
            {
                idmcp_tool_err(call, IDMCP_ERR_INVALID_PARAMS, "type must be whisper, say, or shout");
                return;
            }
            if (!bridge_chat_enabled())
            {
                idmcp_tool_err(call, IDMCP_ERR_CAP_UNAVAIL,
                               "bridge chat is disabled (set IDMCPBridgeChatEnabled)");
                return;
            }

            const S32  channel   = arg_int(args, "channel", 0);
            const bool as_avatar = arg_bool(args, "as_avatar", false);

            if (as_avatar)
            {
                // Direct viewer chat — speaks as the avatar. Only channel 0 /
                // positive (negative channels aren't chat-sendable by the viewer).
                if (channel < 0)
                {
                    idmcp_tool_err(call, IDMCP_ERR_INVALID_PARAMS,
                                   "as_avatar cannot use a negative channel; those require the bridge");
                    return;
                }
                const EChatType ct = (type == "whisper") ? CHAT_TYPE_WHISPER
                                   : (type == "shout")   ? CHAT_TYPE_SHOUT
                                                         : CHAT_TYPE_NORMAL;
                send_chat_from_viewer(message, ct, channel);   // already RLV-filtered internally too

                boost::json::object o;
                o["sent"]      = true;
                o["as_avatar"] = true;
                idmcp_tool_ok(call, o);
                return;
            }

            // Bridge path (default): sends from the bridge object, works on any channel.
            if (!FSLSLBridge::instance().canUseBridge())
            {
                idmcp_tool_err(call, IDMCP_ERR_CAP_UNAVAIL,
                               "the Firestorm LSL bridge is not available (enable it and wait for it to attach)");
                return;
            }
            FSLSLBridge::instance().sendChatViaBridge(channel, type, message);

            boost::json::object o;
            o["sent"] = true;
            idmcp_tool_ok(call, o);
        };
        reg.add(std::move(t));
    }

    // chat.replies -----------------------------------------------------------
    {
        IDMCPTool t;
        t.name = "chat.replies";
        t.description =
            "Read messages heard on a channel you opened with chat.listen. "
            "{\"channel\"} (integer) and optional {\"wait_seconds\"} (max 30): if "
            "nothing is buffered yet, wait up to that long for the first reply "
            "(good for request/response after chat.send). Drains and returns "
            "{listening, channel, replies:[{from_id, from_name, text, time}]}. "
            "Errors if you are not listening on the channel.";
        t.input_schema = boost::json::parse(
            R"({"type":"object","properties":{"channel":{"type":"integer"},"wait_seconds":{"type":"integer"}},"required":["channel"],"additionalProperties":false})");
        t.invoke = [](const boost::json::object& args, const IDMCPCallPtr& call)
        {
            if (!arg_has(args, "channel"))
            {
                idmcp_tool_err(call, IDMCP_ERR_INVALID_PARAMS, "channel is required");
                return;
            }
            const S32 channel = arg_int(args, "channel", 0);
            auto it = g_listens.find(channel);
            if (it == g_listens.end())
            {
                idmcp_tool_err(call, IDMCP_ERR_NOT_FOUND,
                               "not listening on this channel; call chat.listen first");
                return;
            }

            S32 wait = arg_int(args, "wait_seconds", 0);
            if (!it->second.buffer.empty() || wait <= 0)
            {
                boost::json::object o;
                o["channel"]   = channel;
                o["listening"] = true;
                o["replies"]   = std::move(it->second.buffer);
                it->second.buffer = boost::json::array();
                idmcp_tool_ok(call, o);
                return;
            }
            if (wait > ID_REPLIES_MAX_WAIT) wait = ID_REPLIES_MAX_WAIT;
            F64 deadline = LLTimer::getTotalSeconds() + wait;
            deadline = std::min(deadline, it->second.expiry);   // never outlive the listen

            auto w = std::make_shared<Wait>();
            w->call     = call;
            w->channel  = channel;
            w->deadline = deadline;
            w->kind     = WAIT_REPLIES;
            g_waits.push_back(w);
            ensure_tick();
        };
        reg.add(std::move(t));
    }

    // chat.stopListen --------------------------------------------------------
    {
        IDMCPTool t;
        t.name = "chat.stopListen";
        t.description =
            "Close a chat listen and free its bridge slot. {\"channel\"} (integer) "
            "closes that channel; omit it to close ALL your listens. Always call "
            "this when done listening — leaving listens open can exhaust the "
            "bridge. Returns {stopped}.";
        t.input_schema = boost::json::parse(
            R"({"type":"object","properties":{"channel":{"type":"integer"}},"additionalProperties":false})");
        t.invoke = [](const boost::json::object& args, const IDMCPCallPtr& call)
        {
            if (!FSLSLBridge::instance().canUseBridge())
            {
                idmcp_tool_err(call, IDMCP_ERR_CAP_UNAVAIL, "the Firestorm LSL bridge is not available");
                return;
            }
            if (arg_has(args, "channel"))
            {
                const S32 channel = arg_int(args, "channel", 0);
                FSLSLBridge::instance().stopListenViaBridge(std::to_string(channel));
                g_listens.erase(channel);
                // Resolve any pending replies waits on this channel.
                for (auto& w : g_waits)
                {
                    if (!w->done && w->kind == WAIT_REPLIES && w->channel == channel)
                    {
                        finish_replies(w);
                    }
                }
            }
            else
            {
                FSLSLBridge::instance().stopListenViaBridge("all");
                g_listens.clear();
                for (auto& w : g_waits)
                {
                    if (!w->done && w->kind == WAIT_REPLIES)
                    {
                        finish_replies(w);
                    }
                }
            }
            boost::json::object o;
            o["stopped"] = true;
            idmcp_tool_ok(call, o);
        };
        reg.add(std::move(t));
    }
}
