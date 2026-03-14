#include "Container.hpp"
#include "BaseModule.hpp"
#include <iostream>
#include <csignal>
#include <memory>

std::shared_ptr<odoo::Container> g_container;

void handleSignal(int sig) {
    std::cout << "\n[odoo-cpp] Shutting down (signal " << sig << ")...\n";
    if (g_container) g_container->shutdown();
}

int main(int argc, char* argv[]) {
    std::signal(SIGINT,  handleSignal);
    std::signal(SIGTERM, handleSignal);

    // -------------------------------------------------------
    // 1. Configuration
    // -------------------------------------------------------
    odoo::infrastructure::DbConfig dbCfg;
    dbCfg.host     = std::getenv("DB_HOST")     ? std::getenv("DB_HOST")     : "localhost";
    dbCfg.dbName   = std::getenv("DB_NAME")     ? std::getenv("DB_NAME")     : "odoo";
    dbCfg.user     = std::getenv("DB_USER")     ? std::getenv("DB_USER")     : "odoo";
    dbCfg.password = std::getenv("DB_PASSWORD") ? std::getenv("DB_PASSWORD") : "";
    dbCfg.poolSize = 10;

    odoo::infrastructure::HttpConfig httpCfg;
    httpCfg.host    = "0.0.0.0";
    httpCfg.port    = 8069;
    httpCfg.threads = 4;

    // -------------------------------------------------------
    // 2. Build container
    // -------------------------------------------------------
    g_container = std::make_shared<odoo::Container>(dbCfg, httpCfg);

    // -------------------------------------------------------
    // 3. Register modules (add more here as they're ported)
    // -------------------------------------------------------
    g_container->moduleFactory->registerCreator("base", [&] {
        return std::make_shared<odoo::modules::base::BaseModule>(g_container);
    });
    // g_container->moduleFactory->registerCreator("account", [&] {
    //     return std::make_shared<odoo::modules::account::AccountModule>(g_container);
    // });

    // -------------------------------------------------------
    // 4. Boot + run
    // -------------------------------------------------------
    try {
        std::cout << "[odoo-cpp] Booting modules...\n";
        g_container->boot();
        std::cout << "[odoo-cpp] Listening on http://0.0.0.0:" << httpCfg.port << "\n";
        g_container->run();   // blocks until shutdown
    } catch (const std::exception& e) {
        std::cerr << "[odoo-cpp] Fatal: " << e.what() << "\n";
        return 1;
    }

    std::cout << "[odoo-cpp] Goodbye.\n";
    return 0;
}
