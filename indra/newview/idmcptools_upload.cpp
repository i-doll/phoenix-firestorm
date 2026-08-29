/**
 * @file idmcptools_upload.cpp
 * @brief <ID> MCP server: asset upload tools (image / sound / animation / material).
 *
 * Part of Five's custom Firestorm fork. Custom code carries an `ID` prefix.
 *
 * Uploads cost real L$. Every tool is two-phase by design: a call WITHOUT
 * {"confirm":true} performs a dry run — it validates each file and returns a
 * per-file + total L$ cost estimate and the current balance, spending nothing.
 * Passing {"confirm":true} actually enqueues the uploads. There is no RLV gate
 * (uploading is not an RLV-restricted action); the confirm requirement is the
 * guard against an agent spending L$ unattended.
 *
 * Image / sound / animation(.anim) all ride one LLNewFileResourceUploadInfo
 * subclass whose exportTempFile already encodes+validates each type. A .bvh is
 * first converted to a temp .anim file (idmcp_upload_detail::bvh_to_anim_file)
 * and then routed through the same path. Materials use the dedicated GLTF flow
 * (idmcp_upload_detail::material_upload). Cost estimation mirrors
 * get_bulk_upload_expected_cost.
 */

#include "llviewerprecompiledheaders.h"

#include "idmcptools.h"
#include "idmcpserver.h"

#include "llagent.h"
#include "llviewerregion.h"
#include "llviewerassetupload.h"    // LLNewFileResourceUploadInfo, EnqueueInventoryUpload
#include "llagentbenefits.h"        // LLAgentBenefitsMgr upload costs
#include "llfloaterperms.h"         // LLFloaterPerms::get*Perms("Uploads")
#include "llinventorymodel.h"       // gInventory
#include "llappearancemgr.h"        // LLAppearanceMgr (well-known folders)
#include "llstatusbar.h"            // gStatusBar->getBalance()
#include "llviewercontrol.h"        // gSavedSettings
#include "lldiriterator.h"          // LLDirIterator (dir enumeration)
#include "lldir.h"                  // gDirUtilp

#include "llimage.h"                // LLImageFormatted, LLImageRaw
#include "llviewertexture.h"        // LLViewerFetchedTexture::MAX_IMAGE_SIZE_DEFAULT

#include "llassettype.h"
#include "llfoldertype.h"
#include "llinventorytype.h"
#include "llpermissionsflags.h"
#include "lluuid.h"                 // LLUUID, LLTransactionID
#include "llapr.h"                  // LLAPRFile

// Animation (.bvh -> .anim) conversion
#include "llbvhloader.h"            // LLBVHLoader, ELoadStatus
#include "llbvhconsts.h"           // MAX_ANIM_DURATION
#include "lldatapacker.h"           // LLDataPackerBinaryBuffer
#include "llkeyframemotion.h"       // LLKeyframeMotion, LLKeyframeDataCache
#include "llhandmotion.h"           // LLHandMotion::eHandPose
#include "llvoavatarself.h"         // gAgentAvatarp, isAgentAvatarValid

// GLTF material upload
#include "llmaterialeditor.h"       // LLMaterialEditor::uploadMaterialFromModel
#include "lltinygltfhelper.h"       // LLTinyGLTFHelper, tinygltf::Model
#include "llfetchedgltfmaterial.h"  // LLFetchedGLTFMaterial
#include "llgltfmaterial.h"         // LLGLTFMaterial::GLTF_TEXTURE_INFO_*

#include <algorithm>
#include <cctype>
#include <string>
#include <vector>

// Implemented in dedicated helper units below (filled from the type-specific
// upload paths). Declared here so the tool bodies can reference them.
namespace idmcp_upload_detail
{
    // Convert a .bvh file on disk to a temporary .anim file. Returns the temp
    // path (which the caller uploads via the .anim path) or empty on failure,
    // setting error_out. Optional tuning params default to a plain, non-looping
    // priority-0 animation.
    struct AnimParams
    {
        bool loop      = false;
        S32  priority  = 2;   // 0..4 (SL animation priority); 2 = viewer default
        F32  ease_in   = 0.3f;
        F32  ease_out  = 0.3f;
        S32  hand_pose = 1;   // LLHandMotion::eHandPose; 1 = relaxed (viewer default)
    };
    std::string bvh_to_anim_file(const std::string& bvh_path, const AnimParams& p, std::string& error_out);

