/**
 * @file idmcpserver.cpp
 * @brief <ID> Embedded MCP server implementation (see header).
 *
 * Part of Five's custom Firestorm fork.
 */

#include "llviewerprecompiledheaders.h"

#include "idmcp.h"
#include "idmcpserver.h"
#include "idmcpconnection.h"

#include "llapr.h"
#include "lltimer.h"
#include "llviewercontrol.h"
#include "llcontrol.h"
#include "llnotificationsutil.h"   // viewer.notify toast

#include "rlvactions.h"
#include "idmcprlvgate.h"
#include "idmcptools.h"

#include <sstream>

namespace
{
    const char* SETTING_ENABLED = "IDMCPServerEnabled";
    const char* SETTING_PORT    = "IDMCPServerPort";
    const F64   DEFAULT_TOOL_TIMEOUT = 30.0;

    std::string json_string(const boost::json::value& v)
    {
        return v.is_string() ? std::string(v.as_string().c_str()) : std::string();
    }
}

// ===========================================================================
// IDMCPCall
// ===========================================================================

IDMCPCall::IDMCPCall(const std::weak_ptr<IDMCPConnection>& conn, boost::json::value id)
    : mConn(conn), mId(std::move(id))
{
}

IDMCPCall::~IDMCPCall()
{
    // Safety net: an abandoned call (never responded, connection gone) must
    // still release any viewer observer it registered.
    runCleanup();
}

void IDMCPCall::respond(boost::json::value result)
{
    if (mResponded)
    {
        return;
    }
    mResponded = true;

    boost::json::object env;
    env["jsonrpc"] = "2.0";
    env["id"]      = mId;
    env["result"]  = std::move(result);

    if (auto c = mConn.lock())
    {
        c->deliver(env);
    }
    runCleanup();
}

void IDMCPCall::respondError(int code, const std::string& message, boost::json::value data)
{
    if (mResponded)
    {
        return;
    }
    mResponded = true;

    boost::json::object err;
    err["code"]    = code;
    err["message"] = message;
    if (!data.is_null())
    {
        err["data"] = std::move(data);
    }

    boost::json::object env;
    env["jsonrpc"] = "2.0";
    env["id"]      = mId;
    env["error"]   = std::move(err);

    if (auto c = mConn.lock())
    {
        c->deliver(env);
    }
    runCleanup();
}

void IDMCPCall::runCleanup()
{
    if (!mCleanedUp && mCleanup)
    {
        mCleanedUp = true;
        mCleanup();
    }
    mCleanedUp = true;
}

// ===========================================================================
// Tool-result helpers
// ===========================================================================

void idmcp_tool_ok(const IDMCPCallPtr& call, boost::json::value structured)
{
    boost::json::object text;
    text["type"] = "text";
    text["text"] = boost::json::serialize(structured);

    boost::json::array content;
    content.push_back(std::move(text));

    boost::json::object result;
    result["content"]           = std::move(content);
    result["structuredContent"] = structured;
    result["isError"]           = false;

    call->respond(std::move(result));
}

void idmcp_tool_err(const IDMCPCallPtr& call, int code, const std::string& message,
                    boost::json::value data)
{
    call->respondError(code, message, std::move(data));
}

// ===========================================================================
// IDMCPServer
// ===========================================================================

IDMCPServer::IDMCPServer()
{
}

IDMCPServer::~IDMCPServer()
{
    stop();
}

void IDMCPServer::initSingleton()
{
    registerCoreTools();
    idmcp_register_rlv_tools(mRegistry);
    idmcp_register_inventory_tools(mRegistry);
    idmcp_register_appearance_tools(mRegistry);
    idmcp_register_gesture_tools(mRegistry);
    idmcp_register_wearable_tools(mRegistry);
    idmcp_register_profile_tools(mRegistry);
    idmcp_register_search_tools(mRegistry);
    idmcp_register_avatars_tools(mRegistry);
    idmcp_register_manage_tools(mRegistry);
    idmcp_register_group_tools(mRegistry);
    idmcp_register_upload_tools(mRegistry);
    idmcp_register_chat_tools(mRegistry);
    idmcp_register_movement_tools(mRegistry);
    idmcp_register_vision_tools(mRegistry);
    idmcp_register_im_tools(mRegistry);
    idmcp_register_notifications_tools(mRegistry);
    idmcp_register_money_tools(mRegistry);

    // Watch the enable setting so toggling it mid-session starts/stops the
    // server live (e.g. from the Debug Settings floater).
    LLPointer<LLControlVariable> ctrl = gSavedSettings.getControl(SETTING_ENABLED);
    if (ctrl.notNull())
    {
        mEnableSignal = ctrl->getSignal()->connect(
            [](LLControlVariable*, const LLSD& newval, const LLSD&)
            {
                if (newval.asBoolean())
                {
                    IDMCPServer::instance().start();
                }
                else
                {
                    IDMCPServer::instance().stop();
                }
            });
    }
}

