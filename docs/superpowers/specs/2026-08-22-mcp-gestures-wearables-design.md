# Design: `gesture.*` and `wearable.*` MCP tools

Date: 2026-08-22
Status: approved, ready for implementation plan

## Problem

The idmcp surface can wear, detach and organise what already exists, author the two
text asset types (`notecard.write`, `script.write`), and upload raw assets
(`upload.image`, `upload.sound`, `upload.animation`, `upload.material`). It cannot
author the two content types that turn those raw assets into something the avatar can
actually wear or perform.

The gap is concrete. An agent can upload a tattoo texture and then has no way to make a
tattoo out of it. It can upload an animation and no way to bind that animation to a
trigger word. Everything between "asset in inventory" and "worn/performable item" is
missing, and that is exactly the step a human does in the wearable editor and the
gesture editor.

This spec adds `gesture.*` (create/read/write/activate/deactivate/list/play) and
`wearable.*` (create/read/write, all wearable types including body parts).

## Constraints discovered while reading the viewer

These shaped the design more than the feature request did.

1. **Gestures have a modern capability; wearables have none.**
   `llviewerregion.cpp:3588-3597` requests `UpdateGestureAgentInventory`,
   `UpdateNotecardAgentInventory`, `UpdateScriptAgent`, `UpdateSettingsAgentInventory`
   and `UpdateMaterialAgentInventory`. There is no `UpdateWearable*` of any kind.
   Wearables still save through the legacy path: a fresh transaction id,
   `gAssetStorage->storeAssetData`, then `update_inventory_item` with a template item
   carrying that transaction id (`LLAgentWearables::saveWearable`).

2. **Every wearable *write* path is keyed on the `(type, index)` of a currently worn
   wearable — there is no item-id-addressed save.**
   - `LLAgentWearables::saveWearable(type, index, send_update, new_name)`
     — `llagentwearables.h:208`
   - texture assignment is `gAgentAvatarp->setLocalTexture(te, image, false, wearable_index)`,
     where `wearable_index` comes from `LLWearableData::getWearableIndex`
     (`llwearabledata.h:63`) and returns false for anything not worn
     — `llpaneleditwearable.cpp`, `onTexturePickerCommit`
   - both are followed by `gAgentAvatarp->wearableUpdated(type, false)`

   This is why the viewer makes you wear a thing before it will let you edit it. It is
   not a UI choice; it is the only shape the underlying API has.

3. **Reads are not constrained that way.** `LLWearableList::getAsset(assetID, name,
   avatarp, asset_type, callback, userdata)` (`llwearablelist.h:51`) fetches and parses
   any wearable asset into an `LLViewerWearable`, worn or not. Note the C-style
   `void(*)(LLViewerWearable*, void*)` callback — no `std::function`, so the call must
   carry a heap-allocated context struct that owns the `IDMCPCallPtr`, the same shape as
   the existing `IDMCPItemFetch` observers in `idmcptools_inventory.cpp`.

4. **`@edit` is currently in-world-object scope only.** Both `RlvActions::canEdit`
   overloads take either an `ERlvCheckType` or an `LLViewerObject*`
   (`rlvactions.cpp:668`, `:698`), and the toggle handler (`rlvhandler.cpp:2099`) drives
   the build floater and selection. Gating wearable and gesture asset writes on it
   extends `@edit` to cover inventory content. That is a deliberate fork extension and
   must be written down (see *Documentation* below), because a restraint author reading
   the upstream RLV spec will not expect it.

5. **`@sendgesture` is already enforced inside the gesture manager**
   (`llgesturemgr.cpp:557`, via `RlvActions::canPlayGestures`). Routing `gesture.play`
   through `LLGestureMgr::playGesture(item_id)` inherits that check for free, and chat
   steps stay under whatever filtering the manager already applies. No new gate, and —
   importantly — no second code path that could drift out of sync with the first.

6. **Gesture step model** (`llmultigesture.h`): `LLMultiGesture` holds `mTrigger`,
   `mReplaceText`, `mKey`, `mMask` and `mSteps`. Four step types:
   - `LLGestureStepAnimation { mAnimName, mAnimAssetID, mFlags }`, flag `ANIM_FLAG_STOP`
   - `LLGestureStepSound { mSoundName, mSoundAssetID, mFlags }`
   - `LLGestureStepChat { mChatText, mFlags }`
   - `LLGestureStepWait { mWaitSeconds, mFlags }`, flags `WAIT_FLAG_TIME`,
     `WAIT_FLAG_ALL_ANIM`, `WAIT_FLAG_KEY_RELEASE`

   Serialisation is `LLMultiGesture::serialize(LLDataPacker&)` into an
   `LLDataPackerAsciiBuffer` sized by `getMaxSerialSize()`.

