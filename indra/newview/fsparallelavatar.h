/**
 * @file fsparallelavatar.h
 * @brief Evaluates the avatar poses of a frame on worker threads
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

#ifndef FS_PARALLELAVATAR_H
#define FS_PARALLELAVATAR_H

class LLVOAvatar;

// FSPerfParallelAvatarUpdate
//
// LLViewerObjectList::update() brackets the object idle loop with beginIdle() and endIdle().
// In between, LLVOAvatar::updateCharacter() hands each visible avatar that is due a normal
// animation update to queuePose() instead of updating its motions itself: the main thread
// steps the motion clock and loads motions (LLCharacter::prepareParallelMotionUpdate()) and
// the rest of idleUpdate() goes on as before. endIdle() then evaluates the queued poses
// (motion updates, pose blend, joint world matrices) on worker threads, the main thread
// helping, and afterwards runs the main thread part of every avatar in the original order:
// deferred motion deactivations and visual param updates, head offset and footstep sounds.
// With FSPerfParallelAvatarUpdate off nothing is queued and avatars update as before.
class FSParallelAvatar
{
public:
    static void beginIdle();
    static void endIdle();

    static bool isCollecting() { return sCollecting; }

    // Main thread, from LLVOAvatar::updateCharacter(). True if the avatar's motion update was
    // taken over; updateCharacter() must then return without updating motions or joints.
    static bool queuePose(LLVOAvatar* avatar, bool was_sit_ground_constrained);

private:
    static void evaluatePose(LLVOAvatar* avatar);
    static void finishPose(LLVOAvatar* avatar);
    static unsigned long long poseFingerprint(LLVOAvatar* avatar);
    static int verifyWorldMatrices(LLVOAvatar* avatar);
    static void runTask(void* context, unsigned int index);

    static bool sCollecting;
};

#endif // FS_PARALLELAVATAR_H
