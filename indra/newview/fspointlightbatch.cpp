/**
 * @file fspointlightbatch.cpp
 * @brief Instanced drawing of deferred local point lights
 *
 * $LicenseInfo:firstyear=2026&license=fsviewerlgpl$
 * Phoenix Firestorm Viewer Source Code
 * Copyright (C) 2026, Trish
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

#include "fspointlightbatch.h"

#include "llcamera.h"
#include "llenvironment.h"
#include "llglheaders.h"
#include "llglslshader.h"
#include "llrender.h"
#include "llvertexbuffer.h"
#include "llviewercontrol.h"
#include "llviewershadermgr.h"
#include "pipeline.h"

extern U32 gFrameCount;

FSPointLightBatch gFSPointLightBatch;

namespace
{
    constexpr U32 FS_VEC4_PER_LIGHT = 2;
    constexpr U32 FS_CHUNK_BYTES    = FSPointLightBatch::FS_LIGHTS_PER_DRAW * FS_VEC4_PER_LIGHT * sizeof(LLVector4); // 16384

    // Unit cube faces in the order of fs_face_normal[] in fsBatchedPointLightV.glsl:
    // -X, +X, -Y, +Y, -Z, +Z. The vertex shader finds a vertex's face from gl_VertexID / 6
    // and drops faces the camera is behind, so winding does not matter and face culling
    // is left off.
    LLPointer<LLVertexBuffer> create_box_vb()
    {
        LLPointer<LLVertexBuffer> vb = new LLVertexBuffer(LLVertexBuffer::MAP_VERTEX);
        LLStrider<LLVector3> pos;
        if (!vb->allocateBuffer(36, 0) || !vb->getVertexStrider(pos))
        {
            return nullptr;
        }

        for (U32 axis = 0; axis < 3; ++axis)
        {
            const U32 u = (axis + 1) % 3;
            const U32 v = (axis + 2) % 3;
            for (S32 side = 0; side < 2; ++side)
            {
                const F32 sign = side ? 1.f : -1.f;
                LLVector3 corner[4];
                const F32 uv[4][2] = { { -1.f, -1.f }, { 1.f, -1.f }, { 1.f, 1.f }, { -1.f, 1.f } };
                for (U32 i = 0; i < 4; ++i)
                {
                    corner[i].mV[axis] = sign;
                    corner[i].mV[u]    = uv[i][0];
                    corner[i].mV[v]    = uv[i][1];
                }
                *pos++ = corner[0]; *pos++ = corner[1]; *pos++ = corner[2];
                *pos++ = corner[0]; *pos++ = corner[2]; *pos++ = corner[3];
            }
        }

        vb->unmapBuffer();
        return vb;
    }
}

bool FSPointLightBatch::begin()
{
    static LLCachedControl<bool> batched(gSavedSettings, "FSPerfBatchedLocalLights", false);

    mData.clear();

    if (!batched || !gFSBatchedPointLightProgram.isComplete())
    {
        return false;
    }

    if (mUBOAlignment < 0)
    {
        GLint align = 0;
        glGetIntegerv(GL_UNIFORM_BUFFER_OFFSET_ALIGNMENT, &align);
        // Every chunk starts at a multiple of 16 KB; an alignment that does not divide that
        // (none known) falls back to the stock path for good.
        mUBOAlignment = (align > 0 && (FS_CHUNK_BYTES % (U32)align) == 0) ? align : 0;
        LL_INFOS("FSBatchedLights") << "Uniform buffer offset alignment " << align
                                    << (mUBOAlignment ? "" : ", batched local lights unavailable") << LL_ENDL;
    }

    return mUBOAlignment > 0;
}

void FSPointLightBatch::flush(LLCamera* camera)
{
    LL_PROFILE_ZONE_SCOPED_CATEGORY_PIPELINE;

    const U32 count = (U32)(mData.size() / FS_VEC4_PER_LIGHT);
    U32 draws = 0;
    U32 gl_calls = 0;

    if (count > 0)
    {
        if (mBoxVB.isNull())
        {
            mBoxVB = create_box_vb();
        }
        if (!mUBO)
        {
            glGenBuffers(1, &mUBO);
        }

        if (mBoxVB.notNull() && mUBO)
        {
            LL_PROFILE_GPU_ZONE("batched local lights");

            // One upload for every light this frame, padded to whole 16 KB chunks so each
            // bound range backs the shader's full uniform block.
            const U32 chunks = (count + FS_LIGHTS_PER_DRAW - 1) / FS_LIGHTS_PER_DRAW;
            mData.resize((size_t)chunks * FS_LIGHTS_PER_DRAW * FS_VEC4_PER_LIGHT, LLVector4(0.f, 0.f, 0.f, 0.f));

            glBindBuffer(GL_UNIFORM_BUFFER, mUBO);
            glBufferData(GL_UNIFORM_BUFFER, (GLsizeiptr)chunks * FS_CHUNK_BYTES, mData.data(), GL_STREAM_DRAW);
            glBindBuffer(GL_UNIFORM_BUFFER, 0);
            gl_calls += 3;

            LLGLSLShader& shader = gFSBatchedPointLightProgram;
            LLGLDepthTest depth(GL_TRUE, GL_FALSE);
            LLGLDisable cull(GL_CULL_FACE);

            gPipeline.bindDeferredShader(shader);

            // Per frame, not per light (the stock loop sets it for every light).
            static LLStaticHashedString s_camera_origin("fs_camera_origin");
            LLSettingsSky::ptr_t psky = LLEnvironment::instance().getCurrentSky();
            shader.uniform1i(LLShaderMgr::CLASSIC_MODE, (psky && psky->canAutoAdjust()) ? 1 : 0);
            shader.uniform3fv(s_camera_origin, 1, camera->getOrigin().mV);
            gl_calls += 2;

            mBoxVB->setBuffer();
            gGL.syncMatrices();

            for (U32 chunk = 0; chunk < chunks; ++chunk)
            {
                const U32 in_chunk = llmin(count - chunk * FS_LIGHTS_PER_DRAW, FS_LIGHTS_PER_DRAW);
                glBindBufferRange(GL_UNIFORM_BUFFER, FS_UBO_BINDING, mUBO, (GLintptr)chunk * FS_CHUNK_BYTES, FS_CHUNK_BYTES);
                glDrawArraysInstanced(GL_TRIANGLES, 0, 36, in_chunk);
                gl_calls += 2;
                ++draws;
            }

            gPipeline.unbindDeferredShader(shader);
        }
    }

    mFrameLights  = count;
    mFrameDraws   = draws;
    mFrameGLCalls = gl_calls;
    mData.clear();

    // Stock path for comparison: one draw and four or five uniform setters (each a hash
    // lookup, a glUniform* when the value changed) per light.
    if (gFrameCount % 300 == 0)
    {
        LL_DEBUGS("FSBatchedLights") << "Batched point lights: " << mFrameLights << " lights, " << mFrameDraws
                                     << " draws, " << mFrameGLCalls
                                     << " GL calls besides the program bind (stock path: " << mFrameLights
                                     << " draws, up to " << mFrameLights * 5 << " uniform sets)" << LL_ENDL;
    }
}

// static
void FSPointLightBatch::setupProgram(LLGLSLShader& shader)
{
    if (!shader.mProgramObject)
    {
        return;
    }
    GLuint index = glGetUniformBlockIndex(shader.mProgramObject, "FSPointLights");
    if (index != GL_INVALID_INDEX)
    {
        glUniformBlockBinding(shader.mProgramObject, index, FS_UBO_BINDING);
    }
    else
    {
        LL_WARNS("FSBatchedLights") << "FSPointLights block missing from " << shader.mName << LL_ENDL;
    }
}

void FSPointLightBatch::release()
{
    mBoxVB = nullptr;
    if (mUBO)
    {
        glDeleteBuffers(1, &mUBO);
        mUBO = 0;
    }
    mData.clear();
    mData.shrink_to_fit();
    mUBOAlignment = -1;
}
