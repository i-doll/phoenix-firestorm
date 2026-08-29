/**
 * @file idmcptools_movement.cpp
 * @brief <ID> MCP server: embodiment - teleport, sit/stand, self-location, object touch.
 *
 * Part of Five's custom Firestorm fork. Custom code carries an `ID` prefix.
 *
 * These turn the avatar from "a hand reaching into inventory" into someone
 * standing in a place: it can go somewhere (teleport), sit/stand, know where it
 * is (agent.getLocation), and operate in-world objects (object.touch).
 *
 * Teleport and sit/stand are ASYNC - the viewer sends a request and the result
 * lands later. We use the shared deferred pattern (a wait struct + a "mainloop"
 * LLTempBoundListener sweep, as in idmcptools_avatars.cpp): teleport resolves off
 * LLViewerParcelMgr's finished/failed callbacks (state polling is ambiguous -
 * both success and failure return to TELEPORT_NONE); sit/stand poll isSitting().
 * object.touch is synchronous fire-and-forget (a grab immediately followed by a
 * degrab, per LLAgentListener::requestTouch). agent.getLocation is a pure read.
 * All main-thread only. RLV-gated per the tokens noted on each tool.
 */

#include "llviewerprecompiledheaders.h"

#include "idmcp.h"
#include "idmcptools.h"
#include "idmcpserver.h"
#include "idmcprlvgate.h"

#include "llagent.h"                 // gAgent: teleport*, standUp, getPosition*
#include "llvoavatarself.h"          // gAgentAvatarp, isAgentAvatarValid
#include "llvoavatar.h"              // isSitting
#include "llviewerobjectlist.h"      // gObjectList
#include "llviewerobject.h"          // LLViewerObject
#include "llviewerregion.h"          // getName, getHost
#include "llviewerparcelmgr.h"       // LLViewerParcelMgr + teleport callbacks
#include "llparcel.h"                // LLParcel
#include "llviewerinventory.h"       // LLViewerInventoryItem
#include "llinventorymodel.h"        // gInventory
#include "llviewerwindow.h"          // LLPickInfo
#include "lltoolgrab.h"              // send_ObjectGrab_message / send_ObjectDeGrab_message

#include "rlvactions.h"
#include "rlvhandler.h"              // gRlvHandler, RLV_BHVR_*

#include "aoengine.h"                // AOEngine (Firestorm animation overlay)
#include "aoset.h"                   // AOSet::getName
#include "llviewercontrol.h"         // gSavedPerAccountSettings (UseAO)

#include "message.h"                 // gMessageSystem, _PREHASH_*
#include "llevents.h"                // LLEventPumps, LLTempBoundListener
#include "lltimer.h"                 // LLTimer::getTotalSeconds
#include "lluuid.h"
#include "llassettype.h"
#include "llslurl.h"                 // parse a SLURL -> region + position
#include "llworldmapmessage.h"       // sendNamedRegionRequest (resolve region name)
#include "llregionhandle.h"          // from_region_handle

#include "pipeline.h"                // gPipeline.lineSegmentIntersectInWorld (pathfinding rays)
#include "llvector4a.h"
#include "llworld.h"                 // resolveLandHeightGlobal (terrain floor fallback)

#include <algorithm>
#include <cmath>
#include <memory>
#include <queue>
#include <vector>

namespace
{
    // ---- arg helpers -------------------------------------------------------

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

    S32 arg_int(const boost::json::object& args, const char* key, S32 dflt)
    {
        auto it = args.find(key);
        if (it == args.end()) return dflt;
        if (it->value().is_int64())  return (S32)it->value().as_int64();
        if (it->value().is_double()) return (S32)it->value().as_double();
        return dflt;
    }

    bool arg_has(const boost::json::object& args, const char* key)
    {
        return args.find(key) != args.end();
    }

    // Read a [x,y,z] JSON array of numbers into an LLVector3d. Returns false if
    // the key is absent or not a 3-number array.
    bool arg_vec3d(const boost::json::object& args, const char* key, LLVector3d& out)
    {
        auto it = args.find(key);
        if (it == args.end() || !it->value().is_array()) return false;
        const boost::json::array& a = it->value().as_array();
        if (a.size() != 3) return false;
        double v[3];
        for (int i = 0; i < 3; ++i)
        {
            if      (a[i].is_double()) v[i] = a[i].as_double();
            else if (a[i].is_int64())  v[i] = (double)a[i].as_int64();
            else if (a[i].is_uint64()) v[i] = (double)a[i].as_uint64();
            else return false;
        }
        out = LLVector3d(v[0], v[1], v[2]);
        return true;
    }

    IDMCPGateResult deny(const char* label)
    {
        IDMCPGateResult r;
        r.allowed   = false;
        r.behaviour = label;
        return r;
    }

    // ---- teleport destination resolution (shared by gate + invoke) ---------
    //
    // Exactly one of landmark_item_id / global_position / avatar_id / slurl selects
    // the destination. For the location forms we can compute a global position (used
    // for the local-vs-remote RLV split); for landmark we resolve the inventory item
    // to its asset id; for a SLURL we resolve the region name asynchronously.
    enum TpForm { TP_NONE, TP_LANDMARK, TP_LOCATION, TP_SLURL };

    TpForm tp_form(const boost::json::object& args)
    {
        const int n = (int)arg_has(args, "landmark_item_id")
                    + (int)arg_has(args, "global_position")
                    + (int)arg_has(args, "avatar_id")
                    + (int)arg_has(args, "slurl");
        if (n != 1) return TP_NONE;
        if (arg_has(args, "landmark_item_id")) return TP_LANDMARK;
        if (arg_has(args, "slurl"))            return TP_SLURL;
        return TP_LOCATION;
    }

    // Best-effort destination global position for the location forms. Returns
    // false if it can't be determined (e.g. avatar not in range).
    bool tp_location(const boost::json::object& args, LLVector3d& pos)
    {
        if (arg_vec3d(args, "global_position", pos)) return true;
        const std::string spec = arg_str(args, "avatar_id");
        if (looks_like_uuid(spec))
        {
            LLViewerObject* vo = gObjectList.findObject(LLUUID(spec));
            if (vo)
            {
                pos = vo->getPositionGlobal();
                return true;
            }
        }
        return false;
    }

    // ---- deferred waits ----------------------------------------------------

    // Teleport: resolved by the parcel-manager finished/failed callbacks, or a
    // deadline. The callbacks are global (fire for any teleport), so a wait holds
    // its own one-shot connections and resolves itself.
    struct TpWait
    {
        IDMCPCallPtr                  call;
        F64                           deadline = 0.0;
        bool                          done = false;
        boost::signals2::connection   finished;
        boost::signals2::connection   failed;
    };

    // Sit/stand: poll the avatar's sitting state until it matches, or a deadline.
    struct PollWait
    {
        IDMCPCallPtr call;
        F64          deadline = 0.0;
        bool         done = false;
        bool         want_sitting = false;   // target state
        LLUUID       object_id;              // for the sit result
    };

    // Walk-to (autopilot): resolved when gAgent.getAutoPilot() clears (arrived or
    // gave up) or a deadline passes; status is judged by final distance to target.
    struct WalkWait
    {
        IDMCPCallPtr call;
        LLVector3d   target;
        F64          deadline = 0.0;
        bool         done = false;
    };

    // Pathfinding: a raycast occupancy grid + incremental A* + smoothed waypoint
    // walk. Rays are budgeted per mainloop tick so a large grid never stalls a
    // frame. Single-floor (grid sits at the start's floor Z).
    struct PathWait
    {
        IDMCPCallPtr call;
        LLVector3d   goal;
        F64          minX = 0.0, minY = 0.0;   // grid origin corner (global)
        F32          cell = 1.0f;
        int          cols = 0, rows = 0;
        int          startIdx = 0, goalIdx = 0;
        std::vector<F64>    cellZ;   // per-cell floor Z (PATH_Z_UNKNOWN = unresolved)
        std::vector<float>  gscore;
        std::vector<int>    parent;
        std::vector<char>   closed;
        std::priority_queue<std::pair<float, int>,
                            std::vector<std::pair<float, int>>,
                            std::greater<std::pair<float, int>>> open;
        int          expanded = 0;
        std::vector<LLVector3d> waypoints;
        size_t       wp = 0;
        bool         leg_started = false;
        enum Phase { PLAN, WALK } phase = PLAN;
        F64          deadline = 0.0;   // plan deadline, then walk deadline
        bool         done = false;
    };

