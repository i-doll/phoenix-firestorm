#include "llviewerprecompiledheaders.h"
#include "idfloaterwornitems.h"

#include "llagent.h"
#include "llavataractions.h"
#include "llfloaterreg.h"
#include "llscrolllistctrl.h"
#include "llscrolllistitem.h"
#include "llselectmgr.h"
#include "lltrans.h"
#include "llviewerjointattachment.h"
#include "llviewerobjectlist.h"
#include "llviewerregion.h"
#include "llvoavatar.h"
#include "message.h"
#include "rlvactions.h"
#include "rlvcommon.h"

namespace
{
    constexpr std::string_view FLOATER_NAME = "id_worn_items";
}

IDFloaterWornItems::IDFloaterWornItems(const LLSD& key)
    : LLFloater(key)
{
}

IDFloaterWornItems::~IDFloaterWornItems()
{
    mAvatarNameConnection.disconnect();
    for (auto& entry : mCreatorNameConnections)
    {
        entry.second.disconnect();
    }
}

void IDFloaterWornItems::show(const LLUUID& avatar_id)
{
    if (canShow(avatar_id))
    {
        LLFloaterReg::showInstance(FLOATER_NAME, LLSD(avatar_id));
    }
}

bool IDFloaterWornItems::rlvAllowed(const LLUUID& avatar_id)
{
    return !RlvActions::isRlvEnabled() ||
           (RlvActions::canShowNearbyAgents() && RlvActions::canShowName(RlvActions::SNC_DEFAULT, avatar_id));
}

bool IDFloaterWornItems::canShow(const LLUUID& avatar_id)
{
    LLViewerObject* obj = gObjectList.findObject(avatar_id);
    return obj && obj->asAvatar() && !obj->isDead() && rlvAllowed(avatar_id);
}

void IDFloaterWornItems::processObjectProperties(LLMessageSystem* msg)
{
    for (LLFloater* floater : LLFloaterReg::getFloaterList(FLOATER_NAME))
    {
        if (auto self = dynamic_cast<IDFloaterWornItems*>(floater); self && !self->mPendingIds.empty())
        {
            self->handleObjectProperties(msg);
        }
    }
}

bool IDFloaterWornItems::postBuild()
{
    mList = getChild<LLScrollListCtrl>("worn_list");
    mList->setCommitOnSelectionChange(true);
    mList->setCommitCallback(boost::bind(&IDFloaterWornItems::onSelectionChange, this));
    mList->setDoubleClickCallback(boost::bind(&IDFloaterWornItems::onDoubleClick, this));

    childSetAction("refresh_btn", boost::bind(&IDFloaterWornItems::refreshList, this));
    childSetAction("creator_btn", boost::bind(&IDFloaterWornItems::onCreatorProfile, this));
    return true;
}

void IDFloaterWornItems::onOpen(const LLSD& key)
{
    mAvatarId = key.asUUID();
    if (!rlvAllowed(mAvatarId))
    {
        closeFloater();
        return;
    }
    updateTitle();
    refreshList();
}

void IDFloaterWornItems::onClose(bool app_quitting)
{
    if (!app_quitting)
    {
        sendSelect(false, mPendingIds);
    }
    mPendingIds.clear();
}

void IDFloaterWornItems::draw()
{
    if (!rlvAllowed(mAvatarId))
    {
        closeFloater();
        return;
    }
    LLFloater::draw();
}

LLVOAvatar* IDFloaterWornItems::getAvatar() const
{
    LLViewerObject* obj = gObjectList.findObject(mAvatarId);
    return (obj && !obj->isDead()) ? obj->asAvatar() : nullptr;
}

void IDFloaterWornItems::updateTitle()
{
    mAvatarNameConnection.disconnect();
    LLAvatarName av_name;
    if (LLAvatarNameCache::get(mAvatarId, &av_name))
    {
        LLStringUtil::format_map_t args;
        args["[NAME]"] = av_name.getCompleteName();
        setTitle(getString("title_fmt", args));
    }
    else
    {
        mAvatarNameConnection = LLAvatarNameCache::get(mAvatarId, [this](const LLUUID&, const LLAvatarName&) { updateTitle(); });
    }
}

void IDFloaterWornItems::refreshList()
{
    sendSelect(false, mPendingIds);
    mPendingIds.clear();
    mCreatorByObject.clear();
    mList->deleteAllItems();
    getChildView("creator_btn")->setEnabled(false);

    LLVOAvatar* avatar = getAvatar();
    if (!avatar)
    {
        mList->setCommentText(getString("not_in_range"));
        return;
    }
    mList->setCommentText(getString("nothing_worn"));

    for (const auto& [point_id, point] : avatar->mAttachmentPoints)
    {
        // HUDs are never sent to other viewers.
        if (!point || point->getIsHUDAttachment())
        {
            continue;
        }
        for (const auto& obj_ptr : point->mAttachedObjects)
        {
            LLViewerObject* obj = obj_ptr.get();
            if (!obj)
            {
                continue;
            }
            LLSD row;
            row["id"] = obj->getID();
            row["columns"][0]["column"] = "name";
            row["columns"][0]["value"] = getString("loading");
            row["columns"][1]["column"] = "point";
            row["columns"][1]["value"] = LLTrans::getString(point->getName());
            row["columns"][2]["column"] = "creator";
            row["columns"][2]["value"] = "";
            mList->addElement(row);
            mPendingIds.insert(obj->getID());
        }
    }
    mList->sortByColumn("name", true);

    sendSelect(true, mPendingIds);
}

