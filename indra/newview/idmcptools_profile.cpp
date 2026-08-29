/**
 * @file idmcptools_profile.cpp
 * @brief <ID> MCP server: profile view/edit + avatar name lookup.
 *
 * Part of Five's custom Firestorm fork.
 *
 * profile.get uses a one-shot LLAvatarPropertiesObserver (cap AgentProfile, with
 * a legacy-UDP fallback when the cap is absent); viewing another avatar is gated
 * on RLV @shownames. profile.setSelf PUTs to the AgentProfile cap via a
 * replicated coroutine, gated on @editprofile (+ @editpfp for images).
 * people.getNames resolves UUIDs from the name cache.
 */

#include "llviewerprecompiledheaders.h"

#include "idmcptools.h"
#include "idmcpserver.h"
#include "idmcprlvgate.h"

#include "llavatarpropertiesprocessor.h"
#include "llavatarnamecache.h"
#include "llavatarname.h"
#include "llcallingcard.h"      // LLAvatarTracker (friends list)
#include "lluserrelations.h"    // LLRelationship (online + granted rights)
#include "llagent.h"
#include "rlvactions.h"

#include "llcoros.h"
#include "llcorehttputil.h"
#include "llsdutil.h"
#include "lluuid.h"
#include "llevents.h"       // LLEventPumps, LLTempBoundListener
#include "lltimer.h"        // LLTimer::getTotalSeconds
#include "v3dmath.h"        // LLVector3d