void IDMCPServer::start()
{
    if (mRunning)
    {
        return;
    }

    U16 port = (U16)gSavedSettings.getU32(SETTING_PORT);
    if (!bindListener(port))
    {
        LL_WARNS("IDMCP") << "MCP server failed to bind 127.0.0.1:" << port
                          << " - disabling." << LL_ENDL;
        gSavedSettings.setBOOL(SETTING_ENABLED, false);
        return;
    }

    mMainloopConn = LLEventPumps::instance().obtain("mainloop")
        .listen("IDMCPServer", boost::bind(&IDMCPServer::pollTick, this, _1));

    mRunning = true;
    LL_INFOS("IDMCP") << "MCP server listening on 127.0.0.1:" << port << LL_ENDL;
}

void IDMCPServer::stop()
{
    if (!mRunning)
    {
        return;
    }
    mRunning = false;

    // <ID> release any bridge chat listens so a scarce llListen slot never leaks.
    idmcp::stopAllBridgeListens();

    // Fail in-flight deferred calls first so their cleanup (observer removal)
    // runs before we tear down connections.
    for (auto& weak : mDeferred)
    {
        if (auto call = weak.lock())
        {
            call->respondError(IDMCP_ERR_TIMEOUT, "server stopping");
        }
    }
    mDeferred.clear();

    mConnections.clear();
    mListenSocket.reset();
    mMainloopConn = LLTempBoundListener();     // disconnect the per-frame poll

    LL_INFOS("IDMCP") << "MCP server stopped." << LL_ENDL;
}

bool IDMCPServer::bindListener(U16 port)
{
    apr_status_t status = APR_SUCCESS;
    apr_sockaddr_t* addr = NULL;

    mListenSocket = LLSocket::create(gAPRPoolp, LLSocket::STREAM_TCP);
    if (!mListenSocket)
    {
        return false;
    }

    status = apr_sockaddr_info_get(&addr, "127.0.0.1", APR_INET, port, 0, gAPRPoolp);
    if (ll_apr_warn_status(status))
    {
        mListenSocket.reset();
        return false;
    }

    ll_apr_warn_status(apr_socket_opt_set(mListenSocket->getSocket(), APR_SO_REUSEADDR, 1));

    status = apr_socket_bind(mListenSocket->getSocket(), addr);
    if (ll_apr_warn_status(status))
    {
        mListenSocket.reset();
        return false;
    }

    status = apr_socket_opt_set(mListenSocket->getSocket(), APR_SO_NONBLOCK, 1);
    if (ll_apr_warn_status(status))
    {
        mListenSocket.reset();
        return false;
    }
    apr_socket_timeout_set(mListenSocket->getSocket(), 0);

    status = apr_socket_listen(mListenSocket->getSocket(), 10);
    if (ll_apr_warn_status(status))
    {
        mListenSocket.reset();
        return false;
    }

    return true;
}

bool IDMCPServer::pollTick(const LLSD&)
{
    if (!mRunning)
    {
        return false;
    }
    acceptConnections();
    pumpConnections();
    sweepTimeouts();
    return false;                        // don't consume the mainloop event
}

void IDMCPServer::acceptConnections()
{
    for (int i = 0; i < MAX_ACCEPTS_PER_FRAME; ++i)
    {
        if (mConnections.size() >= MAX_CONNECTIONS)
        {
            break;                       // at capacity; leave in the listen backlog
        }

        apr_socket_t* new_socket = NULL;
        apr_status_t status = apr_socket_accept(&new_socket, mListenSocket->getSocket(), gAPRPoolp);

        if (status == APR_SUCCESS)
        {
            // The LLSocket takes ownership of a fresh pool and frees it on destruction.
            apr_pool_t* new_pool = NULL;
            if (ll_apr_warn_status(apr_pool_create(&new_pool, gAPRPoolp)))
            {
                break;
            }
            LLSocket::ptr_t sock = LLSocket::create(new_socket, new_pool);
            if (sock)
            {
                mConnections.push_back(std::make_shared<IDMCPConnection>(sock));
            }
        }
        else if (APR_STATUS_IS_EAGAIN(status))
        {
            break;                       // no pending connections
        }
        else
        {
            ll_apr_warn_status(status);
            break;
        }
    }
}

