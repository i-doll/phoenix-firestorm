/**
 * @file fsmassobjectedit.h
 * @brief Region-wide multi-object task inventory editor.
 */

#ifndef FS_MASSOBJECTEDIT_H
#define FS_MASSOBJECTEDIT_H

#include "llfloater.h"
#include "llframetimer.h"
#include "llpermissions.h"
#include "llviewerassettype.h"

#include <boost/signals2/connection.hpp>
#include <map>
#include <set>
#include <vector>

class LLButton;
class LLAvatarName;
class LLContextMenu;
class LLLineEditor;
class LLMessageSystem;
class LLProgressBar;
class LLScrollListCtrl;
class LLTextBox;
class LLViewerInventoryItem;

class FSMassObjectEdit final : public LLFloater
{
    friend class LLFloaterReg;

public:
    FSMassObjectEdit(const LLSD& key);
    ~FSMassObjectEdit() override;

    bool postBuild() override;
    bool canClose() override { return !mBusy; }
    void draw() override;
    void onOpen(const LLSD& key) override;
    void onClose(bool app_quitting) override;
    void processObjectProperties(LLMessageSystem* msg);
    bool isScanning() const { return mScanning; }

private:
    enum class Operation { ADD, REPLACE, DELETE_ITEMS };
    enum class PropertyRequestState { NEED, DEFERRED, SENT, RECEIVED };

    struct ObjectInfo
    {
        LLUUID id;
        LLUUID root_id;
        LLUUID creator_id;
        LLUUID owner_id;
        U32 local_id{ 0 };
        std::string name;
        std::string creator_name;
        std::string creator_search_name;
        std::string owner_name;
        std::string owner_search_name;
        LLPermissions permissions;
        PropertyRequestState property_request{ PropertyRequestState::NEED };
        bool received{ false };
        bool group_owned{ false };
    };

    struct ContentKey
    {
        std::string name;
        LLAssetType::EType type{ LLAssetType::AT_NONE };
        S32 count{ 0 };
        std::set<LLUUID> targets;
    };

    void refreshObjects();
    void useSelectedSource();
    void scanTargetContents();
    void refreshOccurrenceInventories();
    void beginOperation(Operation operation);
    bool confirmOperation(const LLSD& notification, const LLSD& response);
    void scanTargetContentsCoro(uuid_vec_t targets, bool refresh_existing);
    void scanTargetContentsWorkerCoro(uuid_vec_t targets, std::string done_pump_name,
        bool refresh_existing, bool retry_pass);
    void removeTargetContents(const LLUUID& target_id);
    void refreshTargetContentsList();
    void refreshOccurrenceList();
    void requestTargetListRebuild();
    void requestTargetContentsListRefresh();
    void updateTargetScanProgress();
    void selectAllTargets();
    void clearTargetSelection();
    void operationCoro(Operation operation, uuid_vec_t targets,
        uuid_vec_t source_items, std::vector<std::string> delete_keys);
    void fetchSourceInventoryCoro(LLUUID source_id);
    void updateButtons();
    void setStatus(const std::string& status);

    static void onIdle(void* userdata);
    void requestObjectProperties(const std::vector<U32>& local_ids, bool select);
    void reconcileObjects();
    bool retryUnresolvedProperties();
    void processPropertyRequestQueue();
    void finishObjectScan();
    void rebuildTargetList();
    void processTargetListRebuild();
    void processTargetContentsListRebuild();
    void processOccurrenceListRebuild();
    void avatarNameCallback(const LLUUID& id, const LLAvatarName& av_name);
    void ownerNameCallback(const LLUUID& id, const std::string& name);
    void onObjectListRightClick(LLUICtrl* ctrl, S32 x, S32 y, MASK mask,
        LLScrollListCtrl* list);
    void showObjectContextMenu(LLScrollListCtrl* list, S32 x, S32 y);
    void editObject(const LLUUID& id);
    void zoomObject(const LLUUID& id);
    void beaconObject(const LLUUID& id);
    uuid_vec_t getSelectedTargets() const;
    uuid_vec_t getOperationTargets() const;

