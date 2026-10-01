# Design: MCP object build tools

Date: 2026-10-01
Status: approved in chat, awaiting spec review

## Problem

An agent can upload a mesh, texture or material, but it can't do anything with the
result. Turning an upload into a finished item still needs a person at the build
floater: rezzing it, placing it, texturing faces, filling its contents, setting
next-owner permissions and taking it back into inventory.

The goal is the whole loop: upload → rez or attach → edit → texture → add contents →
set permissions → link → take. It has to work on rezzed objects and on worn
attachments.

## Constraints discovered while reading the viewer

1. **Editing is built around the selection.** `LLSelectMgr` packs every edit message
   (`MultipleObjectUpdate`, `ObjectName`, `ObjectPermissions`, `ObjectLink`,
   `DeRezObject`) from its selected nodes (`sendListToRegions`, llselectmgr.cpp:~5945).
   Driving it would take over the user's own selection and edit floater.
2. **Faces, materials and contents don't need a selection.** The drag-and-drop paths
   already work per object:
   - `dropTextureOneFace` (lltooldraganddrop.cpp:1702) → `setTEImage` + `sendTEUpdate`
   - `dropMaterialOneFace` (:1288) → `LLGLTFMaterialList::queueApply` / `setRenderMaterialID`
   - `dropInventory` (:2064) → `LLViewerObject::updateInventory` (`UpdateTaskInventory`)
   - `dropScript` (:1828) → `LLViewerObject::saveScript` (`RezScript`)
   Their permission rules live in `handleDropMaterialProtections` (:950-1050) and
   `willObjectAcceptInventory` (:2125-2230).
3. **Rezzing doesn't report the new object.** `dropObject` (:1887) sends `RezObject`,
   and the region replies with an ordinary `ObjectUpdate`. The one-shot
   `mNewObjectSignal` only fires for "rez selected" full-perm objects
   (llviewerobjectlist.cpp:365-396). Finding the new object means watching for it.
4. **There is no "rez at position" helper.** `dropObject` builds its ray from the last
   pick (`mLastHitPos`/`mLastCameraPos`). A new sender can pack `RezObject` with
   `BypassRaycast=true`, `RayStart` just above the target and `RayEnd` at it.
5. **Linkset vs. one prim.** `MultipleObjectUpdate` applies to the whole linkset when
   the root's entry carries `UPD_LINKED_SETS`, and to one prim when it doesn't. A child's
   position is relative to its root (llselectmgr.cpp:4893-4955).
6. **Properties need a request.** Names, descriptions and permissions arrive in
   `ObjectProperties` after an `ObjectSelect`. `objects.getNearby` already sends a raw
   `ObjectSelect` per region and waits for the reply without touching `LLSelectMgr`
   (idmcptools_avatars.cpp:407-520). It must be followed by `ObjectDeselect`.
7. **Contents arrive asynchronously.** `requestInventory()` triggers the task-inventory
   download, and listeners registered through `LLVOInventoryListener` are told when it
   lands (`doInventoryCallback`, llviewerobject.cpp:3713).
8. **Attaching to a chosen point** goes through `rez_attachment(item, attachment, replace)`
   (llinventorybridge.cpp:8292). That function already applies RLVa's attachment-lock
   checks. `appearance.wearItems` only uses default points.

## Approach

Selection-free throughout. Faces, materials and contents call the per-object functions
that drag-and-drop uses. Transforms, names, descriptions, permissions, link, unlink and
take send the same messages the build floater would, packed directly from the
`LLViewerObject` instead of from selection nodes. The user's selection and edit floater
are never touched, so a person can keep building while an agent works.

## Targeting

Every object tool takes:

- `object_id` (required): the object's UUID, from `objects.getNearby`, `avatars.getWorn`,
  or a rez/attach result. This can be any prim in a linkset; the tool resolves the root.
- `link` (optional): a link number as reported by `object.get` (1 = root) to act on one
  prim. Without it, the tool acts on the whole object.

Tools resolve the object through `gObjectList.findObject` and require a volume
(`LL_PCODE_VOLUME`). A worn attachment is found the same way, because attachments are in
the object list.

## Tools

