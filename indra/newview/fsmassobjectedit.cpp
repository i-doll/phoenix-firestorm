/**
 * @file fsmassobjectedit.cpp
 * @brief Region-wide multi-object task inventory editor.
 */

#include "llviewerprecompiledheaders.h"

#include "fsmassobjectedit.h"

#include "fscommon.h"
#include "llagent.h"
#include "llagentcamera.h"
#include "llassetstorage.h"
#include "llavatarnamecache.h"
#include "llbutton.h"
#include "llcachename.h"
#include "llcallbacklist.h"
#include "llcoros.h"
#include "lleventcoro.h"
#include "llevents.h"
#include "llinventorydefines.h"
#include "llinventorymodel.h"
#include "lllineeditor.h"
#include "llmenugl.h"
#include "message.h"
#include "llnotificationsutil.h"
#include "llprogressbar.h"
#include "llscrolllistctrl.h"
#include "llselectmgr.h"
#include "lltextbox.h"
#include "lltooldraganddrop.h"
#include "llviewerinventory.h"
#include "llviewerobject.h"
#include "llviewerobjectlist.h"
#include "llviewerregion.h"
#include "llviewerassetupload.h"
#include "llviewercontrol.h"
#include "llvoinventorylistener.h"
#include "llviewermenu.h"
#include "lluicolortable.h"
#include "lluictrlfactory.h"
#include "roles_constants.h"
#include "rlvactions.h"
#include "rlvhandler.h"

#include <algorithm>
#include <memory>
#include <set>

namespace
{
    constexpr F32 INVENTORY_TIMEOUT = 20.f;
    constexpr F32 TARGET_LIST_UPDATE_BUDGET = 0.003f;
    constexpr F32 FILTER_REFRESH_DELAY = 0.25f;
    constexpr F32 NAME_LIST_REFRESH_DELAY = 0.5f;
    constexpr S32 MAX_OBJECTS_PER_PACKET = 50;
    constexpr S32 MAX_PROPERTY_REQUESTS_IN_FLIGHT = MAX_OBJECTS_PER_PACKET * 3;
    constexpr S32 MAX_PROPERTY_RETRIES = 8;
    constexpr F32 PROPERTY_RETRY_INTERVAL = 8.f;
    constexpr F32 OBJECT_RECONCILE_INTERVAL = 1.5f;
    constexpr S32 TARGET_SCAN_WORKERS = 3;
    constexpr S32 TARGET_SCAN_RETRIES = 10;
    constexpr F32 TARGET_SCAN_RETRY_DELAY = 1.f;

    bool canModifyTarget(const LLViewerObject* object)
    {
        return object && !object->isDead() && object->getRegion() &&
            object->getRegion() == gAgent.getRegion() && !object->isAvatar() &&
            !object->isAttachment() && object->permModify() &&
            RlvActions::canEdit(object) && RlvActions::canInteract(object) &&
            !gRlvHandler.hasBehaviour(RLV_BHVR_AREASEARCH);
    }

    std::string avatarSearchTerms(const LLAvatarName& av_name)
    {
        return av_name.getCompleteName() + " " + av_name.getDisplayName(true) + " " +
            av_name.getAccountName() + " " + av_name.getLegacyName() + " " +
            av_name.getUserName();
    }

    bool matchesCountFilter(S32 count, std::string filter)
    {
        LLStringUtil::trim(filter);
        if (filter.empty()) return true;

        const auto parse = [](const std::string& value, S32& result)
        {
            return LLStringUtil::convertToS32(value, result);
        };
        S32 value = 0;
        if (filter.compare(0, 2, ">=") == 0) return parse(filter.substr(2), value) && count >= value;
        if (filter.compare(0, 2, "<=") == 0) return parse(filter.substr(2), value) && count <= value;
        if (filter.compare(0, 1, ">") == 0) return parse(filter.substr(1), value) && count > value;
        if (filter.compare(0, 1, "<") == 0) return parse(filter.substr(1), value) && count < value;
        if (filter.compare(0, 1, "=") == 0) return parse(filter.substr(1), value) && count == value;

        const size_t dash = filter.find('-', 1);
        if (dash != std::string::npos)
        {
            S32 minimum = 0;
            S32 maximum = 0;
            return parse(filter.substr(0, dash), minimum) &&
                parse(filter.substr(dash + 1), maximum) &&
                count >= minimum && count <= maximum;
        }
        return parse(filter, value) && count >= value;
    }

    class InventoryFetcher final : public LLVOInventoryListener
    {
    public:
        InventoryFetcher(LLEventPump& pump, LLViewerObject* object) : mPump(pump)
        {
            registerVOInventoryListener(object, nullptr);
        }

        void fetch() { requestVOInventory(); }
        const LLInventoryObject::object_list_t& inventory() const { return mInventory; }

        void inventoryChanged(LLViewerObject*, LLInventoryObject::object_list_t* inventory,
            S32, void*) override
        {
            mInventory.clear();
            if (inventory)
            {
                mInventory.assign(inventory->begin(), inventory->end());
            }
            mPump.post(LLSDMap("inventory", true));
        }

    private:
        LLEventPump& mPump;
        LLInventoryObject::object_list_t mInventory;
    };

    class ExistingScriptAssetUpload final : public LLScriptAssetUpload
    {
    public:
        ExistingScriptAssetUpload(const LLUUID& task_id, const LLUUID& item_id,
            const LLUUID& asset_id, const std::string& target, taskUploadFinish_f finish,
            uploadFailed_f failed) :
            LLScriptAssetUpload(task_id, item_id, target, false, LLUUID::null,
                std::string(), finish, failed)
        {
            setAssetId(asset_id);
        }

        LLSD prepareUpload() override { return LLSDMap("success", true); }
    };

    void assetFetched(const LLUUID& asset_id, LLAssetType::EType, void* userdata,
        S32 status, LLExtStat)
    {
        std::unique_ptr<std::string> pump_name(static_cast<std::string*>(userdata));
        LLEventPumps::instance().post(*pump_name,
            LLSDMap("asset_id", asset_id)("status", status));
    }

    bool fetchScriptAsset(LLViewerObject* object, LLViewerInventoryItem* item,
        LLEventPump& parent_pump)
    {
        LLEventMailDrop pump(parent_pump.getName() + "-asset-" + LLUUID::generateNewID().asString(), true);
        LLPointer<LLViewerObject> keep_alive = object;
        if (!canModifyTarget(object) || !item || !gAssetStorage)
        {
            return false;
        }
        std::string* pump_name = new std::string(pump.getName());
        gAssetStorage->getInvItemAsset(LLHost(), gAgent.getID(), gAgent.getSessionID(),
            item->getPermissions().getOwner(), object->getID(), item->getUUID(),
            item->getAssetUUID(), item->getType(), &assetFetched, pump_name, true);
        LLSD result = llcoro::suspendUntilEventOnWithTimeout(pump, INVENTORY_TIMEOUT,
            LLSDMap("timeout", true));
        if (result.has("timeout") || result["status"].asInteger() != LL_ERR_NOERR ||
            result["asset_id"].asUUID().isNull())
        {
            return false;
        }
        item->setAssetUUID(result["asset_id"].asUUID());
        return true;
    }

    void uploadSucceeded(const std::string& pump_name, const LLSD& result)
    {
        LLEventPumps::instance().post(pump_name, result);
    }

    bool uploadFailed(const std::string& pump_name, LLSD result, const std::string& reason)
    {
        result["compiled"] = false;
        result["reason"] = reason;
        LLEventPumps::instance().post(pump_name, result);
        return true;
    }

    bool compileScript(LLViewerObject* object, LLViewerInventoryItem* item,
        const std::string& target, LLEventPump& parent_pump, LLSD& result)
    {
        LLEventMailDrop pump(parent_pump.getName() + "-upload-" + LLUUID::generateNewID().asString(), true);
        LLPointer<LLViewerObject> keep_alive = object;
        if (!canModifyTarget(object) || !item || item->getAssetUUID().isNull())
        {
            return false;
        }
        const std::string url = object->getRegion()->getCapability("UpdateScriptTask");
        if (url.empty())
        {
            return false;
        }
        LLResourceUploadInfo::ptr_t upload = std::make_shared<ExistingScriptAssetUpload>(
            object->getID(), item->getUUID(), item->getAssetUUID(), target,
            boost::bind(&uploadSucceeded, pump.getName(), _4),
            boost::bind(&uploadFailed, pump.getName(), _3, _4));
        LLViewerAssetUpload::EnqueueInventoryUpload(url, upload);
        result = llcoro::suspendUntilEventOnWithTimeout(pump, INVENTORY_TIMEOUT,
            LLSDMap("timeout", true));
        return !result.has("timeout") && result["compiled"].asBoolean() &&
            result["new_asset"].asUUID().notNull();
    }

    void setScriptRunning(LLViewerObject* object, const LLUUID& item_id, bool running)
    {
        if (!canModifyTarget(object)) return;
        LLMessageSystem* msg = gMessageSystem;
        msg->newMessageFast(_PREHASH_SetScriptRunning);
        msg->nextBlockFast(_PREHASH_AgentData);
        msg->addUUIDFast(_PREHASH_AgentID, gAgent.getID());
        msg->addUUIDFast(_PREHASH_SessionID, gAgent.getSessionID());
        msg->nextBlockFast(_PREHASH_Script);
        msg->addUUIDFast(_PREHASH_ObjectID, object->getID());
        msg->addUUIDFast(_PREHASH_ItemID, item_id);
        msg->addBOOLFast(_PREHASH_Running, running);
        msg->sendReliable(object->getRegion()->getHost());
    }

    bool fetchInventory(LLViewerObject* object, LLEventPump& parent_pump,
        LLInventoryObject::object_list_t& inventory, bool force_server = false)
    {
        LLEventMailDrop pump(parent_pump.getName() + "-inventory-" + LLUUID::generateNewID().asString(), true);
        LLPointer<LLViewerObject> keep_alive = object;
        if (!object || object->isDead() || !object->getRegion())
        {
            return false;
        }
        if (force_server)
        {
            object->dirtyInventory();
        }

        std::shared_ptr<InventoryFetcher> fetcher = std::make_shared<InventoryFetcher>(pump, object);
        fetcher->fetch();
        LLSD result = llcoro::suspendUntilEventOnWithTimeout(pump, INVENTORY_TIMEOUT,
            LLSDMap("timeout", true));
        if (result.has("timeout"))
        {
            return false;
        }
        inventory.assign(fetcher->inventory().begin(), fetcher->inventory().end());
        return true;
    }

