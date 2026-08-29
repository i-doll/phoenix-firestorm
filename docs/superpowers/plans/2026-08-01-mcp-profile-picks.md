# profile.getPicks / profile.getClassifieds Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Add two MCP tools that read the full text of an avatar's picks and classifieds — description, snapshot, and location — which `profile.get` cannot reach.

**Architecture:** One shared two-stage waiter (list ids → fan out one legacy detail request per id → aggregate on a soft deadline) driven by a `mainloop` tick, plus one `LLAvatarPropertiesObserver` that demuxes replies by item id. Mirrors the existing `WornNameWait` pattern in `idmcptools_avatars.cpp`. All code goes in the existing `idmcptools_profile.cpp`.

**Tech Stack:** C++17, Firestorm viewer internals (`LLAvatarPropertiesProcessor`, `LLEventPumps` mainloop, `LLTimer`), `boost::json`, the in-repo `IDMCPToolRegistry`.

**Spec:** `docs/superpowers/specs/2026-08-01-mcp-profile-picks-design.md`

## Global Constraints

- **No unit-test harness exists for `idmcp*` code.** There is no test target covering any `idmcp*` translation unit — verified by grep over `indra/newview/CMakeLists.txt` and `indra/newview/tests/`. The MCP tools depend on live viewer singletons (`gAgent`, `LLAvatarPropertiesProcessor`, the mainloop pump) and cannot be exercised without a running, logged-in viewer. **This plan therefore substitutes a live-MCP verification matrix (Task 4) for the usual red-green unit cycle.** Do not fabricate a unit test that cannot run; do not skip Task 4.
- **Do not build this viewer.** The build needs special setup and belongs to Five. Per-task verification is careful reading plus the compile checklist in each task; the actual compile and live run happen in Task 4.
- **No new `.cpp` files.** Adding a source file forces a full PCH rebuild via autobuild+cmake. `idmcptools_profile.cpp` is already listed at `indra/newview/CMakeLists.txt:98`; no CMake change is needed by this plan.
- **Custom code carries the `ID` prefix** (`IDMCPDetailWait`, `idmcp_*`), per the fork convention. Never `FS`.
- **Main thread only.** Every function here runs on the viewer main thread; no locking, and no touching these globals from a coroutine.
- **`@showloc` is deliberately NOT gated.** Location fields always populate. This is a recorded decision, not an oversight — do not "fix" it.
- **Commit straight to the current `bot-*-master*` branch.** No feature branches.
- Conventional-commit messages. No Co-Authored-By or "Generated with" trailers.

> **Erratum (post-implementation).** Task 1 Step 8 below specifies an
> unconditional `sendAvatarPicksRequest` for pick discovery. That is the
> OpenSim-only path and returns nothing on Second Life. Review caught it during
> implementation; the shipped code branches on the `AgentProfile` capability
> (cap present → `sendAvatarPropertiesRequest` → `APT_PROPERTIES`, harvesting
> `LLAvatarData::picks_list`; cap absent → `sendAvatarPicksRequest` →
> `APT_PICKS`), and every listing branch opens with a `listing_answered` guard
> so a second listing reply cannot re-pend completed ids. See commit 246d2aa2f6
> and the corrected Flow section of the design spec. Steps 7-8 below are
> preserved as written for the record — do not execute them verbatim.

---

### Task 1: Shared detail waiter + `profile.getPicks`

**Files:**
- Modify: `indra/newview/idmcptools_profile.cpp` — add includes, the anonymous-namespace waiter/observer/serializers, and register one new tool in `idmcp_register_profile_tools`.

**Interfaces:**
- Consumes: existing file-local helpers `arg_str`, `looks_like_uuid`; existing `idmcp_tool_ok`, `IDMCPGateResult`, `IDMCPCallPtr`.
- Produces (Task 2 depends on all of these by exact name):
  - `enum class IDMCPDetailKind { Picks, Classifieds }`
  - `struct IDMCPDetailWait` and `using IDMCPDetailWaitPtr = std::shared_ptr<IDMCPDetailWait>`
  - `class IDMCPDetailObserver : public LLAvatarPropertiesObserver`
  - `void idmcp_detail_start(const boost::json::object& args, const IDMCPCallPtr& call, IDMCPDetailKind kind)`
  - `IDMCPGateResult idmcp_gate_target_shownames(const boost::json::object& args, IDMCPGatePhase)`
  - `boost::json::value idmcp_pos_to_json(const LLVector3d&)`

