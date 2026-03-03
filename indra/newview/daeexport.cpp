/**
 * @file daeexport.cpp
 * @brief A system which allows saving in-world objects to Collada .DAE files for offline texturizing/shading.
 * @authors Latif Khalifa, Cinder Biscuits
 *
 * $LicenseInfo:firstyear=2013&license=LGPLV2.1$
 * Copyright (C) 2013 Latif Khalifa
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General
 * Public License along with this library; if not, write to the
 * Free Software Foundation, Inc., 51 Franklin Street, Fifth Floor,
 * Boston, MA 02110-1301 USA
 */
#include "llviewerprecompiledheaders.h"

#include "daeexport.h"

//colladadom includes
#ifdef __GNUC__
#pragma GCC diagnostic ignored "-Woverloaded-virtual"
#endif

#include "dae.h"
#include "dom/domAsset.h"
#include "dom/domCOLLADA.h"
#include "dom/domController.h"
#include "dom/domEffect.h"
#include "dom/domGeometry.h"
#include "dom/domMatrix.h"

// llimage includes
#include "llimagej2c.h"
#include "llimagepng.h"
#include "llimagetga.h"

// llui includes
#include "lltexturectrl.h"

// newview includes
#include "llagent.h"
#include "llappviewer.h"
#include "llcallbacklist.h"
#include "llfilepicker.h"
#include "llinventoryfunctions.h"
#include "llnotificationsutil.h"
#include "llselectmgr.h"
#include "lltexturecache.h"
#include "llversioninfo.h"
#include "llviewercontrol.h"
#include "llviewermenufile.h"
#include "llviewernetwork.h"
#include "llviewertexturelist.h"
#include "llvovolume.h"

static constexpr F32 TEXTURE_DOWNLOAD_TIMEOUT = 60.f;

// *FIXME: Don't hard code these and allow the floater to resize. Right now, I'm too lazy. <FS:CR>
static constexpr S32 EXPANDED_WIDTH = 500;
static constexpr S32 COLLAPSED_WIDTH = 250;

namespace DAEExportUtil
{
    static const LLUUID LL_TEXTURE_TRANSPARENT = LLUUID("8dcd4a48-2d37-4909-9f78-f7a9eb4ef903");
    static const LLUUID LL_TEXTURE_BLANK = LLUUID("5748decc-f629-461c-9a36-a35a221fe21f");

    static const std::string image_format_ext[] = { "tga", "png", "j2c" };
    enum image_format_type
    {
        ft_tga,
        ft_png,
        ft_j2c
    };
}

