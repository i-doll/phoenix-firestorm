/**
 * @file idmcpconnection.cpp
 * @brief <ID> MCP server: one accepted loopback connection (see header).
 *
 * Part of Five's custom Firestorm fork.
 */

#include "llviewerprecompiledheaders.h"

#include "idmcpconnection.h"
#include "idmcpserver.h"

#include "llapr.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <sstream>

// ---------------------------------------------------------------------------

IDMCPConnection::IDMCPConnection(LLSocket::ptr_t socket)
    : mSocket(socket)
{
    if (mSocket)
    {
        // Non-blocking, zero timeout: recv/send never wait on a frame.
        mSocket->setNonBlocking();
    }
}

IDMCPConnection::~IDMCPConnection()
{
}

// ---------------------------------------------------------------------------

bool IDMCPConnection::pump(std::size_t max_read, std::size_t max_write)
{
    if (mClosed || !mSocket)
    {
        return false;
    }

    doRead(max_read);

    if (!mDispatched && !mClosed)
    {
        tryParseAndDispatch();
    }

    doWrite(max_write);

    // Peer hung up: finish once anything buffered is flushed. If they hung up
    // before we even had a full request, drop immediately.
    if (mPeerClosed && (mOutCursor >= mOut.size() || !mDispatched))
    {
        mClosed = true;
    }

    return !mClosed;
}

void IDMCPConnection::doRead(std::size_t max_read)
{
    if (mPeerClosed)
    {
        return;
    }

    std::size_t total = 0;
    char buf[8192];
    while (total < max_read)
    {
        apr_size_t len = (apr_size_t)std::min(sizeof(buf), max_read - total);
        apr_status_t st = apr_socket_recv(mSocket->getSocket(), buf, &len);

        if (len > 0)
        {
            mIn.append(buf, len);
            total += len;
        }

        if (st == APR_SUCCESS)
        {
            // More may be available; keep reading until EAGAIN or budget out.
            if (len == 0)
            {
                break;
            }
            continue;
        }
        if (APR_STATUS_IS_EAGAIN(st))
        {
            break;                       // nothing more right now
        }
        if (APR_STATUS_IS_EOF(st))
        {
            mPeerClosed = true;
            break;
        }
        // Any other status is a socket error.
        mClosed = true;
        break;
    }

    if (mIn.size() > MAX_REQUEST_BYTES)
    {
        queueHttpError(413, "Payload Too Large", "request too large");
    }
}

void IDMCPConnection::doWrite(std::size_t max_write)
{
    if (mOutCursor >= mOut.size())
    {
        if (mCloseOnDrain)
        {
            mClosed = true;
        }
        return;
    }

    std::size_t total = 0;
    while (mOutCursor < mOut.size() && total < max_write)
    {
        apr_size_t len = (apr_size_t)std::min(mOut.size() - mOutCursor, max_write - total);
        apr_status_t st = apr_socket_send(mSocket->getSocket(), mOut.data() + mOutCursor, &len);
        mOutCursor += len;
        total += len;

        if (st == APR_SUCCESS)
        {
            continue;
        }
        if (APR_STATUS_IS_EAGAIN(st))
        {
            break;                       // socket buffer full; resume next frame
        }
        // Error (incl. peer reset).
        mClosed = true;
        return;
    }

    if (mOutCursor >= mOut.size() && mCloseOnDrain)
    {
        mClosed = true;
    }
}

// ---------------------------------------------------------------------------
// HTTP request parsing (one request per connection).

namespace
{
    std::string to_lower(std::string s)
    {
        std::transform(s.begin(), s.end(), s.begin(),
                       [](unsigned char c){ return (char)std::tolower(c); });
        return s;
    }

    std::string trim(const std::string& s)
    {
        std::size_t b = s.find_first_not_of(" \t");
        std::size_t e = s.find_last_not_of(" \t\r\n");
        return (b == std::string::npos) ? std::string() : s.substr(b, e - b + 1);
    }
}