    LLPointer<LLViewerInventoryItem> findNewItem(
        const LLInventoryObject::object_list_t& before,
        const LLInventoryObject::object_list_t& after,
        const LLViewerInventoryItem* expected)
    {
        std::set<LLUUID> ids;
        for (const LLPointer<LLInventoryObject>& item : before)
        {
            ids.insert(item->getUUID());
        }
        for (const LLPointer<LLInventoryObject>& object : after)
        {
            LLViewerInventoryItem* item = dynamic_cast<LLViewerInventoryItem*>(object.get());
            if (item && item->getType() == expected->getType() &&
                item->getName() == expected->getName() &&
                item->getAssetUUID() == expected->getAssetUUID() &&
                ids.find(item->getUUID()) == ids.end())
            {
                return item;
            }
        }
        return nullptr;
    }

    LLPointer<LLViewerInventoryItem> copyTaskItemToAgent(LLViewerObject* source,
        const LLViewerInventoryItem* source_item, const LLUUID& folder_id)
    {
        LLPointer<LLViewerObject> keep_alive = source;
        if (!canModifyTarget(source) || !source_item || folder_id.isNull())
        {
            return nullptr;
        }

        std::set<LLUUID> before;
        LLInventoryModel::cat_array_t* categories = nullptr;
        LLInventoryModel::item_array_t* items = nullptr;
        gInventory.getDirectDescendentsOf(folder_id, categories, items);
        if (items)
        {
            for (const LLPointer<LLViewerInventoryItem>& item : *items)
            {
                before.insert(item->getUUID());
            }
        }

        source->moveInventory(folder_id, source_item->getUUID());
        for (F32 elapsed = 0.f; elapsed < INVENTORY_TIMEOUT; elapsed += 0.25f)
        {
            llcoro::suspendUntilTimeout(0.25f);
            categories = nullptr;
            items = nullptr;
            gInventory.getDirectDescendentsOf(folder_id, categories, items);
            if (!items)
            {
                continue;
            }
            for (const LLPointer<LLViewerInventoryItem>& item : *items)
            {
                if (before.find(item->getUUID()) == before.end() &&
                    item->getType() == source_item->getType() &&
                    item->getName() == source_item->getName() &&
                    item->getAssetUUID() == source_item->getAssetUUID() && item->isFinished())
                {
                    return item;
                }
            }
        }
        return nullptr;
    }

    std::string contentKey(const std::string& name, LLAssetType::EType type)
    {
        return llformat("%d|", static_cast<S32>(type)) + name;
    }

    std::string assetTypeName(LLAssetType::EType type)
    {
        std::string name = LLAssetType::lookupHumanReadable(type);
        return name.empty() ? LLAssetType::lookup(type) : name;
    }
}

FSMassObjectEdit::FSMassObjectEdit(const LLSD& key) : LLFloater(key)
{
}

FSMassObjectEdit::~FSMassObjectEdit()
{
    gAgent.setFSMassObjectEditActive(false);
    gIdleCallbacks.deleteFunction(onIdle, this);
    for (const auto& connection : mNameCacheConnections)
    {
        connection.second.disconnect();
    }
    if (LLContextMenu* menu = mContextMenuHandle.get())
    {
        menu->die();
    }
}

bool FSMassObjectEdit::postBuild()
{
    mNameFilter = getChild<LLLineEditor>("name_filter");
    mCreatorFilter = getChild<LLLineEditor>("creator_filter");
    mOwnerFilter = getChild<LLLineEditor>("owner_filter");
    mContentNameFilter = getChild<LLLineEditor>("content_name_filter");
    mContentTypeFilter = getChild<LLLineEditor>("content_type_filter");
    mContentCountFilter = getChild<LLLineEditor>("content_count_filter");
    mTargetList = getChild<LLScrollListCtrl>("target_list");
    mSourceList = getChild<LLScrollListCtrl>("source_list");
    mTargetContentsList = getChild<LLScrollListCtrl>("target_contents_list");
    mOccurrenceList = getChild<LLScrollListCtrl>("occurrence_list");
    mSourceLabel = getChild<LLTextBox>("source_label");
    mTargetContentsLabel = getChild<LLTextBox>("target_contents_label");
    mOccurrenceLabel = getChild<LLTextBox>("occurrence_label");
    mOperationScopeLabel = getChild<LLTextBox>("operation_scope_label");
    mStatusText = getChild<LLTextBox>("status_text");
    mProgressBar = getChild<LLProgressBar>("scan_progress");
    mAddButton = getChild<LLButton>("add_btn");
    mReplaceButton = getChild<LLButton>("replace_btn");
    mDeleteButton = getChild<LLButton>("delete_btn");
    mRefreshOccurrencesButton = getChild<LLButton>("refresh_occurrences_btn");

    getChild<LLButton>("refresh_btn")->setCommitCallback(
        boost::bind(&FSMassObjectEdit::refreshObjects, this));
    getChild<LLButton>("use_source_btn")->setCommitCallback(
        boost::bind(&FSMassObjectEdit::useSelectedSource, this));
    getChild<LLButton>("scan_contents_btn")->setCommitCallback(
        boost::bind(&FSMassObjectEdit::scanTargetContents, this));
    mRefreshOccurrencesButton->setCommitCallback(
        boost::bind(&FSMassObjectEdit::refreshOccurrenceInventories, this));
    getChild<LLButton>("select_all_targets_btn")->setCommitCallback(
        boost::bind(&FSMassObjectEdit::selectAllTargets, this));
    getChild<LLButton>("clear_targets_btn")->setCommitCallback(
        boost::bind(&FSMassObjectEdit::clearTargetSelection, this));
    mAddButton->setCommitCallback(boost::bind(&FSMassObjectEdit::beginOperation, this, Operation::ADD));
    mReplaceButton->setCommitCallback(boost::bind(&FSMassObjectEdit::beginOperation, this, Operation::REPLACE));
    mDeleteButton->setCommitCallback(boost::bind(&FSMassObjectEdit::beginOperation, this, Operation::DELETE_ITEMS));
    mTargetList->setCommitCallback(boost::bind(&FSMassObjectEdit::updateButtons, this));
    mSourceList->setCommitCallback(boost::bind(&FSMassObjectEdit::updateButtons, this));
    mTargetContentsList->setCommitCallback(boost::bind(&FSMassObjectEdit::refreshOccurrenceList, this));
    mOccurrenceList->setCommitCallback(boost::bind(&FSMassObjectEdit::updateButtons, this));
    mNameFilter->setKeystrokeCallback(
        boost::bind(&FSMassObjectEdit::requestTargetListRebuild, this), nullptr);
    mCreatorFilter->setKeystrokeCallback(
        boost::bind(&FSMassObjectEdit::requestTargetListRebuild, this), nullptr);
    mOwnerFilter->setKeystrokeCallback(
        boost::bind(&FSMassObjectEdit::requestTargetListRebuild, this), nullptr);
    mContentNameFilter->setKeystrokeCallback(
        boost::bind(&FSMassObjectEdit::requestTargetContentsListRefresh, this), nullptr);
    mContentTypeFilter->setKeystrokeCallback(
        boost::bind(&FSMassObjectEdit::requestTargetContentsListRefresh, this), nullptr);
    mContentCountFilter->setKeystrokeCallback(
        boost::bind(&FSMassObjectEdit::requestTargetContentsListRefresh, this), nullptr);
    mTargetList->setRightMouseDownCallback(boost::bind(
        &FSMassObjectEdit::onObjectListRightClick, this, _1, _2, _3, _4, mTargetList));
    mOccurrenceList->setRightMouseDownCallback(boost::bind(
        &FSMassObjectEdit::onObjectListRightClick, this, _1, _2, _3, _4, mOccurrenceList));
    mTargetContentsList->sortByColumn("name", true);
    mTargetContentsList->setAlternateSort();
    mTargetList->sortByColumn("name", true);
    mOccurrenceList->sortByColumn("name", true);
    mProgressBar->setValue(0.0);
    updateButtons();
    return LLFloater::postBuild();
}

void FSMassObjectEdit::draw()
{
    LLFloater::draw();

    // Full-region interest mode can be restored by another floater or a
    // region transition. Reclaim it while this tool is open, as Scene
    // Explorer does, and periodically discover objects that streamed in
    // after the initial snapshot.
    if (gAgent.getInterestListMode() != IL_MODE_360)
    {
        gAgent.changeInterestListMode(IL_MODE_360);
    }
    LLViewerRegion* current_region = gAgent.getRegion();
    if (!mBusy && current_region && mRegionID.notNull() &&
        current_region->getRegionID() != mRegionID && !mWaitingForInterestList)
    {
        mRegionID = current_region->getRegionID();
        mInterestListTimer.reset();
        mWaitingForInterestList = true;
        gIdleCallbacks.addFunction(onIdle, this);
        setStatus("Region changed; waiting for the new region object stream...");
        updateButtons();
    }
    if (!mBusy && !mWaitingForInterestList &&
        mReconcileTimer.getElapsedTimeF32() >= OBJECT_RECONCILE_INTERVAL)
    {
        mReconcileTimer.reset();
        reconcileObjects();
    }

    if (LLViewerObject* object = gObjectList.findObject(mBeaconObjectID))
    {
        static LLCachedControl<S32> beacon_line_width(gSavedSettings, "DebugBeaconLineWidth");
        static LLUIColor beacon_color = LLUIColorTable::getInstance()->getColor("AreaSearchBeaconColor");
        static LLUIColor beacon_text_color = LLUIColorTable::getInstance()->getColor("PathfindingDefaultBeaconTextColor");
        const auto found = mObjects.find(mBeaconObjectID);
        const std::string name = found != mObjects.end() ? found->second.name : "Mass edit target";
        gObjectList.addDebugBeacon(object->getPositionAgent(), name,
            beacon_color, beacon_text_color, beacon_line_width);
    }
}

void FSMassObjectEdit::onOpen(const LLSD&)
{
    if (mBusy) return;
    gAgent.setFSMassObjectEditActive(true);
    gAgent.changeInterestListMode(IL_MODE_360);

    // The InterestList capability does not report completion. Give the sim a
    // short head start before taking the first complete-region snapshot.
    mInterestListTimer.reset();
    mReconcileTimer.reset();
    mWaitingForInterestList = true;
    gIdleCallbacks.addFunction(onIdle, this);
    setStatus("Requesting all region objects...");
    updateButtons();
}

