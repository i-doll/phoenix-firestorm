# Outfit Subfolders Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Make the Outfits floater render the subfolder structure that already exists in My Outfits, instead of flattening it.

**Architecture:** Views place, base untouched. `LLOutfitListBase::refreshList` already enumerates every descendant category recursively and hands each to `updateAddedCategory`; today both views discard the non-outfit ones. Each view keeps them, renders them as folder entries, and uses `cat->getParentUUID()` for placement. The gallery drills down one level at a time with a breadcrumb; the list nests accordion tabs.

**Tech Stack:** C++17, Firestorm viewer UI (`LLPanel`, `LLAccordionCtrl`, `LLAccordionCtrlTab`, `LLIconCtrl`, `LLTextBox`, `LLUICtrlFactory`), XUI XML.

**Spec:** `docs/superpowers/specs/2026-08-27-outfit-subfolders-design.md`

## Global Constraints

- **No unit-test harness reaches this code.** No test target covers `lloutfitslist.*` or `lloutfitgallery.*`. Do NOT write unit tests — there is nowhere for them to run. Verification is careful reading against the real headers plus the live checklist in Task 6.
- **Do NOT build this viewer.** No `cmake`, no `autobuild`, no `make`. The build belongs to the repo owner.
- **Rendering only.** No creating, renaming, moving or deleting folders from this floater; no drag-and-drop. That is the inventory floater's job and is explicitly out of scope.
- **Do not touch `refreshList`, `computeDifference`, or the `mRefreshListState` idle loop.** They are tuned for 1000+ outfit inventories.
- **Do not change `isOutfitFolder()`'s heuristic** (`lloutfitslist.cpp:1060`). Tightening it would silently reclassify residents' legacy outfits.
- **Custom code carries the `ID` prefix / `<ID>` comment markers**, never `FS`.
- Commit straight to the current `bot-*-master*` branch. No feature branches. No push.
- Conventional-commit messages, no attribution trailers.

## File Structure

| File | Responsibility |
|---|---|
| `indra/newview/lloutfitgallery.h/.cpp` (modify) | Folder tiles, `mCurrentFolder` drill-down, folders-first sort, breadcrumb ownership |
| `indra/newview/lloutfitslist.h/.cpp` (modify) | Nested folder tabs, pending-children map, recursive `arrange()` and filtering |
| `indra/newview/skins/default/xui/en/panel_outfit_gallery.xml` (modify) | Breadcrumb strip above the scroll container |

---

### Task 1: Gallery — folder entries and drill-down

**Files:**
- Modify: `indra/newview/lloutfitgallery.h`
- Modify: `indra/newview/lloutfitgallery.cpp`

**Interfaces:**
- Produces (Task 2 consumes by exact name): `LLUUID LLOutfitGallery::mCurrentFolder`, `void LLOutfitGallery::setCurrentFolder(const LLUUID& cat_id)`, `bool LLOutfitGalleryItem::isFolder() const`, `void LLOutfitGalleryItem::setIsFolder(bool)`.

- [ ] **Step 1: Add the folder flag to `LLOutfitGalleryItem`**

In `lloutfitgallery.h`, in the public section of `LLOutfitGalleryItem` beside `isFavorite()`:

```cpp
    // <ID> Folder tiles render in the same grid as outfits
    bool isFolder() const { return mIsFolder; }
    void setIsFolder(bool is_folder);
    // </ID>
```

and in its private members beside `bool mFavorite;`:

```cpp
    bool     mIsFolder = false; // <ID>
```

- [ ] **Step 2: Implement `setIsFolder`**

In `lloutfitgallery.cpp`, after `LLOutfitGalleryItem::setOutfitFavorite`:

```cpp
// <ID> A folder tile reuses the outfit tile, swapping the thumbnail for a folder icon.
void LLOutfitGalleryItem::setIsFolder(bool is_folder)
{
    mIsFolder = is_folder;
    if (mIsFolder)
    {
        mDefaultImage = false;
        if (mPreviewIcon)
        {
            mPreviewIcon->setValue("Inv_SysOpen");
            mPreviewIcon->setVisible(true);
        }
    }
}
// </ID>
```

- [ ] **Step 3: Add drill-down state to `LLOutfitGallery`**

In `lloutfitgallery.h`, public section of `LLOutfitGallery`:

