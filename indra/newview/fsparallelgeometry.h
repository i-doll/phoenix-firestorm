/**
 * @file fsparallelgeometry.h
 * @brief Fills volume face vertex data on worker threads during geometry rebuilds
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

#ifndef FS_PARALLELGEOMETRY_H
#define FS_PARALLELGEOMETRY_H

class LLDrawable;
class LLFace;
class LLSpatialGroup;

// Parallel geometry fill (FSPerfParallelGeometry).
//
// LLVolumeGeometryManager::rebuildGeom (through genDrawInfo) and rebuildMesh call
// LLFace::getGeometryVolume for every face, which writes the face's positions, normals,
// tangents, texture coordinates, colours, weights and indices into its range of the
// vertex buffer's CPU staging copy. Everything around that call (sorting, buffer
// allocation, draw info building, the GL upload on flushBuffers) stays on the main
// thread, in the original order. Only the fill itself is queued while a Scope is open and
// run when the Scope ends: on a small persistent worker pool plus the main thread when the
// batch is big enough, otherwise serially. Faces are disjoint ranges of their buffers, so
// fills do not overlap; the main thread does every side effect of getGeometryVolume that
// is not local to the face (tangent generation on possibly shared LLVolumes, vertex buffer
// map bookkeeping) before or after the parallel part, and keeps faces that need anything
// else (GLTF selection buffers, emboss bump texel offsets, animated children) on the old
// path. See fsparallelgeometry.cpp for the full list.
class FSParallelGeometry
{
public:
    // Fills are only queued while a Scope is open (and the setting is on). end() (or the
    // destructor) runs every queued fill, registers their mapped ranges and applies the
    // deferred drawable state changes, so after it the buffers are exactly as the serial
    // code leaves them.
    class Scope
    {
    public:
        Scope();
        ~Scope();
        void end();
    private:
        bool mOpen;
    };

    static bool active() { return sDepth != 0; }

    // Queue facep->getGeometryVolume(volume, te, relative xform, inverse transpose,
    // index_offset, force_rebuild, no_debug_assert) for when the Scope ends. Returns false
    // when the face must be filled right away by the caller (old path). With mesh_group
    // set, a failed fill marks that group for a geometry rebuild (rebuildMesh behaviour);
    // without, it logs the warning genDrawInfo logs.
    static bool deferFill(LLFace* facep, unsigned short index_offset, bool force_rebuild,
                          bool no_debug_assert, LLSpatialGroup* mesh_group);

    // Clear drawable state once the queued fills (which read it) have run.
    static void clearStateAfterFill(LLDrawable* drawablep, unsigned int state);

private:
    static void flush();
    static int sDepth;
};

#endif // FS_PARALLELGEOMETRY_H