    // GLTF material cost estimate (dry run): parse the file, sum its materials'
    // texture upload costs. Fills cost_out + material_count; false + error_out on
    // a parse failure or a file with no materials.
    bool material_estimate(const std::string& path, S32& cost_out, S32& material_count, std::string& error_out);

    // GLTF material upload (confirmed): enqueue each material in the file (uploads
    // its textures + the material asset into dest). Fire-and-forget via the
    // proven bulk path; returns the material count enqueued, or -1 + error_out.
    int material_upload(const std::string& path, const LLUUID& dest, std::string& error_out);
}

namespace
{
    using idmcp_upload_detail::AnimParams;

    // ---- small arg helpers -------------------------------------------------

    std::string lower(std::string s)
    {
        std::transform(s.begin(), s.end(), s.begin(),
                       [](unsigned char c){ return (char)std::tolower(c); });
        return s;
    }

    bool looks_like_uuid(const std::string& s)
    {
        return s.size() == 36 && s[8] == '-' && s[13] == '-' && s[18] == '-' && s[23] == '-';
    }

    std::string arg_str(const boost::json::object& o, const char* key)
    {
        auto it = o.find(key);
        return (it != o.end() && it->value().is_string())
                   ? std::string(it->value().as_string().c_str()) : std::string();
    }

    bool arg_bool(const boost::json::object& o, const char* key, bool dflt = false)
    {
        auto it = o.find(key);
        if (it == o.end()) return dflt;
        if (it->value().is_bool()) return it->value().as_bool();
        return dflt;
    }

    bool arg_has(const boost::json::object& o, const char* key)
    {
        return o.find(key) != o.end();
    }

    S32 arg_int(const boost::json::object& o, const char* key, S32 dflt)
    {
        auto it = o.find(key);
        if (it == o.end()) return dflt;
        if (it->value().is_int64())  return (S32)it->value().as_int64();
        if (it->value().is_double()) return (S32)it->value().as_double();
        return dflt;
    }

    F32 arg_flt(const boost::json::object& o, const char* key, F32 dflt)
    {
        auto it = o.find(key);
        if (it == o.end()) return dflt;
        if (it->value().is_double()) return (F32)it->value().as_double();
        if (it->value().is_int64())  return (F32)it->value().as_int64();
        return dflt;
    }

    std::vector<std::string> arg_str_array(const boost::json::object& o, const char* key)
    {
        std::vector<std::string> out;
        auto it = o.find(key);
        if (it != o.end() && it->value().is_array())
        {
            for (const auto& v : it->value().as_array())
            {
                if (v.is_string()) out.emplace_back(v.as_string().c_str());
            }
        }
        return out;
    }

    // Parse a UUID-or-well-known folder name; null if unresolvable. Empty spec
    // returns null too (caller treats null as "the asset type's system folder").
    LLUUID resolve_folder(const std::string& s)
    {
        if (s.empty()) return LLUUID::null;
        if (looks_like_uuid(s)) return LLUUID(s);
        const std::string k = lower(s);
        if (k == "root" || k == "my_inventory")  return gInventory.getRootFolderID();
        if (k == "cof"  || k == "current_outfit") return LLAppearanceMgr::instance().getCOF();
        if (k == "outfits" || k == "my_outfits")  return gInventory.findCategoryUUIDForType(LLFolderType::FT_MY_OUTFITS);
        if (k == "trash")                         return gInventory.findCategoryUUIDForType(LLFolderType::FT_TRASH);
        return LLUUID::null;
    }

    // ---- which files a given tool accepts ----------------------------------

    enum class UploadKind { Image, Sound, Animation, Material };

    bool ext_matches_kind(const std::string& ext, UploadKind kind)
    {
        const std::string e = lower(ext);
        switch (kind)
        {
        case UploadKind::Image:
        {
            LLAssetType::EType at; U32 codec;
            return LLResourceUploadInfo::findAssetTypeAndCodecOfExtension(e, at, codec) && at == LLAssetType::AT_TEXTURE;
        }
        case UploadKind::Sound:
        {
            LLAssetType::EType at; U32 codec;
            return LLResourceUploadInfo::findAssetTypeAndCodecOfExtension(e, at, codec) && at == LLAssetType::AT_SOUND;
        }
        case UploadKind::Animation: return e == "anim" || e == "bvh";
        case UploadKind::Material:  return e == "gltf" || e == "glb";
        }
        return false;
    }

