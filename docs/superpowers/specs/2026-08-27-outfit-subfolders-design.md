# Design: subfolders in the Outfits floater

Date: 2026-08-27
Status: approved, ready for implementation plan

## Problem

My Outfits can already contain subfolders, and outfits nested inside them already show up
in the Outfits floater — but flattened. Both views deliberately discard the grouping:
`LLOutfitsList::updateAddedCategory` and `LLOutfitGallery::updateAddedCategory` each test
`isOutfitFolder(cat)`, and when it says "not an outfit" they register an observer on the
folder so its contents surface, then `return` without rendering anything for the folder
itself (`lloutfitslist.cpp:209`, `lloutfitgallery.cpp:825`).

The result: a resident who organises 200 outfits into `Formal`, `Everyday`, `Roleplay`
gets exactly the same undifferentiated wall of 200 entries they had before. The structure
exists in inventory and is invisible in the floater that exists to browse it.

This spec makes both views render that structure. It does **not** add any way to create or
change it — that stays in the regular inventory floater, which already does it well.

## Scope

**In:** rendering the existing hierarchy in both tabs, navigating it, and making search,
sorting and selection behave sensibly in the presence of folders.

**Out, by decision:** creating subfolders, moving outfits between them, renaming and
deleting folders, and drag-and-drop. All of that is the inventory floater's job. Recorded
here so it is not rediscovered as an oversight — there is no drag-and-drop machinery in
either view today (no `handleDragAndDrop`, no drag sources anywhere in
`lloutfitgallery.*` or `lloutfitslist.*`), so adding it would have been the largest single
piece of the work.

## Constraints discovered while reading the viewer

1. **The base already enumerates the whole tree.** `LLOutfitListBase::refreshList`
   (`lloutfitslist.cpp`) calls `gInventory.collectDescendentsIf` with `LLIsType(AT_CATEGORY)`,
   which recurses. Every descendant category of My Outfits — outfit or not, at any depth —
   already reaches `updateAddedCategory`. Nothing new needs to be fetched or observed.

2. **The diff machinery is tuned for scale and should not be touched.** `refreshList` /
   `computeDifference` / the `mRefreshListState` idle loop are built around a flat
   added/removed diff, and the file carries an explicit warning path for inventories over
   1000 outfits (`FSLargeOutfitsWarningInThisSession`). Re-shaping that diff into a tree
   would put the one already-hardened part of this code at risk for no gain.

3. **`isOutfitFolder()` guesses, and will sometimes guess wrong.**
   (`lloutfitslist.cpp:1060`.) `FT_OUTFIT` is a definite outfit. An `FT_NONE` folder is
   *also* treated as an outfit when it has more than 3 descendants, no subcategories, and
   more than 3 direct items — a heuristic for legacy pre-`FT_OUTFIT` outfits, carrying an
   upstream TODO saying as much. Consequence for this feature: an empty subfolder reads
   correctly as a folder, and a subfolder containing outfits reads correctly as a folder
   (its `cats` are non-empty), but a subfolder holding four or more loose items and no
   subfolders renders as an outfit.

4. **The gallery rebuilds its entire layout on every rearrange.**
   `LLOutfitGallery::reArrangeRows` (`lloutfitgallery.cpp:458`) tears down every item, sorts
   the buffer with `compareGalleryItem`, applies the name filter, and re-adds. That is the
   single choke point where "which entries belong on screen right now" is decided — which
   makes it the natural and only place to apply a current-folder filter.

5. **Gallery sorting is a free function reading a setting.** `compareGalleryItem`
   (`:428`) switches on `OutfitGallerySortOrder`: 0 alphabetical, 1 images-first,
   2 favourites-first. It takes two `LLOutfitGalleryItem*` and nothing else, so a
   folders-first rule belongs at the top of it.

6. **List sorting goes through accordion comparators.** `LLOutfitsList` sets
   `OUTFIT_TAB_NAME_COMPARATOR` or `OUTFIT_TAB_FAV_COMPARATOR` on `mAccordion`
   (`lloutfitslist.cpp:155`, `:167-171`).

7. **Accordion layout is already known-fragile.** `LLOutfitsList::arrange()` exists solely
   to force `mAccordion->arrange()` after a batch of tabs is added — a Firestorm addition
   because the stock layout misbehaved. This is the strongest signal that nesting an
   accordion inside an accordion is the risky part of this work.

