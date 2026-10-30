/**
 * @file idheadless.cpp
 * @brief <ID> Headless test mode: launch flags, isolation and status file.
 *
 * Part of Five's custom Firestorm fork.
 */

#include "llviewerprecompiledheaders.h"

#include "idheadless.h"

#include "indra_constants.h"
#include "llappviewer.h"
#include "llevents.h"
#include "llviewercontrol.h"
#include "llcontrol.h"
#include "lldir.h"
#include "llfile.h"
#include "lllfsthread.h"
#include "llwindow.h"
#include "llsecapi.h"
#include "lllogininstance.h"
#include "idmcpserver.h"
#include "idmcptools_headless.h"
#include "lltimer.h"
#include "llviewernetwork.h"
#include "lldrawable.h"
#include "llstartup.h"
#include "llviewercamera.h"
#include "llviewerregion.h"
#include "llvocache.h"
#include "llworld.h"
#include "pipeline.h"
#include "llagent.h"
#include "llviewerdisplay.h"
#include "llprogressview.h"
#include "llviewerwindow.h"

#include <algorithm>
#include <boost/json.hpp>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <ctime>

#if LL_LINUX
#include <csignal>
#include <cerrno>
#include <cstring>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/file.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#endif

bool gIDHeadless = false;

namespace
{
    std::string s_base_dir;
    std::string s_credential_file;
    U32 s_port = 0;
    std::string s_avatar_dir;
    bool s_ready_to_write = false;
    idheadless::State s_state = idheadless::State::Starting;
    F64 s_ready_at = 0.0;
    boost::json::object s_status;

    const char* stateName(idheadless::State s)
    {
        switch (s)
        {
            case idheadless::State::Starting:   return "starting";
            case idheadless::State::LoggingIn:  return "logging_in";
            case idheadless::State::CleaningUp: return "cleaning_up";
            case idheadless::State::Ready:      return "ready";
            case idheadless::State::Quitting:   return "quitting";
            case idheadless::State::Failed:     return "failed";
        }
        return "starting";
    }

    std::string utcNow()
    {
        time_t now = time(nullptr);
        struct tm utc;
#if LL_WINDOWS
        gmtime_s(&utc, &now);
#else
        gmtime_r(&now, &utc);
#endif
        char buf[32];
        strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%SZ", &utc);
        return buf;
    }

    std::string statusPath()
    {
        std::string path = gSavedSettings.getString("IDHeadlessStatusFile");
        if (path.empty())
        {
            path = gDirUtilp->getOSUserAppDir() + "/headless-status.json";
        }
        return path;
    }

    void writeStatus()
    {
        if (!s_ready_to_write)
        {
            return;
        }
        s_status["updated"] = utcNow();
        const std::string path = statusPath();
        const std::string tmp = path + ".tmp";
        {
            llofstream out(tmp.c_str());
            if (!out.is_open())
            {
                LL_WARNS("Headless") << "Cannot write status file " << tmp << LL_ENDL;
                return;
            }
            out << boost::json::serialize(s_status);
        }
        LLFile::rename(tmp, path);
    }

#if LL_LINUX
    void mkdirP(const std::string& path)
    {
        for (size_t pos = 1; pos != std::string::npos; )
        {
            pos = path.find('/', pos);
            const std::string part = path.substr(0, pos);
            if (mkdir(part.c_str(), 0700) != 0 && errno != EEXIST)
            {
                return;
            }
            if (pos != std::string::npos)
            {
                ++pos;
            }
        }
    }

    bool parsePort(const char* text, U32& port)
    {
        if (!text || !*text)
        {
            return false;
        }
        char* end = nullptr;
        long value = strtol(text, &end, 10);
        if (*end != '\0' || value < 1 || value > 65535)
        {
            return false;
        }
        port = (U32)value;
        return true;
    }

    volatile sig_atomic_t s_term = 0;
    bool s_quit_pending = false;
    bool s_quit_started = false;
    bool s_quit_armed = false;
    F64 s_last_idle_check = 0.0;
    F64 s_init_at = 0.0;
    constexpr F64 READY_DEADLINE_SECS = 300.0;

    void onSignal(int)
    {
        s_term = s_term + 1;
    }

    int s_login_retries = 0;
    bool s_retry_pending = false;
    std::chrono::steady_clock::time_point s_retry_at;

