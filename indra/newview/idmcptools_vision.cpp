/**
 * @file idmcptools_vision.cpp
 * @brief <ID> MCP server: vision - render the current viewport to an image.
 *
 * Part of Five's custom Firestorm fork. Custom code carries an `ID` prefix.
 *
 * Gives the embodied agent eyes: vision.snapshot renders the 3D view and returns
 * it as a base64 image (or writes a temp file). LLViewerWindow::rawSnapshot
 * drives a full synchronous re-render (it calls display() and glReadPixels), so
 * it MUST run at a frame boundary, never inline in the tool-invoke body. We defer
 * one "mainloop" tick (which fires between frames, outside display()) and capture
 * there - the same LLTempBoundListener pattern the other tools use for deferral.
 * Main-thread only. No RLV gate on the user's own view.
 *
 * camera.set/get/clear give the agent its own camera pose, separate from the
 * user's. vision.snapshot {camera:"agent"} swaps LLViewerCamera to that pose for
 * the one rawSnapshot render and restores it afterwards, as cubeSnapshot does.
 * The pose obeys the RLV camera distance, FOV and @setcam_unlock limits.
 */

#include "llviewerprecompiledheaders.h"

#include "idmcp.h"
#include "idmcptools.h"
#include "idmcpserver.h"

#include "llviewerwindow.h"     // gViewerWindow, rawSnapshot
#include "llviewercamera.h"
#include "llviewerobjectlist.h" // gObjectList
#include "llviewerobject.h"
#include "llagent.h"            // gAgent: global <-> agent frame
#include "llvoavatarself.h"     // gAgentAvatarp, isAgentAvatarValid
#include "llrender.h"           // get/set_current_projection/modelview
#include "pipeline.h"           // LLPipeline::sUseOcclusion
#include "idmcprlvgate.h"
#include "rlvactions.h"
#include "rlvhandler.h"         // gRlvHandler, RLV_BHVR_SETCAM_*
#include "llsnapshotmodel.h"    // LLSnapshotModel::SNAPSHOT_TYPE_COLOR
#include "llimage.h"            // LLImageRaw, LLImageFormatted
#include "llimagepng.h"
#include "llimagejpeg.h"
#include "llbase64.h"
#include "lldir.h"              // gDirUtilp
#include "llapr.h"              // LLAPRFile

#include "llevents.h"           // LLEventPumps, LLTempBoundListener
#include "llpointer.h"

#include <algorithm>
#include <cmath>
#include <memory>
#include <vector>

namespace
{
    S32 arg_int(const boost::json::object& args, const char* key, S32 dflt)
    {
        auto it = args.find(key);
        if (it == args.end()) return dflt;
        if (it->value().is_int64())  return (S32)it->value().as_int64();
        if (it->value().is_double()) return (S32)it->value().as_double();
        return dflt;
    }

    bool arg_bool(const boost::json::object& args, const char* key, bool dflt)
    {
        auto it = args.find(key);
        return (it != args.end() && it->value().is_bool()) ? it->value().as_bool() : dflt;
    }

    std::string arg_str(const boost::json::object& args, const char* key)
    {
        auto it = args.find(key);
        return (it != args.end() && it->value().is_string())
                   ? std::string(it->value().as_string().c_str()) : std::string();
    }

    S32 clampi(S32 v, S32 lo, S32 hi) { return v < lo ? lo : (v > hi ? hi : v); }

    bool looks_like_uuid(const std::string& s)
    {
        return s.size() == 36 && s[8] == '-' && s[13] == '-' && s[18] == '-' && s[23] == '-';
    }

