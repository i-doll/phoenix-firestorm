/**
 * @file idsearchmodel.cpp
 * @brief <ID> MCP server: headless directory search (see header).
 *
 * Part of Five's custom Firestorm fork.
 */

#include "llviewerprecompiledheaders.h"

#include "idmcp.h"
#include "idsearchmodel.h"
#include "idmcpserver.h"
#include "idmcprlvgate.h"

#include "message.h"          // gMessageSystem, _PREHASH_*
#include "llagent.h"          // gAgent, gAgentID, gAgentSessionID
#include "llqueryflags.h"     // DFQ_*, ST_*
#include "llparcel.h"         // LLParcel::C_ANY
#include "llclassifiedflags.h" // pack_classified_flags_request
#include "rlvactions.h"

namespace
{
    std::string arg_str(const boost::json::object& args, const char* key)
    {
        auto it = args.find(key);
        return (it != args.end() && it->value().is_string())
                   ? std::string(it->value().as_string().c_str()) : std::string();
    }

    IDMCPGateResult gate_showsearch(const boost::json::object&, IDMCPGatePhase)
    {
        return IDMCPRlvGate::checkBehaviour(RLV_BHVR_SHOWSEARCH, "showsearch");
    }
}

// ---------------------------------------------------------------------------

void IDSearchModel::startPeople(const std::string& text, const IDMCPCallPtr& call)
{
    LLUUID qid;
    qid.generate();
    mPending[qid] = { call, PEOPLE };

    gMessageSystem->newMessage("DirFindQuery");
    gMessageSystem->nextBlock("AgentData");
    gMessageSystem->addUUID("AgentID", gAgentID);
    gMessageSystem->addUUID("SessionID", gAgentSessionID);
    gMessageSystem->nextBlock("QueryData");
    gMessageSystem->addUUID("QueryID", qid);
    gMessageSystem->addString("QueryText", text);
    gMessageSystem->addU32("QueryFlags", DFQ_PEOPLE);
    gMessageSystem->addS32("QueryStart", 0);
    gAgent.sendReliableMessage();

    call->setCleanup([qid]()
    {
        if (IDSearchModel::instanceExists()) IDSearchModel::instance().cancel(qid);
    });
}

void IDSearchModel::startPlaces(const std::string& text, const IDMCPCallPtr& call)
{
    LLUUID qid;
    qid.generate();
    mPending[qid] = { call, PLACES };

    const U32 scope = DFQ_DWELL_SORT | DFQ_INC_PG | DFQ_INC_MATURE | DFQ_INC_ADULT;

    gMessageSystem->newMessage("DirPlacesQuery");
    gMessageSystem->nextBlock("AgentData");
    gMessageSystem->addUUID("AgentID", gAgentID);
    gMessageSystem->addUUID("SessionID", gAgentSessionID);
    gMessageSystem->nextBlock("QueryData");
    gMessageSystem->addUUID("QueryID", qid);
    gMessageSystem->addString("QueryText", text);
    gMessageSystem->addU32("QueryFlags", scope);
    gMessageSystem->addS8("Category", LLParcel::C_ANY);
    gMessageSystem->addString("SimName", "");
    gMessageSystem->addS32("QueryStart", 0);
    gAgent.sendReliableMessage();

    call->setCleanup([qid]()
    {
        if (IDSearchModel::instanceExists()) IDSearchModel::instance().cancel(qid);
    });
}

void IDSearchModel::cancel(const LLUUID& query_id)
{
    mPending.erase(query_id);
}

IDMCPCallPtr IDSearchModel::takePending(const LLUUID& qid, EType type)
{
    auto it = mPending.find(qid);
    if (it == mPending.end() || it->second.type != type)
    {
        return nullptr;
    }
    IDMCPCallPtr call = it->second.call;
    mPending.erase(it);
    return call;
}

namespace
{
    // Common maturity flags (PG + Mature + Adult).
    const U32 ALL_MATURITIES = DFQ_INC_PG | DFQ_INC_MATURE | DFQ_INC_ADULT;

