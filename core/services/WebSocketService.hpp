#pragma once
#include "HttpService.hpp"
#include <drogon/WebSocketController.h>
#include <nlohmann/json.hpp>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>

namespace odoo::infrastructure {

class WebSocketService;

// ================================================================
// BusController
//
// Drogon constraints that drive this design:
//   1. WebSocketController<T, AutoCreation=true>  (the default) requires
//      T to be default-constructible and auto-registers it. We cannot
//      inject a reference into the constructor that way.
//   2. WebSocketController<T, AutoCreation=false> lets us construct T
//      ourselves and call registerController() — BUT registerController()
//      then asserts !isAutoCreation, which is false for AutoCreation=false.
//      Contradiction.
//   3. handleNewMessage signature MUST be:
//        void handleNewMessage(const WebSocketConnectionPtr&,
//                              std::string&&,
//                              const WebSocketMessageType&)   ← const ref
//      NOT value type for the last param.
//
// Solution: use AutoCreation=true (default), make BusController default-
// constructible, and reach WebSocketService via a static pointer set once
// at boot time. This is safe because WebSocketService is owned by Container
// which lives for the entire process lifetime.
// ================================================================
class BusController
    : public drogon::WebSocketController<BusController>   // AutoCreation=true
{
public:
    // Required by Drogon's DrObject machinery
    BusController() = default;

    WS_PATH_LIST_BEGIN
        WS_PATH_ADD("/websocket");
    WS_PATH_LIST_END

    // Called once from WebSocketService::registerRoutes() before the
    // server starts accepting connections.
    static void setService(WebSocketService* svc) { service_ = svc; }

    // ---- Drogon callbacks (correct signatures) ----
    void handleNewMessage(const drogon::WebSocketConnectionPtr& conn,
                          std::string&&                          msg,
                          const drogon::WebSocketMessageType&    type) override;

    void handleNewConnection(const drogon::HttpRequestPtr&        req,
                             const drogon::WebSocketConnectionPtr& conn) override;

    void handleConnectionClosed(const drogon::WebSocketConnectionPtr& conn) override;

private:
    static WebSocketService* service_;
};

// Static member definition (in header — ODR-safe with inline)
inline WebSocketService* BusController::service_ = nullptr;


// ================================================================
// WebSocketService — owns all pub/sub state
// ================================================================
class WebSocketService {
public:
    using ConnPtr = drogon::WebSocketConnectionPtr;
    using Channel = std::string;
    using Payload = nlohmann::json;

    WebSocketService() = default;
    WebSocketService(const WebSocketService&)            = delete;
    WebSocketService& operator=(const WebSocketService&) = delete;

    // ----------------------------------------------------------
    // Route registration — call before http.start()
    // ----------------------------------------------------------
    void registerRoutes(HttpService& /*http*/) {
        // Point the auto-created controller at this service instance.
        // BusController is registered automatically by Drogon via the
        // WS_PATH_LIST macros; we only need to inject the service pointer.
        BusController::setService(this);
    }

    // ----------------------------------------------------------
    // Pub/sub API
    // ----------------------------------------------------------
    void publish(const Channel& channel, const Payload& payload) {
        const std::string msg = nlohmann::json{
            {"type",    "notification"},
            {"channel", channel},
            {"payload", payload},
        }.dump();

        std::scoped_lock lock{mutex_};
        auto it = subscribers_.find(channel);
        if (it == subscribers_.end()) return;
        for (const auto& conn : it->second) sendSafe_(conn, msg);
    }

    void broadcast(const Payload& payload) {
        const std::string msg = payload.dump();
        std::scoped_lock lock{mutex_};
        for (const auto& conn : allConnections_) sendSafe_(conn, msg);
    }

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

    // ----------------------------------------------------------
    // Called by BusController (must be public)
    // ----------------------------------------------------------
    void onConnect(const ConnPtr& conn) {
        std::scoped_lock lock{mutex_};
        allConnections_.insert(conn);
    }

    void onDisconnect(const ConnPtr& conn) {
        std::scoped_lock lock{mutex_};
        allConnections_.erase(conn);
        auto it = connChannels_.find(conn);
        if (it != connChannels_.end()) {
            for (const auto& ch : it->second) {
                auto s = subscribers_.find(ch);
                if (s != subscribers_.end()) s->second.erase(conn);
            }
            connChannels_.erase(it);
        }
    }

    void onMessage(const ConnPtr& conn, const std::string& data) {
        try {
            const auto msg   = nlohmann::json::parse(data);
            const auto event = msg.value("event_name", std::string{});
            if (event == "subscribe")
                handleSubscribe_(conn, msg.value("data", nlohmann::json::object()));
            else if (event == "unsubscribe")
                handleUnsubscribe_(conn, msg.value("data", nlohmann::json::object()));
        } catch (const nlohmann::json::exception&) {}
    }

private:
    void handleSubscribe_(const ConnPtr& conn, const nlohmann::json& data) {
        if (!data.contains("channels") || !data["channels"].is_array()) return;
        std::scoped_lock lock{mutex_};
        for (const auto& ch : data["channels"]) {
            if (!ch.is_string()) continue;
            const Channel channel = ch.get<std::string>();
            subscribers_[channel].insert(conn);
            connChannels_[conn].insert(channel);
        }
        sendSafe_(conn, nlohmann::json{
            {"type",     "subscription_succeeded"},
            {"channels", data["channels"]},
        }.dump());
    }

    void handleUnsubscribe_(const ConnPtr& conn, const nlohmann::json& data) {
        if (!data.contains("channels") || !data["channels"].is_array()) return;
        std::scoped_lock lock{mutex_};
        for (const auto& ch : data["channels"]) {
            if (!ch.is_string()) continue;
            const Channel channel = ch.get<std::string>();
            auto s = subscribers_.find(channel);
            if (s != subscribers_.end()) s->second.erase(conn);
            auto c = connChannels_.find(conn);
            if (c != connChannels_.end()) c->second.erase(channel);
        }
    }

    static void sendSafe_(const ConnPtr& conn, const std::string& msg) {
        if (conn && conn->connected())
            try { conn->send(msg); } catch (...) {}
    }

    mutable std::mutex mutex_;
    std::unordered_set<ConnPtr>                              allConnections_;
    std::unordered_map<Channel, std::unordered_set<ConnPtr>> subscribers_;
    std::unordered_map<ConnPtr, std::unordered_set<Channel>> connChannels_;
};


// ================================================================
// BusController method definitions (after WebSocketService is complete)
// ================================================================
inline void BusController::handleNewConnection(
    const drogon::HttpRequestPtr&,
    const drogon::WebSocketConnectionPtr& conn)
{
    if (service_) service_->onConnect(conn);
}

inline void BusController::handleConnectionClosed(
    const drogon::WebSocketConnectionPtr& conn)
{
    if (service_) service_->onDisconnect(conn);
}

inline void BusController::handleNewMessage(
    const drogon::WebSocketConnectionPtr& conn,
    std::string&&                          msg,
    const drogon::WebSocketMessageType&    type)
{
    if (service_ && type == drogon::WebSocketMessageType::Text)
        service_->onMessage(conn, msg);
}

} // namespace odoo::infrastructure