7. **Wearable texture slots are derivable, not worth hardcoding.**
   `LLAvatarAppearanceDictionary::getTEWearableType(ETextureIndex)`
   (`llavatarappearancedefines.h:245`) plus `getTextures()` yields every texture entry,
   its dictionary name (which matches `avatar_lad.xml`), and `mIsLocalTexture`.
   Filtering the TE list to those whose wearable type equals the item's type reproduces
   `LLEditWearableDictionary::TextureCtrls` (`llpaneleditwearable.cpp:357`) without
   copying its 35-entry table — and stays correct if LL adds a slot.

8. **Tweakable params are self-describing.** `LLWearable::getVisualParams(list)`
   (`llwearable.h:118`) plus `LLVisualParam::isTweakable()` (`llvisualparam.h:162`),
   `getID/getName/getDisplayName/getMinWeight/getMaxWeight/getDefaultWeight`. Values via
   `LLWearable::getVisualParamWeight(id)` and `setVisualParamWeight(id, value, false)`.
   Driver params are included by `getVisualParams` and are the ones a human sees as
   sliders; driven params follow automatically.

## Decisions

**Writes are worn-only (approach A).** `wearable.write` requires the target item to be
currently worn and returns a clear error otherwise. Rejected alternatives:

- *Wear-transparent writes* — reimplement `saveWearable`'s internals outside
  `LLAgentWearables`: fetch the asset, mutate the in-memory `LLWearable`, `exportFile`,
  `storeAssetData`, `update_inventory_item`. Buys the agent one saved
  `appearance.wearItems` call and costs the fork its most fragile code, with
  `LLLocalTextureObject` construction outside the avatar as the worst part. Also
  silently diverges from upstream whenever LL touches that path.
- *Auto wear/edit/restore* — hide the wear behind the tool. Async, mutates the outfit
  invisibly, and breaks the moment RLV blocks either the wear or the restore.

**`@edit` is the only write gate.** No additional check on RLV-locked worn wearables.
Considered and declined: a locked worn restraint stays rewritable whenever `@edit`
happens to be off, so a restraint author who wants content protection must set `@edit`.
One lever, fully predictable — recorded here so the trade-off is not rediscovered later.

**Reads are gated on `@showinv`**, consistent with every other inventory-facing tool.

## Tool surface

### Gestures

| Tool | Arguments | Returns |
|---|---|---|
| `gesture.create` | `parent`, `name` | `{accepted, item_id}` |
| `gesture.read` | `item_id` | `{name, trigger, replace_with, key, mask, active, steps[]}` |
| `gesture.write` | `item_id`, any of `trigger`, `replace_with`, `key`, `mask`, `steps` | `{accepted, new_asset_id}` |
| `gesture.activate` | `item_id` | `{accepted, active}` |
| `gesture.deactivate` | `item_id` | `{accepted, active}` |
| `gesture.list` | — | `{gestures: [{item_id, name, trigger, playing}]}` |

`gesture.list` reports the *active* gestures — `LLGestureMgr::getActiveGestures()` — since
those are the ones whose triggers are live. Inactive gestures are ordinary inventory items
and are found with `inventory.search`. Renaming is `inventory.rename`'s job for gestures;
`gesture.write` does not take a `name`.
| `gesture.play` | `item_id` | `{accepted}` |

Step JSON, mirroring `llmultigesture.h` one-to-one:

```json
{"type": "animation", "animation": "<name or uuid>", "action": "start"|"stop"}
{"type": "sound",     "sound":     "<name or uuid>"}
{"type": "chat",      "text":      "hello"}
{"type": "wait",      "seconds":   1.5, "for_animations": true, "for_key_release": false}
```

`animation` and `sound` accept an inventory item name or a UUID. A name resolves against
loaded inventory filtered to `AT_ANIMATION` / `AT_SOUND`; an ambiguous name is an error
listing the candidate ids rather than a silent pick. On `gesture.read` both the stored
name and the asset id are returned, so a round-trip read/write is lossless.

`gesture.write` is a partial update: absent keys keep their current value, which requires
reading the existing gesture first. A gesture item created by `gesture.create` has no
asset yet, so the read step must treat a null asset id as "empty gesture" rather than an
error.

### Wearables