    // ---- cost estimation ---------------------------------------------------

    // Texture cost mirrors get_bulk_upload_expected_cost's biased-dimension math.
    S32 estimate_texture_cost(const std::string& filename, std::string& err)
    {
        const std::string ext = gDirUtilp->getExtension(filename);
        U32 codec = LLImageBase::getCodecFromExtension(ext);
        LLPointer<LLImageFormatted> img = LLImageFormatted::createFromType(codec);
        if (img.isNull() || !gDirUtilp->fileExists(filename) || !img->load(filename))
        {
            err = "could not read/decode image";
            return -1;
        }
        if (img->getComponents() != 3 && img->getComponents() != 4)
        {
            err = "texture must be RGB or RGBA (3 or 4 components)";
            return -1;
        }
        S32 max_w = gSavedSettings.getS32("max_texture_dimension_X");
        S32 max_h = gSavedSettings.getS32("max_texture_dimension_Y");
        S32 ow = img->getWidth(), oh = img->getHeight();
        S32 bw, bh;
        if (ow > max_w || oh > max_h)
        {
            F32 scale = llmin((F32)max_w / (F32)ow, (F32)max_h / (F32)oh);
            bw = LLImageRaw::contractDimToPowerOfTwo(llclamp((S32)llroundf(ow * scale), 4, max_w));
            bh = LLImageRaw::contractDimToPowerOfTwo(llclamp((S32)llroundf(oh * scale), 4, max_h));
        }
        else
        {
            bw = LLImageRaw::biasedDimToPowerOfTwo(ow, LLViewerFetchedTexture::MAX_IMAGE_SIZE_DEFAULT);
            bh = LLImageRaw::biasedDimToPowerOfTwo(oh, LLViewerFetchedTexture::MAX_IMAGE_SIZE_DEFAULT);
        }
        return LLAgentBenefitsMgr::current().getTextureUploadCost(bw, bh);
    }

    // Estimate the L$ cost of one file for a given tool; -1 + err on invalid.
    S32 estimate_cost(const std::string& filename, UploadKind kind, std::string& err)
    {
        if (!gDirUtilp->fileExists(filename)) { err = "file not found"; return -1; }
        const std::string ext = gDirUtilp->getExtension(filename);
        if (!ext_matches_kind(ext, kind)) { err = "unsupported extension for this tool: ." + ext; return -1; }

        switch (kind)
        {
        case UploadKind::Image:
            return estimate_texture_cost(filename, err);
        case UploadKind::Sound:
        {
            LLAssetType::EType at = LLAssetType::AT_SOUND; S32 cost = 0;
            LLAgentBenefitsMgr::current().findUploadCost(at, cost);
            return cost;
        }
        case UploadKind::Animation:
            return LLAgentBenefitsMgr::current().getAnimationUploadCost();
        case UploadKind::Material:
        {
            S32 cost = 0, ncount = 0;
            if (!idmcp_upload_detail::material_estimate(filename, cost, ncount, err)) return -1;
            return cost;
        }
        }
        err = "internal: unknown kind";
        return -1;
    }

    // ---- file enumeration --------------------------------------------------

    // Collect the target files: explicit {"paths"} plus everything in {"dir"}
    // whose extension matches this tool's kind.
    std::vector<std::string> collect_files(const boost::json::object& args, UploadKind kind)
    {
        std::vector<std::string> files = arg_str_array(args, "paths");
        // single-path convenience
        std::string one = arg_str(args, "path");
        if (!one.empty()) files.push_back(one);

        std::string dir = arg_str(args, "dir");
        if (!dir.empty())
        {
            LLDirIterator it(dir, "*");
            std::string fname;
            while (it.next(fname))
            {
                const std::string full = gDirUtilp->add(dir, fname);
                if (ext_matches_kind(gDirUtilp->getExtension(full), kind))
                {
                    files.push_back(full);
                }
            }
            std::sort(files.begin(), files.end());
        }
        return files;
    }

