/**
 * @file fsgpudrivendraws.h
 * @brief GPU-driven draws, first slice: opaque shadow casters as one multi-draw (FSPerfGPUDrivenDraws)
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

#ifndef FS_GPUDRIVENDRAWS_H
#define FS_GPUDRIVENDRAWS_H

#include "pipeline.h"

#include <vector>

class LLDrawInfo;
class LLGLSLShader;

// Depth only programs that read each draw command's matrix from a storage buffer, through a per command
// record fetched with the command's base instance (fsGPUDrivenShadowV.glsl + shadowF.glsl): plain and, when
// FSPerfShadowSinglePass has its programs, single pass. Built only while FSPerfGPUDrivenDraws is on, shadows
// are on and the driver has gGLManager.mFSHasGPUDrivenDraws.
extern LLGLSLShader gFSGDDShadowProgram;
extern LLGLSLShader gFSGDDShadowSPProgram;

bool fsGDDLoadShaders(S32 shader_level, bool use_shadow);
void fsGDDUnloadShaders();

// Frees the position arenas and the per draw buffers (GL context loss, shader reloads).
void fsGDDReleaseGL();

// FSPerfGPUDrivenDraws is on and the program for this kind of pass is built.
bool fsGDDShadowEnabled(bool single_pass);

// The untextured opaque casters of a shadow pass (LLPipeline::fsRenderShadowOpaqueBatches), with the stock
// depth only program bound and the pass's projection and view loaded. Draws every batch whose vertex buffer
// is (or now gets) a copy in the position arena with one glMultiDrawElementsIndirect, removes them from
// batches and leaves the rest for the stock loop; the stock program is bound again on return.
// Returns the number of multi-draw calls made.
U32 fsGDDDrawShadowOpaque(std::vector<LLDrawInfo*>& batches);

// The same for the recorded opaque non-rigged casters of FSPerfShadowSinglePass (fsShadowSinglePassFinish),
// list sorted by cascade mask, then model matrix, buffer and index offset; clip holds the light projection *
// view of each cascade. One multi-draw per cascade set, one instance per cascade.
U32 fsGDDDrawShadowSinglePass(std::vector<LLPipeline::FSShadowSPEntry>& list, const glm::mat4* clip);

#endif // FS_GPUDRIVENDRAWS_H
