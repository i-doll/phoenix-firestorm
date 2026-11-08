/**
 * @file fsgpudrivenpbr.h
 * @brief GPU-driven draws for the deferred GLTF PBR opaque pass (FSPerfGPUDrivenPBR)
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

#ifndef FS_GPUDRIVENPBR_H
#define FS_GPUDRIVENPBR_H

#include "stdtypes.h"

class LLGLSLShader;

// gDeferredPBROpaqueProgram built with FS_GDD (matrices and material from storage buffers, textures bound
// per material), and with FS_GDD + FS_GDD_BINDLESS (textures as bindless handles too). Built only while
// FSPerfGPUDrivenPBR is on and the driver has gGLManager.mFSHasGPUDrivenDraws; the bindless one only with
// FSPerfGPUDrivenPBRBindless and GL_ARB_bindless_texture as well.
extern LLGLSLShader gFSGDDPBRProgram;
extern LLGLSLShader gFSGDDPBRBindlessProgram;

// After gDeferredPBROpaqueProgram is set up (LLViewerShaderMgr::loadShadersDeferred, via fsGDDLoadShaders).
void fsGDDPBRLoadShaders();
void fsGDDPBRUnloadShaders();
// Frees the arena, the per draw buffers and every bindless handle and sampler.
void fsGDDPBRReleaseGL();

// LLDrawPoolGLTFPBR::renderDeferred, in place of pushGLTFBatches(type) with gDeferredPBROpaqueProgram bound.
// Draws the pass's batches in their order: runs that can come from the arena as multi-draws, the rest the
// stock way (LLRenderPass::pushGLTFBatch). False, with nothing drawn, when the feature is off or can't run
// here; the caller then draws the pass the stock way. gDeferredPBROpaqueProgram is bound again on return.
bool fsGDDDrawPBR(U32 type);

// The bindless texture handles (one sampler per texture state, dropped when LLImageGL changes the texture),
// shared with FSPerfGPUDrivenLegacy (fsgpudrivenlegacy.cpp).
class LLViewerTexture;
// At shader load: NVIDIA only; tests once per GL context that handles can be dropped again, installs the
// LLImageGL hook. True when handles can be used.
bool fsGDDBindlessSetup();
bool fsGDDBindlessReady();
// The resident handle of tex as LLTexUnit would sample it, or 0 if it can't have one (not fetched, not
// loaded, one or two channels, options pending, media). Main thread.
U64 fsGDDTextureHandle(LLViewerTexture* tex, U32 frame);
// Idle handles go non resident, later away; does something every 120 frames.
void fsGDDSweepHandles(U32 frame);

#endif // FS_GPUDRIVENPBR_H
