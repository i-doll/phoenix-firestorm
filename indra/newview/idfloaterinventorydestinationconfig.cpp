/**
 * @file idfloaterinventorydestinationconfig.cpp
 * @brief Configures labeled destinations for received inventory.
 *
 * Portions Copyright (c) 2017, Kitty Barnett
 *
 * $LicenseInfo:firstyear=2026&license=viewerlgpl$
 * Phoenix Firestorm Viewer Source Code
 * Copyright (C) 2026, The Phoenix Firestorm Project, Inc.
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation;
 * version 2.1 of the License only.
 * $/LicenseInfo$
 */

#include "llviewerprecompiledheaders.h"

#include "idfloaterinventorydestinationconfig.h"

#include "idfloaterinventoryfolderbrowse.h"
#include "idinventoryofferredirect.h"

#include "llbutton.h"
#include "llinventoryfunctions.h"
#include "llinventorymodel.h"
#include "lllineeditor.h"
#include "llscrolllistctrl.h"
#include "lltextbox.h"
#include "llviewercontrol.h"

IDFloaterInventoryDestinationConfig::IDFloaterInventoryDestinationConfig(const LLSD& key)
    : LLFloater(key)
{
}

IDFloaterInventoryDestinationConfig::~IDFloaterInventoryDestinationConfig()
{
    if (LLFloater* floater = mBrowseFloater.get())
    {
        floater->closeFloater();
    }
    mBrowseFloater.markDead();
}

bool IDFloaterInventoryDestinationConfig::postBuild()
{
    mDestinationList = findChild<LLScrollListCtrl>("destination_list");
    mLabelEditor = findChild<LLLineEditor>("label_editor");
    mSaveLabelButton = findChild<LLButton>("save_label_button");
    mRemoveButton = findChild<LLButton>("remove_button");
    mValidationText = findChild<LLTextBox>("validation_text");
    if (!mDestinationList || !mLabelEditor || !mSaveLabelButton ||
        !mRemoveButton || !mValidationText)
    {
        return false;
    }

    mDestinationList->setCommitOnSelectionChange(true);
    mDestinationList->setCommitCallback(
        [this](LLUICtrl*, const LLSD&) { onSelectionChanged(); });
    mLabelEditor->setKeystrokeCallback(
        [this](LLLineEditor*, void*) { onLabelKeystroke(); }, nullptr);
    mLabelEditor->setCommitCallback(
        [this](LLUICtrl*, const LLSD&) { onSaveLabel(); });

    findChild<LLButton>("add_button")->setCommitCallback(
        [this](LLUICtrl*, const LLSD&) { onAddDestination(); });
    mRemoveButton->setCommitCallback(
        [this](LLUICtrl*, const LLSD&) { onRemoveDestination(); });
    mSaveLabelButton->setCommitCallback(
        [this](LLUICtrl*, const LLSD&) { onSaveLabel(); });
    findChild<LLButton>("close_button")->setCommitCallback(
        [this](LLUICtrl*, const LLSD&) { onCloseClicked(); });

    refreshList();
    return true;
}

void IDFloaterInventoryDestinationConfig::onOpen(const LLSD& key)
{
    refreshList();
}

void IDFloaterInventoryDestinationConfig::onAddDestination()
{
    if (!mBrowseFloater.isDead())
    {
        return;
    }

    IDFloaterInventoryFolderBrowse* floater = new IDFloaterInventoryFolderBrowse;
    floater->setCommitCallback(
        [this](LLUICtrl*, const LLSD& data) { onBrowseResult(data); });
    floater->openFloater();
    mBrowseFloater = floater->getHandle();
}

void IDFloaterInventoryDestinationConfig::onBrowseResult(const LLSD& data)
{
    const LLUUID folder_id = data["uuid"].asUUID();
    if (!IDInventoryOfferRedirect::isValidOfferDestination(folder_id))
    {
        return;
    }

    LLSD destinations = gSavedPerAccountSettings.getLLSD("IDInventoryAcceptInFolders");
    if (!destinations.isArray())
    {
        destinations = LLSD::emptyArray();
    }

    for (LLSD::array_const_iterator it = destinations.beginArray();
         it != destinations.endArray(); ++it)
    {
        if ((*it)["id"].asUUID() == folder_id)
        {
            refreshList(folder_id);
            return;
        }
    }

    std::string label = data["name"].asString();
    if (label.empty())
    {
        if (const LLInventoryCategory* category = gInventory.getCategory(folder_id))
        {
            label = category->getName();
        }
    }
    const std::string base_label = label;
    for (S32 suffix = 2; !isLabelAvailable(label, LLUUID::null); ++suffix)
    {
        label = llformat("%s (%d)", base_label.c_str(), suffix);
    }
    destinations.append(LLSD().with("id", folder_id).with("name", label));
    persist(destinations);
    refreshList(folder_id);
}

void IDFloaterInventoryDestinationConfig::onCloseClicked()
{
    closeFloater();
}

void IDFloaterInventoryDestinationConfig::onLabelKeystroke()
{
    refreshControls();
}

