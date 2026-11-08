/**
 * @file idmaterialtheme.h
 * @brief <ID:MaterialTheme> Drives the UI colour table from the Material You palette while the
 * idyll_material skin is active.
 *
 * Part of Five's custom Firestorm fork.
 */

#ifndef ID_MATERIALTHEME_H
#define ID_MATERIALTHEME_H

#include "llsingleton.h"
#include "lluicolor.h"

#include <boost/signals2.hpp>
#include <string>
#include <vector>

class IDMaterialTheme : public LLSingleton<IDMaterialTheme>
{
    LLSINGLETON_EMPTY_CTOR(IDMaterialTheme);

public:
    static constexpr const char* SKIN_FOLDER = "idyll_material";

    static bool isActive();
    static const std::vector<std::string>& chromeColorNames();

    static LLUIColor getPreferenceColor(const std::string& name);
    static void setPreferenceColor(const std::string& name, const LLColor4& color);
    static LLColor4 worldChatColor(const LLColor4& color);

    void initOnStartup();
    void apply();

private:
    std::vector<boost::signals2::scoped_connection> mSettingConnections;
};

#endif