void FSMassObjectEdit::onClose(bool app_quitting)
{
    mWaitingForInterestList = false;
    gAgent.setFSMassObjectEditActive(false);
    if (!app_quitting)
    {
        gAgent.changeInterestListMode(IL_MODE_DEFAULT);
    }
    LLFloater::onClose(app_quitting);
}

void FSMassObjectEdit::setStatus(const std::string& status)
{
    if (mStatusText)
    {
        mStatusText->setText(status);
    }
}

void FSMassObjectEdit::updateButtons()
{
    setCanClose(!mBusy);
    getChild<LLButton>("use_source_btn")->setEnabled(!mBusy);
    const uuid_vec_t operation_targets = getOperationTargets();
    const bool targets = !operation_targets.empty();
    const bool source = mSourceList && !mSourceList->getAllSelected().empty();
    const bool contents = mTargetContentsList && !mTargetContentsList->getAllSelected().empty();
    const bool ready = !mBusy && !mScanning && !mWaitingForInterestList && !mTargetRebuildPending &&
        !mTargetFilterRefreshPending && !mContentFilterRefreshPending &&
        !mTargetContentsRebuildPending && !mOccurrenceRebuildPending;
    getChild<LLButton>("refresh_btn")->setEnabled(ready);
    getChild<LLButton>("select_all_targets_btn")->setEnabled(ready &&
        mTargetList && mTargetList->getItemCount() > 0);
    getChild<LLButton>("clear_targets_btn")->setEnabled(ready &&
        mTargetList && !mTargetList->getAllSelected().empty());
    getChild<LLButton>("scan_contents_btn")->setEnabled(ready &&
        mTargetList && !mTargetList->getAllSelected().empty());
    mAddButton->setEnabled(ready && targets && source);
    mReplaceButton->setEnabled(ready && targets && source);
    mDeleteButton->setEnabled(ready && targets && contents);
    mRefreshOccurrencesButton->setEnabled(ready && mOccurrenceList &&
        mOccurrenceList->getItemCount() > 0);
    if (mOperationScopeLabel)
    {
        std::string scope = "selected target prims";
        if (mOccurrenceList && !mOccurrenceList->getAllSelected().empty())
        {
            scope = "selected occurrences";
        }
        mOperationScopeLabel->setText(llformat("Operation scope: %d %s",
            static_cast<S32>(operation_targets.size()), scope.c_str()));
    }
}

void FSMassObjectEdit::refreshObjects()
{
    if (mBusy || mScanning || mWaitingForInterestList || mTargetRebuildPending)
    {
        return;
    }
    LLViewerRegion* region = gAgent.getRegion();
    if (!region)
    {
        return;
    }

    mTargetContentKeys.clear();
    mTargetContentsRebuildPending = false;
    mTargetContentsRebuildClearing = false;
    mTargetContentsRebuildDirty = false;
    mTargetContentsRebuildKeys.clear();
    mTargetContentsRebuildSelection.clear();
    mOccurrenceRebuildPending = false;
    mOccurrenceRebuildClearing = false;
    mOccurrenceRebuildDirty = false;
    mOccurrenceRebuildIDs.clear();
    mOccurrenceRebuildSelection.clear();
    refreshTargetContentsList();
    refreshOccurrenceList();
    mOccurrenceLabel->setText(LLStringExplicit("4. Select content to see where it exists"));
    mProgressBar->setValue(0.0);
    mCreatorRefreshPending = false;
    gIdleCallbacks.deleteFunction(onIdle, this);
    for (const auto& connection : mNameCacheConnections)
    {
        connection.second.disconnect();
    }
    mNameCacheConnections.clear();
    mObjects.clear();
    mCreatorObjectIDs.clear();
    mOwnerObjectIDs.clear();
    mPropertyRequestIDs.clear();
    mPropertyRequestIndex = 0;
    mPropertyRequestsInFlight = 0;
    mPropertyRetryCount = 0;
    mRegionID = region->getRegionID();

    // Clear the previous scan over multiple frames, then append each prim as
    // its properties arrive so discovery is visible instead of appearing
    // frozen until the entire region has replied.
    mTargetRebuildSelection.clear();
    for (LLScrollListItem* row : mTargetList->getAllSelected())
    {
        mTargetRebuildSelection.insert(row->getValue().asUUID());
    }
    mTargetRebuildIDs.clear();
    mTargetRebuildIndex = 0;
    mTargetRebuildEditableCount = 0;
    mTargetRebuildClearing = mTargetList->getItemCount() > 0;
    mTargetRebuildPending = true;

    for (S32 i = 0; i < gObjectList.getNumObjects(); ++i)
    {
        LLViewerObject* object = gObjectList.getObject(i);
        if (!object || object->isDead() || object->getRegion() != region || object->isAvatar() ||
            object->isAttachment() || object->getPCode() == LLViewerObject::LL_VO_SURFACE_PATCH ||
            !object->mbCanSelect)
        {
            continue;
        }
        ObjectInfo info;
        info.id = object->getID();
        info.local_id = object->getLocalID();
        LLViewerObject* root = object->getRootEdit();
        info.root_id = root ? root->getID() : info.id;
        if (mObjects.emplace(info.id, info).second)
        {
            mPropertyRequestIDs.push_back(info.id);
        }
    }

    mPendingProperties = static_cast<S32>(mObjects.size());
    mScanning = mPendingProperties > 0;
    updateButtons();
    mScanTimer.reset();
    if (mScanning)
    {
        gIdleCallbacks.addFunction(onIdle, this);
        setStatus(llformat("Scanning %d current-region prims...", mPendingProperties));
    }
    else
    {
        finishObjectScan();
    }
}

void FSMassObjectEdit::reconcileObjects()
{
    LLViewerRegion* region = gAgent.getRegion();
    if (!region || region->getRegionID() != mRegionID)
    {
        return;
    }

    S32 discovered = 0;
    for (S32 i = 0; i < gObjectList.getNumObjects(); ++i)
    {
        LLViewerObject* object = gObjectList.getObject(i);
        if (!object || object->isDead() || object->getRegion() != region ||
            object->isAvatar() || object->isAttachment() ||
            object->getPCode() == LLViewerObject::LL_VO_SURFACE_PATCH ||
            !object->mbCanSelect || mObjects.find(object->getID()) != mObjects.end())
        {
            continue;
        }

        ObjectInfo info;
        info.id = object->getID();
        info.local_id = object->getLocalID();
        LLViewerObject* root = object->getRootEdit();
        info.root_id = root ? root->getID() : info.id;
        if (mObjects.emplace(info.id, info).second)
        {
            mPropertyRequestIDs.push_back(info.id);
            ++mPendingProperties;
            ++discovered;
        }
    }

    // ObjectSelect/ObjectDeselect is used to request full properties. Do not
    // disturb an object the user is actively editing; queue it once selection
    // has moved elsewhere.
    for (auto& entry : mObjects)
    {
        ObjectInfo& info = entry.second;
        if (info.property_request != PropertyRequestState::DEFERRED)
        {
            continue;
        }
        LLViewerObject* object = gObjectList.findObject(info.id);
        if (object && !object->isDead() && !object->isSelected())
        {
            info.property_request = PropertyRequestState::NEED;
            mPropertyRequestIDs.push_back(info.id);
            ++discovered;
        }
    }

    if (discovered > 0)
    {
        mPropertyRetryCount = 0;
        mScanning = true;
        mScanTimer.reset();
        gIdleCallbacks.addFunction(onIdle, this);
        setStatus(llformat("Queued %d new or deferred prims; fetching %d pending properties...",
            discovered, mPendingProperties));
        updateButtons();
    }
}

bool FSMassObjectEdit::retryUnresolvedProperties()
{
    if (mPropertyRetryCount >= MAX_PROPERTY_RETRIES)
    {
        return false;
    }

    uuid_vec_t unresolved;
    unresolved.reserve(mPendingProperties);
    for (auto& entry : mObjects)
    {
        ObjectInfo& info = entry.second;
        if (!info.received && info.property_request != PropertyRequestState::DEFERRED)
        {
            info.property_request = PropertyRequestState::NEED;
            unresolved.push_back(info.id);
        }
    }
    if (unresolved.empty())
    {
        return false;
    }

    mPropertyRequestIDs.swap(unresolved);
    mPropertyRequestIndex = 0;
    mPropertyRequestsInFlight = 0;
    ++mPropertyRetryCount;
    mScanTimer.reset();
    setStatus(llformat("Retrying %d unresolved prim properties (attempt %d/%d)...",
        mPendingProperties, mPropertyRetryCount, MAX_PROPERTY_RETRIES));
    return true;
}

void FSMassObjectEdit::requestObjectProperties(const std::vector<U32>& local_ids, bool select)
{
    LLViewerRegion* region = gAgent.getRegion();
    if (!region || region->getRegionID() != mRegionID)
    {
        return;
    }
    LLMessageSystem* msg = gMessageSystem;
    bool start = true;
    S32 blocks = 0;
    for (const U32 local_id : local_ids)
    {
        if (start)
        {
            msg->newMessageFast(select ? _PREHASH_ObjectSelect : _PREHASH_ObjectDeselect);
            msg->nextBlockFast(_PREHASH_AgentData);
            msg->addUUIDFast(_PREHASH_AgentID, gAgent.getID());
            msg->addUUIDFast(_PREHASH_SessionID, gAgent.getSessionID());
            start = false;
        }
        msg->nextBlockFast(_PREHASH_ObjectData);
        msg->addU32Fast(_PREHASH_ObjectLocalID, local_id);
        if (++blocks >= MAX_OBJECTS_PER_PACKET || msg->isSendFull(nullptr))
        {
            msg->sendReliable(region->getHost());
            start = true;
            blocks = 0;
        }
    }
    if (!start)
    {
        msg->sendReliable(region->getHost());
    }
}

