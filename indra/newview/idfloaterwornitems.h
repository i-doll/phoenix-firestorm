#ifndef ID_FLOATER_WORN_ITEMS_H
#define ID_FLOATER_WORN_ITEMS_H

#include "llfloater.h"
#include "llavatarnamecache.h"

#include <map>

class LLMessageSystem;
class LLScrollListCtrl;
class LLVOAvatar;

// Lists the attachments another avatar is wearing. Keyed by the avatar's id.
class IDFloaterWornItems : public LLFloater
{
public:
    IDFloaterWornItems(const LLSD& key);
    bool postBuild() override;
    void onOpen(const LLSD& key) override;
    void onClose(bool app_quitting) override;
    void draw() override;

    static void show(const LLUUID& avatar_id);
    static bool canShow(const LLUUID& avatar_id);
    static void processObjectProperties(LLMessageSystem* msg);

private:
    ~IDFloaterWornItems() override;

    static bool rlvAllowed(const LLUUID& avatar_id);

    LLVOAvatar* getAvatar() const;
    void refreshList();
    void updateTitle();
    void setCreatorName(const LLUUID& object_id);
    bool canShowCreator(const LLUUID& creator_id) const;
    void handleObjectProperties(LLMessageSystem* msg);
    void sendSelect(bool select, const uuid_set_t& object_ids);
    void onSelectionChange();
    void onDoubleClick();
    void onCreatorProfile();

    LLUUID            mAvatarId;
    LLScrollListCtrl* mList{ nullptr };

    uuid_set_t               mPendingIds;
    std::map<LLUUID, LLUUID> mCreatorByObject;

    LLAvatarNameCache::callback_connection_t                   mAvatarNameConnection;
    std::map<LLUUID, LLAvatarNameCache::callback_connection_t> mCreatorNameConnections;
};

#endif // ID_FLOATER_WORN_ITEMS_H
