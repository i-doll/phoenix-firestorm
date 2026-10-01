# MCP Object Build Tools Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Give MCP agents eleven `object.*` tools that close the build loop: rez or attach an item, read and edit the object, texture its faces, fill its contents, set next-owner permissions, link and unlink, and take it back into inventory.

**Architecture:** Selection-free throughout. Every edit is packed by hand from the `LLViewerObject` (the same messages the Build floater sends), and properties come from a raw `ObjectSelect`/`ObjectDeselect` pair, so `LLSelectMgr`'s selection and the Build floater are never touched. A shared header, `idmcptools_object.h`, holds targeting, RLV, message, properties and task-inventory helpers plus a `Job` base class: one idle driver ticks every deferred object tool until the region echoes the change or a deadline passes.

**Tech Stack:** C++17, Firestorm viewer internals (`LLViewerObject`, `LLVOInventoryListener`, `LLToolDragAndDrop` public statics, `LLAttachmentsMgr`, `LLGLTFMaterialList`, `LLInventoryModel`, `LLMessageSystem`), RLVa (`RlvActions`, `gRlvAttachmentLocks`), `boost::json`, the in-repo `IDMCPToolRegistry`.

**Spec:** `docs/superpowers/specs/2026-10-01-mcp-object-build-tools-design.md`. API facts verified against this tree are in the scratchpad file `object-api-facts.md` that the controller holds. Where the plan and the spec differ, the plan follows the code, and every difference is listed under "Spec corrections".

## Spec corrections

Each line names what the spec says, what the code does, and what this plan does.

1. **`objects.getNearby` sends no `ObjectDeselect`** (idmcptools_avatars.cpp:499-518). The shared properties request sends `ObjectDeselect` itself when a job ends. It skips any object the user has selected (`LLViewerObject::isSelected()`), because deselecting it would clear the user's selection on the region.
2. **`idmcp::onObjectProperties` already exists** (declared in idmcp.h, defined at idmcptools_avatars.cpp:165). The new `idmcp_obj::onObjectProperties` is called from the first line of that definition, before its `g_worn_waits.empty()` early return. No second definition and no upstream edit.
3. **`LLViewerJointAttachment::getAttachedObject` matches an inventory item id**, not an object id. `object.attach` confirms with `LLVOAvatarSelf::getWornAttachment(item_id)` and `getWornAttachmentPoint(item_id)`.
4. **HUD attachment point names have no "HUD " prefix** ("Center", "Top Left", "Center 2"). Point matching is case-insensitive against the internal name, and also accepts "HUD " + name for HUD points, so the spec's "HUD Center" works.
5. **`rez_attachment(item, pt, replace=true)` opens a modal "ReplaceAttachment" dialog** when the point is occupied, and fails silently. `object.attach` calls `LLAttachmentsMgr::instance().addAttachmentRequest(item_id, pt, !replace)` after making `rez_attachment`'s own checks: `rlvPredCanWearItem`, `gRlvAttachmentLocks.canAttach(pt)`, the duplicate-wear check and `canAttachMoreObjects()`.
6. **`ObjectLink` makes the first local id the root** (llselectmgr.cpp:6022-6027). This matches the spec. `sendLocalIds` repeats that id at the start of every packet, as `sendListToRegions` does.
7. **`ObjectName` and `ObjectDescription` use the field `LocalID`**, not `ObjectLocalID`.
8. **`isRoot()` is false for a worn attachment's root**, because its parent is the avatar. Everything uses `isRootEdit()` and `getRootEdit()`.
9. **`mOwnerID` is usually null.** Ownership checks use `permYouOwner()`.
10. **`setTEColor(te, LLColor3)` sets alpha to 1.** `object.setFaces` builds an `LLColor4` from the face's current color and changes only the channels the caller gave.
11. **`requestInventory()` can call back before it returns.** `ContentsFetch` registers its listener first and only records results in the callback. Jobs read them on the next idle tick.
12. **The modify check needs no properties request.** `permModify()` reads `FLAGS_OBJECT_MODIFY`, which arrives with every `ObjectUpdate` (`loadFlags`, llviewerobject.cpp:1985), so the spec's "permissions haven't arrived" case does not occur.
13. **Edits are not applied locally first.** The spec says to call `setPositionEdit`/`setRotation`/`setScale` before sending, as the floater does. That would make the echo wait match at once, without the region. The plan packs the requested values straight into `MultipleObjectUpdate`, and the object moves when the region's update arrives.
14. **Position and rotation frames.** The spec gives region coordinates for a root and root-relative values for a child, and says nothing about worn objects. The plan reads and writes the frame the region stores (`getPosition()`/`getRotation()`): region for a rezzed root, attachment-point-relative for a worn root, root-relative for a child.
15. **Scale without `link`.** `UPD_SCALE` with `UPD_LINKED_SETS` would scale the linkset. The plan sends position and rotation in one entry with `UPD_LINKED_SETS`, and scale in a second entry without it, so only the root prim scales.
16. **`dropInventory` and `dropScript` are not called.** They return silently while the user is mid-drag (`mSource` check), and `dropInventory` switches the open Build floater to its Contents tab. `object.addContents` checks `LLToolDragAndDrop::isInventoryDropAcceptable` (the public wrapper of the protected `willObjectAcceptInventory`) and then does their core itself: copy the item, delete a no-copy original from local inventory, and call `updateInventory` or `saveScript`. No friend declarations are needed anywhere.
17. **`dropTextureOneFace` and `dropMaterialOneFace` are not called.** They return void, send one `sendTEUpdate` per face, and would rerun the drop rules per face against an item that a no-copy drop has already deleted. `object.setFaces` calls the public `handleDropMaterialProtections` once per prim, then `setRenderMaterialID`/`setTEImage` per face and one `sendTEUpdate()` per prim. A texture clears the face's PBR material for raw asset ids too, so the spec's step 4 repeat with a raw UUID shows the texture.
18. **Drag-and-drop always drops contents into the root.** With `link`, `object.contents` and `object.addContents` target the named prim directly, as the Build floater's Contents tab does in "Edit linked" mode.
19. **Duplicate names in contents.** The region renames an added item whose name already exists. `object.addContents` confirms additions by counting items per asset type before and after, not by name.
20. **Next owner needs copy or transfer.** `LLPermissions::setNextOwnerBits` forces transfer on when copy is off. `object.setPermissions` rejects `copy:false, transfer:false` with `INVALID_PARAMS` instead of silently granting transfer.
21. **Take rules.** The spec only says a non-owner take is refused. The plan also requires ownership for `copy:true`, refuses `copy:true` on a no-copy object, refuses a pathfinding-permanent object for a plain take, and refuses worn attachments (they are detached with `appearance.detachItems`). The RLV gate adds `handle_take_copy`'s rule: no taking the object you sit on while `@unsit` is active.
22. **Link and unlink refuse worn attachments**, as the Build floater does. Unlink first switches any modifiable target prim with physics shape "None" to convex hull, as `LLSelectMgr::sendDelink` does.
23. **`object.get` answers with partial data** after its 10 s properties wait, listing the links that got no reply in `incomplete_links`, instead of failing a whole 200-prim linkset with `TIMEOUT`.
24. **Library items.** Textures and materials from the Library are allowed (the drop rules treat `SOURCE_LIBRARY` as always acceptable). Rez, attach and addContents refuse Library items, as the spec says for rez.

## Global Constraints

- **Do not compile.** This viewer uses a non-standard build that the user runs. Each task ends with a compile-read checklist naming the headers and lines to read. The user builds after Task 5, and Task 7's live matrix runs over MCP after that build.
- **No unit-test harness exists for `idmcp*` code.** Do not invent tests. Task 7's live matrix replaces the red-green cycle, and every Review Focus line has a row there.
- **Every commit must build on its own.** Each task adds its own source file, its registration declaration, its `initSingleton` call and its CMake lines in the same commit.
- **No identifier may be an X11 macro name.** The Linux PCH includes `Xlib.h`. Never use `None, Bool, Status, True, False, Success, Always, Above, Below, Opposite, CurrentTime, NoValue, AllValues, XValue, YValue, Complex, Convex, KeyPress, KeyRelease, ButtonPress, Expose, FocusIn, FocusOut, DestroyNotify, ShiftMask, LockMask, ControlMask, Button1`..`Button5, GrayScale, Unsorted` as an enumerator, variable, function or type name. `Pending` is safe.
- **Custom code carries the `ID` prefix** in file names, `@brief <ID>` headers and CMake comments (`# <ID> Embedded MCP server: ...`). This plan edits no upstream file, so no `// <ID>` trailing comment is needed. If an implementer finds they must touch an upstream file, mark the edit with a trailing `// <ID> ...` comment and tell the controller. No friend declarations.
- **Shared helpers live in `namespace idmcp_obj`** in `idmcptools_object.h/.cpp`, so they never clash with the anonymous-namespace `arg_str`/`looks_like_uuid` copies in the other `idmcptools_*.cpp` files.
- **Main thread only.** Every deferred tool is an `idmcp_obj::Job`. The job holds its `IDMCPCallPtr` strongly (the server tracks calls weakly), the call's cleanup holds the job through a `weak_ptr`, the idle driver ticks a local copy of the job list, and a job sets `mFinished` before it answers.
- **Every deferred tool sets `t.timeout` above its own deadline**: get 25 s (job 15 s), edit 20 s (10 s), setPermissions 45 s (30 s), link and unlink 25 s (15 s), contents 25 s (15 s), addContents 60 s (45 s), rez, attach and take 45 s (30 s).
- **Cancel on disconnect.** `Job::drive` checks `mCall->connected()` every tick and finishes the job without answering when the client is gone.
- **RLV at the commit point.** Tools that send in `invoke` (edit, setFaces, setPermissions for the object, link, unlink, rez, attach) send in the same frame as the request gate, so that gate is the commit check. Tools that send after a wait re-check with `Job::failIfDenied` right before sending: the contents step of setPermissions, the adding step of addContents, the `DeRezObject` of take, and the rotation that rez applies to a new object.
- **Selection-free.** Never call `LLSelectMgr` select, deselect or send APIs. Including `llselectmgr.h` for the `UPD_*` constants and `EDeRezDestination` is fine.
- **Conventional commits.** No `Co-Authored-By` or "Generated with" trailers.
- **Branch:** `feat/mcp-object-build-tools` (already checked out).

## Review Focus

1. **The user has the same object selected in the Build floater while a tool reads it.** Expect their selection to stay, and the region to keep it selected: the job must not send `ObjectDeselect` for a user-selected object. Task 1's `Job::finish` owns this; Task 7 rows 11 and 12.
2. **An avatar sits on the linkset.** Seated avatars are children of the root. Expect `object.get` to count only prims, and link numbers to stay stable. Task 1's `linksOf` owns this; Task 7 row 13.
3. **`object.addContents` with an item whose name already exists in the object.** The region renames the copy. Expect `added`, not `not_found`. Task 4's per-type counting owns this; Task 7 row 14.
4. **A no-copy texture applied to `faces:"all"` on a two-prim linkset.** Expect link 1 to get it (the item moves into its contents) and link 2 to report the item as used up, with no crash from the deleted item. Task 3's `prepare_asset` owns this; Task 7 row 15.
5. **The MCP client disconnects while a job waits** (a take, or a rez that never appears). Expect no crash, the raw selection released, and later calls working. Task 1's `Job::drive` owns this; Task 7 row 16.

## File Structure

| File | Responsibility |
|---|---|
| `indra/newview/idmcptools_object.h` (create, Task 1) | `namespace idmcp_obj`: argument and JSON helpers, link numbering and targeting, RLV edit check, message senders, echo tolerances, `Props` and the properties hook, `ContentsFetch`, the `Job` base. |
| `indra/newview/idmcptools_object.cpp` (create, Task 1; extend, Task 2) | The helpers' definitions, the idle driver, `object.get` (Task 1), `object.edit`, `object.setPermissions`, `object.link`, `object.unlink` (Task 2). |
| `indra/newview/idmcptools_faces.cpp` (create, Task 3) | `object.setFaces`. |
| `indra/newview/idmcptools_contents.cpp` (create, Task 4) | `object.contents`, `object.addContents`. |
| `indra/newview/idmcptools_rez.cpp` (create, Task 5) | `object.rez`, `object.attach`, `object.take`. |
| `indra/newview/idmcptools_avatars.cpp` (modify, Task 1) | Chain `idmcp_obj::onObjectProperties` from the existing `idmcp::onObjectProperties`. |
| `indra/newview/idmcptools.h`, `idmcpserver.cpp`, `CMakeLists.txt` (modify, Tasks 1, 3, 4, 5) | Registration declarations, `initSingleton` calls, source and header entries. |
| `MCP_TOOLS.md` (modify, Task 6) | Sync from the plugin's copy, then add an "Objects" section. |

---

### Task 1: Shared helpers, the job driver, and `object.get`

Creates the shared header and its definitions, chains the properties hook, and registers `object.get`. A reviewer can check the whole helper layer against one read-only tool before any tool edits the world.

**Files:**
- Create: `indra/newview/idmcptools_object.h`
- Create: `indra/newview/idmcptools_object.cpp`
- Modify: `indra/newview/idmcptools_avatars.cpp:15` (include) and `:165-167` (hook)
- Modify: `indra/newview/idmcptools.h:29`
- Modify: `indra/newview/idmcpserver.cpp:171`
- Modify: `indra/newview/CMakeLists.txt:112` and `:988`

**Interfaces:**
- Consumes: `IDMCPCallPtr`, `idmcp_tool_ok`, `idmcp_tool_err`, `IDMCPCall::setCleanup/connected`, `IDMCP_ERR_*` (`idmcpserver.h`); `IDMCPGateResult`, `IDMCPGatePhase`, `IDMCPTool::timeout` (`idmcptoolregistry.h`); `IDMCPRlvGate::isEnabled/checkBehaviour/deny` (`idmcprlvgate.h`).
- Produces (all in `namespace idmcp_obj`, declared in `idmcptools_object.h`; later tasks use these exact names):
  - `bool isUuid(const std::string&)`, `std::string lower(std::string)`, `std::string argStr(const boost::json::object&, const char*)`, `bool argBool(const boost::json::object&, const char*, bool)`, `bool argNum(const boost::json::object&, const char*, F64&)`, `bool argNums(const boost::json::object&, const char*, size_t n, F32* out)`
  - `double round5(F64)`, `boost::json::array vecJson(const LLVector3&)`, `boost::json::array eulerDegJson(const LLQuaternion&)`, `LLQuaternion quatFromEulerDeg(const LLVector3&)`, `boost::json::object maskJson(U32)`, `boost::json::object permsJson(const LLPermissions&)`, `boost::json::object permsJson(const Props&)`, `boost::json::object itemJson(const LLViewerInventoryItem*)`
  - `std::vector<LLViewerObject*> linksOf(LLViewerObject* root)`, `S32 linkNumberOf(LLViewerObject*)`, `struct Target { root, prim, link }`, `bool resolve(const boost::json::object&, const IDMCPCallPtr&, Target&)`, `LLViewerObject* findRoot(const boost::json::object&, const char* key = "object_id")`, `bool requireModify(const Target&, const IDMCPCallPtr&)`, `bool agentSittingOn(const LLViewerObject*)`, `LLViewerInventoryItem* agentItem(const LLUUID&)`, `bool inLibrary(const LLUUID&)`, `bool inTrash(const LLUUID&)`, `std::string attachPointName(LLViewerObject*)`
  - `IDMCPGateResult rlvEdit(const LLViewerObject*)`, `IDMCPGateResult gateEdit(const boost::json::object&, IDMCPGatePhase)`, `std::string rlvErrorMessage(const IDMCPGateResult&)`, `boost::json::object rlvErrorData(const IDMCPGateResult&, const char*)`
  - `void sendLocalIds(const char* msg_name, LLViewerRegion*, const std::vector<U32>&, bool lead_every_packet = false)`, `void sendTransform(LLViewerObject*, U8 type, const LLVector3& pos, const LLQuaternion& rot, const LLVector3& scale)`, `bool posMatches/scaleMatches(const LLVector3&, const LLVector3&)`, `bool rotMatches(const LLQuaternion&, const LLQuaternion&)`
  - `struct Props { object_id, owner_id, base, owner, group, everyone, next_owner, name, description, seq }`, `void onObjectProperties(LLMessageSystem*)`
  - `class ContentsFetch : public LLVOInventoryListener` with `start(LLViewerObject*)`, `stop()`, `done()`, `failed()`, `items()`
  - `class Job` with public `static void launch(const std::shared_ptr<Job>&)`, `finished()`, `drive(F64)`, `cancel()`; protected `Job(const IDMCPCallPtr&, F64 limit)`, `virtual void tick(F64) = 0`, `virtual std::string timeoutMessage() const`, `virtual boost::json::object timeoutData() const`, `virtual void teardown()`, `succeed(boost::json::value)`, `fail(int, const std::string&, boost::json::value = {})`, `bool failIfDenied(const IDMCPGateResult&)`, `requestProps(const std::vector<LLViewerObject*>&)`, `const Props* props(const LLUUID&) const`, members `mCall`, `mDeadline`, `mPropsAskedAt`
  - `void idmcp_register_object_tools(IDMCPToolRegistry&)` (global, in `idmcptools.h`)

- [ ] **Step 1: Create the shared header**

Create `indra/newview/idmcptools_object.h`:

```cpp
/**
 * @file idmcptools_object.h
 * @brief <ID> MCP server: helpers shared by the object build tools.
 *
 * Part of Five's custom Firestorm fork. Custom code carries an `ID` prefix.
 *
 * Used by idmcptools_object.cpp, idmcptools_faces.cpp, idmcptools_contents.cpp
 * and idmcptools_rez.cpp. Everything is in namespace idmcp_obj, so nothing here
 * clashes with the anonymous-namespace arg_str/looks_like_uuid copies in the
 * other idmcptools_*.cpp files. Main thread only.
 *
 * Selection-free: nothing here calls LLSelectMgr's select, deselect or send
 * APIs. Properties come from a raw ObjectSelect that a Job pairs with an
 * ObjectDeselect when it ends.
 */

#ifndef ID_IDMCPTOOLS_OBJECT_H
#define ID_IDMCPTOOLS_OBJECT_H

#include "idmcpserver.h"            // IDMCPCallPtr, idmcp_tool_ok/err, IDMCP_ERR_*
#include "idmcptoolregistry.h"      // IDMCPGateResult, IDMCPGatePhase
#include "llinventory.h"            // LLInventoryObject::object_list_t
#include "llpermissions.h"
#include "llpointer.h"
#include "llquaternion.h"
#include "lluuid.h"
#include "llviewerinventory.h"
#include "llvoinventorylistener.h"
#include "v3math.h"

#include <boost/json.hpp>
#include <map>
#include <memory>
#include <string>
#include <vector>

class LLMessageSystem;
class LLViewerObject;
class LLViewerRegion;

namespace idmcp_obj
{
    // ---- arguments -----------------------------------------------------------
    bool        isUuid(const std::string& s);
    std::string lower(std::string s);
    std::string argStr(const boost::json::object& args, const char* key);
    bool        argBool(const boost::json::object& args, const char* key, bool dflt);
    // True when args[key] is a number (integer or double).
    bool        argNum(const boost::json::object& args, const char* key, F64& out);
    // True when args[key] is an array of exactly n numbers.
    bool        argNums(const boost::json::object& args, const char* key, size_t n, F32* out);

    // ---- JSON output -----------------------------------------------------------
    double              round5(F64 v);
    boost::json::array  vecJson(const LLVector3& v);
    // [x, y, z] degrees, the way the Edit floater shows a rotation.
    boost::json::array  eulerDegJson(const LLQuaternion& q);
    // The Edit floater's conversion (llpanelobject.cpp:2150), so numbers round-trip.
    LLQuaternion        quatFromEulerDeg(const LLVector3& deg);
    boost::json::object maskJson(U32 mask);                    // {copy, modify, transfer}
    boost::json::object permsJson(const LLPermissions& perm);  // {owner_id, base, owner, group, everyone, next_owner}
    boost::json::object itemJson(const LLViewerInventoryItem* item);  // {item_id, name, type, permissions}

    // ---- targeting -------------------------------------------------------------
    // A linkset's prims in link order: the root, then its children in
    // getChildren() order. Seated avatars and non-volume children are skipped.
    // Link N is element N-1. These numbers can differ from LSL's.
    std::vector<LLViewerObject*> linksOf(LLViewerObject* root);
    S32 linkNumberOf(LLViewerObject* prim);   // 1-based; 0 when not found

    struct Target
    {
        LLViewerObject* root = nullptr;
        LLViewerObject* prim = nullptr;   // the root when link == 0
        S32             link = 0;         // 0 = the whole object
    };
    // Resolves args "object_id" (any prim of the linkset) and the optional
    // "link". On failure answers `call` (INVALID_PARAMS / NOT_FOUND), returns false.
    bool resolve(const boost::json::object& args, const IDMCPCallPtr& call, Target& out);
    // Gate-time lookup of args[key]: the linkset root, or null.
    LLViewerObject* findRoot(const boost::json::object& args, const char* key = "object_id");
    // Answers PERMISSION and returns false unless the agent may modify t.prim.
    bool requireModify(const Target& t, const IDMCPCallPtr& call);
    bool agentSittingOn(const LLViewerObject* root);
    // An item in the agent's inventory or the Library, links resolved; null if unknown.
    LLViewerInventoryItem* agentItem(const LLUUID& id);
    bool inLibrary(const LLUUID& id);
    bool inTrash(const LLUUID& id);
    // The worn root's attachment point name ("Chest", "Center 2"); "" if not worn.
    std::string attachPointName(LLViewerObject* root);

    // ---- RLV ---------------------------------------------------------------------
    // RlvActions::canEdit (@edit, @editobj, @editattach, @editworld) plus
    // RlvActions::canInteract (@interact). Allowed when RLV is off or obj is null.
    IDMCPGateResult     rlvEdit(const LLViewerObject* obj);
    // Request gate for tools that target args["object_id"].
    IDMCPGateResult     gateEdit(const boost::json::object& args, IDMCPGatePhase phase);
    std::string         rlvErrorMessage(const IDMCPGateResult& g);
    boost::json::object rlvErrorData(const IDMCPGateResult& g, const char* checked_at);

    // ---- messages ----------------------------------------------------------------
    // Sends msg_name (body: AgentData + ObjectData{ObjectLocalID}) to region,
    // at most 254 ids per packet. With lead_every_packet, ids[0] opens every
    // packet (ObjectLink: the first id becomes the root).
    void sendLocalIds(const char* msg_name, LLViewerRegion* region, const std::vector<U32>& ids,
                      bool lead_every_packet = false);
    // One MultipleObjectUpdate entry for prim. type is UPD_* bits; pos, rot and
    // scale are read only for the bits that are set.
    void sendTransform(LLViewerObject* prim, U8 type, const LLVector3& pos,
                       const LLQuaternion& rot, const LLVector3& scale);
    bool posMatches(const LLVector3& a, const LLVector3& b);        // within 1 cm
    bool rotMatches(const LLQuaternion& a, const LLQuaternion& b);  // within about 1.6 degrees
    bool scaleMatches(const LLVector3& a, const LLVector3& b);      // within 1 mm

    // ---- ObjectProperties ----------------------------------------------------------
    struct Props
    {
        LLUUID      object_id;
        LLUUID      owner_id;
        U32         base       = 0;
        U32         owner      = 0;
        U32         group      = 0;
        U32         everyone   = 0;
        U32         next_owner = 0;
        std::string name;
        std::string description;
        U64         seq = 0;   // arrival order across all replies
    };
    boost::json::object permsJson(const Props& p);
    // Chained from idmcp::onObjectProperties (idmcptools_avatars.cpp). Stores
    // replies only while an object job is running.
    void onObjectProperties(LLMessageSystem* msg);

    // ---- task inventory ------------------------------------------------------------
    // Fetches one prim's contents. Keeps the prim's id, not a pointer, and looks
    // the prim up again to unregister, so a deleted prim is never touched.
    class ContentsFetch : public LLVOInventoryListener
    {
    public:
        ContentsFetch() = default;
        ~ContentsFetch() override;

        // Registers on prim and asks the region for a fresh copy. The answer can
        // arrive before this returns: requestInventory() calls back at once when
        // it already holds the contents.
        void start(LLViewerObject* prim);
        void stop();
        bool done() const   { return mDone; }
        bool failed() const { return mFailed; }
        const std::vector<LLPointer<LLViewerInventoryItem>>& items() const { return mItems; }

        void inventoryChanged(LLViewerObject* object, LLInventoryObject::object_list_t* inventory,
                              S32 serial_num, void* user_data) override;

    private:
        LLUUID mObjectId;
        bool   mRegistered = false;
        bool   mDone       = false;
        bool   mFailed     = false;
        std::vector<LLPointer<LLViewerInventoryItem>> mItems;
    };

    // ---- deferred jobs -------------------------------------------------------------
    // Base for every deferred object tool. The idle driver owns jobs by
    // shared_ptr. A job holds its call strongly (the server tracks calls weakly);
    // the call's cleanup holds the job weakly, so there is no cycle.
    class Job
    {
    public:
        virtual ~Job() = default;

        // Hands the job to the idle driver and ties it to its call.
        static void launch(const std::shared_ptr<Job>& job);

        bool finished() const { return mFinished; }
        void drive(F64 now);   // idle driver only
        void cancel();         // the call ended without our answer

    protected:
        Job(const IDMCPCallPtr& call, F64 limit);

        // Runs once per idle tick until the job finishes.
        virtual void tick(F64 now) = 0;
        virtual std::string timeoutMessage() const { return "the region didn't confirm in time"; }
        virtual boost::json::object timeoutData() const { return boost::json::object(); }
        // Unregister listeners. Runs once, before the answer goes out.
        virtual void teardown() {}

        void succeed(boost::json::value result);
        void fail(int code, const std::string& msg, boost::json::value data = boost::json::value());
        // Commit-phase RLV re-check: answers RLV_RESTRICTED and returns true when denied.
        bool failIfDenied(const IDMCPGateResult& g);

        // Raw ObjectSelect for prims (no LLSelectMgr). props(id) then returns
        // the first reply for id that arrives after this call. The job sends
        // ObjectDeselect for them when it ends.
        void requestProps(const std::vector<LLViewerObject*>& prims);
        const Props* props(const LLUUID& id) const;

        IDMCPCallPtr mCall;
        F64          mDeadline     = 0.0;
        F64          mPropsAskedAt = 0.0;

    private:
        void finish();

        bool                  mFinished = false;
        std::map<LLUUID, U64> mAsked;   // object id -> reply sequence when last asked
    };
}

#endif // ID_IDMCPTOOLS_OBJECT_H
```

- [ ] **Step 2: Create the implementation with the helpers and `object.get`**

Create `indra/newview/idmcptools_object.cpp`:

```cpp
/**
 * @file idmcptools_object.cpp
 * @brief <ID> MCP server: object build tools (get, edit, permissions, link) and their shared helpers.
 *
 * Part of Five's custom Firestorm fork. Custom code carries an `ID` prefix.
 *
 * Edits are packed by hand from the LLViewerObject, never from LLSelectMgr's
 * selected nodes, so the user's selection and Build floater stay untouched.
 * One idle driver ticks every deferred object job (this file and the faces,
 * contents and rez files). Main thread only.
 */

#include "llviewerprecompiledheaders.h"

#include "idmcptools_object.h"
#include "idmcptools.h"
#include "idmcpserver.h"
#include "idmcprlvgate.h"

#include "llagent.h"
#include "llevents.h"               // LLEventPumps, LLTempBoundListener
#include "llinventorymodel.h"       // gInventory
#include "llselectmgr.h"            // UPD_* constants only; no selection calls
#include "lltextureentry.h"
#include "lltimer.h"
#include "llviewerjointattachment.h"
#include "llviewerobject.h"
#include "llviewerobjectlist.h"     // gObjectList
#include "llviewerregion.h"
#include "llvoavatar.h"
#include "llvoavatarself.h"         // gAgentAvatarp, isAgentAvatarValid
#include "message.h"
#include "rlvactions.h"
#include "rlvdefines.h"

#include <algorithm>
#include <cctype>
#include <cmath>

namespace
{
    using idmcp_obj::Job;
    using idmcp_obj::Props;

    std::vector<std::shared_ptr<Job>> g_jobs;
    bool                              g_tick_on = false;
    LLTempBoundListener               g_tick;

    std::map<LLUUID, Props> g_props;          // replies, kept only while a job runs
    U64                     g_props_seq = 0;
    std::map<LLUUID, S32>   g_raw_selected;   // our ObjectSelects per object, not yet deselected

    constexpr size_t MAX_IDS_PER_PACKET = 254;   // MAX_OBJECTS_PER_PACKET, llselectmgr.cpp:125

    void tick_jobs()
    {
        if (g_jobs.empty()) return;
        const F64 now = LLTimer::getTotalSeconds();
        // Local copy: a tick can finish jobs or launch new ones.
        const std::vector<std::shared_ptr<Job>> jobs = g_jobs;
        for (const auto& j : jobs)
        {
            j->drive(now);
        }
        g_jobs.erase(std::remove_if(g_jobs.begin(), g_jobs.end(),
                                    [](const std::shared_ptr<Job>& j) { return j->finished(); }),
                     g_jobs.end());
        if (g_jobs.empty())
        {
            g_props.clear();
        }
    }

    void ensure_tick()
    {
        if (g_tick_on) return;
        g_tick = LLEventPumps::instance().obtain("mainloop").listen(
            "idmcp_object_jobs", [](const LLSD&) -> bool { tick_jobs(); return false; });
        g_tick_on = true;
    }

    void pack_agent(LLMessageSystem* msg)
    {
        msg->nextBlockFast(_PREHASH_AgentData);
        msg->addUUIDFast(_PREHASH_AgentID, gAgent.getID());
        msg->addUUIDFast(_PREHASH_SessionID, gAgent.getSessionID());
    }
}

namespace idmcp_obj
{
    // ---- arguments -----------------------------------------------------------

    bool isUuid(const std::string& s)
    {
        return s.size() == 36 && s[8] == '-' && s[13] == '-' && s[18] == '-' && s[23] == '-';
    }

    std::string lower(std::string s)
    {
        std::transform(s.begin(), s.end(), s.begin(),
                       [](unsigned char c) { return (char)std::tolower(c); });
        return s;
    }

    std::string argStr(const boost::json::object& args, const char* key)
    {
        auto it = args.find(key);
        return (it != args.end() && it->value().is_string())
                   ? std::string(it->value().as_string().c_str()) : std::string();
    }

    bool argBool(const boost::json::object& args, const char* key, bool dflt)
    {
        auto it = args.find(key);
        return (it != args.end() && it->value().is_bool()) ? it->value().as_bool() : dflt;
    }

    bool argNum(const boost::json::object& args, const char* key, F64& out)
    {
        auto it = args.find(key);
        if (it == args.end()) return false;
        const boost::json::value& v = it->value();
        if      (v.is_double()) out = v.as_double();
        else if (v.is_int64())  out = (F64)v.as_int64();
        else if (v.is_uint64()) out = (F64)v.as_uint64();
        else return false;
        return true;
    }

    bool argNums(const boost::json::object& args, const char* key, size_t n, F32* out)
    {
        auto it = args.find(key);
        if (it == args.end() || !it->value().is_array()) return false;
        const boost::json::array& a = it->value().as_array();
        if (a.size() != n) return false;
        for (size_t i = 0; i < n; ++i)
        {
            if      (a[i].is_double()) out[i] = (F32)a[i].as_double();
            else if (a[i].is_int64())  out[i] = (F32)a[i].as_int64();
            else if (a[i].is_uint64()) out[i] = (F32)a[i].as_uint64();
            else return false;
        }
        return true;
    }

    // ---- JSON output -----------------------------------------------------------

    double round5(F64 v)
    {
        return std::round(v * 100000.0) / 100000.0;
    }

    boost::json::array vecJson(const LLVector3& v)
    {
        boost::json::array a;
        a.push_back(round5(v.mV[VX]));
        a.push_back(round5(v.mV[VY]));
        a.push_back(round5(v.mV[VZ]));
        return a;
    }

    boost::json::array eulerDegJson(const LLQuaternion& q)
    {
        F32 roll = 0.f, pitch = 0.f, yaw = 0.f;
        q.getEulerAngles(&roll, &pitch, &yaw);
        boost::json::array a;
        a.push_back(std::round(roll  * RAD_TO_DEG * 1000.0) / 1000.0);
        a.push_back(std::round(pitch * RAD_TO_DEG * 1000.0) / 1000.0);
        a.push_back(std::round(yaw   * RAD_TO_DEG * 1000.0) / 1000.0);
        return a;
    }

    LLQuaternion quatFromEulerDeg(const LLVector3& deg)
    {
        LLQuaternion q;
        q.setQuat(deg.mV[VX] * DEG_TO_RAD, deg.mV[VY] * DEG_TO_RAD, deg.mV[VZ] * DEG_TO_RAD);
        return q;
    }

    boost::json::object maskJson(U32 mask)
    {
        boost::json::object o;
        o["copy"]     = (mask & PERM_COPY) != 0;
        o["modify"]   = (mask & PERM_MODIFY) != 0;
        o["transfer"] = (mask & PERM_TRANSFER) != 0;
        return o;
    }

    boost::json::object permsJson(const LLPermissions& perm)
    {
        boost::json::object o;
        o["owner_id"]   = perm.getOwner().asString();
        o["base"]       = maskJson(perm.getMaskBase());
        o["owner"]      = maskJson(perm.getMaskOwner());
        o["group"]      = maskJson(perm.getMaskGroup());
        o["everyone"]   = maskJson(perm.getMaskEveryone());
        o["next_owner"] = maskJson(perm.getMaskNextOwner());
        return o;
    }

    boost::json::object permsJson(const Props& p)
    {
        boost::json::object o;
        o["owner_id"]   = p.owner_id.asString();
        o["base"]       = maskJson(p.base);
        o["owner"]      = maskJson(p.owner);
        o["group"]      = maskJson(p.group);
        o["everyone"]   = maskJson(p.everyone);
        o["next_owner"] = maskJson(p.next_owner);
        return o;
    }

    boost::json::object itemJson(const LLViewerInventoryItem* item)
    {
        boost::json::object o;
        o["item_id"]     = item->getUUID().asString();
        o["name"]        = item->getName();
        const char* type = LLAssetType::lookup(item->getType());
        o["type"]        = type ? type : "unknown";
        o["permissions"] = permsJson(item->getPermissions());
        return o;
    }

    // ---- targeting -------------------------------------------------------------

    std::vector<LLViewerObject*> linksOf(LLViewerObject* root)
    {
        std::vector<LLViewerObject*> out;
        if (!root) return out;
        out.push_back(root);
        for (const LLPointer<LLViewerObject>& child : root->getChildren())
        {
            LLViewerObject* c = child.get();
            // Seated avatars are children of the root too; they are not links.
            if (c && !c->isDead() && !c->isAvatar() && c->getPCode() == LL_PCODE_VOLUME)
            {
                out.push_back(c);
            }
        }
        return out;
    }

    S32 linkNumberOf(LLViewerObject* prim)
    {
        if (!prim) return 0;
        const std::vector<LLViewerObject*> links = linksOf(prim->getRootEdit());
        for (size_t i = 0; i < links.size(); ++i)
        {
            if (links[i] == prim) return (S32)i + 1;
        }
        return 0;
    }

    bool resolve(const boost::json::object& args, const IDMCPCallPtr& call, Target& out)
    {
        const std::string sid = argStr(args, "object_id");
        if (!isUuid(sid))
        {
            idmcp_tool_err(call, IDMCP_ERR_INVALID_PARAMS, "object_id must be an object UUID");
            return false;
        }
        LLViewerObject* o = gObjectList.findObject(LLUUID(sid));
        if (!o || o->isDead() || o->getPCode() != LL_PCODE_VOLUME)
        {
            idmcp_tool_err(call, IDMCP_ERR_NOT_FOUND, "object not found or out of range");
            return false;
        }
        out.root = o->getRootEdit();
        out.prim = out.root;
        out.link = 0;
        if (args.contains("link"))
        {
            F64 n = 0.0;
            if (!argNum(args, "link", n) || n != std::floor(n))
            {
                idmcp_tool_err(call, IDMCP_ERR_INVALID_PARAMS, "link must be an integer (1 = the root)");
                return false;
            }
            const std::vector<LLViewerObject*> links = linksOf(out.root);
            if (n < 1.0 || n > (F64)links.size())
            {
                idmcp_tool_err(call, IDMCP_ERR_INVALID_PARAMS,
                               llformat("link %d is out of range: this object has %d links (see object.get)",
                                        (S32)n, (S32)links.size()));
                return false;
            }
            out.link = (S32)n;
            out.prim = links[out.link - 1];
        }
        return true;
    }

    LLViewerObject* findRoot(const boost::json::object& args, const char* key)
    {
        const std::string sid = argStr(args, key);
        if (!isUuid(sid)) return nullptr;
        LLViewerObject* o = gObjectList.findObject(LLUUID(sid));
        if (!o || o->isDead() || o->getPCode() != LL_PCODE_VOLUME) return nullptr;
        return o->getRootEdit();
    }

    bool requireModify(const Target& t, const IDMCPCallPtr& call)
    {
        // permModify() reads FLAGS_OBJECT_MODIFY, which every ObjectUpdate carries.
        if (t.prim && t.prim->permModify()) return true;
        idmcp_tool_err(call, IDMCP_ERR_PERMISSION, "you don't have modify permission on this object");
        return false;
    }

    bool agentSittingOn(const LLViewerObject* root)
    {
        if (!root || !isAgentAvatarValid() || !gAgentAvatarp->isSitting()) return false;
        LLViewerObject* seat = dynamic_cast<LLViewerObject*>(gAgentAvatarp->getParent());
        return seat && seat->getRootEdit() == root;
    }

    LLViewerInventoryItem* agentItem(const LLUUID& id)
    {
        if (id.isNull()) return nullptr;
        return gInventory.getItem(gInventory.getLinkedItemID(id));
    }

    bool inLibrary(const LLUUID& id)
    {
        const LLUUID& lib = gInventory.getLibraryRootFolderID();
        return lib.notNull() && (id == lib || gInventory.isObjectDescendentOf(id, lib));
    }

    bool inTrash(const LLUUID& id)
    {
        const LLUUID trash = gInventory.findCategoryUUIDForType(LLFolderType::FT_TRASH);
        return trash.notNull() && (id == trash || gInventory.isObjectDescendentOf(id, trash));
    }

    std::string attachPointName(LLViewerObject* root)
    {
        if (!root || !root->isAttachment()) return std::string();
        LLVOAvatar* av = root->getAvatar();
        LLViewerJointAttachment* pt = av ? av->getTargetAttachmentPoint(root) : nullptr;
        return pt ? pt->getName() : std::string();
    }

    // ---- RLV ---------------------------------------------------------------------

    IDMCPGateResult rlvEdit(const LLViewerObject* obj)
    {
        if (!IDMCPRlvGate::isEnabled() || !obj) return IDMCPGateResult();
        if (!RlvActions::canEdit(obj))
        {
            // Name the restriction that applies. @editworld/@editattach apply
            // unconditionally; @edit and @editobj depend on exceptions.
            if (!obj->isAttachment() && RlvActions::hasBehaviour(RLV_BHVR_EDITWORLD))
                return IDMCPRlvGate::deny(RLV_BHVR_EDITWORLD, "editworld");
            if (obj->isAttachment() && RlvActions::hasBehaviour(RLV_BHVR_EDITATTACH))
                return IDMCPRlvGate::deny(RLV_BHVR_EDITATTACH, "editattach");
            if (RlvActions::hasBehaviour(RLV_BHVR_EDIT))
                return IDMCPRlvGate::deny(RLV_BHVR_EDIT, "edit");
            return IDMCPRlvGate::deny(RLV_BHVR_EDITOBJ, "editobj");
        }
        if (!RlvActions::canInteract(obj))
        {
            return IDMCPRlvGate::deny(RLV_BHVR_INTERACT, "interact");
        }
        return IDMCPGateResult();
    }

    IDMCPGateResult gateEdit(const boost::json::object& args, IDMCPGatePhase)
    {
        // Not found: allow, and let invoke report NOT_FOUND.
        return rlvEdit(findRoot(args));
    }

    std::string rlvErrorMessage(const IDMCPGateResult& g)
    {
        return "blocked by RLV restriction @" + g.behaviour;
    }

    boost::json::object rlvErrorData(const IDMCPGateResult& g, const char* checked_at)
    {
        boost::json::object data;
        data["restriction"] = g.behaviour;
        data["sources"]     = g.sources;
        data["checkedAt"]   = checked_at;
        return data;
    }

    // ---- messages ----------------------------------------------------------------

    void sendLocalIds(const char* msg_name, LLViewerRegion* region, const std::vector<U32>& ids,
                      bool lead_every_packet)
    {
        if (!region || ids.empty()) return;
        LLMessageSystem* msg = gMessageSystem;
        size_t next = 0;
        while (next < ids.size())
        {
            msg->newMessageFast(msg_name);
            pack_agent(msg);
            size_t in_packet = 0;
            if (lead_every_packet && next > 0)
            {
                // ObjectLink: every packet must start with the same root, or the
                // region builds separate linksets (llselectmgr.cpp:6118-6131).
                msg->nextBlockFast(_PREHASH_ObjectData);
                msg->addU32Fast(_PREHASH_ObjectLocalID, ids[0]);
                ++in_packet;
            }
            while (next < ids.size() && in_packet < MAX_IDS_PER_PACKET)
            {
                msg->nextBlockFast(_PREHASH_ObjectData);
                msg->addU32Fast(_PREHASH_ObjectLocalID, ids[next++]);
                ++in_packet;
            }
            msg->sendReliable(region->getHost());
        }
    }

    void sendTransform(LLViewerObject* prim, U8 type, const LLVector3& pos,
                       const LLQuaternion& rot, const LLVector3& scale)
    {
        if (!prim || !prim->getRegion()) return;
        U8  data[36];
        S32 offset = 0;
        // The region reads position, rotation, scale in this order (packMultipleUpdate).
        if (type & UPD_POSITION)
        {
            htolememcpy(&data[offset], pos.mV, MVT_LLVector3, 12);
            offset += 12;
        }
        if (type & UPD_ROTATION)
        {
            LLQuaternion q = rot;
            q.normalize();
            const LLVector3 packed = q.packToVector3();
            htolememcpy(&data[offset], packed.mV, MVT_LLQuaternion, 12);
            offset += 12;
        }
        if (type & UPD_SCALE)
        {
            htolememcpy(&data[offset], scale.mV, MVT_LLVector3, 12);
            offset += 12;
        }
        LLMessageSystem* msg = gMessageSystem;
        msg->newMessageFast(_PREHASH_MultipleObjectUpdate);
        pack_agent(msg);
        msg->nextBlockFast(_PREHASH_ObjectData);
        msg->addU32Fast(_PREHASH_ObjectLocalID, prim->getLocalID());
        msg->addU8Fast(_PREHASH_Type, type);
        msg->addBinaryDataFast(_PREHASH_Data, data, offset);
        msg->sendReliable(prim->getRegion()->getHost());
    }

    bool posMatches(const LLVector3& a, const LLVector3& b)
    {
        return (a - b).length() <= 0.01f;
    }

    bool rotMatches(const LLQuaternion& a, const LLQuaternion& b)
    {
        // Terse updates quantize rotations, so allow a little slack.
        return std::fabs(dot(a, b)) >= 0.9999f;
    }

    bool scaleMatches(const LLVector3& a, const LLVector3& b)
    {
        return (a - b).length() <= 0.001f;
    }

    // ---- ObjectProperties ----------------------------------------------------------

    void onObjectProperties(LLMessageSystem* msg)
    {
        if (!msg || g_jobs.empty()) return;
        const S32 n = msg->getNumberOfBlocksFast(_PREHASH_ObjectData);
        for (S32 i = 0; i < n; ++i)
        {
            Props p;
            msg->getUUIDFast(_PREHASH_ObjectData, _PREHASH_ObjectID, p.object_id, i);
            if (p.object_id.isNull()) continue;
            msg->getUUIDFast(_PREHASH_ObjectData, _PREHASH_OwnerID, p.owner_id, i);
            msg->getU32Fast(_PREHASH_ObjectData, _PREHASH_BaseMask, p.base, i);
            msg->getU32Fast(_PREHASH_ObjectData, _PREHASH_OwnerMask, p.owner, i);
            msg->getU32Fast(_PREHASH_ObjectData, _PREHASH_GroupMask, p.group, i);
            msg->getU32Fast(_PREHASH_ObjectData, _PREHASH_EveryoneMask, p.everyone, i);
            msg->getU32Fast(_PREHASH_ObjectData, _PREHASH_NextOwnerMask, p.next_owner, i);
            msg->getStringFast(_PREHASH_ObjectData, _PREHASH_Name, p.name, i);
            msg->getStringFast(_PREHASH_ObjectData, _PREHASH_Description, p.description, i);
            p.seq = ++g_props_seq;
            g_props[p.object_id] = std::move(p);
        }
    }

    // ---- task inventory ------------------------------------------------------------

    ContentsFetch::~ContentsFetch()
    {
        stop();
    }

    void ContentsFetch::start(LLViewerObject* prim)
    {
        stop();
        mDone   = false;
        mFailed = false;
        mItems.clear();
        if (!prim) return;
        mObjectId = prim->getID();
        // Register directly, not through registerVOInventoryListener: that keeps a
        // raw pointer the base destructor would use after the prim is deleted.
        prim->registerInventoryListener(this, nullptr);
        mRegistered = true;
        // With a listener registered, dirtyInventory() drops the cached copy and
        // requestInventory() fetches a fresh one.
        prim->dirtyInventory();
        prim->requestInventory();
    }

    void ContentsFetch::stop()
    {
        if (!mRegistered) return;
        mRegistered = false;
        if (LLViewerObject* o = gObjectList.findObject(mObjectId))
        {
            o->removeInventoryListener(this);
        }
    }

    void ContentsFetch::inventoryChanged(LLViewerObject*, LLInventoryObject::object_list_t* inventory,
                                         S32, void*)
    {
        // Record only; the owning job reads this on its next tick. Never
        // unregister other listeners from inside this callback.
        if (mDone) return;
        mItems.clear();
        if (!inventory)
        {
            mFailed = true;
            mDone   = true;
            return;
        }
        for (const LLPointer<LLInventoryObject>& obj : *inventory)
        {
            LLInventoryObject* io = obj.get();
            if (!io || io->getType() == LLAssetType::AT_CATEGORY) continue;   // the root folder entry
            if (LLViewerInventoryItem* item = dynamic_cast<LLViewerInventoryItem*>(io))
            {
                mItems.emplace_back(item);
            }
        }
        mDone = true;
    }

    // ---- deferred jobs -------------------------------------------------------------

    Job::Job(const IDMCPCallPtr& call, F64 limit)
        : mCall(call), mDeadline(LLTimer::getTotalSeconds() + limit)
    {
    }

    void Job::launch(const std::shared_ptr<Job>& job)
    {
        g_jobs.push_back(job);
        // weak_ptr, not a copy of job: the job holds the call, so a strong
        // capture here would make a cycle and leak both.
        std::weak_ptr<Job> weak = job;
        job->mCall->setCleanup([weak]()
        {
            if (auto j = weak.lock()) j->cancel();
        });
        ensure_tick();
    }

    void Job::drive(F64 now)
    {
        if (mFinished) return;
        if (!mCall->connected())
        {
            cancel();
            return;
        }
        if (now >= mDeadline)
        {
            fail(IDMCP_ERR_TIMEOUT, timeoutMessage(), timeoutData());
            return;
        }
        tick(now);
    }

    void Job::cancel()
    {
        if (!mFinished) finish();
    }

    void Job::succeed(boost::json::value result)
    {
        if (mFinished) return;
        finish();   // before answering: the call's cleanup calls cancel(), which must be a no-op
        idmcp_tool_ok(mCall, std::move(result));
    }

    void Job::fail(int code, const std::string& msg, boost::json::value data)
    {
        if (mFinished) return;
        finish();
        idmcp_tool_err(mCall, code, msg, std::move(data));
    }

    bool Job::failIfDenied(const IDMCPGateResult& g)
    {
        if (g.allowed) return false;
        fail(IDMCP_ERR_RLV_RESTRICTED, rlvErrorMessage(g), rlvErrorData(g, "commit"));
        return true;
    }

    void Job::requestProps(const std::vector<LLViewerObject*>& prims)
    {
        std::map<LLViewerRegion*, std::vector<U32>> by_region;
        for (LLViewerObject* p : prims)
        {
            if (!p || p->isDead() || !p->getRegion()) continue;
            const LLUUID& id = p->getID();
            if (mAsked.find(id) == mAsked.end())
            {
                ++g_raw_selected[id];
            }
            mAsked[id] = g_props_seq;   // only replies newer than this count
            by_region[p->getRegion()].push_back(p->getLocalID());
        }
        // A local id only means something to its own region.
        for (const auto& kv : by_region)
        {
            sendLocalIds(_PREHASH_ObjectSelect, kv.first, kv.second);
        }
        mPropsAskedAt = LLTimer::getTotalSeconds();
    }

    const Props* Job::props(const LLUUID& id) const
    {
        auto asked = mAsked.find(id);
        if (asked == mAsked.end()) return nullptr;
        auto it = g_props.find(id);
        return (it != g_props.end() && it->second.seq > asked->second) ? &it->second : nullptr;
    }

    void Job::finish()
    {
        mFinished = true;
        teardown();
        std::map<LLViewerRegion*, std::vector<U32>> by_region;
        for (const auto& kv : mAsked)
        {
            auto c = g_raw_selected.find(kv.first);
            if (c == g_raw_selected.end()) continue;
            if (--c->second > 0) continue;   // another job still wants it selected
            g_raw_selected.erase(c);
            LLViewerObject* o = gObjectList.findObject(kv.first);
            // Leave objects the user has selected: a deselect would clear their
            // selection on the region.
            if (o && !o->isDead() && !o->isSelected() && o->getRegion())
            {
                by_region[o->getRegion()].push_back(o->getLocalID());
            }
        }
        mAsked.clear();
        for (const auto& kv : by_region)
        {
            sendLocalIds(_PREHASH_ObjectDeselect, kv.first, kv.second);
        }
    }
}

using namespace idmcp_obj;

namespace
{
    // ---- object.get --------------------------------------------------------------

    constexpr F64 GET_LIMIT   = 15.0;
    constexpr F64 PROPS_LIMIT = 10.0;   // spec: a properties request times out after 10 s

    boost::json::array faces_json(LLViewerObject* p)
    {
        boost::json::array out;
        const S32 n = p->getNumTEs();
        for (S32 i = 0; i < n; ++i)
        {
            const LLTextureEntry* te = p->getTE((U8)i);
            if (!te) continue;
            boost::json::object f;
            f["face"]    = i;
            f["texture"] = te->getID().asString();
            const LLUUID& mat = p->getRenderMaterialID((U8)i);
            f["material"] = mat.isNull() ? boost::json::value(nullptr) : boost::json::value(mat.asString());
            const LLColor4& c = te->getColor();
            boost::json::array rgb;
            rgb.push_back(round5(c.mV[VRED]));
            rgb.push_back(round5(c.mV[VGREEN]));
            rgb.push_back(round5(c.mV[VBLUE]));
            f["color"] = std::move(rgb);
            f["alpha"] = round5(c.mV[VALPHA]);
            boost::json::array rep;
            rep.push_back(round5(te->getScaleS()));
            rep.push_back(round5(te->getScaleT()));
            f["repeats"] = std::move(rep);
            boost::json::array off;
            off.push_back(round5(te->getOffsetS()));
            off.push_back(round5(te->getOffsetT()));
            f["offset"]   = std::move(off);
            f["rotation"] = std::round(te->getRotation() * RAD_TO_DEG * 1000.0) / 1000.0;
            out.push_back(std::move(f));
        }
        return out;
    }

    class GetJob : public Job
    {
    public:
        GetJob(const IDMCPCallPtr& call, LLViewerObject* root, bool faces)
            : Job(call, GET_LIMIT), mRootId(root->getID()), mFaces(faces) {}

        void begin()
        {
            mPropsUntil = LLTimer::getTotalSeconds() + PROPS_LIMIT;
            requestProps(linksOf(gObjectList.findObject(mRootId)));
        }

    protected:
        void tick(F64 now) override
        {
            LLViewerObject* root = gObjectList.findObject(mRootId);
            if (!root || root->isDead())
            {
                fail(IDMCP_ERR_NOT_FOUND, "the object is gone (deleted, taken or out of range)");
                return;
            }
            const std::vector<LLViewerObject*> links = linksOf(root);
            if (now < mPropsUntil)
            {
                for (LLViewerObject* p : links)
                {
                    if (!props(p->getID())) return;
                }
            }
            succeed(describe(root, links));
        }

    private:
        boost::json::object describe(LLViewerObject* root, const std::vector<LLViewerObject*>& links) const
        {
            boost::json::array out_links;
            boost::json::array incomplete;
            for (size_t i = 0; i < links.size(); ++i)
            {
                LLViewerObject* p = links[i];
                const Props* pr = props(p->getID());
                boost::json::object l;
                l["link"]      = (S32)i + 1;
                l["object_id"] = p->getID().asString();
                if (pr)
                {
                    l["name"]        = pr->name;
                    l["description"] = pr->description;
                }
                else
                {
                    incomplete.push_back((S32)i + 1);
                }
                l["position"]   = vecJson(p->getPosition());
                l["rotation"]   = eulerDegJson(p->getRotation());
                l["scale"]      = vecJson(p->getScale());
                l["face_count"] = (S32)p->getNumTEs();
                if (pr) l["permissions"] = permsJson(*pr);
                if (mFaces) l["faces"] = faces_json(p);
                out_links.push_back(std::move(l));
            }
            boost::json::object o;
            o["object_id"]  = root->getID().asString();
            o["link_count"] = (S32)links.size();
            if (root->isAttachment()) o["attachment_point"] = attachPointName(root);
            o["links"] = std::move(out_links);
            if (!incomplete.empty()) o["incomplete_links"] = std::move(incomplete);
            return o;
        }

        LLUUID mRootId;
        bool   mFaces      = false;
        F64    mPropsUntil = 0.0;
    };

    void run_get(const boost::json::object& args, const IDMCPCallPtr& call)
    {
        Target t;
        if (!resolve(args, call, t)) return;
        auto job = std::make_shared<GetJob>(call, t.root, argBool(args, "faces", false));
        Job::launch(job);
        job->begin();
    }
}

// ---------------------------------------------------------------------------

void idmcp_register_object_tools(IDMCPToolRegistry& reg)
{
    // object.get ---------------------------------------------------------------
    {
        IDMCPTool t;
        t.name = "object.get";
        t.description =
            "Describe an object, rezzed or worn, link by link. {\"object_id\"} is any prim "
            "of the object (from objects.getNearby, avatars.getWorn, or an object.rez / "
            "object.attach result). Returns {object_id, link_count, attachment_point (when "
            "worn), links:[{link, object_id, name, description, position, rotation, scale, "
            "face_count, permissions}]}. Link 1 is the root. Use these link numbers with the "
            "other object.* tools; they can differ from LSL's. position (metres) and rotation "
            "([x,y,z] degrees) are region-relative for a rezzed root, attachment-point-relative "
            "for a worn root, and root-relative for a child. permissions has owner_id and "
            "base/owner/group/everyone/next_owner as {copy, modify, transfer}. {\"faces\":true} "
            "adds each face's texture, material, color, alpha, repeats, offset and rotation. "
            "Links whose name and permissions didn't arrive within 10 s are listed in "
            "incomplete_links.";
        t.input_schema = boost::json::parse(
            R"({"type":"object","properties":{"object_id":{"type":"string"},"faces":{"type":"boolean"}},"required":["object_id"],"additionalProperties":false})");
        t.timeout = 25.0;
        t.invoke = [](const boost::json::object& args, const IDMCPCallPtr& call) { run_get(args, call); };
        reg.add(std::move(t));
    }
}
```

