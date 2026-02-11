/**
 * @file llportalfilechooser.cpp
 * @brief xdg-desktop-portal file chooser via D-Bus (GLib/GIO)
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

#include "llviewerprecompiledheaders.h"
#include "llportalfilechooser.h"

#if LL_LINUX

#include <gio/gio.h>
#include <cstdlib>
#include <cstring>
#include <sstream>

static const char* PORTAL_BUS_NAME   = "org.freedesktop.portal.Desktop";
static const char* PORTAL_OBJ_PATH   = "/org/freedesktop/portal/desktop";
static const char* PORTAL_IFACE      = "org.freedesktop.portal.FileChooser";
static const char* PORTAL_REQ_IFACE  = "org.freedesktop.portal.Request";

namespace
{

struct PortalResponseData
{
    bool        done{false};
    guint32     response{1}; // 1 = cancelled by default
    GVariant*   results{nullptr};
};

void on_portal_response(GDBusConnection* /*connection*/,
                        const gchar* /*sender_name*/,
                        const gchar* /*object_path*/,
                        const gchar* /*interface_name*/,
                        const gchar* /*signal_name*/,
                        GVariant* parameters,
                        gpointer user_data)
{
    auto* data = static_cast<PortalResponseData*>(user_data);
    guint32 response = 1;
    GVariant* results = nullptr;

    g_variant_get(parameters, "(u@a{sv})", &response, &results);

    data->response = response;
    data->results = results;
    data->done = true;
}

std::string make_handle_token()
{
    static int counter = 0;
    std::ostringstream ss;
    ss << "firestorm_" << getpid() << "_" << (++counter);
    return ss.str();
}

std::string get_sender_token(GDBusConnection* conn)
{
    const gchar* unique = g_dbus_connection_get_unique_name(conn);
    if (!unique) return {};
    // Convert ":1.42" -> "1_42"
    std::string s(unique);
    std::string result;
    for (char c : s)
    {
        if (c == ':') continue;
        if (c == '.') result += '_';
        else result += c;
    }
    return result;
}

GVariant* build_filters_variant(const std::vector<LLPortalFileChooser::Filter>& filters)
{
    GVariantBuilder filters_builder;
    g_variant_builder_init(&filters_builder, G_VARIANT_TYPE("a(sa(us))"));

    for (const auto& filter : filters)
    {
        GVariantBuilder patterns_builder;
        g_variant_builder_init(&patterns_builder, G_VARIANT_TYPE("a(us)"));
        for (const auto& pattern : filter.second)
        {
            g_variant_builder_add(&patterns_builder, "(us)",
                                  pattern.first, pattern.second.c_str());
        }
        g_variant_builder_add(&filters_builder, "(sa(us))",
                              filter.first.c_str(), &patterns_builder);
    }

    return g_variant_builder_end(&filters_builder);
}

std::vector<std::string> extract_uris(GVariant* results)
{
    std::vector<std::string> paths;
    if (!results) return paths;

    GVariant* uris_v = g_variant_lookup_value(results, "uris", G_VARIANT_TYPE_STRING_ARRAY);
    if (!uris_v) return paths;

    gsize n = 0;
    const gchar** uris = g_variant_get_strv(uris_v, &n);
    for (gsize i = 0; i < n; ++i)
    {
        gchar* local_path = g_filename_from_uri(uris[i], nullptr, nullptr);
        if (local_path)
        {
            paths.emplace_back(local_path);
            g_free(local_path);
        }
    }
    g_free(uris);
    g_variant_unref(uris_v);

    return paths;
}

} // anonymous namespace

std::vector<std::string> LLPortalFileChooser::openFile(
    const std::string& title,
    const std::vector<Filter>& filters,
    bool multiple,
    bool directory)
{
    GError* error = nullptr;
    GDBusConnection* conn = g_bus_get_sync(G_BUS_TYPE_SESSION, nullptr, &error);
    if (!conn)
    {
        LL_WARNS() << "Portal: Failed to get session bus: "
                   << (error ? error->message : "unknown") << LL_ENDL;
        if (error) g_error_free(error);
        return {};
    }

    std::string handle_token = make_handle_token();
    std::string sender_token = get_sender_token(conn);
    std::string request_path = "/org/freedesktop/portal/desktop/request/"
                               + sender_token + "/" + handle_token;

    PortalResponseData resp_data;
    guint sub_id = g_dbus_connection_signal_subscribe(
        conn,
        PORTAL_BUS_NAME,
        PORTAL_REQ_IFACE,
        "Response",
        request_path.c_str(),
        nullptr,
        G_DBUS_SIGNAL_FLAGS_NO_MATCH_RULE,
        on_portal_response,
        &resp_data,
        nullptr);

    // Build options dict
    GVariantBuilder options;
    g_variant_builder_init(&options, G_VARIANT_TYPE_VARDICT);
    g_variant_builder_add(&options, "{sv}", "handle_token",
                          g_variant_new_string(handle_token.c_str()));
    g_variant_builder_add(&options, "{sv}", "modal",
                          g_variant_new_boolean(TRUE));
    g_variant_builder_add(&options, "{sv}", "multiple",
                          g_variant_new_boolean(multiple ? TRUE : FALSE));
    g_variant_builder_add(&options, "{sv}", "directory",
                          g_variant_new_boolean(directory ? TRUE : FALSE));

    if (!filters.empty())
    {
        g_variant_builder_add(&options, "{sv}", "filters",
                              build_filters_variant(filters));
    }

    GVariant* params = g_variant_new("(ss@a{sv})",
                                     "",  // parent_window (empty for unparented)
                                     title.c_str(),
                                     g_variant_builder_end(&options));

    GVariant* result = g_dbus_connection_call_sync(
        conn,
        PORTAL_BUS_NAME,
        PORTAL_OBJ_PATH,
        PORTAL_IFACE,
        "OpenFile",
        params,
        G_VARIANT_TYPE("(o)"),
        G_DBUS_CALL_FLAGS_NONE,
        -1,  // no timeout
        nullptr,
        &error);

    if (!result)
    {
        LL_WARNS() << "Portal: OpenFile call failed: "
                   << (error ? error->message : "unknown") << LL_ENDL;
        if (error) g_error_free(error);
        g_dbus_connection_signal_unsubscribe(conn, sub_id);
        g_object_unref(conn);
        return {};
    }
    g_variant_unref(result);

    // Spin the GLib main loop until the portal sends the Response signal
    while (!resp_data.done)
    {
        g_main_context_iteration(nullptr, TRUE);
    }

    g_dbus_connection_signal_unsubscribe(conn, sub_id);

    std::vector<std::string> paths;
    if (resp_data.response == 0) // 0 = success
    {
        paths = extract_uris(resp_data.results);
    }

    if (resp_data.results)
        g_variant_unref(resp_data.results);
    g_object_unref(conn);

    return paths;
}