#include <map>
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

    boost::json::value profile_to_json(const LLAvatarData& d)
    {
        boost::json::object o;
        o["avatar_id"]     = d.avatar_id.asString();
        o["about_text"]    = d.about_text;
        o["fl_about_text"] = d.fl_about_text;
        o["image_id"]      = d.image_id.asString();
        o["fl_image_id"]   = d.fl_image_id.asString();
        o["partner_id"]    = d.partner_id.asString();
        o["born_on"]       = d.born_on.asString();
        o["profile_url"]   = d.profile_url;
        o["hide_age"]      = d.hide_age;
        o["allow_publish"] = d.allow_publish;
        o["notes"]         = d.notes;

        boost::json::array picks;
        for (const auto& p : d.picks_list)   // pair<UUID, name>
        {
            boost::json::object po;
            po["id"]   = p.first.asString();
            po["name"] = p.second;
            picks.push_back(std::move(po));
        }
        o["picks"] = std::move(picks);

        boost::json::array groups;
        for (const auto& g : d.group_list)
        {
            boost::json::object go;
            go["id"]   = g.group_id.asString();
            go["name"] = g.group_name;
            groups.push_back(std::move(go));
        }
        o["groups"] = std::move(groups);
        return o;
    }

    // One-shot profile observer. Teardown is coordinated by an mSettled flag so
    // the reply path and the abandon (timeout/hangup) path never double-free.
    class IDMCPProfileObserver : public LLAvatarPropertiesObserver
    {
    public:
        IDMCPProfileObserver(IDMCPCallPtr call, const LLUUID& id)
            : mCall(std::move(call)), mId(id) {}

        void processProperties(void* data, EAvatarProcessorType type) override
        {
            if (mSettled)
            {
                return;
            }
            if (type == APT_PROPERTIES)
            {
                unregister();
                idmcp_tool_ok(mCall, profile_to_json(*static_cast<LLAvatarData*>(data)));
                delete this;
            }
            else if (type == APT_PROPERTIES_LEGACY)
            {
                unregister();
                LLAvatarData d(*static_cast<LLAvatarLegacyData*>(data));
                idmcp_tool_ok(mCall, profile_to_json(d));
                delete this;
            }
        }

        // Called from the call's cleanup on timeout / client hangup.
        void abandon()
        {
            if (mSettled)
            {
                return;
            }
            unregister();
            delete this;
        }

    private:
        void unregister()
        {
            mSettled = true;   // set before removeObserver: safe re-entrancy guard
            LLAvatarPropertiesProcessor::getInstance()->removeObserver(mId, this);
        }

        IDMCPCallPtr mCall;
        LLUUID       mId;
        bool         mSettled = false;
    };

    // Replicated from llpanelavatar.cpp's put_avatar_properties_coro (that copy
    // is file-static). PUTs a one-or-more-key LLSD to the AgentProfile cap.
    void idmcp_put_profile_coro(std::string cap_url, LLUUID agent_id, LLSD data,
                                std::function<void(bool)> callback)
    {
        LLCore::HttpRequest::policy_t httpPolicy(LLCore::HttpRequest::DEFAULT_POLICY_ID);
        LLCoreHttpUtil::HttpCoroutineAdapter::ptr_t httpAdapter =
            std::make_shared<LLCoreHttpUtil::HttpCoroutineAdapter>("idmcp_put_profile_coro", httpPolicy);
        LLCore::HttpRequest::ptr_t httpRequest = std::make_shared<LLCore::HttpRequest>();
        LLCore::HttpHeaders::ptr_t httpHeaders;
        LLCore::HttpOptions::ptr_t httpOpts = std::make_shared<LLCore::HttpOptions>();
        httpOpts->setFollowRedirects(true);

        std::string finalUrl = cap_url + "/" + agent_id.asString();
        LLSD result = httpAdapter->putAndSuspend(httpRequest, finalUrl, data, httpOpts, httpHeaders);

        LLSD httpResults = result[LLCoreHttpUtil::HttpCoroutineAdapter::HTTP_RESULTS];
        LLCore::HttpStatus status = LLCoreHttpUtil::HttpCoroutineAdapter::getStatusFromLLSD(httpResults);

        if (callback)
        {
            callback(bool(status));
        }
    }

    // Shared by profile.get / getPicks / getClassifieds: viewing another
    // avatar's profile data is gated on @shownames. Self and a missing/invalid
    // avatar_id are always allowed (invoke defaults those to self).
    IDMCPGateResult idmcp_gate_target_shownames(const boost::json::object& args, IDMCPGatePhase)
    {
        if (!IDMCPRlvGate::isEnabled())
        {
            return IDMCPGateResult();
        }
        const std::string spec = arg_str(args, "avatar_id");
        if (!looks_like_uuid(spec))
        {
            return IDMCPGateResult();
        }
        LLUUID id(spec);
        if (id == gAgent.getID())
        {
            return IDMCPGateResult();
        }
        if (!RlvActions::canShowName(RlvActions::SNC_DEFAULT, id))
        {
            IDMCPGateResult r;
            r.allowed   = false;
            r.behaviour = "shownames";
            return r;
        }
        return IDMCPGateResult();
    }

    boost::json::value idmcp_pos_to_json(const LLVector3d& p)
    {
        boost::json::object o;
        o["x"] = p.mdV[VX];
        o["y"] = p.mdV[VY];
        o["z"] = p.mdV[VZ];
        return o;
    }

    boost::json::value pick_to_json(const LLPickData& d)
    {
        boost::json::object o;
        o["id"]            = d.pick_id.asString();
        o["name"]          = d.name;
        o["desc"]          = d.desc;
        o["snapshot_id"]   = d.snapshot_id.asString();
        o["parcel_id"]     = d.parcel_id.asString();
        o["sim_name"]      = d.sim_name;
        o["pos_global"]    = idmcp_pos_to_json(d.pos_global);
        // Vestigial: the viewer hardcodes 0 on every pick save ("Only top
        // picks have a sort order", llavatarpropertiesprocessor.cpp). Do not
        // build ordering on it - the listing order is the authority.
        o["sort_order"]    = d.sort_order;
        o["enabled"]       = d.enabled;
        o["original_name"] = d.original_name;
        o["user_name"]     = d.user_name;
        return o;
    }

    boost::json::value classified_to_json(const LLAvatarClassifiedInfo& d)
    {
        boost::json::object o;
        o["id"]                = d.classified_id.asString();
        o["name"]              = d.name;
        o["description"]       = d.description;
        o["snapshot_id"]       = d.snapshot_id.asString();
        o["parcel_id"]         = d.parcel_id.asString();
        o["parcel_name"]       = d.parcel_name;
        o["sim_name"]          = d.sim_name;
        o["pos_global"]        = idmcp_pos_to_json(d.pos_global);
        o["category"]          = static_cast<std::uint64_t>(d.category);
        o["creation_date"]     = static_cast<std::uint64_t>(d.creation_date);
        o["expiration_date"]   = static_cast<std::uint64_t>(d.expiration_date);
        o["flags"]             = static_cast<std::uint64_t>(d.flags);
        o["price_for_listing"] = d.price_for_listing;
        return o;
    }

    // ---- picks / classifieds detail fan-out ---------------------------------
    //
    // Descriptions are NOT in the AgentProfile cap - it fills picks_list with
    // (id, name) only - so each item needs its own legacy round-trip
    // (pickinforequest / ClassifiedInfoRequest). Four constraints shape this:
    //
    //  1. Detail replies dispatch on the OWNER's id, not the item id, so a
    //     fan-out over N items lands N replies on one observer; we demux on
    //     pick_id / classified_id.
    //  2. Detail requests are NOT pending-tracked by the processor, so N
    //     concurrent requests are not deduped down to one. (The LISTING
    //     requests ARE tracked, with a 5s window - hence listing_answered,
    //     which distinguishes "no items" from "listing never replied".)
    //  3. APT_CLASSIFIED_INFO notifies twice (creator_id and classified_id);
    //     we register on the owner only, so we see it once. The demux erase is
    //     idempotent regardless.
    //  4. IDMCPCall::setDeadline would make the server respond -32001 and
    //     DISCARD everything collected. Wrong for a fan-out: 4 of 5 replies
    //     should return 4 items. So the deadline is soft and owned here, and a
    //     partial result is a success carrying `missing`.
    //  5. Pick discovery branches on the AgentProfile cap (like profile.get
    //     and llpanelprofilepicks.cpp): cap present -> APT_PROPERTIES carries
    //     picks_list; cap absent -> the OpenSim-only avatarpicksrequest ->
    //     APT_PICKS. Classifieds discovery is unconditional, matching
    //     llpanelprofileclassifieds.cpp. Because the processor's listing
    //     dedupe window (5s) is shorter than our deadline (10s), a SECOND
    //     listing reply within one wait is reachable; every listing branch
    //     ignores it via listing_answered so completed ids are not re-pended.
    //  6. Output order is the LISTING order - what the viewer's own Picks/
    //     Classifieds tab shows - or, for explicit ids, the caller's array
    //     order. Detail replies land in network arrival order, so `order`
    //     records the authoritative sequence and finish emits by walking it.
    //     (sort_order is no help: the viewer saves every pick with 0.)
    //
    // Main-thread only.
    enum class IDMCPDetailKind { Picks, Classifieds };

    class IDMCPDetailObserver;

    struct IDMCPDetailWait
    {
        IDMCPCallPtr         call;
        LLUUID               target;
        IDMCPDetailKind      kind = IDMCPDetailKind::Picks;
        std::set<LLUUID>     pending;             // item ids awaiting a detail reply
        std::vector<LLUUID>  order;               // listing/caller order; the emit sequence
        std::map<LLUUID, boost::json::value> completed;   // items keyed by id for ordered emit
        bool                 listing_answered = false;
        bool                 awaiting_listing = true;
        F64                  deadline = 0.0;
        bool                 done = false;
        IDMCPDetailObserver* obs = nullptr;       // owned; reaped by the tick
    };

    using IDMCPDetailWaitPtr = std::shared_ptr<IDMCPDetailWait>;

    std::vector<IDMCPDetailWaitPtr> g_detail_waits;
    bool                            g_detail_tick_on = false;
    LLTempBoundListener             g_detail_tick;

    // Responds and marks done. Deliberately does NOT tear down the observer:
    // this is reachable from inside IDMCPDetailObserver::processProperties, and
    // deleting the observer there would destroy the very shared_ptr member the
    // caller is still holding. Teardown is deferred to idmcp_detail_tick.
    void idmcp_detail_finish(const IDMCPDetailWaitPtr& w)
    {
        if (w->done)
        {
            return;
        }
        w->done = true;   // set BEFORE responding: the call's cleanup re-enters here

        // Emit in `order` (listing/caller order), not arrival order. Every id
        // in `order` is either completed or still pending, so the walk also
        // yields `missing` in the same order.
        boost::json::array items;
        boost::json::array missing;
        for (const LLUUID& id : w->order)
        {
            auto it = w->completed.find(id);
            if (it != w->completed.end())
            {
                items.push_back(std::move(it->second));
                w->completed.erase(it);
            }
            else
            {
                missing.push_back(boost::json::value(id.asString()));
            }
        }
        // Defensive: a completed item somehow absent from `order` is appended
        // rather than dropped - losing a pick is worse than misplacing one.
        for (auto& entry : w->completed)
        {
            items.push_back(std::move(entry.second));
        }

        boost::json::object o;
        o["avatar_id"] = w->target.asString();
        o[(w->kind == IDMCPDetailKind::Picks) ? "picks" : "classifieds"] = std::move(items);
        o["missing"]          = std::move(missing);
        o["listing_answered"] = w->listing_answered;
        idmcp_tool_ok(w->call, o);
    }

    // Copies `pending` before iterating: cheap, and it removes any chance of
    // iterating a set that a reply mutates.
    void idmcp_detail_request_items(const IDMCPDetailWaitPtr& w)
    {
        const std::set<LLUUID> ids = w->pending;
        LLAvatarPropertiesProcessor* proc = LLAvatarPropertiesProcessor::getInstance();
        for (const LLUUID& id : ids)
        {
            if (w->kind == IDMCPDetailKind::Picks)
            {
                // Must pass the owner id: the pick database is sharded by creator.
                proc->sendPickInfoRequest(w->target, id);
            }
            else
            {
                proc->sendClassifiedInfoRequest(id);
            }
        }
    }

    class IDMCPDetailObserver : public LLAvatarPropertiesObserver
    {
    public:
        IDMCPDetailObserver(IDMCPDetailWaitPtr w, const LLUUID& target)
            : mWait(std::move(w)), mTarget(target) {}

        void processProperties(void* data, EAvatarProcessorType type) override
        {
            if (!mWait || mWait->done || !data)
            {
                return;
            }
            const bool picks = (mWait->kind == IDMCPDetailKind::Picks);

            if (picks && type == APT_PICKS)
            {
                if (mWait->listing_answered)
                {
                    return;
                }
                const LLAvatarPicks* p = static_cast<LLAvatarPicks*>(data);
                mWait->listing_answered = true;
                mWait->awaiting_listing = false;
                for (const auto& entry : p->picks_list)
                {
                    if (mWait->pending.insert(entry.first).second)
                    {
                        mWait->order.push_back(entry.first);
                    }
                }
                idmcp_detail_request_items(mWait);
            }
            else if (picks && type == APT_PROPERTIES)
            {
                // Cap-path pick listing: the AgentProfile reply's picks_list
                // is (id, name) pairs, same shape as APT_PICKS.
                if (mWait->listing_answered)
                {
                    return;
                }
                const LLAvatarData* p = static_cast<LLAvatarData*>(data);
                mWait->listing_answered = true;
                mWait->awaiting_listing = false;
                for (const auto& entry : p->picks_list)
                {
                    if (mWait->pending.insert(entry.first).second)
                    {
                        mWait->order.push_back(entry.first);
                    }
                }
                idmcp_detail_request_items(mWait);
            }
            else if (!picks && type == APT_CLASSIFIEDS)
            {
                if (mWait->listing_answered)
                {
                    return;
                }
                const LLAvatarClassifieds* c = static_cast<LLAvatarClassifieds*>(data);
                mWait->listing_answered = true;
                mWait->awaiting_listing = false;
                for (const auto& entry : c->classifieds_list)
                {
                    if (mWait->pending.insert(entry.classified_id).second)
                    {
                        mWait->order.push_back(entry.classified_id);
                    }
                }
                idmcp_detail_request_items(mWait);
            }
            else if (picks && type == APT_PICK_INFO)
            {
                const LLPickData* d = static_cast<LLPickData*>(data);
                if (mWait->pending.erase(d->pick_id))
                {
                    mWait->completed[d->pick_id] = pick_to_json(*d);
                }
            }
            else if (!picks && type == APT_CLASSIFIED_INFO)
            {
                const LLAvatarClassifiedInfo* d = static_cast<LLAvatarClassifiedInfo*>(data);
                if (mWait->pending.erase(d->classified_id))
                {
                    mWait->completed[d->classified_id] = classified_to_json(*d);
                }
            }
            else
            {
                return;   // not ours (e.g. APT_PROPERTIES on a classifieds wait)
            }

            if (!mWait->awaiting_listing && mWait->pending.empty())
            {
                idmcp_detail_finish(mWait);
            }
        }

        void detach()
        {
            if (mDetached)
            {
                return;
            }
            mDetached = true;
            LLAvatarPropertiesProcessor::getInstance()->removeObserver(mTarget, this);
        }

    private:
        IDMCPDetailWaitPtr mWait;
        LLUUID             mTarget;
        bool               mDetached = false;
    };

    void idmcp_detail_reap(const IDMCPDetailWaitPtr& w)
    {
        if (w->obs)
        {
            w->obs->detach();
            delete w->obs;
            w->obs = nullptr;
        }
    }

    void idmcp_detail_tick()
    {
        if (g_detail_waits.empty())
        {
            return;
        }
        const F64 now = LLTimer::getTotalSeconds();
        for (auto it = g_detail_waits.begin(); it != g_detail_waits.end(); )
        {
            const IDMCPDetailWaitPtr& w = *it;
            if (!w->done && now >= w->deadline)
            {
                idmcp_detail_finish(w);   // partial result is still a success
            }
            if (w->done)
            {
                idmcp_detail_reap(w);
                it = g_detail_waits.erase(it);
            }
            else
            {
                ++it;
            }
        }
    }

    void idmcp_detail_ensure_tick()
    {
        if (g_detail_tick_on)
        {
            return;
        }
        g_detail_tick = LLEventPumps::instance().obtain("mainloop").listen(
            "idmcp_profile_details", [](const LLSD&) -> bool { idmcp_detail_tick(); return false; });
        g_detail_tick_on = true;
    }

    void idmcp_detail_start(const boost::json::object& args, const IDMCPCallPtr& call,
                            IDMCPDetailKind kind)
    {
        const char* ids_key = (kind == IDMCPDetailKind::Picks) ? "pick_ids" : "classified_ids";
        const std::string spec = arg_str(args, "avatar_id");
        const LLUUID target = looks_like_uuid(spec) ? LLUUID(spec) : gAgent.getID();

        auto w = std::make_shared<IDMCPDetailWait>();
        w->call     = call;
        w->target   = target;
        w->kind     = kind;
        // Soft deadline; must stay under the server's hard 30s
        // (DEFAULT_TOOL_TIMEOUT, idmcpserver.cpp:30), past which sweepTimeouts
        // responds -32001 and discards everything collected.
        w->deadline = LLTimer::getTotalSeconds() + 10.0;

        // Explicit ids skip the listing stage entirely.
        auto it = args.find(ids_key);
        if (it != args.end() && it->value().is_array())
        {
            for (const auto& v : it->value().as_array())
            {
                if (!v.is_string())
                {
                    continue;
                }
                const std::string s = v.as_string().c_str();
                if (looks_like_uuid(s))
                {
                    // The caller's array order is the emit order here; a
                    // duplicate id keeps its first position.
                    LLUUID id(s);
                    if (w->pending.insert(id).second)
                    {
                        w->order.push_back(id);
                    }
                }
            }
        }
        const bool explicit_ids = !w->pending.empty();
        w->awaiting_listing = !explicit_ids;
        w->listing_answered = explicit_ids;   // no listing was needed

        w->obs = new IDMCPDetailObserver(w, target);
        LLAvatarPropertiesProcessor::getInstance()->addObserver(target, w->obs);
        g_detail_waits.push_back(w);
        idmcp_detail_ensure_tick();

        // weak_ptr, NOT a copy of w: the wait holds `call`, so capturing w
        // strongly here would create a shared_ptr cycle and leak both.
        std::weak_ptr<IDMCPDetailWait> weak = w;
        call->setCleanup([weak]()
        {
            if (auto p = weak.lock())
            {
                p->done = true;   // idmcp_detail_tick reaps the observer next frame
            }
        });

        if (explicit_ids)
        {
            idmcp_detail_request_items(w);
        }
        else if (kind == IDMCPDetailKind::Picks)
        {
            // avatarpicksrequest is OpenSim-only; on SL the pick list rides
            // the AgentProfile cap reply. Same branch as profile.get above
            // and llpanelprofilepicks.cpp.
            if (!gAgent.getRegionCapability("AgentProfile").empty())
            {
                LLAvatarPropertiesProcessor::getInstance()->sendAvatarPropertiesRequest(target);
            }
            else
            {
                LLAvatarPropertiesProcessor::getInstance()->sendAvatarPicksRequest(target);
            }
        }
        else
        {
            LLAvatarPropertiesProcessor::getInstance()->sendAvatarClassifiedsRequest(target);
        }
    }
}