- [ ] **Step 3: Chain the properties hook**

In `indra/newview/idmcptools_avatars.cpp`, after line 15 (`#include "idmcp.h"       // facade: idmcp::onObjectProperties`), add:

```cpp
#include "idmcptools_object.h"      // idmcp_obj::onObjectProperties (object build tools)
```

Replace the first two lines of the hook's body (lines 165-167):

```cpp
void idmcp::onObjectProperties(LLMessageSystem* msg)
{
    if (!msg || g_worn_waits.empty()) return;
```

with:

```cpp
void idmcp::onObjectProperties(LLMessageSystem* msg)
{
    // Object build tools first: the early return below only concerns getWorn.
    idmcp_obj::onObjectProperties(msg);

    if (!msg || g_worn_waits.empty()) return;
```

- [ ] **Step 4: Register the tool area**

In `indra/newview/idmcptools.h`, after line 29 (`void idmcp_register_money_tools(IDMCPToolRegistry& reg);`), add:

```cpp
void idmcp_register_object_tools(IDMCPToolRegistry& reg);
```

In `indra/newview/idmcpserver.cpp`, after line 171 (`    idmcp_register_money_tools(mRegistry);`), add:

```cpp
    idmcp_register_object_tools(mRegistry);
```

In `indra/newview/CMakeLists.txt`, after line 112 (`    idmcptools_money.cpp  # <ID> Embedded MCP server: L$ balance + guarded pay`), add:

```cmake
    idmcptools_object.cpp  # <ID> Embedded MCP server: object build tools
```

After line 988 (`    idmcp_meshupload.h  # <ID> Embedded MCP server: upload.mesh floater job`), add:

```cmake
    idmcptools_object.h  # <ID> Embedded MCP server: object build helpers
```

- [ ] **Step 5: Compile-read checklist**

Read each of these in the source to confirm:
- `idmcptools_object.h` includes `idmcpserver.h`, which already includes `idmcptoolregistry.h` and `<boost/json.hpp>`.
- `LLVOInventoryListener`'s constructor and destructor are protected (`llvoinventorylistener.h:59-62`); a derived class with a public constructor is fine. `inventoryChanged` is public pure virtual with the exact signature used (`:41-44`).
- `LLViewerObject::registerInventoryListener`, `removeInventoryListener`, `dirtyInventory`, `requestInventory` are public (`llviewerobject.h:547-579`). `isSelected()` is public (`:274`). `getAvatar()` is `virtual LLVOAvatar* getAvatar() const` (`:201`).
- `LLVOAvatar::getTargetAttachmentPoint(LLViewerObject*)` is public (`llvoavatar.h:1063`). `gAgentAvatarp->getParent()` returns `LLXform*`, and `LLXform` is polymorphic, so the `dynamic_cast` compiles.
- `LLPointer<T>::get()` returns `T*` (`llcommon/llpointer.h:111`); `operator->() const` returns `const T*`, which is why `linksOf` takes `child.get()` first.
- `htolememcpy` is in `llmessage/message.h:991`; `MVT_LLVector3` and `MVT_LLQuaternion` come with it. `UPD_POSITION`, `UPD_ROTATION`, `UPD_SCALE`, `UPD_LINKED_SETS` are file-scope `const U8` in `llselectmgr.h:59-64`.
- `LLQuaternion::setQuat(F32 roll, F32 pitch, F32 yaw)`, `getEulerAngles`, `packToVector3`, `normalize` and the friend `dot` are in `llmath/llquaternion.h:92-152`.
- `LLAssetType::lookup(EType)` returns `const char*` (`llcommon/llassettype.h:152`).
- `RlvActions::canEdit(const LLViewerObject*)`, `canInteract`, `hasBehaviour` are public static (`rlvactions.h:313, 325, 421`). `RLV_BHVR_EDIT/EDITATTACH/EDITOBJ/EDITWORLD/INTERACT` exist (`rlvdefines.h:174-184`).
- `IDMCPRlvGate::deny(ERlvBehaviour, const char*)` is public static (`idmcprlvgate.h`).
- No identifier in either new file is on the X11 list. Enumerators: none yet.
- `idmcptools_avatars.cpp` keeps its own anonymous `arg_str`/`looks_like_uuid`; the new header adds nothing to the global or anonymous namespace, so there is no ambiguity.

- [ ] **Step 6: Commit**

```bash
git add indra/newview/idmcptools_object.h indra/newview/idmcptools_object.cpp indra/newview/idmcptools_avatars.cpp indra/newview/idmcptools.h indra/newview/idmcpserver.cpp indra/newview/CMakeLists.txt
git commit -m "feat: add object.get MCP tool and shared object build helpers"
```

---

### Task 2: `object.edit`, `object.setPermissions`, `object.link`, `object.unlink`

Adds the four tools that change an object's transform, name, permissions and linkage. All four live in `idmcptools_object.cpp`, as the spec assigns. Each sends its messages in `invoke`, then a job waits for the region to echo the change.

**Files:**
- Modify: `indra/newview/idmcptools_object.cpp` (includes; a new anonymous-namespace block before `void idmcp_register_object_tools`; four registration blocks at the end of `idmcp_register_object_tools`)

**Interfaces:**
- Consumes (Task 1, `namespace idmcp_obj`): `Target`, `resolve`, `requireModify`, `findRoot`, `linksOf`, `argStr`, `argBool`, `argNum`, `argNums`, `isUuid`, `vecJson`, `eulerDegJson`, `quatFromEulerDeg`, `maskJson`, `rlvEdit`, `gateEdit`, `sendLocalIds`, `sendTransform`, `posMatches`, `rotMatches`, `scaleMatches`, `Props`, `ContentsFetch`, `Job` (`launch`, `tick`, `timeoutMessage`, `timeoutData`, `teardown`, `succeed`, `fail`, `failIfDenied`, `requestProps`, `props`, `mPropsAskedAt`). The file's own anonymous helpers `pack_agent` (Task 1).
- Produces: registered tools `object.edit`, `object.setPermissions`, `object.link`, `object.unlink`. Nothing new for other files.

- [ ] **Step 1: Add includes**

In `indra/newview/idmcptools_object.cpp`, after `#include "llagent.h"`, add:

```cpp
#include "lldbstrings.h"            // DB_INV_ITEM_NAME_STR_LEN, DB_INV_ITEM_DESC_STR_LEN
#include "llinventorydefines.h"     // TASK_INVENTORY_ITEM_KEY, LLInventoryItemFlags
#include "llworld.h"                // region min/max prim scale
```

- [ ] **Step 2: Add the edit, permissions and link jobs**

In `indra/newview/idmcptools_object.cpp`, directly before the line `// ---------------------------------------------------------------------------` that precedes `void idmcp_register_object_tools(IDMCPToolRegistry& reg)`, add:

```cpp
namespace
{
    constexpr U32 PERM_CMT = PERM_COPY | PERM_MODIFY | PERM_TRANSFER;

    // ---- object.edit -------------------------------------------------------------

    constexpr F64 EDIT_LIMIT = 10.0;   // spec: echo wait 10 s

    // ObjectName / ObjectDescription. Their ObjectData field is LocalID, not ObjectLocalID.
    void send_text(LLViewerObject* p, const char* msg_name, const char* field, const std::string& text)
    {
        LLMessageSystem* msg = gMessageSystem;
        msg->newMessageFast(msg_name);
        pack_agent(msg);
        msg->nextBlockFast(_PREHASH_ObjectData);
        msg->addU32Fast(_PREHASH_LocalID, p->getLocalID());
        msg->addStringFast(field, text);
        msg->sendReliable(p->getRegion()->getHost());
    }

    class EditJob : public Job
    {
    public:
        EditJob(const IDMCPCallPtr& call, const Target& t)
            : Job(call, EDIT_LIMIT), mRootId(t.root->getID()), mPrimId(t.prim->getID()), mLink(t.link) {}

        bool         mHasPos = false, mHasRot = false, mHasScale = false, mHasName = false, mHasDesc = false;
        LLVector3    mPos;
        LLVector3    mScale;
        LLQuaternion mRot;
        std::string  mName;
        std::string  mDesc;

        // Sends everything. The values are packed as requested and not applied
        // locally first, so the echo check below only passes once the region
        // has sent the object back.
        void begin()
        {
            LLViewerObject* p = gObjectList.findObject(mPrimId);
            if (!p || !p->getRegion()) return;   // tick() reports NOT_FOUND
            if (mLink == 0)
            {
                // Whole object: position and rotation move the linkset. Scale
                // goes in its own entry without UPD_LINKED_SETS, so only the
                // root prim scales.
                const U8 type = (U8)((mHasPos ? UPD_POSITION : 0) | (mHasRot ? UPD_ROTATION : 0));
                if (type) sendTransform(p, (U8)(type | UPD_LINKED_SETS), mPos, mRot, mScale);
                if (mHasScale) sendTransform(p, UPD_SCALE, mPos, mRot, mScale);
            }
            else
            {
                const U8 type = (U8)((mHasPos ? UPD_POSITION : 0) | (mHasRot ? UPD_ROTATION : 0)
                                     | (mHasScale ? UPD_SCALE : 0));
                if (type) sendTransform(p, type, mPos, mRot, mScale);
            }
            if (mHasName) send_text(p, _PREHASH_ObjectName, _PREHASH_Name, mName);
            if (mHasDesc) send_text(p, _PREHASH_ObjectDescription, _PREHASH_Description, mDesc);
            if (mHasName || mHasDesc) requestProps({ p });
        }

    protected:
        void tick(F64 now) override
        {
            LLViewerObject* p = gObjectList.findObject(mPrimId);
            if (!p || p->isDead())
            {
                fail(IDMCP_ERR_NOT_FOUND, "the object is gone (deleted, taken or out of range)");
                return;
            }
            if (mHasPos && !posMatches(p->getPosition(), mPos)) return;
            if (mHasRot && !rotMatches(p->getRotation(), mRot)) return;
            if (mHasScale && !scaleMatches(p->getScale(), mScale)) return;
            if (mHasName || mHasDesc)
            {
                const Props* pr = props(mPrimId);
                if (!pr || (mHasName && pr->name != mName) || (mHasDesc && pr->description != mDesc))
                {
                    // The viewer keeps no object names, so ask again once a second.
                    if (now - mPropsAskedAt >= 1.0) requestProps({ p });
                    return;
                }
            }
            succeed(current(p));
        }

        boost::json::object timeoutData() const override
        {
            boost::json::object sent;
            if (mHasPos)   sent["position"]    = vecJson(mPos);
            if (mHasRot)   sent["rotation"]    = eulerDegJson(mRot);
            if (mHasScale) sent["scale"]       = vecJson(mScale);
            if (mHasName)  sent["name"]        = mName;
            if (mHasDesc)  sent["description"] = mDesc;
            boost::json::object data;
            data["sent"] = std::move(sent);
            if (LLViewerObject* p = gObjectList.findObject(mPrimId)) data["current"] = current(p);
            return data;
        }

    private:
        boost::json::object current(LLViewerObject* p) const
        {
            boost::json::object o;
            o["object_id"] = mRootId.asString();
            if (mLink)
            {
                o["link"]    = mLink;
                o["prim_id"] = mPrimId.asString();
            }
            o["position"] = vecJson(p->getPosition());
            o["rotation"] = eulerDegJson(p->getRotation());
            o["scale"]    = vecJson(p->getScale());
            if (const Props* pr = props(mPrimId))
            {
                o["name"]        = pr->name;
                o["description"] = pr->description;
            }
            return o;
        }

        LLUUID mRootId;
        LLUUID mPrimId;
        S32    mLink = 0;
    };

    void run_edit(const boost::json::object& args, const IDMCPCallPtr& call)
    {
        Target t;
        if (!resolve(args, call, t) || !requireModify(t, call)) return;
        auto job = std::make_shared<EditJob>(call, t);
        auto bad = [&call](const std::string& m) { idmcp_tool_err(call, IDMCP_ERR_INVALID_PARAMS, m); };

        if (args.contains("position"))
        {
            if (!argNums(args, "position", 3, job->mPos.mV))
            {
                bad("position must be [x, y, z] in metres");
                return;
            }
            // A rezzed root's position is in region coordinates; keep it in the region.
            if (t.prim == t.root && !t.root->isAttachment())
            {
                const F32 w = t.root->getRegion()->getWidth();
                if (job->mPos.mV[VX] < 0.f || job->mPos.mV[VX] >= w || job->mPos.mV[VY] < 0.f || job->mPos.mV[VY] >= w)
                {
                    bad(llformat("position x and y must be inside the region (0 to %.0f)", w));
                    return;
                }
            }
            job->mHasPos = true;
        }
        if (args.contains("rotation"))
        {
            LLVector3 deg;
            if (!argNums(args, "rotation", 3, deg.mV))
            {
                bad("rotation must be [x, y, z] in degrees");
                return;
            }
            job->mRot    = quatFromEulerDeg(deg);
            job->mHasRot = true;
        }
        if (args.contains("scale"))
        {
            if (!argNums(args, "scale", 3, job->mScale.mV))
            {
                bad("scale must be [x, y, z] in metres");
                return;
            }
            const F32 lo = LLWorld::getInstance()->getRegionMinPrimScale();
            const F32 hi = LLWorld::getInstance()->getRegionMaxPrimScale();
            for (S32 i = 0; i < 3; ++i)
            {
                if (job->mScale.mV[i] < lo || job->mScale.mV[i] > hi)
                {
                    bad(llformat("each scale value must be between %.3f and %.1f metres", lo, hi));
                    return;
                }
            }
            job->mHasScale = true;
        }
        if (args.contains("name"))
        {
            job->mName = argStr(args, "name");
            if (job->mName.empty() || (S32)job->mName.size() > DB_INV_ITEM_NAME_STR_LEN)
            {
                bad(llformat("name must be 1 to %d characters", DB_INV_ITEM_NAME_STR_LEN));
                return;
            }
            job->mHasName = true;
        }
        if (args.contains("description"))
        {
            if (!args.at("description").is_string())
            {
                bad("description must be a string");
                return;
            }
            job->mDesc = argStr(args, "description");
            if ((S32)job->mDesc.size() > DB_INV_ITEM_DESC_STR_LEN)
            {
                bad(llformat("description must be at most %d characters", DB_INV_ITEM_DESC_STR_LEN));
                return;
            }
            job->mHasDesc = true;
        }
        if (!(job->mHasPos || job->mHasRot || job->mHasScale || job->mHasName || job->mHasDesc))
        {
            bad("nothing to change: pass position, rotation, scale, name or description");
            return;
        }
        Job::launch(job);
        job->begin();
    }

    // ---- object.setPermissions ----------------------------------------------------

    constexpr F64 PERMS_LIMIT        = 30.0;
    constexpr F64 PERMS_OBJECT_LIMIT = 10.0;

    void send_next_owner(LLViewerObject* root, bool set, U32 mask)
    {
        // As the Build floater's next-owner checkboxes: one message sets bits,
        // another clears them. Sent for the root only.
        LLMessageSystem* msg = gMessageSystem;
        msg->newMessageFast(_PREHASH_ObjectPermissions);
        pack_agent(msg);
        msg->nextBlockFast(_PREHASH_HeaderData);
        msg->addBOOLFast(_PREHASH_Override, false);
        msg->nextBlockFast(_PREHASH_ObjectData);
        msg->addU32Fast(_PREHASH_ObjectLocalID, root->getLocalID());
        msg->addU8Fast(_PREHASH_Field, PERM_NEXT_OWNER);
        msg->addBOOLFast(_PREHASH_Set, set);
        msg->addU32Fast(_PREHASH_Mask, mask);
        msg->sendReliable(root->getRegion()->getHost());
    }

    class PermsJob : public Job
    {
    public:
        PermsJob(const IDMCPCallPtr& call, LLViewerObject* root, U32 want, bool contents)
            : Job(call, PERMS_LIMIT), mRootId(root->getID()), mWant(want), mContents(contents) {}

        void begin()
        {
            LLViewerObject* root = gObjectList.findObject(mRootId);
            if (!root || !root->getRegion()) return;   // tick() reports NOT_FOUND
            send_next_owner(root, true, mWant);
            if (PERM_CMT & ~mWant) send_next_owner(root, false, PERM_CMT & ~mWant);
            mObjectUntil = LLTimer::getTotalSeconds() + PERMS_OBJECT_LIMIT;
            requestProps({ root });
        }

    protected:
        void tick(F64 now) override
        {
            LLViewerObject* root = gObjectList.findObject(mRootId);
            if (!root || root->isDead())
            {
                fail(IDMCP_ERR_NOT_FOUND, "the object is gone (deleted, taken or out of range)");
                return;
            }
            if (!mObjectDone)
            {
                const Props* pr = props(mRootId);
                // The region keeps next-owner bits within the base mask.
                if (!pr || (pr->next_owner & PERM_CMT) != (mWant & pr->base & PERM_CMT))
                {
                    if (now >= mObjectUntil)
                    {
                        fail(IDMCP_ERR_TIMEOUT, "the region didn't confirm the next-owner permissions in time",
                             timeoutData());
                        return;
                    }
                    if (now - mPropsAskedAt >= 1.0) requestProps({ root });
                    return;
                }
                mNextOwner  = pr->next_owner;
                mObjectDone = true;
                if (!mContents)
                {
                    succeed(result());
                    return;
                }
                mFetch.start(root);
                return;
            }

            if (!mFetch.done()) return;
            if (mFetch.failed())
            {
                fail(IDMCP_ERR_NOT_FOUND, "the region didn't return the object's contents");
                return;
            }
            // Commit point for the contents step: a restriction may have arrived meanwhile.
            if (failIfDenied(rlvEdit(root))) return;
            const std::vector<LLPointer<LLViewerInventoryItem>> items = mFetch.items();
            // updateInventory() below calls listeners back with local guesses; stop listening first.
            mFetch.stop();
            for (const LLPointer<LLViewerInventoryItem>& item : items)
            {
                mItemResults.push_back(update_item(root, item.get()));
            }
            succeed(result());
        }

        void teardown() override { mFetch.stop(); }

        boost::json::object timeoutData() const override
        {
            boost::json::object sent;
            sent["next_owner"] = maskJson(mWant);
            boost::json::object data;
            data["sent"] = std::move(sent);
            if (const Props* pr = props(mRootId))
            {
                boost::json::object cur;
                cur["next_owner"] = maskJson(pr->next_owner);
                cur["base"]       = maskJson(pr->base);
                data["current"]   = std::move(cur);
            }
            return data;
        }

    private:
        // The Contents tab's next-owner update (llsidepaneliteminfo.cpp:1038-1120).
        boost::json::object update_item(LLViewerObject* root, LLViewerInventoryItem* item) const
        {
            boost::json::object r;
            r["item_id"] = item->getUUID().asString();
            r["name"]    = item->getName();
            LLPermissions perm(item->getPermissions());
            if (!perm.setNextOwnerBits(gAgent.getID(), LLUUID::null, true, mWant))
            {
                r["result"] = "permission";   // you don't own the item
                return r;
            }
            if (PERM_CMT & ~mWant)
            {
                perm.setNextOwnerBits(gAgent.getID(), LLUUID::null, false, PERM_CMT & ~mWant);
            }
            if (perm == item->getPermissions())
            {
                r["result"] = "unchanged";
                return r;
            }
            LLPointer<LLViewerInventoryItem> new_item = new LLViewerInventoryItem(item);
            new_item->setPermissions(perm);
            if (item->getType() == LLAssetType::AT_OBJECT)
            {
                // Apply the new next-owner bits when the object is next rezzed.
                new_item->setFlags(new_item->getFlags() | LLInventoryItemFlags::II_FLAGS_OBJECT_SLAM_PERM);
            }
            root->updateInventory(new_item.get(), TASK_INVENTORY_ITEM_KEY, false);
            r["result"]     = "updated";
            r["next_owner"] = maskJson(perm.getMaskNextOwner());
            return r;
        }

        boost::json::object result() const
        {
            boost::json::object o;
            o["object_id"]  = mRootId.asString();
            o["next_owner"] = maskJson(mNextOwner);
            if (mContents) o["contents"] = mItemResults;
            return o;
        }

        LLUUID             mRootId;
        U32                mWant        = 0;
        bool               mContents    = false;
        bool               mObjectDone  = false;
        U32                mNextOwner   = 0;
        F64                mObjectUntil = 0.0;
        ContentsFetch      mFetch;
        boost::json::array mItemResults;
    };

    void run_set_permissions(const boost::json::object& args, const IDMCPCallPtr& call)
    {
        Target t;
        if (!resolve(args, call, t)) return;
        if (!t.root->permYouOwner())
        {
            idmcp_tool_err(call, IDMCP_ERR_PERMISSION, "only the owner can set next-owner permissions");
            return;
        }
        if (!requireModify(t, call)) return;
        auto it = args.find("next_owner");
        if (it == args.end() || !it->value().is_object())
        {
            idmcp_tool_err(call, IDMCP_ERR_INVALID_PARAMS, "next_owner must be {copy, modify, transfer}");
            return;
        }
        const boost::json::object& no = it->value().as_object();
        U32 want = 0;
        const std::pair<const char*, U32> bits[] = {
            { "copy", PERM_COPY }, { "modify", PERM_MODIFY }, { "transfer", PERM_TRANSFER } };
        for (const auto& [key, bit] : bits)
        {
            auto b = no.find(key);
            if (b == no.end() || !b->value().is_bool())
            {
                idmcp_tool_err(call, IDMCP_ERR_INVALID_PARAMS,
                               "next_owner needs copy, modify and transfer, each true or false");
                return;
            }
            if (b->value().as_bool()) want |= bit;
        }
        if (!(want & (PERM_COPY | PERM_TRANSFER)))
        {
            idmcp_tool_err(call, IDMCP_ERR_INVALID_PARAMS,
                           "the next owner needs copy or transfer (the region turns transfer on otherwise)");
            return;
        }
        auto job = std::make_shared<PermsJob>(call, t.root, want, argBool(args, "contents", false));
        Job::launch(job);
        job->begin();
    }

    // ---- object.link / object.unlink ----------------------------------------------

    constexpr F64 LINK_LIMIT = 15.0;

    IDMCPGateResult gate_link(const boost::json::object& args, IDMCPGatePhase)
    {
        auto it = args.find("object_ids");
        if (it == args.end() || !it->value().is_array()) return IDMCPGateResult();
        for (const boost::json::value& v : it->value().as_array())
        {
            if (!v.is_string()) continue;
            boost::json::object one;
            one["object_id"] = v;
            IDMCPGateResult g = rlvEdit(findRoot(one));
            if (!g.allowed) return g;
        }
        return IDMCPGateResult();
    }

    class LinkJob : public Job
    {
    public:
        LinkJob(const IDMCPCallPtr& call, std::vector<LLUUID> roots)
            : Job(call, LINK_LIMIT), mRoots(std::move(roots)) {}

    protected:
        void tick(F64) override
        {
            LLViewerObject* root = gObjectList.findObject(mRoots[0]);
            if (!root || root->isDead())
            {
                fail(IDMCP_ERR_NOT_FOUND, "the root object is gone");
                return;
            }
            for (size_t i = 1; i < mRoots.size(); ++i)
            {
                LLViewerObject* o = gObjectList.findObject(mRoots[i]);
                if (!o || o->getRootEdit() != root) return;
            }
            boost::json::object out;
            out["object_id"]  = root->getID().asString();
            out["link_count"] = (S32)linksOf(root).size();
            succeed(std::move(out));
        }

        std::string timeoutMessage() const override
        {
            return "the region didn't link the objects in time (too many prims, too far apart, or not allowed)";
        }

        boost::json::object timeoutData() const override
        {
            boost::json::array ids;
            for (const LLUUID& id : mRoots) ids.emplace_back(id.asString());
            boost::json::object sent;
            sent["object_ids"] = std::move(ids);
            boost::json::object data;
            data["sent"] = std::move(sent);
            return data;
        }

    private:
        std::vector<LLUUID> mRoots;   // [0] becomes the root
    };

    void run_link(const boost::json::object& args, const IDMCPCallPtr& call)
    {
        auto it = args.find("object_ids");
        if (it == args.end() || !it->value().is_array())
        {
            idmcp_tool_err(call, IDMCP_ERR_INVALID_PARAMS, "object_ids must be a list of at least two object UUIDs");
            return;
        }
        std::vector<LLViewerObject*> roots;
        for (const boost::json::value& v : it->value().as_array())
        {
            const std::string sid = v.is_string() ? std::string(v.as_string().c_str()) : std::string();
            if (!isUuid(sid))
            {
                idmcp_tool_err(call, IDMCP_ERR_INVALID_PARAMS, "object_ids must hold object UUIDs");
                return;
            }
            LLViewerObject* o = gObjectList.findObject(LLUUID(sid));
            if (!o || o->isDead() || o->getPCode() != LL_PCODE_VOLUME)
            {
                idmcp_tool_err(call, IDMCP_ERR_NOT_FOUND, "object not found or out of range: " + sid);
                return;
            }
            LLViewerObject* r = o->getRootEdit();
            if (std::find(roots.begin(), roots.end(), r) == roots.end()) roots.push_back(r);
        }
        if (roots.size() < 2)
        {
            idmcp_tool_err(call, IDMCP_ERR_INVALID_PARAMS, "object_ids must name at least two separate objects");
            return;
        }
        if (roots.size() > MAX_IDS_PER_PACKET)
        {
            idmcp_tool_err(call, IDMCP_ERR_INVALID_PARAMS, "too many objects in one link");
            return;
        }
        std::vector<U32>    local_ids;
        std::vector<LLUUID> ids;
        for (LLViewerObject* r : roots)
        {
            if (r->isAttachment())
            {
                idmcp_tool_err(call, IDMCP_ERR_INVALID_PARAMS, "worn attachments can't be linked");
                return;
            }
            if (r->getRegion() != roots[0]->getRegion())
            {
                idmcp_tool_err(call, IDMCP_ERR_INVALID_PARAMS, "all objects must be in the same region");
                return;
            }
            if (!r->permYouOwner() || !r->permModify())
            {
                idmcp_tool_err(call, IDMCP_ERR_PERMISSION,
                               "you must own and be able to modify every object: " + r->getID().asString());
                return;
            }
            local_ids.push_back(r->getLocalID());
            ids.push_back(r->getID());
        }
        // The first local id becomes the root.
        sendLocalIds(_PREHASH_ObjectLink, roots[0]->getRegion(), local_ids, true);
        Job::launch(std::make_shared<LinkJob>(call, std::move(ids)));
    }

    class UnlinkJob : public Job
    {
    public:
        UnlinkJob(const IDMCPCallPtr& call, std::vector<LLUUID> targets, std::vector<LLUUID> rest)
            : Job(call, LINK_LIMIT), mTargets(std::move(targets)), mRest(std::move(rest)) {}

    protected:
        void tick(F64) override
        {
            for (const LLUUID& id : mTargets)
            {
                LLViewerObject* o = gObjectList.findObject(id);
                if (!o || o->isDead())
                {
                    fail(IDMCP_ERR_NOT_FOUND, "a prim being unlinked is gone");
                    return;
                }
                // Done when each target is a root with no other prims.
                if (!o->isRootEdit() || linksOf(o).size() != 1) return;
            }
            boost::json::array ids;
            for (const LLUUID& id : mTargets) ids.emplace_back(id.asString());
            for (const LLUUID& id : mRest)
            {
                // The prims left behind form one object; report its root.
                LLViewerObject* o = gObjectList.findObject(id);
                if (o && !o->isDead())
                {
                    ids.emplace_back(o->getRootEdit()->getID().asString());
                    break;
                }
            }
            boost::json::object out;
            out["object_ids"] = std::move(ids);
            succeed(std::move(out));
        }

        std::string timeoutMessage() const override { return "the region didn't unlink the prims in time"; }

    private:
        std::vector<LLUUID> mTargets;
        std::vector<LLUUID> mRest;
    };

    void run_unlink(const boost::json::object& args, const IDMCPCallPtr& call)
    {
        Target t;
        if (!resolve(args, call, t) || !requireModify(t, call)) return;
        if (t.root->isAttachment())
        {
            idmcp_tool_err(call, IDMCP_ERR_INVALID_PARAMS, "worn attachments can't be unlinked");
            return;
        }
        const std::vector<LLViewerObject*> links = linksOf(t.root);
        if (links.size() < 2)
        {
            idmcp_tool_err(call, IDMCP_ERR_INVALID_PARAMS, "the object is a single prim; there is nothing to unlink");
            return;
        }
        const std::vector<LLViewerObject*> targets = t.link ? std::vector<LLViewerObject*>{ t.prim } : links;
        std::vector<U32>    local_ids;
        std::vector<LLUUID> target_ids;
        std::vector<LLUUID> rest_ids;
        for (LLViewerObject* p : links)
        {
            if (std::find(targets.begin(), targets.end(), p) == targets.end()) rest_ids.push_back(p->getID());
        }
        for (LLViewerObject* p : targets)
        {
            // As LLSelectMgr::sendDelink: a root can't have physics shape "None".
            if (p->permModify() && p->getPhysicsShapeType() == LLViewerObject::PHYSICS_SHAPE_NONE)
            {
                p->setPhysicsShapeType(LLViewerObject::PHYSICS_SHAPE_CONVEX_HULL);
                p->updateFlags();
            }
            local_ids.push_back(p->getLocalID());
            target_ids.push_back(p->getID());
        }
        sendLocalIds(_PREHASH_ObjectDelink, t.root->getRegion(), local_ids);
        Job::launch(std::make_shared<UnlinkJob>(call, std::move(target_ids), std::move(rest_ids)));
    }
}
```

- [ ] **Step 3: Register the four tools**

In `idmcp_register_object_tools`, after the `object.get` block's closing brace, add:

