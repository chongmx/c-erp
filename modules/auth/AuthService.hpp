#pragma once
#include "BaseService.hpp"
#include "ResUsers.hpp"
#include "SessionManager.hpp"
#include <nlohmann/json.hpp>
#include <openssl/evp.h>
#include <openssl/rand.h>
#include <memory>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

// base64 encode/decode (header-only via OpenSSL BIO)
#include <openssl/bio.h>
#include <openssl/buffer.h>

namespace odoo::modules::auth {

// ================================================================
// AuthService
// ================================================================
/**
 * @brief Authentication service implementing Odoo's PBKDF2-SHA512
 *        password scheme.
 *
 * Odoo 19 stores passwords in the format used by passlib / Django:
 *   $pbkdf2-sha512$<rounds>$<base64-salt>$<base64-hash>
 *
 * Example:
 *   $pbkdf2-sha512$600000$abc123...$xyz789...
 *
 * The authenticate() method:
 *   1. Looks up res.users by login in the database
 *   2. Parses the stored hash
 *   3. Re-derives PBKDF2-SHA512 with the same salt and rounds
 *   4. Compares in constant time (no timing attacks)
 *   5. On success, writes uid + context into the session
 *
 * hashPassword() is used when creating or changing passwords.
 */
class AuthService : public core::BaseService {
public:
    explicit AuthService(std::shared_ptr<infrastructure::DbConnection> db)
        : core::BaseService(std::move(db)) {}

    std::string serviceName() const override { return "auth"; }

    // ----------------------------------------------------------
    // authenticate — verify login/password, update session
    // ----------------------------------------------------------

    /**
     * @brief Authenticate a user and populate the session.
     *
     * @param login     Username / email.
     * @param password  Plaintext password (never stored).
     * @param dbName    Database name injected into session context.
     * @param session   Session to populate on success (uid, login, context).
     * @returns true on success, false on wrong credentials.
     * @throws std::runtime_error on database error.
     */
    bool authenticate(const std::string&       login,
                      const std::string&       password,
                      const std::string&       dbName,
                      infrastructure::Session& session)
    {
        // 1. Fetch the user record by login
        auto conn = db_->acquire();
        pqxx::work txn{conn.get()};

        auto res = txn.exec(
            "SELECT id, password, active, lang, tz, company_id, partner_id "
            "FROM res_users WHERE login=$1 LIMIT 1",
            pqxx::params{login});

        if (res.empty()) return false;   // unknown login

        const auto& row = res[0];

        // 2. Reject inactive users
        const std::string activeStr = row["active"].c_str();
        if (activeStr == "f" || activeStr == "false") return false;

        // 3. Verify password
        const std::string storedHash = row["password"].c_str();
        if (!verifyPassword_(password, storedHash)) return false;

        // 4. Populate session
        session.uid   = row["id"].as<int>();
        session.login = login;
        session.db    = dbName;
        session.context = {
            {"uid",  session.uid},
            {"lang", row["lang"].is_null() ? "en_US" : std::string(row["lang"].c_str())},
            {"tz",   row["tz"].is_null()   ? "UTC"   : std::string(row["tz"].c_str())},
        };

        return true;
    }

    // ----------------------------------------------------------
    // hashPassword — create a new PBKDF2-SHA512 hash
    // ----------------------------------------------------------

    /**
     * @brief Hash a plaintext password using PBKDF2-SHA512.
     *
     * Produces a string in Odoo's passlib format:
     *   $pbkdf2-sha512$<rounds>$<base64-salt>$<base64-hash>
     *
     * @param password  Plaintext password.
     * @param rounds    Iteration count (Odoo 19 default: 600000).
     * @returns         Hash string ready for storage in res_users.password.
     */
    static std::string hashPassword(const std::string& password,
                                    int rounds = 600000)
    {
        // Generate 16 bytes of random salt
        unsigned char salt[16];
        if (RAND_bytes(salt, sizeof(salt)) != 1)
            throw std::runtime_error("AuthService: failed to generate random salt");

        // Derive PBKDF2-SHA512
        unsigned char hash[64];
        if (PKCS5_PBKDF2_HMAC(
                password.c_str(), static_cast<int>(password.size()),
                salt, sizeof(salt),
                rounds,
                EVP_sha512(),
                sizeof(hash), hash) != 1)
        {
            throw std::runtime_error("AuthService: PBKDF2 derivation failed");
        }

        return "$pbkdf2-sha512$"
             + std::to_string(rounds) + "$"
             + base64Encode_(salt, sizeof(salt)) + "$"
             + base64Encode_(hash,  sizeof(hash));
    }

