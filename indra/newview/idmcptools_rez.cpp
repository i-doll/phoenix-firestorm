/**
 * @file idmcptools_rez.cpp
 * @brief <ID> MCP server: object.rez, object.create, object.attach and object.take.
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
#include "llinventorydefines.h"      // LLInventoryItemFlags
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
#include "llvolumemessage.h"
#include "material_codes.h"         // LL_MCODE_WOOD
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

    // args "position" (region metres), or 2 m in front of the avatar at its
    // height. On failure answers `call` and returns false.
    bool parse_target(const boost::json::object& args, const IDMCPCallPtr& call, LLViewerRegion* region,
                      LLVector3& target)
    {
        const F32 width = region->getWidth();
        if (args.contains("position"))
        {
            if (!argNums(args, "position", 3, target.mV))
            {
                idmcp_tool_err(call, IDMCP_ERR_INVALID_PARAMS, "position must be [x, y, z] region metres");
                return false;
            }
            if (target.mV[VX] < 0.f || target.mV[VX] >= width || target.mV[VY] < 0.f || target.mV[VY] >= width
                || target.mV[VZ] < 0.f || target.mV[VZ] > BUILD_HEIGHT)
            {
                idmcp_tool_err(call, IDMCP_ERR_INVALID_PARAMS,
                               llformat("position must be inside this region (x and y 0 to %.0f, z 0 to %.0f)",
                                        width, BUILD_HEIGHT));
                return false;
            }
            return true;
        }
        LLVector3 ahead = gAgent.getAtAxis();
        ahead.mV[VZ] = 0.f;
        if (ahead.normalize() < 0.01f) ahead = LLVector3::x_axis;
        target = region->getPosRegionFromGlobal(gAgent.getPositionGlobal()) + ahead * 2.f;
        target.mV[VX] = llclamp(target.mV[VX], 0.f, width - 0.01f);
        target.mV[VY] = llclamp(target.mV[VY], 0.f, width - 0.01f);
        return true;
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
        // Start == end with BypassRaycast places the object exactly at target, as
        // Firestorm's rez-platform command and importer do. A short ray above the
        // target fails mid-air: "Can't rez object: failed to calculate rez position."
        msg->addVector3Fast(_PREHASH_RayStart, target);
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
            return mFound.isNull() ? "the rez was sent but the new object didn't appear in time; check objects.getNearby before retrying"
                                   : "the object was rezzed but the region didn't confirm the rotation in time";
        }

        boost::json::object timeoutData() const override
        {
            boost::json::object sent;
            sent["item_id"]  = mItemId.asString();
            sent["position"] = vecJson(mTarget);
            boost::json::object data;
            data["sent"] = std::move(sent);
            if (mFound.notNull()) data["object_id"] = mFound.asString();
            if (mConsumed)
            {
                data["item_consumed"] = true;
                data["note"] = "a no-copy item leaves your inventory when rezzed; if the rez failed, the region returns it";
            }
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
            if (!o->permModify())
            {
                mRotNote = "the object is no-modify, so the rotation wasn't applied";
                return;
            }
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
        if (item->getFlags() & LLInventoryItemFlags::II_FLAGS_OBJECT_HAS_MULTIPLE_ITEMS)
        {
            idmcp_tool_err(call, IDMCP_ERR_INVALID_PARAMS,
                           "coalesced items (several objects in one item) can't be rezzed with this tool");
            return;
        }
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
        LLVector3 target;
        if (!parse_target(args, call, region, target)) return;
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

    // ---- object.create -----------------------------------------------------------

    constexpr F64 CREATE_LIMIT = 30.0;
    constexpr F32 PRIM_MIN     = 0.01f;
    constexpr F32 PRIM_MAX     = 64.f;

    // The build tool's shapes, with the volume params LLToolPlacer::addObject sends.
    struct Shape
    {
        const char* name;
        U8          profile;
        U8          path;
        F32         begin_s, end_s;
        F32         begin_t, end_t;
        F32         ratio_x, ratio_y;
        F32         shear_x;
        bool        tilted;   // the build tool rezzes it turned 90 degrees about Y
    };

    const Shape SHAPES[] = {
        { "box",           LL_PCODE_PROFILE_SQUARE,      LL_PCODE_PATH_LINE,   0.f,   1.f,   0.f, 1.f,  1.f, 1.f,   0.f,  false },
        { "prism",         LL_PCODE_PROFILE_SQUARE,      LL_PCODE_PATH_LINE,   0.f,   1.f,   0.f, 1.f,  0.f, 1.f,  -0.5f, false },
        { "pyramid",       LL_PCODE_PROFILE_SQUARE,      LL_PCODE_PATH_LINE,   0.f,   1.f,   0.f, 1.f,  0.f, 0.f,   0.f,  false },
        { "tetrahedron",   LL_PCODE_PROFILE_EQUALTRI,    LL_PCODE_PATH_LINE,   0.f,   1.f,   0.f, 1.f,  0.f, 0.f,   0.f,  false },
        { "cylinder",      LL_PCODE_PROFILE_CIRCLE,      LL_PCODE_PATH_LINE,   0.f,   1.f,   0.f, 1.f,  1.f, 1.f,   0.f,  false },
        { "half_cylinder", LL_PCODE_PROFILE_CIRCLE,      LL_PCODE_PATH_LINE,   0.25f, 0.75f, 0.f, 1.f,  1.f, 1.f,   0.f,  false },
        { "cone",          LL_PCODE_PROFILE_CIRCLE,      LL_PCODE_PATH_LINE,   0.f,   1.f,   0.f, 1.f,  0.f, 0.f,   0.f,  false },
        { "half_cone",     LL_PCODE_PROFILE_CIRCLE,      LL_PCODE_PATH_LINE,   0.25f, 0.75f, 0.f, 1.f,  0.f, 0.f,   0.f,  false },
        { "sphere",        LL_PCODE_PROFILE_CIRCLE_HALF, LL_PCODE_PATH_CIRCLE, 0.f,   1.f,   0.f, 1.f,  1.f, 1.f,   0.f,  true  },
        { "half_sphere",   LL_PCODE_PROFILE_CIRCLE_HALF, LL_PCODE_PATH_CIRCLE, 0.f,   1.f,   0.f, 0.5f, 1.f, 1.f,   0.f,  false },
        { "torus",         LL_PCODE_PROFILE_CIRCLE,      LL_PCODE_PATH_CIRCLE, 0.f,   1.f,   0.f, 1.f,  1.f, 0.25f, 0.f,  true  },
        { "tube",          LL_PCODE_PROFILE_SQUARE,      LL_PCODE_PATH_CIRCLE, 0.f,   1.f,   0.f, 1.f,  1.f, 0.25f, 0.f,  true  },
        { "ring",          LL_PCODE_PROFILE_EQUALTRI,    LL_PCODE_PATH_CIRCLE, 0.f,   1.f,   0.f, 1.f,  1.f, 0.25f, 0.f,  true  },
    };

    const Shape* find_shape(const std::string& name)
    {
        const std::string want = lower(name);
        for (const Shape& sh : SHAPES)
        {
            if (want == sh.name) return &sh;
        }
        return nullptr;
    }

    // LLToolPlacer::addObject without FLAGS_CREATE_SELECTED, so nothing gets
    // selected and the default build preferences aren't applied.
    void send_add(LLViewerRegion* region, const Shape& sh, const LLVector3& target, const LLVector3& size,
                  const LLQuaternion& rot)
    {
        LLVolumeParams vp;
        vp.setType(sh.profile, sh.path);
        vp.setBeginAndEndS(sh.begin_s, sh.end_s);
        vp.setBeginAndEndT(sh.begin_t, sh.end_t);
        vp.setRatio(sh.ratio_x, sh.ratio_y);
        vp.setShear(sh.shear_x, 0.f);

        LLMessageSystem* msg = gMessageSystem;
        msg->newMessageFast(_PREHASH_ObjectAdd);
        msg->nextBlockFast(_PREHASH_AgentData);
        msg->addUUIDFast(_PREHASH_AgentID, gAgent.getID());
        msg->addUUIDFast(_PREHASH_SessionID, gAgent.getSessionID());
        msg->addUUIDFast(_PREHASH_GroupID, FSCommon::getGroupForRezzing());
        msg->nextBlockFast(_PREHASH_ObjectData);
        msg->addU8Fast(_PREHASH_Material, LL_MCODE_WOOD);
        msg->addU32Fast(_PREHASH_AddFlags, 0);
        LLVolumeMessage::packVolumeParams(&vp, msg);
        msg->addU8Fast(_PREHASH_PCode, LL_PCODE_VOLUME);
        msg->addVector3Fast(_PREHASH_Scale, size);
        msg->addQuatFast(_PREHASH_Rotation, rot);
        msg->addVector3Fast(_PREHASH_RayStart, target);
        msg->addVector3Fast(_PREHASH_RayEnd, target);
        msg->addU8Fast(_PREHASH_BypassRaycast, (U8)1);
        msg->addU8Fast(_PREHASH_RayEndIsIntersection, (U8)false);
        msg->addU8Fast(_PREHASH_State, (U8)0);
        msg->addUUIDFast(_PREHASH_RayTargetID, LLUUID::null);
        msg->sendReliable(region->getHost());
    }

    class CreateJob : public Job
    {
    public:
        CreateJob(const IDMCPCallPtr& call, U64 region_handle, const LLVector3& target, const LLVector3& size,
                  std::set<LLUUID> before)
            : Job(call, CREATE_LIMIT), mRegionHandle(region_handle), mTarget(target), mSize(size),
              mBefore(std::move(before)) {}

        std::string mShape;

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
            if (mMoved && !posMatches(o->getPositionRegion(), mTarget)) return;
            boost::json::object out;
            out["object_id"] = o->getID().asString();
            out["shape"]     = mShape;
            out["position"]  = vecJson(o->getPosition());
            out["rotation"]  = eulerDegJson(o->getRotation());
            out["scale"]     = vecJson(o->getScale());
            if (!mPosNote.empty()) out["position_skipped"] = mPosNote;
            succeed(std::move(out));
        }

        std::string timeoutMessage() const override
        {
            return mFound.isNull() ? "the create request was sent but the new object didn't appear in time; the land may not "
                                     "allow building here. Check objects.getNearby before retrying"
                                   : "the object was created but the region didn't confirm the position in time";
        }

        boost::json::object timeoutData() const override
        {
            boost::json::object sent;
            sent["shape"]    = mShape;
            sent["position"] = vecJson(mTarget);
            sent["scale"]    = vecJson(mSize);
            boost::json::object data;
            data["sent"] = std::move(sent);
            if (mFound.notNull()) data["object_id"] = mFound.asString();
            return data;
        }

    private:
        // The region doesn't say which object it made: take the first new owned
        // root near the target with the size we asked for.
        void find_new()
        {
            for (const LLUUID& id : own_roots_near(mRegionHandle, mTarget))
            {
                if (mBefore.count(id)) continue;
                LLViewerObject* o = gObjectList.findObject(id);
                if (!o || !scaleMatches(o->getScale(), mSize)) continue;
                mFound = id;
                if (posMatches(o->getPositionRegion(), mTarget)) return;
                // The region may set it down off the target; moving it there is an edit.
                const IDMCPGateResult g = rlvEdit(o);
                if (!g.allowed)
                {
                    mPosNote = rlvErrorMessage(g);
                    return;
                }
                sendTransform(o, (U8)(UPD_POSITION | UPD_LINKED_SETS), mTarget, LLQuaternion(), LLVector3::zero);
                mMoved = true;
                return;
            }
        }

        U64              mRegionHandle = 0;
        LLVector3        mTarget;
        LLVector3        mSize;
        std::set<LLUUID> mBefore;   // owned roots near the target before the create
        LLUUID           mFound;
        bool             mMoved = false;
        std::string      mPosNote;  // why the position wasn't corrected
    };

    void run_create(const boost::json::object& args, const IDMCPCallPtr& call)
    {
        const std::string shape_arg = argStr(args, "shape");
        const Shape* sh = find_shape(shape_arg.empty() ? "box" : shape_arg);
        if (!sh)
        {
            std::string names;
            for (const Shape& s : SHAPES) names += (names.empty() ? "" : ", ") + std::string(s.name);
            idmcp_tool_err(call, IDMCP_ERR_INVALID_PARAMS, "unknown shape: " + shape_arg + " (one of " + names + ")");
            return;
        }
        LLViewerRegion* region = gAgent.getRegion();
        if (!region)
        {
            idmcp_tool_err(call, IDMCP_ERR_NOT_LOGGED_IN, "no current region");
            return;
        }
        LLVector3 target;
        if (!parse_target(args, call, region, target)) return;
        LLVector3 size(0.5f, 0.5f, 0.5f);
        if (args.contains("scale"))
        {
            if (!argNums(args, "scale", 3, size.mV))
            {
                idmcp_tool_err(call, IDMCP_ERR_INVALID_PARAMS, "scale must be [x, y, z] metres");
                return;
            }
            for (F32 v : size.mV)
            {
                if (v < PRIM_MIN || v > PRIM_MAX)
                {
                    idmcp_tool_err(call, IDMCP_ERR_INVALID_PARAMS,
                                   llformat("each scale value must be %.2f to %.0f metres", PRIM_MIN, PRIM_MAX));
                    return;
                }
            }
        }
        LLQuaternion rot;
        if (sh->tilted) rot.setQuat(90.f * DEG_TO_RAD, LLVector3::y_axis);
        if (args.contains("rotation"))
        {
            LLVector3 deg;
            if (!argNums(args, "rotation", 3, deg.mV))
            {
                idmcp_tool_err(call, IDMCP_ERR_INVALID_PARAMS, "rotation must be [x, y, z] in degrees");
                return;
            }
            rot = quatFromEulerDeg(deg);
        }

        auto job = std::make_shared<CreateJob>(call, region->getHandle(), target, size,
                                               own_roots_near(region->getHandle(), target));
        job->mShape = sh->name;
        // The request gate ran this frame, so it is the commit check for the create.
        send_add(region, *sh, target, size, rot);
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

        std::string timeoutMessage() const override { return "the attach request was sent but the object didn't appear on the point in time; check appearance.getWorn before retrying"; }

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
            return mSent ? "the take request was sent but the new item didn't appear in time; check the folder before retrying"
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
            "item, which leaves your inventory. If the new object is no-modify or RLV blocks editing it, the "
            "result has rotation_skipped with the reason. Blocked by RLV @rez and @interact.";
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

    // object.create ------------------------------------------------------------
    {
        IDMCPTool t;
        t.name = "object.create";
        t.description =
            "Create a new prim in the world, like the build tool's Create button. {\"shape\"}: "
            "box (default), prism, pyramid, tetrahedron, cylinder, half_cylinder, cone, half_cone, "
            "sphere, half_sphere, torus, tube or ring. {\"position\"} [x,y,z] region metres "
            "(default 2 m in front of you, at your height); {\"scale\"} [x,y,z] metres, 0.01 to 64 "
            "(default 0.5 each); {\"rotation\"} [x,y,z] degrees (default the build tool's: sphere, "
            "torus, tube and ring are turned 90 degrees about Y). The prim gets the default plywood "
            "texture and the name \"Object\". Waits up to 30 s and returns {object_id, shape, position, "
            "rotation, scale}; use object_id with object.edit, object.setFaces, object.addContents and "
            "object.link. If RLV blocks editing, the prim stays where the region put it and the result "
            "has position_skipped with the reason. Blocked by RLV @rez and @interact.";
        t.input_schema = boost::json::parse(
            R"({"type":"object","properties":{"shape":{"type":"string","enum":["box","prism","pyramid",)"
            R"("tetrahedron","cylinder","half_cylinder","cone","half_cone","sphere","half_sphere","torus",)"
            R"("tube","ring"]},)"
            R"("position":{"type":"array","items":{"type":"number"},"minItems":3,"maxItems":3},)"
            R"("scale":{"type":"array","items":{"type":"number"},"minItems":3,"maxItems":3},)"
            R"("rotation":{"type":"array","items":{"type":"number"},"minItems":3,"maxItems":3}},)"
            R"("additionalProperties":false})");
        t.gate    = gate_rez;
        t.timeout = 45.0;
        t.invoke  = [](const boost::json::object& args, const IDMCPCallPtr& call) { run_create(args, call); };
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