```cpp
    // <ID> Subfolder drill-down
    void setCurrentFolder(const LLUUID& cat_id);
    const LLUUID& getCurrentFolder() const { return mCurrentFolder; }
    // </ID>
```

and in its protected/private data beside `mOutfitMap`:

```cpp
    LLUUID mCurrentFolder; // <ID> folder currently being browsed; null means My Outfits root
```

- [ ] **Step 4: Stop discarding folders in `updateAddedCategory`**

In `lloutfitgallery.cpp`, replace the early return at `:825`:

```cpp
    if (!isOutfitFolder(cat))
    {
        // Assume a subfolder that contains or will contain outfits, track it
        const LLUUID outfits = gInventory.findCategoryUUIDForType(LLFolderType::FT_MY_OUTFITS);
        mCategoriesObserver->addCategory(cat_id, [this, outfits]()
        {
            observerCallback(outfits);
        });
        return;
    }
```

with:

```cpp
    // <ID> Render subfolders as folder tiles instead of discarding them. The observer
    // registration is kept — it is what keeps nested contents live.
    bool is_folder = !isOutfitFolder(cat);
    if (is_folder)
    {
        const LLUUID outfits = gInventory.findCategoryUUIDForType(LLFolderType::FT_MY_OUTFITS);
        mCategoriesObserver->addCategory(cat_id, [this, outfits]()
        {
            observerCallback(outfits);
        });
        if (cat_id == outfits)
        {
            // The root itself is not a tile inside itself.
            return;
        }
    }
    // </ID>
```

Then, immediately after the existing `buildGalleryItem` call, mark the item:

```cpp
    LLOutfitGalleryItem* item = buildGalleryItem(cat->getName(), cat_id, cat->getIsFavorite());
    item->setIsFolder(is_folder); // <ID>
```

- [ ] **Step 5: Filter the layout to the current folder**

In `reArrangeRows` (`:458`), replace the loop body's hidden calculation. The existing loop is:

```cpp
    for (std::vector<LLOutfitGalleryItem*>::const_iterator it = buf_items.begin(); it != buf_items.end(); ++it)
    {
        std::string outfit_name = (*it)->getItemName();
        LLStringUtil::toUpper(outfit_name);

        bool hidden = (std::string::npos == outfit_name.find(cur_filter));
        (*it)->setHidden(hidden);

        addToGallery(*it);
    }
```

Replace with:

```cpp
    // <ID> With no filter, show only the direct children of the folder being browsed.
    // With a filter, flatten: search the whole tree and drop folder tiles, because
    // drill-down would otherwise hide every match that lives somewhere else.
    const bool filtering = !cur_filter.empty();
    const LLUUID browse_folder = mCurrentFolder.isNull()
        ? gInventory.findCategoryUUIDForType(LLFolderType::FT_MY_OUTFITS)
        : mCurrentFolder;
    // </ID>

    for (std::vector<LLOutfitGalleryItem*>::const_iterator it = buf_items.begin(); it != buf_items.end(); ++it)
    {
        std::string outfit_name = (*it)->getItemName();
        LLStringUtil::toUpper(outfit_name);

        bool hidden = (std::string::npos == outfit_name.find(cur_filter));

        // <ID>
        if (filtering)
        {
            if ((*it)->isFolder())
            {
                hidden = true;
            }
        }
        else
        {
            LLViewerInventoryCategory* item_cat = gInventory.getCategory((*it)->getUUID());
            if (!item_cat || item_cat->getParentUUID() != browse_folder)
            {
                hidden = true;
            }
        }
        // </ID>

        (*it)->setHidden(hidden);

        addToGallery(*it);
    }
```

- [ ] **Step 6: Sort folders first**

In `compareGalleryItem` (`:428`), insert ahead of the `switch`:

```cpp
bool compareGalleryItem(LLOutfitGalleryItem* item1, LLOutfitGalleryItem* item2)
{
    // <ID> Folders lead their level under every sort order.
    if (item1->isFolder() != item2->isFolder())
    {
        return item1->isFolder();
    }
    // </ID>
    static LLCachedControl<S32> sort_by_name(gSavedSettings, "OutfitGallerySortOrder", 0);
```

- [ ] **Step 7: Implement `setCurrentFolder`**

In `lloutfitgallery.cpp`:

