#ifndef ID_SENDER_HIGHLIGHT_H
#define ID_SENDER_HIGHLIGHT_H

#include "llsingleton.h"
#include <set>

class LLChat;

class IDSenderHighlight : public LLSingleton<IDSenderHighlight>
{
    LLSINGLETON(IDSenderHighlight);
    virtual ~IDSenderHighlight();

public:
    void updateSenderList();
    bool shouldHighlightSender(const LLChat& chat) const;

private:
    std::set<std::string> mSenderNames;
};

#endif // ID_SENDER_HIGHLIGHT_H