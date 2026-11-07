/**
 * @file fsskinonce.h
 * @brief Skin rigged meshes once per frame instead of in every pass (FSPerfSkinOncePerFrame)
 *
 * $LicenseInfo:firstyear=2026&license=viewerlgpl$
 * Phoenix Firestorm Viewer Source Code
 * Copyright (C) 2026, The Phoenix Firestorm Project, Inc.
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation;
 * version 2.1 of the License only.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301  USA
 *
 * The Phoenix Firestorm Project, Inc., 1831 Oakwood Drive, Fairmont, Minnesota 56031-3225 USA
 * http://www.firestormviewer.org
 * $/LicenseInfo$
 */

#ifndef FS_SKINONCE_H
#define FS_SKINONCE_H

#include "stdtypes.h"
#include "llvertexbuffer.h"

#include <map>
#include <string>

// Rigged meshes are skinned in the vertex shader of every pass that draws them: G-buffer,
// alpha, glow, each sun cascade, projector shadows, probe and mirror captures. The expensive
// part of that is the same in every pass: decoding the four weights and blending four
// palette matrices read by dynamic index from a uniform array.
//
// With FSPerfSkinOncePerFrame on, the first rigged draw of a vertex range in a frame runs a
// small transform feedback program (avatar/fsSkinOnceV.glsl) that writes each vertex's
// blended skin matrix into a pooled buffer. Every rigged draw of that range then reads the
// matrix back by gl_VertexID (FS_SKIN_ONCE in avatar/objectSkinV.glsl) instead of blending.
// When the range's vertex data and its palette are both unchanged since the last capture
// (an avatar standing still with FSPerfRiggedPaletteCache on, or one on its hidden-update
// period), the capture is skipped too.
//
// Hooks: LLVertexBuffer::drawRange() calls the draw hook for programs with object skinning,
// and LLRenderPass::uploadMatrixPalette() reports the palette those draws use. Nothing is
// installed or allocated while the setting is off.
namespace FSSkinOnce
{
    // From the global define setup in LLViewerShaderMgr: when the setting is on and the GL
    // can do it, adds FS_SKIN_ONCE and installs the draw hook; otherwise removes the hook.
    void configureShaders(std::map<std::string, std::string>& global_defines);

    // Removes the draw hook and releases the capture program, the pool and every cached
    // capture (shader unload)
    void cleanupGL();

    void noteSkinImpl(const void* avatar, U64 skin_hash, const F32* gl_palette, U32 joint_count, U32 palette_serial);

    // The palette the following rigged draws use, as uploaded by uploadMatrixPalette()
    inline void noteSkin(const void* avatar, U64 skin_hash, const F32* gl_palette, U32 joint_count, U32 palette_serial)
    {
        if (LLVertexBuffer::sFSRiggedDrawHook)
        {
            noteSkinImpl(avatar, skin_hash, gl_palette, joint_count, palette_serial);
        }
    }
}

#endif // FS_SKINONCE_H
