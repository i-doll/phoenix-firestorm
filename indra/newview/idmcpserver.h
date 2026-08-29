/**
 * @file idmcpserver.h
 * @brief <ID> Embedded MCP (Model Context Protocol) server.
 *
 * Part of Five's custom Firestorm fork. Custom code carries an `ID` prefix.
 *
 * Runs entirely on the main thread: a non-blocking loopback TCP listener polled
 * once per frame off the "mainloop" event pump (same idiom as
 * LLPluginProcessParent). All work is strictly bounded per frame so the render
 * loop is never stalled; long viewer operations complete asynchronously and
 * their completion writes a deferred JSON-RPC response over a held SSE
 * connection. Because everything is main-thread, no locking is needed to touch
 * viewer state (inventory/appearance/profile/RLV).
 *
 * Transport: minimal MCP Streamable-HTTP subset on 127.0.0.1, off by default,
 * no auth (see IDMCPServerEnabled / IDMCPServerPort). Localhost-only bind plus
 * Host/Origin checks are the only guards; enabling grants any local process full
 * control of the avatar.
 */

#ifndef ID_IDMCPSERVER_H
#define ID_IDMCPSERVER_H

#include "llsingleton.h"
#include "llevents.h"
#include "lliosocket.h"
#include "idmcptoolregistry.h"

#include <boost/json.hpp>
#include <functional>
#include <memory>
#include <vector>

class IDMCPConnection;

// -----------------------------------------------------------------------------
// IDMCPCall: a single in-flight JSON-RPC request, fulfilled exactly once.
//
// Holds a weak link to its connection so a completion that arrives after the
// client hung up (or after server shutdown) is a safe no-op. Tools that register
// viewer observers/callbacks attach a cleanup functor via setCleanup(); it runs
// exactly once when the call is settled or abandoned (completion, timeout,
// connection close, or server stop).
// -----------------------------------------------------------------------------
class IDMCPCall
{
public:
    IDMCPCall(const std::weak_ptr<IDMCPConnection>& conn, boost::json::value id);
    ~IDMCPCall();

    void respond(boost::json::value result);
    void respondError(int code, const std::string& message,
                      boost::json::value data = boost::json::value());

    bool responded() const { return mResponded; }
    const boost::json::value& id() const { return mId; }

    void setCleanup(std::function<void()> fn) { mCleanup = std::move(fn); }
    void runCleanup();

    // Monotonic deadline (seconds, LLTimer::getTotalSeconds clock); 0 = none.
    F64  deadline() const { return mDeadline; }
    void setDeadline(F64 t) { mDeadline = t; }

private:
    std::weak_ptr<IDMCPConnection> mConn;
    boost::json::value             mId;
    bool                           mResponded = false;
    bool                           mCleanedUp = false;
    F64                            mDeadline = 0.0;
    std::function<void()>          mCleanup;
};

// JSON-RPC / MCP error codes (implementation-defined -32000 range).
enum EIDMCPError
{
    IDMCP_ERR_PARSE          = -32700,
    IDMCP_ERR_INVALID_REQ    = -32600,
    IDMCP_ERR_METHOD_MISSING = -32601,
    IDMCP_ERR_INVALID_PARAMS = -32602,
    IDMCP_ERR_TIMEOUT        = -32001,
    IDMCP_ERR_NOT_FOUND      = -32002,
    IDMCP_ERR_NOT_LOGGED_IN  = -32003,
    IDMCP_ERR_PERMISSION     = -32004,
    IDMCP_ERR_CAP_UNAVAIL    = -32005,
    IDMCP_ERR_RLV_RESTRICTED = -32011,
};

// -----------------------------------------------------------------------------
// IDMCPServer
// -----------------------------------------------------------------------------
class IDMCPServer : public LLSingleton<IDMCPServer>
{
    LLSINGLETON(IDMCPServer);
    LOG_CLASS(IDMCPServer);

public:
    ~IDMCPServer();

    // Bind the loopback listener and begin the per-frame poll. Idempotent.
    void start();
    // Close the listener + all connections, fail all in-flight calls, and drop
    // the mainloop listener. Idempotent.
    void stop();
    bool isRunning() const { return mRunning; }

    IDMCPToolRegistry& registry() { return mRegistry; }

    // Called by a connection once a full HTTP request body has arrived. Parses
    // JSON-RPC, routes, and either responds immediately or flips the connection
    // to deferred (SSE) mode.
    void handleRequest(const std::shared_ptr<IDMCPConnection>& conn,
                       const std::string& body);

    // Register a deferred call for timeout sweeping.
    void trackDeferred(const IDMCPCallPtr& call);

private:
    void initSingleton() override;   // install the enable-setting watcher
    bool bindListener(U16 port);
    bool pollTick(const LLSD&);      // once per frame; returns false (don't consume)
    void acceptConnections();
    void pumpConnections();
    void sweepTimeouts();

    void registerCoreTools();

    LLSocket::ptr_t                                   mListenSocket;
    std::vector<std::shared_ptr<IDMCPConnection>>     mConnections;
    std::vector<std::weak_ptr<IDMCPCall>>             mDeferred;
    IDMCPToolRegistry                                 mRegistry;
    LLTempBoundListener                               mMainloopConn;
    boost::signals2::scoped_connection                mEnableSignal;
    bool                                              mRunning = false;

    // Per-frame budgets (never stall a frame).
    static constexpr int    MAX_ACCEPTS_PER_FRAME = 4;
    static constexpr size_t MAX_CONNECTIONS       = 8;
    static constexpr size_t MAX_READ_PER_CONN     = 64 * 1024;
    static constexpr size_t MAX_WRITE_PER_CONN    = 64 * 1024;
};

// -----------------------------------------------------------------------------
// Tool-result helpers: wrap a tool's structured result (or error) in the MCP
// tools/call response shape. Successful results carry both a human-readable
// `content` text block and machine-readable `structuredContent`.
// -----------------------------------------------------------------------------
void idmcp_tool_ok(const IDMCPCallPtr& call, boost::json::value structured);
void idmcp_tool_err(const IDMCPCallPtr& call, int code, const std::string& message,
                    boost::json::value data = boost::json::value());

#endif // ID_IDMCPSERVER_H
