#include "Container.hpp"
#include "BaseModule.hpp"
#include "modules/auth/AuthModule.hpp"
#include "modules/ir/IrModule.hpp"
#include "modules/account/AccountModule.hpp"
#include "modules/uom/UomModule.hpp"
#include "modules/product/ProductModule.hpp"
#include "modules/sale/SaleModule.hpp"
#include "modules/purchase/PurchaseModule.hpp"
#include "modules/hr/HrModule.hpp"
#include "modules/auth/AuthSignupModule.hpp"
#include <csignal>
#include <iostream>
#include <memory>
#include <execinfo.h>
#include <cstdio>
#include <exception>

static std::shared_ptr<odoo::infrastructure::Container> g_container;

void handleSignal(int sig) {
    std::cout << "\n[odoo-cpp] Shutting down (signal " << sig << ")...\n";
    if (g_container) g_container->shutdown();
}

int main(int, char**) {
    std::set_terminate([]() {
        fprintf(stderr, "\n=== TERMINATE (pure virtual / unhandled exception) ===\n");
        void* frames[64];
        int n = backtrace(frames, 64);
        backtrace_symbols_fd(frames, n, fileno(stderr));
        fprintf(stderr, "=====================================================\n");
        std::abort();
    });

    std::signal(SIGINT,  handleSignal);
    std::signal(SIGTERM, handleSignal);

    auto cfg = odoo::infrastructure::AppConfig::fromEnv();

    g_container = std::make_shared<odoo::infrastructure::Container>(cfg);

    // Register modules in dependency order
    g_container->addModule<odoo::modules::base::BaseModule>();
    g_container->addModule<odoo::modules::auth::AuthModule>();
    g_container->addModule<odoo::modules::ir::IrModule>();
    g_container->addModule<odoo::modules::account::AccountModule>();
    g_container->addModule<odoo::modules::uom::UomModule>();
    g_container->addModule<odoo::modules::product::ProductModule>();
    g_container->addModule<odoo::modules::sale::SaleModule>();
    g_container->addModule<odoo::modules::purchase::PurchaseModule>();
    g_container->addModule<odoo::modules::hr::HrModule>();
    g_container->addModule<odoo::modules::auth::AuthSignupModule>();

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
