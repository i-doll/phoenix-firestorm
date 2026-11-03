/**
 * @file llimageworker.cpp
 * @brief Base class for images.
 *
 * $LicenseInfo:firstyear=2001&license=viewerlgpl$
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

#include "llimageworker.h"
#include "llimagedxt.h"
#include "threadpool.h"
// <FS:Trish>
#include "llthread.h"
#include <algorithm>
#include <atomic>
#include <mutex>
// </FS:Trish>

/*--------------------------------------------------------------------------*/
class ImageRequest
{
public:
    ImageRequest(const LLPointer<LLImageFormatted>& image,
                 S32 discard,
                 bool needs_aux,
                 const LLPointer<LLImageDecodeThread::Responder>& responder,
                 U32 request_id);
    virtual ~ImageRequest();

    /*virtual*/ bool processRequest();
    /*virtual*/ void finishRequest(bool completed);

private:
    // LLPointers stored in ImageRequest MUST be LLPointer instances rather
    // than references: we need to increment the refcount when storing these.
    // input
    LLPointer<LLImageFormatted> mFormattedImage;
    S32 mDiscardLevel;
    U32 mRequestId;
    bool mNeedsAux;
    // output
    LLPointer<LLImageRaw> mDecodedImageRaw;
    LLPointer<LLImageRaw> mDecodedImageAux;
    bool mDecodedRaw;
    bool mDecodedAux;
    LLPointer<LLImageDecodeThread::Responder> mResponder;
    std::string mErrorString;};


//----------------------------------------------------------------------------

// <FS:Trish> Decode ordering and decode thread priority
namespace
{
    std::atomic<bool> sDecodeInBackground{ false };
    // What the current pool thread last applied, so the priority call only happens on a change
    thread_local bool tAppliedBackground = false;
}

// The pool's own queue is first come first served. Instead each submitted request waits
// here, and every job posted to the pool just takes the most urgent request at the time a
// thread becomes free. One job is posted per request, so the counts always match.
struct LLImageDecodeThread::PendingDecodes
{
    struct Entry
    {
        F32 mPriority;
        U32 mSequence;
        std::unique_ptr<ImageRequest> mRequest;
    };

    // Heap order: the entry that should run last sorts lowest
    static bool lessUrgent(const Entry& lhs, const Entry& rhs)
    {
        if (lhs.mPriority != rhs.mPriority)
        {
            return lhs.mPriority < rhs.mPriority;
        }
        return lhs.mSequence > rhs.mSequence;
    }

    std::mutex mMutex;
    std::vector<Entry> mHeap;
};

//static
void LLImageDecodeThread::setBackgroundPriority(bool background)
{
    sDecodeInBackground = background;
}
// </FS:Trish>

// MAIN THREAD
LLImageDecodeThread::LLImageDecodeThread(bool /*threaded*/)
    // <FS:Trish>
    //: mDecodeCount(0)
    : mPending(std::make_unique<PendingDecodes>()),
      mDecodeCount(0)
    // </FS:Trish>
{
    mThreadPool = std::make_unique<LL::ThreadPool>("ImageDecode", 8);
    mThreadPool->start();
}

//virtual
LLImageDecodeThread::~LLImageDecodeThread()
{}

// MAIN THREAD
// virtual
size_t LLImageDecodeThread::update(F32 max_time_ms)
{
    LL_PROFILE_ZONE_SCOPED_CATEGORY_TEXTURE;
    return getPending();
}

size_t LLImageDecodeThread::getPending()
{
    // <FS:Trish> Requests not started yet
    //return mThreadPool->getQueue().size();
    std::lock_guard<std::mutex> lock(mPending->mMutex);
    return mPending->mHeap.size();
    // </FS:Trish>
}

LLImageDecodeThread::handle_t LLImageDecodeThread::decodeImage(
    const LLPointer<LLImageFormatted>& image,
    S32 discard,
    bool needs_aux,
    // <FS:Trish>
    //const LLPointer<LLImageDecodeThread::Responder>& responder)
    const LLPointer<LLImageDecodeThread::Responder>& responder,
    F32 priority)
    // </FS:Trish>
{
    LL_PROFILE_ZONE_SCOPED_CATEGORY_TEXTURE;

    U32 decode_id = ++mDecodeCount;
    if (decode_id == 0)
        decode_id = ++mDecodeCount;

    // <FS:Trish> Queue by priority, see PendingDecodes
    //// Instantiate the ImageRequest right in the lambda, why not?
    //bool posted = mThreadPool->getQueue().post(
    //    [req = ImageRequest(image, discard, needs_aux, responder, decode_id)]
    //    () mutable
    //    {
    //        auto done = req.processRequest();
    //        req.finishRequest(done);
    //    });
    {
        std::lock_guard<std::mutex> lock(mPending->mMutex);
        mPending->mHeap.push_back({ priority, decode_id,
                                    std::make_unique<ImageRequest>(image, discard, needs_aux, responder, decode_id) });
        std::push_heap(mPending->mHeap.begin(), mPending->mHeap.end(), PendingDecodes::lessUrgent);
    }
    bool posted = mThreadPool->getQueue().post([this]() { runNextDecode(); });
    // </FS:Trish>
    if (! posted)
    {
        LL_DEBUGS() << "Tried to start decoding on shutdown" << LL_ENDL;
        // <FS:Trish> Nothing will run what is left
        std::lock_guard<std::mutex> lock(mPending->mMutex);
        mPending->mHeap.clear();
        // </FS:Trish>
        return 0;
    }

    return decode_id;
}

