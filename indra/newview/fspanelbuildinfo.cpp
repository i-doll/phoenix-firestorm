/**
 * @file fspanelbuildinfo.cpp
 * @brief Info tab of the build floater: selection cost, memory, detail level, identity and bounds
 *
 * $LicenseInfo:firstyear=2026&license=viewerlgpl$
 * Phoenix Firestorm Viewer Source Code
 * Copyright (C) 2026, Trish_sl
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
 * $/LicenseInfo$
 */

#include "llviewerprecompiledheaders.h"

#include "fspanelbuildinfo.h"

#include "llclipboard.h"
#include "lldate.h"
#include "lldrawable.h"
#include "llface.h"
#include "llfetchedgltfmaterial.h"
#include "llselectmgr.h"
#include "lltextbox.h"
#include "llviewercontrol.h"
#include "llviewerobject.h"
#include "llviewerregion.h"
#include "llviewertexture.h"
#include "llvovolume.h"

#include <set>

static LLPanelInjector<FSPanelBuildInfo> t_fs_panel_build_info("fs_panel_build_info");

static const F32 REFRESH_INTERVAL = 0.25f;

FSPanelBuildInfo::FSPanelBuildInfo()
:   LLPanel()
{
}

bool FSPanelBuildInfo::postBuild()
{
    mSelCounts = getChild<LLTextBox>("sel_counts");
    mSelCost = getChild<LLTextBox>("sel_cost");
    mSelRender = getChild<LLTextBox>("sel_render");
    mSelTextures = getChild<LLTextBox>("sel_textures");
    mSelMaterials = getChild<LLTextBox>("sel_materials");
    mSelLOD = getChild<LLTextBox>("sel_lod");
    mObjLink = getChild<LLTextBox>("obj_link");
    mObjIDs = getChild<LLTextBox>("obj_ids");
    mObjCreated = getChild<LLTextBox>("obj_created");
    mObjRegion = getChild<LLTextBox>("obj_region");
    mBoundsSize = getChild<LLTextBox>("bounds_size");
    mBoundsRegionSize = getChild<LLTextBox>("bounds_region_size");
    mBoundsCenter = getChild<LLTextBox>("bounds_center");

    getChild<LLUICtrl>("copy_uuid_btn")->setCommitCallback([this](LLUICtrl*, const LLSD&) { onCopyUUID(); });
    getChild<LLUICtrl>("copy_size_btn")->setCommitCallback([this](LLUICtrl*, const LLSD&) { onCopySize(); });
    getChild<LLUICtrl>("copy_center_btn")->setCommitCallback([this](LLUICtrl*, const LLSD&) { onCopyCenter(); });
    return true;
}

void FSPanelBuildInfo::draw()
{
    if (mRefreshTimer.getElapsedTimeF32() >= REFRESH_INTERVAL)
    {
        mRefreshTimer.reset();
        refreshInfo();
    }
    LLPanel::draw();
}

//static
bool FSPanelBuildInfo::getLinkNumber(LLViewerObject* objectp, S32& link_num, S32& link_count)
{
    link_num = 0;
    link_count = 0;
    LLViewerObject* root = objectp ? objectp->getRootEdit() : nullptr;
    if (!root)
    {
        return false;
    }
    const LLViewerObject::child_list_t& children = root->getChildren();
    link_count = (S32)children.size() + 1;
    if (children.empty())
    {
        link_num = 0; // a childless prim is always link zero
        link_count = 1;
        return true;
    }
    if (objectp == root)
    {
        link_num = 1;
        return true;
    }
    S32 index = 1;
    for (const LLPointer<LLViewerObject>& child : children)
    {
        ++index;
        if (child == objectp)
        {
            link_num = index;
            return true;
        }
    }
    return false;
}

void FSPanelBuildInfo::refreshInfo()
{
    refreshSelection();
    refreshObject();
    refreshBounds();
}

static std::string fs_megabytes(S64 bytes)
{
    return llformat("%.1f MB", (F64)bytes / (1024.0 * 1024.0));
}

