/**
 * @file idmcp.h
 * @brief <ID> Minimal, stable facade for the embedded MCP server.
 *
 * Part of Five's custom Firestorm fork. Custom code carries an `ID` prefix.
 *
 * The heavy MCP headers (boost/json, sockets, the tool registry) pull in a lot
 * and change often as tools are added. Large viewer translation units that only
 * need to *drive* the server (llstartup, llappviewer, llpaneldirbrowser) include
 * THIS header only — it forward-declares everything and never changes — so
 * editing MCP internals never forces those expensive TUs to recompile.
 */

#ifndef ID_IDMCP_H
#define ID_IDMCP_H

#include <string>

class LLMessageSystem;
class LLUUID;

namespace idmcp
{
    // Instantiate the server (installs the enable-setting watcher) and start it
    // if IDMCPServerEnabled is set. Call once at login (STATE_STARTED).
    void startupInit();

    // Stop the server: close sockets, fail in-flight calls, drop the poll.
    // Call from LLAppViewer::disconnectViewer().
    void shutdown();

    // Directory-search reply hooks, called from LLPanelDirBrowser reply handlers.
    void onDirPeopleReply(LLMessageSystem* msg);
    void onDirPlacesReply(LLMessageSystem* msg);
    void onDirGroupsReply(LLMessageSystem* msg);
    void onDirEventsReply(LLMessageSystem* msg);
    void onDirLandReply(LLMessageSystem* msg);
    void onDirClassifiedReply(LLMessageSystem* msg);
    void onGroupNoticesListReply(LLMessageSystem* msg);

    // Full ObjectProperties reply hook (name resolution for avatars.getWorn),
    // called from the ObjectProperties message handler. Used instead of the
    // Family variant, which the sim does not answer for others' attachments.
    void onObjectProperties(LLMessageSystem* msg);

    // Agent chat bridge hooks, called from FSLSLBridge::lslToViewer. A message
    // heard on an agent-opened bridge listen, and open/close/error status for a
    // requested listen. (channel is S32/int.)
    void onBridgeChat(int channel, const LLUUID& from, const std::string& name, const std::string& text);
    void onBridgeChatStatus(bool opened, int channel, const std::string& error);

    // Close all agent chat-bridge listens (called from IDMCPServer::stop so a
    // scarce bridge listen slot is never leaked past the server's lifetime).
    void stopAllBridgeListens();
}

#endif // ID_IDMCP_H
