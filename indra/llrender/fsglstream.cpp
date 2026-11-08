/**
 * @file fsglstream.cpp
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

#include "linden_common.h"

#include "fsglstream.h"

#if LL_WINDOWS

#include "llgl.h"
#include "llglheaders.h"
#include "llprofiler.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <deque>
#include <immintrin.h>
#include <map>
#include <mutex>
#include <string>
#include <new>
#include <thread>
#include <tuple>
#include <type_traits>
#include <unordered_map>
#include <utility>
#include <vector>

namespace
{
    //------------------------------------------------------------------------
    // Entry point table

    enum FSGLId : int
    {
#define FSGL_FN(n) FSGL_ID_##n,
#define FSGL_WGL(n) FSGL_ID_##n,
#include "fsglstream_funcs.inl"
#undef FSGL_FN
#undef FSGL_WGL
        FSGL_COUNT
    };

    const char* const sNames[FSGL_COUNT] =
    {
#define FSGL_FN(n) #n,
#define FSGL_WGL(n) #n,
#include "fsglstream_funcs.inl"
#undef FSGL_FN
#undef FSGL_WGL
    };

    void* sReal[FSGL_COUNT] = {};
    bool sInstalled = false;

    thread_local bool tRecording = false;    // main thread while the stream runs
    thread_local bool tRenderThread = false;

    std::atomic<bool> sCheck{ false };
    std::atomic<bool> sForeignLogged[FSGL_COUNT];
    bool sSyncLogged[FSGL_COUNT] = {};          // main thread
    U32 sSyncCount[FSGL_COUNT] = {};            // main thread
    U32 sCallCount[FSGL_COUNT] = {};            // main thread: calls recorded, per entry point
    U64 sCallBytes[FSGL_COUNT] = {};            // main thread: what they recorded
    U32 sSkipCount[FSGL_COUNT] = {};            // main thread: calls dropped as changing nothing

    // Main thread: recorded calls per GPU zone (LL_PROFILE_GPU_ZONE, innermost), to find which
    // passes make the most calls. Zone names are literals, so keyed by pointer; the last zone's
    // counters are cached, as consecutive calls nearly always share it.
    struct ZoneCalls
    {
        U32 mCalls = 0;
        U32 mDraws = 0;
        U32 mBinds = 0;     // texture binds
    };
    std::unordered_map<const char*, ZoneCalls> sZoneCalls;
    const char* sLastZone = nullptr;
    ZoneCalls* sLastZoneCalls = nullptr;

    constexpr bool isDraw(int id)
    {
        return id == FSGL_ID_glDrawArrays || id == FSGL_ID_glDrawElements || id == FSGL_ID_glDrawRangeElements ||
               id == FSGL_ID_glDrawArraysInstanced || id == FSGL_ID_glDrawElementsInstanced ||
               id == FSGL_ID_glDrawElementsBaseVertex || id == FSGL_ID_glDrawRangeElementsBaseVertex ||
               id == FSGL_ID_glDrawElementsInstancedBaseVertex || id == FSGL_ID_glDrawArraysInstancedBaseInstance ||
               id == FSGL_ID_glDrawElementsInstancedBaseInstance || id == FSGL_ID_glDrawElementsInstancedBaseVertexBaseInstance ||
               id == FSGL_ID_glMultiDrawArraysIndirect || id == FSGL_ID_glMultiDrawElementsIndirect ||
               id == FSGL_ID_glMultiDrawArraysIndirectCount || id == FSGL_ID_glMultiDrawElementsIndirectCount ||
               id == FSGL_ID_glMultiDrawArrays || id == FSGL_ID_glMultiDrawElements ||
               id == FSGL_ID_glMultiDrawElementsBaseVertex || id == FSGL_ID_glDrawArraysIndirect ||
               id == FSGL_ID_glDrawElementsIndirect;
    }

    template <int ID>
    inline void noteCall()
    {
        ++sCallCount[ID];
        const char* zone = LLFSGPUZone::tZone;
        if (zone != sLastZone || !sLastZoneCalls)
        {
            sLastZone = zone;
            sLastZoneCalls = &sZoneCalls[zone];
        }
        ++sLastZoneCalls->mCalls;
        if constexpr (isDraw(ID))
        {
            ++sLastZoneCalls->mDraws;
        }
        if constexpr (ID == FSGL_ID_glBindTexture || ID == FSGL_ID_glBindTextureUnit || ID == FSGL_ID_glBindTextures)
        {
            ++sLastZoneCalls->mBinds;
        }
    }
    U64 sStatSkips = 0;                         // main thread

    // A call that would set state to what it already is: not recorded at all
    bool sFilter = true;                        // main thread: FSRenderThreadFilter

    inline void noteSkip(int id)
    {
        ++sSkipCount[id];
        ++sStatSkips;
    }

    template <typename Fn>
    inline Fn realFn(int id)
    {
        return reinterpret_cast<Fn>(sReal[id]);
    }
#define FSGL_REAL(name) realFn<decltype(::name)>(FSGL_ID_##name)

    //------------------------------------------------------------------------
    // Stats

    using Clock = std::chrono::steady_clock;
    inline U64 nowNs()
    {
        return (U64)std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now().time_since_epoch()).count();
    }

    U64 sStatCalls = 0;             // main
    U64 sStatBytes = 0;             // main
    U64 sStatSyncs = 0;             // main
    U64 sStatSyncWaitNs = 0;        // main
    U64 sStatPresentWaitNs = 0;     // main
    U32 sStatPresents = 0;          // main
    std::atomic<U64> sStatRenderBusyNs{ 0 };
    std::atomic<U64> sStatRenderIdleNs{ 0 };

    //------------------------------------------------------------------------
    // Command buffer

    typedef void (*ExecFn)(U8* payload);

    struct RecHdr
    {
        ExecFn mFn;
        U32 mSize;      // header + payload, multiple of 16
        U32 mPad;
    };
    static_assert(sizeof(RecHdr) == 16, "record header must keep payloads 16 byte aligned");

    constexpr size_t align16(size_t n)
    {
        return (n + 15) & ~size_t(15);
    }

    struct Chunk
    {
        U8* mData = nullptr;
        size_t mCap = 0;
        size_t mUsed = 0;
        bool mStop = false;
    };

    // Chunks are handed over at SUBMIT_BYTES or at every wait, so most are only partly used: a
    // small size keeps the pool's memory in use. Records larger than a chunk (texture and media
    // uploads, buffer data) get a chunk of their own, rounded up to a multiple of CHUNK_SIZE (a
    // power of two would waste up to half of it) and kept for reuse, as allocating and first
    // touching megabytes for every media frame is costly.
    constexpr size_t CHUNK_SIZE = 64 * 1024;
    constexpr size_t SUBMIT_BYTES = 32 * 1024;   // hand chunks over early so replay overlaps recording
    constexpr size_t MAX_FREE_CHUNKS = 256;
    constexpr size_t MAX_FREE_LARGE_CHUNKS = 4;
    constexpr size_t MAX_FREE_LARGE_BYTES = 64 * 1024 * 1024;

    std::mutex sQueueMutex;
    std::condition_variable sQueueCond;
    std::deque<Chunk*> sQueue;
    std::vector<Chunk*> sFreeChunks;
    std::vector<Chunk*> sFreeLargeChunks;
    size_t sFreeLargeBytes = 0;
    Chunk* sCur = nullptr;          // main thread

    size_t largeChunkSize(size_t min_cap)
    {
        return std::max(CHUNK_SIZE * 2, (min_cap + CHUNK_SIZE - 1) / CHUNK_SIZE * CHUNK_SIZE);
    }

    Chunk* allocChunk(size_t min_cap)
    {
        if (min_cap <= CHUNK_SIZE)
        {
            std::lock_guard<std::mutex> lock(sQueueMutex);
            if (!sFreeChunks.empty())
            {
                Chunk* c = sFreeChunks.back();
                sFreeChunks.pop_back();
                c->mUsed = 0;
                return c;
            }
        }
        else
        {
            std::lock_guard<std::mutex> lock(sQueueMutex);
            auto best = sFreeLargeChunks.end();
            for (auto it = sFreeLargeChunks.begin(); it != sFreeLargeChunks.end(); ++it)
            {
                if ((*it)->mCap >= min_cap && (best == sFreeLargeChunks.end() || (*it)->mCap < (*best)->mCap))
                {
                    best = it;
                }
            }
            if (best != sFreeLargeChunks.end())
            {
                Chunk* c = *best;
                sFreeLargeChunks.erase(best);
                sFreeLargeBytes -= c->mCap;
                c->mUsed = 0;
                return c;
            }
        }
        Chunk* c = new Chunk;
        c->mCap = min_cap <= CHUNK_SIZE ? CHUNK_SIZE : largeChunkSize(min_cap);
        c->mData = static_cast<U8*>(::operator new(c->mCap, std::align_val_t(16)));
        return c;
    }

    void freeChunk(Chunk* c)
    {
        if (c->mCap == CHUNK_SIZE)
        {
            std::lock_guard<std::mutex> lock(sQueueMutex);
            if (sFreeChunks.size() < MAX_FREE_CHUNKS)
            {
                sFreeChunks.push_back(c);
                return;
            }
        }
        else if (c->mCap > CHUNK_SIZE)
        {
            std::lock_guard<std::mutex> lock(sQueueMutex);
            if (sFreeLargeChunks.size() < MAX_FREE_LARGE_CHUNKS && sFreeLargeBytes + c->mCap <= MAX_FREE_LARGE_BYTES)
            {
                sFreeLargeChunks.push_back(c);
                sFreeLargeBytes += c->mCap;
                return;
            }
        }
        if (c->mData)
        {
            ::operator delete(c->mData, std::align_val_t(16));
        }
        delete c;
    }

    void submitChunk(Chunk* c)
    {
        {
            std::lock_guard<std::mutex> lock(sQueueMutex);
            sQueue.push_back(c);
        }
        sQueueCond.notify_one();
    }

    bool sQueryEndUnsubmitted = false;  // main: a query end/counter sits in sCur

    void submitCurrent()
    {
        if (sCur && sCur->mUsed)
        {
            submitChunk(sCur);
            sCur = nullptr;
        }
        sQueryEndUnsubmitted = false;
    }

    // Reserves a record; the caller writes the payload and then calls commitRecord().
    U8* allocRecord(ExecFn fn, size_t payload)
    {
        const size_t need = sizeof(RecHdr) + align16(payload);
        if (!sCur || sCur->mUsed + need > sCur->mCap)
        {
            submitCurrent();
            if (!sCur)
            {
                sCur = allocChunk(need);
            }
        }
        RecHdr* hdr = reinterpret_cast<RecHdr*>(sCur->mData + sCur->mUsed);
        hdr->mFn = fn;
        hdr->mSize = (U32)need;
        hdr->mPad = 0;
        sCur->mUsed += need;
        ++sStatCalls;
        sStatBytes += need;
        return reinterpret_cast<U8*>(hdr + 1);
    }

    inline void commitRecord()
    {
        if (sCur->mUsed >= SUBMIT_BYTES)
        {
            submitCurrent();
        }
    }

    void execChunk(Chunk* c)
    {
        U8* p = c->mData;
        U8* end = p + c->mUsed;
        while (p < end)
        {
            RecHdr* hdr = reinterpret_cast<RecHdr*>(p);
            hdr->mFn(reinterpret_cast<U8*>(hdr + 1));
            p += hdr->mSize;
        }
    }

    //------------------------------------------------------------------------
    // Synchronous calls

    struct SyncCtx
    {
        void (*mInvoke)(void*) = nullptr;
        void* mArg = nullptr;
        std::atomic<U32> mDone{ 0 };
    };

    void execSync(U8* p)
    {
        SyncCtx* ctx = *reinterpret_cast<SyncCtx**>(p);
        ctx->mInvoke(ctx->mArg);
        ctx->mDone.store(1, std::memory_order_release);
        ctx->mDone.notify_one();
    }

    template <typename F>
    void runSyncImpl(F& f)
    {
        // Static, not on the stack: the render thread still calls notify_one on it after the
        // store this thread may already have seen and returned on. Only the recording thread
        // gets here, one call at a time.
        static SyncCtx ctx;
        ctx.mDone.store(0, std::memory_order_relaxed);
        ctx.mInvoke = [](void* arg) { (*static_cast<F*>(arg))(); };
        ctx.mArg = &f;
        U8* p = allocRecord(&execSync, sizeof(SyncCtx*));
        *reinterpret_cast<SyncCtx**>(p) = &ctx;
        submitCurrent();

        LL_PROFILE_ZONE_NAMED_CATEGORY_DISPLAY("FSRT sync wait");
        const U64 t0 = nowNs();
        for (U32 spin = 0; spin < 2000 && !ctx.mDone.load(std::memory_order_acquire); ++spin)
        {
            _mm_pause();
        }
        while (!ctx.mDone.load(std::memory_order_acquire))
        {
            ctx.mDone.wait(0, std::memory_order_acquire);
        }
        sStatSyncWaitNs += nowNs() - t0;
    }

    void noteSync(int id)
    {
        ++sStatSyncs;
        ++sSyncCount[id];
        if (sCheck.load(std::memory_order_relaxed) && !sSyncLogged[id])
        {
            sSyncLogged[id] = true;
            LL_INFOS("FSRenderThread") << "FSRenderThreadCheck: first synchronous call of " << sNames[id]
                                       << " (the main thread waits for the render thread)" << LL_ENDL;
        }
    }

    template <typename R, typename F>
    R syncCall(int id, F&& f)
    {
        noteSync(id);
        if constexpr (std::is_void_v<R>)
        {
            auto g = [&]() { f(); };
            runSyncImpl(g);
        }
        else
        {
            R r{};
            auto g = [&]() { r = f(); };
            runSyncImpl(g);
            return r;
        }
    }

    void checkForeign(int id)
    {
        if (tRenderThread || wglGetCurrentContext())
        {
            return;
        }
        if (!sForeignLogged[id].exchange(true))
        {
            LL_WARNS("FSRenderThread") << "FSRenderThreadCheck: " << sNames[id] << " called on thread "
                                       << (U32)GetCurrentThreadId() << ", which has no GL context" << LL_ENDL;
        }
    }

    //------------------------------------------------------------------------
    // Generic proxies

    template <int ID, typename Fn, typename T>
    void execTuple(U8* p)
    {
        std::apply(realFn<Fn>(ID), *reinterpret_cast<T*>(p));
    }

    template <int ID, typename PFN> struct Proxy;

    template <int ID, typename R, typename... A>
    struct Proxy<ID, R (APIENTRY*)(A...)>
    {
        using Fn = R (APIENTRY*)(A...);
        using T = std::tuple<A...>;

        static Fn real()
        {
            return realFn<Fn>(ID);
        }

        static R passthrough(A... a)
        {
            if (sCheck.load(std::memory_order_relaxed))
            {
                checkForeign(ID);
            }
            return real()(a...);
        }

        // Records the call with every argument by value (pointers included).
        static void record(A... a)
        {
            U8* p = allocRecord(&execTuple<ID, Fn, T>, sizeof(T));
            new (p) T(a...);
            noteCall<ID>();
            sCallBytes[ID] += sizeof(RecHdr) + align16(sizeof(T));
            commitRecord();
        }

        static R sync(A... a)
        {
            return syncCall<R>(ID, [&]() -> R { return real()(a...); });
        }

        static R APIENTRY generic(A... a)
        {
            if (!tRecording)
            {
                return passthrough(a...);
            }
            if constexpr (std::is_void_v<R> && (!std::is_pointer_v<A> && ...))
            {
                record(a...);
            }
            else
            {
                return sync(a...);
            }
        }
    };

    // Records a call whose argument I points at `bytes` of client memory; the
    // bytes are copied behind the arguments and the pointer is redirected
    // there on replay.
    template <int ID, typename Fn, typename T, size_t I>
    void execBlob(U8* p)
    {
        T* t = reinterpret_cast<T*>(p);
        using PT = std::tuple_element_t<I, T>;
        if (std::get<I>(*t))
        {
            std::get<I>(*t) = reinterpret_cast<PT>(p + align16(sizeof(T)));
        }
        std::apply(realFn<Fn>(ID), *t);
    }

    template <int ID, size_t I, typename PFN> struct Blob;

    template <int ID, size_t I, typename R, typename... A>
    struct Blob<ID, I, R (APIENTRY*)(A...)>
    {
        using Fn = R (APIENTRY*)(A...);
        using T = std::tuple<A...>;

        static void record(size_t bytes, A... a)
        {
            T t(a...);
            const void* src = (const void*)std::get<I>(t);
            const size_t blob = src ? bytes : 0;
            U8* p = allocRecord(&execBlob<ID, Fn, T, I>, align16(sizeof(T)) + blob);
            new (p) T(t);
            if (blob)
            {
                memcpy(p + align16(sizeof(T)), src, blob);
            }
            noteCall<ID>();
            sCallBytes[ID] += sizeof(RecHdr) + align16(align16(sizeof(T)) + blob);
            commitRecord();
        }
    };

    template <int ID>
    struct Custom
    {
        static constexpr bool kHas = false;
    };

#define FSGL_P(name) Proxy<FSGL_ID_##name, decltype(::name)>
#define FSGL_BLOB(name, idx) Blob<FSGL_ID_##name, idx, decltype(::name)>
#define FSGL_BEGIN(name) template <> struct Custom<FSGL_ID_##name> { static constexpr bool kHas = true; using P = FSGL_P(name);
#define FSGL_END };

    //------------------------------------------------------------------------
    // State the recorder tracks for the main thread's context

    const GLenum PIXEL_STORE_PNAMES[] =
    {
        GL_PACK_ALIGNMENT, GL_PACK_ROW_LENGTH, GL_PACK_SKIP_ROWS, GL_PACK_SKIP_PIXELS, GL_PACK_IMAGE_HEIGHT, GL_PACK_SKIP_IMAGES,
        GL_UNPACK_ALIGNMENT, GL_UNPACK_ROW_LENGTH, GL_UNPACK_SKIP_ROWS, GL_UNPACK_SKIP_PIXELS, GL_UNPACK_IMAGE_HEIGHT, GL_UNPACK_SKIP_IMAGES
    };
    constexpr int NUM_PIXEL_STORE = LL_ARRAY_SIZE(PIXEL_STORE_PNAMES);

    int pixelStoreIndex(GLenum pname)
    {
        for (int i = 0; i < NUM_PIXEL_STORE; ++i)
        {
            if (PIXEL_STORE_PNAMES[i] == pname)
            {
                return i;
            }
        }
        return -1;
    }

#ifndef GL_PARAMETER_BUFFER
#define GL_PARAMETER_BUFFER 0x80EE
#endif
#ifndef GL_PARAMETER_BUFFER_BINDING
#define GL_PARAMETER_BUFFER_BINDING 0x80EF
#endif

    enum BindSlot { B_ARRAY, B_PACK, B_UNPACK, B_INDIRECT, B_PARAM, B_COUNT };
    const GLenum BIND_TARGETS[B_COUNT] = { GL_ARRAY_BUFFER, GL_PIXEL_PACK_BUFFER, GL_PIXEL_UNPACK_BUFFER, GL_DRAW_INDIRECT_BUFFER, GL_PARAMETER_BUFFER };
    const GLenum BIND_QUERIES[B_COUNT] = { GL_ARRAY_BUFFER_BINDING, GL_PIXEL_PACK_BUFFER_BINDING, GL_PIXEL_UNPACK_BUFFER_BINDING, GL_DRAW_INDIRECT_BUFFER_BINDING, GL_PARAMETER_BUFFER_BINDING };

    int bindSlot(GLenum target)
    {
        for (int i = 0; i < B_COUNT; ++i)
        {
            if (BIND_TARGETS[i] == target)
            {
                return i;
            }
        }
        return -1;
    }

    F32 sGLVersion = 0.f;   // context version, from ContextOps

    bool bindQuerySupported(int slot)
    {
        switch (slot)
        {
        case B_INDIRECT: return sGLVersion >= 4.f;
        case B_PARAM: return sGLVersion >= 4.55f;
        default: return true;
        }
    }

    // The shadow's hot lookups are arrays for what the viewer uses (texture units and targets,
    // sampler units, capabilities), with a map for anything else, so the calls made per draw
    // don't hash.

    constexpr GLuint TEX_UNITS = 32;
    constexpr int NUM_TEX_TARGETS = 11;

    int texTargetIndex(GLenum target)
    {
        switch (target)
        {
        case GL_TEXTURE_2D: return 0;
        case GL_TEXTURE_CUBE_MAP: return 1;
        case GL_TEXTURE_2D_ARRAY: return 2;
        case GL_TEXTURE_3D: return 3;
        case GL_TEXTURE_2D_MULTISAMPLE: return 4;
        case GL_TEXTURE_RECTANGLE: return 5;
        case GL_TEXTURE_1D: return 6;
        case GL_TEXTURE_CUBE_MAP_ARRAY: return 7;
        case GL_TEXTURE_BUFFER: return 8;
        case GL_TEXTURE_2D_MULTISAMPLE_ARRAY: return 9;
        case GL_TEXTURE_1D_ARRAY: return 10;
        default: return -1;
        }
    }

    // Texture bound per unit and target, where known
    struct TexBindings
    {
        GLuint mTex[TEX_UNITS][NUM_TEX_TARGETS] = {};
        U16 mKnown[TEX_UNITS] = {};                 // a bit per target index
        std::unordered_map<U64, GLuint> mOther;     // (unit << 32 | target) -> texture

        static U64 key(GLuint unit, GLenum target)
        {
            return ((U64)unit << 32) | (U64)target;
        }

        bool find(GLuint unit, GLenum target, GLuint& texture) const
        {
            const int t = texTargetIndex(target);
            if (unit < TEX_UNITS && t >= 0)
            {
                if (mKnown[unit] & (1u << t))
                {
                    texture = mTex[unit][t];
                    return true;
                }
                return false;
            }
            auto it = mOther.find(key(unit, target));
            if (it == mOther.end())
            {
                return false;
            }
            texture = it->second;
            return true;
        }

        void set(GLuint unit, GLenum target, GLuint texture)
        {
            const int t = texTargetIndex(target);
            if (unit < TEX_UNITS && t >= 0)
            {
                mTex[unit][t] = texture;
                mKnown[unit] |= (U16)(1u << t);
            }
            else
            {
                mOther[key(unit, target)] = texture;
            }
        }

        void forgetUnit(GLuint unit)
        {
            if (unit < TEX_UNITS)
            {
                mKnown[unit] = 0;
            }
            for (auto it = mOther.begin(); it != mOther.end();)
            {
                it = (GLuint)(it->first >> 32) == unit ? mOther.erase(it) : std::next(it);
            }
        }

        void clear()
        {
            memset(mKnown, 0, sizeof(mKnown));
            mOther.clear();
        }

        // a deleted texture leaves 0 bound wherever it was
        void deleted(GLuint texture)
        {
            for (GLuint u = 0; u < TEX_UNITS; ++u)
            {
                const U32 known = mKnown[u];
                if (!known)
                {
                    continue;
                }
                for (int t = 0; t < NUM_TEX_TARGETS; ++t)
                {
                    if ((known & (1u << t)) && mTex[u][t] == texture)
                    {
                        mTex[u][t] = 0;
                    }
                }
            }
            for (auto& [k, bound] : mOther)
            {
                if (bound == texture)
                {
                    bound = 0;
                }
            }
        }
    };

    // Sampler bound per unit, where known
    struct SamplerBindings
    {
        GLuint mSampler[TEX_UNITS] = {};
        U32 mKnown = 0;                                 // a bit per unit
        std::unordered_map<GLuint, GLuint> mOther;      // unit -> sampler

        bool find(GLuint unit, GLuint& sampler) const
        {
            if (unit < TEX_UNITS)
            {
                if (mKnown & (1u << unit))
                {
                    sampler = mSampler[unit];
                    return true;
                }
                return false;
            }
            auto it = mOther.find(unit);
            if (it == mOther.end())
            {
                return false;
            }
            sampler = it->second;
            return true;
        }

        void set(GLuint unit, GLuint sampler)
        {
            if (unit < TEX_UNITS)
            {
                mSampler[unit] = sampler;
                mKnown |= 1u << unit;
            }
            else
            {
                mOther[unit] = sampler;
            }
        }

        void forget(GLuint unit)
        {
            if (unit < TEX_UNITS)
            {
                mKnown &= ~(1u << unit);
            }
            else
            {
                mOther.erase(unit);
            }
        }

        void deleted(GLuint sampler)
        {
            for (GLuint u = 0; u < TEX_UNITS; ++u)
            {
                if ((mKnown & (1u << u)) && mSampler[u] == sampler)
                {
                    mSampler[u] = 0;
                }
            }
            for (auto& [unit, bound] : mOther)
            {
                if (bound == sampler)
                {
                    bound = 0;
                }
            }
        }
    };

    // glIsEnabled answers we know
    constexpr int NUM_CAPS = 20;

    int capIndex(GLenum cap)
    {
        switch (cap)
        {
        case GL_BLEND: return 0;
        case GL_DEPTH_TEST: return 1;
        case GL_CULL_FACE: return 2;
        case GL_SCISSOR_TEST: return 3;
        case GL_STENCIL_TEST: return 4;
        case GL_POLYGON_OFFSET_FILL: return 5;
        case GL_POLYGON_OFFSET_LINE: return 6;
        case GL_LINE_SMOOTH: return 7;
        case GL_MULTISAMPLE: return 8;
        case GL_SAMPLE_ALPHA_TO_COVERAGE: return 9;
        case GL_DEPTH_CLAMP: return 10;
        case GL_TEXTURE_CUBE_MAP_SEAMLESS: return 11;
        case GL_FRAMEBUFFER_SRGB: return 12;
        case GL_PROGRAM_POINT_SIZE: return 13;
        case GL_DITHER: return 14;
        case GL_CLIP_DISTANCE0: return 15;
        case GL_PRIMITIVE_RESTART: return 16;
        case GL_RASTERIZER_DISCARD: return 17;
        case GL_SAMPLE_COVERAGE: return 18;
        case GL_POLYGON_SMOOTH: return 19;
        default: return -1;
        }
    }

    struct CapStates
    {
        bool mKnown[NUM_CAPS] = {};
        bool mOn[NUM_CAPS] = {};
        std::unordered_map<GLenum, bool> mOther;

        bool find(GLenum cap, bool& on) const
        {
            const int i = capIndex(cap);
            if (i >= 0)
            {
                on = mOn[i];
                return mKnown[i];
            }
            auto it = mOther.find(cap);
            if (it == mOther.end())
            {
                return false;
            }
            on = it->second;
            return true;
        }

        void set(GLenum cap, bool on)
        {
            const int i = capIndex(cap);
            if (i >= 0)
            {
                mOn[i] = on;
                mKnown[i] = true;
            }
            else
            {
                mOther[cap] = on;
            }
        }

        void forget(GLenum cap)
        {
            const int i = capIndex(cap);
            if (i >= 0)
            {
                mKnown[i] = false;
            }
            else
            {
                mOther.erase(cap);
            }
        }
    };

    struct Shadow
    {
        bool mViewportOk = false;
        GLint mViewport[4] = {};
        bool mScissorOk = false;
        GLint mScissor[4] = {};
        bool mPolyOk = false;
        GLfloat mPoly[2] = {};
        bool mActiveTexOk = false;
        GLint mActiveTex = 0;
        bool mClearColorOk = false;
        GLfloat mClearColor[4] = {};
        bool mLineWidthOk = false;
        GLfloat mLineWidth = 1.f;
        bool mColorMaskOk = false;
        GLboolean mColorMask[4] = {};
        bool mDepthMaskOk = false;
        GLboolean mDepthMask = GL_TRUE;
        bool mProgramOk = false;
        GLuint mProgram = 0;
        bool mDrawFBOk = false;
        GLuint mDrawFB = 0;
        bool mReadFBOk = false;
        GLuint mReadFB = 0;
        bool mBlendOk = false;
        GLenum mBlend[4] = {};                      // src rgb, dst rgb, src alpha, dst alpha
        bool mDepthFuncOk = false;
        GLenum mDepthFunc = GL_LESS;
        bool mCullFaceOk = false;
        GLenum mCullFace = GL_BACK;
        TexBindings mTex;
        SamplerBindings mSampler;
        bool mPixelStoreOk[NUM_PIXEL_STORE] = {};
        GLint mPixelStore[NUM_PIXEL_STORE] = {};
        bool mBindOk[B_COUNT] = {};
        GLuint mBind[B_COUNT] = {};
        bool mVAOOk = false;
        GLuint mVAO = 0;
        // Element buffer of each VAO we know. The bound VAO's is held in mCurEBO instead (its map
        // entry is stale while mCurEBODirty), so draws and binds don't look it up.
        std::unordered_map<GLuint, GLuint> mEBO;
        bool mCurEBOOk = false;
        bool mCurEBODirty = false;
        GLuint mCurEBO = 0;
        CapStates mCaps;
        bool mClientArrays = false;                 // a vertex attribute was set without an array buffer
    };
    Shadow sSh;     // main thread
    bool sCoreProfile = false;  // main thread: no client-side vertex arrays in this context

    // The bound VAO's element buffer: set (known) or forget (unknown)
    void setCurEBO(GLuint buffer)
    {
        sSh.mCurEBO = buffer;
        sSh.mCurEBOOk = true;
        sSh.mCurEBODirty = true;
    }

    void forgetCurEBO()
    {
        sSh.mCurEBOOk = false;
        sSh.mCurEBODirty = true;
    }

    // Main thread: another VAO is bound. What we know of the old one's element buffer goes back
    // into the map, the new one's comes out of it.
    void switchVAO(GLuint vao)
    {
        if (sSh.mVAOOk && sSh.mCurEBODirty)
        {
            if (sSh.mCurEBOOk)
            {
                sSh.mEBO[sSh.mVAO] = sSh.mCurEBO;
            }
            else
            {
                sSh.mEBO.erase(sSh.mVAO);
            }
        }
        sSh.mVAO = vao;
        sSh.mVAOOk = true;
        auto it = sSh.mEBO.find(vao);
        sSh.mCurEBOOk = it != sSh.mEBO.end();
        sSh.mCurEBO = sSh.mCurEBOOk ? it->second : 0;
        sSh.mCurEBODirty = false;
    }

    // Main thread, direct mode, context current: read what the shadow tracks.
    void snapshotState()
    {
        sSh = Shadow();
        sCoreProfile = false;
        auto getI = FSGL_REAL(glGetIntegerv);
        auto getF = FSGL_REAL(glGetFloatv);
        auto getB = FSGL_REAL(glGetBooleanv);
        auto getErr = FSGL_REAL(glGetError);
        if (!getI || !getF || !getB)
        {
            return;
        }
        if (sGLVersion >= 3.2f)
        {
            GLint profile = 0;
            getI(GL_CONTEXT_PROFILE_MASK, &profile);
            sCoreProfile = (profile & GL_CONTEXT_CORE_PROFILE_BIT) != 0;
        }
        getI(GL_VIEWPORT, sSh.mViewport);
        sSh.mViewportOk = true;
        getI(GL_SCISSOR_BOX, sSh.mScissor);
        sSh.mScissorOk = true;
        getF(GL_POLYGON_OFFSET_FACTOR, &sSh.mPoly[0]);
        getF(GL_POLYGON_OFFSET_UNITS, &sSh.mPoly[1]);
        sSh.mPolyOk = true;
        getI(GL_ACTIVE_TEXTURE, &sSh.mActiveTex);
        sSh.mActiveTexOk = true;
        getF(GL_COLOR_CLEAR_VALUE, sSh.mClearColor);
        sSh.mClearColorOk = true;
        getF(GL_LINE_WIDTH, &sSh.mLineWidth);
        sSh.mLineWidthOk = true;
        getB(GL_COLOR_WRITEMASK, sSh.mColorMask);
        sSh.mColorMaskOk = true;
        getB(GL_DEPTH_WRITEMASK, &sSh.mDepthMask);
        sSh.mDepthMaskOk = true;
        {
            GLint v = 0;
            getI(GL_CURRENT_PROGRAM, &v);
            sSh.mProgram = (GLuint)v;
            sSh.mProgramOk = true;
            getI(GL_DRAW_FRAMEBUFFER_BINDING, &v);
            sSh.mDrawFB = (GLuint)v;
            sSh.mDrawFBOk = true;
            getI(GL_READ_FRAMEBUFFER_BINDING, &v);
            sSh.mReadFB = (GLuint)v;
            sSh.mReadFBOk = true;
            const GLenum blend_pnames[4] = { GL_BLEND_SRC_RGB, GL_BLEND_DST_RGB, GL_BLEND_SRC_ALPHA, GL_BLEND_DST_ALPHA };
            for (int i = 0; i < 4; ++i)
            {
                getI(blend_pnames[i], &v);
                sSh.mBlend[i] = (GLenum)v;
            }
            sSh.mBlendOk = true;
            getI(GL_DEPTH_FUNC, &v);
            sSh.mDepthFunc = (GLenum)v;
            sSh.mDepthFuncOk = true;
            getI(GL_CULL_FACE_MODE, &v);
            sSh.mCullFace = (GLenum)v;
            sSh.mCullFaceOk = true;
        }
        for (int i = 0; i < NUM_PIXEL_STORE; ++i)
        {
            getI(PIXEL_STORE_PNAMES[i], &sSh.mPixelStore[i]);
            sSh.mPixelStoreOk[i] = true;
        }
        for (int i = 0; i < B_COUNT; ++i)
        {
            if (bindQuerySupported(i))
            {
                GLint v = 0;
                getI(BIND_QUERIES[i], &v);
                sSh.mBind[i] = (GLuint)v;
                sSh.mBindOk[i] = true;
            }
        }
        GLint vao = 0, ebo = 0;
        getI(GL_VERTEX_ARRAY_BINDING, &vao);
        getI(GL_ELEMENT_ARRAY_BUFFER_BINDING, &ebo);
        switchVAO((GLuint)vao);
        setCurEBO((GLuint)ebo);
        if (getErr)
        {
            while (getErr() != GL_NO_ERROR)
            {
            }
        }
    }

    enum OffsetKind
    {
        K_DRAW,             // non-indexed draw: only needs no client arrays
        K_EBO,              // indices come from the element buffer
        K_ARRAY,            // vertex attribute pointer is an array buffer offset
        K_INDIRECT,         // draw commands come from the indirect buffer
        K_INDIRECT_EBO,
        K_INDIRECT_EBO_PARAM,
        K_INDIRECT_PARAM,
        K_PACK,             // pixels go to the pack buffer
    };

    bool eboBound()
    {
        return sSh.mVAOOk && sSh.mCurEBOOk && sSh.mCurEBO != 0;
    }

    bool slotBound(int slot)
    {
        return sSh.mBindOk[slot] && sSh.mBind[slot] != 0;
    }

    bool offsetOk(OffsetKind kind)
    {
        switch (kind)
        {
        case K_DRAW: return !sSh.mClientArrays;
        case K_EBO: return !sSh.mClientArrays && eboBound();
        case K_ARRAY: return slotBound(B_ARRAY);
        case K_INDIRECT: return !sSh.mClientArrays && slotBound(B_INDIRECT);
        case K_INDIRECT_EBO: return !sSh.mClientArrays && slotBound(B_INDIRECT) && eboBound();
        case K_INDIRECT_EBO_PARAM: return !sSh.mClientArrays && slotBound(B_INDIRECT) && slotBound(B_PARAM) && eboBound();
        case K_INDIRECT_PARAM: return !sSh.mClientArrays && slotBound(B_INDIRECT) && slotBound(B_PARAM);
        case K_PACK: return slotBound(B_PACK);
        }
        return false;
    }

    // Runs the call synchronously and reads back the bindings the kind
    // depends on, so the next call of the same kind can be deferred.
    struct Heal
    {
        GLint mVAO = -1;
        GLint mEBO = -1;
        GLint mBind[B_COUNT] = { -1, -1, -1, -1, -1 };
    };

    void healRead(OffsetKind kind, Heal& h)
    {
        auto getI = FSGL_REAL(glGetIntegerv);
        auto readSlot = [&](int slot)
        {
            if (bindQuerySupported(slot))
            {
                getI(BIND_QUERIES[slot], &h.mBind[slot]);
            }
        };
        switch (kind)
        {
        case K_EBO:
        case K_INDIRECT_EBO:
        case K_INDIRECT_EBO_PARAM:
            getI(GL_VERTEX_ARRAY_BINDING, &h.mVAO);
            getI(GL_ELEMENT_ARRAY_BUFFER_BINDING, &h.mEBO);
            break;
        default:
            break;
        }
        switch (kind)
        {
        case K_ARRAY: readSlot(B_ARRAY); break;
        case K_INDIRECT:
        case K_INDIRECT_EBO: readSlot(B_INDIRECT); break;
        case K_INDIRECT_EBO_PARAM:
        case K_INDIRECT_PARAM: readSlot(B_INDIRECT); readSlot(B_PARAM); break;
        case K_PACK: readSlot(B_PACK); break;
        default: break;
        }
    }

    void healApply(OffsetKind kind, const Heal& h)
    {
        if (h.mVAO >= 0 && h.mEBO >= 0)
        {
            if (!sSh.mVAOOk || sSh.mVAO != (GLuint)h.mVAO)
            {
                switchVAO((GLuint)h.mVAO);
            }
            setCurEBO((GLuint)h.mEBO);
        }
        for (int i = 0; i < B_COUNT; ++i)
        {
            if (h.mBind[i] >= 0)
            {
                sSh.mBind[i] = (GLuint)h.mBind[i];
                sSh.mBindOk[i] = true;
            }
        }
        if (kind == K_ARRAY && sSh.mBindOk[B_ARRAY] && sSh.mBind[B_ARRAY] == 0 && !sSh.mClientArrays)
        {
            // Client-side vertex arrays exist only in a compatibility context with vertex array 0
            // bound. Anywhere else the call was a GL error (it ran synchronously above) and must not
            // make every later draw synchronous; the viewer never uses client arrays.
            if (!sCoreProfile && (!sSh.mVAOOk || sSh.mVAO == 0))
            {
                // draws read client memory when they run, so from now on every draw is synchronous
                sSh.mClientArrays = true;
                LL_WARNS("FSRenderThread") << "Client-side vertex arrays in use; draws are no longer deferred" << LL_ENDL;
            }
            else
            {
                LL_WARNS_ONCE("FSRenderThread") << "A vertex attribute pointer was set without an array buffer bound (GL error)" << LL_ENDL;
            }
        }
    }

    template <int ID, typename PFN, OffsetKind KIND> struct OffsetCall;

    template <int ID, typename R, typename... A, OffsetKind KIND>
    struct OffsetCall<ID, R (APIENTRY*)(A...), KIND>
    {
        static constexpr bool kHas = true;
        using P = Proxy<ID, R (APIENTRY*)(A...)>;

        static void APIENTRY call(A... a)
        {
            if (!tRecording)
            {
                P::passthrough(a...);
                return;
            }
            if (offsetOk(KIND))
            {
                P::record(a...);
                return;
            }
            Heal h;
            syncCall<void>(ID, [&]()
            {
                P::real()(a...);
                healRead(KIND, h);
            });
            healApply(KIND, h);
        }
    };

#define FSGL_OFFSET(name, kind) template <> struct Custom<FSGL_ID_##name> : OffsetCall<FSGL_ID_##name, decltype(::name), kind> {};

    FSGL_OFFSET(glDrawArrays, K_DRAW)
    FSGL_OFFSET(glDrawArraysInstanced, K_DRAW)
    FSGL_OFFSET(glDrawArraysInstancedBaseInstance, K_DRAW)
    FSGL_OFFSET(glDrawElements, K_EBO)
    FSGL_OFFSET(glDrawRangeElements, K_EBO)
    FSGL_OFFSET(glDrawElementsInstanced, K_EBO)
    FSGL_OFFSET(glDrawElementsBaseVertex, K_EBO)
    FSGL_OFFSET(glDrawRangeElementsBaseVertex, K_EBO)
    FSGL_OFFSET(glDrawElementsInstancedBaseVertex, K_EBO)
    FSGL_OFFSET(glDrawElementsInstancedBaseInstance, K_EBO)
    FSGL_OFFSET(glDrawElementsInstancedBaseVertexBaseInstance, K_EBO)
    FSGL_OFFSET(glVertexAttribPointer, K_ARRAY)
    FSGL_OFFSET(glVertexAttribIPointer, K_ARRAY)
    FSGL_OFFSET(glVertexAttribLPointer, K_ARRAY)
    FSGL_OFFSET(glDrawArraysIndirect, K_INDIRECT)
    FSGL_OFFSET(glMultiDrawArraysIndirect, K_INDIRECT)
    FSGL_OFFSET(glDrawElementsIndirect, K_INDIRECT_EBO)
    FSGL_OFFSET(glMultiDrawElementsIndirect, K_INDIRECT_EBO)
    FSGL_OFFSET(glMultiDrawElementsIndirectCount, K_INDIRECT_EBO_PARAM)
    FSGL_OFFSET(glMultiDrawArraysIndirectCount, K_INDIRECT_PARAM)
    FSGL_OFFSET(glReadPixels, K_PACK)
    FSGL_OFFSET(glReadnPixels, K_PACK)
    FSGL_OFFSET(glGetTexImage, K_PACK)
    FSGL_OFFSET(glGetCompressedTexImage, K_PACK)

    //------------------------------------------------------------------------
    // Tracked state setters

    FSGL_BEGIN(glViewport)
        static void APIENTRY call(GLint x, GLint y, GLsizei w, GLsizei h)
        {
            if (!tRecording) { P::passthrough(x, y, w, h); return; }
            if (sFilter && (sSh.mViewportOk && sSh.mViewport[0] == x && sSh.mViewport[1] == y && sSh.mViewport[2] == w && sSh.mViewport[3] == h))
            {
                noteSkip(FSGL_ID_glViewport);
                return;
            }
            sSh.mViewport[0] = x; sSh.mViewport[1] = y; sSh.mViewport[2] = w; sSh.mViewport[3] = h;
            sSh.mViewportOk = true;
            P::record(x, y, w, h);
        }
    FSGL_END

    FSGL_BEGIN(glScissor)
        static void APIENTRY call(GLint x, GLint y, GLsizei w, GLsizei h)
        {
            if (!tRecording) { P::passthrough(x, y, w, h); return; }
            if (sFilter && (sSh.mScissorOk && sSh.mScissor[0] == x && sSh.mScissor[1] == y && sSh.mScissor[2] == w && sSh.mScissor[3] == h))
            {
                noteSkip(FSGL_ID_glScissor);
                return;
            }
            sSh.mScissor[0] = x; sSh.mScissor[1] = y; sSh.mScissor[2] = w; sSh.mScissor[3] = h;
            sSh.mScissorOk = true;
            P::record(x, y, w, h);
        }
    FSGL_END

    // Indexed and array forms change viewport/scissor 0 too: forget what we know.
    FSGL_BEGIN(glViewportIndexedf)
        static void APIENTRY call(GLuint i, GLfloat x, GLfloat y, GLfloat w, GLfloat h)
        {
            if (tRecording) { sSh.mViewportOk = false; }
            P::generic(i, x, y, w, h);
        }
    FSGL_END

    FSGL_BEGIN(glViewportIndexedfv)
        static void APIENTRY call(GLuint i, const GLfloat* v)
        {
            if (tRecording) { sSh.mViewportOk = false; }
            P::generic(i, v);
        }
    FSGL_END

    FSGL_BEGIN(glViewportArrayv)
        static void APIENTRY call(GLuint first, GLsizei count, const GLfloat* v)
        {
            if (tRecording) { sSh.mViewportOk = false; }
            P::generic(first, count, v);
        }
    FSGL_END

    FSGL_BEGIN(glScissorIndexed)
        static void APIENTRY call(GLuint i, GLint x, GLint y, GLsizei w, GLsizei h)
        {
            if (tRecording) { sSh.mScissorOk = false; }
            P::generic(i, x, y, w, h);
        }
    FSGL_END

    FSGL_BEGIN(glScissorIndexedv)
        static void APIENTRY call(GLuint i, const GLint* v)
        {
            if (tRecording) { sSh.mScissorOk = false; }
            P::generic(i, v);
        }
    FSGL_END

    FSGL_BEGIN(glScissorArrayv)
        static void APIENTRY call(GLuint first, GLsizei count, const GLint* v)
        {
            if (tRecording) { sSh.mScissorOk = false; }
            P::generic(first, count, v);
        }
    FSGL_END

    FSGL_BEGIN(glPolygonOffset)
        static void APIENTRY call(GLfloat factor, GLfloat units)
        {
            if (!tRecording) { P::passthrough(factor, units); return; }
            if (sFilter && (sSh.mPolyOk && sSh.mPoly[0] == factor && sSh.mPoly[1] == units))
            {
                noteSkip(FSGL_ID_glPolygonOffset);
                return;
            }
            sSh.mPoly[0] = factor; sSh.mPoly[1] = units; sSh.mPolyOk = true;
            P::record(factor, units);
        }
    FSGL_END

    FSGL_BEGIN(glPolygonOffsetClamp)
        static void APIENTRY call(GLfloat factor, GLfloat units, GLfloat clamp)
        {
            if (!tRecording) { P::passthrough(factor, units, clamp); return; }
            // the shadow has no clamp: a later glPolygonOffset with the same factor and units
            // resets it to 0 and must not be dropped
            sSh.mPolyOk = false;
            P::record(factor, units, clamp);
        }
    FSGL_END

    FSGL_BEGIN(glActiveTexture)
        static void APIENTRY call(GLenum tex)
        {
            if (!tRecording) { P::passthrough(tex); return; }
            if (sFilter && (sSh.mActiveTexOk && sSh.mActiveTex == (GLint)tex))
            {
                noteSkip(FSGL_ID_glActiveTexture);
                return;
            }
            sSh.mActiveTex = (GLint)tex; sSh.mActiveTexOk = true;
            P::record(tex);
        }
    FSGL_END

    FSGL_BEGIN(glClearColor)
        static void APIENTRY call(GLfloat r, GLfloat g, GLfloat b, GLfloat a)
        {
            if (!tRecording) { P::passthrough(r, g, b, a); return; }
            if (sFilter && (sSh.mClearColorOk && sSh.mClearColor[0] == r && sSh.mClearColor[1] == g && sSh.mClearColor[2] == b && sSh.mClearColor[3] == a))
            {
                noteSkip(FSGL_ID_glClearColor);
                return;
            }
            sSh.mClearColor[0] = r; sSh.mClearColor[1] = g; sSh.mClearColor[2] = b; sSh.mClearColor[3] = a;
            sSh.mClearColorOk = true;
            P::record(r, g, b, a);
        }
    FSGL_END

    FSGL_BEGIN(glLineWidth)
        static void APIENTRY call(GLfloat w)
        {
            if (!tRecording) { P::passthrough(w); return; }
            // GL clamps and may reject values; the shadow only holds what was asked
            // for, so let the driver answer the first read after an unusual value.
            sSh.mLineWidth = w; sSh.mLineWidthOk = (w > 0.f);
            P::record(w);
        }
    FSGL_END

    FSGL_BEGIN(glColorMask)
        static void APIENTRY call(GLboolean r, GLboolean g, GLboolean b, GLboolean a)
        {
            if (!tRecording) { P::passthrough(r, g, b, a); return; }
            if (sFilter && (sSh.mColorMaskOk && sSh.mColorMask[0] == (r ? GL_TRUE : GL_FALSE) && sSh.mColorMask[1] == (g ? GL_TRUE : GL_FALSE) &&
                sSh.mColorMask[2] == (b ? GL_TRUE : GL_FALSE) && sSh.mColorMask[3] == (a ? GL_TRUE : GL_FALSE)))
            {
                noteSkip(FSGL_ID_glColorMask);
                return;
            }
            sSh.mColorMask[0] = r ? GL_TRUE : GL_FALSE; sSh.mColorMask[1] = g ? GL_TRUE : GL_FALSE;
            sSh.mColorMask[2] = b ? GL_TRUE : GL_FALSE; sSh.mColorMask[3] = a ? GL_TRUE : GL_FALSE;
            sSh.mColorMaskOk = true;
            P::record(r, g, b, a);
        }
    FSGL_END

    FSGL_BEGIN(glColorMaski)
        static void APIENTRY call(GLuint i, GLboolean r, GLboolean g, GLboolean b, GLboolean a)
        {
            // any draw buffer: glColorMask sets them all, so it can't be dropped while one differs
            if (tRecording) { sSh.mColorMaskOk = false; }
            P::generic(i, r, g, b, a);
        }
    FSGL_END

    // Tracked so GL_DEPTH_WRITEMASK queries need no wait
    FSGL_BEGIN(glDepthMask)
        static void APIENTRY call(GLboolean flag)
        {
            if (!tRecording) { P::passthrough(flag); return; }
            if (sFilter && (sSh.mDepthMaskOk && sSh.mDepthMask == (flag ? GL_TRUE : GL_FALSE)))
            {
                noteSkip(FSGL_ID_glDepthMask);
                return;
            }
            sSh.mDepthMask = flag ? GL_TRUE : GL_FALSE;
            sSh.mDepthMaskOk = true;
            P::record(flag);
        }
    FSGL_END

    FSGL_BEGIN(glPixelStorei)
        static void APIENTRY call(GLenum pname, GLint v)
        {
            if (!tRecording) { P::passthrough(pname, v); return; }
            const int idx = pixelStoreIndex(pname);
            if (idx >= 0)
            {
                // Out of range values raise an error and change nothing; let the
                // driver answer reads after one.
                const bool align = (pname == GL_PACK_ALIGNMENT || pname == GL_UNPACK_ALIGNMENT);
                const bool valid = align ? (v == 1 || v == 2 || v == 4 || v == 8) : (v >= 0);
                sSh.mPixelStore[idx] = v;
                sSh.mPixelStoreOk[idx] = valid;
            }
            P::record(pname, v);
        }
    FSGL_END

    FSGL_BEGIN(glPixelStoref)
        static void APIENTRY call(GLenum pname, GLfloat v)
        {
            if (tRecording)
            {
                const int idx = pixelStoreIndex(pname);
                if (idx >= 0)
                {
                    sSh.mPixelStoreOk[idx] = false;
                }
            }
            P::generic(pname, v);
        }
    FSGL_END

    FSGL_BEGIN(glEnable)
        static void APIENTRY call(GLenum cap)
        {
            if (!tRecording) { P::passthrough(cap); return; }
            bool on = false;
            if (sFilter && (sSh.mCaps.find(cap, on) && on))
            {
                noteSkip(FSGL_ID_glEnable);
                return;
            }
            sSh.mCaps.set(cap, true);
            P::record(cap);
        }
    FSGL_END

    FSGL_BEGIN(glDisable)
        static void APIENTRY call(GLenum cap)
        {
            if (!tRecording) { P::passthrough(cap); return; }
            bool on = true;
            if (sFilter && (sSh.mCaps.find(cap, on) && !on))
            {
                noteSkip(FSGL_ID_glDisable);
                return;
            }
            sSh.mCaps.set(cap, false);
            P::record(cap);
        }
    FSGL_END

    FSGL_BEGIN(glEnablei)
        static void APIENTRY call(GLenum cap, GLuint i)
        {
            if (tRecording) { sSh.mCaps.forget(cap); }
            P::generic(cap, i);
        }
    FSGL_END

    FSGL_BEGIN(glDisablei)
        static void APIENTRY call(GLenum cap, GLuint i)
        {
            if (tRecording) { sSh.mCaps.forget(cap); }
            P::generic(cap, i);
        }
    FSGL_END

    FSGL_BEGIN(glIsEnabled)
        static GLboolean APIENTRY call(GLenum cap)
        {
            if (!tRecording) { return P::passthrough(cap); }
            bool on = false;
            if (sSh.mCaps.find(cap, on))
            {
                return on ? GL_TRUE : GL_FALSE;
            }
            GLboolean r = P::sync(cap);
            sSh.mCaps.set(cap, r != GL_FALSE);
            return r;
        }
    FSGL_END

    FSGL_BEGIN(glBindBuffer)
        static void APIENTRY call(GLenum target, GLuint buffer)
        {
            if (!tRecording) { P::passthrough(target, buffer); return; }
            if (target == GL_ELEMENT_ARRAY_BUFFER)
            {
                if (sSh.mVAOOk)
                {
                    if (sFilter && (sSh.mCurEBOOk && sSh.mCurEBO == buffer))
                    {
                        noteSkip(FSGL_ID_glBindBuffer);
                        return;
                    }
                    setCurEBO(buffer);
                }
            }
            else
            {
                const int slot = bindSlot(target);
                if (slot >= 0)
                {
                    if (sFilter && (sSh.mBindOk[slot] && sSh.mBind[slot] == buffer))
                    {
                        noteSkip(FSGL_ID_glBindBuffer);
                        return;
                    }
                    sSh.mBind[slot] = buffer;
                    sSh.mBindOk[slot] = true;
                }
            }
            P::record(target, buffer);
        }
    FSGL_END

    FSGL_BEGIN(glBindVertexArray)
        static void APIENTRY call(GLuint vao)
        {
            if (!tRecording) { P::passthrough(vao); return; }
            if (sFilter && (sSh.mVAOOk && sSh.mVAO == vao))
            {
                noteSkip(FSGL_ID_glBindVertexArray);
                return;
            }
            switchVAO(vao);
            P::record(vao);
        }
    FSGL_END

    FSGL_BEGIN(glVertexArrayElementBuffer)
        static void APIENTRY call(GLuint vao, GLuint buffer)
        {
            if (!tRecording) { P::passthrough(vao, buffer); return; }
            if (sSh.mVAOOk && sSh.mVAO == vao)
            {
                setCurEBO(buffer);
            }
            else
            {
                sSh.mEBO[vao] = buffer;
            }
            P::record(vao, buffer);
        }
    FSGL_END

    //------------------------------------------------------------------------
    // More tracked state: bindings and render state set far more often than changed

    FSGL_BEGIN(glUseProgram)
        static void APIENTRY call(GLuint program)
        {
            if (!tRecording) { P::passthrough(program); return; }
            // a relinked program in use runs its new code without being used again, so the
            // same name is always the same state
            if (sFilter && (sSh.mProgramOk && sSh.mProgram == program))
            {
                noteSkip(FSGL_ID_glUseProgram);
                return;
            }
            sSh.mProgram = program;
            sSh.mProgramOk = true;
            P::record(program);
        }
    FSGL_END

    FSGL_BEGIN(glBindTexture)
        static void APIENTRY call(GLenum target, GLuint texture)
        {
            if (!tRecording) { P::passthrough(target, texture); return; }
            if (sSh.mActiveTexOk)
            {
                const GLuint unit = (GLuint)(sSh.mActiveTex - GL_TEXTURE0);
                GLuint bound = 0;
                if (sFilter && (sSh.mTex.find(unit, target, bound) && bound == texture))
                {
                    noteSkip(FSGL_ID_glBindTexture);
                    return;
                }
                sSh.mTex.set(unit, target, texture);
            }
            else
            {
                sSh.mTex.clear(); // which unit this binds is unknown
            }
            P::record(target, texture);
        }
    FSGL_END

    // The target of a DSA bind is the texture's own, which isn't known here: forget the unit
    void forgetTextureUnit(GLuint unit)
    {
        sSh.mTex.forgetUnit(unit);
    }

    FSGL_BEGIN(glBindTextureUnit)
        static void APIENTRY call(GLuint unit, GLuint texture)
        {
            if (tRecording) { forgetTextureUnit(unit); }
            P::generic(unit, texture);
        }
    FSGL_END

    FSGL_BEGIN(glBindTextures)
        static void APIENTRY call(GLuint first, GLsizei count, const GLuint* textures)
        {
            if (!tRecording) { P::passthrough(first, count, textures); return; }
            for (GLsizei i = 0; i < count; ++i)
            {
                forgetTextureUnit(first + (GLuint)i);
            }
            FSGL_BLOB(glBindTextures, 2)::record(size_t(count > 0 ? count : 0) * sizeof(GLuint), first, count, textures);
        }
    FSGL_END

    FSGL_BEGIN(glBindFramebuffer)
        static void APIENTRY call(GLenum target, GLuint fb)
        {
            if (!tRecording) { P::passthrough(target, fb); return; }
            const bool draw = target == GL_FRAMEBUFFER || target == GL_DRAW_FRAMEBUFFER;
            const bool read = target == GL_FRAMEBUFFER || target == GL_READ_FRAMEBUFFER;
            if (sFilter && ((!draw || (sSh.mDrawFBOk && sSh.mDrawFB == fb)) && (!read || (sSh.mReadFBOk && sSh.mReadFB == fb)) && (draw || read)))
            {
                noteSkip(FSGL_ID_glBindFramebuffer);
                return;
            }
            if (draw)
            {
                sSh.mDrawFB = fb;
                sSh.mDrawFBOk = true;
            }
            if (read)
            {
                sSh.mReadFB = fb;
                sSh.mReadFBOk = true;
            }
            P::record(target, fb);
        }
    FSGL_END

    FSGL_BEGIN(glBindSampler)
        static void APIENTRY call(GLuint unit, GLuint sampler)
        {
            if (!tRecording) { P::passthrough(unit, sampler); return; }
            GLuint bound = 0;
            if (sFilter && (sSh.mSampler.find(unit, bound) && bound == sampler))
            {
                noteSkip(FSGL_ID_glBindSampler);
                return;
            }
            sSh.mSampler.set(unit, sampler);
            P::record(unit, sampler);
        }
    FSGL_END

    FSGL_BEGIN(glBindSamplers)
        static void APIENTRY call(GLuint first, GLsizei count, const GLuint* samplers)
        {
            if (!tRecording) { P::passthrough(first, count, samplers); return; }
            for (GLsizei i = 0; i < count; ++i)
            {
                sSh.mSampler.forget(first + (GLuint)i);
            }
            FSGL_BLOB(glBindSamplers, 2)::record(size_t(count > 0 ? count : 0) * sizeof(GLuint), first, count, samplers);
        }
    FSGL_END

    FSGL_BEGIN(glBlendFuncSeparate)
        static void APIENTRY call(GLenum src_rgb, GLenum dst_rgb, GLenum src_a, GLenum dst_a)
        {
            if (!tRecording) { P::passthrough(src_rgb, dst_rgb, src_a, dst_a); return; }
            if (sFilter && (sSh.mBlendOk && sSh.mBlend[0] == src_rgb && sSh.mBlend[1] == dst_rgb && sSh.mBlend[2] == src_a && sSh.mBlend[3] == dst_a))
            {
                noteSkip(FSGL_ID_glBlendFuncSeparate);
                return;
            }
            sSh.mBlend[0] = src_rgb; sSh.mBlend[1] = dst_rgb; sSh.mBlend[2] = src_a; sSh.mBlend[3] = dst_a;
            sSh.mBlendOk = true;
            P::record(src_rgb, dst_rgb, src_a, dst_a);
        }
    FSGL_END

    FSGL_BEGIN(glBlendFunc)
        static void APIENTRY call(GLenum src, GLenum dst)
        {
            if (!tRecording) { P::passthrough(src, dst); return; }
            if (sFilter && (sSh.mBlendOk && sSh.mBlend[0] == src && sSh.mBlend[1] == dst && sSh.mBlend[2] == src && sSh.mBlend[3] == dst))
            {
                noteSkip(FSGL_ID_glBlendFunc);
                return;
            }
            sSh.mBlend[0] = src; sSh.mBlend[1] = dst; sSh.mBlend[2] = src; sSh.mBlend[3] = dst;
            sSh.mBlendOk = true;
            P::record(src, dst);
        }
    FSGL_END

    FSGL_BEGIN(glDepthFunc)
        static void APIENTRY call(GLenum func)
        {
            if (!tRecording) { P::passthrough(func); return; }
            if (sFilter && (sSh.mDepthFuncOk && sSh.mDepthFunc == func))
            {
                noteSkip(FSGL_ID_glDepthFunc);
                return;
            }
            sSh.mDepthFunc = func;
            sSh.mDepthFuncOk = true;
            P::record(func);
        }
    FSGL_END

    FSGL_BEGIN(glCullFace)
        static void APIENTRY call(GLenum mode)
        {
            if (!tRecording) { P::passthrough(mode); return; }
            if (sFilter && (sSh.mCullFaceOk && sSh.mCullFace == mode))
            {
                noteSkip(FSGL_ID_glCullFace);
                return;
            }
            sSh.mCullFace = mode;
            sSh.mCullFaceOk = true;
            P::record(mode);
        }
    FSGL_END

    //------------------------------------------------------------------------
    // Uniform values of up to 64 bytes (scalars, vectors, matrices, short arrays), per program
    // and location: setting one to what it already holds isn't recorded. The viewer caches its
    // scalar uniforms itself, but not vectors and matrices (model and texture matrices per draw).
    // Every way a uniform of the current program can be set goes through here, and linking,
    // loading a binary or deleting a program forgets its values (forgetProgram).

    struct UniformValue
    {
        U8 mSize = 0;           // 0: not known
        U8 mBytes[72] = {};
    };

    // A program's values: a vector indexed by location (locations are small and dense), a map for
    // any beyond DENSE_LOCATIONS
    struct ProgramUniforms
    {
        static constexpr GLint DENSE_LOCATIONS = 1024;
        std::vector<UniformValue> mDense;
        std::unordered_map<GLint, UniformValue> mSparse;

        UniformValue& get(GLint loc)
        {
            if (loc < DENSE_LOCATIONS)
            {
                if ((size_t)loc >= mDense.size())
                {
                    mDense.resize((size_t)loc + 1);
                }
                return mDense[loc];
            }
            return mSparse[loc];
        }

        void forget(GLint loc)
        {
            if (loc < DENSE_LOCATIONS)
            {
                if ((size_t)loc < mDense.size())
                {
                    mDense[loc].mSize = 0;
                }
            }
            else
            {
                mSparse.erase(loc);
            }
        }
    };
    std::unordered_map<GLuint, ProgramUniforms> sUniformValues; // main

    // The current program's entry, looked up once per program change rather than per uniform.
    // Map nodes don't move; forgetProgram and clearUniformValues drop the pointer with the node.
    ProgramUniforms* sCurUniforms = nullptr;    // main
    GLuint sCurUniformsProgram = 0;             // main

    ProgramUniforms& currentUniforms()
    {
        if (!sCurUniforms || sCurUniformsProgram != sSh.mProgram)
        {
            sCurUniforms = &sUniformValues[sSh.mProgram];
            sCurUniformsProgram = sSh.mProgram;
        }
        return *sCurUniforms;
    }

    void clearUniformValues()
    {
        sUniformValues.clear();
        sCurUniforms = nullptr;
    }

    void eraseUniformValues(GLuint program)
    {
        sUniformValues.erase(program);
        if (sCurUniformsProgram == program)
        {
            sCurUniforms = nullptr;
        }
    }

    // An array set as a whole: its elements are locations of their own, set one by one elsewhere,
    // so the whole array isn't cached and every element it covers is forgotten
    void forgetUniforms(GLint loc, GLsizei count)
    {
        if (!sSh.mProgramOk || loc < 0)
        {
            return;
        }
        auto it = sUniformValues.find(sSh.mProgram);
        if (it != sUniformValues.end())
        {
            for (GLsizei i = 0; i < count; ++i)
            {
                it->second.forget(loc + i);
            }
        }
    }

    // True when the uniform at loc of the current program already holds these bytes; otherwise
    // remembers them (or, when they can't be compared, forgets the location)
    bool uniformUnchanged(GLint loc, const void* data, size_t bytes, U8 extra = 0)
    {
        if (!sSh.mProgramOk || sSh.mProgram == 0 || loc < 0)
        {
            return false;
        }
        ProgramUniforms& per_program = currentUniforms();
        if (!data || bytes == 0 || bytes > 64)
        {
            per_program.forget(loc);
            return false;
        }
        UniformValue& v = per_program.get(loc);
        const U8 size = (U8)(bytes + 1);
        if (v.mSize == size && v.mBytes[bytes] == extra && memcmp(v.mBytes, data, bytes) == 0)
        {
            return true;
        }
        v.mSize = size;
        memcpy(v.mBytes, data, bytes);
        v.mBytes[bytes] = extra;
        return false;
    }

#define FSGL_UNIFORMS(name, T, N) \
    FSGL_BEGIN(name) \
        template <typename... A> static void set(GLint loc, A... a) \
        { \
            if (!tRecording) { P::passthrough(loc, a...); return; } \
            const T v[N] = { a... }; \
            if (sFilter && (uniformUnchanged(loc, v, sizeof(v)))) \
            { \
                noteSkip(FSGL_ID_##name); \
                return; \
            } \
            P::record(loc, a...); \
        }

    FSGL_UNIFORMS(glUniform1f, GLfloat, 1)
        static void APIENTRY call(GLint loc, GLfloat x) { set(loc, x); }
    FSGL_END
    FSGL_UNIFORMS(glUniform2f, GLfloat, 2)
        static void APIENTRY call(GLint loc, GLfloat x, GLfloat y) { set(loc, x, y); }
    FSGL_END
    FSGL_UNIFORMS(glUniform3f, GLfloat, 3)
        static void APIENTRY call(GLint loc, GLfloat x, GLfloat y, GLfloat z) { set(loc, x, y, z); }
    FSGL_END
    FSGL_UNIFORMS(glUniform4f, GLfloat, 4)
        static void APIENTRY call(GLint loc, GLfloat x, GLfloat y, GLfloat z, GLfloat w) { set(loc, x, y, z, w); }
    FSGL_END
    FSGL_UNIFORMS(glUniform1i, GLint, 1)
        static void APIENTRY call(GLint loc, GLint x) { set(loc, x); }
    FSGL_END
    FSGL_UNIFORMS(glUniform2i, GLint, 2)
        static void APIENTRY call(GLint loc, GLint x, GLint y) { set(loc, x, y); }
    FSGL_END
    FSGL_UNIFORMS(glUniform3i, GLint, 3)
        static void APIENTRY call(GLint loc, GLint x, GLint y, GLint z) { set(loc, x, y, z); }
    FSGL_END
    FSGL_UNIFORMS(glUniform4i, GLint, 4)
        static void APIENTRY call(GLint loc, GLint x, GLint y, GLint z, GLint w) { set(loc, x, y, z, w); }
    FSGL_END

    //------------------------------------------------------------------------
    // Client memory copied at record time

#define FSGL_UNIFORMV(name, T, N) \
    FSGL_BEGIN(name) \
        static void APIENTRY call(GLint loc, GLsizei count, const T* v) \
        { \
            if (!tRecording) { P::passthrough(loc, count, v); return; } \
            const size_t bytes = size_t(count > 0 ? count : 0) * (N) * sizeof(T); \
            if (count != 1) \
            { \
                forgetUniforms(loc, count); \
            } \
            else if (sFilter && (uniformUnchanged(loc, v, bytes))) \
            { \
                noteSkip(FSGL_ID_##name); \
                return; \
            } \
            FSGL_BLOB(name, 2)::record(bytes, loc, count, v); \
        } \
    FSGL_END

#define FSGL_UNIFORMMATRIXV(name, N) \
    FSGL_BEGIN(name) \
        static void APIENTRY call(GLint loc, GLsizei count, GLboolean transpose, const GLfloat* v) \
        { \
            if (!tRecording) { P::passthrough(loc, count, transpose, v); return; } \
            const size_t bytes = size_t(count > 0 ? count : 0) * (N) * sizeof(GLfloat); \
            if (count != 1) \
            { \
                forgetUniforms(loc, count); \
            } \
            else if (sFilter && (uniformUnchanged(loc, v, bytes, transpose ? 1 : 2))) \
            { \
                noteSkip(FSGL_ID_##name); \
                return; \
            } \
            FSGL_BLOB(name, 3)::record(bytes, loc, count, transpose, v); \
        } \
    FSGL_END

    FSGL_UNIFORMV(glUniform1fv, GLfloat, 1)
    FSGL_UNIFORMV(glUniform2fv, GLfloat, 2)
    FSGL_UNIFORMV(glUniform3fv, GLfloat, 3)
    FSGL_UNIFORMV(glUniform4fv, GLfloat, 4)
    FSGL_UNIFORMV(glUniform1iv, GLint, 1)
    FSGL_UNIFORMV(glUniform2iv, GLint, 2)
    FSGL_UNIFORMV(glUniform3iv, GLint, 3)
    FSGL_UNIFORMV(glUniform4iv, GLint, 4)
    FSGL_UNIFORMV(glUniform1uiv, GLuint, 1)
    FSGL_UNIFORMV(glUniform2uiv, GLuint, 2)
    FSGL_UNIFORMV(glUniform3uiv, GLuint, 3)
    FSGL_UNIFORMV(glUniform4uiv, GLuint, 4)
    FSGL_UNIFORMMATRIXV(glUniformMatrix2fv, 4)
    FSGL_UNIFORMMATRIXV(glUniformMatrix3fv, 9)
    FSGL_UNIFORMMATRIXV(glUniformMatrix4fv, 16)
    FSGL_UNIFORMMATRIXV(glUniformMatrix2x3fv, 6)
    FSGL_UNIFORMMATRIXV(glUniformMatrix3x2fv, 6)
    FSGL_UNIFORMMATRIXV(glUniformMatrix2x4fv, 8)
    FSGL_UNIFORMMATRIXV(glUniformMatrix4x2fv, 8)
    FSGL_UNIFORMMATRIXV(glUniformMatrix3x4fv, 12)
    FSGL_UNIFORMMATRIXV(glUniformMatrix4x3fv, 12)

    // Uniform setters the cache doesn't compare still change what a location holds: forget it
    // (glProgramUniform*: in the named program's cache), or a later set to the cached value
    // would be dropped
    void forgetUniformsOf(GLuint program, GLint loc, GLsizei count)
    {
        if (loc < 0)
        {
            return;
        }
        auto it = sUniformValues.find(program);
        if (it == sUniformValues.end())
        {
            return;
        }
        count = std::min<GLsizei>(std::max<GLsizei>(count, 1), 4096);
        for (GLsizei i = 0; i < count; ++i)
        {
            it->second.forget(loc + i);
        }
    }

    // PROG, LOC and COUNT: argument indices of the program, location and element count (-1: none)
    template <int ID, typename PFN, int PROG, int LOC, int COUNT> struct ForgetUniformCall;

    template <int ID, typename R, typename... A, int PROG, int LOC, int COUNT>
    struct ForgetUniformCall<ID, R (APIENTRY*)(A...), PROG, LOC, COUNT>
    {
        static constexpr bool kHas = true;
        using P = Proxy<ID, R (APIENTRY*)(A...)>;

        static R APIENTRY call(A... a)
        {
            if (tRecording)
            {
                const std::tuple<A...> t(a...);
                const GLint loc = (GLint)std::get<LOC>(t);
                GLsizei count = 1;
                if constexpr (COUNT >= 0)
                {
                    count = (GLsizei)std::get<COUNT>(t);
                }
                if constexpr (PROG >= 0)
                {
                    forgetUniformsOf((GLuint)std::get<PROG>(t), loc, count);
                }
                else if (sSh.mProgramOk)
                {
                    forgetUniformsOf(sSh.mProgram, loc, count);
                }
                else
                {
                    clearUniformValues();   // the program in use isn't known
                }
            }
            return P::generic(a...);
        }
    };

#define FSGL_FORGET_UNIFORM(name, prog, loc, count) \
    template <> struct Custom<FSGL_ID_##name> : ForgetUniformCall<FSGL_ID_##name, decltype(::name), prog, loc, count> {};
#define FSGL_FORGET_UNIFORM4(prefix, suffix, prog, loc, count) \
    FSGL_FORGET_UNIFORM(prefix##1##suffix, prog, loc, count) \
    FSGL_FORGET_UNIFORM(prefix##2##suffix, prog, loc, count) \
    FSGL_FORGET_UNIFORM(prefix##3##suffix, prog, loc, count) \
    FSGL_FORGET_UNIFORM(prefix##4##suffix, prog, loc, count)
#define FSGL_FORGET_UNIFORM_MATRICES(prefix, suffix, prog, loc, count) \
    FSGL_FORGET_UNIFORM(prefix##2##suffix, prog, loc, count) \
    FSGL_FORGET_UNIFORM(prefix##3##suffix, prog, loc, count) \
    FSGL_FORGET_UNIFORM(prefix##4##suffix, prog, loc, count) \
    FSGL_FORGET_UNIFORM(prefix##2x3##suffix, prog, loc, count) \
    FSGL_FORGET_UNIFORM(prefix##3x2##suffix, prog, loc, count) \
    FSGL_FORGET_UNIFORM(prefix##2x4##suffix, prog, loc, count) \
    FSGL_FORGET_UNIFORM(prefix##4x2##suffix, prog, loc, count) \
    FSGL_FORGET_UNIFORM(prefix##3x4##suffix, prog, loc, count) \
    FSGL_FORGET_UNIFORM(prefix##4x3##suffix, prog, loc, count)

    FSGL_FORGET_UNIFORM4(glUniform, ui, -1, 0, -1)
    FSGL_FORGET_UNIFORM4(glUniform, d, -1, 0, -1)
    FSGL_FORGET_UNIFORM4(glUniform, dv, -1, 0, 1)
    FSGL_FORGET_UNIFORM_MATRICES(glUniformMatrix, dv, -1, 0, 1)
    FSGL_FORGET_UNIFORM4(glProgramUniform, i, 0, 1, -1)
    FSGL_FORGET_UNIFORM4(glProgramUniform, f, 0, 1, -1)
    FSGL_FORGET_UNIFORM4(glProgramUniform, d, 0, 1, -1)
    FSGL_FORGET_UNIFORM4(glProgramUniform, ui, 0, 1, -1)
    FSGL_FORGET_UNIFORM4(glProgramUniform, iv, 0, 1, 2)
    FSGL_FORGET_UNIFORM4(glProgramUniform, fv, 0, 1, 2)
    FSGL_FORGET_UNIFORM4(glProgramUniform, dv, 0, 1, 2)
    FSGL_FORGET_UNIFORM4(glProgramUniform, uiv, 0, 1, 2)
    FSGL_FORGET_UNIFORM_MATRICES(glProgramUniformMatrix, fv, 0, 1, 2)
    FSGL_FORGET_UNIFORM_MATRICES(glProgramUniformMatrix, dv, 0, 1, 2)

    FSGL_BEGIN(glVertexAttrib4fv)
        static void APIENTRY call(GLuint index, const GLfloat* v)
        {
            if (!tRecording) { P::passthrough(index, v); return; }
            FSGL_BLOB(glVertexAttrib4fv, 1)::record(4 * sizeof(GLfloat), index, v);
        }
    FSGL_END

    FSGL_BEGIN(glBufferData)
        static void APIENTRY call(GLenum target, GLsizeiptr size, const void* data, GLenum usage)
        {
            if (!tRecording) { P::passthrough(target, size, data, usage); return; }
            FSGL_BLOB(glBufferData, 2)::record(size > 0 ? (size_t)size : 0, target, size, data, usage);
        }
    FSGL_END

    FSGL_BEGIN(glBufferSubData)
        static void APIENTRY call(GLenum target, GLintptr offset, GLsizeiptr size, const void* data)
        {
            if (!tRecording) { P::passthrough(target, offset, size, data); return; }
            FSGL_BLOB(glBufferSubData, 3)::record(size > 0 ? (size_t)size : 0, target, offset, size, data);
        }
    FSGL_END

    FSGL_BEGIN(glNamedBufferData)
        static void APIENTRY call(GLuint buffer, GLsizeiptr size, const void* data, GLenum usage)
        {
            if (!tRecording) { P::passthrough(buffer, size, data, usage); return; }
            FSGL_BLOB(glNamedBufferData, 2)::record(size > 0 ? (size_t)size : 0, buffer, size, data, usage);
        }
    FSGL_END

    FSGL_BEGIN(glNamedBufferSubData)
        static void APIENTRY call(GLuint buffer, GLintptr offset, GLsizeiptr size, const void* data)
        {
            if (!tRecording) { P::passthrough(buffer, offset, size, data); return; }
            FSGL_BLOB(glNamedBufferSubData, 3)::record(size > 0 ? (size_t)size : 0, buffer, offset, size, data);
        }
    FSGL_END

    FSGL_BEGIN(glBufferStorage)
        static void APIENTRY call(GLenum target, GLsizeiptr size, const void* data, GLbitfield flags)
        {
            if (!tRecording) { P::passthrough(target, size, data, flags); return; }
            FSGL_BLOB(glBufferStorage, 2)::record(size > 0 ? (size_t)size : 0, target, size, data, flags);
        }
    FSGL_END

    FSGL_BEGIN(glNamedBufferStorage)
        static void APIENTRY call(GLuint buffer, GLsizeiptr size, const void* data, GLbitfield flags)
        {
            if (!tRecording) { P::passthrough(buffer, size, data, flags); return; }
            FSGL_BLOB(glNamedBufferStorage, 2)::record(size > 0 ? (size_t)size : 0, buffer, size, data, flags);
        }
    FSGL_END

    FSGL_BEGIN(glDrawBuffers)
        static void APIENTRY call(GLsizei n, const GLenum* bufs)
        {
            if (!tRecording) { P::passthrough(n, bufs); return; }
            FSGL_BLOB(glDrawBuffers, 1)::record(size_t(n > 0 ? n : 0) * sizeof(GLenum), n, bufs);
        }
    FSGL_END

    FSGL_BEGIN(glInvalidateFramebuffer)
        static void APIENTRY call(GLenum target, GLsizei n, const GLenum* att)
        {
            if (!tRecording) { P::passthrough(target, n, att); return; }
            FSGL_BLOB(glInvalidateFramebuffer, 2)::record(size_t(n > 0 ? n : 0) * sizeof(GLenum), target, n, att);
        }
    FSGL_END

#define FSGL_CLEARBUFFERV(name, T) \
    FSGL_BEGIN(name) \
        static void APIENTRY call(GLenum buffer, GLint drawbuffer, const T* v) \
        { \
            if (!tRecording) { P::passthrough(buffer, drawbuffer, v); return; } \
            FSGL_BLOB(name, 2)::record((buffer == GL_COLOR ? 4 : 1) * sizeof(T), buffer, drawbuffer, v); \
        } \
    FSGL_END

    FSGL_CLEARBUFFERV(glClearBufferfv, GLfloat)
    FSGL_CLEARBUFFERV(glClearBufferiv, GLint)
    FSGL_CLEARBUFFERV(glClearBufferuiv, GLuint)

    inline size_t paramCount(GLenum pname)
    {
        return (pname == GL_TEXTURE_BORDER_COLOR || pname == GL_TEXTURE_SWIZZLE_RGBA) ? 4 : 1;
    }

#define FSGL_PARAMV(name, H, T) \
    FSGL_BEGIN(name) \
        static void APIENTRY call(H target, GLenum pname, const T* v) \
        { \
            if (!tRecording) { P::passthrough(target, pname, v); return; } \
            FSGL_BLOB(name, 2)::record(paramCount(pname) * sizeof(T), target, pname, v); \
        } \
    FSGL_END

    FSGL_PARAMV(glTexParameterfv, GLenum, GLfloat)
    FSGL_PARAMV(glTexParameteriv, GLenum, GLint)
    FSGL_PARAMV(glTexParameterIiv, GLenum, GLint)
    FSGL_PARAMV(glTexParameterIuiv, GLenum, GLuint)
    FSGL_PARAMV(glSamplerParameterfv, GLuint, GLfloat)
    FSGL_PARAMV(glSamplerParameteriv, GLuint, GLint)

    //------------------------------------------------------------------------
    // Texture uploads

    // Bytes glTex(Sub)Image reads from client memory with the current unpack
    // state, or SIZE_MAX if we can't tell (the call is then synchronous).
    size_t texBytes(GLsizei w, GLsizei h, GLsizei d, GLenum format, GLenum type, bool is3d)
    {
        if (w <= 0 || h <= 0 || d <= 0)
        {
            return 0;
        }
        size_t comps = 0;
        switch (format)
        {
        case GL_RED: case GL_GREEN: case GL_BLUE: case GL_ALPHA: case GL_LUMINANCE:
        case GL_RED_INTEGER: case GL_GREEN_INTEGER: case GL_BLUE_INTEGER:
        case GL_DEPTH_COMPONENT: case GL_STENCIL_INDEX:
            comps = 1; break;
        case GL_RG: case GL_RG_INTEGER: case GL_LUMINANCE_ALPHA: case GL_DEPTH_STENCIL:
            comps = 2; break;
        case GL_RGB: case GL_BGR: case GL_RGB_INTEGER: case GL_BGR_INTEGER:
            comps = 3; break;
        case GL_RGBA: case GL_BGRA: case GL_RGBA_INTEGER: case GL_BGRA_INTEGER:
            comps = 4; break;
        default:
            return SIZE_MAX;
        }
        size_t esize = 0;
        bool packed = false;
        switch (type)
        {
        case GL_UNSIGNED_BYTE: case GL_BYTE: esize = 1; break;
        case GL_UNSIGNED_SHORT: case GL_SHORT: case GL_HALF_FLOAT: esize = 2; break;
        case GL_UNSIGNED_INT: case GL_INT: case GL_FLOAT: esize = 4; break;
        case GL_UNSIGNED_BYTE_3_3_2: case GL_UNSIGNED_BYTE_2_3_3_REV:
            esize = 1; packed = true; break;
        case GL_UNSIGNED_SHORT_5_6_5: case GL_UNSIGNED_SHORT_5_6_5_REV:
        case GL_UNSIGNED_SHORT_4_4_4_4: case GL_UNSIGNED_SHORT_4_4_4_4_REV:
        case GL_UNSIGNED_SHORT_5_5_5_1: case GL_UNSIGNED_SHORT_1_5_5_5_REV:
            esize = 2; packed = true; break;
        case GL_UNSIGNED_INT_8_8_8_8: case GL_UNSIGNED_INT_8_8_8_8_REV:
        case GL_UNSIGNED_INT_10_10_10_2: case GL_UNSIGNED_INT_2_10_10_10_REV:
        case GL_UNSIGNED_INT_24_8: case GL_UNSIGNED_INT_10F_11F_11F_REV: case GL_UNSIGNED_INT_5_9_9_9_REV:
            esize = 4; packed = true; break;
        case GL_FLOAT_32_UNSIGNED_INT_24_8_REV:
            esize = 8; packed = true; break;
        default:
            return SIZE_MAX;
        }
        const int i_align = pixelStoreIndex(GL_UNPACK_ALIGNMENT);
        const int i_row = pixelStoreIndex(GL_UNPACK_ROW_LENGTH);
        const int i_skiprows = pixelStoreIndex(GL_UNPACK_SKIP_ROWS);
        const int i_skippix = pixelStoreIndex(GL_UNPACK_SKIP_PIXELS);
        const int i_imgh = pixelStoreIndex(GL_UNPACK_IMAGE_HEIGHT);
        const int i_skipimg = pixelStoreIndex(GL_UNPACK_SKIP_IMAGES);
        if (!sSh.mPixelStoreOk[i_align] || !sSh.mPixelStoreOk[i_row] || !sSh.mPixelStoreOk[i_skiprows] ||
            !sSh.mPixelStoreOk[i_skippix] || (is3d && (!sSh.mPixelStoreOk[i_imgh] || !sSh.mPixelStoreOk[i_skipimg])))
        {
            return SIZE_MAX;
        }
        const size_t align = (size_t)sSh.mPixelStore[i_align];
        const size_t row_len = sSh.mPixelStore[i_row] > 0 ? (size_t)sSh.mPixelStore[i_row] : (size_t)w;
        const size_t skip_rows = (size_t)sSh.mPixelStore[i_skiprows];
        const size_t skip_pix = (size_t)sSh.mPixelStore[i_skippix];
        const size_t bpp = packed ? esize : comps * esize;
        size_t stride = row_len * bpp;
        if (esize < align)
        {
            stride = (stride + align - 1) / align * align;
        }
        size_t total = stride * (skip_rows + (size_t)h - 1) + (skip_pix + (size_t)w) * bpp;
        if (is3d)
        {
            const size_t img_rows = sSh.mPixelStore[i_imgh] > 0 ? (size_t)sSh.mPixelStore[i_imgh] : (size_t)h;
            total += stride * img_rows * ((size_t)sSh.mPixelStore[i_skipimg] + (size_t)d - 1);
        }
        return total;
    }

    // UNPACK buffer state: 1 bound (pointer is an offset), 0 not bound, -1 unknown.
    int unpackBound()
    {
        if (!sSh.mBindOk[B_UNPACK])
        {
            return -1;
        }
        return sSh.mBind[B_UNPACK] != 0 ? 1 : 0;
    }

    template <typename P, typename B, size_t I, typename... A>
    void texUpload(size_t bytes, A... a)
    {
        const int bound = unpackBound();
        if (bound == 1)
        {
            P::record(a...);
        }
        else if (bound == 0 && bytes != SIZE_MAX)
        {
            B::record(bytes, a...);
        }
        else
        {
            P::sync(a...);
        }
    }

    FSGL_BEGIN(glTexImage2D)
        static void APIENTRY call(GLenum target, GLint level, GLint ifmt, GLsizei w, GLsizei h, GLint border, GLenum fmt, GLenum type, const void* px)
        {
            if (!tRecording) { P::passthrough(target, level, ifmt, w, h, border, fmt, type, px); return; }
            texUpload<P, FSGL_BLOB(glTexImage2D, 8), 8>(px ? texBytes(w, h, 1, fmt, type, false) : 0, target, level, ifmt, w, h, border, fmt, type, px);
        }
    FSGL_END

    FSGL_BEGIN(glTexSubImage2D)
        static void APIENTRY call(GLenum target, GLint level, GLint x, GLint y, GLsizei w, GLsizei h, GLenum fmt, GLenum type, const void* px)
        {
            if (!tRecording) { P::passthrough(target, level, x, y, w, h, fmt, type, px); return; }
            texUpload<P, FSGL_BLOB(glTexSubImage2D, 8), 8>(px ? texBytes(w, h, 1, fmt, type, false) : 0, target, level, x, y, w, h, fmt, type, px);
        }
    FSGL_END

    FSGL_BEGIN(glTextureSubImage2D)
        static void APIENTRY call(GLuint tex, GLint level, GLint x, GLint y, GLsizei w, GLsizei h, GLenum fmt, GLenum type, const void* px)
        {
            if (!tRecording) { P::passthrough(tex, level, x, y, w, h, fmt, type, px); return; }
            texUpload<P, FSGL_BLOB(glTextureSubImage2D, 8), 8>(px ? texBytes(w, h, 1, fmt, type, false) : 0, tex, level, x, y, w, h, fmt, type, px);
        }
    FSGL_END

    FSGL_BEGIN(glTexImage3D)
        static void APIENTRY call(GLenum target, GLint level, GLint ifmt, GLsizei w, GLsizei h, GLsizei d, GLint border, GLenum fmt, GLenum type, const void* px)
        {
            if (!tRecording) { P::passthrough(target, level, ifmt, w, h, d, border, fmt, type, px); return; }
            texUpload<P, FSGL_BLOB(glTexImage3D, 9), 9>(px ? texBytes(w, h, d, fmt, type, true) : 0, target, level, ifmt, w, h, d, border, fmt, type, px);
        }
    FSGL_END

    FSGL_BEGIN(glTexSubImage3D)
        static void APIENTRY call(GLenum target, GLint level, GLint x, GLint y, GLint z, GLsizei w, GLsizei h, GLsizei d, GLenum fmt, GLenum type, const void* px)
        {
            if (!tRecording) { P::passthrough(target, level, x, y, z, w, h, d, fmt, type, px); return; }
            texUpload<P, FSGL_BLOB(glTexSubImage3D, 10), 10>(px ? texBytes(w, h, d, fmt, type, true) : 0, target, level, x, y, z, w, h, d, fmt, type, px);
        }
    FSGL_END

    FSGL_BEGIN(glCompressedTexImage2D)
        static void APIENTRY call(GLenum target, GLint level, GLenum ifmt, GLsizei w, GLsizei h, GLint border, GLsizei size, const void* data)
        {
            if (!tRecording) { P::passthrough(target, level, ifmt, w, h, border, size, data); return; }
            texUpload<P, FSGL_BLOB(glCompressedTexImage2D, 7), 7>(size > 0 ? (size_t)size : 0, target, level, ifmt, w, h, border, size, data);
        }
    FSGL_END

    FSGL_BEGIN(glCompressedTexSubImage2D)
        static void APIENTRY call(GLenum target, GLint level, GLint x, GLint y, GLsizei w, GLsizei h, GLenum fmt, GLsizei size, const void* data)
        {
            if (!tRecording) { P::passthrough(target, level, x, y, w, h, fmt, size, data); return; }
            texUpload<P, FSGL_BLOB(glCompressedTexSubImage2D, 8), 8>(size > 0 ? (size_t)size : 0, target, level, x, y, w, h, fmt, size, data);
        }
    FSGL_END

    //------------------------------------------------------------------------
    // Object names: reserved in batches on the render thread

    // glGen* only reserve names, so a pool may grow to many; glCreate* make real objects, each of
    // which costs driver memory until the pool hands it out: those batches stay small
    constexpr GLsizei NAME_BATCH_MAX = 4096;
    constexpr GLsizei OBJECT_BATCH_MAX = 256;

    template <int ID, typename PFN, bool VAO, GLsizei MAX_BATCH>
    struct PoolCall
    {
        static constexpr bool kHas = true;
        using P = Proxy<ID, PFN>;
        static std::vector<GLuint> sPool;   // main thread

        static void APIENTRY call(GLsizei n, GLuint* out)
        {
            if (!tRecording || n <= 0 || !out)
            {
                if (!tRecording)
                {
                    P::passthrough(n, out);
                }
                else
                {
                    P::sync(n, out);
                }
                return;
            }
            if (sPool.size() < (size_t)n)
            {
                // Each refill waits for the render thread to work through everything queued,
                // which in a busy scene is tens of milliseconds: a pool that keeps running dry
                // refills in growing batches (to MAX_BATCH names) so a burst costs a few waits
                static GLsizei s_batch = std::min<GLsizei>(256, MAX_BATCH);
                const GLsizei batch = std::max<GLsizei>(n, s_batch);
                s_batch = std::min<GLsizei>(s_batch * 2, MAX_BATCH);
                std::vector<GLuint> fresh((size_t)batch);
                syncCall<void>(ID, [&]() { P::real()(batch, fresh.data()); });
                sPool.insert(sPool.end(), fresh.rbegin(), fresh.rend());
            }
            for (GLsizei i = 0; i < n; ++i)
            {
                out[i] = sPool.back();
                sPool.pop_back();
                if constexpr (VAO)
                {
                    // a new vertex array has no element buffer
                    if (sSh.mVAOOk && sSh.mVAO == out[i])
                    {
                        setCurEBO(0);
                    }
                    else
                    {
                        sSh.mEBO[out[i]] = 0;
                    }
                }
            }
        }
    };
    template <int ID, typename PFN, bool VAO, GLsizei MAX_BATCH>
    std::vector<GLuint> PoolCall<ID, PFN, VAO, MAX_BATCH>::sPool;

#define FSGL_POOL(name, vao, max_batch) template <> struct Custom<FSGL_ID_##name> : PoolCall<FSGL_ID_##name, decltype(::name), vao, max_batch> {};

    FSGL_POOL(glGenBuffers, false, NAME_BATCH_MAX)
    FSGL_POOL(glCreateBuffers, false, OBJECT_BATCH_MAX)
    FSGL_POOL(glGenTextures, false, NAME_BATCH_MAX)
    FSGL_POOL(glGenFramebuffers, false, NAME_BATCH_MAX)
    FSGL_POOL(glGenRenderbuffers, false, NAME_BATCH_MAX)
    FSGL_POOL(glGenSamplers, false, NAME_BATCH_MAX)
    FSGL_POOL(glGenVertexArrays, true, NAME_BATCH_MAX)
    FSGL_POOL(glCreateFramebuffers, false, OBJECT_BATCH_MAX)
    FSGL_POOL(glCreateRenderbuffers, false, OBJECT_BATCH_MAX)
    FSGL_POOL(glCreateSamplers, false, OBJECT_BATCH_MAX)
    FSGL_POOL(glCreateVertexArrays, true, OBJECT_BATCH_MAX)

    // glCreateTextures and glCreateQueries make objects of a given target, so
    // they are reserved per target.
    template <int ID, typename PFN>
    struct TargetPoolCall
    {
        static constexpr bool kHas = true;
        using P = Proxy<ID, PFN>;
        static std::unordered_map<GLenum, std::vector<GLuint>> sPools;  // main thread

        static void APIENTRY call(GLenum target, GLsizei n, GLuint* out)
        {
            if (!tRecording || n <= 0 || !out)
            {
                if (!tRecording)
                {
                    P::passthrough(target, n, out);
                }
                else
                {
                    P::sync(target, n, out);
                }
                return;
            }
            std::vector<GLuint>& pool = sPools[target];
            if (pool.size() < (size_t)n)
            {
                // As PoolCall: batches grow while the pool keeps running dry (one per target), to
                // OBJECT_BATCH_MAX as these are real objects
                static std::unordered_map<GLenum, GLsizei> s_batches;
                GLsizei& next_batch = s_batches.emplace(target, 64).first->second;
                const GLsizei batch = std::max<GLsizei>(n, next_batch);
                next_batch = std::min<GLsizei>(next_batch * 2, OBJECT_BATCH_MAX);
                std::vector<GLuint> fresh((size_t)batch);
                syncCall<void>(ID, [&]() { P::real()(target, batch, fresh.data()); });
                pool.insert(pool.end(), fresh.rbegin(), fresh.rend());
            }
            for (GLsizei i = 0; i < n; ++i)
            {
                out[i] = pool.back();
                pool.pop_back();
            }
        }
    };
    template <int ID, typename PFN>
    std::unordered_map<GLenum, std::vector<GLuint>> TargetPoolCall<ID, PFN>::sPools;

    template <> struct Custom<FSGL_ID_glCreateTextures> : TargetPoolCall<FSGL_ID_glCreateTextures, decltype(::glCreateTextures)> {};
    template <> struct Custom<FSGL_ID_glCreateQueries> : TargetPoolCall<FSGL_ID_glCreateQueries, decltype(::glCreateQueries)> {};

    // glGetError stays synchronous (the generic proxy): callers decide on its answer, such as
    // whether an allocation just failed. Code that only wants the error flags cleared calls
    // FSGLStream::clearErrors(), which is deferred.
    void execClearErrors(U8*)
    {
        auto get_error = FSGL_REAL(glGetError);
        for (int i = 0; i < 16 && get_error() != GL_NO_ERROR; ++i)
        {
        }
    }

    // The palette is a small client array: copied into the stream
    FSGL_BEGIN(fs_glShadingRateImagePaletteNV)
        static void APIENTRY call(GLuint viewport, GLuint first, GLsizei count, const GLenum* rates)
        {
            if (!tRecording) { P::passthrough(viewport, first, count, rates); return; }
            FSGL_BLOB(fs_glShadingRateImagePaletteNV, 3)::record(size_t(count > 0 ? count : 0) * sizeof(GLenum), viewport, first, count, rates);
        }
    FSGL_END

    template <typename F>
    void recordDeleted(GLsizei n, const GLuint* ids, F&& each)
    {
        if (n > 0 && ids)
        {
            for (GLsizei i = 0; i < n; ++i)
            {
                each(ids[i]);
            }
        }
    }

    FSGL_BEGIN(glDeleteBuffers)
        static void APIENTRY call(GLsizei n, const GLuint* ids)
        {
            if (!tRecording) { P::passthrough(n, ids); return; }
            recordDeleted(n, ids, [](GLuint id)
            {
                if (id == 0) return;
                for (int s = 0; s < B_COUNT; ++s)
                {
                    if (sSh.mBind[s] == id) sSh.mBind[s] = 0;
                }
                // GL detaches a deleted buffer only from the bound vertex array. The others keep
                // the orphan attached under a name that is free again: what they hold is unknown.
                for (auto it = sSh.mEBO.begin(); it != sSh.mEBO.end();)
                {
                    if (it->second == id && !(sSh.mVAOOk && it->first == sSh.mVAO))
                    {
                        it = sSh.mEBO.erase(it);
                    }
                    else
                    {
                        ++it;   // the bound VAO's entry is mCurEBO's business
                    }
                }
                if (sSh.mVAOOk && sSh.mCurEBOOk && sSh.mCurEBO == id)
                {
                    setCurEBO(0);
                }
            });
            FSGL_BLOB(glDeleteBuffers, 1)::record(size_t(n > 0 ? n : 0) * sizeof(GLuint), n, ids);
        }
    FSGL_END

    FSGL_BEGIN(glDeleteVertexArrays)
        static void APIENTRY call(GLsizei n, const GLuint* ids)
        {
            if (!tRecording) { P::passthrough(n, ids); return; }
            recordDeleted(n, ids, [](GLuint id)
            {
                if (id == 0) return;
                if (sSh.mVAOOk && sSh.mVAO == id)
                {
                    sSh.mCurEBODirty = false;   // nothing to keep: the object is gone
                    sSh.mEBO.erase(id);
                    switchVAO(0);
                }
                else
                {
                    sSh.mEBO.erase(id);
                }
            });
            FSGL_BLOB(glDeleteVertexArrays, 1)::record(size_t(n > 0 ? n : 0) * sizeof(GLuint), n, ids);
        }
    FSGL_END

#define FSGL_DELETE(name) \
    FSGL_BEGIN(name) \
        static void APIENTRY call(GLsizei n, const GLuint* ids) \
        { \
            if (!tRecording) { P::passthrough(n, ids); return; } \
            FSGL_BLOB(name, 1)::record(size_t(n > 0 ? n : 0) * sizeof(GLuint), n, ids); \
        } \
    FSGL_END

    // Deleting a bound texture, framebuffer or sampler binds 0 in its place
    FSGL_BEGIN(glDeleteTextures)
        static void APIENTRY call(GLsizei n, const GLuint* ids)
        {
            if (!tRecording) { P::passthrough(n, ids); return; }
            recordDeleted(n, ids, [](GLuint id)
            {
                sSh.mTex.deleted(id);
            });
            FSGL_BLOB(glDeleteTextures, 1)::record(size_t(n > 0 ? n : 0) * sizeof(GLuint), n, ids);
        }
    FSGL_END

    FSGL_BEGIN(glDeleteFramebuffers)
        static void APIENTRY call(GLsizei n, const GLuint* ids)
        {
            if (!tRecording) { P::passthrough(n, ids); return; }
            recordDeleted(n, ids, [](GLuint id)
            {
                if (id != 0 && sSh.mDrawFB == id)
                {
                    sSh.mDrawFB = 0;
                }
                if (id != 0 && sSh.mReadFB == id)
                {
                    sSh.mReadFB = 0;
                }
            });
            FSGL_BLOB(glDeleteFramebuffers, 1)::record(size_t(n > 0 ? n : 0) * sizeof(GLuint), n, ids);
        }
    FSGL_END

    FSGL_BEGIN(glDeleteSamplers)
        static void APIENTRY call(GLsizei n, const GLuint* ids)
        {
            if (!tRecording) { P::passthrough(n, ids); return; }
            recordDeleted(n, ids, [](GLuint id)
            {
                sSh.mSampler.deleted(id);
            });
            FSGL_BLOB(glDeleteSamplers, 1)::record(size_t(n > 0 ? n : 0) * sizeof(GLuint), n, ids);
        }
    FSGL_END

    FSGL_DELETE(glDeleteRenderbuffers)

    //------------------------------------------------------------------------
    // Sync objects (glFenceSync and glClientWaitSync stay synchronous)

    FSGL_BEGIN(glWaitSync)
        static void APIENTRY call(GLsync s, GLbitfield flags, GLuint64 timeout)
        {
            if (!tRecording) { P::passthrough(s, flags, timeout); return; }
            P::record(s, flags, timeout);
        }
    FSGL_END

    FSGL_BEGIN(glDeleteSync)
        static void APIENTRY call(GLsync s)
        {
            if (!tRecording) { P::passthrough(s); return; }
            P::record(s);
        }
    FSGL_END

    FSGL_BEGIN(glFinish)
        static void APIENTRY call()
        {
            if (!tRecording) { P::passthrough(); return; }
            P::sync();
        }
    FSGL_END

    // GL_FALSE only reports a store corrupted by a display mode change, which
    // nothing in the viewer checks; the unmap itself keeps its order.
    FSGL_BEGIN(glUnmapBuffer)
        static GLboolean APIENTRY call(GLenum target)
        {
            if (!tRecording) { return P::passthrough(target); }
            P::record(target);
            return GL_TRUE;
        }
    FSGL_END

    FSGL_BEGIN(glUnmapNamedBuffer)
        static GLboolean APIENTRY call(GLuint buffer)
        {
            if (!tRecording) { return P::passthrough(buffer); }
            P::record(buffer);
            return GL_TRUE;
        }
    FSGL_END

    //------------------------------------------------------------------------
    // Query objects: the render thread collects results, the main thread
    // reads them without waiting. Availability may come later than before,
    // which GL allows; a blocking read of a result that isn't in yet is
    // synchronous, as it was.

    std::unordered_map<GLuint, U64> sQueryIssued;           // main: id -> generation of its latest end
    std::unordered_map<GLenum, GLuint> sQueryActive;        // main: target -> open query
    U64 sQueryGen = 0;                                      // main
    struct QueryResult
    {
        U64 mGen;
        U64 mValue;
        bool mDropped;      // never came in: reads of this generation ask the driver
    };
    struct PendingQuery
    {
        GLuint mId;
        U64 mGen;
        U64 mEndNs;         // when its end was replayed
    };
    std::mutex sQueryResultMutex;
    std::unordered_map<GLuint, QueryResult> sQueryResults;  // id -> result of its latest collected generation
    std::deque<PendingQuery> sQueryPending;                 // render: ended, not collected
    std::unordered_map<GLuint, U64> sQueryRenderGen;        // render: id -> latest ended generation (0 while open)

    // A result not in after this long is given up on (a query whose begin failed never becomes
    // available), so it can't keep the poll from reaching the ones behind it
    constexpr U64 QUERY_GIVE_UP_NS = 2000000000ull;
    // Results come in about in order: after this many in a row that aren't in yet, stop looking
    constexpr U32 QUERY_SCAN_MISSES = 4;

    struct QueryRec
    {
        GLenum mTarget;
        GLuint mId;
        U64 mGen;
    };

    void execBeginQuery(U8* p)
    {
        const QueryRec& r = *reinterpret_cast<QueryRec*>(p);
        sQueryRenderGen[r.mId] = 0;
        FSGL_REAL(glBeginQuery)(r.mTarget, r.mId);
    }

    void execEndQuery(U8* p)
    {
        const QueryRec& r = *reinterpret_cast<QueryRec*>(p);
        FSGL_REAL(glEndQuery)(r.mTarget);
        if (r.mId)
        {
            sQueryRenderGen[r.mId] = r.mGen;
            sQueryPending.push_back({ r.mId, r.mGen, nowNs() });
        }
    }

    void execQueryCounter(U8* p)
    {
        const QueryRec& r = *reinterpret_cast<QueryRec*>(p);
        FSGL_REAL(glQueryCounter)(r.mId, r.mTarget);
        sQueryRenderGen[r.mId] = r.mGen;
        sQueryPending.push_back({ r.mId, r.mGen, nowNs() });
    }

    void execDeleteQueries(U8* p)
    {
        const GLsizei n = *reinterpret_cast<GLsizei*>(p);
        const GLuint* ids = reinterpret_cast<const GLuint*>(p + 16);
        {
            // A result collected between the main thread's delete and this replay would stay
            // in the map for good
            std::lock_guard<std::mutex> lock(sQueryResultMutex);
            for (GLsizei i = 0; i < n; ++i)
            {
                sQueryRenderGen.erase(ids[i]);
                sQueryResults.erase(ids[i]);
            }
        }
        FSGL_REAL(glDeleteQueries)(n, ids);
    }

    void recordQuery(ExecFn fn, GLenum target, GLuint id, U64 gen)
    {
        U8* p = allocRecord(fn, sizeof(QueryRec));
        QueryRec* r = reinterpret_cast<QueryRec*>(p);
        r->mTarget = target;
        r->mId = id;
        r->mGen = gen;
        commitRecord();
    }

    // Render thread: collect what the GPU has finished, oldest first. One result that isn't in
    // doesn't hold back the ones behind it (the scan goes on past it, a few misses deep), and one
    // that never comes in is dropped after QUERY_GIVE_UP_NS.
    void pollQueries()
    {
        if (sQueryPending.empty())
        {
            return;
        }
        auto avail_fn = FSGL_REAL(glGetQueryObjectuiv);
        auto res64_fn = FSGL_REAL(glGetQueryObjectui64v);
        const U64 now = nowNs();
        const size_t count = sQueryPending.size();
        size_t keep = 0;    // entries still pending are moved down to here, in order
        size_t r = 0;
        U32 misses = 0;
        for (; r < count && misses < QUERY_SCAN_MISSES; ++r)
        {
            const PendingQuery q = sQueryPending[r];
            auto it = sQueryRenderGen.find(q.mId);
            if (it == sQueryRenderGen.end() || it->second != q.mGen)
            {
                continue;   // re-issued or deleted since
            }
            GLuint avail = 0;
            avail_fn(q.mId, GL_QUERY_RESULT_AVAILABLE, &avail);
            if (!avail)
            {
                if (now > q.mEndNs && now - q.mEndNs > QUERY_GIVE_UP_NS)
                {
                    std::lock_guard<std::mutex> lock(sQueryResultMutex);
                    sQueryResults[q.mId] = { q.mGen, 0, true };
                    continue;
                }
                ++misses;
                sQueryPending[keep++] = q;
                continue;
            }
            misses = 0;
            GLuint64 value = 0;
            if (res64_fn)
            {
                res64_fn(q.mId, GL_QUERY_RESULT, &value);
            }
            else
            {
                GLuint v32 = 0;
                avail_fn(q.mId, GL_QUERY_RESULT, &v32);
                value = v32;
            }
            std::lock_guard<std::mutex> lock(sQueryResultMutex);
            sQueryResults[q.mId] = { q.mGen, (U64)value, false };
        }
        for (; r < count; ++r)
        {
            sQueryPending[keep++] = sQueryPending[r];
        }
        sQueryPending.resize(keep);
    }

    FSGL_BEGIN(glBeginQuery)
        static void APIENTRY call(GLenum target, GLuint id)
        {
            if (!tRecording) { P::passthrough(target, id); return; }
            sQueryActive[target] = id;
            recordQuery(&execBeginQuery, target, id, 0);
        }
    FSGL_END

    FSGL_BEGIN(glEndQuery)
        static void APIENTRY call(GLenum target)
        {
            if (!tRecording) { P::passthrough(target); return; }
            GLuint id = 0;
            auto it = sQueryActive.find(target);
            if (it != sQueryActive.end())
            {
                id = it->second;
                sQueryActive.erase(it);
            }
            const U64 gen = ++sQueryGen;
            if (id)
            {
                sQueryIssued[id] = gen;
            }
            recordQuery(&execEndQuery, target, id, gen);
            sQueryEndUnsubmitted = true;
        }
    FSGL_END

    FSGL_BEGIN(glQueryCounter)
        static void APIENTRY call(GLuint id, GLenum target)
        {
            if (!tRecording) { P::passthrough(id, target); return; }
            const U64 gen = ++sQueryGen;
            sQueryIssued[id] = gen;
            recordQuery(&execQueryCounter, target, id, gen);
            sQueryEndUnsubmitted = true;
        }
    FSGL_END

    FSGL_BEGIN(glBeginQueryIndexed)
        static void APIENTRY call(GLenum target, GLuint index, GLuint id)
        {
            if (tRecording) { sQueryIssued.erase(id); }  // not tracked: reads are synchronous
            P::generic(target, index, id);
        }
    FSGL_END

    FSGL_BEGIN(glDeleteQueries)
        static void APIENTRY call(GLsizei n, const GLuint* ids)
        {
            if (!tRecording) { P::passthrough(n, ids); return; }
            if (n <= 0 || !ids)
            {
                P::sync(n, ids);
                return;
            }
            {
                std::lock_guard<std::mutex> lock(sQueryResultMutex);
                for (GLsizei i = 0; i < n; ++i)
                {
                    sQueryIssued.erase(ids[i]);
                    sQueryResults.erase(ids[i]);
                }
            }
            U8* p = allocRecord(&execDeleteQueries, 16 + (size_t)n * sizeof(GLuint));
            *reinterpret_cast<GLsizei*>(p) = n;
            memcpy(p + 16, ids, (size_t)n * sizeof(GLuint));
            commitRecord();
        }
    FSGL_END

    FSGL_POOL(glGenQueries, false, NAME_BATCH_MAX)

    // Answers a query object read on the main thread if the cache can.
    template <typename T>
    bool cachedQueryRead(GLuint id, GLenum pname, T* params)
    {
        auto it = sQueryIssued.find(id);
        if (it == sQueryIssued.end() || !params)
        {
            return false;
        }
        bool have = false;
        U64 value = 0;
        {
            std::lock_guard<std::mutex> lock(sQueryResultMutex);
            auto r = sQueryResults.find(id);
            if (r != sQueryResults.end() && r->second.mGen == it->second)
            {
                if (r->second.mDropped)
                {
                    return false;   // the render thread gave up on it: let the driver answer
                }
                have = true;
                value = r->second.mValue;
            }
        }
        if (!have && sQueryEndUnsubmitted)
        {
            // The caller may poll until it's there: make sure the render
            // thread has the query's end.
            submitCurrent();
        }
        switch (pname)
        {
        case GL_QUERY_RESULT_AVAILABLE:
            *params = have ? (T)GL_TRUE : (T)GL_FALSE;
            return true;
        case GL_QUERY_RESULT_NO_WAIT:
            if (have)
            {
                *params = (T)value;
            }
            return true;
        case GL_QUERY_RESULT:
            if (have)
            {
                *params = (T)value;
                return true;
            }
            return false;
        default:
            return false;
        }
    }

#define FSGL_QUERYOBJECT(name, T) \
    FSGL_BEGIN(name) \
        static void APIENTRY call(GLuint id, GLenum pname, T* params) \
        { \
            if (!tRecording) { P::passthrough(id, pname, params); return; } \
            if (cachedQueryRead(id, pname, params)) return; \
            P::sync(id, pname, params); \
        } \
    FSGL_END

    FSGL_QUERYOBJECT(glGetQueryObjectiv, GLint)
    FSGL_QUERYOBJECT(glGetQueryObjectuiv, GLuint)
    FSGL_QUERYOBJECT(glGetQueryObjecti64v, GLint64)
    FSGL_QUERYOBJECT(glGetQueryObjectui64v, GLuint64)

    //------------------------------------------------------------------------
    // State queries

#ifndef GL_SHADING_RATE_IMAGE_TEXEL_WIDTH_NV
#define GL_SHADING_RATE_IMAGE_TEXEL_WIDTH_NV 0x955B
#define GL_SHADING_RATE_IMAGE_TEXEL_HEIGHT_NV 0x955C
#define GL_SHADING_RATE_IMAGE_PALETTE_SIZE_NV 0x955D
#define GL_MAX_COARSE_FRAGMENT_SAMPLES_NV 0x955F
#endif
#ifndef GL_MAX_TEXTURE_MAX_ANISOTROPY
#define GL_MAX_TEXTURE_MAX_ANISOTROPY 0x84FF
#endif

    // Implementation limits never change for a context: ask once.
    int constantCount(GLenum pname)
    {
        switch (pname)
        {
        case GL_MAX_VIEWPORT_DIMS:
        case GL_ALIASED_LINE_WIDTH_RANGE:
        case GL_SMOOTH_LINE_WIDTH_RANGE:
        case GL_POINT_SIZE_RANGE:
            return 2;
        case GL_MAX_TEXTURE_SIZE: case GL_MAX_3D_TEXTURE_SIZE: case GL_MAX_CUBE_MAP_TEXTURE_SIZE:
        case GL_MAX_ARRAY_TEXTURE_LAYERS: case GL_MAX_TEXTURE_IMAGE_UNITS: case GL_MAX_COMBINED_TEXTURE_IMAGE_UNITS:
        case GL_MAX_VERTEX_TEXTURE_IMAGE_UNITS: case GL_MAX_TEXTURE_BUFFER_SIZE: case GL_MAX_VIEWPORTS:
        case GL_UNIFORM_BUFFER_OFFSET_ALIGNMENT: case GL_MAX_UNIFORM_BLOCK_SIZE: case GL_MAX_UNIFORM_BUFFER_BINDINGS:
        case GL_MAX_SAMPLES: case GL_MAX_COLOR_TEXTURE_SAMPLES: case GL_MAX_DEPTH_TEXTURE_SAMPLES:
        case GL_MAX_INTEGER_SAMPLES: case GL_MAX_COLOR_ATTACHMENTS: case GL_MAX_DRAW_BUFFERS:
        case GL_MAX_VERTEX_ATTRIBS: case GL_MAX_ELEMENTS_VERTICES: case GL_MAX_ELEMENTS_INDICES:
        case GL_MAX_RENDERBUFFER_SIZE: case GL_NUM_EXTENSIONS: case GL_MAX_VARYING_VECTORS:
        case GL_MAX_SAMPLE_MASK_WORDS: case GL_MAX_VERTEX_SHADER_STORAGE_BLOCKS:
        case GL_MAX_COMBINED_SHADER_STORAGE_BLOCKS: case GL_SHADER_STORAGE_BUFFER_OFFSET_ALIGNMENT:
        case GL_MAX_SHADER_STORAGE_BLOCK_SIZE: case GL_SUBPIXEL_BITS: case GL_MAX_CLIP_DISTANCES:
        case GL_NUM_PROGRAM_BINARY_FORMATS: case GL_MAX_VERTEX_UNIFORM_COMPONENTS:
        case GL_MAX_FRAGMENT_UNIFORM_COMPONENTS: case GL_MAX_VERTEX_UNIFORM_BLOCKS:
        case GL_MAX_FRAGMENT_UNIFORM_BLOCKS: case GL_MAX_COMBINED_UNIFORM_BLOCKS:
        case GL_MAJOR_VERSION: case GL_MINOR_VERSION: case GL_CONTEXT_PROFILE_MASK: case GL_CONTEXT_FLAGS:
        case GL_MAX_TEXTURE_LOD_BIAS: case GL_MAX_TEXTURE_MAX_ANISOTROPY:
        case GL_SHADING_RATE_IMAGE_TEXEL_WIDTH_NV: case GL_SHADING_RATE_IMAGE_TEXEL_HEIGHT_NV:
        case GL_SHADING_RATE_IMAGE_PALETTE_SIZE_NV: case GL_MAX_COARSE_FRAGMENT_SAMPLES_NV:
            return 1;
        default:
            return 0;
        }
    }

    // Free memory estimates: the previous value is as good as a fresh one,
    // so answer from the last read and refresh it in the stream.
    int staleCount(GLenum pname)
    {
        switch (pname)
        {
        case GL_GPU_MEMORY_INFO_DEDICATED_VIDMEM_NVX:
        case GL_GPU_MEMORY_INFO_TOTAL_AVAILABLE_MEMORY_NVX:
        case GL_GPU_MEMORY_INFO_CURRENT_AVAILABLE_VIDMEM_NVX:
        case GL_GPU_MEMORY_INFO_EVICTION_COUNT_NVX:
        case GL_GPU_MEMORY_INFO_EVICTED_MEMORY_NVX:
            return 1;
        case GL_VBO_FREE_MEMORY_ATI:
        case GL_TEXTURE_FREE_MEMORY_ATI:
        case GL_RENDERBUFFER_FREE_MEMORY_ATI:
            return 4;
        default:
            return 0;
        }
    }

    template <typename T>
    struct ConstCache
    {
        std::unordered_map<GLenum, std::array<T, 4>> mValues;   // main
    };
    ConstCache<GLint> sConstI;
    ConstCache<GLfloat> sConstF;

    struct StaleEntry
    {
        std::atomic<GLint> mValue[4];
        std::atomic<bool> mValid{ false };
        U32 mRequestedFrame = U32_MAX;  // main
    };
    std::unordered_map<GLenum, StaleEntry> sStale;  // keys inserted by the main thread only, before first refresh is recorded
    U32 sPresentCounter = 0;    // main

    void execStaleRefresh(U8* p)
    {
        StaleEntry* e = *reinterpret_cast<StaleEntry**>(p);
        const GLenum pname = *reinterpret_cast<GLenum*>(p + 8);
        GLint v[4] = {};
        FSGL_REAL(glGetIntegerv)(pname, v);
        for (int i = 0; i < 4; ++i)
        {
            e->mValue[i].store(v[i], std::memory_order_relaxed);
        }
        e->mValid.store(true, std::memory_order_release);
    }

    template <typename P>
    bool staleRead(GLenum pname, GLint* data)
    {
        const int n = staleCount(pname);
        if (!n)
        {
            return false;
        }
        StaleEntry& e = sStale[pname];
        if (!e.mValid.load(std::memory_order_acquire))
        {
            P::sync(pname, data);
            for (int i = 0; i < n; ++i)
            {
                e.mValue[i].store(data[i], std::memory_order_relaxed);
            }
            e.mValid.store(true, std::memory_order_release);
            e.mRequestedFrame = sPresentCounter;
            return true;
        }
        for (int i = 0; i < n; ++i)
        {
            data[i] = e.mValue[i].load(std::memory_order_relaxed);
        }
        if (e.mRequestedFrame != sPresentCounter)
        {
            e.mRequestedFrame = sPresentCounter;
            U8* p = allocRecord(&execStaleRefresh, 16);
            *reinterpret_cast<StaleEntry**>(p) = &e;
            *reinterpret_cast<GLenum*>(p + 8) = pname;
            commitRecord();
        }
        return true;
    }

    template <typename T, typename P>
    bool constRead(ConstCache<T>& cache, GLenum pname, T* data)
    {
        const int n = constantCount(pname);
        if (!n)
        {
            return false;
        }
        auto it = cache.mValues.find(pname);
        if (it == cache.mValues.end())
        {
            std::array<T, 4> v{};
            P::sync(pname, v.data());
            it = cache.mValues.emplace(pname, v).first;
        }
        for (int i = 0; i < n; ++i)
        {
            data[i] = it->second[i];
        }
        return true;
    }

    FSGL_BEGIN(glGetIntegerv)
        static void APIENTRY call(GLenum pname, GLint* data)
        {
            if (!tRecording || !data) { if (!tRecording) P::passthrough(pname, data); else P::sync(pname, data); return; }
            switch (pname)
            {
            case GL_VIEWPORT:
                if (sSh.mViewportOk) { std::copy_n(sSh.mViewport, 4, data); return; }
                P::sync(pname, data);
                std::copy_n(data, 4, sSh.mViewport);
                sSh.mViewportOk = true;
                return;
            case GL_SCISSOR_BOX:
                if (sSh.mScissorOk) { std::copy_n(sSh.mScissor, 4, data); return; }
                P::sync(pname, data);
                std::copy_n(data, 4, sSh.mScissor);
                sSh.mScissorOk = true;
                return;
            case GL_ACTIVE_TEXTURE:
                if (sSh.mActiveTexOk) { *data = sSh.mActiveTex; return; }
                break;
            case GL_VERTEX_ARRAY_BINDING:
                if (sSh.mVAOOk) { *data = (GLint)sSh.mVAO; return; }
                break;
            case GL_ELEMENT_ARRAY_BUFFER_BINDING:
                if (sSh.mVAOOk && sSh.mCurEBOOk) { *data = (GLint)sSh.mCurEBO; return; }
                break;
            default:
                break;
            }
            for (int s = 0; s < B_COUNT; ++s)
            {
                if (BIND_QUERIES[s] == pname && sSh.mBindOk[s])
                {
                    *data = (GLint)sSh.mBind[s];
                    return;
                }
            }
            const int ps = pixelStoreIndex(pname);
            if (ps >= 0 && sSh.mPixelStoreOk[ps])
            {
                *data = sSh.mPixelStore[ps];
                return;
            }
            if (constRead<GLint, P>(sConstI, pname, data) || staleRead<P>(pname, data))
            {
                return;
            }
            P::sync(pname, data);
        }
    FSGL_END

    FSGL_BEGIN(glGetFloatv)
        static void APIENTRY call(GLenum pname, GLfloat* data)
        {
            if (!tRecording || !data) { if (!tRecording) P::passthrough(pname, data); else P::sync(pname, data); return; }
            switch (pname)
            {
            case GL_POLYGON_OFFSET_FACTOR:
                if (sSh.mPolyOk) { *data = sSh.mPoly[0]; return; }
                break;
            case GL_POLYGON_OFFSET_UNITS:
                if (sSh.mPolyOk) { *data = sSh.mPoly[1]; return; }
                break;
            case GL_COLOR_CLEAR_VALUE:
                if (sSh.mClearColorOk) { std::copy_n(sSh.mClearColor, 4, data); return; }
                break;
            case GL_LINE_WIDTH:
                if (sSh.mLineWidthOk) { *data = sSh.mLineWidth; return; }
                P::sync(pname, data);
                sSh.mLineWidth = *data;
                sSh.mLineWidthOk = true;
                return;
            default:
                break;
            }
            if (constRead<GLfloat, P>(sConstF, pname, data))
            {
                return;
            }
            P::sync(pname, data);
        }
    FSGL_END

    FSGL_BEGIN(glGetBooleanv)
        static void APIENTRY call(GLenum pname, GLboolean* data)
        {
            if (!tRecording || !data) { if (!tRecording) P::passthrough(pname, data); else P::sync(pname, data); return; }
            if (pname == GL_COLOR_WRITEMASK && sSh.mColorMaskOk)
            {
                std::copy_n(sSh.mColorMask, 4, data);
                return;
            }
            if (pname == GL_DEPTH_WRITEMASK && sSh.mDepthMaskOk)
            {
                *data = sSh.mDepthMask;
                return;
            }
            P::sync(pname, data);
            // What was read is current from here on
            if (pname == GL_COLOR_WRITEMASK)
            {
                std::copy_n(data, 4, sSh.mColorMask);
                sSh.mColorMaskOk = true;
            }
            else if (pname == GL_DEPTH_WRITEMASK)
            {
                sSh.mDepthMask = *data;
                sSh.mDepthMaskOk = true;
            }
        }
    FSGL_END

    std::unordered_map<GLenum, const GLubyte*> sStrings;                    // main
    std::unordered_map<U64, const GLubyte*> sStringsI;                      // main

    FSGL_BEGIN(glGetString)
        static const GLubyte* APIENTRY call(GLenum name)
        {
            if (!tRecording) { return P::passthrough(name); }
            auto it = sStrings.find(name);
            if (it != sStrings.end())
            {
                return it->second;
            }
            const GLubyte* s = P::sync(name);
            if (s)
            {
                sStrings[name] = s;
            }
            return s;
        }
    FSGL_END

    FSGL_BEGIN(glGetStringi)
        static const GLubyte* APIENTRY call(GLenum name, GLuint index)
        {
            if (!tRecording) { return P::passthrough(name, index); }
            const U64 key = ((U64)name << 32) | index;
            auto it = sStringsI.find(key);
            if (it != sStringsI.end())
            {
                return it->second;
            }
            const GLubyte* s = P::sync(name, index);
            if (s)
            {
                sStringsI[key] = s;
            }
            return s;
        }
    FSGL_END

    //------------------------------------------------------------------------
    // Uniform and attribute locations are fixed once a program is linked:
    // answer repeats from a cache that a relink, binary load or delete clears.

    std::unordered_map<GLuint, std::unordered_map<std::string, GLint>> sUniformLocs;  // main
    std::unordered_map<GLuint, std::unordered_map<std::string, GLint>> sAttribLocs;   // main

    void forgetProgram(GLuint program)
    {
        sUniformLocs.erase(program);
        sAttribLocs.erase(program);
        eraseUniformValues(program);
    }

    template <typename P>
    GLint cachedLocation(std::unordered_map<GLuint, std::unordered_map<std::string, GLint>>& cache, GLuint program, const GLchar* name)
    {
        if (!name)
        {
            return P::sync(program, name);
        }
        auto& per_program = cache[program];
        auto it = per_program.find(name);
        if (it != per_program.end())
        {
            return it->second;
        }
        const GLint loc = P::sync(program, name);
        per_program.emplace(name, loc);
        return loc;
    }

    FSGL_BEGIN(glGetUniformLocation)
        static GLint APIENTRY call(GLuint program, const GLchar* name)
        {
            if (!tRecording) { return P::passthrough(program, name); }
            return cachedLocation<P>(sUniformLocs, program, name);
        }
    FSGL_END

    FSGL_BEGIN(glGetAttribLocation)
        static GLint APIENTRY call(GLuint program, const GLchar* name)
        {
            if (!tRecording) { return P::passthrough(program, name); }
            return cachedLocation<P>(sAttribLocs, program, name);
        }
    FSGL_END

    // Relinking or loading a binary into the program in use: whether its new executable is
    // installed depends on the outcome, so don't drop the next glUseProgram of it
    void forgetRelinked(GLuint program)
    {
        forgetProgram(program);
        if (sSh.mProgramOk && sSh.mProgram == program)
        {
            sSh.mProgramOk = false;
        }
    }

    FSGL_BEGIN(glLinkProgram)
        static void APIENTRY call(GLuint program)
        {
            if (tRecording) { forgetRelinked(program); }
            P::generic(program);
        }
    FSGL_END

    FSGL_BEGIN(glDeleteProgram)
        static void APIENTRY call(GLuint program)
        {
            if (tRecording) { forgetProgram(program); }
            P::generic(program);
        }
    FSGL_END

    FSGL_BEGIN(glProgramBinary)
        static void APIENTRY call(GLuint program, GLenum format, const void* binary, GLsizei length)
        {
            if (tRecording) { forgetRelinked(program); }
            P::generic(program, format, binary, length);
        }
    FSGL_END

    // A program deleted while in use lives on until it is no longer current, and its name may be
    // handed out again: whatever was cached for the name is not the new program's
    FSGL_BEGIN(glCreateProgram)
        static GLuint APIENTRY call()
        {
            const GLuint program = P::generic();
            if (tRecording && program) { forgetProgram(program); }
            return program;
        }
    FSGL_END

    FSGL_BEGIN(glCreateShaderProgramv)
        static GLuint APIENTRY call(GLenum type, GLsizei count, const GLchar* const* strings)
        {
            const GLuint program = P::generic(type, count, strings);
            if (tRecording && program) { forgetProgram(program); }
            return program;
        }
    FSGL_END

    // Attribute bindings take effect at the next link, which clears the cache.

    //------------------------------------------------------------------------
    // Installation

    template <int ID, typename PFN>
    void installOne(PFN* slot)
    {
        sReal[ID] = reinterpret_cast<void*>(*slot);
        if (!*slot)
        {
            return;
        }
        // Another thread (the texture worker) may call through the slot right away: the real
        // pointer must be visible before the proxy that reads it
        std::atomic_thread_fence(std::memory_order_release);
        if constexpr (Custom<ID>::kHas)
        {
            *slot = &Custom<ID>::call;
        }
        else
        {
            *slot = &Proxy<ID, PFN>::generic;
        }
    }

    //------------------------------------------------------------------------
    // Render thread

    FSGLStream::ContextOps sOps;
    std::thread* sThread = nullptr;  // never destroyed while running, so an exit without cleanup can't std::terminate
    std::atomic<int> sStartResult{ 0 };
    bool sRunning = false;                  // main
    std::atomic<U64> sPresented{ 0 };
    U64 sRecordedFrames = 0;                // main
    std::atomic<bool (*)(bool)> sRenderPoll{ nullptr };  // FSGLStream::setRenderPoll

    // Render thread: true while the poll hook has work left
    bool runRenderPoll()
    {
        bool (*fn)(bool) = sRenderPoll.load(std::memory_order_acquire);
        return fn && fn(false);
    }

    void execPresent(U8* p)
    {
        std::function<void()>* fn = *reinterpret_cast<std::function<void()>**>(p);
        {
            LL_PROFILE_ZONE_NAMED_CATEGORY_DISPLAY("FSRT present");
            (*fn)();
        }
        delete fn;
        sPresented.fetch_add(1, std::memory_order_release);
        sPresented.notify_all();
    }

    void renderMain()
    {
        tRenderThread = true;
        LL_PROFILER_SET_THREAD_NAME("FS Render");
        const bool ok = sOps.mAcquireOnRender && sOps.mAcquireOnRender();
        sStartResult.store(ok ? 1 : -1, std::memory_order_release);
        sStartResult.notify_all();
        if (!ok)
        {
            return;
        }

        bool flushed_for_queries = false;
        bool poll_pending = false;  // the poll hook has work left (readbacks waiting on fences)
        for (;;)
        {
            Chunk* c = nullptr;
            const U64 wait0 = nowNs();
            {
                std::unique_lock<std::mutex> lock(sQueueMutex);
                // Polling backs off while results keep not being ready (0.5 ms up to 4 ms):
                // occlusion queries are pending most of every frame, and a fixed 0.5 ms meant
                // about 2000 wake-ups and driver calls a second
                U32 poll_wait_us = 500;
                while (sQueue.empty())
                {
                    if (!sQueryPending.empty() || poll_pending)
                    {
                        // Nothing to replay: make sure the GPU sees the queries
                        // and keep collecting their results while we wait.
                        lock.unlock();
                        if (!flushed_for_queries)
                        {
                            FSGL_REAL(glFlush)();
                            flushed_for_queries = true;
                        }
                        const size_t pending_before = sQueryPending.size();
                        pollQueries();
                        if (poll_pending)
                        {
                            poll_pending = runRenderPoll();
                        }
                        if (sQueryPending.size() < pending_before)
                        {
                            poll_wait_us = 500;
                        }
                        else
                        {
                            poll_wait_us = std::min<U32>(poll_wait_us * 2, 4000);
                        }
                        lock.lock();
                        if (sQueue.empty())
                        {
                            sQueueCond.wait_for(lock, std::chrono::microseconds(poll_wait_us));
                        }
                    }
                    else
                    {
                        sQueueCond.wait(lock);
                    }
                }
                c = sQueue.front();
                sQueue.pop_front();
            }
            const U64 t0 = nowNs();
            sStatRenderIdleNs.fetch_add(t0 - wait0, std::memory_order_relaxed);
            if (c->mStop)
            {
                delete c;
                break;
            }
            {
                LL_PROFILE_ZONE_NAMED_CATEGORY_DISPLAY("FSRT replay");
                execChunk(c);
                pollQueries();
                poll_pending = runRenderPoll();
            }
            flushed_for_queries = false;
            freeChunk(c);
            sStatRenderBusyNs.fetch_add(nowNs() - t0, std::memory_order_relaxed);
        }
        if (sOps.mReleaseOnRender)
        {
            sOps.mReleaseOnRender();
        }
    }

    void resetStreamState()
    {
        sQueryIssued.clear();
        sQueryActive.clear();
        {
            std::lock_guard<std::mutex> lock(sQueryResultMutex);
            sQueryResults.clear();
        }
        sQueryPending.clear();
        sQueryRenderGen.clear();
        sStrings.clear();
        sStringsI.clear();
        sStale.clear();
        sUniformLocs.clear();
        sAttribLocs.clear();
        sPresented.store(0);
        sRecordedFrames = 0;
    }
}

namespace FSGLStream
{
    bool install()
    {
        if (sInstalled)
        {
            return true;
        }
#define FSGL_FN(n) installOne<FSGL_ID_##n>(&::n);
#define FSGL_WGL(n) installOne<FSGL_ID_##n>(&::n);
#include "fsglstream_funcs.inl"
#undef FSGL_FN
#undef FSGL_WGL
        sInstalled = true;
        LL_INFOS("FSRenderThread") << "GL command stream installed for " << (S32)FSGL_COUNT << " entry points" << LL_ENDL;
        return true;
    }

    bool isInstalled()
    {
        return sInstalled;
    }

    bool start(const ContextOps& ops)
    {
        if (!sInstalled || sRunning)
        {
            return false;
        }
        sOps = ops;
        sGLVersion = ops.mGLVersion;
        resetStreamState();
        snapshotState();

        sStartResult.store(0);
        sOps.mReleaseOnMain();
        sThread = new std::thread(renderMain);
        while (sStartResult.load(std::memory_order_acquire) == 0)
        {
            sStartResult.wait(0, std::memory_order_acquire);
        }
        if (sStartResult.load() < 0)
        {
            sThread->join();
            delete sThread;
            sThread = nullptr;
            sOps.mAcquireOnMain();
            LL_WARNS("FSRenderThread") << "The render thread could not make the GL context current; staying single-threaded" << LL_ENDL;
            return false;
        }
        sRunning = true;
        tRecording = true;
        return true;
    }

    void stop()
    {
        if (!sRunning)
        {
            return;
        }
        submitCurrent();
        Chunk* stop_chunk = new Chunk;
        stop_chunk->mStop = true;
        submitChunk(stop_chunk);
        sThread->join();
        delete sThread;
        sThread = nullptr;
        tRecording = false;
        sRunning = false;
        sOps.mAcquireOnMain();
        // what the render thread still waited on completes here, with the context
        if (bool (*fn)(bool) = sRenderPoll.load(std::memory_order_acquire))
        {
            fn(true);
        }
        resetStreamState();
        LL_INFOS("FSRenderThread") << "Render thread stopped; GL is back on the main thread" << LL_ENDL;
    }

    bool isRunning()
    {
        return sRunning;
    }

    bool isRecording()
    {
        return tRecording;
    }

    void runSync(const std::function<void()>& fn)
    {
        if (!tRecording)
        {
            fn();
            return;
        }
        ++sStatSyncs;
        auto g = [&]() { fn(); };
        runSyncImpl(g);
    }

    void recordCall(void (*fn)(U64), U64 value)
    {
        if (!tRecording)
        {
            fn(value);
            return;
        }
        struct Rec
        {
            void (*mFn)(U64);
            U64 mValue;
        };
        U8* p = allocRecord([](U8* q)
        {
            const Rec* r = reinterpret_cast<const Rec*>(q);
            r->mFn(r->mValue);
        }, sizeof(Rec));
        new (p) Rec{ fn, value };
        commitRecord();
    }

    void setRenderPoll(bool (*fn)(bool finish))
    {
        sRenderPoll.store(fn, std::memory_order_release);
    }

    void setFilter(bool filter)
    {
        if (filter && !sFilter)
        {
            clearUniformValues(); // not kept up to date while off
        }
        sFilter = filter;
    }

    void clearErrors()
    {
        if (!tRecording)
        {
            auto get_error = FSGL_REAL(glGetError);
            if (!get_error)
            {
                get_error = ::glGetError;
            }
            for (int i = 0; i < 16 && get_error() != GL_NO_ERROR; ++i)
            {
            }
            return;
        }
        allocRecord(&execClearErrors, 0);
        commitRecord();
    }

    void present(std::function<void()> fn)
    {
        if (!tRecording)
        {
            fn();
            return;
        }
        U8* p = allocRecord(&execPresent, sizeof(void*));
        *reinterpret_cast<std::function<void()>**>(p) = new std::function<void()>(std::move(fn));
        submitCurrent();
        ++sRecordedFrames;
        ++sStatPresents;
        ++sPresentCounter;

        // At most one frame in flight: wait for the previous present.
        if (sPresented.load(std::memory_order_acquire) + 1 < sRecordedFrames)
        {
            LL_PROFILE_ZONE_NAMED_CATEGORY_DISPLAY("FSRT frame wait");
            const U64 t0 = nowNs();
            for (;;)
            {
                const U64 done = sPresented.load(std::memory_order_acquire);
                if (done + 1 >= sRecordedFrames)
                {
                    break;
                }
                sPresented.wait(done, std::memory_order_acquire);
            }
            sStatPresentWaitNs += nowNs() - t0;
        }
    }

    void setCheck(bool check)
    {
        sCheck.store(check, std::memory_order_relaxed);
    }

    Stats takeStats()
    {
        Stats s;
        s.mPresents = sStatPresents;
        s.mRecordedCalls = sStatCalls;
        s.mRecordedBytes = sStatBytes;
        s.mSyncCalls = sStatSyncs;
        s.mSyncWaitMs = (F64)sStatSyncWaitNs * 1e-6;
        s.mPresentWaitMs = (F64)sStatPresentWaitNs * 1e-6;
        s.mRenderBusyMs = (F64)sStatRenderBusyNs.exchange(0) * 1e-6;
        s.mRenderIdleMs = (F64)sStatRenderIdleNs.exchange(0) * 1e-6;

        std::vector<std::pair<U32, int>> top;
        for (int i = 0; i < FSGL_COUNT; ++i)
        {
            if (sSyncCount[i])
            {
                top.emplace_back(sSyncCount[i], i);
                sSyncCount[i] = 0;
            }
        }
        std::sort(top.begin(), top.end(), [](const auto& a, const auto& b) { return a.first > b.first; });
        for (size_t i = 0; i < top.size() && i < 5; ++i)
        {
            if (i)
            {
                s.mTopSyncs += ", ";
            }
            s.mTopSyncs += llformat("%s x%u", sNames[top[i].second], top[i].first);
        }

        // Which calls make up the stream, per frame, and which were dropped as redundant
        const F64 frames = (F64)llmax(sStatPresents, 1u);
        std::vector<std::pair<U32, int>> calls;
        std::vector<std::pair<U32, int>> skips;
        for (int i = 0; i < FSGL_COUNT; ++i)
        {
            if (sCallCount[i])
            {
                calls.emplace_back(sCallCount[i], i);
            }
            if (sSkipCount[i])
            {
                skips.emplace_back(sSkipCount[i], i);
            }
        }
        std::sort(calls.begin(), calls.end(), [](const auto& a, const auto& b) { return a.first > b.first; });
        std::sort(skips.begin(), skips.end(), [](const auto& a, const auto& b) { return a.first > b.first; });
        for (size_t i = 0; i < calls.size() && i < 16; ++i)
        {
            const int id = calls[i].second;
            s.mTopCalls += llformat("%s%s %.0f (%.0f KB)", i ? ", " : "", sNames[id], (F64)calls[i].first / frames,
                                    (F64)sCallBytes[id] / frames / 1024.0);
        }
        // Which passes make them: the same name may be several literals, so merged by text
        {
            std::map<std::string, ZoneCalls> zones;
            for (const auto& [name, counts] : sZoneCalls)
            {
                ZoneCalls& z = zones[name ? name : "(outside any zone)"];
                z.mCalls += counts.mCalls;
                z.mDraws += counts.mDraws;
                z.mBinds += counts.mBinds;
            }
            std::vector<std::pair<std::string, ZoneCalls>> top_zones(zones.begin(), zones.end());
            std::sort(top_zones.begin(), top_zones.end(), [](const auto& a, const auto& b) { return a.second.mCalls > b.second.mCalls; });
            for (size_t i = 0; i < top_zones.size() && i < 16; ++i)
            {
                const ZoneCalls& z = top_zones[i].second;
                s.mTopZones += llformat("%s%s %.0f (draws %.0f, texture binds %.0f)", i ? ", " : "", top_zones[i].first.c_str(),
                                        (F64)z.mCalls / frames, (F64)z.mDraws / frames, (F64)z.mBinds / frames);
            }
            sZoneCalls.clear();
            sLastZone = nullptr;
            sLastZoneCalls = nullptr;
        }
        for (size_t i = 0; i < skips.size() && i < 10; ++i)
        {
            s.mTopSkips += llformat("%s%s %.0f", i ? ", " : "", sNames[skips[i].second], (F64)skips[i].first / frames);
        }
        s.mSkippedCalls = sStatSkips;
        memset(sCallCount, 0, sizeof(sCallCount));
        memset(sCallBytes, 0, sizeof(sCallBytes));
        memset(sSkipCount, 0, sizeof(sSkipCount));
        sStatSkips = 0;

        sStatPresents = 0;
        sStatCalls = 0;
        sStatBytes = 0;
        sStatSyncs = 0;
        sStatSyncWaitNs = 0;
        sStatPresentWaitNs = 0;
        return s;
    }
}

#else // !LL_WINDOWS

namespace FSGLStream
{
    bool install() { return false; }
    bool isInstalled() { return false; }
    bool start(const ContextOps&) { return false; }
    void stop() {}
    bool isRunning() { return false; }
    bool isRecording() { return false; }
    void runSync(const std::function<void()>& fn) { fn(); }
    void present(std::function<void()> fn) { fn(); }
    void setCheck(bool) {}
    Stats takeStats() { return Stats(); }
}

#endif // LL_WINDOWS
