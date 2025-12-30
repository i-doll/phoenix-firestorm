#include "llviewerprecompiledheaders.h"
#include "idsenderhighlight.h"
#include "llchat.h"
#include "llagent.h"
#include "llviewercontrol.h"
#include <boost/regex.hpp>

IDSenderHighlight::IDSenderHighlight()
{
    gSavedPerAccountSettings.getControl("IDSenderHighlightNames")->getSignal()->connect(
        boost::bind(&IDSenderHighlight::updateSenderList, this)
    );
    updateSenderList();
}

IDSenderHighlight::~IDSenderHighlight() {}

void IDSenderHighlight::updateSenderList()
{
    mSenderNames.clear();
    std::string names = gSavedPerAccountSettings.getString("IDSenderHighlightNames");

    boost::regex re(",");
    boost::sregex_token_iterator begin(names.begin(), names.end(), re, -1), end;
    while (begin != end)
    {
        std::string name(*begin++);
        LLStringUtil::trim(name);
        LLStringUtil::toLower(name);
        if (!name.empty())
        {
            mSenderNames.insert(name);
        }
    }
    LL_INFOS("IDSenderHighlight") << "Loaded " << mSenderNames.size() << " sender names from per-account settings" << LL_ENDL;
}

bool IDSenderHighlight::shouldHighlightSender(const LLChat& chat) const
{
    static LLCachedControl<bool> enabled(gSavedPerAccountSettings, "IDSenderHighlightEnabled", false);
    if (!enabled || chat.mFromID == gAgentID) return false;

    std::string sender = chat.mFromName;
    LLStringUtil::toLower(sender);

    return mSenderNames.find(sender) != mSenderNames.end();
}