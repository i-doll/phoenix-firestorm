/**
 * @file idmcptools_notifications.cpp
 * @brief <ID> MCP server: generic incoming offer / dialog responder.
 *
 * Part of Five's custom Firestorm fork. Custom code carries an `ID` prefix.
 *
 * Everything pushed AT the avatar - inventory offers, teleport offers, friendship
 * requests, group invites, and llDialog blue-menus - is an LLNotification with a
 * payload and a button form, all answered through one respond() path. So a single
 * generic pair covers them all (and any future type), and is the ONLY way to
 * answer a blue-menu dialog at all:
 *   notifications.list    -> enumerate pending notifications (id, type, buttons, payload)
 *   notifications.respond -> press a button by name (accept/decline/etc.)
 * Notifications persist in the "Visible" channel until answered or expired, so no
 * buffer is needed - list reads the live store. Main-thread only.
 */

#include "llviewerprecompiledheaders.h"

#include "idmcp.h"
#include "idmcptools.h"
#include "idmcpserver.h"
#include "idmcprlvgate.h"

#include "llnotifications.h"
#include "rlvactions.h"
#include "rlvhandler.h"     // gRlvHandler, RLV_BHVR_SENDCHAT
#include "llsd.h"
#include "lluuid.h"

#include <string>
#include <vector>

namespace
{
    std::string arg_str(const boost::json::object& args, const char* key)
    {
        auto it = args.find(key);
        return (it != args.end() && it->value().is_string())
                   ? std::string(it->value().as_string().c_str()) : std::string();
    }

    // boost::json::value has no implicit std::string ctor (string -> string_view
    // -> value is two user conversions); build one explicitly.
    boost::json::value j_str(const std::string& s)
    {
        boost::json::value v;
        v = s;
        return v;
    }

    // Friendly type from the raw notification name.
    std::string friendly_type(const std::string& name)
    {
        if (name == "UserGiveItem" || name == "ObjectGiveItem" || name == "OwnObjectGiveItem")
            return "offer_inventory";
        if (name == "TeleportOffered" || name == "TeleportOffered_MaturityExceeded")
            return "offer_teleport";
        if (name == "OfferFriendship" || name == "OfferFriendshipNoMessage")
            return "offer_friendship";
        if (name == "JoinGroup" || name == "JoinGroupCanAfford" || name == "JoinGroupNoCost")
            return "invite_group";
        if (name == "ScriptDialog" || name == "ScriptDialogGroup")
            return "script_dialog";
        if (name == "GroupNotice")
            return "group_notice";
        return "other";
    }

    // Minimal LLSD -> boost::json conversion for exposing notification payloads.
    boost::json::value llsd_to_json(const LLSD& sd)
    {
        switch (sd.type())
        {
        case LLSD::TypeMap:
        {
            boost::json::object o;
            for (LLSD::map_const_iterator it = sd.beginMap(); it != sd.endMap(); ++it)
                o[it->first] = llsd_to_json(it->second);
            return o;
        }
        case LLSD::TypeArray:
        {
            boost::json::array a;
            for (LLSD::array_const_iterator it = sd.beginArray(); it != sd.endArray(); ++it)
                a.push_back(llsd_to_json(*it));
            return a;
        }
        case LLSD::TypeBoolean: return sd.asBoolean();
        case LLSD::TypeInteger: return (int64_t)sd.asInteger();
        case LLSD::TypeReal:    return sd.asReal();
        case LLSD::TypeUndefined: return nullptr;
        default:                return j_str(sd.asString());   // String, UUID, Date, URI, Binary
        }
    }

    // Enabled button names of a notification's form (the pressable options).
    boost::json::array form_buttons(const LLNotificationPtr& n)
    {
        boost::json::array buttons;
        LLNotificationFormPtr form = n->getForm();
        if (form)
        {
            const S32 count = form->getNumElements();
            for (S32 i = 0; i < count; ++i)
            {
                LLSD el = form->getElement(i);
                if (el["type"].asString() == "button" && el["name"].isDefined())
                    buttons.push_back(j_str(el["name"].asString()));
            }
        }
        return buttons;
    }
}

// ---------------------------------------------------------------------------