    // Inventory name for a file: {"names"} override by index, else the stem.
    std::string name_for(const std::vector<std::string>& names, size_t idx, const std::string& filename)
    {
        if (idx < names.size() && !names[idx].empty()) return names[idx];
        std::string n = gDirUtilp->getBaseFileName(filename, /*strip_exten*/ true);
        LLStringUtil::replaceNonstandardASCII(n, '?');
        LLStringUtil::replaceChar(n, '|', '?');
        LLStringUtil::stripNonprintable(n);
        LLStringUtil::trim(n);
        if (n.empty()) n = "Uploaded asset";
        return n;
    }

    // ---- the batch collector -----------------------------------------------
    //
    // One MCP call fans out to N async uploads; we respond once when all N have
    // reported. Pre-validation failures are recorded immediately and excluded
    // from the async count.
    struct UploadBatch
    {
        IDMCPCallPtr       call;
        size_t             expected = 0;   // async uploads still outstanding
        size_t             done     = 0;
        int                uploaded = 0;
        int                failed   = 0;
        S32                spent    = 0;
        boost::json::array results;

        void addResult(boost::json::object e, bool ok, S32 cost)
        {
            if (ok) { uploaded++; spent += cost; } else { failed++; }
            results.push_back(std::move(e));
        }

        void reportAsync(boost::json::object e, bool ok, S32 cost)
        {
            addResult(std::move(e), ok, cost);
            if (++done >= expected) finish();
        }

        void finish()
        {
            boost::json::object o;
            o["uploaded"]   = uploaded;
            o["failed"]     = failed;
            o["total_cost"] = spent;
            o["results"]    = std::move(results);
            idmcp_tool_ok(call, o);
        }
    };
    using UploadBatchPtr = std::shared_ptr<UploadBatch>;

    // LLNewFileResourceUploadInfo that reports its result into an UploadBatch.
    // exportTempFile (base) does all the encoding/validation for image/sound/anim.
    class IDMCPFileUpload : public LLNewFileResourceUploadInfo
    {
    public:
        IDMCPFileUpload(std::string filename, std::string name, S32 expectedCost,
                        const LLUUID& dest, UploadBatchPtr batch, std::string reportPath)
            : LLNewFileResourceUploadInfo(filename, name, name, 0,
                                          LLFolderType::FT_NONE, LLInventoryType::IT_NONE,
                                          LLFloaterPerms::getNextOwnerPerms("Uploads"),
                                          LLFloaterPerms::getGroupPerms("Uploads"),
                                          LLFloaterPerms::getEveryonePerms("Uploads"),
                                          expectedCost, dest, /*show_inventory*/ false),
              mBatch(std::move(batch)), mReportPath(std::move(reportPath)), mEstCost(expectedCost) {}

        LLUUID finishUpload(LLSD& result) override
        {
            LLUUID item = LLNewFileResourceUploadInfo::finishUpload(result);
            boost::json::object e;
            e["path"]    = mReportPath;
            e["ok"]      = true;
            e["item_id"] = item.asString();
            if (result.has("new_asset"))
            {
                e["asset_id"] = result["new_asset"].asUUID().asString();
            }
            S32 cost = result.has("upload_price") ? (S32)result["upload_price"].asInteger() : mEstCost;
            e["cost"] = cost;
            if (mBatch) mBatch->reportAsync(std::move(e), true, cost);
            return item;
        }

        bool failedUpload(LLSD& result, std::string& reason) override
        {
            boost::json::object e;
            e["path"]  = mReportPath;
            e["ok"]    = false;
            e["error"] = reason;
            if (mBatch) mBatch->reportAsync(std::move(e), false, 0);
            return true;   // handled: suppress the default UI notification
        }

    private:
        UploadBatchPtr mBatch;
        std::string    mReportPath;
        S32            mEstCost;
    };

    std::string new_file_agent_cap()
    {
        LLViewerRegion* region = gAgent.getRegion();
        return region ? region->getCapability("NewFileAgentInventory") : std::string();
    }

    // ---- the shared tool body ----------------------------------------------

