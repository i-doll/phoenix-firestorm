/**
 * @file lllivefile.cpp
 *
 * $LicenseInfo:firstyear=2006&license=viewerlgpl$
 * Second Life Viewer Source Code
 * Copyright (C) 2010, Linden Research, Inc.
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
 * Linden Research, Inc., 945 Battery Street, San Francisco, CA  94111  USA
 * $/LicenseInfo$
 */

#include "linden_common.h"

#include "lllivefile.h"
#include "llframetimer.h"
#include "lleventtimer.h"

// <FS:Trish> stat the file off the main thread
#include <atomic>
#include <memory>
#include <thread>
// </FS:Trish>

const F32 DEFAULT_CONFIG_FILE_REFRESH = 5.0f;


class LLLiveFile::Impl
{
public:
    Impl(const std::string& filename, const F32 refresh_period);
    ~Impl();

    bool check();
    void changed();

    // <FS:Trish> The periodic checks stat the file on a short lived thread: a stat can block for
    // seconds when the disk is busy (a build, antivirus scanning a large file operation), and these
    // checks run on the main thread every few seconds, freezing the viewer for that long. The first
    // (forced) check stays synchronous, as callers expect the file loaded once it returns.
    struct AsyncStat
    {
        std::atomic<int> mState{ 0 };   // 0 idle, 1 running, 2 result ready
        bool mExists = false;
        time_t mModTime = 0;
    };
    std::shared_ptr<AsyncStat> mAsync;  // shared with a thread that may outlive this
    bool checkAsync();
    bool applyStat(bool exists, time_t mod_time);
    // </FS:Trish>

    bool mForceCheck;
    F32 mRefreshPeriod;
    LLFrameTimer mRefreshTimer;

    std::string mFilename;
    time_t mLastModTime;
    time_t mLastStatTime;
    bool mLastExists;

    LLEventTimer* mEventTimer;
private:
    LOG_CLASS(LLLiveFile);
};

LLLiveFile::Impl::Impl(const std::string& filename, const F32 refresh_period)
    :
    mForceCheck(true),
    mRefreshPeriod(refresh_period),
    mFilename(filename),
    mLastModTime(0),
    mLastStatTime(0),
    mLastExists(false),
    mEventTimer(NULL)
{
}

LLLiveFile::Impl::~Impl()
{
    delete mEventTimer;
}

LLLiveFile::LLLiveFile(const std::string& filename, const F32 refresh_period)
    : impl(* new Impl(filename, refresh_period))
{
}

LLLiveFile::~LLLiveFile()
{
    delete &impl;
}


// <FS:Trish> see AsyncStat
bool LLLiveFile::Impl::applyStat(bool exists, time_t mod_time)
{
    bool detected_change = false;
    if (!exists)
    {
        if (mLastExists)
        {
            mLastExists = false;
            detected_change = true; // no longer existing is a change!
            LL_DEBUGS() << "detected deleted file '" << mFilename << "'" << LL_ENDL;
        }
    }
    else
    {
        if (!mLastExists)
        {
            LL_DEBUGS() << "detected created file '" << mFilename << "'" << LL_ENDL;
            detected_change = true;
        }
        else if (mod_time > mLastModTime)
        {
            LL_DEBUGS() << "detected updated file '" << mFilename << "'" << LL_ENDL;
            detected_change = true;
        }
        mLastExists = true;
        mLastStatTime = mod_time;
    }
    if (detected_change)
    {
        LL_INFOS() << "detected file change '" << mFilename << "'" << LL_ENDL;
    }
    return detected_change;
}

