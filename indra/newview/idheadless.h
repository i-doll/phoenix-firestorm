/**
 * @file idheadless.h
 * @brief <ID> Headless test mode: launch flags, isolation and status file.
 *
 * Part of Five's custom Firestorm fork.
 */

#ifndef ID_IDHEADLESS_H
#define ID_IDHEADLESS_H

#include <string>
#include "stdtypes.h"
#include "llpointer.h"
#include <boost/json/value.hpp>

class LLCredential;

extern bool gIDHeadless;

namespace idheadless
{
    enum ExitCode : S32
    {
        EXIT_STARTUP = 10,
        EXIT_PORT_BUSY = 11,
        EXIT_LOGIN = 12,
        EXIT_AVATAR_LOCKED = 13,
        EXIT_DISCONNECTED = 14
    };

    enum class State { Starting, LoggingIn, CleaningUp, Ready, Quitting, Failed };

    int  earlyInit(int argc, char** argv);
    void init();
    [[noreturn]] void fail(S32 code, const std::string& reason);
    void setState(State s);
    bool isQuitting();
    void setStatusField(const std::string& key, boost::json::value v);
    void setStatusField(const std::string& key, const std::string& v);
    const std::string& headlessBaseDir();
    const std::string& normalCredentialFile();
    void prepareLogin();
    void beginLogin(const LLPointer<LLCredential>& cred, bool show_connect_box);
    const std::string& avatarDir();
    void retryLoginLater();
    void requestCleanQuit(const std::string& why);
    std::string statusString(const std::string& key);
}

#endif
