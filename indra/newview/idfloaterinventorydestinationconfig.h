/**
 * @file idfloaterinventorydestinationconfig.h
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

#ifndef ID_FLOATER_INVENTORY_DESTINATION_CONFIG_H
#define ID_FLOATER_INVENTORY_DESTINATION_CONFIG_H

#include "llfloater.h"

class LLButton;
class LLLineEditor;
class LLScrollListCtrl;
class LLTextBox;

class IDFloaterInventoryDestinationConfig final : public LLFloater
{
public:
    explicit IDFloaterInventoryDestinationConfig(const LLSD& key);
    ~IDFloaterInventoryDestinationConfig() override;

    bool postBuild() override;
    void onOpen(const LLSD& key) override;

private:
    void onAddDestination();
    void onBrowseResult(const LLSD& data);
    void onCloseClicked();
    void onLabelKeystroke();
    void onRemoveDestination();
    void onSaveLabel();
    void onSelectionChanged();

    bool isLabelAvailable(const std::string& label, const LLUUID& except_id) const;
    void persist(const LLSD& destinations);
    void refreshControls();
    void refreshList(const LLUUID& selected_id = LLUUID::null);

    LLScrollListCtrl* mDestinationList { nullptr };
    LLLineEditor* mLabelEditor { nullptr };
    LLButton* mSaveLabelButton { nullptr };
    LLButton* mRemoveButton { nullptr };
    LLTextBox* mValidationText { nullptr };
    LLHandle<LLFloater> mBrowseFloater;
};

#endif // ID_FLOATER_INVENTORY_DESTINATION_CONFIG_H
