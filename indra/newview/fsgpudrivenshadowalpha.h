/**
 * @file fsgpudrivenshadowalpha.h
 * @brief GPU-driven alpha tested shadow casters: multi-draws with bindless textures (FSPerfGPUDrivenShadowAlpha)
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

#ifndef FS_GPUDRIVENSHADOWALPHA_H
#define FS_GPUDRIVENSHADOWALPHA_H

#include "pipeline.h"

#include <vector>

class LLDrawInfo;
class LLGLSLShader;

// fsGPUDrivenShadowAlphaV.glsl + fsGPUDrivenShadowAlphaF.glsl: plain and, when FSPerfShadowSinglePass has its
// programs, single pass. Built only while FSPerfGPUDrivenShadowAlpha is on, shadows are on, the driver has
// gGLManager.mFSHasGPUDrivenDraws and bindless textures are usable (fsGDDBindlessSetup, NVIDIA only).
extern LLGLSLShader gFSGDDShadowAlphaProgram;
extern LLGLSLShader gFSGDDShadowAlphaSPProgram;

// What the stock path draws a batch with, and so what the GPU-driven program does for it
enum EFSGDDShadowAlphaKind
{
    FS_GDD_SA_ALPHA_MASK = 0,   // gDeferredShadowAlphaMaskProgram: texture batch, vertex alpha, semi transparent dither
    FS_GDD_SA_FULLBRIGHT_MASK,  // gDeferredShadowFullbrightAlphaMaskProgram: the same without the vertex alpha
    FS_GDD_SA_TREE,             // gDeferredTreeShadowProgram: one texture with its animation, cutoff only
    FS_GDD_SA_GLTF_MASK,        // gDeferredShadowGLTFAlphaMaskProgram: base colour texture and transform, vertex alpha
    FS_GDD_SA_KINDS
};

// Called by fsGDDLoadShaders, fsGDDUnloadShaders and fsGDDReleaseGL (fsgpudrivendraws.cpp).
void fsGDDShadowAlphaLoadShaders(S32 shader_level, bool use_shadow);
void fsGDDShadowAlphaUnloadShaders();
void fsGDDShadowAlphaReleaseGL();

// Starts the pending list of a shadow pass (renderShadow) or of a single pass update (fsShadowSinglePassFinish).
// False when the feature is off or its program for this kind of pass isn't usable; then nothing is collected
// and the caller draws everything the stock way.
bool fsGDDShadowAlphaBegin(bool single_pass);

// Moves the batches the GPU-driven path can draw (not rigged, in the arena, every texture the stock path would
// sample with a bindless handle) from batches into the pending list; the rest stay, in order, for the stock
// loop. fixed_cutoff < 0: each batch's mAlphaMaskCutoff, as pushMaskBatches sets it; otherwise the minimum
// alpha the caller gave the stock program (pushBatches). GLTF batches use their material's cutoff.
void fsGDDShadowAlphaCollect(U32 kind, std::vector<LLDrawInfo*>& batches, F32 fixed_cutoff);
// The same for a single pass list (entries with their cascade masks), each batch's own cutoff.
void fsGDDShadowAlphaCollectSP(U32 kind, std::vector<LLPipeline::FSShadowSPEntry>& list);

// Draws the pending casters with the pass's projection and view loaded, target_width as the stock alpha mask
// programs get it (LLRenderTarget::sCurResX). The program bound on entry is bound again on return.
// Returns the number of multi-draw calls.
U32 fsGDDShadowAlphaFlush(U32 target_width);
// The same for a single pass update into the atlas: clip holds the light projection * view of each cascade,
// widths the width of each cascade's map; one multi-draw per cascade set, one instance per cascade.
U32 fsGDDShadowAlphaFlushSP(const glm::mat4* clip, const F32* widths);

#endif // FS_GPUDRIVENSHADOWALPHA_H
