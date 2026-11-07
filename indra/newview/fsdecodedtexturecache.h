/**
 * @file fsdecodedtexturecache.h
 * @brief Disk cache of decoded texture pixels, so textures seen before skip JPEG2000 decoding
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

#ifndef FS_DECODEDTEXTURECACHE_H
#define FS_DECODEDTEXTURECACHE_H

#include "llimage.h"
#include "lluuid.h"
#include "threadpool_fwd.h"

#include <atomic>
#include <map>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <unordered_set>

// Texture assets never change, so the pixels a JPEG2000 decode produced can be kept and loaded
// back far faster than decoding again. One entry per texture, holding the highest resolution
// decoded so far; lower resolutions are made from it by halving. The index lives in memory and
// is rebuilt from the file names at startup. Loads run on the image decode pool, writes and
// eviction on a thread of their own.
//
// Entries are tagged with the region the agent was in when they were decoded, which is what the
// modes below key on. A region can be several GB of pixels, so what is kept is selective:
//  - MODE_ALL: everything decoded
//  - MODE_REVISITED: only textures whose JPEG2000 data came from the texture cache (seen before)
//  - MODE_CURRENT_REGION: everything decoded in the current region; another region's entries are
//    dropped on arriving somewhere else, so relogging or moving around the same sim keeps them
//  - MODE_LISTED_ONLY: only the regions listed in FSDecodedTextureCacheRegions
// Listed regions are always stored, never dropped on a region change, and evicted last.
class FSDecodedTextureCache
{
public:
    enum EMode : U32
    {
        MODE_ALL = 0,
        MODE_REVISITED = 1,
        MODE_CURRENT_REGION = 2,
        MODE_LISTED_ONLY = 3
    };

    static FSDecodedTextureCache& instance();

    // Main thread
    void init(const std::string& cache_dir, bool purge, bool read_only);
    void shutdown();
    void clear();

    // Any thread
    bool isEnabled() const { return mReady && sEnabled; }
    // Fetch thread: whether a fresh decode should be kept under the current mode
    bool wantsStore(bool from_texture_cache) const;

    // Any thread. True when an entry can produce 'discard' and the result is at least the
    // configured minimum size (smaller decodes are cheap enough already).
    bool canLoad(const LLUUID& id, S32 discard) const;

    // Decode pool. Returns null on failure, dropping the entry.
    LLPointer<LLImageRaw> load(const LLUUID& id, S32 discard);

    // Fetch thread. Queues a write when this is better than what is stored. 'raw' must not be
    // modified while the write is pending (texture fetch results are not).
    void store(const LLUUID& id, const LLPointer<LLImageRaw>& raw, S32 discard);

    struct Stats
    {
        bool mEnabled = false;
        bool mReady = false;
        U32 mEntries = 0;
        U64 mBytes = 0;
        U64 mMaxBytes = 0;
        U32 mLoads = 0;         // this session
        U32 mLoadFailures = 0;
        U32 mStores = 0;
        U32 mRemoved = 0;
    };
    Stats getStats() const;

private:
    struct Entry
    {
        S8 mDiscard = -1;
        U16 mWidth = 0; // at mDiscard
        U16 mHeight = 0;
        U8 mComponents = 0;
        U32 mRegion = 0; // hash of the region name, 0 when unknown
        U64 mFileBytes = 0;
        S64 mLastUse = 0; // file clock ticks
    };

    FSDecodedTextureCache() = default;

    static U32 regionHash(const std::string& name);
    void onRegionChanged();
    void setPinnedRegions(const std::string& names);
    bool isPinned(U32 region) const; // mMutex held

    std::string entryPath(const LLUUID& id, const Entry& entry) const;
    bool parseEntryName(const std::string& name, LLUUID& id, Entry& entry) const;
    void scan();
    void write(const LLUUID& id, const LLPointer<LLImageRaw>& raw, S32 discard, U16 width, U16 height, U8 components, U32 region);
    void retag(const LLUUID& id, U32 region);
    // Drops tagged entries that are neither from 'region' nor listed (region 0: all unlisted ones)
    void dropOtherRegions(U32 region);
    // Posts the clean up the current mode calls for, after a region, mode or list change
    void applyModeToEntries();
    void dropEntry(const LLUUID& id);
    void evictIfNeeded();
    static S64 now();

    std::string mDir;
    bool mReadOnly = false;
    std::atomic<bool> mReady{ false };
    std::unique_ptr<LL::ThreadPool> mWriter;

    mutable std::mutex mMutex;
    std::unordered_map<LLUUID, Entry> mEntries;
    std::unordered_set<LLUUID> mPendingWrites;
    std::unordered_set<U32> mPinnedRegions;
    U64 mTotalBytes = 0;

    std::atomic<U32> mCurrentRegion{ 0 };

    // Session counters for the stats
    std::atomic<U32> mLoads{ 0 };
    std::atomic<U32> mLoadFailures{ 0 };
    std::atomic<U32> mStores{ 0 };
    std::atomic<U32> mRemoved{ 0 };

    // Mirrors of the settings, updated on the main thread
    static std::atomic<bool> sEnabled;
    static std::atomic<bool> sUncompressed;
    static std::atomic<U32> sMode;
    static std::atomic<U32> sMaxMB;
    static std::atomic<U32> sMinSize;
};

// Preferences > Experimental shows how the cache is doing
#include "llfloaterpreference.h"
#include "llframetimer.h"

class LLButton;
class LLComboBox;
class LLControlVariable;
class LLTextBox;

class FSPanelPreferenceExperimental : public LLPanelPreference
{
public:
    FSPanelPreferenceExperimental();
    bool postBuild() override;
    void draw() override;

private:
    std::string formatStats() const;

    // <FS:Trish> Presets of the rendering tabs' settings
    typedef std::map<std::string, LLControlVariable*> preset_controls_t;
    preset_controls_t collectPresetControls();
    void applyPreset(const LLSD& settings);
    void refreshPresetList(const std::string& select_value);
    std::string selectedPreset() const;
    void updatePresetButtons();
    void onPresetLoad();
    void onPresetSaveAs();
    void onPresetNameEntered(std::string name);
    void savePreset(const std::string& name);
    void onPresetDelete();

    LLComboBox* mPresetCombo = nullptr;
    LLButton* mPresetLoadBtn = nullptr;
    LLButton* mPresetDeleteBtn = nullptr;
    // </FS:Trish>

    LLTextBox* mCacheStats = nullptr;
    LLFrameTimer mStatsTimer;
    bool mStatsDirty = true;
};

#endif // FS_DECODEDTEXTURECACHE_H
