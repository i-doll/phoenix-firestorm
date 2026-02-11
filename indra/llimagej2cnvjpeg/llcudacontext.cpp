/**
 * @file llcudacontext.cpp
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

#include "linden_common.h"
#include "llcudacontext.h"

#if LL_NVJPEG2K

// Static member definitions
thread_local std::unique_ptr<LLCUDAContext> LLCUDAContext::sThreadContext;
std::atomic<int> LLCUDAContext::sCUDAAvailable{-1};
std::mutex LLCUDAContext::sContextListMutex;
std::vector<LLCUDAContext*> LLCUDAContext::sAllContexts;

LLCUDAContext::LLCUDAContext()
{
    mValid = initialize();

    if (mValid)
    {
        // Track this context for cleanup
        std::lock_guard<std::mutex> lock(sContextListMutex);
        sAllContexts.push_back(this);
    }
}

LLCUDAContext::~LLCUDAContext()
{
    // Remove from tracking list
    {
        std::lock_guard<std::mutex> lock(sContextListMutex);
        auto it = std::find(sAllContexts.begin(), sAllContexts.end(), this);
        if (it != sAllContexts.end())
        {
            sAllContexts.erase(it);
        }
    }

    // Clean up nvJPEG2000 resources
    if (mDecodeParams)
    {
        nvjpeg2kDecodeParamsDestroy(mDecodeParams);
        mDecodeParams = nullptr;
    }

    if (mDecodeState)
    {
        nvjpeg2kDecodeStateDestroy(mDecodeState);
        mDecodeState = nullptr;
    }

    if (mDecoder)
    {
        nvjpeg2kDestroy(mDecoder);
        mDecoder = nullptr;
    }

    // Clean up CUDA resources
    if (mStream)
    {
        cudaStreamDestroy(mStream);
        mStream = nullptr;
    }
}

bool LLCUDAContext::initialize()
{
    // Initialize CUDA runtime (handles driver init internally)
    int deviceCount = 0;
    cudaError_t cudaResult = cudaGetDeviceCount(&deviceCount);
    if (cudaResult != cudaSuccess || deviceCount == 0)
    {
        LL_WARNS("CUDA") << "No CUDA devices found: " << cudaGetErrorString(cudaResult) << LL_ENDL;
        return false;
    }

    // Set the first device as active
    cudaResult = cudaSetDevice(0);
    if (cudaResult != cudaSuccess)
    {
        LL_WARNS("CUDA") << "cudaSetDevice failed: " << cudaGetErrorString(cudaResult) << LL_ENDL;
        return false;
    }

    // Create CUDA stream
    cudaResult = cudaStreamCreate(&mStream);
    if (cudaResult != cudaSuccess)
    {
        LL_WARNS("CUDA") << "cudaStreamCreate failed: " << cudaGetErrorString(cudaResult) << LL_ENDL;
        return false;
    }

    // Create nvJPEG2000 decoder handle
    nvjpeg2kStatus_t nvResult = nvjpeg2kCreateSimple(&mDecoder);
    if (nvResult != NVJPEG2K_STATUS_SUCCESS)
    {
        LL_WARNS("CUDA") << "nvjpeg2kCreateSimple failed with error " << nvResult << LL_ENDL;
        cudaStreamDestroy(mStream);
        mStream = nullptr;
        return false;
    }

    // Create decode state
    nvResult = nvjpeg2kDecodeStateCreate(mDecoder, &mDecodeState);
    if (nvResult != NVJPEG2K_STATUS_SUCCESS)
    {
        LL_WARNS("CUDA") << "nvjpeg2kDecodeStateCreate failed with error " << nvResult << LL_ENDL;
        nvjpeg2kDestroy(mDecoder);
        mDecoder = nullptr;
        cudaStreamDestroy(mStream);
        mStream = nullptr;
        return false;
    }

    // Create decode parameters
    nvResult = nvjpeg2kDecodeParamsCreate(&mDecodeParams);
    if (nvResult != NVJPEG2K_STATUS_SUCCESS)
    {
        LL_WARNS("CUDA") << "nvjpeg2kDecodeParamsCreate failed with error " << nvResult << LL_ENDL;
        nvjpeg2kDecodeStateDestroy(mDecodeState);
        mDecodeState = nullptr;
        nvjpeg2kDestroy(mDecoder);
        mDecoder = nullptr;
        cudaStreamDestroy(mStream);
        mStream = nullptr;
        return false;
    }

    LL_INFOS("CUDA") << "CUDA context initialized successfully for thread" << LL_ENDL;
    return true;
}

// static
bool LLCUDAContext::isCUDAAvailable()
{
    int expected = -1;
    if (sCUDAAvailable.compare_exchange_strong(expected, 0))
    {
        // First time check - try to initialize CUDA
        int deviceCount = 0;
        cudaError_t result = cudaGetDeviceCount(&deviceCount);
        if (result == cudaSuccess && deviceCount > 0)
        {
            // Check if nvJPEG2000 is available by trying to create a decoder
            nvjpeg2kHandle_t testDecoder = nullptr;
            nvjpeg2kStatus_t nvResult = nvjpeg2kCreateSimple(&testDecoder);
            if (nvResult == NVJPEG2K_STATUS_SUCCESS)
            {
                nvjpeg2kDestroy(testDecoder);
                sCUDAAvailable.store(1);
                LL_INFOS("CUDA") << "CUDA and nvJPEG2000 are available (" << deviceCount << " device(s))" << LL_ENDL;
            }
            else
            {
                LL_WARNS("CUDA") << "nvJPEG2000 initialization failed with error " << nvResult << LL_ENDL;
            }
        }
        else
        {
            LL_INFOS("CUDA") << "No CUDA devices available: " << cudaGetErrorString(result) << LL_ENDL;
        }
    }

    return sCUDAAvailable.load() == 1;
}

// static
LLCUDAContext* LLCUDAContext::getThreadContext()
{
    if (!isCUDAAvailable())
    {
        return nullptr;
    }

    if (!sThreadContext)
    {
        sThreadContext = std::unique_ptr<LLCUDAContext>(new LLCUDAContext());
        if (!sThreadContext->isValid())
        {
            sThreadContext.reset();
            return nullptr;
        }
    }

    return sThreadContext.get();
}

// static
void LLCUDAContext::cleanupAll()
{
    // Clear the thread-local context for this thread
    sThreadContext.reset();

    // Note: Other thread contexts will be cleaned up when their threads exit
    // or when their thread_local storage is destroyed.
    // The sAllContexts list is mainly for debugging/tracking purposes.

    LL_INFOS("CUDA") << "CUDA context cleanup initiated" << LL_ENDL;
}

// static
std::string LLCUDAContext::getVersionString()
{
    // Get nvJPEG2000 version
    uint32_t major = 0, minor = 0, patch = 0;
    nvjpeg2kStatus_t result = nvjpeg2kGetProperty(MAJOR_VERSION, (int*)&major);
    if (result == NVJPEG2K_STATUS_SUCCESS)
    {
        nvjpeg2kGetProperty(MINOR_VERSION, (int*)&minor);
        nvjpeg2kGetProperty(PATCH_LEVEL, (int*)&patch);
    }

    // Get CUDA runtime version
    int cudaRuntimeVersion = 0;
    cudaRuntimeGetVersion(&cudaRuntimeVersion);
    int cudaMajor = cudaRuntimeVersion / 1000;
    int cudaMinor = (cudaRuntimeVersion % 1000) / 10;

    return llformat("nvJPEG2K %u.%u.%u CUDA %d.%d",
                    major, minor, patch, cudaMajor, cudaMinor);
}

#endif // LL_NVJPEG2K
