/**
 * @file lloutfitslist.cpp
 * @brief List of agent's outfits for My Appearance side panel.
 *
 * $LicenseInfo:firstyear=2010&license=viewerlgpl$
 * Second Life Viewer Source Code
 * Copyright (C) 2010, Linden Research, Inc.
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation;
 * version 2.1 of the License only.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301  USA
 *
 * Linden Research, Inc., 945 Battery Street, San Francisco, CA  94111  USA
 * $/LicenseInfo$
 */

#include "llviewerprecompiledheaders.h"

#include "lloutfitslist.h"

// llcommon
#include "llcommonutils.h"

#include "llaccordionctrl.h"
#include "llaccordionctrltab.h"
#include "llagentwearables.h"
#include "llaisapi.h"
#include "llappearancemgr.h"
#include "llappviewer.h"
#include "llfloaterreg.h"
#include "llfloatersidepanelcontainer.h"
#include "llinspecttexture.h"
#include "llinventorymodelbackgroundfetch.h"
#include "llinventoryfunctions.h"
#include "llinventorymodel.h"
#include "llmenubutton.h"
#include "llnotificationsutil.h"
#include "lloutfitobserver.h"
#include "lltoggleablemenu.h"
#include "lltransutil.h"
#include "llviewercontrol.h"
#include "llviewermenu.h"
#include "llvoavatar.h"
#include "llvoavatarself.h"
#include "llwearableitemslist.h"

#include "llviewercontrol.h" // <FS:ND/> for gSavedSettings
#include "llresmgr.h"
#include "lltextbox.h"
#include "lleconomy.h"

#include "rlvactions.h"
#include "rlvlocks.h"

static bool is_tab_header_clicked(LLOutfitAccordionCtrlTab* tab, S32 y);

static const LLOutfitTabNameComparator OUTFIT_TAB_NAME_COMPARATOR;
static const LLOutfitTabFavComparator OUTFIT_TAB_FAV_COMPARATOR;

/*virtual*/
// <ID> Folder tabs lead their level under every sort order. A folder tab is the one whose
// body is a nested accordion rather than a wearable list.
static bool id_tab_is_folder(const LLAccordionCtrlTab* tab)
{
    if (!tab)
    {
        return false;
    }
    return dynamic_cast<const LLAccordionCtrl*>(
        const_cast<LLAccordionCtrlTab*>(tab)->getAccordionView()) != NULL;
}
// </ID>

bool LLOutfitTabNameComparator::compare(const LLAccordionCtrlTab* tab1, const LLAccordionCtrlTab* tab2) const
{
    // <ID>
    const bool folder1 = id_tab_is_folder(tab1);
    const bool folder2 = id_tab_is_folder(tab2);
    if (folder1 != folder2)
    {
        return folder1;
    }
    // </ID>
    return (LLStringUtil::compareDict(tab1->getTitle(), tab2->getTitle()) < 0);
}

bool LLOutfitTabFavComparator::compare(const LLAccordionCtrlTab* tab1, const LLAccordionCtrlTab* tab2) const
{
    // <ID>
    const bool folder1 = id_tab_is_folder(tab1);
    const bool folder2 = id_tab_is_folder(tab2);
    if (folder1 != folder2)
    {
        return folder1;
    }
    // </ID>
    LLOutfitAccordionCtrlTab* taba = (LLOutfitAccordionCtrlTab*)tab1;
    LLOutfitAccordionCtrlTab* tabb = (LLOutfitAccordionCtrlTab*)tab2;
    if (taba->getFavorite() != tabb->getFavorite())
    {
        return taba->getFavorite();
    }

    return (LLStringUtil::compareDict(tab1->getTitle(), tab2->getTitle()) < 0);
}

struct outfit_accordion_tab_params : public LLInitParam::Block<outfit_accordion_tab_params, LLOutfitAccordionCtrlTab::Params>
{
    Mandatory<LLWearableItemsList::Params> wearable_list;

    outfit_accordion_tab_params()
    :   wearable_list("wearable_items_list")
    {}
};

const outfit_accordion_tab_params& get_accordion_tab_params()
{
    static outfit_accordion_tab_params tab_params;
    static bool initialized = false;
    if (!initialized)
    {
        initialized = true;

        LLOutfitAccordionCtrlTab::sFavoriteIcon = LLUI::getUIImage("Inv_Favorite_Star_Full");
        LLOutfitAccordionCtrlTab::sFgColor = LLUIColorTable::instance().getColor("MenuItemEnabledColor", LLColor4U(255, 255, 255));

        LLXMLNodePtr xmlNode;
        if (LLUICtrlFactory::getLayeredXMLNode("outfit_accordion_tab.xml", xmlNode))
        {
            LLXUIParser parser;
            parser.readXUI(xmlNode, tab_params, "outfit_accordion_tab.xml");
        }
        else
        {
            LL_WARNS() << "Failed to read xml of Outfit's Accordion Tab from outfit_accordion_tab.xml" << LL_ENDL;
        }
    }

    return tab_params;
}


static LLPanelInjector<LLOutfitsList> t_outfits_list("outfits_list");

LLOutfitsList::LLOutfitsList()
    :   LLOutfitListBase()
    ,   mAccordion(NULL)
    ,   mListCommands(NULL)
// <ID:i.doll> [COF worn-state refresh performance]
    ,   mCOFRefreshPending(false)
// </ID:i.doll>
    ,   mSortMenu(nullptr)
// <ID:i.doll> [COF worn-state refresh performance]
    ,   mItemSelected(false)
// </ID:i.doll>
{
    LLControlVariable* ctrl = gSavedSettings.getControl("InventoryFavoritesColorText");
    if (ctrl)
    {
        mSavedSettingInvFavColor = ctrl->getSignal()->connect(boost::bind(&LLOutfitsList::handleInvFavColorChange, this));
    }
}

LLOutfitsList::~LLOutfitsList()
{
    delete mSortMenu;
    mSavedSettingInvFavColor.disconnect();
    mGearMenuConnection.disconnect();
}

bool LLOutfitsList::postBuild()
{
    mAccordion = getChild<LLAccordionCtrl>("outfits_accordion");
    mAccordion->setComparator(&OUTFIT_TAB_NAME_COMPARATOR);

    initComparator();

    return LLOutfitListBase::postBuild();
}

void LLOutfitsList::initComparator()
{
    S32 mode = gSavedSettings.getS32("OutfitListSortOrder");
    const LLAccordionCtrl::LLTabComparator* comp = (mode == 0)
        ? static_cast<const LLAccordionCtrl::LLTabComparator*>(&OUTFIT_TAB_NAME_COMPARATOR)
        : static_cast<const LLAccordionCtrl::LLTabComparator*>(&OUTFIT_TAB_FAV_COMPARATOR);
    mAccordion->setComparator(comp);

    // <ID> Nested accordions need the same comparator, or subfolder contents sort in
    // insertion order while the top level sorts properly.
    for (std::map<LLUUID, LLAccordionCtrl*>::iterator it = mFolderAccordions.begin();
         it != mFolderAccordions.end(); ++it)
    {
        if (it->second)
        {
            it->second->setComparator(comp);
        }
    }
    // </ID>

    sortOutfits();
}

//virtual
void LLOutfitsList::onOpen(const LLSD& info)
{
    if (!mIsInitialized)
    {
        // Start observing changes in Current Outfit category.
        LLOutfitObserver::instance().addCOFChangedCallback(boost::bind(&LLOutfitsList::onCOFChanged, this));
    }

    LLOutfitListBase::onOpen(info);

// <ID:i.doll> [COF worn-state refresh performance]
    if (mCOFRefreshPending)
    {
        mCOFRefreshPending = false;
        onCOFChanged();
    }
// </ID:i.doll>

    LLAccordionCtrlTab* selected_tab = mAccordion->getSelectedTab();
    if (!selected_tab) return;

    // Pass focus to the selected outfit tab.
    selected_tab->showAndFocusHeader();
}


// <ID> A folder tab survives the filter if its own name matches or any descendant outfit
// survives. Recurses depth-first through inventory rather than through the accordion,
// because LLAccordionCtrl keeps its tab vector private. Returns whether the tab is visible.
bool LLOutfitsList::applyFilterToFolderTab(const LLUUID& cat_id,
                                           LLOutfitAccordionCtrlTab* tab,
                                           const std::string& filter_substring)
{
    if (!tab)
    {
        return false;
    }

    std::string title = tab->getTitle();
    LLStringUtil::toUpper(title);
    std::string cur_filter = filter_substring;
    LLStringUtil::toUpper(cur_filter);

    const bool self_matches = (std::string::npos != title.find(cur_filter));
    bool any_child_visible = false;

    LLInventoryModel::cat_array_t* cats = NULL;
    LLInventoryModel::item_array_t* items = NULL;
    gInventory.getDirectDescendentsOf(cat_id, cats, items);
    if (cats)
    {
        for (const LLPointer<LLViewerInventoryCategory>& child : *cats)
        {
            const LLUUID child_id = child->getUUID();

            std::map<LLUUID, LLOutfitAccordionCtrlTab*>::iterator folder_it = mFolderTabs.find(child_id);
            if (folder_it != mFolderTabs.end())
            {
                if (applyFilterToFolderTab(child_id, folder_it->second, filter_substring))
                {
                    any_child_visible = true;
                }
                continue;
            }

            outfits_map_t::iterator outfit_it = mOutfitsMap.find(child_id);
            if (outfit_it != mOutfitsMap.end())
            {
                applyFilterToTab(child_id, outfit_it->second, filter_substring);
                if (outfit_it->second->getVisible())
                {
                    any_child_visible = true;
                }
            }
        }
    }

    tab->setTitle(tab->getTitle(), cur_filter);
    tab->setFilterGeneration(getFilterGeneration());
    tab->setVisible(self_matches || any_child_visible);
    return tab->getVisible();
}

// Where does this category's tab belong? Its parent folder's inner accordion if that
// folder already has a tab, the top-level accordion if it sits at the root, or NULL
// meaning "parent folder exists but has no tab yet" — the caller then defers.
// <ID> The accordion that actually holds a tab, read from the view tree. Unlike
// getParentAccordion this stays correct when inventory has already changed under the
// tab (a move, or a delete that re-parented the category to Trash).
static LLAccordionCtrl* id_owning_accordion(LLAccordionCtrlTab* tab)
{
    return tab ? dynamic_cast<LLAccordionCtrl*>(tab->getParent()) : NULL;
}
// </ID>

LLAccordionCtrl* LLOutfitsList::getParentAccordion(const LLUUID& cat_id)
{
    LLViewerInventoryCategory* cat = gInventory.getCategory(cat_id);
    if (!cat)
    {
        return mAccordion;
    }
    const LLUUID parent = cat->getParentUUID();
    const LLUUID root = gInventory.findCategoryUUIDForType(LLFolderType::FT_MY_OUTFITS);
    if (parent.isNull() || parent == root)
    {
        return mAccordion;
    }
    std::map<LLUUID, LLAccordionCtrl*>::iterator it = mFolderAccordions.find(parent);
    return (it == mFolderAccordions.end()) ? NULL : it->second;
}