```cpp
    // object.edit --------------------------------------------------------------
    {
        IDMCPTool t;
        t.name = "object.edit";
        t.description =
            "Move, rotate, scale, rename or describe an object you can modify. "
            "{\"object_id\"}* is any prim of the object; {\"link\"} picks one prim (link "
            "numbers from object.get). Optional: {\"position\"} [x,y,z] metres, {\"rotation\"} "
            "[x,y,z] degrees, {\"scale\"} [x,y,z] metres, {\"name\"}, {\"description\"}. "
            "Without link, position and rotation move the whole object; with link, only "
            "that prim. scale always changes one prim: the root, or the one named by link. "
            "Frames as in object.get: region for a rezzed root, attachment point for a worn "
            "root, the root for a child. Waits up to 10 s for the region to confirm and "
            "returns the values the object now reports. Blocked by RLV @edit, @editobj, "
            "@editworld, @editattach and @interact.";
        t.input_schema = boost::json::parse(
            R"({"type":"object","properties":{"object_id":{"type":"string"},"link":{"type":"integer"},)"
            R"("position":{"type":"array","items":{"type":"number"},"minItems":3,"maxItems":3},)"
            R"("rotation":{"type":"array","items":{"type":"number"},"minItems":3,"maxItems":3},)"
            R"("scale":{"type":"array","items":{"type":"number"},"minItems":3,"maxItems":3},)"
            R"("name":{"type":"string"},"description":{"type":"string"}},)"
            R"("required":["object_id"],"additionalProperties":false})");
        t.gate    = gateEdit;
        t.timeout = 20.0;
        t.invoke  = [](const boost::json::object& args, const IDMCPCallPtr& call) { run_edit(args, call); };
        reg.add(std::move(t));
    }

    // object.setPermissions ----------------------------------------------------
    {
        IDMCPTool t;
        t.name = "object.setPermissions";
        t.description =
            "Set the next-owner permissions of an object you own and can modify. "
            "{\"object_id\"}*, {\"next_owner\"}*: {copy, modify, transfer} booleans; the next "
            "owner needs copy or transfer. {\"contents\":true} also sets the same next-owner "
            "permissions on every item inside the root prim. Returns {object_id, next_owner, "
            "contents:[{item_id, name, result}]} where result is updated, unchanged or "
            "permission. Bits the object's base permissions lack stay off. Blocked by RLV "
            "@edit, @editobj, @editworld, @editattach and @interact.";
        t.input_schema = boost::json::parse(
            R"({"type":"object","properties":{"object_id":{"type":"string"},)"
            R"("next_owner":{"type":"object","properties":{"copy":{"type":"boolean"},"modify":{"type":"boolean"},"transfer":{"type":"boolean"}},)"
            R"("required":["copy","modify","transfer"],"additionalProperties":false},)"
            R"("contents":{"type":"boolean"}},"required":["object_id","next_owner"],"additionalProperties":false})");
        t.gate    = gateEdit;
        t.timeout = 45.0;
        t.invoke  = [](const boost::json::object& args, const IDMCPCallPtr& call) { run_set_permissions(args, call); };
        reg.add(std::move(t));
    }

    // object.link --------------------------------------------------------------
    {
        IDMCPTool t;
        t.name = "object.link";
        t.description =
            "Link two or more rezzed objects you own and can modify into one. "
            "{\"object_ids\"}*: the objects (any prim of each); the first becomes the root. "
            "All must be in the same region. Returns {object_id (the root), link_count}. The "
            "region refuses links that are too large or too spread out; that shows as a "
            "timeout. Blocked by RLV @edit, @editobj, @editworld and @interact.";
        t.input_schema = boost::json::parse(
            R"({"type":"object","properties":{"object_ids":{"type":"array","items":{"type":"string"},"minItems":2}},)"
            R"("required":["object_ids"],"additionalProperties":false})");
        t.gate    = gate_link;
        t.timeout = 25.0;
        t.invoke  = [](const boost::json::object& args, const IDMCPCallPtr& call) { run_link(args, call); };
        reg.add(std::move(t));
    }

    // object.unlink ------------------------------------------------------------
    {
        IDMCPTool t;
        t.name = "object.unlink";
        t.description =
            "Unlink a rezzed object you can modify. {\"object_id\"}*; {\"link\"} unlinks only "
            "that prim (link numbers from object.get). Without link, every prim becomes its "
            "own object. Returns {object_ids}: the unlinked prims, then the root of whatever "
            "is left. Blocked by RLV @edit, @editobj, @editworld and @interact.";
        t.input_schema = boost::json::parse(
            R"({"type":"object","properties":{"object_id":{"type":"string"},"link":{"type":"integer"}},)"
            R"("required":["object_id"],"additionalProperties":false})");
        t.gate    = gateEdit;
        t.timeout = 25.0;
        t.invoke  = [](const boost::json::object& args, const IDMCPCallPtr& call) { run_unlink(args, call); };
        reg.add(std::move(t));
    }
```

- [ ] **Step 4: Compile-read checklist**

Read each of these in the source to confirm:
- `MAX_IDS_PER_PACKET` and `pack_agent` are in the first anonymous namespace of this file (Task 1), so the new anonymous block sees them.
- `_PREHASH_ObjectName`, `_PREHASH_ObjectDescription`, `_PREHASH_LocalID`, `_PREHASH_ObjectPermissions`, `_PREHASH_HeaderData`, `_PREHASH_Override`, `_PREHASH_Field`, `_PREHASH_Set`, `_PREHASH_Mask`, `_PREHASH_ObjectLink`, `_PREHASH_ObjectDelink` exist in `llmessage/message_prehash.h`.
- `PERM_NEXT_OWNER` is a `constexpr U8` (`llinventory/llpermissionsflags.h:85`); `addU8Fast` takes it directly.
- `LLPermissions::setNextOwnerBits(const LLUUID&, const LLUUID&, bool, PermissionMask)` returns the ownership check (`llpermissions.h:253`), and `operator==` exists (`:314`).
- `LLViewerInventoryItem(const LLViewerInventoryItem*)` exists (`llviewerinventory.h:116`); `setFlags(U32)` is public on `LLInventoryItem` (`llinventory.h:185`).
- `LLInventoryItemFlags::II_FLAGS_OBJECT_SLAM_PERM` (`llinventorydefines.h:61`) and `TASK_INVENTORY_ITEM_KEY` (`:31`).
- `LLViewerObject::updateInventory(LLViewerInventoryItem*, U8, bool)` (`llviewerobject.h:559`), `getPhysicsShapeType()` (`:630`), `updateFlags(bool = false)` (`:651`), `setPhysicsShapeType(U8)` (`:655`), and the `PHYSICS_SHAPE_NONE`/`PHYSICS_SHAPE_CONVEX_HULL` enumerators (`:792-793`).
- `LLWorld::getRegionMinPrimScale()`/`getRegionMaxPrimScale()` (`llworld.h:129-130`), `LLViewerRegion::getWidth()` (`llviewerregion.h:259`).
- `DB_INV_ITEM_NAME_STR_LEN` (63) and `DB_INV_ITEM_DESC_STR_LEN` (127) are `const S32` in `llmessage/lldbstrings.h:41,47`.
- `t.gate = gateEdit;` assigns a function whose signature matches `std::function<IDMCPGateResult(const boost::json::object&, IDMCPGatePhase)>`.
- No new identifier is on the X11 list.

- [ ] **Step 5: Commit**

```bash
git add indra/newview/idmcptools_object.cpp
git commit -m "feat: add object.edit, setPermissions, link and unlink MCP tools"
```

---

### Task 3: `object.setFaces`

Creates `idmcptools_faces.cpp`. The tool is synchronous: it applies the drop rules once per prim, changes each face locally, sends one `sendTEUpdate()` per prim, flushes queued material changes, and answers with a result per face.

**Files:**
- Create: `indra/newview/idmcptools_faces.cpp`
- Modify: `indra/newview/idmcptools.h` (after the `idmcp_register_object_tools` line from Task 1)
- Modify: `indra/newview/idmcpserver.cpp` (after the `idmcp_register_object_tools(mRegistry);` line from Task 1)
- Modify: `indra/newview/CMakeLists.txt` (after the `idmcptools_object.cpp` line from Task 1)

**Interfaces:**
- Consumes (Task 1, `namespace idmcp_obj`): `Target`, `resolve`, `requireModify`, `linksOf`, `linkNumberOf`, `agentItem`, `inLibrary`, `isUuid`, `lower`, `argStr`, `argNum`, `argNums`, `gateEdit`.
- Produces: `void idmcp_register_faces_tools(IDMCPToolRegistry&)` and the registered tool `object.setFaces`.

- [ ] **Step 1: Create the file**

Create `indra/newview/idmcptools_faces.cpp`:

```cpp
/**
 * @file idmcptools_faces.cpp
 * @brief <ID> MCP server: object.setFaces (texture, material, color and mapping per face).
 *
 * Part of Five's custom Firestorm fork. Custom code carries an `ID` prefix.
 *
 * Follows the texture and material drag-and-drop paths without calling
 * dropTextureOneFace/dropMaterialOneFace: those send one update per face and
 * would rerun the drop rules against an item a no-copy drop already deleted.
 * Here the public handleDropMaterialProtections runs once per prim, the faces
 * change locally, and each prim sends one sendTEUpdate(). Synchronous; main
 * thread only. Never touches the user's selection.
 */

#include "llviewerprecompiledheaders.h"

#include "idmcptools_object.h"
#include "idmcptools.h"
#include "idmcpserver.h"

#include "llgltfmateriallist.h"     // flushUpdates
#include "llinventorymodel.h"       // gInventory
#include "lltextureentry.h"
#include "lltooldraganddrop.h"      // handleDropMaterialProtections (public static)
#include "llviewerinventory.h"
#include "llviewerobject.h"
#include "llviewertexture.h"        // LLViewerTextureManager
#include "v4color.h"

#include <vector>

using namespace idmcp_obj;

namespace
{
    // A texture or material argument: an inventory item (drop rules apply) or
    // any other UUID, applied directly as an asset.
    struct AssetArg
    {
        bool   given = false;
        bool   clear = false;   // material "none"
        LLUUID item_id;         // set when the id is an inventory item
        LLUUID asset_id;        // set when it isn't
        LLToolDragAndDrop::ESource source = LLToolDragAndDrop::SOURCE_AGENT;
    };

    bool parse_asset(const boost::json::object& args, const char* key, LLAssetType::EType type,
                     bool allow_none, const IDMCPCallPtr& call, AssetArg& out)
    {
        if (!args.contains(key)) return true;
        out.given = true;
        const std::string s = argStr(args, key);
        if (allow_none && lower(s) == "none")
        {
            out.clear = true;
            return true;
        }
        if (!isUuid(s))
        {
            idmcp_tool_err(call, IDMCP_ERR_INVALID_PARAMS,
                           std::string(key) + (allow_none ? " must be a UUID or \"none\"" : " must be a UUID"));
            return false;
        }
        const LLUUID id(s);
        if (LLViewerInventoryItem* item = agentItem(id))
        {
            if (item->getType() != type)
            {
                idmcp_tool_err(call, IDMCP_ERR_INVALID_PARAMS,
                               std::string(key) + ": that inventory item isn't a " + key);
                return false;
            }
            if (!item->isFinished())
            {
                idmcp_tool_err(call, IDMCP_ERR_INVALID_PARAMS,
                               std::string(key) + ": the item hasn't finished loading; try again");
                return false;
            }
            out.item_id = item->getUUID();
            out.source  = inLibrary(out.item_id) ? LLToolDragAndDrop::SOURCE_LIBRARY
                                                 : LLToolDragAndDrop::SOURCE_AGENT;
        }
        else
        {
            out.asset_id = id;
        }
        return true;
    }

    // The asset to apply on prim. For an inventory item, runs the drop rules
    // once for this prim; a no-copy item moves into the prim's contents, so a
    // later prim finds it gone.
    bool prepare_asset(LLViewerObject* prim, const AssetArg& a, LLUUID& asset, std::string& error)
    {
        if (a.clear)
        {
            asset.setNull();
            return true;
        }
        if (a.item_id.isNull())
        {
            asset = a.asset_id;
            return true;
        }
        LLViewerInventoryItem* item = gInventory.getItem(a.item_id);
        if (!item)
        {
            error = "the item is no longer in your inventory (a no-copy item moves into the first prim it's applied to)";
            return false;
        }
        // Save the asset id first: the protections can delete `item` (SL-20013).
        asset = item->getAssetUUID();
        if (!LLToolDragAndDrop::handleDropMaterialProtections(prim, item, a.source, LLUUID::null))
        {
            error = "the object won't accept this item; check the item's and the object's permissions";
            return false;
        }
        return true;
    }

    void run_set_faces(const boost::json::object& args, const IDMCPCallPtr& call)
    {
        Target t;
        if (!resolve(args, call, t) || !requireModify(t, call)) return;
        auto bad = [&call](const std::string& m) { idmcp_tool_err(call, IDMCP_ERR_INVALID_PARAMS, m); };

        bool all = false;
        std::vector<S32> faces;
        auto fit = args.find("faces");
        if (fit != args.end() && fit->value().is_string()
            && lower(std::string(fit->value().as_string().c_str())) == "all")
        {
            all = true;
        }
        else if (fit != args.end() && fit->value().is_array() && !fit->value().as_array().empty())
        {
            for (const boost::json::value& v : fit->value().as_array())
            {
                if (!v.is_int64())
                {
                    bad("faces must be \"all\" or a list of face numbers");
                    return;
                }
                faces.push_back((S32)v.as_int64());
            }
        }
        else
        {
            bad("faces must be \"all\" or a list of face numbers");
            return;
        }

        AssetArg tex, mat;
        if (!parse_asset(args, "texture", LLAssetType::AT_TEXTURE, false, call, tex)) return;
        if (!parse_asset(args, "material", LLAssetType::AT_MATERIAL, true, call, mat)) return;
        if (tex.given && mat.given && !mat.clear)
        {
            bad("pass texture or material, not both (a material carries its own textures)");
            return;
        }

        F32 color[3] = { 0.f, 0.f, 0.f };
        const bool has_color = args.contains("color");
        if (has_color)
        {
            if (!argNums(args, "color", 3, color)) { bad("color must be [r, g, b], each 0 to 1"); return; }
            for (F32 c : color)
            {
                if (c < 0.f || c > 1.f) { bad("color must be [r, g, b], each 0 to 1"); return; }
            }
        }
        F64 alpha = 1.0;
        const bool has_alpha = args.contains("alpha");
        if (has_alpha && (!argNum(args, "alpha", alpha) || alpha < 0.0 || alpha > 1.0))
        {
            bad("alpha must be a number from 0 to 1");
            return;
        }
        F32 repeats[2] = { 1.f, 1.f };
        const bool has_repeats = args.contains("repeats");
        if (has_repeats && !argNums(args, "repeats", 2, repeats)) { bad("repeats must be [u, v]"); return; }
        F32 offset[2] = { 0.f, 0.f };
        const bool has_offset = args.contains("offset");
        if (has_offset && !argNums(args, "offset", 2, offset)) { bad("offset must be [u, v]"); return; }
        F64 rot_deg = 0.0;
        const bool has_rot = args.contains("rotation");
        if (has_rot && !argNum(args, "rotation", rot_deg)) { bad("rotation must be a number of degrees"); return; }

        if (!(tex.given || mat.given || has_color || has_alpha || has_repeats || has_offset || has_rot))
        {
            bad("nothing to change: pass texture, material, color, alpha, repeats, offset or rotation");
            return;
        }

        // "all" without link covers every prim; a face list without link means the root.
        const std::vector<LLViewerObject*> prims =
            (all && t.link == 0) ? linksOf(t.root) : std::vector<LLViewerObject*>{ t.prim };

        boost::json::array results;
        bool any_applied  = false;
        bool any_material = false;
        for (LLViewerObject* prim : prims)
        {
            const S32 link = linkNumberOf(prim);
            const S32 n    = prim->getNumTEs();
            std::vector<S32> todo = faces;
            if (all)
            {
                todo.clear();
                for (S32 f = 0; f < n; ++f) todo.push_back(f);
            }
            bool any_valid = false;
            for (S32 f : todo) any_valid = any_valid || (f >= 0 && f < n);

            // Drop rules once per prim, and only when a face here will use the item.
            LLUUID      tex_asset, mat_asset;
            std::string tex_err, mat_err;
            const bool tex_ok = !tex.given || !any_valid || prepare_asset(prim, tex, tex_asset, tex_err);
            const bool mat_ok = !mat.given || !any_valid || prepare_asset(prim, mat, mat_asset, mat_err);
            if (mat.given && mat_ok && !mat.clear && mat.item_id.notNull() && mat_asset.isNull())
            {
                mat_asset = BLANK_MATERIAL_ASSET_ID;   // as dropMaterialOneFace
            }

            bool touched = false;
            for (S32 f : todo)
            {
                boost::json::object r;
                r["link"] = link;
                r["face"] = f;
                if (f < 0 || f >= n)
                {
                    r["ok"]    = false;
                    r["error"] = llformat("no such face (this prim has %d)", n);
                    results.push_back(std::move(r));
                    continue;
                }
                const U8    te = (U8)f;
                std::string error;
                bool        applied = false;
                if (tex.given)
                {
                    if (!tex_ok)
                    {
                        error = "texture: " + tex_err;
                    }
                    else
                    {
                        // As a texture drop with remove_pbr: a PBR material would hide the texture.
                        if (prim->getRenderMaterialID(te).notNull())
                        {
                            prim->setRenderMaterialID(f, LLUUID::null);
                            any_material = true;
                        }
                        prim->setTEImage(te, LLViewerTextureManager::getFetchedTexture(tex_asset));
                        applied = true;
                    }
                }
                if (mat.given)
                {
                    if (!mat_ok)
                    {
                        error = "material: " + mat_err;
                    }
                    else
                    {
                        prim->setRenderMaterialID(f, mat_asset);   // queues the region update itself
                        any_material = true;
                        applied      = true;
                    }
                }
                if (has_color || has_alpha)
                {
                    // Start from the face's color: setTEColor(LLColor3) would reset alpha to 1.
                    LLColor4 c = prim->getTE(te)->getColor();
                    if (has_color)
                    {
                        c.mV[VRED]   = color[0];
                        c.mV[VGREEN] = color[1];
                        c.mV[VBLUE]  = color[2];
                    }
                    if (has_alpha) c.mV[VALPHA] = (F32)alpha;
                    prim->setTEColor(te, c);
                    applied = true;
                }
                if (has_repeats)
                {
                    prim->setTEScale(te, repeats[0], repeats[1]);
                    applied = true;
                }
                if (has_offset)
                {
                    prim->setTEOffset(te, offset[0], offset[1]);
                    applied = true;
                }
                if (has_rot)
                {
                    prim->setTERotation(te, (F32)(rot_deg * DEG_TO_RAD));
                    applied = true;
                }
                r["ok"] = error.empty();
                if (!error.empty()) r["error"] = error;
                any_applied = any_applied || applied;
                touched     = touched || applied;
                results.push_back(std::move(r));
            }
            if (touched) prim->sendTEUpdate();   // one ObjectImage per prim
        }
        if (any_material) LLGLTFMaterialList::flushUpdates();

        if (!any_applied)
        {
            boost::json::object data;
            data["faces"] = std::move(results);
            idmcp_tool_err(call, IDMCP_ERR_INVALID_PARAMS, "nothing could be applied", std::move(data));
            return;
        }
        boost::json::object out;
        out["object_id"] = t.root->getID().asString();
        out["faces"]     = std::move(results);
        idmcp_tool_ok(call, std::move(out));
    }
}

// ---------------------------------------------------------------------------

void idmcp_register_faces_tools(IDMCPToolRegistry& reg)
{
    // object.setFaces ------------------------------------------------------------
    {
        IDMCPTool t;
        t.name = "object.setFaces";
        t.description =
            "Texture, color and map the faces of an object you can modify. {\"object_id\"}*, "
            "{\"faces\"}*: \"all\" or a list of face numbers, {\"link\"} (link numbers from "
            "object.get). \"all\" without link covers every prim; a face list without link "
            "means the root prim. Optional: {\"texture\"} and {\"material\"}: an inventory item "
            "id (the usual drop rules apply, so a no-copy item moves into the prim's contents) "
            "or a raw asset UUID; material \"none\" removes a PBR material, and a texture also "
            "removes one. {\"color\"} [r,g,b] 0-1, {\"alpha\"} 0-1, {\"repeats\"} [u,v], "
            "{\"offset\"} [u,v], {\"rotation\"} degrees. Returns {object_id, faces:[{link, face, "
            "ok, error}]}. Blocked by RLV @edit, @editobj, @editworld, @editattach and @interact.";
        t.input_schema = boost::json::parse(
            R"({"type":"object","properties":{"object_id":{"type":"string"},"link":{"type":"integer"},)"
            R"("faces":{"oneOf":[{"type":"string","enum":["all"]},{"type":"array","items":{"type":"integer"},"minItems":1}]},)"
            R"("texture":{"type":"string"},"material":{"type":"string"},)"
            R"("color":{"type":"array","items":{"type":"number"},"minItems":3,"maxItems":3},)"
            R"("alpha":{"type":"number"},)"
            R"("repeats":{"type":"array","items":{"type":"number"},"minItems":2,"maxItems":2},)"
            R"("offset":{"type":"array","items":{"type":"number"},"minItems":2,"maxItems":2},)"
            R"("rotation":{"type":"number"}},)"
            R"("required":["object_id","faces"],"additionalProperties":false})");
        t.gate   = gateEdit;
        t.invoke = [](const boost::json::object& args, const IDMCPCallPtr& call) { run_set_faces(args, call); };
        reg.add(std::move(t));
    }
}
```

- [ ] **Step 2: Register the tool area**

In `indra/newview/idmcptools.h`, after `void idmcp_register_object_tools(IDMCPToolRegistry& reg);`, add:

```cpp
void idmcp_register_faces_tools(IDMCPToolRegistry& reg);
```

In `indra/newview/idmcpserver.cpp`, after `    idmcp_register_object_tools(mRegistry);`, add:

```cpp
    idmcp_register_faces_tools(mRegistry);
```

In `indra/newview/CMakeLists.txt`, after `    idmcptools_object.cpp  # <ID> Embedded MCP server: object build tools`, add:

```cmake
    idmcptools_faces.cpp  # <ID> Embedded MCP server: object.setFaces
```

- [ ] **Step 3: Compile-read checklist**

