/**
 * @file fspanelbuildinfo.h
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

#ifndef FS_PANELBUILDINFO_H
#define FS_PANELBUILDINFO_H

#include "llpanel.h"
#include "lltimer.h"

class LLTextBox;
class LLViewerObject;

class FSPanelBuildInfo : public LLPanel
{
public:
    FSPanelBuildInfo();

    bool postBuild() override;
    void draw() override;

    // Link number as scripts count it: 0 unlinked, 1 root, then the children in order.
    // link_count is the number of prims in the linkset.
    static bool getLinkNumber(LLViewerObject* objectp, S32& link_num, S32& link_count);

private:
    void refreshInfo();
    void refreshSelection();
    void refreshObject();
    void refreshBounds();

    void onCopyUUID();
    void onCopySize();
    void onCopyCenter();

    LLTextBox* mSelCounts = nullptr;
    LLTextBox* mSelCost = nullptr;
    LLTextBox* mSelRender = nullptr;
    LLTextBox* mSelTextures = nullptr;
    LLTextBox* mSelMaterials = nullptr;
    LLTextBox* mSelLOD = nullptr;
    LLTextBox* mObjLink = nullptr;
    LLTextBox* mObjIDs = nullptr;
    LLTextBox* mObjCreated = nullptr;
    LLTextBox* mObjRegion = nullptr;
    LLTextBox* mBoundsSize = nullptr;
    LLTextBox* mBoundsRegionSize = nullptr;
    LLTextBox* mBoundsCenter = nullptr;

    LLUUID      mObjectID;
    LLVector3   mBoundsSizeValue;
    LLVector3   mBoundsCenterValue;
    LLFrameTimer mRefreshTimer;
};

#endif // FS_PANELBUILDINFO_H