// A folder tab looks like an outfit tab but holds a nested accordion instead of a
// wearable list.
// <ID> A folder tab's body is an accordion, and LLAccordionCtrl::notifyParent swallows
// "size_changes" — it schedules an arrange of its own children and returns without
// forwarding. A nested outfit's wearable list reports its real height asynchronously
// (when the list populates), and that report dies here, leaving the folder tab sized for
// the stale height. This subclass tells the outfits list so it can resize the folder;
// the folder tab's own handler then forwards upward, so nested folders propagate too.
class IDFolderAccordionCtrl : public LLAccordionCtrl
{
public:
    IDFolderAccordionCtrl(const Params& p) : LLAccordionCtrl(p) {}

    void setContentSizeChangedCallback(std::function<void()> cb) { mContentSizeChangedCb = cb; }

    S32 notifyParent(const LLSD& info) override
    {
        S32 handled = LLAccordionCtrl::notifyParent(info);
        if (mContentSizeChangedCb && info.has("action") && info["action"].asString() == "size_changes")
        {
            mContentSizeChangedCb();
        }
        return handled;
    }

private:
    std::function<void()> mContentSizeChangedCb;
};
// </ID>

void LLOutfitsList::addFolderTab(LLViewerInventoryCategory* cat)
{
    if (!cat || mFolderAccordions.count(cat->getUUID()))
    {
        return;
    }
    const LLUUID cat_id = cat->getUUID();

    LLAccordionCtrl* parent = getParentAccordion(cat_id);
    if (!parent)
    {
        // Our own parent folder has no tab yet; it will flush us when it gets one.
        mPendingChildren.insert(std::make_pair(cat->getParentUUID(), cat_id));
        return;
    }

    outfit_accordion_tab_params tab_params(get_accordion_tab_params());
    tab_params.cat_id = cat_id;
    LLOutfitAccordionCtrlTab* tab = LLUICtrlFactory::create<LLOutfitAccordionCtrlTab>(tab_params);
    if (!tab)
    {
        return;
    }

    LLAccordionCtrl::Params inner_params;
    inner_params.name = "folder_accordion";
    IDFolderAccordionCtrl* inner = LLUICtrlFactory::create<IDFolderAccordionCtrl>(inner_params);
    inner->setContentSizeChangedCallback([this, cat_id]() { resizeFolderTab(cat_id); });
    inner->setShape(tab->getLocalRect());
    // Match the top level's ordering, including for folders created after initComparator.
    inner->setComparator((gSavedSettings.getS32("OutfitListSortOrder") == 0)
        ? static_cast<const LLAccordionCtrl::LLTabComparator*>(&OUTFIT_TAB_NAME_COMPARATOR)
        : static_cast<const LLAccordionCtrl::LLTabComparator*>(&OUTFIT_TAB_FAV_COMPARATOR));
    tab->addChild(inner);

    const std::string name = cat->getName();
    tab->setName(name);
    tab->setTitle(name);
    tab->setFavorite(cat->getIsFavorite());
    tab->setDisplayChildren(false);

    parent->addCollapsibleCtrl(tab);
    mFolderAccordions[cat_id] = inner;
    mFolderTabs[cat_id] = tab;

    // Expanding or collapsing a folder changes how much room it needs, and nothing else
    // recomputes that — without this the outer accordion keeps the collapsed height and
    // the contents draw over whatever sits below.
    tab->setDropDownStateChangedCallback([this](LLUICtrl*, const LLSD&)
    {
        arrange();
    });

    tab->setRightMouseDownCallback(boost::bind(&LLOutfitListBase::outfitRightClickCallBack, this,
        _1, _2, _3, cat_id));

    flushPendingChildren(cat_id);
}

// Re-run updateAddedCategory for anything that was waiting on this folder's tab.
void LLOutfitsList::flushPendingChildren(const LLUUID& parent_id)
{
    std::vector<LLUUID> ready;
    auto range = mPendingChildren.equal_range(parent_id);
    for (auto it = range.first; it != range.second; ++it)
    {
        ready.push_back(it->second);
    }
    mPendingChildren.erase(parent_id);

    for (const LLUUID& child : ready)
    {
        updateAddedCategory(child);
    }
}
// </ID>

void LLOutfitsList::updateAddedCategory(LLUUID cat_id)
{
    LL_PROFILE_ZONE_SCOPED;
    LLViewerInventoryCategory *cat = gInventory.getCategory(cat_id);
    if (!cat) return;

    if (!isOutfitFolder(cat))
    {
        // Assume a subfolder that contains or will contain outfits, track it
        const LLUUID outfits = gInventory.findCategoryUUIDForType(LLFolderType::FT_MY_OUTFITS);
        mCategoriesObserver->addCategory(cat_id, [this, outfits]()
        {
            observerCallback(outfits);
        });
        // <ID> Render it as a nested tab instead of discarding it. The observer
        // registration above is kept — it keeps nested contents live.
        if (cat_id != outfits)
        {
            addFolderTab(cat);
        }
        // </ID>
        return;
    }

    std::string name = cat->getName();

    outfit_accordion_tab_params tab_params(get_accordion_tab_params());
    tab_params.cat_id = cat_id;
    LLOutfitAccordionCtrlTab *tab = LLUICtrlFactory::create<LLOutfitAccordionCtrlTab>(tab_params);
    if (!tab) return;
    LLWearableItemsList* wearable_list = LLUICtrlFactory::create<LLWearableItemsList>(tab_params.wearable_list);
    wearable_list->setDoubleClickCallback(boost::bind(&LLOutfitsList::onDoubleClick, this, wearable_list)); // <FS:Ansariel> FIRE-22484: Double-click wear in outfits list
    wearable_list->setShape(tab->getLocalRect());
    tab->addChild(wearable_list);

    tab->setName(name);
    tab->setTitle(name);
    tab->setFavorite(cat->getIsFavorite());

    // *TODO: LLUICtrlFactory::defaultBuilder does not use "display_children" from xml. Should be investigated.
    tab->setDisplayChildren(false);

    // <ID> Place the outfit under its folder when it has one. A NULL target means the
    // parent folder's tab has not been built yet, so defer until it is.
    LLAccordionCtrl* target = getParentAccordion(cat_id);
    if (!target)
    {
        mPendingChildren.insert(std::make_pair(cat->getParentUUID(), cat_id));
        tab->die();
        return;
    }
    target->addCollapsibleCtrl(tab);
    // </ID>

    // Start observing the new outfit category.
    LLWearableItemsList* list = tab->getChild<LLWearableItemsList>("wearable_items_list");
    if (!mCategoriesObserver->addCategory(cat_id, boost::bind(&LLWearableItemsList::updateList, list, cat_id)))
    {
        // Remove accordion tab if category could not be added to observer.
        target->removeCollapsibleCtrl(tab); // was mAccordion

        // kill removed tab
        tab->die();
        return;
    }

    // Map the new tab with outfit category UUID.
    mOutfitsMap.insert(LLOutfitsList::outfits_map_value_t(cat_id, tab));

    tab->setRightMouseDownCallback(boost::bind(&LLOutfitListBase::outfitRightClickCallBack, this,
        _1, _2, _3, cat_id));

    // Setting tab focus callback to monitor currently selected outfit.
    tab->setFocusReceivedCallback(boost::bind(&LLOutfitListBase::ChangeOutfitSelection, this, list, cat_id));

    // Setting callback to reset items selection inside outfit on accordion collapsing and expanding (EXT-7875)
    // <ID> For a nested outfit the toggle also changes how much room its folder needs,
    // and the inner accordion swallows the size_changes notify, so recompute here.
    const bool nested = (target != mAccordion);
    tab->setDropDownStateChangedCallback([this, list, cat_id, nested](LLUICtrl*, const LLSD&)
    {
        resetItemSelection(list, cat_id);
        if (nested)
        {
            arrange();
        }
    });
    // </ID>

    // Depending on settings, force showing list items that don't match current filter(EXT-7158)
    static LLCachedControl<bool> list_filter(gSavedSettings, "OutfitListFilterFullList");
    list->setForceShowingUnmatchedItems(list_filter(), false);

    // Setting list commit callback to monitor currently selected wearable item.
    list->setCommitCallback(boost::bind(&LLOutfitsList::onListSelectionChange, this, _1));

    // Setting list refresh callback to apply filter on list change.
    list->setRefreshCompleteCallback([this, tab](LLUICtrl* ctrl, const LLSD& sd)
    {
        onRefreshComplete(ctrl, tab);
    });

    list->setRightMouseDownCallback(boost::bind(&LLOutfitsList::onWearableItemsListRightClick, this, _1, _2, _3));

    if (AISAPI::isAvailable() && LLInventoryModelBackgroundFetch::instance().folderFetchActive())
    {
        // For reliability just fetch it whole, linked items included
        // Todo: list is not warrantied to exist once callback arrives
        // Fix it!
        LLInventoryModelBackgroundFetch::instance().fetchFolderAndLinks(cat_id, [cat_id, list]
        {
            if (list)
            {
                list->updateList(cat_id);
                list->setForceRefresh(true);
            }
        });
    }
    else
    {
        // Fetch the new outfit contents.
        cat->fetch();
        // Refresh the list of outfit items after fetch().
        // Further list updates will be triggered by the category observer.
        list->updateList(cat_id);
        list->setForceRefresh(true);
    }

    // If filter is currently applied we store the initial tab state.
    if (!getFilterSubString().empty())
    {
        tab->notifyChildren(LLSD().with("action", "store_state"));

        // Setting mForceRefresh flag will make the list refresh its contents
        // even if it is not currently visible. This is required to apply the
        // filter to the newly added list.
        list->setForceRefresh(true);

        list->setFilterSubString(getFilterSubString(), false);
    }
}

void LLOutfitsList::updateRemovedCategory(LLUUID cat_id)
{
    // <ID> A removed subfolder takes its tab and inner accordion with it. Its children are
    // removed by the base's own diff, so nothing else needs re-parenting here.
    {
        std::map<LLUUID, LLOutfitAccordionCtrlTab*>::iterator folder_iter = mFolderTabs.find(cat_id);
        if (folder_iter != mFolderTabs.end())
        {
            LLOutfitAccordionCtrlTab* folder_tab = folder_iter->second;
            mCategoriesObserver->removeCategory(cat_id);
            deselectOutfit(cat_id);
            mFolderTabs.erase(folder_iter);
            mFolderAccordions.erase(cat_id);
            mPendingChildren.erase(cat_id);

            // The owner must come from the view tree: for a deleted or moved category the
            // inventory parent already points elsewhere, and removing from the wrong
            // accordion leaves a dead pointer in the real owner's tab list.
            LLAccordionCtrl* parent = id_owning_accordion(folder_tab);
            if (!parent)
            {
                parent = mAccordion;
            }
            parent->removeCollapsibleCtrl(folder_tab);
            if (folder_tab)
            {
                folder_tab->die();
            }
            return;
        }
    }
    // </ID>

    outfits_map_t::iterator outfits_iter = mOutfitsMap.find(cat_id);
    if (outfits_iter != mOutfitsMap.end())
    {
        const LLUUID& outfit_id = outfits_iter->first;
        LLOutfitAccordionCtrlTab* tab = outfits_iter->second;

        // An outfit is removed from the list. Do the following:
        // 1. Remove outfit category from observer to stop monitoring its changes.
        mCategoriesObserver->removeCategory(outfit_id);

        // 2. Remove the outfit from selection.
        deselectOutfit(outfit_id);

        // 3. Remove category UUID to accordion tab mapping.
        mOutfitsMap.erase(outfits_iter);

        // 4. Remove outfit tab from the accordion it actually lives in — read from the
        // view tree, since inventory may already say Trash or a new folder.
        // <ID> was unconditionally mAccordion, which silently no-ops for a nested tab
        LLAccordionCtrl* owner = id_owning_accordion(tab);
        if (!owner)
        {
            owner = mAccordion;
        }
        owner->removeCollapsibleCtrl(tab);
        // </ID>

        // kill removed tab
        if (tab != NULL)
        {
            tab->die();
        }
    }
}

