/**
 * @file fsparcelderender.cpp
 * @brief Derenders every object on a parcel, leaving the terrain
 *
 * $LicenseInfo:firstyear=2026&license=fsviewerlgpl$
 * Phoenix Firestorm Viewer Source Code
 * Copyright (C) 2026, Trish
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

#include "fsparcelderender.h"

#include "llcallbacklist.h"
#include "llparcel.h"
#include "llselectmgr.h"
#include "lltoolmgr.h"
#include "lltoolpie.h"
#include "llviewerobjectlist.h"
#include "llviewerregion.h"
#include "llworld.h"

// Moving objects and stood up sitters are caught by this, the rest when they are created
constexpr F32 SWEEP_INTERVAL_SECONDS = 3.f;
// How long a derender from the menu waits for the parcel's shape to arrive
constexpr F32 PENDING_TIMEOUT_SECONDS = 10.f;

bool FSParcelDerender::sHasParcels = false;
bool FSParcelDerender::sHasSuppressed = false;

// Objects from the sim. The viewer's own, such as water, terrain patches and the sky, are in the object list too.
static bool is_sim_object(const LLViewerObject* objectp)
{
    switch (objectp->getPCode())
    {
        case LL_PCODE_VOLUME:
        case LL_PCODE_LEGACY_GRASS:
        case LL_PCODE_LEGACY_TREE:
        case LL_PCODE_TREE_NEW:
            return true;
        default:
            return false;
    }
}

bool FSParcelDerender::Parcel::contains(const LLVector3& pos_region, F32 region_width) const
{
    if (mGridSize <= 0 || pos_region.mV[VX] < 0.f || pos_region.mV[VY] < 0.f
        || pos_region.mV[VX] >= region_width || pos_region.mV[VY] >= region_width)
    {
        return false;
    }

    const F32 cell_size = region_width / mGridSize;
    const S32 x = llmin((S32)(pos_region.mV[VX] / cell_size), mGridSize - 1);
    const S32 y = llmin((S32)(pos_region.mV[VY] / cell_size), mGridSize - 1);
    const S32 cell = x + y * mGridSize;
    return (mBitmap[cell / 8] & (1 << (cell % 8))) != 0;
}

LLSD FSParcelDerender::Parcel::toLLSD() const
{
    LLSD data;
    data["region_id"] = mRegionID;
    data["local_id"] = mLocalID;
    data["bitmap"] = LLSD::Binary(mBitmap.begin(), mBitmap.end());
    return data;
}

// static
bool FSParcelDerender::Parcel::fromLLSD(const LLSD& data, Parcel& parcel)
{
    if (!data.isMap() || !data.has("bitmap"))
    {
        return false;
    }

    const LLSD::Binary& bitmap = data["bitmap"].asBinary();
    const S32 grid_size = gridSizeFromBitmap((S32)bitmap.size());
    if (grid_size <= 0)
    {
        return false;
    }

    parcel.mRegionID = data["region_id"].asUUID();
    parcel.mLocalID = data["local_id"].asInteger();
    parcel.mGridSize = grid_size;
    parcel.mBitmap.assign(bitmap.begin(), bitmap.end());
    return parcel.mRegionID.notNull();
}

FSParcelDerender::FSParcelDerender()
{
    gIdleCallbacks.addFunction(onIdle, this);
}

FSParcelDerender::~FSParcelDerender()
{
    gIdleCallbacks.deleteFunction(onIdle, this);
}

void FSParcelDerender::init()
{
    mParcels.clear();
    mParcelsByRegion.clear();
    sHasParcels = false;

    for (const auto& [id, data] : FSAssetBlacklist::instance().getBlacklistData())
    {
        Parcel parcel;
        if (data.type == FS_AT_PARCEL && Parcel::fromLLSD(data.extra, parcel))
        {
            addParcel(id, parcel);
        }
    }

    mBlacklistConnection = FSAssetBlacklist::instance().setBlacklistChangedCallback(
        [this](const FSAssetBlacklist::changed_signal_data_t& data, FSAssetBlacklist::eBlacklistOperation op) { onBlacklistChanged(data, op); });
}

// static
LLUUID FSParcelDerender::makeParcelID(const LLUUID& region_id, S32 local_id)
{
    // Stable for as long as the parcel isn't joined or subdivided, local ids then change anyway
    LLUUID id;
    id.generate(region_id.asString() + llformat(":parcel:%d", local_id));
    return id;
}

// static
S32 FSParcelDerender::gridSizeFromBitmap(S32 bitmap_size)
{
    const S32 cells = bitmap_size * 8;
    const S32 grid_size = ll_round(sqrtf((F32)cells));
    return (bitmap_size > 0 && grid_size * grid_size == cells) ? grid_size : 0;
}

bool FSParcelDerender::canDerenderSelectedParcel() const
{
    const LLPickInfo& pick = LLToolPie::getInstance()->getPick();
    return pick.mPickType == LLPickInfo::PICK_LAND && LLWorld::getInstance()->getRegionFromPosGlobal(pick.mPosGlobal);
}

void FSParcelDerender::derenderSelectedParcel(bool permanent)
{
    const LLPickInfo& pick = LLToolPie::getInstance()->getPick();
    LLViewerRegion* regionp = LLWorld::getInstance()->getRegionFromPosGlobal(pick.mPosGlobal);
    if (pick.mPickType != LLPickInfo::PICK_LAND || !regionp)
    {
        return;
    }

    // Right-clicking land selects the parcel, its shape arrives with the reply. Use it if it already
    // came and is the parcel that was clicked, otherwise wait for it.
    if (mSelectedRegionID == regionp->getRegionID() && !mSelectedBitmap.empty())
    {
        Parcel selected;
        selected.mGridSize = gridSizeFromBitmap((S32)mSelectedBitmap.size());
        selected.mBitmap = mSelectedBitmap;
        if (selected.contains(regionp->getPosRegionFromGlobal(pick.mPosGlobal), regionp->getWidth()))
        {
            derenderParcel(mSelectedRegionID, mSelectedLocalID, mSelectedName, mSelectedBitmap, permanent);
            mPending = false;
            return;
        }
    }

    mPending = true;
    mPendingPermanent = permanent;
    mPendingPosGlobal = pick.mPosGlobal;
    mPendingTimer.reset();
}

void FSParcelDerender::onSelectedParcelProperties(LLViewerRegion* region, const LLParcel* parcel, const U8* bitmap, S32 bitmap_size)
{
    if (!region || !parcel || !bitmap || gridSizeFromBitmap(bitmap_size) <= 0)
    {
        return;
    }

    mSelectedRegionID = region->getRegionID();
    mSelectedLocalID = parcel->getLocalID();
    mSelectedName = parcel->getName();
    mSelectedBitmap.assign(bitmap, bitmap + bitmap_size);

    // Selecting a derendered parcel again picks up changes to its shape
    const LLUUID id = makeParcelID(mSelectedRegionID, mSelectedLocalID);
    if (auto it = mParcels.find(id); it != mParcels.end() && it->second.mBitmap != mSelectedBitmap)
    {
        blacklist_data_t blacklist = FSAssetBlacklist::instance().getBlacklistData();
        if (auto entry = blacklist.find(id); entry != blacklist.end())
        {
            Parcel updated = it->second;
            updated.mGridSize = gridSizeFromBitmap(bitmap_size);
            updated.mBitmap = mSelectedBitmap;
            entry->second.extra = updated.toLLSD();
            FSAssetBlacklist::instance().addNewItemToBlacklistData(id, entry->second, entry->second.permanent);
        }
    }

    if (mPending)
    {
        Parcel selected;
        selected.mGridSize = gridSizeFromBitmap(bitmap_size);
        selected.mBitmap = mSelectedBitmap;
        if (LLWorld::getInstance()->getRegionFromPosGlobal(mPendingPosGlobal) == region
            && selected.contains(region->getPosRegionFromGlobal(mPendingPosGlobal), region->getWidth()))
        {
            mPending = false;
            derenderParcel(mSelectedRegionID, mSelectedLocalID, mSelectedName, mSelectedBitmap, mPendingPermanent);
        }
    }
}

void FSParcelDerender::derenderParcel(const LLUUID& region_id, S32 local_id, const std::string& name, const std::vector<U8>& bitmap, bool permanent)
{
    Parcel parcel;
    parcel.mRegionID = region_id;
    parcel.mLocalID = local_id;
    parcel.mGridSize = gridSizeFromBitmap((S32)bitmap.size());
    parcel.mBitmap = bitmap;

    FSAssetBlacklistData data;
    data.name = name;
    LLViewerRegion* regionp = LLWorld::getInstance()->getRegionFromID(region_id);
    data.region = regionp ? regionp->getName() : std::string();
    data.type = FS_AT_PARCEL;
    data.date = LLDate((double)time_corrected());
    data.permanent = permanent;
    data.extra = parcel.toLLSD();

    // The blacklist's change signal adds it here
    FSAssetBlacklist::instance().addNewItemToBlacklistData(makeParcelID(region_id, local_id), data, permanent);
}

void FSParcelDerender::onBlacklistChanged(const FSAssetBlacklist::changed_signal_data_t& data, FSAssetBlacklist::eBlacklistOperation op)
{
    for (const auto& [id, entry] : data)
    {
        if (op == FSAssetBlacklist::eBlacklistOperation::BLACKLIST_ADD)
        {
            Parcel parcel;
            if (entry && entry->type == FS_AT_PARCEL && Parcel::fromLLSD(entry->extra, parcel))
            {
                addParcel(id, parcel);
            }
        }
        else if (mParcels.contains(id))
        {
            removeParcel(id);
        }
    }
}

void FSParcelDerender::addParcel(const LLUUID& id, const Parcel& parcel)
{
    if (auto it = mParcels.find(id); it != mParcels.end())
    {
        it->second = parcel;
    }
    else
    {
        mParcels.emplace(id, parcel);
        mParcelsByRegion[parcel.mRegionID].push_back(id);
    }
    sHasParcels = true;
    mSweepNow = true;
}

void FSParcelDerender::removeParcel(const LLUUID& id)
{
    auto it = mParcels.find(id);
    if (it == mParcels.end())
    {
        return;
    }

    if (auto region_it = mParcelsByRegion.find(it->second.mRegionID); region_it != mParcelsByRegion.end())
    {
        std::erase(region_it->second, id);
        if (region_it->second.empty())
        {
            mParcelsByRegion.erase(region_it);
        }
    }
    mParcels.erase(it);
    sHasParcels = !mParcels.empty();

    restore([&id](const SuppressedObject& suppressed) { return suppressed.mParcelID == id; });
}

void FSParcelDerender::restore(const std::function<bool(const SuppressedObject&)>& match)
{
    // Their cache entries were dropped, so ask the regions for full updates
    std::unordered_set<LLViewerRegion*> regions;
    for (auto it = mSuppressed.begin(); it != mSuppressed.end();)
    {
        if (!match(it->second))
        {
            ++it;
            continue;
        }

        if (LLViewerRegion* regionp = LLWorld::getInstance()->getRegionFromHandle(it->second.mRegionHandle))
        {
            regionp->addCacheMissFull(it->second.mLocalID);
            regions.insert(regionp);
        }
        it = mSuppressed.erase(it);
    }
    sHasSuppressed = !mSuppressed.empty();

    for (LLViewerRegion* regionp : regions)
    {
        regionp->requestCacheMisses();
    }
}

void FSParcelDerender::onAvatarOrphaned(LLViewerRegion* regionp, U32 parent_local_id)
{
    if (!regionp)
    {
        return;
    }

    // Usually a vehicle that was derendered before its driver's update arrived
    const U64 region_handle = regionp->getHandle();
    for (const auto& [id, suppressed] : mSuppressed)
    {
        if (suppressed.mRegionHandle == region_handle && suppressed.mLocalID == parent_local_id)
        {
            constexpr F64 KEEP_SECONDS = 10.0;
            const LLUUID root_id = suppressed.mRootID;
            mKeepUntil[root_id] = LLFrameTimer::getTotalSeconds() + KEEP_SECONDS;
            restore([&root_id](const SuppressedObject& other) { return other.mRootID == root_id; });
            return;
        }
    }
}

const FSParcelDerender::Parcel* FSParcelDerender::findParcel(LLViewerRegion* regionp, const LLVector3& pos_region, LLUUID* parcel_id) const
{
    auto region_it = mParcelsByRegion.find(regionp->getRegionID());
    if (region_it == mParcelsByRegion.end())
    {
        return nullptr;
    }

    for (const LLUUID& id : region_it->second)
    {
        if (auto it = mParcels.find(id); it != mParcels.end() && it->second.contains(pos_region, regionp->getWidth()))
        {
            if (parcel_id)
            {
                *parcel_id = id;
            }
            return &it->second;
        }
    }
    return nullptr;
}

void FSParcelDerender::onObjectCreated(LLViewerObject* objectp)
{
    // Positions and parents are only reliable once the update is processed, check on the next frame
    LLViewerRegion* regionp = objectp ? objectp->getRegion() : nullptr;
    if (regionp && is_sim_object(objectp) && mParcelsByRegion.contains(regionp->getRegionID()))
    {
        mCreatedObjects.push_back(objectp->getID());
    }
}

bool FSParcelDerender::derenderIfOnParcel(LLViewerObject* objectp)
{
    // Roots only, children go with them. Orphans have no position of their own yet.
    if (!objectp || objectp->isDead() || !is_sim_object(objectp) || objectp->getParent() || objectp->isOrphaned())
    {
        return false;
    }

    LLViewerRegion* regionp = objectp->getRegion();
    LLUUID parcel_id;
    if (!regionp || !findParcel(regionp, objectp->getPositionRegion(), &parcel_id))
    {
        return false;
    }

    // Restored for a sitter whose update came first, give the linkset time to come back together
    if (auto keep = mKeepUntil.find(objectp->getID()); keep != mKeepUntil.end())
    {
        if (LLFrameTimer::getTotalSeconds() < keep->second)
        {
            return false;
        }
        mKeepUntil.erase(keep);
    }

    // Anything an avatar sits on stays, a vehicle someone drives or a chair someone uses
    for (LLViewerObject* child : objectp->getChildren())
    {
        if (child->isAvatar())
        {
            return false;
        }
        for (LLViewerObject* grandchild : child->getChildren())
        {
            if (grandchild->isAvatar())
            {
                return false;
            }
        }
    }

    // Ignore the updates of the whole linkset from now on
    const U64 region_handle = regionp->getHandle();
    const LLUUID& root_id = objectp->getID();
    mSuppressed[root_id] = { parcel_id, root_id, region_handle, objectp->getLocalID() };
    for (LLViewerObject* child : objectp->getChildren())
    {
        mSuppressed[child->getID()] = { parcel_id, root_id, region_handle, child->getLocalID() };
    }
    sHasSuppressed = true;

    LLSelectMgr::getInstance()->removeObjectFromSelections(objectp->getID());
    LLTool* tool = LLToolMgr::getInstance()->getCurrentTool();
    LLViewerObject* tool_editing_object = tool ? tool->getEditingObject() : nullptr;
    if (tool_editing_object && tool_editing_object->getRootEdit() == objectp)
    {
        tool->stopEditing();
    }

    const U32 local_id = objectp->getLocalID();
    gObjectList.killObject(objectp);
    if (LLViewerRegion::sVOCacheCullingEnabled)
    {
        regionp->killCacheEntry(local_id);
    }
    return true;
}

void FSParcelDerender::sweep()
{
    std::unordered_set<LLViewerRegion*> regions;
    for (LLViewerRegion* regionp : LLWorld::getInstance()->getRegionList())
    {
        if (mParcelsByRegion.contains(regionp->getRegionID()))
        {
            regions.insert(regionp);
        }
    }
    if (regions.empty())
    {
        return;
    }

    // Killing changes the object list, collect first
    std::vector<LLPointer<LLViewerObject>> roots;
    for (S32 i = 0; i < gObjectList.getNumObjects(); ++i)
    {
        LLViewerObject* objectp = gObjectList.getObject(i);
        if (objectp && !objectp->isDead() && !objectp->getParent() && is_sim_object(objectp) && regions.contains(objectp->getRegion()))
        {
            roots.emplace_back(objectp);
        }
    }

    for (LLViewerObject* objectp : roots)
    {
        derenderIfOnParcel(objectp);
    }
}

// static
void FSParcelDerender::onIdle(void* userdata)
{
    static_cast<FSParcelDerender*>(userdata)->idle();
}

void FSParcelDerender::idle()
{
    if (mPending && mPendingTimer.getElapsedTimeF32() > PENDING_TIMEOUT_SECONDS)
    {
        // Public land and selections without a parcel shape never get one
        LL_INFOS("ParcelDerender") << "No parcel shape arrived for the derender request" << LL_ENDL;
        mPending = false;
    }

    if (!sHasParcels)
    {
        mCreatedObjects.clear();
        return;
    }

    if (!mCreatedObjects.empty())
    {
        std::vector<LLUUID> created;
        created.swap(mCreatedObjects);
        for (const LLUUID& id : created)
        {
            derenderIfOnParcel(gObjectList.findObject(id));
        }
    }

    if (mSweepNow || mSweepTimer.getElapsedTimeF32() > SWEEP_INTERVAL_SECONDS)
    {
        mSweepNow = false;
        mSweepTimer.reset();
        sweep();
    }
}