    void run_upload_tool(const boost::json::object& args, const IDMCPCallPtr& call, UploadKind kind)
    {
        std::vector<std::string> files = collect_files(args, kind);
        if (files.empty())
        {
            idmcp_tool_err(call, IDMCP_ERR_INVALID_PARAMS,
                           "no files: supply {\"paths\":[...]} and/or {\"dir\":\"...\"}");
            return;
        }

        const std::vector<std::string> names = arg_str_array(args, "names");
        const bool   confirm = arg_bool(args, "confirm", false);
        const LLUUID dest    = resolve_folder(arg_str(args, "dest"));
        if (arg_has(args, "dest") && !arg_str(args, "dest").empty() && dest.isNull())
        {
            idmcp_tool_err(call, IDMCP_ERR_NOT_FOUND, "unknown dest folder: " + arg_str(args, "dest"));
            return;
        }

        // ---- dry run: validate + total the cost, spend nothing -------------
        if (!confirm)
        {
            boost::json::array manifest;
            S32 total = 0;
            int ok_count = 0;
            for (size_t i = 0; i < files.size(); ++i)
            {
                boost::json::object f;
                f["path"] = files[i];
                f["name"] = name_for(names, i, files[i]);
                std::string err;
                S32 cost = estimate_cost(files[i], kind, err);
                if (cost < 0)
                {
                    f["ok"]    = false;
                    f["error"] = err;
                }
                else
                {
                    f["ok"]   = true;
                    f["cost"] = cost;
                    total += cost;
                    ok_count++;
                }
                manifest.push_back(std::move(f));
            }
            const S32 balance = gStatusBar ? gStatusBar->getBalance() : -1;
            boost::json::object o;
            o["dry_run"]        = true;
            o["confirm_hint"]   = "re-call with \"confirm\":true to upload";
            o["file_count"]     = (int)files.size();
            o["uploadable"]     = ok_count;
            o["total_cost"]     = total;
            o["balance"]        = balance;
            o["affordable"]     = (balance < 0) || (balance >= total);
            o["files"]          = std::move(manifest);
            idmcp_tool_ok(call, o);
            return;
        }

        // ---- confirmed: enqueue uploads ------------------------------------
        const std::string cap = new_file_agent_cap();
        if (kind != UploadKind::Material && cap.empty())
        {
            idmcp_tool_err(call, IDMCP_ERR_CAP_UNAVAIL, "NewFileAgentInventory capability unavailable");
            return;
        }

        AnimParams ap;
        ap.loop      = arg_bool(args, "loop", false);
        ap.priority  = arg_int(args, "priority", 2);
        ap.ease_in   = arg_flt(args, "ease_in", 0.3f);
        ap.ease_out  = arg_flt(args, "ease_out", 0.3f);
        ap.hand_pose = arg_int(args, "hand_pose", 1);

        auto batch = std::make_shared<UploadBatch>();
        batch->call = call;

        for (size_t i = 0; i < files.size(); ++i)
        {
            const std::string& src = files[i];
            const std::string  nm  = name_for(names, i, src);
            std::string err;
            S32 cost = estimate_cost(src, kind, err);
            if (cost < 0)
            {
                boost::json::object e;
                e["path"] = src; e["ok"] = false; e["error"] = err;
                batch->addResult(std::move(e), false, 0);
                continue;
            }

            if (kind == UploadKind::Material)
            {
                // Materials upload via the proven bulk path (textures + material
                // asset) and don't surface a per-item completion callback, so we
                // record the enqueue synchronously and note it's asynchronous.
                std::string uerr;
                int n = idmcp_upload_detail::material_upload(src, dest, uerr);
                boost::json::object e;
                e["path"] = src;
                if (n < 0)
                {
                    e["ok"] = false; e["error"] = uerr;
                    batch->addResult(std::move(e), false, 0);
                }
                else
                {
                    e["ok"]        = true;
                    e["materials"] = n;
                    e["cost"]      = cost;   // estimate; actual price settles on the server
                    e["note"]      = "uploading asynchronously into dest; verify with inventory.getFolder";
                    batch->addResult(std::move(e), true, cost);
                }
                continue;
            }

            std::string upload_path = src;
            const std::string ext = lower(gDirUtilp->getExtension(src));
            if (kind == UploadKind::Animation && ext == "bvh")
            {
                std::string cverr;
                std::string tmp = idmcp_upload_detail::bvh_to_anim_file(src, ap, cverr);
                if (tmp.empty())
                {
                    boost::json::object e;
                    e["path"] = src; e["ok"] = false; e["error"] = "bvh convert failed: " + cverr;
                    batch->addResult(std::move(e), false, 0);
                    continue;
                }
                // The temp .anim is read by the upload coro's prepareUpload; it's
                // left for the OS temp sweep (small, in the temp dir).
                upload_path = tmp;
            }

            batch->expected++;
            LLResourceUploadInfo::ptr_t info =
                std::make_shared<IDMCPFileUpload>(upload_path, nm, cost, dest, batch, src);
            LLViewerAssetUpload::EnqueueInventoryUpload(cap, info);
        }

        // Nothing enqueued (all pre-failed) -> respond now.
        if (batch->expected == 0)
        {
            batch->finish();
        }
    }