    // ----------------------------------------------------------
    // Health check
    // ----------------------------------------------------------
    nlohmann::json healthCheck() const override {
        try {
            auto conn = db_->acquire();
            pqxx::work txn{conn.get()};
            txn.exec("SELECT 1 FROM res_users LIMIT 1");
            return {{"service", "auth"}, {"status", "ok"}};
        } catch (...) {
            return {{"service", "auth"}, {"status", "degraded"}};
        }
    }

private:
    // ----------------------------------------------------------
    // verifyPassword_ — parse stored hash and re-derive
    // ----------------------------------------------------------

    /**
     * Parse Odoo's passlib PBKDF2-SHA512 hash string and verify.
     *
     * Format: $pbkdf2-sha512$<rounds>$<base64-salt>$<base64-hash>
     *
     * Uses constant-time comparison to prevent timing attacks.
     */
    static bool verifyPassword_(const std::string& plaintext,
                                 const std::string& stored)
    {
        // Must start with the scheme identifier
        const std::string prefix = "$pbkdf2-sha512$";
        if (stored.substr(0, prefix.size()) != prefix) return false;

        // Split: rounds $ salt $ hash
        const auto body = stored.substr(prefix.size());
        const auto p1   = body.find('$');
        if (p1 == std::string::npos) return false;
        const auto p2   = body.find('$', p1 + 1);
        if (p2 == std::string::npos) return false;

        const int rounds = std::stoi(body.substr(0, p1));
        const auto saltB64 = body.substr(p1 + 1, p2 - p1 - 1);
        const auto hashB64 = body.substr(p2 + 1);

        const auto salt     = base64Decode_(saltB64);
        const auto expected = base64Decode_(hashB64);

        // Re-derive
        std::vector<unsigned char> derived(expected.size());
        if (PKCS5_PBKDF2_HMAC(
                plaintext.c_str(), static_cast<int>(plaintext.size()),
                reinterpret_cast<const unsigned char*>(salt.data()),
                static_cast<int>(salt.size()),
                rounds,
                EVP_sha512(),
                static_cast<int>(derived.size()),
                derived.data()) != 1)
        {
            return false;
        }

        // Constant-time comparison
        if (derived.size() != expected.size()) return false;
        unsigned char diff = 0;
        for (std::size_t i = 0; i < derived.size(); ++i)
            diff |= derived[i] ^ static_cast<unsigned char>(expected[i]);
        return diff == 0;
    }

    // ----------------------------------------------------------
    // Base64 helpers (passlib uses standard base64, no padding strip)
    // ----------------------------------------------------------

    static std::string base64Encode_(const unsigned char* data, std::size_t len) {
        // Use OpenSSL BIO chain for base64 encoding
        BIO* b64  = BIO_new(BIO_f_base64());
        BIO* bmem = BIO_new(BIO_s_mem());
        b64 = BIO_push(b64, bmem);
        BIO_set_flags(b64, BIO_FLAGS_BASE64_NO_NL);
        BIO_write(b64, data, static_cast<int>(len));
        BIO_flush(b64);

        BUF_MEM* bptr = nullptr;
        BIO_get_mem_ptr(b64, &bptr);
        std::string result(bptr->data, bptr->length);
        BIO_free_all(b64);
        return result;
    }

    static std::vector<char> base64Decode_(const std::string& encoded) {
        BIO* b64  = BIO_new(BIO_f_base64());
        BIO* bmem = BIO_new_mem_buf(encoded.data(), static_cast<int>(encoded.size()));
        bmem = BIO_push(b64, bmem);
        BIO_set_flags(bmem, BIO_FLAGS_BASE64_NO_NL);

        std::vector<char> buf(encoded.size());
        const int len = BIO_read(bmem, buf.data(), static_cast<int>(buf.size()));
        BIO_free_all(bmem);

        if (len < 0) return {};
        buf.resize(static_cast<std::size_t>(len));
        return buf;
    }
};

} // namespace odoo::modules::auth
