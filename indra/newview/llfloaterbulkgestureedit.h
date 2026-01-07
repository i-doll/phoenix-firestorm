/**
 * @file llfloaterbulkgestureedit.h
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

#ifndef LL_LLFLOATERBULKGESTUREEDIT_H
#define LL_LLFLOATERBULKGESTUREEDIT_H

#include "llfloater.h"
#include "lluuid.h"

class LLLineEditor;
class LLComboBox;
class LLButton;
class LLTextBox;
class LLMultiGesture;

class LLFloaterBulkGestureEdit : public LLFloater
{
    LOG_CLASS(LLFloaterBulkGestureEdit);
public:
    LLFloaterBulkGestureEdit(const LLSD& key);
    virtual ~LLFloaterBulkGestureEdit();

    /*virtual*/ bool postBuild() override;

    // Show the floater for editing the given gesture UUIDs
    static void show(const uuid_vec_t& gesture_ids);

protected:
    // Initialize UI from the selected gestures
    void initFromGestures();

    // Load gesture asset data asynchronously
    void loadGestureAsset(const LLUUID& item_id);
    static void onLoadComplete(const LLUUID& asset_uuid,
                               LLAssetType::EType type,
                               void* user_data, S32 status, LLExtStat ext_status);

    // Called when all gestures have been loaded
    void onAllGesturesLoaded();

    // Compute shared values or "Mixed" for each property
    void computeSharedValues();

    // Populate modifier and key combos
    void addModifiers();
    void addKeys();

    // UI callbacks
    void onTriggerChanged();
    void onReplaceChanged();
    void onModifierChanged();
    void onKeyChanged();
    void onClickSave();
    void onClickCancel();

    // Save modified properties to all gestures
    void saveGestures();
    void saveNextGesture();

    // Static callback for gesture save completion
    static void finishGestureUpload(LLUUID itemId, LLUUID newAssetId);

private:
    // The gesture item UUIDs being edited
    uuid_vec_t mGestureIDs;

    // Loaded gesture data (item_id -> gesture data)
    std::map<LLUUID, LLMultiGesture*> mLoadedGestures;
    S32 mPendingLoads;

    // UI controls
    LLLineEditor*   mTriggerEditor;
    LLLineEditor*   mReplaceEditor;
    LLComboBox*     mModifierCombo;
    LLComboBox*     mKeyCombo;
    LLButton*       mSaveBtn;
    LLButton*       mCancelBtn;
    LLTextBox*      mStatusText;

    // Original values (for detecting changes)
    std::string mOriginalTrigger;
    std::string mOriginalReplace;
    std::string mOriginalModifier;
    std::string mOriginalKey;

    // Whether each property has been modified by the user
    bool mTriggerModified;
    bool mReplaceModified;
    bool mModifierModified;
    bool mKeyModified;

    // Whether values are mixed across gestures
    bool mTriggerMixed;
    bool mReplaceMixed;
    bool mModifierMixed;
    bool mKeyMixed;

    // For sequential saving
    uuid_vec_t mGesturesToSave;
    S32 mSaveIndex;

    // Mixed value placeholder
    static const std::string MIXED_LABEL;
};

#endif // LL_LLFLOATERBULKGESTUREEDIT_H