| Tool | Arguments | Returns |
|---|---|---|
| `wearable.create` | `parent`, `name`, `type`, optional `wear` (default true) | `{accepted, item_id, wear_requested}` |
| `wearable.read` | `item_id` | `{type, name, worn, textures{}, colors{}, params[]}` |
| `wearable.write` | `item_id`, any of `name`, `textures`, `colors`, `params` | `{accepted}` |

`type` is a `LLWearableType` name: `shape`, `skin`, `hair`, `eyes`, `shirt`, `pants`,
`shoes`, `socks`, `jacket`, `gloves`, `undershirt`, `underpants`, `skirt`, `alpha`,
`tattoo`, `physics`, `universal`.

`read` output shape:

```json
{
  "type": "tattoo",
  "worn": true,
  "textures": {"head_tattoo": "<uuid>", "upper_tattoo": "<uuid>", "lower_tattoo": "<uuid>"},
  "colors":   {"head_tattoo": [1.0, 1.0, 1.0]},
  "params":   [{"id": 1062, "name": "tattoo_red", "label": "Red", "value": 1.0,
                "min": 0.0, "max": 1.0, "default": 1.0}]
}
```

Texture and colour keys are the TE dictionary names from constraint 7, filtered to the
item's wearable type. `params` is `getVisualParams()` filtered to `isTweakable()`.
`write` accepts any subset of the three maps and ignores nothing silently — an unknown
texture key, colour key or param name is an error naming the valid keys for that type.

Param references accept either the numeric `id` or the `name`. Values outside
`[min, max]` are rejected rather than clamped, so a mistaken slider write fails loudly.

## Architecture

Two new files, following the existing per-area pattern:

- `indra/newview/idmcptools_gesture.cpp` — `idmcp_register_gesture_tools(IDMCPToolRegistry&)`
- `indra/newview/idmcptools_wearable.cpp` — `idmcp_register_wearable_tools(IDMCPToolRegistry&)`

with declarations in `idmcptools.h` and calls added to `IDMCPServer::initSingleton`, plus
both files added to `indra/newview/CMakeLists.txt`.

This costs one full PCH rebuild (adding any `.cpp` does). The alternative — folding
~1150 lines into `idmcptools_appearance.cpp` — would take that file to roughly 1500 lines
covering three unrelated concerns, and was declined.

### Gesture read

`gesture.read` resolves the item, then:

- asset id null → return an empty gesture (`steps: []`, empty trigger)
- gesture already active → read the live `LLMultiGesture*` from
  `LLGestureMgr::getActiveGestures()`, no fetch needed. The map holds a null pointer
  while an activation is still loading its asset, so a null entry falls through to the
  fetch below rather than being treated as an empty gesture.
- otherwise → `gAssetStorage->getAssetData(asset_id, AT_GESTURE, cb, ctx, high_priority)`,
  read the file via `LLFileSystem`, `LLDataPackerAsciiBuffer` → `deserialize`

The C-style callback carries a heap `struct { IDMCPCallPtr call; LLUUID item_id; }`,
deleted in the callback on every path including failure.

### Gesture write

Build the `LLMultiGesture` (merged over the current one), serialise into an
`LLDataPackerAsciiBuffer` sized by `getMaxSerialSize()`, then reuse the exact upload
shape `idmcp_write_notecard` already uses: `LLBufferedAssetUploadInfo(item_id,
AT_GESTURE, buffer, on_done, on_fail)` → `LLViewerAssetUpload::EnqueueInventoryUpload(url,
info)` against `UpdateGestureAgentInventory`, with `idmcp_set_item_asset` in the
completion. If the gesture was active, re-activate with the new asset via
`activateGestureWithAsset` so the live copy matches what was saved.

### Wearable read

- worn → `gAgentWearables.getWearableFromItemID(item_id)` gives the live
  `LLViewerWearable*` directly, including unsaved edits
- not worn → `LLWearableList::getAsset(asset_id, name, gAgentAvatarp, asset_type, cb, ctx)`

Both then run the same serialiser over `LLWearable`, so the output is identical either
way. Same heap-context pattern as the gesture read.

### Wearable write

1. `gAgentWearables.getWearableFromItemID(item_id)` — null means not worn, so return
   `IDMCP_ERR_INVALID_PARAMS` with "wearable is not currently worn; wear it first with
   appearance.wearItems". Explicit and actionable; it is a precondition on the argument,
   not a permission failure.
2. `LLWearableData::getWearableIndex(wearable, index)` for the `(type, index)` pair.
3. Apply, in order: params (`setVisualParamWeight(id, value, false)`), colours
   (`setClothesColor(te, color, false)`), textures
   (`gAgentAvatarp->setLocalTexture(te, LLViewerTextureManager::getFetchedTexture(uuid), false, index)`,
   substituting `IMG_DEFAULT_AVATAR` for `IMG_DEFAULT` exactly as
   `onTexturePickerCommit` does).
