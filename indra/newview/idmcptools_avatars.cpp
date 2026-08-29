/**
 * @file idmcptools_avatars.cpp
 * @brief <ID> MCP server: nearby-avatar inspection (read-only).
 *
 * Part of Five's custom Firestorm fork.
 *
 * For OTHER avatars only attachments and baked appearance are observable — their
 * clothing/bodypart wearable items are NOT enumerable (the viewer holds only the
 * composited result). avatars.getWorn therefore reports attached objects only.
 * Gated on RLV @shownearby (list) and @shownames (identity).
 */

#include "llviewerprecompiledheaders.h"

#include "idmcp.h"       // facade: idmcp::onObjectProperties
#include "idmcptools.h"
#include "idmcpserver.h"
#include "idmcprlvgate.h"

#include "llworld.h"
#include "llvoavatar.h"
#include "llviewerobject.h"
#include "llviewerobjectlist.h"     // gObjectList
#include "llviewerjointattachment.h"
#include "llviewerregion.h"         // region->getHost()
#include "llagent.h"
#include "rlvactions.h"

#include "llavatarnamecache.h"
#include "llavatarname.h"
#include "lluuid.h"
#include "message.h"                // LLMessageSystem getUUID/getString
#include "llevents.h"               // LLEventPumps, LLTempBoundListener
#include "lltimer.h"                // LLTimer::getTotalSeconds

#include <algorithm>
#include <cmath>
#include <map>
#include <memory>
#include <set>
#include <vector>

namespace
{
    bool looks_like_uuid(const std::string& s)
    {
        return s.size() == 36 && s[8] == '-' && s[13] == '-' && s[18] == '-' && s[23] == '-';
    }

    std::string arg_str(const boost::json::object& args, const char* key)
    {
        auto it = args.find(key);
        return (it != args.end() && it->value().is_string())
                   ? std::string(it->value().as_string().c_str()) : std::string();
    }

    bool arg_bool(const boost::json::object& args, const char* key, bool dflt)
    {
        auto it = args.find(key);
        return (it != args.end() && it->value().is_bool()) ? it->value().as_bool() : dflt;
    }

    // ---- deferred object-name resolution for getWorn -----------------------
    //
    // Others' attachment objects arrive without names; a name comes from an
    // ObjectPropertiesFamily round-trip. getWorn fires a request per attachment,
    // then names are filled in as replies land (forwarded from the message
    // handler via idmcp::onObjectProperties below) and the call responds
    // once all resolve or a short deadline passes. Main-thread only.
    struct WornNameWait
    {
        IDMCPCallPtr        call;
        boost::json::object out;          // avatar_id, is_loaded (list added at finish)
        boost::json::array  attachments;  // each entry gains "name" when resolved
        std::set<LLUUID>    pending;      // object ids still awaiting a name
        F64                 deadline = 0.0;
        bool                done = false;
        std::string         result_key = "attachments";  // output key for the list
    };

    std::vector<std::shared_ptr<WornNameWait>> g_worn_waits;
    bool                                       g_worn_tick_on = false;
    LLTempBoundListener                        g_worn_tick;

    void worn_finish(const std::shared_ptr<WornNameWait>& w)
    {
        if (w->done) return;
        w->done = true;
        w->out[w->result_key] = std::move(w->attachments);
        idmcp_tool_ok(w->call, w->out);
    }

    void worn_tick()
    {
        if (g_worn_waits.empty()) return;
        const F64 now = LLTimer::getTotalSeconds();
        for (auto it = g_worn_waits.begin(); it != g_worn_waits.end(); )
        {
            if ((*it)->pending.empty() || now >= (*it)->deadline)
            {
                worn_finish(*it);
                it = g_worn_waits.erase(it);
            }
            else
            {
                ++it;
            }
        }
    }

    void worn_ensure_tick()
    {
        if (g_worn_tick_on) return;
        g_worn_tick = LLEventPumps::instance().obtain("mainloop").listen(
            "idmcp_worn_names", [](const LLSD&) -> bool { worn_tick(); return false; });
        g_worn_tick_on = true;
    }