// <FS:Ansariel> Arrange accordions after all have been added
//virtual
// <ID> How deep is this folder under My Outfits? Used to size folders bottom-up, since a
// parent's height depends on its children's final heights.
S32 LLOutfitsList::getFolderDepth(const LLUUID& cat_id) const
{
    const LLUUID root = gInventory.findCategoryUUIDForType(LLFolderType::FT_MY_OUTFITS);
    S32 depth = 0;
    LLUUID walk = cat_id;
    while (walk.notNull() && walk != root && depth < 64)
    {
        LLViewerInventoryCategory* cat = gInventory.getCategory(walk);
        if (!cat)
        {
            break;
        }
        walk = cat->getParentUUID();
        ++depth;
    }
    return depth;
}


// LLAccordionCtrlTab does not take its height from reshape() — setDisplayChildren()
// re-derives the rect from mExpandedHeight, so a reshape is overwritten the moment a tab
// is expanded or collapsed. The only channel that sets mExpandedHeight is a "size_changes"
// notifyParent message, which is how LLFlatListView drives ordinary outfit tabs
// (llflatlistview.cpp:1186). LLAccordionCtrl never sends it, so a nested accordion leaves
// its folder tab stuck at creation height and the contents clip. Send it ourselves.
void LLOutfitsList::resizeFolderTab(const LLUUID& cat_id)
{
    std::map<LLUUID, LLAccordionCtrl*>::iterator acc_it = mFolderAccordions.find(cat_id);
    std::map<LLUUID, LLOutfitAccordionCtrlTab*>::iterator tab_it = mFolderTabs.find(cat_id);
    if (acc_it == mFolderAccordions.end() || tab_it == mFolderTabs.end())
    {
        return;
    }
    LLAccordionCtrl* inner = acc_it->second;
    LLOutfitAccordionCtrlTab* tab = tab_it->second;
    if (!inner || !tab)
    {
        return;
    }

    S32 content_height = 0;
    LLInventoryModel::cat_array_t* cats = NULL;
    LLInventoryModel::item_array_t* items = NULL;
    gInventory.getDirectDescendentsOf(cat_id, cats, items);
    if (cats)
    {
        for (const LLPointer<LLViewerInventoryCategory>& child : *cats)
        {
            const LLUUID child_id = child->getUUID();

            LLAccordionCtrlTab* child_tab = NULL;
            std::map<LLUUID, LLOutfitAccordionCtrlTab*>::iterator f = mFolderTabs.find(child_id);
            if (f != mFolderTabs.end())
            {
                child_tab = f->second;
            }
            else
            {
                outfits_map_t::iterator o = mOutfitsMap.find(child_id);
                if (o != mOutfitsMap.end())
                {
                    child_tab = o->second;
                }
            }

            if (child_tab && child_tab->getVisible())
            {
                content_height += child_tab->getRect().getHeight();
            }
        }
    }

    // The accordion needs more than the bare sum of its tabs, or it grows a scrollbar
    // instead of letting the folder tab grow. Two margins are involved, both
    // BORDER_MARGIN (2) in llaccordionctrl.cpp: calcRecuiredHeight() adds one to the sum
    // when deciding whether to show a scrollbar, and arrangeMultiple() consumes another
    // as a top inset before laying out the first tab. Those members are private, so the
    // arithmetic has to be reproduced here rather than asked for.
    static const S32 ACCORDION_BORDER_MARGIN = 2;
    content_height += ACCORDION_BORDER_MARGIN * 2;

    // The tab's handler clamps to a 10px floor and adds the header and padding itself, so
    // pass the content height only.
    content_height = llmax(content_height, 10);

    // The message must go to the TAB, not the inner accordion: LLAccordionCtrl also
    // overrides notifyParent and its "size_changes" branch swallows the message
    // (llaccordionctrl.cpp:689) — it schedules an arrange of its own children and returns
    // without forwarding, so a notify sent through the inner accordion never reaches the
    // tab and mExpandedHeight never changes. Sent to the tab directly, its handler sets
    // mExpandedHeight, reshapes (which cascades into the inner accordion via
    // adjustContainerPanel), and forwards to the outer accordion to reposition siblings.
    tab->notifyParent(LLSD().with("action", "size_changes").with("height", content_height));

    // The reshape cascade above fixed the inner accordion's rect synchronously, but its
    // children keep their old positions until an arrange runs — and the one the reshape
    // scheduled is deferred to the next updateClass tick. Arrange now, inside the correct
    // rect, so the children never render at stale coordinates.
    inner->arrange();
}
// </ID>

void LLOutfitsList::arrange()
{
    // <ID> Size folders bottom-up: a parent folder's height is the sum of its children's
    // final heights, so the deepest must settle first. Arranging the outer accordion first
    // would size every folder tab from a stale inner height — which is exactly the
    // clipped, overlapping layout that nesting produces otherwise.
    std::vector<std::pair<S32, LLUUID> > by_depth;
    by_depth.reserve(mFolderAccordions.size());
    for (std::map<LLUUID, LLAccordionCtrl*>::iterator it = mFolderAccordions.begin();
         it != mFolderAccordions.end(); ++it)
    {
        by_depth.push_back(std::make_pair(getFolderDepth(it->first), it->first));
    }
    std::sort(by_depth.begin(), by_depth.end(),
        [](const std::pair<S32, LLUUID>& a, const std::pair<S32, LLUUID>& b)
        {
            return a.first > b.first;
        });

    for (const std::pair<S32, LLUUID>& entry : by_depth)
    {
        resizeFolderTab(entry.second);
    }
    // </ID>

    if (mAccordion)
    {
        mAccordion->arrange();
    }
}
// </FS:Ansariel>

// <FS:Ansariel> FIRE-22484: Double-click wear in outfits list
void LLOutfitsList::onDoubleClick(LLWearableItemsList* list)
{
    if (!list)
    {
        return;
    }

    LLUUID selected_item_id = list->getSelectedUUID();
    if (selected_item_id.notNull())
    {
        uuid_vec_t ids;
        ids.push_back(selected_item_id);
        LLViewerInventoryItem* item = gInventory.getItem(selected_item_id);

        if (get_is_item_worn(selected_item_id))
        {
            if ((item->getType() == LLAssetType::AT_CLOTHING && (!RlvActions::isRlvEnabled() || gRlvWearableLocks.canRemove(item))) ||
                ((item->getType() == LLAssetType::AT_OBJECT) && (!RlvActions::isRlvEnabled() || gRlvAttachmentLocks.canDetach(item))))
            {
                LLAppearanceMgr::instance().removeItemsFromAvatar(ids);
            }
        }
        else
        {
            if (item->getType() == LLAssetType::AT_BODYPART && (!RlvActions::isRlvEnabled() || (gRlvWearableLocks.canWear(item) & RLV_WEAR_REPLACE) == RLV_WEAR_REPLACE))
            {
                wear_multiple(ids, true);
            }
            else if (item->getType() == LLAssetType::AT_CLOTHING && LLAppearanceMgr::instance().canAddWearables(ids) && (!RlvActions::isRlvEnabled() || (gRlvWearableLocks.canWear(item) & RLV_WEAR_ADD) == RLV_WEAR_ADD))
            {
                wear_multiple(ids, false);
            }
            else if (item->getType() == LLAssetType::AT_OBJECT && LLAppearanceMgr::instance().canAddWearables(ids) && (!RlvActions::isRlvEnabled() || (gRlvAttachmentLocks.canAttach(item) & RLV_WEAR_ADD) == RLV_WEAR_ADD))
            {
                wear_multiple(ids, false);
            }
        }
    }
}
// </FS:Ansariel>

//virtual
void LLOutfitsList::onHighlightBaseOutfit(LLUUID base_id, LLUUID prev_id)
{
    if (mOutfitsMap[prev_id])
    {
        mOutfitsMap[prev_id]->setOutfitSelected(false);
    }
    if (mOutfitsMap[base_id])
    {
        mOutfitsMap[base_id]->setOutfitSelected(true);
    }
}

void LLOutfitsList::onListSelectionChange(LLUICtrl* ctrl)
{
    LLWearableItemsList* list = dynamic_cast<LLWearableItemsList*>(ctrl);
    if (!list) return;

    LLViewerInventoryItem *item = gInventory.getItem(list->getSelectedUUID());
    if (!item) return;

    ChangeOutfitSelection(list, item->getParentUUID());
}

void LLOutfitListBase::performAction(std::string action)
{
    if (mSelectedOutfitUUID.isNull()) return;

    LLViewerInventoryCategory* cat = gInventory.getCategory(mSelectedOutfitUUID);
    if (!cat) return;

    if ("replaceoutfit" == action)
    {
        LLAppearanceMgr::instance().wearInventoryCategory( cat, false, false );
    }
    if ("replaceitems" == action)
    {
        LLAppearanceMgr::instance().wearInventoryCategory( cat, false, true );
    }
    else if ("addtooutfit" == action)
    {
        LLAppearanceMgr::instance().wearInventoryCategory( cat, false, true );
    }
    else if ("rename_outfit" == action)
    {
        LLAppearanceMgr::instance().renameOutfit(mSelectedOutfitUUID);
    }
}

void LLOutfitsList::onSetSelectedOutfitByUUID(const LLUUID& outfit_uuid)
{
    for (outfits_map_t::iterator iter = mOutfitsMap.begin();
            iter != mOutfitsMap.end();
            ++iter)
    {
        if (outfit_uuid == iter->first)
        {
            LLOutfitAccordionCtrlTab* tab = iter->second;
            if (!tab) continue;

            LLWearableItemsList* list = dynamic_cast<LLWearableItemsList*>(tab->getAccordionView());
            if (!list) continue;

            tab->setFocus(true);
            ChangeOutfitSelection(list, outfit_uuid);

            tab->changeOpenClose(false);
        }
    }
}

void LLOutfitListBase::onAction(const LLSD& userdata)
{
    performAction(userdata.asString());
}