void FSMassObjectEdit::processPropertyRequestQueue()
{
    if (!mScanning)
    {
        return;
    }

    LLViewerRegion* region = gAgent.getRegion();
    if (!region || region->getRegionID() != mRegionID)
    {
        mPropertyRequestIndex = mPropertyRequestIDs.size();
        mPropertyRequestsInFlight = 0;
        return;
    }

    if (mPropertyRequestsInFlight >= MAX_PROPERTY_REQUESTS_IN_FLIGHT ||
        mPropertyRequestIndex >= mPropertyRequestIDs.size())
    {
        return;
    }

    std::vector<U32> request_ids;
    request_ids.reserve(MAX_OBJECTS_PER_PACKET);
    const S32 available = MAX_PROPERTY_REQUESTS_IN_FLIGHT - mPropertyRequestsInFlight;
    const S32 batch_size = llmin(MAX_OBJECTS_PER_PACKET, available);
    while (mPropertyRequestIndex < mPropertyRequestIDs.size() &&
        static_cast<S32>(request_ids.size()) < batch_size)
    {
        const LLUUID id = mPropertyRequestIDs[mPropertyRequestIndex++];
        auto found = mObjects.find(id);
        if (found == mObjects.end() || found->second.property_request != PropertyRequestState::NEED)
        {
            continue;
        }
        LLViewerObject* object = gObjectList.findObject(id);
        if (object && object->isSelected())
        {
            found->second.property_request = PropertyRequestState::DEFERRED;
            continue;
        }
        found->second.property_request = PropertyRequestState::SENT;
        request_ids.push_back(found->second.local_id);
    }

    if (!request_ids.empty())
    {
        requestObjectProperties(request_ids, true);
        requestObjectProperties(request_ids, false);
        mPropertyRequestsInFlight += static_cast<S32>(request_ids.size());
        mScanTimer.reset();
    }
}

void FSMassObjectEdit::processObjectProperties(LLMessageSystem* msg)
{
    if (!mScanning)
    {
        return;
    }
    const S32 count = msg->getNumberOfBlocksFast(_PREHASH_ObjectData);
    for (S32 i = 0; i < count; ++i)
    {
        LLUUID id;
        msg->getUUIDFast(_PREHASH_ObjectData, _PREHASH_ObjectID, id, i);
        auto found = mObjects.find(id);
        if (found == mObjects.end() || found->second.received)
        {
            continue;
        }

        ObjectInfo& info = found->second;
        LLUUID group, last_owner;
        U32 base = 0, owner = 0, everyone = 0, group_mask = 0, next = 0;
        msg->getUUIDFast(_PREHASH_ObjectData, _PREHASH_CreatorID, info.creator_id, i);
        msg->getUUIDFast(_PREHASH_ObjectData, _PREHASH_OwnerID, info.owner_id, i);
        msg->getUUIDFast(_PREHASH_ObjectData, _PREHASH_GroupID, group, i);
        msg->getUUIDFast(_PREHASH_ObjectData, _PREHASH_LastOwnerID, last_owner, i);
        msg->getU32Fast(_PREHASH_ObjectData, _PREHASH_BaseMask, base, i);
        msg->getU32Fast(_PREHASH_ObjectData, _PREHASH_OwnerMask, owner, i);
        msg->getU32Fast(_PREHASH_ObjectData, _PREHASH_EveryoneMask, everyone, i);
        msg->getU32Fast(_PREHASH_ObjectData, _PREHASH_GroupMask, group_mask, i);
        msg->getU32Fast(_PREHASH_ObjectData, _PREHASH_NextOwnerMask, next, i);
        msg->getStringFast(_PREHASH_ObjectData, _PREHASH_Name, info.name, i);
        info.permissions.init(info.creator_id, info.owner_id, last_owner, group);
        info.permissions.initMasks(base, owner, everyone, group_mask, next);
        info.permissions.getOwnership(info.owner_id, info.group_owned);
        if (info.property_request == PropertyRequestState::SENT && mPropertyRequestsInFlight > 0)
        {
            --mPropertyRequestsInFlight;
        }
        info.property_request = PropertyRequestState::RECEIVED;
        info.received = true;
        mTargetRebuildIDs.push_back(id);
        --mPendingProperties;
        mScanTimer.reset();
    }
}

void FSMassObjectEdit::onIdle(void* userdata)
{
    FSMassObjectEdit* self = static_cast<FSMassObjectEdit*>(userdata);
    if (!self)
    {
        return;
    }
    if (self->mWaitingForInterestList &&
        self->mInterestListTimer.getElapsedTimeF32() >= 2.0f)
    {
        self->mWaitingForInterestList = false;
        self->refreshObjects();
    }
    if (self->mScanning)
    {
        self->processPropertyRequestQueue();
        const bool queue_drained =
            self->mPropertyRequestIndex >= self->mPropertyRequestIDs.size() &&
            self->mPropertyRequestsInFlight == 0;
        const bool retry_due = queue_drained ||
            self->mScanTimer.getElapsedTimeF32() >= PROPERTY_RETRY_INTERVAL;
        if (self->mPendingProperties == 0 ||
            (retry_due && !self->retryUnresolvedProperties()))
        {
            self->finishObjectScan();
        }
    }
    if (self->mTargetRebuildPending)
    {
        self->processTargetListRebuild();
    }
    if (self->mTargetContentsRebuildPending)
    {
        self->processTargetContentsListRebuild();
    }
    if (self->mOccurrenceRebuildPending)
    {
        self->processOccurrenceListRebuild();
    }
    if (self->mTargetFilterRefreshPending && !self->mTargetRebuildPending &&
        self->mTargetFilterRefreshTimer.getElapsedTimeF32() >= FILTER_REFRESH_DELAY)
    {
        self->mTargetFilterRefreshPending = false;
        self->rebuildTargetList();
    }
    if (self->mContentFilterRefreshPending && !self->mTargetContentsRebuildPending &&
        self->mContentFilterRefreshTimer.getElapsedTimeF32() >= FILTER_REFRESH_DELAY)
    {
        self->mContentFilterRefreshPending = false;
        self->refreshTargetContentsList();
    }
    if (self->mCreatorRefreshPending && !self->mTargetRebuildPending &&
        self->mCreatorRefreshTimer.getElapsedTimeF32() >= NAME_LIST_REFRESH_DELAY)
    {
        self->mCreatorRefreshPending = false;
        self->rebuildTargetList();
    }
    if (!self->mScanning && !self->mCreatorRefreshPending &&
        !self->mTargetFilterRefreshPending && !self->mContentFilterRefreshPending &&
        !self->mTargetRebuildPending && !self->mTargetContentsRebuildPending &&
        !self->mOccurrenceRebuildPending)
    {
        gIdleCallbacks.deleteFunction(onIdle, self);
    }
}

void FSMassObjectEdit::finishObjectScan()
{
    mScanning = false;
    mPropertyRequestsInFlight = 0;
    mCreatorObjectIDs.clear();
    mOwnerObjectIDs.clear();
    for (auto& entry : mObjects)
    {
        ObjectInfo& info = entry.second;
        if (!info.received || !gAgent.allowOperation(PERM_MODIFY, info.permissions,
            GP_OBJECT_MANIPULATE))
        {
            continue;
        }

        if (info.creator_id.notNull())
        {
            mCreatorObjectIDs[info.creator_id].push_back(info.id);
        }
        if (info.owner_id.notNull())
        {
            mOwnerObjectIDs[info.owner_id].push_back(info.id);
        }

        LLAvatarName avatar_name;
        if (LLAvatarNameCache::get(info.creator_id, &avatar_name))
        {
            info.creator_name = avatar_name.getCompleteName();
            info.creator_search_name = avatarSearchTerms(avatar_name);
        }
        else if (info.creator_id.notNull() &&
            mNameCacheConnections.find(info.creator_id) == mNameCacheConnections.end())
        {
            mNameCacheConnections.emplace(info.creator_id,
                LLAvatarNameCache::get(info.creator_id,
                    boost::bind(&FSMassObjectEdit::avatarNameCallback, this, _1, _2)));
        }

        if (info.group_owned)
        {
            bool is_group = false;
            if (!gCacheName->getIfThere(info.owner_id, info.owner_name, is_group) &&
                info.owner_id.notNull() &&
                mNameCacheConnections.find(info.owner_id) == mNameCacheConnections.end())
            {
                mNameCacheConnections.emplace(info.owner_id,
                    gCacheName->get(info.owner_id, true,
                        boost::bind(&FSMassObjectEdit::ownerNameCallback, this, _1, _2)));
            }
            else
            {
                info.owner_search_name = info.owner_name;
            }
        }
        else if (LLAvatarNameCache::get(info.owner_id, &avatar_name))
        {
            info.owner_name = avatar_name.getCompleteName();
            info.owner_search_name = avatarSearchTerms(avatar_name);
        }
        else if (info.owner_id.notNull() &&
            mNameCacheConnections.find(info.owner_id) == mNameCacheConnections.end())
        {
            mNameCacheConnections.emplace(info.owner_id,
                LLAvatarNameCache::get(info.owner_id,
                    boost::bind(&FSMassObjectEdit::avatarNameCallback, this, _1, _2)));
        }
    }
    rebuildTargetList();
}

void FSMassObjectEdit::rebuildTargetList()
{
    if (!mTargetList)
    {
        return;
    }

    if (!mTargetRebuildPending)
    {
        mTargetRebuildSelection.clear();
    }
    for (LLScrollListItem* row : mTargetList->getAllSelected())
    {
        mTargetRebuildSelection.insert(row->getValue().asUUID());
    }

    mTargetNameFilter = mNameFilter ? mNameFilter->getText() : std::string();
    mTargetCreatorFilter = mCreatorFilter ? mCreatorFilter->getText() : std::string();
    mTargetOwnerFilter = mOwnerFilter ? mOwnerFilter->getText() : std::string();
    LLStringUtil::trim(mTargetNameFilter);
    LLStringUtil::trim(mTargetCreatorFilter);
    LLStringUtil::trim(mTargetOwnerFilter);
    LLStringUtil::toLower(mTargetNameFilter);
    LLStringUtil::toLower(mTargetCreatorFilter);
    LLStringUtil::toLower(mTargetOwnerFilter);

    mTargetRebuildIDs.clear();
    mTargetRebuildIDs.reserve(mObjects.size());
    for (const auto& entry : mObjects)
    {
        mTargetRebuildIDs.push_back(entry.first);
    }
    mTargetRebuildIndex = 0;
    mTargetRebuildEditableCount = 0;
    mTargetRebuildClearing = mTargetList->getItemCount() > 0;
    mTargetRebuildPending = true;
    setStatus(llformat("Updating target list: 0/%d prims...",
        static_cast<S32>(mTargetRebuildIDs.size())));
    updateButtons();
    gIdleCallbacks.deleteFunction(onIdle, this);
    gIdleCallbacks.addFunction(onIdle, this);
}