- [ ] **Step 1: Add the required includes**

At the top of `idmcptools_profile.cpp`, alongside the existing `#include "llavatarpropertiesprocessor.h"`, add:

```cpp
#include "llevents.h"       // LLEventPumps, LLTempBoundListener
#include "lltimer.h"        // LLTimer::getTotalSeconds
#include "v3dmath.h"        // LLVector3d

#include <set>
#include <vector>
```

- [ ] **Step 2: Extract the existing `@shownames` gate into a reusable helper**

`gate_profile_get` currently holds this logic inline. Inside the anonymous namespace, add the shared helper *above* `gate_profile_get`:

```cpp
    // Shared by profile.get / getPicks / getClassifieds: viewing another
    // avatar's profile data is gated on @shownames. Self and a missing/invalid
    // avatar_id are always allowed (invoke defaults those to self).
    IDMCPGateResult idmcp_gate_target_shownames(const boost::json::object& args, IDMCPGatePhase)
    {
        if (!IDMCPRlvGate::isEnabled())
        {
            return IDMCPGateResult();
        }
        const std::string spec = arg_str(args, "avatar_id");
        if (!looks_like_uuid(spec))
        {
            return IDMCPGateResult();
        }
        LLUUID id(spec);
        if (id == gAgent.getID())
        {
            return IDMCPGateResult();
        }
        if (!RlvActions::canShowName(RlvActions::SNC_DEFAULT, id))
        {
            IDMCPGateResult r;
            r.allowed   = false;
            r.behaviour = "shownames";
            return r;
        }
        return IDMCPGateResult();
    }
```

Then delete the now-duplicated `gate_profile_get` function entirely, and change the `profile.get` registration to use the helper. In `idmcp_register_profile_tools`, replace:

```cpp
        t.gate = gate_profile_get;
```

with:

```cpp
        t.gate = idmcp_gate_target_shownames;
```

- [ ] **Step 3: Add the JSON serializers**

Inside the anonymous namespace, after `profile_to_json`:

```cpp
    boost::json::value idmcp_pos_to_json(const LLVector3d& p)
    {
        boost::json::object o;
        o["x"] = p.mdV[VX];
        o["y"] = p.mdV[VY];
        o["z"] = p.mdV[VZ];
        return o;
    }

    boost::json::value pick_to_json(const LLPickData& d)
    {
        boost::json::object o;
        o["id"]            = d.pick_id.asString();
        o["name"]          = d.name;
        o["desc"]          = d.desc;
        o["snapshot_id"]   = d.snapshot_id.asString();
        o["parcel_id"]     = d.parcel_id.asString();
        o["sim_name"]      = d.sim_name;
        o["pos_global"]    = idmcp_pos_to_json(d.pos_global);
        o["sort_order"]    = d.sort_order;
        o["enabled"]       = d.enabled;
        o["original_name"] = d.original_name;
        o["user_name"]     = d.user_name;
        return o;
    }
```

- [ ] **Step 4: Add the waiter struct and its globals**

Inside the anonymous namespace. Read the comment carefully — it records the four viewer constraints that dictate this shape:

```cpp
    // ---- picks / classifieds detail fan-out ---------------------------------
    //
    // Descriptions are NOT in the AgentProfile cap - it fills picks_list with
    // (id, name) only - so each item needs its own legacy round-trip
    // (pickinforequest / ClassifiedInfoRequest). Four constraints shape this:
    //
    //  1. Detail replies dispatch on the OWNER's id, not the item id, so a
    //     fan-out over N items lands N replies on one observer; we demux on
    //     pick_id / classified_id.
    //  2. Detail requests are NOT pending-tracked by the processor, so N
    //     concurrent requests are not deduped down to one. (The LISTING
    //     requests ARE tracked, with a 5s window - hence listing_answered,
    //     which distinguishes "no items" from "listing never replied".)
    //  3. APT_CLASSIFIED_INFO notifies twice (creator_id and classified_id);
    //     we register on the owner only, so we see it once. The demux erase is
    //     idempotent regardless.
    //  4. IDMCPCall::setDeadline would make the server respond -32006 and
    //     DISCARD everything collected. Wrong for a fan-out: 4 of 5 replies
    //     should return 4 items. So the deadline is soft and owned here, and a
    //     partial result is a success carrying `missing`.
    //
    // Main-thread only.
    enum class IDMCPDetailKind { Picks, Classifieds };

    class IDMCPDetailObserver;

    struct IDMCPDetailWait
    {
        IDMCPCallPtr         call;
        LLUUID               target;
        IDMCPDetailKind      kind = IDMCPDetailKind::Picks;
        std::set<LLUUID>     pending;             // item ids awaiting a detail reply
        boost::json::array   out;                 // completed item objects
        bool                 listing_answered = false;
        bool                 awaiting_listing = true;
        F64                  deadline = 0.0;
        bool                 done = false;
        IDMCPDetailObserver* obs = nullptr;       // owned; reaped by the tick
    };

    using IDMCPDetailWaitPtr = std::shared_ptr<IDMCPDetailWait>;

    std::vector<IDMCPDetailWaitPtr> g_detail_waits;
    bool                            g_detail_tick_on = false;
    LLTempBoundListener             g_detail_tick;
```

- [ ] **Step 5: Add finish / reap / tick**

Inside the anonymous namespace, after the struct. Note that `idmcp_detail_finish` **must not** delete the observer — see the comment; that is the use-after-free this split avoids:

```cpp
    // Responds and marks done. Deliberately does NOT tear down the observer:
    // this is reachable from inside IDMCPDetailObserver::processProperties, and
    // deleting the observer there would destroy the very shared_ptr member the
    // caller is still holding. Teardown is deferred to idmcp_detail_tick.
    void idmcp_detail_finish(const IDMCPDetailWaitPtr& w)
    {
        if (w->done)
        {
            return;
        }
        w->done = true;   // set BEFORE responding: the call's cleanup re-enters here

        boost::json::array missing;
        for (const LLUUID& id : w->pending)
        {
            missing.push_back(boost::json::value(id.asString()));
        }

        boost::json::object o;
        o["avatar_id"] = w->target.asString();
        o[(w->kind == IDMCPDetailKind::Picks) ? "picks" : "classifieds"] = std::move(w->out);
        o["missing"]          = std::move(missing);
        o["listing_answered"] = w->listing_answered;
        idmcp_tool_ok(w->call, o);
    }

    void idmcp_detail_reap(const IDMCPDetailWaitPtr& w)
    {
        if (w->obs)
        {
            w->obs->detach();
            delete w->obs;
            w->obs = nullptr;
        }
    }

    void idmcp_detail_tick()
    {
        if (g_detail_waits.empty())
        {
            return;
        }
        const F64 now = LLTimer::getTotalSeconds();
        for (auto it = g_detail_waits.begin(); it != g_detail_waits.end(); )
        {
            const IDMCPDetailWaitPtr& w = *it;
            if (!w->done && now >= w->deadline)
            {
                idmcp_detail_finish(w);   // partial result is still a success
            }
            if (w->done)
            {
                idmcp_detail_reap(w);
                it = g_detail_waits.erase(it);
            }
            else
            {
                ++it;
            }
        }
    }

    void idmcp_detail_ensure_tick()
    {
        if (g_detail_tick_on)
        {
            return;
        }
        g_detail_tick = LLEventPumps::instance().obtain("mainloop").listen(
            "idmcp_profile_details", [](const LLSD&) -> bool { idmcp_detail_tick(); return false; });
        g_detail_tick_on = true;
    }
```

- [ ] **Step 6: Add the detail-request fan-out**

Inside the anonymous namespace:

```cpp
    // Copies `pending` before iterating: cheap, and it removes any chance of
    // iterating a set that a reply mutates.
    void idmcp_detail_request_items(const IDMCPDetailWaitPtr& w)
    {
        const std::set<LLUUID> ids = w->pending;
        LLAvatarPropertiesProcessor* proc = LLAvatarPropertiesProcessor::getInstance();
        for (const LLUUID& id : ids)
        {
            if (w->kind == IDMCPDetailKind::Picks)
            {
                // Must pass the owner id: the pick database is sharded by creator.
                proc->sendPickInfoRequest(w->target, id);
            }
            else
            {
                proc->sendClassifiedInfoRequest(id);
            }
        }
    }
```

- [ ] **Step 7: Add the observer**

Inside the anonymous namespace, after `idmcp_detail_request_items`:

```cpp
    class IDMCPDetailObserver : public LLAvatarPropertiesObserver
    {
    public:
        IDMCPDetailObserver(IDMCPDetailWaitPtr w, const LLUUID& target)
            : mWait(std::move(w)), mTarget(target) {}

        void processProperties(void* data, EAvatarProcessorType type) override
        {
            if (!mWait || mWait->done || !data)
            {
                return;
            }
            const bool picks = (mWait->kind == IDMCPDetailKind::Picks);

            if (picks && type == APT_PICKS)
            {
                const LLAvatarPicks* p = static_cast<LLAvatarPicks*>(data);
                mWait->listing_answered = true;
                mWait->awaiting_listing = false;
                for (const auto& entry : p->picks_list)
                {
                    mWait->pending.insert(entry.first);
                }
                idmcp_detail_request_items(mWait);
            }
            else if (!picks && type == APT_CLASSIFIEDS)
            {
                const LLAvatarClassifieds* c = static_cast<LLAvatarClassifieds*>(data);
                mWait->listing_answered = true;
                mWait->awaiting_listing = false;
                for (const auto& entry : c->classifieds_list)
                {
                    mWait->pending.insert(entry.classified_id);
                }
                idmcp_detail_request_items(mWait);
            }
            else if (picks && type == APT_PICK_INFO)
            {
                const LLPickData* d = static_cast<LLPickData*>(data);
                if (mWait->pending.erase(d->pick_id))
                {
                    mWait->out.push_back(pick_to_json(*d));
                }
            }
            else if (!picks && type == APT_CLASSIFIED_INFO)
            {
                const LLAvatarClassifiedInfo* d = static_cast<LLAvatarClassifiedInfo*>(data);
                if (mWait->pending.erase(d->classified_id))
                {
                    mWait->out.push_back(classified_to_json(*d));
                }
            }
            else
            {
                return;   // not ours (e.g. a concurrent APT_PROPERTIES fetch)
            }

            if (!mWait->awaiting_listing && mWait->pending.empty())
            {
                idmcp_detail_finish(mWait);
            }
        }

        void detach()
        {
            if (mDetached)
            {
                return;
            }
            mDetached = true;
            LLAvatarPropertiesProcessor::getInstance()->removeObserver(mTarget, this);
        }

    private:
        IDMCPDetailWaitPtr mWait;
        LLUUID             mTarget;
        bool               mDetached = false;
    };
```

**Note for Task 2:** `classified_to_json` is referenced here but defined in Task 2. Until Task 2 lands, this file will not compile. That is expected and is why Task 1 and Task 2 share a single build in Task 4 — do **not** stub it.

- [ ] **Step 8: Add the shared call starter**

Inside the anonymous namespace, after the observer:

