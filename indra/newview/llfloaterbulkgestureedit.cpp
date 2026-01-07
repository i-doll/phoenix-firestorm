/**
 * @file llfloaterbulkgestureedit.cpp
 * @brief Bulk editing of multiple gestures' shared properties.
 *
 * $LicenseInfo:firstyear=2024&license=viewerlgpl$
 * Second Life Viewer Source Code
 * Copyright (C) 2024, Linden Research, Inc.
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation;
 * version 2.1 of the License only.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301  USA
 *
 * Linden Research, Inc., 945 Battery Street, San Francisco, CA  94111  USA
 * $/LicenseInfo$
 */

// <ID> Bulk gesture editing feature

#include "llviewerprecompiledheaders.h"
#include "llfloaterbulkgestureedit.h"

#include "llagent.h"
#include "llbutton.h"
#include "llcombobox.h"
#include "lldatapacker.h"
#include "llfilesystem.h"
#include "llfloaterreg.h"
#include "llgesturemgr.h"
#include "llinventory.h"
#include "llinventorymodel.h"
#include "llkeyboard.h"
#include "lllineeditor.h"
#include "llmultigesture.h"
#include "llnotificationsutil.h"
#include "lltextbox.h"
#include "lltrans.h"
#include "llviewerassetupload.h"
#include "llviewerregion.h"

const std::string LLFloaterBulkGestureEdit::MIXED_LABEL = "-- Mixed --";

// Helper struct for async load callbacks
struct LLBulkGestureLoadData
{
    LLUUID mItemID;
    LLHandle<LLFloater> mFloaterHandle;
};

LLFloaterBulkGestureEdit::LLFloaterBulkGestureEdit(const LLSD& key)
    : LLFloater(key)
    , mTriggerEditor(nullptr)
    , mReplaceEditor(nullptr)
    , mModifierCombo(nullptr)
    , mKeyCombo(nullptr)
    , mSaveBtn(nullptr)
    , mCancelBtn(nullptr)
    , mStatusText(nullptr)
    , mPendingLoads(0)
    , mTriggerModified(false)
    , mReplaceModified(false)
    , mModifierModified(false)
    , mKeyModified(false)
    , mTriggerMixed(false)
    , mReplaceMixed(false)
    , mModifierMixed(false)
    , mKeyMixed(false)
    , mSaveIndex(0)
{
}

LLFloaterBulkGestureEdit::~LLFloaterBulkGestureEdit()
{
    // Clean up loaded gestures
    for (auto& pair : mLoadedGestures)
    {
        delete pair.second;
    }
    mLoadedGestures.clear();
}

bool LLFloaterBulkGestureEdit::postBuild()
{
    mTriggerEditor = getChild<LLLineEditor>("trigger_editor");
    mReplaceEditor = getChild<LLLineEditor>("replace_editor");
    mModifierCombo = getChild<LLComboBox>("modifier_combo");
    mKeyCombo = getChild<LLComboBox>("key_combo");
    mSaveBtn = getChild<LLButton>("save_btn");
    mCancelBtn = getChild<LLButton>("cancel_btn");
    mStatusText = getChild<LLTextBox>("status_text");

    mTriggerEditor->setKeystrokeCallback([this](LLLineEditor*, void*) { onTriggerChanged(); }, nullptr);
    mReplaceEditor->setKeystrokeCallback([this](LLLineEditor*, void*) { onReplaceChanged(); }, nullptr);
    mModifierCombo->setCommitCallback([this](LLUICtrl*, const LLSD&) { onModifierChanged(); });
    mKeyCombo->setCommitCallback([this](LLUICtrl*, const LLSD&) { onKeyChanged(); });

    mSaveBtn->setClickedCallback([this](void*) { onClickSave(); }, nullptr);
    mCancelBtn->setClickedCallback([this](void*) { onClickCancel(); }, nullptr);

    addModifiers();
    addKeys();

    // Disable save until we've loaded everything
    mSaveBtn->setEnabled(false);

    return true;
}

// static
void LLFloaterBulkGestureEdit::show(const uuid_vec_t& gesture_ids)
{
    if (gesture_ids.size() < 2)
    {
        LL_WARNS() << "Bulk gesture edit requires at least 2 gestures" << LL_ENDL;
        return;
    }

    LLFloaterBulkGestureEdit* floater = LLFloaterReg::showTypedInstance<LLFloaterBulkGestureEdit>("bulk_gesture_edit");
    if (floater)
    {
        floater->mGestureIDs = gesture_ids;
        floater->initFromGestures();
    }
}

