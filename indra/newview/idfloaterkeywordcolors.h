#ifndef ID_FLOATER_KEYWORD_COLORS_H
#define ID_FLOATER_KEYWORD_COLORS_H

#include "llfloater.h"
#include "v4color.h"

class LLScrollListCtrl;
class LLLineEditor;
class LLColorSwatchCtrl;
class LLCheckBoxCtrl;

class IDFloaterKeywordColors : public LLFloater
{
public:
    IDFloaterKeywordColors(const LLSD& key);
    bool postBuild() override;
    void onOpen(const LLSD& key) override;

private:
    ~IDFloaterKeywordColors() {};

    void refreshList();
    void onAdd();
    void onUpdate();
    void onRemove();
    void onSelectionChange();
    void clearInputs();

    LLScrollListCtrl* mKeywordList;
    LLLineEditor* mKeywordInput;
    LLColorSwatchCtrl* mColorSwatch;
    LLCheckBoxCtrl* mMatchNameCheckbox;
};

#endif // FLOATER_ID_KEYWORD_COLORS_H
