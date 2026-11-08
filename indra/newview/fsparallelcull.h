/**
 * @file fsparallelcull.h
 * @brief Worker pool for the parallel octree culls (FSPerfParallelShadowCull, FSPerfParallelMainCull)
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

#ifndef FS_PARALLELCULL_H
#define FS_PARALLELCULL_H

#include "stdtypes.h"

#include <functional>

// A small set of persistent worker threads, started on first use. run() hands the job indices
// 0..count-1 out to the workers and to the calling thread, which works through jobs too instead of
// sleeping, and returns once every job has finished. Jobs must not throw and must not call run().
namespace FSParallelCull
{
    void run(U32 count, const std::function<void(U32)>& job);
    U32  workerCount();     // worker threads besides the caller, 0 before the first run or when none could start
    void shutdown();        // LLPipeline::cleanup: stop and join the workers
}

#endif // FS_PARALLELCULL_H