std::string LLPortalFileChooser::saveFile(
    const std::string& title,
    const std::vector<Filter>& filters,
    const std::string& suggested_name)
{
    GError* error = nullptr;
    GDBusConnection* conn = g_bus_get_sync(G_BUS_TYPE_SESSION, nullptr, &error);
    if (!conn)
    {
        LL_WARNS() << "Portal: Failed to get session bus: "
                   << (error ? error->message : "unknown") << LL_ENDL;
        if (error) g_error_free(error);
        return {};
    }

    std::string handle_token = make_handle_token();
    std::string sender_token = get_sender_token(conn);
    std::string request_path = "/org/freedesktop/portal/desktop/request/"
                               + sender_token + "/" + handle_token;

    PortalResponseData resp_data;
    guint sub_id = g_dbus_connection_signal_subscribe(
        conn,
        PORTAL_BUS_NAME,
        PORTAL_REQ_IFACE,
        "Response",
        request_path.c_str(),
        nullptr,
        G_DBUS_SIGNAL_FLAGS_NO_MATCH_RULE,
        on_portal_response,
        &resp_data,
        nullptr);

    // Build options dict
    GVariantBuilder options;
    g_variant_builder_init(&options, G_VARIANT_TYPE_VARDICT);
    g_variant_builder_add(&options, "{sv}", "handle_token",
                          g_variant_new_string(handle_token.c_str()));
    g_variant_builder_add(&options, "{sv}", "modal",
                          g_variant_new_boolean(TRUE));

    if (!suggested_name.empty())
    {
        g_variant_builder_add(&options, "{sv}", "current_name",
                              g_variant_new_string(suggested_name.c_str()));
    }

    if (!filters.empty())
    {
        g_variant_builder_add(&options, "{sv}", "filters",
                              build_filters_variant(filters));
    }

    GVariant* params = g_variant_new("(ss@a{sv})",
                                     "",  // parent_window
                                     title.c_str(),
                                     g_variant_builder_end(&options));

    GVariant* result = g_dbus_connection_call_sync(
        conn,
        PORTAL_BUS_NAME,
        PORTAL_OBJ_PATH,
        PORTAL_IFACE,
        "SaveFile",
        params,
        G_VARIANT_TYPE("(o)"),
        G_DBUS_CALL_FLAGS_NONE,
        -1,
        nullptr,
        &error);

    if (!result)
    {
        LL_WARNS() << "Portal: SaveFile call failed: "
                   << (error ? error->message : "unknown") << LL_ENDL;
        if (error) g_error_free(error);
        g_dbus_connection_signal_unsubscribe(conn, sub_id);
        g_object_unref(conn);
        return {};
    }
    g_variant_unref(result);

    while (!resp_data.done)
    {
        g_main_context_iteration(nullptr, TRUE);
    }

    g_dbus_connection_signal_unsubscribe(conn, sub_id);

    std::string path;
    if (resp_data.response == 0)
    {
        auto paths = extract_uris(resp_data.results);
        if (!paths.empty())
            path = paths[0];
    }

    if (resp_data.results)
        g_variant_unref(resp_data.results);
    g_object_unref(conn);

    return path;
}

bool LLPortalFileChooser::isAvailable()
{
    GError* error = nullptr;
    GDBusConnection* conn = g_bus_get_sync(G_BUS_TYPE_SESSION, nullptr, &error);
    if (!conn)
    {
        if (error) g_error_free(error);
        return false;
    }

    GVariant* result = g_dbus_connection_call_sync(
        conn,
        "org.freedesktop.DBus",
        "/org/freedesktop/DBus",
        "org.freedesktop.DBus",
        "NameHasOwner",
        g_variant_new("(s)", PORTAL_BUS_NAME),
        G_VARIANT_TYPE("(b)"),
        G_DBUS_CALL_FLAGS_NONE,
        1000,
        nullptr,
        &error);

    bool available = false;
    if (result)
    {
        gboolean has_owner = FALSE;
        g_variant_get(result, "(b)", &has_owner);
        available = has_owner;
        g_variant_unref(result);
    }
    else
    {
        if (error) g_error_free(error);
    }

    g_object_unref(conn);
    return available;
}

#endif // LL_LINUX
