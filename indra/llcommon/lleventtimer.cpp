/**
 * @file lleventtimer.cpp
 * @brief Cross-platform objects for doing timing
 *
 * $LicenseInfo:firstyear=2000&license=viewerlgpl$
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

#include "lleventtimer.h"

#include "u64.h"

// <FS:Trish> Lightweight registry of live timers so updateClass() can see that nothing is due
// without building an LLInstanceTracker snapshot (vector of weak_ptrs) every frame.
// Leaked on purpose so timers destroyed during static destruction never touch a dead container.
#include <algorithm>
#include <mutex>
#include <vector>

namespace
{
    std::mutex& timer_registry_mutex()
    {
        static std::mutex* sMutex = new std::mutex;
        return *sMutex;
    }

    std::vector<LLEventTimer*>& timer_registry()
    {
        static std::vector<LLEventTimer*>* sTimers = new std::vector<LLEventTimer*>;
        return *sTimers;
    }

    void register_timer(LLEventTimer* timer)
    {
        std::lock_guard<std::mutex> lock(timer_registry_mutex());
        timer_registry().push_back(timer);
    }

    void unregister_timer(LLEventTimer* timer)
    {
        std::lock_guard<std::mutex> lock(timer_registry_mutex());
        auto& timers = timer_registry();
        auto it = std::find(timers.begin(), timers.end(), timer);
        if (it != timers.end())
        {
            *it = timers.back();
            timers.pop_back();
        }
    }
}
// </FS:Trish>


//////////////////////////////////////////////////////////////////////////////
//
//      LLEventTimer Implementation
//
//////////////////////////////////////////////////////////////////////////////

LLEventTimer::LLEventTimer(F32 period)
: mEventTimer()
{
    mPeriod = period;
    register_timer(this); // <FS:Trish/>
}

LLEventTimer::LLEventTimer(const LLDate& time)
: mEventTimer()
{
    mPeriod = (F32)(time.secondsSinceEpoch() - LLDate::now().secondsSinceEpoch());
    register_timer(this); // <FS:Trish/>
}


LLEventTimer::~LLEventTimer()
{
    unregister_timer(this); // <FS:Trish/>
}

//static
void LLEventTimer::updateClass()
{
    // <FS:Trish> Usually no timer is due. Check that with the same test as the loop below,
    // without a snapshot. Subclasses change mEventTimer and mPeriod directly, so there is no
    // cached expiry to trust; every timer is tested each frame as before.
    {
        bool any_due = false;
        std::lock_guard<std::mutex> lock(timer_registry_mutex());
        for (const LLEventTimer* timer : timer_registry())
        {
            if (timer->mEventTimer.getStarted() && timer->mEventTimer.getElapsedTimeF32() > timer->mPeriod)
            {
                any_due = true;
                break;
            }
        }
        if (!any_due)
        {
            return;
        }
    }
    // </FS:Trish>

    for (auto& timer : instance_snapshot())
    {
        F32 et = timer.mEventTimer.getElapsedTimeF32();
        if (timer.mEventTimer.getStarted() && et > timer.mPeriod) {
            timer.mEventTimer.reset();
            if ( timer.tick() )
            {
                delete &timer;
            }
        }
    }
}


