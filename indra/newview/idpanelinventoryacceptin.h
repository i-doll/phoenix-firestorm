/**
 * @file idpanelinventoryacceptin.h
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

#ifndef ID_PANEL_INVENTORY_ACCEPT_IN_H
#define ID_PANEL_INVENTORY_ACCEPT_IN_H

#include "llpanel.h"

#include <boost/signals2/connection.hpp>

class LLButton;
class LLCheckBoxCtrl;
class LLComboBox;
class LLFloater;

class IDPanelInventoryAcceptIn final : public LLPanel
{
public:
    IDPanelInventoryAcceptIn();
    ~IDPanelInventoryAcceptIn() override;

    bool postBuild() override;

    bool getAcceptIn() const;
    LLUUID getSelectedFolder() const;
    void refresh();
    void setObjectFolder(const LLUUID& folder_id);

private:
    void onAcceptInChanged();
    void onBrowse();
    void onBrowseResult(const LLSD& data);
    void onConfigure();
    void onFolderChanged();
    void refreshControls();
    void refreshFolders();

    LLCheckBoxCtrl* mAcceptInCheck { nullptr };
    LLComboBox* mFolderList { nullptr };
    LLButton* mBrowseButton { nullptr };
    LLButton* mConfigureButton { nullptr };
    LLUUID mObjectFolder;
    LLHandle<LLFloater> mBrowseFloater;
    boost::signals2::connection mDestinationsChangedConnection;
};

#endif // ID_PANEL_INVENTORY_ACCEPT_IN_H
