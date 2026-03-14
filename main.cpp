#include "Container.hpp"
#include "BaseModule.hpp"
#include <csignal>
#include <iostream>
#include <memory>

static std::shared_ptr<odoo::infrastructure::Container> g_container;

void handleSignal(int sig) {
    std::cout << "\n[odoo-cpp] Shutting down (signal " << sig << ")...\n";
    if (g_container) g_container->shutdown();
}

int main(int, char**) {
    std::signal(SIGINT,  handleSignal);
    std::signal(SIGTERM, handleSignal);

    // ------------------------------------------------------------------
    // 1. Configuration — use AppConfig::fromEnv() or fill fields manually
    // ------------------------------------------------------------------
    auto cfg = odoo::infrastructure::AppConfig::fromEnv();

    // Override specific fields if needed (AppConfig::fromEnv covers env vars):
    // cfg.db.host     = "localhost";
    // cfg.db.name     = "odoo";
    // cfg.http.port   = 8069;

    // ------------------------------------------------------------------
    // 2. Build container (infrastructure + empty factories)
    // ------------------------------------------------------------------
    g_container = std::make_shared<odoo::infrastructure::Container>(cfg);

    // ------------------------------------------------------------------
    // 3. Register modules
    //    addModule<T>() is the preferred API — it captures factory refs
    //    and constructs the module during bootAll().
    //    Uncomment additional modules as they are ported.
    // ------------------------------------------------------------------
    g_container->addModule<odoo::modules::base::BaseModule>();
    // g_container->addModule<odoo::modules::account::AccountModule>();

    // ------------------------------------------------------------------
    // 4. Boot + run
    // ------------------------------------------------------------------
    try {
        std::cout << "[odoo-cpp] Booting modules...\n";
        g_container->boot();
        std::cout << "[odoo-cpp] Listening on http://"
                  << cfg.http.host << ":" << cfg.http.port << "\n";
        g_container->run();   // blocks until SIGINT / SIGTERM
    } catch (const std::exception& e) {
        std::cerr << "[odoo-cpp] Fatal: " << e.what() << "\n";
        return 1;
    }

    std::cout << "[odoo-cpp] Goodbye.\n";
    return 0;
}