namespace
{
struct RigWeightInfluence
{
    S32 mJoint = 0;
    F32 mWeight = 0.f;
};

using rig_weight_list_t = std::vector<RigWeightInfluence>;
using rig_vertex_weights_t = std::vector<rig_weight_list_t>;

std::string getTextureNameFromInventory(const LLUUID& texture_id)
{
    LLViewerInventoryCategory::cat_array_t cats;
    LLViewerInventoryItem::item_array_t items;
    LLAssetIDMatches asset_id_matches(texture_id);
    gInventory.collectDescendentsIf(LLUUID::null, cats, items, LLInventoryModel::INCLUDE_TRASH, asset_id_matches);

    if (!items.empty())
    {
        return items[0]->getName();
    }

    return std::string();
}

std::string makeTextureExportName(const LLUUID& texture_id)
{
    std::string safe_name = gDirUtilp->getScrubbedFileName(getTextureNameFromInventory(texture_id));
    std::replace(safe_name.begin(), safe_name.end(), ' ', '_');

    if (safe_name.empty())
    {
        safe_name = "texture";
    }

    return llformat("%s_%s", safe_name.c_str(), texture_id.asString().c_str());
}

std::string makeColladaId(const std::string& base)
{
    std::string result;
    result.reserve(base.size());

    for (char ch : base)
    {
        const bool is_alpha = (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z');
        const bool is_digit = (ch >= '0' && ch <= '9');
        if (is_alpha || is_digit || ch == '_' || ch == '-' || ch == '.')
        {
            result.push_back(ch);
        }
        else
        {
            result.push_back('_');
        }
    }

    if (result.empty())
    {
        result = "id";
    }

    if (result[0] >= '0' && result[0] <= '9')
    {
        result.insert(result.begin(), '_');
    }

    return result;
}

void appendMatrixColumnMajor(const LLMatrix4a& matrix, std::vector<F32>& out)
{
    LLMatrix4 mat(matrix.getF32ptr());
    for (S32 col = 0; col < 4; ++col)
    {
        for (S32 row = 0; row < 4; ++row)
        {
            out.push_back(mat.mMatrix[row][col]);
        }
    }
}

std::string joinFloatValues(const std::vector<F32>& values)
{
    std::string out;
    for (size_t i = 0; i < values.size(); ++i)
    {
        if (i != 0)
        {
            out += " ";
        }
        out += llformat("%f", values[i]);
    }
    return out;
}

std::string joinNames(const std::vector<std::string>& names)
{
    std::string out;
    for (size_t i = 0; i < names.size(); ++i)
    {
        if (i != 0)
        {
            out += " ";
        }
        out += names[i];
    }
    return out;
}

std::string joinIntValues(const std::vector<S32>& values)
{
    std::string out;
    for (size_t i = 0; i < values.size(); ++i)
    {
        if (i != 0)
        {
            out += " ";
        }
        out += llformat("%d", values[i]);
    }
    return out;
}

bool getRiggedSkinInfo(LLViewerObject* obj, const LLMeshSkinInfo*& skin_info)
{
    skin_info = nullptr;

    if (!obj || obj->getPCode() != LL_PCODE_VOLUME)
    {
        return false;
    }

    LLVOVolume* volobjp = static_cast<LLVOVolume*>(obj);
    if (!volobjp->isRiggedMesh())
    {
        return false;
    }

    skin_info = volobjp->getSkinInfo();
    if (!skin_info || skin_info->mJointNames.empty())
    {
        return false;
    }

    return true;
}

rig_weight_list_t decodePackedWeights(const LLVector4a& packed_weights, U32 joint_count)
{
    rig_weight_list_t weights;
    if (joint_count == 0)
    {
        return weights;
    }

    F32 total = 0.f;
    const F32* packed = packed_weights.getF32ptr();
    for (S32 idx = 0; idx < 4; ++idx)
    {
        F32 packed_weight = packed[idx];
        S32 joint_idx = llclamp((S32)floorf(packed_weight), 0, (S32)joint_count - 1);
        F32 weight = packed_weight - floorf(packed_weight);
        if (weight > 0.f)
        {
            weights.push_back({ joint_idx, weight });
            total += weight;
        }
    }

    if (total > 0.f)
    {
        F32 norm = 1.f / total;
        for (RigWeightInfluence& influence : weights)
        {
            influence.mWeight *= norm;
        }
    }
    else
    {
        weights.push_back({ 0, 1.f });
    }

    return weights;
}

void addSkeletonNodes(daeElement* scene, const std::string& skeleton_root_id, const LLMeshSkinInfo* skin_info, const std::string& id_prefix)
{
    if (!scene || !skin_info)
    {
        return;
    }

    daeElement* skeleton_root = scene->add("node");
    skeleton_root->setAttribute("type", "JOINT");
    skeleton_root->setAttribute("id", skeleton_root_id.c_str());
    skeleton_root->setAttribute("sid", skeleton_root_id.c_str());
    skeleton_root->setAttribute("name", skeleton_root_id.c_str());
    skeleton_root->add("matrix")->setCharData("1 0 0 0 0 1 0 0 0 0 1 0 0 0 0 1");

    for (S32 idx = 0; idx < skin_info->mJointNames.size(); ++idx)
    {
        const std::string joint_name = skin_info->mJointNames[idx];
        daeElement* joint_node = skeleton_root->add("node");
        joint_node->setAttribute("type", "JOINT");
        joint_node->setAttribute("id", llformat("%s-joint-%d", id_prefix.c_str(), idx).c_str());
        joint_node->setAttribute("sid", joint_name.c_str());
        joint_node->setAttribute("name", joint_name.c_str());
        joint_node->add("matrix")->setCharData("1 0 0 0 0 1 0 0 0 0 1 0 0 0 0 1");
    }
}
} // namespace


ColladaExportFloater::ColladaExportFloater(const LLSD& key)
: LLFloater(key),
  mCurrentObjectID(LLUUID::null),
  mDirty(true)
{
    mCommitCallbackRegistrar.add("ColladaExport.TextureExport", boost::bind(&ColladaExportFloater::onTextureExportCheck, this));
}

//virtual
ColladaExportFloater::~ColladaExportFloater()
{
    if (gIdleCallbacks.containsFunction(CacheReadResponder::saveTexturesWorker, this))
    {
        gIdleCallbacks.deleteFunction(CacheReadResponder::saveTexturesWorker, this);
    }
}

bool ColladaExportFloater::postBuild()
{
    mTitleProgress = getString("texture_progress");
    mTexturePanel = getChild<LLPanel>("textures_panel");
    childSetAction("export_btn", boost::bind(&ColladaExportFloater::onClickExport, this));
    LLSelectMgr::getInstance()->mUpdateSignal.connect(boost::bind(&ColladaExportFloater::updateSelection, this));

    return true;
}

void ColladaExportFloater::draw()
{
    if (mDirty)
    {
        refresh();
        mDirty = false;
    }
    LLFloater::draw();
}

void ColladaExportFloater::dirty()
{
    mDirty = true;
}

void ColladaExportFloater::refresh()
{
    addSelectedObjects();
    onTextureExportCheck();
    addTexturePreview();
    updateUI();
}

void ColladaExportFloater::onOpen(const LLSD& key)
{
    LLObjectSelectionHandle object_selection = LLSelectMgr::getInstance()->getSelection();
    if(!(object_selection->getPrimaryObject()))
    {
        closeFloater();
        return;
    }
    mObjectSelection = LLSelectMgr::getInstance()->getEditSelection();
    refresh();
}

void ColladaExportFloater::updateTitleProgress()
{
    LLSD args;
    args["OBJECT"] = mObjectName;
    args["COUNT"] = llformat("%d", mTexturesToSave.size());
    mTitleProgress.setArgs(args);
    setTitle(mTitleProgress);
}

void ColladaExportFloater::updateUI()
{
    childSetTextArg("NameText", "[NAME]", mObjectName);
    childSetTextArg("exportable_prims", "[COUNT]", llformat("%d", mIncluded));
    childSetTextArg("exportable_prims", "[TOTAL]", llformat("%d", mTotal));
    childSetTextArg("exportable_textures", "[COUNT]", llformat("%d", mNumExportableTextures));
    childSetTextArg("exportable_textures", "[TOTAL]", llformat("%d", mNumTextures));

    LLUIString title = getString("floater_title");
    title.setArg("[OBJECT]", mObjectName);
    setTitle(title);
    childSetEnabled("export_textures_check", mNumExportableTextures);
    childSetEnabled("export_btn", mIncluded);
}

void ColladaExportFloater::onClickExport()
{
    LLFilePickerReplyThread::startPicker(boost::bind(&ColladaExportFloater::onExportFileSelected, this, _1),
        LLFilePicker::FFSAVE_COLLADA, LLDir::getScrubbedFileName(mObjectName + ".dae"));
}

void ColladaExportFloater::onExportFileSelected(const std::vector<std::string>& filenames)
{
    mFilename = filenames[0];

    if (gSavedSettings.getBOOL("DAEExportTextures"))
    {
        saveTextures();
    }
    else
    {
        onTexturesSaved();
    }
}

void ColladaExportFloater::onTextureExportCheck()
{
    bool show_tex_panel = (gSavedSettings.getBOOL("DAEExportTextures") && mNumExportableTextures);

    getChild<LLPanel>("tex_layout_panel")->setVisible(show_tex_panel);
    if (show_tex_panel)
    {
        reshape(EXPANDED_WIDTH, getRect().getHeight());
    }
    else
    {
        reshape(COLLAPSED_WIDTH, getRect().getHeight());
    }
}

void ColladaExportFloater::onTexturesSaved()
{
    bool success = mSaver.saveDAE(mFilename);
    LLSD args;
    args["OBJECT"] = mObjectName;
    args["FILENAME"] = mFilename;
    if (success)
    {
        LL_INFOS() << "Collada DAE export successful" << LL_ENDL;
        LLNotificationsUtil::add("ExportColladaSuccess", args);
    }
    else
    {
        LL_WARNS() << "Collada DAE export failed" << LL_ENDL;
        LLNotificationsUtil::add("ExportColladaFailure", args);
    }
    closeFloater();
}

void ColladaExportFloater::addSelectedObjects()
{
    mTotal = 0;
    mIncluded = 0;
    mNumTextures = 0;
    mNumExportableTextures = 0;
    mSaver.mObjects.clear();
    mSaver.mTextures.clear();
    mSaver.mTextureNames.clear();
    if (mObjectSelection)
    {
        LLSelectNode* node = mObjectSelection->getFirstRootNode();
        if (node)
        {
            mCurrentObjectID = node->getObject()->getID();
            mSaver.mOffset = -mObjectSelection->getFirstRootObject()->getRenderPosition();
            mObjectName = node->mName;

            for (LLObjectSelection::iterator iter = mObjectSelection->begin(); iter != mObjectSelection->end(); ++iter)
            {
                mTotal++;
                LLSelectNode* node = *iter;
                if (!node->getObject()->getVolume()) continue;
                mIncluded++;
                mSaver.add(node->getObject(), node->mName);
            }

            if (mSaver.mObjects.empty())
            {
                //LLNotificationsUtil::add("ExportFailed");
                return;
            }
        }
        else
        {
            mObjectName = "";
        }
        mSaver.updateTextureInfo();
        mNumTextures = static_cast<S32>(mSaver.mTextures.size());
        mNumExportableTextures = getNumExportableTextures();
    }
}

void ColladaExportFloater::updateSelection()
{
    LLObjectSelectionHandle object_selection = LLSelectMgr::getInstance()->getSelection();
    LLSelectNode* node = object_selection->getFirstRootNode();

    if (node && !node->mValid && node->getObject()->getID() == mCurrentObjectID)
    {
        return;
    }

    mObjectSelection = object_selection;
    dirty();
    refresh();
}

S32 ColladaExportFloater::getNumExportableTextures() const
{
    S32 res = 0;
    for (DAESaver::string_list_t::const_iterator t = mSaver.mTextureNames.begin(); t != mSaver.mTextureNames.end(); ++t)
    {
        std::string name = *t;
        if (!name.empty())
        {
            ++res;
        }
    }

    return res;
}


void ColladaExportFloater::addTexturePreview()
{
    S32 num_text = mNumExportableTextures;
    if (num_text == 0) return;
    S32 img_width = 100;
    S32 img_height = img_width + 15;
    S32 panel_height = (num_text / 2 + 1) * (img_height) + 10;
    // *TODO: It would be better to check against a list of controls
    mTexturePanel->deleteAllChildren();
    mTexturePanel->reshape(230, panel_height);
    S32 img_nr = 0;
    for (S32 i=0; i < mSaver.mTextures.size(); i++)
    {
        if (mSaver.mTextureNames[i].empty()) continue;
        S32 left = 8 + (img_nr % 2) * (img_width + 13);
        S32 bottom = panel_height - (10 + (img_nr / 2 + 1) * (img_height));
        LLRect r(left, bottom + img_height, left + img_width, bottom);
        LLTextureCtrl::Params p;
        p.rect(r);
        p.layout("topleft");
        p.name(mSaver.mTextureNames[i]);
        p.image_id(mSaver.mTextures[i]);
        p.tool_tip(mSaver.mTextureNames[i]);
        LLTextureCtrl* texture_block = LLUICtrlFactory::create<LLTextureCtrl>(p);
        mTexturePanel->addChild(texture_block);
        img_nr++;
    }
}

void ColladaExportFloater::saveTextures()
{
    mTexturesToSave.clear();
    for (S32 i=0; i < mSaver.mTextures.size(); i++)
    {
        if (mSaver.mTextureNames[i].empty()) continue;

        mTexturesToSave[mSaver.mTextures[i]] = mSaver.mTextureNames[i];
    }

    mSaver.mImageFormat = DAEExportUtil::image_format_ext[gSavedSettings.getS32("DAEExportTexturesFormat")];

    LL_DEBUGS("export") << "Starting to save textures" << LL_ENDL;
    mTimer.start();
    mTimer.setTimerExpirySec(TEXTURE_DOWNLOAD_TIMEOUT);
    updateTitleProgress();
    gIdleCallbacks.addFunction(CacheReadResponder::saveTexturesWorker, this);
}


ColladaExportFloater::CacheReadResponder::CacheReadResponder(const LLUUID& id, LLImageFormatted* image, std::string name, S32 img_type)
    : mFormattedImage(image), mID(id), mName(name), mImageType(img_type)
{
    setImage(image);
}

void ColladaExportFloater::CacheReadResponder::setData(U8* data, S32 datasize, S32 imagesize, S32 imageformat, bool imagelocal)
{
    if (imageformat == IMG_CODEC_TGA && mFormattedImage->getCodec() == IMG_CODEC_J2C)
    {
        LL_WARNS("export") << "FAILED: texture " << mID << " is formatted as TGA. Not saving." << LL_ENDL;
        mFormattedImage = NULL;
        mImageSize = 0;
        return;
    }

    if (mFormattedImage.notNull())
    {
        if (mFormattedImage->getCodec() == imageformat)
        {
            mFormattedImage->appendData(data, datasize);
        }
        else
        {
            LL_WARNS("export") << "FAILED: texture " << mID << " in wrong format." << LL_ENDL;
            mFormattedImage = NULL;
            mImageSize = 0;
            return;
        }
    }
    else
    {
        mFormattedImage = LLImageFormatted::createFromType(imageformat);
        mFormattedImage->setData(data, datasize);
    }
    mImageSize = imagesize;
    mImageLocal = imagelocal;
}

//virtual
void ColladaExportFloater::CacheReadResponder::completed(bool success)
{
    if (success && mFormattedImage.notNull() && mImageSize > 0)
    {
        bool ok = false;

        // If we are saving jpeg2000, no need to do anything, just write to disk
        if (mImageType == DAEExportUtil::ft_j2c)
        {
            mName += "." + mFormattedImage->getExtension();
            ok = mFormattedImage->save(mName);
        }
        else
        {
            // For other formats we need to decode first
            if (mFormattedImage->updateData() && ( (mFormattedImage->getWidth() * mFormattedImage->getHeight() * mFormattedImage->getComponents()) != 0 ) )
            {
                mFormattedImage->setDiscardLevel(0); // <FS/> [FIRE-35292] Fix for textures getting downscaled and compressed

                LLPointer<LLImageRaw> raw = new LLImageRaw;
                raw->resize(mFormattedImage->getWidth(), mFormattedImage->getHeight(),  mFormattedImage->getComponents());

                if (mFormattedImage->decode(raw, 0))
                {
                    LLPointer<LLImageFormatted> img = NULL;
                    switch (mImageType)
                    {
                    case DAEExportUtil::ft_tga:
                        img = new LLImageTGA;
                        break;
                    case DAEExportUtil::ft_png:
                        img = new LLImagePNG;
                        break;
                    }

                    if (!img.isNull())
                    {
                        if (img->encode(raw, 0))
                        {
                            mName += "." + img->getExtension();
                            ok = img->save(mName);
                        }
                    }
                }
            }
        }

        if (ok)
        {
            LL_DEBUGS("export") << "Saved texture to " << mName << LL_ENDL;
        }
        else
        {
            LL_WARNS("export") << "FAILED to save texture " << mID << LL_ENDL;
        }
    }
    else
    {
        LL_WARNS("export") << "FAILED to save texture " << mID << LL_ENDL;
    }
}

//static
void ColladaExportFloater::CacheReadResponder::saveTexturesWorker(void* data)
{
    ColladaExportFloater* me = (ColladaExportFloater *)data;
    if (me->mTexturesToSave.size() == 0)
    {
        LL_DEBUGS("export") << "Done saving textures" << LL_ENDL;
        me->updateTitleProgress();
        gIdleCallbacks.deleteFunction(saveTexturesWorker, me);
        me->mTimer.stop();
        me->onTexturesSaved();
        return;
    }

    LLUUID id = me->mTexturesToSave.begin()->first;
    LLViewerTexture* imagep = LLViewerTextureManager::findFetchedTexture(id, TEX_LIST_STANDARD);
    if (!imagep)
    {
        me->mTexturesToSave.erase(id);
        me->updateTitleProgress();
        me->mTimer.reset();
        me->mTimer.setTimerExpirySec(TEXTURE_DOWNLOAD_TIMEOUT);
    }
    else
    {
        if (imagep->getDiscardLevel() == 0) // image download is complete
        {
            LL_DEBUGS("export") << "Saving texture " << id << LL_ENDL;
            LLImageFormatted* img = new LLImageJ2C;
            S32 img_type = gSavedSettings.getS32("DAEExportTexturesFormat");
            std::string name = gDirUtilp->getDirName(me->mFilename);
            name += gDirUtilp->getDirDelimiter() + me->mTexturesToSave[id];
            CacheReadResponder* responder = new CacheReadResponder(id, img, name, img_type);
            // <FS:minerjr> [FIRE-35292] Fix for textures getting downscaled and compressed
            //LLAppViewer::getTextureCache()->readFromCache(id, 0, 999999, responder);
            // The above line hard coded the size of data to read from the cached version of the texture as 999999,
            // where now we will calcuate the correct value based upon the texture's full width, height and # of components (3=RGB, 4=RGBA) and
            // the discard level (0)). There is a choice to change the rate, but we seem to use the value of 1/8 compression level
            S32 texture_size = LLImageJ2C::calcDataSizeJ2C(imagep->getFullWidth(), imagep->getFullHeight(), imagep->getComponents(), 0);// , F32 rate) rate = const F32 DEFAULT_COMPRESSION_RATE = 1.f/8.f;
            // Use calculated texture_size (from LLTextureFetch::createRequest see "else if (w*h*c > 0)" statement for more info)
            LLAppViewer::getTextureCache()->readFromCache(id, 0, texture_size, responder);
            // </FS:minerjr> [FIRE-35292]
            me->mTexturesToSave.erase(id);
            me->updateTitleProgress();
            me->mTimer.reset();
            me->mTimer.setTimerExpirySec(TEXTURE_DOWNLOAD_TIMEOUT);
        }
        else if (me->mTimer.hasExpired())
        {
            LL_WARNS("export") << "Timed out downloading texture " << id << LL_ENDL;
            me->mTexturesToSave.erase(id);
            me->updateTitleProgress();
            me->mTimer.reset();
            me->mTimer.setTimerExpirySec(TEXTURE_DOWNLOAD_TIMEOUT);
        }
    }
}

void DAESaver::add(const LLViewerObject* prim, const std::string name)
{
    mObjects.push_back(std::pair<LLViewerObject*,std::string>((LLViewerObject*)prim, name));
}

void DAESaver::updateTextureInfo()
{
    mTextures.clear();
    mTextureNames.clear();

    for (obj_info_t::iterator obj_iter = mObjects.begin(); obj_iter != mObjects.end(); ++obj_iter)
    {
        LLViewerObject* obj = obj_iter->first;
        S32 num_faces = obj->getVolume()->getNumVolumeFaces();
        for (S32 face_num = 0; face_num < num_faces; ++face_num)
        {
            LLTextureEntry* te = obj->getTE(face_num);
            const LLUUID id = te->getID();

            if (std::find(mTextures.begin(), mTextures.end(), id) != mTextures.end()) continue;

            mTextures.push_back(id);
            if (id != DAEExportUtil::LL_TEXTURE_BLANK)
            {
                mTextureNames.push_back(makeTextureExportName(id));
            }
            else
            {
                mTextureNames.push_back(std::string());
            }
        }
    }
}

class v4adapt
{
private:
    LLStrider<LLVector4a> mV4aStrider;
public:
    v4adapt(LLVector4a* vp){ mV4aStrider = vp; }
    inline LLVector3 operator[] (const unsigned int i)
    {
        return LLVector3((F32*)&mV4aStrider[i]);
    }
};

void DAESaver::addSource(daeElement* mesh, const char* src_id, std::string params, const std::vector<F32> &vals)
{
    daeElement* source = mesh->add("source");
    source->setAttribute("id", src_id);
    daeElement* src_array = source->add("float_array");

    src_array->setAttribute("id", llformat("%s-%s", src_id, "array").c_str());
    src_array->setAttribute("count", llformat("%d", vals.size()).c_str());

    for (S32 i = 0; i < vals.size(); i++)
    {
        ((domFloat_array*)src_array)->getValue().append(vals[i]);
    }

    domAccessor* acc = daeSafeCast<domAccessor>(source->add("technique_common accessor"));
    acc->setSource(llformat("#%s-%s", src_id, "array").c_str());
    acc->setCount(vals.size() / params.size());
    acc->setStride(params.size());

    for (std::string::iterator p_iter = params.begin(); p_iter != params.end(); ++p_iter)
    {
        domElement* pX = acc->add("param");
        pX->setAttribute("name", llformat("%c", *p_iter).c_str());
        pX->setAttribute("type", "float");
    }
}

void DAESaver::addPolygons(daeElement* mesh, const char* geomID, const char* materialID, LLViewerObject* obj, int_list_t* faces_to_include)
{
    domPolylist* polylist = daeSafeCast<domPolylist>(mesh->add("polylist"));
    polylist->setMaterial(materialID);

    // Vertices semantic
    {
        domInputLocalOffset* input = daeSafeCast<domInputLocalOffset>(polylist->add("input"));
        input->setSemantic("VERTEX");
        input->setOffset(0);
        input->setSource(llformat("#%s-%s", geomID, "vertices").c_str());
    }

    // Normals semantic
    {
        domInputLocalOffset* input = daeSafeCast<domInputLocalOffset>(polylist->add("input"));
        input->setSemantic("NORMAL");
        input->setOffset(0);
        input->setSource(llformat("#%s-%s", geomID, "normals").c_str());
    }

    // UV semantic
    {
        domInputLocalOffset* input = daeSafeCast<domInputLocalOffset>(polylist->add("input"));
        input->setSemantic("TEXCOORD");
        input->setOffset(0);
        if(gSavedSettings.getBOOL("DAEExportSingleUVMap"))
        {
            input->setSource(llformat("#%s-%s", "unified", "map0").c_str());
        }
        else
        {
            input->setSource(llformat("#%s-%s", geomID, "map0").c_str());
        }
    }

    // Save indices
    domP* p = daeSafeCast<domP>(polylist->add("p"));
    domPolylist::domVcount *vcount = daeSafeCast<domPolylist::domVcount>(polylist->add("vcount"));
    S32 index_offset = 0;
    S32 num_tris = 0;
    for (S32 face_num = 0; face_num < obj->getVolume()->getNumVolumeFaces(); face_num++)
    {
        if (skipFace(obj->getTE(face_num))) continue;

        const LLVolumeFace* face = (LLVolumeFace*)&obj->getVolume()->getVolumeFace(face_num);

        if (faces_to_include == NULL || (std::find(faces_to_include->begin(), faces_to_include->end(), face_num) != faces_to_include->end()))
        {
            for (S32 i = 0; i < face->mNumIndices; i++)
            {
                // FIRE-24016 Allow >64k verts in exported mesh
                // Contributed by Angus Boyd
                U32 index = index_offset + face->mIndices[i];
                (p->getValue()).append(index);
                if (i % 3 == 0)
                {
                    (vcount->getValue()).append(3);
                    num_tris++;
                }
            }
        }
        index_offset += face->mNumVertices;
    }
    polylist->setCount(num_tris);
}

void DAESaver::transformTexCoord(S32 num_vert, LLVector2* coord, LLVector3* positions, LLVector3* normals, LLTextureEntry* te, LLVector3 scale)
{
    F32 cosineAngle = cos(te->getRotation());
    F32 sinAngle = sin(te->getRotation());

    for (S32 ii=0; ii<num_vert; ii++)
    {
        if (LLTextureEntry::TEX_GEN_PLANAR == te->getTexGen())
        {
            LLVector3 normal = normals[ii];
            LLVector3 pos = positions[ii];
            LLVector3 binormal;
            F32 d = normal * LLVector3::x_axis;
            if (d >= 0.5f || d <= -0.5f)
            {
                binormal = LLVector3::y_axis;
                if (normal.mV[0] < 0)
                    binormal *= -1.0f;
            }
            else
            {
                binormal = LLVector3::x_axis;
                if (normal.mV[1] > 0)
                    binormal *= -1.0f;
            }
            LLVector3 tangent = binormal % normal;
            LLVector3 scaledPos = pos.scaledVec(scale);
            coord[ii].mV[0] = 1.f + ((binormal * scaledPos) * 2.f - 0.5f);
            coord[ii].mV[1] = -((tangent * scaledPos) * 2.f - 0.5f);
        }

        F32 repeatU;
        F32 repeatV;
        te->getScale(&repeatU, &repeatV);
        F32 tX = coord[ii].mV[0] - 0.5f;
        F32 tY = coord[ii].mV[1] - 0.5f;

        F32 offsetU;
        F32 offsetV;
        te->getOffset(&offsetU, &offsetV);

        coord[ii].mV[0] = (tX * cosineAngle + tY * sinAngle) * repeatU + offsetU + 0.5f;
        coord[ii].mV[1] = (-tX * sinAngle + tY * cosineAngle) * repeatV + offsetV + 0.5f;
    }
}

bool DAESaver::saveDAE(std::string filename)
{
    // Collada expects file and folder names to be escaped
    // Note: cdom::nativePathToUri()
    // Same as in LLDAELoader::OpenFile()
    const char* allowed =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZ"
        "abcdefghijklmnopqrstuvwxyz"
        "0123456789"
        "%-._~:\"|\\/";
    std::string uri_filename = LLURI::escape(filename, allowed);

    mAllMaterials.clear();
    mTotalNumMaterials = 0;
    DAE dae;
    // First set the filename to save
    daeElement* root = dae.add(uri_filename);

    // Obligatory elements in header
    daeElement* asset = root->add("asset");
    // Get ISO format time
    time_t rawtime;
    time(&rawtime);
    struct tm* utc_time = gmtime(&rawtime);
    std::string date = llformat("%04d-%02d-%02dT%02d:%02d:%02d", utc_time->tm_year + 1900, utc_time->tm_mon + 1, utc_time->tm_mday, utc_time->tm_hour, utc_time->tm_min, utc_time->tm_sec);
    daeElement* created = asset->add("created");
    created->setCharData(date);
    daeElement* modified = asset->add("modified");
    modified->setCharData(date);
    daeElement* unit = asset->add("unit");
    unit->setAttribute("name", "meter");
    unit->setAttribute("value", "1");
    daeElement* up_axis = asset->add("up_axis");
    up_axis->setCharData("Z_UP");

    // File creator
    std::string author = "Unknown";
    if (!gAgentUsername.empty())
        author = gAgentUsername;

    daeElement* contributor = asset->add("contributor");
    contributor->add("author")->setCharData(author);
    contributor->add("authoring_tool")->setCharData(LLVersionInfo::getInstance()->getChannelAndVersion());

    daeElement* images = root->add("library_images");
    daeElement* geomLib = root->add("library_geometries");
    daeElement* controllerLib = root->add("library_controllers");
    daeElement* effects = root->add("library_effects");
    daeElement* materials = root->add("library_materials");
    daeElement* scene = root->add("library_visual_scenes visual_scene");
    scene->setAttribute("id", "Scene");
    scene->setAttribute("name", "Scene");

    if (gSavedSettings.getBOOL("DAEExportTextures"))
    {
        generateImagesSection(images);
    }

    S32 prim_nr = 0;

    for (obj_info_t::iterator obj_iter = mObjects.begin(); obj_iter != mObjects.end(); ++obj_iter)
    {
        LLViewerObject* obj = obj_iter->first;

        std::string name = llformat("prim%d", prim_nr++);

        const char* geomID = name.c_str();
        const LLMeshSkinInfo* skin_info = nullptr;
        bool has_rigging = getRiggedSkinInfo(obj, skin_info);
        rig_vertex_weights_t rig_vertex_weights;

        daeElement* geom = geomLib->add("geometry");
        geom->setAttribute("id", llformat("%s-%s", geomID, "mesh").c_str());
        daeElement* mesh = geom->add("mesh");

        std::vector<F32> position_data;
        std::vector<F32> normal_data;
        std::vector<F32> uv_data;
        bool applyTexCoord = gSavedSettings.getBOOL("DAEExportTextureParams");
        bool consolidateUVMap = gSavedSettings.getBOOL("DAEExportSingleUVMap");

        S32 num_faces = obj->getVolume()->getNumVolumeFaces();
        for (S32 face_num = 0; face_num < num_faces; face_num++)
        {
            if (skipFace(obj->getTE(face_num))) continue;

            const LLVolumeFace* face = (LLVolumeFace*)&obj->getVolume()->getVolumeFace(face_num);

            v4adapt verts(face->mPositions);
            v4adapt norms(face->mNormals);

            LLVector2* newCoord = NULL;

            if (applyTexCoord)
            {
                newCoord = new LLVector2[face->mNumVertices];
                LLVector3* newPos = new LLVector3[face->mNumVertices];
                LLVector3* newNormal = new LLVector3[face->mNumVertices];
                for (S32 i = 0; i < face->mNumVertices; i++)
                {
                    newPos[i] = verts[i];
                    newNormal[i] = norms[i];
                    newCoord[i] = face->mTexCoords[i];
                }
                transformTexCoord(face->mNumVertices, newCoord, newPos, newNormal, obj->getTE(face_num), obj->getScale());
                delete[] newPos;
                delete[] newNormal;
            }

            for (S32 i=0; i < face->mNumVertices; i++)
            {
                const LLVector3 v = verts[i];
                position_data.push_back(v.mV[VX]);
                position_data.push_back(v.mV[VY]);
                position_data.push_back(v.mV[VZ]);

                const LLVector3 n = norms[i];
                normal_data.push_back(n.mV[VX]);
                normal_data.push_back(n.mV[VY]);
                normal_data.push_back(n.mV[VZ]);

                const LLVector2 uv = applyTexCoord ? newCoord[i] : face->mTexCoords[i];

                uv_data.push_back(uv.mV[VX]);
                uv_data.push_back(uv.mV[VY]);

                if (has_rigging)
                {
                    if (face->mWeights)
                    {
                        rig_vertex_weights.push_back(decodePackedWeights(face->mWeights[i], (U32)skin_info->mJointNames.size()));
                    }
                    else
                    {
                        rig_weight_list_t fallback_weights;
                        fallback_weights.push_back({ 0, 1.f });
                        rig_vertex_weights.push_back(fallback_weights);
                    }
                }
            }

            if (applyTexCoord)
            {
                delete[] newCoord;
            }
        }

        if (has_rigging && rig_vertex_weights.size() != position_data.size() / 3)
        {
            LL_WARNS("export") << "Rigged export fallback to static mesh for " << geomID << " due to vertex/weight mismatch." << LL_ENDL;
            has_rigging = false;
            rig_vertex_weights.clear();
        }

        addSource(mesh, llformat("%s-%s", geomID, "positions").c_str(), "XYZ", position_data);
        addSource(mesh, llformat("%s-%s", geomID, "normals").c_str(), "XYZ", normal_data);
        if(consolidateUVMap)
        {
            addSource(mesh, llformat("%s-%s", "unified", "map0").c_str(), "ST", uv_data);
        }
        else
        {
            addSource(mesh, llformat("%s-%s", geomID, "map0").c_str(), "ST", uv_data);
        }


        // Add the <vertices> element
        {
            daeElement* verticesNode = mesh->add("vertices");
            verticesNode->setAttribute("id", llformat("%s-%s", geomID, "vertices").c_str());
            daeElement* verticesInput = verticesNode->add("input");
            verticesInput->setAttribute("semantic", "POSITION");
            verticesInput->setAttribute("source", llformat("#%s-%s", geomID, "positions").c_str());
        }

        material_list_t objMaterials;
        getMaterials(obj, &objMaterials);

        // Add triangles
        if (gSavedSettings.getBOOL("DAEExportConsolidateMaterials"))
        {
            for (S32 objMaterial = 0; objMaterial < objMaterials.size(); objMaterial++)
            {
                int_list_t faces;
                getFacesWithMaterial(obj, objMaterials[objMaterial], &faces);
                std::string matName = objMaterials[objMaterial].name;
                addPolygons(mesh, geomID, (matName + "-material").c_str(), obj, &faces);
            }
        }
        else
        {
            S32 mat_nr = 0;
            for (S32 face_num = 0; face_num < num_faces; face_num++)
            {
                if (skipFace(obj->getTE(face_num))) continue;
                int_list_t faces;
                faces.push_back(face_num);
                std::string matName = objMaterials[mat_nr++].name;
                addPolygons(mesh, geomID, (matName + "-material").c_str(), obj, &faces);
            }
        }

        std::string controller_id;
        std::string skeleton_root_id;
        if (has_rigging && !rig_vertex_weights.empty())
        {
            controller_id = llformat("%s-skin", geomID);
            skeleton_root_id = llformat("%s-skeleton", geomID);

            daeElement* controller = controllerLib->add("controller");
            controller->setAttribute("id", controller_id.c_str());
            daeElement* skin = controller->add("skin");
            skin->setAttribute("source", llformat("#%s-mesh", geomID).c_str());

            std::vector<F32> bind_shape_values;
            appendMatrixColumnMajor(skin_info->mBindShapeMatrix, bind_shape_values);
            std::string bind_shape_data = joinFloatValues(bind_shape_values);
            skin->add("bind_shape_matrix")->setCharData(bind_shape_data.c_str());

            std::string joint_source_id = llformat("%s-joints", geomID);
            daeElement* joint_source = skin->add("source");
            joint_source->setAttribute("id", joint_source_id.c_str());
            daeElement* joint_array = joint_source->add("Name_array");
            std::string joint_array_id = joint_source_id + "-array";
            joint_array->setAttribute("id", joint_array_id.c_str());
            joint_array->setAttribute("count", llformat("%d", (S32)skin_info->mJointNames.size()).c_str());
            std::string joint_names = joinNames(skin_info->mJointNames);
            joint_array->setCharData(joint_names.c_str());

            domAccessor* joint_accessor = daeSafeCast<domAccessor>(joint_source->add("technique_common accessor"));
            std::string joint_array_ref = "#" + joint_array_id;
            joint_accessor->setSource(joint_array_ref.c_str());
            joint_accessor->setCount((S32)skin_info->mJointNames.size());
            joint_accessor->setStride(1);
            domElement* joint_param = joint_accessor->add("param");
            joint_param->setAttribute("name", "JOINT");
            joint_param->setAttribute("type", "Name");

            std::vector<F32> inv_bind_data;
            inv_bind_data.reserve(skin_info->mJointNames.size() * 16);
            LLMatrix4 identity;
            identity.setIdentity();
            LLMatrix4a identity4(identity);
            for (S32 joint_idx = 0; joint_idx < skin_info->mJointNames.size(); ++joint_idx)
            {
                if (joint_idx < skin_info->mInvBindMatrix.size())
                {
                    appendMatrixColumnMajor(skin_info->mInvBindMatrix[joint_idx], inv_bind_data);
                }
                else
                {
                    appendMatrixColumnMajor(identity4, inv_bind_data);
                }
            }
            std::string inv_bind_source_id = llformat("%s-bind_poses", geomID);
            addSource(skin, inv_bind_source_id.c_str(), "ABCDEFGHIJKLMNOP", inv_bind_data);

            std::vector<F32> weight_data;
            std::vector<S32> vcount_data;
            std::vector<S32> v_data;
            vcount_data.reserve(rig_vertex_weights.size());
            for (const rig_weight_list_t& influences : rig_vertex_weights)
            {
                vcount_data.push_back((S32)influences.size());
                for (const RigWeightInfluence& influence : influences)
                {
                    const S32 weight_idx = (S32)weight_data.size();
                    weight_data.push_back(influence.mWeight);
                    v_data.push_back(influence.mJoint);
                    v_data.push_back(weight_idx);
                }
            }

            std::string weight_source_id = llformat("%s-weights", geomID);
            addSource(skin, weight_source_id.c_str(), "W", weight_data);

            daeElement* joints = skin->add("joints");
            daeElement* joints_input = joints->add("input");
            joints_input->setAttribute("semantic", "JOINT");
            joints_input->setAttribute("source", ("#" + joint_source_id).c_str());
            daeElement* bind_input = joints->add("input");
            bind_input->setAttribute("semantic", "INV_BIND_MATRIX");
            bind_input->setAttribute("source", ("#" + inv_bind_source_id).c_str());

            daeElement* vertex_weights = skin->add("vertex_weights");
            vertex_weights->setAttribute("count", llformat("%d", (S32)rig_vertex_weights.size()).c_str());
            daeElement* joint_input = vertex_weights->add("input");
            joint_input->setAttribute("semantic", "JOINT");
            joint_input->setAttribute("source", ("#" + joint_source_id).c_str());
            joint_input->setAttribute("offset", "0");
            daeElement* weight_input = vertex_weights->add("input");
            weight_input->setAttribute("semantic", "WEIGHT");
            weight_input->setAttribute("source", ("#" + weight_source_id).c_str());
            weight_input->setAttribute("offset", "1");
            std::string vcount_str = joinIntValues(vcount_data);
            vertex_weights->add("vcount")->setCharData(vcount_str.c_str());
            std::string v_str = joinIntValues(v_data);
            vertex_weights->add("v")->setCharData(v_str.c_str());

            addSkeletonNodes(scene, skeleton_root_id, skin_info, makeColladaId(name));
        }
        else
        {
            has_rigging = false;
        }

        daeElement* node = scene->add("node");
        node->setAttribute("type", "NODE");
        node->setAttribute("id", geomID);
        node->setAttribute("name", geomID);

        // Set tranform matrix (node position, rotation and scale)
        domMatrix* matrix = (domMatrix*)node->add("matrix");
        LLXform srt;
        srt.setScale(obj->getScale());
        srt.setPosition(obj->getRenderPosition() + mOffset);
        srt.setRotation(obj->getRenderRotation());
        LLMatrix4 m4;
        srt.getLocalMat4(m4);
        for (int i=0; i<4; i++)
            for (int j=0; j<4; j++)
                (matrix->getValue()).append(m4.mMatrix[j][i]);

        // Geometry/controller of the node
        daeElement* nodeGeometry = has_rigging ? node->add("instance_controller") : node->add("instance_geometry");
        if (has_rigging)
        {
            nodeGeometry->setAttribute("url", ("#" + controller_id).c_str());
            nodeGeometry->add("skeleton")->setCharData(("#" + skeleton_root_id).c_str());
        }
        else
        {
            nodeGeometry->setAttribute("url", llformat("#%s-%s", geomID, "mesh").c_str());
        }

        // Bind materials
        daeElement* tq = nodeGeometry->add("bind_material technique_common");
        for (S32 objMaterial = 0; objMaterial < objMaterials.size(); objMaterial++)
        {
            std::string matName = objMaterials[objMaterial].name;
            daeElement* instanceMaterial = tq->add("instance_material");
            instanceMaterial->setAttribute("symbol", (matName + "-material").c_str());
            instanceMaterial->setAttribute("target", ("#" + matName + "-material").c_str());
        }

    }

    // Effects (face texture, color, alpha)
    generateEffects(effects);

    // Materials
    for (S32 objMaterial = 0; objMaterial < mAllMaterials.size(); objMaterial++)
    {
        daeElement* mat = materials->add("material");
        mat->setAttribute("id", (mAllMaterials[objMaterial].name + "-material").c_str());
        daeElement* matEffect = mat->add("instance_effect");
        matEffect->setAttribute("url", ("#" + mAllMaterials[objMaterial].name + "-fx").c_str());
    }

    root->add("scene instance_visual_scene")->setAttribute("url", "#Scene");

    return dae.writeAll();
}

bool DAESaver::skipFace(LLTextureEntry *te)
{
    return (gSavedSettings.getBOOL("DAEExportSkipTransparent")
        && (te->getColor().mV[3] < 0.01f || te->getID() == DAEExportUtil::LL_TEXTURE_TRANSPARENT));
}

DAESaver::MaterialInfo DAESaver::getMaterial(LLTextureEntry* te)
{
    if (gSavedSettings.getBOOL("DAEExportConsolidateMaterials"))
    {
        for (S32 i=0; i < mAllMaterials.size(); i++)
        {
            if (mAllMaterials[i].matches(te))
            {
                return mAllMaterials[i];
            }
        }
    }

    MaterialInfo ret;
    ret.textureID = te->getID();
    ret.color = te->getColor();
    ret.name = llformat("Material%d", mAllMaterials.size());
    mAllMaterials.push_back(ret);
    return mAllMaterials[mAllMaterials.size() - 1];
}

void DAESaver::getMaterials(LLViewerObject* obj, material_list_t* ret)
{
    S32 num_faces = obj->getVolume()->getNumVolumeFaces();
    for (S32 face_num = 0; face_num < num_faces; ++face_num)
    {
        LLTextureEntry* te = obj->getTE(face_num);

        if (skipFace(te)) continue;

        MaterialInfo mat = getMaterial(te);
        if (!gSavedSettings.getBOOL("DAEExportConsolidateMaterials")
            || std::find(ret->begin(), ret->end(), mat) == ret->end())
        {
            ret->push_back(mat);
        }
    }
}

void DAESaver::getFacesWithMaterial(LLViewerObject* obj, MaterialInfo& mat, int_list_t* ret)
{
    S32 num_faces = obj->getVolume()->getNumVolumeFaces();
    for (S32 face_num = 0; face_num < num_faces; ++face_num)
    {
        if (mat == getMaterial(obj->getTE(face_num)))
        {
            ret->push_back(face_num);
        }
    }
}

void DAESaver::generateEffects(daeElement *effects)
{
    // Effects (face color, alpha)
    bool export_textures = gSavedSettings.getBOOL("DAEExportTextures");

    for (S32 mat = 0; mat < mAllMaterials.size(); mat++)
    {
        LLColor4 color = mAllMaterials[mat].color;
        domEffect* effect = (domEffect*)effects->add("effect");
        effect->setId((mAllMaterials[mat].name + "-fx").c_str());
        daeElement* profile = effect->add("profile_COMMON");
        std::string colladaName;

        if (export_textures)
        {
            LLUUID textID;
            S32 i = 0;
            for (; i < mTextures.size(); i++)
            {
                if (mAllMaterials[mat].textureID == mTextures[i])
                {
                    textID = mTextures[i];
                    break;
                }
            }

            if (!textID.isNull() && !mTextureNames[i].empty())
            {
                colladaName = mTextureNames[i] + "_" + mImageFormat;
                daeElement* newparam = profile->add("newparam");
                newparam->setAttribute("sid", (colladaName + "-surface").c_str());
                daeElement* surface = newparam->add("surface");
                surface->setAttribute("type", "2D");
                surface->add("init_from")->setCharData(colladaName.c_str());
                newparam = profile->add("newparam");
                newparam->setAttribute("sid", (colladaName + "-sampler").c_str());
                newparam->add("sampler2D source")->setCharData((colladaName + "-surface").c_str());
            }
        }

        daeElement* t = profile->add("technique");
        t->setAttribute("sid", "common");
        domElement* phong = t->add("phong");
        domElement* diffuse = phong->add("diffuse");
        // Only one <color> or <texture> can appear inside diffuse element
        if (!colladaName.empty())
        {
            daeElement* txtr = diffuse->add("texture");
            txtr->setAttribute("texture", (colladaName + "-sampler").c_str());
            txtr->setAttribute("texcoord", colladaName.c_str());
        }
        else
        {
            daeElement* diffuseColor = diffuse->add("color");
            diffuseColor->setAttribute("sid", "diffuse");
            diffuseColor->setCharData(llformat("%f %f %f %f", color.mV[0], color.mV[1], color.mV[2], color.mV[3]).c_str());
            phong->add("transparency float")->setCharData(llformat("%f", color.mV[3]).c_str());
        }
    }
}

void DAESaver::generateImagesSection(daeElement* images)
{
    for (S32 i=0; i < mTextureNames.size(); i++)
    {
        std::string name = mTextureNames[i];
        if (name.empty()) continue;
        std::string colladaName = name + "_" + mImageFormat;
        daeElement* image = images->add("image");
        image->setAttribute("id", colladaName.c_str());
        image->setAttribute("name", colladaName.c_str());
        image->add("init_from")->setCharData(LLURI::escape(name + "." + mImageFormat));
    }
}
