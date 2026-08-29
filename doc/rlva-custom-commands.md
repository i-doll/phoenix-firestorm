# Firestorm fork — custom RLVa commands

Reference for the RLVa commands **added by this fork** — author **Five** (Amalthea
Skydancer) and **Trish** — plus any stock command whose **behaviour** this fork
extends (see *Extended stock commands*). Stock RLVa (Kitty Barnett) and upstream
Firestorm commands are otherwise **not** documented here.

Attribution below is by **commit authorship** (`git log --author`), which is
authoritative — not `git blame` (merge/reformat commits reassign lines to upstream)
and not the `[RLVa:ID]` marker alone. Sourced from `indra/newview/rlvhelper.cpp`
(registrations), `rlvdefines.h` (enums), and the cited enforcement sites.

---

## Notation

RLVa commands are issued by scripted objects (`llOwnerSay`) as `@command[:option]=param`.

| Form | Meaning |
|------|---------|
| `@cmd=n` / `@cmd=y` | add (restrict) / remove (release) a restriction |
| `@cmd=force` | a one-shot forced action |
| `@cmd:<option>=…` | scoped/parameterised by `<option>` — a UUID, folder path, message, or number |
| `@cmd:<n>=n` | a **modifier** value (distance in m, angle in radians) that tunes the restriction |

- **Option type** — parser grammar: `NONE`, `NONE_OR_EXCEPTION` (`:<uuid>` whitelist),
  `NONE_OR_EXCEPTION_OR_MODIFIER` (also a numeric value), `MODIFIER` (requires a value).