```cpp
// <ID> Descend into (or back out of) a subfolder. Selecting a folder is not selecting an
// outfit, so the outfit selection is cleared on navigation.
void LLOutfitGallery::setCurrentFolder(const LLUUID& cat_id)
{
    const LLUUID root = gInventory.findCategoryUUIDForType(LLFolderType::FT_MY_OUTFITS);
    LLUUID target = cat_id;

    // A folder deleted or moved out from under us falls back to the root.
    if (target.notNull() && target != root && !gInventory.getCategory(target))
    {
        target.setNull();
    }
    if (target == root)
    {
        target.setNull();
    }

    if (target == mCurrentFolder)
    {
        return;
    }
    mCurrentFolder = target;

    if (!getSelectedOutfitUUID().isNull())
    {
        ChangeOutfitSelection(NULL, LLUUID::null);
    }
    reArrangeRows();
    updateBreadcrumb();
}
// </ID>
```

`updateBreadcrumb()` is added in Task 2. For this task, declare it in the header and give it an empty body so Task 1 compiles standalone:

```cpp
void LLOutfitGallery::updateBreadcrumb() {} // <ID> filled in by the breadcrumb task
```

with, in `lloutfitgallery.h`, protected:

```cpp
    void updateBreadcrumb(); // <ID>
```

- [ ] **Step 8: Descend on double click**

In `lloutfitgallery.cpp`, change `LLOutfitGalleryItem::handleDoubleClick`:

```cpp
bool LLOutfitGalleryItem::handleDoubleClick(S32 x, S32 y, MASK mask)
{
    // <ID> Double-clicking a folder descends into it rather than jumping to the list tab.
    if (mIsFolder)
    {
        if (mGallery)
        {
            mGallery->setCurrentFolder(mUUID);
            return true;
        }
        return false;
    }
    // </ID>
    return openOutfitsContent() || LLPanel::handleDoubleClick(x, y, mask);
}
```

- [ ] **Step 9: Compile checklist (read, do not build)**

Confirm by reading:
- `LLOutfitGalleryItem` has `mPreviewIcon` as an `LLIconCtrl*` and `mUUID`, `mDefaultImage`, `mFavorite` as members (`lloutfitgallery.h:265-280`).
- `mGallery` is an `LLOutfitGallery*` member of the item, set by `setGallery` in `buildGalleryItem`.
- `reArrangeRows` has the exact loop quoted in Step 5 — if it has drifted, adapt rather than paste.
- `ChangeOutfitSelection(LLWearableItemsList*, const LLUUID&)` is public on `LLOutfitListBase` (`lloutfitslist.h:92`) and accepts a NULL list — check its body before relying on that; if it dereferences the list, clear `mSelectedOutfitUUID` via `deselectOutfit(getSelectedOutfitUUID())` instead.
- `"Inv_SysOpen"` exists as a UI image name in `textures.xml`; if not, pick the folder icon name that does.
- `LLFolderType::FT_MY_OUTFITS` and `gInventory.findCategoryUUIDForType` are already used in this file.

- [ ] **Step 10: Commit**

```bash
git add indra/newview/lloutfitgallery.cpp indra/newview/lloutfitgallery.h
git commit -m "feat: render outfit subfolders as tiles in the gallery"
```

---

### Task 2: Gallery — breadcrumb

**Files:**
- Modify: `indra/newview/lloutfitgallery.h`
- Modify: `indra/newview/lloutfitgallery.cpp`
- Modify: `indra/newview/skins/default/xui/en/panel_outfit_gallery.xml`

**Interfaces:**
- Consumes: `mCurrentFolder`, `setCurrentFolder`, `updateBreadcrumb` (Task 1).

- [ ] **Step 1: Add the breadcrumb panel to the XML**

In `panel_outfit_gallery.xml`, immediately before the `scroll_container` named `gallery_scroll_panel`:

```xml
  <!-- <ID> Subfolder breadcrumb; hidden at the root and while filtering -->
  <panel
   background_visible="false"
   follows="left|top|right"
   height="18"
   layout="topleft"
   left="0"
   name="outfit_breadcrumb"
   top="0"
   visible="false"
   width="312" />
  <!-- </ID> -->
```

Then increase the `scroll_container`'s `top` by 18 and reduce its `height` by 18 so the strip does not overlap the grid. Read the existing values first — do not guess them.

- [ ] **Step 2: Hold the panel**

In `lloutfitgallery.h`, protected data:

```cpp
    LLPanel* mBreadcrumbPanel = nullptr; // <ID>
```

In `postBuild()` in `lloutfitgallery.cpp`, alongside the other `getChild` calls:

```cpp
    mBreadcrumbPanel = getChild<LLPanel>("outfit_breadcrumb"); // <ID>
```

- [ ] **Step 3: Build the breadcrumb**

Replace the empty `updateBreadcrumb()` from Task 1 Step 7 with:

```cpp
// <ID> Rebuild the "Outfits > Formal > Winter" strip from the parent chain of the folder
// being browsed. Hidden at the root and while a filter is active, since filtering
// flattens the hierarchy and the trail would be a lie.
void LLOutfitGallery::updateBreadcrumb()
{
    if (!mBreadcrumbPanel)
    {
        return;
    }

    mBreadcrumbPanel->deleteAllChildren();

    if (mCurrentFolder.isNull() || !getFilterSubString().empty())
    {
        mBreadcrumbPanel->setVisible(false);
        return;
    }

    const LLUUID root = gInventory.findCategoryUUIDForType(LLFolderType::FT_MY_OUTFITS);

    // Walk up to the root, then render left-to-right.
    std::vector<std::pair<LLUUID, std::string> > trail;
    LLUUID walk = mCurrentFolder;
    while (walk.notNull() && walk != root)
    {
        LLViewerInventoryCategory* cat = gInventory.getCategory(walk);
        if (!cat)
        {
            break;
        }
        trail.push_back(std::make_pair(walk, cat->getName()));
        walk = cat->getParentUUID();
    }
    trail.push_back(std::make_pair(root, LLTrans::getString("OutfitsBreadcrumbRoot")));
    std::reverse(trail.begin(), trail.end());

    S32 left = 2;
    for (size_t i = 0; i < trail.size(); ++i)
    {
        if (i > 0)
        {
            LLTextBox::Params sep;
            sep.name = "sep";
            sep.initial_value(std::string(" > "));
            sep.rect(LLRect(left, 16, left + 14, 0));
            sep.follows.flags(FOLLOWS_LEFT | FOLLOWS_TOP);
            LLTextBox* sep_box = LLUICtrlFactory::create<LLTextBox>(sep);
            mBreadcrumbPanel->addChild(sep_box);
            left += 14;
        }

        const LLUUID seg_id = trail[i].first;
        const std::string& seg_name = trail[i].second;
        const bool is_last = (i + 1 == trail.size());

        LLTextBox::Params p;
        p.name = "crumb";
        p.initial_value(seg_name);
        S32 width = 8 + (S32)seg_name.size() * 6;
        p.rect(LLRect(left, 16, left + width, 0));
        p.follows.flags(FOLLOWS_LEFT | FOLLOWS_TOP);
        LLTextBox* box = LLUICtrlFactory::create<LLTextBox>(p);
        if (!is_last)
        {
            // Only ancestors navigate; the trailing segment is where you already are.
            box->setClickedCallback([this, seg_id](LLUICtrl*, const LLSD&)
            {
                setCurrentFolder(seg_id);
            });
        }
        mBreadcrumbPanel->addChild(box);
        left += width;
    }

    mBreadcrumbPanel->setVisible(true);
}
// </ID>
```

- [ ] **Step 4: Add the root label string**

In `panel_outfit_gallery.xml`, beside the existing `<string>` entries near the top:

```xml
  <string name="OutfitsBreadcrumbRoot">Outfits</string>
```

and change the `LLTrans::getString("OutfitsBreadcrumbRoot")` call in Step 3 to `getString("OutfitsBreadcrumbRoot")`, which reads the panel-local string. Drop the `lltrans.h` dependency if it is not already included.

- [ ] **Step 5: Refresh the breadcrumb when the filter changes**

`LLOutfitGallery::onFilterSubStringChanged` is at `lloutfitgallery.cpp:782`. Add
`updateBreadcrumb();` as its last statement, so the strip disappears when a filter is typed
and comes back when it is cleared:

```cpp
    updateBreadcrumb(); // <ID> hidden while filtering, restored when the filter clears
```

- [ ] **Step 6: Compile checklist (read, do not build)**