// virtual
bool LLOutfitListBase::isActionEnabled(const LLSD& userdata)
{
    if (mSelectedOutfitUUID.isNull()) return false;

    // <ID> A subfolder is not an outfit — every action below assumes it is one. Clicking a
    // folder tile or tab routes through ChangeOutfitSelection like any other entry, so
    // this is a real state, not a defensive one.
    LLViewerInventoryCategory* selected_cat = gInventory.getCategory(mSelectedOutfitUUID);
    if (selected_cat && !isOutfitFolder(selected_cat))
    {
        return false;
    }
    // </ID>

    const std::string command_name = userdata.asString();
    if (command_name == "delete")
    {
        return !hasItemSelected() && LLAppearanceMgr::instance().getCanRemoveOutfit(mSelectedOutfitUUID);
    }
    if (command_name == "rename")
    {
        return get_is_category_renameable(&gInventory, mSelectedOutfitUUID);
    }
    if (command_name == "save_outfit")
    {
        bool outfit_locked = LLAppearanceMgr::getInstance()->isOutfitLocked();
        bool outfit_dirty = LLAppearanceMgr::getInstance()->isOutfitDirty();
        // allow save only if outfit isn't locked and is dirty
        return !outfit_locked && outfit_dirty;
    }
    if (command_name == "wear")
    {
        if (gAgentWearables.isCOFChangeInProgress())
        {
            return false;
        }

        if (hasItemSelected())
        {
            return canWearSelected();
        }

        // outfit selected
        return LLAppearanceMgr::instance().getCanReplaceCOF(mSelectedOutfitUUID);
    }
    if (command_name == "take_off")
    {
        // Enable "Take Off" if any of selected items can be taken off
        // or the selected outfit contains items that can be taken off.
        return ( hasItemSelected() && canTakeOffSelected() )
                || ( !hasItemSelected() && LLAppearanceMgr::getCanRemoveFromCOF(mSelectedOutfitUUID) );
    }

    if (command_name == "wear_add")
    {
        // *TODO: do we ever get here?
        return LLAppearanceMgr::getCanAddToCOF(mSelectedOutfitUUID);
    }

    return false;
}

void LLOutfitsList::getSelectedItemsUUIDs(uuid_vec_t& selected_uuids) const
{
    // Collect selected items from all selected lists.
    for (wearables_lists_map_t::const_iterator iter = mSelectedListsMap.begin();
            iter != mSelectedListsMap.end();
            ++iter)
    {
        uuid_vec_t uuids;
        (*iter).second->getSelectedUUIDs(uuids);

        auto prev_size = selected_uuids.size();
        selected_uuids.resize(prev_size + uuids.size());
        std::copy(uuids.begin(), uuids.end(), selected_uuids.begin() + prev_size);
    }
}

void LLOutfitsList::onCollapseAllFolders()
{
    for (outfits_map_t::iterator iter = mOutfitsMap.begin();
            iter != mOutfitsMap.end();
            ++iter)
    {
        LLOutfitAccordionCtrlTab* tab = iter->second;
        if(tab && tab->isExpanded())
        {
            tab->changeOpenClose(true);
        }
    }
}

void LLOutfitsList::onExpandAllFolders()
{
    for (outfits_map_t::iterator iter = mOutfitsMap.begin();
            iter != mOutfitsMap.end();
            ++iter)
    {
        LLOutfitAccordionCtrlTab* tab = iter->second;
        if(tab && !tab->isExpanded())
        {
            tab->changeOpenClose(false);
        }
    }
}

bool LLOutfitsList::hasItemSelected()
{
    return mItemSelected;
}

//////////////////////////////////////////////////////////////////////////
// Private methods
//////////////////////////////////////////////////////////////////////////

void LLOutfitsList::updateChangedCategoryName(LLViewerInventoryCategory *cat, std::string name)
{
    // <ID> Folder tabs are tracked separately from outfit tabs and rename too.
    std::map<LLUUID, LLOutfitAccordionCtrlTab*>::iterator folder_iter = mFolderTabs.find(cat->getUUID());
    if (folder_iter != mFolderTabs.end())
    {
        LLOutfitAccordionCtrlTab* folder_tab = folder_iter->second;
        if (folder_tab)
        {
            folder_tab->setName(name);
            folder_tab->setTitle(name);
            folder_tab->setFavorite(cat->getIsFavorite());
        }
        return;
    }
    // </ID>

    outfits_map_t::iterator outfits_iter = mOutfitsMap.find(cat->getUUID());
    if (outfits_iter != mOutfitsMap.end())
    {
        // Update tab name with the new category name.
        LLOutfitAccordionCtrlTab* tab = outfits_iter->second;
        if (tab)
        {
            tab->setName(name);
            tab->setTitle(name);
            tab->setFavorite(cat->getIsFavorite());
        }
    }
}

void LLOutfitsList::resetItemSelection(LLWearableItemsList* list, const LLUUID& category_id)
{
    list->resetSelection();
    mItemSelected = false;
    signalSelectionOutfitUUID(category_id);
}

void LLOutfitsList::onChangeOutfitSelection(LLWearableItemsList* list, const LLUUID& category_id)
{
    MASK mask = gKeyboard->currentMask(true);

    // Reset selection in all previously selected tabs except for the current
    // if new selection is started.
    if (list && !(mask & MASK_CONTROL))
    {
        for (wearables_lists_map_t::iterator iter = mSelectedListsMap.begin();
                iter != mSelectedListsMap.end();
                ++iter)
        {
            LLWearableItemsList* selected_list = (*iter).second;
            if (selected_list != list)
            {
                selected_list->resetSelection();
            }
        }

        // Clear current selection.
        mSelectedListsMap.clear();
    }

    mItemSelected = list && (list->getSelectedItem() != NULL);

    mSelectedListsMap.insert(wearables_lists_map_value_t(category_id, list));
}

void LLOutfitsList::deselectOutfit(const LLUUID& category_id)
{
    // Remove selected lists map entry.
    mSelectedListsMap.erase(category_id);

    LLOutfitListBase::deselectOutfit(category_id);
}

void LLOutfitsList::restoreOutfitSelection(LLOutfitAccordionCtrlTab* tab, const LLUUID& category_id)
{
    // Try restoring outfit selection after filtering.
    if (mAccordion->getSelectedTab() == tab)
    {
        signalSelectionOutfitUUID(category_id);
    }
}

void LLOutfitsList::onRefreshComplete(LLUICtrl* ctrl, LLOutfitAccordionCtrlTab* tab)
{
    if (!ctrl || getFilterSubString().empty())
        return;

    LLWearableItemsList* list = dynamic_cast<LLWearableItemsList*>(tab->getAccordionView());
    if (list != ctrl)
    {
        llassert(false);
        LL_WARNS() << "LLOutfitsList::onRefreshComplete: ctrl does not match tab's list!" << LL_ENDL;
        return;
    }
    if (tab->getFilterGeneration() != getFilterGeneration())
    {
        applyFilterToTab(tab->getFolderID(), tab, getFilterSubString());
    }
}

// virtual
void LLOutfitsList::onFilterSubStringChanged(const std::string& new_string, const std::string& old_string)
{
    mAccordion->setFilterSubString(new_string);

    outfits_map_t::iterator iter = mOutfitsMap.begin(), iter_end = mOutfitsMap.end();
    while (iter != iter_end)
    {
        const LLUUID& category_id = iter->first;
        LLOutfitAccordionCtrlTab* tab = iter++->second;
        if (!tab) continue;

        LLWearableItemsList* list = dynamic_cast<LLWearableItemsList*>(tab->getAccordionView());
        if (list)
        {
            list->setFilterSubString(new_string, tab->getDisplayChildren());
        }

        if (old_string.empty())
        {
            // Store accordion tab state when filter is not empty
            tab->notifyChildren(LLSD().with("action", "store_state"));
        }

        if (!new_string.empty())
        {
            applyFilterToTab(category_id, tab, new_string);
        }
        else
        {
            tab->setVisible(true);

            // Restore tab title when filter is empty
            tab->setTitle(tab->getTitle());

            // Restore accordion state after all those accodrion tab manipulations
            tab->notifyChildren(LLSD().with("action", "restore_state"));

            // Try restoring the tab selection.
            restoreOutfitSelection(tab, category_id);
        }
    }

    // <ID> The loop above walks mOutfitsMap only, so folder tabs would never be filtered
    // and — worse — would never be made visible again once a filter was cleared. Handle
    // them here. Only root-level folders are seeded: applyFilterToFolderTab recurses
    // depth-first, so nested folders are reached with their children already resolved.
    const LLUUID outfits_root = gInventory.findCategoryUUIDForType(LLFolderType::FT_MY_OUTFITS);
    for (std::map<LLUUID, LLOutfitAccordionCtrlTab*>::iterator fit = mFolderTabs.begin();
         fit != mFolderTabs.end(); ++fit)
    {
        LLOutfitAccordionCtrlTab* folder_tab = fit->second;
        if (!folder_tab)
        {
            continue;
        }

        if (new_string.empty())
        {
            folder_tab->setVisible(true);
            folder_tab->setTitle(folder_tab->getTitle());
            continue;
        }

        LLViewerInventoryCategory* cat = gInventory.getCategory(fit->first);
        if (cat && cat->getParentUUID() == outfits_root)
        {
            applyFilterToFolderTab(fit->first, folder_tab, new_string);
        }
    }

    for (std::map<LLUUID, LLAccordionCtrl*>::iterator ait = mFolderAccordions.begin();
         ait != mFolderAccordions.end(); ++ait)
    {
        if (ait->second)
        {
            ait->second->setFilterSubString(new_string);
            ait->second->arrange();
        }
    }

    // Filtering changes which nested tabs are visible, so folder heights must be
    // recomputed; arrange() does that bottom-up and ends with mAccordion->arrange().
    arrange();
    // </ID>
}

void LLOutfitsList::applyFilterToTab(
    const LLUUID&       category_id,
    LLOutfitAccordionCtrlTab* tab,
    const std::string&  filter_substring)
{
    LL_PROFILE_ZONE_SCOPED;
    if (!tab) return;

    // <ID> A folder tab's body is a nested accordion, not a wearable list, so it would
    // fall straight through the dynamic_cast below and escape the filter entirely —
    // visible, with every child hidden. Route it through the folder rule instead.
    if (mFolderTabs.count(category_id))
    {
        applyFilterToFolderTab(category_id, tab, filter_substring);
        return;
    }
    // </ID>

    LLWearableItemsList* list = dynamic_cast<LLWearableItemsList*>(tab->getAccordionView());
    if (!list) return;

    std::string title = tab->getTitle();
    LLStringUtil::toUpper(title);

    std::string cur_filter = filter_substring;
    LLStringUtil::toUpper(cur_filter);

    tab->setTitle(tab->getTitle(), cur_filter);
    tab->setFilterGeneration(getFilterGeneration());

    if (std::string::npos == title.find(cur_filter))
    {
        // Hide tab if its title doesn't pass filter
        // and it has no matched items
        tab->setVisible(list->hasMatchedItems());

        // Remove title highlighting because it might
        // have been previously highlighted by less restrictive filter
        tab->setTitle(tab->getTitle());

        // Remove the tab from selection.
        deselectOutfit(category_id);
    }
    else
    {
        tab->setVisible(true); // <FS:PP> FIRE-36651 Fix outfit search so accordion tabs become visible again when the filter matches the outfit name after a typo
        // Try restoring the tab selection.
        restoreOutfitSelection(tab, category_id);
    }
}

