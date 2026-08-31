#pragma once
// ============================================================
// core/CacheInvalidation.hpp
//
// A tiny hook so a ViewModel can drop a process-wide cache without
// holding a reference to whatever owns it. (P2)
//
// The problem it solves: JsonRpcDispatcher::invalidateFieldsGetCache()
// and invalidateCurrencyCache() existed but nothing ever called them —
// there was no path from a ViewModel to the dispatcher instance. So the
// fields_get cache (300 s) would keep serving stale field metadata after
// a Settings write, and the currency cache (60 s) after a rate change.
//
// Container registers the hooks at boot; callers fire them by name. No
// singleton dependency, no header cycle, and the caches stay owned by
// the dispatcher.
//
// Hooks are registered once during single-threaded boot and only read
// afterwards, so no lock is needed on the fire path.
// ============================================================
#include <functional>
#include <string>
#include <vector>

namespace cerp::core {

class CacheInvalidation {
public:
    using Hook = std::function<void()>;

    /// Register a callback to run when fields_get metadata becomes stale.
    static void onFieldsGet(Hook h) { fieldsGetHooks_().push_back(std::move(h)); }

    /// Register a callback to run when currency data becomes stale.
    static void onCurrency(Hook h)  { currencyHooks_().push_back(std::move(h)); }

    /// Call after writing anything that changes field metadata —
    /// decimal_precision in particular, since fields_get carries `digits`.
    static void fieldsGet() { fire_(fieldsGetHooks_()); }

    /// Call after writing res_currency (rate, decimal_places, active).
    static void currency()  { fire_(currencyHooks_()); }

private:
    static std::vector<Hook>& fieldsGetHooks_() { static std::vector<Hook> h; return h; }
    static std::vector<Hook>& currencyHooks_()  { static std::vector<Hook> h; return h; }

    static void fire_(const std::vector<Hook>& hooks) {
        for (const auto& h : hooks) {
            // A cache drop must never break the write that triggered it.
            try { if (h) h(); } catch (...) {}
        }
    }
};

} // namespace cerp::core
