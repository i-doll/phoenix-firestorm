/**
 * @file idinventoryofferredirect.cpp
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

#include "llviewerprecompiledheaders.h"

#include "idinventoryofferredirect.h"

#include "llagent.h"
#include "llappviewer.h"
#include "llinventorymodel.h"
#include "llinventoryobserver.h"
#include "llnotificationsutil.h"
#include "lltrans.h"
#include "llviewerinventory.h"

LLUUID IDInventoryOfferRedirect::sDestination;
LLUUID IDInventoryOfferRedirect::sInventoryRoot;
LLUUID IDInventoryOfferRedirect::sAgentSession;

namespace
{
    class IDRedirectInboxObserver;
    IDRedirectInboxObserver* sInboxObserver = nullptr;

    bool is_in_special_folder(const LLUUID& folder_id, LLFolderType::EType folder_type)
    {
        const LLUUID special_id = gInventory.findCategoryUUIDForType(folder_type);
        return special_id.notNull() &&
               (folder_id == special_id || gInventory.isObjectDescendentOf(folder_id, special_id));
    }

    void mark_folder_label_changed(const LLUUID& folder_id)
    {
        if (folder_id.notNull() && gInventory.getCategory(folder_id))
        {
            gInventory.addChangedMask(LLInventoryObserver::LABEL, folder_id);
        }
    }

    void move_offer_object(const LLUUID& object_id, const LLUUID& destination)
    {
        if (!IDInventoryOfferRedirect::isValidOfferDestination(destination))
        {
            return;
        }

        if (LLViewerInventoryCategory* category = gInventory.getCategory(object_id))
        {
            if (category->getParentUUID() != destination &&
                !gInventory.isObjectDescendentOf(destination, object_id))
            {
                gInventory.changeCategoryParent(category, destination, false);
            }
        }
        else if (LLViewerInventoryItem* item = gInventory.getItem(object_id))
        {
            if (item->getParentUUID() != destination)
            {
                gInventory.changeItemParent(item, destination, true);
            }
        }
    }

    void move_offer_objects(const uuid_vec_t& object_ids, const LLUUID& destination)
    {
        for (const LLUUID& object_id : object_ids)
        {
            move_offer_object(object_id, destination);
        }
    }

    class IDRedirectAgentOfferObserver final : public LLInventoryFetchItemsObserver
    {
    public:
        IDRedirectAgentOfferObserver(const LLUUID& object_id, const LLUUID& destination)
            : LLInventoryFetchItemsObserver(object_id),
              mObjectID(object_id),
              mDestination(destination)
        {
        }

        void changed(U32 mask) override
        {
            if (gInventory.getCategory(mObjectID))
            {
                done();
                return;
            }
            LLInventoryFetchItemsObserver::changed(mask);
        }

        void done() override
        {
            const LLUUID object_id = mObjectID;
            const LLUUID destination = mDestination;

            // Inventory observers run inside notifyObservers(); defer the move
            // to avoid a nested observer notification.
            gInventory.removeObserver(this);
            LLAppViewer::instance()->addOnIdleCallback(
                [object_id, destination]() { move_offer_object(object_id, destination); });
            delete this;
        }

    private:
        LLUUID mObjectID;
        LLUUID mDestination;
    };

    class IDRedirectTaskOfferObserver final : public LLInventoryObserver
    {
    public:
        IDRedirectTaskOfferObserver(const std::string& description,
                                    const LLUUID& transaction_id,
                                    const LLUUID& destination)
            : mDescription(description),
              mTransactionID(transaction_id),
              mDestination(destination)
        {
        }

        void changed(U32 mask) override
        {
            if ((mask & LLInventoryObserver::ADD) &&
                gInventory.getTransactionId().notNull() &&
                mTransactionID == gInventory.getTransactionId())
            {
                collectAddedObjects();
                done();
            }
            else if (mask & (LLInventoryObserver::ADD | LLInventoryObserver::UPDATE_CREATE))
            {
                for (const LLUUID& object_id : gInventory.getAddedIDs())
                {
                    LLViewerInventoryItem* item = gInventory.getItem(object_id);
                    if (item && mDescription.find("'" + item->getName() + "'") == 0)
                    {
                        mObjects.push_back(object_id);
                        done();
                        break;
                    }
                }
            }
        }

    private:
        void collectAddedObjects()
        {
            const uuid_set_t& added_ids = gInventory.getAddedIDs();
            uuid_set_t added_categories;

            // Folders arrive before their children. Moving just the top-level
            // folder avoids separately moving everything inside it.
            for (const LLUUID& object_id : added_ids)
            {
                if (LLViewerInventoryCategory* category = gInventory.getCategory(object_id))
                {
                    added_categories.insert(category->getUUID());
                }
            }
            for (const LLUUID& category_id : added_categories)
            {
                const LLViewerInventoryCategory* category = gInventory.getCategory(category_id);
                if (category && added_categories.find(category->getParentUUID()) == added_categories.end())
                {
                    mObjects.push_back(category_id);
                }
            }
            for (const LLUUID& object_id : added_ids)
            {
                LLViewerInventoryItem* item = gInventory.getItem(object_id);
                if (!item || added_categories.find(item->getParentUUID()) != added_categories.end())
                {
                    continue;
                }
                mObjects.push_back(item->getUUID());
            }
        }

        void done()
        {
            gInventory.removeObserver(this);
            const uuid_vec_t objects = mObjects;
            const LLUUID destination = mDestination;
            LLAppViewer::instance()->addOnIdleCallback(
                [objects, destination]() { move_offer_objects(objects, destination); });
            delete this;
        }

        std::string mDescription;
        LLUUID mTransactionID;
        LLUUID mDestination;
        uuid_vec_t mObjects;
    };

    class IDRedirectInboxObserver final : public LLInventoryObserver
    {
    public:
        void changed(U32 mask) override
        {
            const LLUUID destination = IDInventoryOfferRedirect::destination();
            if (destination.notNull() && !IDInventoryOfferRedirect::isValidDestination(destination))
            {
                LLAppViewer::instance()->addOnIdleCallback(
                    []() { IDInventoryOfferRedirect::clearDestination(); });
                return;
            }

            if (!(mask & LLInventoryObserver::ADD))
            {
                return;
            }

            const LLUUID inbox = gInventory.findCategoryUUIDForType(LLFolderType::FT_INBOX);
            if (destination.isNull() || inbox.isNull())
            {
                return;
            }

            uuid_vec_t received_objects;
            for (const LLUUID& object_id : gInventory.getAddedIDs())
            {
                const LLInventoryObject* object = gInventory.getObject(object_id);
                if (object && object->getParentUUID() == inbox)
                {
                    received_objects.push_back(object_id);
                }
            }

            if (!received_objects.empty())
            {
                LLAppViewer::instance()->addOnIdleCallback(
                    [received_objects, destination]()
                    {
                        move_offer_objects(received_objects, destination);
                    });
            }
        }
    };

    void start_inbox_redirect()
    {
        if (!sInboxObserver)
        {
            sInboxObserver = new IDRedirectInboxObserver;
            gInventory.addObserver(sInboxObserver);
        }
    }

    void stop_inbox_redirect()
    {
        if (sInboxObserver)
        {
            gInventory.removeObserver(sInboxObserver);
            delete sInboxObserver;
            sInboxObserver = nullptr;
        }
    }
}

LLUUID IDInventoryOfferRedirect::destination()
{
    if (sDestination.notNull() &&
        (sInventoryRoot != gInventory.getRootFolderID() || sAgentSession != gAgent.getSessionID()))
    {
        stop_inbox_redirect();
        sDestination.setNull();
        sInventoryRoot.setNull();
        sAgentSession.setNull();
    }
    return sDestination;
}

LLUUID IDInventoryOfferRedirect::resolveDestination(const LLUUID& default_destination)
{
    if (sDestination.notNull() && isValidDestination(sDestination))
    {
        return sDestination;
    }
    if (sDestination.notNull())
    {
        clearDestination();
    }
    return default_destination;
}

bool IDInventoryOfferRedirect::isDestination(const LLUUID& folder_id)
{
    return folder_id.notNull() && destination() == folder_id;
}

bool IDInventoryOfferRedirect::isValidDestination(const LLUUID& folder_id)
{
    const LLViewerInventoryCategory* category = gInventory.getCategory(folder_id);
    if (!category || category->getPreferredType() != LLFolderType::FT_NONE)
    {
        return false;
    }

    const LLUUID inventory_root = gInventory.getRootFolderID();
    if (inventory_root.isNull() || !gInventory.isObjectDescendentOf(folder_id, inventory_root))
    {
        return false;
    }

    // These trees have special server or viewer semantics and are not safe as
    // a catch-all destination for arbitrary inventory types.
    return !is_in_special_folder(folder_id, LLFolderType::FT_TRASH) &&
           !is_in_special_folder(folder_id, LLFolderType::FT_LOST_AND_FOUND) &&
           !is_in_special_folder(folder_id, LLFolderType::FT_FAVORITE) &&
           !is_in_special_folder(folder_id, LLFolderType::FT_CURRENT_OUTFIT) &&
           !is_in_special_folder(folder_id, LLFolderType::FT_MY_OUTFITS) &&
           !is_in_special_folder(folder_id, LLFolderType::FT_INBOX) &&
           !is_in_special_folder(folder_id, LLFolderType::FT_OUTBOX) &&
           !is_in_special_folder(folder_id, LLFolderType::FT_MARKETPLACE_LISTINGS);
}

bool IDInventoryOfferRedirect::isValidOfferDestination(const LLUUID& folder_id)
{
    const LLViewerInventoryCategory* category = gInventory.getCategory(folder_id);
    if (!category)
    {
        return false;
    }

    const LLUUID inventory_root = gInventory.getRootFolderID();
    if (inventory_root.isNull() ||
        (folder_id != inventory_root && !gInventory.isObjectDescendentOf(folder_id, inventory_root)))
    {
        return false;
    }

    // Per-offer routing may intentionally use Received Items or a type-specific
    // system folder. Keep only trees whose semantics make arbitrary receipt
    // unsafe out of the picker.
    return !is_in_special_folder(folder_id, LLFolderType::FT_TRASH) &&
           !is_in_special_folder(folder_id, LLFolderType::FT_LOST_AND_FOUND) &&
           !is_in_special_folder(folder_id, LLFolderType::FT_FAVORITE) &&
           !is_in_special_folder(folder_id, LLFolderType::FT_CURRENT_OUTFIT) &&
           !is_in_special_folder(folder_id, LLFolderType::FT_MY_OUTFITS) &&
           !is_in_special_folder(folder_id, LLFolderType::FT_OUTBOX) &&
           !is_in_special_folder(folder_id, LLFolderType::FT_MARKETPLACE_LISTINGS);
}

bool IDInventoryOfferRedirect::setDestination(const LLUUID& folder_id)
{
    if (!isValidDestination(folder_id))
    {
        return false;
    }

    if (sDestination == folder_id)
    {
        return true;
    }

    const LLUUID previous = sDestination;
    sDestination = folder_id;
    sInventoryRoot = gInventory.getRootFolderID();
    sAgentSession = gAgent.getSessionID();
    start_inbox_redirect();
    mark_folder_label_changed(previous);
    mark_folder_label_changed(sDestination);
    gInventory.notifyObservers();

    LLSD args;
    args["FOLDER"] = gInventory.getCategory(sDestination)->getName();
    LLNotificationsUtil::add("SystemMessageTip", LLSD().with("MESSAGE", LLTrans::getString("InventoryReceivedItemsRedirectSet", args)));
    return true;
}

void IDInventoryOfferRedirect::clearDestination()
{
    if (sDestination.isNull())
    {
        return;
    }

    const LLUUID previous = sDestination;
    sDestination.setNull();
    sInventoryRoot.setNull();
    sAgentSession.setNull();
    stop_inbox_redirect();
    mark_folder_label_changed(previous);
    gInventory.notifyObservers();
    LLNotificationsUtil::add("SystemMessageTip", LLSD().with("MESSAGE", LLTrans::getString("InventoryReceivedItemsRedirectCleared")));
}

void IDInventoryOfferRedirect::redirectAgentOffer(const LLUUID& object_id)
{
    const LLUUID redirect = resolveDestination(LLUUID::null);
    redirectAgentOffer(object_id, redirect);
}

void IDInventoryOfferRedirect::redirectAgentOffer(const LLUUID& object_id,
                                                  const LLUUID& redirect)
{
    if (!isValidOfferDestination(redirect) || object_id.isNull())
    {
        return;
    }

    IDRedirectAgentOfferObserver* observer = new IDRedirectAgentOfferObserver(object_id, redirect);
    observer->startFetch();
    if (observer->isFinished())
    {
        observer->done();
    }
    else
    {
        gInventory.addObserver(observer);
    }
}

void IDInventoryOfferRedirect::redirectTaskOffer(const std::string& description,
                                                 const LLUUID& transaction_id)
{
    const LLUUID redirect = resolveDestination(LLUUID::null);
    redirectTaskOffer(description, transaction_id, redirect);
}

void IDInventoryOfferRedirect::redirectTaskOffer(const std::string& description,
                                                 const LLUUID& transaction_id,
                                                 const LLUUID& redirect)
{
    if (!isValidOfferDestination(redirect) || transaction_id.isNull())
    {
        return;
    }

    gInventory.addObserver(new IDRedirectTaskOfferObserver(description, transaction_id, redirect));
}