- **Strict** — supports RLVa secure-exception semantics (exceptions from one object don't let another bypass).

---

# Five's commands

Commit hashes are Five's authoring commits.

## Visibility / floater restrictions

### `@showpeople`
- **Syntax:** `@showpeople=n|y` · no parameters.
- **Description:** Blocks the **People** floater/panel (friends, groups, nearby, recent); force-hides it while active.
- **Enforced at:** `rlvui.cpp:254` (`onToggleShowPeople`); registered `rlvhelper.cpp:163`.
- **Added by:** Five, `548438ffd5` ("Added custom RLV command").

### `@showareasearch`
- **Syntax:** `@showareasearch=n|y` · no parameters.
- **Description:** Blocks the **Area Search** floater; force-closes it on open.
- **Enforced at:** `fsareasearch.cpp:223` (`onOpen`→`closeFloater`); UI enabler `rlvui.cpp:294`.
- **Added by:** Five, `b8d84f8d20` ("add @showareasearch=n"). *Later reworked into the
  `<FS:Trish>` block by Trish (`a13437afba`), which is why the current registration sits
  there and the enum was renamed `RLV_BHVR_AREASEARCH`.*

## Profile restrictions (`[RLVa:ID]`)

The four `@edit*` commands are plain `=n`/`=y` restrictions (no options); gates live at
`rlvactions.cpp:577-599`.

### `@editprofile`
- **Syntax:** `@editprofile=n|y` · no parameters.
- **Description:** Blocks editing own profile content — About text, first-life bio, images.
- **Enforced at:** `rlvactions.cpp:579` (`canEditProfile`).
- **Added by:** Five, `27b35db0bd` ("Add @editprofile and @editpicks").

### `@editpicks`
- **Syntax:** `@editpicks=n|y` · no parameters.
- **Description:** Blocks creating/editing/deleting profile **Picks**.
- **Enforced at:** `rlvactions.cpp:584` (`canEditPicks`).
- **Added by:** Five, `27b35db0bd`.

### `@editdisplayname`
- **Syntax:** `@editdisplayname=n|y` · no parameters. (Toggle.)
- **Description:** Blocks changing the **display name**; also closes and filters the Display Name floater while active.
- **Enforced at:** `rlvactions.cpp:589` (`canEditDisplayName`); floater filter `rlvhandler.cpp:2140`.
- **Added by:** Five, `3f503aec9a` ("add @editdisplayname=n").

### `@editpfp`
- **Syntax:** `@editpfp=n|y` · no parameters.
- **Description:** Blocks changing own **profile picture** (PFP).
- **Enforced at:** `rlvactions.cpp:594` (`canEditProfileImage` → `RLV_BHVR_EDITPFP`).
- **Added by:** Five, `009f19b6c5` ("Add @editpfp=n").

### `@setprofileimage`  (force)
- **Syntax:** `@setprofileimage:<texture-uuid>=force`
- **Parameter:** `<texture-uuid>` — required, valid non-null UUID (else `RLV_RET_FAILED_OPTION`).
- **Description:** **Forces** the user's profile picture to the given texture, server-side, via the `AgentProfile` capability (`PUT` of `sl_image_id`).
- **Implemented at:** `rlvhandler.cpp:3449` (`RlvForceHandler<SETPROFILEIMAGE>`), coroutine `:3418`.
- **Added by:** Five, `c89b944e46` ("Add RLVa command for forcing profile picture").

## Environment

### `@lockenv`
- **Syntax:** `@lockenv=n|y` · no parameters. (Toggle.)
- **Description:** Locks the **environment** (EEP). Forces the shared parcel/region
  environment (`LLEnvironment::setSharedEnvironment`) and closes + filters the env
  editors (Personal Lighting/snapshot, Extended Day-Cycle, Fixed Sky, Fixed Water, My
  Environments). Distinct from upstream `@setenv` (which restricts programmatic env
  changes) — `@lockenv` also forces shared env and blocks the UI.
- **Enforced at:** `rlvhandler.cpp:2612`.
- **Added by:** Five, `27357fa4ac` ("add @lockenv=n"). (`@setenv`, adjacent, is upstream.)

## Land

### `@showpropertylines`
- **Syntax:** `@showpropertylines=n|y` · no parameters.
- **Description:** Suppresses the parcel **property-line** overlay. Checked every frame
  at the draw site so it can't be defeated by toggling `ShowPropertyLines`; the user's
  own setting is untouched, and the View ▸ Property Lines menu item greys out.
- **Enforced at:** `llviewerparceloverlay.cpp:685`.
- **Added by:** Five, `5175a6602d` (2026-08-09).

## Inventory (shared-folder exceptions)

### `@sharedwear` / `@sharedunwear` — per-folder exceptions
> The base `@sharedwear=n|y` / `@sharedunwear=n|y` are **upstream RLVa 1.3**. The fork
> addition is the optional `:<path>` **exception** argument.

- **Syntax:** `@sharedwear[:<path>]=n|y` · `@sharedunwear[:<path>]=n|y`
- **Parameter `<path>`:** a shared-folder path relative to `#RLV` (e.g. `Hats/Party`) —
  a folder path, not a UUID; resolved lazily (a bad path is a silent no-op).
- **Description:** No option → the whole `#RLV` subtree is locked against wearing
  (`sharedwear`) / removing (`sharedunwear`). With `:<path>` → that subfolder+subtree is
  carved out as an **allow-exception** (like `@unsharedwear` excepts the shared root).
  Only the global lock is reference-counted; exceptions are not, and an exception only
  lifts a restriction from the **same object**.
- **Implemented at:** `rlvhandler.cpp:1654-1675` → `RlvFolderLocks::addFolderLock`/`removeFolderLock`.
- **Added by:** Five, `d6517e9249` (2026-08-09). Base commands upstream.

## Communication

### `@blockedimmsg`  (force)
- **Syntax:** `@blockedimmsg:<message>=force` (set) · `@blockedimmsg=force` (clear).
- **Parameter:** `<message>` — the custom text to use.
- **Description:** Sets (or clears) a **custom "IM blocked" auto-response** message for
  the issuing object — the text sent back when RLV blocks the user from answering an IM.
  Each object may register its own string; when several are set, one is chosen at random
  (per the commit's intent), so a captor's gear can vary the blocked-IM reply.
- **Implemented at:** `rlvhandler.cpp:3472` (`RlvForceHandler<BLOCKEDIMMSG>`) → `RlvStrings::setScriptBlockedSendImString`/`clear…`.
- **Added by:** Five, `ecef73913e` ("Random select between arbitrary number of im blocked strings").

## Camera limits

Five's additions to the `@setcam_*` suite — clamp where the camera may point. All take
a **modifier** value in **radians** (`RLV_OPTION_MODIFIER`). Enforced in
`rlvactions.cpp:132-156` (`getCameraPitchLimits` / `getCameraYawLimit`), applied by the
camera code. Added together in `4e8720e831` ("Add sitting yaw clamp, RLVa pitch/yaw
limits, and fix mouselook mouse bypass").

### `@setcam_pitchmin`
- **Syntax:** `@setcam_pitchmin:<radians>=n`
- **Description:** Clamps how far **up** the camera may pitch from horizontal. Modifier default `π/2` (90°).

### `@setcam_pitchmax`
- **Syntax:** `@setcam_pitchmax:<radians>=n`
- **Description:** Clamps how far **down** the camera may pitch from horizontal. Default `π/2`.

### `@setcam_yaw`
- **Syntax:** `@setcam_yaw:<radians>=n`
- **Description:** Clamps the camera **yaw** half-range around the sit/forward direction (fixes gaze left/right). Default `π` (180°).

> Related Five change (not a new command): `0558ff00d1` makes `@setcam_avdistmax:0`
> (a.k.a. `@camdistmax:0`) also **prevent exiting mouselook** — forced first-person.

---

# Trish's commands

Source commits `a13437afba` ("new RLVa functions favwear, lookat, areasearch",
2026-05-10) and `a842d45879` ("RLVa restrictions for sounds, search, online status",
2026-07-20). Registered in the `<FS:Trish>` block, `rlvhelper.cpp:164-177`. Five later
added the `BHVR_EXTENDED` flag to these (`0bcf0f055d`, metadata only).

## Visibility / floater restrictions

Each is a plain `@cmd=n|y` toggle, no parameters.

### `@showlookat`
- **Description:** Suppresses the **LookAt** (gaze/focus) HUD effect — hides where avatars are looking.
- **Enforced at:** `llhudeffectlookat.cpp:711`.

### `@showfavwear`
- **Description:** Blocks the **Wearable Favorites** floater; force-closes it on open.
- **Enforced at:** `fsfloaterwearablefavorites.cpp:162`, toggle `:382`.

### `@showcontacts`
- **Description:** Hides friends'/contacts' **online status** in the Contacts floater and avatar lists.
- **Enforced at:** `fsfloatercontacts.cpp:694,761`; `llavatarlistitem.cpp:288`; `llavatarlist.cpp:289`; `llcallingcard.cpp:853`; UI enabler `rlvui.cpp:350`.

### `@showsearch`
- **Description:** Blocks the **Search** floater. The Search toolbar button is **disabled (greyed), not removed** (refined by Five, `6c4531a433`).
- **Enforced at:** `llviewermenu.cpp:8276`; `fsfloaterplacedetails.cpp:578`; UI enabler `rlvui.cpp:313`; embedded-agent gate `idsearchmodel.cpp:33`.

## Sound restrictions

All three are **strict-capable** and accept a per-source **UUID exception**
(`:<uuid>=add|rem`, keeps that source audible). `@worldsounds`/`@soundothers` also accept
a **distance modifier** (metres); `@soundself` does **not**.

Sounds kept audible by a distance modifier **taper off** toward the edge of that radius
instead of cutting out at it: gain is multiplied by a raised cosine, `0.5 * (1 + cos(pi * d/R))`
— untouched at the ear, -6 dB at half the radius, silent exactly at `R`. `getSoundTaperGain()`
in `rlvactions.cpp`.

Attached/looping object sounds are re-evaluated **every frame** (`llaudiosourcevo.cpp`), so the
fade follows the listener as they move. A restricted source is **muted, not stopped** — and one
that arrives while restricted is still created — so a looping sound fades back in when the
restriction lifts or the listener walks into range. The trade-off is that a restricted sound is
still registered with the Sound Explorer; an accepted leak in exchange for a symmetric fade.
One-shot triggered sounds are simply dropped when the gain reaches zero.

### `@worldsounds`  (synonym `@worldsound`)
- **Syntax:** `@worldsounds=n|y` · `@worldsounds:<metres>=n` · `@worldsounds:<uuid>=add|rem`
- **Modifier:** `WorldSoundsDist`, default `0.0` m, comparator `min`.
- **Description:** Silences in-world (non-avatar object) sounds. Plain `=n` is unbounded;
  a distance modifier keeps sounds within `<metres>` audible. The object you're sitting
  on and per-UUID exceptions stay audible. `@worldsound` is a registered alias.
- **Enforced at:** `rlvactions.cpp` (`getWorldSoundGain`/`getSoundGain`); per-frame taper in `llaudiosourcevo.cpp`.

### `@soundothers`
- **Syntax:** `@soundothers=n|y` · `@soundothers:<metres>=n` · `@soundothers:<uuid>=add|rem`
- **Modifier:** `OtherAvatarSoundsDist`, default `0.0` m, comparator `min`.
- **Description:** Silences sounds from **other avatars** and their attachments; same semantics as `@worldsounds`.
  Also covers other avatars' footsteps and typing sounds, which follow the same distance taper.
- **Enforced at:** `rlvactions.cpp` (`getAvatarSoundGain`); avatar sounds `llvoavatar.cpp:5157,7058`.

### `@soundself`
- **Syntax:** `@soundself=n|y` · `@soundself:<uuid>=add|rem` (**no distance form**).
- **Description:** Silences sounds from **your own** avatar and attachments (HUD audio never covered).
- **Enforced at:** `rlvactions.cpp` (`getAvatarSoundGain`, `getSoundGain`).

---

## Excluded (upstream, for reference)

Upstream RLVa 2.x / stock Firestorm — not fork additions, despite the `BHVR_EXTENDED`
flag: `@interact`, `@touchhud`, `@tprequest`, `@accepttprequest`, `@findfolders`,
`@getcommand`, `@getheightoffset`.

Also authored by Five but **not new commands**: `d2223c4a1a` ("Auto accept give to
#RLV" — the `RestrainedLoveAutoAcceptGiveToRLV` setting) and `04f8a3f85c` /
`0bcf0f055d` (a `@viewtransparent` enforcement fix and the `BHVR_EXTENDED` flagging).

---

# Extended stock commands

Stock RLVa commands whose scope this fork **widens**. A restraint author working
from the upstream RLVa spec will not expect the extra behaviour documented here.

## `@edit` — extended to inventory content

- **Syntax:** `@edit=n|y` · unchanged; still `NONE_OR_EXCEPTION`.
- **Stock behaviour:** blocks editing **in-world objects** — the build tools, the
  edit floater and object selection. `RlvActions::canEdit` takes an
  `LLViewerObject*` (`rlvactions.cpp:698`) or an `ERlvCheckType` (`:668`); the
  toggle handler drives the build floater (`rlvhandler.cpp:2099`).
- **Fork extension:** additionally blocks **authoring inventory content** through
  the embedded MCP server — `gesture.create`, `gesture.write`, `wearable.create`
  and `wearable.write` return the structured RLV denial while `@edit` is active.
- **Not affected:** reads. `gesture.read`, `gesture.list` and `wearable.read`
  follow `@showinv`, as the rest of the inventory tools do. `gesture.play` is
  governed by `@sendgesture` (enforced inside `LLGestureMgr`, `llgesturemgr.cpp:557`).
- **Deliberate gap:** a worn wearable held by an RLV lock is still rewritable while
  `@edit` is off — the lock governs detaching, not content. Content protection
  requires `@edit`. This was considered and chosen, not overlooked.
- **Enforced at:** `idmcptools_gesture.cpp` and `idmcptools_wearable.cpp` — the
  request-phase `gate_edit` on all four tools, plus a re-check in `gesture.write`
  before its deferred upload is enqueued (`idmcp_gesture_upload`), covering a
  restriction that arrives during the async gesture-asset load. There is no
  post-completion check: once a side effect has landed, reporting it blocked
  would be a false negative.
