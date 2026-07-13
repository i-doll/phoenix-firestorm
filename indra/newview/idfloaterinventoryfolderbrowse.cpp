/**
 * @file idfloaterinventoryfolderbrowse.cpp
 * @brief Folder browser for per-transaction inventory destinations.
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

#include "idfloaterinventoryfolderbrowse.h"

#include "idinventoryofferredirect.h"

#include "llbutton.h"
#include "llfiltereditor.h"
#include "llfolderview.h"
#include "llinventorybridge.h"
#include "llinventorymodel.h"
#include "llinventorypanel.h"
#include "llviewerfoldertype.h"

namespace
{
    bool is_selectable_destination(const LLUUID& folder_id)
    {
        if (!IDInventoryOfferRedirect::isValidOfferDestination(folder_id))
        {
            return false;
        }

        // While the session redirect is active, its Received Items observer
        // would immediately move a deliberately inbox-bound purchase again.
        const LLUUID inbox = gInventory.findCategoryUUIDForType(LLFolderType::FT_INBOX);
        return IDInventoryOfferRedirect::destination().isNull() || folder_id != inbox;
    }
}

IDFloaterInventoryFolderBrowse::IDFloaterInventoryFolderBrowse()
    : LLFloater(LLSD())
{
    buildFromFile("floater_id_inventory_folder_browse.xml");
}

bool IDFloaterInventoryFolderBrowse::postBuild()
{
    mFilterEditor = findChild<LLFilterEditor>("inventory_filter");
    mInventoryPanel = findChild<LLInventoryPanel>("inventory_panel");
    if (!mFilterEditor || !mInventoryPanel)
    {
        return false;
    }

    mFilterEditor->setCommitCallback(
        [this](LLUICtrl*, const LLSD& value) { onFilterEdit(value.asString()); });

    mInventoryPanel->setFilterTypes(1ULL << LLInventoryType::IT_CATEGORY);
    mInventoryPanel->setShowFolderState(LLInventoryFilter::SHOW_ALL_FOLDERS);
    mInventoryPanel->getFilter().setFilterCategoryTypes(
        mInventoryPanel->getFilter().getFilterCategoryTypes() | (1ULL << LLFolderType::FT_INBOX));
    mInventoryPanel->getFilter().markDefault();
    mInventoryPanel->setSelectCallback(
        [this](const std::deque<LLFolderViewItem*>&, bool) { refreshSelection(); });

    findChild<LLButton>("select_button")->setCommitCallback(
        [this](LLUICtrl*, const LLSD&) { onSelect(); });
    findChild<LLButton>("cancel_button")->setCommitCallback(
        [this](LLUICtrl*, const LLSD&) { onCancel(); });
    findChild<LLButton>("close_all_button")->setCommitCallback(
        [this](LLUICtrl*, const LLSD&) { onCloseAllFolders(); });

    refreshSelection();
    return true;
}

void IDFloaterInventoryFolderBrowse::onCommit()
{
    if (!mCommitSignal || !mInventoryPanel)
    {
        return;
    }

    LLFolderView::selected_items_t& selected = mInventoryPanel->getRootFolder()->getSelectedItems();
    const LLInvFVBridge* bridge = selected.empty()
        ? nullptr
        : dynamic_cast<const LLInvFVBridge*>(selected.front()->getViewModelItem());
    if (bridge && is_selectable_destination(bridge->getUUID()))
    {
        (*mCommitSignal)(this, LLSD().with("name", bridge->getName()).with("uuid", bridge->getUUID()));
    }
}

void IDFloaterInventoryFolderBrowse::onOpen(const LLSD& key)
{
    const LLUUID folder_id = key["folder_id"].asUUID();
    if (folder_id.notNull())
    {
        mInventoryPanel->setSelection(folder_id, TAKE_FOCUS_NO);
    }
    refreshSelection();
}

void IDFloaterInventoryFolderBrowse::onCancel()
{
    closeFloater();
}

void IDFloaterInventoryFolderBrowse::onCloseAllFolders()
{
    mInventoryPanel->closeAllFolders();
}

void IDFloaterInventoryFolderBrowse::onFilterEdit(const std::string& filter)
{
    mInventoryPanel->setFilterSubString(filter);
}

void IDFloaterInventoryFolderBrowse::onSelect()
{
    if (getChildView("select_button")->getEnabled())
    {
        onCommit();
        closeFloater();
    }
}

void IDFloaterInventoryFolderBrowse::refreshSelection()
{
    bool valid = false;
    if (mInventoryPanel)
    {
        LLFolderView::selected_items_t& selected = mInventoryPanel->getRootFolder()->getSelectedItems();
        const LLInvFVBridge* bridge = selected.empty()
            ? nullptr
            : dynamic_cast<const LLInvFVBridge*>(selected.front()->getViewModelItem());
        valid = bridge && is_selectable_destination(bridge->getUUID());
    }
    getChildView("select_button")->setEnabled(valid);
}