void FSMassObjectEdit::requestTargetListRebuild()
{
    mTargetFilterRefreshPending = true;
    mTargetFilterRefreshTimer.reset();
    gIdleCallbacks.addFunction(onIdle, this);
    updateButtons();
}

void FSMassObjectEdit::processTargetListRebuild()
{
    LLFrameTimer budget;
    budget.reset();
    while (mTargetRebuildClearing && mTargetList->getItemCount() > 0 &&
        budget.getElapsedTimeF32() < TARGET_LIST_UPDATE_BUDGET)
    {
        mTargetList->deleteSingleItem(mTargetList->getItemCount() - 1);
    }
    if (mTargetRebuildClearing)
    {
        if (mTargetList->getItemCount() > 0)
        {
            return;
        }
        mTargetRebuildClearing = false;
        budget.reset();
    }

    while (mTargetRebuildIndex < mTargetRebuildIDs.size() &&
        budget.getElapsedTimeF32() < TARGET_LIST_UPDATE_BUDGET)
    {
        const LLUUID id = mTargetRebuildIDs[mTargetRebuildIndex++];
        const auto found = mObjects.find(id);
        if (found == mObjects.end())
        {
            continue;
        }
        const ObjectInfo& info = found->second;
        if (!info.received || !gAgent.allowOperation(PERM_MODIFY, info.permissions,
            GP_OBJECT_MANIPULATE))
        {
            continue;
        }
        ++mTargetRebuildEditableCount;

        std::string object_name = info.name;
        std::string creator = info.creator_name.empty() ?
            info.creator_id.asString() : info.creator_name;
        std::string owner = info.owner_name.empty() ?
            info.owner_id.asString() : info.owner_name;
        std::string creator_search = (info.creator_search_name.empty() ? creator :
            info.creator_search_name) + " " + info.creator_id.asString();
        std::string owner_search = (info.owner_search_name.empty() ? owner :
            info.owner_search_name) + " " + info.owner_id.asString();
        if (info.owner_id == gAgent.getID())
        {
            owner_search += " me";
        }
        LLStringUtil::toLower(object_name);
        LLStringUtil::toLower(creator_search);
        LLStringUtil::toLower(owner_search);
        if ((!mTargetNameFilter.empty() && object_name.find(mTargetNameFilter) == std::string::npos) ||
            (!mTargetCreatorFilter.empty() && creator_search.find(mTargetCreatorFilter) == std::string::npos) ||
            (!mTargetOwnerFilter.empty() && owner_search.find(mTargetOwnerFilter) == std::string::npos))
        {
            continue;
        }

        LLSD row;
        row["id"] = info.id;
        row["columns"][0]["column"] = "name";
        row["columns"][0]["value"] = info.name;
        row["columns"][1]["column"] = "creator";
        row["columns"][1]["value"] = creator;
        row["columns"][2]["column"] = "owner";
        row["columns"][2]["value"] = info.owner_id == gAgent.getID() ? "Me" :
            owner;
        LLScrollListItem* added = mTargetList->addElement(row, ADD_BOTTOM);
        if (added && mTargetRebuildSelection.find(info.id) != mTargetRebuildSelection.end())
        {
            added->setSelected(true);
        }
    }
    if (mTargetRebuildIndex < mTargetRebuildIDs.size())
    {
        setStatus(llformat("Updating target list: %d/%d prims...",
            static_cast<S32>(mTargetRebuildIndex),
            static_cast<S32>(mTargetRebuildIDs.size())));
        return;
    }

    if (mScanning)
    {
        setStatus(llformat("Discovering target prims: %d/%d replies, %d editable shown...",
            static_cast<S32>(mObjects.size()) - mPendingProperties,
            static_cast<S32>(mObjects.size()), mTargetRebuildEditableCount));
        return;
    }

    mTargetRebuildPending = false;
    mTargetList->setNeedsSort();
    setStatus(llformat("Showing %d of %d editable prims.",
        mTargetList->getItemCount(), mTargetRebuildEditableCount));
    updateButtons();
}

void FSMassObjectEdit::avatarNameCallback(const LLUUID& id, const LLAvatarName& av_name)
{
    const std::string complete_name = av_name.getCompleteName();
    if (const auto creators = mCreatorObjectIDs.find(id); creators != mCreatorObjectIDs.end())
    {
        for (const LLUUID& object_id : creators->second)
        {
            if (auto object = mObjects.find(object_id); object != mObjects.end())
            {
                object->second.creator_name = complete_name;
                object->second.creator_search_name = avatarSearchTerms(av_name);
            }
        }
    }
    if (const auto owners = mOwnerObjectIDs.find(id); owners != mOwnerObjectIDs.end())
    {
        for (const LLUUID& object_id : owners->second)
        {
            if (auto object = mObjects.find(object_id);
                object != mObjects.end() && !object->second.group_owned)
            {
                object->second.owner_name = complete_name;
                object->second.owner_search_name = avatarSearchTerms(av_name);
            }
        }
    }
    if (auto found = mNameCacheConnections.find(id); found != mNameCacheConnections.end())
    {
        found->second.disconnect();
        mNameCacheConnections.erase(found);
    }
    if (!mCreatorRefreshPending)
    {
        mCreatorRefreshPending = true;
        gIdleCallbacks.addFunction(onIdle, this);
    }
    mCreatorRefreshTimer.reset();
}

void FSMassObjectEdit::ownerNameCallback(const LLUUID& id, const std::string& name)
{
    if (const auto owners = mOwnerObjectIDs.find(id); owners != mOwnerObjectIDs.end())
    {
        for (const LLUUID& object_id : owners->second)
        {
            if (auto object = mObjects.find(object_id);
                object != mObjects.end() && object->second.group_owned)
            {
                object->second.owner_name = name;
                object->second.owner_search_name = name;
            }
        }
    }
    if (auto found = mNameCacheConnections.find(id); found != mNameCacheConnections.end())
    {
        found->second.disconnect();
        mNameCacheConnections.erase(found);
    }
    if (!mCreatorRefreshPending)
    {
        mCreatorRefreshPending = true;
        gIdleCallbacks.addFunction(onIdle, this);
    }
    mCreatorRefreshTimer.reset();
}

uuid_vec_t FSMassObjectEdit::getSelectedTargets() const
{
    uuid_vec_t ids;
    for (LLScrollListItem* row : mTargetList->getAllSelected())
    {
        ids.push_back(row->getValue().asUUID());
    }
    return ids;
}

uuid_vec_t FSMassObjectEdit::getOperationTargets() const
{
    std::set<LLUUID> ids;
    if (mOccurrenceList)
    {
        for (LLScrollListItem* row : mOccurrenceList->getAllSelected())
        {
            ids.insert(row->getValue().asUUID());
        }
    }
    if (ids.empty())
    {
        const uuid_vec_t selected_targets = getSelectedTargets();
        ids.insert(selected_targets.begin(), selected_targets.end());
    }
    return uuid_vec_t(ids.begin(), ids.end());
}

void FSMassObjectEdit::useSelectedSource()
{
    if (mBusy) return;
    LLViewerObject* object = LLSelectMgr::getInstance()->getSelection()->getFirstObject();
    if (!canModifyTarget(object))
    {
        setStatus("Select one rezzed prim, then click Use Selected Prim as Source.");
        return;
    }
    mBusy = true;
    updateButtons();
    mSourceObjectID = object->getID();
    const auto source_info = mObjects.find(mSourceObjectID);
    mSourceLabel->setText(source_info != mObjects.end() ?
        source_info->second.name : object->getID().asString());
    mSourceList->deleteAllItems();
    mSourceItems.clear();
    LLCoros::instance().launch("FSMassObjectEdit::fetchSourceInventory",
        boost::bind(&FSMassObjectEdit::fetchSourceInventoryCoro, this, mSourceObjectID));
}

void FSMassObjectEdit::fetchSourceInventoryCoro(LLUUID source_id)
{
    LLUUID pump_id = LLUUID::generateNewID();
    LLEventMailDrop pump("MassObjectSource-" + pump_id.asString(), true);
    LLViewerObject* object = gObjectList.findObject(source_id);
    LLInventoryObject::object_list_t inventory;
    if (!fetchInventory(object, pump, inventory))
    {
        mBusy = false;
        updateButtons();
        setStatus("Could not retrieve source inventory.");
        return;
    }

    for (const LLPointer<LLInventoryObject>& inv_object : inventory)
    {
        LLViewerInventoryItem* item = dynamic_cast<LLViewerInventoryItem*>(inv_object.get());
        if (!item)
        {
            continue;
        }
        mSourceItems[item->getUUID()] = new LLViewerInventoryItem(item);
        LLSD row;
        row["id"] = item->getUUID();
        row["columns"][0]["column"] = "name";
        row["columns"][0]["value"] = item->getName();
        row["columns"][1]["column"] = "type";
        row["columns"][1]["value"] = assetTypeName(item->getType());
        row["columns"][2]["column"] = "permissions";
        row["columns"][2]["value"] = item->getPermissions().allowCopyBy(
            gAgent.getID(), gAgent.getGroupID()) ? "Yes" : "No";
        mSourceList->addElement(row, ADD_BOTTOM);
    }
    mSourceLabel->setText(llformat("%d source items", mSourceList->getItemCount()));
    mBusy = false;
    setStatus("Source inventory loaded.");
    updateButtons();
}

void FSMassObjectEdit::scanTargetContents()
{
    const uuid_vec_t targets = getSelectedTargets();
    if (mBusy || mScanning || mTargetRebuildPending || targets.empty())
    {
        return;
    }
    mBusy = true;
    updateButtons();
    setStatus(llformat("Starting target-content scan for %d prims...",
        static_cast<S32>(targets.size())));
    LLCoros::instance().launch("FSMassObjectEdit::scanTargetContents",
        boost::bind(&FSMassObjectEdit::scanTargetContentsCoro, this, targets, false));
}

void FSMassObjectEdit::refreshOccurrenceInventories()
{
    uuid_vec_t targets;
    if (mOccurrenceList)
    {
        for (LLScrollListItem* row : mOccurrenceList->getAllData())
        {
            targets.push_back(row->getValue().asUUID());
        }
    }
    if (mBusy || targets.empty())
    {
        return;
    }

    mBusy = true;
    updateButtons();
    setStatus(llformat("Refreshing inventory for %d occurrence objects...",
        static_cast<S32>(targets.size())));
    LLCoros::instance().launch("FSMassObjectEdit::refreshOccurrenceInventories",
        boost::bind(&FSMassObjectEdit::scanTargetContentsCoro, this, targets, true));
}

