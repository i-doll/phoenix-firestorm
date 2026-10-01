/**
 * @file idmcp_meshupload.h
 * @brief <ID> MCP server: upload.mesh job (drives the Upload Model floater).
 *
 * Part of Five's custom Firestorm fork. Custom code carries an `ID` prefix.
 */

#ifndef ID_IDMCP_MESHUPLOAD_H
#define ID_IDMCP_MESHUPLOAD_H

#include "idmcpserver.h"   // IDMCPCallPtr
#include "lluuid.h"

#include <string>

struct IDMCPMeshUploadParams
{
    std::string path;                 // .dae / .gltf / .glb, already validated
    std::string name;                 // inventory name
    LLUUID      dest;                 // null = the Objects folder
    F32         scale           = 1.f;
    bool        textures        = false;
    int         skin_weights    = -1; // -1 = keep what the floater detected, 0 = off, 1 = on
    int         joint_positions = -1;
    int         lock_scale      = -1;
    std::string lod_file[3];          // by LLModel::LOD_IMPOSTOR..LOD_MEDIUM; empty = auto
    std::string physics = "none";     // a keyword (lower case) or a file path; none = floater default
    bool        analyze = false;
    bool        confirm = false;
};

// True for "none", "high", "medium", "low", "lowest", "cube". Expects lower case.
bool idmcp_mesh_is_physics_keyword(const std::string& lowered);

// Starts the job. Always answers `call`, possibly synchronously (busy, bad
// args). Main thread only.
void idmcp_mesh_upload_start(const IDMCPMeshUploadParams& p, const IDMCPCallPtr& call);

#endif // ID_IDMCP_MESHUPLOAD_H