    void set_query_cleanup(const IDMCPCallPtr& call, const LLUUID& qid)
    {
        call->setCleanup([qid]()
        {
            if (IDSearchModel::instanceExists()) IDSearchModel::instance().cancel(qid);
        });
    }
}

void IDSearchModel::startGroups(const std::string& text, const IDMCPCallPtr& call)
{
    LLUUID qid; qid.generate();
    mPending[qid] = { call, GROUPS };

    gMessageSystem->newMessage("DirFindQuery");
    gMessageSystem->nextBlock("AgentData");
    gMessageSystem->addUUID("AgentID", gAgentID);
    gMessageSystem->addUUID("SessionID", gAgentSessionID);
    gMessageSystem->nextBlock("QueryData");
    gMessageSystem->addUUID("QueryID", qid);
    gMessageSystem->addString("QueryText", text);
    gMessageSystem->addU32("QueryFlags", DFQ_GROUPS | ALL_MATURITIES);
    gMessageSystem->addS32("QueryStart", 0);
    gAgent.sendReliableMessage();

    set_query_cleanup(call, qid);
}

void IDSearchModel::startEvents(const std::string& text, const IDMCPCallPtr& call)
{
    LLUUID qid; qid.generate();
    mPending[qid] = { call, EVENTS };

    // Event query text is encoded "<day|u>|<category>|<text>"; "u" = upcoming,
    // category 0 = all.
    const std::string q = "u|0|" + text;

    gMessageSystem->newMessage("DirFindQuery");
    gMessageSystem->nextBlock("AgentData");
    gMessageSystem->addUUID("AgentID", gAgentID);
    gMessageSystem->addUUID("SessionID", gAgentSessionID);
    gMessageSystem->nextBlock("QueryData");
    gMessageSystem->addUUID("QueryID", qid);
    gMessageSystem->addString("QueryText", q);
    gMessageSystem->addU32("QueryFlags", DFQ_DATE_EVENTS | ALL_MATURITIES);
    gMessageSystem->addS32("QueryStart", 0);
    gAgent.sendReliableMessage();

    set_query_cleanup(call, qid);
}

void IDSearchModel::startLand(S32 max_price, S32 min_area, const IDMCPCallPtr& call)
{
    LLUUID qid; qid.generate();
    mPending[qid] = { call, LAND };

    U32 scope = ALL_MATURITIES | DFQ_PRICE_SORT | DFQ_SORT_ASC;
    if (max_price > 0) scope |= DFQ_LIMIT_BY_PRICE;
    if (min_area > 0)  scope |= DFQ_LIMIT_BY_AREA;

    gMessageSystem->newMessage("DirLandQuery");
    gMessageSystem->nextBlock("AgentData");
    gMessageSystem->addUUID("AgentID", gAgentID);
    gMessageSystem->addUUID("SessionID", gAgentSessionID);
    gMessageSystem->nextBlock("QueryData");
    gMessageSystem->addUUID("QueryID", qid);
    gMessageSystem->addU32("QueryFlags", scope);
    gMessageSystem->addU32("SearchType", ST_ALL);
    gMessageSystem->addS32("Price", max_price > 0 ? max_price : 0);
    gMessageSystem->addS32("Area", min_area > 0 ? min_area : 0);
    gMessageSystem->addS32("QueryStart", 0);
    gAgent.sendReliableMessage();

    set_query_cleanup(call, qid);
}

