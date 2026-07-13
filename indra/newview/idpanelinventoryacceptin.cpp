/**
 * @file idpanelinventoryacceptin.cpp
 * @brief Catznip-style destination controls for received inventory.
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

#include "idpanelinventoryacceptin.h"

#include "idfloaterinventoryfolderbrowse.h"
#include "idinventoryofferredirect.h"

#include "llbutton.h"
#include "llcheckboxctrl.h"
#include "llcombobox.h"
#include "llfloater.h"
#include "llfloaterreg.h"
#include "llinventorymodel.h"
#include "llviewercontrol.h"
#include "llviewerfoldertype.h"

#include <algorithm>
#include <vector>

namespace
{
    LLPanelInjector<IDPanelInventoryAcceptIn> sInventoryAcceptInPanel("id_panel_inventory_accept_in");
}

IDPanelInventoryAcceptIn::IDPanelInventoryAcceptIn()
    : LLPanel()
{
    setXMLFilename("panel_id_inventory_accept_in.xml");
}

IDPanelInventoryAcceptIn::~IDPanelInventoryAcceptIn()
{
    if (LLFloater* floater = mBrowseFloater.get())
    {
        floater->closeFloater();
    }
    mBrowseFloater.markDead();
    mDestinationsChangedConnection.disconnect();
}

bool IDPanelInventoryAcceptIn::postBuild()
{
    mAcceptInCheck = findChild<LLCheckBoxCtrl>("accept_in_check");
    mFolderList = findChild<LLComboBox>("folder_list");
    mBrowseButton = findChild<LLButton>("browse_button");
    mConfigureButton = findChild<LLButton>("configure_button");
    if (!mAcceptInCheck || !mFolderList || !mBrowseButton || !mConfigureButton)
    {
        return false;
    }

    mAcceptInCheck->setCommitCallback(
        [this](LLUICtrl*, const LLSD&) { onAcceptInChanged(); });
    mFolderList->setCommitCallback(
        [this](LLUICtrl*, const LLSD&) { onFolderChanged(); });
    mBrowseButton->setCommitCallback(
        [this](LLUICtrl*, const LLSD&) { onBrowse(); });
    mConfigureButton->setCommitCallback(
        [this](LLUICtrl*, const LLSD&) { onConfigure(); });

    mDestinationsChangedConnection =
        gSavedPerAccountSettings.getControl("IDInventoryAcceptInFolders")
            ->getSignal()->connect(
                boost::bind(&IDPanelInventoryAcceptIn::refreshFolders, this));

    refresh();
    return true;
}

bool IDPanelInventoryAcceptIn::getAcceptIn() const
{
    return mAcceptInCheck && mAcceptInCheck->get();
}

LLUUID IDPanelInventoryAcceptIn::getSelectedFolder() const
{
    if (!getAcceptIn() || !mFolderList)
    {
        return LLUUID::null;
    }

    const LLUUID folder_id = mFolderList->getValue().asUUID();
    return IDInventoryOfferRedirect::isValidOfferDestination(folder_id)
        ? folder_id
        : LLUUID::null;
}

void IDPanelInventoryAcceptIn::refresh()
{
    if (!mAcceptInCheck)
    {
        return;
    }

    const LLUUID temporary = IDInventoryOfferRedirect::destination();
    mAcceptInCheck->set(temporary.notNull() ||
                        gSavedPerAccountSettings.getBOOL("IDInventoryAcceptInEnabled"));
    refreshFolders();
    refreshControls();
}

void IDPanelInventoryAcceptIn::setObjectFolder(const LLUUID& folder_id)
{
    mObjectFolder = IDInventoryOfferRedirect::isValidOfferDestination(folder_id)
        ? folder_id
        : LLUUID::null;
    refreshFolders();
}

void IDPanelInventoryAcceptIn::onAcceptInChanged()
{
    gSavedPerAccountSettings.setBOOL("IDInventoryAcceptInEnabled", mAcceptInCheck->get());
    refreshControls();
}

void IDPanelInventoryAcceptIn::onBrowse()
{
    if (!mBrowseFloater.isDead())
    {
        return;
    }

    IDFloaterInventoryFolderBrowse* floater = new IDFloaterInventoryFolderBrowse;
    floater->setCommitCallback(
        [this](LLUICtrl*, const LLSD& data) { onBrowseResult(data); });
    floater->openFloater(LLSD().with("folder_id", getSelectedFolder()));
    mBrowseFloater = floater->getHandle();
}

void IDPanelInventoryAcceptIn::onBrowseResult(const LLSD& data)
{
    const LLUUID folder_id = data["uuid"].asUUID();
    const std::string folder_name = data["name"].asString();
    if (!IDInventoryOfferRedirect::isValidOfferDestination(folder_id))
    {
        return;
    }

    LLSD saved = gSavedPerAccountSettings.getLLSD("IDInventoryAcceptInFolders");
    std::string saved_label = folder_name;
    if (saved.isArray())
    {
        for (LLSD::array_const_iterator it = saved.beginArray(); it != saved.endArray(); ++it)
        {
            if ((*it)["id"].asUUID() == folder_id && !(*it)["name"].asString().empty())
            {
                saved_label = (*it)["name"].asString();
                break;
            }
        }
    }
    if (saved_label == folder_name && saved.isArray())
    {
        const std::string base_label = saved_label;
        for (S32 suffix = 2;; ++suffix)
        {
            bool duplicate = false;
            for (LLSD::array_const_iterator it = saved.beginArray(); it != saved.endArray(); ++it)
            {
                if ((*it)["id"].asUUID() != folder_id &&
                    LLStringUtil::compareInsensitive((*it)["name"].asString(), saved_label) == 0)
                {
                    duplicate = true;
                    break;
                }
            }
            if (!duplicate)
            {
                break;
            }
            saved_label = llformat("%s (%d)", base_label.c_str(), suffix);
        }
    }
    LLSD updated = LLSD::emptyArray();
    updated.append(LLSD().with("id", folder_id).with("name", saved_label));
    if (saved.isArray())
    {
        for (LLSD::array_const_iterator it = saved.beginArray();
             it != saved.endArray();
             ++it)
        {
            if ((*it)["id"].asUUID() != folder_id)
            {
                updated.append(*it);
            }
        }
    }
    gSavedPerAccountSettings.setLLSD("IDInventoryAcceptInFolders", updated);
    gSavedPerAccountSettings.setString("IDInventoryAcceptInFolder", folder_id.asString());

    refreshFolders();
    mFolderList->selectByValue(folder_id);
}

void IDPanelInventoryAcceptIn::onConfigure()
{
    LLFloaterReg::showInstance("id_inventory_destinations");
}

void IDPanelInventoryAcceptIn::onFolderChanged()
{
    const LLUUID folder_id = mFolderList->getValue().asUUID();
    gSavedPerAccountSettings.setString("IDInventoryAcceptInFolder", folder_id.asString());
}

void IDPanelInventoryAcceptIn::refreshControls()
{
    const bool enabled = mAcceptInCheck && mAcceptInCheck->get();
    if (mFolderList)
    {
        mFolderList->setEnabled(enabled);
    }
    if (mBrowseButton)
    {
        mBrowseButton->setEnabled(enabled);
    }
}

void IDPanelInventoryAcceptIn::refreshFolders()
{
    if (!mFolderList)
    {
        return;
    }

    LLUUID selected(gSavedPerAccountSettings.getString("IDInventoryAcceptInFolder"));
    const LLUUID temporary = IDInventoryOfferRedirect::destination();
    if (temporary.notNull() && IDInventoryOfferRedirect::isValidDestination(temporary))
    {
        selected = temporary;
    }

    mFolderList->clearRows();
    mFolderList->add(getString("default_folder"), LLUUID::null);

    if (temporary.notNull() && IDInventoryOfferRedirect::isValidDestination(temporary))
    {
        const LLInventoryCategory* category = gInventory.getCategory(temporary);
        LLStringUtil::format_map_t args;
        args["[FOLDER]"] = category->getName();
        mFolderList->add(getString("temporary_folder", args), temporary);
    }

    const LLUUID inbox = gInventory.findCategoryUUIDForType(LLFolderType::FT_INBOX);
    if (IDInventoryOfferRedirect::isValidOfferDestination(inbox))
    {
        mFolderList->add(temporary.isNull()
                             ? getString("received_items_folder")
                             : getString("received_items_unavailable"),
                         inbox, ADD_BOTTOM, temporary.isNull());
    }

    if (mObjectFolder.notNull() && (temporary.isNull() || mObjectFolder != inbox))
    {
        const LLInventoryCategory* category = gInventory.getCategory(mObjectFolder);
        if (category)
        {
            LLStringUtil::format_map_t args;
            args["[FOLDER]"] = category->getName();
            mFolderList->add(getString("object_folder", args), mObjectFolder);
        }
    }

    std::vector<std::pair<std::string, LLUUID>> saved_folders;
    const LLSD saved = gSavedPerAccountSettings.getLLSD("IDInventoryAcceptInFolders");
    if (saved.isArray())
    {
        for (LLSD::array_const_iterator it = saved.beginArray(); it != saved.endArray(); ++it)
        {
            const LLUUID folder_id = (*it)["id"].asUUID();
            const LLInventoryCategory* category = gInventory.getCategory(folder_id);
            if (category && IDInventoryOfferRedirect::isValidOfferDestination(folder_id) &&
                (temporary.isNull() || folder_id != inbox))
            {
                std::string label = (*it)["name"].asString();
                LLStringUtil::trim(label);
                saved_folders.emplace_back(label.empty() ? category->getName() : label, folder_id);
            }
        }
    }
    std::sort(saved_folders.begin(), saved_folders.end(),
              [](const auto& lhs, const auto& rhs) { return lhs.first < rhs.first; });
    for (const auto& saved_folder : saved_folders)
    {
        mFolderList->add(saved_folder.first, saved_folder.second);
    }

    if (!mFolderList->selectByValue(selected))
    {
        mFolderList->selectByValue(LLUUID::null);
    }
}