void FSMassObjectEdit::scanTargetContentsCoro(uuid_vec_t targets, bool refresh_existing)
{
    LLUUID pump_id = LLUUID::generateNewID();
    const std::string done_pump_name = "MassObjectTargetsDone-" + pump_id.asString();
    LLEventMailDrop done_pump(done_pump_name, true);
    if (!refresh_existing)
    {
        mTargetContentKeys.clear();
        mTargetContentsRebuildPending = false;
        mTargetContentsRebuildClearing = false;
        mTargetContentsRebuildDirty = false;
        mTargetContentsRebuildKeys.clear();
        refreshTargetContentsList();
    }
    mTargetScanTotal = static_cast<S32>(targets.size());
    mTargetScanProcessed = 0;
    mTargetScanSucceeded = 0;
    mTargetScanFailed = 0;
    mTargetContentRefreshTimer.reset();
    mProgressBar->setValue(0.0);
    if (!refresh_existing)
    {
        mOccurrenceRebuildPending = false;
        mOccurrenceRebuildClearing = false;
        mOccurrenceRebuildDirty = false;
        mOccurrenceRebuildIDs.clear();
        refreshOccurrenceList();
        mOccurrenceLabel->setText(LLStringExplicit("Objects containing the selected content"));
    }

    uuid_vec_t pass_targets = targets;
    S32 retries_used = 0;
    for (S32 attempt = 0; !pass_targets.empty() && attempt <= TARGET_SCAN_RETRIES; ++attempt)
    {
        const bool retry_pass = attempt > 0;
        if (retry_pass)
        {
            retries_used = attempt;
            setStatus(llformat("Retrying %d unavailable target inventories (attempt %d/%d)...",
                static_cast<S32>(pass_targets.size()), attempt, TARGET_SCAN_RETRIES));
            llcoro::suspendUntilTimeout(TARGET_SCAN_RETRY_DELAY * static_cast<F32>(attempt));
        }

        mTargetScanRetryIDs.clear();
        const S32 pass_count = static_cast<S32>(pass_targets.size());
        static LLCachedControl<S32> inventory_workers(gSavedSettings,
            "FSMassObjectEditInventoryWorkers", TARGET_SCAN_WORKERS);
        const S32 worker_count = llmin(llclamp(static_cast<S32>(inventory_workers), 1, 8),
            pass_count);
        std::vector<uuid_vec_t> work(static_cast<size_t>(worker_count));
        for (S32 i = 0; i < pass_count; ++i)
        {
            work[static_cast<size_t>(i % worker_count)].push_back(
                pass_targets[static_cast<size_t>(i)]);
        }

        for (S32 i = 0; i < worker_count; ++i)
        {
            LLCoros::instance().launch("FSMassObjectEdit::scanTargetContentsWorker",
                boost::bind(&FSMassObjectEdit::scanTargetContentsWorkerCoro, this,
                    work[static_cast<size_t>(i)], done_pump_name,
                    refresh_existing, retry_pass));
        }
        for (S32 i = 0; i < worker_count; ++i)
        {
            llcoro::suspendUntilEventOn(done_pump);
        }

        mTargetScanFailed = static_cast<S32>(mTargetScanRetryIDs.size());
        mTargetScanSucceeded = mTargetScanTotal - mTargetScanFailed;
        pass_targets = mTargetScanRetryIDs;
        if (retry_pass)
        {
            refreshTargetContentsList();
        }
    }

    refreshTargetContentsList();
    mProgressBar->setValue(100.0);
    mBusy = false;
    setStatus(llformat(refresh_existing ?
        "Refreshed %d of %d occurrence objects; %d unavailable after %d retries. Found %d unique contents." :
        "Scanned %d of %d targets; %d unavailable after %d retries. Found %d unique contents.",
        mTargetScanSucceeded, mTargetScanTotal, mTargetScanFailed,
        retries_used,
        static_cast<S32>(mTargetContentKeys.size())));
    updateButtons();
}

void FSMassObjectEdit::scanTargetContentsWorkerCoro(uuid_vec_t targets,
    std::string done_pump_name, bool refresh_existing, bool retry_pass)
{
    LLUUID pump_id = LLUUID::generateNewID();
    LLEventMailDrop pump("MassObjectTargetWorker-" + pump_id.asString(), true);
    for (const LLUUID& id : targets)
    {
        LLInventoryObject::object_list_t inventory;
        if (fetchInventory(gObjectList.findObject(id), pump, inventory,
            refresh_existing || retry_pass))
        {
            if (!retry_pass)
            {
                ++mTargetScanSucceeded;
            }
            if (refresh_existing)
            {
                removeTargetContents(id);
            }
            std::set<std::string> seen;
            for (const LLPointer<LLInventoryObject>& object : inventory)
            {
                LLViewerInventoryItem* item = dynamic_cast<LLViewerInventoryItem*>(object.get());
                if (!item)
                {
                    continue;
                }
                const std::string key = contentKey(item->getName(), item->getType());
                if (seen.insert(key).second)
                {
                    ContentKey& content = mTargetContentKeys[key];
                    content.name = item->getName();
                    content.type = item->getType();
                    content.targets.insert(id);
                    content.count = static_cast<S32>(content.targets.size());
                }
            }
        }
        else
        {
            mTargetScanRetryIDs.push_back(id);
            if (!retry_pass)
            {
                ++mTargetScanFailed;
            }
        }
        if (!retry_pass)
        {
            ++mTargetScanProcessed;
            updateTargetScanProgress();
        }
    }
    LLEventPumps::instance().post(done_pump_name, LLSDMap("done", true));
}

void FSMassObjectEdit::removeTargetContents(const LLUUID& target_id)
{
    for (auto iter = mTargetContentKeys.begin(); iter != mTargetContentKeys.end();)
    {
        ContentKey& content = iter->second;
        content.targets.erase(target_id);
        content.count = static_cast<S32>(content.targets.size());
        if (content.targets.empty())
        {
            iter = mTargetContentKeys.erase(iter);
        }
        else
        {
            ++iter;
        }
    }
}

void FSMassObjectEdit::refreshTargetContentsList()
{
    if (!mTargetContentsList)
    {
        return;
    }
    if (mTargetContentsRebuildPending)
    {
        mTargetContentsRebuildDirty = true;
        return;
    }

    mTargetContentsRebuildSelection.clear();
    for (LLScrollListItem* row : mTargetContentsList->getAllSelected())
    {
        mTargetContentsRebuildSelection.insert(row->getValue().asString());
    }
    std::string name_filter = mContentNameFilter ? mContentNameFilter->getText() : std::string();
    std::string type_filter = mContentTypeFilter ? mContentTypeFilter->getText() : std::string();
    std::string count_filter = mContentCountFilter ? mContentCountFilter->getText() : std::string();
    LLStringUtil::trim(name_filter);
    LLStringUtil::trim(type_filter);
    LLStringUtil::trim(count_filter);
    LLStringUtil::toLower(name_filter);
    LLStringUtil::toLower(type_filter);

    mTargetContentsRebuildKeys.clear();
    mTargetContentsRebuildKeys.reserve(mTargetContentKeys.size());
    for (const auto& entry : mTargetContentKeys)
    {
        const ContentKey& content = entry.second;
        std::string searchable_name = content.name;
        std::string type_name = assetTypeName(content.type);
        std::string searchable_type = type_name;
        LLStringUtil::toLower(searchable_name);
        LLStringUtil::toLower(searchable_type);
        if (!matchesCountFilter(content.count, count_filter) ||
            (!name_filter.empty() && searchable_name.find(name_filter) == std::string::npos) ||
            (!type_filter.empty() && searchable_type.find(type_filter) == std::string::npos))
        {
            continue;
        }
        mTargetContentsRebuildKeys.push_back(entry.first);
    }
    mTargetContentsRebuildIndex = 0;
    mTargetContentsRebuildClearing = mTargetContentsList->getItemCount() > 0;
    mTargetContentsRebuildPending = true;
    if (mTargetContentsLabel)
    {
        mTargetContentsLabel->setText(llformat("3. Review contents (updating 0/%d)",
            static_cast<S32>(mTargetContentsRebuildKeys.size())));
    }
    gIdleCallbacks.addFunction(onIdle, this);
    updateButtons();
}

void FSMassObjectEdit::requestTargetContentsListRefresh()
{
    mContentFilterRefreshPending = true;
    mContentFilterRefreshTimer.reset();
    gIdleCallbacks.addFunction(onIdle, this);
    updateButtons();
}

void FSMassObjectEdit::processTargetContentsListRebuild()
{
    LLFrameTimer budget;
    budget.reset();
    while (mTargetContentsRebuildClearing && mTargetContentsList->getItemCount() > 0 &&
        budget.getElapsedTimeF32() < TARGET_LIST_UPDATE_BUDGET)
    {
        mTargetContentsList->deleteSingleItem(mTargetContentsList->getItemCount() - 1);
    }
    if (mTargetContentsRebuildClearing)
    {
        if (mTargetContentsList->getItemCount() > 0)
        {
            return;
        }
        mTargetContentsRebuildClearing = false;
        budget.reset();
    }

    while (mTargetContentsRebuildIndex < mTargetContentsRebuildKeys.size() &&
        budget.getElapsedTimeF32() < TARGET_LIST_UPDATE_BUDGET)
    {
        const std::string& key = mTargetContentsRebuildKeys[mTargetContentsRebuildIndex++];
        const auto found = mTargetContentKeys.find(key);
        if (found == mTargetContentKeys.end())
        {
            continue;
        }
        const ContentKey& content = found->second;
        LLSD row;
        row["id"] = key;
        row["columns"][0]["column"] = "name";
        row["columns"][0]["value"] = content.name;
        row["columns"][1]["column"] = "type";
        row["columns"][1]["value"] = assetTypeName(content.type);
        row["columns"][2]["column"] = "count";
        row["columns"][2]["value"] = content.count;
        row["columns"][2]["alt_value"] = llformat("%010d", content.count);
        LLScrollListItem* added = mTargetContentsList->addElement(row, ADD_BOTTOM);
        if (added && mTargetContentsRebuildSelection.find(key) != mTargetContentsRebuildSelection.end())
        {
            added->setSelected(true);
        }
    }
    if (mTargetContentsRebuildIndex < mTargetContentsRebuildKeys.size())
    {
        if (mTargetContentsLabel)
        {
            mTargetContentsLabel->setText(llformat("3. Review contents (updating %d/%d)",
                static_cast<S32>(mTargetContentsRebuildIndex),
                static_cast<S32>(mTargetContentsRebuildKeys.size())));
        }
        return;
    }

    mTargetContentsRebuildPending = false;
    mTargetContentsList->setNeedsSort();
    if (mTargetContentsRebuildDirty)
    {
        mTargetContentsRebuildDirty = false;
        refreshTargetContentsList();
        return;
    }
    if (mTargetContentsLabel)
    {
        mTargetContentsLabel->setText(llformat("3. Review contents (%d shown)",
            mTargetContentsList->getItemCount()));
    }
    refreshOccurrenceList();
    updateButtons();
}

