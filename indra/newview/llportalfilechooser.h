/**
 * @file llportalfilechooser.h
 * @brief xdg-desktop-portal file chooser via D-Bus
 *
 * $LicenseInfo:firstyear=2024&license=viewerlgpl$
 * Second Life Viewer Source Code
 * Copyright (C) 2024, Linden Research, Inc.
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation;
 * version 2.1 of the License only.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301  USA
 *
 * Linden Research, Inc., 945 Battery Street, San Francisco, CA  94111  USA
 * $/LicenseInfo$
 */

#ifndef LL_LLPORTALFILECHOOSER_H
#define LL_LLPORTALFILECHOOSER_H

#include <string>
#include <vector>
#include <utility>

namespace LLPortalFileChooser
{
    using FilterPattern = std::pair<unsigned int, std::string>; // (0=glob|1=mime, pattern)
    using Filter = std::pair<std::string, std::vector<FilterPattern>>;

    std::vector<std::string> openFile(const std::string& title,
                                       const std::vector<Filter>& filters,
                                       bool multiple = false,
                                       bool directory = false);

    std::string saveFile(const std::string& title,
                         const std::vector<Filter>& filters,
                         const std::string& suggested_name = std::string());

    bool isAvailable();
}

#endif // LL_LLPORTALFILECHOOSER_H
