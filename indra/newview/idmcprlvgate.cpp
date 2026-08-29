/**
 * @file idmcprlvgate.cpp
 * @brief <ID> MCP server: RLVa enforcement gate + reporting (see header).
 *
 * Part of Five's custom Firestorm fork.
 */

#include "llviewerprecompiledheaders.h"

#include "idmcprlvgate.h"
#include "idmcpserver.h"

#include "rlvhandler.h"
#include "rlvhelper.h"
#include "rlvactions.h"

#include "llviewerobject.h"
#include "llviewerobjectlist.h"     // gObjectList
#include "llviewerinventory.h"      // LLViewerInventoryItem
#include "llinventorymodel.h"       // gInventory
#include "llavatarnamecache.h"
#include "llavatarname.h"

namespace
{
    // All objects that currently impose `bhvr`, as JSON source records.
    boost::json::array attribute(ERlvBehaviour bhvr)
    {
        boost::json::array sources;
        for (const auto& entry : gRlvHandler.getObjectMap())
        {
            const RlvObject& obj = entry.second;
            bool matches = false;
            for (const RlvCommand& cmd : obj.getCommandList())
            {
                if (cmd.getBehaviourType() == bhvr)
                {
                    matches = true;
                    break;
                }
            }
            if (!matches)
            {
                continue;
            }
            boost::json::object s;
            s["object_id"] = obj.getObjectID().asString();
            s["root_id"]   = obj.getRootID().asString();
            s["attach_pt"] = obj.getAttachPt();
            s["name"]      = IDMCPRlvGate::sourceName(obj.getObjectID());
            sources.push_back(std::move(s));
        }
        return sources;
    }
}

// ---------------------------------------------------------------------------

bool IDMCPRlvGate::isEnabled()
{
    return RlvActions::isRlvEnabled();
}

IDMCPGateResult IDMCPRlvGate::deny(ERlvBehaviour bhvr, const char* label)
{
    IDMCPGateResult r;
    r.allowed   = false;
    r.behaviour = label;
    r.sources   = attribute(bhvr);
    return r;
}

IDMCPGateResult IDMCPRlvGate::checkBehaviour(ERlvBehaviour bhvr, const char* label)
{
    if (!RlvActions::isRlvEnabled())
    {
        return IDMCPGateResult();      // allowed
    }
    if (gRlvHandler.hasBehaviour(bhvr))
    {
        return deny(bhvr, label);
    }
    return IDMCPGateResult();          // allowed
}

std::string IDMCPRlvGate::sourceName(const LLUUID& idObj)
{
    const LLViewerObject* pObj = gObjectList.findObject(idObj);
    if (pObj && pObj->isAvatar())
    {
        LLAvatarName avName;
        if (LLAvatarNameCache::get(idObj, &avName))
        {
            return avName.getCompleteName();
        }
    }
    const LLViewerObject* pRoot = pObj ? pObj->getRootEdit() : nullptr;
    if (pRoot && pRoot->isAttachment())
    {
        const LLViewerInventoryItem* pItem = gInventory.getItem(pRoot->getAttachmentItemID());
        if (pItem)
        {
            return pItem->getName();
        }
    }
    return idObj.asString();
}

boost::json::value IDMCPRlvGate::report(const std::string& filter)
{
    boost::json::object out;
    out["enabled"] = RlvActions::isRlvEnabled();

    boost::json::array arr;
    for (const auto& entry : gRlvHandler.getObjectMap())
    {
        const RlvObject& obj = entry.second;
        for (const RlvCommand& cmd : obj.getCommandList())
        {
            if (!filter.empty() && cmd.getBehaviour().find(filter) == std::string::npos)
            {
                continue;
            }
            boost::json::object c;
            c["behaviour"] = cmd.getBehaviour();
            c["option"]    = cmd.getOption();
            c["param"]     = cmd.getParam();
            c["strict"]    = cmd.isStrict();
            c["command"]   = cmd.asString();

            boost::json::object src;
            src["object_id"] = obj.getObjectID().asString();
            src["root_id"]   = obj.getRootID().asString();
            src["attach_pt"] = obj.getAttachPt();
            src["name"]      = sourceName(obj.getObjectID());
            c["source"] = std::move(src);

            arr.push_back(std::move(c));
        }
    }
    out["restrictions"] = std::move(arr);
    return out;
}

// ---------------------------------------------------------------------------

void idmcp_register_rlv_tools(IDMCPToolRegistry& reg)
{
    IDMCPTool t;
    t.name = "rlv.getRestrictions";
    t.description =
        "List all active RLVa restrictions with their source objects. Optional "
        "{\"filter\"} substring-matches the behaviour name. Consult this to know "
        "which actions are currently blocked before attempting them.";
    t.input_schema = boost::json::parse(
        R"({"type":"object","properties":{"filter":{"type":"string"}},"additionalProperties":false})");
    t.invoke = [](const boost::json::object& args, const IDMCPCallPtr& call)
    {
        std::string filter;
        auto it = args.find("filter");
        if (it != args.end() && it->value().is_string())
        {
            filter = it->value().as_string().c_str();
        }
        idmcp_tool_ok(call, IDMCPRlvGate::report(filter));
    };
    reg.add(std::move(t));

    // rlv.canDo: dry-run another tool's RLV gate without executing it.
    IDMCPTool canDo;
    canDo.name = "rlv.canDo";
    canDo.description =
        "Check whether a tool call would be allowed under current RLV "
        "restrictions, WITHOUT executing it. {\"tool\"} (tool name) and optional "
        "{\"arguments\"} (that tool's arguments). Returns {allowed} and, if "
        "blocked, the restriction and its source object(s).";
    canDo.input_schema = boost::json::parse(
        R"({"type":"object","properties":{"tool":{"type":"string"},"arguments":{"type":"object"}},"required":["tool"],"additionalProperties":false})");
    canDo.invoke = [](const boost::json::object& args, const IDMCPCallPtr& call)
    {
        std::string tool_name;
        auto tit = args.find("tool");
        if (tit != args.end() && tit->value().is_string())
        {
            tool_name = tit->value().as_string().c_str();
        }
        const IDMCPTool* tool = IDMCPServer::instance().registry().find(tool_name);
        if (!tool)
        {
            idmcp_tool_err(call, IDMCP_ERR_METHOD_MISSING, "unknown tool: " + tool_name);
            return;
        }

        boost::json::object tool_args;
        auto ait = args.find("arguments");
        if (ait != args.end() && ait->value().is_object())
        {
            tool_args = ait->value().as_object();
        }

        boost::json::object out;
        out["tool"] = tool_name;
        if (!tool->gate)
        {
            out["allowed"] = true;   // no gate = never RLV-restricted
        }
        else
        {
            IDMCPGateResult g = tool->gate(tool_args, IDMCPGatePhase::Request);
            out["allowed"] = g.allowed;
            if (!g.allowed)
            {
                out["restriction"] = g.behaviour;
                out["sources"]     = g.sources;
            }
        }
        idmcp_tool_ok(call, out);
    };
    reg.add(std::move(canDo));
}