    // Shared JSON-Schema fragment for the simple (image/sound/material) tools.
    const char* SCHEMA_SIMPLE =
        R"({"type":"object","properties":{)"
        R"("paths":{"type":"array","items":{"type":"string"}},)"
        R"("path":{"type":"string"},)"
        R"("dir":{"type":"string"},)"
        R"("dest":{"type":"string"},)"
        R"("names":{"type":"array","items":{"type":"string"}},)"
        R"("confirm":{"type":"boolean"}},"additionalProperties":false})";

    const char* SCHEMA_ANIM =
        R"({"type":"object","properties":{)"
        R"("paths":{"type":"array","items":{"type":"string"}},)"
        R"("path":{"type":"string"},)"
        R"("dir":{"type":"string"},)"
        R"("dest":{"type":"string"},)"
        R"("names":{"type":"array","items":{"type":"string"}},)"
        R"("loop":{"type":"boolean"},)"
        R"("priority":{"type":"integer"},)"
        R"("ease_in":{"type":"number"},)"
        R"("ease_out":{"type":"number"},)"
        R"("hand_pose":{"type":"integer"},)"
        R"("confirm":{"type":"boolean"}},"additionalProperties":false})";
}

// ---------------------------------------------------------------------------

void idmcp_register_upload_tools(IDMCPToolRegistry& reg)
{
    // upload.image -----------------------------------------------------------
    {
        IDMCPTool t;
        t.name = "upload.image";
        t.description =
            "Upload image files (png/jpg/jpeg/tga/bmp) as textures. Give "
            "{\"paths\":[...]} and/or {\"dir\":\"...\"} (globs that folder), "
            "optional {\"dest\"} (folder UUID/name; default = Textures), "
            "{\"names\":[...]} to override per-file names. WITHOUT "
            "{\"confirm\":true} this is a DRY RUN: returns a per-file + total L$ "
            "cost estimate and your balance, spending nothing. With "
            "{\"confirm\":true} it uploads (costs L$). Large images are downscaled "
            "to the texture size limit.";
        t.input_schema = boost::json::parse(SCHEMA_SIMPLE);
        t.invoke = [](const boost::json::object& args, const IDMCPCallPtr& call)
        { run_upload_tool(args, call, UploadKind::Image); };
        reg.add(std::move(t));
    }

    // upload.sound -----------------------------------------------------------
    {
        IDMCPTool t;
        t.name = "upload.sound";
        t.description =
            "Upload sound files. WAV must be 44.1 kHz, mono, 16-bit PCM, <=10s "
            "(encoded to Ogg Vorbis on upload). Same args as upload.image; default "
            "{\"dest\"} = Sounds. WITHOUT {\"confirm\":true} = dry-run cost "
            "estimate; with {\"confirm\":true} = upload (costs L$).";
        t.input_schema = boost::json::parse(SCHEMA_SIMPLE);
        t.invoke = [](const boost::json::object& args, const IDMCPCallPtr& call)
        { run_upload_tool(args, call, UploadKind::Sound); };
        reg.add(std::move(t));
    }

    // upload.animation -------------------------------------------------------
    {
        IDMCPTool t;
        t.name = "upload.animation";
        t.description =
            "Upload animations (.bvh, converted client-side, or pre-baked .anim). "
            "Same file args as upload.image; default {\"dest\"} = Animations. For "
            ".bvh, optional tuning: {\"loop\"} (bool), {\"priority\"} (0-4), "
            "{\"ease_in\"}/{\"ease_out\"} (seconds), {\"hand_pose\"} (int); "
            "ignored for .anim (already baked). WITHOUT {\"confirm\":true} = "
            "dry-run cost estimate; with {\"confirm\":true} = upload (costs L$).";
        t.input_schema = boost::json::parse(SCHEMA_ANIM);
        t.invoke = [](const boost::json::object& args, const IDMCPCallPtr& call)
        { run_upload_tool(args, call, UploadKind::Animation); };
        reg.add(std::move(t));
    }

    // upload.material --------------------------------------------------------
    {
        IDMCPTool t;
        t.name = "upload.material";
        t.description =
            "Upload GLTF materials (.gltf/.glb). A file may contain several "
            "materials; each becomes an inventory item, and its referenced "
            "textures are uploaded too (cost = sum of those textures). Same file "
            "args as upload.image; default {\"dest\"} = Materials. WITHOUT "
            "{\"confirm\":true} = dry-run cost estimate; with {\"confirm\":true} = "
            "upload (costs L$).";
        t.input_schema = boost::json::parse(SCHEMA_SIMPLE);
        t.invoke = [](const boost::json::object& args, const IDMCPCallPtr& call)
        { run_upload_tool(args, call, UploadKind::Material); };
        reg.add(std::move(t));
    }
}