void LLFloaterBulkGestureEdit::initFromGestures()
{
    // Clean up any previous state
    for (auto& pair : mLoadedGestures)
    {
        delete pair.second;
    }
    mLoadedGestures.clear();

    mPendingLoads = static_cast<S32>(mGestureIDs.size());

    // Update status
    std::string status = llformat("Loading %d gestures...", mPendingLoads);
    mStatusText->setText(status);

    // Update title
    std::string title = llformat("Bulk Edit %d Gestures", static_cast<int>(mGestureIDs.size()));
    setTitle(title);

    // Start loading all gesture assets
    for (const LLUUID& item_id : mGestureIDs)
    {
        loadGestureAsset(item_id);
    }
}

void LLFloaterBulkGestureEdit::loadGestureAsset(const LLUUID& item_id)
{
    LLViewerInventoryItem* item = gInventory.getItem(item_id);
    if (!item)
    {
        LL_WARNS() << "Could not find gesture item " << item_id << LL_ENDL;
        mPendingLoads--;
        if (mPendingLoads <= 0)
        {
            onAllGesturesLoaded();
        }
        return;
    }

    LLUUID asset_id = item->getAssetUUID();
    if (asset_id.isNull())
    {
        // Freshly created gesture with no asset yet
        LLMultiGesture* gesture = new LLMultiGesture();
        mLoadedGestures[item_id] = gesture;
        mPendingLoads--;
        if (mPendingLoads <= 0)
        {
            onAllGesturesLoaded();
        }
        return;
    }

    LLBulkGestureLoadData* data = new LLBulkGestureLoadData();
    data->mItemID = item_id;
    data->mFloaterHandle = getHandle();

    gAssetStorage->getAssetData(asset_id,
                                LLAssetType::AT_GESTURE,
                                onLoadComplete,
                                (void*)data,
                                true); // high priority
}

// static
void LLFloaterBulkGestureEdit::onLoadComplete(const LLUUID& asset_uuid,
                                              LLAssetType::EType type,
                                              void* user_data, S32 status, LLExtStat ext_status)
{
    LLBulkGestureLoadData* data = (LLBulkGestureLoadData*)user_data;
    if (!data)
    {
        return;
    }

    LLFloaterBulkGestureEdit* self = dynamic_cast<LLFloaterBulkGestureEdit*>(data->mFloaterHandle.get());
    if (!self)
    {
        delete data;
        return;
    }

    if (status == 0)
    {
        LLFileSystem file(asset_uuid, type, LLFileSystem::READ);
        S32 size = file.getSize();

        std::vector<char> buffer(size + 1);
        file.read((U8*)&buffer[0], size);
        buffer[size] = '\0';

        LLMultiGesture* gesture = new LLMultiGesture();
        LLDataPackerAsciiBuffer dp(&buffer[0], size + 1);
        bool ok = gesture->deserialize(dp);

        if (ok)
        {
            self->mLoadedGestures[data->mItemID] = gesture;
        }
        else
        {
            LL_WARNS() << "Failed to deserialize gesture " << data->mItemID << LL_ENDL;
            delete gesture;
        }
    }
    else
    {
        LL_WARNS() << "Failed to load gesture asset " << asset_uuid << " status " << status << LL_ENDL;
    }

    self->mPendingLoads--;
    if (self->mPendingLoads <= 0)
    {
        self->onAllGesturesLoaded();
    }

    delete data;
}

void LLFloaterBulkGestureEdit::onAllGesturesLoaded()
{
    if (mLoadedGestures.empty())
    {
        mStatusText->setText(std::string("Failed to load gestures."));
        return;
    }

    computeSharedValues();

    std::string status = llformat("Editing %d gestures", static_cast<int>(mLoadedGestures.size()));
    mStatusText->setText(status);
    mSaveBtn->setEnabled(true);
}