```cpp
    void idmcp_detail_start(const boost::json::object& args, const IDMCPCallPtr& call,
                            IDMCPDetailKind kind)
    {
        const char* ids_key = (kind == IDMCPDetailKind::Picks) ? "pick_ids" : "classified_ids";
        const std::string spec = arg_str(args, "avatar_id");
        const LLUUID target = looks_like_uuid(spec) ? LLUUID(spec) : gAgent.getID();

        auto w = std::make_shared<IDMCPDetailWait>();
        w->call     = call;
        w->target   = target;
        w->kind     = kind;
        w->deadline = LLTimer::getTotalSeconds() + 10.0;

        // Explicit ids skip the listing stage entirely.
        auto it = args.find(ids_key);
        if (it != args.end() && it->value().is_array())
        {
            for (const auto& v : it->value().as_array())
            {
                if (!v.is_string())
                {
                    continue;
                }
                const std::string s = v.as_string().c_str();
                if (looks_like_uuid(s))
                {
                    w->pending.insert(LLUUID(s));
                }
            }
        }
        const bool explicit_ids = !w->pending.empty();
        w->awaiting_listing = !explicit_ids;
        w->listing_answered = explicit_ids;   // no listing was needed

        w->obs = new IDMCPDetailObserver(w, target);
        LLAvatarPropertiesProcessor::getInstance()->addObserver(target, w->obs);
        g_detail_waits.push_back(w);
        idmcp_detail_ensure_tick();

        // weak_ptr, NOT a copy of w: the wait holds `call`, so capturing w
        // strongly here would create a shared_ptr cycle and leak both.
        std::weak_ptr<IDMCPDetailWait> weak = w;
        call->setCleanup([weak]()
        {
            if (auto p = weak.lock())
            {
                p->done = true;   // idmcp_detail_tick reaps the observer next frame
            }
        });

        if (explicit_ids)
        {
            idmcp_detail_request_items(w);
        }
        else if (kind == IDMCPDetailKind::Picks)
        {
            LLAvatarPropertiesProcessor::getInstance()->sendAvatarPicksRequest(target);
        }
        else
        {
            LLAvatarPropertiesProcessor::getInstance()->sendAvatarClassifiedsRequest(target);
        }
    }
```

- [ ] **Step 9: Register `profile.getPicks`**

In `idmcp_register_profile_tools`, after the `profile.get` block:

```cpp
    // profile.getPicks -------------------------------------------------------
    {
        IDMCPTool t;
        t.name = "profile.getPicks";
        t.description =
            "Read the FULL TEXT of an avatar's profile picks - description, snapshot "
            "and location - which profile.get cannot return (it gives ids and names "
            "only). Optional {\"avatar_id\"} defaults to yourself; optional "
            "{\"pick_ids\"} (array of UUIDs) reads just those and skips discovery. "
            "Each pick needs its own round-trip, so results can be PARTIAL: any id "
            "that did not answer within ~10s is listed in \"missing\". "
            "\"listing_answered\" is false if the pick list itself never came back "
            "(distinguishing that from an avatar who simply has no picks). Picks are "
            "often used as prose - a serialized backstory, or a captioned gallery - so "
            "read \"desc\" and order by \"sort_order\". Blocked by RLV @shownames.";
        t.input_schema = boost::json::parse(
            R"({"type":"object","properties":{"avatar_id":{"type":"string"},"pick_ids":{"type":"array","items":{"type":"string"}}},"additionalProperties":false})");
        t.gate = idmcp_gate_target_shownames;
        t.invoke = [](const boost::json::object& args, const IDMCPCallPtr& call)
        {
            idmcp_detail_start(args, call, IDMCPDetailKind::Picks);
        };
        reg.add(std::move(t));
    }
```

- [ ] **Step 10: Compile checklist (read, do not build)**

Confirm each by eye before committing:
- `VX`/`VY`/`VZ` and `LLVector3d::mdV` are reachable — `v3dmath.h` is included (Step 1).
- `LLAvatarPicks`, `LLAvatarClassifieds`, `LLPickData`, `LLAvatarClassifiedInfo`, and the `APT_*` enumerators all come from the already-included `llavatarpropertiesprocessor.h`.
- `IDMCPDetailObserver` is forward-declared (Step 4) before `IDMCPDetailWait` names it as a pointer, and defined (Step 7) before `idmcp_detail_reap` calls `detach()`. **`idmcp_detail_reap` therefore must appear after the class** — if you followed step order it does not. Move `idmcp_detail_reap` and `idmcp_detail_tick` to sit *after* the observer class, or forward-declare `void idmcp_detail_reap(const IDMCPDetailWaitPtr&);` above them. Pick one and make it so.
- `idmcp_detail_finish` is declared before `IDMCPDetailObserver::processProperties` calls it. ✔ (Step 5 precedes Step 7.)
- `idmcp_detail_request_items` precedes the observer. ✔ (Step 6 precedes Step 7.)
- `boost::json::object::operator[]` accepts the `const char*` from the ternary in `idmcp_detail_finish`. ✔
- `o["sort_order"] = d.sort_order;` — `S32` converts to `boost::json::value` via `std::int64_t`. ✔
- `gate_profile_get` has no remaining references anywhere in the file.