void FSPanelBuildInfo::refreshSelection()
{
    LLSelectMgr* selmgr = LLSelectMgr::getInstance();
    LLObjectSelectionHandle selection = selmgr->getSelection();

    S32 faces = 0;
    S32 pbr_faces = 0;
    S32 legacy_mat_faces = 0;
    S32 triangles = 0;
    S32 lod_count[4] = { 0, 0, 0, 0 };
    std::set<LLViewerTexture*> textures;

    for (LLObjectSelection::iterator iter = selection->begin(); iter != selection->end(); ++iter)
    {
        LLViewerObject* object = (*iter)->getObject();
        if (!object || object->isDead())
        {
            continue;
        }

        const U8 num_tes = object->getNumTEs();
        faces += num_tes;
        for (U8 te = 0; te < num_tes; ++te)
        {
            if (LLViewerTexture* tex = object->getTEImage(te))
            {
                textures.insert(tex);
            }
            LLViewerTexture* normal = object->getTENormalMap(te);
            LLViewerTexture* specular = object->getTESpecularMap(te);
            if (normal)
            {
                textures.insert(normal);
            }
            if (specular)
            {
                textures.insert(specular);
            }

            const LLTextureEntry* tep = object->getTE(te);
            LLFetchedGLTFMaterial* gltf = tep ? dynamic_cast<LLFetchedGLTFMaterial*>(tep->getGLTFRenderMaterial()) : nullptr;
            if (gltf)
            {
                ++pbr_faces;
                for (LLViewerFetchedTexture* t : { gltf->mBaseColorTexture.get(), gltf->mNormalTexture.get(),
                                                   gltf->mMetallicRoughnessTexture.get(), gltf->mEmissiveTexture.get() })
                {
                    if (t)
                    {
                        textures.insert(t);
                    }
                }
            }
            else if (normal || specular)
            {
                ++legacy_mat_faces;
            }
        }

        if (LLVOVolume* volume = dynamic_cast<LLVOVolume*>(object))
        {
            lod_count[llclamp(volume->getLOD(), 0, 3)]++;
        }

        if (LLDrawable* drawable = object->mDrawable)
        {
            for (S32 i = 0; i < drawable->getNumFaces(); ++i)
            {
                if (LLFace* face = drawable->getFace(i))
                {
                    triangles += (S32)(face->getIndicesCount() / 3);
                }
            }
        }
    }

    S64 loaded_bytes = 0;
    S64 full_bytes = 0;
    for (LLViewerTexture* tex : textures)
    {
        if (LLImageGL* image = tex->getGLTexture())
        {
            if (tex->hasGLTexture())
            {
                loaded_bytes += image->getMipBytes(-1);
            }
            full_bytes += image->getMipBytes(0);
        }
    }

    LLStringUtil::format_map_t args;
    args["[OBJECTS]"] = llformat("%d", selection->getRootObjectCount());
    args["[PRIMS]"] = llformat("%d", selection->getObjectCount());
    args["[FACES]"] = llformat("%d", faces);
    args["[TRIS]"] = llformat("%d", triangles);
    mSelCounts->setText(getString("sel_counts_text", args));

    args.clear();
    args["[LI]"] = llformat("%.2f", selection->getSelectedLinksetCost());
    args["[PHYSICS]"] = llformat("%.2f", selection->getSelectedLinksetPhysicsCost());
    args["[DOWNLOAD]"] = llformat("%.2f", selection->getSelectedObjectStreamingCost());
    mSelCost->setText(getString("sel_cost_text", args));

    args.clear();
    args["[COMPLEXITY]"] = llformat("%d", selection->getSelectedObjectRenderCost());
    mSelRender->setText(getString("sel_render_text", args));

    args.clear();
    args["[COUNT]"] = llformat("%d", (S32)textures.size());
    args["[LOADED]"] = fs_megabytes(loaded_bytes);
    args["[FULL]"] = fs_megabytes(full_bytes);
    mSelTextures->setText(getString("sel_textures_text", args));

    args.clear();
    args["[PBR]"] = llformat("%d", pbr_faces);
    args["[LEGACY]"] = llformat("%d", legacy_mat_faces);
    mSelMaterials->setText(getString("sel_materials_text", args));

    args.clear();
    args["[HIGH]"] = llformat("%d", lod_count[3]);
    args["[MEDIUM]"] = llformat("%d", lod_count[2]);
    args["[LOW]"] = llformat("%d", lod_count[1]);
    args["[LOWEST]"] = llformat("%d", lod_count[0]);
    mSelLOD->setText(getString("sel_lod_text", args));
}