    std::vector<std::shared_ptr<TpWait>>   g_tp_waits;
    std::vector<std::shared_ptr<PollWait>> g_poll_waits;
    std::vector<std::shared_ptr<WalkWait>> g_walk_waits;
    std::vector<std::shared_ptr<PathWait>> g_path_waits;
    bool                                   g_tick_on = false;
    LLTempBoundListener                    g_tick;
    bool                                   g_slurl_resolving = false;  // SLURL teleport awaiting region resolution

    void path_tick();   // defined further down; driven from movement_tick

    // A deferred move (teleport / sit / stand / walk) is in flight. Used to reject
    // a second move issued before the first completes: overlapping calls otherwise
    // stomp each other (a new startAutoPilotGlobal / teleport overrides the prior
    // one), which reads to the caller as "arrived, but ended up somewhere else".
    bool movement_busy()
    {
        if (g_slurl_resolving) return true;
        for (const auto& w : g_tp_waits)   if (w && !w->done) return true;
        for (const auto& w : g_poll_waits) if (w && !w->done) return true;
        for (const auto& w : g_walk_waits) if (w && !w->done) return true;
        for (const auto& w : g_path_waits) if (w && !w->done) return true;
        return false;
    }

    void tp_finish(const std::shared_ptr<TpWait>& w, const char* status)
    {
        if (w->done) return;
        w->done = true;
        w->finished.disconnect();
        w->failed.disconnect();
        boost::json::object o;
        o["status"] = status;
        if (LLViewerRegion* r = gAgent.getRegion())
        {
            o["region"] = r->getName();
        }
        const LLVector3d p = gAgent.getPositionGlobal();
        boost::json::array pa;
        pa.push_back(p.mdV[0]); pa.push_back(p.mdV[1]); pa.push_back(p.mdV[2]);
        o["global_position"] = std::move(pa);
        idmcp_tool_ok(w->call, o);
    }

    void poll_finish(const std::shared_ptr<PollWait>& w)
    {
        if (w->done) return;
        w->done = true;
        const bool sitting = isAgentAvatarValid() && gAgentAvatarp->isSitting();
        boost::json::object o;
        o["sitting"] = sitting;
        if (!w->object_id.isNull()) o["object_id"] = w->object_id.asString();
        idmcp_tool_ok(w->call, o);
    }

    void walk_finish(const std::shared_ptr<WalkWait>& w)
    {
        if (w->done) return;
        w->done = true;
        const F64 dist = (gAgent.getPositionGlobal() - w->target).length();
        boost::json::object o;
        o["status"]   = (dist <= 3.0) ? "arrived" : "stopped";
        o["distance"] = dist;
        idmcp_tool_ok(w->call, o);
    }

    void movement_tick()
    {
        const F64 now = LLTimer::getTotalSeconds();

        for (auto it = g_tp_waits.begin(); it != g_tp_waits.end(); )
        {
            if (!(*it)->done && now >= (*it)->deadline) tp_finish(*it, "timeout");
            if ((*it)->done) it = g_tp_waits.erase(it);
            else             ++it;
        }

        for (auto it = g_poll_waits.begin(); it != g_poll_waits.end(); )
        {
            auto& w = *it;
            const bool sitting = isAgentAvatarValid() && gAgentAvatarp->isSitting();
            if (!w->done && (sitting == w->want_sitting || now >= w->deadline)) poll_finish(w);
            if (w->done) it = g_poll_waits.erase(it);
            else         ++it;
        }

        for (auto it = g_walk_waits.begin(); it != g_walk_waits.end(); )
        {
            auto& w = *it;
            if (!w->done && (!gAgent.getAutoPilot() || now >= w->deadline))
            {
                if (gAgent.getAutoPilot()) gAgent.stopAutoPilot(true);   // deadline hit mid-walk
                walk_finish(w);
            }
            if (w->done) it = g_walk_waits.erase(it);
            else         ++it;
        }

        path_tick();
    }

    void ensure_tick()
    {
        if (g_tick_on) return;
        g_tick = LLEventPumps::instance().obtain("mainloop").listen(
            "idmcp_movement", [](const LLSD&) -> bool { movement_tick(); return false; });
        g_tick_on = true;
    }

    void start_tp_wait(const IDMCPCallPtr& call)
    {
        auto w = std::make_shared<TpWait>();
        w->call     = call;
        w->deadline = LLTimer::getTotalSeconds() + 60.0;   // teleports are slow
        std::weak_ptr<TpWait> weak = w;
        LLViewerParcelMgr* pm = LLViewerParcelMgr::getInstance();
        w->finished = pm->setTeleportFinishedCallback(
            [weak](const LLVector3d&, const bool&) { if (auto s = weak.lock()) tp_finish(s, "arrived"); });
        w->failed = pm->setTeleportFailedCallback(
            [weak]() { if (auto s = weak.lock()) tp_finish(s, "failed"); });
        g_tp_waits.push_back(w);
        ensure_tick();
    }

    void start_poll_wait(const IDMCPCallPtr& call, bool want_sitting, const LLUUID& object_id)
    {
        auto w = std::make_shared<PollWait>();
        w->call         = call;
        w->deadline     = LLTimer::getTotalSeconds() + 5.0;
        w->want_sitting = want_sitting;
        w->object_id    = object_id;
        g_poll_waits.push_back(w);
        ensure_tick();
    }

    // Resolve a walk-to destination: exactly one of global_position / object_id /
    // avatar_id (both object and avatar resolve to their in-world position).
    bool walk_target(const boost::json::object& args, LLVector3d& pos)
    {
        if (arg_vec3d(args, "global_position", pos)) return true;
        const std::string oid = arg_str(args, "object_id");
        if (looks_like_uuid(oid))
        {
            if (LLViewerObject* vo = gObjectList.findObject(LLUUID(oid))) { pos = vo->getPositionGlobal(); return true; }
        }
        const std::string aid = arg_str(args, "avatar_id");
        if (looks_like_uuid(aid))
        {
            if (LLViewerObject* vo = gObjectList.findObject(LLUUID(aid))) { pos = vo->getPositionGlobal(); return true; }
        }
        return false;
    }

    void start_walk_wait(const IDMCPCallPtr& call, const LLVector3d& target)
    {
        auto w = std::make_shared<WalkWait>();
        w->call     = call;
        w->target   = target;
        w->deadline = LLTimer::getTotalSeconds() + 60.0;   // walking is slow
        g_walk_waits.push_back(w);
        ensure_tick();
    }

    // ==================== pathfinding ========================================

    static const F32 PATH_CELL       = 1.0f;    // grid cell size (m)
    static const F64 PATH_PAD        = 6.0;     // area padding around start+goal (m)
    static const F64 PATH_MAX_DIM    = 56.0;    // max grid span per axis (m)
    static const int PATH_MAX_CELLS  = 6000;    // A* expansion cap
    static const int PATH_RAY_BUDGET = 700;     // rays per mainloop tick
    static const F32 PATH_KNEE       = 0.35f;   // edge-ray heights above the floor
    static const F32 PATH_CHEST      = 0.95f;
    static const F64 PATH_STEP       = 0.8;     // max floor-Z change between adjacent cells (stairs/ramps)
    static const F32 PATH_LEG_STOP   = 0.4f;    // per-leg autopilot stop distance
    static const F64 PATH_PLAN_SECS  = 20.0;
    static const F64 PATH_WALK_SECS  = 120.0;
    static const F64 PATH_Z_UNKNOWN  = -1.0e30; // sentinel: cell floor not yet resolved

