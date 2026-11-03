/**
 * @file llimageworker.h
 * @brief Object for managing images and their textures.
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

#ifndef LL_LLIMAGEWORKER_H
#define LL_LLIMAGEWORKER_H

#include "llimage.h"
#include "llpointer.h"
#include "threadpool_fwd.h"

// <FS:Trish>
#include <atomic>
#include <functional>
#include <mutex>
#include <vector>
// </FS:Trish>

class LLImageDecodeThread
{
public:
    class Responder : public LLThreadSafeRefCount
    {
    protected:
        virtual ~Responder();
    public:
        virtual void completed(bool success, const std::string& error_message, LLImageRaw* raw, LLImageRaw* aux, U32 request_id) = 0;
        // <FS:Trish> Decode thread, right before decoding: true when nothing waits for this result
        // anymore. completed() is still called, with success false.
        virtual bool isObsolete(U32 request_id) { return false; }
        // </FS:Trish>
    };

public:
    LLImageDecodeThread(bool threaded = true);
    virtual ~LLImageDecodeThread();

    // meant to resemble LLQueuedThread::handle_t
    typedef U32 handle_t;
    // <FS:Trish> priority: higher decodes first while sPrioritize is set
    //handle_t decodeImage(const LLPointer<LLImageFormatted>& image,
    //                     S32 discard, bool needs_aux,
    //                     const LLPointer<Responder>& responder);
    handle_t decodeImage(const LLPointer<LLImageFormatted>& image,
                         S32 discard, bool needs_aux,
                         const LLPointer<Responder>& responder,
                         F32 priority = 0.f);
    // Other work for the decode pool, ordered with the decodes
    bool postTask(std::function<void()> task, F32 priority);
    // Run queued work by priority instead of in submission order
    static std::atomic<bool> sPrioritize;
    // </FS:Trish>
    size_t getPending();
    size_t update(F32 max_time_ms);
    S32 getTotalDecodeCount() { return mDecodeCount; }
    void shutdown();

    // <FS:Trish> Run decode threads below normal priority, applied by each thread before its next decode
    static void setBackgroundPriority(bool background);
    // </FS:Trish>

private:
    // <FS:Trish> Each item posted to the pool runs whichever pending task has the highest priority.
    // Declared before the pool so it outlives the pool threads using it.
    struct PendingTask
    {
        F32 mPriority;
        U64 mSeq;
        std::function<void()> mTask;
    };
    bool enqueue(std::function<void()> task, F32 priority);
    void runNext();
    std::mutex mPendingMutex;
    std::vector<PendingTask> mPending; // heap, highest priority then oldest on top
    U64 mPendingSeq = 0;
    // </FS:Trish>

    // As of SL-17483, LLImageDecodeThread is no longer itself an
    // LLQueuedThread - instead this is the API by which we submit work to the
    // "ImageDecode" ThreadPool.
    std::unique_ptr<LL::ThreadPool> mThreadPool;
    LLAtomicU32 mDecodeCount;
};

#endif