void LLFloaterBulkGestureEdit::computeSharedValues()
{
    if (mLoadedGestures.empty())
    {
        return;
    }

    // Get first gesture as reference
    auto it = mLoadedGestures.begin();
    LLMultiGesture* first = it->second;

    std::string firstTrigger = first->mTrigger;
    std::string firstReplace = first->mReplaceText;
    MASK firstMask = first->mMask;
    KEY firstKey = first->mKey;

    mTriggerMixed = false;
    mReplaceMixed = false;
    mModifierMixed = false;
    mKeyMixed = false;

    // Compare with all other gestures
    ++it;
    for (; it != mLoadedGestures.end(); ++it)
    {
        LLMultiGesture* gesture = it->second;

        if (gesture->mTrigger != firstTrigger)
        {
            mTriggerMixed = true;
        }
        if (gesture->mReplaceText != firstReplace)
        {
            mReplaceMixed = true;
        }
        if (gesture->mMask != firstMask)
        {
            mModifierMixed = true;
        }
        if (gesture->mKey != firstKey)
        {
            mKeyMixed = true;
        }
    }

    // Set UI values
    if (mTriggerMixed)
    {
        mTriggerEditor->setText(std::string());
        mTriggerEditor->setLabel(MIXED_LABEL);
    }
    else
    {
        mTriggerEditor->setText(firstTrigger);
        mTriggerEditor->setLabel(std::string());
    }

    if (mReplaceMixed)
    {
        mReplaceEditor->setText(std::string());
        mReplaceEditor->setLabel(MIXED_LABEL);
    }
    else
    {
        mReplaceEditor->setText(firstReplace);
        mReplaceEditor->setLabel(std::string());
    }

    // Set modifier combo
    if (mModifierMixed)
    {
        // Add Mixed option if not present
        if (!mModifierCombo->itemExists(MIXED_LABEL))
        {
            mModifierCombo->add(MIXED_LABEL, ADD_TOP);
        }
        mModifierCombo->setSimple(MIXED_LABEL);
    }
    else
    {
        std::string modLabel = LLTrans::getString("---");
        if (firstMask == MASK_SHIFT)
        {
            modLabel = LLTrans::getString("KBShift");
        }
        else if (firstMask == MASK_CONTROL)
        {
            modLabel = LLTrans::getString("KBCtrl");
        }
        mModifierCombo->setSimple(modLabel);
    }

    // Set key combo
    if (mKeyMixed)
    {
        if (!mKeyCombo->itemExists(MIXED_LABEL))
        {
            mKeyCombo->add(MIXED_LABEL, ADD_TOP);
        }
        mKeyCombo->setSimple(MIXED_LABEL);
    }
    else
    {
        if (firstKey == KEY_NONE)
        {
            mKeyCombo->setSimple(LLTrans::getString("---"));
        }
        else
        {
            mKeyCombo->setSimple(LLKeyboard::stringFromKey(firstKey));
        }
    }

    // Store original values for change detection
    mOriginalTrigger = mTriggerMixed ? "" : firstTrigger;
    mOriginalReplace = mReplaceMixed ? "" : firstReplace;
    mOriginalModifier = mModifierCombo->getSimple();
    mOriginalKey = mKeyCombo->getSimple();

    // Reset modification flags
    mTriggerModified = false;
    mReplaceModified = false;
    mModifierModified = false;
    mKeyModified = false;
}

void LLFloaterBulkGestureEdit::addModifiers()
{
    mModifierCombo->removeall();
    mModifierCombo->add(LLTrans::getString("---"));
    mModifierCombo->add(LLTrans::getString("KBShift"));
    mModifierCombo->add(LLTrans::getString("KBCtrl"));
    mModifierCombo->setCurrentByIndex(0);
}

void LLFloaterBulkGestureEdit::addKeys()
{
    mKeyCombo->removeall();
    mKeyCombo->add(LLTrans::getString("---"));

    for (KEY key = ' '; key < KEY_NONE; key++)
    {
        char buffer[] = {(char)key, '\0'};
        std::string str_org(buffer);
        std::string str_translated = LLKeyboard::stringFromKey(key);

        if (str_org == str_translated)
        {
            if (key >= ' ' && key <= '~')
            {
                mKeyCombo->add(str_translated, ADD_BOTTOM);
            }
        }
        else
        {
            mKeyCombo->add(str_translated, ADD_BOTTOM);
        }
    }
    mKeyCombo->setCurrentByIndex(0);
}

void LLFloaterBulkGestureEdit::onTriggerChanged()
{
    std::string current = mTriggerEditor->getText();
    // Mark as modified if value changed from original (or if originally mixed and now has content)
    mTriggerModified = (mTriggerMixed && !current.empty()) || (!mTriggerMixed && current != mOriginalTrigger);
}

void LLFloaterBulkGestureEdit::onReplaceChanged()
{
    std::string current = mReplaceEditor->getText();
    mReplaceModified = (mReplaceMixed && !current.empty()) || (!mReplaceMixed && current != mOriginalReplace);
}

void LLFloaterBulkGestureEdit::onModifierChanged()
{
    std::string current = mModifierCombo->getSimple();
    mModifierModified = (current != MIXED_LABEL) && (current != mOriginalModifier);
}

void LLFloaterBulkGestureEdit::onKeyChanged()
{
    std::string current = mKeyCombo->getSimple();
    mKeyModified = (current != MIXED_LABEL) && (current != mOriginalKey);
}

void LLFloaterBulkGestureEdit::onClickSave()
{
    // Check if anything was modified
    if (!mTriggerModified && !mReplaceModified && !mModifierModified && !mKeyModified)
    {
        LLNotificationsUtil::add("GestureBulkEditNoChanges");
        return;
    }

    // Prepare list of gestures to save
    mGesturesToSave.clear();
    for (const auto& pair : mLoadedGestures)
    {
        mGesturesToSave.push_back(pair.first);
    }
    mSaveIndex = 0;

    mSaveBtn->setEnabled(false);
    mStatusText->setText(std::string("Saving..."));

    saveGestures();
}