    // Raycast between two GLOBAL points; reports whether a SOLID obstacle blocks the
    // segment and the hit point. Rays run in the agent frame. PHANTOM objects (walk-
    // through) and ATTACHMENTS (the avatar's own / others' worn prims) are NOT
    // obstacles - the ray passes THROUGH them and continues to the real surface
    // behind, rather than stopping on your own hip prim and reporting a bogus floor.
    struct RayHit { bool blocked = false; LLVector3d point; };
    RayHit path_ray(const LLVector3d& a_global, const LLVector3d& b_global)
    {
        RayHit r;
        const LLVector3  bAgent = gAgent.getPosAgentFromGlobal(b_global);
        LLVector4a end; end.load3(bAgent.mV);
        const F64 total = (b_global - a_global).length();
        if (total < 1e-4) return r;
        const LLVector3d dir = (b_global - a_global) * (1.0 / total);

        LLVector3d cur = a_global;
        for (int iter = 0; iter < 8; ++iter)   // skip up to 8 pass-through hits
        {
            const LLVector3 aAgent = gAgent.getPosAgentFromGlobal(cur);
            LLVector4a start; start.load3(aAgent.mV);
            S32 face = -1;
            LLVector4a isect;
            LLViewerObject* hit = gPipeline.lineSegmentIntersectInWorld(
                start, end, /*transparent*/ true, /*rigged*/ false, /*unselectable*/ true,
                /*reflection*/ false, &face, nullptr, nullptr, &isect);
            if (!hit) return r;   // nothing solid the rest of the way
            const F32* p = isect.getF32ptr();
            const LLVector3d hp = gAgent.getPosGlobalFromAgent(LLVector3(p[0], p[1], p[2]));
            // A real obstacle/floor = something the avatar actually collides with:
            // not phantom, not a worn attachment, and with a physics shape (objects
            // set to PHYSICS_SHAPE_NONE are walk-through decoration). This is the
            // physics-shape test for walkable-vs-blocking.
            if (!hit->flagPhantom() && !hit->isAttachment()
                && hit->getPhysicsShapeType() != LLViewerObject::PHYSICS_SHAPE_NONE)
            {
                r.blocked = true;
                r.point   = hp;
                return r;
            }
            // Pass through this non-colliding hit: resume just past it toward the end.
            const LLVector3d next = hp + dir * 0.05;
            if ((next - a_global).length() >= total) return r;   // reached the far end
            cur = next;
        }
        return r;   // too many pass-throughs; treat as clear
    }

    void p_xy(const PathWait& w, int idx, F64& x, F64& y)
    {
        const int i = idx % w.cols, j = idx / w.cols;
        x = w.minX + (i + 0.5) * w.cell;
        y = w.minY + (j + 0.5) * w.cell;
    }
    LLVector3d p_pt(const PathWait& w, int idx, F64 z)
    {
        F64 x, y; p_xy(w, idx, x, y);
        return LLVector3d(x, y, z);
    }

    // Floor Z under cell `idx`, searched in a narrow band around `refZ` (the floor
    // of the cell we're stepping from). The narrow band follows stairs/ramps up and
    // down and won't grab a ceiling far above. False if no floor is near. 1 ray.
    bool p_floor(const PathWait& w, int idx, F64 refZ, F64& outZ, int& budget)
    {
        F64 x, y; p_xy(w, idx, x, y);
        LLVector3d top(x, y, refZ + PATH_STEP + 0.4);
        LLVector3d bot(x, y, refZ - PATH_STEP - 0.4);
        RayHit h = path_ray(top, bot); --budget;
        if (h.blocked) { outZ = h.point.mdV[2]; return true; }   // prim / mesh floor
        // No object floor near this level - fall back to the terrain height, so
        // ground-level parcels (and gaps between floor prims) still register.
        const F32 land = (F32)LLWorld::getInstance()->resolveLandHeightGlobal(LLVector3d(x, y, refZ));
        if (std::fabs((F64)land - refZ) <= (PATH_STEP + 0.4)) { outZ = (F64)land; return true; }
        return false;
    }

    // Knee- and chest-height rays between two floor points (sloped along the
    // surface); clear = no wall between them. Up to 2 rays.
    bool p_edge(const PathWait& w, int a, F64 az, int b, F64 bz, int& budget)
    {
        const LLVector3d pa = p_pt(w, a, az), pb = p_pt(w, b, bz);
        for (F32 h : { PATH_KNEE, PATH_CHEST })
        {
            LLVector3d qa = pa; qa.mdV[2] = az + h;
            LLVector3d qb = pb; qb.mdV[2] = bz + h;
            --budget;
            if (path_ray(qa, qb).blocked) return false;
        }
        return true;
    }

    // Can we step from cell `cur` (floor at fromZ) into cell `nb`? Resolves nb's
    // floor near fromZ (cached once known), checks the step height, and that the
    // edge is clear. Sets nbZ.
    bool p_step(PathWait& w, int cur, F64 fromZ, int nb, F64& nbZ, int& budget)
    {
        if (w.cellZ[nb] > PATH_Z_UNKNOWN) nbZ = w.cellZ[nb];
        else if (!p_floor(w, nb, fromZ, nbZ, budget)) return false;
        if (std::fabs(nbZ - fromZ) > PATH_STEP) return false;   // too steep -> wall/cliff
        return p_edge(w, cur, fromZ, nb, nbZ, budget);
    }

    // Clear straight line between two GLOBAL floor points (for smoothing): rays at
    // knee+chest above each end, so a flat run merges but a staircase keeps its steps.
    bool p_los(const LLVector3d& a, const LLVector3d& b)
    {
        for (F32 h : { PATH_KNEE, PATH_CHEST })
        {
            LLVector3d pa = a; pa.mdV[2] = a.mdV[2] + h;
            LLVector3d pb = b; pb.mdV[2] = b.mdV[2] + h;
            if (path_ray(pa, pb).blocked) return false;
        }
        return true;
    }

    F32 p_heur(const PathWait& w, int idx)
    {
        const int i = idx % w.cols, j = idx / w.cols;
        const int gi = w.goalIdx % w.cols, gj = w.goalIdx / w.cols;
        const F32 dx = (F32)(i - gi), dy = (F32)(j - gj);
        return std::sqrt(dx * dx + dy * dy) * w.cell;
    }

    void path_report(const std::shared_ptr<PathWait>& w, const char* status)
    {
        if (w->done) return;
        w->done = true;
        const F64 dist = (gAgent.getPositionGlobal() - w->goal).length();
        boost::json::object o;
        o["status"]    = status;
        o["distance"]  = dist;
        o["waypoints"] = (int)w->waypoints.size();
        idmcp_tool_ok(w->call, o);
    }
    void path_err(const std::shared_ptr<PathWait>& w, int code, const std::string& msg)
    {
        if (w->done) return;
        w->done = true;
        idmcp_tool_err(w->call, code, msg);
    }

    // Reconstruct the cell path, string-pull-smooth it, then switch to walking.
    void path_finish_plan(const std::shared_ptr<PathWait>& w)
    {
        std::vector<int> cells;
        for (int c = w->goalIdx; c != -1; c = w->parent[c]) cells.push_back(c);
        std::reverse(cells.begin(), cells.end());

        std::vector<LLVector3d> pts;
        pts.reserve(cells.size());
        for (int c : cells) pts.push_back(p_pt(*w, c, w->cellZ[c]));
        if (!pts.empty()) pts.back() = w->goal;   // end exactly at the goal

        std::vector<LLVector3d> smooth;
        if (!pts.empty())
        {
            smooth.push_back(pts.front());
            size_t i = 0;
            while (i + 1 < pts.size())
            {
                size_t j = pts.size() - 1;
                for (; j > i + 1; --j)
                    if (p_los(pts[i], pts[j])) break;
                smooth.push_back(pts[j]);
                i = j;
            }
        }
        w->waypoints   = std::move(smooth);
        w->wp          = 0;
        w->leg_started = false;
        w->phase       = PathWait::WALK;
        w->deadline    = LLTimer::getTotalSeconds() + PATH_WALK_SECS;
        LL_INFOS("IDMCP") << "pathfind: " << w->waypoints.size() << " waypoints, "
                          << w->expanded << " cells expanded" << LL_ENDL;
    }

