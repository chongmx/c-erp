#pragma once
#include "../http/HttpService.hpp"   // pulls in drogon/drogon.h via HttpService
#include <drogon/WebSocketController.h>
#include <nlohmann/json.hpp>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>

namespace odoo::infrastructure {

// ============================================================
// WebSocketService
// ============================================================
/**
 * @brief Implements Odoo's bus.Bus pub/sub channel over Drogon WebSockets.
 *
 * Mounts a WebSocket endpoint at /websocket that the OWL frontend connects
 * to for real-time notifications (mail, discuss, longpolling).
 *
 * Odoo bus protocol:
 *   Client → Server:  { "event_name": "subscribe",   "data": { "channels": ["res.partner#42", ...] } }
 *   Client → Server:  { "event_name": "unsubscribe", "data": { "channels": [...] } }
 *   Server → Client:  { "type": "notification", "channel": "res.partner#42", "payload": { ... } }
 *   Server → Client:  { "type": "subscription_succeeded", "channels": [...] }
 *
 * Publishing from a service or ViewModel:
 * @code
 *   wsService->publish("res.partner#42", {
 *       {"type",       "partner_update"},
 *       {"partner_id", 42},
 *       {"name",       "Acme Corp"},
 *   });
 * @endcode
 *
 * Thread-safety:
 *   All public methods and Drogon callbacks are mutex-protected.
 *   Drogon invokes WebSocket callbacks on its own IO thread pool.
 */
class WebSocketService {
public:
    using ConnPtr  = drogon::WebSocketConnectionPtr;
    using Channel  = std::string;
    using Payload  = nlohmann::json;

    WebSocketService() = default;

    // Non-copyable
    WebSocketService(const WebSocketService&)            = delete;
    WebSocketService& operator=(const WebSocketService&) = delete;

    // ----------------------------------------------------------
    // Route registration
    // ----------------------------------------------------------

    /**
     * @brief Mount the WebSocket endpoint on the HTTP server.
     * Called once from Container::boot() after all routes are registered.
     *
     * Drogon WebSocket handlers are registered as lambdas on the app
     * directly — there is no separate controller class needed here because
     * we manage state inside WebSocketService itself.
     */
    void registerRoutes(HttpService& http) {
        // Capture a raw pointer — WebSocketService is owned by Container
        // (shared_ptr) which outlives the Drogon app loop.
        auto* self = this;

        http.app().registerHandler("/websocket",
            [self](const HttpRequestPtr&         req,
                   const HttpCallback&           cb,
                   const ConnPtr&                conn) {
                // Drogon calls this once on upgrade — ignored here,
                // onConnect_ handles the open event.
                (void)req; (void)cb;
                self->onConnect_(conn);
            },
            {drogon::Get});

        // Register the three WebSocket lifecycle hooks Drogon exposes
        // via the app's WebSocket connection factory.
        http.app().setWebSocketConnectionCallback(
            [self](const ConnPtr& conn) {
                self->onConnect_(conn);
            });

        http.app().setWebSocketMessageCallback(
            [self](const ConnPtr&    conn,
                   std::string&&     msg,
                   const drogon::WebSocketMessageType type) {
                if (type == drogon::WebSocketMessageType::Text)
                    self->onMessage_(conn, msg);
            });

        http.app().setWebSocketDisconnectionCallback(
            [self](const ConnPtr& conn) {
                self->onDisconnect_(conn);
            });
    }

    // ----------------------------------------------------------
    // Pub/sub API
    // ----------------------------------------------------------

    /**
     * @brief Send a payload to all connections subscribed to channel.
     *
     * @param channel  e.g. "res.partner#42", "mail.channel#7"
     * @param payload  JSON object; wrapped in the bus notification envelope.
     */
    void publish(const Channel& channel, const Payload& payload) {
        const std::string msg = nlohmann::json{
            {"type",    "notification"},
            {"channel", channel},
            {"payload", payload},
        }.dump();

        std::scoped_lock lock{mutex_};
        auto it = subscribers_.find(channel);
        if (it == subscribers_.end()) return;

        for (const auto& conn : it->second) {
            if (conn && conn->connected()) {
                try { conn->send(msg); } catch (...) {}
            }
        }
    }