    bool arg_num(const boost::json::object& args, const char* key, double& out)
    {
        auto it = args.find(key);
        if (it == args.end()) return false;
        if (it->value().is_double()) { out = it->value().as_double(); return true; }
        if (it->value().is_int64())  { out = (double)it->value().as_int64(); return true; }
        return false;
    }

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
            else return false;
        }
        out = LLVector3d(v[0], v[1], v[2]);
        return true;
    }

    boost::json::array vec_json(const LLVector3d& v)
    {
        boost::json::array a;
        a.push_back(v.mdV[0]); a.push_back(v.mdV[1]); a.push_back(v.mdV[2]);
        return a;
    }

    // ==================== agent camera =======================================

    struct AgentCam
    {
        bool       set = false;
        LLVector3d eye;
        LLVector3d target;
        F32        fov = 0.f;     // vertical, radians; 0 => the user's current FOV
    };

    AgentCam g_cam;

    LLVector3d head_global()
    {
        return gAgent.getPosGlobalFromAgent(isAgentAvatarValid() ? gAgentAvatarp->mHeadp->getWorldPosition()
                                                                 : gAgent.getPositionAgent());
    }

    F32 effective_fov(const AgentCam& cam)
    {
        LLViewerCamera& vc = LLViewerCamera::instance();
        F32 fov = llclamp(cam.fov > 0.f ? cam.fov : vc.getView(), vc.getMinView(), vc.getMaxView());
        F32 lo, hi;
        if (IDMCPRlvGate::isEnabled() && RlvActions::getCameraFOVLimits(lo, hi))
            fov = llclamp(fov, lo, hi);
        return fov;
    }

    // RLV measures the main camera's avatar distance from the head, and its origin
    // distance from the default camera spot just behind the head. The agent camera
    // has no default spot, so both limits are measured from the head. `limit` is
    // set to the violated distance limit, if any.
    IDMCPGateResult check_rlv_pose(const AgentCam& cam, F32& limit)
    {
        limit = -1.f;
        if (!IDMCPRlvGate::isEnabled()) return IDMCPGateResult();

        if (gRlvHandler.hasBehaviour(RLV_BHVR_SETCAM_UNLOCK)
            && (cam.target - gAgent.getPositionGlobal()).magVec() > 2.0)
            return IDMCPRlvGate::deny(RLV_BHVR_SETCAM_UNLOCK, "setcam_unlock");

        const F32 dist = (F32)(cam.eye - head_global()).magVec();
        F32 lo, hi;
        if (RlvActions::getCameraAvatarDistanceLimits(lo, hi))
        {
            if (dist > hi) { limit = hi; return IDMCPRlvGate::deny(RLV_BHVR_SETCAM_AVDISTMAX, "setcam_avdistmax"); }
            if (dist < lo) { limit = lo; return IDMCPRlvGate::deny(RLV_BHVR_SETCAM_AVDISTMIN, "setcam_avdistmin"); }
        }
        if (RlvActions::getCameraOriginDistanceLimits(lo, hi))
        {
            if (dist > hi) { limit = hi; return IDMCPRlvGate::deny(RLV_BHVR_SETCAM_ORIGINDISTMAX, "setcam_origindistmax"); }
            if (dist < lo) { limit = lo; return IDMCPRlvGate::deny(RLV_BHVR_SETCAM_ORIGINDISTMIN, "setcam_origindistmin"); }
        }
        return IDMCPGateResult();
    }

    void respond_rlv(const IDMCPCallPtr& call, const IDMCPGateResult& g, F32 limit)
    {
        boost::json::object data;
        data["restriction"] = g.behaviour;
        data["sources"]     = g.sources;
        if (limit >= 0.f) data["limit_m"] = limit;
        idmcp_tool_err(call, IDMCP_ERR_RLV_RESTRICTED, "blocked by RLV restriction @" + g.behaviour, std::move(data));
    }

    // Bounding sphere of an object and its linked prims (avatars: ~2m tall).
    void object_bounds(LLViewerObject* vo, LLVector3d& center, F32& radius)
    {
        if (vo->isAvatar())
        {
            center = gAgent.getPosGlobalFromAgent(vo->getRenderPosition());
            radius = 1.f;
            return;
        }
        LLVector3 lo(F32_MAX, F32_MAX, F32_MAX), hi(-F32_MAX, -F32_MAX, -F32_MAX);
        auto add = [&](const LLViewerObject* p)
        {
            const LLVector3 c = p->getRenderPosition();
            const F32       r = p->getScale().magVec() * 0.5f;
            for (int i = 0; i < 3; ++i)
            {
                lo.mV[i] = llmin(lo.mV[i], c.mV[i] - r);
                hi.mV[i] = llmax(hi.mV[i], c.mV[i] + r);
            }
        };
        add(vo);
        for (const auto& child : vo->getChildren()) add(child.get());
        center = gAgent.getPosGlobalFromAgent((lo + hi) * 0.5f);
        radius = llmax(0.25f, (hi - lo).magVec() * 0.5f);
    }

    boost::json::object pose_json(const AgentCam& cam)
    {
        boost::json::object o;
        if (!cam.set) { o["set"] = false; return o; }
        const LLVector3d d = cam.eye - cam.target;
        const double dist = d.magVec();
        o["set"]       = true;
        o["eye"]       = vec_json(cam.eye);
        o["target"]    = vec_json(cam.target);
        o["distance"]  = dist;
        o["yaw_deg"]   = std::atan2(d.mdV[1], d.mdV[0]) * RAD_TO_DEG;
        o["pitch_deg"] = dist > 0.0 ? std::asin(llclamp(d.mdV[2] / dist, -1.0, 1.0)) * RAD_TO_DEG : 0.0;
        o["fov_deg"]   = effective_fov(cam) * RAD_TO_DEG;
        return o;
    }

    struct VisionWait
    {
        IDMCPCallPtr call;
        S32          width = 1024;
        S32          height = 0;      // 0 => derive from window aspect
        bool         show_ui = false;
        bool         show_hud = false;
        bool         png = false;     // else jpeg
        S32          quality = 80;
        bool         to_file = false;
        bool         agent_cam = false;
    };

    std::vector<std::shared_ptr<VisionWait>> g_waits;
    bool                                     g_tick_on = false;
    LLTempBoundListener                      g_tick;

    void capture(const std::shared_ptr<VisionWait>& w)
    {
        S32 width  = clampi(w->width, 256, 2048);
        S32 height = w->height;
        if (height <= 0)
        {
            const S32 ww = gViewerWindow->getWindowWidthRaw();
            const S32 wh = gViewerWindow->getWindowHeightRaw();
            height = (ww > 0) ? (S32)((F32)width * (F32)wh / (F32)ww) : width;
        }
        height = clampi(height, 256, 2048);

        LLPointer<LLImageRaw> raw = new LLImageRaw;
        bool ok = false;
        if (w->agent_cam)
        {
            if (!g_cam.set)
            {
                idmcp_tool_err(w->call, IDMCP_ERR_INVALID_PARAMS, "no agent camera set; call camera.set first");
                return;
            }
            F32 limit;
            const IDMCPGateResult g = check_rlv_pose(g_cam, limit);
            if (!g.allowed) { respond_rlv(w->call, g, limit); return; }

            LLViewerCamera* camera = LLViewerCamera::getInstance();
            const LLViewerCamera saved_camera = *camera;
            const glm::mat4 saved_proj = get_current_projection();
            const glm::mat4 saved_mod  = get_current_modelview();
            const S32 old_occlusion = LLPipeline::sUseOcclusion;
            // Occlusion results come from the user's view and would hide things
            // the agent camera can see.
            LLPipeline::sUseOcclusion = 0;

            const LLVector3 eye    = gAgent.getPosAgentFromGlobal(g_cam.eye);
            const LLVector3 target = gAgent.getPosAgentFromGlobal(g_cam.target);
            LLVector3 dir = target - eye;
            dir.normVec();
            camera->setViewNoBroadcast(effective_fov(g_cam));
            camera->setOriginAndLookAt(eye, fabsf(dir.mV[VZ]) > 0.999f ? LLVector3::x_axis : LLVector3::z_axis, target);

            ok = gViewerWindow->rawSnapshot(
                raw, width, height,
                /*keep_window_aspect*/ true, /*is_texture*/ false,
                /*show_ui*/ false, /*show_hud*/ false,
                /*do_rebuild*/ true, /*no_post*/ false, /*show_balance*/ false,
                LLSnapshotModel::SNAPSHOT_TYPE_COLOR);

            *camera = saved_camera;
            set_current_modelview(saved_mod);
            set_current_projection(saved_proj);
            gViewerWindow->setup3DViewport();
            LLPipeline::sUseOcclusion = old_occlusion;
        }
        else
        {
            ok = gViewerWindow->rawSnapshot(
                raw, width, height,
                /*keep_window_aspect*/ true, /*is_texture*/ false,
                /*show_ui*/ w->show_ui, /*show_hud*/ w->show_hud,
                /*do_rebuild*/ false, /*no_post*/ false, /*show_balance*/ false,
                LLSnapshotModel::SNAPSHOT_TYPE_COLOR);
        }
        if (!ok)
        {
            idmcp_tool_err(w->call, IDMCP_ERR_CAP_UNAVAIL, "snapshot capture failed");
            return;
        }

        LLPointer<LLImageFormatted> fmt;
        if (w->png) fmt = new LLImagePNG;
        else        fmt = new LLImageJPEG(w->quality);
        if (!fmt->encode(raw, 0.f))
        {
            idmcp_tool_err(w->call, IDMCP_ERR_CAP_UNAVAIL, "snapshot encode failed");
            return;
        }

        boost::json::object out;
        out["format"] = w->png ? "png" : "jpeg";
        out["width"]  = raw->getWidth();
        out["height"] = raw->getHeight();

        if (w->to_file)
        {
            std::string path = gDirUtilp->getTempFilename();
            path += (w->png ? ".png" : ".jpg");
            const S32 n = (S32)fmt->getDataSize();
            if (LLAPRFile::writeEx(path, (void*)fmt->getData(), 0, n) != n)
            {
                idmcp_tool_err(w->call, IDMCP_ERR_CAP_UNAVAIL, "could not write snapshot temp file");
                return;
            }
            out["path"] = path;
        }
        else
        {
            out["image_base64"] = LLBase64::encode(fmt->getData(), fmt->getDataSize());
        }

        idmcp_tool_ok(w->call, out);
    }

    void vision_tick()
    {
        if (g_waits.empty()) return;
        // Capture pending snapshots (one heavy re-render each) at this frame boundary.
        auto pending = std::move(g_waits);
        g_waits.clear();
        for (auto& w : pending) capture(w);
    }

    void ensure_tick()
    {
        if (g_tick_on) return;
        g_tick = LLEventPumps::instance().obtain("mainloop").listen(
            "idmcp_vision", [](const LLSD&) -> bool { vision_tick(); return false; });
        g_tick_on = true;
    }
}