bool LLOutfitsList::canWearSelected()
{
    if (!isAgentAvatarValid())
    {
        return false;
    }

    uuid_vec_t selected_items;
    getSelectedItemsUUIDs(selected_items);
    S32 nonreplacable_objects = 0;

    for (uuid_vec_t::const_iterator it = selected_items.begin(); it != selected_items.end(); ++it)
    {
        const LLUUID& id = *it;

        // Check whether the item is worn.
        if (!get_can_item_be_worn(id))
        {
            return false;
        }

        const LLViewerInventoryItem* item = gInventory.getItem(id);
        if (!item)
        {
            return false;
        }

        if (item->getType() == LLAssetType::AT_OBJECT)
        {
            nonreplacable_objects++;
        }
    }

    // All selected items can be worn. But do we have enough space for them?
    return nonreplacable_objects == 0 || gAgentAvatarp->canAttachMoreObjects(nonreplacable_objects);
}

void LLOutfitsList::wearSelectedItems()
{
    uuid_vec_t selected_uuids;
    getSelectedItemsUUIDs(selected_uuids);

    if(selected_uuids.empty())
    {
        return;
    }

    wear_multiple(selected_uuids, false);
}

void LLOutfitsList::onWearableItemsListRightClick(LLUICtrl* ctrl, S32 x, S32 y)
{
    LLWearableItemsList* list = dynamic_cast<LLWearableItemsList*>(ctrl);
    if (!list) return;

    uuid_vec_t selected_uuids;

    getSelectedItemsUUIDs(selected_uuids);

    LLWearableItemsList::ContextMenu::instance().show(list, selected_uuids, x, y);
}

void LLOutfitsList::onCOFChanged()
{
// <ID:i.doll> [COF worn-state refresh performance]
    if (!isInVisibleChain())
    {
        mCOFRefreshPending = true;
        return;
    }
// </ID:i.doll>

    LLInventoryModel::cat_array_t cat_array;
    LLInventoryModel::item_array_t item_array;

    // Collect current COF items
    gInventory.collectDescendents(
        LLAppearanceMgr::instance().getCOF(),
        cat_array,
        item_array,
        LLInventoryModel::EXCLUDE_TRASH);

    uuid_vec_t vnew;
    uuid_vec_t vadded;
    uuid_vec_t vremoved;

    // From gInventory we get the UUIDs of links that are currently in COF.
    // These links UUIDs are not the same UUIDs that we have in each wearable items list.
    // So we collect base items' UUIDs to find them or links that point to them in wearable
    // items lists and update their worn state there.
    LLInventoryModel::item_array_t::const_iterator array_iter = item_array.begin(), array_end = item_array.end();
    while (array_iter < array_end)
    {
        vnew.push_back((*(array_iter++))->getLinkedUUID());
    }

    // We need to update only items that were added or removed from COF.
    LLCommonUtils::computeDifference(vnew, mCOFLinkedItems, vadded, vremoved);

    // Store the ids of items currently linked from COF.
    mCOFLinkedItems = vnew;

    // Append removed ids to added ids because we should update all of them.
    vadded.reserve(vadded.size() + vremoved.size());
    vadded.insert(vadded.end(), vremoved.begin(), vremoved.end());
    vremoved.clear();

    outfits_map_t::iterator map_iter = mOutfitsMap.begin(), map_end = mOutfitsMap.end();
    while (map_iter != map_end)
    {
        LLOutfitAccordionCtrlTab* tab = (map_iter++)->second;
        if (!tab) continue;

        LLWearableItemsList* list = dynamic_cast<LLWearableItemsList*>(tab->getAccordionView());
        if (!list) continue;

        // Every list updates the labels of changed items  or
        // the links that point to these items.
        list->updateChangedItems(vadded);
    }
}

void LLOutfitsList::getCurrentCategories(uuid_vec_t& vcur)
{
    // Creating a vector of currently displayed sub-categories UUIDs.
    for (outfits_map_t::const_iterator iter = mOutfitsMap.begin();
        iter != mOutfitsMap.end();
        iter++)
    {
        vcur.push_back((*iter).first);
    }
}


// <ID> The refresh diff reports adds and removes only — a category whose PARENT changed
// is in neither list, so after a move in inventory its tab would stay under the old
// accordion forever (while the folder heights, computed from inventory truth, already
// account for it). Sweep every known tab and rebuild the ones whose owning accordion no
// longer matches inventory. Rebuild (remove + re-add) instead of moving the widget:
// addCollapsibleCtrl wires callbacks bound to the owning accordion that cannot be
// unhooked, and a migrated widget would keep firing into its old home.
void LLOutfitsList::reparentMovedTabs()
{
    std::vector<LLUUID> moved_folders;
    for (std::map<LLUUID, LLOutfitAccordionCtrlTab*>::iterator it = mFolderTabs.begin();
         it != mFolderTabs.end(); ++it)
    {
        LLAccordionCtrl* desired = getParentAccordion(it->first);
        if (desired && desired != id_owning_accordion(it->second))
        {
            moved_folders.push_back(it->first);
        }
    }

    bool moved_any = false;

    // Folders first: rebuilding one rebuilds its whole subtree, which may already fix
    // outfits that moved along with it.
    for (const LLUUID& folder_id : moved_folders)
    {
        LLInventoryModel::cat_array_t cats;
        LLInventoryModel::item_array_t items;
        LLIsType is_category(LLAssetType::AT_CATEGORY);
        gInventory.collectDescendentsIf(folder_id, cats, items,
                                        LLInventoryModel::EXCLUDE_TRASH, is_category);

        // Children first: their views die with the folder tab, but the maps and observer
        // registrations must be cleaned by the same path that created them.
        for (const LLPointer<LLViewerInventoryCategory>& cat : cats)
        {
            updateRemovedCategory(cat->getUUID());
        }
        updateRemovedCategory(folder_id);

        updateAddedCategory(folder_id);
        for (const LLPointer<LLViewerInventoryCategory>& cat : cats)
        {
            // Ordering inside the subtree resolves itself: a child whose parent tab is
            // not built yet parks in mPendingChildren and is flushed when it appears.
            updateAddedCategory(cat->getUUID());
        }
        moved_any = true;
    }

    std::vector<LLUUID> moved_outfits;
    for (outfits_map_t::iterator it = mOutfitsMap.begin(); it != mOutfitsMap.end(); ++it)
    {
        LLAccordionCtrl* desired = getParentAccordion(it->first);
        if (desired && desired != id_owning_accordion(it->second))
        {
            moved_outfits.push_back(it->first);
        }
    }
    for (const LLUUID& outfit_id : moved_outfits)
    {
        updateRemovedCategory(outfit_id);
        updateAddedCategory(outfit_id);
        moved_any = true;
    }

    if (moved_any)
    {
        arrange();
    }
}
// </ID>

void LLOutfitsList::sortOutfits()
{
    reparentMovedTabs();

    // <ID> Sort nested contents too, or subfolders keep insertion order.
    for (std::map<LLUUID, LLAccordionCtrl*>::iterator it = mFolderAccordions.begin();
         it != mFolderAccordions.end(); ++it)
    {
        if (it->second)
        {
            it->second->sort();
        }
    }
    // </ID>
    mAccordion->sort();
}

void LLOutfitsList::onOutfitRightClick(LLUICtrl* ctrl, S32 x, S32 y, const LLUUID& cat_id)
{
    LLOutfitAccordionCtrlTab* tab = dynamic_cast<LLOutfitAccordionCtrlTab*>(ctrl);
    if (mOutfitMenu && is_tab_header_clicked(tab, y) && cat_id.notNull())
    {
        // Focus tab header to trigger tab selection change.
        LLUICtrl* header = tab->findChild<LLUICtrl>("dd_header");
        if (header)
        {
            header->setFocus(true);
        }

        uuid_vec_t selected_uuids;
        selected_uuids.push_back(cat_id);
        mOutfitMenu->show(ctrl, selected_uuids, x, y);
    }
}

void LLOutfitsList::handleInvFavColorChange()
{
    for (outfits_map_t::iterator iter = mOutfitsMap.begin();
        iter != mOutfitsMap.end();
        ++iter)
    {
        if (!iter->second) continue;
        LLOutfitAccordionCtrlTab* tab = iter->second;

        // refresh font color
        tab->setFavorite(tab->getFavorite());
    }
}

void LLOutfitsList::onChangeSortOrder(const LLSD& userdata)
{
    std::string sort_data = userdata.asString();
    if (sort_data == "favorites_to_top")
    {
        // at the moment this is a toggle
        S32 val = gSavedSettings.getS32("OutfitListSortOrder");
        gSavedSettings.setS32("OutfitListSortOrder", (val ? 0 : 1));

        initComparator();
    }
    else if (sort_data == "show_entire_outfit")
    {
        bool new_val = !gSavedSettings.getBOOL("OutfitListFilterFullList");
        gSavedSettings.setBOOL("OutfitListFilterFullList", new_val);

        if (!getFilterSubString().empty())
        {
            for (outfits_map_t::value_type& outfit : mOutfitsMap)
            {
                LLOutfitAccordionCtrlTab* tab = outfit.second;
                const LLUUID& category_id = outfit.first;
                if (!tab) continue;

                LLWearableItemsList* list = dynamic_cast<LLWearableItemsList*>(tab->getAccordionView());
                if (list)
                {
                    list->setForceRefresh(true);
                    list->setForceShowingUnmatchedItems(new_val, tab->getDisplayChildren());
                }
                applyFilterToTab(category_id, tab, getFilterSubString());
            }
            mAccordion->arrange();
        }
    }
}

LLToggleableMenu* LLOutfitsList::getSortMenu()
{
    if (!mSortMenu)
    {
        mSortMenu = new LLOutfitListSortMenu(this);
    }
    return mSortMenu->getMenu();
}

void LLOutfitsList::updateMenuItemsVisibility()
{
    if (mSortMenu)
    {
        mSortMenu->updateItemsVisibility();
    }
    LLOutfitListBase::updateMenuItemsVisibility();
}

LLOutfitListGearMenuBase* LLOutfitsList::createGearMenu()
{
    return new LLOutfitListGearMenu(this);
}


bool is_tab_header_clicked(LLOutfitAccordionCtrlTab* tab, S32 y)
{
    if(!tab || !tab->getHeaderVisible()) return false;

    S32 header_bottom = tab->getLocalRect().getHeight() - tab->getHeaderHeight();
    return y >= header_bottom;
}

LLOutfitListBase::LLOutfitListBase()
    :   LLPanelAppearanceTab()
    ,   mIsInitialized(false)
    ,   mGearMenu(nullptr)
    ,   mAvatarComplexityLabel(NULL) // <FS:Ansariel> Show avatar complexity in appearance floater
{
    mCategoriesObserver = new LLInventoryCategoriesObserver();
    mOutfitMenu = new LLOutfitContextMenu(this);
}

LLOutfitListBase::~LLOutfitListBase()
{
    delete mOutfitMenu;
    delete mGearMenu;

    if (gInventory.containsObserver(mCategoriesObserver))
    {
        gInventory.removeObserver(mCategoriesObserver);
    }
    delete mCategoriesObserver;
}