    // Incremental A*; consumes up to `budget` rays this tick.
    void path_plan_step(const std::shared_ptr<PathWait>& w, int budget)
    {
        while (budget > 0 && !w->open.empty())
        {
            const int cur = w->open.top().second;
            w->open.pop();
            if (w->closed[cur]) continue;
            w->closed[cur] = 1;

            if (cur == w->goalIdx) { path_finish_plan(w); return; }
            if (++w->expanded > PATH_MAX_CELLS)
            {
                path_err(w, IDMCP_ERR_NOT_FOUND, "no route found (search space exhausted)");
                return;
            }

            const F64 fromZ = w->cellZ[cur];
            const int ci = cur % w->cols, cj = cur / w->cols;
            for (int dj = -1; dj <= 1; ++dj)
            for (int di = -1; di <= 1; ++di)
            {
                if (di == 0 && dj == 0) continue;
                const int ni = ci + di, nj = cj + dj;
                if (ni < 0 || ni >= w->cols || nj < 0 || nj >= w->rows) continue;
                const int nb = nj * w->cols + ni;
                if (w->closed[nb]) continue;

                F64 nbZ;
                if (!p_step(*w, cur, fromZ, nb, nbZ, budget)) continue;
                const bool diag = (di != 0 && dj != 0);
                if (diag)   // no corner-cut: both orthogonal steps must be walkable too
                {
                    F64 tz;
                    if (!p_step(*w, cur, fromZ, cj * w->cols + (ci + di), tz, budget)) continue;
                    if (!p_step(*w, cur, fromZ, (cj + dj) * w->cols + ci, tz, budget)) continue;
                }
                const float step = (diag ? 1.41421356f : 1.0f) * w->cell;
                const float ng   = w->gscore[cur] + step;
                if (ng < w->gscore[nb])
                {
                    w->gscore[nb] = ng;
                    w->parent[nb] = cur;
                    w->cellZ[nb]  = nbZ;
                    w->open.push({ ng + p_heur(*w, nb), nb });
                }
            }
        }
        if (w->open.empty() && !w->done)
            path_err(w, IDMCP_ERR_NOT_FOUND, "no walkable route to that spot");
    }

    // Walk the smoothed waypoints, one autopilot leg at a time (on foot).
    void path_walk_step(const std::shared_ptr<PathWait>& w)
    {
        if (w->waypoints.empty()) { path_report(w, "arrived"); return; }
        if (!w->leg_started)
        {
            gAgent.startAutoPilotGlobal(w->waypoints[w->wp], "idmcp pathTo",
                                        nullptr, nullptr, nullptr, PATH_LEG_STOP,
                                        0.03f, /*allow_flying*/ false);
            w->leg_started = true;
            return;
        }
        if (gAgent.getAutoPilot()) return;   // still walking this leg

        const F64 leg_dist = (gAgent.getPositionGlobal() - w->waypoints[w->wp]).length();
        if (leg_dist > 3.0) { path_report(w, "stopped"); return; }   // gave up short -> blocked
        if (++w->wp >= w->waypoints.size()) { path_report(w, "arrived"); return; }
        w->leg_started = false;   // next leg next tick
    }

    void path_tick()
    {
        if (g_path_waits.empty()) return;
        const F64 now = LLTimer::getTotalSeconds();
        for (auto it = g_path_waits.begin(); it != g_path_waits.end(); )
        {
            auto& w = *it;
            if (!w->done)
            {
                if (now >= w->deadline)
                {
                    if (w->phase == PathWait::WALK && gAgent.getAutoPilot()) gAgent.stopAutoPilot(true);
                    path_report(w, "timeout");
                }
                else if (w->phase == PathWait::PLAN) path_plan_step(w, PATH_RAY_BUDGET);
                else                                 path_walk_step(w);
            }
            if (w->done) it = g_path_waits.erase(it);
            else         ++it;
        }
    }

    // Diagnostic: run the pathfinder's rays at the start position and report what
    // they hit, WITHOUT planning/moving. Reveals whether floor detection works here.
    void path_probe(const IDMCPCallPtr& call)
    {
        const LLVector3d start = gAgent.getPositionGlobal();
        boost::json::object d;
        boost::json::array sg;
        sg.push_back(start.mdV[0]); sg.push_back(start.mdV[1]); sg.push_back(start.mdV[2]);
        d["start_global"] = std::move(sg);

        // Floor down-ray under the avatar (start.Z+1 -> start.Z-3).
        LLVector3d top = start; top.mdV[2] = start.mdV[2] + 1.0;
        LLVector3d bot = start; bot.mdV[2] = start.mdV[2] - 3.0;
        RayHit dh = path_ray(top, bot);
        d["floor_ray_hit"] = dh.blocked;
        if (dh.blocked) d["floor_ray_z"] = dh.point.mdV[2];
        d["land_height"]   = (double)LLWorld::getInstance()->resolveLandHeightGlobal(start);

        const F64 fz = dh.blocked ? dh.point.mdV[2] : start.mdV[2];
        // A neighbour cell's floor 3m north.
        LLVector3d nt(start.mdV[0], start.mdV[1] + 3.0, fz + 1.0);
        LLVector3d nbo(start.mdV[0], start.mdV[1] + 3.0, fz - 1.0);
        RayHit nh = path_ray(nt, nbo);
        d["cell_3m_floor_hit"] = nh.blocked;
        if (nh.blocked) d["cell_3m_floor_z"] = nh.point.mdV[2];

        // Horizontal chest-height ray 4m north (edge-clearance style).
        LLVector3d ha(start.mdV[0], start.mdV[1], fz + 0.95);
        LLVector3d hb(start.mdV[0], start.mdV[1] + 4.0, fz + 0.95);
        d["horiz_4m_north_blocked"] = path_ray(ha, hb).blocked;

        boost::json::object o;
        o["pathfind_probe"] = true;
        o["diag"] = std::move(d);
        idmcp_tool_ok(call, o);
    }

    // Begin a pathfind to `goal`. Reports the error itself if out of range.
    void start_path(const IDMCPCallPtr& call, const LLVector3d& goal)
    {
        const LLVector3d start = gAgent.getPositionGlobal();
        const F64 minx = std::min(start.mdV[0], goal.mdV[0]) - PATH_PAD;
        const F64 maxx = std::max(start.mdV[0], goal.mdV[0]) + PATH_PAD;
        const F64 miny = std::min(start.mdV[1], goal.mdV[1]) - PATH_PAD;
        const F64 maxy = std::max(start.mdV[1], goal.mdV[1]) + PATH_PAD;
        if ((maxx - minx) > PATH_MAX_DIM || (maxy - miny) > PATH_MAX_DIM)
        {
            idmcp_tool_err(call, IDMCP_ERR_INVALID_PARAMS,
                           "target too far for pathfinding; use movement.teleport or move closer");
            return;
        }

        auto w = std::make_shared<PathWait>();
        w->call   = call;
        w->goal   = goal;
        w->minX   = minx;
        w->minY   = miny;
        w->cell   = PATH_CELL;
        w->cols   = (int)std::ceil((maxx - minx) / w->cell);
        w->rows   = (int)std::ceil((maxy - miny) / w->cell);
        const int n = w->cols * w->rows;
        w->cellZ.assign(n, PATH_Z_UNKNOWN);
        w->gscore.assign(n, 1e30f);
        w->parent.assign(n, -1);
        w->closed.assign(n, 0);

        auto cell_of = [&](const LLVector3d& p) -> int {
            int i = (int)((p.mdV[0] - minx) / w->cell);
            int j = (int)((p.mdV[1] - miny) / w->cell);
            i = std::max(0, std::min(w->cols - 1, i));
            j = std::max(0, std::min(w->rows - 1, j));
            return j * w->cols + i;
        };
        w->startIdx = cell_of(start);
        w->goalIdx  = cell_of(goal);
        // The avatar ROOT (getPositionGlobal) sits ~1m above its feet; seeding the
        // start cell with it makes the first step a ~1m drop past the step cap and
        // kills A* immediately (every target -> "no route"). Resolve the actual
        // floor the avatar stands on.
        F64 startFloorZ = start.mdV[2];
        {
            LLVector3d top = start; top.mdV[2] = start.mdV[2] + 1.0;
            LLVector3d bot = start; bot.mdV[2] = start.mdV[2] - 3.0;
            RayHit h = path_ray(top, bot);
            if (h.blocked) startFloorZ = h.point.mdV[2];
            else
            {
                const F32 land = (F32)LLWorld::getInstance()->resolveLandHeightGlobal(start);
                if ((F64)land < start.mdV[2] + 1.0) startFloorZ = (F64)land;
            }
        }
        w->cellZ[w->startIdx]  = startFloorZ;
        w->gscore[w->startIdx] = 0.f;
        w->open.push({ p_heur(*w, w->startIdx), w->startIdx });
        w->deadline = LLTimer::getTotalSeconds() + PATH_PLAN_SECS;
        g_path_waits.push_back(w);
        ensure_tick();
    }