Read each of these in the source to confirm:
- `LLToolDragAndDrop::handleDropMaterialProtections(LLViewerObject*, LLInventoryItem*, ESource, const LLUUID&)` is public static (`lltooldraganddrop.h:97-100`), and `ESource` with `SOURCE_AGENT`/`SOURCE_LIBRARY` is public (`:61-69`).
- `LLViewerObject::setTEImage(const U8, LLViewerTexture*)` (`llviewerobject.h:427`), `setTEColor(const U8, const LLColor4&)` (`:401`), `setTEScale(const U8, F32, F32)` (`:402`), `setTEOffset` (`:405`), `setTERotation(const U8, F32)` in radians (`:408`), `setRenderMaterialID(S32, const LLUUID&, bool = true, bool = true)` (`:212`), `getRenderMaterialID(U8) const` (`:206`), `sendTEUpdate() const` (`:440`).
- `LLViewerTextureManager::getFetchedTexture(const LLUUID&, ...)` returns `LLViewerFetchedTexture*`, which converts to `LLViewerTexture*` (`llviewertexture.h:669`).
- `LLGLTFMaterialList::flushUpdates(void(*)(bool) = nullptr)` is public static (`llgltfmateriallist.h:84`).
- `BLANK_MATERIAL_ASSET_ID` is declared in `llcommon/indra_constants.h:265`, which the PCH includes. If the build says otherwise, add `#include "indra_constants.h"`.
- `LLTextureEntry::getColor()` returns `const LLColor4&` (`lltextureentry.h:139`).
- `DEG_TO_RAD` comes from `llmath.h` (PCH).
- No identifier on the X11 list (`AssetArg`, `prepare_asset`, `parse_asset` are fine).

- [ ] **Step 4: Commit**

```bash
git add indra/newview/idmcptools_faces.cpp indra/newview/idmcptools.h indra/newview/idmcpserver.cpp indra/newview/CMakeLists.txt
git commit -m "feat: add object.setFaces MCP tool"
```

---

### Task 4: `object.contents` and `object.addContents`

Creates `idmcptools_contents.cpp`. Listing uses Task 1's `ContentsFetch`. Adding lists the contents first as a baseline, adds one item per idle tick, then re-lists every 2 s until each added item shows up or 20 s pass.

**Files:**
- Create: `indra/newview/idmcptools_contents.cpp`
- Modify: `indra/newview/idmcptools.h` (after the `idmcp_register_faces_tools` line)
- Modify: `indra/newview/idmcpserver.cpp` (after `idmcp_register_faces_tools(mRegistry);`)
- Modify: `indra/newview/CMakeLists.txt` (after the `idmcptools_faces.cpp` line)

**Interfaces:**
- Consumes (Task 1, `namespace idmcp_obj`): `Target`, `resolve`, `findRoot`, `agentItem`, `inLibrary`, `agentSittingOn`, `isUuid`, `argBool`, `itemJson`, `rlvEdit`, `ContentsFetch`, `Job` (`launch`, `tick`, `timeoutMessage`, `teardown`, `succeed`, `fail`, `failIfDenied`).
- Produces: `void idmcp_register_contents_tools(IDMCPToolRegistry&)` and the registered tools `object.contents`, `object.addContents`.

- [ ] **Step 1: Create the file**

Create `indra/newview/idmcptools_contents.cpp`:

```cpp
/**
 * @file idmcptools_contents.cpp
 * @brief <ID> MCP server: object.contents and object.addContents (task inventory).
 *
 * Part of Five's custom Firestorm fork. Custom code carries an `ID` prefix.
 *
 * Adding follows dropInventory/dropScript without calling them: they return
 * silently while the user is mid-drag, and dropInventory switches an open
 * Build floater to its Contents tab. The drop rules come from the public
 * isInventoryDropAcceptable. Main thread only; never touches the selection.
 */

#include "llviewerprecompiledheaders.h"

#include "idmcptools_object.h"
#include "idmcptools.h"
#include "idmcpserver.h"
#include "idmcprlvgate.h"

#include "llagent.h"
#include "llinventorydefines.h"     // TASK_INVENTORY_ITEM_KEY
#include "llinventorymodel.h"       // gInventory
#include "lltimer.h"                // time_corrected
#include "lltooldraganddrop.h"      // isInventoryDropAcceptable (public static)
#include "llviewerinventory.h"
#include "llviewerobject.h"
#include "llviewerobjectlist.h"     // gObjectList
#include "rlvactions.h"
#include "rlvdefines.h"
#include "rlvlocks.h"               // gRlvAttachmentLocks

#include <map>
#include <vector>

using namespace idmcp_obj;

namespace
{
    constexpr F64    LIST_LIMIT   = 15.0;   // spec: 15 s
    constexpr F64    ADD_LIMIT    = 45.0;
    constexpr F64    VERIFY_LIMIT = 20.0;
    constexpr F64    REFETCH_GAP  = 2.0;
    constexpr size_t MAX_ITEMS    = 32;

    // As edit, plus willObjectAcceptInventory's RLVa rules, reported as RLV errors.
    IDMCPGateResult rlv_add_contents(LLViewerObject* root)
    {
        IDMCPGateResult g = rlvEdit(root);
        if (!g.allowed || !IDMCPRlvGate::isEnabled() || !root) return g;
        if (gRlvAttachmentLocks.isLockedAttachment(root))
        {
            return IDMCPRlvGate::deny(RLV_BHVR_DETACH, "detach");
        }
        if (agentSittingOn(root))
        {
            if (RlvActions::hasBehaviour(RLV_BHVR_UNSIT)) return IDMCPRlvGate::deny(RLV_BHVR_UNSIT, "unsit");
            if (RlvActions::hasBehaviour(RLV_BHVR_SITTP)) return IDMCPRlvGate::deny(RLV_BHVR_SITTP, "sittp");
        }
        return g;
    }

    IDMCPGateResult gate_add_contents(const boost::json::object& args, IDMCPGatePhase)
    {
        return rlv_add_contents(findRoot(args));
    }

    // ---- object.contents ---------------------------------------------------------

    class ListJob : public Job
    {
    public:
        ListJob(const IDMCPCallPtr& call, const Target& t)
            : Job(call, LIST_LIMIT), mRootId(t.root->getID()), mPrimId(t.prim->getID()), mLink(t.link) {}

        void begin()
        {
            if (LLViewerObject* p = gObjectList.findObject(mPrimId)) mFetch.start(p);
        }

    protected:
        void tick(F64) override
        {
            LLViewerObject* p = gObjectList.findObject(mPrimId);
            if (!p || p->isDead())
            {
                fail(IDMCP_ERR_NOT_FOUND, "the object is gone (deleted, taken or out of range)");
                return;
            }
            if (!mFetch.done()) return;
            if (mFetch.failed())
            {
                fail(IDMCP_ERR_NOT_FOUND, "the region didn't return the object's contents");
                return;
            }
            boost::json::array items;
            for (const LLPointer<LLViewerInventoryItem>& item : mFetch.items())
            {
                items.push_back(itemJson(item.get()));
            }
            boost::json::object out;
            out["object_id"] = mRootId.asString();
            if (mLink) out["link"] = mLink;
            out["items"] = std::move(items);
            succeed(std::move(out));
        }

        void teardown() override { mFetch.stop(); }

        std::string timeoutMessage() const override { return "the region didn't send the object's contents in time"; }

    private:
        LLUUID        mRootId;
        LLUUID        mPrimId;
        S32           mLink = 0;
        ContentsFetch mFetch;
    };

    void run_contents(const boost::json::object& args, const IDMCPCallPtr& call)
    {
        Target t;
        if (!resolve(args, call, t)) return;
        auto job = std::make_shared<ListJob>(call, t);
        Job::launch(job);
        job->begin();
    }

    // ---- object.addContents ------------------------------------------------------

    struct AddEntry
    {
        LLUUID             item_id;
        std::string        name;
        LLAssetType::EType type = LLAssetType::AT_NONE;
        std::string        result;   // "added", "not_found", or why it wasn't sent
        bool               sent = false;
    };

    std::map<S32, S32> count_by_type(const std::vector<LLPointer<LLViewerInventoryItem>>& items)
    {
        std::map<S32, S32> out;
        for (const LLPointer<LLViewerInventoryItem>& item : items)
        {
            ++out[(S32)item->getType()];
        }
        return out;
    }

    class AddJob : public Job
    {
    public:
        AddJob(const IDMCPCallPtr& call, const Target& t, const std::vector<LLUUID>& items, bool running)
            : Job(call, ADD_LIMIT), mRootId(t.root->getID()), mPrimId(t.prim->getID()), mLink(t.link),
              mRunning(running)
        {
            for (const LLUUID& id : items)
            {
                AddEntry e;
                e.item_id = id;
                mEntries.push_back(e);
            }
        }

        void begin()
        {
            if (LLViewerObject* p = gObjectList.findObject(mPrimId)) mFetch.start(p);
        }

    protected:
        void tick(F64 now) override
        {
            LLViewerObject* prim = gObjectList.findObject(mPrimId);
            if (!prim || prim->isDead())
            {
                fail(IDMCP_ERR_NOT_FOUND, "the object is gone (deleted, taken or out of range)");
                return;
            }
            switch (mStep)
            {
            case Step::Baseline:
                if (!mFetch.done()) return;
                if (mFetch.failed())
                {
                    fail(IDMCP_ERR_NOT_FOUND, "the region didn't return the object's contents");
                    return;
                }
                mBaseline = count_by_type(mFetch.items());
                mFetch.stop();
                // Commit point: a restriction may have arrived while the contents loaded.
                if (failIfDenied(rlv_add_contents(prim->getRootEdit()))) return;
                mStep = Step::Adding;
                return;

            case Step::Adding:
                if (mNext < mEntries.size())
                {
                    add_one(prim, mEntries[mNext++]);   // one item per frame, in order
                    return;
                }
                if (std::none_of(mEntries.begin(), mEntries.end(), [](const AddEntry& e) { return e.sent; }))
                {
                    fail(IDMCP_ERR_INVALID_PARAMS, "none of the items could be added", result_data());
                    return;
                }
                mStep        = Step::Verifying;
                mVerifyUntil = now + VERIFY_LIMIT;
                mFetchedAt   = now;
                mFetch.start(prim);
                return;

            case Step::Verifying:
                if (mFetch.done() && !mFetch.failed() && confirm(count_by_type(mFetch.items())))
                {
                    succeed(result_data());
                    return;
                }
                if (now >= mVerifyUntil)
                {
                    for (AddEntry& e : mEntries)
                    {
                        if (e.sent && e.result.empty()) e.result = "not_found";
                    }
                    succeed(result_data());
                    return;
                }
                if (mFetch.done() && now - mFetchedAt >= REFETCH_GAP)
                {
                    mFetchedAt = now;
                    mFetch.start(prim);
                }
                return;
            }
        }

        void teardown() override { mFetch.stop(); }

        std::string timeoutMessage() const override { return "the region didn't send the object's contents in time"; }

    private:
        enum class Step { Baseline, Adding, Verifying };

        // The core of dropInventory/dropScript (lltooldraganddrop.cpp:1828-1885, 2064-2126).
        void add_one(LLViewerObject* prim, AddEntry& e)
        {
            LLViewerInventoryItem* item = agentItem(e.item_id);
            if (!item)
            {
                e.result = "not in your inventory";
                return;
            }
            e.name = item->getName();
            e.type = item->getType();
            if (inLibrary(item->getUUID()))
            {
                e.result = "Library items can't be added; copy the item to your inventory first";
                return;
            }
            if (!item->isFinished())
            {
                e.result = "the item hasn't finished loading; try again";
                return;
            }
            if (!LLToolDragAndDrop::isInventoryDropAcceptable(prim, item))
            {
                e.result = "refused: the object must be yours and modifiable or accept drops, the item must be "
                           "transferable or yours, and worn items can't be added";
                return;
            }
            LLPointer<LLViewerInventoryItem> copy = new LLViewerInventoryItem(item);
            const bool no_copy = !item->getPermissions().allowCopyBy(gAgent.getID());
            if (no_copy)
            {
                // The region moves a no-copy item; drop it locally the way a drag does.
                gInventory.deleteObject(item->getUUID());   // `item` is gone after this line
                gInventory.notifyObservers();
            }
            if (e.type == LLAssetType::AT_LSL_TEXT)
            {
                prim->saveScript(copy.get(), mRunning, true);
            }
            else
            {
                copy->setCreationDate(time_corrected());
                prim->updateInventory(copy.get(), TASK_INVENTORY_ITEM_KEY, true);
            }
            e.sent = true;
        }

        // Matches by asset type, not name: the region renames an item whose name
        // is already taken in the object.
        bool confirm(const std::map<S32, S32>& counts)
        {
            std::map<S32, S32> fresh;
            for (const auto& kv : counts)
            {
                auto b = mBaseline.find(kv.first);
                fresh[kv.first] = kv.second - (b == mBaseline.end() ? 0 : b->second);
            }
            bool all = true;
            for (AddEntry& e : mEntries)
            {
                if (!e.sent) continue;
                S32& left = fresh[(S32)e.type];
                if (left > 0)
                {
                    e.result = "added";
                    --left;
                }
                else
                {
                    e.result.clear();
                    all = false;
                }
            }
            return all;
        }

        boost::json::object result_data() const
        {
            boost::json::array items;
            for (const AddEntry& e : mEntries)
            {
                boost::json::object r;
                r["item_id"] = e.item_id.asString();
                if (!e.name.empty()) r["name"] = e.name;
                r["result"] = e.result;
                items.push_back(std::move(r));
            }
            boost::json::object o;
            o["object_id"] = mRootId.asString();
            if (mLink) o["link"] = mLink;
            o["items"] = std::move(items);
            return o;
        }

        LLUUID                mRootId;
        LLUUID                mPrimId;
        S32                   mLink    = 0;
        bool                  mRunning = true;
        Step                  mStep    = Step::Baseline;
        std::vector<AddEntry> mEntries;
        size_t                mNext        = 0;
        std::map<S32, S32>    mBaseline;
        F64                   mVerifyUntil = 0.0;
        F64                   mFetchedAt   = 0.0;
        ContentsFetch         mFetch;
    };

    void run_add_contents(const boost::json::object& args, const IDMCPCallPtr& call)
    {
        Target t;
        if (!resolve(args, call, t)) return;
        auto it = args.find("item_ids");
        if (it == args.end() || !it->value().is_array() || it->value().as_array().empty())
        {
            idmcp_tool_err(call, IDMCP_ERR_INVALID_PARAMS, "item_ids must be a list of inventory item UUIDs");
            return;
        }
        if (it->value().as_array().size() > MAX_ITEMS)
        {
            idmcp_tool_err(call, IDMCP_ERR_INVALID_PARAMS, llformat("at most %d items per call", (S32)MAX_ITEMS));
            return;
        }
        std::vector<LLUUID> ids;
        for (const boost::json::value& v : it->value().as_array())
        {
            const std::string s = v.is_string() ? std::string(v.as_string().c_str()) : std::string();
            if (!isUuid(s))
            {
                idmcp_tool_err(call, IDMCP_ERR_INVALID_PARAMS, "item_ids must hold inventory item UUIDs");
                return;
            }
            ids.emplace_back(s);
        }
        auto job = std::make_shared<AddJob>(call, t, ids, argBool(args, "running", true));
        Job::launch(job);
        job->begin();
    }
}

// ---------------------------------------------------------------------------

void idmcp_register_contents_tools(IDMCPToolRegistry& reg)
{
    // object.contents --------------------------------------------------------------
    {
        IDMCPTool t;
        t.name = "object.contents";
        t.description =
            "List the items inside an object (its Contents tab). {\"object_id\"}*; {\"link\"} "
            "lists one prim's contents instead of the root's (link numbers from object.get). "
            "Returns {object_id, items:[{item_id, name, type, permissions}]}. The region only "
            "shows contents of objects you can modify.";
        t.input_schema = boost::json::parse(
            R"({"type":"object","properties":{"object_id":{"type":"string"},"link":{"type":"integer"}},)"
            R"("required":["object_id"],"additionalProperties":false})");
        t.timeout = 25.0;
        t.invoke  = [](const boost::json::object& args, const IDMCPCallPtr& call) { run_contents(args, call); };
        reg.add(std::move(t));
    }

    // object.addContents -----------------------------------------------------------
    {
        IDMCPTool t;
        t.name = "object.addContents";
        t.description =
            "Put inventory items into an object, as dragging them onto it would. "
            "{\"object_id\"}*, {\"item_ids\"}* (up to 32), {\"link\"} to fill one prim instead "
            "of the root, {\"running\"} for scripts (default true). The usual drop rules apply: "
            "a no-copy item moves out of your inventory, and worn items are refused. Returns "
            "{object_id, items:[{item_id, name, result}]} where result is added, not_found (sent "
            "but not seen in the contents within 20 s), or why it wasn't sent. Blocked by RLV "
            "@edit, @editobj, @editworld, @editattach, @interact, a locked attachment, or "
            "@unsit/@sittp while you sit on the object.";
        t.input_schema = boost::json::parse(
            R"({"type":"object","properties":{"object_id":{"type":"string"},"link":{"type":"integer"},)"
            R"("item_ids":{"type":"array","items":{"type":"string"},"minItems":1,"maxItems":32},)"
            R"("running":{"type":"boolean"}},"required":["object_id","item_ids"],"additionalProperties":false})");
        t.gate    = gate_add_contents;
        t.timeout = 60.0;
        t.invoke  = [](const boost::json::object& args, const IDMCPCallPtr& call) { run_add_contents(args, call); };
        reg.add(std::move(t));
    }
}
```

- [ ] **Step 2: Register the tool area**

In `indra/newview/idmcptools.h`, after `void idmcp_register_faces_tools(IDMCPToolRegistry& reg);`, add:

```cpp
void idmcp_register_contents_tools(IDMCPToolRegistry& reg);
```

In `indra/newview/idmcpserver.cpp`, after `    idmcp_register_faces_tools(mRegistry);`, add:

```cpp
    idmcp_register_contents_tools(mRegistry);
```

In `indra/newview/CMakeLists.txt`, after `    idmcptools_faces.cpp  # <ID> Embedded MCP server: object.setFaces`, add:

```cmake
    idmcptools_contents.cpp  # <ID> Embedded MCP server: object contents tools
```

- [ ] **Step 3: Compile-read checklist**

Read each of these in the source to confirm:
- `LLToolDragAndDrop::isInventoryDropAcceptable(LLViewerObject*, LLInventoryItem*)` is public static (`lltooldraganddrop.h:235`).
- `LLViewerObject::saveScript(const LLViewerInventoryItem*, bool, bool)` (`llviewerobject.h:584`) and `updateInventory(LLViewerInventoryItem*, U8, bool)` (`:559`) are public.
- `time_corrected()` is declared in `llcommon/lltimer.h:139`.
- `LLInventoryItem::setCreationDate(time_t)` is virtual public (`llinventory.h:92`).
- `RlvAttachmentLocks::isLockedAttachment(const LLViewerObject*) const` (`rlvlocks.h:80`) and `extern RlvAttachmentLocks gRlvAttachmentLocks;` (`:134`).
- `RLV_BHVR_DETACH`, `RLV_BHVR_UNSIT`, `RLV_BHVR_SITTP` exist (`rlvdefines.h:105, 196, 199`).
- `std::none_of` needs `<algorithm>`; the PCH has it, but add `#include <algorithm>` if the build complains.
- `ids.emplace_back(s)` uses `LLUUID(const std::string&)`.
- Enumerators `Baseline`, `Adding`, `Verifying` are not X11 macros.

- [ ] **Step 4: Commit**

```bash
git add indra/newview/idmcptools_contents.cpp indra/newview/idmcptools.h indra/newview/idmcpserver.cpp indra/newview/CMakeLists.txt
git commit -m "feat: add object.contents and object.addContents MCP tools"
```

---

### Task 5: `object.rez`, `object.attach`, `object.take`

Creates `idmcptools_rez.cpp`, the tools that move objects between inventory and the world. Rez packs `RezObject` by hand (`dropObject` is a protected member that reads drag state) and watches for the new object. Attach goes through `LLAttachmentsMgr` to avoid the replace dialog. Take reads the object's name, then sends `DeRezObject` and watches the folder.

**Files:**
- Create: `indra/newview/idmcptools_rez.cpp`
- Modify: `indra/newview/idmcptools.h` (after the `idmcp_register_contents_tools` line)
- Modify: `indra/newview/idmcpserver.cpp` (after `idmcp_register_contents_tools(mRegistry);`)
- Modify: `indra/newview/CMakeLists.txt` (after the `idmcptools_contents.cpp` line)

**Interfaces:**
- Consumes (Task 1, `namespace idmcp_obj`): `Target`, `resolve`, `findRoot`, `agentItem`, `inLibrary`, `inTrash`, `agentSittingOn`, `isUuid`, `lower`, `argStr`, `argBool`, `argNums`, `vecJson`, `eulerDegJson`, `quatFromEulerDeg`, `rotMatches`, `sendTransform`, `rlvEdit`, `rlvErrorMessage`, `Props`, `Job` (`launch`, `tick`, `timeoutMessage`, `timeoutData`, `succeed`, `fail`, `failIfDenied`, `requestProps`, `props`, `mPropsAskedAt`).
- Produces: `void idmcp_register_rez_tools(IDMCPToolRegistry&)` and the registered tools `object.rez`, `object.attach`, `object.take`.

- [ ] **Step 1: Create the file**

Create `indra/newview/idmcptools_rez.cpp`:

