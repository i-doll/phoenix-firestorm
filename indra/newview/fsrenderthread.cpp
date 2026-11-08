/**
 * @file fsrenderthread.cpp
 * @brief Optional render thread (FSRenderThread): viewer side.
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

#include "llviewerprecompiledheaders.h"

#include "fsrenderthread.h"

#include "fsglstream.h"
#include "llappviewer.h"
#include "llgl.h"
#include "llimagegl.h"
#include "lltimer.h"
#include "llviewercontrol.h"
#include "llviewerwindow.h"

#if LL_WINDOWS
#include "llwindowwin32.h"
#endif

namespace
{
    bool sTried = false;
    bool sActive = false;
    bool sPrevThreadedTextures = false;
    bool sPrevThreadedMedia = false;

    LLTimer sStatsTimer;
    LLTimer sFrameTimer;
    U32 sFrames = 0;
    F64 sFrameSeconds = 0.0;

    constexpr F32 STATS_PERIOD = 10.f;

#if LL_WINDOWS
    HDC sDC = nullptr;
    HGLRC sRC = nullptr;

    bool swapHook()
    {
        if (!FSGLStream::isRecording())
        {
            return false;
        }
        HDC dc = sDC;
        FSGLStream::present([dc]()
        {
            SwapBuffers(dc);
        });
        return true;
    }
#endif

    void logStats(const char* what)
    {
        const FSGLStream::Stats s = FSGLStream::takeStats();
        const F64 frames = sFrames ? (F64)sFrames : 1.0;
        const F64 frame_ms = sFrameSeconds * 1000.0 / frames;
        const F64 sync_ms = s.mSyncWaitMs / frames;
        const F64 wait_ms = s.mPresentWaitMs / frames;
        LL_INFOS("FSRenderThread") << what
            << llformat(" %u frames, %.2f ms/frame: main busy %.2f ms (waits: sync %.2f ms, frame bound %.2f ms), "
                        "render busy %.2f ms, render idle %.2f ms; per frame %.1f sync calls, %.0f GL calls, %.0f KB recorded",
                        sFrames, frame_ms, frame_ms - sync_ms - wait_ms, sync_ms, wait_ms,
                        s.mRenderBusyMs / frames, s.mRenderIdleMs / frames,
                        (F64)s.mSyncCalls / frames, (F64)s.mRecordedCalls / frames, (F64)s.mRecordedBytes / 1024.0 / frames)
            << llformat(", %.0f redundant calls dropped", (F64)s.mSkippedCalls / frames)
            << (s.mTopSyncs.empty() ? std::string() : "; most frequent sync calls: " + s.mTopSyncs)
            << LL_ENDL;
        // Which calls make up the stream: what to cut next
        if (!s.mTopCalls.empty())
        {
            LL_INFOS("FSRenderThread") << "Calls per frame (recorded bytes): " << s.mTopCalls << LL_ENDL;
        }
        if (!s.mTopSkips.empty())
        {
            LL_INFOS("FSRenderThread") << "Redundant calls dropped per frame: " << s.mTopSkips << LL_ENDL;
        }
        if (!s.mTopZones.empty())
        {
            LL_INFOS("FSRenderThread") << "Calls per frame by GPU zone: " << s.mTopZones << LL_ENDL;
        }
        sFrames = 0;
        sFrameSeconds = 0.0;
    }

    void start()
    {
#if LL_WINDOWS
        if (!gViewerWindow || !gViewerWindow->getWindow() || !gGLManager.mInited || gHeadlessClient || gNonInteractive)
        {
            LL_WARNS("FSRenderThread") << "FSRenderThread is on but there is no GL window; staying single-threaded" << LL_ENDL;
            return;
        }
        LLWindowWin32* window = dynamic_cast<LLWindowWin32*>(gViewerWindow->getWindow());
        if (!window || !window->getFSDeviceContext() || !window->getFSGLContext())
        {
            LL_WARNS("FSRenderThread") << "FSRenderThread is on but the window has no GL context; staying single-threaded" << LL_ENDL;
            return;
        }
        sDC = window->getFSDeviceContext();
        sRC = window->getFSGLContext();

        LL_PROFILE_ZONE_NAMED_CATEGORY_DISPLAY("FSRT start");

        // The shared-context texture and media workers run their GL right
        // away while the main thread's GL runs later on the render thread; a
        // worker could then touch a texture before the main thread's commands
        // that create it have executed. Uploads go through the stream instead
        // (they no longer cost the main thread anything but a copy).
        sPrevThreadedTextures = LLImageGLThread::sEnabledTextures;
        sPrevThreadedMedia = LLImageGLThread::sEnabledMedia;
        LLImageGLThread::sEnabledTextures = false;
        LLImageGLThread::sEnabledMedia = false;

        FSGLStream::install();

        FSGLStream::ContextOps ops;
        ops.mReleaseOnMain = []() { wglMakeCurrent(NULL, NULL); };
        ops.mAcquireOnMain = []()
        {
            if (!wglMakeCurrent(sDC, sRC))
            {
                LL_WARNS("FSRenderThread") << "Main thread could not take the GL context back, error " << (U32)GetLastError() << LL_ENDL;
            }
        };
        ops.mAcquireOnRender = []() -> bool
        {
            if (!wglMakeCurrent(sDC, sRC))
            {
                LL_WARNS("FSRenderThread") << "Render thread could not make the GL context current, error " << (U32)GetLastError() << LL_ENDL;
                return false;
            }
            return true;
        };
        ops.mReleaseOnRender = []() { wglMakeCurrent(NULL, NULL); };
        ops.mGLVersion = gGLManager.mGLVersion;

        if (!FSGLStream::start(ops))
        {
            LLImageGLThread::sEnabledTextures = sPrevThreadedTextures;
            LLImageGLThread::sEnabledMedia = sPrevThreadedMedia;
            return;
        }
        LLWindowWin32::sFSSwapBuffersHook = &swapHook;
        sActive = true;
        sStatsTimer.reset();
        sFrameTimer.reset();
        sFrames = 0;
        sFrameSeconds = 0.0;
        LL_INFOS("FSRenderThread") << "Render thread started: GL runs on its own thread, the main thread records" << LL_ENDL;
#else
        LL_WARNS("FSRenderThread") << "FSRenderThread is only implemented on Windows; staying single-threaded" << LL_ENDL;
#endif
    }
}

namespace FSRenderThread
{
    void frameStart()
    {
        if (!sTried)
        {
            sTried = true;
            static LLCachedControl<bool> render_thread(gSavedSettings, "FSRenderThread", false);
            if (render_thread)
            {
                start();
            }
        }
        if (!sActive)
        {
            return;
        }

        static LLCachedControl<bool> check(gSavedSettings, "FSRenderThreadCheck", false);
        FSGLStream::setCheck(check);
        static LLCachedControl<bool> filter(gSavedSettings, "FSRenderThreadFilter", true);
        FSGLStream::setFilter(filter);

        ++sFrames;
        sFrameSeconds += sFrameTimer.getElapsedTimeAndResetF64();
        if (sStatsTimer.getElapsedTimeF32() >= STATS_PERIOD)
        {
            sStatsTimer.reset();
            logStats("Stats:");
        }
    }

    void shutdown()
    {
        if (!sActive)
        {
            return;
        }
#if LL_WINDOWS
        LLWindowWin32::sFSSwapBuffersHook = nullptr;
#endif
        logStats("Final stats:");
        FSGLStream::stop();
        LLImageGLThread::sEnabledTextures = sPrevThreadedTextures;
        LLImageGLThread::sEnabledMedia = sPrevThreadedMedia;
        sActive = false;
    }

    bool isActive()
    {
        return sActive;
    }
}