| Tool | Arguments | Returns |
|---|---|---|
| `object.rez` | `item_id`*, `position` [x,y,z] region metres (default 2 m in front of the avatar, at ground or avatar height), `rotation` [x,y,z] degrees | `object_id`, `name`, `position`, and `item_consumed: true` for a no-copy item |
| `object.attach` | `item_id`*, `point`* (attachment point name, e.g. "Chest", "Spine", "HUD Center"; case-insensitive), `replace` (default false) | `object_id`, `point` |
| `object.get` | `object_id`*, `faces` (default false) | Per link: `link`, `object_id`, `name`, `description`, `position` (region for the root, root-relative for children), `rotation` (degrees), `scale`, `face_count`, `permissions` `{owner_id, base, owner, group, everyone, next_owner}` as `copy`/`modify`/`transfer` booleans per mask. With `faces: true`, each face's `texture`, `material`, `color`, `alpha`, `repeats`, `offset`, `rotation`. Plus `attachment_point` if worn. |
| `object.edit` | `object_id`*, `link`, `position`, `rotation`, `scale`, `name`, `description` | The new values, once the region has echoed them |
| `object.setFaces` | `object_id`*, `link`, `faces`* ("all" or [ints]), `texture`, `material` (an id, or "none" to clear), `color` [r,g,b] 0-1, `alpha` 0-1, `repeats` [u,v], `offset` [u,v], `rotation` degrees | A result per face |
| `object.contents` | `object_id`*, `link` | `items: [{item_id, name, type, permissions}]` |
| `object.addContents` | `object_id`*, `link`, `item_ids`*, `running` (scripts; default true) | A result per item |
| `object.setPermissions` | `object_id`*, `next_owner`* `{copy, modify, transfer}`, `contents` (default false) | A result for the object, and per item with `contents` |
| `object.link` | `object_ids`* (2 or more; the first becomes the root) | The new root `object_id` and link count |
| `object.unlink` | `object_id`*, `link` (omit to unlink the whole set) | The resulting `object_id`s |
| `object.take` | `object_id`*, `copy` (default false), `folder` (UUID or well-known name; default Objects) | `item_id` of the new inventory item |

Notes on the tool surface:

- **`texture` and `material` accept either an inventory item id or a raw asset UUID.** An
  id that resolves to an inventory item goes through the drag-and-drop path, so its
  permission rules apply, including copying a no-copy texture's use into the object's
  contents. Any other UUID is applied directly as an asset, the way pasting a UUID into
  the texture picker does.
- **Rotations are Euler degrees** `[x, y, z]`, which agents handle more reliably than
  quaternions. They convert with `LLQuaternion::setEulerAngles` in radians.
- **`scale` applies to one prim**: the root, or the prim named by `link`. Scaling a
  linkset as a unit is out of scope.
- **Positions are region coordinates** for the root and root-relative for a child link,
  matching what the region expects in `MultipleObjectUpdate`.

## Mechanisms

### Shared helpers (`idmcptools_object.cpp`)

- **Resolve** `object_id` + `link` to a root `LLViewerObject*` and a target prim. Link
  numbers are the ones `object.get` reports: 1 for the root, then the children in
  `getChildren()` order. They may not match LSL's `llGetLinkNumber` order, so agents
  must take them from `object.get`, and every tool uses the same numbering. Errors:
  `NOT_FOUND`, or `INVALID_PARAMS` for a bad link.
- **Properties request:** send `ObjectSelect` for the prims' local ids, wait for their
  `ObjectProperties`, then send `ObjectDeselect`. Reuse or extract the pattern in
  `idmcptools_avatars.cpp`. A request times out after 10 s.
- **Echo wait:** after an edit, poll the target on idle until its position, rotation,
  scale or name match what was sent (within a small tolerance), or time out after 10 s.
  A timeout is `TIMEOUT` with what was sent and what the object currently reports.
- **Modify check:** before sending any edit, require `permModify()` on the target. For
  a freshly seen object whose permissions haven't arrived, run the properties request
  first.

### `object.rez` (`idmcptools_rez.cpp`)

1. Resolve the item. It must be an object owned by the agent and not in the Trash or the
   Library. Library items aren't supported.