// Selecting an object is how the viewer asks for its full properties, the only
// reply that carries name and creator for another avatar's attachments. This
// talks to the region directly so nothing shows up as selected in the world.
void IDFloaterWornItems::sendSelect(bool select, const uuid_set_t& object_ids)
{
    LLVOAvatar* avatar = getAvatar();
    LLViewerRegion* region = avatar ? avatar->getRegion() : nullptr;
    if (!region)
    {
        return;
    }

    LLMessageSystem* msg = gMessageSystem;
    bool start_new_message = true;
    for (const LLUUID& object_id : object_ids)
    {
        LLViewerObject* obj = gObjectList.findObject(object_id);
        // Leave anything the user has selected themselves alone.
        if (!obj || (!select && LLSelectMgr::getInstance()->getSelection()->findNode(obj)))
        {
            continue;
        }
        if (start_new_message)
        {
            msg->newMessageFast(select ? _PREHASH_ObjectSelect : _PREHASH_ObjectDeselect);
            msg->nextBlockFast(_PREHASH_AgentData);
            msg->addUUIDFast(_PREHASH_AgentID, gAgent.getID());
            msg->addUUIDFast(_PREHASH_SessionID, gAgent.getSessionID());
            start_new_message = false;
        }
        msg->nextBlockFast(_PREHASH_ObjectData);
        msg->addU32Fast(_PREHASH_ObjectLocalID, obj->getLocalID());
        if (msg->isSendFull())
        {
            msg->sendReliable(region->getHost());
            start_new_message = true;
        }
    }
    if (!start_new_message)
    {
        msg->sendReliable(region->getHost());
    }
}

void IDFloaterWornItems::handleObjectProperties(LLMessageSystem* msg)
{
    const S32 name_col = mList->getColumn("name")->mIndex;
    uuid_set_t answered;

    const S32 count = msg->getNumberOfBlocksFast(_PREHASH_ObjectData);
    for (S32 i = 0; i < count; ++i)
    {
        LLUUID object_id;
        msg->getUUIDFast(_PREHASH_ObjectData, _PREHASH_ObjectID, object_id, i);
        LLScrollListItem* item = mList->getItem(LLSD(object_id));
        if (!item)
        {
            continue;
        }

        std::string name;
        LLUUID creator_id;
        msg->getStringFast(_PREHASH_ObjectData, _PREHASH_Name, name, i);
        msg->getUUIDFast(_PREHASH_ObjectData, _PREHASH_CreatorID, creator_id, i);

        item->getColumn(name_col)->setValue(name);
        mCreatorByObject[object_id] = creator_id;
        setCreatorName(object_id);

        if (mPendingIds.erase(object_id))
        {
            answered.insert(object_id);
        }
    }

    sendSelect(false, answered);
    if (!answered.empty())
    {
        mList->updateSort();
        onSelectionChange();
    }
}

bool IDFloaterWornItems::canShowCreator(const LLUUID& creator_id) const
{
    // Same rule as the Inspect floater: hide the creator only when they're the
    // wearer or someone nearby.
    return !RlvActions::isRlvEnabled() ||
           RlvActions::canShowName(RlvActions::SNC_DEFAULT, creator_id) ||
           (creator_id != mAvatarId && !RlvUtil::isNearbyAgent(creator_id));
}

void IDFloaterWornItems::setCreatorName(const LLUUID& object_id)
{
    auto it = mCreatorByObject.find(object_id);
    LLScrollListItem* item = mList->getItem(LLSD(object_id));
    if (it == mCreatorByObject.end() || !item)
    {
        return;
    }
    const LLUUID& creator_id = it->second;

    LLAvatarName av_name;
    if (LLAvatarNameCache::get(creator_id, &av_name))
    {
        const std::string name = canShowCreator(creator_id) ? av_name.getCompleteName() : RlvStrings::getAnonym(av_name);
        item->getColumn(mList->getColumn("creator")->mIndex)->setValue(name);
        return;
    }

    if (!mCreatorNameConnections[creator_id].connected())
    {
        mCreatorNameConnections[creator_id] = LLAvatarNameCache::get(creator_id,
            [this](const LLUUID& id, const LLAvatarName&)
            {
                mCreatorNameConnections.erase(id);
                for (const auto& [obj_id, creator] : mCreatorByObject)
                {
                    if (creator == id)
                    {
                        setCreatorName(obj_id);
                    }
                }
            });
    }
}

void IDFloaterWornItems::onSelectionChange()
{
    LLScrollListItem* item = mList->getFirstSelected();
    auto it = item ? mCreatorByObject.find(item->getUUID()) : mCreatorByObject.end();
    getChildView("creator_btn")->setEnabled(it != mCreatorByObject.end() && canShowCreator(it->second));
}

void IDFloaterWornItems::onDoubleClick()
{
    LLScrollListItem* item = mList->getFirstSelected();
    LLViewerObject* obj = item ? gObjectList.findObject(item->getUUID()) : nullptr;
    if (!obj || (RlvActions::isRlvEnabled() && !RlvActions::canEdit(obj)))
    {
        return;
    }
    LLSelectMgr::getInstance()->selectObjectAndFamily(obj);
    LLFloaterReg::showInstance("task_properties");
}

void IDFloaterWornItems::onCreatorProfile()
{
    LLScrollListItem* item = mList->getFirstSelected();
    auto it = item ? mCreatorByObject.find(item->getUUID()) : mCreatorByObject.end();
    if (it != mCreatorByObject.end() && canShowCreator(it->second))
    {
        LLAvatarActions::showProfile(it->second);
    }
}
