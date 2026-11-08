/**
 * @file fsshadowsinglepass.h
 * @brief Sun shadow cascades drawn in one pass (FSPerfShadowSinglePass)
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

#ifndef FS_SHADOWSINGLEPASS_H
#define FS_SHADOWSINGLEPASS_H

#include "stdtypes.h"

class LLGLSLShader;

// Single pass copies of the sun shadow caster programs, built with FS_SHADOW_SINGLE_PASS set to the
// extension that lets the vertex shader write gl_ViewportIndex (1 ARB, 2 NV, 3 AMD), and the program
// that merges an atlas cell into a cascade's shadow map. Built only while FSPerfShadowSinglePass is
// on, sun shadows are on and the extension is there.
extern LLGLSLShader gFSShadowSPProgram;                     // opaque, shadowV / shadowSkinnedV
extern LLGLSLShader gFSShadowSPAlphaMaskProgram;            // shadowAlphaMaskV
extern LLGLSLShader gFSShadowSPFullbrightAlphaMaskProgram;  // shadowAlphaMaskV, IS_FULLBRIGHT
extern LLGLSLShader gFSShadowSPTreeProgram;                 // treeShadowV / treeShadowSkinnedV, material alpha masks
extern LLGLSLShader gFSShadowSPMergeProgram;
extern bool gFSShadowSinglePassShaders;                     // all of the above are built

bool fsLoadShadowSinglePassShaders(S32 shader_level, bool use_sun_shadow);
void fsUnloadShadowSinglePassShaders();

#endif // FS_SHADOWSINGLEPASS_H
