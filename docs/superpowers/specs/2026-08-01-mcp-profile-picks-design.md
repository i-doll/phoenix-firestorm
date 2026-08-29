# Design: `profile.getPicks` / `profile.getClassifieds`

Date: 2026-08-01
Status: approved, ready for implementation plan

## Problem

`profile.get` returns an avatar's picks as `(id, name)` pairs only. There is no way to
read a pick's description, snapshot, or location through the MCP surface, so an agent
asked "what do this person's picks say?" can report the titles and nothing else.

This bites in practice. Residents commonly use picks as prose — a serialized backstory
across `part 1`..`part 5`, or a photo gallery titled with their own name. The titles alone
carry almost no information, and the answer "open the Picks tab yourself" defeats the
point of the tool.

Classifieds have the same gap, and worse: `profile.get` does not list them at all.

## Why it can't ride along on `profile.get`

The `AgentProfile` capability fills `picks_list` with ids and names only —
`llavatarpropertiesprocessor.cpp:419`. Descriptions exist solely behind the legacy UDP
`pickinforequest`, which is **one round-trip per pick**
(`sendPickInfoRequest(creator_id, pick_id)`, line 784). There is no batch form and no cap
equivalent.

So pick detail is necessarily a separate, slower, fan-out-shaped operation. Folding it
into `profile.get` would make the common path pay for the rare one, and would let a single
stalled pick delay the whole profile.

## Constraints discovered while reading the viewer

1. **Detail replies dispatch on `creator_id`, not the detail id.** `processPickInfoReply`
   calls `notifyObservers(pick_data.creator_id, ...)` (line 612). A fan-out over N picks
   therefore lands N replies on a single observer key; the waiter must demux on
   `pick_data.pick_id`.

2. **Detail requests are not pending-tracked.** Both `sendPickInfoRequest` and
   `sendClassifiedInfoRequest` send directly without `addPendingRequest`. Good news for
   batching: N concurrent detail requests are *not* deduped down to one.

3. **Listing requests *are* pending-tracked.** `avatarpicksrequest` /
   `avatarclassifiedsrequest` go through `sendGenericRequest`, which drops a re-request
   for the same `(avatar_id, type)` within 5 s (`isPendingRequest`, line 149). Two
   `getPicks` calls on one avatar in quick succession can have stage 1 silently no-op.
   The output must distinguish "this avatar has no picks" from "the listing never
   answered."

4. **Classifieds notify twice.** `processClassifiedInfoReply` calls `notifyObservers`
   on both `creator_id` and `classified_id` (the FS legacy-search patch, line 530).
   Registering on `creator_id` alone yields one delivery; the `pending.erase` demux is
   idempotent regardless.

5. **`sweepTimeouts` is a hard failure.** A deadline set via `IDMCPCall::setDeadline`
   makes the server respond `-32001 TIMEOUT` and discard collected results
   (`idmcpserver.cpp:367`). Wrong for a fan-out — four of five picks arriving should
   return four picks. The waiter owns a soft deadline instead.

## Architecture

All code lands in the existing `indra/newview/idmcptools_profile.cpp`. A new `.cpp` would
force a full PCH rebuild via autobuild+cmake, and this is profile-domain code.

Picks and classifieds have an identical control shape — list, fan out, aggregate — so
they share one waiter and one observer, discriminated by a `Kind` enum. This mirrors the
established `WornNameWait` pattern in `idmcptools_avatars.cpp:68`.

```
enum class DetailKind { PICKS, CLASSIFIEDS };

struct ProfileDetailWait
{
    IDMCPCallPtr       call;
    LLUUID             target;              // avatar whose picks/classifieds these are
    DetailKind         kind;
    std::set<LLUUID>   pending;             // detail ids still awaiting a reply
    boost::json::array out;                 // completed detail objects
    bool               listing_answered = false;
    F64                deadline = 0.0;
    bool               done = false;
};
```

A file-static `std::vector<std::shared_ptr<ProfileDetailWait>>` plus a `mainloop`
listener drives progress, exactly as `worn_tick` does: finish when `pending` is empty, or
when the deadline passes, whichever comes first.

### Flow