// ---------------------------------------------------------------------------

void idmcp_register_vision_tools(IDMCPToolRegistry& reg)
{
    IDMCPTool t;
    t.name = "vision.snapshot";
    t.description =
        "Render the current 3D view and return it as an image you can look at - "
        "your avatar's eyes. Optional {\"width\"} (default 1024, 256-2048), "
        "{\"height\"} (default from window aspect), {\"hide_ui\"} (default true, "
        "excludes the on-screen UI), {\"show_hud\"} (default false), {\"format\"} "
        "\"jpeg\"(default)|\"png\", {\"quality\"} (jpeg, default 80), {\"to_file\"} "
        "(default false: return inline base64; true: write a temp file and return "
        "its path), {\"camera\"} \"user\"(default: the user's view)|\"agent\" (your own "
        "camera placed with camera.set; the user's view is untouched, UI and HUDs "
        "are never shown). The agent camera only sees what the viewer has loaded "
        "around the user's camera, and far-off detail can be low-res. Returns "
        "{format, width, height, image_base64} or {..., path}.";
    t.input_schema = boost::json::parse(
        R"({"type":"object","properties":{"width":{"type":"integer"},"height":{"type":"integer"},"hide_ui":{"type":"boolean"},"show_hud":{"type":"boolean"},"format":{"type":"string","enum":["jpeg","png"]},"quality":{"type":"integer"},"to_file":{"type":"boolean"},"camera":{"type":"string","enum":["user","agent"]}},"additionalProperties":false})");
    t.invoke = [](const boost::json::object& args, const IDMCPCallPtr& call)
    {
        auto w = std::make_shared<VisionWait>();
        w->call     = call;
        w->width    = arg_int(args, "width", 1024);
        w->height   = arg_int(args, "height", 0);
        w->show_ui  = !arg_bool(args, "hide_ui", true);
        w->show_hud = arg_bool(args, "show_hud", false);
        w->png      = (arg_str(args, "format") == "png");
        w->quality  = clampi(arg_int(args, "quality", 80), 1, 100);
        w->to_file  = arg_bool(args, "to_file", false);
        w->agent_cam = (arg_str(args, "camera") == "agent");
        // Defer to the next mainloop tick so rawSnapshot runs at a frame boundary.
        g_waits.push_back(w);
        ensure_tick();
    };
    reg.add(std::move(t));

    IDMCPTool set;
    set.name = "camera.set";
    set.description =
        "Place your own camera, separate from the user's, for vision.snapshot "
        "{camera:\"agent\"}. Aim it at exactly one of {\"object_id\"}, "
        "{\"avatar_id\"} or {\"global_position\"}:[x,y,z]. Then either give "
        "{\"eye\"}:[x,y,z] (global) or orbit the target: {\"distance\"} (m, default "
        "frames the target), {\"yaw_deg\"} (the side of the target the camera sits "
        "on, as a heading from it: 0=East, 90=North; default the side facing you, or "
        "in front of you when the target is you), {\"pitch_deg\"} (above the "
        "target, default 15). Optional {\"fov_deg\"} (vertical, default the user's). "
        "RLV camera distance limits are measured from your head; @setcam_unlock "
        "keeps the camera on your avatar. Returns the pose {eye, target, distance, "
        "yaw_deg, pitch_deg, fov_deg}. The pose stays until camera.set or camera.clear.";
    set.input_schema = boost::json::parse(
        R"({"type":"object","properties":{"object_id":{"type":"string"},"avatar_id":{"type":"string"},"global_position":{"type":"array","items":{"type":"number"},"minItems":3,"maxItems":3},"eye":{"type":"array","items":{"type":"number"},"minItems":3,"maxItems":3},"distance":{"type":"number"},"yaw_deg":{"type":"number"},"pitch_deg":{"type":"number"},"fov_deg":{"type":"number"}},"additionalProperties":false})");
    set.invoke = [](const boost::json::object& args, const IDMCPCallPtr& call)
    {
        LLVector3d target;
        F32        radius = 1.f;
        if (!arg_vec3d(args, "global_position", target))
        {
            std::string id = arg_str(args, "object_id");
            if (id.empty()) id = arg_str(args, "avatar_id");
            LLViewerObject* vo = looks_like_uuid(id) ? gObjectList.findObject(LLUUID(id)) : nullptr;
            if (!vo)
            {
                idmcp_tool_err(call, IDMCP_ERR_NOT_FOUND,
                               id.empty() ? "give one of object_id, avatar_id or global_position"
                                          : "target not found nearby");
                return;
            }
            object_bounds(vo, target, radius);
        }

        AgentCam cam;
        cam.set    = true;
        cam.target = target;
        double fov_deg;
        if (arg_num(args, "fov_deg", fov_deg)) cam.fov = (F32)(fov_deg * DEG_TO_RAD);

        if (!arg_vec3d(args, "eye", cam.eye))
        {
            const bool target_is_self = (target - gAgent.getPositionGlobal()).magVec() <= 2.0;
            LLVector3d side = target_is_self ? LLVector3d(gAgent.getAtAxis()) : head_global() - target;
            double yaw   = std::atan2(side.mdV[1], side.mdV[0]);
            double pitch = 15.0 * DEG_TO_RAD;
            double v;
            if (arg_num(args, "yaw_deg", v))   yaw   = v * DEG_TO_RAD;
            if (arg_num(args, "pitch_deg", v)) pitch = llclamp(v, -89.0, 89.0) * DEG_TO_RAD;
            double dist = radius / std::tan(effective_fov(cam) * 0.5) * 1.2;
            if (arg_num(args, "distance", v)) dist = v;
            dist = llclamp(dist, 0.1, 512.0);
            cam.eye = target + LLVector3d(std::cos(pitch) * std::cos(yaw),
                                          std::cos(pitch) * std::sin(yaw),
                                          std::sin(pitch)) * dist;
        }
        if ((cam.eye - cam.target).magVec() < 0.05)
        {
            idmcp_tool_err(call, IDMCP_ERR_INVALID_PARAMS, "eye and target are the same point");
            return;
        }

        F32 limit;
        const IDMCPGateResult g = check_rlv_pose(cam, limit);
        if (!g.allowed) { respond_rlv(call, g, limit); return; }

        g_cam = cam;
        idmcp_tool_ok(call, pose_json(g_cam));
    };
    reg.add(std::move(set));

    IDMCPTool get;
    get.name = "camera.get";
    get.description =
        "Read your own camera's pose (see camera.set). Returns {set:false} when none is "
        "placed, else {set:true, eye, target, distance, yaw_deg, pitch_deg, fov_deg}.";
    get.input_schema = boost::json::parse(R"({"type":"object","properties":{},"additionalProperties":false})");
    get.invoke = [](const boost::json::object&, const IDMCPCallPtr& call)
    {
        idmcp_tool_ok(call, pose_json(g_cam));
    };
    reg.add(std::move(get));

    IDMCPTool clear;
    clear.name = "camera.clear";
    clear.description = "Remove your own camera. Returns {set:false}.";
    clear.input_schema = boost::json::parse(R"({"type":"object","properties":{},"additionalProperties":false})");
    clear.invoke = [](const boost::json::object&, const IDMCPCallPtr& call)
    {
        g_cam = AgentCam();
        idmcp_tool_ok(call, pose_json(g_cam));
    };
    reg.add(std::move(clear));
}
