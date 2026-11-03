/**
 * @file fsdecodedtexturecache.cpp
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

#include "llviewerprecompiledheaders.h"

#include "fsdecodedtexturecache.h"

#include "fsyspath.h"
#include "llagent.h"
#include "llcallbacklist.h"
#include "lldir.h"
#include "llfile.h"
#include "llimageworker.h"
#include "llviewercontrol.h"
#include "llviewerregion.h"
#include "threadpool.h"

#ifdef LL_USESYSTEMLIBS
# include <zlib.h>
#else
# include "zlib-ng/zlib.h"
#endif

#include <algorithm>
#include <filesystem>
#include <vector>

// File name: <uuid>.<discard>.<width>.<height>.<components>.<region>.fsdt, so the index can be
// rebuilt from a directory listing without opening anything (names without the region field are
// from before region tags and count as unknown region). The file is a small header and the
// pixels, deflated when that saves space.
namespace
{
    constexpr char ENTRY_EXTENSION[] = ".fsdt";
    constexpr char TEMP_EXTENSION[] = ".tmp";
    constexpr U32 FILE_MAGIC = 0x54444346; // "FCDT"
    constexpr U32 FILE_VERSION = 1;

    struct FileHeader
    {
        U32 mMagic;
        U32 mVersion;
        U16 mWidth;
        U16 mHeight;
        U8 mComponents;
        S8 mDiscard;
        U8 mCompressed;
        U8 mPad;
        U32 mRawSize;
        U32 mDataSize;
    };
}

std::atomic<bool> FSDecodedTextureCache::sEnabled{ false };
std::atomic<U32> FSDecodedTextureCache::sMode{ FSDecodedTextureCache::MODE_LISTED_ONLY };
std::atomic<U32> FSDecodedTextureCache::sMaxMB{ 20480 };
std::atomic<U32> FSDecodedTextureCache::sMinSize{ 256 };
std::atomic<bool> FSDecodedTextureCache::sUncompressed{ false };

// static
FSDecodedTextureCache& FSDecodedTextureCache::instance()
{
    static FSDecodedTextureCache sInstance;
    return sInstance;
}

// static
S64 FSDecodedTextureCache::now()
{
    return (S64)std::filesystem::file_time_type::clock::now().time_since_epoch().count();
}

// FNV-1a offset basis, i.e. the hash an empty name had before empty names counted as unknown
constexpr U32 EMPTY_NAME_HASH = 2166136261u;

// static
U32 FSDecodedTextureCache::regionHash(const std::string& name)
{
    // FNV-1a over the lower case name, stable between sessions. 0 means unknown.
    if (name.empty())
    {
        return 0;
    }
    U32 hash = EMPTY_NAME_HASH;
    for (char c : name)
    {
        hash ^= (U8)tolower((U8)c);
        hash *= 16777619u;
    }
    return hash ? hash : 1;
}

void FSDecodedTextureCache::init(const std::string& cache_dir, bool purge, bool read_only)
{
    mDir = cache_dir;
    mReadOnly = read_only;

    auto bind_setting = [](const char* name, auto apply)
    {
        if (LLControlVariable* control = gSavedSettings.getControl(name))
        {
            apply(control->getValue());
            control->getSignal()->connect([apply](LLControlVariable*, const LLSD& value, const LLSD&) { apply(value); });
        }
    };
    bind_setting("FSDecodedTextureCache", [](const LLSD& value) { sEnabled = value.asBoolean(); });
    bind_setting("FSDecodedTextureCacheSizeMB", [](const LLSD& value) { sMaxMB = (U32)llmax(value.asInteger(), 0); });
    bind_setting("FSDecodedTextureCacheMinSize", [](const LLSD& value) { sMinSize = (U32)llmax(value.asInteger(), 0); });
    bind_setting("FSDecodedTextureCacheUncompressed", [](const LLSD& value) { sUncompressed = value.asBoolean(); });
    bind_setting("FSDecodedTextureCacheMode", [this](const LLSD& value)
        {
            sMode = (U32)llclamp(value.asInteger(), (LLSD::Integer)MODE_ALL, (LLSD::Integer)MODE_LISTED_ONLY);
            applyModeToEntries();
        });
    bind_setting("FSDecodedTextureCacheRegions", [this](const LLSD& value)
        {
            setPinnedRegions(value.asString());
            applyModeToEntries();
        });
    // Decode ordering lives in llimage, which can't read settings
    bind_setting("FSPrioritizeTextureDecodes", [](const LLSD& value) { LLImageDecodeThread::sPrioritize = value.asBoolean(); });

    gAgent.addRegionChangedCallback([this]() { onRegionChanged(); });

    if (purge && !read_only)
    {
        LL_INFOS("DecodedTextureCache") << "Purging " << mDir << LL_ENDL;
        gDirUtilp->deleteDirAndContents(mDir);
    }
    LLFile::mkdir(mDir);

    mWriter = std::make_unique<LL::ThreadPool>("FSDecodedTextureCache", 1);
    mWriter->start();
    mWriter->getQueue().post([this]() { scan(); });
}

void FSDecodedTextureCache::shutdown()
{
    mReady = false;
    if (mWriter)
    {
        mWriter->close();
    }
}

void FSDecodedTextureCache::clear()
{
    mReady = false;
    {
        std::lock_guard<std::mutex> lock(mMutex);
        mEntries.clear();
        mTotalBytes = 0;
    }
    if (mWriter && !mReadOnly)
    {
        mWriter->getQueue().post([this]()
            {
                gDirUtilp->deleteDirAndContents(mDir);
                LLFile::mkdir(mDir);
                mReady = true;
            });
    }
}

// Main thread
void FSDecodedTextureCache::onRegionChanged()
{
    LLViewerRegion* regionp = gAgent.getRegion();
    if (!regionp)
    {
        return;
    }
    // The region changes before its handshake brings the name. Until then nothing is tagged with
    // the old region (current region mode stores nothing), and the name is checked again shortly.
    const U32 region = regionHash(regionp->getName());
    if (!region)
    {
        mCurrentRegion = 0;
        doAfterInterval([this]() { onRegionChanged(); }, 0.5f);
        return;
    }
    const U32 previous = mCurrentRegion.exchange(region);
    if (region == previous)
    {
        return;
    }
    LL_DEBUGS("DecodedTextureCache") << "Region " << regionp->getName() << " (" << region << ")" << LL_ENDL;

    // Also on the first region of a session: logging in somewhere else leaves the last session's
    // region behind just like flying there would
    applyModeToEntries();
}

// Main thread (or init). Current region mode keeps only this region and the listed ones, listed
// only mode just the listed ones. Other modes leave it to size based eviction.
void FSDecodedTextureCache::applyModeToEntries()
{
    if (!mWriter || mReadOnly || !mReady)
    {
        return; // scan() applies it once the index is built
    }
    const U32 mode = sMode;
    const U32 region = mCurrentRegion;
    if (mode == MODE_CURRENT_REGION && region)
    {
        mWriter->getQueue().post([this, region]() { dropOtherRegions(region); });
    }
    else if (mode == MODE_LISTED_ONLY)
    {
        mWriter->getQueue().post([this]() { dropOtherRegions(0); });
    }
}

// Main thread
void FSDecodedTextureCache::setPinnedRegions(const std::string& names)
{
    std::unordered_set<U32> pinned;
    std::string name;
    auto flush = [&]()
    {
        LLStringUtil::trim(name);
        if (!name.empty())
        {
            pinned.insert(regionHash(name));
        }
        name.clear();
    };
    for (char c : names)
    {
        if (c == ',' || c == ';' || c == '\n')
        {
            flush();
        }
        else
        {
            name += c;
        }
    }
    flush();

    std::lock_guard<std::mutex> lock(mMutex);
    mPinnedRegions.swap(pinned);
}

bool FSDecodedTextureCache::isPinned(U32 region) const
{
    return region && mPinnedRegions.count(region);
}

bool FSDecodedTextureCache::wantsStore(bool from_texture_cache) const
{
    if (!isEnabled() || mReadOnly)
    {
        return false;
    }
    const U32 region = mCurrentRegion;
    {
        std::lock_guard<std::mutex> lock(mMutex);
        if (isPinned(region))
        {
            return true;
        }
    }
    switch (sMode)
    {
    case MODE_ALL:
        return true;
    case MODE_REVISITED:
        return from_texture_cache;
    case MODE_CURRENT_REGION:
        return region != 0; // only once we know which region this belongs to
    default:
        return false; // listed only, and the current region isn't listed
    }
}

std::string FSDecodedTextureCache::entryPath(const LLUUID& id, const Entry& entry) const
{
    return llformat("%s%s%s.%d.%u.%u.%u.%08x%s", mDir.c_str(), gDirUtilp->getDirDelimiter().c_str(), id.asString().c_str(),
                    (S32)entry.mDiscard, (U32)entry.mWidth, (U32)entry.mHeight, (U32)entry.mComponents, entry.mRegion, ENTRY_EXTENSION);
}

bool FSDecodedTextureCache::parseEntryName(const std::string& name, LLUUID& id, Entry& entry) const
{
    constexpr size_t UUID_LENGTH = 36;
    const size_t ext_len = sizeof(ENTRY_EXTENSION) - 1;
    if (name.size() <= UUID_LENGTH + ext_len || name.compare(name.size() - ext_len, ext_len, ENTRY_EXTENSION) != 0)
    {
        return false;
    }
    const std::string uuid_str = name.substr(0, UUID_LENGTH);
    if (!LLUUID::validate(uuid_str))
    {
        return false;
    }
    id.set(uuid_str);

    S32 discard = 0;
    U32 width = 0, height = 0, components = 0, region = 0;
    const std::string fields = name.substr(UUID_LENGTH, name.size() - UUID_LENGTH - ext_len);
    const int count = sscanf(fields.c_str(), ".%d.%u.%u.%u.%x", &discard, &width, &height, &components, &region);
    if (count < 4 ||
        discard < 0 || discard > MAX_DISCARD_LEVEL || width == 0 || height == 0 || width > 65535 || height > 65535 ||
        components < 1 || components > 4)
    {
        return false;
    }
    entry.mDiscard = (S8)discard;
    entry.mWidth = (U16)width;
    entry.mHeight = (U16)height;
    entry.mComponents = (U8)components;
    entry.mRegion = count == 5 && region != EMPTY_NAME_HASH ? region : 0;
    return true;
}

// Writer thread
void FSDecodedTextureCache::scan()
{
    LL_PROFILE_ZONE_SCOPED_CATEGORY_TEXTURE;
    std::unordered_map<LLUUID, std::pair<Entry, std::filesystem::path>> found;
    std::vector<std::filesystem::path> stale;
    U64 total = 0;

    std::error_code ec;
    for (std::filesystem::directory_iterator iter(fsyspath(mDir), ec), end; !ec && iter != end; iter.increment(ec))
    {
        // directory_entry members come from the listing, no per file stat on Windows
        const std::filesystem::directory_entry& dir_entry = *iter;
        std::error_code entry_ec;
        if (!dir_entry.is_regular_file(entry_ec) || entry_ec)
        {
            continue;
        }

        const std::string name = fsyspath(dir_entry.path().filename()).string();
        LLUUID id;
        Entry entry;
        if (!parseEntryName(name, id, entry))
        {
            // leftovers of interrupted writes
            if (name.size() > 4 && name.compare(name.size() - 4, 4, TEMP_EXTENSION) == 0)
            {
                stale.push_back(dir_entry.path());
            }
            continue;
        }
        entry.mFileBytes = dir_entry.file_size(entry_ec);
        entry.mLastUse = entry_ec ? 0 : (S64)dir_entry.last_write_time(entry_ec).time_since_epoch().count();

        auto [it, inserted] = found.try_emplace(id, entry, dir_entry.path());
        if (!inserted)
        {
            // keep the higher resolution one
            if (entry.mDiscard < it->second.first.mDiscard)
            {
                stale.push_back(it->second.second);
                total -= it->second.first.mFileBytes;
                it->second = { entry, dir_entry.path() };
            }
            else
            {
                stale.push_back(dir_entry.path());
                continue;
            }
        }
        total += entry.mFileBytes;
    }

    for (const auto& path : stale)
    {
        std::filesystem::remove(path, ec);
    }

    std::unordered_map<LLUUID, Entry> entries;
    entries.reserve(found.size());
    for (auto& [id, entry_path] : found)
    {
        entries.emplace(id, entry_path.first);
    }
    {
        std::lock_guard<std::mutex> lock(mMutex);
        mEntries.swap(entries);
        mTotalBytes = total;
    }
    LL_INFOS("DecodedTextureCache") << "Indexed " << found.size() << " decoded textures, " << (total >> 20) << " MB" << LL_ENDL;
    mReady = true;

    const U32 region = mCurrentRegion;
    if (sMode == MODE_CURRENT_REGION && region)
    {
        dropOtherRegions(region);
    }
    else if (sMode == MODE_LISTED_ONLY)
    {
        dropOtherRegions(0);
    }
    evictIfNeeded();
}

bool FSDecodedTextureCache::canLoad(const LLUUID& id, S32 discard) const
{
    if (!isEnabled() || discard < 0)
    {
        return false;
    }
    std::lock_guard<std::mutex> lock(mMutex);
    auto it = mEntries.find(id);
    if (it == mEntries.end() || it->second.mDiscard > discard)
    {
        return false;
    }
    const Entry& entry = it->second;
    const S32 shift = discard - entry.mDiscard;
    // halving needs sizes that divide evenly
    if (((entry.mWidth >> shift) << shift) != entry.mWidth || ((entry.mHeight >> shift) << shift) != entry.mHeight)
    {
        return false;
    }
    const U32 width = entry.mWidth >> shift;
    const U32 height = entry.mHeight >> shift;
    return width > 0 && height > 0 && llmax(width, height) >= sMinSize;
}

// Decode pool
LLPointer<LLImageRaw> FSDecodedTextureCache::load(const LLUUID& id, S32 discard)
{
    LL_PROFILE_ZONE_SCOPED_CATEGORY_TEXTURE;
    Entry entry;
    {
        std::lock_guard<std::mutex> lock(mMutex);
        auto it = mEntries.find(id);
        if (it == mEntries.end() || it->second.mDiscard > discard)
        {
            return nullptr;
        }
        entry = it->second;
        it->second.mLastUse = now();
    }

    const std::string path = entryPath(id, entry);
    LLPointer<LLImageRaw> raw;
    bool ok = false;
    if (LLFILE* fp = LLFile::fopen(path, "rb"))
    {
        FileHeader header{};
        if (fread(&header, sizeof(header), 1, fp) == 1 &&
            header.mMagic == FILE_MAGIC && header.mVersion == FILE_VERSION &&
            header.mWidth == entry.mWidth && header.mHeight == entry.mHeight &&
            header.mComponents == entry.mComponents && header.mDiscard == entry.mDiscard &&
            header.mRawSize == (U32)entry.mWidth * entry.mHeight * entry.mComponents)
        {
            raw = new LLImageRaw(entry.mWidth, entry.mHeight, entry.mComponents);
            U8* dest = raw->getData();
            if (dest && raw->getDataSize() >= (S32)header.mRawSize)
            {
                if (header.mCompressed)
                {
                    std::vector<U8> packed(header.mDataSize);
                    uLongf dest_len = header.mRawSize;
                    ok = fread(packed.data(), 1, packed.size(), fp) == packed.size() &&
                         uncompress(dest, &dest_len, packed.data(), (uLong)packed.size()) == Z_OK &&
                         dest_len == header.mRawSize;
                }
                else
                {
                    ok = header.mDataSize == header.mRawSize && fread(dest, 1, header.mRawSize, fp) == header.mRawSize;
                }
            }
        }
        LLFile::close(fp);
    }

    if (!ok)
    {
        LL_DEBUGS("DecodedTextureCache") << "Dropping unreadable entry " << path << LL_ENDL;
        ++mLoadFailures;
        dropEntry(id);
        return nullptr;
    }
    ++mLoads;

    // Used in this region now: tag it so leaving the region it came from doesn't drop it
    const U32 region = mCurrentRegion;
    bool retag_here = false;
    if (region && entry.mRegion != region && mWriter && !mReadOnly)
    {
        if (sMode == MODE_CURRENT_REGION)
        {
            retag_here = true;
        }
        else if (sMode == MODE_LISTED_ONLY)
        {
            std::lock_guard<std::mutex> lock(mMutex);
            retag_here = isPinned(region);
        }
    }
    if (retag_here)
    {
        mWriter->getQueue().post([this, id, region]() { retag(id, region); });
    }

    // Halve down to the requested resolution
    S32 width = entry.mWidth;
    S32 height = entry.mHeight;
    for (S32 level = entry.mDiscard; level < discard; ++level)
    {
        width >>= 1;
        height >>= 1;
        LLPointer<LLImageRaw> smaller = new LLImageRaw((U16)width, (U16)height, entry.mComponents);
        if (!smaller->getData())
        {
            return nullptr;
        }
        LLImageBase::generateMip(raw->getData(), smaller->getData(), width, height, entry.mComponents);
        raw = smaller;
    }

    raw->computeAlphaInfo();
    return raw;
}

// Fetch thread
void FSDecodedTextureCache::store(const LLUUID& id, const LLPointer<LLImageRaw>& raw, S32 discard)
{
    if (!isEnabled() || mReadOnly || raw.isNull() || discard < 0 || discard > MAX_DISCARD_LEVEL)
    {
        return;
    }
    const U16 width = raw->getWidth();
    const U16 height = raw->getHeight();
    const U8 components = (U8)raw->getComponents();
    if (components < 1 || components > 4 || llmax(width, height) < sMinSize)
    {
        return;
    }
    {
        std::lock_guard<std::mutex> lock(mMutex);
        auto it = mEntries.find(id);
        if ((it != mEntries.end() && it->second.mDiscard <= discard) || !mPendingWrites.insert(id).second)
        {
            return;
        }
    }
    const U32 region = mCurrentRegion;
    mWriter->getQueue().post([this, id, raw, discard, width, height, components, region]()
        {
            write(id, raw, discard, width, height, components, region);
            std::lock_guard<std::mutex> lock(mMutex);
            mPendingWrites.erase(id);
        });
}

// Writer thread
void FSDecodedTextureCache::write(const LLUUID& id, const LLPointer<LLImageRaw>& raw, S32 discard, U16 width, U16 height, U8 components, U32 region)
{
    LL_PROFILE_ZONE_SCOPED_CATEGORY_TEXTURE;
    const U32 raw_size = (U32)width * height * components;
    std::vector<U8> pixels;
    {
        LLImageDataSharedLock lock(raw);
        // The texture may have rescaled its copy meanwhile, then this is not what was decoded
        const U8* data = static_cast<const LLImageRaw*>(raw.get())->getData();
        if (raw->getWidth() != width || raw->getHeight() != height || raw->getComponents() != components ||
            raw->getDataSize() < (S32)raw_size || !data)
        {
            return;
        }
        pixels.assign(data, data + raw_size);
    }

    FileHeader header{};
    header.mMagic = FILE_MAGIC;
    header.mVersion = FILE_VERSION;
    header.mWidth = width;
    header.mHeight = height;
    header.mComponents = components;
    header.mDiscard = (S8)discard;
    header.mRawSize = raw_size;

    // Level 1: inflating stays far cheaper than a JPEG2000 decode, keep it only if it pays.
    // Texture pixels only shrink to about 65%, and inflating is most of a load's cost, so storing
    // them raw is an option for faster loads at about 1.5x the disk space.
    std::vector<U8> packed;
    uLongf packed_len = 0;
    bool compressed = false;
    if (!sUncompressed)
    {
        packed.resize(compressBound(raw_size));
        packed_len = (uLongf)packed.size();
        compressed = compress2(packed.data(), &packed_len, pixels.data(), raw_size, 1) == Z_OK &&
                     packed_len < raw_size - raw_size / 10;
    }
    header.mCompressed = compressed ? 1 : 0;
    header.mDataSize = compressed ? (U32)packed_len : raw_size;
    const U8* payload = compressed ? packed.data() : pixels.data();

    Entry entry;
    entry.mDiscard = (S8)discard;
    entry.mWidth = width;
    entry.mHeight = height;
    entry.mComponents = components;
    entry.mRegion = region;
    entry.mFileBytes = sizeof(header) + header.mDataSize;
    entry.mLastUse = now();

    const std::string path = entryPath(id, entry);
    const std::string temp_path = path + TEMP_EXTENSION;
    bool ok = false;
    if (LLFILE* fp = LLFile::fopen(temp_path, "wb"))
    {
        ok = fwrite(&header, sizeof(header), 1, fp) == 1 && fwrite(payload, 1, header.mDataSize, fp) == header.mDataSize;
        LLFile::close(fp);
    }
    if (ok)
    {
        LLFile::remove(path, ENOENT);
        ok = LLFile::rename(temp_path, path) == 0;
    }
    if (!ok)
    {
        LLFile::remove(temp_path, ENOENT);
        return;
    }

    std::string remove_path;
    {
        std::lock_guard<std::mutex> lock(mMutex);
        auto [it, inserted] = mEntries.try_emplace(id, entry);
        if (inserted)
        {
            mTotalBytes += entry.mFileBytes;
        }
        else
        {
            const std::string existing_path = entryPath(id, it->second);
            if (existing_path == path)
            {
                // same file, rewritten
                mTotalBytes -= it->second.mFileBytes;
                mTotalBytes += entry.mFileBytes;
                it->second = entry;
            }
            else if (it->second.mDiscard <= discard)
            {
                // something at least as good landed meanwhile, ours is redundant
                remove_path = path;
            }
            else
            {
                remove_path = existing_path;
                mTotalBytes -= it->second.mFileBytes;
                mTotalBytes += entry.mFileBytes;
                it->second = entry;
            }
        }
    }
    if (!remove_path.empty())
    {
        LLFile::remove(remove_path, ENOENT);
    }
    ++mStores;

    evictIfNeeded();
}

FSDecodedTextureCache::Stats FSDecodedTextureCache::getStats() const
{
    Stats stats;
    stats.mEnabled = sEnabled;
    stats.mReady = mReady;
    stats.mMaxBytes = (U64)sMaxMB.load() << 20;
    stats.mLoads = mLoads;
    stats.mLoadFailures = mLoadFailures;
    stats.mStores = mStores;
    stats.mRemoved = mRemoved;
    std::lock_guard<std::mutex> lock(mMutex);
    stats.mEntries = (U32)mEntries.size();
    stats.mBytes = mTotalBytes;
    return stats;
}

// Writer thread
void FSDecodedTextureCache::retag(const LLUUID& id, U32 region)
{
    std::string old_path;
    std::string new_path;
    {
        std::lock_guard<std::mutex> lock(mMutex);
        auto it = mEntries.find(id);
        if (it == mEntries.end() || it->second.mRegion == region || mPendingWrites.count(id))
        {
            return;
        }
        old_path = entryPath(id, it->second);
        Entry moved = it->second;
        moved.mRegion = region;
        new_path = entryPath(id, moved);
        if (LLFile::rename(old_path, new_path) != 0)
        {
            return;
        }
        it->second = moved;
    }
}

// Writer thread
void FSDecodedTextureCache::dropOtherRegions(U32 region)
{
    LL_PROFILE_ZONE_SCOPED_CATEGORY_TEXTURE;
    std::vector<std::string> victims;
    {
        std::lock_guard<std::mutex> lock(mMutex);
        for (auto it = mEntries.begin(); it != mEntries.end();)
        {
            const Entry& entry = it->second;
            // untagged entries from before region tags get tagged when they are used
            if (entry.mRegion && entry.mRegion != region && !isPinned(entry.mRegion))
            {
                victims.push_back(entryPath(it->first, entry));
                mTotalBytes -= entry.mFileBytes;
                it = mEntries.erase(it);
            }
            else
            {
                ++it;
            }
        }
    }
    if (!victims.empty())
    {
        LL_INFOS("DecodedTextureCache") << "Dropping " << victims.size() << " decoded textures from other regions" << LL_ENDL;
        mRemoved += (U32)victims.size();
    }
    for (const std::string& path : victims)
    {
        LLFile::remove(path, ENOENT);
    }
}

void FSDecodedTextureCache::dropEntry(const LLUUID& id)
{
    std::string path;
    {
        std::lock_guard<std::mutex> lock(mMutex);
        auto it = mEntries.find(id);
        if (it == mEntries.end())
        {
            return;
        }
        path = entryPath(id, it->second);
        mTotalBytes -= it->second.mFileBytes;
        mEntries.erase(it);
    }
    ++mRemoved;
    LLFile::remove(path, ENOENT);
}

// Writer thread
void FSDecodedTextureCache::evictIfNeeded()
{
    const U64 max_bytes = (U64)sMaxMB.load() << 20;
    std::vector<std::string> victims;
    {
        std::lock_guard<std::mutex> lock(mMutex);
        if (mTotalBytes <= max_bytes)
        {
            return;
        }
        // Least recently used first, listed regions last, down to 90% so this doesn't run on every write
        std::vector<std::tuple<bool, S64, LLUUID>> order;
        order.reserve(mEntries.size());
        for (const auto& [id, entry] : mEntries)
        {
            order.emplace_back(isPinned(entry.mRegion), entry.mLastUse, id);
        }
        std::sort(order.begin(), order.end());
        const U64 target = max_bytes - max_bytes / 10;
        for (const auto& [pinned, last_use, id] : order)
        {
            if (mTotalBytes <= target)
            {
                break;
            }
            auto it = mEntries.find(id);
            victims.push_back(entryPath(id, it->second));
            mTotalBytes -= it->second.mFileBytes;
            mEntries.erase(it);
        }
    }
    mRemoved += (U32)victims.size();
    for (const std::string& path : victims)
    {
        LLFile::remove(path, ENOENT);
    }
}

//----------------------------------------------------------------------------
// Preferences > Experimental shows how the cache is doing

#include "lltextbox.h"

bool FSPanelPreferenceExperimental::postBuild()
{
    mCacheStats = findChild<LLTextBox>("decoded_texture_cache_stats");
    return LLPanelPreference::postBuild();
}

void FSPanelPreferenceExperimental::draw()
{
    if (mCacheStats && (mStatsDirty || mStatsTimer.getElapsedTimeF32() >= 1.f))
    {
        mStatsDirty = false;
        mStatsTimer.reset();
        mCacheStats->setText(formatStats());
    }
    LLPanelPreference::draw();
}

std::string FSPanelPreferenceExperimental::formatStats() const
{
    const FSDecodedTextureCache::Stats stats = FSDecodedTextureCache::instance().getStats();
    if (!stats.mEnabled)
    {
        return getString("decoded_cache_stats_off");
    }
    if (!stats.mReady)
    {
        return getString("decoded_cache_stats_indexing");
    }
    LLStringUtil::format_map_t args;
    args["[ENTRIES]"] = llformat("%u", stats.mEntries);
    args["[USED]"] = llformat("%.1f", (F64)stats.mBytes / (1024.0 * 1024.0 * 1024.0));
    args["[MAX]"] = llformat("%.1f", (F64)stats.mMaxBytes / (1024.0 * 1024.0 * 1024.0));
    args["[LOADS]"] = llformat("%u", stats.mLoads);
    args["[FAILURES]"] = llformat("%u", stats.mLoadFailures);
    args["[STORES]"] = llformat("%u", stats.mStores);
    args["[REMOVED]"] = llformat("%u", stats.mRemoved);
    return getString("decoded_cache_stats", args);
}

static LLPanelInjector<FSPanelPreferenceExperimental> t_pref_experimental("panel_preference_experimental");
