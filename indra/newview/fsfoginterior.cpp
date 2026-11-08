/**
 * @file fsfoginterior.cpp
 * @brief Keeps the height fog out of rooms around the camera (FSRenderHeightFogInteriors)
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

#include "llviewerprecompiledheaders.h"

#include "fsfoginterior.h"

#include "lldrawable.h"
#include "llenvironment.h"
#include "llface.h"
#include "llframetimer.h"
#include "llglslshader.h"
#include "llsmoothstep.h"
#include "llspatialpartition.h"
#include "lltextureentry.h"
#include "lltimer.h"
#include "llviewercamera.h"
#include "llviewercontrol.h"
#include "llviewerregion.h"
#include "llvolume.h"
#include "llvovolume.h"
#include "llworld.h"
#include "pipeline.h"

#include <algorithm>
#include <vector>

extern bool gCubeSnapshot;

namespace
{
    const LLStaticHashedString sInterior("fs_height_fog_interior");

    // Rays around the camera, agent frame (z up): one straight up, eight level ones every 45
    // degrees and four 45 degrees up between them. Fixed in the world, so turning the camera
    // does not change the result.
    constexpr S32 NUM_LEVEL = 8;
    constexpr S32 NUM_DIAGONAL = 4;
    constexpr S32 NUM_SUN = 3;  // toward the sun or moon, from the camera and 0.75 m to each side

    constexpr size_t MAX_CANDIDATES = 2048; // objects tested per cast, a guard for dense builds
    constexpr F32 MIN_OBJECT_SIZE = 0.4f;   // smaller static objects are clutter, not walls
    constexpr F32 SMOOTH_TIME = 0.2f;       // time constant: about 0.5 s to settle after a doorway
    constexpr F32 MIN_INTERVAL = 0.25f;     // casts per second at most 4
    constexpr F32 MAX_INTERVAL = 1.f;
    constexpr F32 MOVE_RECAST = 4.f;        // moved this far since the last cast: cast again sooner
    constexpr F32 JUMP_SNAP = 16.f;         // moved this far (teleport, camera jump): no smoothing

    struct Candidate
    {
        LLDrawable* mDrawable;
        LLVOVolume* mVolume;
        bool mHasBox;           // static drawables: agent space box to skip most rays early
        LLVector4a mCenter;
        LLVector4a mHalfSize;
    };

    struct Values
    {
        F32 mIndoor = 0.f;      // 0 outdoors, 1 in a closed room (strength applied)
        F32 mRoom = 0.f;        // room radius (m)
        F32 mShelter = 0.f;     // length of the sheltered stretch of air around the camera (m)
        F32 mAmbient = 0.f;     // ambient light the sheltered air does not get, 0-1
        F32 mSun = 0.f;         // sun or moon light it does not get, 0-1
    };

    struct State
    {
        Values mTarget;
        Values mSmoothed;
        LLVector3 mLastCastPos;
        LLTimer mCastTimer;
        LLTimer mFrameTimer;
        F32 mInterval = MIN_INTERVAL;
        U32 mLastFrame = 0;
        bool mHaveCast = false;
        bool mSnap = true;
    };

    State sState;

    bool box_overlaps(const LLVector4a& center, const LLVector4a& half_size, const LLVector3& pos, F32 radius)
    {
        const F32* c = center.getF32ptr();
        const F32* s = half_size.getF32ptr();
        for (S32 i = 0; i < 3; ++i)
        {
            if (fabsf(c[i] - pos.mV[i]) > s[i] + radius)
            {
                return false;
            }
        }
        return true;
    }

    void add_candidate(LLDrawable* drawable, bool in_bridge, const LLVector3& pos, F32 radius, std::vector<Candidate>& out)
    {
        if (!drawable || drawable->isDead() || !gPipeline.hasRenderType(drawable->getRenderType())
            || drawable->isState(LLDrawable::RIGGED))
        {
            return;
        }

        LLViewerObject* vobj = drawable->getVObj();
        if (!vobj || vobj->isDead() || vobj->getPCode() != LL_PCODE_VOLUME || vobj->isAvatar()
            || vobj->isAttachment() || vobj->isHUDAttachment() || vobj->isReflectionProbe())
        {
            return;
        }

        Candidate cand;
        cand.mDrawable = drawable;
        cand.mVolume = (LLVOVolume*)vobj;
        cand.mHasBox = !in_bridge && !drawable->isActive();
        if (cand.mHasBox)
        {
            // Static drawables keep agent space extents
            const LLVector4a* ext = drawable->getSpatialExtents();
            cand.mCenter.setAdd(ext[0], ext[1]);
            cand.mCenter.mul(0.5f);
            cand.mHalfSize.setSub(ext[1], ext[0]);
            cand.mHalfSize.mul(0.5f);
            const F32* hs = cand.mHalfSize.getF32ptr();
            if (2.f * llmax(hs[0], hs[1], hs[2]) < MIN_OBJECT_SIZE || !box_overlaps(cand.mCenter, cand.mHalfSize, pos, radius))
            {
                return;
            }
        }
        out.push_back(cand);
    }

    // Static drawables (and spatial bridges, the moving linksets) of a partition's octree within
    // radius of pos. Group bounds are in agent space in the region partitions; inside a bridge
    // they are in the bridge's own frame, so the whole bridge (culled by its agent space box) is
    // taken.
    void collect(const OctreeNode* node, bool in_bridge, const LLVector3& pos, F32 radius, std::vector<Candidate>& out)
    {
        if (!node || out.size() >= MAX_CANDIDATES)
        {
            return;
        }

        if (!in_bridge)
        {
            const LLSpatialGroup* group = (const LLSpatialGroup*)node->getListener(0);
            if (group)
            {
                const LLVector4a* bounds = group->getBounds();
                if (!box_overlaps(bounds[0], bounds[1], pos, radius))
                {
                    return;
                }
            }
        }

        for (OctreeNode::const_element_iter it = node->getDataBegin(); it != node->getDataEnd() && out.size() < MAX_CANDIDATES; ++it)
        {
            LLViewerOctreeEntry* entry = *it;
            LLDrawable* drawable = entry ? (LLDrawable*)entry->getDrawable() : nullptr;
            if (!drawable || drawable->isDead())
            {
                continue;
            }

            if (drawable->isSpatialBridge())
            {
                LLSpatialPartition* part = drawable->asPartition();
                LLSpatialBridge* bridge = part ? part->asBridge() : nullptr;
                if (!bridge || !gPipeline.hasRenderType(bridge->mDrawableType) || bridge->mDrawableType == LLPipeline::RENDER_TYPE_AVATAR
                    || bridge->mDrawableType == LLPipeline::RENDER_TYPE_CONTROL_AV)
                {
                    continue;
                }
                const LLVector4a* ext = drawable->getSpatialExtents();
                LLVector4a center, half_size;
                center.setAdd(ext[0], ext[1]);
                center.mul(0.5f);
                half_size.setSub(ext[1], ext[0]);
                half_size.mul(0.5f);
                if (box_overlaps(center, half_size, pos, radius))
                {
                    collect(part->mOctree, true, pos, radius, out);
                }
            }
            else
            {
                add_candidate(drawable, in_bridge, pos, radius, out);
            }
        }

        for (U32 i = 0; i < node->getChildCount(); ++i)
        {
            collect(node->getChild(i), in_bridge, pos, radius, out);
        }
    }

    // Distance from start to the nearest opaque face on the way to end, or a negative value if
    // there is none. Faces drawn by the alpha pools (glass, blended textures) and faces with any
    // transparency in their colour let the ray through.
    F32 cast_ray(const std::vector<Candidate>& cands, const LLVector3& start, const LLVector3& end)
    {
        LLVector3 cur_end = end;
        bool hit = false;

        LLVector4a start4a;
        start4a.load3(start.mV);

        for (const Candidate& cand : cands)
        {
            if (cand.mHasBox)
            {
                LLVector4a end4a;
                end4a.load3(cur_end.mV);
                if (!LLLineSegmentBoxIntersect(start4a, end4a, cand.mCenter, cand.mHalfSize))
                {
                    continue;
                }
            }

            LLVOVolume* vovol = cand.mVolume;
            LLVolume* volume = vovol->getVolume();
            if (!volume)
            {
                continue;
            }

            LLVector4a local_start, local_end;
            local_start.load3(vovol->agentPositionToVolume(start).mV);
            local_end.load3(vovol->agentPositionToVolume(cur_end).mV);

            bool obj_hit = false;
            const S32 num_faces = llmin(volume->getNumVolumeFaces(), (S32)cand.mDrawable->getNumFaces());
            for (S32 i = 0; i < num_faces; ++i)
            {
                const LLFace* face = cand.mDrawable->getFace(i);
                const LLTextureEntry* te = vovol->getTE(i);
                if (!face || !te || face->isInAlphaPool() || te->getColor().mV[VALPHA] < 0.999f)
                {
                    continue;
                }

                LLVector4a p;
                if (volume->lineSegmentIntersect(local_start, local_end, i, &p) >= 0)
                {
                    local_end = p; // only nearer hits from here on
                    obj_hit = true;
                }
            }

            if (obj_hit)
            {
                cur_end = vovol->volumePositionToAgent(LLVector3(local_end.getF32ptr()));
                hit = true;
            }
        }

        return hit ? (cur_end - start).length() : -1.f;
    }

    // Casts every ray from pos and turns the hits into the target values
    void cast(const LLVector3& pos, F32 range, F32 strength)
    {
        LL_PROFILE_ZONE_SCOPED_CATEGORY_PIPELINE;

        LLTimer cost;

        static std::vector<Candidate> cands;
        cands.clear();
        // The sun rays reach twice as far: a roof's shadow can come from a high eave
        const F32 radius = range * 2.f;
        for (LLViewerRegion* region : LLWorld::getInstance()->getRegionList())
        {
            for (U32 j : { (U32)LLViewerRegion::PARTITION_VOLUME, (U32)LLViewerRegion::PARTITION_BRIDGE })
            {
                LLSpatialPartition* part = region->getSpatialPartition(j);
                if (part && gPipeline.hasRenderType(part->mDrawableType))
                {
                    collect(part->mOctree, false, pos, radius, cands);
                }
            }
        }

        Values target;

        if (!cands.empty())
        {
            // Straight up
            const F32 up = cast_ray(cands, pos, pos + LLVector3(0.f, 0.f, range));

            // Level and 45 degrees up
            F32 level[NUM_LEVEL];
            S32 level_hits = 0;
            F32 hit_sum = 0.f;
            S32 hit_count = 0;
            for (S32 i = 0; i < NUM_LEVEL; ++i)
            {
                const F32 a = F_TWO_PI * (F32)i / (F32)NUM_LEVEL;
                level[i] = cast_ray(cands, pos, pos + LLVector3(cosf(a), sinf(a), 0.f) * range);
                if (level[i] >= 0.f)
                {
                    ++level_hits;
                    hit_sum += level[i];
                    ++hit_count;
                }
            }
            S32 diag_hits = 0;
            for (S32 i = 0; i < NUM_DIAGONAL; ++i)
            {
                const F32 a = F_TWO_PI * ((F32)i + 0.5f) / (F32)NUM_DIAGONAL;
                LLVector3 dir(cosf(a), sinf(a), 1.f);
                dir.normalize();
                const F32 d = cast_ray(cands, pos, pos + dir * range);
                if (d >= 0.f)
                {
                    ++diag_hits;
                    hit_sum += d;
                    ++hit_count;
                }
            }

            // A ceiling: the up ray hits within 20 m, or most of the rays 45 degrees up do
            // (a skylight or an atrium straight overhead)
            const F32 ceil_max = llmin(range, 20.f);
            F32 ceiling = (up >= 0.f) ? 1.f - llsmoothstep(0.6f * ceil_max, ceil_max, up) : 0.f;
            ceiling = llmax(ceiling, llsmoothstep(0.5f, 1.f, (F32)diag_hits / (F32)NUM_DIAGONAL));

            // Walls: most of the level rays hit something
            const F32 walls = llsmoothstep(0.4f, 0.85f, (F32)level_hits / (F32)NUM_LEVEL);

            // Room radius: median level distance, misses at the range (a door or a window does
            // not shrink it)
            F32 sorted[NUM_LEVEL];
            for (S32 i = 0; i < NUM_LEVEL; ++i)
            {
                sorted[i] = (level[i] >= 0.f) ? level[i] : range;
            }
            std::sort(sorted, sorted + NUM_LEVEL);
            const F32 room = 0.5f * (sorted[NUM_LEVEL / 2 - 1] + sorted[NUM_LEVEL / 2]);

            // Sky seen from the camera: up counts twice, the 45 degree rays once, level rays a
            // quarter (the sky's light comes from above)
            const F32 sky_seen = (2.f * (up < 0.f ? 1.f : 0.f) + (F32)(NUM_DIAGONAL - diag_hits)
                                  + 0.25f * (F32)(NUM_LEVEL - level_hits))
                                 / (2.f + (F32)NUM_DIAGONAL + 0.25f * (F32)NUM_LEVEL);

            // The sheltered air: as far as the walls and posts around, or under a roof with
            // nothing around (a porch) about three times its height
            F32 shelter = 0.f;
            if (hit_count > 0)
            {
                shelter = hit_sum / (F32)hit_count;
            }
            else if (up >= 0.f)
            {
                shelter = 3.f * up;
            }

            // Sun or moon, whichever lights the fog (the shader's sun_up_factor)
            LLEnvironment& environment = LLEnvironment::instance();
            LLVector3 light = environment.getIsSunUp() ? environment.getSunDirection() : environment.getMoonDirection();
            light.normalize();
            LLVector3 side = light % LLVector3::z_axis;
            if (side.normalize() < 1e-3f)
            {
                side = LLVector3::x_axis;
            }
            S32 sun_hits = 0;
            for (S32 i = 0; i < NUM_SUN; ++i)
            {
                const LLVector3 start = pos + side * (0.75f * (F32)(i - 1));
                if (cast_ray(cands, start, start + light * radius) >= 0.f)
                {
                    ++sun_hits;
                }
            }

            target.mIndoor = llclamp(ceiling * walls * strength, 0.f, 1.f);
            target.mRoom = llclamp(room, 0.f, range);
            target.mShelter = llclamp(shelter, 0.f, range);
            target.mAmbient = llclamp((1.f - sky_seen) * strength, 0.f, 0.9f);
            target.mSun = llclamp((F32)sun_hits / (F32)NUM_SUN * strength, 0.f, 1.f);
        }

        sState.mTarget = target;
        sState.mLastCastPos = pos;
        sState.mCastTimer.reset();
        sState.mHaveCast = true;

        // Budget: a cast that took t seconds is repeated no sooner than 100 t later, so the
        // casts take at most about 1% of the frame time (and run at most 4 times a second)
        sState.mInterval = llclamp(cost.getElapsedTimeF32() * 100.f, MIN_INTERVAL, MAX_INTERVAL);
    }

    void update_frame()
    {
        const U32 frame = LLFrameTimer::getFrameCount();
        if (frame == sState.mLastFrame && sState.mHaveCast)
        {
            return;
        }
        sState.mLastFrame = frame;

        static LLCachedControl<F32> range_setting(gSavedSettings, "FSRenderHeightFogInteriorRange", 30.f);
        static LLCachedControl<F32> strength_setting(gSavedSettings, "FSRenderHeightFogInteriorStrength", 1.f);
        const F32 range = llclamp((F32)range_setting, 5.f, 64.f);
        const F32 strength = llclamp((F32)strength_setting, 0.f, 1.f);

        // A long gap since the last update (the feature was off, or the main view was not
        // rendered) snaps to the new values instead of fading
        F32 dt = sState.mFrameTimer.getElapsedTimeAndResetF32();
        if (dt > 1.f)
        {
            sState.mSnap = true;
        }

        const LLVector3 pos = LLViewerCamera::getInstance()->getOrigin();
        const F32 moved = (pos - sState.mLastCastPos).length();
        const F32 since = sState.mCastTimer.getElapsedTimeF32();
        if (moved > JUMP_SNAP)
        {
            sState.mSnap = true;
        }

        // Moving recasts sooner, but still no more often than half the cost-based interval
        if (!sState.mHaveCast || sState.mSnap || since >= sState.mInterval ||
            (moved > MOVE_RECAST && since >= llmax(0.1f, 0.5f * sState.mInterval)))
        {
            cast(pos, range, strength);
        }

        const F32 k = sState.mSnap ? 1.f : 1.f - expf(-llmax(dt, 0.f) / SMOOTH_TIME);
        sState.mSnap = false;

        Values& s = sState.mSmoothed;
        const Values& t = sState.mTarget;
        s.mIndoor += (t.mIndoor - s.mIndoor) * k;
        s.mRoom += (t.mRoom - s.mRoom) * k;
        s.mShelter += (t.mShelter - s.mShelter) * k;
        s.mAmbient += (t.mAmbient - s.mAmbient) * k;
        s.mSun += (t.mSun - s.mSun) * k;
    }

    bool rendering_main_view()
    {
        return !gCubeSnapshot && !LLPipeline::sShadowRender && !LLPipeline::sReflectionRender
            && !LLPipeline::sImpostorRender && !LLPipeline::sRenderingHUDs;
    }
}

void FSFogInterior::uploadUniforms(LLGLSLShader& shader)
{
    if (!LLGLSLShader::sGlobalDefines.count("FS_HEIGHT_FOG"))
    {
        return;
    }

    static LLCachedControl<bool> enabled(gSavedSettings, "FSRenderHeightFogInteriors", false);
    if (!enabled || !rendering_main_view())
    {
        // The plain fog: zero start, no sheltered stretch
        shader.uniform4f(sInterior, 0.f, 0.f, 0.f, 0.f);
        if (!enabled)
        {
            sState.mHaveCast = false; // snap to fresh values when turned back on
            sState.mSnap = true;
        }
        return;
    }

    update_frame();

    const Values& s = sState.mSmoothed;
    const F32 start = s.mIndoor * s.mRoom;
    // The sheltered stretch only matters while some light is kept out of it
    const F32 shelter_end = (s.mAmbient > 0.001f || s.mSun > 0.001f) ? llmax(s.mShelter, start) : start;
    shader.uniform4f(sInterior, start, shelter_end, s.mAmbient, s.mSun);
}
