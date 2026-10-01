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
        // Registered with the prim by address; a copy would claim a registration it never made.
        ContentsFetch(const ContentsFetch&) = delete;
        ContentsFetch& operator=(const ContentsFetch&) = delete;

        // Registers on prim and asks the region for a fresh copy. The answer can
        // arrive before this returns: requestInventory() calls back at once when
        // it already holds the contents.
        void start(LLViewerObject* prim);
        void stop();
        // Also notices a request the viewer abandoned without calling back (see .cpp).
        bool done();
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
