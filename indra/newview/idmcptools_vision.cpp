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
 * Main-thread only. No RLV gate (it's the user's own view).
 */

#include "llviewerprecompiledheaders.h"

#include "idmcp.h"
#include "idmcptools.h"
#include "idmcpserver.h"

#include "llviewerwindow.h"     // gViewerWindow, rawSnapshot
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
        const bool ok = gViewerWindow->rawSnapshot(
            raw, width, height,
            /*keep_window_aspect*/ true, /*is_texture*/ false,
            /*show_ui*/ w->show_ui, /*show_hud*/ w->show_hud,
            /*do_rebuild*/ false, /*no_post*/ false, /*show_balance*/ false,
            LLSnapshotModel::SNAPSHOT_TYPE_COLOR);
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
        "its path). Returns {format, width, height, image_base64} or {..., path}.";
    t.input_schema = boost::json::parse(
        R"({"type":"object","properties":{"width":{"type":"integer"},"height":{"type":"integer"},"hide_ui":{"type":"boolean"},"show_hud":{"type":"boolean"},"format":{"type":"string","enum":["jpeg","png"]},"quality":{"type":"integer"},"to_file":{"type":"boolean"}},"additionalProperties":false})");
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
        // Defer to the next mainloop tick so rawSnapshot runs at a frame boundary.
        g_waits.push_back(w);
        ensure_tick();
    };
    reg.add(std::move(t));
}