void idmcp_register_notifications_tools(IDMCPToolRegistry& reg)
{
    // notifications.list -----------------------------------------------------
    {
        IDMCPTool t;
        t.name = "notifications.list";
        t.description =
            "List pending notifications addressed to you - inventory offers, "
            "teleport offers, friendship requests, group invites, and llDialog "
            "blue-menus from scripted objects (e.g. after object.touch). Each: "
            "{id, name, type (offer_inventory|offer_teleport|offer_friendship|"
            "invite_group|script_dialog|other), message, buttons:[names], payload}. "
            "Answer one with notifications.respond. No arguments.";
        t.input_schema = boost::json::parse(
            R"({"type":"object","properties":{},"additionalProperties":false})");
        t.invoke = [](const boost::json::object&, const IDMCPCallPtr& call)
        {
            boost::json::array arr;
            LLNotificationChannelPtr chan = LLNotifications::instance().getChannel("Visible");
            if (chan)
            {
                chan->forEachNotification([&arr](LLNotificationPtr n)
                {
                    if (!n) return;
                    boost::json::object o;
                    o["id"]      = n->getID().asString();
                    o["name"]    = n->getName();
                    o["type"]    = friendly_type(n->getName());
                    o["message"] = n->getMessage();
                    o["buttons"] = form_buttons(n);
                    o["payload"] = llsd_to_json(n->getPayload());
                    arr.push_back(std::move(o));
                });
            }
            boost::json::object out;
            out["notifications"] = std::move(arr);
            idmcp_tool_ok(call, out);
        };
        reg.add(std::move(t));
    }

    // notifications.respond --------------------------------------------------
    {
        IDMCPTool t;
        t.name = "notifications.respond";
        t.description =
            "Respond to a pending notification by pressing one of its buttons. "
            "{\"id\"} (from notifications.list) and {\"button\"} (a button NAME "
            "from that notification's buttons, e.g. \"Keep\"/\"Discard\" for an "
            "item offer, \"Accept\"/\"Decline\" for offers, or a blue-menu label). "
            "Returns {responded, button}. For script dialogs, RLV @sendchat / "
            "@sendchannel on the dialog's channel are enforced.";
        t.input_schema = boost::json::parse(
            R"({"type":"object","properties":{"id":{"type":"string"},"button":{"type":"string"}},"required":["id","button"],"additionalProperties":false})");
        t.invoke = [](const boost::json::object& args, const IDMCPCallPtr& call)
        {
            const std::string idstr = arg_str(args, "id");
            const std::string button = arg_str(args, "button");
            if (idstr.empty() || button.empty())
            {
                idmcp_tool_err(call, IDMCP_ERR_INVALID_PARAMS, "id and button are required");
                return;
            }
            LLNotificationPtr n = LLNotifications::instance().find(LLUUID(idstr));
            if (!n)
            {
                idmcp_tool_err(call, IDMCP_ERR_NOT_FOUND, "no such pending notification (may have expired)");
                return;
            }

            // Validate the button is a real form element.
            LLNotificationFormPtr form = n->getForm();
            bool found = false;
            if (form)
            {
                for (S32 i = 0, c = form->getNumElements(); i < c && !found; ++i)
                {
                    LLSD el = form->getElement(i);
                    if (el["type"].asString() == "button" && el["name"].asString() == button)
                        found = true;
                }
            }
            if (!found)
            {
                idmcp_tool_err(call, IDMCP_ERR_INVALID_PARAMS,
                               "no such button on that notification (see notifications.list buttons)");
                return;
            }

            // Pressing a blue-menu button sends chat on the dialog's channel -
            // enforce RLV @sendchat / @sendchannel just like chat.send.
            if (friendly_type(n->getName()) == "script_dialog" && IDMCPRlvGate::isEnabled())
            {
                const S32 channel = n->getPayload()["chat_channel"].asInteger();
                if (channel == 0)
                {
                    if (gRlvHandler.hasBehaviour(RLV_BHVR_SENDCHAT))
                    {
                        idmcp_tool_err(call, IDMCP_ERR_RLV_RESTRICTED,
                                       "blocked by RLV @sendchat");
                        return;
                    }
                }
                else if (!RlvActions::canSendChannel(channel))
                {
                    idmcp_tool_err(call, IDMCP_ERR_RLV_RESTRICTED,
                                   "blocked by RLV @sendchannel");
                    return;
                }
            }

            LLSD response = n->getResponseTemplate();
            response[button] = true;
            n->respond(response);

            boost::json::object o;
            o["responded"] = true;
            o["button"]    = button;
            idmcp_tool_ok(call, o);
        };
        reg.add(std::move(t));
    }

    // notifications.dismiss --------------------------------------------------
    {
        IDMCPTool t;
        t.name = "notifications.dismiss";
        t.description =
            "Clear a pending notification WITHOUT answering it - it just disappears "
            "from the list; no button is pressed, so an offer is NOT accepted or "
            "declined and an attached item is NOT kept or discarded. {\"id\"} "
            "dismisses one; OMIT id to dismiss ALL currently-listed notifications. "
            "Use this for things you only need to clear - group notices, info toasts "
            "- and use notifications.respond when you actually want to accept/decline "
            "an offer or press a dialog button. Returns {dismissed:<count>}.";
        t.input_schema = boost::json::parse(
            R"({"type":"object","properties":{"id":{"type":"string"}},"additionalProperties":false})");
        t.invoke = [](const boost::json::object& args, const IDMCPCallPtr& call)
        {
            const std::string idstr = arg_str(args, "id");
            int dismissed = 0;

            if (!idstr.empty())
            {
                LLNotificationPtr n = LLNotifications::instance().find(LLUUID(idstr));
                if (!n)
                {
                    idmcp_tool_err(call, IDMCP_ERR_NOT_FOUND,
                                   "no such pending notification (may have expired)");
                    return;
                }
                LLNotifications::instance().cancel(n);   // clear, don't run the responder
                dismissed = 1;
            }
            else
            {
                // Dismiss all. Collect first, then cancel - cancelling mutates the
                // channel, so it isn't safe to cancel during forEachNotification.
                std::vector<LLNotificationPtr> all;
                LLNotificationChannelPtr chan = LLNotifications::instance().getChannel("Visible");
                if (chan)
                {
                    chan->forEachNotification([&all](LLNotificationPtr n) { if (n) all.push_back(n); });
                }
                for (const auto& n : all)
                {
                    LLNotifications::instance().cancel(n);
                    ++dismissed;
                }
            }

            boost::json::object o;
            o["dismissed"] = dismissed;
            idmcp_tool_ok(call, o);
        };
        reg.add(std::move(t));
    }
}