void FSMassObjectEdit::refreshOccurrenceList()
{
    if (!mOccurrenceList)
    {
        return;
    }
    if (mOccurrenceRebuildPending)
    {
        mOccurrenceRebuildDirty = true;
        return;
    }

    mOccurrenceRebuildSelection.clear();
    for (LLScrollListItem* row : mOccurrenceList->getAllSelected())
    {
        mOccurrenceRebuildSelection.insert(row->getValue().asUUID());
    }
    std::set<LLUUID> target_ids;
    for (LLScrollListItem* row : mTargetContentsList->getAllSelected())
    {
        auto found = mTargetContentKeys.find(row->getValue().asString());
        if (found != mTargetContentKeys.end())
        {
            target_ids.insert(found->second.targets.begin(), found->second.targets.end());
        }
    }

    mOccurrenceRebuildIDs.assign(target_ids.begin(), target_ids.end());
    mOccurrenceRebuildIndex = 0;
    mOccurrenceRebuildClearing = mOccurrenceList->getItemCount() > 0;
    mOccurrenceRebuildPending = true;
    gIdleCallbacks.addFunction(onIdle, this);
    updateButtons();
}

void FSMassObjectEdit::processOccurrenceListRebuild()
{
    LLFrameTimer budget;
    budget.reset();
    while (mOccurrenceRebuildClearing && mOccurrenceList->getItemCount() > 0 &&
        budget.getElapsedTimeF32() < TARGET_LIST_UPDATE_BUDGET)
    {
        mOccurrenceList->deleteSingleItem(mOccurrenceList->getItemCount() - 1);
    }
    if (mOccurrenceRebuildClearing)
    {
        if (mOccurrenceList->getItemCount() > 0)
        {
            return;
        }
        mOccurrenceRebuildClearing = false;
        budget.reset();
    }

    while (mOccurrenceRebuildIndex < mOccurrenceRebuildIDs.size() &&
        budget.getElapsedTimeF32() < TARGET_LIST_UPDATE_BUDGET)
    {
        const LLUUID& id = mOccurrenceRebuildIDs[mOccurrenceRebuildIndex++];
        auto found = mObjects.find(id);
        if (found == mObjects.end())
        {
            continue;
        }
        const ObjectInfo& info = found->second;
        LLSD row;
        row["id"] = id;
        row["columns"][0]["column"] = "name";
        row["columns"][0]["value"] = info.name;
        row["columns"][1]["column"] = "owner";
        row["columns"][1]["value"] = info.owner_id == gAgent.getID() ? "Me" :
            (info.owner_name.empty() ? info.owner_id.asString() : info.owner_name);
        LLScrollListItem* added = mOccurrenceList->addElement(row, ADD_BOTTOM);
        if (added && mOccurrenceRebuildSelection.find(id) != mOccurrenceRebuildSelection.end())
        {
            added->setSelected(true);
        }
    }
    if (mOccurrenceRebuildIndex < mOccurrenceRebuildIDs.size())
    {
        return;
    }

    mOccurrenceRebuildPending = false;
    mOccurrenceList->setNeedsSort();
    mOccurrenceLabel->setText(mOccurrenceRebuildIDs.empty() ?
        "4. Select content to see where it exists" :
        llformat("4. Objects containing selected content (%d)",
            mOccurrenceList->getItemCount()));
    if (mOccurrenceRebuildDirty)
    {
        mOccurrenceRebuildDirty = false;
        refreshOccurrenceList();
        return;
    }
    updateButtons();
}

void FSMassObjectEdit::updateTargetScanProgress()
{
    if (mTargetContentRefreshTimer.getElapsedTimeF32() < 0.5f &&
        mTargetScanProcessed < mTargetScanTotal)
    {
        return;
    }
    refreshTargetContentsList();
    mProgressBar->setValue(mTargetScanTotal > 0 ?
        100.0 * static_cast<F64>(mTargetScanProcessed) / static_cast<F64>(mTargetScanTotal) : 0.0);
    setStatus(llformat("Scanning target contents: %d/%d processed, %d unavailable, %d unique contents...",
        mTargetScanProcessed, mTargetScanTotal, mTargetScanFailed,
        static_cast<S32>(mTargetContentKeys.size())));
    mTargetContentRefreshTimer.reset();
    updateButtons();
}

void FSMassObjectEdit::selectAllTargets()
{
    if (mScanning || mTargetRebuildPending)
    {
        return;
    }
    for (LLScrollListItem* row : mTargetList->getAllData())
    {
        row->setSelected(true);
    }
    updateButtons();
}

void FSMassObjectEdit::clearTargetSelection()
{
    if (mScanning || mTargetRebuildPending)
    {
        return;
    }
    for (LLScrollListItem* row : mTargetList->getAllSelected())
    {
        row->setSelected(false);
    }
    updateButtons();
}

void FSMassObjectEdit::onObjectListRightClick(LLUICtrl*, S32 x, S32 y, MASK,
    LLScrollListCtrl* list)
{
    if (!list)
    {
        return;
    }
    list->selectItemAt(x, y, MASK_NONE);
    showObjectContextMenu(list, x, y);
}

void FSMassObjectEdit::showObjectContextMenu(LLScrollListCtrl* list, S32 x, S32 y)
{
    LLScrollListItem* selected = list ? list->getFirstSelected() : nullptr;
    if (!selected)
    {
        return;
    }
    const LLUUID id = selected->getValue().asUUID();
    const bool loaded = gObjectList.findObject(id) != nullptr;

    if (LLContextMenu* old_menu = mContextMenuHandle.get())
    {
        old_menu->die();
        mContextMenuHandle.markDead();
    }

    LLContextMenu::Params menu_params;
    menu_params.name("mass_object_edit_context_menu");
    menu_params.visible(false);
    LLContextMenu* menu = LLUICtrlFactory::create<LLContextMenu>(menu_params);
    auto add_item = [&](const std::string& name, const std::string& label,
        const std::function<void()>& action)
    {
        LLMenuItemCallGL::Params params;
        params.name(name);
        params.label(label);
        params.enabled.set(loaded);
        params.on_click.function([action](LLUICtrl*, const LLSD&) { action(); });
        menu->addChild(LLUICtrlFactory::create<LLMenuItemCallGL>(params));
    };
    add_item("edit", "Edit Object", [this, id]() { editObject(id); });
    add_item("zoom", "Zoom to Object", [this, id]() { zoomObject(id); });
    add_item("beacon", mBeaconObjectID == id ? "Hide Beacon" : "Show Beacon",
        [this, id]() { beaconObject(id); });

    mContextMenuHandle = menu->getHandle();
    gMenuHolder->addChild(menu);
    S32 screen_x = 0, screen_y = 0;
    list->localPointToScreen(x, y, &screen_x, &screen_y);
    menu->show(screen_x, screen_y, list);
}

void FSMassObjectEdit::editObject(const LLUUID& id)
{
    if (LLViewerObject* object = gObjectList.findObject(id))
    {
        LLSelectMgr::getInstance()->deselectAll();
        LLSelectMgr::getInstance()->selectObjectAndFamily(object);
        handle_object_edit();
    }
}

void FSMassObjectEdit::zoomObject(const LLUUID& id)
{
    if (LLViewerObject* object = gObjectList.findObject(id))
    {
        LLSelectMgr::getInstance()->deselectAll();
        LLSelectMgr::getInstance()->selectObjectAndFamily(object);
        handle_look_at_selection("zoom");
    }
}

void FSMassObjectEdit::beaconObject(const LLUUID& id)
{
    mBeaconObjectID = mBeaconObjectID == id ? LLUUID::null : id;
}

void FSMassObjectEdit::beginOperation(Operation operation)
{
    if (mBusy || mScanning || mWaitingForInterestList) return;
    uuid_vec_t targets = getOperationTargets();
    uuid_vec_t source_items;
    std::vector<std::string> delete_keys;
    if (operation == Operation::DELETE_ITEMS)
    {
        for (LLScrollListItem* row : mTargetContentsList->getAllSelected())
        {
            delete_keys.push_back(row->getValue().asString());
        }
    }
    else
    {
        for (LLScrollListItem* row : mSourceList->getAllSelected())
        {
            source_items.push_back(row->getValue().asUUID());
        }
    }
    if (targets.empty() || (source_items.empty() && delete_keys.empty()))
    {
        return;
    }

    LLSD args;
    args["OPERATION"] = operation == Operation::ADD ? "Add" :
        (operation == Operation::REPLACE ? "Replace" : "Delete");
    args["ITEM_COUNT"] = static_cast<S32>(operation == Operation::DELETE_ITEMS ?
        delete_keys.size() : source_items.size());
    args["TARGET_COUNT"] = static_cast<S32>(targets.size());
    args["WARNING"] = operation == Operation::REPLACE ?
        "Replacement is limited to objects you own. Originals are removed only after a replacement is confirmed. Scripts are compiled for LSL-Luau or Lua and started." :
        (operation == Operation::DELETE_ITEMS ? "Deletion cannot be undone." :
            "Scripts are compiled for LSL-Luau or Lua and started.");
    LLSD payload;
    payload["operation"] = static_cast<S32>(operation);
    payload["source_object"] = mSourceObjectID;
    payload["region"] = mRegionID;
    for (const LLUUID& id : targets) payload["targets"].append(id);
    for (const LLUUID& id : source_items) payload["source_items"].append(id);
    for (const std::string& key : delete_keys) payload["delete_keys"].append(key);
    LLNotificationsUtil::add("ConfirmMassObjectEdit", args, payload,
        boost::bind(&FSMassObjectEdit::confirmOperation, this, _1, _2));
}