void IDMCPServer::pumpConnections()
{
    for (auto it = mConnections.begin(); it != mConnections.end(); )
    {
        bool alive = (*it)->pump(MAX_READ_PER_CONN, MAX_WRITE_PER_CONN);
        if (alive)
        {
            ++it;
        }
        else
        {
            it = mConnections.erase(it);
        }
    }
}

void IDMCPServer::sweepTimeouts()
{
    if (mDeferred.empty())
    {
        return;
    }
    const F64 now = LLTimer::getTotalSeconds();

    for (auto it = mDeferred.begin(); it != mDeferred.end(); )
    {
        auto call = it->lock();
        if (!call || call->responded())
        {
            it = mDeferred.erase(it);
            continue;
        }
        if (call->deadline() > 0.0 && now > call->deadline())
        {
            call->respondError(IDMCP_ERR_TIMEOUT, "tool timed out");
            it = mDeferred.erase(it);
            continue;
        }
        ++it;
    }
}

void IDMCPServer::trackDeferred(const IDMCPCallPtr& call)
{
    mDeferred.push_back(call);
}

// ---------------------------------------------------------------------------
// JSON-RPC routing

void IDMCPServer::handleRequest(const std::shared_ptr<IDMCPConnection>& conn,
                                const std::string& body)
{
    boost::json::value parsed;
    boost::system::error_code ec;
    parsed = boost::json::parse(body, ec);
    if (ec)
    {
        // Parse error: id is unknown, respond with null id per JSON-RPC.
        auto call = std::make_shared<IDMCPCall>(conn, boost::json::value());
        call->respondError(IDMCP_ERR_PARSE, "parse error");
        return;
    }
    if (!parsed.is_object())
    {
        auto call = std::make_shared<IDMCPCall>(conn, boost::json::value());
        call->respondError(IDMCP_ERR_INVALID_REQ, "expected a JSON-RPC object");
        return;
    }

    const boost::json::object& obj = parsed.as_object();

    const std::string method =
        obj.contains("method") ? json_string(obj.at("method")) : std::string();

    // A request without an id is a notification (e.g. notifications/initialized):
    // acknowledge, produce no result.
    if (!obj.contains("id"))
    {
        conn->sendAccepted();
        return;
    }

    auto call = std::make_shared<IDMCPCall>(conn, obj.at("id"));

    if (method == "initialize")
    {
        boost::json::object toolscap;   toolscap["listChanged"] = false;
        boost::json::object caps;       caps["tools"] = std::move(toolscap);
        boost::json::object info;       info["name"] = "firestorm-id-mcp"; info["version"] = "0.1";
        boost::json::object result;
        result["protocolVersion"] = "2025-06-18";
        result["capabilities"]    = std::move(caps);
        result["serverInfo"]      = std::move(info);
        call->respond(std::move(result));
        return;
    }

    if (method == "tools/list")
    {
        boost::json::object result;
        result["tools"] = mRegistry.listForClient();
        call->respond(std::move(result));
        return;
    }

    if (method == "tools/call")
    {
        const boost::json::object* params =
            (obj.contains("params") && obj.at("params").is_object())
                ? &obj.at("params").as_object() : nullptr;

        const std::string name =
            (params && params->contains("name")) ? json_string(params->at("name")) : std::string();

        const IDMCPTool* tool = mRegistry.find(name);
        if (!tool)
        {
            call->respondError(IDMCP_ERR_METHOD_MISSING, "unknown tool: " + name);
            return;
        }

        boost::json::object args;
        if (params && params->contains("arguments") && params->at("arguments").is_object())
        {
            args = params->at("arguments").as_object();
        }

        // RLV gate (request phase): deny with a structured -32011 naming the
        // restriction and its source object(s).
        if (tool->gate)
        {
            IDMCPGateResult g = tool->gate(args, IDMCPGatePhase::Request);
            if (!g.allowed)
            {
                boost::json::object data;
                data["restriction"] = g.behaviour;
                data["sources"]     = g.sources;
                data["checkedAt"]   = "request";
                call->respondError(IDMCP_ERR_RLV_RESTRICTED,
                                   "blocked by RLV restriction @" + g.behaviour,
                                   std::move(data));
                return;
            }
        }

        tool->invoke(args, call);

        // If the tool did not respond synchronously it is deferred: open the SSE
        // stream and register it for timeout sweeping.
        if (!call->responded())
        {
            conn->beginDeferred();
            const F64 now = LLTimer::getTotalSeconds();
            call->setDeadline(now + DEFAULT_TOOL_TIMEOUT);
            trackDeferred(call);
        }
        return;
    }

    call->respondError(IDMCP_ERR_METHOD_MISSING, "unknown method: " + method);
}

