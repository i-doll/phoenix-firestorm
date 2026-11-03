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
#include "llthread.h" // <FS:Trish/>

#include <algorithm> // <FS:Trish/>

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
    bool mObsolete = false; // <FS:Trish/> skipped, nobody waits for the result
    LLPointer<LLImageDecodeThread::Responder> mResponder;
    std::string mErrorString;};

std::atomic<bool> LLImageDecodeThread::sPrioritize{ true }; // <FS:Trish/>


//----------------------------------------------------------------------------

// <FS:Trish> Decode thread scheduling priority
namespace
{
    std::atomic<bool> sDecodeInBackground{ false };
    // What the current pool thread last applied, so the priority call only happens on a change
    thread_local bool tAppliedBackground = false;

    void applyDecodeThreadPriority()
    {
        const bool background = sDecodeInBackground;
        if (background != tAppliedBackground)
        {
            LLThread::setCurrentThreadBackground(background);
            tAppliedBackground = background;
        }
    }
}

//static
void LLImageDecodeThread::setBackgroundPriority(bool background)
{
    sDecodeInBackground = background;
}
// </FS:Trish>

// MAIN THREAD
LLImageDecodeThread::LLImageDecodeThread(bool /*threaded*/)
    : mDecodeCount(0)
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
    return mThreadPool->getQueue().size();
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

    // Instantiate the ImageRequest right in the lambda, why not?
    // <FS:Trish> Through the priority queue
    //bool posted = mThreadPool->getQueue().post(
    bool posted = enqueue(
    // </FS:Trish>
        [req = ImageRequest(image, discard, needs_aux, responder, decode_id)]
        () mutable
        {
            auto done = req.processRequest();
            req.finishRequest(done);
        }, priority); // <FS:Trish/>
    if (! posted)
    {
        LL_DEBUGS() << "Tried to start decoding on shutdown" << LL_ENDL;
        return 0;
    }

    return decode_id;
}

// <FS:Trish>
bool LLImageDecodeThread::postTask(std::function<void()> task, F32 priority)
{
    return enqueue(std::move(task), priority);
}

// Decodes ran strictly in submission order, so after a teleport or a fast move the textures in
// front of the camera waited behind everything requested on the way there. Pending work now sits
// in a heap and every item posted to the pool takes the most important one at the time it runs.
bool LLImageDecodeThread::enqueue(std::function<void()> task, F32 priority)
{
    if (!sPrioritize)
    {
        return mThreadPool->getQueue().post([task = std::move(task)]()
            {
                applyDecodeThreadPriority();
                task();
            });
    }

    auto less_important = [](const PendingTask& a, const PendingTask& b)
    {
        return a.mPriority != b.mPriority ? a.mPriority < b.mPriority : a.mSeq > b.mSeq;
    };
    {
        std::lock_guard<std::mutex> lock(mPendingMutex);
        mPending.push_back({ priority, mPendingSeq++, std::move(task) });
        std::push_heap(mPending.begin(), mPending.end(), less_important);
    }
    return mThreadPool->getQueue().post([this]() { runNext(); });
}

void LLImageDecodeThread::runNext()
{
    applyDecodeThreadPriority();

    auto less_important = [](const PendingTask& a, const PendingTask& b)
    {
        return a.mPriority != b.mPriority ? a.mPriority < b.mPriority : a.mSeq > b.mSeq;
    };
    std::function<void()> task;
    {
        std::lock_guard<std::mutex> lock(mPendingMutex);
        if (mPending.empty())
        {
            return;
        }
        std::pop_heap(mPending.begin(), mPending.end(), less_important);
        task = std::move(mPending.back().mTask);
        mPending.pop_back();
    }
    task();
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

    // <FS:Trish> The texture moved on (new request, aborted, gone): don't spend a decode on it.
    // finishRequest() still reports, as a failure, which the requester ignores for a stale id.
    if (mResponder.notNull() && mResponder->isObsolete(mRequestId))
    {
        mObsolete = true;
        mErrorString = "obsolete";
        return true;
    }
    // </FS:Trish>

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