```cpp
/**
 * @file idmcptools_rez.cpp
 * @brief <ID> MCP server: object.rez, object.attach and object.take.
 *
 * Part of Five's custom Firestorm fork. Custom code carries an `ID` prefix.
 *
 * Rez packs RezObject like LLToolDragAndDrop::dropObject, with a fixed ray
 * onto the target instead of the last mouse pick, and RezSelected off so the
 * user's selection stays as it is. The region doesn't say which object it
 * made, so the job watches for a new owned object near the target and checks
 * its name. Attach calls LLAttachmentsMgr directly: rez_attachment with
 * replace opens a confirmation dialog. Main thread only.
 */

#include "llviewerprecompiledheaders.h"

#include "idmcptools_object.h"
#include "idmcptools.h"
#include "idmcpserver.h"
#include "idmcprlvgate.h"

#include "fscommon.h"               // FSCommon::getGroupForRezzing, sObjectAddMsg
#include "llagent.h"
#include "llattachmentsmgr.h"
#include "llinventorymodel.h"       // gInventory
#include "llselectmgr.h"            // UPD_*, EDeRezDestination; no selection calls
#include "lltimer.h"
#include "lltooldraganddrop.h"      // pack_permissions_slam
#include "llviewerinventory.h"
#include "llviewerjointattachment.h"
#include "llviewerobject.h"
#include "llviewerobjectlist.h"     // gObjectList
#include "llviewerregion.h"
#include "llvoavatarself.h"         // gAgentAvatarp
#include "message.h"
#include "rlvactions.h"
#include "rlvcommon.h"              // rlvPredCanWearItem
#include "rlvdefines.h"
#include "rlvlocks.h"               // gRlvAttachmentLocks

#include <set>
#include <vector>

using namespace idmcp_obj;

namespace
{
    // An object item the agent may rez or wear. Answers `call` and returns false otherwise.
    bool check_object_item(LLViewerInventoryItem* item, const IDMCPCallPtr& call)
    {
        if (!item)
        {
            idmcp_tool_err(call, IDMCP_ERR_NOT_FOUND, "item not found in your inventory");
            return false;
        }
        const char* why = nullptr;
        if (item->getType() != LLAssetType::AT_OBJECT) why = "the item isn't an object";
        else if (inLibrary(item->getUUID()))           why = "Library items aren't supported; copy the item to your inventory first";
        else if (inTrash(item->getUUID()))             why = "the item is in the Trash";
        else if (!item->isFinished())                  why = "the item hasn't finished loading; try again in a moment";
        if (why)
        {
            idmcp_tool_err(call, IDMCP_ERR_INVALID_PARAMS, why);
            return false;
        }
        return true;
    }

    // ---- object.rez --------------------------------------------------------------

    constexpr F64 REZ_LIMIT    = 30.0;   // spec: 30 s
    constexpr F32 REZ_RADIUS   = 10.f;   // spec: look within 10 m of the target
    constexpr F32 BUILD_HEIGHT = 4096.f;

    IDMCPGateResult gate_rez(const boost::json::object&, IDMCPGatePhase)
    {
        IDMCPGateResult g = IDMCPRlvGate::checkBehaviour(RLV_BHVR_REZ, "rez");
        if (!g.allowed) return g;
        return IDMCPRlvGate::checkBehaviour(RLV_BHVR_INTERACT, "interact");
    }

    // The agent's rezzed roots within REZ_RADIUS of pos in the region with this handle.
    std::set<LLUUID> own_roots_near(U64 region_handle, const LLVector3& pos)
    {
        std::set<LLUUID> out;
        const S32 count = gObjectList.getNumObjects();
        for (S32 i = 0; i < count; ++i)
        {
            LLViewerObject* o = gObjectList.getObject(i);
            if (!o || o->isDead() || o->isAttachment() || o->getPCode() != LL_PCODE_VOLUME) continue;
            if (!o->isRootEdit() || !o->permYouOwner()) continue;
            if (!o->getRegion() || o->getRegion()->getHandle() != region_handle) continue;
            if ((o->getPositionRegion() - pos).length() > REZ_RADIUS) continue;
            out.insert(o->getID());
        }
        return out;
    }

    // Packed like LLToolDragAndDrop::dropObject (lltooldraganddrop.cpp:1887-2062).
    void send_rez(LLViewerRegion* region, LLViewerInventoryItem* item, const LLVector3& target, bool remove)
    {
        LLMessageSystem* msg = gMessageSystem;
        msg->newMessageFast(_PREHASH_RezObject);
        msg->nextBlockFast(_PREHASH_AgentData);
        msg->addUUIDFast(_PREHASH_AgentID, gAgent.getID());
        msg->addUUIDFast(_PREHASH_SessionID, gAgent.getSessionID());
        msg->addUUIDFast(_PREHASH_GroupID, FSCommon::getGroupForRezzing());
        msg->nextBlockFast(_PREHASH_RezData);
        msg->addUUIDFast(_PREHASH_FromTaskID, LLUUID::null);
        msg->addU8Fast(_PREHASH_BypassRaycast, (U8)1);
        msg->addVector3Fast(_PREHASH_RayStart, target + LLVector3(0.f, 0.f, 0.5f));
        msg->addVector3Fast(_PREHASH_RayEnd, target);
        msg->addUUIDFast(_PREHASH_RayTargetID, LLUUID::null);
        msg->addBOOLFast(_PREHASH_RayEndIsIntersection, false);
        msg->addBOOLFast(_PREHASH_RezSelected, false);   // never auto-select: the user's selection stays
        msg->addBOOLFast(_PREHASH_RemoveItem, remove);
        pack_permissions_slam(msg, item->getFlags(), item->getPermissions());
        msg->nextBlockFast(_PREHASH_InventoryData);
        item->packMessage(msg);
        msg->sendReliable(region->getHost());
        // As dropObject: "Prevent default build parms from being applied due to lost packet."
        FSCommon::sObjectAddMsg = 0;
    }

    class RezJob : public Job
    {
    public:
        RezJob(const IDMCPCallPtr& call, U64 region_handle, const LLVector3& target, std::set<LLUUID> before)
            : Job(call, REZ_LIMIT), mRegionHandle(region_handle), mTarget(target), mBefore(std::move(before)) {}

        LLUUID       mItemId;
        std::string  mName;
        bool         mConsumed = false;
        bool         mHasRot   = false;
        LLQuaternion mRot;

    protected:
        void tick(F64) override
        {
            if (mFound.isNull())
            {
                find_new();
                return;
            }
            LLViewerObject* o = gObjectList.findObject(mFound);
            if (!o || o->isDead())
            {
                fail(IDMCP_ERR_NOT_FOUND, "the new object disappeared");
                return;
            }
            if (mHasRot && mRotNote.empty() && !rotMatches(o->getRotation(), mRot)) return;
            boost::json::object out;
            out["object_id"] = o->getID().asString();
            out["name"]      = mName;
            out["position"]  = vecJson(o->getPosition());
            out["rotation"]  = eulerDegJson(o->getRotation());
            if (mConsumed) out["item_consumed"] = true;
            if (!mRotNote.empty()) out["rotation_skipped"] = mRotNote;
            succeed(std::move(out));
        }

        std::string timeoutMessage() const override
        {
            return mFound.isNull() ? "the new object didn't appear in time"
                                   : "the region didn't confirm the rotation in time";
        }

        boost::json::object timeoutData() const override
        {
            boost::json::object sent;
            sent["item_id"]  = mItemId.asString();
            sent["position"] = vecJson(mTarget);
            boost::json::object data;
            data["sent"] = std::move(sent);
            if (mFound.notNull()) data["object_id"] = mFound.asString();
            data["hint"] = "the object may still appear; check objects.getNearby before rezzing again";
            return data;
        }

    private:
        void find_new()
        {
            // Earlier candidates whose properties have arrived.
            for (const LLUUID& id : mCandidates)
            {
                const Props*    pr = props(id);
                LLViewerObject* o  = gObjectList.findObject(id);
                if (pr && o && !o->isDead() && pr->name == mName)
                {
                    found(o);
                    return;
                }
            }
            // New owned roots near the target: ask for their names.
            std::vector<LLViewerObject*> fresh;
            for (const LLUUID& id : own_roots_near(mRegionHandle, mTarget))
            {
                if (mBefore.count(id) || mCandidates.count(id)) continue;
                mCandidates.insert(id);
                if (LLViewerObject* o = gObjectList.findObject(id)) fresh.push_back(o);
            }
            if (!fresh.empty()) requestProps(fresh);
        }

        void found(LLViewerObject* o)
        {
            mFound = o->getID();
            if (!mHasRot) return;
            // Rotating the new object is an edit: the commit point for it is now.
            const IDMCPGateResult g = rlvEdit(o);
            if (!g.allowed)
            {
                mRotNote = rlvErrorMessage(g);
                return;
            }
            sendTransform(o, (U8)(UPD_ROTATION | UPD_LINKED_SETS), LLVector3::zero, mRot, LLVector3::zero);
        }

        U64              mRegionHandle = 0;
        LLVector3        mTarget;
        std::set<LLUUID> mBefore;       // owned roots near the target before the rez
        std::set<LLUUID> mCandidates;   // new ones whose names we asked for
        LLUUID           mFound;
        std::string      mRotNote;      // why the rotation wasn't applied
    };

    void run_rez(const boost::json::object& args, const IDMCPCallPtr& call)
    {
        const std::string sid = argStr(args, "item_id");
        if (!isUuid(sid))
        {
            idmcp_tool_err(call, IDMCP_ERR_INVALID_PARAMS, "item_id must be an inventory item UUID");
            return;
        }
        LLViewerInventoryItem* item = agentItem(LLUUID(sid));
        if (!check_object_item(item, call)) return;
        if (isAgentAvatarValid() && gAgentAvatarp->isWearingAttachment(item->getUUID()))
        {
            idmcp_tool_err(call, IDMCP_ERR_INVALID_PARAMS, "the item is worn; detach it first");
            return;
        }
        LLViewerRegion* region = gAgent.getRegion();
        if (!region)
        {
            idmcp_tool_err(call, IDMCP_ERR_NOT_LOGGED_IN, "no current region");
            return;
        }
        const F32 width = region->getWidth();
        LLVector3 target;
        if (args.contains("position"))
        {
            if (!argNums(args, "position", 3, target.mV))
            {
                idmcp_tool_err(call, IDMCP_ERR_INVALID_PARAMS, "position must be [x, y, z] region metres");
                return;
            }
            if (target.mV[VX] < 0.f || target.mV[VX] >= width || target.mV[VY] < 0.f || target.mV[VY] >= width
                || target.mV[VZ] < 0.f || target.mV[VZ] > BUILD_HEIGHT)
            {
                idmcp_tool_err(call, IDMCP_ERR_INVALID_PARAMS,
                               llformat("position must be inside this region (x and y 0 to %.0f, z 0 to %.0f)",
                                        width, BUILD_HEIGHT));
                return;
            }
        }
        else
        {
            // 2 m in front of the avatar, at avatar height.
            LLVector3 ahead = gAgent.getAtAxis();
            ahead.mV[VZ] = 0.f;
            if (ahead.normalize() < 0.01f) ahead = LLVector3::x_axis;
            target = region->getPosRegionFromGlobal(gAgent.getPositionGlobal()) + ahead * 2.f;
            target.mV[VX] = llclamp(target.mV[VX], 0.f, width - 0.01f);
            target.mV[VY] = llclamp(target.mV[VY], 0.f, width - 0.01f);
        }
        LLVector3  deg;
        const bool has_rot = args.contains("rotation");
        if (has_rot && !argNums(args, "rotation", 3, deg.mV))
        {
            idmcp_tool_err(call, IDMCP_ERR_INVALID_PARAMS, "rotation must be [x, y, z] in degrees");
            return;
        }

        auto job = std::make_shared<RezJob>(call, region->getHandle(), target,
                                            own_roots_near(region->getHandle(), target));
        job->mItemId   = item->getUUID();
        job->mName     = item->getName();
        job->mHasRot   = has_rot;
        if (has_rot) job->mRot = quatFromEulerDeg(deg);
        const bool remove = !item->getPermissions().allowCopyBy(gAgent.getID());
        job->mConsumed = remove;

        // The request gate ran this frame, so it is the commit check for the rez.
        send_rez(region, item, target, remove);
        if (remove)
        {
            // As dropObject: drop a no-copy item from inventory at once; the
            // region puts it back if the rez fails.
            gInventory.deleteObject(job->mItemId);   // `item` is gone after this line
            gInventory.notifyObservers();
        }
        Job::launch(job);
    }

    // ---- object.attach -----------------------------------------------------------

    constexpr F64 ATTACH_LIMIT = 30.0;

    // Internal point names are English and have no "HUD" prefix ("Center",
    // "Top Left"), so also accept "HUD <name>" for HUD points.
    LLViewerJointAttachment* find_point(const std::string& name, S32& id_out)
    {
        if (!isAgentAvatarValid() || name.empty()) return nullptr;
        const std::string want = lower(name);
        for (const auto& kv : gAgentAvatarp->mAttachmentPoints)
        {
            LLViewerJointAttachment* pt = kv.second;
            if (!pt) continue;
            const std::string n = lower(pt->getName());
            if (want == n || (pt->getIsHUDAttachment() && want == "hud " + n))
            {
                id_out = kv.first;
                return pt;
            }
        }
        return nullptr;
    }

    // rez_attachment's RLVa checks, for the chosen point.
    IDMCPGateResult rlv_attach(const LLViewerInventoryItem* item, const LLViewerJointAttachment* pt, bool replace)
    {
        if (!IDMCPRlvGate::isEnabled()) return IDMCPGateResult();
        const ERlvWearMask mask = replace ? RLV_WEAR_REPLACE : RLV_WEAR_ADD;
        if ((item && !rlvPredCanWearItem(item, mask)) || (pt && !(gRlvAttachmentLocks.canAttach(pt) & mask)))
        {
            return IDMCPRlvGate::deny(RLV_BHVR_ADDATTACH, "addattach");
        }
        return IDMCPGateResult();
    }

    IDMCPGateResult gate_attach(const boost::json::object& args, IDMCPGatePhase)
    {
        const std::string sid = argStr(args, "item_id");
        const LLViewerInventoryItem* item = isUuid(sid) ? agentItem(LLUUID(sid)) : nullptr;
        S32 id = 0;
        return rlv_attach(item, find_point(argStr(args, "point"), id), argBool(args, "replace", false));
    }

    class AttachJob : public Job
    {
    public:
        AttachJob(const IDMCPCallPtr& call, const LLUUID& item_id, const std::string& point)
            : Job(call, ATTACH_LIMIT), mItemId(item_id), mPoint(point) {}

    protected:
        void tick(F64) override
        {
            if (!isAgentAvatarValid()) return;
            // getAttachedObject() matches item ids too; this resolves links and scans every point.
            LLViewerObject* o = gAgentAvatarp->getWornAttachment(mItemId);
            if (!o) return;
            const LLViewerJointAttachment* pt = gAgentAvatarp->getWornAttachmentPoint(mItemId);
            boost::json::object out;
            out["object_id"] = o->getID().asString();
            out["item_id"]   = mItemId.asString();
            out["point"]     = pt ? pt->getName() : mPoint;
            succeed(std::move(out));
        }

        std::string timeoutMessage() const override { return "the attachment didn't appear in time"; }

        boost::json::object timeoutData() const override
        {
            boost::json::object sent;
            sent["item_id"] = mItemId.asString();
            sent["point"]   = mPoint;
            boost::json::object data;
            data["sent"] = std::move(sent);
            return data;
        }

    private:
        LLUUID      mItemId;
        std::string mPoint;
    };

    void run_attach(const boost::json::object& args, const IDMCPCallPtr& call)
    {
        const std::string sid = argStr(args, "item_id");
        if (!isUuid(sid))
        {
            idmcp_tool_err(call, IDMCP_ERR_INVALID_PARAMS, "item_id must be an inventory item UUID");
            return;
        }
        LLViewerInventoryItem* item = agentItem(LLUUID(sid));
        if (!check_object_item(item, call)) return;
        if (!isAgentAvatarValid())
        {
            idmcp_tool_err(call, IDMCP_ERR_NOT_LOGGED_IN, "your avatar isn't loaded");
            return;
        }
        if (gAgentAvatarp->isWearingAttachment(item->getUUID()))
        {
            const LLViewerJointAttachment* worn = gAgentAvatarp->getWornAttachmentPoint(item->getUUID());
            idmcp_tool_err(call, IDMCP_ERR_INVALID_PARAMS,
                           "the item is already worn" + (worn ? " on " + worn->getName() : std::string())
                           + "; detach it first");
            return;
        }
        const std::string point = argStr(args, "point");
        S32 pt_id = 0;
        LLViewerJointAttachment* pt = find_point(point, pt_id);
        if (!pt)
        {
            idmcp_tool_err(call, IDMCP_ERR_NOT_FOUND,
                           "unknown attachment point: " + point + " (names like Chest, Spine, Left Hand, HUD Center)");
            return;
        }
        const bool replace  = argBool(args, "replace", false);
        const bool occupied = pt->getNumObjects() > 0;
        if ((!replace || !occupied) && !gAgentAvatarp->canAttachMoreObjects())
        {
            idmcp_tool_err(call, IDMCP_ERR_INVALID_PARAMS,
                           llformat("you're wearing the most attachments allowed (%d)", gAgentAvatarp->getMaxAttachments()));
            return;
        }
        // The request gate ran this frame, so it is the commit check. No dialog:
        // this is what confirm_attachment_rez does after "ReplaceAttachment".
        LLAttachmentsMgr::instance().addAttachmentRequest(item->getUUID(), (U8)pt_id, !replace);
        Job::launch(std::make_shared<AttachJob>(call, item->getUUID(), pt->getName()));
    }

    // ---- object.take -------------------------------------------------------------

    constexpr F64 TAKE_LIMIT       = 30.0;   // spec: 30 s
    constexpr F64 TAKE_PROPS_LIMIT = 10.0;

    IDMCPGateResult rlv_take(LLViewerObject* root)
    {
        IDMCPGateResult g = rlvEdit(root);
        if (!g.allowed) return g;
        g = IDMCPRlvGate::checkBehaviour(RLV_BHVR_REZ, "rez");
        if (!g.allowed) return g;
        // As handle_take_copy: no taking the object you sit on while you can't stand.
        if (IDMCPRlvGate::isEnabled() && root && agentSittingOn(root) && !RlvActions::canStand())
        {
            return IDMCPRlvGate::deny(RLV_BHVR_UNSIT, "unsit");
        }
        return g;
    }

    IDMCPGateResult gate_take(const boost::json::object& args, IDMCPGatePhase)
    {
        return rlv_take(findRoot(args));
    }

    // A folder UUID or well-known name; null when unusable. Default: Objects.
    LLUUID resolve_take_folder(const std::string& s)
    {
        const std::string k = lower(s);
        if (k.empty() || k == "objects")         return gInventory.findCategoryUUIDForType(LLFolderType::FT_OBJECT);
        if (k == "root" || k == "my_inventory")  return gInventory.getRootFolderID();
        if (!isUuid(s)) return LLUUID::null;
        const LLUUID id(s);
        if (!gInventory.getCategory(id) || inLibrary(id) || inTrash(id)) return LLUUID::null;
        return id;
    }

    // derez_objects (llviewermenu.cpp:6557-6644) for a single root.
    void send_derez(LLViewerObject* root, EDeRezDestination dest, const LLUUID& folder)
    {
        LLUUID tid;
        tid.generate();
        LLMessageSystem* msg = gMessageSystem;
        msg->newMessageFast(_PREHASH_DeRezObject);
        msg->nextBlockFast(_PREHASH_AgentData);
        msg->addUUIDFast(_PREHASH_AgentID, gAgent.getID());
        msg->addUUIDFast(_PREHASH_SessionID, gAgent.getSessionID());
        msg->nextBlockFast(_PREHASH_AgentBlock);
        msg->addUUIDFast(_PREHASH_GroupID, gAgent.getGroupID());
        msg->addU8Fast(_PREHASH_Destination, (U8)dest);
        msg->addUUIDFast(_PREHASH_DestinationID, folder);
        msg->addUUIDFast(_PREHASH_TransactionID, tid);
        msg->addU8Fast(_PREHASH_PacketCount, (U8)1);
        msg->addU8Fast(_PREHASH_PacketNumber, (U8)0);
        msg->nextBlockFast(_PREHASH_ObjectData);
        msg->addU32Fast(_PREHASH_ObjectLocalID, root->getLocalID());
        msg->sendReliable(root->getRegion()->getHost());
    }

    class TakeJob : public Job
    {
    public:
        TakeJob(const IDMCPCallPtr& call, LLViewerObject* root, const LLUUID& folder, bool copy)
            : Job(call, TAKE_LIMIT), mRootId(root->getID()), mFolder(folder), mCopy(copy) {}

        void begin()
        {
            mPropsUntil = LLTimer::getTotalSeconds() + TAKE_PROPS_LIMIT;
            requestProps({ gObjectList.findObject(mRootId) });
        }

    protected:
        void tick(F64 now) override
        {
            if (!mSent)
            {
                ask_then_send(now);
                return;
            }
            LLInventoryModel::cat_array_t*  cats  = nullptr;
            LLInventoryModel::item_array_t* items = nullptr;
            gInventory.getDirectDescendentsOf(mFolder, cats, items);
            if (!items) return;
            for (const LLPointer<LLViewerInventoryItem>& it : *items)
            {
                if (it.isNull() || mBefore.count(it->getUUID())) continue;
                if (it->getType() == LLAssetType::AT_OBJECT && it->getName() == mName)
                {
                    boost::json::object out;
                    out["item_id"]   = it->getUUID().asString();
                    out["name"]      = mName;
                    out["folder_id"] = mFolder.asString();
                    out["copy"]      = mCopy;
                    succeed(std::move(out));
                    return;
                }
            }
        }

        std::string timeoutMessage() const override
        {
            return mSent ? "the new inventory item didn't appear in time"
                         : "the region didn't send the object's properties in time";
        }

        boost::json::object timeoutData() const override
        {
            boost::json::object sent;
            sent["object_id"] = mRootId.asString();
            sent["folder_id"] = mFolder.asString();
            sent["copy"]      = mCopy;
            sent["derez_sent"] = mSent;
            boost::json::object data;
            data["sent"] = std::move(sent);
            return data;
        }

    private:
        void ask_then_send(F64 now)
        {
            LLViewerObject* root = gObjectList.findObject(mRootId);
            if (!root || root->isDead())
            {
                fail(IDMCP_ERR_NOT_FOUND, "the object is gone (deleted, taken or out of range)");
                return;
            }
            const Props* pr = props(mRootId);
            if (!pr)
            {
                if (now >= mPropsUntil)
                {
                    fail(IDMCP_ERR_TIMEOUT, "the region didn't send the object's properties in time", timeoutData());
                    return;
                }
                if (now - mPropsAskedAt >= 1.0) requestProps({ root });
                return;
            }
            mName = pr->name;   // the new item takes the object's name
            // Commit point: a restriction may have arrived during the wait.
            if (failIfDenied(rlv_take(root))) return;
            LLInventoryModel::cat_array_t*  cats  = nullptr;
            LLInventoryModel::item_array_t* items = nullptr;
            gInventory.getDirectDescendentsOf(mFolder, cats, items);
            if (items)
            {
                for (const LLPointer<LLViewerInventoryItem>& it : *items)
                {
                    if (it.notNull()) mBefore.insert(it->getUUID());
                }
            }
            send_derez(root, mCopy ? DRD_ACQUIRE_TO_AGENT_INVENTORY : DRD_TAKE_INTO_AGENT_INVENTORY, mFolder);
            mSent = true;
        }

        LLUUID           mRootId;
        LLUUID           mFolder;
        bool             mCopy = false;
        bool             mSent = false;
        F64              mPropsUntil = 0.0;
        std::string      mName;
        std::set<LLUUID> mBefore;   // folder items before the take
    };

    void run_take(const boost::json::object& args, const IDMCPCallPtr& call)
    {
        Target t;
        if (!resolve(args, call, t)) return;
        LLViewerObject* root = t.root;
        if (root->isAttachment())
        {
            idmcp_tool_err(call, IDMCP_ERR_INVALID_PARAMS,
                           "worn attachments can't be taken; detach them with appearance.detachItems");
            return;
        }
        if (!root->permYouOwner())
        {
            idmcp_tool_err(call, IDMCP_ERR_PERMISSION, "you can only take objects you own");
            return;
        }
        const bool copy = argBool(args, "copy", false);
        if (copy && !root->permCopy())
        {
            idmcp_tool_err(call, IDMCP_ERR_PERMISSION, "the object is no-copy; take it without copy:true");
            return;
        }
        if (!copy && root->isPermanentEnforced())
        {
            idmcp_tool_err(call, IDMCP_ERR_PERMISSION, "the object is a permanent pathfinding object and can't be taken");
            return;
        }
        const std::string folder_arg = argStr(args, "folder");
        const LLUUID folder = resolve_take_folder(folder_arg);
        if (folder.isNull())
        {
            idmcp_tool_err(call, IDMCP_ERR_NOT_FOUND, "folder not found: " + folder_arg);
            return;
        }
        auto job = std::make_shared<TakeJob>(call, root, folder, copy);
        Job::launch(job);
        job->begin();
    }
}

// ---------------------------------------------------------------------------

void idmcp_register_rez_tools(IDMCPToolRegistry& reg)
{
    // object.rez ---------------------------------------------------------------
    {
        IDMCPTool t;
        t.name = "object.rez";
        t.description =
            "Rez an object item from your inventory into the world. {\"item_id\"}*; "
            "{\"position\"} [x,y,z] region metres (default 2 m in front of you, at your "
            "height); {\"rotation\"} [x,y,z] degrees. Waits up to 30 s for the new object and "
            "returns {object_id, name, position, rotation}, plus item_consumed:true for a no-copy "
            "item, which leaves your inventory. Blocked by RLV @rez and @interact.";
        t.input_schema = boost::json::parse(
            R"({"type":"object","properties":{"item_id":{"type":"string"},)"
            R"("position":{"type":"array","items":{"type":"number"},"minItems":3,"maxItems":3},)"
            R"("rotation":{"type":"array","items":{"type":"number"},"minItems":3,"maxItems":3}},)"
            R"("required":["item_id"],"additionalProperties":false})");
        t.gate    = gate_rez;
        t.timeout = 45.0;
        t.invoke  = [](const boost::json::object& args, const IDMCPCallPtr& call) { run_rez(args, call); };
        reg.add(std::move(t));
    }

    // object.attach ------------------------------------------------------------
    {
        IDMCPTool t;
        t.name = "object.attach";
        t.description =
            "Wear an object item on a chosen attachment point. {\"item_id\"}*, {\"point\"}*: a "
            "point name such as \"Chest\", \"Spine\", \"Left Hand\" or \"HUD Center\" (case "
            "doesn't matter). {\"replace\"} (default false) replaces what is on the point "
            "instead of adding alongside it. Waits up to 30 s and returns {object_id, item_id, "
            "point}. Use object_id with object.edit and object.setFaces. Blocked by RLV "
            "attachment locks and @addattach.";
        t.input_schema = boost::json::parse(
            R"({"type":"object","properties":{"item_id":{"type":"string"},"point":{"type":"string"},)"
            R"("replace":{"type":"boolean"}},"required":["item_id","point"],"additionalProperties":false})");
        t.gate    = gate_attach;
        t.timeout = 45.0;
        t.invoke  = [](const boost::json::object& args, const IDMCPCallPtr& call) { run_attach(args, call); };
        reg.add(std::move(t));
    }

    // object.take --------------------------------------------------------------
    {
        IDMCPTool t;
        t.name = "object.take";
        t.description =
            "Take a rezzed object you own back into inventory. {\"object_id\"}*; "
            "{\"copy\"} (default false) takes a copy and leaves the object in place; "
            "{\"folder\"}: a folder UUID, \"objects\" (default) or \"root\". Waits up to 30 s "
            "and returns {item_id, name, folder_id, copy}. Worn attachments aren't taken; "
            "detach them instead. Blocked by RLV @edit, @editobj, @editworld, @interact, @rez, "
            "and @unsit while you sit on the object.";
        t.input_schema = boost::json::parse(
            R"({"type":"object","properties":{"object_id":{"type":"string"},"copy":{"type":"boolean"},)"
            R"("folder":{"type":"string"}},"required":["object_id"],"additionalProperties":false})");
        t.gate    = gate_take;
        t.timeout = 45.0;
        t.invoke  = [](const boost::json::object& args, const IDMCPCallPtr& call) { run_take(args, call); };
        reg.add(std::move(t));
    }
}
```

