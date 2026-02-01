/**
 * @file llcudacontext.h
 * @brief Thread-safe CUDA context management for nvJPEG2000 decoder
 *
 * $LicenseInfo:firstyear=2024&license=viewerlgpl$
 * Second Life Viewer Source Code
 * Copyright (C) 2024, Linden Research, Inc.
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

#ifndef LL_LLCUDACONTEXT_H
#define LL_LLCUDACONTEXT_H

#include "linden_common.h"

#if LL_NVJPEG2K

#include <cuda_runtime.h>
#include <nvjpeg2k.h>

#include <atomic>
#include <memory>
#include <mutex>
#include <vector>

/**
 * LLCUDAContext manages per-thread CUDA contexts for nvJPEG2000 decoding.
 *
 * Each decode worker thread gets its own CUDA context, stream, and nvJPEG2000
 * decoder state, stored in thread_local storage for efficient access.
 *
 * The class tracks all created contexts globally for cleanup on shutdown.
 */
class LLCUDAContext
{
public:
    ~LLCUDAContext();

    // Non-copyable, non-movable
    LLCUDAContext(const LLCUDAContext&) = delete;
    LLCUDAContext& operator=(const LLCUDAContext&) = delete;
    LLCUDAContext(LLCUDAContext&&) = delete;
    LLCUDAContext& operator=(LLCUDAContext&&) = delete;

    /**
     * Check if CUDA is available on this system.
     * Result is cached after first call.
     * @return true if CUDA is available and initialized
     */
    static bool isCUDAAvailable();

    /**
     * Get the CUDA context for the current thread.
     * Creates one if it doesn't exist yet.
     * @return pointer to thread's CUDA context, or nullptr if CUDA unavailable
     */
    static LLCUDAContext* getThreadContext();

    /**
     * Clean up all CUDA contexts. Call on application shutdown.
     */
    static void cleanupAll();

    /**
     * Get version string for nvJPEG2000
     * @return version info string
     */
    static std::string getVersionString();

    // Accessors for CUDA resources
    cudaStream_t getStream() const { return mStream; }
    nvjpeg2kHandle_t getDecoder() const { return mDecoder; }
    nvjpeg2kDecodeState_t getDecodeState() const { return mDecodeState; }
    nvjpeg2kDecodeParams_t getDecodeParams() const { return mDecodeParams; }

    bool isValid() const { return mValid; }

private:
    LLCUDAContext();
    bool initialize();

    cudaStream_t mStream = nullptr;
    nvjpeg2kHandle_t mDecoder = nullptr;
    nvjpeg2kDecodeState_t mDecodeState = nullptr;
    nvjpeg2kDecodeParams_t mDecodeParams = nullptr;
    bool mValid = false;

    // Thread-local context storage
    static thread_local std::unique_ptr<LLCUDAContext> sThreadContext;

    // CUDA availability: -1 = unchecked, 0 = unavailable, 1 = available
    static std::atomic<int> sCUDAAvailable;

    // Global tracking for cleanup
    static std::mutex sContextListMutex;
    static std::vector<LLCUDAContext*> sAllContexts;
};

#endif // LL_NVJPEG2K

#endif // LL_LLCUDACONTEXT_H
