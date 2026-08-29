/**
 * @file idmcprlvgate.h
 * @brief <ID> MCP server: RLVa enforcement gate + restriction reporting.
 *
 * Part of Five's custom Firestorm fork. Custom code carries an `ID` prefix.
 *
 * Single choke-point for RLV checks. Tools declare a `gate` (see IDMCPTool) that
 * calls these helpers; the server runs it before invoke (Request phase), and
 * deferred tools re-run the relevant helper at Commit phase before the side
 * effect lands (a restriction can arrive between request and completion).
 */

#ifndef ID_IDMCPRLVGATE_H
#define ID_IDMCPRLVGATE_H

#include "idmcptoolregistry.h"
#include "rlvdefines.h"    // ERlvBehaviour
#include "lluuid.h"

#include <string>

class IDMCPRlvGate
{
public:
    static bool isEnabled();

    // Allow unless RLV is enabled and `bhvr` is active; on denial, attribute the
    // restriction to its source object(s). `label` is the behaviour name used in
    // the error (e.g. "showinv").
    static IDMCPGateResult checkBehaviour(ERlvBehaviour bhvr, const char* label);

    // Build a denial result for `label`, attributing every object that set `bhvr`.
    static IDMCPGateResult deny(ERlvBehaviour bhvr, const char* label);

    // Structured report of all active restrictions (optionally filtered by a
    // substring of the behaviour name).
    static boost::json::value report(const std::string& filter);

    // Best-effort human name for a restriction source object (avatar display
    // name, or the attached inventory item's name, else the raw id).
    static std::string sourceName(const LLUUID& idObj);
};

// Registration entry point (called from IDMCPServer::initSingleton).
void idmcp_register_rlv_tools(IDMCPToolRegistry& reg);

#endif // ID_IDMCPRLVGATE_H