Confirm by reading:
- `LLPanel::deleteAllChildren()` exists and is safe to call on a panel built from XUI with no children.
- `LLTextBox::Params` accepts `initial_value` and `rect` the way other dynamically-created text boxes in this codebase do — find one and match it exactly rather than trusting the shape above.
- `setClickedCallback` on `LLTextBox` takes a `commit_callback_t`; check the signature and adapt the lambda if it differs.
- `getString(...)` on `LLPanel` reads `<string>` children of that panel's XML.

- [ ] **Step 7: Commit**

```bash
git add indra/newview/lloutfitgallery.cpp indra/newview/lloutfitgallery.h \
        indra/newview/skins/default/xui/en/panel_outfit_gallery.xml
git commit -m "feat: breadcrumb navigation for outfit gallery subfolders"
```

---

### Task 3: List — nested folder tabs

**Files:**
- Modify: `indra/newview/lloutfitslist.h`
- Modify: `indra/newview/lloutfitslist.cpp`

**Interfaces:**
- Produces (Task 4 consumes): `std::map<LLUUID, LLAccordionCtrl*> LLOutfitsList::mFolderAccordions`, `LLAccordionCtrl* LLOutfitsList::getParentAccordion(const LLUUID& cat_id)`.

- [ ] **Step 1: Add the folder bookkeeping**

In `lloutfitslist.h`, in `LLOutfitsList`'s private data beside `mAccordion`:

```cpp
    // <ID> Subfolder support: the inner accordion inside each folder tab, and outfits that
    // arrived before their parent folder's tab existed (the base's diff gives no ordering
    // guarantee between a folder and its children).
    std::map<LLUUID, LLAccordionCtrl*> mFolderAccordions;
    std::multimap<LLUUID, LLUUID>      mPendingChildren; // parent cat id -> outfit cat id
    LLAccordionCtrl* getParentAccordion(const LLUUID& cat_id);
    void addFolderTab(LLViewerInventoryCategory* cat);
    void flushPendingChildren(const LLUUID& parent_id);
    // </ID>
```

- [ ] **Step 2: Resolve a category's target accordion**

In `lloutfitslist.cpp`:

```cpp
// <ID> Where does this outfit's tab belong? Its parent folder's inner accordion if that
// folder has a tab, otherwise the top-level accordion.
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
// </ID>
```

Returning NULL means "parent folder exists but has no tab yet" — the caller defers.

- [ ] **Step 3: Create folder tabs**

In `lloutfitslist.cpp`:

```cpp
// <ID> A folder tab looks like an outfit tab but holds a nested accordion instead of a
// wearable list.
void LLOutfitsList::addFolderTab(LLViewerInventoryCategory* cat)
{
    if (!cat || mFolderAccordions.count(cat->getUUID()))
    {
        return;
    }
    const LLUUID cat_id = cat->getUUID();

    outfit_accordion_tab_params tab_params(get_accordion_tab_params());
    tab_params.cat_id = cat_id;
    LLOutfitAccordionCtrlTab* tab = LLUICtrlFactory::create<LLOutfitAccordionCtrlTab>(tab_params);
    if (!tab)
    {
        return;
    }

    LLAccordionCtrl::Params inner_params;
    inner_params.name = "folder_accordion";
    inner_params.rect = tab->getLocalRect();
    inner_params.fit_parent = false;
    LLAccordionCtrl* inner = LLUICtrlFactory::create<LLAccordionCtrl>(inner_params);
    tab->addChild(inner);

    tab->setName(cat->getName());
    tab->setTitle(cat->getName());
    tab->setFavorite(cat->getIsFavorite());
    tab->setDisplayChildren(false);

    LLAccordionCtrl* parent = getParentAccordion(cat_id);
    if (!parent)
    {
        // Parent folder's tab not built yet; it will flush us when it is.
        tab->die();
        mPendingChildren.insert(std::make_pair(gInventory.getCategory(cat_id)->getParentUUID(), cat_id));
        return;
    }
    parent->addCollapsibleCtrl(tab);
    mFolderAccordions[cat_id] = inner;

    flushPendingChildren(cat_id);
}
// </ID>
```

- [ ] **Step 4: Flush deferred children**

```cpp
// <ID> Re-run updateAddedCategory for anything that was waiting on this folder's tab.
void LLOutfitsList::flushPendingChildren(const LLUUID& parent_id)
{
    std::vector<LLUUID> ready;
    std::pair<std::multimap<LLUUID, LLUUID>::iterator, std::multimap<LLUUID, LLUUID>::iterator>
        range = mPendingChildren.equal_range(parent_id);
    for (std::multimap<LLUUID, LLUUID>::iterator it = range.first; it != range.second; ++it)
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
```

