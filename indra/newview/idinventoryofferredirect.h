/**
 * @file idinventoryofferredirect.h
 * @brief Session-scoped destination for received inventory offers.
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
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
 * Lesser General Public License for more details.
 *
 * The Phoenix Firestorm Project, Inc., 1831 Oakwood Drive, Fairmont, Minnesota 56031-3225 USA
 * http://www.firestormviewer.org
 * $/LicenseInfo$
 */

#ifndef ID_INVENTORY_OFFER_REDIRECT_H
#define ID_INVENTORY_OFFER_REDIRECT_H

#include "lluuid.h"

#include <string>

class IDInventoryOfferRedirect
{
public:
    static LLUUID destination();
    static LLUUID resolveDestination(const LLUUID& default_destination);
    static bool isDestination(const LLUUID& folder_id);
    static bool isValidDestination(const LLUUID& folder_id);
    static bool isValidOfferDestination(const LLUUID& folder_id);

    static bool setDestination(const LLUUID& folder_id);
    static void clearDestination();

    // Agent offers are copied into inventory before the viewer can choose a
    // destination, so they need to be moved after they arrive locally.
    static void redirectAgentOffer(const LLUUID& object_id);
    static void redirectAgentOffer(const LLUUID& object_id, const LLUUID& destination);

    // Task offers can arrive through more than one inventory update path. The
    // observer is a fallback for simulators that ignore the requested folder.
    static void redirectTaskOffer(const std::string& description,
                                  const LLUUID& transaction_id);
    static void redirectTaskOffer(const std::string& description,
                                  const LLUUID& transaction_id,
                                  const LLUUID& destination);

private:
    static LLUUID sDestination;
    static LLUUID sInventoryRoot;
    static LLUUID sAgentSession;
};

#endif // ID_INVENTORY_OFFER_REDIRECT_H
