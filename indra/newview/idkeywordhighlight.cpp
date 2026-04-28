#include "llviewerprecompiledheaders.h"
#include "idkeywordhighlight.h"
#include "llchat.h"
#include "llagent.h"
#include "llviewercontrol.h"
#include "llsdserialize.h"

IDKeywordHighlight::IDKeywordHighlight()
{
    LLControlVariable* control = gSavedPerAccountSettings.getControl("IDKeywordColorMap");
    if (control)
    {
        control->getSignal()->connect(
            boost::bind(&IDKeywordHighlight::updateKeywordList, this)
        );
    }
    else
    {
        LL_WARNS("IDKeywordHighlight") << "IDKeywordColorMap control not found in per-account settings" << LL_ENDL;
    }
    updateKeywordList();
}

IDKeywordHighlight::~IDKeywordHighlight() {}

void IDKeywordHighlight::updateKeywordList()
{
    mKeywordEntries.clear();
    LLSD colorMap = gSavedPerAccountSettings.getLLSD("IDKeywordColorMap");

    LL_DEBUGS("IDKeywordHighlight") << "Loading keyword color map, array size: " << colorMap.size() << LL_ENDL;

    for (LLSD::array_const_iterator it = colorMap.beginArray(); it != colorMap.endArray(); ++it)
    {
        const LLSD& entry = *it;
        if (entry.has("keyword") && entry.has("color"))
        {
            std::string keyword = entry["keyword"].asString();
            std::string lowerKeyword = keyword;
            LLStringUtil::toLower(lowerKeyword);
            if (!lowerKeyword.empty())
            {
                IDKeywordEntry kwEntry;
                kwEntry.color.setValue(entry["color"]);
                kwEntry.matchSenderName = entry.has("match_name") ? entry["match_name"].asBoolean() : false;
                mKeywordEntries[lowerKeyword] = kwEntry;
                LL_DEBUGS("IDKeywordHighlight") << "Loaded keyword: '" << lowerKeyword 
                    << "' matchSenderName: " << kwEntry.matchSenderName << LL_ENDL;
            }
        }
    }
    LL_INFOS("IDKeywordHighlight") << "Loaded " << mKeywordEntries.size() << " keyword entries from per-account settings" << LL_ENDL;
}

std::optional<LLColor4> IDKeywordHighlight::getMatchingKeywordColor(const LLChat& chat) const
{
    static LLCachedControl<bool> enabled(gSavedPerAccountSettings, "IDKeywordHighlightEnabled", false);
    if (!enabled)
    {
        return std::nullopt;
    }
    
    // Don't highlight own messages
    if (chat.mFromID == gAgentID)
    {
        return std::nullopt;
    }

    if (mKeywordEntries.empty())
    {
        return std::nullopt;
    }

    std::string text = chat.mText;
    LLStringUtil::toLower(text);
    
    std::string senderName = chat.mFromName;
    LLStringUtil::toLower(senderName);

    // Check each keyword for a match
    for (const auto& pair : mKeywordEntries)
    {
        const std::string& keyword = pair.first;
        const IDKeywordEntry& entry = pair.second;
        
        if (entry.matchSenderName)
        {
            // Match against sender name
            if (senderName.find(keyword) != std::string::npos)
            {
                LL_DEBUGS("IDKeywordHighlight") << "Matched keyword '" << keyword << "' in sender name: " << senderName << LL_ENDL;
                return entry.color;
            }
        }
        else
        {
            // Match against message text
            if (text.find(keyword) != std::string::npos)
            {
                LL_DEBUGS("IDKeywordHighlight") << "Matched keyword '" << keyword << "' in text: " << text << LL_ENDL;
                return entry.color;
            }
        }
    }

    return std::nullopt;
}

LLSD IDKeywordHighlight::getKeywordColorMap() const
{
    return gSavedPerAccountSettings.getLLSD("IDKeywordColorMap");
}

void IDKeywordHighlight::setKeywordColorMap(const LLSD& map)
{
    gSavedPerAccountSettings.setLLSD("IDKeywordColorMap", map);
    // Explicitly update in case signal isn't connected yet
    updateKeywordList();
}

void IDKeywordHighlight::addKeyword(const std::string& keyword, const LLColor4& color, bool matchSenderName)
{
    LLSD colorMap = getKeywordColorMap();
    LLSD entry;
    entry["keyword"] = keyword;
    entry["color"] = color.getValue();
    entry["match_name"] = matchSenderName;
    colorMap.append(entry);
    setKeywordColorMap(colorMap);
}

void IDKeywordHighlight::removeKeyword(const std::string& keyword)
{
    LLSD colorMap = getKeywordColorMap();
    LLSD newMap;
    
    std::string lowerKeyword = keyword;
    LLStringUtil::toLower(lowerKeyword);

    for (LLSD::array_const_iterator it = colorMap.beginArray(); it != colorMap.endArray(); ++it)
    {
        const LLSD& entry = *it;
        std::string entryKeyword = entry["keyword"].asString();
        LLStringUtil::toLower(entryKeyword);
        if (entryKeyword != lowerKeyword)
        {
            newMap.append(entry);
        }
    }
    setKeywordColorMap(newMap);
}

void IDKeywordHighlight::updateKeyword(const std::string& keyword, const LLColor4& color, bool matchSenderName)
{
    LLSD colorMap = getKeywordColorMap();
    
    std::string lowerKeyword = keyword;
    LLStringUtil::toLower(lowerKeyword);

    for (LLSD::array_iterator it = colorMap.beginArray(); it != colorMap.endArray(); ++it)
    {
        LLSD& entry = *it;
        std::string entryKeyword = entry["keyword"].asString();
        LLStringUtil::toLower(entryKeyword);
        if (entryKeyword == lowerKeyword)
        {
            entry["color"] = color.getValue();
            entry["match_name"] = matchSenderName;
            break;
        }
    }
    setKeywordColorMap(colorMap);
}
