#pragma once
/**
 * HttpClient.hpp — the one place this codebase makes an OUTBOUND request.
 *
 * Built on drogon's client rather than libcurl: drogon is already vendored and
 * linked, and its client is async on a trantor loop, so nothing new has to be
 * pulled in. TLS comes from the same OpenSSL drogon itself is built against —
 * which means a build where drogon did not find OpenSSL cannot do HTTPS at all,
 * silently. `tlsAvailable()` below exists to make that visible rather than
 * letting it surface as a connection failure nobody can explain.
 *
 * WHY THE DEDICATED LOOP
 * ---------------------
 * A request handler already runs ON a drogon event loop. Calling the blocking
 * form of sendRequest from that thread waits for a loop that is waiting for
 * you — a deadlock, not a slow call. So every request here runs on its own
 * short-lived EventLoopThread and the caller blocks on that instead. It costs a
 * thread per call, which is the right trade for something done a few times a
 * minute by an operator, and it cannot stall the server.
 *
 * Header-only, matching DbBackup.hpp and JsonRpcDispatcher.hpp in this
 * directory — no CMake change needed to add it.
 */
#include <drogon/HttpClient.h>
#include <drogon/HttpRequest.h>
#include <drogon/HttpResponse.h>
#include <trantor/net/EventLoopThread.h>

#include <string>
#include <utility>
#include <vector>

namespace odoo::infrastructure {

struct HttpResult {
    bool        ok = false;      ///< a response arrived AND its status was 2xx
    int         status = 0;      ///< HTTP status, 0 if nothing came back
    std::string body;            ///< response body, empty on transport failure
    std::string error;           ///< why it failed, safe to show a user
};

class HttpClient {
public:
    /**
     * Was drogon built with OpenSSL?
     *
     * Checked at the call site rather than assumed. Without it, an https URL
     * fails at connect time with a message that looks like a network problem
     * and is actually a build problem.
     */
    static bool tlsAvailable() {
#ifdef OpenSSL_FOUND
        return true;
#elif defined(USE_OPENSSL)
        return true;
#else
        // drogon's generated config.h defines OpenSSL_FOUND when it has TLS.
        // If neither macro reached us, probe instead of guessing: creating an
        // https client throws on a build without TLS.
        try {
            auto c = drogon::HttpClient::newHttpClient("https://127.0.0.1:1");
            return c != nullptr;
        } catch (...) {
            return false;
        }
#endif
    }

    /**
     * POST a JSON body and wait for the answer.
     *
     * @param baseUrl  scheme + host, e.g. "https://api.anthropic.com"
     * @param path     "/v1/messages"
     * @param body     the JSON payload
     * @param headers  extra headers; the caller owns anything secret in here
     * @param timeout  seconds, hard ceiling for the whole exchange
     *
     * Never logs the headers. One of them is an API key, and a debug line that
     * prints the request is a debug line that prints the credential.
     */
    static HttpResult postJson(const std::string& baseUrl,
                               const std::string& path,
                               const std::string& body,
                               const std::vector<std::pair<std::string, std::string>>& headers,
                               double timeout = 30.0)
    {
        HttpResult out;
        try {
            // Its own loop: see the note at the top of this file.
            trantor::EventLoopThread loopThread;
            loopThread.run();

            auto client = drogon::HttpClient::newHttpClient(baseUrl, loopThread.getLoop());
            if (!client) { out.error = "could not create a client for " + baseUrl; return out; }

            auto req = drogon::HttpRequest::newHttpRequest();
            req->setMethod(drogon::Post);
            req->setPath(path);
            req->setContentTypeCode(drogon::CT_APPLICATION_JSON);
            req->setBody(body);
            for (const auto& [k, v] : headers) req->addHeader(k, v);

            auto [result, resp] = client->sendRequest(req, timeout);

            if (result != drogon::ReqResult::Ok || !resp) {
                out.error = describe_(result);
                return out;
            }
            out.status = static_cast<int>(resp->statusCode());
            out.body.assign(resp->body().data(), resp->body().size());
            out.ok = (out.status >= 200 && out.status < 300);
            if (!out.ok && out.error.empty())
                out.error = "the service replied " + std::to_string(out.status);
            return out;
        } catch (const std::exception& e) {
            // The message is ours, not the exception's: an exception here can
            // carry the URL and, on some paths, the request.
            out.error = std::string("the request could not be made (") + e.what() + ")";
            return out;
        }
    }

private:
    static std::string describe_(drogon::ReqResult r) {
        using R = drogon::ReqResult;
        switch (r) {
            case R::Ok:               return "";
            case R::BadResponse:      return "the service sent a response this client could not read";
            case R::NetworkFailure:   return "could not reach the service (network failure)";
            case R::BadServerAddress: return "the address could not be resolved";
            case R::Timeout:          return "the service did not answer in time";
            case R::HandshakeError:   return "the TLS handshake failed — is this build's drogon compiled with OpenSSL?";
            case R::InvalidCertificate: return "the service presented a certificate this client rejected";
            default:                  return "the request failed";
        }
    }
};

} // namespace odoo::infrastructure