void FSPanelBuildInfo::refreshObject()
{
    LLObjectSelectionHandle selection = LLSelectMgr::getInstance()->getSelection();
    static LLCachedControl<bool> edit_linked(gSavedSettings, "EditLinkedParts", false);
    LLViewerObject* object = edit_linked ? selection->getFirstObject() : selection->getFirstRootObject(true);
    LLSelectNode* node = object ? selection->findNode(object) : nullptr;

    if (!object)
    {
        mObjectID.setNull();
        mObjLink->setText(LLStringUtil::null);
        mObjIDs->setText(getString("none_selected"));
        mObjCreated->setText(LLStringUtil::null);
        mObjRegion->setText(LLStringUtil::null);
        getChildView("copy_uuid_btn")->setEnabled(false);
        return;
    }

    mObjectID = object->getID();
    getChildView("copy_uuid_btn")->setEnabled(true);

    S32 link_num = 0;
    S32 link_count = 0;
    LLStringUtil::format_map_t args;
    if (getLinkNumber(object, link_num, link_count))
    {
        args["[LINK]"] = llformat("%d", link_num);
        args["[COUNT]"] = llformat("%d", link_count);
        mObjLink->setText(getString("obj_link_text", args));
    }
    else
    {
        mObjLink->setText(LLStringUtil::null);
    }

    args.clear();
    args["[LOCALID]"] = llformat("%u", object->getLocalID());
    args["[UUID]"] = mObjectID.asString();
    mObjIDs->setText(getString("obj_ids_text", args));

    std::string created;
    if (node && node->mValid && node->mCreationDate != 0)
    {
        // The simulator sends microseconds since the epoch
        F64 seconds = (F64)node->mCreationDate;
        if (seconds > 1.0e12)
        {
            seconds /= 1.0e6;
        }
        created = LLDate(seconds).toHTTPDateString(std::string("%Y-%m-%d %H:%M UTC"));
    }
    args.clear();
    args["[DATE]"] = created.empty() ? getString("unknown") : created;
    mObjCreated->setText(getString("obj_created_text", args));

    args.clear();
    args["[REGION]"] = object->getRegion() ? object->getRegion()->getName() : getString("unknown");
    mObjRegion->setText(getString("obj_region_text", args));
}

