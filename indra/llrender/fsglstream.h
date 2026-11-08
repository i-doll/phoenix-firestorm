/**
 * @file fsglstream.h
 * @brief GL command stream for the optional render thread (FSRenderThread).
 *
 * $LicenseInfo:firstyear=2026&license=fsviewerlgpl$
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

#ifndef FS_GLSTREAM_H
#define FS_GLSTREAM_H

#include "stdtypes.h"
#include <functional>
#include <string>

// On Windows every GL entry point the viewer calls is a function pointer
// loaded by LLGLManager (llglheaders.h). The stream replaces each of those
// pointers with a proxy. While the stream runs, a call on the main thread is
// recorded into a command buffer instead of reaching the driver, and a render
// thread that owns the GL context replays the buffer in order. The main
// thread keeps running the old code unchanged; only the driver work moves.
//
// A call is deferred only when that is exactly equivalent:
//  - it returns nothing and all its arguments are values, or
//  - it reads client memory of a size that can be computed (uniform arrays,
//    buffer and texture uploads, name lists), which is copied at record time, or
//  - its pointer argument is a buffer offset because the matching buffer is
//    bound (draw indices, vertex attributes, indirect draws, PBO transfers).
// Everything else (any return value, any output, any read of client memory
// of unknown size) is synchronous: the main thread waits until the render
// thread has replayed everything recorded so far and has made the call with
// the main thread's own arguments. A few frequent queries are answered on the
// main thread from state the recorder tracks itself, from constants, or (query
// objects) from results the render thread collects, which keeps the GL rules
// (a query result may become available later than it would have before).
//
// Proxies are only installed when FSRenderThread was on at startup. Without
// them nothing here runs and GL calls go straight to the driver as before.
namespace FSGLStream
{
    struct ContextOps
    {
        std::function<void()> mReleaseOnMain;    // main thread gives the context up
        std::function<void()> mAcquireOnMain;    // main thread takes it back
        std::function<bool()> mAcquireOnRender;  // render thread makes it current
        std::function<void()> mReleaseOnRender;  // render thread gives it up
        F32 mGLVersion = 0.f;                    // gGLManager.mGLVersion
    };

    // Replaces the GL function pointers with proxies. Main thread, after
    // LLGLManager loaded them, before any other GL thread could be mid-call
    // through a proxy that does not exist yet. Idempotent.
    bool install();
    bool isInstalled();

    // Moves the context to a new render thread and starts recording.
    bool start(const ContextOps& ops);
    // Replays what is left, moves the context back to the main thread and
    // joins the render thread. GL calls go straight to the driver afterwards.
    void stop();
    bool isRunning();

    // True on the main thread while its GL calls are recorded.
    bool isRecording();

    // Runs fn on the render thread, in stream order, and waits for it.
    void runSync(const std::function<void()>& fn);

    // Calls fn(value) on the render thread in stream order without waiting (directly when not
    // recording). For GL calls made outside the proxies that return nothing.
    void recordCall(void (*fn)(U64), U64 value);

    // Clears the GL error flags without waiting: in stream order while recording, directly
    // otherwise. For code that only wants a clean slate before its own glGetError check.
    void clearErrors();

    // Records the present (fn runs on the render thread, in stream order)
    // and waits until the previous present has been executed, so the main
    // thread is never more than one frame ahead of the render thread.
    void present(std::function<void()> fn);

    // FSRenderThreadCheck: log GL calls made on a thread that has no GL
    // context, and the first synchronous call of each entry point.
    void setCheck(bool check);

    // FSRenderThreadFilter: drop calls that would set state to what it already is (on by
    // default; the stats line counts them). Main thread.
    void setFilter(bool filter);

    struct Stats
    {
        U32 mPresents = 0;
        U64 mRecordedCalls = 0;
        U64 mRecordedBytes = 0;
        U64 mSyncCalls = 0;
        F64 mSyncWaitMs = 0.0;      // main thread blocked on synchronous calls
        F64 mPresentWaitMs = 0.0;   // main thread blocked on the frame bound
        F64 mRenderBusyMs = 0.0;    // render thread replaying
        F64 mRenderIdleMs = 0.0;    // render thread waiting for work
        std::string mTopSyncs;      // "glGetError x12, ..." most frequent synchronous calls
        std::string mTopCalls;      // "glUniformMatrix4fv 812 (50 KB), ..." most recorded calls, per frame
        std::string mTopSkips;      // "glBindTexture 340, ..." calls dropped as redundant, per frame
        U64 mSkippedCalls = 0;      // calls dropped as redundant (they would set state to what it is)
    };
    // Main thread. Returns the counters since the last call and resets them.
    Stats takeStats();
}

#endif // FS_GLSTREAM_H
