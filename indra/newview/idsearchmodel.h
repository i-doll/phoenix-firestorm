/**
 * @file idsearchmodel.h
 * @brief <ID> MCP server: headless directory search (people/places).
 *
 * Part of Five's custom Firestorm fork. Custom code carries an `ID` prefix.
 *
 * Sends legacy Dir*Query UDP messages with its own QueryIDs and resolves the
 * matching Dir*Reply into a deferred MCP response. Replies are delivered by
 * forwarding calls added to LLPanelDirBrowser::processDir{People,Places}Reply
 * (the LL handlers already forward to the FS search panels; we ride alongside,
 * and each consumer ignores QueryIDs it doesn't own).
 */

#ifndef ID_IDSEARCHMODEL_H
#define ID_IDSEARCHMODEL_H

#include "llsingleton.h"
#include "lluuid.h"
#include "idmcptoolregistry.h"   // IDMCPCallPtr

#include <map>
#include <string>

class LLMessageSystem;

class IDSearchModel : public LLSingleton<IDSearchModel>
{
    LLSINGLETON_EMPTY_CTOR(IDSearchModel);

public:
    void startPeople(const std::string& text, const IDMCPCallPtr& call);
    void startPlaces(const std::string& text, const IDMCPCallPtr& call);
    void startGroups(const std::string& text, const IDMCPCallPtr& call);
    void startEvents(const std::string& text, const IDMCPCallPtr& call);
    void startLand(S32 max_price, S32 min_area, const IDMCPCallPtr& call);
    void startClassifieds(const std::string& text, const IDMCPCallPtr& call);

    // Called from LLPanelDirBrowser reply handlers.
    void handlePeopleReply(LLMessageSystem* msg);
    void handlePlacesReply(LLMessageSystem* msg);
    void handleGroupsReply(LLMessageSystem* msg);
    void handleEventsReply(LLMessageSystem* msg);
    void handleLandReply(LLMessageSystem* msg);
    void handleClassifiedsReply(LLMessageSystem* msg);

    // Drop a pending query (invoked from a call's cleanup on timeout/hangup).
    void cancel(const LLUUID& query_id);

private:
    enum EType { PEOPLE, PLACES, GROUPS, EVENTS, LAND, CLASSIFIEDS };
    struct Pending { IDMCPCallPtr call; EType type; };

    // Match a reply's QueryID to a pending call of the given type, remove it,
    // and return the call (null if not ours). Erasing first makes the call's
    // cleanup->cancel a no-op.
    IDMCPCallPtr takePending(const LLUUID& query_id, EType type);

    std::map<LLUUID, Pending> mPending;
};

void idmcp_register_search_tools(IDMCPToolRegistry& reg);

#endif // ID_IDSEARCHMODEL_H
