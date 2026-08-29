/**
 * @file idmcpconnection.h
 * @brief <ID> MCP server: one accepted loopback connection.
 *
 * Part of Five's custom Firestorm fork. Custom code carries an `ID` prefix.
 *
 * Incremental, non-blocking HTTP/1.1 handling for the MCP Streamable-HTTP
 * transport: parse one request (request line, headers, Content-Length body),
 * validate it is a local POST to /mcp, hand the body to IDMCPServer, then write
 * either an immediate application/json response or (for deferred tools) a single
 * text/event-stream data frame. One request per connection (Connection: close).
 * All I/O is bounded per frame.
 */

#ifndef ID_IDMCPCONNECTION_H
#define ID_IDMCPCONNECTION_H

#include "lliosocket.h"
#include <boost/json.hpp>
#include <memory>
#include <string>

class IDMCPConnection : public std::enable_shared_from_this<IDMCPConnection>
{
public:
    explicit IDMCPConnection(LLSocket::ptr_t socket);
    ~IDMCPConnection();

    // Bounded per-frame read/parse/write. Returns false once the connection is
    // finished and the server should drop it.
    bool pump(std::size_t max_read, std::size_t max_write);

    // Deliver a completed JSON-RPC response value. Immediate mode ->
    // application/json; SSE mode -> one data frame. Closes after drain.
    void deliver(const boost::json::value& jsonrpc_response);

    // Flip to SSE (deferred) mode: emit SSE response headers now, hold open.
    void beginDeferred();
    bool isDeferred() const { return mMode == Mode::eSSE; }

    // JSON-RPC notification (no id): acknowledge with 202 and close.
    void sendAccepted();

private:
    enum class Mode { eImmediate, eSSE };

    void doRead(std::size_t max_read);
    void doWrite(std::size_t max_write);
    void tryParseAndDispatch();
    void queueRaw(std::string bytes);
    void queueHttpError(int status, const char* reason, const std::string& text);
    static bool hostIsLocal(const std::string& host);

    LLSocket::ptr_t mSocket;
    std::string     mIn;
    std::string     mOut;
    std::size_t     mOutCursor = 0;

    bool mDispatched   = false;   // request handed to the server
    bool mHeadersSent  = false;   // response status line already emitted
    bool mCloseOnDrain = false;   // close once mOut fully written
    bool mClosed       = false;   // socket dead / finished
    bool mPeerClosed   = false;   // EOF from peer

    Mode mMode = Mode::eImmediate;

    static constexpr std::size_t MAX_REQUEST_BYTES = 1024 * 1024;
};

#endif // ID_IDMCPCONNECTION_H
