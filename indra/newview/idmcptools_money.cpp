/**
 * @file idmcptools_money.cpp
 * @brief <ID> MCP server: L$ balance (read) and guarded pay.
 *
 * Part of Five's custom Firestorm fork. Custom code carries an `ID` prefix.
 *
 * money.pay moves REAL currency, so it is fenced: it does nothing unless the
 * IDMCPMoneyEnabled debug setting is on (default OFF), it refuses amounts above
 * IDMCPMoneyMaxAmount, it honours RLV @pay / @buy, and it checks affordability
 * (never auto-opening the buy-currency floater). A {dry_run:true} preview resolves
 * the target and amount without sending. money.getBalance is a read (allowed
 * regardless of the switch); its {fresh:true} form requests a refresh and waits
 * for the balance to update (there is no per-request correlation, so it resolves
 * on change-or-deadline). Main-thread only.
 */

#include "llviewerprecompiledheaders.h"

#include "idmcp.h"
#include "idmcptools.h"
#include "idmcpserver.h"
#include "idmcprlvgate.h"

#include "llstatusbar.h"        // gStatusBar: getBalance, sendMoneyBalanceRequest
#include "llviewermessage.h"    // give_money, can_afford_transaction
#include "lltransactiontypes.h" // TRANS_GIFT, TRANS_PAY_OBJECT
#include "llagent.h"            // gAgent.getRegion
#include "llviewerregion.h"
#include "llviewercontrol.h"    // gSavedSettings
#include "rlvactions.h"

#include "llavatarnamecache.h"
#include "llavatarname.h"
#include "llevents.h"           // LLEventPumps, LLTempBoundListener
#include "lltimer.h"
#include "lluuid.h"

#include <memory>
#include <vector>

namespace
{
    bool looks_like_uuid(const std::string& s)
    {
        return s.size() == 36 && s[8] == '-' && s[13] == '-' && s[18] == '-' && s[23] == '-';
    }

    std::string arg_str(const boost::json::object& args, const char* key)
    {
        auto it = args.find(key);
        return (it != args.end() && it->value().is_string())
                   ? std::string(it->value().as_string().c_str()) : std::string();
    }

    S32 arg_int(const boost::json::object& args, const char* key, S32 dflt)
    {
        auto it = args.find(key);
        if (it == args.end()) return dflt;
        if (it->value().is_int64())  return (S32)it->value().as_int64();
        if (it->value().is_double()) return (S32)it->value().as_double();
        return dflt;
    }

    bool arg_bool(const boost::json::object& args, const char* key, bool dflt)
    {
        auto it = args.find(key);
        return (it != args.end() && it->value().is_bool()) ? it->value().as_bool() : dflt;
    }

    // ---- fresh-balance deferred wait ---------------------------------------

    struct BalWait
    {
        IDMCPCallPtr call;
        S32          baseline = 0;
        F64          deadline = 0.0;
        bool         done = false;
    };
    std::vector<std::shared_ptr<BalWait>> g_bal_waits;
    bool                                  g_tick_on = false;
    LLTempBoundListener                   g_tick;

    void bal_finish(const std::shared_ptr<BalWait>& w)
    {
        if (w->done) return;
        w->done = true;
        boost::json::object o;
        o["balance"]  = gStatusBar ? gStatusBar->getBalance() : 0;
        o["currency"] = "L$";
        o["fresh"]    = true;
        idmcp_tool_ok(w->call, o);
    }

    void money_tick()
    {
        if (g_bal_waits.empty()) return;
        const F64 now = LLTimer::getTotalSeconds();
        const S32 bal = gStatusBar ? gStatusBar->getBalance() : 0;
        for (auto it = g_bal_waits.begin(); it != g_bal_waits.end(); )
        {
            if (!(*it)->done && (bal != (*it)->baseline || now >= (*it)->deadline)) bal_finish(*it);
            if ((*it)->done) it = g_bal_waits.erase(it);
            else             ++it;
        }
    }

    void ensure_tick()
    {
        if (g_tick_on) return;
        g_tick = LLEventPumps::instance().obtain("mainloop").listen(
            "idmcp_money", [](const LLSD&) -> bool { money_tick(); return false; });
        g_tick_on = true;
    }

    std::string resolve_name(const LLUUID& id, bool is_avatar)
    {
        if (is_avatar)
        {
            LLAvatarName av;
            if (LLAvatarNameCache::get(id, &av)) return av.getCompleteName();
        }
        return id.asString();
    }
}

// ---------------------------------------------------------------------------