// ---------------------------------------------------------------------------
// Core tools (skeleton). Feature tools are added by idmcptools_*.cpp.

// ---------------------------------------------------------------------------
// Stable facade (idmcp.h) — keeps heavy MCP headers out of the big viewer TUs.

void idmcp::startupInit()
{
    IDMCPServer::instance();   // installs the enable-setting watcher
    if (gSavedSettings.getBOOL("IDMCPServerEnabled"))
    {
        IDMCPServer::instance().start();
    }
}

void idmcp::shutdown()
{
    if (IDMCPServer::instanceExists())
    {
        IDMCPServer::instance().stop();
    }
}

// ---------------------------------------------------------------------------

void IDMCPServer::registerCoreTools()
{
    {
        IDMCPTool ping;
        ping.name = "ping";
        ping.description = "Liveness check. Returns {\"pong\": true}.";
        ping.input_schema = boost::json::parse(
            R"({"type":"object","properties":{},"additionalProperties":false})");
        ping.invoke = [](const boost::json::object&, const IDMCPCallPtr& call)
        {
            boost::json::object s; s["pong"] = true;
            idmcp_tool_ok(call, s);
        };
        mRegistry.add(std::move(ping));
    }

    {
        IDMCPTool status;
        status.name = "rlv.getStatus";
        status.description = "Whether RLVa restriction enforcement is currently enabled.";
        status.input_schema = boost::json::parse(
            R"({"type":"object","properties":{},"additionalProperties":false})");
        status.invoke = [](const boost::json::object&, const IDMCPCallPtr& call)
        {
            boost::json::object s; s["enabled"] = RlvActions::isRlvEnabled();
            idmcp_tool_ok(call, s);
        };
        mRegistry.add(std::move(status));
    }

    {
        IDMCPTool t;
        t.name = "viewer.notify";
        t.description =
            "Post a message to the viewer as a toast, to get the user's attention. "
            "{\"text\"} (required) is the body; optional {\"title\"} is shown on the "
            "first line. By default the toast persists (stays on screen until "
            "dismissed and is kept in the notification well); pass {\"tip\":true} for "
            "a lighter tip toast that fades on its own.";
        t.input_schema = boost::json::parse(
            R"({"type":"object","properties":{"text":{"type":"string"},"title":{"type":"string"},"tip":{"type":"boolean"}},"required":["text"],"additionalProperties":false})");
        t.invoke = [](const boost::json::object& args, const IDMCPCallPtr& call)
        {
            auto tit = args.find("text");
            std::string text = (tit != args.end() && tit->value().is_string())
                                   ? std::string(tit->value().as_string().c_str()) : std::string();
            if (text.empty())
            {
                idmcp_tool_err(call, IDMCP_ERR_INVALID_PARAMS, "text is required");
                return;
            }
            auto ttl = args.find("title");
            const std::string title = (ttl != args.end() && ttl->value().is_string())
                                          ? std::string(ttl->value().as_string().c_str()) : std::string();
            auto tp = args.find("tip");
            const bool tip = (tp != args.end() && tp->value().is_bool()) && tp->value().as_bool();

            LLSD sd;
            sd["MESSAGE"] = title.empty() ? text : (title + "\n" + text);
            LLNotificationsUtil::add(tip ? "SystemMessageTip" : "SystemMessage", sd);

            boost::json::object o; o["shown"] = true;
            idmcp_tool_ok(call, o);
        };
        mRegistry.add(std::move(t));
    }
}