    LLLineEditor* mNameFilter{ nullptr };
    LLLineEditor* mCreatorFilter{ nullptr };
    LLLineEditor* mOwnerFilter{ nullptr };
    LLLineEditor* mContentNameFilter{ nullptr };
    LLLineEditor* mContentTypeFilter{ nullptr };
    LLLineEditor* mContentCountFilter{ nullptr };
    LLScrollListCtrl* mTargetList{ nullptr };
    LLScrollListCtrl* mSourceList{ nullptr };
    LLScrollListCtrl* mTargetContentsList{ nullptr };
    LLScrollListCtrl* mOccurrenceList{ nullptr };
    LLTextBox* mSourceLabel{ nullptr };
    LLTextBox* mTargetContentsLabel{ nullptr };
    LLTextBox* mOccurrenceLabel{ nullptr };
    LLTextBox* mOperationScopeLabel{ nullptr };
    LLTextBox* mStatusText{ nullptr };
    LLProgressBar* mProgressBar{ nullptr };
    LLButton* mAddButton{ nullptr };
    LLButton* mReplaceButton{ nullptr };
    LLButton* mDeleteButton{ nullptr };
    LLButton* mRefreshOccurrencesButton{ nullptr };

    std::map<LLUUID, ObjectInfo> mObjects;
    std::map<LLUUID, boost::signals2::connection> mNameCacheConnections;
    std::map<LLUUID, uuid_vec_t> mCreatorObjectIDs;
    std::map<LLUUID, uuid_vec_t> mOwnerObjectIDs;
    std::map<LLUUID, LLPointer<LLViewerInventoryItem>> mSourceItems;
    std::map<std::string, ContentKey> mTargetContentKeys;
    uuid_vec_t mTargetScanRetryIDs;
    uuid_vec_t mPropertyRequestIDs;
    uuid_vec_t mTargetRebuildIDs;
    uuid_vec_t mOccurrenceRebuildIDs;
    std::vector<std::string> mTargetContentsRebuildKeys;
    std::set<LLUUID> mTargetRebuildSelection;
    std::set<LLUUID> mOccurrenceRebuildSelection;
    std::set<std::string> mTargetContentsRebuildSelection;
    std::string mTargetNameFilter;
    std::string mTargetCreatorFilter;
    std::string mTargetOwnerFilter;
    LLHandle<LLContextMenu> mContextMenuHandle;
    LLUUID mRegionID;
    LLUUID mSourceObjectID;
    LLUUID mBeaconObjectID;
    LLFrameTimer mScanTimer;
    LLFrameTimer mCreatorRefreshTimer;
    LLFrameTimer mTargetFilterRefreshTimer;
    LLFrameTimer mContentFilterRefreshTimer;
    LLFrameTimer mTargetContentRefreshTimer;
    LLFrameTimer mInterestListTimer;
    LLFrameTimer mReconcileTimer;
    S32 mPendingProperties{ 0 };
    S32 mPropertyRequestsInFlight{ 0 };
    S32 mPropertyRetryCount{ 0 };
    S32 mTargetScanTotal{ 0 };
    S32 mTargetScanProcessed{ 0 };
    S32 mTargetScanSucceeded{ 0 };
    S32 mTargetScanFailed{ 0 };
    size_t mTargetRebuildIndex{ 0 };
    size_t mPropertyRequestIndex{ 0 };
    size_t mTargetContentsRebuildIndex{ 0 };
    size_t mOccurrenceRebuildIndex{ 0 };
    S32 mTargetRebuildEditableCount{ 0 };
    bool mScanning{ false };
    bool mCreatorRefreshPending{ false };
    bool mTargetFilterRefreshPending{ false };
    bool mContentFilterRefreshPending{ false };
    bool mTargetRebuildPending{ false };
    bool mTargetRebuildClearing{ false };
    bool mTargetContentsRebuildPending{ false };
    bool mTargetContentsRebuildClearing{ false };
    bool mTargetContentsRebuildDirty{ false };
    bool mOccurrenceRebuildPending{ false };
    bool mOccurrenceRebuildClearing{ false };
    bool mOccurrenceRebuildDirty{ false };
    bool mBusy{ false };
    bool mWaitingForInterestList{ false };
};

#endif