- [ ] **Step 5: Route `updateAddedCategory`**

In `LLOutfitsList::updateAddedCategory` (`lloutfitslist.cpp:209`), replace the early return:

```cpp
    if (!isOutfitFolder(cat))
    {
        // Assume a subfolder that contains or will contain outfits, track it
        const LLUUID outfits = gInventory.findCategoryUUIDForType(LLFolderType::FT_MY_OUTFITS);
        mCategoriesObserver->addCategory(cat_id, [this, outfits]()
        {
            observerCallback(outfits);
        });
        return;
    }
```

with:

```cpp
    // <ID> Subfolders become nested tabs instead of being discarded. The observer
    // registration is kept — it keeps nested contents live.
    if (!isOutfitFolder(cat))
    {
        const LLUUID outfits = gInventory.findCategoryUUIDForType(LLFolderType::FT_MY_OUTFITS);
        mCategoriesObserver->addCategory(cat_id, [this, outfits]()
        {
            observerCallback(outfits);
        });
        if (cat_id != outfits)
        {
            addFolderTab(cat);
        }
        return;
    }
    // </ID>
```

Then, further down the same function, replace the single `mAccordion->addCollapsibleCtrl(tab);` (`:237`) with:

```cpp
    // <ID> Place the outfit under its folder when it has one.
    LLAccordionCtrl* target = getParentAccordion(cat_id);
    if (!target)
    {
        mPendingChildren.insert(std::make_pair(cat->getParentUUID(), cat_id));
        tab->die();
        return;
    }
    target->addCollapsibleCtrl(tab);
    // </ID>
```

- [ ] **Step 6: Clean up on removal**

In `LLOutfitsList::updateRemovedCategory`, after the existing tab teardown, add:

```cpp
    // <ID>
    mFolderAccordions.erase(cat_id);
    mPendingChildren.erase(cat_id);
    // </ID>
```

- [ ] **Step 7: Arrange nested accordions too**

Replace `LLOutfitsList::arrange()`:

```cpp
void LLOutfitsList::arrange()
{
    // <ID> Inner accordions must be arranged before the outer one, or the outer one sizes
    // its folder tabs from stale inner heights.
    for (std::map<LLUUID, LLAccordionCtrl*>::iterator it = mFolderAccordions.begin();
         it != mFolderAccordions.end(); ++it)
    {
        if (it->second)
        {
            it->second->arrange();
        }
    }
    // </ID>
    if (mAccordion)
    {
        mAccordion->arrange();
    }
}
```

- [ ] **Step 8: Compile checklist (read, do not build)**

Confirm by reading:
- `LLAccordionCtrl::Params` field names (`fit_parent` in particular) — open `llaccordionctrl.h` and match exactly; drop any field that does not exist.
- `LLAccordionCtrl::addCollapsibleCtrl(LLView*)` signature.
- `get_accordion_tab_params()` and `outfit_accordion_tab_params` are file-local in `lloutfitslist.cpp` and already used at `:227`.
- `LLOutfitAccordionCtrlTab::setDisplayChildren`, `setTitle`, `setName`, `setFavorite` all exist (`lloutfitslist.h:274-310`).
- `updateRemovedCategory`'s existing body, so the erase lands after the tab is destroyed, not before.

**If nested accordions fight the layout during the live pass (Task 7), the spec sanctions a fallback:** a single flat accordion with non-collapsible folder header rows and indented outfit tabs. Do not pre-emptively build the fallback; try nesting first.

- [ ] **Step 9: Commit**

```bash
git add indra/newview/lloutfitslist.cpp indra/newview/lloutfitslist.h
git commit -m "feat: nest outfit subfolders as accordion tabs in the outfits list"
```

---

### Task 4: List — filter nested tabs

**Files:**
- Modify: `indra/newview/lloutfitslist.cpp`

**Interfaces:**
- Consumes: `mFolderAccordions` (Task 3).

- [ ] **Step 1: Give `applyFilterToTab` a folder branch**

`applyFilterToTab` currently reaches the tab body with
`dynamic_cast<LLWearableItemsList*>(tab->getAccordionView())` and returns early on null.
A folder tab's body is an `LLAccordionCtrl`, so it would fall straight through that cast
and escape filtering — visible, with every child hidden. Insert ahead of the existing
wearable-list logic, right after the `if (!tab) return;` guard:

```cpp
    // <ID> A folder tab survives the filter if its own name matches or any descendant
    // outfit survives. Recurses depth-first so a folder several levels up stays visible on
    // the strength of one match at the bottom.
    if (LLAccordionCtrl* inner = dynamic_cast<LLAccordionCtrl*>(tab->getAccordionView()))
    {
        std::string folder_title = tab->getTitle();
        LLStringUtil::toUpper(folder_title);
        std::string folder_filter = filter_substring;
        LLStringUtil::toUpper(folder_filter);

        bool self_matches = (std::string::npos != folder_title.find(folder_filter));
        bool any_child_visible = false;

        for (size_t i = 0; i < inner->getTabCount(); ++i)
        {
            LLOutfitAccordionCtrlTab* child =
                dynamic_cast<LLOutfitAccordionCtrlTab*>(inner->getTab((S32)i));
            if (!child)
            {
                continue;
            }
            applyFilterToTab(child->getFolderID(), child, filter_substring);
            if (child->getVisible())
            {
                any_child_visible = true;
            }
        }

        tab->setTitle(tab->getTitle(), folder_filter);
        tab->setFilterGeneration(getFilterGeneration());
        tab->setVisible(self_matches || any_child_visible);
        return;
    }
    // </ID>
```

- [ ] **Step 2: Apply the filter to nested tabs when the filter changes**

`onFilterSubStringChanged` (`:667`) iterates `mAccordion`'s tabs. Confirm by reading it that
its loop calls `applyFilterToTab` per top-level tab — the recursion in Step 1 then reaches
nested tabs for free. If instead it reaches into `LLWearableItemsList` directly per tab,
add a folder guard there too so folder tabs route through `applyFilterToTab`.

- [ ] **Step 3: Compile checklist (read, do not build)**

Confirm by reading:
- `LLAccordionCtrl::getTabCount()` and `getTab(S32)` exist with those names and return types (`llaccordionctrl.h`). If they are named differently (e.g. `mAccordionTabs` accessor), adapt.
- `LLAccordionCtrlTab::getAccordionView()` returns `LLView*`, so both `dynamic_cast`s are valid.
- `applyFilterToTab`'s declaration in `lloutfitslist.h` — the recursive call must match it.
- `getFilterGeneration()` is available on the panel (already used at `:664`).

- [ ] **Step 4: Commit**

```bash
git add indra/newview/lloutfitslist.cpp
git commit -m "fix: outfit list filter descends into subfolder tabs"
```

---

### Task 5: List — sort folders before outfits

**Files:**
- Modify: `indra/newview/lloutfitslist.cpp`

The gallery got folders-first in Task 1 Step 6. The list sorts through accordion
comparators instead (`LLOutfitsList` sets `OUTFIT_TAB_NAME_COMPARATOR` or
`OUTFIT_TAB_FAV_COMPARATOR` on `mAccordion` at `lloutfitslist.cpp:155`, `:167-171`), so the
same precedence has to be added there. Both comparators take two `LLAccordionCtrlTab*`.

- [ ] **Step 1: Add a shared folders-first helper**

In `lloutfitslist.cpp`, in the anonymous namespace or beside the comparator
implementations, above both `compare` bodies:

```cpp
// <ID> Folder tabs lead their level under every sort order. A folder tab is the one whose
// body is a nested accordion rather than a wearable list — the same test the filter uses.
static bool id_tab_is_folder(const LLAccordionCtrlTab* tab)
{
    if (!tab)
    {
        return false;
    }
    return dynamic_cast<const LLAccordionCtrl*>(tab->getAccordionView()) != NULL;
}
// </ID>
```

- [ ] **Step 2: Apply it in both comparators**

At the top of `LLOutfitTabNameComparator::compare` and again at the top of
`LLOutfitTabFavComparator::compare`, before their existing logic:

```cpp
    // <ID>
    const bool folder1 = id_tab_is_folder(tab1);
    const bool folder2 = id_tab_is_folder(tab2);
    if (folder1 != folder2)
    {
        return folder1;
    }
    // </ID>
```

Repeated in both rather than factored into a shared prologue, because the two `compare`
bodies are independent virtual overrides with no common base implementation to hook.