- [ ] **Step 11: Commit**

```bash
git add indra/newview/idmcptools_profile.cpp
git commit -m "feat: profile.getPicks - read full pick text via a shared detail waiter"
```

---

### Task 2: `profile.getClassifieds`

**Files:**
- Modify: `indra/newview/idmcptools_profile.cpp`

**Interfaces:**
- Consumes from Task 1: `IDMCPDetailKind`, `idmcp_detail_start`, `idmcp_gate_target_shownames`, `idmcp_pos_to_json`.
- Produces: `boost::json::value classified_to_json(const LLAvatarClassifiedInfo&)` — resolves the forward reference left by Task 1 Step 7.

- [ ] **Step 1: Add the classifieds serializer**

Inside the anonymous namespace, directly after `pick_to_json` (it must precede the observer that calls it):

```cpp
    boost::json::value classified_to_json(const LLAvatarClassifiedInfo& d)
    {
        boost::json::object o;
        o["id"]                = d.classified_id.asString();
        o["name"]              = d.name;
        o["description"]       = d.description;
        o["snapshot_id"]       = d.snapshot_id.asString();
        o["parcel_id"]         = d.parcel_id.asString();
        o["parcel_name"]       = d.parcel_name;
        o["sim_name"]          = d.sim_name;
        o["pos_global"]        = idmcp_pos_to_json(d.pos_global);
        o["category"]          = static_cast<std::uint64_t>(d.category);
        o["creation_date"]     = static_cast<std::uint64_t>(d.creation_date);
        o["expiration_date"]   = static_cast<std::uint64_t>(d.expiration_date);
        o["flags"]             = static_cast<std::uint64_t>(d.flags);
        o["price_for_listing"] = d.price_for_listing;
        return o;
    }
```

The `static_cast<std::uint64_t>` on the `U32`/`U8` fields is deliberate: `boost::json::value` has no `U32` overload, and an unqualified `U8` would otherwise be ambiguous.

- [ ] **Step 2: Register `profile.getClassifieds`**

In `idmcp_register_profile_tools`, after the `profile.getPicks` block:

```cpp
    // profile.getClassifieds -------------------------------------------------
    {
        IDMCPTool t;
        t.name = "profile.getClassifieds";
        t.description =
            "Read an avatar's classified ads in full - description, snapshot, location, "
            "category, price and expiry. profile.get does not list classifieds at all, "
            "so this tool discovers them itself. Optional {\"avatar_id\"} defaults to "
            "yourself; optional {\"classified_ids\"} (array of UUIDs) reads just those "
            "and skips discovery. Each ad needs its own round-trip, so results can be "
            "PARTIAL: ids that did not answer within ~10s appear in \"missing\". "
            "\"listing_answered\" is false if the list itself never came back, as "
            "opposed to the avatar having no classifieds. Blocked by RLV @shownames.";
        t.input_schema = boost::json::parse(
            R"({"type":"object","properties":{"avatar_id":{"type":"string"},"classified_ids":{"type":"array","items":{"type":"string"}}},"additionalProperties":false})");
        t.gate = idmcp_gate_target_shownames;
        t.invoke = [](const boost::json::object& args, const IDMCPCallPtr& call)
        {
            idmcp_detail_start(args, call, IDMCPDetailKind::Classifieds);
        };
        reg.add(std::move(t));
    }
```

- [ ] **Step 3: Compile checklist (read, do not build)**

- `classified_to_json` is defined *before* `IDMCPDetailObserver` uses it. ✔
- `<cstdint>` for `std::uint64_t` — already reachable via `boost/json.hpp`; add `#include <cstdint>` if your reading says otherwise.
- Both new tools appear inside `idmcp_register_profile_tools`, not after its closing brace.
- No remaining reference to any symbol that no task defines.

- [ ] **Step 4: Commit**

```bash
git add indra/newview/idmcptools_profile.cpp
git commit -m "feat: profile.getClassifieds - read classified ads in full"
```