// ===========================================================================
// idmcp_upload_detail — type-specific helpers. Kept in this TU (no new file =
// no extra full rebuild; see the file header) despite the heavy includes.
// ===========================================================================

namespace idmcp_upload_detail
{
    // BVH text -> serialized AT_ANIMATION asset bytes. gAgentAvatarp supplies the
    // joint-alias map and serves as the keyframe character so the tuning knobs
    // (loop/priority/ease/hand) apply. Mirrors LLFloaterBvhPreview's load+bake.
    static std::string bvh_to_anim_buffer(const std::string& bvh_text, const AnimParams& p, std::string& error_out)
    {
        error_out.clear();
        if (!isAgentAvatarValid())
        {
            error_out = "avatar not ready";
            return std::string();
        }
        LLVOAvatar* character  = gAgentAvatarp;
        auto        alias_map  = character->getJointAliases();

        ELoadStatus status = E_ST_OK;
        S32         line   = 0;
        LLBVHLoader loader(bvh_text.c_str(), status, line, alias_map);
        if (!loader.isInitialized() || status != E_ST_OK)
        {
            error_out = llformat("BVH parse failed at line %d (status %d)", line, (int)loader.getStatus());
            return std::string();
        }
        if (loader.getDuration() > MAX_ANIM_DURATION)
        {
            error_out = llformat("animation too long: %.1fs (max %.0fs)", loader.getDuration(), (F32)MAX_ANIM_DURATION);
            return std::string();
        }

        LLTransactionID tid;
        tid.generate();
        LLUUID motion_id = tid.makeAssetID(gAgent.getSecureSessionID());

        LLKeyframeMotion* motionp = dynamic_cast<LLKeyframeMotion*>(character->createMotion(motion_id));
        if (!motionp)
        {
            error_out = "could not create motion";
            return std::string();
        }

        {   // feed the loader bytes into the keyframe motion
            U32                      in_size = loader.getOutputSize();
            std::vector<U8>          in_buf(in_size);
            LLDataPackerBinaryBuffer dp(in_buf.data(), (S32)in_size);
            loader.serialize(dp);
            dp.reset();
            if (!motionp->deserialize(dp, motion_id, /*allow_invalid_joints*/ false))
            {
                character->removeMotion(motion_id);
                error_out = "unsupported joints in BVH";
                return std::string();
            }
        }

        // Apply the tuning knobs (mirrors llfloaterbvhpreview onBtnOK).
        F32 dur = motionp->getDuration();
        motionp->setLoop(p.loop);
        motionp->setLoopIn(0.f);
        motionp->setLoopOut(dur);
        motionp->setPriority(p.priority);
        motionp->setHandPose((LLHandMotion::eHandPose)p.hand_pose);
        F32 ei = p.ease_in, eo = p.ease_out;
        if (dur != 0.f && !p.loop && (ei + eo) > dur)
        {
            F32 factor = dur / (ei + eo);
            ei *= factor;
            eo *= factor;
        }
        motionp->setEaseIn(ei);
        motionp->setEaseOut(eo);

        // Serialize the final asset bytes.
        std::string              result;
        S32                      out_size = motionp->getFileSize();
        std::vector<U8>          out_buf(out_size);
        LLDataPackerBinaryBuffer dp2(out_buf.data(), out_size);
        if (motionp->serialize(dp2))
        {
            result.assign(reinterpret_cast<char*>(out_buf.data()), dp2.getCurrentSize());
        }
        else
        {
            error_out = "animation serialize failed";
        }

        character->removeMotion(motion_id);
        LLKeyframeDataCache::removeKeyframeData(motion_id);
        return result;
    }

