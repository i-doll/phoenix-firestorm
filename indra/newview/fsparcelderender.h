/**
 * @file fsparcelderender.h
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

#ifndef FS_PARCELDERENDER_H
#define FS_PARCELDERENDER_H

#include <functional>
#include <unordered_map>
#include <unordered_set>

#include "llframetimer.h"
#include "llsingleton.h"
#include "v3dmath.h"
#include "fsassetblacklist.h"

class LLParcel;
class LLViewerObject;
class LLViewerRegion;

// A derendered parcel is kept in the asset blacklist like any other entry, with its shape in the
// entry's extra data. Objects whose root is on it are killed and their updates ignored, avatars
// and anything an avatar sits on stay rendered.
class FSParcelDerender : public LLSingleton<FSParcelDerender>
{
    LLSINGLETON(FSParcelDerender);
    ~FSParcelDerender();

public:
    // Call after FSAssetBlacklist::init(), picks up the parcels it loaded
    void init();

    // Land menu, derenders the parcel that was right-clicked
    void derenderSelectedParcel(bool permanent);
    bool canDerenderSelectedParcel() const;

    // From LLViewerParcelMgr, the reply to selecting an owned parcel
    void onSelectedParcelProperties(LLViewerRegion* region, const LLParcel* parcel, const U8* bitmap, S32 bitmap_size);

    // From LLViewerObjectList for every object update, so they skip the singleton while nothing is derendered
    static bool isObjectSuppressed(const LLUUID& id) { return sHasSuppressed && instance().mSuppressed.contains(id); }
    static void objectCreated(LLViewerObject* objectp) { if (sHasParcels) instance().onObjectCreated(objectp); }
    // An avatar sitting on an object that isn't there, brings the object back if it was derendered
    static void avatarOrphaned(LLViewerRegion* regionp, U32 parent_local_id) { if (sHasSuppressed) instance().onAvatarOrphaned(regionp, parent_local_id); }

private:
    void onObjectCreated(LLViewerObject* objectp);
    void onAvatarOrphaned(LLViewerRegion* regionp, U32 parent_local_id);

    struct Parcel
    {
        LLUUID              mRegionID;
        S32                 mLocalID{ 0 };
        S32                 mGridSize{ 0 };     // cells per region edge
        std::vector<U8>     mBitmap;            // one bit per cell, row by row from the south west corner

        bool contains(const LLVector3& pos_region, F32 region_width) const;
        LLSD toLLSD() const;
        static bool fromLLSD(const LLSD& data, Parcel& parcel);
    };

    struct SuppressedObject
    {
        LLUUID  mParcelID;
        LLUUID  mRootID;
        U64     mRegionHandle{ 0 };
        U32     mLocalID{ 0 };
    };

    static LLUUID makeParcelID(const LLUUID& region_id, S32 local_id);
    static S32 gridSizeFromBitmap(S32 bitmap_size);

    void derenderParcel(const LLUUID& region_id, S32 local_id, const std::string& name, const std::vector<U8>& bitmap, bool permanent);
    void onBlacklistChanged(const FSAssetBlacklist::changed_signal_data_t& data, FSAssetBlacklist::eBlacklistOperation op);
    void addParcel(const LLUUID& id, const Parcel& parcel);
    void removeParcel(const LLUUID& id);

    // Asks the regions again for the matching suppressed objects
    void restore(const std::function<bool(const SuppressedObject&)>& match);

    const Parcel* findParcel(LLViewerRegion* regionp, const LLVector3& pos_region, LLUUID* parcel_id = nullptr) const;
    bool derenderIfOnParcel(LLViewerObject* objectp);
    void sweep();

    static void onIdle(void* userdata);
    void idle();

    static bool sHasParcels;
    static bool sHasSuppressed;

    std::unordered_map<LLUUID, Parcel>                  mParcels;           // by blacklist entry id
    std::unordered_map<LLUUID, std::vector<LLUUID>>     mParcelsByRegion;   // region id -> blacklist entry ids
    std::unordered_map<LLUUID, SuppressedObject>        mSuppressed;        // object id -> where it came from
    std::unordered_map<LLUUID, F64>                     mKeepUntil;         // restored for a sitter, kept until its sitter arrives

    std::vector<LLUUID>     mCreatedObjects;    // checked on the next frame, positions are set by then
    LLFrameTimer            mSweepTimer;
    bool                    mSweepNow{ false };

    // The last selected parcel, and a derender that waits for it
    LLUUID                  mSelectedRegionID;
    S32                     mSelectedLocalID{ 0 };
    std::string             mSelectedName;
    std::vector<U8>         mSelectedBitmap;
    bool                    mPending{ false };
    bool                    mPendingPermanent{ false };
    LLVector3d              mPendingPosGlobal;
    LLFrameTimer            mPendingTimer;

    boost::signals2::scoped_connection mBlacklistConnection;
};

#endif // FS_PARCELDERENDER_H
