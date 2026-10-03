# Firestorm Embedded MCP Server — Tool Reference

Custom fork (`ID` prefix). An in-process MCP server that lets an AI agent drive the avatar, with RLVa restrictions hard-enforced.

## Connecting

- **Endpoint:** `POST http://127.0.0.1:33777/mcp` (MCP Streamable-HTTP; JSON-RPC 2.0)
- **Enable:** Debug Setting `IDMCPServerEnabled = 1` (off by default). Port: `IDMCPServerPort` (default 33777).
- **Security:** loopback-only, no auth — any local process can drive the avatar while enabled.
- Deferred results (fetches, searches, uploads) stream back over SSE; `*` marks required params.

## RLV enforcement

Every mutating or identity-revealing tool passes a single RLV gate. A blocked call returns JSON-RPC error `-32011` with `{restriction, sources:[{object_id,root_id,attach_pt,name}], checkedAt}`. Use `rlv.getRestrictions` to see what's active and `rlv.canDo` to check a call before making it.

**98 tools** across 23 areas.

## Health

| Tool | Params | Description |
|---|---|---|
| `ping` | — | Liveness check. Returns {"pong": true}. |

## Viewer

| Tool | Params | Description |
|---|---|---|
| `viewer.notify` | `text*`: string<br>`title`: string<br>`tip`: boolean | Post a toast to the viewer to get the user's attention. `text` is the body; optional `title` shows on the first line. Default is a persistent notify toast (stays until dismissed, kept in the notification well); `tip:true` gives a lighter toast that fades on its own. |

## Chat (in-world, via LSL bridge)

Talk to in-world scripted objects, HUDs, vendors, and **RLV relays** on any channel — including negative/protocol channels the viewer itself can't use — by routing through the Firestorm LSL bridge. Requires `UseLSLBridge` **and** the opt-in Debug Setting `IDMCPBridgeChatEnabled = 1` (off by default). Messages are sent *from the bridge object*, not shown as your avatar's public chat.