void LLOutfitListBase::onOpen(const LLSD& info)
{
    if (!mIsInitialized)
    {
        // *TODO: I'm not sure is this check necessary but it never match while developing.
        if (!gInventory.isInventoryUsable())
            return;

        const LLUUID outfits = gInventory.findCategoryUUIDForType(LLFolderType::FT_MY_OUTFITS);

        // *TODO: I'm not sure is this check necessary but it never match while developing.
        LLViewerInventoryCategory* category = gInventory.getCategory(outfits);
        if (!category)
            return;

        gInventory.addObserver(mCategoriesObserver);

        // Start observing changes in "My Outfits" category.
        mCategoriesObserver->addCategory(outfits,
            boost::bind(&LLOutfitListBase::observerCallback, this, outfits));

        //const LLUUID cof = gInventory.findCategoryUUIDForType(LLFolderType::FT_CURRENT_OUTFIT);
        // Start observing changes in Current Outfit category.
        //mCategoriesObserver->addCategory(cof, boost::bind(&LLOutfitsList::onCOFChanged, this));

        LLOutfitObserver::instance().addBOFChangedCallback(boost::bind(&LLOutfitListBase::highlightBaseOutfit, this));
        LLOutfitObserver::instance().addBOFReplacedCallback(boost::bind(&LLOutfitListBase::highlightBaseOutfit, this));

        // Fetch "My Outfits" contents and refresh the list to display
        // initially fetched items. If not all items are fetched now
        // the observer will refresh the list as soon as the new items
        // arrive.
        category->fetch();
        refreshList(outfits);

        mIsInitialized = true;
    }
}

void LLOutfitListBase::observerCallback(const LLUUID& category_id)
{
    const LLInventoryModel::changed_items_t& changed_items = gInventory.getChangedIDs();
    mChangedItems.insert(changed_items.begin(), changed_items.end());
    refreshList(category_id);
}

bool LLOutfitListBase::isOutfitFolder(LLViewerInventoryCategory* cat) const
{
    if (!cat)
    {
        return false;
    }
    if (cat->getPreferredType() == LLFolderType::FT_OUTFIT)
    {
        return true;
    }
    // assumes that folder is somewhere inside MyOutfits
    if (cat->getPreferredType() == LLFolderType::FT_NONE)
    {
        LLViewerInventoryCategory* inv_cat = dynamic_cast<LLViewerInventoryCategory*>(cat);
        if (inv_cat && inv_cat->getDescendentCount() > 3)
        {
            LLInventoryModel::cat_array_t* cats;
            LLInventoryModel::item_array_t* items;
            gInventory.getDirectDescendentsOf(inv_cat->getUUID(), cats, items);
            if (cats->empty() // protection against outfits inside
                && items->size() > 3) // arbitrary, if doesn't have at least base parts, not an outfit
            {
                // For now assume this to be an old style outfit, not a subfolder
                // but ideally no such 'outfits' should be left in My Outfits
                // Todo: stop counting FT_NONE as outfits,
                // convert obvious outfits into FT_OUTFIT
                return true;
            }
        }
    }
    return false;
}

void LLOutfitListBase::refreshList(const LLUUID& category_id)
{
    if (LLAppViewer::instance()->quitRequested())
    {
        return;
    }
    bool wasNull = mRefreshListState.CategoryUUID.isNull();

    // <FS:PP> FIRE-36116 (saving a second outfit freezes Firestorm indefinitely)
    static LLCachedControl<bool> fsExperimentalOutfitsReturn(gSavedSettings, "FSExperimentalOutfitsReturn");
    if (fsExperimentalOutfitsReturn && !wasNull && mRefreshListState.CategoryUUID == category_id)
    {
        return;
    }
    // </FS:PP>

    mRefreshListState.CategoryUUID.setNull();

    LLInventoryModel::cat_array_t cat_array;
    LLInventoryModel::item_array_t item_array;

    // Collect all sub-categories of a given category.
    LLIsType is_category(LLAssetType::AT_CATEGORY);
    gInventory.collectDescendentsIf(
        category_id,
        cat_array,
        item_array,
        LLInventoryModel::EXCLUDE_TRASH,
        is_category);

    // Memorize item names for each UUID
    std::map<LLUUID, std::string> names;
    for (const LLPointer<LLViewerInventoryCategory>& cat : cat_array)
    {
        names.emplace(std::make_pair(cat->getUUID(), cat->getName()));
    }

    // Fill added and removed items vectors.
    mRefreshListState.Added.clear();
    mRefreshListState.Removed.clear();
    computeDifference(cat_array, mRefreshListState.Added, mRefreshListState.Removed);
    // Sort added items vector by item name.
    std::sort(mRefreshListState.Added.begin(), mRefreshListState.Added.end(),
        [names](const LLUUID& a, const LLUUID& b)
        {
            return LLStringUtil::compareDict(names.at(a), names.at(b)) < 0;
        });
    // Initialize iterators for added and removed items vectors.
    mRefreshListState.AddedIterator = mRefreshListState.Added.begin();
    mRefreshListState.RemovedIterator = mRefreshListState.Removed.begin();

    LL_INFOS() << "added: " << mRefreshListState.Added.size() <<
        ", removed: " << mRefreshListState.Removed.size() <<
        ", changed: " << gInventory.getChangedIDs().size() <<
        LL_ENDL;

    mRefreshListState.CategoryUUID = category_id;
    if (wasNull)
    {
        gIdleCallbacks.addFunction(onIdle, this);
    }

    // <FS:ND> FIRE-6958/VWR-2862; Handle large amounts of outfits, write a least a warning into the logs.
    S32 currentOutfitsAmount = (S32)mRefreshListState.Added.size();
    constexpr S32 maxSuggestedOutfits = 1000;
    if (currentOutfitsAmount > maxSuggestedOutfits)
    {
        LL_WARNS() << "Large amount of outfits found: " << currentOutfitsAmount << " this may cause hangs and disconnects" << LL_ENDL;
        static LLCachedControl<bool> fsLargeOutfitsWarningInThisSession(gSavedSettings, "FSLargeOutfitsWarningInThisSession");
        if (!fsLargeOutfitsWarningInThisSession)
        {
            gSavedSettings.setBOOL("FSLargeOutfitsWarningInThisSession", true);
            LLSD args;
            args["AMOUNT"] = currentOutfitsAmount;
            args["MAX"] = maxSuggestedOutfits;
            LLNotificationsUtil::add("FSLargeOutfitsWarningInThisSession", args);
        }
    }
    // </FS:ND>

    // <FS:Ansariel> FIRE-12939: Add outfit count to outfits list
    {
        std::string count_string;
        LLLocale locale("");
        LLResMgr::getInstance()->getIntegerString(count_string, (S32)cat_array.size());
        getChild<LLTextBox>("OutfitcountText")->setTextArg("COUNT", count_string);
    }
    // </FS:Ansariel>
}

void LLOutfitListBase::startIdleLoop(const LLUUID cat_id)
{
    if (mRefreshListState.CategoryUUID.isNull())
    {
        mRefreshListState.CategoryUUID = cat_id;
        gIdleCallbacks.addFunction(onIdle, this);
    }
}

// static
void LLOutfitListBase::onIdle(void* userdata)
{
    LLOutfitListBase* self = (LLOutfitListBase*)userdata;

    self->onIdleRefreshList();
}

void LLOutfitListBase::onIdleRefreshList()
{
    LL_PROFILE_ZONE_SCOPED;
    if (LLAppViewer::instance()->quitRequested())
    {
        mRefreshListState.CategoryUUID.setNull();
        gIdleCallbacks.deleteFunction(onIdle, this);
        return;
    }
    if (mRefreshListState.CategoryUUID.isNull())
    {
        LL_WARNS() << "Called onIdleRefreshList without id" << LL_ENDL;
        gIdleCallbacks.deleteFunction(onIdle, this);
        return;
    }

    // <FS:PP> Scale MAX_TIME with FPS to avoid overloading the viewer with function calls at low frame rates
    // const F64 MAX_TIME = 0.005f;
    F64 MAX_TIME = 0.005f;
    constexpr F64 min_time = 0.001f;
    constexpr F64 threshold_fps = 30.0;
    const auto current_fps = LLTrace::get_frame_recording().getPeriodMedianPerSec(LLStatViewer::FPS,10);
    if (current_fps < threshold_fps)
    {
        MAX_TIME = min_time + (current_fps / threshold_fps) * (MAX_TIME - min_time);
    }
    // </FS:PP>

    F64 curent_time = LLTimer::getTotalSeconds();
    const F64 end_time = curent_time + MAX_TIME;

    // Handle added tabs.
    while (mRefreshListState.AddedIterator < mRefreshListState.Added.end())
    {
        const LLUUID cat_id = (*mRefreshListState.AddedIterator++);
        updateAddedCategory(cat_id);

        curent_time = LLTimer::getTotalSeconds();
        if (curent_time >= end_time)
            return;
    }
    mRefreshListState.Added.clear();
    mRefreshListState.AddedIterator = mRefreshListState.Added.end();

    // Handle removed tabs.
    while (mRefreshListState.RemovedIterator < mRefreshListState.Removed.end())
    {
        const LLUUID cat_id = (*mRefreshListState.RemovedIterator++);
        updateRemovedCategory(cat_id);

        curent_time = LLTimer::getTotalSeconds();
        if (curent_time >= end_time)
            return;
    }
    mRefreshListState.Removed.clear();
    mRefreshListState.RemovedIterator = mRefreshListState.Removed.end();

    // Get changed items from inventory model and update outfit tabs
    // which might have been renamed.
    while (!mChangedItems.empty())
    {
        std::set<LLUUID>::const_iterator items_iter = mChangedItems.begin();
        LLViewerInventoryCategory *cat = gInventory.getCategory(*items_iter);
        mChangedItems.erase(items_iter);

        // Links aren't supposed to be allowed here, check only cats
        if (cat)
        {
            std::string name = cat->getName();
            updateChangedCategoryName(cat, name);
        }

        curent_time = LLTimer::getTotalSeconds();
        if (curent_time >= end_time)
            return;
    }

    // Let derived classes process their own updates.
    while (updateOneOutfit())
    {
        curent_time = LLTimer::getTotalSeconds();
        if (curent_time >= end_time)
            return;
    }

    sortOutfits();
    highlightBaseOutfit();

    gIdleCallbacks.deleteFunction(onIdle, this);
    mRefreshListState.CategoryUUID.setNull();

    LL_INFOS() << "done" << LL_ENDL;
}

void LLOutfitListBase::computeDifference(
    const LLInventoryModel::cat_array_t& vcats,
    uuid_vec_t& vadded,
    uuid_vec_t& vremoved)
{
    uuid_vec_t vnew;
    // Creating a vector of newly collected sub-categories UUIDs.
    for (LLInventoryModel::cat_array_t::const_iterator iter = vcats.begin();
        iter != vcats.end();
        iter++)
    {
        vnew.push_back((*iter)->getUUID());
    }

    uuid_vec_t vcur;
    getCurrentCategories(vcur);

    LLCommonUtils::computeDifference(vnew, vcur, vadded, vremoved);
}

void LLOutfitListBase::sortOutfits()
{
}

void LLOutfitListBase::highlightBaseOutfit()
{
    // id of base outfit
    LLUUID base_id = LLAppearanceMgr::getInstance()->getBaseOutfitUUID();
    if (base_id != mHighlightedOutfitUUID)
    {
        LLUUID prev_id = mHighlightedOutfitUUID;
        mHighlightedOutfitUUID = base_id;
        onHighlightBaseOutfit(base_id, prev_id);
    }
}

