#pragma once
// ============================================================
// core/infrastructure/ClientIp.hpp
//
// Proxy-aware client IP resolution (S-40, SEC-32).
//
// The problem this solves:
//   Rate limiters keyed on req->getPeerAddr() are correct only when
//   clients connect directly. Behind nginx every request arrives from
//   127.0.0.1, so a per-IP limiter collapses into a single global
//   bucket. That inverts it into a weapon:
//     * 10 bad logins from one attacker lock out EVERY user, because
//       the counter is shared (unauthenticated denial of service);
//     * recordSuccess() erases that shared bucket, so anyone holding
//       one valid credential can reset the counter every few attempts
//       and brute-force indefinitely.
//
// Why the LAST X-Forwarded-For entry:
//   nginx's $proxy_add_x_forwarded_for APPENDS the peer it actually saw
//   to whatever the client sent. So:
//     client sends nothing        -> "203.0.113.7"
//     client sends "1.2.3.4"      -> "1.2.3.4, 203.0.113.7"
//   The last element is the address nginx observed and is therefore the
//   only element a client cannot forge. Taking the FIRST element — the
//   common mistake — reads attacker-controlled data.
//
// Why the trusted-proxy check:
//   XFF/X-Real-IP are only meaningful when the immediate peer is our own
//   proxy. If the app is ever reached directly, a client could otherwise
//   set the header itself and pick its own rate-limit bucket. Unknown
//   peers therefore fall back to the socket address.
//
// Header-only by design: this is a few string operations with no heavy
// includes, and JsonRpcDispatcher (which needs it) is itself
// header-only. PERF-E's split applies to translation-unit weight, and
// there is none here.
// ============================================================
#include <drogon/HttpRequest.h>

#include <string>
#include <unordered_set>

namespace cerp::infrastructure {

class ClientIpResolver {
public:
    ClientIpResolver() { setTrusted("127.0.0.1,::1"); }

    /// @param csv Comma-separated proxy addresses, e.g. "127.0.0.1,::1".
    explicit ClientIpResolver(const std::string& csv) { setTrusted(csv); }

    void setTrusted(const std::string& csv) {
        trusted_.clear();
        std::string cur;
        for (char c : csv) {
            if (c == ',') { addTrusted_(cur); cur.clear(); }
            else          { cur += c; }
        }
        addTrusted_(cur);
    }

    /**
     * @brief Best-known client address for rate limiting and logging.
     *
     * Returns the socket peer unless that peer is a configured proxy, in
     * which case X-Real-IP (preferred, single-valued) or the last
     * X-Forwarded-For element is used.
     */
    std::string operator()(const drogon::HttpRequestPtr& req) const {
        const std::string peer = normalize_(req->getPeerAddr().toIp());
        if (!trusted_.count(peer)) return peer;   // direct client — trust the socket

        const std::string realIp = normalize_(trim_(req->getHeader("x-real-ip")));
        if (!realIp.empty()) return realIp;

        const std::string xff = req->getHeader("x-forwarded-for");
        if (!xff.empty()) {
            const auto pos  = xff.find_last_of(',');
            const auto last = trim_(pos == std::string::npos
                                    ? xff
                                    : xff.substr(pos + 1));
            if (!last.empty()) return normalize_(last);
        }
        // Proxy sent no forwarding headers — nothing better than the peer.
        return peer;
    }

private:
    void addTrusted_(const std::string& raw) {
        const std::string ip = normalize_(trim_(raw));
        if (!ip.empty()) trusted_.insert(ip);
    }

    static std::string trim_(const std::string& s) {
        const auto b = s.find_first_not_of(" \t");
        if (b == std::string::npos) return {};
        const auto e = s.find_last_not_of(" \t");
        return s.substr(b, e - b + 1);
    }

    /// Collapse IPv4-mapped IPv6 ("::ffff:127.0.0.1") so it matches "127.0.0.1".
    static std::string normalize_(const std::string& ip) {
        constexpr const char* kMapped = "::ffff:";
        if (ip.rfind(kMapped, 0) == 0) return ip.substr(std::strlen(kMapped));
        return ip;
    }

    std::unordered_set<std::string> trusted_;
};

} // namespace cerp::infrastructure