// ---------------------------------------------------------------------------

void idmcp_register_profile_tools(IDMCPToolRegistry& reg)
{
    // profile.get ------------------------------------------------------------
    {
        IDMCPTool t;
        t.name = "profile.get";
        t.description =
            "Fetch an avatar's profile. Optional {\"avatar_id\"} (UUID) defaults "
            "to yourself. Viewing another avatar is blocked by RLV @shownames.";
        t.input_schema = boost::json::parse(
            R"({"type":"object","properties":{"avatar_id":{"type":"string"}},"additionalProperties":false})");
        t.gate = idmcp_gate_target_shownames;
        t.invoke = [](const boost::json::object& args, const IDMCPCallPtr& call)
        {
            const std::string spec = arg_str(args, "avatar_id");
            LLUUID id = looks_like_uuid(spec) ? LLUUID(spec) : gAgent.getID();

            IDMCPProfileObserver* obs = new IDMCPProfileObserver(call, id);
            LLAvatarPropertiesProcessor::getInstance()->addObserver(id, obs);
            call->setCleanup([obs]() { obs->abandon(); });

            if (!gAgent.getRegionCapability("AgentProfile").empty())
            {
                LLAvatarPropertiesProcessor::getInstance()->sendAvatarPropertiesRequest(id);
            }
            else
            {
                LLAvatarPropertiesProcessor::getInstance()->sendAvatarLegacyPropertiesRequest(id);
            }
        };
        reg.add(std::move(t));
    }

    // profile.getPicks -------------------------------------------------------
    {
        IDMCPTool t;
        t.name = "profile.getPicks";
        t.description =
            "Read the FULL TEXT of an avatar's profile picks - description, snapshot "
            "and location - which profile.get cannot return (it gives ids and names "
            "only). Optional {\"avatar_id\"} defaults to yourself; optional "
            "{\"pick_ids\"} (array of UUIDs) reads just those and skips discovery. "
            "Each pick needs its own round-trip, so results can be PARTIAL: any id "
            "that did not answer within ~10s is listed in \"missing\". "
            "\"listing_answered\" is false if the pick list itself never came back "
            "(distinguishing that from an avatar who simply has no picks). Picks "
            "return in the profile's listing order - what the viewer's Picks tab "
            "shows (\"sort_order\" is vestigial, always 0; do not sort by it). "
            "\"sim_name\" is usually empty on this reply; \"pos_global\" and "
            "\"original_name\" are the reliable location signals. Picks are often "
            "used as prose - a serialized backstory, or a captioned gallery - so "
            "read \"desc\" in the order given. Blocked by RLV @shownames.";
        t.input_schema = boost::json::parse(
            R"({"type":"object","properties":{"avatar_id":{"type":"string"},"pick_ids":{"type":"array","items":{"type":"string"}}},"additionalProperties":false})");
        t.gate = idmcp_gate_target_shownames;
        t.invoke = [](const boost::json::object& args, const IDMCPCallPtr& call)
        {
            idmcp_detail_start(args, call, IDMCPDetailKind::Picks);
        };
        reg.add(std::move(t));
    }

    // profile.getClassifieds -------------------------------------------------
    {
        IDMCPTool t;
        t.name = "profile.getClassifieds";
        t.description =
            "Read an avatar's classified ads in full - description, snapshot, location, "
            "category, price and expiry. profile.get does not list classifieds at all, "
            "so this tool discovers them itself. Optional {\"avatar_id\"} defaults to "
            "yourself; optional {\"classified_ids\"} (array of UUIDs) reads just those "
            "and skips discovery. Each ad needs its own round-trip, so results can be "
            "PARTIAL: ids that did not answer within ~10s appear in \"missing\". "
            "\"listing_answered\" is false if the list itself never came back, as "
            "opposed to the avatar having no classifieds. Ads return in the "
            "profile's listing order. Blocked by RLV @shownames.";
        t.input_schema = boost::json::parse(
            R"({"type":"object","properties":{"avatar_id":{"type":"string"},"classified_ids":{"type":"array","items":{"type":"string"}}},"additionalProperties":false})");
        t.gate = idmcp_gate_target_shownames;
        t.invoke = [](const boost::json::object& args, const IDMCPCallPtr& call)
        {
            idmcp_detail_start(args, call, IDMCPDetailKind::Classifieds);
        };
        reg.add(std::move(t));
    }

    // profile.setSelf --------------------------------------------------------
    {
        IDMCPTool t;
        t.name = "profile.setSelf";
        t.description =
            "Update your own profile. Any of {\"about_text\", \"fl_about_text\", "
            "\"allow_publish\" (bool), \"hide_age\" (bool), \"image_id\", "
            "\"fl_image_id\"}. Blocked by RLV @editprofile (and @editpfp for images).";
        t.input_schema = boost::json::parse(
            R"({"type":"object","properties":{"about_text":{"type":"string"},"fl_about_text":{"type":"string"},"allow_publish":{"type":"boolean"},"hide_age":{"type":"boolean"},"image_id":{"type":"string"},"fl_image_id":{"type":"string"}},"additionalProperties":false})");
        t.gate = [](const boost::json::object& args, IDMCPGatePhase) -> IDMCPGateResult
        {
            if (!IDMCPRlvGate::isEnabled())
            {
                return IDMCPGateResult();
            }
            if (!RlvActions::canEditProfile())
            {
                IDMCPGateResult r; r.allowed = false; r.behaviour = "editprofile"; return r;
            }
            const bool touches_image =
                args.contains("image_id") || args.contains("fl_image_id");
            if (touches_image && !RlvActions::canEditProfileImage())
            {
                IDMCPGateResult r; r.allowed = false; r.behaviour = "editpfp"; return r;
            }
            return IDMCPGateResult();
        };
        t.invoke = [](const boost::json::object& args, const IDMCPCallPtr& call)
        {
            LLSD data;
            if (args.contains("about_text"))    data["sl_about_text"] = arg_str(args, "about_text");
            if (args.contains("fl_about_text")) data["fl_about_text"] = arg_str(args, "fl_about_text");
            if (args.contains("allow_publish") && args.at("allow_publish").is_bool())
                data["allow_publish"] = args.at("allow_publish").as_bool();
            if (args.contains("hide_age") && args.at("hide_age").is_bool())
                data["hide_age"] = args.at("hide_age").as_bool();
            if (looks_like_uuid(arg_str(args, "image_id")))
                data["sl_image_id"] = LLUUID(arg_str(args, "image_id"));
            if (looks_like_uuid(arg_str(args, "fl_image_id")))
                data["fl_image_id"] = LLUUID(arg_str(args, "fl_image_id"));

            if (data.size() == 0)
            {
                idmcp_tool_err(call, IDMCP_ERR_INVALID_PARAMS, "no updatable fields supplied");
                return;
            }

            const std::string cap = gAgent.getRegionCapability("AgentProfile");
            if (cap.empty())
            {
                idmcp_tool_err(call, IDMCP_ERR_CAP_UNAVAIL, "AgentProfile capability unavailable");
                return;
            }

            const LLUUID self = gAgent.getID();
            LLCoros::instance().launch("idmcp_put_profile", [call, cap, self, data]()
            {
                idmcp_put_profile_coro(cap, self, data, [call](bool ok)
                {
                    if (ok)
                    {
                        boost::json::object o; o["saved"] = true;
                        idmcp_tool_ok(call, o);
                    }
                    else
                    {
                        idmcp_tool_err(call, IDMCP_ERR_PERMISSION, "profile update failed");
                    }
                });
            });
        };
        reg.add(std::move(t));
    }

    // people.getNames --------------------------------------------------------
    {
        IDMCPTool t;
        t.name = "people.getNames";
        t.description =
            "Resolve avatar UUIDs to names. {\"ids\"} is an array of UUIDs. "
            "Cached names return immediately; misses are warmed for a retry. "
            "Names hidden by RLV @shownames are omitted.";
        t.input_schema = boost::json::parse(
            R"({"type":"object","properties":{"ids":{"type":"array","items":{"type":"string"}}},"required":["ids"],"additionalProperties":false})");
        t.invoke = [](const boost::json::object& args, const IDMCPCallPtr& call)
        {
            boost::json::array out;
            auto it = args.find("ids");
            if (it != args.end() && it->value().is_array())
            {
                const bool rlv = IDMCPRlvGate::isEnabled();
                for (const auto& v : it->value().as_array())
                {
                    if (!v.is_string()) continue;
                    std::string s = v.as_string().c_str();
                    if (!looks_like_uuid(s)) continue;
                    LLUUID id(s);

                    boost::json::object o;
                    o["id"] = id.asString();

                    if (rlv && id != gAgent.getID() &&
                        !RlvActions::canShowName(RlvActions::SNC_DEFAULT, id))
                    {
                        o["hidden"] = true;
                        out.push_back(std::move(o));
                        continue;
                    }

                    LLAvatarName av;
                    if (LLAvatarNameCache::get(id, &av))
                    {
                        o["username"]     = av.getUserName();
                        o["display_name"] = av.getDisplayName();
                        o["complete"]     = av.getCompleteName();
                        o["cached"]       = true;
                    }
                    else
                    {
                        o["cached"] = false;
                        LLAvatarNameCache::getInstance()->fetch(id);   // warm for retry
                    }
                    out.push_back(std::move(o));
                }
            }
            boost::json::object result;
            result["names"] = std::move(out);
            idmcp_tool_ok(call, result);
        };
        reg.add(std::move(t));
    }

    // people.getFriends ------------------------------------------------------
    {
        IDMCPTool t;
        t.name = "people.getFriends";
        t.description =
            "List your friends/contacts: {id, name, online, they_grant_me, "
            "i_grant_them}. The two rights objects are see-online / see-on-map / "
            "modify-objects in each direction (they_grant_me = what the friend "
            "granted you; i_grant_them = what you granted the friend). Optional "
            "{\"online_only\":true} returns only friends currently shown online. "
            "NOTE: a friend's online flag is only meaningful (and only ever true) "
            "when they granted you see-online (they_grant_me.online) - otherwise it "
            "is always false regardless of whether they're actually online. Names "
            "hidden by RLV @shownames are omitted.";
        t.input_schema = boost::json::parse(
            R"({"type":"object","properties":{"online_only":{"type":"boolean"}},"additionalProperties":false})");
        t.invoke = [](const boost::json::object& args, const IDMCPCallPtr& call)
        {
            const bool online_only = arg_bool(args, "online_only", false);
            const bool rlv         = IDMCPRlvGate::isEnabled();

            LLAvatarTracker::buddy_map_t buddies;
            LLAvatarTracker::instance().copyBuddyList(buddies);

            boost::json::array arr;
            for (const auto& entry : buddies)
            {
                const LLUUID&         id  = entry.first;
                const LLRelationship* rel = entry.second;
                if (!rel)
                {
                    continue;
                }
                const bool online = rel->isOnline();
                if (online_only && !online)
                {
                    continue;
                }

                boost::json::object o;
                o["id"] = id.asString();
                if (!(rlv && !RlvActions::canShowName(RlvActions::SNC_DEFAULT, id)))
                {
                    LLAvatarName av;
                    if (LLAvatarNameCache::get(id, &av))
                    {
                        o["name"] = av.getCompleteName();
                    }
                }
                o["online"] = online;

                const S32 from = rel->getRightsGrantedFrom();   // friend -> me
                const S32 to   = rel->getRightsGrantedTo();     // me -> friend
                boost::json::object they, mine;
                they["online"] = (bool)(from & LLRelationship::GRANT_ONLINE_STATUS);
                they["map"]    = (bool)(from & LLRelationship::GRANT_MAP_LOCATION);
                they["modify"] = (bool)(from & LLRelationship::GRANT_MODIFY_OBJECTS);
                mine["online"] = (bool)(to & LLRelationship::GRANT_ONLINE_STATUS);
                mine["map"]    = (bool)(to & LLRelationship::GRANT_MAP_LOCATION);
                mine["modify"] = (bool)(to & LLRelationship::GRANT_MODIFY_OBJECTS);
                o["they_grant_me"] = std::move(they);
                o["i_grant_them"]  = std::move(mine);

                arr.push_back(std::move(o));
            }

            boost::json::object result;
            result["count"]   = (int)arr.size();
            result["friends"] = std::move(arr);
            idmcp_tool_ok(call, result);
        };
        reg.add(std::move(t));
    }
}