    void updateScene()
    {
        display_update_teleport_headless();
        LLProgressView* progress = gViewerWindow ? gViewerWindow->getProgressView() : nullptr;
        static bool s_throttle_armed = false;
        if (progress && !s_throttle_armed && LLViewerRegion::isNewObjectCreationThrottleDisabled())
        {
            // calcNewObjectCreationThrottle only leaves its unthrottled start value after
            // it has seen the progress view visible once, then hidden for the delay time.
            progress->setVisible(true);
            LLViewerRegion::calcNewObjectCreationThrottle();
            s_throttle_armed = true;
        }
        if (progress && progress->getVisible())
        {
            progress->setVisible(false);
        }
        LLDrawable::incrementVisible();
        gPipeline.createObjects(LLPipeline::getUpdateTimeBudget());
        LLCamera& camera = *LLViewerCamera::getInstance();
        for (LLViewerRegion* region : LLWorld::getInstance()->getRegionList())
        {
            if (LLVOCachePartition* part = region->getVOCachePartition())
            {
                part->cullAroundCamera(camera);
            }
        }
    }

    bool tick(const LLSD&)
    {
        if (LLStartUp::getStartupState() == STATE_STARTED && !LLApp::isExiting())
        {
            updateScene();
        }
        if (s_retry_pending && std::chrono::steady_clock::now() >= s_retry_at)
        {
            s_retry_pending = false;
            LLLoginInstance::getInstance()->reconnect();
        }
        if (s_term)
        {
            if (s_term > 1)
            {
                LLApp::setQuitting();
            }
            else
            {
                idheadless::requestCleanQuit("signal");
            }
        }
        if (s_quit_pending && !s_quit_started)
        {
            if (!s_quit_armed)
            {
                s_quit_armed = true;
            }
            else if (LLAppViewer::instance())
            {
                s_quit_started = true;
                const bool cleanup = s_state == idheadless::State::Ready || s_state == idheadless::State::CleaningUp;
                idheadless::setState(idheadless::State::Quitting);
                if (cleanup)
                {
                    idmcp_headless::finish([]() { LLAppViewer::instance()->requestQuit(); });
                }
                else
                {
                    LLAppViewer::instance()->requestQuit();
                }
            }
        }
        if (s_state != idheadless::State::Ready && s_state != idheadless::State::Quitting && s_state != idheadless::State::Failed
            && LLTimer::getTotalSeconds() - s_init_at > READY_DEADLINE_SECS)
        {
            idheadless::fail(idheadless::EXIT_STARTUP, std::string("not ready after 300 s (state ") + stateName(s_state) + ")");
        }
        if (s_state == idheadless::State::Ready)
        {
            LLViewerRegion* region = gAgent.getRegion();
            if (region && !region->getName().empty() && gAgent.getTeleportState() == LLAgent::TELEPORT_NONE
                && region->getName() != idheadless::statusString("region"))
            {
                idheadless::setStatusField("region", region->getName());
            }
        }
        if (s_state == idheadless::State::Ready && !s_quit_pending && IDMCPServer::instanceExists())
        {
            const F64 now = LLTimer::getTotalSeconds();
            if (now - s_last_idle_check >= 1.0)
            {
                s_last_idle_check = now;
                const U32 limit = gSavedSettings.getU32("IDHeadlessIdleQuit");
                if (limit > 0)
                {
                    const F64 last = std::max(s_ready_at, IDMCPServer::instance().lastToolCallTime());
                    if (now - last > limit)
                    {
                        idheadless::requestCleanQuit("idle");
                    }
                }
            }
        }
        return false;
    }

    bool portIsFree(U32 port)
    {
        int fd = socket(AF_INET, SOCK_STREAM, 0);
        if (fd < 0)
        {
            return false;
        }
        int one = 1;
        setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
        sockaddr_in addr;
        memset(&addr, 0, sizeof(addr));
        addr.sin_family = AF_INET;
        addr.sin_port = htons((uint16_t)port);
        inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);
        bool free_port = bind(fd, (sockaddr*)&addr, sizeof(addr)) == 0;
        close(fd);
        return free_port;
    }

    void applyFixedSettings()
    {
        gSavedSettings.setBOOL("AllowMultipleViewers", true);
        gSavedSettings.setBOOL("SLURLPassToOtherInstance", false);
        gSavedSettings.setBOOL("NoAudio", true);
        gSavedSettings.setBOOL("NoHardwareProbe", true);
        gSavedSettings.setBOOL("SkipBenchmark", true);
        gSavedSettings.setBOOL("EnableVoiceChat", false);
        gSavedSettings.setBOOL("FSLoginDontSavePassword", true);
        gSavedSettings.setBOOL("IDMCPServerEnabled", true);
        gSavedSettings.setBOOL("AudioStreamingMedia", false);
        gSavedSettings.setBOOL("AudioStreamingMusic", false);
        gSavedSettings.setBOOL("RenderGLMultiThreadedTextures", false);
        gSavedSettings.setBOOL("RenderGLMultiThreadedMedia", false);
        gSavedSettings.setBOOL("IDMCPBridgeChatEnabled", true);
        gSavedSettings.setF32("SceneLoadFrontPixelThreshold", 0.f);
    }