bool FSMassObjectEdit::confirmOperation(const LLSD& notification, const LLSD& response)
{
    if (LLNotificationsUtil::getSelectedOption(notification, response) != 0 || mBusy)
    {
        return false;
    }
    const LLSD& payload = notification["payload"];
    if (mScanning || mWaitingForInterestList || !gAgent.getRegion() ||
        payload["region"].asUUID() != gAgent.getRegion()->getRegionID() ||
        payload["source_object"].asUUID() != mSourceObjectID)
    {
        setStatus("Source or region changed; review the operation again.");
        return false;
    }
    uuid_vec_t targets, source_items;
    std::vector<std::string> delete_keys;
    for (const LLSD& value : llsd::inArray(payload["targets"])) targets.push_back(value.asUUID());
    for (const LLSD& value : llsd::inArray(payload["source_items"])) source_items.push_back(value.asUUID());
    for (const LLSD& value : llsd::inArray(payload["delete_keys"])) delete_keys.push_back(value.asString());
    mBusy = true;
    updateButtons();
    LLCoros::instance().launch("FSMassObjectEdit::operation",
        boost::bind(&FSMassObjectEdit::operationCoro, this,
            static_cast<Operation>(payload["operation"].asInteger()), targets,
            source_items, delete_keys));
    return false;
}

void FSMassObjectEdit::operationCoro(Operation operation, uuid_vec_t targets,
    uuid_vec_t source_item_ids, std::vector<std::string> delete_keys)
{
    LLUUID pump_id = LLUUID::generateNewID();
    LLEventMailDrop pump("MassObjectApply-" + pump_id.asString(), true);
    LLPointer<LLViewerObject> source_object = gObjectList.findObject(mSourceObjectID);
    std::map<LLUUID, LLPointer<LLViewerInventoryItem>> agent_items;
    uuid_vec_t temporary_items;
    S32 changes = 0;
    S32 failures = 0;

    LLUUID staging_folder;
    if (operation != Operation::DELETE_ITEMS)
    {
        LLInventoryObject::object_list_t source_inventory;
        if (!canModifyTarget(source_object) ||
            !fetchInventory(source_object, pump, source_inventory, true) ||
            !canModifyTarget(source_object))
        {
            mBusy = false;
            setStatus("Source is unavailable or no longer editable; no objects changed.");
            updateButtons();
            return;
        }
        mSourceItems.clear();
        for (const auto& object : source_inventory)
        {
            if (auto* item = dynamic_cast<LLViewerInventoryItem*>(object.get()))
                mSourceItems[item->getUUID()] = new LLViewerInventoryItem(item);
        }
        // Isolate staging copies from unrelated inventory arrivals. On failure,
        // retain the folder so late copies can be recovered by the user.
        LLEventMailDrop folder_pump("MassObjectFolder-" + LLUUID::generateNewID().asString(), true);
        const std::string pump_name = folder_pump.getName();
        gInventory.createNewCategory(gInventory.getRootFolderID(), LLFolderType::FT_NONE,
            "Mass Object Edit staging", [pump_name](const LLUUID& id)
            {
                LLEventPumps::instance().post(pump_name, LLSDMap("folder", id));
            });
        staging_folder = llcoro::suspendUntilEventOnWithTimeout(folder_pump, INVENTORY_TIMEOUT,
            LLSDMap("timeout", true))["folder"].asUUID();
        if (staging_folder.isNull())
        {
            mBusy = false;
            setStatus("Could not create a staging folder; no objects changed.");
            updateButtons();
            return;
        }
        for (const LLUUID& source_id : source_item_ids)
        {
            auto found = mSourceItems.find(source_id);
            if (found == mSourceItems.end() ||
                !found->second->getPermissions().allowCopyBy(gAgent.getID(), gAgent.getGroupID()))
            {
                ++failures;
                continue;
            }
            LLPointer<LLViewerInventoryItem> item = copyTaskItemToAgent(source_object, found->second, staging_folder);
            if (!item)
            {
                ++failures;
                continue;
            }
            agent_items[source_id] = item;
            temporary_items.push_back(item->getUUID());
        }
    }

    for (const LLUUID& target_id : targets)
    {
        LLPointer<LLViewerObject> target = gObjectList.findObject(target_id);
        auto object_info = mObjects.find(target_id);
        if (!canModifyTarget(target) || object_info == mObjects.end() || !gAgent.allowOperation(
            PERM_MODIFY, object_info->second.permissions, GP_OBJECT_MANIPULATE))
        {
            ++failures;
            continue;
        }

        if (operation == Operation::REPLACE && !target->permYouOwner())
        {
            ++failures;
            continue;
        }
        LLInventoryObject::object_list_t inventory;
        if (!fetchInventory(target, pump, inventory, true) || !canModifyTarget(target))
        {
            ++failures;
            continue;
        }

        if (operation == Operation::DELETE_ITEMS)
        {
            std::set<std::string> selected(delete_keys.begin(), delete_keys.end());
            std::set<LLUUID> removed;
            for (const LLPointer<LLInventoryObject>& object : inventory)
            {
                LLViewerInventoryItem* item = dynamic_cast<LLViewerInventoryItem*>(object.get());
                if (item && selected.find(contentKey(item->getName(), item->getType())) != selected.end())
                {
                    removed.insert(item->getUUID());
                    target->removeInventory(item->getUUID());
                }
            }
            LLInventoryObject::object_list_t after_delete;
            if (fetchInventory(target, pump, after_delete, true))
            {
                for (const auto& object : after_delete)
                    if (removed.erase(object->getUUID())) ++failures;
                changes += static_cast<S32>(removed.size());
            }
            else
            {
                failures += static_cast<S32>(removed.size());
            }
            continue;
        }

        for (const LLUUID& source_id : source_item_ids)
        {
            auto found = agent_items.find(source_id);
            if (found == agent_items.end())
            {
                continue;
            }
            LLPointer<LLViewerInventoryItem> agent_item = found->second;
            if (!canModifyTarget(target) ||
                (!target->permYouOwner() && !RlvActions::canGiveInventory()) ||
                !agent_item->getPermissions().allowCopyBy(gAgent.getID(), gAgent.getGroupID()) ||
                !LLToolDragAndDrop::isInventoryDropAcceptable(target, agent_item))
            {
                ++failures;
                continue;
            }
            std::vector<LLUUID> matches;
            for (const LLPointer<LLInventoryObject>& object : inventory)
            {
                LLViewerInventoryItem* item = dynamic_cast<LLViewerInventoryItem*>(object.get());
                if (item && item->getName() == agent_item->getName() &&
                    item->getType() == agent_item->getType())
                {
                    matches.push_back(item->getUUID());
                }
            }

            const bool replace = operation == Operation::REPLACE && !matches.empty();
            const bool own_object = target->permYouOwner();
            LLInventoryObject::object_list_t before_add = inventory;
            LLPointer<LLViewerInventoryItem> item_to_add = new LLViewerInventoryItem(agent_item.get());
            if (replace && !own_object)
            {
                ++failures;
                continue;
            }
            else if (replace)
            {
                item_to_add->rename("__FS_MASS_EDIT_" + item_to_add->getUUID().asString());
            }

            if (item_to_add->getType() == LLAssetType::AT_LSL_TEXT)
            {
                target->saveScript(item_to_add, false, true);
            }
            else
            {
                // Permissions were checked above. Avoid the drag tool's global
                // source state, which can refer to an unrelated earlier drag.
                target->updateInventory(item_to_add, TASK_INVENTORY_ITEM_KEY, true);
            }

            LLInventoryObject::object_list_t after_add;
            LLPointer<LLViewerInventoryItem> added;
            for (S32 attempt = 0; attempt < 10 && !added; ++attempt)
            {
                if (fetchInventory(target, pump, after_add, true))
                {
                    added = findNewItem(before_add, after_add, item_to_add);
                }
                if (!added) llcoro::suspendUntilTimeout(0.25f);
            }
            if (!added || !canModifyTarget(target))
            {
                ++failures;
                continue;
            }

            if (added->getType() == LLAssetType::AT_LSL_TEXT)
            {
                LLSD compile_result;
                const bool fetched = fetchScriptAsset(target, added, pump);
                bool compiled = fetched && compileScript(target, added, "lsl-luau",
                    pump, compile_result);
                if (fetched && !compiled && !compile_result.has("timeout"))
                {
                    compiled = compileScript(target, added, "luau",
                        pump, compile_result);
                }
                if (!compiled)
                {
                    if (canModifyTarget(target)) target->removeInventory(added->getUUID());
                    ++failures;
                    FSCommon::report_to_nearby_chat(std::string(
                        "Warning: mass edit script compilation failed for object \"") +
                        object_info->second.name + "\", script \"" + agent_item->getName() +
                        "\". The original content was retained.");
                    continue;
                }
                added->setAssetUUID(compile_result["new_asset"].asUUID());
            }

            if (!canModifyTarget(target) || (replace && !target->permYouOwner()))
            {
                ++failures;
                continue;
            }
            if (replace && own_object)
            {
                for (const LLUUID& id : matches) target->removeInventory(id);
                LLPointer<LLViewerInventoryItem> renamed = new LLViewerInventoryItem(added.get());
                renamed->rename(agent_item->getName());
                target->updateInventory(renamed, TASK_INVENTORY_ITEM_KEY, false);
            }
            if (added->getType() == LLAssetType::AT_LSL_TEXT)
            {
                setScriptRunning(target, added->getUUID(), true);
            }
            inventory.clear();
            if (!fetchInventory(target, pump, inventory, true))
            {
                ++failures;
                break;
            }
            bool confirmed = false;
            bool originals_remain = false;
            for (const auto& object : inventory)
            {
                if (object->getUUID() == added->getUUID() && object->getName() == agent_item->getName())
                    confirmed = true;
                if (replace && std::find(matches.begin(), matches.end(), object->getUUID()) != matches.end())
                    originals_remain = true;
            }
            if (confirmed && !originals_remain) ++changes;
            else ++failures;
            llcoro::suspend();
        }
    }

    for (const LLUUID& id : temporary_items)
    {
        remove_inventory_item(id, nullptr, true);
    }
    if (failures == 0 && staging_folder.notNull() &&
        gInventory.categoryHasChildren(staging_folder) == LLInventoryModel::CHILDREN_NO)
    {
        remove_inventory_category(staging_folder, nullptr);
    }
    mBusy = false;
    setStatus(llformat("Mass object edit complete: %d changes, %d failures.%s", changes, failures,
        failures && staging_folder.notNull() ? " Check the staging folder for delayed copies." : ""));
    updateButtons();
}