void LLOutfitListBase::removeSelected()
{
    // <FS:Ansariel> FIRE-15888: Include outfit name in delete outfit confirmation dialog
    LLViewerInventoryCategory* cat = gInventory.getCategory(mSelectedOutfitUUID);
    if (cat)
    {
        LLSD args;
        args["NAME"] = cat->getName();
        LLNotificationsUtil::add("DeleteOutfitsWithName", args, LLSD(), boost::bind(&LLOutfitsList::onOutfitsRemovalConfirmation, this, _1, _2));
    }
    else
    // </FS:Ansariel>
    LLNotificationsUtil::add("DeleteOutfits", LLSD(), LLSD(), boost::bind(&LLOutfitListBase::onOutfitsRemovalConfirmation, this, _1, _2));
}

void LLOutfitListBase::onOutfitsRemovalConfirmation(const LLSD& notification, const LLSD& response)
{
    S32 option = LLNotificationsUtil::getSelectedOption(notification, response);
    if (option != 0) return; // canceled

    if (mSelectedOutfitUUID.notNull())
    {
        gInventory.removeCategory(mSelectedOutfitUUID);
    }
}

void LLOutfitListBase::setSelectedOutfitByUUID(const LLUUID& outfit_uuid)
{
    onSetSelectedOutfitByUUID(outfit_uuid);
}

boost::signals2::connection LLOutfitListBase::setSelectionChangeCallback(selection_change_callback_t cb)
{
    return mSelectionChangeSignal.connect(cb);
}

void LLOutfitListBase::signalSelectionOutfitUUID(const LLUUID& category_id)
{
    mSelectionChangeSignal(category_id);
}

void LLOutfitListBase::outfitRightClickCallBack(LLUICtrl* ctrl, S32 x, S32 y, const LLUUID& cat_id)
{
    onOutfitRightClick(ctrl, x, y, cat_id);
}

void LLOutfitListBase::ChangeOutfitSelection(LLWearableItemsList* list, const LLUUID& category_id)
{
    onChangeOutfitSelection(list, category_id);
    mSelectedOutfitUUID = category_id;
    signalSelectionOutfitUUID(category_id);
}

bool LLOutfitListBase::postBuild()
{
    // <FS:Ansariel> Show avatar complexity in appearance floater
    mAvatarComplexityLabel = getChild<LLTextBox>("avatar_complexity_label");
    return true;
}

void LLOutfitListBase::collapseAllFolders()
{
    onCollapseAllFolders();
}

void LLOutfitListBase::expandAllFolders()
{
    onExpandAllFolders();
}

void LLOutfitListBase::updateMenuItemsVisibility()
{
    mGearMenu->updateItemsVisibility();
}

LLToggleableMenu* LLOutfitListBase::getGearMenu()
{
    if (!mGearMenu)
    {
        mGearMenu = createGearMenu();
    }
    return mGearMenu->getMenu();
};

void LLOutfitListBase::deselectOutfit(const LLUUID& category_id)
{
    // Reset selection if the outfit is selected.
    if (category_id == mSelectedOutfitUUID)
    {
        mSelectedOutfitUUID = LLUUID::null;
        signalSelectionOutfitUUID(mSelectedOutfitUUID);
    }
}

// <FS:Ansariel> Show avatar complexity in appearance floater
void LLOutfitListBase::updateAvatarComplexity(U32 complexity)
{
    std::string complexity_string;
    LLLocale locale("");
    LLResMgr::getInstance()->getIntegerString(complexity_string, complexity);

    mAvatarComplexityLabel->setTextArg("[WEIGHT]", complexity_string);
}
// </FS:Ansariel>

LLContextMenu* LLOutfitContextMenu::createMenu()
{
    LLUICtrl::CommitCallbackRegistry::ScopedRegistrar registrar;
    LLUICtrl::EnableCallbackRegistry::ScopedRegistrar enable_registrar;
    LLUUID selected_id = mUUIDs.front();

    registrar.add("Outfit.WearReplace",
        boost::bind(&LLAppearanceMgr::replaceCurrentOutfit, &LLAppearanceMgr::instance(), selected_id));
    registrar.add("Outfit.WearAdd",
        boost::bind(&LLAppearanceMgr::addCategoryToCurrentOutfit, &LLAppearanceMgr::instance(), selected_id));
    registrar.add("Outfit.TakeOff",
        boost::bind(&LLAppearanceMgr::takeOffOutfit, &LLAppearanceMgr::instance(), selected_id));
    registrar.add("Outfit.Edit", boost::bind(editOutfit));
    registrar.add("Outfit.Rename", boost::bind(renameOutfit, selected_id));
    registrar.add("Outfit.Delete", boost::bind(&LLOutfitListBase::removeSelected, mOutfitList));
    registrar.add("Outfit.Thumbnail", boost::bind(&LLOutfitContextMenu::onThumbnail, this, selected_id));
    registrar.add("Outfit.Favorite", boost::bind(&LLOutfitContextMenu::onFavorite, this, selected_id));
    registrar.add("Outfit.Save", boost::bind(&LLOutfitContextMenu::onSave, this, selected_id));

    enable_registrar.add("Outfit.OnEnable", boost::bind(&LLOutfitContextMenu::onEnable, this, _2));
    enable_registrar.add("Outfit.OnVisible", boost::bind(&LLOutfitContextMenu::onVisible, this, _2));

    return createFromFile("menu_outfit_tab.xml");

}

bool LLOutfitContextMenu::onEnable(LLSD::String param)
{
    LLUUID outfit_cat_id = mUUIDs.back();

    if ("rename" == param)
    {
        return get_is_category_renameable(&gInventory, outfit_cat_id);
    }
    else if ("wear_replace" == param)
    {
        return LLAppearanceMgr::instance().getCanReplaceCOF(outfit_cat_id);
    }
    else if ("wear_add" == param)
    {
        return LLAppearanceMgr::getCanAddToCOF(outfit_cat_id);
    }
    else if ("take_off" == param)
    {
        return LLAppearanceMgr::getCanRemoveFromCOF(outfit_cat_id);
    }

    return true;
}

bool LLOutfitContextMenu::onVisible(LLSD::String param)
{
    LLUUID outfit_cat_id = mUUIDs.back();

    if ("edit" == param)
    {
        bool is_worn = LLAppearanceMgr::instance().getBaseOutfitUUID() == outfit_cat_id;
        return is_worn;
    }
    else if ("wear_replace" == param)
    {
        return true;
    }
    else if ("delete" == param)
    {
        return LLAppearanceMgr::instance().getCanRemoveOutfit(outfit_cat_id);
    }
    else if ("favorites_add" == param)
    {
        LLViewerInventoryCategory* cat = gInventory.getCategory(outfit_cat_id);
        return cat && !cat->getIsFavorite();
    }
    else if ("favorites_remove" == param)
    {
        LLViewerInventoryCategory* cat = gInventory.getCategory(outfit_cat_id);
        return cat && cat->getIsFavorite();
    }

    return true;
}

//static
void LLOutfitContextMenu::editOutfit()
{
    LLFloaterSidePanelContainer::showPanel("appearance", LLSD().with("type", "edit_outfit"));
}

void LLOutfitContextMenu::renameOutfit(const LLUUID& outfit_cat_id)
{
    LLAppearanceMgr::instance().renameOutfit(outfit_cat_id);
}

void LLOutfitContextMenu::onThumbnail(const LLUUID &outfit_cat_id)
{
    if (outfit_cat_id.notNull())
    {
        LLSD data(outfit_cat_id);
        LLFloaterReg::showInstance("change_item_thumbnail", data);
    }
}

void LLOutfitContextMenu::onFavorite(const LLUUID& outfit_cat_id)
{
    if (outfit_cat_id.notNull())
    {
        toggle_favorite(outfit_cat_id);
    }
}

void LLOutfitContextMenu::onSave(const LLUUID &outfit_cat_id)
{
    if (outfit_cat_id.notNull())
    {
        LLNotificationsUtil::add("ConfirmOverwriteOutfit", LLSD(), LLSD(),
            [outfit_cat_id](const LLSD &notif, const LLSD &resp)
        {
            S32 opt = LLNotificationsUtil::getSelectedOption(notif, resp);
            if (opt == 0)
            {
                LLAppearanceMgr::getInstance()->onOutfitFolderCreated(outfit_cat_id, true);
            }
        });
    }
}

LLOutfitListGearMenuBase::LLOutfitListGearMenuBase(LLOutfitListBase* olist)
    :   mOutfitList(olist),
        mMenu(NULL)
{
    llassert_always(mOutfitList);

    LLUICtrl::CommitCallbackRegistry::ScopedRegistrar registrar;
    LLUICtrl::EnableCallbackRegistry::ScopedRegistrar enable_registrar;

    registrar.add("Gear.Wear", boost::bind(&LLOutfitListGearMenuBase::onWear, this));
    registrar.add("Gear.TakeOff", boost::bind(&LLOutfitListGearMenuBase::onTakeOff, this));
    registrar.add("Gear.Rename", boost::bind(&LLOutfitListGearMenuBase::onRename, this));
    registrar.add("Gear.Delete", boost::bind(&LLOutfitListBase::removeSelected, mOutfitList));
    registrar.add("Gear.Create", boost::bind(&LLOutfitListGearMenuBase::onCreate, this, _2));

    registrar.add("Gear.WearAdd", boost::bind(&LLOutfitListGearMenuBase::onAdd, this));
    registrar.add("Gear.Save", boost::bind(&LLOutfitListGearMenuBase::onSave, this));

    registrar.add("Gear.Thumbnail", boost::bind(&LLOutfitListGearMenuBase::onThumbnail, this));
    registrar.add("Gear.Favorite", boost::bind(&LLOutfitListGearMenuBase::onFavorite, this));
    registrar.add("Gear.SortByImage", boost::bind(&LLOutfitListGearMenuBase::onChangeSortOrder, this));

    enable_registrar.add("Gear.OnEnable", boost::bind(&LLOutfitListGearMenuBase::onEnable, this, _2));
    enable_registrar.add("Gear.OnVisible", boost::bind(&LLOutfitListGearMenuBase::onVisible, this, _2));

    mMenu = LLUICtrlFactory::getInstance()->createFromFile<LLToggleableMenu>(
        "menu_outfit_gear.xml", gMenuHolder, LLViewerMenuHolderGL::child_registry_t::instance());
    llassert(mMenu);
}

LLOutfitListGearMenuBase::~LLOutfitListGearMenuBase()
{}

void LLOutfitListGearMenuBase::updateItemsVisibility()
{
    onUpdateItemsVisibility();
}

void LLOutfitListGearMenuBase::onUpdateItemsVisibility()
{
    if (!mMenu) return;

    bool have_selection = getSelectedOutfitID().notNull();
    mMenu->setItemVisible("wear_separator", have_selection);
    mMenu->arrangeAndClear(); // update menu height
}

LLToggleableMenu* LLOutfitListGearMenuBase::getMenu()
{
    return mMenu;
}
const LLUUID& LLOutfitListGearMenuBase::getSelectedOutfitID()
{
    return mOutfitList->getSelectedOutfitUUID();
}

