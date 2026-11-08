/**
 * @file fsshadowsinglepass.cpp
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

// Every sun cascade culls, sorts and draws its own casters, so a caster that reaches several cascades
// is submitted once per cascade: one draw call, one model matrix load, one buffer bind and, for alpha
// masked and rigged casters, texture binds and a skin palette upload each time.
//
// With FSPerfShadowSinglePass on, the cascade loop in generateSunShadow runs as before (cull, sort, the
// static cache, everything the cascade draws), except that renderShadow records the batches of the
// opaque, alpha masked, fullbright alpha masked and material alpha masked pools (rigged or not) instead
// of drawing them, each with the cascade's bit. After the last cascade, fsShadowSinglePassFinish draws
// every recorded batch once into a depth atlas that holds one cell per cascade, the size of that
// cascade's map: an instanced draw with one instance per cascade that recorded the batch, the vertex
// shader transforming the vertex with that cascade's light matrices and sending it to that cascade's
// viewport (gl_ViewportIndex, so the cells clip exactly like the cascade maps). Then each cascade map
// keeps the nearer of its own depth and its atlas cell's (fsShadowSinglePassMergeF.glsl, depth test
// GL_LESS). A depth only pass doesn't depend on draw order, so the maps end up as they would have.
//
// What each cascade draws is exactly what it recorded, its own cull result, so the per cascade cull,
// the minimum caster size of each cascade, the far cascade throttle (skipped cascades never record) and
// the static cache (cached cascades record only their dynamic opaque casters) keep working unchanged.
// Coloured shadows test translucent casters against the cascade's finished depth, so for those
// cascades that pass runs after the merge. Opaque GLTF casters are recorded too (those with double sided
// materials in a list drawn without face culling), as they use the same depth only program. GLTF alpha
// masked, alpha blended, grass, avatar, terrain and tree casters keep their per cascade passes. Probe captures and projector shadows are untouched.
//
// The consumers of the shadow maps (shadowUtil.glsl, PCSS, blur, coloured shadows, cloud and contact
// shadows) read the same textures as before and are unchanged. The cost is the atlas (one more depth
// buffer the size of all four cascade maps) and one full screen depth merge per cascade.

#include "llviewerprecompiledheaders.h"

#include "fsshadowsinglepass.h"

#include "fsgpudrivendraws.h"
#include "pipeline.h"

#include "lldrawpool.h"
#include "llenvironment.h"
#include "llgl.h"
#include "llglslshader.h"
#include "llrender.h"
#include "llrendertarget.h"
#include "llspatialpartition.h"
#include "llvertexbuffer.h"
#include "llviewercontrol.h"
#include "llviewershadermgr.h"
#include "llvoavatar.h"

#include <algorithm>
#include <utility>

extern bool gCubeSnapshot;

LLGLSLShader gFSShadowSPProgram;
LLGLSLShader gFSShadowSPSkinnedProgram;
LLGLSLShader gFSShadowSPAlphaMaskProgram;
LLGLSLShader gFSShadowSPSkinnedAlphaMaskProgram;
LLGLSLShader gFSShadowSPFullbrightAlphaMaskProgram;
LLGLSLShader gFSShadowSPSkinnedFullbrightAlphaMaskProgram;
LLGLSLShader gFSShadowSPTreeProgram;
LLGLSLShader gFSShadowSPSkinnedTreeProgram;
LLGLSLShader gFSShadowSPMergeProgram;
bool gFSShadowSinglePassShaders = false;

namespace
{
    // as the stock programs' add_common_permutations
    void add_common_permutations(LLGLSLShader& shader)
    {
        static LLCachedControl<bool> emissive(gSavedSettings, "RenderEnableEmissiveBuffer", false);
        if (emissive)
        {
            shader.addPermutation("HAS_EMISSIVE", "1");
        }
    }

    // as llviewershadermgr.cpp's make_rigged_variant
    bool make_rigged_variant(LLGLSLShader& shader, LLGLSLShader& rigged)
    {
        rigged.mName = llformat("Skinned %s", shader.mName.c_str());
        rigged.mFeatures = shader.mFeatures;
        rigged.mFeatures.hasObjectSkinning = true;
        rigged.mDefines = shader.mDefines;
        rigged.addPermutation("HAS_SKIN", "1");
        rigged.mShaderFiles = shader.mShaderFiles;
        rigged.mShaderLevel = shader.mShaderLevel;
        rigged.mShaderGroup = shader.mShaderGroup;
        shader.mRiggedVariant = &rigged;
        return rigged.createShader();
    }

    void begin_program(LLGLSLShader& shader, const char* name, S32 shader_level)
    {
        shader.mName = name;
        shader.mShaderFiles.clear();
        shader.clearPermutations();
        shader.mShaderLevel = shader_level;
    }

    const LLStaticHashedString sClip("fs_shadow_clip");
    const LLStaticHashedString sCascade("fs_shadow_cascade");
    const LLStaticHashedString sWidth("fs_shadow_width");
    const LLStaticHashedString sCell("fs_shadow_cell");

    const F32 FS_ALPHA_BLEND_CUTOFF = 0.598f;   // pipeline.cpp's ALPHA_BLEND_CUTOFF
}

void fsUnloadShadowSinglePassShaders()
{
    gFSShadowSPProgram.unload();
    gFSShadowSPSkinnedProgram.unload();
    gFSShadowSPAlphaMaskProgram.unload();
    gFSShadowSPSkinnedAlphaMaskProgram.unload();
    gFSShadowSPFullbrightAlphaMaskProgram.unload();
    gFSShadowSPSkinnedFullbrightAlphaMaskProgram.unload();
    gFSShadowSPTreeProgram.unload();
    gFSShadowSPSkinnedTreeProgram.unload();
    gFSShadowSPMergeProgram.unload();
    gFSShadowSinglePassShaders = false;
}

// Each program mirrors the stock one it stands in for (llviewershadermgr.cpp), plus FS_SHADOW_SINGLE_PASS.
bool fsLoadShadowSinglePassShaders(S32 shader_level, bool use_sun_shadow)
{
    fsUnloadShadowSinglePassShaders();

    static LLCachedControl<bool> single_pass(gSavedSettings, "FSPerfShadowSinglePass", false);
    if (!use_sun_shadow || !single_pass)
    {
        return false;
    }
    const S32 ext = gGLManager.mFSVertexViewportIndex;
    if (ext <= 0)
    {
        LL_INFOS_ONCE("ShaderLoading") << "FSPerfShadowSinglePass: the driver can't set the viewport from the vertex shader "
                                       << "(needs OpenGL 4.1 and GL_ARB_shader_viewport_layer_array, GL_NV_viewport_array2 or "
                                       << "GL_AMD_vertex_shader_viewport_index), sun shadow cascades keep their own passes" << LL_ENDL;
        return false;
    }
    const std::string define = llformat("%d", ext);

    // opaque, gDeferredShadowProgram and gDeferredSkinnedShadowProgram
    begin_program(gFSShadowSPProgram, "FS Single Pass Shadow Shader", shader_level);
    gFSShadowSPProgram.mShaderFiles.push_back(std::make_pair("deferred/shadowV.glsl", GL_VERTEX_SHADER));
    gFSShadowSPProgram.mShaderFiles.push_back(std::make_pair("deferred/shadowF.glsl", GL_FRAGMENT_SHADER));
    gFSShadowSPProgram.addPermutation("FS_SHADOW_SINGLE_PASS", define);
    gFSShadowSPProgram.mRiggedVariant = &gFSShadowSPSkinnedProgram;
    bool success = gFSShadowSPProgram.createShader();

    if (success)
    {
        begin_program(gFSShadowSPSkinnedProgram, "FS Single Pass Skinned Shadow Shader", shader_level);
        gFSShadowSPSkinnedProgram.mFeatures.isDeferred = true;
        gFSShadowSPSkinnedProgram.mFeatures.hasShadows = true;
        gFSShadowSPSkinnedProgram.mFeatures.hasObjectSkinning = true;
        gFSShadowSPSkinnedProgram.mShaderFiles.push_back(std::make_pair("deferred/shadowSkinnedV.glsl", GL_VERTEX_SHADER));
        gFSShadowSPSkinnedProgram.mShaderFiles.push_back(std::make_pair("deferred/shadowF.glsl", GL_FRAGMENT_SHADER));
        add_common_permutations(gFSShadowSPSkinnedProgram);
        gFSShadowSPSkinnedProgram.addPermutation("FS_SHADOW_SINGLE_PASS", define);
        success = gFSShadowSPSkinnedProgram.createShader();
    }

    // gDeferredShadowFullbrightAlphaMaskProgram
    if (success)
    {
        begin_program(gFSShadowSPFullbrightAlphaMaskProgram, "FS Single Pass Shadow Fullbright Alpha Mask Shader", shader_level);
        gFSShadowSPFullbrightAlphaMaskProgram.mFeatures.mIndexedTextureChannels = LLGLSLShader::sIndexedTextureChannels;
        gFSShadowSPFullbrightAlphaMaskProgram.mShaderFiles.push_back(std::make_pair("deferred/shadowAlphaMaskV.glsl", GL_VERTEX_SHADER));
        gFSShadowSPFullbrightAlphaMaskProgram.mShaderFiles.push_back(std::make_pair("deferred/shadowAlphaMaskF.glsl", GL_FRAGMENT_SHADER));
        gFSShadowSPFullbrightAlphaMaskProgram.addPermutation("DEPTH_CLAMP", "1");
        gFSShadowSPFullbrightAlphaMaskProgram.addPermutation("IS_FULLBRIGHT", "1");
        add_common_permutations(gFSShadowSPFullbrightAlphaMaskProgram);
        gFSShadowSPFullbrightAlphaMaskProgram.addPermutation("FS_SHADOW_SINGLE_PASS", define);
        success = make_rigged_variant(gFSShadowSPFullbrightAlphaMaskProgram, gFSShadowSPSkinnedFullbrightAlphaMaskProgram);
        success = success && gFSShadowSPFullbrightAlphaMaskProgram.createShader();
    }

    // gDeferredShadowAlphaMaskProgram
    if (success)
    {
        begin_program(gFSShadowSPAlphaMaskProgram, "FS Single Pass Shadow Alpha Mask Shader", shader_level);
        gFSShadowSPAlphaMaskProgram.mFeatures.mIndexedTextureChannels = LLGLSLShader::sIndexedTextureChannels;
        gFSShadowSPAlphaMaskProgram.mShaderFiles.push_back(std::make_pair("deferred/shadowAlphaMaskV.glsl", GL_VERTEX_SHADER));
        gFSShadowSPAlphaMaskProgram.mShaderFiles.push_back(std::make_pair("deferred/shadowAlphaMaskF.glsl", GL_FRAGMENT_SHADER));
        gFSShadowSPAlphaMaskProgram.addPermutation("FS_SHADOW_SINGLE_PASS", define);
        success = make_rigged_variant(gFSShadowSPAlphaMaskProgram, gFSShadowSPSkinnedAlphaMaskProgram);
        success = success && gFSShadowSPAlphaMaskProgram.createShader();
    }

    // gDeferredTreeShadowProgram and gDeferredSkinnedTreeShadowProgram
    if (success)
    {
        begin_program(gFSShadowSPTreeProgram, "FS Single Pass Tree Shadow Shader", shader_level);
        gFSShadowSPTreeProgram.mShaderFiles.push_back(std::make_pair("deferred/treeShadowV.glsl", GL_VERTEX_SHADER));
        gFSShadowSPTreeProgram.mShaderFiles.push_back(std::make_pair("deferred/treeShadowF.glsl", GL_FRAGMENT_SHADER));
        gFSShadowSPTreeProgram.addPermutation("FS_SHADOW_SINGLE_PASS", define);
        gFSShadowSPTreeProgram.mRiggedVariant = &gFSShadowSPSkinnedTreeProgram;
        success = gFSShadowSPTreeProgram.createShader();
    }
    if (success)
    {
        begin_program(gFSShadowSPSkinnedTreeProgram, "FS Single Pass Skinned Tree Shadow Shader", shader_level);
        gFSShadowSPSkinnedTreeProgram.mFeatures.hasObjectSkinning = true;
        gFSShadowSPSkinnedTreeProgram.mShaderFiles.push_back(std::make_pair("deferred/treeShadowSkinnedV.glsl", GL_VERTEX_SHADER));
        gFSShadowSPSkinnedTreeProgram.mShaderFiles.push_back(std::make_pair("deferred/treeShadowF.glsl", GL_FRAGMENT_SHADER));
        gFSShadowSPSkinnedTreeProgram.addPermutation("FS_SHADOW_SINGLE_PASS", define);
        success = gFSShadowSPSkinnedTreeProgram.createShader();
    }

    if (success)
    {
        begin_program(gFSShadowSPMergeProgram, "FS Single Pass Shadow Merge Shader", shader_level);
        gFSShadowSPMergeProgram.mShaderFiles.push_back(std::make_pair("deferred/postDeferredNoTCV.glsl", GL_VERTEX_SHADER));
        gFSShadowSPMergeProgram.mShaderFiles.push_back(std::make_pair("deferred/fsShadowSinglePassMergeF.glsl", GL_FRAGMENT_SHADER));
        success = gFSShadowSPMergeProgram.createShader();
    }

    if (!success)
    {
        LL_WARNS("ShaderLoading") << "FSPerfShadowSinglePass: the single pass shadow programs failed to build, "
                                  << "sun shadow cascades keep their own passes" << LL_ENDL;
        fsUnloadShadowSinglePassShaders();
        return false;
    }
    gFSShadowSinglePassShaders = true;
    return true;
}

void LLPipeline::fsReleaseShadowSinglePass()
{
    mFSShadowAtlas.release();
    for (auto& kind : mFSShadowSPEntries)
    {
        for (auto& list : kind)
        {
            list.clear();
        }
    }
    mFSShadowSPActive = false;
    mFSShadowSPCascade = -1;
    mFSShadowSPRecorded = 0;
    mFSShadowSPColored = 0;
    mFSShadowSPLastCull = nullptr;
}

// generateSunShadow, before the cascade loop: decides whether this update records, and lays out the atlas.
void LLPipeline::fsShadowSinglePassBegin()
{
    mFSShadowSPActive = false;
    mFSShadowSPCascade = -1;
    mFSShadowSPRecorded = 0;
    mFSShadowSPColored = 0;
    mFSShadowSPLastCull = nullptr;

    if (gCubeSnapshot)
    {   // probe shadows have their own path; keep the atlas for the main view
        return;
    }

    static LLCachedControl<bool> single_pass(gSavedSettings, "FSPerfShadowSinglePass", false);
    if (!single_pass || !gFSShadowSinglePassShaders || RenderShadowDetail <= 0 || RenderShadowDetail > 2 ||
        mRT != &mMainRT || mScreenTriangleVB.isNull())
    {
        if (mFSShadowAtlas.getDepth())
        {
            mFSShadowAtlas.release();
        }
        return;
    }

    U32 width[4];
    U32 height[4];
    for (U32 c = 0; c < 4; ++c)
    {
        width[c] = mRT->shadow[c].getWidth();
        height[c] = mRT->shadow[c].getHeight();
        if (!width[c] || !height[c] || !mRT->shadow[c].getDepth())
        {
            return;
        }
    }

    // 2 x 2 cells, each the size of its cascade's map: 0 1 on the bottom row, 2 3 above
    const U32 col0 = llmax(width[0], width[2]);
    const U32 row0 = llmax(height[0], height[1]);
    const U32 atlas_width = col0 + llmax(width[1], width[3]);
    const U32 atlas_height = row0 + llmax(height[2], height[3]);
    static GLint max_viewport[2] = { 0, 0 };
    if (!max_viewport[0])
    {
        glGetIntegerv(GL_MAX_VIEWPORT_DIMS, max_viewport);
    }
    const U32 max_size = (U32)llmax(gGLManager.mGLMaxTextureSize, 0);
    if (atlas_width > max_size || atlas_height > max_size ||
        llmax(col0, atlas_width - col0) > (U32)max_viewport[0] || llmax(row0, atlas_height - row0) > (U32)max_viewport[1])
    {
        LL_WARNS_ONCE("Rendering") << "FSPerfShadowSinglePass: a " << atlas_width << "x" << atlas_height
                                   << " shadow atlas is over the texture size limit, sun shadow cascades keep their own passes" << LL_ENDL;
        mFSShadowAtlas.release();
        return;
    }
    mFSShadowSPCell[0][0] = 0;
    mFSShadowSPCell[0][1] = 0;
    mFSShadowSPCell[1][0] = (S32)col0;
    mFSShadowSPCell[1][1] = 0;
    mFSShadowSPCell[2][0] = 0;
    mFSShadowSPCell[2][1] = (S32)row0;
    mFSShadowSPCell[3][0] = (S32)col0;
    mFSShadowSPCell[3][1] = (S32)row0;

    if (mFSShadowAtlas.getWidth() != atlas_width || mFSShadowAtlas.getHeight() != atlas_height || !mFSShadowAtlas.getDepth())
    {
        mFSShadowAtlas.release();
        // the format of the cascade maps, so depth values carry over exactly
        if (!mFSShadowAtlas.allocate(atlas_width, atlas_height, 0, true))
        {
            LL_WARNS_ONCE("Rendering") << "FSPerfShadowSinglePass: could not allocate the shadow atlas, sun shadow cascades keep their own passes" << LL_ENDL;
            mFSShadowAtlas.release();
            return;
        }
        // read with texelFetch by the merge: no mipmaps, no compare
        gGL.getTexUnit(0)->bind(&mFSShadowAtlas, true);
        gGL.getTexUnit(0)->setTextureFilteringOption(LLTexUnit::TFO_POINT);
        gGL.getTexUnit(0)->setTextureAddressMode(LLTexUnit::TAM_CLAMP);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_COMPARE_MODE, GL_NONE);
        gGL.getTexUnit(0)->unbind(LLTexUnit::TT_TEXTURE);
    }

    mFSShadowSPActive = true;
}

// generateSunShadow, right before renderShadow of a sun cascade
void LLPipeline::fsShadowSinglePassBeginCascade(S32 cascade, const glm::mat4& proj, const glm::mat4& view, LLCullResult& result)
{
    mFSShadowSPCascade = -1;
    if (!mFSShadowSPActive || cascade < 0 || cascade > 3)
    {
        return;
    }
    mFSShadowSPView[cascade] = view;
    mFSShadowSPProj[cascade] = proj;
    mFSShadowSPCull[cascade] = &result;
    mFSShadowSPLastCull = &result;
    mFSShadowSPRecorded |= 1u << cascade;
    mFSShadowSPCascade = cascade;
}

void LLPipeline::fsShadowSinglePassEndCascade()
{
    mFSShadowSPCascade = -1;
}

// renderShadow: instead of drawing a pool, record its batches for this cascade. False when not recording,
// then renderShadow draws the pool as usual. A null types takes the kind's own pass types.
bool LLPipeline::fsShadowSinglePassRecord(U32 kind, bool rigged, const U32* types, U32 type_count)
{
    if (mFSShadowSPCascade < 0 || kind >= FS_SHADOW_SP_KINDS)
    {
        return false;
    }
    LL_PROFILE_ZONE_SCOPED_CATEGORY_PIPELINE;

    static const U32 alpha_mask[] = { LLRenderPass::PASS_ALPHA_MASK };
    static const U32 fullbright_mask[] = { LLRenderPass::PASS_FULLBRIGHT_ALPHA_MASK };
    static const U32 gltf[] = { LLRenderPass::PASS_GLTF_PBR };
    // the order renderShadow draws them in
    static const U32 material_mask[] = { LLRenderPass::PASS_NORMSPEC_MASK, LLRenderPass::PASS_MATERIAL_ALPHA_MASK,
                                         LLRenderPass::PASS_SPECMAP_MASK, LLRenderPass::PASS_NORMMAP_MASK };
    if (!types)
    {
        switch (kind)
        {
            case FS_SHADOW_SP_ALPHA_MASK:       types = alpha_mask;      type_count = LL_ARRAY_SIZE(alpha_mask);      break;
            case FS_SHADOW_SP_FULLBRIGHT_MASK:  types = fullbright_mask; type_count = LL_ARRAY_SIZE(fullbright_mask); break;
            case FS_SHADOW_SP_MATERIAL_MASK:    types = material_mask;   type_count = LL_ARRAY_SIZE(material_mask);   break;
            case FS_SHADOW_SP_GLTF:             types = gltf;            type_count = LL_ARRAY_SIZE(gltf);            break;
            default:                            return false;
        }
    }

    // with the static cache on this cascade, its opaque non-rigged casters are either static (already in
    // the map from the cache) or dynamic, the same split fsRenderShadowOpaqueBatches makes
    const U32 filter = (kind == FS_SHADOW_SP_OPAQUE && !rigged) ? mFSShadowCasterFilter : (U32)FS_SHADOW_ALL;
    const U32 bit = 1u << mFSShadowSPCascade;
    std::vector<FSShadowSPEntry>& list = mFSShadowSPEntries[kind][rigged ? 1 : 0];
    // GLTF casters with double sided materials go to their own list, drawn without face culling
    std::vector<FSShadowSPEntry>* double_sided = kind == FS_SHADOW_SP_GLTF ?
        &mFSShadowSPEntries[FS_SHADOW_SP_GLTF_DOUBLE_SIDED][rigged ? 1 : 0] : nullptr;
    for (U32 t = 0; t < type_count; ++t)
    {
        const U32 type = rigged ? types[t] + 1 : types[t];   // rigged batches are in the next pass map
        for (LLCullResult::drawinfo_iterator i = beginRenderMap(type), end = endRenderMap(type); i != end; ++i)
        {
            LLDrawInfo* params = *i;
            if (!params || !params->mCount || params->mVertexBuffer.isNull())
            {
                continue;
            }
            if (filter != FS_SHADOW_ALL && fsIsStaticShadowBatch(params) != (filter == FS_SHADOW_STATIC))
            {
                continue;
            }
            if (double_sided && params->mGLTFMaterial.notNull() && params->mGLTFMaterial->mDoubleSided)
            {
                double_sided->push_back({ params, params->mVertexBuffer, bit });
                continue;
            }
            list.push_back({ params, params->mVertexBuffer, bit });
        }
    }
    return true;
}

// renderShadow: coloured casters test against the finished cascade depth, which a recording cascade only
// has after the merge. True when they are left for fsShadowSinglePassFinish.
bool LLPipeline::fsShadowSinglePassDeferColored()
{
    if (mFSShadowSPCascade < 0 || mFSSpotColoredSlot >= 0 || mColoredShadowCascade != mFSShadowSPCascade)
    {
        return false;
    }
    mFSShadowSPColored |= 1u << mFSShadowSPCascade;
    std::vector<FSShadowSPEntry>& held = mFSShadowSPColoredCasters[mFSShadowSPCascade];
    held.clear();
    for (LLCullResult::drawinfo_iterator i = beginRenderMap(LLRenderPass::PASS_ALPHA), end = endRenderMap(LLRenderPass::PASS_ALPHA); i != end; ++i)
    {
        LLDrawInfo* params = *i;
        if (params && params->mCount && params->mVertexBuffer.notNull())
        {
            held.push_back({ params, params->mVertexBuffer, 0 });
        }
    }
    return true;
}

// generateSunShadow, after the cascade loop: draws the recorded batches once into the atlas, merges each
// recorded cascade's cell into its map, then draws the coloured casters that waited for it.
void LLPipeline::fsShadowSinglePassFinish()
{
    if (!mFSShadowSPActive)
    {
        return;
    }
    mFSShadowSPActive = false;
    mFSShadowSPCascade = -1;

    const U32 recorded = mFSShadowSPRecorded;
    if (!recorded)
    {
        return;
    }

    LL_PROFILE_ZONE_SCOPED_CATEGORY_PIPELINE;
    LL_PROFILE_GPU_ZONE("shadow single pass");

    static LLCachedControl<bool> batch_stats(gSavedSettings, "FSPerfShadowBatchStats", false);
    U32 recorded_batches = 0;
    U32 draws = 0;

    // one entry per batch, with the bits of every cascade that recorded it
    for (U32 kind = 0; kind < FS_SHADOW_SP_KINDS; ++kind)
    {
        for (U32 r = 0; r < 2; ++r)
        {
            std::vector<FSShadowSPEntry>& list = mFSShadowSPEntries[kind][r];
            recorded_batches += (U32)list.size();
            if (list.empty())
            {
                continue;
            }
            std::sort(list.begin(), list.end(), [](const FSShadowSPEntry& a, const FSShadowSPEntry& b)
            {
                return a.mParams.get() < b.mParams.get();
            });
            size_t out = 0;
            for (size_t i = 0; i < list.size(); ++i)
            {
                if (out && list[out - 1].mParams.get() == list[i].mParams.get())
                {
                    list[out - 1].mMask |= list[i].mMask;
                }
                else
                {
                    list[out++] = list[i];
                }
            }
            list.resize(out);
            list.erase(std::remove_if(list.begin(), list.end(), [](const FSShadowSPEntry& e)
            {
                return !e.mParams->mCount || e.mParams->mVertexBuffer.get() != e.mBuffer.get();
            }), list.end());

            // grouped by cascade set, then by what saves state changes: model matrix and buffer (opaque, where
            // runs of index ranges merge), skin palette (rigged), texture (alpha masked)
            const bool rigged = r == 1;
            const bool opaque = kind <= FS_SHADOW_SP_GLTF_DOUBLE_SIDED; // depth only, untextured
            std::sort(list.begin(), list.end(), [rigged, opaque](const FSShadowSPEntry& a, const FSShadowSPEntry& b)
            {
                if (a.mMask != b.mMask)
                {
                    return a.mMask < b.mMask;
                }
                const LLDrawInfo* lhs = a.mParams.get();
                const LLDrawInfo* rhs = b.mParams.get();
                if (rigged)
                {
                    if (lhs->mAvatar.get() != rhs->mAvatar.get())
                    {
                        return lhs->mAvatar.get() < rhs->mAvatar.get();
                    }
                    if (lhs->mSkinInfo.get() != rhs->mSkinInfo.get())
                    {
                        return lhs->mSkinInfo.get() < rhs->mSkinInfo.get();
                    }
                }
                if (!opaque && lhs->mTexture.get() != rhs->mTexture.get())
                {
                    return lhs->mTexture.get() < rhs->mTexture.get();
                }
                if (lhs->mModelMatrix != rhs->mModelMatrix)
                {
                    return lhs->mModelMatrix < rhs->mModelMatrix;
                }
                if (lhs->mVertexBuffer.get() != rhs->mVertexBuffer.get())
                {
                    return lhs->mVertexBuffer.get() < rhs->mVertexBuffer.get();
                }
                if (lhs->mOffset != rhs->mOffset)
                {
                    return lhs->mOffset < rhs->mOffset;
                }
                return lhs->mCount > rhs->mCount;
            });
        }
    }

    glm::mat4 clip[4];
    F32 widths[4];
    for (U32 c = 0; c < 4; ++c)
    {
        clip[c] = (recorded & (1u << c)) ? mFSShadowSPProj[c] * mFSShadowSPView[c] : glm::mat4(1.f);
        widths[c] = (F32)mRT->shadow[c].getWidth();
    }

    const glm::mat4 saved_view = get_current_modelview();
    const glm::mat4 saved_proj = get_current_projection();
    const bool saved_shadow_render = LLPipeline::sShadowRender;
    LLPipeline::sShadowRender = true;

    gGL.matrixMode(LLRender::MM_PROJECTION);
    gGL.pushMatrix();
    gGL.matrixMode(LLRender::MM_MODELVIEW);
    gGL.pushMatrix();

    GLint saved_scissor[4] = { 0, 0, 0, 0 };
    glGetIntegerv(GL_SCISSOR_BOX, saved_scissor);

    // ---- every recorded batch once, into the atlas cells of the cascades that recorded it
    mFSShadowAtlas.bindTarget();
    {
        LLGLDepthTest depth(GL_TRUE, GL_TRUE, GL_LESS);
        mFSShadowAtlas.clear(GL_DEPTH_BUFFER_BIT);
    }
    for (U32 c = 0; c < 4; ++c)
    {
        const LLRenderTarget& shadow = mRT->shadow[c];
        glViewportIndexedf(c, (F32)mFSShadowSPCell[c][0], (F32)mFSShadowSPCell[c][1], (F32)shadow.getWidth(), (F32)shadow.getHeight());
        glScissorIndexed(c, mFSShadowSPCell[c][0], mFSShadowSPCell[c][1], (GLsizei)shadow.getWidth(), (GLsizei)shadow.getHeight());
    }

    {
        LLGLEnable scissor(GL_SCISSOR_TEST);
        LLGLEnable cull(GL_CULL_FACE);
        LLGLEnable clamp_depth(GL_DEPTH_CLAMP);
        LLGLDepthTest depth_test(GL_TRUE, GL_TRUE, GL_LESS);
        gGL.setColorMask(false, false);

        // the programs transform agent space positions by each cascade's matrices, so the view is identity
        // and the model matrices alone go into modelview
        set_current_modelview(glm::mat4(1.f));
        set_current_projection(glm::mat4(1.f));
        gGL.matrixMode(LLRender::MM_PROJECTION);
        gGL.loadIdentity();
        gGL.matrixMode(LLRender::MM_MODELVIEW);
        gGL.loadIdentity();

        const S32 sun_up = LLEnvironment::instance().getIsSunUp() ? 1 : 0;
        LLVertexBuffer::unbind();

        for (U32 kind = 0; kind < FS_SHADOW_SP_KINDS; ++kind)
        {
            for (U32 r = 0; r < 2; ++r)
            {
                std::vector<FSShadowSPEntry>& list = mFSShadowSPEntries[kind][r];
                if (list.empty())
                {
                    continue;
                }
                const bool rigged = r == 1;
                const bool opaque = kind <= FS_SHADOW_SP_GLTF_DOUBLE_SIDED; // depth only, untextured
                LLGLDisable no_cull(kind == FS_SHADOW_SP_GLTF_DOUBLE_SIDED ? GL_CULL_FACE : 0);
                if (opaque && !rigged)
                {   // FSPerfGPUDrivenDraws: what is in the position arena goes out as one multi-draw per cascade set,
                    // the rest stays in the list for the loop below
                    draws += fsGDDDrawShadowSinglePass(list, clip);
                    if (list.empty())
                    {
                        continue;
                    }
                }
                LLGLSLShader& program = opaque ? gFSShadowSPProgram :
                                        kind == FS_SHADOW_SP_ALPHA_MASK ? gFSShadowSPAlphaMaskProgram :
                                        kind == FS_SHADOW_SP_FULLBRIGHT_MASK ? gFSShadowSPFullbrightAlphaMaskProgram :
                                        gFSShadowSPTreeProgram;
                // the stock code binds indexed texture batches for these two, the tree program samples one
                const bool batch_textures = kind == FS_SHADOW_SP_ALPHA_MASK || kind == FS_SHADOW_SP_FULLBRIGHT_MASK;

                program.bind(rigged);
                LLGLSLShader* shader = LLGLSLShader::sCurBoundShaderPtr;
                shader->uniformMatrix4fv(sClip, 4, GL_FALSE, glm::value_ptr(clip[0]));
                if (opaque)
                {
                    gGL.diffuseColor4f(1, 1, 1, 1);
                    gGL.getTexUnit(0)->disable();
                }
                else
                {
                    shader->uniform4fv(sWidth, 1, widths);
                    shader->uniform1i(LLShaderMgr::SUN_UP_FACTOR, sun_up);
                    shader->uniform1f(LLShaderMgr::DEFERRED_SHADOW_TARGET_WIDTH, widths[0]);
                    shader->setMinimumAlpha(FS_ALPHA_BLEND_CUTOFF);
                }

                gGL.loadMatrix(gGLModelView);
                gGLLastMatrix = NULL;

                const LLVOAvatar* last_avatar = nullptr;
                U64 last_mesh_id = 0;
                bool skip_last_skin = false;
                U32 mask = 0;
                const size_t count = list.size();
                size_t i = 0;
                while (i < count)
                {
                    if (list[i].mMask != mask)
                    {   // instance n goes to the n-th cascade in the set
                        mask = list[i].mMask;
                        F32 cascades[4] = { 0.f, 0.f, 0.f, 0.f };
                        U32 n = 0;
                        for (U32 c = 0; c < 4; ++c)
                        {
                            if (mask & (1u << c))
                            {
                                cascades[n++] = (F32)c;
                            }
                        }
                        shader->uniform4f(sCascade, cascades[0], cascades[1], cascades[2], cascades[3]);
                        LLVertexBuffer::sFSInstanceCount = n;
                    }

                    LLDrawInfo& params = *list[i].mParams;
                    if (opaque && !rigged)
                    {   // depth only, model matrix only: draw a run of contiguous index ranges as one call
                        LLVertexBuffer* buffer = params.mVertexBuffer.get();
                        U32 start = params.mStart;
                        U32 end = params.mEnd;
                        const U32 offset = params.mOffset;
                        U32 index_end = offset + params.mCount;
                        size_t next = i + 1;
                        for (; next < count; ++next)
                        {
                            const LLDrawInfo* other = list[next].mParams.get();
                            if (list[next].mMask != mask || other->mModelMatrix != params.mModelMatrix || other->mVertexBuffer.get() != buffer)
                            {
                                break;
                            }
                            if (other->mOffset + other->mCount <= index_end)
                            {   // inside the range just drawn
                                continue;
                            }
                            if (other->mOffset != index_end)
                            {
                                break;
                            }
                            index_end += other->mCount;
                            start = llmin(start, (U32)other->mStart);
                            end = llmax(end, (U32)other->mEnd);
                        }
                        LLRenderPass::applyModelMatrix(params.mModelMatrix);
                        buffer->setBuffer();
                        buffer->drawRange(LLRender::TRIANGLES, start, end, index_end - offset, offset);
                        ++draws;
                        i = next;
                        continue;
                    }

                    ++i;
                    if (rigged && !LLRenderPass::uploadMatrixPalette(params.mAvatar, params.mSkinInfo, last_avatar, last_mesh_id, skip_last_skin))
                    {
                        continue;
                    }
                    if (opaque)
                    {
                        mSimplePool->pushUntexturedBatch(params);
                    }
                    else
                    {
                        shader->setMinimumAlpha(params.mAlphaMaskCutoff);
                        mSimplePool->pushBatch(params, true, batch_textures);
                    }
                    ++draws;
                }
                LLVertexBuffer::sFSInstanceCount = 0;

                gGL.loadMatrix(gGLModelView);
                gGLLastMatrix = NULL;
                if (opaque)
                {
                    gGL.getTexUnit(0)->enable(LLTexUnit::TT_TEXTURE);
                }
            }
        }
        LLVertexBuffer::sFSInstanceCount = 0;
        LLVertexBuffer::unbind();
    }
    mFSShadowAtlas.flush(); // glViewport puts every viewport back
    glScissor(saved_scissor[0], saved_scissor[1], saved_scissor[2], saved_scissor[3]);

    // ---- each recorded cascade keeps the nearer of its own depth and its atlas cell's
    for (U32 c = 0; c < 4; ++c)
    {
        if (!(recorded & (1u << c)))
        {
            continue;
        }
        LLRenderTarget& shadow = mRT->shadow[c];
        shadow.bindTarget();
        {
            LLGLDisable cull(GL_CULL_FACE);
            LLGLDepthTest depth(GL_TRUE, GL_TRUE, GL_LESS);
            gGL.setColorMask(false, false);
            gFSShadowSPMergeProgram.bind();
            const S32 channel = gFSShadowSPMergeProgram.enableTexture(LLShaderMgr::DIFFUSE_MAP);
            if (channel >= 0)
            {
                gGL.getTexUnit(channel)->bind(&mFSShadowAtlas, true);
            }
            gFSShadowSPMergeProgram.uniform2f(sCell, (F32)mFSShadowSPCell[c][0], (F32)mFSShadowSPCell[c][1]);
            mScreenTriangleVB->setBuffer();
            mScreenTriangleVB->drawArrays(LLRender::TRIANGLES, 0, 3);
            if (channel >= 0)
            {
                gGL.getTexUnit(channel)->unbind(LLTexUnit::TT_TEXTURE);
            }
        }

        if (mFSShadowSPColored & (1u << c))
        {   // as renderShadow would have, with this cascade's matrices and render map
            LLGLEnable cull(GL_CULL_FACE);
            LLGLEnable clamp_depth(GL_DEPTH_CLAMP);
            set_current_modelview(mFSShadowSPView[c]);
            set_current_projection(mFSShadowSPProj[c]);
            gGL.matrixMode(LLRender::MM_PROJECTION);
            gGL.loadMatrix(glm::value_ptr(mFSShadowSPProj[c]));
            gGL.matrixMode(LLRender::MM_MODELVIEW);
            gGL.loadMatrix(glm::value_ptr(mFSShadowSPView[c]));
            gGLLastMatrix = NULL;
            // a later cascade's cull can rebuild or free the casters this cascade's render map listed
            std::vector<LLDrawInfo*> casters;
            for (const FSShadowSPEntry& e : mFSShadowSPColoredCasters[c])
            {
                if (e.mParams->mCount && e.mParams->mVertexBuffer.get() == e.mBuffer.get())
                {
                    casters.push_back(e.mParams.get());
                }
            }
            if (c < FS_COLORED_SHADOW_CASCADES && !casters.empty() && drawColoredShadowCasters(mColoredShadowTarget[c], casters.data(), casters.data() + casters.size()))
            {
                mColoredShadowFrame[c] = LLFrameTimer::getFrameCount();
            }
            gGL.setColorMask(false, false);
        }
        shadow.flush();
    }

    gDeferredShadowCubeProgram.bind();  // as renderShadow leaves it
    gGL.setColorMask(true, true);
    gGL.matrixMode(LLRender::MM_PROJECTION);
    gGL.popMatrix();
    gGL.matrixMode(LLRender::MM_MODELVIEW);
    gGL.popMatrix();
    gGLLastMatrix = NULL;
    set_current_modelview(saved_view);
    set_current_projection(saved_proj);
    LLPipeline::sShadowRender = saved_shadow_render;
    if (mFSShadowSPLastCull)
    {   // the cull the last cascade left current
        grabReferences(*mFSShadowSPLastCull);
    }

    for (auto& kind : mFSShadowSPEntries)
    {
        for (auto& list : kind)
        {
            list.clear();
        }
    }
    mFSShadowSPRecorded = 0;
    mFSShadowSPColored = 0;
    mFSShadowSPLastCull = nullptr;
    for (auto& held : mFSShadowSPColoredCasters)
    {
        held.clear();
    }

    if (batch_stats)
    {   // recorded counts each cascade a batch went to, as the per cascade passes would draw it
        static U64 total_recorded = 0;
        static U64 total_draws = 0;
        static U32 updates = 0;
        static LLFrameTimer timer;
        total_recorded += recorded_batches;
        total_draws += draws;
        ++updates;
        if (timer.getElapsedTimeF32() >= 10.f)
        {
            LL_INFOS("FSShadowBatches") << "Single pass sun shadows: " << total_recorded << " cascade batches in "
                                        << total_draws << " instanced draw calls over " << updates << " updates ("
                                        << (updates ? total_draws / updates : 0) << " draws per update)" << LL_ENDL;
            total_recorded = 0;
            total_draws = 0;
            updates = 0;
            timer.reset();
        }
    }
}