**Stage 1 — listing** (skipped when the caller supplies explicit ids):
pick discovery branches on the `AgentProfile` capability, mirroring `profile.get` and
`llpanelprofilepicks.cpp` — cap present: `sendAvatarPropertiesRequest` → `APT_PROPERTIES`,
harvesting `LLAvatarData::picks_list`; cap absent (OpenSim): `sendAvatarPicksRequest` →
`APT_PICKS`. Classifieds discovery is unconditional (`sendAvatarClassifiedsRequest` →
`APT_CLASSIFIEDS`). On the listing reply, set `listing_answered = true` and seed `pending`
with the returned ids. A second listing reply for the same wait (reachable because the
processor's 5 s dedupe window is shorter than the 10 s deadline) is ignored via the
`listing_answered` guard, so completed ids are never re-pended.

**Stage 2 — fan-out:** issue one `sendPickInfoRequest` / `sendClassifiedInfoRequest` per
pending id.

**Aggregation:** each `APT_PICK_INFO` / `APT_CLASSIFIED_INFO` reply is matched by
`pick_id` / `classified_id`, serialized into `out`, and erased from `pending`.

**Finish:** `out` plus a `missing` array (whatever remained in `pending`) plus
`listing_answered`. Partial results are a success, not an error.

Deadline: 10 s overall, covering both stages.

Teardown follows the existing `IDMCPProfileObserver` discipline — an `mSettled`-style
guard so the reply path and the abandon path (client hangup) never double-free, with
`call->setCleanup()` wired to abandon the waiter.

## Tool surface

```
profile.getPicks       {avatar_id, pick_ids?: string[]}
  -> {avatar_id, picks: [...], missing: [...], listing_answered: bool}

  pick: id, name, desc, snapshot_id, parcel_id, sim_name (usually empty),
        pos_global {x, y, z}, sort_order (vestigial, always 0), enabled,
        original_name, user_name

  picks[] is emitted in the profile's listing order (the viewer's Picks tab
  order), or the caller's array order when pick_ids is given.

profile.getClassifieds {avatar_id, classified_ids?: string[]}
  -> {avatar_id, classifieds: [...], missing: [...], listing_answered: bool}

  classified: id, name, description, snapshot_id, parcel_id, parcel_name,
              sim_name, pos_global {x, y, z}, category, creation_date,
              expiration_date, flags, price_for_listing
```

`avatar_id` defaults to self when omitted, consistent with `profile.get`.

Both tool descriptions must state that pick/classified detail requires a per-item
round-trip and may return partial results, so a caller understands `missing`.

## RLV gating

- **`@shownames`** on the target gates both tools. `gate_profile_get`'s body is extracted
  into a shared helper and reused, rather than duplicated three ways.
- **`@showloc` is deliberately not consulted.** Location fields (`sim_name`, `parcel_name`,
  `pos_global`) always populate. Rationale: a pick is published directory data, and
  `@showloc` conceals where *you* are, not where a public parcel is.
  Accepted trade-off, recorded so it is not mistaken for an oversight: these become the
  only tools on the surface that read world-location data with no RLV gate, so a future
  scene applying `@showloc` and expecting blindness will find this hole.
- **`@showsearch` is not consulted**, matching `profile.get`. These are profile reads, not
  directory searches.

## Decided against

**Adding a classifieds listing to `profile.get`.** It is the same compound-tool objection
that ruled out putting pick detail there: an extra UDP round-trip on the common path so
the rare path saves a call. `getClassifieds` auto-discovers ids itself, so the listing is
never needed in `profile.get`. Cost accepted: nothing hints that an avatar *has*
classifieds, and calling `getClassifieds` blind is cheap (empty array).

**A single-pick tool.** Reading a five-part backstory would cost five sequential tool
calls and five agent turns. The batch tool is the real use case.

## Testing

This viewer is not built in-session; correctness is established by reading the viewer's
own call/reply paths, and Five runs the build.

Live verification once built:

1. `getPicks` on a nearby avatar with several picks whose titles are numbered parts of one
   text — exercises the multi-reply demux, and confirms the parts come back in the
   profile's listing order (the viewer's Picks tab order). `sort_order` is no help here:
   the viewer saves every pick with 0.
2. `getPicks` on a second avatar with a different pick count — guards against an
   off-by-one or a stale `pending` set leaking between calls.
3. `getPicks` on self — the `avatar_id`-omitted default.
4. Same-avatar repeat inside 5 s — expect the picks returned normally,
   `listing_answered: true` (a settled first request clears its pending entry so the
   second call sends its own; an in-flight one suppresses our send, but its reply
   notifies every observer on that avatar id, so we receive it anyway). The failure
   being watched for is a bogus empty `picks` array with `listing_answered: true`,
   falsely claiming the avatar has no picks; `listing_answered: false` is an
   acceptable alternative outcome, not the expected one.
5. `getClassifieds` on an avatar with none — empty array, `listing_answered: true`.
6. Explicit `pick_ids` with one well-formed UUID that matches no existing pick — confirm
   it lands in `missing` and the call still returns the valid picks after the deadline
   rather than erroring. (A *malformed* string is silently dropped instead — it appears
   nowhere, and if it were the only id the call degrades to full discovery.)

## Also to update

- `MCP_TOOLS.md` — two rows in the Profile table (format: `| tool | params | description |`).
- The `firestorm-avatar` skill's tool table and its "Gotchas" section, which currently
  tells the agent that pick text is unreachable.