void LLFloaterBulkGestureEdit::saveGestures()
{
    saveNextGesture();
}

void LLFloaterBulkGestureEdit::saveNextGesture()
{
    if (mSaveIndex >= static_cast<S32>(mGesturesToSave.size()))
    {
        // All done
        mStatusText->setText(std::string("All gestures saved successfully!"));
        LLGestureMgr::instance().notifyObservers();
        gInventory.notifyObservers();

        // Close after a brief delay to show the success message
        closeFloater();
        return;
    }

    LLUUID item_id = mGesturesToSave[mSaveIndex];
    auto it = mLoadedGestures.find(item_id);
    if (it == mLoadedGestures.end())
    {
        mSaveIndex++;
        saveNextGesture();
        return;
    }

    LLMultiGesture* gesture = it->second;

    // Apply modifications
    if (mTriggerModified)
    {
        gesture->mTrigger = mTriggerEditor->getText();
    }
    if (mReplaceModified)
    {
        gesture->mReplaceText = mReplaceEditor->getText();
    }
    if (mModifierModified)
    {
        std::string modStr = mModifierCombo->getSimple();
        if (modStr == LLTrans::getString("KBShift"))
        {
            gesture->mMask = MASK_SHIFT;
        }
        else if (modStr == LLTrans::getString("KBCtrl"))
        {
            gesture->mMask = MASK_CONTROL;
        }
        else
        {
            gesture->mMask = MASK_NONE;
        }
    }
    if (mKeyModified)
    {
        std::string keyStr = mKeyCombo->getSimple();
        if (keyStr == LLTrans::getString("---"))
        {
            gesture->mKey = KEY_NONE;
        }
        else
        {
            LLKeyboard::keyFromString(keyStr, &gesture->mKey);
        }
    }

    // Serialize the gesture
    S32 maxSize = gesture->getMaxSerialSize();
    char* buffer = new char[maxSize];
    LLDataPackerAsciiBuffer dp(buffer, maxSize);
    bool ok = gesture->serialize(dp);

    if (!ok || dp.getCurrentSize() > 1000)
    {
        LL_WARNS() << "Failed to serialize gesture " << item_id << LL_ENDL;
        delete[] buffer;
        mSaveIndex++;
        saveNextGesture();
        return;
    }

    LLViewerInventoryItem* item = gInventory.getItem(item_id);
    if (!item)
    {
        LL_WARNS() << "Could not find item " << item_id << LL_ENDL;
        delete[] buffer;
        mSaveIndex++;
        saveNextGesture();
        return;
    }

    // Check permissions
    if (!item->getPermissions().allowModifyBy(gAgent.getID()))
    {
        LL_WARNS() << "No modify permission for gesture " << item_id << LL_ENDL;
        delete[] buffer;
        mSaveIndex++;
        saveNextGesture();
        return;
    }

    const LLViewerRegion* region = gAgent.getRegion();
    if (!region)
    {
        LL_WARNS() << "Not connected to a region" << LL_ENDL;
        delete[] buffer;
        mSaveIndex++;
        saveNextGesture();
        return;
    }

    std::string agent_url = region->getCapability("UpdateGestureAgentInventory");

    if (!agent_url.empty())
    {
        // Capture index for the lambda
        S32 saveIndex = mSaveIndex;
        LLHandle<LLFloater> floaterHandle = getHandle();

        auto uploadInfo = std::make_shared<LLBufferedAssetUploadInfo>(
            item_id,
            LLAssetType::AT_GESTURE,
            buffer,
            [floaterHandle, saveIndex](LLUUID itemId, LLUUID newAssetId, LLUUID, LLSD)
            {
                // Update gesture manager if active
                if (LLGestureMgr::instance().isGestureActive(itemId))
                {
                    LLGestureMgr::instance().replaceGesture(itemId, newAssetId);
                }

                // Continue to next gesture
                LLFloaterBulkGestureEdit* floater = dynamic_cast<LLFloaterBulkGestureEdit*>(floaterHandle.get());
                if (floater)
                {
                    floater->mSaveIndex++;
                    std::string status = llformat("Saved %d of %d...",
                                                  floater->mSaveIndex,
                                                  static_cast<int>(floater->mGesturesToSave.size()));
                    floater->mStatusText->setText(status);
                    floater->saveNextGesture();
                }
            },
            nullptr);

        LLViewerAssetUpload::EnqueueInventoryUpload(agent_url, uploadInfo);
    }
    else
    {
        // Fallback to old asset storage method
        LL_WARNS() << "No UpdateGestureAgentInventory capability, skipping gesture " << item_id << LL_ENDL;
        delete[] buffer;
        mSaveIndex++;
        saveNextGesture();
    }
}

void LLFloaterBulkGestureEdit::onClickCancel()
{
    closeFloater();
}