8. **There is no breadcrumb widget in this codebase.** Searching the viewer sources and XUI
   for `breadcrumb` returns nothing. One has to be built.

9. **The list already filters by hiding tabs, and its filter assumes every tab body is a
   wearable list.** `LLOutfitsList::applyFilterToTab` hides a tab when its title misses the
   filter and `list->hasMatchedItems()` is false. It reaches that list via
   `dynamic_cast<LLWearableItemsList*>(tab->getAccordionView())` and returns early on null.
   A folder tab whose body is an `LLAccordionCtrl` therefore falls straight through the
   cast and escapes filtering entirely — it would stay visible with every child hidden.
   This is the single most important detail for the list half of this feature.

## Decisions

**Views place; the base is left alone.** Each view keeps the folder categories it currently
discards, renders them as folder entries, and uses `cat->getParentUUID()` to decide where
they belong. `LLOutfitListBase` gains only a `getMyOutfitsRoot()` helper and the
`mCurrentFolder` the gallery navigates with.

Rejected alternatives:

- *Model the hierarchy in `LLOutfitListBase`* — views become dumb renderers over a real
  tree. Cleaner in the abstract, but requires re-shaping the flat diff in constraint 2.
- *Replace both views with an `LLInventoryPanel` filtered to My Outfits* — inherits trees,
  drag-and-drop, rename, move and delete for free, and throws away the gallery's thumbnails
  and the accordion's wearable-list-inside-a-tab, which are the entire reason these two
  views exist rather than a folder tree.

**Each view uses its own idiom** — the gallery drills down one level at a time, the list
nests in place. They are different widgets that are good at different things; forcing one
model on both would mean either a gallery that scrolls forever or an accordion that has
lost its see-everything-at-once character.

**Search behaves per view, for the same reason navigation does.**

The gallery *flattens*: with a filter active it ignores `mCurrentFolder`, shows every
matching outfit from the whole tree, drops folder tiles, and hides the breadcrumb. It has
to — under drill-down, filtering within the current folder would hide every match that
lives somewhere else, which is exactly when search matters.

The list *filters in place*: non-matching outfit tabs hide, and a folder tab hides when
neither its own title matches nor any descendant survives. The hierarchy stays visible.
This is not a different philosophy, it is the mechanism the list already has (constraint 9)
extended to fold folder tabs into the same rule — and unlike the gallery, nothing is hidden
behind a navigation step, so there is nothing to flatten away.

**Selecting a folder is not selecting an outfit.** `mSelectedOutfitUUID` remains
outfit-only.

## Architecture

### Gallery — drill-down

`mCurrentFolder` (defaulting to the My Outfits root) is the only new state.

- `updateAddedCategory` (`lloutfitgallery.cpp:818`) stops discarding non-outfit folders.
  It builds an `LLOutfitGalleryItem` for them too, flagged `mIsFolder`, and registers it in
  `mOutfitMap` alongside outfits. The existing observer registration for the folder stays —
  it is what keeps nested contents live.
- `LLOutfitGalleryItem` gains `mIsFolder` and a folder icon. A flag rather than a subclass:
  the tile differs only in its icon and what a double click does, and `mItems` /
  `mOutfitMap` / `compareGalleryItem` are all typed on the concrete class.
- `reArrangeRows` (`:458`) gains one filter: when no name filter is active, an entry is laid
  out only if its category's parent is `mCurrentFolder`. When a name filter *is* active, the
  parent test is skipped entirely and folder entries are excluded — that is the flattened
  search.
- `compareGalleryItem` (`:428`) gains a folders-before-outfits test ahead of the existing
  switch, so folders lead each level under every sort order.
- Double-clicking a folder tile sets `mCurrentFolder` and calls `reArrangeRows`. Descending
  clears the outfit selection.

### Gallery — breadcrumb

A new `LLPanel` above the scroll container in `panel_outfit_gallery.xml`, holding a
horizontal run of `LLTextBox` links built from the parent chain of `mCurrentFolder` up to
the My Outfits root: `Outfits > Formal > Winter`. Clicking a segment sets `mCurrentFolder`
to it. The strip is hidden when `mCurrentFolder` is the root and while a filter is active.

Rebuilt whenever `mCurrentFolder` changes. It does not need to react to inventory changes
directly; a rename or move of an ancestor arrives through the existing observer and
triggers a rearrange, which rebuilds it.

### List — nested tabs