- [ ] **Step 2: Register the tool area**

In `indra/newview/idmcptools.h`, after `void idmcp_register_contents_tools(IDMCPToolRegistry& reg);`, add:

```cpp
void idmcp_register_rez_tools(IDMCPToolRegistry& reg);
```

In `indra/newview/idmcpserver.cpp`, after `    idmcp_register_contents_tools(mRegistry);`, add:

```cpp
    idmcp_register_rez_tools(mRegistry);
```

In `indra/newview/CMakeLists.txt`, after `    idmcptools_contents.cpp  # <ID> Embedded MCP server: object contents tools`, add:

```cmake
    idmcptools_rez.cpp  # <ID> Embedded MCP server: rez/attach/take objects
```

- [ ] **Step 3: Compile-read checklist**

Read each of these in the source to confirm:
- `pack_permissions_slam(LLMessageSystem*, U32, const LLPermissions&)` is a free function declared in `lltooldraganddrop.h:321`.
- `FSCommon::getGroupForRezzing()` and `extern S32 sObjectAddMsg;` are in `namespace FSCommon` in `fscommon.h:81, 97`.
- `LLViewerInventoryItem::packMessage(LLMessageSystem*) const` (`llviewerinventory.h:132`), `getFlags()` (`:82`), `getPermissions()` (`:70`), `isFinished()` (`:138`).
- `LLAttachmentsMgr::addAttachmentRequest(const LLUUID&, const U8, const bool, const bool = false)` is public (`llattachmentsmgr.h:77-79`).
- `LLVOAvatarSelf::isWearingAttachment`, `getWornAttachment` (non-const), `getWornAttachmentPoint` (const) are public (`llvoavatarself.h:331-336`). `mAttachmentPoints` is a public `std::map<S32, LLViewerJointAttachment*>` (`llvoavatar.h:1076-1077`). `canAttachMoreObjects(U32 = 1) const` and `getMaxAttachments() const` (`llvoavatar.h:1093-1094`).
- `LLViewerJointAttachment::getIsHUDAttachment()` and `getNumObjects()` are public (`llviewerjointattachment.h`); `getName()` comes from `LLJoint` (`llcharacter/lljoint.h:202`).
- `rlvPredCanWearItem(const LLViewerInventoryItem*, ERlvWearMask)` (`rlvcommon.h:292`); `RlvAttachmentLocks::canAttach(const LLViewerJointAttachment*) const` returns `ERlvWearMask` (`rlvlocks.h:110`); `RLV_WEAR_ADD`, `RLV_WEAR_REPLACE` (`rlvdefines.h:396-410`).
- `RlvActions::canStand()` (`rlvactions.h:360`).
- `EDeRezDestination`, `DRD_TAKE_INTO_AGENT_INVENTORY`, `DRD_ACQUIRE_TO_AGENT_INVENTORY` are in `llselectmgr.h:83-98`.
- `LLInventoryModel::getDirectDescendentsOf(const LLUUID&, cat_array_t*&, item_array_t*&) const` (`llinventorymodel.h:273-275`), `findCategoryUUIDForType`, `getRootFolderID`, `getCategory`.
- `LLViewerObject::isPermanentEnforced()`, `permCopy()`, `permYouOwner()` (`llviewerobject.h:594-636`), `getPositionRegion()` (`:351`); `LLViewerRegion::getHandle()` returns `U64`; `getPosRegionFromGlobal` (`llviewerregion.h:331`).
- `LLVector3::normalize()` returns the old length; `LLVector3::x_axis` and `LLVector3::zero` exist (`v3math.h`).
- `gObjectList.getObject(S32)` and `getNumObjects()` (`llviewerobjectlist.h:68, 140`).
- No identifier is on the X11 list.

- [ ] **Step 4: Commit**

```bash
git add indra/newview/idmcptools_rez.cpp indra/newview/idmcptools.h indra/newview/idmcpserver.cpp indra/newview/CMakeLists.txt
git commit -m "feat: add object.rez, object.attach and object.take MCP tools"
```

---

### Task 6: `MCP_TOOLS.md`: sync from the plugin, then document the object tools

One commit: the sync and the new section both change the tool count line. The plugin repo is only read here; a later PR updates it.

**Files:**
- Modify: `MCP_TOOLS.md`
- Read only: `/home/thea/Work/plugins/firestorm-avatar/MCP_TOOLS.md`

**Interfaces:**
- Consumes: the tool names, arguments and results registered in Tasks 1-5.
- Produces: the viewer's tool reference with an "Objects" section.

- [ ] **Step 1: Check that the plugin copy is a superset**

Run:

```bash
diff /home/thea/Work/phoenix-firestorm/MCP_TOOLS.md /home/thea/Work/plugins/firestorm-avatar/MCP_TOOLS.md
```

Expected: the only `<` line (viewer side) is the count line `**74 tools** across 20 areas.`; every other difference is `>` lines (the plugin's "Gestures" and "Wearables" sections, after the "Scripts (LSL)" section). If any other `<` line appears, the viewer has content the plugin lacks: keep the union by copying the plugin file and then re-adding each viewer-only line at the same place, and tell the controller which lines they were.

- [ ] **Step 2: Copy the plugin's file over the viewer's**

```bash
cp /home/thea/Work/plugins/firestorm-avatar/MCP_TOOLS.md /home/thea/Work/phoenix-firestorm/MCP_TOOLS.md
```

- [ ] **Step 3: Update the count line**

Replace:

```markdown
**85 tools** across 22 areas.
```

with:

```markdown
**96 tools** across 23 areas.
```

- [ ] **Step 4: Add the Objects section**

Insert this section directly before the `## Money (L$)` heading:

```markdown
## Objects

Build with what you uploaded: rez an item, edit it, texture it, fill its contents, set next-owner permissions, link it, and take it back. The tools work on rezzed objects and on your own worn attachments. They never touch your selection or the Build floater, so you can keep building while an agent works.

**Targeting.** `object_id` is any prim of the object, from `objects.getNearby`, `avatars.getWorn`, or an `object.rez`/`object.attach` result. `link` picks one prim by the number `object.get` reports (1 = the root). Without `link`, a tool acts on the whole object. These link numbers can differ from LSL's `llGetLinkNumber`, so always take them from `object.get`.

**Position and rotation.** `position` is in metres and `rotation` is `[x,y,z]` Euler degrees, as the Edit floater shows them. Both are relative to the region for a rezzed root, to the attachment point for a worn root, and to the root for a child link. `scale` changes one prim: the root, or the prim named by `link`.

| Tool | Params | Description |
|---|---|---|
| `object.get` | `object_id*`: string<br>`faces`: boolean | Describe an object link by link: `{object_id, link_count, attachment_point, links:[{link, object_id, name, description, position, rotation, scale, face_count, permissions}]}`. `permissions` has `owner_id` and `base`/`owner`/`group`/`everyone`/`next_owner` as `{copy, modify, transfer}`. `faces:true` adds each face's `texture`, `material`, `color`, `alpha`, `repeats`, `offset`, `rotation`. Links whose name didn't arrive within 10 s are listed in `incomplete_links`. |
| `object.edit` | `object_id*`: string<br>`link`: integer<br>`position`: [x,y,z]<br>`rotation`: [x,y,z]<br>`scale`: [x,y,z]<br>`name`: string<br>`description`: string | Move, rotate, scale, rename or describe an object you can modify. Without `link`, position and rotation move the whole object. Returns the values the object reports once the region confirms (up to 10 s). Blocked by RLV @edit, @editobj, @editworld, @editattach, @interact. |
| `object.setFaces` | `object_id*`: string<br>`faces*`: `"all"` or integer[]<br>`link`: integer<br>`texture`: string<br>`material`: string<br>`color`: [r,g,b]<br>`alpha`: number<br>`repeats`: [u,v]<br>`offset`: [u,v]<br>`rotation`: number | Texture, color and map faces. `"all"` without `link` covers every prim; a face list without `link` means the root. `texture` and `material` take an inventory item id (drop rules apply, so a no-copy item moves into the prim's contents) or a raw asset UUID. `material:"none"` removes a PBR material, and setting a texture removes one too. Colors and alpha are 0-1; rotation is degrees. Returns `{object_id, faces:[{link, face, ok, error}]}`. Blocked by the edit restrictions. |
| `object.contents` | `object_id*`: string<br>`link`: integer | List the items inside the root prim, or inside the prim named by `link`: `{object_id, items:[{item_id, name, type, permissions}]}`. The region only shows contents of objects you can modify. |
| `object.addContents` | `object_id*`: string<br>`item_ids*`: string[]<br>`link`: integer<br>`running`: boolean | Put up to 32 inventory items into the object, as dragging them onto it would. Scripts start running unless `running:false`. A no-copy item leaves your inventory; worn items are refused. Returns `{object_id, items:[{item_id, name, result}]}`: `added`, `not_found` (sent but not seen within 20 s), or why it wasn't sent. Blocked by the edit restrictions, a locked attachment, or @unsit/@sittp while you sit on the object. |
| `object.setPermissions` | `object_id*`: string<br>`next_owner*`: {`copy`, `modify`, `transfer`}<br>`contents`: boolean | Set next-owner permissions on an object you own. The next owner needs copy or transfer. `contents:true` sets the same on every item in the root prim. Returns `{object_id, next_owner, contents:[{item_id, name, result}]}`, where `result` is `updated`, `unchanged` or `permission`. Bits missing from the object's base permissions stay off. Blocked by the edit restrictions. |
| `object.link` | `object_ids*`: string[] | Link two or more of your rezzed objects in one region. The first becomes the root. Returns `{object_id, link_count}`. A link the region refuses (too many prims, too far apart) shows as a timeout. Blocked by the edit restrictions. |
| `object.unlink` | `object_id*`: string<br>`link`: integer | Unlink one prim (`link`), or every prim when `link` is omitted. Returns `{object_ids}`: the unlinked prims, then the root of what's left. Blocked by the edit restrictions. |
| `object.rez` | `item_id*`: string<br>`position`: [x,y,z]<br>`rotation`: [x,y,z] | Rez an object item. The default position is 2 m in front of you at your height. Returns `{object_id, name, position, rotation}`, plus `item_consumed:true` for a no-copy item. Waits up to 30 s. Blocked by RLV @rez and @interact. |
| `object.attach` | `item_id*`: string<br>`point*`: string<br>`replace`: boolean | Wear an object item on a named point: "Chest", "Spine", "Left Hand", "HUD Center", and so on, in any case. `replace:true` replaces what's on the point; the default adds alongside it. Returns `{object_id, item_id, point}`. Blocked by RLV attachment locks and @addattach. |
| `object.take` | `object_id*`: string<br>`copy`: boolean<br>`folder`: string | Take a rezzed object you own into inventory, or a copy of it with `copy:true`. `folder` is a folder UUID, `"objects"` (default) or `"root"`. Returns `{item_id, name, folder_id, copy}`. Worn attachments are refused; detach them instead. Blocked by the edit restrictions, @rez, and @unsit while you sit on the object. |

- **Edits wait for the region.** `object.edit`, `object.setPermissions`, `object.link` and `object.unlink` answer only once the region reports the change. A timeout (-32001) carries `sent` and, where known, `current`.
- **Batch tools report per item.** `object.setFaces`, `object.addContents` and `object.setPermissions` with `contents` give a result for each face or item, and fail the whole call only when nothing could be done.
- **The build loop.** `upload.mesh` → `object.rez` → `object.edit` → `object.setFaces` → `object.addContents` → `object.setPermissions` → `object.take`. Use `object.attach` instead of `object.rez` for something worn.
```

- [ ] **Step 5: Check the result**

Run:

```bash
grep -c '^| `object\.' /home/thea/Work/phoenix-firestorm/MCP_TOOLS.md
grep -n '^## ' /home/thea/Work/phoenix-firestorm/MCP_TOOLS.md
```

Expected: `11`, and 23 tool-area headings after "Connecting" and "RLV enforcement" with "Objects" just before "Money (L$)".

- [ ] **Step 6: Commit**

```bash
git add MCP_TOOLS.md
git commit -m "docs: sync MCP_TOOLS.md from the plugin and document object tools"
```

---

### Task 7: Build and live verification (user and controller)

The user builds and logs in. The controller runs this matrix over MCP and records results. Use a region where you can rez, with RLVa on. Rows 1-11 are the spec's Testing list; rows 12-18 cover the Review Focus and regressions.

**Files:**
- None committed. Scratch: a 1 m cube `.dae` for `upload.mesh`, a small `.png` for `upload.image`, a notecard, a script that says "hello" on channel 0 in `state_entry`, a no-copy texture, a no-modify object, and a two-prim linkset you own.

- [ ] **Step 1: Ask the user to build**

Tell the user: "Ready to build. Please build the viewer, log in somewhere you can rez, and enable the MCP server. I'll run the checks below over MCP."

- [ ] **Step 2: Run the matrix**

| # | Call | Expected |
|---|---|---|
| 1 | Upload a texture (`upload.image`) and a cube (`upload.mesh`), each with `confirm:true` | Both return `item_id`s. |
| 2 | `object.rez` the cube with `position` set and `rotation:[0,0,45]` | `object_id` returned within 30 s; it appears in `objects.getNearby`; the rotation reads about 45 on z. |
| 3 | `object.edit`: move, rotate, rename, set a description; then `object.get` | Each edit returns the new values; `object.get` shows them. |
| 4 | `object.setFaces`: the texture item on face 2, then a PBR material on `faces:"all"`; `object.get faces:true`. Repeat the texture with its raw asset UUID | Face 2 shows the texture; then every face has the material; then the raw UUID texture shows again and that face's `material` is null. |
| 5 | `object.addContents`: a notecard and the script; `object.contents` | Both report `added`; the contents list both; the script says "hello" in chat. |
| 6 | `object.setPermissions` `next_owner:{copy:true, modify:false, transfer:false}` with `contents:true`; `object.get` and `object.contents` | The object's and both items' `next_owner` show copy only. |
| 7 | Rez a second cube; `object.link` both; `object.get`; `object.unlink` | `link_count` 2, `object.get` shows two links; unlink returns two ids and each is a single prim again. |
| 8 | `object.take copy:true`, then `object.take` | Two new items appear in Objects; the object is gone after the second take. |
| 9 | `object.attach` the cube to "Chest", then `object.setFaces` and `object.edit` on the worn object | The attach returns `point:"Chest"`; edits apply to the worn object (position relative to the chest). Try "HUD Center" too: it attaches to the HUD. |
| 10 | Errors: with RLV `@edit=n` from a test object, `object.edit`; `object.edit` on a no-modify object; the no-copy texture on one face; `object.edit link:99` | -32011 with `restriction:"edit"`; -32004; the texture moves into the object's contents and leaves inventory; -32602 naming the link count. |
| 11 | Start `object.get` on a 50+ prim object (or `object.take`), and while it runs select a different object in the Build floater | The user's selection is unchanged afterwards. |
| 12 | Select the cube in the Build floater, then run `object.get` on that same cube | The floater still shows it selected, and moving it with the arrows still works (no deselect was sent). |
| 13 | Sit on the two-prim linkset, then `object.get` and `object.setFaces link:2 faces:[0] color:[1,0,0]` | `link_count` 2 (the avatar isn't a link); link 2's face turns red. |
| 14 | `object.addContents` the same notecard twice, in two calls | Both calls report `added`; the object holds the notecard and a renamed copy. |
| 15 | On a two-prim linkset, `object.setFaces faces:"all"` with the no-copy texture | Link 1's faces get the texture; link 2's faces report the item is no longer in inventory; no crash. |
| 16 | Start `object.take` on an object, then kill the MCP client before it answers. Reconnect and run `object.get` on another object | No crash; the second call works. |
| 17 | `avatars.getWorn` on a nearby avatar and `objects.getNearby` | Names still resolve (the properties hook chain still feeds them). |
| 18 | `object.rez` an item while `@rez=n` is active; `object.attach` to a point locked with `@addattach:chest=n` | -32011 `restriction:"rez"`; -32011 `restriction:"addattach"`. |

- [ ] **Step 3: Record results and fix**

For any failure, use superpowers:systematic-debugging before changing code. Commit each fix as `fix: ...` on this branch.
