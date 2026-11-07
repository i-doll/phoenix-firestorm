/**
 * @file fsrepresentativefragment.h
 * @brief GL_NV_representative_fragment_test around occlusion query draws (FSPerfRepresentativeFragmentTest)
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

#ifndef FS_REPRESENTATIVEFRAGMENT_H
#define FS_REPRESENTATIVEFRAGMENT_H

#include "llgl.h"
#include "llglslshader.h"
#include "llrender.h"
#include "llviewercontrol.h"

// Scoped GL_NV_representative_fragment_test for a run of occlusion query box draws.
//
// With the test enabled the GPU may stop rasterizing a triangle once one of its fragments has
// passed the depth test, and drop the rest. That only changes the outcome of a draw that has
// no visible effect except its query, so the scope enables it only when the current state is
// query-only: color writes, depth writes and the stencil test all off. The state is read back
// from GL, not from the viewer's caches, so a cache that drifted can't let a writing draw
// through. The bound shader must have no side effects either (occlusionF.glsl writes only
// color, which the mask drops).
//
// Every passing triangle still keeps at least one fragment, so a query whose consumer only asks
// whether any sample passed (GL_ANY_SAMPLES_PASSED, or GL_SAMPLES_PASSED tested against 0) gets
// the same answer. Queries that need the actual count (llscenemonitor, the shader profiler's
// GL_SAMPLES_PASSED) must not run inside the scope; it stays off while the shader profiler is on.
//
// With FSPerfRepresentativeFragmentTest off, or without the extension, the scope does nothing
// beyond one cached bool read.
class FSRepresentativeFragmentScope
{
public:
    FSRepresentativeFragmentScope()
    {
        static LLCachedControl<bool> enabled(gSavedSettings, "FSPerfRepresentativeFragmentTest", false);
        if (!enabled || !gGLManager.mHasNVRepresentativeFragmentTest || LLGLSLShader::sProfileEnabled)
        {
            return;
        }

        gGL.flush();
        GLboolean color_mask[4] = { GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE };
        GLboolean depth_mask = GL_TRUE;
        glGetBooleanv(GL_COLOR_WRITEMASK, color_mask);
        glGetBooleanv(GL_DEPTH_WRITEMASK, &depth_mask);
        if (color_mask[0] || color_mask[1] || color_mask[2] || color_mask[3] || depth_mask ||
            glIsEnabled(GL_STENCIL_TEST))
        {
            return; // this run writes something: leave every fragment in
        }

        glEnable(GL_REPRESENTATIVE_FRAGMENT_TEST_NV);
        mEnabled = true;
    }

    ~FSRepresentativeFragmentScope()
    {
        if (mEnabled)
        {
            gGL.flush();
            glDisable(GL_REPRESENTATIVE_FRAGMENT_TEST_NV);
        }
    }

    FSRepresentativeFragmentScope(const FSRepresentativeFragmentScope&) = delete;
    FSRepresentativeFragmentScope& operator=(const FSRepresentativeFragmentScope&) = delete;

private:
    bool mEnabled = false;
};

#endif // FS_REPRESENTATIVEFRAGMENT_H