**Workflow: `chat.listen` → `chat.send` → `chat.replies` → `chat.stopListen`.** You must open a listen *before* sending if you want the reply. Bridge listen slots are scarce — **always `chat.stopListen` when done** (listens also auto-expire, and there's a hard cap of 4 concurrent). See the `firestorm-chat` and `firestorm-rlv-relay` skills.

| Tool | Params | Description |
|---|---|---|
| `chat.listen` | `channel*`: integer<br>`seconds`: integer | Open a bridge listen on `channel` (any integer, incl. negative) so replies can be captured. `seconds` default 30, max 120; auto-closes at the deadline. Returns `{listening, channel, expires_in}`. Call this **before** `chat.send`. |
| `chat.send` | `channel*`: integer<br>`message*`: string<br>`type`: whisper \| say \| shout<br>`as_avatar`: boolean | Say `message` on `channel`. Default routes via the bridge (any channel incl. negative; sent from the bridge object). `as_avatar:true` speaks as **your avatar** instead (channel 0 / positive only — for normal public chat). `type` sets range (~10/20/100 m). RLV `@sendchat`/`@sendchannel` enforced (`-32011`). |
| `chat.replies` | `channel*`: integer<br>`wait_seconds`: integer | Drain messages heard on a listened channel: `{listening, channel, replies:[{from_id, from_name, text, time}]}`. If nothing buffered and `wait_seconds` (max 30) given, waits up to that for the first reply. Errors if not listening on the channel. |
| `chat.stopListen` | `channel`: integer | Close a listen and free its bridge slot. Give `channel` to close one, or omit to close **all**. Always call when done. Returns `{stopped}`. |

## RLV

| Tool | Params | Description |
|---|---|---|
| `rlv.canDo` | `tool*`: string<br>`arguments`: object | Check whether a tool call would be allowed under current RLV restrictions, WITHOUT executing it. {"tool"} (tool name) and optional {"arguments"} (that tool's arguments). Returns {allowed} and, if blocked, the restriction and its source object(s). |
| `rlv.getRestrictions` | `filter`: string | List all active RLVa restrictions with their source objects. Optional {"filter"} substring-matches the behaviour name. Consult this to know which actions are currently blocked before attempting them. |
| `rlv.getStatus` | — | Whether RLVa restriction enforcement is currently enabled. |

## Inventory

| Tool | Params | Description |
|---|---|---|
| `inventory.copy` | `id*`: string<br>`dest*`: string<br>`name`: string | Copy an item or folder into another folder, as Copy and Paste would. `id` can be in the Library; `dest` is a folder UUID or "root". `name` renames an item's copy. No-copy items are refused, and left behind when copying a folder. Returns `{item_id}` or `{folder_id, skipped:[{item_id, name}]}`; a folder's contents finish copying shortly after the answer. A folder that hasn't finished loading returns error -32005; retry after a few seconds. Copying into an RLV-locked folder is refused. |
| `inventory.createFolder` | `parent*`: string<br>`name*`: string | Create a new folder. {"parent"} (folder UUID or "root") and {"name"}. Returns the new folder id. |
| `inventory.createItem` | `parent*`: string<br>`name*`: string<br>`type*`: notecard \| script | Create a new empty notecard or script. Returns the new item id; add content with notecard.write / script.write. |
| `inventory.delete` | `id*`: string<br>`confirm*`: boolean | Move an item or folder to Trash. Requires {"id"} and {"confirm": true}. |
| `inventory.getFolder` | `folder*`: string | List the direct contents of an inventory folder. {"folder"} is a folder UUID or a well-known name: "root", "cof", "outfits", "trash". Fetches the folder if needed. Blocked by RLV @showinv. |
| `inventory.getItem` | `item_id*`: string | Get details for one inventory item by {"item_id"} (UUID). Fetches it if not yet loaded. Blocked by RLV @showinv. |
| `inventory.giveFolder` | `folder_id*`: string<br>`to_agent*`: string | Give a folder (and its contents) to another avatar. {"folder_id"} and {"to_agent"} (UUID). Blocked by RLV @share. |
| `inventory.giveItem` | `item_id*`: string<br>`to_agent*`: string | Give a copy of an inventory item to another avatar. {"item_id"} and {"to_agent"} (UUID). No-copy items are refused. Blocked by RLV @share. |
| `inventory.move` | `id*`: string<br>`dest*`: string | Move an item or folder into another folder. {"id"} and {"dest"} (folder UUID). |
| `inventory.refresh` | `item_id*`: string | Force a fresh server fetch of an inventory item's metadata ({"item_id"}) — e.g. to pick up a rename/move made elsewhere. Returns {refreshed, has_asset}. Scripts/notecards carry no local asset id (resolved server-side on read), so has_asset is false for them even though they read fine. |
| `inventory.rename` | `id*`: string<br>`name*`: string | Rename an inventory item or folder. {"id"} and {"name"}. |
| `inventory.search` | `name*`: string<br>`limit`: integer | Search loaded inventory by name substring. {"name"} required; optional {"limit"} (default 100, max 500). Blocked by RLV @showinv. |

## Notecards

| Tool | Params | Description |
|---|---|---|
| `notecard.read` | `item_id*`: string | Read a notecard's text by {"item_id"} (UUID). Returns {text, has_embedded_items}. Blocked by RLV @viewnote. |
| `notecard.write` | `item_id*`: string<br>`text*`: string | Replace a notecard's text. {"item_id"} (UUID) and {"text"}. The notecard must be modifiable. Blocked by RLV @viewnote. |

## Scripts (LSL)

| Tool | Params | Description |
|---|---|---|
| `script.read` | `item_id*`: string | Read an LSL script's source by {"item_id"} (UUID). Returns {text}. Blocked by RLV @viewscript. |
| `script.write` | `item_id*`: string<br>`text*`: string<br>`target`: mono \| lsl2 \| luau \| lsl-luau | Replace a script's source and recompile. `mono` (default) and `lsl2` take **LSL** source; `lsl-luau` takes **LSL** source and compiles it to Luau; `luau` takes **Luau** source. `lsl-luau`/`luau` need region Luau support. Returns {compiled, errors?}. Blocked by RLV @viewscript. |

## Gestures

Author and drive gestures. A gesture is a trigger word and/or key binding plus an ordered list of steps (animation, sound, chat, wait). `gesture.create` makes an empty item, `gesture.write` fills it in, `gesture.activate` makes the trigger live.

**A newly created gesture arrives with the server's default trigger `/hey` and replace-text `Hey!`.** `gesture.write` is a partial update, so omitting those keeps them — pass `trigger:""` and `replace_with:""` explicitly if you want a key-only gesture.

Gestures sharing a key binding are **not** all fired at once: the viewer picks one at random from the matching set, which is how "9 copies on F2" gives variation rather than noise.

| Tool | Params | Description |
|---|---|---|
| `gesture.list` | — | List the currently **active** gestures — the ones whose trigger words are live. Returns `{gestures:[{item_id, name, trigger, playing}]}`. Inactive gestures are ordinary inventory items; find them with `inventory.search`. Blocked by RLV @showinv. |
| `gesture.read` | `item_id*`: string | Read a gesture. Returns `{name, trigger, replace_with, key, mask, active, steps[]}`. Steps are `{type:"animation", animation, asset_id, action:"start"\|"stop"}`, `{type:"sound", sound, asset_id}`, `{type:"chat", text}`, `{type:"wait", seconds, for_animations, for_key_release}`. A gesture with no asset yet reads as an empty step list. Blocked by RLV @showinv. |
| `gesture.create` | `parent*`: string<br>`name*`: string | Create a new empty gesture in a folder. Returns the new item id; add trigger and steps with `gesture.write`, then `gesture.activate`. Blocked by RLV @edit. |
| `gesture.write` | `item_id*`: string<br>`trigger`: string<br>`replace_with`: string<br>`key`: integer<br>`mask`: integer<br>`steps`: object[] | Update a gesture. Omitted fields keep their current value; `steps` replaces the whole list. Step `animation`/`sound` accept an inventory item name **or** a UUID; if a step also carries the `asset_id` that `gesture.read` returned, that wins, so read-modify-write round-trips exactly. `key` is a viewer keycode (F2=162, F3=163, F6=166); `mask` is 0 none / **1 Ctrl** / 2 Alt / **4 Shift**. Rename the item itself with `inventory.rename`. Serialized gestures are capped at 1000 bytes. Blocked by RLV @edit. |
| `gesture.activate` | `item_id*`: string | Activate a gesture so its trigger word and key binding go live. Asynchronous — confirm with `gesture.list`. Returns `{accepted, active}`. |
| `gesture.deactivate` | `item_id*`: string | Deactivate a gesture so its trigger stops firing. Returns `{accepted, active}`. |
| `gesture.play` | `item_id*`: string | Play an **active** gesture now. Runs through the viewer's gesture manager, so RLV @sendgesture and chat restrictions apply as for a hand-triggered gesture. Note a suppressed play still returns `accepted:true` — the manager fails silently. |

## Wearables

Read and author clothing and body parts — textures, tint colours, and the visual params a human sees as sliders. Slot and param names come from `wearable.read`.

**Writes are worn-only.** The viewer has no capability for saving an unworn wearable; every save path is keyed to a currently-worn `(type, index)`. `wearable.write` on an unworn item returns an error telling you to wear it first. Reads work either way.

| Tool | Params | Description |
|---|---|---|
| `wearable.read` | `item_id*`: string | Read a wearable. Returns `{type, name, worn, textures{slot:uuid}, colors{slot:[r,g,b]}, params[{id, name, label, value, min, max, default}]}`. Slot names are the avatar texture-layer names for that wearable type; `params` is the tweakable set. Works worn or unworn. Blocked by RLV @showinv. |
| `wearable.create` | `parent*`: string<br>`name*`: string<br>`type*`: string<br>`wear`: boolean | Create a wearable at default settings. `type` is one of shape, skin, hair, eyes, shirt, pants, shoes, socks, jacket, gloves, undershirt, underpants, skirt, alpha, tattoo, physics, universal. `wear` default true — it must be worn before `wearable.write` can change it. Returns `{accepted, item_id, wear_requested}`; the wear completes asynchronously and is subject to RLV wearable locks, so confirm with `appearance.getWorn`. Blocked by RLV @edit. |
| `wearable.write` | `item_id*`: string<br>`name`: string<br>`textures`: object<br>`colors`: object<br>`params`: object | Change a **worn** wearable and save it. Any subset of `name`, `textures` `{slot: texture-uuid}`, `colors` `{slot: [r,g,b]}`, `params` `{name-or-id: value}`. Param values outside min/max are **rejected, not clamped**; colour writes to a slot with no colour channel (bodypaint, alpha) are rejected. All validation runs before any mutation, so a bad field never half-writes. Blocked by RLV @edit. |

## Appearance

| Tool | Params | Description |
|---|---|---|
| `appearance.detachItems` | `item_ids*`: string[] | Take off / detach worn items. {"item_ids"} is an array of item UUIDs. Blocked per item by RLV @detach and wearable locks. |
| `appearance.getWorn` | — | List the items currently worn/attached (the Current Outfit Folder). Blocked by RLV @showinv. |
| `appearance.listOutfits` | — | List saved outfits (subfolders of My Outfits). Blocked by RLV @showinv. |
| `appearance.wearItems` | `item_ids*`: string[]<br>`replace`: boolean | Wear/attach inventory items. {"item_ids"} is an array of item UUIDs; optional {"replace"} (default false) replaces what's on the same slot/point. Blocked per item by RLV wearable/attachment locks. |
| `appearance.wearOutfit` | `folder`: string<br>`name`: string<br>`mode`: string | Wear a saved outfit folder. {"folder"} (outfit UUID) or {"name"} (an outfit under My Outfits); {"mode"} "replace" (default) or "add". Every item is checked against RLV wearable/attachment locks; a single locked item blocks the whole outfit. |

## Profile

| Tool | Params | Description |
|---|---|---|
| `profile.get` | `avatar_id`: string | Fetch an avatar's profile. Optional {"avatar_id"} (UUID) defaults to yourself. Viewing another avatar is blocked by RLV @shownames. |
| `profile.getPicks` | `avatar_id`: string<br>`pick_ids`: string[] | Read the full text of an avatar's picks — `desc`, `snapshot_id`, `parcel_id`, `pos_global`. Picks return in the profile's listing order (the viewer's Picks tab order); `sort_order` is vestigial and always 0 — don't sort by it. `sim_name` is usually empty; `pos_global` and `original_name` are the reliable location signals. `profile.get` returns pick ids and names only. Omit `pick_ids` to discover them (explicit ids return in the order you gave them). Results may be partial: unanswered ids land in `missing`, and `listing_answered: false` means the pick list never came back (as opposed to the avatar having none). Blocked by RLV @shownames. |
| `profile.getClassifieds` | `avatar_id`: string<br>`classified_ids`: string[] | Read an avatar's classified ads in full — `description`, `snapshot_id`, `parcel_name`, `sim_name`, `pos_global`, `category`, `creation_date`, `expiration_date`, `price_for_listing`. Ads return in the profile's listing order. `profile.get` does not list classifieds at all. Same `missing` / `listing_answered` semantics as `profile.getPicks`. Blocked by RLV @shownames. |
| `profile.setSelf` | `about_text`: string<br>`fl_about_text`: string<br>`allow_publish`: boolean<br>`hide_age`: boolean<br>`image_id`: string<br>`fl_image_id`: string | Update your own profile. Any of {"about_text", "fl_about_text", "allow_publish" (bool), "hide_age" (bool), "image_id", "fl_image_id"}. Blocked by RLV @editprofile (and @editpfp for images). |

## People

| Tool | Params | Description |
|---|---|---|
| `people.getNames` | `ids*`: string[] | Resolve avatar UUIDs to names. {"ids"} is an array of UUIDs. Cached names return immediately; misses are warmed for a retry. Names hidden by RLV @shownames are omitted. |
| `people.getFriends` | `online_only`: boolean | List your friends/contacts: id, name, `online`, and the granted rights each way — `they_grant_me` / `i_grant_them` (each with `online`/`map`/`modify`). `online_only:true` filters to those shown online. A friend's `online` is only meaningful/true when they granted you see-online (`they_grant_me.online`). Names hidden by RLV @shownames are omitted. |

## Directory search

| Tool | Params | Description |
|---|---|---|
| `search.classifieds` | `query*`: string | Search Classifieds. {"query"} (min 3 chars). Blocked by RLV @showsearch. |
| `search.events` | `query*`: string | Search upcoming Events. {"query"} (min 3 chars). Blocked by RLV @showsearch. |
| `search.groups` | `query*`: string | Search the Groups directory. {"query"} (min 3 chars). Blocked by RLV @showsearch. |
| `search.land` | `max_price`: integer<br>`min_area`: integer | Search for-sale Land (parcels), price-sorted ascending. Optional {"max_price"} (L$) and {"min_area"} (m2) filters. Blocked by RLV @showsearch. |
| `search.people` | `query*`: string | Search the People directory. {"query"} (min 3 chars). Returns matching residents. Blocked by RLV @showsearch; names hidden by @shownames are omitted. |
| `search.places` | `query*`: string | Search the Places directory. {"query"} (min 3 chars). Returns matching parcels (name, dwell, for-sale). Blocked by RLV @showsearch. |

## Nearby avatars

| Tool | Params | Description |
|---|---|---|
| `avatars.getNearby` | `radius`: number | List avatars within {"radius"} metres (default 128, max 512) of you: id, distance, **global `position:[x,y,z]`**, **which way they face** (`facing:[x,y,z]` unit + `heading_deg`, 0=E/90=N; loaded avatars only), whether their body is loaded, and name (subject to RLV @shownames). To stand **behind** someone, walk to `position − N·facing`; **in front**, `position + N·facing`. Blocked by RLV @shownearby. |
| `avatars.getWorn` | `avatar_id*`: string<br>`resolve_names`: boolean | List the publicly-visible ATTACHMENTS worn by a nearby avatar ({"avatar_id"}): attach-point name, point id, object id, and the object **name** (resolved via a server round-trip; pass `resolve_names:false` to skip and return immediately). **To identify an item type (boots, hat, collar…), enumerate all attachments and match on the object name — never infer from the attach point, since rigged mesh can attach anywhere.** Only in-world attachments are observable — HUDs and clothing/bodypart wearables cannot be enumerated for anyone. The avatar must be loaded in range. Blocked by RLV @shownearby / @shownames. |

## Movement & world

Embodiment: find things around you, move to/among them, know where you are, and operate them. Teleport, sit/stand, and walkTo are async (the tool waits for the result). `object.touch` is fire-and-forget — a resulting blue-menu (`llDialog`) arrives via `notifications.list` / `notifications.respond`, not this tool's result. **`objects.getNearby` is how you get the `object_id`s** the sit/touch/walkTo tools need.

| Tool | Params | Description |
|---|---|---|
| `objects.getNearby` | `radius`: number<br>`limit`: integer<br>`scripted_only`: boolean<br>`resolve_names`: boolean | List in-world objects near you (rezzed prims/linksets — seats, vendors, doors, RLV furniture; **not** avatars or attachments). `radius` default 16 (max 64), `limit` default 32 (max 128, closest first), `scripted_only` default false, `resolve_names` default true (quick round-trip; false = immediate, name-less). Returns `{objects:[{object_id, name, distance, position:[x,y,z], scripted}]}`. Feed an `object_id` to `object.touch` / `movement.sit` / `movement.walkTo`. |
| `movement.teleport` | `landmark_item_id`: string<br>`global_position`: number[3]<br>`avatar_id`: string<br>`slurl`: string | Teleport your avatar. Provide **exactly one** destination: a landmark inventory item, a grid-global `[x,y,z]` (from `search.places`/`avatars.getNearby`), a nearby avatar to go to, or a **SLURL** (`maps.secondlife.com/secondlife/Region/x/y/z` or `secondlife://Region/x/y/z` — its region name is resolved automatically). Waits up to 60s; returns `{status: "arrived"\|"failed"\|"timeout", region, global_position}`. Blocked by RLV @tplm (landmark) / @tploc / @tplocal. |
| `movement.walkTo` | `global_position`: number[3]<br>`object_id`: string<br>`avatar_id`: string<br>`stop_distance`: number<br>`fly`: boolean<br>`pathfind`: boolean | Walk on foot to a spot (autopilot / "move to here"). **Exactly one** target: a `[x,y,z]`, an object, or a nearby avatar. **Stays on the ground — never flies**, even for far/elevated targets (pass `fly:true` to allow flying). **`pathfind:true`** routes *around* obstacles, *through doorways*, and *up/down stairs & ramps* — a raycast-grid A\* (per-cell floor height, so elevation changes are followed) finds a walkable route and walks it (use for cluttered / multi-room / multi-level spaces; ~1s to plan; returns `{status, distance, waypoints}`). Default (no `pathfind`) is a fast straight line. Waits up to 60s → `{status: "arrived"\|"stopped", distance}`. `stop_distance` default 1.5 m. |
| `movement.turn` | `degrees`: number<br>`global_position`: number[3]<br>`object_id`: string<br>`avatar_id`: string | Turn in place (no walking). **Exactly one**: `degrees` (relative; +left/CCW, −right), or something to face — a `[x,y,z]`, object, or avatar. Returns `{turned, facing:[x,y,z], heading_deg}` (heading 0=East, 90=North). |
| `movement.sit` | `object_id*`: string | Sit on an in-world object (must be loaded in range). Waits up to 5s → `{sitting, object_id}`. Blocked by RLV @sit. |
| `movement.stand` | — | Stand up if sitting. Waits up to 5s → `{sitting}`. Blocked by RLV @unsit. |
| `agent.getLocation` | — | Where you are now: `{region:{name}, position:{global,region}, parcel:{name, owner_id, area, flags, raw_flags}}`. |
| `object.touch` | `id*`: string<br>`face`: integer | Touch/click an in-world object (operates vendors, doors, RLV furniture). Fire-and-forget; any resulting blue-menu arrives via `notifications.*`. Blocked by RLV @touchall / @touchworld / @touchthis / @interact. |
| `hud.click` | `x*`: number<br>`y*`: number | Click a button on one of your worn HUDs by screen coordinate. `x`,`y` are **normalized 0..1 from the top-left**, as you'd read a screenshot — take one with `vision.snapshot {show_hud:true}` to see the buttons, then aim. Only registers a hit on a HUD (use `object.touch` for the world). Returns `{clicked, object_id, face}`. Blocked by RLV @touchhud. |

## Vision

| Tool | Params | Description |
|---|---|---|
| `vision.snapshot` | `width`: integer<br>`height`: integer<br>`hide_ui`: boolean<br>`show_hud`: boolean<br>`format`: jpeg \| png<br>`quality`: integer<br>`to_file`: boolean | Render the current 3D view as an image you can look at — the avatar's eyes. `width` default 1024 (256–2048), `height` from window aspect, `hide_ui` default true, `format` jpeg (default)/png, `quality` (jpeg) default 80. `show_hud:true` includes your HUD overlays (the viewer UI itself stays hidden) — use it to see HUD buttons before `hud.click`. Returns `{format, width, height, image_base64}`, or `{..., path}` with `to_file:true`. No RLV gate (own view). |

## Animation overlay (AO)

The Firestorm client-side AO — the animations your avatar plays when idle, walking, sitting, etc.

| Tool | Params | Description |
|---|---|---|
| `ao.getStatus` | — | `{enabled, stands_enabled, current_set, sets:[names]}`. |
| `ao.setEnabled` | `enabled*`: boolean | Turn the AO on/off. Returns `{enabled}`. |
| `ao.selectSet` | `name*`: string | Switch the active AO set by name (see `ao.getStatus`). Returns `{current_set}`. |
| `ao.cycle` | `direction`: next \| prev | Cycle the current stand to the next/previous animation (default next). Returns `{cycled, direction}`. |

## Instant messaging (1:1)

Private person-to-person IM (not public chat, not group chat). Inbound P2P IMs are buffered from server start; `im.replies` drains them. RLV @sendim gates sending; @recvim filters what's received.

| Tool | Params | Description |
|---|---|---|
| `im.send` | `avatar_id*`: string<br>`message*`: string | Send a 1:1 IM to an avatar. Returns `{sent, session_id}`. Blocked by RLV @sendim. |
| `im.replies` | `avatar_id`: string<br>`session_id`: string<br>`wait_seconds`: integer | Read newly-received IMs (optionally filter to one correspondent; omit both for all). `wait_seconds` (max 30) waits for the first if nothing buffered. Returns `{replies:[{from_id, from, text, session_id, time}]}`. |
| `im.getConversations` | — | List open 1:1 conversations: `{conversations:[{session_id, avatar_id, name, num_unread}]}`. |
| `im.getMessages` | `avatar_id`: string<br>`session_id`: string<br>`limit`: integer | Read stored history for one conversation (`limit` default 20, newest first). Read-only — does **not** clear the user's unread badge. Returns `{session_id, messages:[{from, from_id, message, time, timestamp, is_history}]}` (`time` is SL's display string; `timestamp` epoch is often 0). |

## Notifications & offers

One generic pair answers everything pushed at the avatar — inventory offers, teleport offers, friendship requests, group invites, and blue-menu `llDialog`s. This is the only way to accept an offer or press a dialog button.

| Tool | Params | Description |
|---|---|---|
| `notifications.list` | — | List pending notifications: `{id, name, type (offer_inventory\|offer_teleport\|offer_friendship\|invite_group\|script_dialog\|other), message, buttons:[names], payload}`. |
| `notifications.respond` | `id*`: string<br>`button*`: string | Press a button on a pending notification by **name** (e.g. `Keep`/`Discard` for an item offer, `Accept`/`Decline`, or a blue-menu label). Returns `{responded, button}`. Script-dialog presses enforce RLV @sendchat / @sendchannel. |
| `notifications.dismiss` | `id`: string | Clear a notification **without answering it** — no button is pressed, so an offer isn't accepted/declined and an attached item isn't kept/discarded. `id` dismisses one; **omit `id` to dismiss all**. Use this to clear things you only need gone (group notices `group_notice`, info toasts); use `notifications.respond` to actually act on an offer. Returns `{dismissed:<count>}`. |

## Groups

| Tool | Params | Description |
|---|---|---|
| `group.activate` | `group_id*`: string | Set your active (tag) group to {"group_id"}. Blocked by RLV @setgroup. |
| `group.getInfo` | `group_id*`: string | Fetch a group's details ({"group_id"}): name, charter, member count. Fetches from the server if not cached. |
| `group.getNotices` | `group_id*`: string | List a group's notices ({"group_id"}): notice id, subject, sender, whether it has an attachment, and timestamp. |
| `group.list` | — | List the groups you belong to, plus your active group. |
| `group.sendIM` | `group_id*`: string<br>`message*`: string | Send a message to a group's chat. {"group_id"} and {"message"}. You must be a member of the group. |
| `group.sendNotice` | `group_id*`: string<br>`subject*`: string<br>`message*`: string | Send a notice to a group. {"group_id"}, {"subject"}, {"message"}. Requires the group's Send Notices power. |

## Uploads

Uploads cost real **L$**. Every upload tool is two-phase: a call **without** `confirm:true` is a **dry run** — it validates each file and returns a per-file + total L$ cost estimate plus your balance, spending nothing. Pass `confirm:true` to actually upload. Files come from `paths` (explicit list) and/or `dir` (globs that type's extensions). `dest` is a folder UUID/name (default = the type's system folder); `names` overrides per-file inventory names. No RLV gate — the confirm requirement is the spend guard. Results stream back over SSE.

| Tool | Params | Description |
|---|---|---|
| `upload.image` | `paths`: string[]<br>`path`: string<br>`dir`: string<br>`dest`: string<br>`names`: string[]<br>`confirm`: boolean | Upload images (png/jpg/jpeg/tga/bmp) as textures. Large images are downscaled to the texture size limit. Dry-run without `confirm:true`; upload with it (costs L$; textures ≥2K cost more). |
| `upload.sound` | `paths`: string[]<br>`path`: string<br>`dir`: string<br>`dest`: string<br>`names`: string[]<br>`confirm`: boolean | Upload sounds. WAV must be **44.1 kHz, mono, 16-bit PCM, ≤10s** (encoded to Ogg Vorbis). Dry-run without `confirm:true`; upload with it. |
| `upload.animation` | `paths`: string[]<br>`path`: string<br>`dir`: string<br>`dest`: string<br>`names`: string[]<br>`loop`: boolean<br>`priority`: integer<br>`ease_in`: number<br>`ease_out`: number<br>`hand_pose`: integer<br>`confirm`: boolean | Upload animations: `.bvh` (converted client-side) or pre-baked `.anim`. For `.bvh`, optional tuning: `loop`, `priority` (0-4, default 2), `ease_in`/`ease_out` (seconds), `hand_pose` (default 1); ignored for `.anim`. Dry-run without `confirm:true`; upload with it. |
| `upload.material` | `paths`: string[]<br>`path`: string<br>`dir`: string<br>`dest`: string<br>`names`: string[]<br>`confirm`: boolean | Upload GLTF materials (`.gltf`/`.glb`). A file may hold several materials; each becomes an item and its textures upload too (cost = sum of those textures). Dry-run without `confirm:true`; upload with it. Note: materials upload asynchronously — verify results with `inventory.getFolder` on the dest. |
| `upload.mesh` | `path`*: string<br>`name`: string<br>`dest`: string<br>`scale`: number<br>`textures`: boolean<br>`rigged`: {`skin_weights`, `joint_positions`, `lock_scale_if_joint_position`}: boolean<br>`lods`: {`medium`, `low`, `lowest`}: `"auto"` or file path<br>`physics`: string<br>`analyze`: boolean<br>`confirm`: boolean | Upload one mesh model (`.dae`/`.gltf`/`.glb`) through the Upload Model floater, which opens while the tool runs. `rigged` options default to what the model contains. `physics` is `none` (default), `high`, `medium`, `low`, `lowest`, `cube`, or a file path. `analyze` runs convex hull decomposition on the physics shape, so it needs a `physics` value other than `none`. The dry run returns the server's fee quote with land impact, weights and triangle counts per LOD. Dry-run without `confirm:true`; upload with it. |

**Upload notes / gotchas:**

- **Cost is grid- and account-dependent.** Uploads are free on the Beta/test grid **and for some accounts** (certain benefit tiers), so `cost: 0` is often legitimate — not a bug. On accounts/grids that do charge, the same benefits lookup reports the real per-item price (≈L$10, more for ≥2K textures). Either way, the dry-run's `total_cost` + `balance` are the source of truth; confirm from those.
- **Server-side validation still bites at upload.** The dry-run checks extension + existence (+ decodability for images), but the strict per-type rules are enforced by the server/encoder at upload time: sounds must be **≤10s** 44.1 kHz mono 16-bit PCM — an over-length WAV *passes dry-run* but fails on `confirm` with SL's generic "server difficulties" error. Trim to ≤10s.
- **Confirmed results.** Image/sound/animation return real `item_id` + `asset_id` per file once the server finishes. Materials are fire-and-forget (no per-item callback) — the response reports the enqueue + estimated cost; confirm the actual items with `inventory.getFolder` on the dest.
- **Batch = one response.** A batch fans out to N concurrent uploads and the tool replies once, when all N have reported (or pre-validation-failed).
- **Mesh takes one file per call and can take minutes.** `upload.mesh` has a 25-minute timeout. It returns a busy error (-32006) if the Upload Model floater is already open or a previous mesh upload is still waiting on the server. Closing the floater before the upload is sent cancels the call; after that, the upload carries on. The dry run always includes `upload_blocked`; when it is true, `blocked_reason` says why a confirmed upload would be refused. A confirmed upload that timed out after being sent returns an error with `upload_sent: true` — it may still complete, so check inventory and balance before retrying.

## Objects

Build with what you uploaded: rez an item, edit it, texture it, fill its contents, set next-owner permissions, link it, and take it back. The tools work on rezzed objects and on your own worn attachments. They never touch your selection or the Build floater, so you can keep building while an agent works.

**Targeting.** `object_id` is any prim of the object, from `objects.getNearby`, `avatars.getWorn`, or an `object.rez`/`object.attach` result. `link` picks one prim by the number `object.get` reports (1 = the root). Without `link`, a tool acts on the whole object. These link numbers can differ from LSL's `llGetLinkNumber`, so always take them from `object.get`.

**Position and rotation.** `position` is in metres and `rotation` is `[x,y,z]` Euler degrees, as the Edit floater shows them. Both are relative to the region for a rezzed root, to the attachment point for a worn root, and to the root for a child link. `scale` changes one prim: the root, or the prim named by `link`.

| Tool | Params | Description |
|---|---|---|
| `object.get` | `object_id*`: string<br>`faces`: boolean | Describe an object link by link: `{object_id, link_count, attachment_point, links:[{link, object_id, name, description, position, rotation, scale, face_count, permissions}]}`. `attachment_point` appears only for a worn object. `permissions` has `owner_id` and `base`/`owner`/`group`/`everyone`/`next_owner` as `{copy, modify, transfer}`. `faces:true` adds each face's `texture`, `material`, `color`, `alpha`, `repeats`, `offset`, `rotation`. Links whose name and permissions didn't arrive within 10 s are listed in `incomplete_links`. |
| `object.edit` | `object_id*`: string<br>`link`: integer<br>`position`: [x,y,z]<br>`rotation`: [x,y,z]<br>`scale`: [x,y,z]<br>`name`: string<br>`description`: string | Move, rotate, scale, rename or describe an object you can modify. Without `link`, position and rotation move the whole object. Names and descriptions must be printable ASCII without `\|`. Returns the values the object reports once the region confirms (up to 10 s). Blocked by RLV @edit, @editobj, @editworld, @editattach, @interact. |
| `object.setFaces` | `object_id*`: string<br>`faces*`: `"all"` or integer[]<br>`link`: integer<br>`texture`: string<br>`material`: string<br>`color`: [r,g,b]<br>`alpha`: number<br>`repeats`: [u,v]<br>`offset`: [u,v]<br>`rotation`: number | Texture, color and map faces. `"all"` without `link` covers every prim; a face list without `link` means the root. `texture` and `material` take an inventory item id (drop rules apply, so a no-copy item moves into the prim's contents) or a raw asset UUID. `material:"none"` removes a PBR material, and setting a texture removes one too. On a face with a PBR material, `repeats`, `offset` and `rotation` change the material's texture transform, and applying a material keeps the face's current mapping. Colors and alpha are 0-1; rotation is degrees. Returns `{object_id, faces:[{link, face, ok, error}]}`. Blocked by RLV @edit, @editobj, @editworld, @editattach, @interact. |
| `object.contents` | `object_id*`: string<br>`link`: integer | List the items inside the root prim, or inside the prim named by `link`: `{object_id, items:[{item_id, name, type, permissions}]}`. The region only shows contents of objects you can modify, and returns error -32005 if the region can't send them. |
| `object.addContents` | `object_id*`: string<br>`item_ids*`: string[]<br>`link`: integer<br>`running`: boolean | Put up to 32 inventory items into the object, as dragging them onto it would. Scripts start running unless `running:false`. A no-copy item leaves your inventory; worn items are refused. Returns `{object_id, items:[{item_id, name, result}]}`: `added`, `not_found` (sent but not seen within 20 s), or why it wasn't sent. If the call times out after sending, the error lists what was sent. Blocked by RLV @edit, @editobj, @editworld, @editattach, @interact, a locked attachment, or @unsit/@sittp while you sit on the object. |
| `object.copyContents` | `object_id*`: string<br>`item_ids`: string[]<br>`link`: integer<br>`dest`: string | Copy up to 32 items out of the object into your inventory, as dragging them out of its Contents would. Omit `item_ids` to copy everything. `dest` is a folder UUID or "root"; without it each item goes to its type's folder (Objects, Scripts, and so on). A no-copy item in an object you own moves out of the object. Returns `{object_id, items:[{item_id, name, result, new_item_id}]}`: `copied`, `moved`, `not_found` (sent but not seen in your inventory within 20 s), or why it wasn't sent. Blocked by RLV @edit, @editobj, @editworld, @editattach, @interact, a locked attachment, @unsit/@sittp while you sit on the object, or an RLV-locked `dest`. |
| `object.removeContents` | `object_id*`: string<br>`item_ids*`: string[]<br>`confirm*`: boolean<br>`link`: integer | Delete up to 32 items (ids from `object.contents`) from an object you can modify. Requires `confirm:true`. Removed items are deleted outright, not moved to Trash. Returns `{object_id, items:[{item_id, name, result}]}`: `removed`, `still_there` (sent but still listed after 20 s), or `not in the object's contents`. Blocked by RLV @edit, @editobj, @editworld, @editattach, @interact, a locked attachment, or @unsit/@sittp while you sit on the object. |
| `object.setPermissions` | `object_id*`: string<br>`next_owner*`: {`copy`, `modify`, `transfer`}: boolean<br>`contents`: boolean | Set next-owner permissions on a rezzed object you own; worn objects are refused, so rez them first. The next owner needs copy or transfer, so turning copy off keeps transfer on. `contents:true` sets the same on every item in the root prim. Returns `{object_id, next_owner, contents:[{item_id, name, result}]}`, where `result` is `sent` (the region doesn't confirm item updates), `unchanged` or `permission`. Bits missing from the object's base permissions stay off. Blocked by RLV @edit, @editobj, @editworld, @editattach, @interact. |
| `object.link` | `object_ids*`: string[] | Link two or more of your rezzed objects in one region. The first becomes the root. Returns `{object_id, link_count}`. A link the region refuses (too many prims, too far apart) shows as a timeout. Blocked by RLV @edit, @editobj, @editworld, @interact. |
| `object.unlink` | `object_id*`: string<br>`link`: integer | Unlink one prim (`link`), or every prim when `link` is omitted. Returns `{object_ids}`: the unlinked prims, then the root of what's left. Blocked by RLV @edit, @editobj, @editworld, @interact. |
| `object.rez` | `item_id*`: string<br>`position`: [x,y,z]<br>`rotation`: [x,y,z] | Rez an object item. The default position is 2 m in front of you at your height. Returns `{object_id, name, position, rotation}`, plus `item_consumed:true` for a no-copy item. If the new object is no-modify or RLV blocks editing it, the result has `rotation_skipped` with the reason. Items holding several objects (coalesced) can't be rezzed. Waits up to 30 s; a timeout says the rez was sent and carries `item_consumed` for a no-copy item. Blocked by RLV @rez and @interact. |
| `object.attach` | `item_id*`: string<br>`point*`: string<br>`replace`: boolean | Wear an object item on a named point: "Chest", "Spine", "Left Hand", "HUD Center", and so on, in any case. `replace:true` replaces what's on the point; the default adds alongside it. Returns `{object_id, item_id, point}`. Blocked by RLV attachment locks and @addattach. |
| `object.take` | `object_id*`: string<br>`copy`: boolean<br>`folder`: string | Take a rezzed object you own into inventory, or a copy of it with `copy:true`. `folder` is a folder UUID, `"objects"` (default) or `"root"`. Returns `{item_id, name, folder_id, copy}`. Worn attachments are refused; detach them instead. A timeout after the request was sent names the folder to check. Blocked by RLV @edit, @editobj, @editworld, @interact, @rez, and @unsit while you sit on the object. |

- **Edits wait for the region.** `object.edit`, `object.setPermissions`, `object.link` and `object.unlink` answer only once the region reports the change. A timeout (-32001) carries `sent` and, where known, `current`.
- **Batch tools report per item.** `object.setFaces`, `object.addContents`, `object.copyContents`, `object.removeContents` and `object.setPermissions` with `contents` give a result for each face or item, and fail the whole call only when nothing could be done.
- **The build loop.** `upload.mesh` → `object.rez` → `object.edit` → `object.setFaces` → `object.addContents` → `object.setPermissions` → `object.take`. Use `object.attach` instead of `object.rez` for something worn.

## Money (L$)

`money.pay` spends **real currency**. It is fenced: refused unless Debug Setting `IDMCPMoneyEnabled = 1` (off by default), capped by `IDMCPMoneyMaxAmount` (default 1000), gated by RLV @pay/@buy, and it checks affordability (never auto-opening the buy-currency floater). Use `dry_run:true` to preview without spending. `money.getBalance` is read-only and always available.

| Tool | Params | Description |
|---|---|---|
| `money.getBalance` | `fresh`: boolean | Your current L$ balance. `fresh:true` requests an update from the server and waits briefly. Returns `{balance, currency, fresh}`. |
| `money.pay` | `target_id*`: string<br>`target_kind*`: avatar \| object<br>`amount*`: integer<br>`description`: string<br>`dry_run`: boolean | Pay L$ to an avatar or object. `dry_run:true` → `{would_pay:{target_id, resolved_name, amount}, affordable}` without sending. On send → `{paid, target_id, amount}`. Requires `IDMCPMoneyEnabled`, within `IDMCPMoneyMaxAmount`, RLV @pay/@buy allowed, and sufficient funds. |