#endif
}

namespace idheadless
{

int earlyInit(int argc, char** argv)
{
#if LL_LINUX
    bool headless = false;
    bool port_ok = false;
    U32 port = 0;
    for (int i = 1; i < argc; ++i)
    {
        const std::string arg = argv[i];
        if (arg == "--headless")
        {
            headless = true;
        }
        else if (arg == "--mcp-port")
        {
            port_ok = (i + 1 < argc) && parsePort(argv[i + 1], port);
        }
        else if (arg.compare(0, 11, "--mcp-port=") == 0)
        {
            port_ok = parsePort(arg.c_str() + 11, port);
        }
    }
    if (!headless)
    {
        return 0;
    }
    if (!port_ok)
    {
        fprintf(stderr, "--headless requires --mcp-port N\n");
        return EXIT_STARTUP;
    }

    gIDHeadless = true;
    gOSMessageBoxDisabled = true;
    s_port = port;

    const char* home_env = getenv("HOME");
    const std::string home = home_env ? home_env : "";
    std::string user_dir_var = APP_NAME;
    for (char& c : user_dir_var)
    {
        c = (char)toupper((unsigned char)c);
    }
    user_dir_var += "_X64_USER_DIR";
    const char* incoming = getenv(user_dir_var.c_str());
    const std::string normal_dir = incoming && *incoming ? std::string(incoming) : home + "/.firestorm_x64";
    s_credential_file = normal_dir + "/user_settings/bin_conf.dat";

    s_base_dir = home + "/.firestorm_x64_headless";
    const std::string run_dir = s_base_dir + "/" + std::to_string(port);
    mkdirP(s_base_dir);
    mkdirP(run_dir);
    mkdirP(s_base_dir + "/avatars");
    setenv(user_dir_var.c_str(), run_dir.c_str(), 1);

    setenv("SDL_VIDEODRIVER", "offscreen", 1);
    setenv("LIBGL_ALWAYS_SOFTWARE", "1", 1);
    setenv("GALLIUM_DRIVER", "llvmpipe", 1);
    unsetenv("DISPLAY");
    unsetenv("WAYLAND_DISPLAY");
#endif
    return 0;
}

void init()
{
#if LL_LINUX
    if (!gIDHeadless)
    {
        return;
    }

    applyFixedSettings();

    const U32 port = gSavedSettings.getU32("IDMCPServerPort");
    s_port = port;
    s_ready_to_write = true;
    s_init_at = LLTimer::getTotalSeconds();
    s_status["pid"] = (int64_t)getpid();
    s_status["port"] = (int64_t)port;
    s_status["run"] = gSavedSettings.getString("IDHeadlessRunName");

    if (!portIsFree(port))
    {
        fail(EXIT_PORT_BUSY, "MCP port " + std::to_string(port) + " is in use");
    }

    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = onSignal;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGTERM, &sa, nullptr);
    sigaction(SIGINT, &sa, nullptr);

    LLEventPumps::instance().obtain("mainloop").listen("IDHeadless", &tick);

    setState(State::Starting);
#endif
}

void fail(S32 code, const std::string& reason)
{
    s_status["error"] = reason;
    s_status["exit_code"] = (int64_t)code;
    setState(State::Failed);
    LL_WARNS("Headless") << reason << LL_ENDL;
    if (LLAppViewer::instance() && LLLFSThread::sLocal)
    {
        LLAppViewer::instance()->fastQuit(code);
    }
    std::_Exit(code);
}

void setState(State s)
{
    s_state = s;
    if (s == State::Ready)
    {
        s_ready_at = LLTimer::getTotalSeconds();
    }
    s_status["state"] = stateName(s);
    writeStatus();
}

bool isQuitting()
{
    return s_state == State::Quitting;
}

void setStatusField(const std::string& key, boost::json::value v)
{
    s_status[key] = std::move(v);
    writeStatus();
}

void setStatusField(const std::string& key, const std::string& v)
{
    setStatusField(key, boost::json::value(v));
}

const std::string& headlessBaseDir()
{
    return s_base_dir;
}

const std::string& normalCredentialFile()
{
    return s_credential_file;
}