---

### Task 3: Documentation

**Files:**
- Modify: `MCP_TOOLS.md` (Profile table, currently lines 95-96)
- Modify: `/home/thea/.claude/skills/firestorm-avatar/SKILL.md` (tool table + Gotchas)

**Interfaces:** none — docs only.

- [ ] **Step 1: Add both tools to `MCP_TOOLS.md`**

The Profile table format is `| tool | params | description |` with `<br>`-separated params. Insert after the `profile.get` row (line 95), before `profile.setSelf`:

```markdown
| `profile.getPicks` | `avatar_id`: string<br>`pick_ids`: string[] | Read the full text of an avatar's picks — `desc`, `snapshot_id`, `parcel_id`, `sim_name`, `pos_global`, `sort_order`. `profile.get` returns pick ids and names only. Omit `pick_ids` to discover them. Results may be partial: unanswered ids land in `missing`, and `listing_answered: false` means the pick list never came back (as opposed to the avatar having none). Blocked by RLV @shownames. |
| `profile.getClassifieds` | `avatar_id`: string<br>`classified_ids`: string[] | Read an avatar's classified ads in full — `description`, `snapshot_id`, `parcel_name`, `sim_name`, `pos_global`, `category`, `creation_date`, `expiration_date`, `price_for_listing`. `profile.get` does not list classifieds at all. Same `missing` / `listing_answered` semantics as `profile.getPicks`. Blocked by RLV @shownames. |
```

- [ ] **Step 2: Update the `firestorm-avatar` skill's tool table**

In `/home/thea/.claude/skills/firestorm-avatar/SKILL.md`, the Profile row currently reads:

```markdown
| Profile | `profile.get`/`setSelf`, `people.getNames` | View/edit profiles, resolve names |
```

Replace with:

```markdown
| Profile | `profile.get`/`setSelf`, `profile.getPicks`/`getClassifieds`, `people.getNames` | View/edit profiles, read picks & classified ads in full, resolve names |
```

- [ ] **Step 3: Replace the stale "picks are unreachable" guidance**

The Gotchas section must stop telling the agent that pick text can't be read. Add this bullet to Gotchas:

```markdown
- **Picks and classifieds need their own call.** `profile.get` returns pick ids and names
  only, and no classifieds at all — descriptions live behind a per-item round-trip. Use
  `profile.getPicks` / `profile.getClassifieds` to read the actual text. Residents often
  use picks as prose (a backstory split across `part 1`..`part 5`, or a captioned gallery),
  so titles alone tell you almost nothing — read `desc` and order by `sort_order`. Both
  tools can return PARTIAL results: check `missing` for ids that didn't answer, and treat
  `listing_answered: false` as "the list never arrived", not "they have none".
```

- [ ] **Step 4: Commit**

The skill file lives outside this repo, so only the repo doc is committed here.

```bash
git add MCP_TOOLS.md
git commit -m "docs: document profile.getPicks and profile.getClassifieds"
```

---

### Task 4: Build handoff and live verification

**Files:** none modified unless a defect is found.

**Interfaces:** exercises the tools registered in Tasks 1-2.

This task replaces the unit-test cycle that this codebase cannot support (see Global Constraints). It cannot be completed by an agent alone: **Five builds the viewer.**

- [ ] **Step 1: Hand off the build**

Ask Five to build and relaunch with `IDMCPServerEnabled = 1`. Report that Tasks 1-3 are committed and that `idmcptools_profile.cpp` is the only source file touched, so no CMake regeneration is needed. Fix any compiler diagnostics Five reports, then re-hand off.

- [ ] **Step 2: Confirm the tools are registered**

Call `tools/list` (or any MCP call that surfaces the tool set) and confirm `profile.getPicks` and `profile.getClassifieds` are present with their schemas.
Expected: both listed; `pick_ids` / `classified_ids` typed as string arrays.

- [ ] **Step 3: Run the verification matrix**

