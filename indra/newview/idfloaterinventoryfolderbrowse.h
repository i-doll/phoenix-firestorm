/**
 * @file idfloaterinventoryfolderbrowse.h
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

#ifndef ID_FLOATER_INVENTORY_FOLDER_BROWSE_H
#define ID_FLOATER_INVENTORY_FOLDER_BROWSE_H

#include "llfloater.h"

class LLFilterEditor;
class LLInventoryPanel;

class IDFloaterInventoryFolderBrowse final : public LLFloater
{
public:
    IDFloaterInventoryFolderBrowse();

    bool postBuild() override;
    void onCommit() override;
    void onOpen(const LLSD& key) override;

private:
    void onCancel();
    void onCloseAllFolders();
    void onFilterEdit(const std::string& filter);
    void onSelect();
    void refreshSelection();

    LLFilterEditor* mFilterEditor { nullptr };
    LLInventoryPanel* mInventoryPanel { nullptr };
};

#endif // ID_FLOATER_INVENTORY_FOLDER_BROWSE_H