void prepareLogin()
{
#if LL_LINUX
    std::string name = gSavedSettings.getString("IDHeadlessLoginName");
    if (!name.empty())
    {
        if (name.find('@') == std::string::npos)
        {
            name += "@" + LLGridManager::getInstance()->getGrid();
        }
        gSavedSettings.setString("UserLoginInfo", name);
        gSavedSettings.setBOOL("AutoLogin", true);
        setStatusField("avatar", name);
        return;
    }

    const char* user = getenv("IDHEADLESS_USERNAME");
    const char* pass = getenv("IDHEADLESS_PASSWORD");
    if (user && *user && pass && *pass)
    {
        const std::string username = user;
        const std::string password = pass;
        unsetenv("IDHEADLESS_USERNAME");
        unsetenv("IDHEADLESS_PASSWORD");

        std::string first = username;
        std::string last = "Resident";
        const size_t split = username.find_first_of(" .");
        if (split != std::string::npos)
        {
            first = username.substr(0, split);
            if (split + 1 < username.size())
            {
                last = username.substr(split + 1);
            }
        }
        LLSD login = LLSD::emptyArray();
        login.append(first);
        login.append(last);
        login.append(password);
        gSavedSettings.getControl("UserLoginInfoCmdLine")->setValue(login, false);
        setStatusField("avatar", first + " " + last);
        return;
    }
    unsetenv("IDHEADLESS_USERNAME");
    unsetenv("IDHEADLESS_PASSWORD");
    fail(EXIT_LOGIN, "no credentials: pass --login-name or set IDHEADLESS_USERNAME and IDHEADLESS_PASSWORD");
#endif
}

void beginLogin(const LLPointer<LLCredential>& cred_in, bool show_connect_box)
{
#if LL_LINUX
    LLPointer<LLCredential> cred = cred_in;
    const bool saved_login = !gSavedSettings.getString("IDHeadlessLoginName").empty();
    bool usable = cred.notNull() && !show_connect_box && !cred->getAuthenticator().isUndefined();
    if (usable && saved_login)
    {
        const LLSD stored = gSecAPIHandler->getProtectedData("credential", cred->getCredentialName());
        const LLSD auth = stored.isMap() ? stored.get("authenticator") : LLSD();
        usable = stored.has("identifier") && auth.isMap() && auth.size() > 0;
    }
    if (!usable)
    {
        const std::string name = saved_login ? statusString("avatar") : std::string();
        fail(EXIT_LOGIN, name.empty() ? std::string("login credentials unusable")
                                      : "saved login '" + name + "' not found or has no saved password");
    }

    const std::string cred_name = cred->getCredentialName();
    std::string key = cred_name;
    const size_t at = key.find('@');
    std::string user_part = key.substr(0, at);
    const std::string grid_part = at == std::string::npos ? std::string() : key.substr(at);
    for (char& c : user_part)
    {
        c = (char)tolower((unsigned char)c);
        if (c == ' ' || c == '_')
        {
            c = '.';
        }
    }
    const std::string resident = ".resident";
    if (user_part.size() > resident.size() && user_part.compare(user_part.size() - resident.size(), resident.size(), resident) == 0)
    {
        user_part.resize(user_part.size() - resident.size());
    }
    key = user_part + grid_part;
    for (char& c : key)
    {
        c = (char)tolower((unsigned char)c);
        if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '.' || c == '_' || c == '@' || c == '-'))
        {
            c = '_';
        }
    }

    s_avatar_dir = s_base_dir + "/avatars/" + key;
    mkdirP(s_avatar_dir);
    const std::string lock_path = s_avatar_dir + "/lock";
    const int fd = open(lock_path.c_str(), O_RDWR | O_CREAT | O_CLOEXEC, 0600);
    if (fd < 0)
    {
        fail(EXIT_LOGIN, "cannot create avatar lock " + lock_path + ": " + strerror(errno));
    }
    if (flock(fd, LOCK_EX | LOCK_NB) != 0)
    {
        if (errno == EWOULDBLOCK)
        {
            fail(EXIT_AVATAR_LOCKED, cred_name + " is already running headless");
        }
        fail(EXIT_LOGIN, "cannot lock " + lock_path + ": " + strerror(errno));
    }

    setState(State::LoggingIn);
#endif
}

void retryLoginLater()
{
#if LL_LINUX
    if (s_login_retries >= 3)
    {
        fail(EXIT_LOGIN, "already logged in elsewhere (presence) after 3 retries");
    }
    ++s_login_retries;
    LL_INFOS("Headless") << "Login refused (presence); retry " << s_login_retries << " of 3 in 20 s" << LL_ENDL;
    setStatusField("login_retry", (int64_t)s_login_retries);
    s_retry_at = std::chrono::steady_clock::now() + std::chrono::seconds(20);
    s_retry_pending = true;
#endif
}

void requestCleanQuit(const std::string& why)
{
#if LL_LINUX
    if (s_quit_pending)
    {
        return;
    }
    s_quit_pending = true;
    LL_INFOS("Headless") << "Quit requested: " << why << LL_ENDL;
#endif
}

std::string statusString(const std::string& key)
{
    const auto it = s_status.find(key);
    if (it == s_status.end() || !it->value().is_string())
    {
        return std::string();
    }
    return std::string(it->value().as_string().c_str());
}

const std::string& avatarDir()
{
    return s_avatar_dir;
}

}
