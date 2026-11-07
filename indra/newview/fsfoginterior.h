/**
 * @file fsfoginterior.h
 * @brief Keeps the height fog out of rooms around the camera (FSRenderHeightFogInteriors)
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

#ifndef FS_FOGINTERIOR_H
#define FS_FOGINTERIOR_H

class LLGLSLShader;

// The height fog (fsHeightFogF.glsl) knows nothing about walls and roofs, so it fills rooms and
// glows with sky and sun light there. With FSRenderHeightFogInteriors on, a few times per second a
// handful of short rays are cast from the render camera against nearby static, opaque prim and
// mesh geometry (up, around and toward the sun or moon). From them come, smoothed over time:
//  - an indoor factor and a room radius: the fog only starts that far along each ray;
//  - the stretch of air around the camera that is sheltered, and how much of the sky and the sun
//    it sees: the fog in that stretch is lit with that much ambient and sun light.
// The shader reads them from fs_height_fog_interior. They apply to the main view only: reflection
// probes, mirrors, shadow and impostor passes get zeros, the plain fog.
namespace FSFogInterior
{
    // Uploads fs_height_fog_interior to a program that has the height fog. While rendering the
    // main view this also runs the (rate limited) ray casts and the smoothing, once per frame.
    // No-op unless FSRenderHeightFog is on (the uniform does not exist otherwise).
    void uploadUniforms(LLGLSLShader& shader);
}

#endif // FS_FOGINTERIOR_H
