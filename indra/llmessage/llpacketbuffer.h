/**
 * @file llpacketbuffer.h
 * @brief definition of LLPacketBuffer class for implementing a
 * resend, drop, or delay in packet transmissions.
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

#ifndef LL_LLPACKETBUFFER_H
#define LL_LLPACKETBUFFER_H

#include "net.h"        // for NET_BUFFER_SIZE
#include "llhost.h"

class LLPacketBuffer
{
public:
    LLPacketBuffer(const LLHost &host, const char *datap, const S32 size);
    LLPacketBuffer(S32 hSocket);    // receive a packet
    ~LLPacketBuffer();

    // <FS:Trish> Copy only the used bytes, not the whole 8 KB buffer. The
    // ring copies every buffered packet in on push and out on pop.
    //LLPacketBuffer(const LLPacketBuffer&) = default;
    //LLPacketBuffer& operator=(const LLPacketBuffer&) = default;
    LLPacketBuffer(const LLPacketBuffer& other) { copyFrom(other); }
    LLPacketBuffer& operator=(const LLPacketBuffer& other)
    {
        if (this != &other)
        {
            copyFrom(other);
        }
        return *this;
    }
    // </FS:Trish>

    S32         getSize() const                 { return mSize; }
    const char  *getData() const                { return mData; }
    LLHost      getHost() const                 { return mHost; }
    LLHost      getReceivingInterface() const   { return mReceivingIF; }

    void init(S32 hSocket);
    void init(const char* buffer, S32 data_size, const LLHost& host);

    // Whether LLCircuitData::checkPacketInID() has already been run for this
    // packet (done at socket-read time, before this packet was sorted into
    // the high/low priority inbound queue).
    bool    getPacketIDChecked() const          { return mPacketIDChecked; }
    void    setPacketIDChecked(bool checked)    { mPacketIDChecked = checked; }

    // <FS:Trish> Whether the reliable ack for this packet was already queued
    // with LLCircuitData::collectRAck() at socket-read time.
    bool    getAckCollected() const             { return mAckCollected; }
    void    setAckCollected(bool collected)     { mAckCollected = collected; }
    // </FS:Trish>

protected:
    // <FS:Trish>
    void copyFrom(const LLPacketBuffer& other)
    {
        // mSize can be negative after a failed receive; copy no data then.
        S32 bytes = other.mSize;
        if (bytes < 0)
        {
            bytes = 0;
        }
        else if (bytes > NET_BUFFER_SIZE)
        {
            bytes = NET_BUFFER_SIZE;
        }
        if (bytes > 0)
        {
            memcpy(mData, other.mData, bytes); /* Flawfinder: ignore */
        }
        mSize = other.mSize;
        mHost = other.mHost;
        mReceivingIF = other.mReceivingIF;
        mPacketIDChecked = other.mPacketIDChecked;
        mAckCollected = other.mAckCollected;
    }
    // </FS:Trish>

    char    mData[NET_BUFFER_SIZE]; // packet data       /* Flawfinder : ignore */
    S32     mSize;                  // size of buffer in bytes
    LLHost  mHost;                  // source/dest IP and port
    LLHost  mReceivingIF;           // source/dest IP and port
    bool    mPacketIDChecked = false;
    bool    mAckCollected = false; // <FS:Trish/>
};

#endif


