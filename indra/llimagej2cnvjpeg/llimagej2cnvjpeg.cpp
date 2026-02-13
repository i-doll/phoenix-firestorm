/**
 * @file llimagej2cnvjpeg.cpp
 * @brief NVIDIA nvJPEG2000 CUDA-accelerated JPEG2000 decoder implementation
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
#include "llimagej2cnvjpeg.h"

#if LL_NVJPEG2K

#include "llcudacontext.h"
#include <nvjpeg2k.h>

// External factory function for OpenJPEG fallback (defined in llimagej2coj.cpp)
extern LLImageJ2CImpl* fallbackCreateLLImageJ2CImpl();

// Factory function: creates nvJPEG2000 implementation
// This is called by createLLImageJ2CImpl() in llimagej2c.cpp when CUDA is available
LLImageJ2CImpl* createLLImageJ2CNVJPEG()
{
    return new LLImageJ2CNVJPEG();
}

LLImageJ2CNVJPEG::LLImageJ2CNVJPEG()
    : LLImageJ2CImpl()
{
}

LLImageJ2CNVJPEG::~LLImageJ2CNVJPEG()
{
}

LLImageJ2CImpl* LLImageJ2CNVJPEG::getOpenJPEGImpl()
{
    if (!mOpenJPEGImpl)
    {
        mOpenJPEGImpl.reset(fallbackCreateLLImageJ2CImpl());
    }
    return mOpenJPEGImpl.get();
}

std::string LLImageJ2CNVJPEG::getEngineInfo() const
{
    if (LLCUDAContext::isCUDAAvailable())
    {
        return LLCUDAContext::getVersionString();
    }
    else
    {
        // Create a temporary impl to get the engine info
        std::unique_ptr<LLImageJ2CImpl> impl(fallbackCreateLLImageJ2CImpl());
        return impl->getEngineInfo() + " (CUDA unavailable)";
    }
}

bool LLImageJ2CNVJPEG::getMetadata(LLImageJ2C &base)
{
    LLCUDAContext* ctx = LLCUDAContext::getThreadContext();
    if (!ctx)
    {
        // Fall back to OpenJPEG
        return getOpenJPEGImpl()->getMetadata(base);
    }

    try
    {
        LLImageDataLock lock(&base);

        // Create nvJPEG2000 stream for parsing
        nvjpeg2kStream_t jpeg2k_stream = nullptr;
        nvjpeg2kStatus_t status = nvjpeg2kStreamCreate(&jpeg2k_stream);
        if (status != NVJPEG2K_STATUS_SUCCESS)
        {
            LL_DEBUGS("NVJPEG2K") << "nvjpeg2kStreamCreate failed: " << status << LL_ENDL;
            return getOpenJPEGImpl()->getMetadata(base);
        }

        // Parse the J2K stream
        status = nvjpeg2kStreamParse(ctx->getDecoder(), base.getData(), base.getDataSize(),
                                      0, 0, jpeg2k_stream);
        if (status != NVJPEG2K_STATUS_SUCCESS)
        {
            // BAD_JPEG (3) is common for incomplete texture data - don't spam logs
            if (status != NVJPEG2K_STATUS_BAD_JPEG)
            {
                LL_DEBUGS("NVJPEG2K") << "nvjpeg2kStreamParse failed: " << status << LL_ENDL;
            }
            nvjpeg2kStreamDestroy(jpeg2k_stream);
            return getOpenJPEGImpl()->getMetadata(base);
        }

        // Get image info
        nvjpeg2kImageInfo_t image_info;
        status = nvjpeg2kStreamGetImageInfo(jpeg2k_stream, &image_info);
        if (status != NVJPEG2K_STATUS_SUCCESS)
        {
            LL_DEBUGS("NVJPEG2K") << "nvjpeg2kStreamGetImageInfo failed: " << status << LL_ENDL;
            nvjpeg2kStreamDestroy(jpeg2k_stream);
            return getOpenJPEGImpl()->getMetadata(base);
        }

        // Set the image dimensions and components
        S32 width = static_cast<S32>(image_info.image_width);
        S32 height = static_cast<S32>(image_info.image_height);
        S32 components = static_cast<S32>(image_info.num_components);

        // Calculate discard level from image dimensions
        S32 discard_level = 0;
        S32 w = width;
        S32 h = height;
        while (w > 1 && h > 1 && discard_level < MAX_DISCARD_LEVEL)
        {
            discard_level++;
            w >>= 1;
            h >>= 1;
        }

        base.mDiscardLevel = discard_level;
        base.setSize(width, height, components);

        nvjpeg2kStreamDestroy(jpeg2k_stream);
        return true;
    }
    catch (...)
    {
        LL_WARNS("NVJPEG2K") << "Exception in nvJPEG2K getMetadata, falling back to OpenJPEG" << LL_ENDL;
        return getOpenJPEGImpl()->getMetadata(base);
    }
}

bool LLImageJ2CNVJPEG::initDecode(LLImageJ2C &base, LLImageRaw &raw_image, int discard_level, int* region)
{
    base.mDiscardLevel = discard_level != -1 ? discard_level : 0;
    return true;
}

bool LLImageJ2CNVJPEG::initEncode(LLImageJ2C &base, LLImageRaw &raw_image, int blocks_size, int precincts_size, int levels)
{
    // Delegate to OpenJPEG
    return getOpenJPEGImpl()->initEncode(base, raw_image, blocks_size, precincts_size, levels);
}

bool LLImageJ2CNVJPEG::decodeImpl(LLImageJ2C &base, LLImageRaw &raw_image, F32 decode_time,
                                   S32 first_channel, S32 max_channel_count)
{
    LL_PROFILE_ZONE_SCOPED_CATEGORY_TEXTURE;

    // Try CUDA decode first
    if (decodeCUDA(base, raw_image, first_channel, max_channel_count))
    {
        return true;
    }

    // Fall back to OpenJPEG
    LL_DEBUGS("NVJPEG2K") << "Falling back to OpenJPEG for decode" << LL_ENDL;
    return getOpenJPEGImpl()->decodeImpl(base, raw_image, decode_time, first_channel, max_channel_count);
}

bool LLImageJ2CNVJPEG::decodeCUDA(LLImageJ2C &base, LLImageRaw &raw_image,
                                   S32 first_channel, S32 max_channel_count)
{
    LLCUDAContext* ctx = LLCUDAContext::getThreadContext();
    if (!ctx)
    {
        return false;
    }

    try
    {
        LLImageDataLock lockIn(&base);
        LLImageDataLock lockOut(&raw_image);

        // Create nvJPEG2000 stream
        nvjpeg2kStream_t jpeg2k_stream = nullptr;
        nvjpeg2kStatus_t status = nvjpeg2kStreamCreate(&jpeg2k_stream);
        if (status != NVJPEG2K_STATUS_SUCCESS)
        {
            LL_DEBUGS("NVJPEG2K") << "nvjpeg2kStreamCreate failed: " << status << LL_ENDL;
            return false;
        }

        // Parse the J2K stream
        S32 data_size = base.getDataSize();
        S32 max_bytes = base.getMaxBytes() ? base.getMaxBytes() : data_size;

        status = nvjpeg2kStreamParse(ctx->getDecoder(), base.getData(), max_bytes,
                                      0, 0, jpeg2k_stream);
        if (status != NVJPEG2K_STATUS_SUCCESS)
        {
            // BAD_JPEG (3) is common for incomplete texture data - don't spam logs
            if (status != NVJPEG2K_STATUS_BAD_JPEG)
            {
                LL_DEBUGS("NVJPEG2K") << "nvjpeg2kStreamParse failed: " << status << LL_ENDL;
            }
            nvjpeg2kStreamDestroy(jpeg2k_stream);
            return false;
        }

        // Get image info
        nvjpeg2kImageInfo_t image_info;
        status = nvjpeg2kStreamGetImageInfo(jpeg2k_stream, &image_info);
        if (status != NVJPEG2K_STATUS_SUCCESS)
        {
            LL_DEBUGS("NVJPEG2K") << "nvjpeg2kStreamGetImageInfo failed: " << status << LL_ENDL;
            nvjpeg2kStreamDestroy(jpeg2k_stream);
            return false;
        }

        // Get number of resolution levels from first tile
        uint32_t num_res = 1;
        status = nvjpeg2kStreamGetResolutionsInTile(jpeg2k_stream, 0, &num_res);
        if (status != NVJPEG2K_STATUS_SUCCESS)
        {
            // Default to 1 if we can't get the info
            num_res = 1;
        }

        // Set reduction factor for discard level
        U32 discard = static_cast<U32>(base.mDiscardLevel);
        U32 reduction = (discard < num_res) ? discard : (num_res > 0 ? num_res - 1 : 0);

        // Calculate output dimensions based on reduction
        U32 output_width = image_info.image_width >> reduction;
        U32 output_height = image_info.image_height >> reduction;
        if (output_width == 0) output_width = 1;
        if (output_height == 0) output_height = 1;

        U32 num_components = image_info.num_components;
        S32 channels = static_cast<S32>(num_components) - first_channel;
        channels = llmin(channels, max_channel_count);
        if (channels <= 0)
        {
            nvjpeg2kStreamDestroy(jpeg2k_stream);
            return false;
        }

        // nvJPEG2000 outputs in planar format (one buffer per component)
        // Allocate device memory for each component separately
        constexpr U32 MAX_COMPONENTS = 4;
        void* d_pixel_data[MAX_COMPONENTS] = {nullptr};
        size_t pitch_in_bytes[MAX_COMPONENTS] = {0};
        size_t component_size = output_width * output_height;

        // Check for previous CUDA errors before allocating
        cudaError_t cuda_err = cudaGetLastError();
        if (cuda_err != cudaSuccess)
        {
            // Clear any previous errors and try to continue
            LL_DEBUGS("NVJPEG2K") << "Clearing previous CUDA error: " << cudaGetErrorString(cuda_err) << LL_ENDL;
            cudaDeviceSynchronize();
            cudaGetLastError(); // Clear error
        }

        for (U32 c = 0; c < num_components && c < MAX_COMPONENTS; c++)
        {
            cuda_err = cudaMalloc(&d_pixel_data[c], component_size);
            if (cuda_err != cudaSuccess)
            {
                LL_DEBUGS("NVJPEG2K") << "cudaMalloc failed for component " << c << ": "
                                     << cudaGetErrorString(cuda_err) << LL_ENDL;
                // Clean up already allocated buffers
                for (U32 i = 0; i < c; i++)
                {
                    cudaFree(d_pixel_data[i]);
                }
                nvjpeg2kStreamDestroy(jpeg2k_stream);
                return false;
            }
            pitch_in_bytes[c] = output_width;  // 1 byte per pixel per component
        }

        // Set up output image structure for planar format
        nvjpeg2kImage_t output_image;
        output_image.pixel_data = d_pixel_data;
        output_image.pitch_in_bytes = pitch_in_bytes;
        output_image.pixel_type = NVJPEG2K_UINT8;
        output_image.num_components = num_components;

        // Decode
        status = nvjpeg2kDecode(ctx->getDecoder(), ctx->getDecodeState(),
                                jpeg2k_stream, &output_image, ctx->getStream());
        if (status != NVJPEG2K_STATUS_SUCCESS)
        {
            LL_DEBUGS("NVJPEG2K") << "nvjpeg2kDecode failed: " << status << LL_ENDL;
            for (U32 c = 0; c < num_components && c < MAX_COMPONENTS; c++)
            {
                cudaFree(d_pixel_data[c]);
            }
            nvjpeg2kStreamDestroy(jpeg2k_stream);
            return false;
        }

        // Synchronize stream
        cuda_err = cudaStreamSynchronize(ctx->getStream());
        if (cuda_err != cudaSuccess)
        {
            LL_DEBUGS("NVJPEG2K") << "cudaStreamSynchronize failed: " << cudaGetErrorString(cuda_err) << LL_ENDL;
            for (U32 c = 0; c < num_components && c < MAX_COMPONENTS; c++)
            {
                cudaFree(d_pixel_data[c]);
            }
            nvjpeg2kStreamDestroy(jpeg2k_stream);
            return false;
        }

        // Copy each component from device to host
        std::vector<std::vector<U8>> h_components(num_components);
        for (U32 c = 0; c < num_components && c < MAX_COMPONENTS; c++)
        {
            h_components[c].resize(component_size);
            cuda_err = cudaMemcpy(h_components[c].data(), d_pixel_data[c], component_size, cudaMemcpyDeviceToHost);
            if (cuda_err != cudaSuccess)
            {
                LL_DEBUGS("NVJPEG2K") << "cudaMemcpy failed for component " << c << ": "
                                     << cudaGetErrorString(cuda_err) << LL_ENDL;
                for (U32 i = 0; i < num_components && i < MAX_COMPONENTS; i++)
                {
                    cudaFree(d_pixel_data[i]);
                }
                nvjpeg2kStreamDestroy(jpeg2k_stream);
                return false;
            }
            cudaFree(d_pixel_data[c]);
        }

        nvjpeg2kStreamDestroy(jpeg2k_stream);

        // Resize the output image
        raw_image.resize(static_cast<U16>(output_width), static_cast<U16>(output_height), static_cast<S8>(channels));

        U8* rawp = raw_image.getData();
        if (!rawp)
        {
            LL_DEBUGS("NVJPEG2K") << "Failed to allocate raw image data" << LL_ENDL;
            base.decodeFailed();
            return true; // Return true to indicate decode is "done" (failed)
        }

        // Convert from planar top-down to interleaved bottom-up
        // nvJPEG2000 outputs top-down, LLImageRaw expects bottom-up
        for (U32 y = 0; y < output_height; y++)
        {
            U32 src_row = y;
            U32 dst_row = output_height - 1 - y;

            U8* dst = rawp + dst_row * output_width * channels;

            for (U32 x = 0; x < output_width; x++)
            {
                size_t src_idx = src_row * output_width + x;
                for (S32 c = 0; c < channels; c++)
                {
                    // Get from the appropriate component plane
                    dst[x * channels + c] = h_components[first_channel + c][src_idx];
                }
            }
        }

        base.setDiscardLevel(reduction);

        return true;
    }
    catch (...)
    {
        LL_WARNS("NVJPEG2K") << "Exception in nvJPEG2K decode, falling back to OpenJPEG" << LL_ENDL;
        return false;
    }
}

bool LLImageJ2CNVJPEG::encodeImpl(LLImageJ2C &base, const LLImageRaw &raw_image,
                                   const char* comment_text, F32 encode_time, bool reversible)
{
    // nvJPEG2000 is decode-only, delegate to OpenJPEG
    return getOpenJPEGImpl()->encodeImpl(base, raw_image, comment_text, encode_time, reversible);
}

#endif // LL_NVJPEG2K