void IDSearchModel::startClassifieds(const std::string& text, const IDMCPCallPtr& call)
{
    LLUUID qid; qid.generate();
    mPending[qid] = { call, CLASSIFIEDS };

    const ClassifiedFlags flags = pack_classified_flags_request(false, true, true, true);

    gMessageSystem->newMessage("DirClassifiedQuery");
    gMessageSystem->nextBlock("AgentData");
    gMessageSystem->addUUID("AgentID", gAgentID);
    gMessageSystem->addUUID("SessionID", gAgentSessionID);
    gMessageSystem->nextBlock("QueryData");
    gMessageSystem->addUUID("QueryID", qid);
    gMessageSystem->addString("QueryText", text);
    gMessageSystem->addU32("QueryFlags", (U32)flags);
    gMessageSystem->addU32("Category", 0);
    gMessageSystem->addS32("QueryStart", 0);
    gAgent.sendReliableMessage();

    set_query_cleanup(call, qid);
}

void IDSearchModel::handleGroupsReply(LLMessageSystem* msg)
{
    LLUUID qid;
    msg->getUUIDFast(_PREHASH_QueryData, _PREHASH_QueryID, qid);
    IDMCPCallPtr call = takePending(qid, GROUPS);
    if (!call) return;

    boost::json::array arr;
    const S32 rows = msg->getNumberOfBlocksFast(_PREHASH_QueryReplies);
    for (S32 i = 0; i < rows; ++i)
    {
        LLUUID group_id;
        std::string group_name;
        S32 members = 0;
        msg->getUUIDFast(  _PREHASH_QueryReplies, _PREHASH_GroupID,   group_id,   i);
        msg->getStringFast(_PREHASH_QueryReplies, _PREHASH_GroupName, group_name, i);
        msg->getS32Fast(   _PREHASH_QueryReplies, _PREHASH_Members,   members,    i);
        if (group_id.isNull()) continue;
        boost::json::object o;
        o["id"]      = group_id.asString();
        o["name"]    = group_name;
        o["members"] = members;
        arr.push_back(std::move(o));
    }
    boost::json::object out;
    out["results"]  = std::move(arr);
    out["query_id"] = qid.asString();
    idmcp_tool_ok(call, out);
}

void IDSearchModel::handleEventsReply(LLMessageSystem* msg)
{
    LLUUID qid;
    msg->getUUID("QueryData", "QueryID", qid);
    IDMCPCallPtr call = takePending(qid, EVENTS);
    if (!call) return;

    boost::json::array arr;
    const S32 rows = msg->getNumberOfBlocks("QueryReplies");
    for (S32 i = 0; i < rows; ++i)
    {
        LLUUID owner_id;
        std::string name, date;
        U32 event_id = 0, unix_time = 0;
        msg->getUUID(  "QueryReplies", "OwnerID",  owner_id,  i);
        msg->getString("QueryReplies", "Name",     name,      i);
        msg->getU32(   "QueryReplies", "EventID",  event_id,  i);
        msg->getString("QueryReplies", "Date",     date,      i);
        msg->getU32(   "QueryReplies", "UnixTime", unix_time, i);
        if (event_id == 0) continue;
        boost::json::object o;
        o["event_id"]  = (int64_t)event_id;
        o["name"]      = name;
        o["date"]      = date;
        o["unix_time"] = (int64_t)unix_time;
        o["owner_id"]  = owner_id.asString();
        arr.push_back(std::move(o));
    }
    boost::json::object out;
    out["results"]  = std::move(arr);
    out["query_id"] = qid.asString();
    idmcp_tool_ok(call, out);
}

void IDSearchModel::handleLandReply(LLMessageSystem* msg)
{
    LLUUID qid;
    msg->getUUID("QueryData", "QueryID", qid);
    IDMCPCallPtr call = takePending(qid, LAND);
    if (!call) return;

    boost::json::array arr;
    const S32 rows = msg->getNumberOfBlocks("QueryReplies");
    for (S32 i = 0; i < rows; ++i)
    {
        LLUUID parcel_id;
        std::string name;
        bool auction = false, for_sale = false;
        S32 price = 0, area = 0;
        msg->getUUID(  "QueryReplies", "ParcelID",   parcel_id, i);
        msg->getString("QueryReplies", "Name",       name,      i);
        msg->getBOOL(  "QueryReplies", "Auction",    auction,   i);
        msg->getBOOL(  "QueryReplies", "ForSale",    for_sale,  i);
        msg->getS32(   "QueryReplies", "SalePrice",  price,     i);
        msg->getS32(   "QueryReplies", "ActualArea", area,      i);
        if (parcel_id.isNull()) continue;
        boost::json::object o;
        o["parcel_id"] = parcel_id.asString();
        o["name"]      = name;
        o["auction"]   = auction;
        o["for_sale"]  = for_sale;
        o["price"]     = price;
        o["area"]      = area;
        arr.push_back(std::move(o));
    }
    boost::json::object out;
    out["results"]  = std::move(arr);
    out["query_id"] = qid.asString();
    idmcp_tool_ok(call, out);
}

