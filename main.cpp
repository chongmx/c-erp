#include "Container.hpp"
#include "BaseModule.hpp"
#include "modules/auth/AuthModule.hpp"
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

    auto cfg = odoo::infrastructure::AppConfig::fromEnv();

    g_container = std::make_shared<odoo::infrastructure::Container>(cfg);

    // Register modules in dependency order
    g_container->addModule<odoo::modules::base::BaseModule>();
    g_container->addModule<odoo::modules::auth::AuthModule>();

    try {
        std::cout << "[odoo-cpp] Booting modules...\n";
        g_container->boot();
        std::cout << "[odoo-cpp] Listening on http://"
                  << cfg.http.host << ":" << cfg.http.port << "\n";
        g_container->run();
    } catch (const std::exception& e) {
        std::cerr << "[odoo-cpp] Fatal: " << e.what() << "\n";
        return 1;
    }

    std::cout << "[odoo-cpp] Goodbye.\n";
    return 0;
}
