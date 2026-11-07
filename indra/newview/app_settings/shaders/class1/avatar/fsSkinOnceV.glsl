/**
 * @file fsSkinOnceV.glsl
 * @brief Captures each vertex's blended skin matrix by transform feedback (FSPerfSkinOncePerFrame)
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

// Linked with the shared avatar/objectSkinV.glsl object. fs_skin_once is never set in this
// program, so getSkinBlend() takes the palette path. Drawn as points with rasterizer discard;
// the three rows land interleaved in the skin-once pool, which rigged draws then read back.

mat3x4 getSkinBlend();

out vec4 fs_skin_row0;
out vec4 fs_skin_row1;
out vec4 fs_skin_row2;

void main()
{
    mat3x4 skin = getSkinBlend();
    fs_skin_row0 = skin[0];
    fs_skin_row1 = skin[1];
    fs_skin_row2 = skin[2];
    gl_Position = vec4(0.0, 0.0, 0.0, 1.0);
}