void idmcp_register_money_tools(IDMCPToolRegistry& reg)
{
    // money.getBalance -------------------------------------------------------
    {
        IDMCPTool t;
        t.name = "money.getBalance";
        t.description =
            "Your current L$ balance. {\"fresh\":true} requests an up-to-date "
            "figure from the server and waits briefly for it (otherwise returns the "
            "cached value). Read-only. Returns {balance, currency, fresh}.";
        t.input_schema = boost::json::parse(
            R"({"type":"object","properties":{"fresh":{"type":"boolean"}},"additionalProperties":false})");
        t.invoke = [](const boost::json::object& args, const IDMCPCallPtr& call)
        {
            const S32 bal = gStatusBar ? gStatusBar->getBalance() : 0;
            if (!arg_bool(args, "fresh", false))
            {
                boost::json::object o;
                o["balance"]  = bal;
                o["currency"] = "L$";
                o["fresh"]    = false;
                idmcp_tool_ok(call, o);
                return;
            }
            // Fresh: request an update and resolve on change-or-deadline.
            LLStatusBar::sendMoneyBalanceRequest();
            auto w = std::make_shared<BalWait>();
            w->call     = call;
            w->baseline = bal;
            w->deadline = LLTimer::getTotalSeconds() + 5.0;
            g_bal_waits.push_back(w);
            ensure_tick();
        };
        reg.add(std::move(t));
    }

    // money.pay --------------------------------------------------------------
    {
        IDMCPTool t;
        t.name = "money.pay";
        t.description =
            "Pay L$ to an avatar or object. {\"target_id\"} (UUID), "
            "{\"target_kind\"} \"avatar\"|\"object\", {\"amount\"} (>0), optional "
            "{\"description\"}. SPENDS REAL CURRENCY - it is refused unless the "
            "IDMCPMoneyEnabled setting is on, the amount is within IDMCPMoneyMaxAmount, "
            "RLV @pay/@buy allow it, and you can afford it. Use {\"dry_run\":true} to "
            "preview {would_pay, affordable} without sending. On send returns "
            "{paid:true, target_id, amount}.";
        t.input_schema = boost::json::parse(
            R"({"type":"object","properties":{"target_id":{"type":"string"},"target_kind":{"type":"string","enum":["avatar","object"]},"amount":{"type":"integer"},"description":{"type":"string"},"dry_run":{"type":"boolean"}},"required":["target_id","target_kind","amount"],"additionalProperties":false})");
        t.invoke = [](const boost::json::object& args, const IDMCPCallPtr& call)
        {
            const std::string spec = arg_str(args, "target_id");
            const std::string kind = arg_str(args, "target_kind");
            const S32          amount = arg_int(args, "amount", 0);
            const bool         dry_run = arg_bool(args, "dry_run", false);

            if (!looks_like_uuid(spec) || (kind != "avatar" && kind != "object"))
            {
                idmcp_tool_err(call, IDMCP_ERR_INVALID_PARAMS,
                               "target_id (UUID) and target_kind (avatar|object) required");
                return;
            }
            if (amount <= 0)
            {
                idmcp_tool_err(call, IDMCP_ERR_INVALID_PARAMS, "amount must be a positive integer");
                return;
            }
            const LLUUID target(spec);
            const bool   is_avatar = (kind == "avatar");

            // dry-run preview: no spend, no switch required.
            if (dry_run)
            {
                boost::json::object wp;
                wp["target_id"]     = spec;
                wp["resolved_name"] = resolve_name(target, is_avatar);
                wp["amount"]        = amount;
                boost::json::object o;
                o["would_pay"]  = std::move(wp);
                o["affordable"] = can_afford_transaction(amount);
                idmcp_tool_ok(call, o);
                return;
            }

            // Real send - full gating stack.
            if (!gSavedSettings.getBOOL("IDMCPMoneyEnabled"))
            {
                idmcp_tool_err(call, IDMCP_ERR_PERMISSION,
                               "paying is disabled (set IDMCPMoneyEnabled); use dry_run to preview");
                return;
            }
            if (IDMCPRlvGate::isEnabled())
            {
                if (is_avatar && !RlvActions::canPayAvatar(target))
                {
                    idmcp_tool_err(call, IDMCP_ERR_RLV_RESTRICTED, "blocked by RLV @pay");
                    return;
                }
                if (!is_avatar && !RlvActions::canPayObject(target))
                {
                    idmcp_tool_err(call, IDMCP_ERR_RLV_RESTRICTED, "blocked by RLV @buy");
                    return;
                }
            }
            const S32 cap = gSavedSettings.getS32("IDMCPMoneyMaxAmount");
            if (amount > cap)
            {
                idmcp_tool_err(call, IDMCP_ERR_PERMISSION,
                               "amount exceeds IDMCPMoneyMaxAmount (" + std::to_string(cap) + ")");
                return;
            }
            if (!can_afford_transaction(amount))
            {
                idmcp_tool_err(call, IDMCP_ERR_PERMISSION, "insufficient L$ balance");
                return;
            }

            give_money(target, gAgent.getRegion(), amount, /*is_group*/ false,
                       is_avatar ? TRANS_GIFT : TRANS_PAY_OBJECT, arg_str(args, "description"));

            boost::json::object o;
            o["paid"]      = true;
            o["target_id"] = spec;
            o["amount"]    = amount;
            idmcp_tool_ok(call, o);
        };
        reg.add(std::move(t));
    }
}
