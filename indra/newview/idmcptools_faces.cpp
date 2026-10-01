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

#include "llgltfmateriallist.h"     // flushUpdates, queueApply, queueModify
#include "llgltfmaterial.h"
#include "llinventorymodel.h"       // gInventory
#include "lltextureentry.h"
#include "lltooldraganddrop.h"      // handleDropMaterialProtections (public static)
#include "llviewercontrol.h"        // gSavedSettings
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
        // As LLToolDragAndDrop::handleDropMaterialProtections does (:971-973, :1113): a dirty
        // inventory with listeners makes it open a modal notification. Request a refresh
        // (the only thing that clears the flag) and refuse instead. Library items are
        // accepted before that check (:960).
        if (a.source != LLToolDragAndDrop::SOURCE_LIBRARY
            && prim->isInventoryDirty() && prim->hasInventoryListeners())
        {
            prim->requestInventory();
            error = "the object's contents are updating; try again";
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
        std::vector<S64> faces;
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
                faces.push_back((S64)v.as_int64());   // bounded per prim below, never narrowed first
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
            std::vector<S64> todo = faces;
            if (all)
            {
                todo.clear();
                for (S32 f = 0; f < n; ++f) todo.push_back((S64)f);
            }
            bool any_valid = false;
            for (S64 f : todo) any_valid = any_valid || (f >= 0 && f < n);

            // Drop rules once per prim, and only when a face here will use the item.
            LLUUID      tex_asset, mat_asset;
            std::string tex_err, mat_err;
            // The selection hides PBR materials locally (LLSelectMgr::hideGLTFMaterial), so
            // getRenderMaterialID reads null there; decide from the selection, not the id.
            const bool selected_blinn = prim->isSelected() && gSavedSettings.getBOOL("FSShowSelectedInBlinnPhong");
            const char* const selected_msg =
                "deselect the object in the Build floater first (Show selected in Blinn-Phong is on)";
            const bool tex_hidden = tex.given && any_valid && selected_blinn;
            if (tex_hidden) tex_err = selected_msg;
            const bool tex_ok = !tex.given || !any_valid
                                || (!tex_hidden && prepare_asset(prim, tex, tex_asset, tex_err));
            // setRenderMaterialID silently sends a clear for a selected prim while
            // FSShowSelectedInBlinnPhong is on (llviewerobject.cpp:7967-7974).
            const bool hidden_by_selection = mat.given && !mat.clear && selected_blinn;
            if (hidden_by_selection) mat_err = selected_msg;
            const bool mat_ok = !mat.given || !any_valid
                                || (!hidden_by_selection && prepare_asset(prim, mat, mat_asset, mat_err));
            if (mat.given && mat_ok && !mat.clear && mat.item_id.notNull() && mat_asset.isNull())
            {
                mat_asset = BLANK_MATERIAL_ASSET_ID;   // as dropMaterialOneFace
            }

            // PBR texture transform edits, as LLPanelFace::updateGLTFTextureTransform
            // (llpanelface.cpp:5046-5069): the same values on every texture slot of the
            // face's GLTF override. Rotation is radians, as onCommitGLTFRotation (:5284).
            auto edit_pbr = [&](LLGLTFMaterial* m)
            {
                for (U32 i = 0; i < LLGLTFMaterial::GLTF_TEXTURE_INFO_COUNT; ++i)
                {
                    LLGLTFMaterial::TextureTransform& tr = m->mTextureTransform[i];
                    if (has_repeats) tr.mScale.set(repeats[0], repeats[1]);
                    if (has_offset)  tr.mOffset.set(offset[0], offset[1]);
                    if (has_rot)     tr.mRotation = (F32)(rot_deg * DEG_TO_RAD);
                }
            };
            const bool has_mapping = has_repeats || has_offset || has_rot;

            bool touched = false;
            for (S64 f : todo)
            {
                boost::json::object r;
                r["link"] = link;
                r["face"] = (std::int64_t)f;
                if (f < 0 || f >= n)
                {
                    r["ok"]    = false;
                    r["error"] = llformat("no such face (this prim has %d)", n);
                    results.push_back(std::move(r));
                    continue;
                }
                const S32   fi = (S32)f;   // in 0..n-1 here
                const U8    te = (U8)fi;
                std::string error;
                bool        applied = false;
                bool        pbr_done = false;   // this face's mapping already rides in its material apply
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
                            prim->setRenderMaterialID(fi, LLUUID::null);
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
                        // Keep the face's mapping across the material change, as
                        // LLToolDragAndDrop::dropMaterialOneFace (lltooldraganddrop.cpp:1316-1391)
                        // and LLSelectMgr::selectionSetGLTFMaterial (llselectmgr.cpp:2170-2230).
                        LLTextureEntry*  tep = prim->getTE(te);
                        LLGLTFMaterial*  preserved_override   = nullptr;
                        bool             should_preserve      = false;
                        if (tep && mat_asset.notNull())
                        {
                            LLGLTFMaterial* existing_override = tep->getGLTFMaterialOverride();
                            if (existing_override)
                            {
                                const LLGLTFMaterial::TextureTransform& existing_transform = existing_override->mTextureTransform[0];
                                const LLGLTFMaterial::TextureTransform& default_transform  = LLGLTFMaterial::TextureTransform();
                                if (existing_transform.mScale != default_transform.mScale ||
                                    existing_transform.mOffset != default_transform.mOffset ||
                                    existing_transform.mRotation != default_transform.mRotation)
                                {
                                    preserved_override = new LLGLTFMaterial();
                                    for (U32 i = 0; i < LLGLTFMaterial::GLTF_TEXTURE_INFO_COUNT; ++i)
                                    {
                                        preserved_override->mTextureTransform[i].mScale    = existing_transform.mScale;
                                        preserved_override->mTextureTransform[i].mOffset   = existing_transform.mOffset;
                                        preserved_override->mTextureTransform[i].mRotation = existing_transform.mRotation;
                                    }
                                    should_preserve = true;
                                }
                            }
                            else
                            {
                                F32 existing_scale_s, existing_scale_t, existing_offset_s, existing_offset_t, existing_rotation;
                                tep->getScale(&existing_scale_s, &existing_scale_t);
                                tep->getOffset(&existing_offset_s, &existing_offset_t);
                                existing_rotation = tep->getRotation();

                                const LLGLTFMaterial::TextureTransform& default_transform = LLGLTFMaterial::TextureTransform();
                                if (existing_scale_s != default_transform.mScale.mV[0] || existing_scale_t != default_transform.mScale.mV[1] ||
                                    existing_offset_s != default_transform.mOffset.mV[0] || existing_offset_t != default_transform.mOffset.mV[1] ||
                                    existing_rotation != default_transform.mRotation)
                                {
                                    preserved_override = new LLGLTFMaterial();
                                    for (U32 i = 0; i < LLGLTFMaterial::GLTF_TEXTURE_INFO_COUNT; ++i)
                                    {
                                        LLVector2 pbr_scale, pbr_offset;
                                        F32 pbr_rotation;
                                        LLGLTFMaterial::convertTextureTransformToPBR(
                                            existing_scale_s, existing_scale_t,
                                            existing_offset_s, existing_offset_t,
                                            existing_rotation,
                                            pbr_scale, pbr_offset, pbr_rotation);
                                        preserved_override->mTextureTransform[i].mScale    = pbr_scale;
                                        preserved_override->mTextureTransform[i].mOffset   = pbr_offset;
                                        preserved_override->mTextureTransform[i].mRotation = pbr_rotation;
                                    }
                                    should_preserve = true;
                                }
                            }
                            // Explicit repeats/offset/rotation ride in the same apply: the flush
                            // sends queued modifies before applies, so a separate modify would be
                            // wiped by the apply.
                            if (has_mapping)
                            {
                                if (!preserved_override) preserved_override = new LLGLTFMaterial();
                                edit_pbr(preserved_override);
                                should_preserve = true;
                                pbr_done        = true;
                            }
                        }

                        if (should_preserve && preserved_override)
                        {
                            LLGLTFMaterialList::queueApply(prim, fi, mat_asset, preserved_override);
                            prim->setRenderMaterialID(fi, mat_asset, false, true);
                            tep->setGLTFMaterialOverride(preserved_override);
                        }
                        else
                        {
                            prim->setRenderMaterialID(fi, mat_asset);   // queues the region update itself
                        }
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
                if (has_mapping && !pbr_done && selected_blinn)
                {
                    error = error.empty() ? std::string("mapping: ") + selected_msg : error;
                }
                else if (has_mapping && !pbr_done)
                {
                    if (prim->getRenderMaterialID(te).notNull())
                    {
                        // A PBR face: edit its override, as LLPanelFace::updateSelectedGLTFMaterials
                        // (llpanelface.cpp:140-160).
                        LLGLTFMaterial new_override;
                        const LLTextureEntry* tep = prim->getTE(te);
                        if (tep->getGLTFMaterialOverride())
                        {
                            new_override = *tep->getGLTFMaterialOverride();
                        }
                        edit_pbr(&new_override);
                        LLGLTFMaterialList::queueModify(prim, fi, &new_override);
                        any_material = true;
                    }
                    else
                    {
                        if (has_repeats) prim->setTEScale(te, repeats[0], repeats[1]);
                        if (has_offset)  prim->setTEOffset(te, offset[0], offset[1]);
                        if (has_rot)     prim->setTERotation(te, (F32)(rot_deg * DEG_TO_RAD));
                    }
                    applied = true;
                }
                else if (has_mapping)
                {
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
            "{\"offset\"} [u,v], {\"rotation\"} degrees; these three set the PBR texture transform "
            "when the face has a material. Applying a material keeps the face's current mapping. Returns {object_id, faces:[{link, face, "
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
