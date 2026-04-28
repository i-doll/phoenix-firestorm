#ifndef ID_KEYWORD_HIGHLIGHT_H
#define ID_KEYWORD_HIGHLIGHT_H

#include "llsingleton.h"
#include "v4color.h"
#include <map>
#include <optional>

class LLChat;

struct IDKeywordEntry
{
    LLColor4 color;
    bool matchSenderName;  // If true, match against sender name; if false, match against message text
};

class IDKeywordHighlight : public LLSingleton<IDKeywordHighlight>
{
    LLSINGLETON(IDKeywordHighlight);
    virtual ~IDKeywordHighlight();

public:
    void updateKeywordList();
    std::optional<LLColor4> getMatchingKeywordColor(const LLChat& chat) const;

    // For floater management
    LLSD getKeywordColorMap() const;
    void setKeywordColorMap(const LLSD& map);
    void addKeyword(const std::string& keyword, const LLColor4& color, bool matchSenderName);
    void removeKeyword(const std::string& keyword);
    void updateKeyword(const std::string& keyword, const LLColor4& color, bool matchSenderName);

private:
    std::map<std::string, IDKeywordEntry> mKeywordEntries;
};

#endif // ID_KEYWORD_HIGHLIGHT_H