    /**
     * @brief Send a payload to every connected client regardless of channel.
     * Use sparingly — prefer channel-scoped publish().
     */
    void broadcast(const Payload& payload) {
        const std::string msg = payload.dump();
        std::scoped_lock lock{mutex_};
        for (const auto& conn : allConnections_) {
            if (conn && conn->connected()) {
                try { conn->send(msg); } catch (...) {}
            }
        }
    }

    /** @brief Number of currently open WebSocket connections. */
    std::size_t connectionCount() const {
        std::scoped_lock lock{mutex_};
        return allConnections_.size();
    }

    nlohmann::json healthInfo() const {
        std::scoped_lock lock{mutex_};
        return {
            {"connections", static_cast<int>(allConnections_.size())},
            {"channels",    static_cast<int>(subscribers_.size())},
        };
    }

private:
    // ----------------------------------------------------------
    // Drogon lifecycle callbacks
    // ----------------------------------------------------------

    void onConnect_(const ConnPtr& conn) {
        std::scoped_lock lock{mutex_};
        allConnections_.insert(conn);
    }

    void onDisconnect_(const ConnPtr& conn) {
        std::scoped_lock lock{mutex_};
        allConnections_.erase(conn);

        // Remove from every channel subscription
        auto chanIt = connChannels_.find(conn);
        if (chanIt != connChannels_.end()) {
            for (const auto& ch : chanIt->second) {
                auto subIt = subscribers_.find(ch);
                if (subIt != subscribers_.end())
                    subIt->second.erase(conn);
            }
            connChannels_.erase(chanIt);
        }
    }

    void onMessage_(const ConnPtr& conn, const std::string& data) {
        try {
            const auto msg   = nlohmann::json::parse(data);
            const auto event = msg.value("event_name", std::string{});

            if (event == "subscribe")
                handleSubscribe_(conn, msg.value("data", nlohmann::json::object()));
            else if (event == "unsubscribe")
                handleUnsubscribe_(conn, msg.value("data", nlohmann::json::object()));
            // Other events (heartbeats, custom) are silently ignored.

        } catch (const nlohmann::json::exception&) {
            // Malformed JSON — ignore without closing the connection.
        }
    }

    // ----------------------------------------------------------
    // Subscription management
    // ----------------------------------------------------------

    void handleSubscribe_(const ConnPtr& conn, const nlohmann::json& data) {
        if (!data.contains("channels") || !data["channels"].is_array()) return;

        std::scoped_lock lock{mutex_};
        for (const auto& ch : data["channels"]) {
            if (!ch.is_string()) continue;
            const Channel channel = ch.get<std::string>();
            subscribers_[channel].insert(conn);
            connChannels_[conn].insert(channel);
        }

        if (conn->connected()) {
            conn->send(nlohmann::json{
                {"type",     "subscription_succeeded"},
                {"channels", data["channels"]},
            }.dump());
        }
    }

    void handleUnsubscribe_(const ConnPtr& conn, const nlohmann::json& data) {
        if (!data.contains("channels") || !data["channels"].is_array()) return;

        std::scoped_lock lock{mutex_};
        for (const auto& ch : data["channels"]) {
            if (!ch.is_string()) continue;
            const Channel channel = ch.get<std::string>();

            auto subIt = subscribers_.find(channel);
            if (subIt != subscribers_.end()) subIt->second.erase(conn);

            auto chanIt = connChannels_.find(conn);
            if (chanIt != connChannels_.end()) chanIt->second.erase(channel);
        }
    }

    // ----------------------------------------------------------
    // State
    // ----------------------------------------------------------
    mutable std::mutex mutex_;

    // All open connections
    std::unordered_set<ConnPtr> allConnections_;

    // channel → subscribed connections
    std::unordered_map<Channel, std::unordered_set<ConnPtr>> subscribers_;

    // connection → subscribed channels  (for O(channels) cleanup on disconnect)
    std::unordered_map<ConnPtr, std::unordered_set<Channel>> connChannels_;
};

} // namespace odoo::infrastructure