- [ ] **Step 3: Compile checklist (read, do not build)**

Confirm by reading:
- `LLAccordionCtrlTab::getAccordionView()` is `const`-callable, or drop the `const` from the
  helper's parameter to match.
- Both `compare` overrides take `(const LLAccordionCtrlTab*, const LLAccordionCtrlTab*)` and
  return `bool` meaning "tab1 sorts first".
- The comparator is applied to inner accordions too — Task 3 creates them without setting a
  comparator, so add `inner->setComparator(...)` in `addFolderTab` mirroring whichever
  comparator `mAccordion` currently holds, or nested tabs will sort in insertion order.

- [ ] **Step 4: Commit**

```bash
git add indra/newview/lloutfitslist.cpp
git commit -m "feat: sort subfolders before outfits in the outfits list"
```

---

### Task 6: Folder selection must not enable outfit actions

**Files:**
- Modify: `indra/newview/lloutfitslist.cpp`

- [ ] **Step 1: Guard the shared action test**

`LLOutfitListBase::isActionEnabled` (`:475`) decides Wear / Replace / Delete / Rename and
friends from `mSelectedOutfitUUID`. Folders are never assigned to `mSelectedOutfitUUID`
(Task 1 clears the selection on navigation, and folder tabs are not outfit tabs), so the
gate is mostly automatic. Add an explicit guard at the top of the function so a folder id
arriving by any other route cannot enable an outfit action:

```cpp
bool LLOutfitListBase::isActionEnabled(const LLSD& userdata)
{
    // <ID> A folder is not an outfit; no outfit action applies to one.
    if (mSelectedOutfitUUID.notNull())
    {
        LLViewerInventoryCategory* sel = gInventory.getCategory(mSelectedOutfitUUID);
        if (sel && !isOutfitFolder(sel))
        {
            return false;
        }
    }
    // </ID>
```

- [ ] **Step 2: Compile checklist (read, do not build)**

Confirm by reading the existing body of `isActionEnabled` that an early `return false` is
safe there — in particular that it does not handle any action that should stay enabled with
nothing selected. If it does, restrict the guard to the outfit-specific action names rather
than returning false wholesale.

- [ ] **Step 3: Commit**

```bash
git add indra/newview/lloutfitslist.cpp
git commit -m "fix: outfit actions stay disabled when a subfolder is selected"
```

---

### Task 7: Live verification

The code is unverifiable in this workspace. This task is the verification and is not optional.

- [ ] **Step 1: Hand the build to the repo owner**

Report that the branch is ready and needs an incremental rebuild (no new source files). Do
not attempt the build.

- [ ] **Step 2: Work the checklist**

Set up a test structure in inventory first: under My Outfits, a folder `Formal` containing
two outfits, a folder `Formal/Winter` containing one, and an empty folder `Empty`.

1. Outfits nested one level deep appear under their folder in both tabs, not at top level.
2. Gallery: double-click `Formal` descends; the breadcrumb appears; each segment navigates;
   the breadcrumb is hidden at the root.
3. List: the `Formal` tab expands to reveal its outfits; collapsing hides them.
4. `Formal/Winter` behaves the same as one level — check both tabs.
5. Gallery search flattens: typing shows matching outfits from the whole tree with no
   folder tiles and no breadcrumb; clearing restores the hierarchy and the folder you were
   in.
6. List search filters in place: non-matching outfits hide, a folder with no surviving
   children hides, a folder whose *own* name matches stays visible. Specifically search for
   the outfit inside `Formal/Winter` — both ancestor tabs must remain visible.
7. Wearing an outfit from inside a nested folder works, and the worn-outfit highlight lands
   on the right entry.
8. `Empty` is visible in both views and does not vanish.
9. Selecting a folder disables Wear / Delete / Rename in the gear and context menus.
10. Rename `Formal` in the inventory floater — the outfits view updates without a relog.
11. Open the floater against the full inventory; it opens and scrolls no slower than before.
12. Nested accordion layout is sane — no clipped, overlapping or zero-height tabs. **This is
    the failure mode most likely to appear.** If it does, apply the spec's sanctioned
    fallback: flat accordion, folder header rows, indented outfit tabs.

- [ ] **Step 3: Fix what the checklist finds, then commit**

Any failure is a real bug, not a test artifact. Fix it, re-run the affected rows, commit
with a `fix:` message.