A folder becomes an `LLOutfitAccordionCtrlTab` whose body holds a child `LLAccordionCtrl`
rather than an `LLWearableItemsList`. An outfit tab is added to the inner accordion of its
parent folder's tab when it has one, and to `mAccordion` otherwise. A map from category id
to that category's inner accordion resolves the parent.

Because `updateAddedCategory` is driven by the base's diff and gives no ordering guarantee
between a folder and its children, a child may arrive before its parent tab exists. Outfits
whose parent folder has no tab yet are held in a pending map keyed by parent id and attached
when that folder's tab is created.

`arrange()` must walk the inner accordions as well as `mAccordion`.

#### Filtering nested tabs

`applyFilterToTab` gains a folder branch ahead of its existing wearable-list path. For a
folder tab it recurses into the inner accordion's tabs, applies the same rule to each, and
then sets its own visibility to `own_title_matches || any_descendant_visible`. Because it
recurses depth-first, a folder several levels up correctly survives on the strength of one
matching outfit at the bottom.

The existing early `return` on a null `dynamic_cast` must be reached only by genuinely
malformed tabs, not by folder tabs — otherwise folders silently ignore the filter.

**Fallback, to be decided during implementation, not now.** If nested accordions fight the
layout (constraint 7), fall back to a single flat accordion with non-collapsible folder
header rows and indented outfit tabs beneath them — visually grouped without real nesting.
This is a real possibility, not a hedge; the implementation plan should try nesting first
and treat the fallback as a live option rather than a failure.

### Sorting and selection

- Gallery: folders first, then the existing `OutfitGallerySortOrder` rule within each group.
- List: the accordion comparators get the same folders-first precedence, applied to the tabs
  within each accordion (outer and inner alike).
- Gear and context menu actions that act on an outfit — Wear, Replace, Delete, Rename,
  Save, thumbnail actions — disable when the selection is a folder, through the existing
  `isActionEnabled` override in each view. Folders expose no actions of their own; that is
  what "rendering only" means.

## Error handling and edge cases

- **Empty subfolder** — renders as a folder tile / an empty folder tab. It must not vanish;
  a folder the resident made and can see in inventory disappearing from the outfits view
  would read as data loss.
- **Deep nesting** — no depth limit is imposed. The gallery is unaffected (one level at a
  time); the list indents and will eventually look silly, which is the resident's problem
  to solve in inventory.
- **A folder that is deleted or moved while open in the gallery** — `mCurrentFolder` no
  longer resolves; fall back to the My Outfits root and rebuild.
- **Legacy `FT_NONE` outfits** — rendered as outfits, per constraint 3. Deliberately
  unchanged: tightening the heuristic would silently reclassify existing residents' legacy
  outfits into folders, which is a worse failure than the one it fixes.
- **The 1000-outfit warning path** is untouched. The current-folder filter runs at layout
  time over the already-built item list, so the cost profile of a large inventory is
  unchanged.

## Testing

No test target covers `lloutfitslist.*` or `lloutfitgallery.*`, and this viewer is not built
in this workspace, so verification is careful reading plus a live pass by the repo owner:

1. Outfits nested one level deep appear under their folder in both tabs, not at top level.
2. Gallery: double-click a folder descends; the breadcrumb appears and each segment
   navigates; the breadcrumb is hidden at the root.
3. List: a folder tab expands to reveal its outfits; collapsing hides them.
4. Two levels of nesting behave the same as one.
5. Gallery search flattens: typing shows matching outfits from the whole tree with no
   folder tiles and no breadcrumb; clearing restores the hierarchy and the folder you were
   in.
6. List search filters in place: non-matching outfits hide, a folder with no surviving
   children hides, and a folder whose *own name* matches stays visible. Specifically check
   a match nested two levels down — its ancestors must both remain visible.
7. Wearing an outfit from inside a nested folder works, and the worn-outfit highlight lands
   on the right entry.
8. An empty subfolder is visible in both views.
9. Selecting a folder disables Wear / Delete / Rename in the gear and context menus.
10. Renaming or moving a folder in the inventory floater updates the outfits view without a
   relog.
11. A resident-scale inventory (several hundred outfits) opens and scrolls no slower than
    before.

## Out of scope

- Creating, renaming, moving or deleting folders from this floater
- Drag-and-drop
- Changing `isOutfitFolder`'s legacy heuristic
- The "Wearing" tab, which has no hierarchy to show