void IDMCPConnection::tryParseAndDispatch()
{
    const std::size_t header_end = mIn.find("\r\n\r\n");
    if (header_end == std::string::npos)
    {
        return;                          // headers not complete yet
    }

    const std::string head = mIn.substr(0, header_end);

    // Request line: METHOD SP PATH SP HTTP/x.y
    std::size_t line_end = head.find("\r\n");
    const std::string request_line = head.substr(0, line_end);
    std::size_t sp1 = request_line.find(' ');
    std::size_t sp2 = (sp1 == std::string::npos) ? std::string::npos : request_line.find(' ', sp1 + 1);
    if (sp1 == std::string::npos || sp2 == std::string::npos)
    {
        queueHttpError(400, "Bad Request", "malformed request line");
        return;
    }
    const std::string method = request_line.substr(0, sp1);
    const std::string path   = request_line.substr(sp1 + 1, sp2 - sp1 - 1);

    // Headers of interest.
    long content_length = 0;
    std::string host, origin;
    std::size_t pos = (line_end == std::string::npos) ? head.size() : line_end + 2;
    while (pos < head.size())
    {
        std::size_t nl = head.find("\r\n", pos);
        std::string line = head.substr(pos, (nl == std::string::npos) ? std::string::npos : nl - pos);
        std::size_t colon = line.find(':');
        if (colon != std::string::npos)
        {
            std::string key = to_lower(trim(line.substr(0, colon)));
            std::string val = trim(line.substr(colon + 1));
            if (key == "content-length")  content_length = std::max(0L, atol(val.c_str()));
            else if (key == "host")       host = val;
            else if (key == "origin")     origin = val;
        }
        if (nl == std::string::npos) break;
        pos = nl + 2;
    }

    // Security: loopback only. Reject non-local Host, and any non-local Origin
    // (DNS-rebinding defense per the MCP Streamable-HTTP transport guidance).
    if (!hostIsLocal(host))
    {
        queueHttpError(403, "Forbidden", "non-local Host");
        return;
    }
    if (!origin.empty() && origin != "null" && !hostIsLocal(origin))
    {
        queueHttpError(403, "Forbidden", "non-local Origin");
        return;
    }

    if (path != "/mcp" && path != "/")
    {
        queueHttpError(404, "Not Found", "unknown path");
        return;
    }
    if (method != "POST")
    {
        // GET (SSE server-notification stream) is a v3 feature; not yet.
        queueHttpError(405, "Method Not Allowed", "expected POST");
        return;
    }

    // Wait for the full body.
    const std::size_t body_start = header_end + 4;
    if (content_length > 0 &&
        mIn.size() < body_start + (std::size_t)content_length)
    {
        return;                          // body not fully arrived yet
    }

    const std::string body =
        (content_length > 0) ? mIn.substr(body_start, (std::size_t)content_length)
                             : std::string();

    mDispatched = true;
    IDMCPServer::instance().handleRequest(shared_from_this(), body);
}

// ---------------------------------------------------------------------------
// Response emission.

void IDMCPConnection::queueRaw(std::string bytes)
{
    mOut += bytes;
}

void IDMCPConnection::deliver(const boost::json::value& jsonrpc_response)
{
    const std::string payload = boost::json::serialize(jsonrpc_response);

    if (mMode == Mode::eSSE)
    {
        // Stream already opened by beginDeferred(); emit one data frame then end.
        queueRaw("data: " + payload + "\n\n");
    }
    else
    {
        std::ostringstream resp;
        resp << "HTTP/1.1 200 OK\r\n"
             << "Content-Type: application/json\r\n"
             << "Content-Length: " << payload.size() << "\r\n"
             << "Connection: close\r\n"
             << "\r\n"
             << payload;
        queueRaw(resp.str());
        mHeadersSent = true;
    }
    mCloseOnDrain = true;
}

void IDMCPConnection::beginDeferred()
{
    if (mHeadersSent)
    {
        return;
    }
    queueRaw("HTTP/1.1 200 OK\r\n"
             "Content-Type: text/event-stream\r\n"
             "Cache-Control: no-cache\r\n"
             "Connection: close\r\n"
             "\r\n");
    mHeadersSent = true;
    mMode = Mode::eSSE;
}

void IDMCPConnection::sendAccepted()
{
    queueRaw("HTTP/1.1 202 Accepted\r\n"
             "Content-Length: 0\r\n"
             "Connection: close\r\n"
             "\r\n");
    mHeadersSent = true;
    mCloseOnDrain = true;
}

void IDMCPConnection::queueHttpError(int status, const char* reason, const std::string& text)
{
    boost::json::object body;
    body["error"] = text;
    const std::string payload = boost::json::serialize(body);

    std::ostringstream resp;
    resp << "HTTP/1.1 " << status << " " << reason << "\r\n"
         << "Content-Type: application/json\r\n"
         << "Content-Length: " << payload.size() << "\r\n"
         << "Connection: close\r\n"
         << "\r\n"
         << payload;
    queueRaw(resp.str());
    mHeadersSent = true;
    mCloseOnDrain = true;
    mDispatched = true;                  // don't try to parse further
}

bool IDMCPConnection::hostIsLocal(const std::string& host)
{
    if (host.empty())
    {
        return true;                     // HTTP/1.0-style, no Host — allow (loopback socket)
    }
    // Strip a trailing :port.
    std::string h = host;
    std::size_t colon = h.rfind(':');
    if (colon != std::string::npos && h.find(']') == std::string::npos)
    {
        h = h.substr(0, colon);
    }
    return (h == "127.0.0.1" || h == "localhost" || h == "[::1]" || h == "::1");
}
