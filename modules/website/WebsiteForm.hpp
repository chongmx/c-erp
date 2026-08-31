#pragma once
// =============================================================
// modules/website/WebsiteForm.hpp — the form builder (docs/116 A1)
//
// the reference ERP's `website_form`, adapted. the reference ERP lets a form post into ANY model field
// it has been told to allow, which is powerful and is also why it ships with a
// per-model allow-list and a history of needing it.
//
// c-erp's version is narrower on purpose: a submission is stored as its own
// record, and routing it anywhere else (a task, later a lead) is an explicit,
// server-side mapping. A public form can therefore never write to an arbitrary
// column, because there is no code path that would let it.
//
// The security properties, all enforced server-side:
//
//   * FIELD ALLOW-LIST per form. Only fields declared on the form are read;
//     anything else in the body is discarded, not stored "just in case".
//   * Values are LENGTH-CAPPED and type-checked; required means required.
//   * A HONEYPOT field that must stay empty — the cheapest bot filter there is.
//   * RATE LIMITED per IP, because this is an unauthenticated write endpoint.
//   * Submissions are stored as data and escaped when displayed, so a payload
//     typed into a form cannot execute in the back office later.
// =============================================================
#include <memory>
#include <string>

namespace cerp::core { class ModelFactory; class ViewModelFactory; }
namespace cerp::infrastructure { class DbConnection; }
namespace pqxx { class transaction_base; }

namespace cerp::modules::website {

class WebsiteForm {
public:
    static void ensureSchema(pqxx::transaction_base& txn);
    static void seedMenus(pqxx::transaction_base& txn);

    static void registerModels(core::ModelFactory& models,
                               std::shared_ptr<infrastructure::DbConnection> db);
    static void registerViewModels(core::ViewModelFactory& viewModels,
                                   std::shared_ptr<infrastructure::DbConnection> db);

    /// POST /site/form/{slug}
    static void registerRoutes(std::shared_ptr<infrastructure::DbConnection> db,
                               bool devMode,
                               const std::string& trustedProxies);

    /// Render a published form as HTML, for the `form` page block.
    /// Returns "" when the slug names no active form.
    static std::string renderForm(pqxx::transaction_base& txn,
                                  const std::string& slug);

    /// The field types a form may declare. Anything else is refused on save,
    /// so the renderer and the validator never meet a type they do not know.
    static bool isValidFieldType(const std::string& t);
};

} // namespace cerp::modules::website
