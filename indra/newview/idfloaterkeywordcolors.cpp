#include "llviewerprecompiledheaders.h"
#include "idfloaterkeywordcolors.h"

#include "idkeywordhighlight.h"
#include "llcheckboxctrl.h"
#include "llcolorswatch.h"
#include "lllineeditor.h"
#include "llscrolllistctrl.h"

IDFloaterKeywordColors::IDFloaterKeywordColors(const LLSD& key)
    : LLFloater(key)
    , mKeywordList(nullptr)
    , mKeywordInput(nullptr)
    , mColorSwatch(nullptr)
    , mMatchNameCheckbox(nullptr)
{
}

bool IDFloaterKeywordColors::postBuild()
{
    mKeywordList = getChild<LLScrollListCtrl>("keyword_list");
    mKeywordList->setCommitOnSelectionChange(true);
    mKeywordList->setCommitCallback(boost::bind(&IDFloaterKeywordColors::onSelectionChange, this));

    mKeywordInput = getChild<LLLineEditor>("keyword_input");
    mColorSwatch = getChild<LLColorSwatchCtrl>("color_swatch");
    mMatchNameCheckbox = getChild<LLCheckBoxCtrl>("match_name_checkbox");
    
    // Set default color to yellow
    mColorSwatch->set(LLColor4::yellow);

    childSetAction("add_btn", boost::bind(&IDFloaterKeywordColors::onAdd, this));
    childSetAction("update_btn", boost::bind(&IDFloaterKeywordColors::onUpdate, this));
    childSetAction("remove_btn", boost::bind(&IDFloaterKeywordColors::onRemove, this));

    refreshList();

    return true;
}

void IDFloaterKeywordColors::onOpen(const LLSD& key)
{
    refreshList();
}

void IDFloaterKeywordColors::refreshList()
{
    if (!mKeywordList) return;

    mKeywordList->clearRows();

    LLSD colorMap = IDKeywordHighlight::getInstance()->getKeywordColorMap();
    for (LLSD::array_const_iterator it = colorMap.beginArray(); it != colorMap.endArray(); ++it)
    {
        const LLSD& entry = *it;
        if (entry.has("keyword") && entry.has("color"))
        {
            std::string keyword = entry["keyword"].asString();
            bool matchName = entry.has("match_name") ? entry["match_name"].asBoolean() : false;
            LLColor4 color;
            color.setValue(entry["color"]);

            LLSD row;
            row["columns"][0]["column"] = "keyword";
            row["columns"][0]["value"] = keyword;
            row["columns"][1]["column"] = "match_name";
            row["columns"][1]["value"] = matchName ? "Yes" : "No";
            row["columns"][2]["column"] = "color";
            row["columns"][2]["value"] = "████";  // Color block character
            row["columns"][2]["color"] = color.getValue();

            LLScrollListItem* item = mKeywordList->addElement(row);
            if (item)
            {
                LLScrollListCell* colorCell = item->getColumn(2);
                if (colorCell)
                {
                    colorCell->setColor(color);
                }
            }
        }
    }

    onSelectionChange();
}

void IDFloaterKeywordColors::onAdd()
{
    std::string keyword = mKeywordInput->getText();
    LLStringUtil::trim(keyword);
    
    if (keyword.empty())
    {
        return;
    }

    LLColor4 color = mColorSwatch->get();
    bool matchName = mMatchNameCheckbox->get();
    IDKeywordHighlight::getInstance()->addKeyword(keyword, color, matchName);
    
    clearInputs();
    refreshList();
}

void IDFloaterKeywordColors::onUpdate()
{
    LLScrollListItem* selected = mKeywordList->getFirstSelected();
    if (!selected)
    {
        return;
    }

    std::string oldKeyword = selected->getColumn(0)->getValue().asString();
    std::string newKeyword = mKeywordInput->getText();
    LLStringUtil::trim(newKeyword);
    
    if (newKeyword.empty())
    {
        return;
    }

    LLColor4 color = mColorSwatch->get();
    bool matchName = mMatchNameCheckbox->get();
    
    // Remove old and add new
    IDKeywordHighlight::getInstance()->removeKeyword(oldKeyword);
    IDKeywordHighlight::getInstance()->addKeyword(newKeyword, color, matchName);
    
    clearInputs();
    refreshList();
}

void IDFloaterKeywordColors::onRemove()
{
    LLScrollListItem* selected = mKeywordList->getFirstSelected();
    if (selected)
    {
        std::string keyword = selected->getColumn(0)->getValue().asString();
        IDKeywordHighlight::getInstance()->removeKeyword(keyword);
        clearInputs();
        refreshList();
    }
}

void IDFloaterKeywordColors::onSelectionChange()
{
    LLScrollListItem* selected = mKeywordList->getFirstSelected();
    bool hasSelection = (selected != nullptr);
    
    getChild<LLButton>("update_btn")->setEnabled(hasSelection);
    getChild<LLButton>("remove_btn")->setEnabled(hasSelection);

    if (hasSelection)
    {
        // Populate input fields with selected item's values
        std::string keyword = selected->getColumn(0)->getValue().asString();
        mKeywordInput->setText(keyword);
        
        // Get match_name and color from the stored data
        LLSD colorMap = IDKeywordHighlight::getInstance()->getKeywordColorMap();
        std::string lowerKeyword = keyword;
        LLStringUtil::toLower(lowerKeyword);
        
        for (LLSD::array_const_iterator it = colorMap.beginArray(); it != colorMap.endArray(); ++it)
        {
            const LLSD& entry = *it;
            std::string entryKeyword = entry["keyword"].asString();
            LLStringUtil::toLower(entryKeyword);
            if (entryKeyword == lowerKeyword)
            {
                LLColor4 color;
                color.setValue(entry["color"]);
                mColorSwatch->set(color);
                
                bool matchName = entry.has("match_name") ? entry["match_name"].asBoolean() : false;
                mMatchNameCheckbox->set(matchName);
                break;
            }
        }
    }
}

void IDFloaterKeywordColors::clearInputs()
{
    mKeywordInput->clear();
    mColorSwatch->set(LLColor4::yellow);
    mMatchNameCheckbox->set(false);
    mKeywordList->deselectAllItems();
    onSelectionChange();
}