// <FS:Trish>
// DECODE THREAD
void LLImageDecodeThread::runNextDecode()
{
    const bool background = sDecodeInBackground;
    if (background != tAppliedBackground)
    {
        LLThread::setCurrentThreadBackground(background);
        tAppliedBackground = background;
    }

    std::unique_ptr<ImageRequest> request;
    {
        std::lock_guard<std::mutex> lock(mPending->mMutex);
        if (mPending->mHeap.empty())
        {
            return;
        }
        std::pop_heap(mPending->mHeap.begin(), mPending->mHeap.end(), PendingDecodes::lessUrgent);
        request = std::move(mPending->mHeap.back().mRequest);
        mPending->mHeap.pop_back();
    }

    bool done = request->processRequest();
    request->finishRequest(done);
}
// </FS:Trish>

void LLImageDecodeThread::shutdown()
{
    mThreadPool->close();
}

LLImageDecodeThread::Responder::~Responder()
{
}

//----------------------------------------------------------------------------

ImageRequest::ImageRequest(const LLPointer<LLImageFormatted>& image,
                           S32 discard,
                           bool needs_aux,
                           const LLPointer<LLImageDecodeThread::Responder>& responder,
                           U32 request_id)
    : mFormattedImage(image),
      mDiscardLevel(discard),
      mNeedsAux(needs_aux),
      mDecodedRaw(false),
      mDecodedAux(false),
      mResponder(responder),
      mRequestId(request_id)
{
}

ImageRequest::~ImageRequest()
{
    mDecodedImageRaw = NULL;
    mDecodedImageAux = NULL;
    mFormattedImage = NULL;
}

//----------------------------------------------------------------------------


// Returns true when done, whether or not decode was successful.
bool ImageRequest::processRequest()
{
    LL_PROFILE_ZONE_SCOPED_CATEGORY_TEXTURE;

    if (mFormattedImage.isNull())
        return true;

    const F32 decode_time_slice = 0.f; //disable time slicing
    bool done = true;

    LLImageDataLock lockFormatted(mFormattedImage);
    LLImageDataLock lockDecodedRaw(mDecodedImageRaw);
    LLImageDataLock lockDecodedAux(mDecodedImageAux);

    if (!mDecodedRaw)
    {
        // Decode primary channels
        if (mDecodedImageRaw.isNull())
        {
            // parse formatted header
            if (!mFormattedImage->updateData())
            {
                return true; // done (failed)
            }
            if ((mFormattedImage->getWidth() * mFormattedImage->getHeight() * mFormattedImage->getComponents()) == 0)
            {
                return true; // done (failed)
            }
            if (mDiscardLevel >= 0)
            {
                mFormattedImage->setDiscardLevel(mDiscardLevel);
            }
            // <FS:Trish> Allocate at the size being decoded. The J2C decoders resize the buffer to the
            // reduced size anyway, so the full size allocation was freed right away (64MB for a 4096
            // RGBA texture decoded at discard 3, to hold 1MB).
            //mDecodedImageRaw = new LLImageRaw(mFormattedImage->getWidth(),
            //                                  mFormattedImage->getHeight(),
            //                                  mFormattedImage->getComponents());
            S32 width = mFormattedImage->getWidth();
            S32 height = mFormattedImage->getHeight();
            const S32 discard = mFormattedImage->getDiscardLevel();
            if (mFormattedImage->getCodec() == IMG_CODEC_J2C && discard > 0)
            {
                const S32 shift = llmin(discard, 15);
                width = llmax(1, (width + (1 << shift) - 1) >> shift);
                height = llmax(1, (height + (1 << shift) - 1) >> shift);
            }
            mDecodedImageRaw = new LLImageRaw(width, height, mFormattedImage->getComponents());
            // </FS:Trish>
        }

        // <FS:ND> Probably out of memory crash
        // done = mFormattedImage->decode(mDecodedImageRaw, decode_time_slice);
        if( mDecodedImageRaw->getData() )
            done = mFormattedImage->decode(mDecodedImageRaw, decode_time_slice);
        else
        {
            LL_WARNS() << "No memory for LLImageRaw of size " << (U32)mFormattedImage->getWidth() << "x" << (U32)mFormattedImage->getHeight() << "x"
                       << (U32)mFormattedImage->getComponents() << LL_ENDL;
            done = false;
        }
        // </FS:ND>

        // some decoders are removing data when task is complete and there were errors
        mDecodedRaw = done && mDecodedImageRaw->getData();

        // <FS:Trish> Alpha mask analysis and pick mask here rather than on the main thread at GL upload
        if (mDecodedRaw)
        {
            mDecodedImageRaw->computeAlphaInfo();
        }
        // </FS:Trish>

        // Pick up errors from decoding
        mErrorString = LLImage::getLastThreadError();
    }
    if (done && mNeedsAux && !mDecodedAux && mFormattedImage.notNull())
    {
        // Decode aux channel
        if (!mDecodedImageAux)
        {
            mDecodedImageAux = new LLImageRaw(mFormattedImage->getWidth(),
                                              mFormattedImage->getHeight(),
                                              1);
        }
        done = mFormattedImage->decodeChannels(mDecodedImageAux, decode_time_slice, 4, 4);
        mDecodedAux = done && mDecodedImageAux->getData();

        // Pick up errors from decoding
        mErrorString = LLImage::getLastThreadError();
    }

    return done;
}

void ImageRequest::finishRequest(bool completed)
{
    LL_PROFILE_ZONE_SCOPED_CATEGORY_TEXTURE;
    if (mResponder.notNull())
    {
        bool success = completed && mDecodedRaw && (!mNeedsAux || mDecodedAux);
        mResponder->completed(success, mErrorString, mDecodedImageRaw, mDecodedImageAux, mRequestId);
    }
    // Will automatically be deleted
}
