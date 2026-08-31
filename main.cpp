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
#include "modules/stock/StockModule.hpp"
#include "modules/mail/MailModule.hpp"
#include "modules/mrp/MrpModule.hpp"
#include "modules/project/ProjectModule.hpp"
#include "modules/help/HelpModule.hpp"
#include "modules/bom/BomModule.hpp"
#include "modules/report/ReportModule.hpp"
#include "modules/portal/PortalModule.hpp"
#include "modules/website/WebsiteModule.hpp"
#include "modules/rental/RentalModule.hpp"
#include <csignal>
#include <iostream>
#include <memory>
#include <execinfo.h>
#include <cstdio>
#include <exception>

static std::shared_ptr<cerp::infrastructure::Container> g_container;

void handleSignal(int sig) {
    std::cout << "\n[c-erp] Shutting down (signal " << sig << ")...\n";
    if (g_container) g_container->shutdown();
}

int main(int argc, char** argv) {
    // --provision / --migrate: boot (which provisions + migrates EVERY tenant
    // database — the cross-tenant migration runner in docs/072) then exit
    // without serving. Used by tools/provision_tenant.sh and deploy migrations.
    bool provisionOnly = false;
    std::string configPath = "config/system.cfg";
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--provision" || a == "--migrate") provisionOnly = true;
        else if (a == "--config" && i + 1 < argc)   configPath = argv[++i];
    }

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

    auto cfg = cerp::infrastructure::AppConfig::fromFileOrEnv(configPath);

    g_container = std::make_shared<cerp::infrastructure::Container>(cfg);

    // Register modules in dependency order
    g_container->addModule<cerp::modules::base::BaseModule>();
    g_container->addModule<cerp::modules::auth::AuthModule>();
    g_container->addModule<cerp::modules::mail::MailModule>();
    g_container->addModule<cerp::modules::ir::IrModule>();
    g_container->addModule<cerp::modules::account::AccountModule>();
    g_container->addModule<cerp::modules::uom::UomModule>();
    g_container->addModule<cerp::modules::product::ProductModule>();
    g_container->addModule<cerp::modules::sale::SaleModule>();
    g_container->addModule<cerp::modules::purchase::PurchaseModule>();
    g_container->addModule<cerp::modules::hr::HrModule>();
    g_container->addModule<cerp::modules::auth::AuthSignupModule>();
    g_container->addModule<cerp::modules::stock::StockModule>();
    g_container->addModule<cerp::modules::mrp::MrpModule>();
    g_container->addModule<cerp::modules::project::ProjectModule>();
    g_container->addModule<cerp::modules::help::HelpModule>();
    g_container->addModule<cerp::modules::bom::BomModule>();
    g_container->addModule<cerp::modules::report::ReportModule>();
    g_container->addModule<cerp::modules::portal::PortalModule>();
    g_container->addModule<cerp::modules::rental::RentalModule>();
    g_container->addModule<cerp::modules::website::WebsiteModule>();

    try {
        std::cout << "[c-erp] Booting modules...\n";
        g_container->boot();
        if (provisionOnly) {
            std::cout << "[c-erp] Provisioning + migration complete for all "
                         "tenants. Exiting (--provision).\n";
            return 0;
        }
        std::cout << "[c-erp] Listening on http://"
                  << cfg.http.host << ":" << cfg.http.port << "\n";
        g_container->run();
    } catch (const std::exception& e) {
        std::cerr << "[c-erp] Fatal: " << e.what() << "\n";
        return 1;
    }

    std::cout << "[c-erp] Goodbye.\n";
    return 0;
}