4. `gAgentAvatarp->wearableUpdated(type, false)`.
5. `gAgentWearables.saveWearable(type, index, true, new_name_or_empty)`.

Validation happens entirely before step 3, so a request with one bad param does not leave
the wearable half-modified.

### Wearable create

`LLAgentWearables::createWearable(type, wear, parent_id, created_cb)`
(`llagentwearables.h:156`) with `wear` defaulting to true, since an unworn new wearable
cannot then be written. The callback returns the new item id. Next-owner permissions come
from `LLFloaterPerms::getNextOwnerPerms("Wearables")`; gestures use `"Gestures"` — both
keys exist (`llfloaterperms.cpp:122-127`), matching the fix already made for
`inventory.createItem`.

## RLV gating

| Tool | Gate |
|---|---|
| `gesture.read`, `gesture.list`, `wearable.read` | `@showinv` |
| `gesture.create`, `gesture.write` | `@edit` |
| `wearable.create`, `wearable.write` | `@edit` |
| `gesture.play` | none declared — `LLGestureMgr` enforces `@sendgesture` internally |
| `gesture.activate`, `gesture.deactivate` | none |

Gates are declared as `IDMCPTool::gate` functions using
`IDMCPRlvGate::checkBehaviour(RLV_BHVR_EDIT, "edit")`, so denial produces the standard
structured `-32011` with attributed source objects. Deferred tools (every write, and the
unworn read) re-run their gate at Commit phase before the side effect lands, per the
existing contract in `idmcprlvgate.h`.

`wearable.create` with `wear: true` performs a wear, which is separately subject to the
wearable-lock predicates. Rather than duplicate that logic, the create path routes the
wear through `LLAppearanceMgr::wearItemsOnAvatar` — the same RLV-gated path
`appearance.wearItems` uses — and reports `wear_requested` in the result rather than a
`worn` verdict: the COF round-trip has not completed at callback time, so `worn` would
read `false` even for a wear that is about to succeed. The caller confirms the actual
outcome with `appearance.getWorn`. (This field changed from the originally specified
`worn` during implementation for exactly that reason.)

## Error handling

Reuses the existing codes in `idmcpserver.h`:

- `IDMCP_ERR_INVALID_PARAMS` — bad type name, unknown texture/colour/param key,
  out-of-range param value, ambiguous animation/sound name, wearable not worn
- `IDMCP_ERR_NOT_FOUND` — no such item, or item is the wrong asset type
- `IDMCP_ERR_CAP_UNAVAIL` — `UpdateGestureAgentInventory` missing
- `IDMCP_ERR_PERMISSION` — upload rejected, or item not modifiable
- `IDMCP_ERR_RLV_RESTRICTED` — from the gate

Asset fetches that never complete are covered by the server's existing deadline sweep.
Every heap callback context is deleted on all paths.

## Testing

Firestorm is not built in this workspace, so verification is by inspection plus a live
pass by Five after a rebuild. The live checklist:

1. `gesture.create` → `gesture.write` with one animation step and one chat step →
   `gesture.activate` → the trigger word works in-world, and the gesture editor shows the
   same steps the tool wrote.
2. `gesture.read` on an existing multi-step gesture round-trips: read, write back
   unchanged, read again, compare.
3. `upload.image` a texture → `wearable.create` type `tattoo` → `wearable.write` setting
   all three tattoo TEs → the tattoo renders.
4. `wearable.read` on the worn shape returns ~190 tweakable params; writing `height`
   visibly changes the avatar and survives a relog.
5. `wearable.write` on an unworn item returns the "wear it first" error, not a crash.
6. With `@edit=n` set by a relay: all four write tools return `-32011` with the
   restricting object named; reads still work.
7. With `@showinv=n`: reads return `-32011`; `gesture.play` still works.

## Documentation

`doc/rlva-custom-commands.md` documents this fork's RLVa deviations. The `@edit`
extension to inventory content (constraint 4) is a behaviour change to a standard
command, not a new command, so it needs an explicit note there — a restraint author
reading the upstream spec would otherwise not expect `@edit` to block a wearable edit.

## Out of scope

- Editing wearables that are not worn (see *Decisions*)
- Gesture steps in task inventory (`UpdateGestureTaskInventory`) — agent inventory only
- Creating outfits or links; `appearance.*` already covers that ground
- Any new RLV restriction word
