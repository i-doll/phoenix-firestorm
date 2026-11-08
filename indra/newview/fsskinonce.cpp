/**
 * @file fsskinonce.cpp
 * @brief Skin rigged meshes once per frame instead of in every pass (FSPerfSkinOncePerFrame)
 *
 * $LicenseInfo:firstyear=2026&license=viewerlgpl$
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

#include "fsskinonce.h"

#include "llgl.h"
#include "llglslshader.h"
#include "llrender.h"
#include "llshadermgr.h"
#include "llviewercontrol.h"

#include <unordered_map>

extern U32 gFrameCount;

namespace
{
    // One captured vertex: the three rows of its blended mat3x4, as RGBA32F texels
    constexpr U32 BYTES_PER_VERTEX = 3 * 4 * sizeof(F32);
    constexpr U32 MIN_POOL_VERTICES = 64 * 1024;    // 3 MB
    constexpr U32 SWEEP_INTERVAL = 32;              // frames between sweeps of unused captures
    constexpr U32 MAX_UNUSED_FRAMES = 120;          // a capture unused this long is freed

    const char* const CAPTURE_SHADER = "avatar/fsSkinOnceV.glsl";
    const char* const SKIN_OBJECT = "avatar/objectSkinV.glsl";

    // ---- what uploadMatrixPalette() last uploaded to a program ----------------------------
    // Kept per program, exactly like the palette uniforms it describes: passes interleave
    // programs and rely on each one keeping its own palette.
    struct SkinContext
    {
        const void* mAvatar = nullptr;
        U64 mSkinHash = 0;
        const F32* mPalette = nullptr;  // MatrixPaletteCache::mGLMp, valid for the frame it was reported in
        U32 mJointCount = 0;
        U32 mPaletteSerial = 0;
        U32 mFrame = 0;
    };

    // ---- the capture program -------------------------------------------------------------
    GLuint sCaptureProgram = 0;
    GLint sCapturePaletteLoc = -1;
    GLint sCapturePaletteSizeLoc = -1;
    U32 sCaptureUploadedSerial = 0;     // palette serial last uploaded to the capture program
    bool sCaptureFailed = false;        // don't retry a failed build until the next shader load

    // ---- the pool: one buffer of captured vertices, read through one buffer texture ------
    GLuint sPoolBuffer = 0;
    GLuint sPoolTexture = 0;
    U32 sPoolCapacity = 0;              // vertices
    U32 sPoolUsed = 0;                  // vertices
    U32 sPoolNextCapacity = MIN_POOL_VERTICES;
    U32 sPoolMaxVertices = 0;           // from GL_MAX_TEXTURE_BUFFER_SIZE
    std::map<U32, U32> sPoolFree;       // offset -> count of free vertex runs
    S32 sTextureUnit = -1;              // reserved unit the pool texture is bound on
    bool sPoolTextureBound = false;     // bound on sTextureUnit since the last unbind

    // ---- one capture per (vertex buffer, vertex range) ------------------------------------
    struct CaptureKey
    {
        const LLVertexBuffer* mBuffer;
        U32 mStart;
        U32 mEnd;
        bool operator==(const CaptureKey& other) const
        {
            return mBuffer == other.mBuffer && mStart == other.mStart && mEnd == other.mEnd;
        }
    };
    struct CaptureKeyHash
    {
        size_t operator()(const CaptureKey& key) const
        {
            size_t h = std::hash<const void*>()(key.mBuffer);
            h ^= (size_t(key.mStart) * 0x9E3779B97F4A7C15ull) + (h << 6) + (h >> 2);
            h ^= (size_t(key.mEnd) * 0xC2B2AE3D27D4EB4Full) + (h << 6) + (h >> 2);
            return h;
        }
    };
    struct Capture
    {
        U32 mOffset = 0;                // first pool vertex
        U32 mCount = 0;                 // pool vertices owned, 0 = none
        U32 mContentSerial = 0;         // LLVertexBuffer::getFSContentSerial() when captured
        const void* mAvatar = nullptr;
        U64 mSkinHash = 0;
        U32 mPaletteSerial = 0;         // MatrixPaletteCache::mFSBuildSerial when captured
        U32 mLastUsed = 0;
        bool mValid = false;
    };
    std::unordered_map<CaptureKey, Capture, CaptureKeyHash> sCaptures;

    // ---- per rigged program: uniform locations and the values last set -------------------
    struct ProgramState
    {
        GLuint mProgram = 0;
        U32 mFrame = 0;                 // locations are re-read once a frame
        GLint mOnceLoc = -1;
        GLint mBaseLoc = -1;
        S32 mOnce = -1;
        S32 mBase = 0;
        bool mUsable = false;
        SkinContext mContext;
    };
    std::unordered_map<const LLGLSLShader*, ProgramState> sPrograms;

    // ---- measurement ---------------------------------------------------------------------
    struct FrameStats
    {
        U32 mDraws = 0;                 // rigged draws: each was a skinning run before
        U64 mDrawVertices = 0;
        U32 mCaptured = 0;              // skinning runs this frame with the feature
        U64 mCapturedVertices = 0;
        U32 mFallbacks = 0;             // rigged draws that skinned in the shader as before
    };
    FrameStats sStats;
    U32 sStatsFrame = 0;

    // ---------------------------------------------------------------------------------------

    void setActiveUnitTexture(GLuint texture)
    {
        const U32 current = gGL.getCurrentTexUnitIndex();
        glActiveTexture(GL_TEXTURE0 + sTextureUnit);
        glBindTexture(GL_TEXTURE_BUFFER, texture);
        glActiveTexture(GL_TEXTURE0 + current);
    }

    void bindPoolTexture()
    {
        if (!sPoolTextureBound && sPoolTexture)
        {
            setActiveUnitTexture(sPoolTexture);
            sPoolTextureBound = true;
        }
    }

    void unbindPoolTexture()
    {
        if (sPoolTextureBound)
        {
            setActiveUnitTexture(0);
            sPoolTextureBound = false;
        }
    }

    void releasePool()
    {
        unbindPoolTexture();
        if (sPoolTexture)
        {
            glDeleteTextures(1, &sPoolTexture);
            sPoolTexture = 0;
        }
        if (sPoolBuffer)
        {
            glDeleteBuffers(1, &sPoolBuffer);
            sPoolBuffer = 0;
        }
        sPoolCapacity = 0;
        sPoolUsed = 0;
        sPoolFree.clear();
        sCaptures.clear();
    }

    void releaseProgram()
    {
        if (sCaptureProgram)
        {
            glDeleteProgram(sCaptureProgram);
            sCaptureProgram = 0;
        }
        sCapturePaletteLoc = -1;
        sCapturePaletteSizeLoc = -1;
        sCaptureUploadedSerial = 0;
    }

    // Returns a vertex run to the free list, merging it with its neighbours
    void addFreeRun(U32 offset, U32 count)
    {
        auto next = sPoolFree.lower_bound(offset);
        if (next != sPoolFree.begin())
        {
            auto prev = std::prev(next);
            if (prev->first + prev->second == offset)
            {
                offset = prev->first;
                count += prev->second;
                sPoolFree.erase(prev);
            }
        }
        if (next != sPoolFree.end() && offset + count == next->first)
        {
            count += next->second;
            sPoolFree.erase(next);
        }
        sPoolFree[offset] = count;
    }

    void freeRun(U32 offset, U32 count)
    {
        if (count)
        {
            sPoolUsed -= count;
            addFreeRun(offset, count);
        }
    }

    // Grows the pool to hold at least min_capacity vertices, keeping what it holds
    bool growPool(U32 min_capacity)
    {
        U32 capacity = llmax(sPoolCapacity ? sPoolCapacity * 2 : sPoolNextCapacity, MIN_POOL_VERTICES);
        while (capacity < min_capacity)
        {
            capacity *= 2;
        }
        capacity = llmin(capacity, sPoolMaxVertices);
        if (capacity < min_capacity || capacity <= sPoolCapacity)
        {
            return false;
        }

        GLuint buffer = 0;
        glGenBuffers(1, &buffer);
        if (!buffer)
        {
            return false;
        }
        // GL_COPY_* targets leave the GL_ARRAY_BUFFER binding LLVertexBuffer tracks alone
        glBindBuffer(GL_COPY_WRITE_BUFFER, buffer);
        glBufferData(GL_COPY_WRITE_BUFFER, (GLsizeiptr)capacity * BYTES_PER_VERTEX, nullptr, GL_DYNAMIC_COPY);
        if (sPoolBuffer && sPoolCapacity)
        {
            glBindBuffer(GL_COPY_READ_BUFFER, sPoolBuffer);
            glCopyBufferSubData(GL_COPY_READ_BUFFER, GL_COPY_WRITE_BUFFER, 0, 0, (GLsizeiptr)sPoolCapacity * BYTES_PER_VERTEX);
            glBindBuffer(GL_COPY_READ_BUFFER, 0);
        }
        glBindBuffer(GL_COPY_WRITE_BUFFER, 0);
        if (sPoolBuffer)
        {
            glDeleteBuffers(1, &sPoolBuffer);
        }

        if (!sPoolTexture)
        {
            glGenTextures(1, &sPoolTexture);
        }
        sPoolBuffer = buffer;
        const U32 current = gGL.getCurrentTexUnitIndex();
        glActiveTexture(GL_TEXTURE0 + sTextureUnit);
        glBindTexture(GL_TEXTURE_BUFFER, sPoolTexture);
        glTexBuffer(GL_TEXTURE_BUFFER, GL_RGBA32F, sPoolBuffer);
        glActiveTexture(GL_TEXTURE0 + current);
        sPoolTextureBound = true;

        addFreeRun(sPoolCapacity, capacity - sPoolCapacity);
        sPoolCapacity = capacity;
        LL_DEBUGS("FSSkinOnce") << "Pool grown to " << capacity << " vertices ("
            << (capacity * BYTES_PER_VERTEX) / (1024 * 1024) << " MB)" << LL_ENDL;
        return true;
    }

    bool allocRun(U32 count, U32& offset)
    {
        for (int attempt = 0; attempt < 2; ++attempt)
        {
            for (auto it = sPoolFree.begin(); it != sPoolFree.end(); ++it)
            {
                if (it->second >= count)
                {
                    offset = it->first;
                    const U32 rest = it->second - count;
                    sPoolFree.erase(it);
                    if (rest)
                    {
                        sPoolFree[offset + count] = rest;
                    }
                    sPoolUsed += count;
                    return true;
                }
            }
            // Growing adds at least count vertices at the end, after any free tail
            if (attempt == 0 && !growPool(sPoolCapacity + count))
            {
                return false;
            }
        }
        return false;
    }

    // Frees captures nobody drew for a while (avatars that left, old LODs, rebuilt buffers),
    // and starts over with a smaller pool when most of it went unused.
    void sweep()
    {
        for (auto it = sCaptures.begin(); it != sCaptures.end(); )
        {
            if (gFrameCount - it->second.mLastUsed > MAX_UNUSED_FRAMES)
            {
                freeRun(it->second.mOffset, it->second.mCount);
                it = sCaptures.erase(it);
            }
            else
            {
                ++it;
            }
        }

        if (sPoolCapacity > MIN_POOL_VERTICES && sPoolUsed * 4 < sPoolCapacity)
        {
            U32 next = MIN_POOL_VERTICES;
            while (next < sPoolUsed * 2)
            {
                next *= 2;
            }
            LL_DEBUGS("FSSkinOnce") << "Pool shrinks from " << sPoolCapacity << " to " << next
                << " vertices, " << sCaptures.size() << " captures are redone" << LL_ENDL;
            releasePool();
            sPoolNextCapacity = next;
        }
    }

    void beginFrame()
    {
        if (sStatsFrame == gFrameCount)
        {
            return;
        }
        if (sStats.mDraws)
        {
            LL_DEBUGS("FSSkinOnce") << "Frame " << sStatsFrame << ": " << sStats.mDraws
                << " rigged draws over " << sStats.mDrawVertices << " vertices (each skinned in the shader before), "
                << sStats.mCaptured << " captures over " << sStats.mCapturedVertices << " vertices, "
                << sStats.mFallbacks << " draws skinned in the shader, pool "
                << sPoolUsed << "/" << sPoolCapacity << " vertices, " << sCaptures.size() << " captures held" << LL_ENDL;
        }
        sStats = FrameStats();
        sStatsFrame = gFrameCount;
        // Rebind once a frame rather than trust that nothing touched the unit since
        sPoolTextureBound = false;
        bindPoolTexture();

        if (gFrameCount % SWEEP_INTERVAL == 0)
        {
            sweep();
        }
    }

    bool buildCaptureProgram()
    {
        if (sCaptureProgram)
        {
            return true;
        }
        if (sCaptureFailed || LLShaderMgr::instance()->fsIsBatching())
        {
            return false;
        }

        auto& objects = LLShaderMgr::instance()->mVertexShaderObjects;
        auto skin = objects.find(SKIN_OBJECT);
        if (skin == objects.end() || !skin->second)
        {
            sCaptureFailed = true;
            return false;
        }

        S32 level = 1;
        GLuint main_object = LLShaderMgr::instance()->loadShaderFile(CAPTURE_SHADER, level, GL_VERTEX_SHADER, nullptr);
        objects.erase(CAPTURE_SHADER); // owned here, not by the shared object map
        if (!main_object)
        {
            LL_WARNS("FSSkinOnce") << "Capture shader failed to compile, skinning stays per pass" << LL_ENDL;
            sCaptureFailed = true;
            return false;
        }

        GLuint program = glCreateProgram();
        glAttachShader(program, skin->second);
        glAttachShader(program, main_object);
        glBindAttribLocation(program, LLVertexBuffer::TYPE_WEIGHT4, "weight4");
        const GLchar* varyings[] = { "fs_skin_row0", "fs_skin_row1", "fs_skin_row2" };
        glTransformFeedbackVaryings(program, 3, varyings, GL_INTERLEAVED_ATTRIBS);
        glLinkProgram(program);
        glDetachShader(program, skin->second);
        glDetachShader(program, main_object);
        glDeleteShader(main_object);

        GLint linked = GL_FALSE;
        glGetProgramiv(program, GL_LINK_STATUS, &linked);
        GLint palette = glGetUniformLocation(program, "matrixPalette");
        GLint palette_size = glGetUniformLocation(program, "matrixPaletteSize");
        if (linked != GL_TRUE || palette < 0 || palette_size < 0
            || glGetAttribLocation(program, "weight4") != LLVertexBuffer::TYPE_WEIGHT4)
        {
            LL_WARNS("FSSkinOnce") << "Capture program failed to link, skinning stays per pass" << LL_ENDL;
            glDeleteProgram(program);
            sCaptureFailed = true;
            return false;
        }

        // The capture never takes the buffer path, but keep its sampler off unit 0
        GLint sampler = glGetUniformLocation(program, "fs_skin_buffer");
        if (sampler >= 0)
        {
            glUseProgram(program);
            glUniform1i(sampler, sTextureUnit);
            glUseProgram(LLGLSLShader::sCurBoundShaderPtr ? LLGLSLShader::sCurBoundShaderPtr->mProgramObject : 0);
        }

        sCaptureProgram = program;
        sCapturePaletteLoc = palette;
        sCapturePaletteSizeLoc = palette_size;
        sCaptureUploadedSerial = 0;
        LL_INFOS("FSSkinOnce") << "Skinning rigged meshes once per frame, pool texture unit " << sTextureUnit << LL_ENDL;
        return true;
    }

    // Runs the capture program over vertices [start, start + count) of vb, the buffer bound
    // for the draw in progress, into pool vertices [offset, offset + count).
    void capture(const SkinContext& context, U32 start, U32 count, U32 offset)
    {
        LL_PROFILE_ZONE_SCOPED_CATEGORY_AVATAR;

        // The draw's setBuffer() already pointed the weight4 attribute at vb; the capture
        // program reads that attribute only, at the same location.
        unbindPoolTexture();
        glUseProgram(sCaptureProgram);
        if (sCaptureUploadedSerial != context.mPaletteSerial)
        {
            glUniformMatrix3x4fv(sCapturePaletteLoc, context.mJointCount, GL_FALSE, context.mPalette);
            glUniform1i(sCapturePaletteSizeLoc, context.mJointCount);
            sCaptureUploadedSerial = context.mPaletteSerial;
        }

        glEnable(GL_RASTERIZER_DISCARD);
        glBindBufferRange(GL_TRANSFORM_FEEDBACK_BUFFER, 0, sPoolBuffer,
            (GLintptr)offset * BYTES_PER_VERTEX, (GLsizeiptr)count * BYTES_PER_VERTEX);
        glBeginTransformFeedback(GL_POINTS);
        glDrawArrays(GL_POINTS, start, count);
        glEndTransformFeedback();
        glBindBufferBase(GL_TRANSFORM_FEEDBACK_BUFFER, 0, 0);
        glDisable(GL_RASTERIZER_DISCARD);

        glUseProgram(LLGLSLShader::sCurBoundShaderPtr->mProgramObject);

        ++sStats.mCaptured;
        sStats.mCapturedVertices += count;
    }

    ProgramState* programState(const LLGLSLShader* shader)
    {
        ProgramState& state = sPrograms[shader];
        if (state.mProgram != shader->mProgramObject || state.mFrame != gFrameCount)
        {
            // Re-read once a frame: a reloaded program may reuse a name
            if (state.mProgram != shader->mProgramObject)
            {
                state.mContext = SkinContext();
            }
            state.mProgram = shader->mProgramObject;
            state.mFrame = gFrameCount;
            state.mOnceLoc = glGetUniformLocation(state.mProgram, "fs_skin_once");
            state.mBaseLoc = glGetUniformLocation(state.mProgram, "fs_skin_base");
            const GLint sampler = glGetUniformLocation(state.mProgram, "fs_skin_buffer");
            state.mUsable = state.mOnceLoc >= 0 && state.mBaseLoc >= 0 && sampler >= 0
                && shader->mActiveTextureChannels <= sTextureUnit
                && (shader->mAttributeMask & LLVertexBuffer::MAP_WEIGHT4);
            if (state.mUsable)
            {
                glUniform1i(sampler, sTextureUnit); // the program is the bound one
            }
            state.mOnce = -1;
            state.mBase = S32_MIN;
        }
        return &state;
    }

    void setUniforms(ProgramState& state, S32 once, S32 base)
    {
        if (state.mOnce != once)
        {
            glUniform1i(state.mOnceLoc, once);
            state.mOnce = once;
        }
        if (once && state.mBase != base)
        {
            glUniform1i(state.mBaseLoc, base);
            state.mBase = base;
        }
    }

    // LLVertexBuffer::drawRange() hook: vb is bound and an object-skinning program is current
    void onRiggedDraw(const LLVertexBuffer* vb, U32 start, U32 end)
    {
        const LLGLSLShader* shader = LLGLSLShader::sCurBoundShaderPtr;
        beginFrame();

        ProgramState* state = programState(shader);
        if (!state->mUsable)
        {
            return; // nothing to switch; the program skins as before
        }

        const U32 count = end >= start ? end - start + 1 : 0;
        ++sStats.mDraws;
        sStats.mDrawVertices += count;

        // The palette must have been uploaded this frame: a stale context is never trusted
        const SkinContext& context = state->mContext;
        const bool have_context = context.mFrame == gFrameCount && context.mPalette && context.mJointCount
            && context.mPaletteSerial;
        if (!count || !have_context || !vb->hasDataType(LLVertexBuffer::TYPE_WEIGHT4) || !buildCaptureProgram())
        {
            ++sStats.mFallbacks;
            setUniforms(*state, 0, 0);
            return;
        }

        Capture& entry = sCaptures[CaptureKey{ vb, start, end }];
        entry.mLastUsed = gFrameCount;

        const U32 content = vb->getFSContentSerial();
        const bool current = entry.mValid && entry.mCount == count
            && entry.mContentSerial == content
            && entry.mAvatar == context.mAvatar
            && entry.mSkinHash == context.mSkinHash
            && entry.mPaletteSerial == context.mPaletteSerial;
        if (!current)
        {
            if (entry.mCount != count)
            {
                freeRun(entry.mOffset, entry.mCount);
                entry.mCount = 0;
                entry.mValid = false;
                if (!allocRun(count, entry.mOffset))
                {
                    sCaptures.erase(CaptureKey{ vb, start, end });
                    ++sStats.mFallbacks;
                    setUniforms(*state, 0, 0);
                    return;
                }
                entry.mCount = count;
            }
            capture(context, start, count, entry.mOffset);
            entry.mContentSerial = content;
            entry.mAvatar = context.mAvatar;
            entry.mSkinHash = context.mSkinHash;
            entry.mPaletteSerial = context.mPaletteSerial;
            entry.mValid = true;
        }

        bindPoolTexture();
        // Pool vertex for gl_VertexID v is offset + (v - start)
        setUniforms(*state, 1, S32(entry.mOffset) - S32(start));
    }

    bool supported()
    {
        if (gGLManager.mGLVersion < 3.1f)
        {
            return false;
        }
        GLint combined = 0;
        glGetIntegerv(GL_MAX_COMBINED_TEXTURE_IMAGE_UNITS, &combined);
        GLint max_texels = 0;
        glGetIntegerv(GL_MAX_TEXTURE_BUFFER_SIZE, &max_texels);
        sPoolMaxVertices = U32(llmax(max_texels, 0)) / 3;
        // Well above anything LLTexUnit or a rigged program's samplers use
        sTextureUnit = combined - 1;
        return sTextureUnit >= 16 && sPoolMaxVertices >= MIN_POOL_VERTICES;
    }
}

namespace FSSkinOnce
{
    void configureShaders(std::map<std::string, std::string>& global_defines)
    {
        static LLCachedControl<bool> skin_once(gSavedSettings, "FSPerfSkinOncePerFrame", false);
        cleanupGL();
        if (skin_once && supported())
        {
            global_defines["FS_SKIN_ONCE"] = "1";
            LLVertexBuffer::sFSRiggedDrawHook = &onRiggedDraw;
        }
    }

    void cleanupGL()
    {
        LLVertexBuffer::sFSRiggedDrawHook = nullptr;
        releasePool();
        releaseProgram();
        sPoolNextCapacity = MIN_POOL_VERTICES;
        sCaptureFailed = false;
        sPrograms.clear();
    }

    void noteSkinImpl(const void* avatar, U64 skin_hash, const F32* gl_palette, U32 joint_count, U32 palette_serial)
    {
        const LLGLSLShader* shader = LLGLSLShader::sCurBoundShaderPtr;
        if (!shader || !shader->mFeatures.hasObjectSkinning)
        {
            return;
        }
        SkinContext& context = sPrograms[shader].mContext;
        context.mAvatar = avatar;
        context.mSkinHash = skin_hash;
        context.mPalette = gl_palette;
        context.mJointCount = joint_count;
        context.mPaletteSerial = palette_serial;
        context.mFrame = gFrameCount;
    }
}