void IDSearchModel::handleClassifiedsReply(LLMessageSystem* msg)
{
    LLUUID qid;
    msg->getUUID("QueryData", "QueryID", qid);
    IDMCPCallPtr call = takePending(qid, CLASSIFIEDS);
    if (!call) return;

    boost::json::array arr;
    const S32 rows = msg->getNumberOfBlocks("QueryReplies");
    for (S32 i = 0; i < rows; ++i)
    {
        LLUUID classified_id;
        std::string name;
        U32 creation_date = 0;
        S32 price = 0;
        msg->getUUID(  "QueryReplies", "ClassifiedID",    classified_id, i);
        msg->getString("QueryReplies", "Name",            name,          i);
        msg->getU32(   "QueryReplies", "CreationDate",    creation_date, i);
        msg->getS32(   "QueryReplies", "PriceForListing", price,         i);
        if (classified_id.isNull()) continue;
        boost::json::object o;
        o["classified_id"] = classified_id.asString();
        o["name"]          = name;
        o["creation_date"] = (int64_t)creation_date;
        o["price"]         = price;
        arr.push_back(std::move(o));
    }
    boost::json::object out;
    out["results"]  = std::move(arr);
    out["query_id"] = qid.asString();
    idmcp_tool_ok(call, out);
}

// Stable facade (idmcp.h): called from LLPanelDirBrowser without pulling the
// heavy MCP headers into that TU.
void idmcp::onDirPeopleReply(LLMessageSystem* msg)
{
    if (IDSearchModel::instanceExists()) IDSearchModel::instance().handlePeopleReply(msg);
}

void idmcp::onDirPlacesReply(LLMessageSystem* msg)
{
    if (IDSearchModel::instanceExists()) IDSearchModel::instance().handlePlacesReply(msg);
}

void idmcp::onDirGroupsReply(LLMessageSystem* msg)
{
    if (IDSearchModel::instanceExists()) IDSearchModel::instance().handleGroupsReply(msg);
}

void idmcp::onDirEventsReply(LLMessageSystem* msg)
{
    if (IDSearchModel::instanceExists()) IDSearchModel::instance().handleEventsReply(msg);
}

void idmcp::onDirLandReply(LLMessageSystem* msg)
{
    if (IDSearchModel::instanceExists()) IDSearchModel::instance().handleLandReply(msg);
}

void idmcp::onDirClassifiedReply(LLMessageSystem* msg)
{
    if (IDSearchModel::instanceExists()) IDSearchModel::instance().handleClassifiedsReply(msg);
}