void IDFloaterInventoryDestinationConfig::onRemoveDestination()
{
    const LLUUID selected_id = mDestinationList->getSelectedValue().asUUID();
    if (selected_id.isNull())
    {
        return;
    }

    const LLSD destinations = gSavedPerAccountSettings.getLLSD("IDInventoryAcceptInFolders");
    LLSD updated = LLSD::emptyArray();
    if (destinations.isArray())
    {
        for (LLSD::array_const_iterator it = destinations.beginArray();
             it != destinations.endArray(); ++it)
        {
            if ((*it)["id"].asUUID() != selected_id)
            {
                updated.append(*it);
            }
        }
    }
    persist(updated);
    refreshList();
}

void IDFloaterInventoryDestinationConfig::onSaveLabel()
{
    const LLUUID selected_id = mDestinationList->getSelectedValue().asUUID();
    std::string label = mLabelEditor->getText();
    LLStringUtil::trim(label);
    if (selected_id.isNull() || label.empty() || !isLabelAvailable(label, selected_id))
    {
        refreshControls();
        return;
    }

    const LLSD destinations = gSavedPerAccountSettings.getLLSD("IDInventoryAcceptInFolders");
    LLSD updated = LLSD::emptyArray();
    if (destinations.isArray())
    {
        for (LLSD::array_const_iterator it = destinations.beginArray();
             it != destinations.endArray(); ++it)
        {
            LLSD entry = *it;
            if (entry["id"].asUUID() == selected_id)
            {
                entry["name"] = label;
            }
            updated.append(entry);
        }
    }
    persist(updated);
    refreshList(selected_id);
}

void IDFloaterInventoryDestinationConfig::onSelectionChanged()
{
    const LLUUID selected_id = mDestinationList->getSelectedValue().asUUID();
    std::string label;
    const LLSD destinations = gSavedPerAccountSettings.getLLSD("IDInventoryAcceptInFolders");
    if (destinations.isArray())
    {
        for (LLSD::array_const_iterator it = destinations.beginArray();
             it != destinations.endArray(); ++it)
        {
            if ((*it)["id"].asUUID() == selected_id)
            {
                label = (*it)["name"].asString();
                break;
            }
        }
    }
    mLabelEditor->setText(label);
    refreshControls();
}

bool IDFloaterInventoryDestinationConfig::isLabelAvailable(
    const std::string& label, const LLUUID& except_id) const
{
    const LLSD destinations = gSavedPerAccountSettings.getLLSD("IDInventoryAcceptInFolders");
    if (!destinations.isArray())
    {
        return true;
    }

    for (LLSD::array_const_iterator it = destinations.beginArray();
         it != destinations.endArray(); ++it)
    {
        if ((*it)["id"].asUUID() != except_id &&
            LLStringUtil::compareInsensitive((*it)["name"].asString(), label) == 0)
        {
            return false;
        }
    }
    return true;
}

void IDFloaterInventoryDestinationConfig::persist(const LLSD& destinations)
{
    gSavedPerAccountSettings.setLLSD("IDInventoryAcceptInFolders", destinations);
    LLFloater::onCommit();
}

void IDFloaterInventoryDestinationConfig::refreshControls()
{
    const LLUUID selected_id = mDestinationList->getSelectedValue().asUUID();
    std::string label = mLabelEditor->getText();
    LLStringUtil::trim(label);
    const bool has_selection = selected_id.notNull();
    const bool duplicate = has_selection && !label.empty() &&
                           !isLabelAvailable(label, selected_id);

    mLabelEditor->setEnabled(has_selection);
    mSaveLabelButton->setEnabled(has_selection && !label.empty() && !duplicate);
    mRemoveButton->setEnabled(has_selection);
    mValidationText->setVisible(has_selection && (label.empty() || duplicate));
    if (label.empty())
    {
        mValidationText->setText(getString("label_required"));
    }
    else if (duplicate)
    {
        mValidationText->setText(getString("label_duplicate"));
    }
}

void IDFloaterInventoryDestinationConfig::refreshList(const LLUUID& selected_id)
{
    if (!mDestinationList)
    {
        return;
    }

    mDestinationList->clearRows();
    const LLSD destinations = gSavedPerAccountSettings.getLLSD("IDInventoryAcceptInFolders");
    if (destinations.isArray())
    {
        for (LLSD::array_const_iterator it = destinations.beginArray();
             it != destinations.endArray(); ++it)
        {
            const LLUUID folder_id = (*it)["id"].asUUID();
            const LLInventoryCategory* category = gInventory.getCategory(folder_id);
            if (!category || !IDInventoryOfferRedirect::isValidOfferDestination(folder_id))
            {
                continue;
            }

            std::string label = (*it)["name"].asString();
            if (label.empty())
            {
                label = category->getName();
            }
            const std::string path = make_inventory_path(folder_id);

            LLSD row;
            row["value"] = folder_id;
            row["columns"][0]["column"] = "label";
            row["columns"][0]["value"] = label;
            row["columns"][1]["column"] = "folder";
            row["columns"][1]["value"] = path;
            mDestinationList->addElement(row);
        }
    }

    if (selected_id.notNull())
    {
        mDestinationList->selectByValue(selected_id);
    }
    onSelectionChanged();
}