bool LLLiveFile::Impl::checkAsync()
{
    bool detected_change = false;
    if (mAsync && mAsync->mState.load(std::memory_order_acquire) == 2)
    {
        detected_change = applyStat(mAsync->mExists, mAsync->mModTime);
        mAsync->mState.store(0, std::memory_order_relaxed);
    }
    if (mRefreshTimer.getElapsedTimeF32() >= mRefreshPeriod &&
        (!mAsync || mAsync->mState.load(std::memory_order_acquire) == 0))
    {
        mRefreshTimer.reset();
        if (!mAsync)
        {
            mAsync = std::make_shared<AsyncStat>();
        }
        mAsync->mState.store(1, std::memory_order_relaxed);
        std::shared_ptr<AsyncStat> state = mAsync;
        std::string filename = mFilename;
        try
        {
            std::thread([state, filename]()
            {
                llstat stat_data;
                const bool exists = LLFile::stat(filename, &stat_data) == 0;
                state->mExists = exists;
                state->mModTime = exists ? stat_data.st_mtime : 0;
                state->mState.store(2, std::memory_order_release);
            }).detach();
        }
        catch (const std::system_error&)
        {   // no thread: check the old way
            mAsync->mState.store(0, std::memory_order_relaxed);
            llstat stat_data;
            const bool exists = LLFile::stat(mFilename, &stat_data) == 0;
            detected_change |= applyStat(exists, exists ? stat_data.st_mtime : 0);
        }
    }
    return detected_change;
}
// </FS:Trish>

bool LLLiveFile::Impl::check()
{
    // <FS:Trish> periodic checks don't block on the disk
    if (!mForceCheck)
    {
        return checkAsync();
    }
    // </FS:Trish>
    bool detected_change = false;
    // Skip the check if not enough time has elapsed and we're not
    // forcing a check of the file
    if (mForceCheck || mRefreshTimer.getElapsedTimeF32() >= mRefreshPeriod)
    {
        mForceCheck = false;   // force only forces one check
        mRefreshTimer.reset(); // don't check again until mRefreshPeriod has passed

        // Stat the file to see if it exists and when it was last modified.
        llstat stat_data;
        if (LLFile::stat(mFilename, &stat_data))
        {
            // Couldn't stat the file, that means it doesn't exist or is
            // broken somehow.
            if (mLastExists)
            {
                mLastExists = false;
                detected_change = true; // no longer existing is a change!
                LL_DEBUGS() << "detected deleted file '" << mFilename << "'" << LL_ENDL;
            }
        }
        else
        {
            // The file exists
            if ( ! mLastExists )
            {
                // last check, it did not exist - that counts as a change
                LL_DEBUGS() << "detected created file '" << mFilename << "'" << LL_ENDL;
                detected_change = true;
            }
            else if ( stat_data.st_mtime > mLastModTime )
            {
                // file modification time is newer than last check
                LL_DEBUGS() << "detected updated file '" << mFilename << "'" << LL_ENDL;
                detected_change = true;
            }
            mLastExists = true;
            mLastStatTime = stat_data.st_mtime;
        }
    }
    if (detected_change)
    {
        LL_INFOS() << "detected file change '" << mFilename << "'" << LL_ENDL;
    }
    return detected_change;
}

void LLLiveFile::Impl::changed()
{
    // we wanted to read this file, and we were successful.
    mLastModTime = mLastStatTime;
}

bool LLLiveFile::checkAndReload()
{
    bool changed = impl.check();
    if (changed)
    {
        if(loadFile())
        {
            impl.changed();
            this->changed();
        }
        else
        {
            changed = false;
        }
    }
    return changed;
}

std::string LLLiveFile::filename() const
{
    return impl.mFilename;
}

namespace
{
    class LiveFileEventTimer : public LLEventTimer
    {
    public:
        LiveFileEventTimer(LLLiveFile& f, F32 refresh)
            : LLEventTimer(refresh), mLiveFile(f)
            { }

        bool tick()
        {
            mLiveFile.checkAndReload();
            return false;
        }

    private:
        LLLiveFile& mLiveFile;
    };

}

void LLLiveFile::addToEventTimer()
{
    impl.mEventTimer = new LiveFileEventTimer(*this, impl.mRefreshPeriod);
}

void LLLiveFile::setRefreshPeriod(F32 seconds)
{
    if (seconds < 0.f)
    {
        seconds = -seconds;
    }
    impl.mRefreshPeriod = seconds;
}

