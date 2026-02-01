/**
 * @file llimagej2cnvjpeg.h
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

#ifndef LL_LLIMAGEJ2CNVJPEG_H
#define LL_LLIMAGEJ2CNVJPEG_H

#include "llimagej2c.h"

#if LL_NVJPEG2K

#include <memory>

/**
 * LLImageJ2CNVJPEG implements JPEG2000 decoding using NVIDIA's nvJPEG2000
 * CUDA-accelerated library with OpenJPEG fallback.
 *
 * This implementation uses nvJPEG2000 for decoding operations when CUDA is
 * available, providing significant performance improvements on NVIDIA GPUs.
 * Encoding is always delegated to OpenJPEG since nvJPEG2000 is decode-only.
 *
 * If CUDA initialization fails or a decode error occurs, the implementation
 * transparently falls back to OpenJPEG.
 */
class LLImageJ2CNVJPEG : public LLImageJ2CImpl
{
public:
    LLImageJ2CNVJPEG();
    virtual ~LLImageJ2CNVJPEG();

protected:
    /**
     * Get image metadata (dimensions, components) from J2C header.
     * Uses nvJPEG2000 to parse the header.
     */
    virtual bool getMetadata(LLImageJ2C &base) override;

    /**
     * Decode JPEG2000 image data to raw pixels.
     * Uses CUDA-accelerated nvJPEG2000, falls back to OpenJPEG on error.
     */
    virtual bool decodeImpl(LLImageJ2C &base, LLImageRaw &raw_image, F32 decode_time,
                           S32 first_channel, S32 max_channel_count) override;

    /**
     * Encode raw pixels to JPEG2000.
     * Always delegates to OpenJPEG (nvJPEG2000 is decode-only).
     */
    virtual bool encodeImpl(LLImageJ2C &base, const LLImageRaw &raw_image,
                           const char* comment_text, F32 encode_time = 0.0,
                           bool reversible = false) override;

    /**
     * Initialize decode parameters.
     */
    virtual bool initDecode(LLImageJ2C &base, LLImageRaw &raw_image,
                           int discard_level = -1, int* region = nullptr) override;

    /**
     * Initialize encode parameters.
     * Delegates to OpenJPEG.
     */
    virtual bool initEncode(LLImageJ2C &base, LLImageRaw &raw_image,
                           int blocks_size = -1, int precincts_size = -1,
                           int levels = 0) override;

    /**
     * Get engine information string.
     */
    virtual std::string getEngineInfo() const override;

private:
    /**
     * Attempt to decode using nvJPEG2000 CUDA decoder.
     * @return true if decode succeeded, false if fallback needed
     */
    bool decodeCUDA(LLImageJ2C &base, LLImageRaw &raw_image,
                    S32 first_channel, S32 max_channel_count);

    /**
     * Get or create the OpenJPEG fallback implementation.
     */
    LLImageJ2CImpl* getOpenJPEGImpl();

    // OpenJPEG implementation for fallback and encoding (lazy initialized)
    std::unique_ptr<LLImageJ2CImpl> mOpenJPEGImpl;
};

#endif // LL_NVJPEG2K

#endif // LL_LLIMAGEJ2CNVJPEG_H