    IDMCPGateResult deny(const char* label)
    {
        IDMCPGateResult r;
        r.allowed   = false;
        r.behaviour = label;
        return r;
    }

    IDMCPGateResult gate_nearby(const boost::json::object&, IDMCPGatePhase)
    {
        if (IDMCPRlvGate::isEnabled() && !RlvActions::canShowNearbyAgents())
        {
            return deny("shownearby");
        }
        return IDMCPGateResult();
    }

    IDMCPGateResult gate_getworn(const boost::json::object& args, IDMCPGatePhase)
    {
        if (!IDMCPRlvGate::isEnabled())
        {
            return IDMCPGateResult();
        }
        if (!RlvActions::canShowNearbyAgents())
        {
            return deny("shownearby");
        }
        const std::string spec = arg_str(args, "avatar_id");
        if (looks_like_uuid(spec))
        {
            LLUUID id(spec);
            if (id != gAgent.getID() && !RlvActions::canShowName(RlvActions::SNC_DEFAULT, id))
            {
                return deny("shownames");
            }
        }
        return IDMCPGateResult();
    }
}

// ---------------------------------------------------------------------------
// Full ObjectProperties reply hook: fill names into any pending getWorn waits.
// Forwarded from process_object_properties in llviewermessage.cpp. The full
// variant is multi-block (one avatar's attachments may span several blocks and
// several messages), so loop every ObjectData block and match by ObjectID. The
// message was already read by the LL handler; re-reading the fields is safe.
void idmcp::onObjectProperties(LLMessageSystem* msg)
{
    if (!msg || g_worn_waits.empty()) return;

    const S32 n = msg->getNumberOfBlocksFast(_PREHASH_ObjectData);
    for (S32 i = 0; i < n; ++i)
    {
        LLUUID object_id;
        msg->getUUIDFast(_PREHASH_ObjectData, _PREHASH_ObjectID, object_id, i);
        if (object_id.isNull()) continue;
        std::string name;
        msg->getStringFast(_PREHASH_ObjectData, _PREHASH_Name, name, i);

        const std::string oid = object_id.asString();
        for (auto& w : g_worn_waits)
        {
            if (w->pending.erase(object_id))
            {
                for (auto& v : w->attachments)
                {
                    boost::json::object& o = v.as_object();
                    if (std::string(o.at("object_id").as_string().c_str()) == oid)
                    {
                        o["name"] = name;
                    }
                }
            }
        }
    }
}

// ---------------------------------------------------------------------------

