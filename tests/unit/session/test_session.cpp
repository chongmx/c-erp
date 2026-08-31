// =============================================================
// Standalone unit test for SessionManager (S-42 / S-43).
//
// Covers behaviour the HTTP path cannot exercise quickly: TTL expiry,
// the store cap, anonymous-first eviction, and rotate() semantics.
//
//   ./build/erp_tests Session
//
// P7: was scripts/test_sessionmanager.cpp with its own main(). Now part of
// the erp_tests target, so it builds with everything else and cannot rot
// unnoticed.
// =============================================================
#include "SessionManager.hpp"
#include "TestHarness.hpp"

#include <cctype>
#include <chrono>
#include <cstdio>
#include <thread>

using namespace cerp::infrastructure;

static void check(bool cond, const char* what) {
    erptest::check(cond, what);
}

ERP_TEST(Session, lifecycle) {
    // ---------------------------------------------------------
    std::puts("############ S-43 — expiry actually reclaims ############");
    {
        SessionManager sm{std::chrono::seconds{1}};
        for (int i = 0; i < 100; ++i) sm.create();
        check(sm.size() == 100, "100 sessions stored");

        // Not yet expired.
        check(sm.evictExpired() == 0, "nothing evicted before TTL");

        std::this_thread::sleep_for(std::chrono::milliseconds{1200});

        const std::size_t n = sm.evictExpired();
        std::printf("      evicted=%zu remaining=%zu\n", n, sm.size());
        check(n == 100,      "all expired sessions evicted");
        check(sm.size() == 0, "store is empty afterwards");
    }

    // ---------------------------------------------------------
    std::puts("############ S-43 — store is bounded ############");
    {
        SessionManager sm{std::chrono::hours{1}};
        sm.setMaxSessions(50);
        for (int i = 0; i < 500; ++i) sm.create();     // 10x the cap
        std::printf("      after 500 creates, size=%zu (cap 50)\n", sm.size());
        check(sm.size() <= 50, "size never exceeds the cap");
    }

    // ---------------------------------------------------------
    std::puts("############ S-43 — authenticated sessions survive a flood ############");
    {
        SessionManager sm{std::chrono::hours{1}};
        sm.setMaxSessions(20);

        const std::string authed = sm.create();
        sm.update(authed, [](Session& s) { s.uid = 42; s.login = "real.user"; });
        check(sm.get(authed).has_value(), "authenticated session present");

        // Flood with anonymous sessions well past the cap.
        for (int i = 0; i < 200; ++i) sm.create();

        auto still = sm.get(authed);
        std::printf("      size=%zu, authenticated session %s\n",
                    sm.size(), still.has_value() ? "SURVIVED" : "was evicted");
        check(still.has_value() && still->uid == 42,
              "anonymous flood does not evict a logged-in user");
    }

    // ---------------------------------------------------------
    std::puts("############ S-42 — rotate() re-keys and invalidates ############");
    {
        SessionManager sm{std::chrono::hours{1}};
        const std::string oldId = sm.create();
        sm.update(oldId, [](Session& s) {
            s.uid = 7; s.login = "victim"; s.isAdmin = true;
            s.groupIds = {1, 2, 3};
        });

        const std::string newId = sm.rotate(oldId);
        std::printf("      old=%.8s... new=%.8s...\n", oldId.c_str(), newId.c_str());

        check(!newId.empty(),            "rotate returned a new id");
        check(newId != oldId,            "new id differs from the old one");
        check(!sm.get(oldId).has_value(),"OLD id is no longer usable (fixation closed)");

        auto s = sm.get(newId);
        check(s.has_value(),                       "new id resolves");
        check(s && s->uid == 7,                    "uid carried over");
        check(s && s->login == "victim",           "login carried over");
        check(s && s->isAdmin,                     "isAdmin carried over");
        check(s && s->groupIds.size() == 3,        "groupIds carried over");
        check(s && s->sessionId == newId,          "sessionId field matches the new id");

        check(sm.rotate("nonexistent").empty(),    "rotating an unknown id returns empty");

        // An expired session must not be resurrectable by rotation.
        SessionManager sm2{std::chrono::seconds{1}};
        const std::string dying = sm2.create();
        std::this_thread::sleep_for(std::chrono::milliseconds{1200});
        check(sm2.rotate(dying).empty(), "rotating an expired id returns empty");
    }

    // ---------------------------------------------------------
    std::puts("############ id quality ############");
    {
        SessionManager sm{std::chrono::hours{1}};
        const std::string a = sm.create(), b = sm.create();
        check(a.size() == 32,              "id is 32 hex chars (128 bits)");
        check(a != b,                      "ids are distinct");
        bool hex = true;
        for (char c : a) if (!std::isxdigit(static_cast<unsigned char>(c))) hex = false;
        check(hex, "id is hex");
    }

}