| # | Call | Expected |
|---|---|---|
| 1 | `getPicks` on a nearby avatar whose pick titles are numbered parts of one text | All picks returned, each with non-empty `desc`; `missing` empty; `listing_answered: true`. Exercises the multi-reply demux. Confirm `sort_order` puts the parts in the author's order. |
| 2 | `getPicks` on a second avatar with a different pick count | Correct count. Guards against a stale `pending` set leaking between calls. |
| 3 | `getPicks` with no `avatar_id` | Returns your own picks. |
| 4 | `getPicks` twice on one avatar within 5 s | Returns the picks normally — `listing_answered: true` (a settled first request clears its pending entry so the second call sends its own; an in-flight one suppresses our send, but its reply notifies every observer on that avatar id, so we receive it anyway). The failure being watched for is a bogus empty `picks` list with `listing_answered: true`, as if the avatar had none; `listing_answered: false` is an acceptable alternative outcome, not the expected one. This is the `sendGenericRequest` dedup path. |
| 5 | `getPicks` with explicit `pick_ids` including one well-formed UUID that matches no existing pick | Valid picks returned; the nonexistent id in `missing`; call succeeds after ~10 s rather than erroring. (A *malformed* string is silently dropped instead — it appears nowhere, and if it were the only id the call degrades to full discovery.) |
| 6 | `getClassifieds` on an avatar with none | `classifieds: []`, `missing: []`, `listing_answered: true`. |
| 7 | `getClassifieds` on an avatar with at least one ad | Full fields populated, including `price_for_listing` and both dates. |
| 8 | `getPicks` on an avatar while `@shownames` restricts them | Fails `-32011` naming `shownames` and its source object. |

- [ ] **Step 4: Confirm no leak or stall after the run**

Have Five leave the viewer running a minute after the last call and confirm no repeating log spew and no frame-rate change — evidence the mainloop tick drained `g_detail_waits` rather than spinning on a wait that never reaps.

- [ ] **Step 5: Report results honestly**

State per-row pass/fail with the actual output. If a row fails, stop and diagnose rather than declaring the feature done. Only claim completion when rows 1-8 pass.

---

## Self-Review

**Spec coverage:**
- Shared two-stage waiter → Task 1 Steps 4-8. ✔
- `creator_id` demux → Task 1 Step 7. ✔
- Soft deadline instead of `setDeadline` → Task 1 Step 5 + its comment. ✔
- `listing_answered` for the listing-dedup window → Task 1 Steps 4/5/8, verified by matrix row 4. ✔
- Classifieds double-notify → handled by registering on the owner only, recorded in the Step 4 comment. ✔
- Tool surface and field lists → Task 1 Step 9, Task 2 Steps 1-2. ✔
- `@shownames` gate extracted and shared → Task 1 Step 2. ✔
- `@showloc` deliberately ungated → Global Constraints, with a do-not-fix note. ✔
- `@showsearch` not consulted → no gate added; nothing to do. ✔
- Decided against touching `profile.get` for classifieds → no task does. ✔
- `MCP_TOOLS.md` + skill update → Task 3. ✔
- Live verification matrix → Task 4, covering all six spec test cases plus the RLV case. ✔

**Placeholder scan:** no TBD/TODO; every code step carries real code; the one intentional cross-task forward reference (`classified_to_json`) is called out explicitly in both directions rather than stubbed.

**Type consistency:** `IDMCPDetailWaitPtr`, `idmcp_detail_start`, `idmcp_detail_finish`, `idmcp_detail_reap`, `idmcp_detail_tick`, `idmcp_detail_ensure_tick`, `idmcp_detail_request_items`, `idmcp_gate_target_shownames`, `idmcp_pos_to_json`, `pick_to_json`, `classified_to_json` — each spelled identically at definition and every call site. `IDMCPDetailKind::Picks` / `::Classifieds` used consistently. Output keys (`picks`, `classifieds`, `missing`, `listing_answered`, `avatar_id`) match between the serializers, `idmcp_detail_finish`, the tool descriptions, `MCP_TOOLS.md`, and the verification matrix.

**One ordering defect found and left as an explicit step rather than silently reshuffled:** following Steps 4-7 in order puts `idmcp_detail_reap` (which calls `IDMCPDetailObserver::detach()`) before the class definition. Task 1 Step 10 names this and requires the implementer to resolve it one of two stated ways.
