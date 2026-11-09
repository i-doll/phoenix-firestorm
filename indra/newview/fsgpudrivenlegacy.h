/**
 * @file fsgpudrivenlegacy.h
 * @brief GPU-driven draws for the deferred legacy (non PBR) opaque passes (FSPerfGPUDrivenLegacy)
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

#ifndef FS_GPUDRIVENLEGACY_H
#define FS_GPUDRIVENLEGACY_H

#include "stdtypes.h"

class LLGLSLShader;
class LLRenderPass;
class LLViewerTexture;

// gDeferredDiffuseProgram, gDeferredDiffuseAlphaMaskProgram, gDeferredFullbrightProgram and
// gDeferredFullbrightAlphaMaskProgram built with FS_GDD_LEGACY: matrices from a storage buffer, the diffuse
// texture as a bindless handle. Built only while FSPerfGPUDrivenLegacy is on, the driver has
// gGLManager.mFSHasGPUDrivenDraws and bindless textures work (NVIDIA, fsGDDBindlessSetup).
extern LLGLSLShader gFSGDDDiffuseProgram;
extern LLGLSLShader gFSGDDDiffuseAlphaMaskProgram;
extern LLGLSLShader gFSGDDFullbrightProgram;
extern LLGLSLShader gFSGDDFullbrightAlphaMaskProgram;

// After the stock programs are set up (via fsGDDLoadShaders, after fsGDDPBRLoadShaders).
void fsGDDLegacyLoadShaders();
void fsGDDLegacyUnloadShaders();
// Frees the arena and the per draw buffers.
void fsGDDLegacyReleaseGL();

// In place of pushBatches(type, true, true) (PASS_SIMPLE, PASS_FULLBRIGHT) or pushMaskBatches(type, true, true)
// (PASS_ALPHA_MASK, PASS_FULLBRIGHT_ALPHA_MASK) with the pass's stock program bound. Draws the pass's batches
// in their order: runs that can come from the arena as multi-draws, the rest the stock way (pool.pushBatch).
// False, with nothing drawn, when the feature is off or can't run here; the caller then draws the pass the
// stock way. The stock program is bound again on return.
bool fsGDDDrawLegacy(LLRenderPass& pool, U32 type);

// The per pass handle cache of the bindless textures (LLViewerTexture::mFSGDDPass), shared by the passes that
// look textures up per batch (also FSPerfGPUDrivenMaterials, fsgpudrivenmaterials.cpp). fsGDDBeginTexturePass
// starts a pass; fsGDDPassTextureHandle is fsGDDTextureHandle looked up once per texture per pass, and sets a
// texture with a handle active, as bindFast would.
void fsGDDBeginTexturePass();
U64 fsGDDPassTextureHandle(LLViewerTexture* tex, U32 frame);

#endif // FS_GPUDRIVENLEGACY_H
