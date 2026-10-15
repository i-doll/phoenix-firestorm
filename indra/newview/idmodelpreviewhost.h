/**
 * @file idmodelpreviewhost.h
 * @brief <ID> What LLModelPreview needs from whoever drives it.
 *
 * Part of Five's custom Firestorm fork. Custom code carries an `ID` prefix.
 *
 * LLModelPreview used to read its upload options straight from the Upload
 * Model floater's widgets, so it couldn't run without that floater. It now
 * asks its host instead. LLFloaterModelPreview answers from its widgets; the
 * MCP upload.mesh job (idmcp_meshupload.cpp) answers from its arguments, so
 * several jobs can run while the floater is free for the user.
 *
 * Merging upstream: when new code in llmodelpreview.cpp reads an option from
 * a widget, add a field here, fill it in LLFloaterModelPreview::
 * getPreviewSettings(), and read it through mHost->getPreviewSettings().
 * Widget writes go through mUI, which is null when there is no floater.
 */

#ifndef ID_IDMODELPREVIEWHOST_H
#define ID_IDMODELPREVIEWHOST_H

#include "llmodelloader.h"  // NUM_LOD
#include "llsd.h"

#include <string>

struct IDModelPreviewSettings
{
    std::string name;                   // description_form
    F32  import_scale    = 1.f;         // import_scale
    F32  pelvis_offset   = 0.f;         // pelvis_offset
    bool upload_textures = false;       // upload_textures
    bool upload_skin     = false;       // upload_skin
    bool upload_joints   = false;       // upload_joints
    F32  crease_angle    = 75.f;        // crease_angle

    // Per LOD, indexed LOD_IMPOSTOR..LOD_HIGH.
    // lod_source_*: an LLModelPreview::eLoDMode. High comes from the file, the
    // rest are generated (LOD_FROM_FILE = 0, MESH_OPTIMIZER_AUTO = 1).
    S32 lod_source[NUM_LOD]          = { 1, 1, 1, 0 };
    S32 lod_limit_mode[NUM_LOD]      = { 0, 0, 0, 0 };  // lod_mode_*: an LLModelPreview::eLoDLimit
    S32 lod_triangle_limit[NUM_LOD]  = { 0, 0, 0, 0 };  // lod_triangle_limit_*
    F32 lod_error_threshold[NUM_LOD] = { 0, 0, 0, 0 };  // lod_error_threshold_*, in percent
};

class IDModelPreviewHost
{
public:
    virtual ~IDModelPreviewHost() = default;

    virtual IDModelPreviewSettings getPreviewSettings() = 0;

    // Importer log. The second form takes a floater string name and its
    // arguments; lod tags the line, -1 for none.
    virtual void addPreviewLog(const std::string& str, bool flash) = 0;
    virtual void addPreviewLog(const std::string& message, const LLSD& args, bool flash, S32 lod) = 0;
};

#endif // ID_IDMODELPREVIEWHOST_H
