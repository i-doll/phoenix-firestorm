/**
 * @file idmcptoolregistry.h
 * @brief <ID> MCP server: tool registry (name -> schema + invoke fn).
 *
 * Part of Five's custom Firestorm fork. Custom code carries an `ID` prefix.
 */

#ifndef ID_IDMCPTOOLREGISTRY_H
#define ID_IDMCPTOOLREGISTRY_H

#include <boost/json.hpp>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <vector>

// A single in-flight JSON-RPC call. Defined in idmcpserver.h; here we only need
// the pointer type for the invoke signature, so a forward declaration suffices.
class IDMCPCall;
using IDMCPCallPtr = std::shared_ptr<IDMCPCall>;

// RLV gate: a tool may declare a `gate` that runs before invoke (Request phase)
// and again inside deferred completions before a side effect lands (Commit
// phase). A disallowed result maps to a structured JSON-RPC -32011 error.
enum class IDMCPGatePhase { Request, Commit };

struct IDMCPGateResult
{
    bool               allowed = true;
    std::string        behaviour;   // e.g. "detach" (no leading @)
    boost::json::array sources;      // [{object_id, root_id, attach_pt, name}]
};

// One MCP tool. `invoke` receives the parsed `arguments` object and the call to
// fulfil. Synchronous tools call `call->respond(...)` before returning;
// deferred tools retain `call` and fulfil it later (the connection is flipped to
// SSE by the server when a tool returns without responding).
struct IDMCPTool
{
    std::string       name;
    std::string       description;
    boost::json::value input_schema;   // JSON Schema object for `arguments`
    std::function<void(const boost::json::object& args, const IDMCPCallPtr& call)> invoke;
    std::function<IDMCPGateResult(const boost::json::object& args, IDMCPGatePhase phase)> gate;
};

class IDMCPToolRegistry
{
public:
    void add(IDMCPTool tool)
    {
        mTools[tool.name] = std::move(tool);
    }

    const IDMCPTool* find(const std::string& name) const
    {
        auto it = mTools.find(name);
        return (it == mTools.end()) ? nullptr : &it->second;
    }

    // JSON array of {name, description, inputSchema} for tools/list.
    boost::json::array listForClient() const
    {
        boost::json::array arr;
        for (const auto& [name, tool] : mTools)
        {
            boost::json::object o;
            o["name"] = tool.name;
            o["description"] = tool.description;
            o["inputSchema"] = tool.input_schema;
            arr.push_back(std::move(o));
        }
        return arr;
    }

    bool empty() const { return mTools.empty(); }

private:
    std::map<std::string, IDMCPTool> mTools;
};

#endif // ID_IDMCPTOOLREGISTRY_H