    // ---- RLV gates ---------------------------------------------------------

    IDMCPGateResult gate_teleport(const boost::json::object& args, IDMCPGatePhase)
    {
        if (!IDMCPRlvGate::isEnabled()) return IDMCPGateResult();
        switch (tp_form(args))
        {
        case TP_LANDMARK:
            if (gRlvHandler.hasBehaviour(RLV_BHVR_TPLM))
                return IDMCPRlvGate::deny(RLV_BHVR_TPLM, "tplm");
            return IDMCPGateResult();
        case TP_LOCATION:
        {
            LLVector3d pos;
            const bool have = tp_location(args, pos);
            const bool blocked = have
                ? (RlvActions::isLocalTp(pos) ? !RlvActions::canTeleportToLocal(pos)
                                              : !RlvActions::canTeleportToLocation())
                : !RlvActions::canTeleportToLocation();
            if (blocked) return IDMCPRlvGate::deny(RLV_BHVR_TPLOC, "tploc");
            return IDMCPGateResult();
        }
        case TP_SLURL:
            // A SLURL resolves to a (usually remote) region location; gate on @tploc.
            if (!RlvActions::canTeleportToLocation())
                return IDMCPRlvGate::deny(RLV_BHVR_TPLOC, "tploc");
            return IDMCPGateResult();
        default:
            return IDMCPGateResult();   // invalid form; invoke will report it
        }
    }

    IDMCPGateResult gate_sit(const boost::json::object& args, IDMCPGatePhase)
    {
        if (!IDMCPRlvGate::isEnabled()) return IDMCPGateResult();
        const std::string spec = arg_str(args, "object_id");
        if (looks_like_uuid(spec))
        {
            LLViewerObject* vo = gObjectList.findObject(LLUUID(spec));
            if (vo && !RlvActions::canSit(vo)) return deny("sit");
        }
        return IDMCPGateResult();
    }

    IDMCPGateResult gate_stand(const boost::json::object&, IDMCPGatePhase)
    {
        if (IDMCPRlvGate::isEnabled() && !RlvActions::canStand()) return deny("unsit");
        return IDMCPGateResult();
    }

    IDMCPGateResult gate_touch(const boost::json::object& args, IDMCPGatePhase)
    {
        if (!IDMCPRlvGate::isEnabled()) return IDMCPGateResult();
        const std::string spec = arg_str(args, "id");
        if (looks_like_uuid(spec))
        {
            LLViewerObject* vo = gObjectList.findObject(LLUUID(spec));
            if (vo && !RlvActions::canTouch(vo)) return deny("touch");
        }
        return IDMCPGateResult();
    }
}

// ---------------------------------------------------------------------------

