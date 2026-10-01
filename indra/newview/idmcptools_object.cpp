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
#include "lldbstrings.h"            // DB_INV_ITEM_NAME_STR_LEN, DB_INV_ITEM_DESC_STR_LEN
#include "llinventorydefines.h"     // TASK_INVENTORY_ITEM_KEY, LLInventoryItemFlags
#include "llworld.h"                // region min/max prim scale
#include "llevents.h"               // LLEventPumps, LLTempBoundListener
#include "llinventorymodel.h"       // gInventory
#include "llselectmgr.h"            // UPD_* constants only; no selection calls
#include "lltextureentry.h"
#include "lltimer.h"
#include "llviewerinventory.h"       // LLViewerInventoryItem (worn rename)
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
        return LLUUID::validate(s);
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
        LLViewerInventoryItem* item = gInventory.getItem(gInventory.getLinkedItemID(id));
        // An item nothing has displayed yet stays incomplete until something
        // asks for it. Ask, so the callers' "try again" can succeed.
        if (item && !item->isFinished()) item->fetchFromServer();
        return item;
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

    bool ContentsFetch::done()
    {
        if (!mDone && mRegistered)
        {
            // start() leaves the prim's request pending, or calls back at once. The
            // viewer resets that state only after it has called every listener, so
            // "not done and not pending" means the request ended with no answer
            // (llviewerobject.cpp fetchInventoryFromCapCoro: HTTP error status).
            LLViewerObject* o = gObjectList.findObject(mObjectId);
            if (o && !o->isInventoryPending())
            {
                mFailed = true;
                mDone   = true;
            }
        }
        return mDone;
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

namespace
{
    constexpr U32 PERM_CMT = PERM_COPY | PERM_MODIFY | PERM_TRANSFER;

    // ---- object.edit -------------------------------------------------------------

    constexpr F64 EDIT_LIMIT = 10.0;   // spec: echo wait 10 s

    // ObjectName / ObjectDescription. Their ObjectData field is LocalID, not ObjectLocalID.
    // The Build floater's rule for names and descriptions: printable ASCII, no '|'.
    bool printable_no_pipe(const std::string& s)
    {
        for (unsigned char c : s)
        {
            if (c < 0x20 || c > 0x7E || c == '|') return false;
        }
        return true;
    }

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
            if (!p || !p->getRegion())
            {
                fail(IDMCP_ERR_NOT_FOUND, "object's region isn't available");
                return;
            }
            if (mLink == 0)
            {
                // Whole object: position and rotation move the linkset. Scale
                // goes in its own entry without UPD_LINKED_SETS, so only the
                // root prim scales.
                const U8 type = (U8)((mHasPos ? UPD_POSITION : 0) | (mHasRot ? UPD_ROTATION : 0));
                if (type) sendTransform(p, (U8)(type | UPD_LINKED_SETS), mPos, mRot, mScale);
                // A scale update always carries a position, as LLPanelObject
                // sends it: scale alone throws a worn object out to the 3.5 m
                // attachment limit.
                if (mHasScale)
                    sendTransform(p, UPD_SCALE | UPD_POSITION, mHasPos ? mPos : p->getPosition(), mRot, mScale);
            }
            else
            {
                const bool with_pos = mHasPos || mHasScale;
                const U8 type = (U8)((with_pos ? UPD_POSITION : 0) | (mHasRot ? UPD_ROTATION : 0)
                                     | (mHasScale ? UPD_SCALE : 0));
                if (type) sendTransform(p, type, mHasPos ? mPos : p->getPosition(), mRot, mScale);
            }
            if (mHasName) send_text(p, _PREHASH_ObjectName, _PREHASH_Name, mName);
            if (mHasDesc) send_text(p, _PREHASH_ObjectDescription, _PREHASH_Description, mDesc);
            if ((mHasName || mHasDesc) && p->isAttachment() && p->isRootEdit())
            {
                // A worn object keeps the name and description of its inventory
                // item when it's detached, so update the item too, as
                // LLPanelPermissions::onCommitName/onCommitDesc do.
                if (LLViewerInventoryItem* item = gInventory.getItem(p->getAttachmentItemID()))
                {
                    LLPointer<LLViewerInventoryItem> new_item = new LLViewerInventoryItem(item);
                    if (mHasName) new_item->rename(mName);
                    if (mHasDesc) new_item->setDescription(mDesc);
                    new_item->setComplete(true);
                    new_item->updateServer(false);
                    gInventory.updateItem(new_item);
                    gInventory.notifyObservers();
                }
            }
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
            if (!printable_no_pipe(job->mName))
            {
                bad("name must be printable ASCII without '|'");
                return;
            }
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
            if (!printable_no_pipe(job->mDesc))
            {
                bad("description must be printable ASCII without '|'");
                return;
            }
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
            if (!root || !root->getRegion())
            {
                fail(IDMCP_ERR_NOT_FOUND, "object's region isn't available");
                return;
            }
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
                // The region's rule (LLPermissions::setNextOwnerBits + fix): without copy,
                // transfer is forced on; then everything is cut to the base mask.
                U32 expect = pr ? (mWant & pr->base) : 0;
                if (pr && !(expect & PERM_COPY)) expect |= PERM_TRANSFER;
                if (!pr || (pr->next_owner & PERM_CMT) != (expect & pr->base & PERM_CMT))
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
                fail(IDMCP_ERR_CAP_UNAVAIL, "the region couldn't send the object's contents; try again");
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

        std::string timeoutMessage() const override
        {
            return mObjectDone ? "next-owner permissions are set on the object; its contents didn't arrive in time"
                               : "the region didn't confirm in time";
        }

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
            r["result"]     = "sent";
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
        if (t.root->isAttachment())
        {
            // The region ignores next-owner changes on a worn object (seen live).
            idmcp_tool_err(call, IDMCP_ERR_INVALID_PARAMS,
                           "next-owner permissions can't be changed while the object is worn; rez it first");
            return;
        }
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

        boost::json::object timeoutData() const override
        {
            boost::json::array ids;
            for (const LLUUID& id : mTargets) ids.emplace_back(id.asString());
            boost::json::object sent;
            sent["object_ids"] = std::move(ids);
            boost::json::object data;
            data["sent"] = std::move(sent);
            return data;
        }

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
            "Set the next-owner permissions of a rezzed object you own and can modify. "
            "{\"object_id\"}*, {\"next_owner\"}*: {copy, modify, transfer} booleans; the next "
            "owner needs copy or transfer. {\"contents\":true} also sets the same next-owner "
            "permissions on every item inside the root prim. Returns {object_id, next_owner, "
            "contents:[{item_id, name, result}]} where result is sent (the update was sent; "
            "the region doesn't confirm it), unchanged or permission. Turning copy off keeps "
            "transfer on, and bits missing from the object's base permissions stay off. Blocked by RLV "
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
}
