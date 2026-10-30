/**
 * @file idmcptools_headless.cpp
 * @brief <ID> Headless test mode: session lifecycle and MCP session tools.
 *
 * Part of Five's custom Firestorm fork.
 */

#include "llviewerprecompiledheaders.h"

#include "idmcp.h"
#include "idmcptools.h"
#include "idmcpserver.h"
#include "idmcptools_headless.h"
#include "idheadless.h"

#include "idmcptools_object.h"

#include "llagent.h"
#include "llagentcamera.h"
#include "llevents.h"
#include "llfile.h"
#include "llinventorymodel.h"
#include "lltimer.h"
#include "llviewercontrol.h"
#include "llviewerobject.h"
#include "llviewerobjectlist.h"
#include "llviewerregion.h"
#include "rlvinventory.h"

#include <boost/json.hpp>
#include <cerrno>
#include <cstdlib>
#include <ctime>
#include <fstream>
#include <map>
#include <set>
#include <vector>

namespace idmcp_headless
{

namespace
{
constexpr F32 NEAR_RANGE  = 64.f;
constexpr F32 LOAD_MARGIN = 16.f;
constexpr F64 SETTLE_SECS = 5.0;
constexpr F64 WAIT_SECS   = 15.0;
constexpr F64 ADOPT_SECS  = 30.0;

struct Entry
{
    LLUUID      id;
    U64         region = 0;
    std::string region_name;
    LLVector3   pos;
    LLVector3   persisted_pos;
    std::string tool;
    S64         t = 0;
    F64         added_at = -1.0e9;   // this session only
    F64         missing_since = 0.0;
};

std::map<LLUUID, Entry> s_ledger;
std::set<LLUUID> s_keep;

std::string ledgerPath()
{
    const std::string& dir = idheadless::avatarDir();
    return dir.empty() ? std::string() : dir + "/ledger.jsonl";
}

boost::json::array posJson(const LLVector3& p)
{
    return boost::json::array{ (double)p.mV[VX], (double)p.mV[VY], (double)p.mV[VZ] };
}

boost::json::object entryJson(const Entry& e)
{
    boost::json::object o;
    o["op"]          = "add";
    o["id"]          = e.id.asString();
    o["region"]      = std::to_string(e.region);
    o["region_name"] = e.region_name;
    o["pos"]         = posJson(e.pos);
    o["tool"]        = e.tool;
    o["t"]           = (int64_t)e.t;
    return o;
}

void appendLine(const boost::json::object& rec)
{
    const std::string path = ledgerPath();
    if (path.empty()) return;
    llofstream out(path.c_str(), std::ios::out | std::ios::app);
    if (!out.is_open())
    {
        LL_WARNS("Headless") << "Cannot append to " << path << LL_ENDL;
        return;
    }
    out << boost::json::serialize(rec) << std::endl;
}

void loadLedger()
{
    s_ledger.clear();
    const std::string path = ledgerPath();
    if (path.empty()) return;
    llifstream in(path.c_str());
    std::string line;
    while (std::getline(in, line))
    {
        boost::system::error_code ec;
        const boost::json::value v = boost::json::parse(line, ec);
        if (ec || !v.is_object()) continue;
        const boost::json::object& o = v.as_object();
        const auto op = o.find("op");
        const auto id = o.find("id");
        if (op == o.end() || id == o.end() || !op->value().is_string() || !id->value().is_string()) continue;
        const LLUUID uuid(std::string(id->value().as_string().c_str()));
        if (uuid.isNull()) continue;
        if (op->value().as_string() == "remove")
        {
            s_ledger.erase(uuid);
            continue;
        }
        Entry e;
        e.id = uuid;
        if (const auto r = o.find("region"); r != o.end() && r->value().is_string())
        {
            e.region = std::strtoull(r->value().as_string().c_str(), nullptr, 10);
        }
        if (const auto r = o.find("region_name"); r != o.end() && r->value().is_string())
        {
            e.region_name = std::string(r->value().as_string().c_str());
        }
        if (const auto r = o.find("tool"); r != o.end() && r->value().is_string())
        {
            e.tool = std::string(r->value().as_string().c_str());
        }
        if (const auto r = o.find("t"); r != o.end() && r->value().is_number())
        {
            e.t = r->value().to_number<int64_t>();
        }
        if (const auto r = o.find("pos"); r != o.end() && r->value().is_array() && r->value().as_array().size() == 3)
        {
            for (size_t i = 0; i < 3; ++i)
            {
                e.pos.mV[i] = (F32)r->value().as_array()[i].to_number<double>();
            }
        }
        e.persisted_pos = e.pos;
        s_ledger[uuid] = e;
    }
}

void rewriteLedger()
{
    const std::string path = ledgerPath();
    if (path.empty()) return;
    if (s_ledger.empty())
    {
        LLFile::remove(path, ENOENT);
        return;
    }
    const std::string tmp = path + ".tmp";
    bool ok = false;
    {
        llofstream out(tmp.c_str(), std::ios::out | std::ios::trunc);
        if (!out.is_open())
        {
            LL_WARNS("Headless") << "Cannot write " << tmp << LL_ENDL;
            return;
        }
        for (auto& kv : s_ledger)
        {
            out << boost::json::serialize(entryJson(kv.second)) << '\n';
            kv.second.persisted_pos = kv.second.pos;
        }
        out.flush();
        ok = out.good();
    }
    if (!ok)
    {
        LL_WARNS("Headless") << "Failed writing " << tmp << ", keeping the old ledger" << LL_ENDL;
        LLFile::remove(tmp, ENOENT);
        return;
    }
    LLFile::rename(tmp, path);
}

F32 goneRange()
{
    return llmin(NEAR_RANGE, gAgentCamera.mDrawDistance - LOAD_MARGIN);
}

LLVector3 agentRegionPos(LLViewerRegion* region)
{
    return region->getPosRegionFromGlobal(gAgent.getPositionGlobal());
}

bool gone(const LLUUID& id)
{
    LLViewerObject* o = gObjectList.findObject(id);
    return !o || o->isDead();
}

void addPrim(LLViewerObject* prim, const std::string& tool, F64 added_at)
{
    if (s_keep.count(prim->getID())) return;
    LLViewerRegion* region = prim->getRegion();
    Entry e;
    e.id          = prim->getID();
    e.region      = region ? region->getHandle() : 0;
    e.region_name = region ? region->getName() : std::string();
    e.pos         = prim->getPositionRegion();
    e.tool        = tool;
    e.t           = (S64)time_corrected();
    e.persisted_pos = e.pos;
    e.added_at    = added_at;
    s_ledger[e.id] = e;
    appendLine(entryJson(e));
}

void watchTick()
{
    static F64 s_last = 0.0;
    static U64 s_seen_region = 0;
    static F64 s_region_changed_at = 0.0;
    const F64 now = LLTimer::getTotalSeconds();
    if (now - s_last < 1.0) return;
    s_last = now;
    LLViewerRegion* region = gAgent.getRegion();
    const U64 handle = region ? region->getHandle() : 0;
    if (handle != s_seen_region)
    {
        s_seen_region = handle;
        s_region_changed_at = now;
    }
    if (s_ledger.empty() || !region || idheadless::statusString("state") != "ready") return;
    const LLVector3 me = agentRegionPos(region);
    const bool settled = now - s_region_changed_at >= WAIT_SECS;
    std::vector<LLUUID> dead;
    std::vector<std::pair<LLViewerObject*, Entry>> adopt;
    for (auto& kv : s_ledger)
    {
        Entry& e = kv.second;
        if (e.region != handle)
        {
            e.missing_since = 0.0;
            continue;
        }
        LLViewerObject* o = gObjectList.findObject(e.id);
        if (!o || o->isDead())
        {
            if (e.missing_since == 0.0) e.missing_since = now;
            if (settled && now - e.missing_since >= WAIT_SECS && dist_vec(me, e.pos) < goneRange())
            {
                dead.push_back(e.id);
            }
        }
        else
        {
            e.missing_since = 0.0;
            e.pos = o->getPositionRegion();
            if (dist_vec(e.pos, e.persisted_pos) > 1.f)
            {
                e.persisted_pos = e.pos;
                appendLine(entryJson(e));
            }
            if (now - e.added_at < ADOPT_SECS)
            {
                adopt.emplace_back(o, e);
            }
        }
    }
    for (const auto& pr : adopt)
    {
        LLViewerObject* root = pr.first->getRootEdit();
        for (LLViewerObject* prim : idmcp_obj::linksOf(root))
        {
            if (!s_ledger.count(prim->getID())) addPrim(prim, pr.second.tool, pr.second.added_at);
        }
    }
    for (const LLUUID& id : dead)
    {
        ledgerRemove(id);
    }
}

bool s_watching = false;

void startWatch()
{
    if (s_watching) return;
    s_watching = true;
    LLEventPumps::instance().obtain("mainloop").listen("IDHeadlessLedgerWatch", [](const LLSD&)
    {
        watchTick();
        return false;
    });
}

enum class Stage { Idle, Settle, Classify, WaitDerez };

struct Cleanup
{
    Stage                              stage = Stage::Idle;
    std::string                        phase;
    F64                                settle_until = 0.0;
    std::map<LLUUID, F64>              missing_since;
    std::map<LLUUID, std::set<LLUUID>> derezzed;   // root id -> its ledgered prims
    F64                                derez_deadline = 0.0;
    S32                                derezzed_count = 0;
    S32                                gone_count = 0;
    std::set<LLUUID>                   mixed;         // roots left in-world
    std::set<LLUUID>                   mixed_prims;   // every prim of those linksets
    std::set<LLUUID>                   rlv_prims;     // prims of roots RLV would not let us derez
    std::vector<std::function<void()>> done;
    LLBoundListener                    listener;
};

struct Queued
{
    std::string           phase;
    F64                   settle;
    std::function<void()> done;
};

std::vector<Queued> s_queued;

void startPass(const std::string& phase, F64 settle, std::function<void()> done);

Cleanup s_cleanup;

void finishCleanup()
{
    Cleanup& c = s_cleanup;
    boost::json::array unresolved;
    for (const auto& kv : s_ledger)
    {
        const Entry& e = kv.second;
        if (c.mixed_prims.count(e.id)) continue;
        boost::json::object u;
        u["id"]          = e.id.asString();
        u["region_name"] = e.region_name;
        if (c.rlv_prims.count(e.id)) u["reason"] = "rlv";
        unresolved.push_back(std::move(u));
    }
    rewriteLedger();

    boost::json::array mixed;
    for (const LLUUID& id : c.mixed)
    {
        mixed.push_back(boost::json::value(id.asString()));
    }
    boost::json::object report;
    report["phase"]      = c.phase;
    report["derezzed"]   = (int64_t)c.derezzed_count;
    report["gone"]       = (int64_t)c.gone_count;
    report["mixed"]      = mixed;
    report["unresolved"] = unresolved;
    LL_INFOS("Headless") << "Cleanup (" << c.phase << "): derezzed " << c.derezzed_count << ", gone " << c.gone_count
                         << ", mixed " << mixed.size() << ", unresolved " << unresolved.size() << LL_ENDL;
    idheadless::setStatusField("cleanup", report);

    std::vector<std::function<void()>> done;
    done.swap(c.done);
    c.listener.disconnect();
    c.stage = Stage::Idle;
    c.missing_since.clear();
    c.derezzed.clear();
    c.mixed.clear();
    c.mixed_prims.clear();
    c.rlv_prims.clear();
    for (auto& fn : done)
    {
        if (fn) fn();
    }
    if (!s_queued.empty())
    {
        Queued q = std::move(s_queued.front());
        s_queued.erase(s_queued.begin());
        startPass(q.phase, q.settle, std::move(q.done));
    }
}

void classify()
{
    Cleanup& c = s_cleanup;
    LLViewerRegion* region = gAgent.getRegion();
    const U64 handle = region ? region->getHandle() : 0;
    const LLVector3 me = region ? agentRegionPos(region) : LLVector3::zero;

    std::vector<LLUUID> ids;
    for (const auto& kv : s_ledger) ids.push_back(kv.first);

    std::map<LLUUID, LLViewerObject*> roots;
    for (const LLUUID& id : ids)
    {
        const auto it = s_ledger.find(id);
        if (it == s_ledger.end()) continue;
        const Entry e = it->second;
        if (!region || e.region != handle) continue;
        LLViewerObject* o = gObjectList.findObject(id);
        if (!o || o->isDead())
        {
            if (dist_vec(me, e.pos) < goneRange())
            {
                ++c.gone_count;
                ledgerRemove(id);
            }
            continue;
        }
        LLViewerObject* root = o->getRootEdit();
        if (!root || roots.count(root->getID()) || c.mixed.count(root->getID())) continue;
        bool all = true;
        const std::vector<LLViewerObject*> links = idmcp_obj::linksOf(root);
        for (LLViewerObject* prim : links)
        {
            if (!s_ledger.count(prim->getID())) { all = false; break; }
        }
        if (all)
        {
            roots[root->getID()] = root;
        }
        else
        {
            c.mixed.insert(root->getID());
            for (LLViewerObject* prim : links) c.mixed_prims.insert(prim->getID());
        }
    }

    const LLUUID trash = gInventory.findCategoryUUIDForType(LLFolderType::FT_TRASH);
    if (roots.empty() || trash.isNull())
    {
        finishCleanup();
        return;
    }
    for (const auto& kv : roots)
    {
        std::set<LLUUID> prims;
        for (LLViewerObject* prim : idmcp_obj::linksOf(kv.second))
        {
            prims.insert(prim->getID());
        }
        if (!idmcp_obj::rlvTakeGate(kv.second).allowed)
        {
            c.rlv_prims.insert(prims.begin(), prims.end());
            continue;
        }
        c.derezzed[kv.first] = std::move(prims);
        idmcp_obj::sendDerez(kv.second, DRD_TRASH, trash);
    }
    if (c.derezzed.empty())
    {
        finishCleanup();
        return;
    }
    c.derez_deadline = LLTimer::getTotalSeconds() + WAIT_SECS;
    c.stage = Stage::WaitDerez;
}

void waitDerez()
{
    Cleanup& c = s_cleanup;
    for (auto it = c.derezzed.begin(); it != c.derezzed.end();)
    {
        bool all_gone = true;
        for (const LLUUID& id : it->second)
        {
            if (!gone(id)) { all_gone = false; break; }
        }
        if (all_gone)
        {
            for (const LLUUID& id : it->second)
            {
                ledgerRemove(id);
            }
            ++c.derezzed_count;
            it = c.derezzed.erase(it);
        }
        else
        {
            ++it;
        }
    }
    if (c.derezzed.empty() || LLTimer::getTotalSeconds() >= c.derez_deadline)
    {
        finishCleanup();
    }
}

void cleanupTick()
{
    Cleanup& c = s_cleanup;
    const F64 now = LLTimer::getTotalSeconds();
    switch (c.stage)
    {
    case Stage::Settle:
        if (now >= c.settle_until) c.stage = Stage::Classify;
        break;
    case Stage::Classify:
    {
        LLViewerRegion* region = gAgent.getRegion();
        bool pending = false;
        if (region)
        {
            for (const auto& kv : s_ledger)
            {
                if (kv.second.region != region->getHandle()) continue;
                if (gone(kv.first))
                {
                    const F64 since = c.missing_since.emplace(kv.first, now).first->second;
                    if (now - since < WAIT_SECS) pending = true;
                }
                else
                {
                    c.missing_since.erase(kv.first);
                }
            }
        }
        if (!pending) classify();
        break;
    }
    case Stage::WaitDerez:
        waitDerez();
        break;
    case Stage::Idle:
        break;
    }
}

void startPass(const std::string& phase, F64 settle, std::function<void()> done)
{
    Cleanup& c = s_cleanup;
    c.done.push_back(std::move(done));
    c.phase = phase;
    c.derezzed_count = 0;
    c.gone_count = 0;
    c.mixed.clear();
    c.mixed_prims.clear();
    c.rlv_prims.clear();
    c.missing_since.clear();
    c.derezzed.clear();
    c.settle_until = LLTimer::getTotalSeconds() + settle;
    c.stage = settle > 0.0 ? Stage::Settle : Stage::Classify;
    c.listener = LLEventPumps::instance().obtain("mainloop").listen("IDHeadlessCleanup", [](const LLSD&)
    {
        cleanupTick();
        return false;
    });
}

void runCleanup(const std::string& phase, F64 settle, std::function<void()> done)
{
    if (s_cleanup.stage != Stage::Idle)
    {
        s_queued.push_back({ phase, settle, std::move(done) });
        return;
    }
    startPass(phase, settle, std::move(done));
}
}

void ledgerAdd(LLViewerObject* root, const std::string& tool)
{
    if (!gIDHeadless || !root) return;
    const F64 now = LLTimer::getTotalSeconds();
    for (LLViewerObject* prim : idmcp_obj::linksOf(root))
    {
        if (!s_ledger.count(prim->getID())) addPrim(prim, tool, now);
    }
}

void ledgerKeep(LLViewerObject* root)
{
    if (!gIDHeadless || !root) return;
    for (LLViewerObject* prim : idmcp_obj::linksOf(root))
    {
        s_keep.insert(prim->getID());
    }
}

void ledgerRemove(const LLUUID& root_id)
{
    if (!gIDHeadless) return;
    if (s_ledger.erase(root_id) == 0) return;
    boost::json::object rec;
    rec["op"] = "remove";
    rec["id"] = root_id.asString();
    appendLine(rec);
}

namespace
{
std::string s_run_name;
LLUUID s_run_folder;
LLUUID s_rlv_run_folder;
LLBoundListener s_folder_listener;

std::string resolveRunName()
{
    std::string name = gSavedSettings.getString("IDHeadlessRunName");
    if (name.empty())
    {
        const time_t now = time(nullptr);
        struct tm tmv;
#if LL_WINDOWS
        gmtime_s(&tmv, &now);
#else
        gmtime_r(&now, &tmv);
#endif
        char buf[32];
        strftime(buf, sizeof(buf), "%Y-%m-%dT%H-%M-%S", &tmv);
        name = buf;
    }
    LLInventoryObject::correctInventoryName(name);
    return name;
}

std::string idString(const LLUUID& id)
{
    return id.isNull() ? std::string() : id.asString();
}

LLUUID findChild(const LLUUID& parent, const std::string& name)
{
    LLInventoryModel::cat_array_t* cats = nullptr;
    LLInventoryModel::item_array_t* items = nullptr;
    gInventory.getDirectDescendentsOf(parent, cats, items);
    if (cats)
    {
        for (const auto& cat : *cats)
        {
            if (cat && cat->getName() == name) return cat->getUUID();
        }
    }
    return LLUUID::null;
}

void ensureChild(const LLUUID& parent, const std::string& name, std::function<void(const LLUUID&)> next)
{
    const LLUUID existing = findChild(parent, name);
    if (existing.notNull())
    {
        next(existing);
        return;
    }
    gInventory.createNewCategory(parent, LLFolderType::FT_NONE, name,
        [next = std::move(next)](const LLUUID& id)
        {
            if (id.isNull())
            {
                idheadless::fail(idheadless::EXIT_STARTUP, "could not create run folder");
            }
            next(id);
        });
}

void ensureRunFolder(const LLUUID& root, std::function<void(const LLUUID&)> done)
{
    ensureChild(root, "Headless Tests", [done = std::move(done)](const LLUUID& tests)
    {
        ensureChild(tests, s_run_name, done);
    });
}

void setupFolders(std::function<void()> done)
{
    s_run_name = resolveRunName();
    idheadless::setStatusField("run", s_run_name);
    s_folder_listener.disconnect();
    s_folder_listener = LLEventPumps::instance().obtain("mainloop").listen("IDHeadlessRunFolders",
        [done = std::move(done)](const LLSD&)
        {
            if (!gInventory.isInventoryUsable() || gInventory.getRootFolderID().isNull())
            {
                return false;
            }
            const LLUUID rlv_root = RlvInventory::instance().getSharedRootID();
            auto finish_setup = [done]()
            {
                idheadless::setStatusField("run_folder_id", idString(s_run_folder));
                idheadless::setStatusField("rlv_run_folder_id", idString(s_rlv_run_folder));
                done();
            };
            s_folder_listener.disconnect();
            ensureRunFolder(gInventory.getRootFolderID(), [rlv_root, finish_setup](const LLUUID& id)
            {
                s_run_folder = id;
                if (rlv_root.isNull())
                {
                    finish_setup();
                    return;
                }
                ensureRunFolder(rlv_root, [finish_setup](const LLUUID& rlv_id)
                {
                    s_rlv_run_folder = rlv_id;
                    finish_setup();
                });
            });
            return false;
        });
}

bool inside(const LLUUID& folder, const LLUUID& root)
{
    return root.notNull() && gInventory.isObjectDescendentOf(folder, root);
}
}

LLUUID runFolder()
{
    return gIDHeadless ? s_run_folder : LLUUID::null;
}

LLUUID rlvRunFolder()
{
    return gIDHeadless ? s_rlv_run_folder : LLUUID::null;
}

bool applyDest(bool given, LLUUID& folder, std::string& why)
{
    if (!gIDHeadless) return true;
    if (!given)
    {
        if (s_run_folder.notNull())
        {
            folder = s_run_folder;
            return true;
        }
    }
    else if (inside(folder, s_run_folder) || inside(folder, s_rlv_run_folder))
    {
        return true;
    }
    why = "destination must be inside Headless Tests/" + s_run_name + "/ (" + s_run_folder.asString()
        + ") or #RLV/Headless Tests/" + s_run_name + "/";
    return false;
}

void begin(std::function<void()> on_ready)
{
    loadLedger();
    startWatch();
    setupFolders([on_ready = std::move(on_ready)]() mutable
    {
        runCleanup("start", s_ledger.empty() ? 0.0 : SETTLE_SECS, std::move(on_ready));
    });
}

void finish(std::function<void()> done)
{
    runCleanup("quit", 0.0, std::move(done));
}

boost::json::object sessionInfo()
{
    boost::json::object info;
    info["run"] = s_run_name;
    info["run_folder_id"] = idString(s_run_folder);
    info["rlv_run_folder_id"] = idString(s_rlv_run_folder);
    info["port"] = (int64_t)gSavedSettings.getU32("IDMCPServerPort");
    info["avatar"] = idheadless::statusString("avatar");
    boost::json::array ledger;
    for (const auto& kv : s_ledger)
    {
        boost::json::object o;
        o["id"]          = kv.second.id.asString();
        o["region_name"] = kv.second.region_name;
        o["tool"]        = kv.second.tool;
        ledger.push_back(std::move(o));
    }
    info["ledger"] = std::move(ledger);
    return info;
}

}

void idmcp_register_headless_tools(IDMCPToolRegistry& reg)
{
    if (!gIDHeadless)
    {
        return;
    }

    {
        IDMCPTool quit;
        quit.name = "viewer.quit";
        quit.description = "Headless only. Clean up this session's rezzed objects, log out and exit.";
        quit.input_schema = boost::json::parse(
            R"({"type":"object","properties":{},"additionalProperties":false})");
        quit.invoke = [](const boost::json::object&, const IDMCPCallPtr& call)
        {
            boost::json::object result;
            result["quitting"] = true;
            idmcp_tool_ok(call, result);
            idheadless::requestCleanQuit("viewer.quit");
        };
        reg.add(std::move(quit));
    }

    {
        IDMCPTool session;
        session.name = "headless.getSession";
        session.description = "Headless only. Run name, run folder ids and this session's rezzed-object ledger.";
        session.input_schema = boost::json::parse(
            R"({"type":"object","properties":{},"additionalProperties":false})");
        session.invoke = [](const boost::json::object&, const IDMCPCallPtr& call)
        {
            idmcp_tool_ok(call, idmcp_headless::sessionInfo());
        };
        reg.add(std::move(session));
    }
}