void idmcp_register_movement_tools(IDMCPToolRegistry& reg)
{
    // movement.teleport ------------------------------------------------------
    {
        IDMCPTool t;
        t.name = "movement.teleport";
        t.description =
            "Teleport your avatar. Provide EXACTLY ONE destination: "
            "{\"landmark_item_id\"} (a landmark inventory item UUID), "
            "{\"global_position\":[x,y,z]} (grid-global metres - get one from "
            "search.places or avatars.getNearby), {\"avatar_id\"} (a nearby avatar "
            "to teleport to), or {\"slurl\"} (a Second Life location URL - "
            "maps.secondlife.com/secondlife/Region/x/y/z, secondlife://Region/x/y/z, "
            "or an app/teleport SLURL; its region name is resolved automatically). "
            "Teleport is slow; this waits up to 60s and returns "
            "{status:\"arrived\"|\"failed\"|\"timeout\", region, global_position}. "
            "Blocked by RLV @tplm (landmark) or @tploc/@tplocal (location/slurl).";
        t.input_schema = boost::json::parse(
            R"({"type":"object","properties":{"landmark_item_id":{"type":"string"},"global_position":{"type":"array","items":{"type":"number"},"minItems":3,"maxItems":3},"avatar_id":{"type":"string"},"slurl":{"type":"string"}},"additionalProperties":false})");
        t.gate = gate_teleport;
        t.invoke = [](const boost::json::object& args, const IDMCPCallPtr& call)
        {
            if (movement_busy())
            {
                idmcp_tool_err(call, IDMCP_ERR_CAP_UNAVAIL,
                               "a movement is already in progress; wait for it to finish before the next move");
                return;
            }
            switch (tp_form(args))
            {
            case TP_LANDMARK:
            {
                const std::string spec = arg_str(args, "landmark_item_id");
                if (!looks_like_uuid(spec))
                {
                    idmcp_tool_err(call, IDMCP_ERR_INVALID_PARAMS, "landmark_item_id must be a UUID");
                    return;
                }
                LLViewerInventoryItem* item = gInventory.getItem(LLUUID(spec));
                if (!item)
                {
                    idmcp_tool_err(call, IDMCP_ERR_NOT_FOUND, "landmark item not found in inventory");
                    return;
                }
                if (item->getType() != LLAssetType::AT_LANDMARK)
                {
                    idmcp_tool_err(call, IDMCP_ERR_INVALID_PARAMS, "that inventory item is not a landmark");
                    return;
                }
                gAgent.teleportViaLandmark(item->getAssetUUID());
                break;
            }
            case TP_LOCATION:
            {
                LLVector3d pos;
                if (!tp_location(args, pos))
                {
                    idmcp_tool_err(call, IDMCP_ERR_NOT_FOUND,
                                   "could not resolve destination (avatar not in range?)");
                    return;
                }
                gAgent.teleportViaLocation(pos);
                break;
            }
            case TP_SLURL:
            {
                // A SLURL names a region + local x/y/z. Resolve the region name to a
                // handle asynchronously, then teleport there and arm the arrival wait
                // in the callback. (url_callback_t is a std::function - capturing OK.)
                LLSLURL slurl(arg_str(args, "slurl"));
                if (slurl.getType() != LLSLURL::LOCATION)
                {
                    idmcp_tool_err(call, IDMCP_ERR_INVALID_PARAMS,
                                   "not a location SLURL (expected maps.secondlife.com/secondlife/Region/x/y/z "
                                   "or secondlife://Region/x/y/z)");
                    return;
                }
                const LLVector3 local_pos = slurl.getPosition();
                g_slurl_resolving = true;
                LLWorldMapMessage::getInstance()->sendNamedRegionRequest(
                    slurl.getRegion(),
                    [call, local_pos](U64 region_handle, const std::string&, const LLUUID&, bool)
                    {
                        g_slurl_resolving = false;
                        if (region_handle == 0)
                        {
                            idmcp_tool_err(call, IDMCP_ERR_NOT_FOUND,
                                           "could not resolve the SLURL's region (not found)");
                            return;
                        }
                        const LLVector3d global_pos = from_region_handle(region_handle) + LLVector3d(local_pos);
                        gAgent.teleportViaLocation(global_pos);
                        if (gAgent.getTeleportState() == LLAgent::TELEPORT_NONE)
                        {
                            idmcp_tool_err(call, IDMCP_ERR_PERMISSION,
                                           "teleport did not start (blocked by RLV, or not fully logged in)");
                            return;
                        }
                        start_tp_wait(call);
                    },
                    slurl.getSLURLString(),
                    /*teleport*/ true);
                return;   // deferred: the callback teleports and arms the arrival wait
            }
            default:
                idmcp_tool_err(call, IDMCP_ERR_INVALID_PARAMS,
                               "provide exactly one of landmark_item_id, global_position, avatar_id, slurl");
                return;
            }

            // Blocked/no-op paths (RLV, pre-login) return without starting a
            // request, leaving the state at TELEPORT_NONE - detect that and error
            // now rather than hanging to the 60s deadline.
            if (gAgent.getTeleportState() == LLAgent::TELEPORT_NONE)
            {
                idmcp_tool_err(call, IDMCP_ERR_PERMISSION,
                               "teleport did not start (blocked by RLV, or not fully logged in)");
                return;
            }
            start_tp_wait(call);
        };
        reg.add(std::move(t));
    }

    // movement.sit -----------------------------------------------------------
    {
        IDMCPTool t;
        t.name = "movement.sit";
        t.description =
            "Sit your avatar on an in-world object ({\"object_id\"} UUID; the "
            "object must be loaded in range). Waits up to 5s and returns "
            "{sitting, object_id}. Blocked by RLV @sit.";
        t.input_schema = boost::json::parse(
            R"({"type":"object","properties":{"object_id":{"type":"string"}},"required":["object_id"],"additionalProperties":false})");
        t.gate = gate_sit;
        t.invoke = [](const boost::json::object& args, const IDMCPCallPtr& call)
        {
            if (movement_busy())
            {
                idmcp_tool_err(call, IDMCP_ERR_CAP_UNAVAIL,
                               "a movement is already in progress; wait for it to finish before the next move");
                return;
            }
            const std::string spec = arg_str(args, "object_id");
            if (!looks_like_uuid(spec))
            {
                idmcp_tool_err(call, IDMCP_ERR_INVALID_PARAMS, "object_id must be a UUID");
                return;
            }
            LLUUID id(spec);
            LLViewerObject* obj = gObjectList.findObject(id);
            if (!obj || obj->getPCode() != LL_PCODE_VOLUME)
            {
                idmcp_tool_err(call, IDMCP_ERR_NOT_FOUND, "object not found / not in range");
                return;
            }

            gMessageSystem->newMessageFast(_PREHASH_AgentRequestSit);
            gMessageSystem->nextBlockFast(_PREHASH_AgentData);
            gMessageSystem->addUUIDFast(_PREHASH_AgentID, gAgent.getID());
            gMessageSystem->addUUIDFast(_PREHASH_SessionID, gAgent.getSessionID());
            gMessageSystem->nextBlockFast(_PREHASH_TargetObject);
            gMessageSystem->addUUIDFast(_PREHASH_TargetID, obj->mID);
            gMessageSystem->addVector3Fast(_PREHASH_Offset, LLVector3(0, 0, 0));
            obj->getRegion()->sendReliableMessage();

            start_poll_wait(call, /*want_sitting*/ true, id);
        };
        reg.add(std::move(t));
    }

    // movement.stand ---------------------------------------------------------
    {
        IDMCPTool t;
        t.name = "movement.stand";
        t.description =
            "Stand your avatar up (if sitting). Waits up to 5s and returns "
            "{sitting}. Blocked by RLV @unsit.";
        t.input_schema = boost::json::parse(
            R"({"type":"object","properties":{},"additionalProperties":false})");
        t.gate = gate_stand;
        t.invoke = [](const boost::json::object&, const IDMCPCallPtr& call)
        {
            if (movement_busy())
            {
                idmcp_tool_err(call, IDMCP_ERR_CAP_UNAVAIL,
                               "a movement is already in progress; wait for it to finish before the next move");
                return;
            }
            gAgent.standUp();
            start_poll_wait(call, /*want_sitting*/ false, LLUUID::null);
        };
        reg.add(std::move(t));
    }

    // movement.walkTo --------------------------------------------------------
    {
        IDMCPTool t;
        t.name = "movement.walkTo";
        t.description =
            "Walk your avatar to a spot on foot (the viewer's autopilot / 'move to "
            "here'). Provide EXACTLY ONE target: {\"global_position\":[x,y,z]}, "
            "{\"object_id\"} (walk to an in-world object), or {\"avatar_id\"} (walk "
            "to a nearby avatar). Best for short in-region moves; waits up to 60s and "
            "returns {status:\"arrived\"|\"stopped\", distance}. Always on foot - it "
            "will NOT fly, even for far or elevated targets (pass {\"fly\":true} to "
            "allow flying). Set {\"pathfind\":true} to ROUTE AROUND obstacles and "
            "through doorways (a raycast-grid A* that finds a walkable path on the "
            "current floor, then walks it) instead of the straight line - use it for "
            "cluttered/multi-room spaces; it takes ~1s to plan and returns "
            "{status, distance, waypoints}. For longer hops use movement.teleport. "
            "Optional {\"stop_distance\"} metres (default 1.5).";
        t.input_schema = boost::json::parse(
            R"({"type":"object","properties":{"global_position":{"type":"array","items":{"type":"number"},"minItems":3,"maxItems":3},"object_id":{"type":"string"},"avatar_id":{"type":"string"},"stop_distance":{"type":"number"},"fly":{"type":"boolean"},"pathfind":{"type":"boolean"},"probe":{"type":"boolean"}},"additionalProperties":false})");
        t.invoke = [](const boost::json::object& args, const IDMCPCallPtr& call)
        {
            if (movement_busy())
            {
                idmcp_tool_err(call, IDMCP_ERR_CAP_UNAVAIL,
                               "a movement is already in progress; wait for it to finish before the next move");
                return;
            }
            const int n = (int)arg_has(args, "global_position")
                        + (int)arg_has(args, "object_id")
                        + (int)arg_has(args, "avatar_id");
            if (n != 1)
            {
                idmcp_tool_err(call, IDMCP_ERR_INVALID_PARAMS,
                               "provide exactly one of global_position, object_id, avatar_id");
                return;
            }
            LLVector3d target;
            if (!walk_target(args, target))
            {
                idmcp_tool_err(call, IDMCP_ERR_NOT_FOUND,
                               "could not resolve destination (object/avatar not in range?)");
                return;
            }

            F32 stop_distance = 1.5f;
            auto sit = args.find("stop_distance");
            if (sit != args.end())
            {
                if      (sit->value().is_double()) stop_distance = (F32)sit->value().as_double();
                else if (sit->value().is_int64())  stop_distance = (F32)sit->value().as_int64();
            }

            // Pathfinding: route around obstacles / through doorways (raycast grid +
            // A*, on foot) instead of the straight-line autopilot. Deferred.
            if (auto it = args.find("pathfind");
                it != args.end() && it->value().is_bool() && it->value().as_bool())
            {
                auto pit = args.find("probe");
                if (pit != args.end() && pit->value().is_bool() && pit->value().as_bool())
                {
                    path_probe(call);   // diagnostic: report the start-position rays, no move
                    return;
                }
                start_path(call, target);
                return;
            }

            // Default to NO flying regardless of distance/height (autopilot would
            // otherwise force flight past ~30m or over a height delta). Opt in with {fly}.
            bool allow_fly = false;
            if (auto it = args.find("fly"); it != args.end() && it->value().is_bool())
            {
                allow_fly = it->value().as_bool();
            }

            gAgent.startAutoPilotGlobal(target, "idmcp walkTo", nullptr, nullptr, nullptr,
                                        stop_distance, /*rot_threshold*/ 0.03f, /*allow_flying*/ allow_fly);
            start_walk_wait(call, target);
        };
        reg.add(std::move(t));
    }

    // movement.turn ----------------------------------------------------------
    {
        IDMCPTool t;
        t.name = "movement.turn";
        t.description =
            "Turn your avatar in place, without walking. Provide EXACTLY ONE: "
            "{\"degrees\"} a relative turn (positive = left / counter-clockwise, "
            "negative = right), or something to face - {\"global_position\":[x,y,z]}, "
            "{\"object_id\"}, or {\"avatar_id\"}. Returns {turned, facing:[x,y,z], "
            "heading_deg} (heading 0 = East, 90 = North).";
        t.input_schema = boost::json::parse(
            R"({"type":"object","properties":{"degrees":{"type":"number"},"global_position":{"type":"array","items":{"type":"number"},"minItems":3,"maxItems":3},"object_id":{"type":"string"},"avatar_id":{"type":"string"}},"additionalProperties":false})");
        t.invoke = [](const boost::json::object& args, const IDMCPCallPtr& call)
        {
            const int n = (int)arg_has(args, "degrees")
                        + (int)arg_has(args, "global_position")
                        + (int)arg_has(args, "object_id")
                        + (int)arg_has(args, "avatar_id");
            if (n != 1)
            {
                idmcp_tool_err(call, IDMCP_ERR_INVALID_PARAMS,
                               "provide exactly one of degrees, global_position, object_id, avatar_id");
                return;
            }

            if (arg_has(args, "degrees"))
            {
                double deg = 0.0;
                auto it = args.find("degrees");
                if      (it->value().is_double()) deg = it->value().as_double();
                else if (it->value().is_int64())  deg = (double)it->value().as_int64();
                gAgent.yaw((F32)(deg * DEG_TO_RAD));
            }
            else
            {
                LLVector3d tgt;
                if (!walk_target(args, tgt))
                {
                    idmcp_tool_err(call, IDMCP_ERR_NOT_FOUND,
                                   "could not resolve what to face (object/avatar not in range?)");
                    return;
                }
                const LLVector3d d3 = tgt - gAgent.getPositionGlobal();
                LLVector3 dir((F32)d3.mdV[0], (F32)d3.mdV[1], 0.f);   // keep upright
                if (dir.lengthSquared() < 1e-4f)
                {
                    idmcp_tool_err(call, IDMCP_ERR_INVALID_PARAMS, "target coincides with your position");
                    return;
                }
                dir.normalize();
                gAgent.resetAxes(dir);
            }

            const LLVector3 at = gAgent.getAtAxis();
            boost::json::object o;
            o["turned"] = true;
            boost::json::array fa;
            fa.push_back((double)at.mV[0]); fa.push_back((double)at.mV[1]); fa.push_back((double)at.mV[2]);
            o["facing"]      = std::move(fa);
            o["heading_deg"] = (double)(std::atan2((double)at.mV[1], (double)at.mV[0]) * RAD_TO_DEG);
            idmcp_tool_ok(call, o);
        };
        reg.add(std::move(t));
    }

    // agent.getLocation ------------------------------------------------------
    {
        IDMCPTool t;
        t.name = "agent.getLocation";
        t.description =
            "Where your avatar is right now: region name, global + region-local "
            "position, and current parcel (name, owner, area, and restriction "
            "flags). No arguments. Hidden by RLV @showloc.";
        t.input_schema = boost::json::parse(
            R"({"type":"object","properties":{},"additionalProperties":false})");
        t.invoke = [](const boost::json::object&, const IDMCPCallPtr& call)
        {
            boost::json::object out;

            // RLV @showloc hides the agent's location everywhere in the human UI
            // (mini-map, About Land, position readouts). Mirror that: report only
            // that the location is hidden, none of the identifying fields.
            if (IDMCPRlvGate::isEnabled() && !RlvActions::canShowLocation())
            {
                out["location_hidden"] = true;
                idmcp_tool_ok(call, out);
                return;
            }

            boost::json::object region;
            if (LLViewerRegion* r = gAgent.getRegion())
            {
                region["name"] = r->getName();
            }
            out["region"] = std::move(region);

            const LLVector3d g = gAgent.getPositionGlobal();
            const LLVector3  l = gAgent.getPositionAgent();
            boost::json::object pos;
            boost::json::array ga; ga.push_back(g.mdV[0]); ga.push_back(g.mdV[1]); ga.push_back(g.mdV[2]);
            boost::json::array la; la.push_back((double)l.mV[0]); la.push_back((double)l.mV[1]); la.push_back((double)l.mV[2]);
            pos["global"] = std::move(ga);
            pos["region"] = std::move(la);
            out["position"] = std::move(pos);

            if (LLParcel* p = LLViewerParcelMgr::getInstance()->getAgentParcel())
            {
                boost::json::object parcel;
                parcel["name"]      = p->getName();
                parcel["owner_id"]  = p->getOwnerID().asString();
                parcel["area"]      = p->getArea();
                parcel["raw_flags"] = (int64_t)p->getParcelFlags();
                boost::json::object flags;
                flags["damage"]          = p->getAllowDamage();
                flags["fly"]             = p->getAllowFly();
                flags["scripts"]         = p->getAllowOtherScripts();
                flags["build"]           = p->getAllowModify();
                flags["terraform"]       = p->getAllowTerraform();
                flags["push_restricted"] = p->getRestrictPushObject();
                flags["for_sale"]        = p->getForSale();
                parcel["flags"] = std::move(flags);
                out["parcel"] = std::move(parcel);
            }

            idmcp_tool_ok(call, out);
        };
        reg.add(std::move(t));
    }

    // object.touch -----------------------------------------------------------
    {
        IDMCPTool t;
        t.name = "object.touch";
        t.description =
            "Touch/click an in-world object ({\"id\"} UUID, optional {\"face\"} "
            "integer, default 0) - this operates vendors, doors, and RLV "
            "furniture. The object must be loaded in range. Fire-and-forget: any "
            "resulting blue-menu (llDialog) arrives asynchronously as a "
            "notification - read/answer it with notifications.list / "
            "notifications.respond, not this tool's result. Blocked by RLV "
            "@touchall / @touchworld / @touchthis / @interact.";
        t.input_schema = boost::json::parse(
            R"({"type":"object","properties":{"id":{"type":"string"},"face":{"type":"integer"}},"required":["id"],"additionalProperties":false})");
        t.gate = gate_touch;
        t.invoke = [](const boost::json::object& args, const IDMCPCallPtr& call)
        {
            const std::string spec = arg_str(args, "id");
            if (!looks_like_uuid(spec))
            {
                idmcp_tool_err(call, IDMCP_ERR_INVALID_PARAMS, "id must be a UUID");
                return;
            }
            LLViewerObject* obj = gObjectList.findObject(LLUUID(spec));
            if (!obj || obj->getPCode() != LL_PCODE_VOLUME)
            {
                idmcp_tool_err(call, IDMCP_ERR_NOT_FOUND, "object not found / not in range");
                return;
            }

            LLPickInfo pick;
            pick.mObjectFace = arg_int(args, "face", 0);
            // A touch is grab-then-immediately-degrab (per LLAgentListener::requestTouch).
            send_ObjectGrab_message(obj, pick, LLVector3::zero);
            send_ObjectDeGrab_message(obj, pick);

            boost::json::object o;
            o["touched"] = true;
            o["id"]      = spec;
            idmcp_tool_ok(call, o);
        };
        reg.add(std::move(t));
    }

    // hud.click --------------------------------------------------------------
    {
        IDMCPTool t;
        t.name = "hud.click";
        t.description =
            "Click a button on one of YOUR worn HUDs by screen coordinate. "
            "{\"x\"},{\"y\"} are normalized 0..1 from the TOP-LEFT, exactly as you'd "
            "read a screenshot - take one with vision.snapshot {show_hud:true} to see "
            "where the HUD buttons are, then aim here. Only registers a hit on a HUD "
            "(not the world - use object.touch for in-world objects). Returns "
            "{clicked, object_id, face}. Blocked by RLV @touchhud.";
        t.input_schema = boost::json::parse(
            R"({"type":"object","properties":{"x":{"type":"number"},"y":{"type":"number"},"probe":{"type":"boolean"}},"required":["x","y"],"additionalProperties":false})");
        t.invoke = [](const boost::json::object& args, const IDMCPCallPtr& call)
        {
            auto num = [&](const char* k, double dflt) -> double {
                auto it = args.find(k);
                if (it == args.end()) return dflt;
                if (it->value().is_double()) return it->value().as_double();
                if (it->value().is_int64())  return (double)it->value().as_int64();
                return dflt;
            };
            auto bl = [&](const char* k) -> bool {
                auto it = args.find(k);
                return it != args.end() && it->value().is_bool() && it->value().as_bool();
            };
            double nx = num("x", -1.0), ny = num("y", -1.0);
            if (nx < 0.0 || nx > 1.0 || ny < 0.0 || ny > 1.0)
            {
                idmcp_tool_err(call, IDMCP_ERR_INVALID_PARAMS, "x and y must be 0..1 (from top-left)");
                return;
            }

            // Map the normalized coordinate through the SCALED world-view rect.
            // vision.snapshot (hide_ui) captures the world-view rect, so a normalized
            // snapshot coord is a normalized world-view coord. pickImmediate works in
            // SCALED/GL pixels (the mouse-coordinate space), NOT raw device pixels -
            // on a HiDPI display (raw = scale x scaled) using the raw rect overshoots
            // every click by the display scale. Use getWorldViewRectScaled().
            const LLRect wr  = gViewerWindow->getWorldViewRectRaw();      // reported for reference
            const LLRect wrs = gViewerWindow->getWorldViewRectScaled();   // pick space
            const S32 gx = wrs.mLeft   + (S32)(nx * wrs.getWidth());
            const S32 gy = wrs.mBottom + (S32)((1.0 - ny) * wrs.getHeight());   // y-from-bottom

            LLPickInfo pick = gViewerWindow->pickImmediate(gx, gy, /*pick_transparent*/ true, /*pick_rigged*/ true);
            LLViewerObject* obj = pick.getObject();

            // probe:true reports the mapping geometry + what the pick hit, WITHOUT
            // clicking — for calibrating aim. Normal clicks return a clean result.
            if (bl("probe"))
            {
                auto rect_arr = [](const LLRect& r) {
                    boost::json::array a;
                    a.push_back(r.mLeft); a.push_back(r.mBottom); a.push_back(r.getWidth()); a.push_back(r.getHeight());
                    return a;
                };
                boost::json::array win;  win.push_back(gViewerWindow->getWindowWidthRaw());    win.push_back(gViewerWindow->getWindowHeightRaw());
                boost::json::array wins; wins.push_back(gViewerWindow->getWindowWidthScaled()); wins.push_back(gViewerWindow->getWindowHeightScaled());
                boost::json::array px;   px.push_back(gx); px.push_back(gy);
                boost::json::object dbg;
                dbg["window_raw"]        = std::move(win);
                dbg["window_scaled"]     = std::move(wins);
                dbg["world_view_raw"]    = rect_arr(wr);
                dbg["world_view_scaled"] = rect_arr(wrs);
                dbg["picked_px"]         = std::move(px);
                dbg["hit_is_hud"]        = (obj && obj->isHUDAttachment());
                dbg["hit_object"]        = obj ? obj->getID().asString() : std::string();
                boost::json::object o;
                o["probe"] = true;
                o["debug"] = std::move(dbg);
                idmcp_tool_ok(call, o);
                return;
            }

            if (!obj || !obj->isHUDAttachment())
            {
                idmcp_tool_err(call, IDMCP_ERR_NOT_FOUND,
                               "no HUD at that coordinate (screenshot with vision.snapshot show_hud:true to aim)");
                return;
            }
            if (IDMCPRlvGate::isEnabled() && !RlvActions::canTouch(obj))
            {
                idmcp_tool_err(call, IDMCP_ERR_RLV_RESTRICTED, "blocked by RLV @touchhud");
                return;
            }

            send_ObjectGrab_message(obj, pick, LLVector3::zero);
            send_ObjectDeGrab_message(obj, pick);

            boost::json::object o;
            o["clicked"]   = true;
            o["object_id"] = obj->getID().asString();
            o["face"]      = pick.mObjectFace;
            idmcp_tool_ok(call, o);
        };
        reg.add(std::move(t));
    }

    // ao.getStatus -----------------------------------------------------------
    {
        IDMCPTool t;
        t.name = "ao.getStatus";
        t.description =
            "Firestorm Animation Overlay (AO) status: {enabled, stands_enabled, "
            "current_set, sets:[names]}. No arguments.";
        t.input_schema = boost::json::parse(
            R"({"type":"object","properties":{},"additionalProperties":false})");
        t.invoke = [](const boost::json::object&, const IDMCPCallPtr& call)
        {
            boost::json::object o;
            o["enabled"]        = gSavedPerAccountSettings.getBOOL("UseAO");
            o["stands_enabled"] = gSavedPerAccountSettings.getBOOL("UseAOStands");
            o["current_set"]    = AOEngine::instance().getCurrentSetName();
            boost::json::array sets;
            for (const AOSet* s : AOEngine::instance().getSetList())
            {
                if (!s) continue;
                boost::json::value v;
                v = s->getName();
                sets.push_back(std::move(v));
            }
            o["sets"] = std::move(sets);
            idmcp_tool_ok(call, o);
        };
        reg.add(std::move(t));
    }

    // ao.setEnabled ----------------------------------------------------------
    {
        IDMCPTool t;
        t.name = "ao.setEnabled";
        t.description =
            "Turn the Firestorm AO on or off ({\"enabled\"} boolean). Returns "
            "{enabled}.";
        t.input_schema = boost::json::parse(
            R"({"type":"object","properties":{"enabled":{"type":"boolean"}},"required":["enabled"],"additionalProperties":false})");
        t.invoke = [](const boost::json::object& args, const IDMCPCallPtr& call)
        {
            auto it = args.find("enabled");
            if (it == args.end() || !it->value().is_bool())
            {
                idmcp_tool_err(call, IDMCP_ERR_INVALID_PARAMS, "enabled (boolean) is required");
                return;
            }
            const bool enabled = it->value().as_bool();
            // Drive the per-account setting; its commit signal runs AOEngine::enable.
            gSavedPerAccountSettings.setBOOL("UseAO", enabled);
            boost::json::object o;
            o["enabled"] = enabled;
            idmcp_tool_ok(call, o);
        };
        reg.add(std::move(t));
    }

    // ao.selectSet -----------------------------------------------------------
    {
        IDMCPTool t;
        t.name = "ao.selectSet";
        t.description =
            "Switch the active AO set by name ({\"name\"}; see ao.getStatus for the "
            "list). Returns {current_set}.";
        t.input_schema = boost::json::parse(
            R"({"type":"object","properties":{"name":{"type":"string"}},"required":["name"],"additionalProperties":false})");
        t.invoke = [](const boost::json::object& args, const IDMCPCallPtr& call)
        {
            const std::string name = arg_str(args, "name");
            if (name.empty())
            {
                idmcp_tool_err(call, IDMCP_ERR_INVALID_PARAMS, "name is required");
                return;
            }
            if (!AOEngine::instance().selectSetByName(name))
            {
                idmcp_tool_err(call, IDMCP_ERR_NOT_FOUND,
                               "no AO set by that name (see ao.getStatus sets)");
                return;
            }
            boost::json::object o;
            o["current_set"] = name;
            idmcp_tool_ok(call, o);
        };
        reg.add(std::move(t));
    }

    // ao.cycle ---------------------------------------------------------------
    {
        IDMCPTool t;
        t.name = "ao.cycle";
        t.description =
            "Cycle the current AO stand to the next/previous animation "
            "({\"direction\"} \"next\"(default)|\"prev\"). Returns {cycled, direction}.";
        t.input_schema = boost::json::parse(
            R"({"type":"object","properties":{"direction":{"type":"string","enum":["next","prev"]}},"additionalProperties":false})");
        t.invoke = [](const boost::json::object& args, const IDMCPCallPtr& call)
        {
            std::string dir = arg_str(args, "direction");
            if (dir.empty()) dir = "next";
            if (dir != "next" && dir != "prev")
            {
                idmcp_tool_err(call, IDMCP_ERR_INVALID_PARAMS, "direction must be next or prev");
                return;
            }
            AOEngine::instance().cycle(dir == "prev" ? AOEngine::CyclePrevious : AOEngine::CycleNext);
            boost::json::object o;
            o["cycled"]    = true;
            o["direction"] = dir;
            idmcp_tool_ok(call, o);
        };
        reg.add(std::move(t));
    }
}