2. Record the ids of the agent's root objects within 10 m of the target.
3. Send `RezObject`, packed like `dropObject` but with: `BypassRaycast=true`,
   `RayStart` = target + (0, 0, 0.5), `RayEnd` = target, `RayTargetID` null,
   `RayEndIsIntersection=false`, `RezSelected=false`, `RemoveItem` true for no-copy
   items, the rez group from `FSCommon::getGroupForRezzing()`, and the item's
   `packMessage`. Set `FSCommon::sObjectAddMsg = 0` as `dropObject` does. No-copy
   items are removed from local inventory the same way `dropObject` does it.
4. Watch on idle for a root volume object owned by the agent within 10 m of the target
   that wasn't in the step 2 list. Confirm it by name with a properties request. Apply
   `rotation` with an `object.edit`-style update once it appears.
5. Time out after 30 s with `TIMEOUT`.

### `object.attach` (`idmcptools_rez.cpp`)

Resolve `point` against `gAgentAvatarp->mAttachmentPoints` by name, then call
`rez_attachment(item, attachment, replace)`. Wait on idle until an attachment with the
item's id appears on that point (`getAttachedObject`), for up to 30 s.

### `object.edit` (`idmcptools_object.cpp`)

- **Transforms:** pack `MultipleObjectUpdate` directly, in the order position, rotation,
  scale (`packMultipleUpdate`'s order). Without `link`, send one entry for the root with
  `UPD_LINKED_SETS` set. With `link`, send one entry for that prim without it. Update the
  local object first (`setPositionEdit`, `setRotation`, `setScale`) as the floater does.
- **Name / description:** send `ObjectName` / `ObjectDescription` for the target prim
  (the root without `link`).
- Wait for the echo, then return the values the object now reports.

### `object.setFaces` (`idmcptools_faces.cpp`)

For each requested face, on the target prim (each prim of the linkset when `link` is
omitted and `faces` is "all"):

- **`texture`:** an inventory item goes through `dropTextureOneFace`'s logic
  (`handleDropMaterialProtections`, then `setTEImage`). Clear the face's PBR material
  first, as a texture drop with `remove_pbr` does. A raw UUID calls `setTEImage` directly.
- **`material`:** an inventory item goes through `dropMaterialOneFace`'s logic
  (`queueApply`). A raw UUID calls `setRenderMaterialID`. `"none"` clears the material.
- **`color`, `alpha`, `repeats`, `offset`, `rotation`:** `setTEColor`, `setTEScale`,
  `setTEOffset`, `setTERotation`.
- Finish with one `sendTEUpdate()` per prim. Material changes are flushed with
  `LLGLTFMaterialList::flushUpdates`.

If a drag-and-drop helper the job needs is private, add an `<ID>`-tagged friend
declaration rather than duplicating its logic.

### `object.contents` / `object.addContents` (`idmcptools_contents.cpp`)

- **List:** register an `LLVOInventoryListener` on the target prim, call
  `requestInventory()`, and answer from the `inventoryChanged` callback. Skip the
  root folder entry. Time out after 15 s.
- **Add:** for each item, apply `willObjectAcceptInventory`'s rules, then call
  `dropScript` (scripts, with `running`) or `dropInventory` (everything else). After
  sending, re-list the contents and report each item as added or not found. Items are
  added one at a time, since the region processes them in order.

### `object.setPermissions` (`idmcptools_object.cpp`)

- **Object:** send `ObjectPermissions` for the root with field `PERM_NEXT_OWNER`, once to
  set the requested bits and once to clear the others, like the floater's next-owner
  checkboxes.
- **Contents:** list the contents, then for each item copy its `LLPermissions`, set the
  next-owner mask, and send it back with `updateInventory`, as the contents tab does.

### `object.link` / `object.unlink` (`idmcptools_object.cpp`)

- **Link:** send `ObjectLink` with the root first. Require all objects to be in the
  same region, owned by the agent, and modifiable. Wait until each object's root is the
  first object.
- **Unlink:** send `ObjectDelink` for the target prim, or for every prim when `link` is
  omitted. Wait until each target reports itself as a root.

### `object.take` (`idmcptools_rez.cpp`)

Send `DeRezObject` for the root with destination `DRD_TAKE_INTO_AGENT_INVENTORY`
(take) or `DRD_ACQUIRE_TO_AGENT_INVENTORY` (take copy) and the destination folder. Watch
the folder for a new object item with the object's name. Time out after 30 s. A take
of an object the agent doesn't own is refused before sending.

## RLV

Gates run at request time and again before the deferred step, as in the existing tools.

| Tool | Blocked by |
|---|---|
| `object.rez` | `@rez`, `@interact` |
| `object.attach` | Attachment-point locks and `@addattach` (via `rez_attachment`'s own checks, plus a request-time check) |
| `object.get`, `object.contents` | Nothing (read-only) |
| `object.edit`, `object.setFaces`, `object.setPermissions`, `object.link`, `object.unlink` | `RlvActions::canEdit(obj)` (`@edit`, `@editobj`, `@editattach`, `@editworld`) and `@interact` |
| `object.addContents` | As edit, plus a locked attachment root, or an object the avatar is sitting on while `@unsit` or `@sittp` is active |
| `object.take` | As edit, plus `@rez` |

## Errors

Existing codes only:

- `NOT_FOUND`: object, item, link or attachment point not found.
- `INVALID_PARAMS`: bad arguments, wrong item type, a no-copy item that the drop rules refuse.
- `PERMISSION`: no modify permission, not the owner, not transferable.
- `RLV_RESTRICTED`: as above.
- `TIMEOUT`: the region didn't confirm in time. The error data says what was sent.

Batch tools (`setFaces` across faces, `addContents`, `setPermissions` with `contents`)
report per item and fail the whole call only when nothing could be attempted.

## Files

| File | Responsibility |
|---|---|
| `indra/newview/idmcptools_object.cpp` (create) | Shared resolve, properties and echo helpers, plus `object.get`, `object.edit`, `object.setPermissions`, `object.link`, `object.unlink` |
| `indra/newview/idmcptools_rez.cpp` (create) | `object.rez`, `object.attach`, `object.take` |
| `indra/newview/idmcptools_faces.cpp` (create) | `object.setFaces` |
| `indra/newview/idmcptools_contents.cpp` (create) | `object.contents`, `object.addContents` |
| `indra/newview/idmcptools.h`, `idmcpserver.cpp`, `CMakeLists.txt` (modify) | Registration and build entries |
| `MCP_TOOLS.md` (modify) | First sync the viewer copy from the plugin's (it lacks the gesture and wearable sections), then add an "Objects" section |
| Plugin repo `firestorm-avatar` (separate PR) | The same docs, plus skill guidance for the build loop |

The shared helpers are declared in a small header, `idmcptools_object.h`, so the other
three files can use them.

## Non-goals

- Creating new prims from scratch (`ObjectAdd`), and editing prim shape parameters.
- Scaling a linkset as a unit.
- Deleting or returning objects.
- For-sale settings, group deeding, and object-level "everyone"/"group" permissions.
- Editing objects the agent doesn't own, beyond what the region allows for modify-rights
  holders.

## Testing

No unit-test harness exists for `idmcp*` code, and the viewer can't be built here. The
user builds; the agent runs this matrix over MCP:

1. Upload a texture (`upload.image`) and a cube (`upload.mesh`).
2. `object.rez` the cube at a set position; the result's `object_id` appears in
   `objects.getNearby`.
3. `object.edit`: move, rotate, rename, set a description; `object.get` shows the new values.
4. `object.setFaces`: the texture on face 2, then a PBR material on all faces; `object.get
   faces:true` shows them. Repeat with the texture's raw asset UUID.
5. `object.addContents`: a notecard and a script; `object.contents` lists both, and the
   script runs (it says something on chat).
6. `object.setPermissions` next owner copy-only with `contents: true`; `object.get` and
   `object.contents` show it.
7. Rez a second cube, `object.link` the two, `object.get` shows two links; `object.unlink`.
8. `object.take copy:true`, then `object.take`; both items appear in Objects, and the
   object is gone after the second.
9. `object.attach` the cube to Chest, then `object.setFaces` and `object.edit` on the worn
   attachment.
10. Error cases: an RLV `@edit` restriction blocks `object.edit`; a no-modify object
    returns `PERMISSION`; a no-copy texture follows the drop rules; a bad `link` returns
    `INVALID_PARAMS`.
11. While a tool runs, select something else in the build floater; the user's
    selection is unchanged afterwards.