void idmcp_register_avatars_tools(IDMCPToolRegistry& reg)
{
    // avatars.getNearby ------------------------------------------------------
    {
        IDMCPTool t;
        t.name = "avatars.getNearby";
        t.description =
            "List avatars within {\"radius\"} metres (default 128, max 512) of "
            "you: id, distance, global position:[x,y,z], which way they face "
            "(facing:[x,y,z] unit vector + heading_deg, 0=East/90=North; loaded "
            "avatars only), whether their body is loaded, and name (subject to RLV "
            "@shownames). To place yourself BEHIND someone, walk to "
            "position - N*facing; to their FRONT, position + N*facing. Blocked by "
            "RLV @shownearby.";
        t.input_schema = boost::json::parse(
            R"({"type":"object","properties":{"radius":{"type":"number"}},"additionalProperties":false})");
        t.gate = gate_nearby;
        t.invoke = [](const boost::json::object& args, const IDMCPCallPtr& call)
        {
            F32 radius = 128.f;
            auto rit = args.find("radius");
            if (rit != args.end())
            {
                double v = 0.0;
                if      (rit->value().is_double()) v = rit->value().as_double();
                else if (rit->value().is_int64())  v = (double)rit->value().as_int64();
                else if (rit->value().is_uint64()) v = (double)rit->value().as_uint64();
                if (v > 0) radius = (F32)std::min(v, 512.0);
            }

            const LLVector3d origin = gAgent.getPositionGlobal();
            uuid_vec_t ids;
            std::vector<LLVector3d> positions;
            LLWorld::getInstance()->getAvatars(&ids, &positions, origin, radius);

            const bool rlv = RlvActions::isRlvEnabled();
            boost::json::array arr;
            for (size_t i = 0; i < ids.size(); ++i)
            {
                boost::json::object o;
                o["id"] = ids[i].asString();
                if (i < positions.size())
                {
                    o["distance"] = (positions[i] - origin).length();
                    boost::json::array pa;
                    pa.push_back(positions[i].mdV[0]);
                    pa.push_back(positions[i].mdV[1]);
                    pa.push_back(positions[i].mdV[2]);
                    o["position"] = std::move(pa);
                }
                LLViewerObject* vo = gObjectList.findObject(ids[i]);
                o["is_loaded"] = (vo && vo->asAvatar() != nullptr);

                // Which way they face (loaded avatars only). Forward is +X in the
                // avatar's frame; behind them = position - facing. heading_deg:
                // 0 = East (+X), 90 = North (+Y).
                if (vo && vo->asAvatar())
                {
                    const LLVector3 fwd = LLVector3(1.f, 0.f, 0.f) * vo->getRotationRegion();
                    boost::json::array fa;
                    fa.push_back((double)fwd.mV[0]);
                    fa.push_back((double)fwd.mV[1]);
                    fa.push_back((double)fwd.mV[2]);
                    o["facing"]      = std::move(fa);
                    o["heading_deg"] = (double)(std::atan2((double)fwd.mV[1], (double)fwd.mV[0]) * RAD_TO_DEG);
                }

                if (rlv && ids[i] != gAgent.getID() &&
                    !RlvActions::canShowName(RlvActions::SNC_DEFAULT, ids[i]))
                {
                    o["hidden"] = true;
                }
                else
                {
                    LLAvatarName av;
                    if (LLAvatarNameCache::get(ids[i], &av))
                    {
                        o["name"] = av.getCompleteName();
                    }
                }
                arr.push_back(std::move(o));
            }

            boost::json::object out;
            out["avatars"] = std::move(arr);
            idmcp_tool_ok(call, out);
        };
        reg.add(std::move(t));
    }

    // avatars.getWorn --------------------------------------------------------
    {
        IDMCPTool t;
        t.name = "avatars.getWorn";
        t.description =
            "List the publicly-visible ATTACHMENTS worn by a nearby avatar "
            "({\"avatar_id\"}): attach-point name, point id, object id, and the "
            "object's NAME (resolved via a quick server round-trip; pass "
            "{\"resolve_names\":false} to skip and return immediately). To identify "
            "a specific item type (boots, a hat, a collar...), enumerate ALL "
            "attachments and match on the object NAME - do NOT infer from the "
            "attach point, since rigged mesh can be attached anywhere on the body. "
            "\"boots\" may read as boot/shoe/heels/stilettos/etc., so match "
            "loosely. NOTE: only in-world attachments are observable - HUDs and "
            "clothing/bodypart wearable items cannot be enumerated for anyone. The "
            "avatar must be loaded in range. Blocked by RLV @shownearby / @shownames.";
        t.input_schema = boost::json::parse(
            R"({"type":"object","properties":{"avatar_id":{"type":"string"},"resolve_names":{"type":"boolean"}},"required":["avatar_id"],"additionalProperties":false})");
        t.gate = gate_getworn;
        t.invoke = [](const boost::json::object& args, const IDMCPCallPtr& call)
        {
            const std::string spec = arg_str(args, "avatar_id");
            if (!looks_like_uuid(spec))
            {
                idmcp_tool_err(call, IDMCP_ERR_INVALID_PARAMS, "avatar_id must be a UUID");
                return;
            }
            LLUUID id(spec);
            LLViewerObject* vo = gObjectList.findObject(id);
            LLVOAvatar* av = vo ? vo->asAvatar() : nullptr;
            if (!av)
            {
                idmcp_tool_err(call, IDMCP_ERR_NOT_FOUND, "avatar not loaded / not in range");
                return;
            }

            boost::json::array           atts;
            std::vector<LLViewerObject*> objs;   // objects to query for names
            for (const auto& entry : av->mAttachmentPoints)
            {
                const LLViewerJointAttachment* pt = entry.second;
                if (!pt)
                {
                    continue;
                }
                // HUD attachments are private to their wearer and are never
                // transmitted to other clients - exclude them so the tool only
                // reports publicly-observable attachments (consistent for self
                // and others).
                if (pt->getIsHUDAttachment())
                {
                    continue;
                }
                for (const auto& objPtr : pt->mAttachedObjects)
                {
                    LLViewerObject* obj = objPtr.get();
                    if (!obj)
                    {
                        continue;
                    }
                    boost::json::object o;
                    o["point_name"] = pt->getName();
                    o["point_id"]   = entry.first;
                    o["object_id"]  = obj->getID().asString();
                    atts.push_back(std::move(o));
                    objs.push_back(obj);
                }
            }

            // Fast path: caller opted out of names, or nothing to name.
            if (!arg_bool(args, "resolve_names", true) || atts.empty())
            {
                boost::json::object out;
                out["avatar_id"]   = id.asString();
                out["is_loaded"]   = true;
                out["attachments"] = std::move(atts);
                idmcp_tool_ok(call, out);
                return;
            }

            // Deferred: request each object's name and fill it in as replies land.
            auto w = std::make_shared<WornNameWait>();
            w->call = call;
            w->out["avatar_id"] = id.asString();
            w->out["is_loaded"] = true;
            for (const auto& v : atts)
            {
                w->pending.insert(LLUUID(v.as_object().at("object_id").as_string().c_str()));
            }
            w->attachments = std::move(atts);
            w->deadline    = LLTimer::getTotalSeconds() + 5.0;
            g_worn_waits.push_back(w);
            worn_ensure_tick();

            // Trigger full ObjectProperties (which carries names and DOES answer
            // for others' attachments) by sending a raw ObjectSelect for all the
            // attachment local IDs. We deliberately do NOT go through LLSelectMgr
            // (no mSelectedObjects entry, no deselectAll) so there are zero visible
            // side effects: no selection outline, no disruption to the user's own
            // selection. The sim replies with ObjectProperties -> onObjectProperties.
            LLViewerRegion* region = av->getRegion();
            if (region)
            {
                LLMessageSystem* msg = gMessageSystem;
                msg->newMessageFast(_PREHASH_ObjectSelect);
                msg->nextBlockFast(_PREHASH_AgentData);
                msg->addUUIDFast(_PREHASH_AgentID, gAgent.getID());
                msg->addUUIDFast(_PREHASH_SessionID, gAgent.getSessionID());
                for (LLViewerObject* o : objs)
                {
                    msg->nextBlockFast(_PREHASH_ObjectData);
                    msg->addU32Fast(_PREHASH_ObjectLocalID, o->getLocalID());
                }
                msg->sendReliable(region->getHost());
            }
        };
        reg.add(std::move(t));
    }

    // objects.getNearby ------------------------------------------------------
    {
        IDMCPTool t;
        t.name = "objects.getNearby";
        t.description =
            "List in-world OBJECTS near you (rezzed prims/linksets — seats, vendors, "
            "doors, RLV furniture — NOT avatars or attachments). Optional "
            "{\"radius\"} metres (default 16, max 64), {\"limit\"} (default 32, max "
            "128, closest first), {\"scripted_only\"} (default false — set true to "
            "show only scripted/interactive objects), {\"resolve_names\"} (default "
            "true; a quick server round-trip for names — set false to return "
            "immediately without them). Returns {objects:[{object_id, name, "
            "distance, position:[x,y,z], scripted}]}. Use an object_id with "
            "object.touch, movement.sit, or movement.walkTo.";
        t.input_schema = boost::json::parse(
            R"({"type":"object","properties":{"radius":{"type":"number"},"limit":{"type":"integer"},"scripted_only":{"type":"boolean"},"resolve_names":{"type":"boolean"}},"additionalProperties":false})");
        t.invoke = [](const boost::json::object& args, const IDMCPCallPtr& call)
        {
            F64 radius = 16.0;
            if (auto it = args.find("radius"); it != args.end())
            {
                double v = 0.0;
                if      (it->value().is_double()) v = it->value().as_double();
                else if (it->value().is_int64())  v = (double)it->value().as_int64();
                if (v > 0) radius = std::min(v, 64.0);
            }
            S32 limit = 32;
            if (auto it = args.find("limit"); it != args.end() && it->value().is_int64())
            {
                S32 v = (S32)it->value().as_int64();
                if (v > 0) limit = std::min(v, 128);
            }
            const bool scripted_only = arg_bool(args, "scripted_only", false);
            const bool resolve       = arg_bool(args, "resolve_names", true);

            const LLVector3d origin = gAgent.getPositionGlobal();
            std::vector<std::pair<F64, LLViewerObject*>> cands;
            const S32 count = gObjectList.getNumObjects();
            for (S32 i = 0; i < count; ++i)
            {
                LLViewerObject* o = gObjectList.getObject(i);
                if (!o || o->isDead() || o->asAvatar() || o->isAttachment()) continue;
                if (o->getPCode() != LL_PCODE_VOLUME) continue;
                if (o != o->getRootEdit()) continue;   // root prims only (dedupe linksets)
                if (scripted_only && !o->flagScripted()) continue;
                const F64 d = (o->getPositionGlobal() - origin).length();
                if (d > radius) continue;
                cands.emplace_back(d, o);
            }
            std::sort(cands.begin(), cands.end(),
                      [](const auto& a, const auto& b) { return a.first < b.first; });
            if ((S32)cands.size() > limit) cands.resize(limit);

            boost::json::array           arr;
            std::vector<LLViewerObject*> objs;
            for (const auto& c : cands)
            {
                LLViewerObject* o = c.second;
                boost::json::object e;
                e["object_id"] = o->getID().asString();
                e["distance"]  = c.first;
                const LLVector3d p = o->getPositionGlobal();
                boost::json::array pa;
                pa.push_back(p.mdV[0]); pa.push_back(p.mdV[1]); pa.push_back(p.mdV[2]);
                e["position"]  = std::move(pa);
                if (o->flagScripted()) e["scripted"] = true;
                arr.push_back(std::move(e));
                objs.push_back(o);
            }

            if (!resolve || arr.empty())
            {
                boost::json::object out;
                out["objects"] = std::move(arr);
                idmcp_tool_ok(call, out);
                return;
            }

            // Deferred: resolve each object's name via ObjectProperties, reusing the
            // getWorn machinery (onObjectProperties fills "name" by object_id).
            auto w = std::make_shared<WornNameWait>();
            w->call       = call;
            w->result_key = "objects";
            for (const auto& v : arr)
            {
                w->pending.insert(LLUUID(v.as_object().at("object_id").as_string().c_str()));
            }
            w->attachments = std::move(arr);
            w->deadline    = LLTimer::getTotalSeconds() + 5.0;
            g_worn_waits.push_back(w);
            worn_ensure_tick();

            // ObjectSelect (raw, no LLSelectMgr → no visible selection) per region,
            // since a local id is only meaningful to its own region's host.
            std::map<LLViewerRegion*, std::vector<LLViewerObject*>> by_region;
            for (LLViewerObject* o : objs) by_region[o->getRegion()].push_back(o);
            for (const auto& kv : by_region)
            {
                LLViewerRegion* region = kv.first;
                if (!region) continue;
                LLMessageSystem* msg = gMessageSystem;
                msg->newMessageFast(_PREHASH_ObjectSelect);
                msg->nextBlockFast(_PREHASH_AgentData);
                msg->addUUIDFast(_PREHASH_AgentID, gAgent.getID());
                msg->addUUIDFast(_PREHASH_SessionID, gAgent.getSessionID());
                for (LLViewerObject* o : kv.second)
                {
                    msg->nextBlockFast(_PREHASH_ObjectData);
                    msg->addU32Fast(_PREHASH_ObjectLocalID, o->getLocalID());
                }
                msg->sendReliable(region->getHost());
            }
        };
        reg.add(std::move(t));
    }
}
