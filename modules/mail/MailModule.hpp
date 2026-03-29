#pragma once
// =============================================================
// modules/mail/MailModule.hpp
//
// Phase 27 — Chatter / Audit Log
//
// Provides:
//   mail.message  (table: mail_message)
//     - res_model, res_id  : polymorphic target
//     - author_id          : FK → res_users (nullable)
//     - body               : message text
//     - subtype            : 'note' | 'comment'
//     - date               : auto-set at insert
//
//   MailMessageViewModel  — search_read + create
//     search_read does a LEFT JOIN on res_users to return
//     author_name in every record.
// =============================================================
#include "IModule.hpp"
#include "Factories.hpp"
#include <memory>
#include <string>
#include <vector>

namespace odoo::modules::mail {

// ================================================================
// MailModule — IModule implementation
// ================================================================
class MailModule : public core::IModule {
public:
    static constexpr const char* staticModuleName() { return "mail"; }

    explicit MailModule(core::ModelFactory&     modelFactory,
                        core::ServiceFactory&   serviceFactory,
                        core::ViewModelFactory& viewModelFactory,
                        core::ViewFactory&      viewFactory);

    std::string              moduleName()   const override;
    std::string              version()      const override;
    std::vector<std::string> dependencies() const override;

    void registerModels()     override;
    void registerServices()   override;
    void registerViews()      override;
    void registerViewModels() override;
    void registerRoutes()     override;
    void initialize()         override;

private:
    core::ModelFactory&     models_;
    core::ServiceFactory&   services_;
    core::ViewModelFactory& viewModels_;
    core::ViewFactory&      views_;

    void ensureSchema_();
};

} // namespace odoo::modules::mail