// Bounds of every selected linkset, from each prim's box. The main figure is measured along the
// root prim's axes (what builders usually want); the region aligned box is shown too.
void FSPanelBuildInfo::refreshBounds()
{
    LLObjectSelectionHandle selection = LLSelectMgr::getInstance()->getSelection();
    LLViewerObject* frame_root = selection->getFirstRootObject(true);
    frame_root = frame_root ? frame_root->getRootEdit() : nullptr;
    if (!frame_root)
    {
        mBoundsSize->setText(getString("none_selected"));
        mBoundsRegionSize->setText(LLStringUtil::null);
        mBoundsCenter->setText(LLStringUtil::null);
        getChildView("copy_size_btn")->setEnabled(false);
        getChildView("copy_center_btn")->setEnabled(false);
        return;
    }

    const LLVector3 frame_pos = frame_root->getPositionRegion();
    const LLQuaternion frame_rot = frame_root->getRotationRegion();
    const LLQuaternion frame_inv = ~frame_rot;

    LLVector3 local_min(F32_MAX, F32_MAX, F32_MAX), local_max(-F32_MAX, -F32_MAX, -F32_MAX);
    LLVector3 region_min = local_min, region_max = local_max;
    std::set<LLViewerObject*> roots;

    auto add_prim = [&](LLViewerObject* prim)
    {
        if (!prim || prim->isDead() || prim->isAvatar())
        {
            return;
        }
        const LLVector3 center = prim->getPositionRegion();
        const LLQuaternion rot = prim->getRotationRegion();
        const LLVector3 half = prim->getScale() * 0.5f;
        for (S32 i = 0; i < 8; ++i)
        {
            LLVector3 corner((i & 1) ? half.mV[VX] : -half.mV[VX],
                             (i & 2) ? half.mV[VY] : -half.mV[VY],
                             (i & 4) ? half.mV[VZ] : -half.mV[VZ]);
            const LLVector3 region_corner = center + corner * rot;
            update_min_max(region_min, region_max, region_corner);
            update_min_max(local_min, local_max, (region_corner - frame_pos) * frame_inv);
        }
    };

    for (LLObjectSelection::root_iterator iter = selection->root_begin(); iter != selection->root_end(); ++iter)
    {
        LLViewerObject* object = (*iter)->getObject();
        LLViewerObject* root = object ? object->getRootEdit() : nullptr;
        if (!root || !roots.insert(root).second)
        {
            continue;
        }
        add_prim(root);
        for (const LLPointer<LLViewerObject>& child : root->getChildren())
        {
            add_prim(child);
        }
    }

    if (local_min.mV[VX] > local_max.mV[VX])
    {
        mBoundsSize->setText(getString("none_selected"));
        return;
    }

    mBoundsSizeValue = local_max - local_min;
    mBoundsCenterValue = frame_pos + ((local_min + local_max) * 0.5f) * frame_rot;
    const LLVector3 region_size = region_max - region_min;

    LLStringUtil::format_map_t args;
    args["[X]"] = llformat("%.3f", mBoundsSizeValue.mV[VX]);
    args["[Y]"] = llformat("%.3f", mBoundsSizeValue.mV[VY]);
    args["[Z]"] = llformat("%.3f", mBoundsSizeValue.mV[VZ]);
    mBoundsSize->setText(getString("bounds_size_text", args));

    args["[X]"] = llformat("%.3f", region_size.mV[VX]);
    args["[Y]"] = llformat("%.3f", region_size.mV[VY]);
    args["[Z]"] = llformat("%.3f", region_size.mV[VZ]);
    mBoundsRegionSize->setText(getString("bounds_region_size_text", args));

    args["[X]"] = llformat("%.3f", mBoundsCenterValue.mV[VX]);
    args["[Y]"] = llformat("%.3f", mBoundsCenterValue.mV[VY]);
    args["[Z]"] = llformat("%.3f", mBoundsCenterValue.mV[VZ]);
    mBoundsCenter->setText(getString("bounds_center_text", args));

    getChildView("copy_size_btn")->setEnabled(true);
    getChildView("copy_center_btn")->setEnabled(true);
}

static void fs_copy_text(const std::string& text)
{
    LLWString wtext = utf8str_to_wstring(text);
    LLClipboard::instance().copyToClipboard(wtext, 0, (S32)wtext.size());
}

void FSPanelBuildInfo::onCopyUUID()
{
    if (mObjectID.notNull())
    {
        fs_copy_text(mObjectID.asString());
    }
}

void FSPanelBuildInfo::onCopySize()
{
    fs_copy_text(llformat("<%.3f, %.3f, %.3f>", mBoundsSizeValue.mV[VX], mBoundsSizeValue.mV[VY], mBoundsSizeValue.mV[VZ]));
}

void FSPanelBuildInfo::onCopyCenter()
{
    fs_copy_text(llformat("<%.3f, %.3f, %.3f>", mBoundsCenterValue.mV[VX], mBoundsCenterValue.mV[VY], mBoundsCenterValue.mV[VZ]));
}