    std::string bvh_to_anim_file(const std::string& bvh_path, const AnimParams& p, std::string& error_out)
    {
        error_out.clear();
        if (!gDirUtilp->fileExists(bvh_path))
        {
            error_out = "file not found";
            return std::string();
        }

        // Read the BVH text (LLBVHLoader reads a NUL-terminated buffer).
        LLAPRFile infile;
        infile.open(bvh_path, LL_APR_RB);
        if (!infile.getFileHandle())
        {
            error_out = "could not open file";
            return std::string();
        }
        S32         size = LLAPRFile::size(bvh_path);
        std::string bvh_text;
        if (size > 0)
        {
            std::vector<char> raw(size + 1, 0);
            S32               got = infile.read(raw.data(), size);
            bvh_text.assign(raw.data(), (got > 0 ? got : 0));
        }
        infile.close();
        if (bvh_text.empty())
        {
            error_out = "empty file";
            return std::string();
        }

        std::string buf = bvh_to_anim_buffer(bvh_text, p, error_out);
        if (buf.empty())
        {
            return std::string();   // error_out already set
        }

        // Stage as a temp .anim so it rides the shared file-based .anim path.
        std::string tmp = gDirUtilp->getTempFilename() + ".anim";
        if (LLAPRFile::writeEx(tmp, (void*)buf.data(), 0, (S32)buf.size()) != (S32)buf.size())
        {
            error_out = "could not write temp animation file";
            return std::string();
        }
        return tmp;
    }

    bool material_estimate(const std::string& path, S32& cost_out, S32& material_count, std::string& error_out)
    {
        cost_out       = 0;
        material_count = 0;
        error_out.clear();

        tinygltf::Model model;
        if (!LLTinyGLTFHelper::loadModel(path, model))
        {
            error_out = "could not parse GLTF file";
            return false;
        }
        material_count = (S32)model.materials.size();
        if (material_count == 0)
        {
            error_out = "file contains no materials";
            return false;
        }

        const LLAgentBenefits& ben = LLAgentBenefitsMgr::current();
        for (S32 i = 0; i < material_count; ++i)
        {
            LLPointer<LLFetchedGLTFMaterial> m = new LLFetchedGLTFMaterial();
            std::string                      mname;
            if (!LLTinyGLTFHelper::getMaterialFromModel(path, model, i, m.get(), mname))
            {
                continue;
            }
            if (m->mTextureId[LLGLTFMaterial::GLTF_TEXTURE_INFO_BASE_COLOR].notNull() && m->mBaseColorTexture)
                cost_out += ben.getTextureUploadCost(m->mBaseColorTexture);
            if (m->mTextureId[LLGLTFMaterial::GLTF_TEXTURE_INFO_METALLIC_ROUGHNESS].notNull() && m->mMetallicRoughnessTexture)
                cost_out += ben.getTextureUploadCost(m->mMetallicRoughnessTexture);
            if (m->mTextureId[LLGLTFMaterial::GLTF_TEXTURE_INFO_NORMAL].notNull() && m->mNormalTexture)
                cost_out += ben.getTextureUploadCost(m->mNormalTexture);
            if (m->mTextureId[LLGLTFMaterial::GLTF_TEXTURE_INFO_EMISSIVE].notNull() && m->mEmissiveTexture)
                cost_out += ben.getTextureUploadCost(m->mEmissiveTexture);
        }
        return true;
    }

    int material_upload(const std::string& path, const LLUUID& dest, std::string& error_out)
    {
        error_out.clear();
        if (!LLMaterialEditor::capabilitiesAvailable())
        {
            error_out = "material upload capability unavailable";
            return -1;
        }
        tinygltf::Model model;
        if (!LLTinyGLTFHelper::loadModel(path, model))
        {
            error_out = "could not parse GLTF file";
            return -1;
        }
        S32 n = (S32)model.materials.size();
        if (n == 0)
        {
            error_out = "file contains no materials";
            return -1;
        }
        for (S32 i = 0; i < n; ++i)
        {
            LLMaterialEditor::uploadMaterialFromModel(path, model, i, dest);
        }
        return n;
    }
}