void IDSearchModel::handlePeopleReply(LLMessageSystem* msg)
{
    LLUUID qid;
    msg->getUUIDFast(_PREHASH_QueryData, _PREHASH_QueryID, qid);

    auto it = mPending.find(qid);
    if (it == mPending.end() || it->second.type != PEOPLE)
    {
        return;
    }
    IDMCPCallPtr call = it->second.call;
    mPending.erase(it);       // erase before respond so cleanup->cancel no-ops

    const bool rlv = RlvActions::isRlvEnabled();
    boost::json::array arr;
    const S32 rows = msg->getNumberOfBlocksFast(_PREHASH_QueryReplies);
    for (S32 i = 0; i < rows; ++i)
    {
        std::string first_name, last_name;
        LLUUID agent_id;
        msg->getStringFast(_PREHASH_QueryReplies, _PREHASH_FirstName, first_name, i);
        msg->getStringFast(_PREHASH_QueryReplies, _PREHASH_LastName,  last_name,  i);
        msg->getUUIDFast(  _PREHASH_QueryReplies, _PREHASH_AgentID,   agent_id,   i);
        if (agent_id.isNull())
        {
            continue;
        }
        boost::json::object o;
        o["id"] = agent_id.asString();
        if (rlv && !RlvActions::canShowName(RlvActions::SNC_DEFAULT, agent_id))
        {
            o["hidden"] = true;
        }
        else
        {
            std::string name = first_name;
            if (!last_name.empty() && last_name != "Resident")
            {
                name += " " + last_name;
            }
            o["name"] = name;
        }
        arr.push_back(std::move(o));
    }

    boost::json::object out;
    out["results"]  = std::move(arr);
    out["query_id"] = qid.asString();
    idmcp_tool_ok(call, out);
}

void IDSearchModel::handlePlacesReply(LLMessageSystem* msg)
{
    LLUUID qid;
    msg->getUUIDFast(_PREHASH_QueryData, _PREHASH_QueryID, qid);

    auto it = mPending.find(qid);
    if (it == mPending.end() || it->second.type != PLACES)
    {
        return;
    }
    IDMCPCallPtr call = it->second.call;
    mPending.erase(it);

    boost::json::array arr;
    const S32 rows = msg->getNumberOfBlocks("QueryReplies");
    for (S32 i = 0; i < rows; ++i)
    {
        LLUUID parcel_id;
        std::string name;
        bool for_sale = false, auction = false;
        F32 dwell = 0.f;
        msg->getUUID(  "QueryReplies", "ParcelID", parcel_id, i);
        msg->getString("QueryReplies", "Name",     name,      i);
        msg->getBOOL(  "QueryReplies", "ForSale",  for_sale,  i);
        msg->getBOOL(  "QueryReplies", "Auction",  auction,   i);
        msg->getF32(   "QueryReplies", "Dwell",    dwell,     i);
        if (parcel_id.isNull())
        {
            continue;
        }
        boost::json::object o;
        o["parcel_id"] = parcel_id.asString();
        o["name"]      = name;
        o["for_sale"]  = for_sale;
        o["auction"]   = auction;
        o["dwell"]     = dwell;
        arr.push_back(std::move(o));
    }

    boost::json::object out;
    out["results"]  = std::move(arr);
    out["query_id"] = qid.asString();
    idmcp_tool_ok(call, out);
}

// ---------------------------------------------------------------------------