LLViewerInventoryCategory* LLOutfitListGearMenuBase::getSelectedOutfit()
{
    const LLUUID& selected_outfit_id = getSelectedOutfitID();
    if (selected_outfit_id.isNull())
    {
        return NULL;
    }

    LLViewerInventoryCategory* cat = gInventory.getCategory(selected_outfit_id);
    return cat;
}

void LLOutfitListGearMenuBase::onWear()
{
    LLViewerInventoryCategory* selected_outfit = getSelectedOutfit();
    if (selected_outfit)
    {
        LLAppearanceMgr::instance().wearInventoryCategory(
            selected_outfit, /*copy=*/ false, /*append=*/ false);
    }
}

void LLOutfitListGearMenuBase::onAdd()
{
    const LLUUID& selected_id = getSelectedOutfitID();

    if (selected_id.notNull())
    {
        LLAppearanceMgr::getInstance()->addCategoryToCurrentOutfit(selected_id);
    }
}

void LLOutfitListGearMenuBase::onSave()
{
    const LLUUID &selected_id = getSelectedOutfitID();
    LLNotificationsUtil::add("ConfirmOverwriteOutfit", LLSD(), LLSD(),
        [selected_id](const LLSD &notif, const LLSD &resp)
    {
        S32 opt = LLNotificationsUtil::getSelectedOption(notif, resp);
        if (opt == 0)
        {
            LLAppearanceMgr::getInstance()->onOutfitFolderCreated(selected_id, true);
        }
    });
}

void LLOutfitListGearMenuBase::onTakeOff()
{
    // Take off selected outfit.
    const LLUUID& selected_outfit_id = getSelectedOutfitID();
    if (selected_outfit_id.notNull())
    {
        LLAppearanceMgr::instance().takeOffOutfit(selected_outfit_id);
    }
}

void LLOutfitListGearMenuBase::onRename()
{
    const LLUUID& selected_outfit_id = getSelectedOutfitID();
    if (selected_outfit_id.notNull())
    {
        LLAppearanceMgr::instance().renameOutfit(selected_outfit_id);
    }
}

void LLOutfitListGearMenuBase::onCreate(const LLSD& data)
{
    LLWearableType::EType type = LLWearableType::getInstance()->typeNameToType(data.asString());
    if (type == LLWearableType::WT_NONE)
    {
        LL_WARNS() << "Invalid wearable type" << LL_ENDL;
        return;
    }

    LLAgentWearables::createWearable(type, true);
}

bool LLOutfitListGearMenuBase::onEnable(LLSD::String param)
{
    // Handle the "Wear - Replace Current Outfit" menu option specially
    // because LLOutfitList::isActionEnabled() checks whether it's allowed
    // to wear selected outfit OR selected items, while we're only
    // interested in the outfit (STORM-183).
    if ("wear" == param)
    {
        return LLAppearanceMgr::instance().getCanReplaceCOF(mOutfitList->getSelectedOutfitUUID());
    }

    return mOutfitList->isActionEnabled(param);
}

bool LLOutfitListGearMenuBase::onVisible(LLSD::String param)
{
    const LLUUID& selected_outfit_id = getSelectedOutfitID();
    if (selected_outfit_id.isNull()) // no selection or invalid outfit selected
    {
        return false;
    }
    else if ("favorites_add" == param)
    {
        LLViewerInventoryCategory* cat = gInventory.getCategory(selected_outfit_id);
        return cat && !cat->getIsFavorite();
    }
    else if ("favorites_remove" == param)
    {
        LLViewerInventoryCategory* cat = gInventory.getCategory(selected_outfit_id);
        return cat && cat->getIsFavorite();
    }

    return true;
}

void LLOutfitListGearMenuBase::onThumbnail()
{
    const LLUUID& selected_outfit_id = getSelectedOutfitID();
    LLSD data(selected_outfit_id);
    LLFloaterReg::showInstance("change_item_thumbnail", data);
}

void LLOutfitListGearMenuBase::onFavorite()
{
    const LLUUID& selected_outfit_id = getSelectedOutfitID();
    toggle_favorite(selected_outfit_id);
}

void LLOutfitListGearMenuBase::onChangeSortOrder()
{

}

LLOutfitListGearMenu::LLOutfitListGearMenu(LLOutfitListBase* olist)
    : LLOutfitListGearMenuBase(olist)
{}

LLOutfitListGearMenu::~LLOutfitListGearMenu()
{}

void LLOutfitListGearMenu::onUpdateItemsVisibility()
{
    if (!mMenu) return;
    mMenu->setItemVisible("thumbnail", getSelectedOutfitID().notNull());
    mMenu->setItemVisible("favorite", getSelectedOutfitID().notNull());
    mMenu->setItemVisible("sepatator3", false);
    mMenu->setItemVisible("sort_folders_by_name", false);
    LLOutfitListGearMenuBase::onUpdateItemsVisibility();
}

//////////////////// LLOutfitListSortMenu ////////////////////

LLOutfitListSortMenu::LLOutfitListSortMenu(LLOutfitListBase* parent_panel)
    : mPanelHandle(parent_panel->getHandle())
{
    LLUICtrl::CommitCallbackRegistry::ScopedRegistrar registrar;
    LLUICtrl::EnableCallbackRegistry::ScopedRegistrar enable_registrar;

    registrar.add("Sort.Collapse", boost::bind(&LLOutfitListBase::onCollapseAllFolders, parent_panel));
    registrar.add("Sort.Expand", boost::bind(&LLOutfitListBase::onExpandAllFolders, parent_panel));
    registrar.add("Sort.OnSort", boost::bind(&LLOutfitListBase::onChangeSortOrder, parent_panel, _2));
    enable_registrar.add("Sort.OnEnable", boost::bind(&LLOutfitListSortMenu::onEnable, this, _2));

    mMenu = LLUICtrlFactory::getInstance()->createFromFile<LLToggleableMenu>(
        "menu_outfit_list_sort.xml", gMenuHolder, LLViewerMenuHolderGL::child_registry_t::instance());
    llassert(mMenu);
}


LLToggleableMenu* LLOutfitListSortMenu::getMenu()
{
    return mMenu;
}

void LLOutfitListSortMenu::updateItemsVisibility()
{
    onUpdateItemsVisibility();
}

void LLOutfitListSortMenu::onUpdateItemsVisibility()
{
    if (!mMenu) return;
    mMenu->setItemVisible("expand", true);
    mMenu->setItemVisible("collapse", true);
    mMenu->setItemVisible("sort_favorites_to_top", true);
    mMenu->setItemVisible("show_entire_outfit_in_search", true);
}

bool LLOutfitListSortMenu::onEnable(LLSD::String param)
{
    if ("favorites_to_top" == param)
    {
        static LLCachedControl<S32> sort_order(gSavedSettings, "OutfitListSortOrder", 0);
        return sort_order == 1;
    }
    else if ("show_entire_outfit" == param)
    {
        static LLCachedControl<bool> filter_mode(gSavedSettings, "OutfitListFilterFullList", 0);
        return filter_mode;
    }

    return false;
}


//////////////////// LLOutfitAccordionCtrlTab ////////////////////

LLUIImage* LLOutfitAccordionCtrlTab::sFavoriteIcon;
LLUIColor LLOutfitAccordionCtrlTab::sFgColor;

void LLOutfitAccordionCtrlTab::draw()
{
    LLAccordionCtrlTab::draw();
    drawFavoriteIcon();
}

bool LLOutfitAccordionCtrlTab::handleToolTip(S32 x, S32 y, MASK mask)
{
    // <FS:Ansariel> Make thumbnail tooltip work properly
    //if (y >= getLocalRect().getHeight() - getHeaderHeight())
    static LLCachedControl<bool> showInventoryThumbnailTooltips(gSavedSettings, "FSShowInventoryThumbnailTooltips");
    if (showInventoryThumbnailTooltips && y >= getLocalRect().getHeight() - getHeaderHeight())
    {
        LLSD params;
        params["inv_type"] = LLInventoryType::IT_CATEGORY;
        LLViewerInventoryCategory* cat = gInventory.getCategory(mFolderID);
        // <FS:TJ> Make thumbnail tooltip work properly
        if (!cat || cat->getThumbnailUUID().isNull())
        {
            return LLAccordionCtrlTab::handleToolTip(x, y, mask);
        }
        // </FS:TJ>

        if (cat)
        {
            params["thumbnail_id"] = cat->getThumbnailUUID();
        }
        // else consider returning
        params["item_id"] = mFolderID;

        // <FS:Ansariel> Make thumbnail tooltip work properly
        static LLCachedControl<F32> inventoryThumbnailTooltipsDelay(gSavedSettings, "FSInventoryThumbnailTooltipsDelay");
        static LLCachedControl<F32> tooltip_fast_delay(gSavedSettings, "ToolTipFastDelay");
        F32 tooltipDelay = LLToolTipMgr::instance().toolTipVisible() ? tooltip_fast_delay() : inventoryThumbnailTooltipsDelay();
        // </FS:Ansariel>

        LLToolTipMgr::instance().show(LLToolTip::Params()
                                    // <FS:Ansariel> Make thumbnail tooltip work properly
                                    //.message(getToolTip())
                                    .message(gInventory.getCategory(mFolderID)->getName())
                                    .sticky_rect(calcScreenRect())
                                    // <FS:Ansariel> Make thumbnail tooltip work properly
                                    //.delay_time(LLView::getTooltipTimeout())
                                    .delay_time(tooltipDelay)
                                    .create_callback(boost::bind(&LLInspectTextureUtil::createInventoryToolTip, _1))
                                    .create_params(params));
        return true;
    }

    return LLAccordionCtrlTab::handleToolTip(x, y, mask);
}

void LLOutfitAccordionCtrlTab::setFavorite(bool is_favorite)
{
    mIsFavorite = is_favorite;
    updateTitleColor();
}

void LLOutfitAccordionCtrlTab::setOutfitSelected(bool val)
{
    mIsSelected = val;
    setTitleFontStyle(mIsSelected ? "BOLD" : "NORMAL");
    updateTitleColor();
    }

void LLOutfitAccordionCtrlTab::updateTitleColor()
    {
        static LLUICachedControl<bool> highlight_color("InventoryFavoritesColorText", true);
        if (mIsFavorite && highlight_color())
        {
            setTitleColor(LLUIColorTable::instance().getColor("InventoryFavoriteColor"));
        }
    else if (mIsSelected)
    {
        setTitleColor(LLUIColorTable::instance().getColor("SelectedOutfitTextColor"));
    }
        else
        {
            setTitleColor(LLUIColorTable::instance().getColor("AccordionHeaderTextColor"));
        }
    }

void LLOutfitAccordionCtrlTab::drawFavoriteIcon()
{
    if (!mIsFavorite)
    {
        return;
    }
    static LLUICachedControl<bool> draw_star("InventoryFavoritesUseStar", true);
    if (!draw_star)
    {
        return;
    }

    const S32 PAD = 2;
    const S32 image_size = 18;

    gl_draw_scaled_image(
        getRect().getWidth() - image_size - PAD, getRect().getHeight() - image_size - PAD,
        image_size, image_size, sFavoriteIcon->getImage(), sFgColor);
}
// EOF
