/**
 * @file idmcptools_headless.h
 * @brief <ID> Headless test mode: session lifecycle and MCP session tools.
 *
 * Part of Five's custom Firestorm fork.
 */

#ifndef ID_IDMCPTOOLS_HEADLESS_H
#define ID_IDMCPTOOLS_HEADLESS_H

#include "lluuid.h"
#include <boost/json/object.hpp>
#include <functional>
#include <string>

class LLViewerObject;

namespace idmcp_headless
{
void begin(std::function<void()> on_ready);
void finish(std::function<void()> done);
boost::json::object sessionInfo();
void ledgerAdd(LLViewerObject* root, const std::string& tool);
void ledgerRemove(const LLUUID& root_id);
void ledgerKeep(LLViewerObject* root);
LLUUID runFolder();
LLUUID rlvRunFolder();
bool applyDest(bool given, LLUUID& folder, std::string& why);
}

#endif