void idmcp_register_search_tools(IDMCPToolRegistry& reg)
{
    {
        IDMCPTool t;
        t.name = "search.people";
        t.description =
            "Search the People directory. {\"query\"} (min 3 chars). Returns "
            "matching residents. Blocked by RLV @showsearch; names hidden by "
            "@shownames are omitted.";
        t.input_schema = boost::json::parse(
            R"({"type":"object","properties":{"query":{"type":"string"}},"required":["query"],"additionalProperties":false})");
        t.gate = gate_showsearch;
        t.invoke = [](const boost::json::object& args, const IDMCPCallPtr& call)
        {
            const std::string q = arg_str(args, "query");
            if (q.size() < 3)
            {
                idmcp_tool_err(call, IDMCP_ERR_INVALID_PARAMS, "query too short (min 3 chars)");
                return;
            }
            IDSearchModel::instance().startPeople(q, call);   // deferred -> SSE
        };
        reg.add(std::move(t));
    }

    {
        IDMCPTool t;
        t.name = "search.places";
        t.description =
            "Search the Places directory. {\"query\"} (min 3 chars). Returns "
            "matching parcels (name, dwell, for-sale). Blocked by RLV @showsearch.";
        t.input_schema = boost::json::parse(
            R"({"type":"object","properties":{"query":{"type":"string"}},"required":["query"],"additionalProperties":false})");
        t.gate = gate_showsearch;
        t.invoke = [](const boost::json::object& args, const IDMCPCallPtr& call)
        {
            const std::string q = arg_str(args, "query");
            if (q.size() < 3)
            {
                idmcp_tool_err(call, IDMCP_ERR_INVALID_PARAMS, "query too short (min 3 chars)");
                return;
            }
            IDSearchModel::instance().startPlaces(q, call);   // deferred -> SSE
        };
        reg.add(std::move(t));
    }

    {
        IDMCPTool t;
        t.name = "search.groups";
        t.description = "Search the Groups directory. {\"query\"} (min 3 chars). Blocked by RLV @showsearch.";
        t.input_schema = boost::json::parse(
            R"({"type":"object","properties":{"query":{"type":"string"}},"required":["query"],"additionalProperties":false})");
        t.gate = gate_showsearch;
        t.invoke = [](const boost::json::object& args, const IDMCPCallPtr& call)
        {
            const std::string q = arg_str(args, "query");
            if (q.size() < 3) { idmcp_tool_err(call, IDMCP_ERR_INVALID_PARAMS, "query too short (min 3 chars)"); return; }
            IDSearchModel::instance().startGroups(q, call);
        };
        reg.add(std::move(t));
    }

    {
        IDMCPTool t;
        t.name = "search.events";
        t.description = "Search upcoming Events. {\"query\"} (min 3 chars). Blocked by RLV @showsearch.";
        t.input_schema = boost::json::parse(
            R"({"type":"object","properties":{"query":{"type":"string"}},"required":["query"],"additionalProperties":false})");
        t.gate = gate_showsearch;
        t.invoke = [](const boost::json::object& args, const IDMCPCallPtr& call)
        {
            const std::string q = arg_str(args, "query");
            if (q.size() < 3) { idmcp_tool_err(call, IDMCP_ERR_INVALID_PARAMS, "query too short (min 3 chars)"); return; }
            IDSearchModel::instance().startEvents(q, call);
        };
        reg.add(std::move(t));
    }

    {
        IDMCPTool t;
        t.name = "search.classifieds";
        t.description = "Search Classifieds. {\"query\"} (min 3 chars). Blocked by RLV @showsearch.";
        t.input_schema = boost::json::parse(
            R"({"type":"object","properties":{"query":{"type":"string"}},"required":["query"],"additionalProperties":false})");
        t.gate = gate_showsearch;
        t.invoke = [](const boost::json::object& args, const IDMCPCallPtr& call)
        {
            const std::string q = arg_str(args, "query");
            if (q.size() < 3) { idmcp_tool_err(call, IDMCP_ERR_INVALID_PARAMS, "query too short (min 3 chars)"); return; }
            IDSearchModel::instance().startClassifieds(q, call);
        };
        reg.add(std::move(t));
    }

    {
        IDMCPTool t;
        t.name = "search.land";
        t.description =
            "Search for-sale Land (parcels), price-sorted ascending. Optional "
            "{\"max_price\"} (L$) and {\"min_area\"} (m2) filters. Blocked by RLV @showsearch.";
        t.input_schema = boost::json::parse(
            R"({"type":"object","properties":{"max_price":{"type":"integer"},"min_area":{"type":"integer"}},"additionalProperties":false})");
        t.gate = gate_showsearch;
        t.invoke = [](const boost::json::object& args, const IDMCPCallPtr& call)
        {
            auto get_int = [&](const char* k) -> S32 {
                auto it = args.find(k);
                if (it != args.end() && it->value().is_int64()) return (S32)it->value().as_int64();
                return 0;
            };
            IDSearchModel::instance().startLand(get_int("max_price"), get_int("min_area"), call);
        };
        reg.add(std::move(t));
    }
}
