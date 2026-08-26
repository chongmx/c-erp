#pragma once
// =============================================================
// modules/help/HelpModule.hpp
//
// docs/101 — the in-app Help Centre.
//
// Help lives in the database, not in a bundle of static pages, for one reason
// that shapes the whole design: an AI assistant is meant to answer from it
// later. That means the content has to be addressable in pieces — a row per
// article, with a stable slug, a title, keywords and a markdown body — so it
// can be retrieved, ranked and cited. A folder of HTML could be rendered but
// not searched, and could never be cited back to the user as "see this page".
//
// Models:
//   help.article (help_article) — one article, or one section when is_section
//
// ViewModels:
//   HelpArticleViewModel — CRUD + books + tree + article + search
//
// Menus:  id=400 Help app tile, 401 Help Centre
// Actions: id=114 Help Centre (help.center)
// =============================================================
#include "IModule.hpp"
#include "Factories.hpp"
#include <memory>
#include <string>
#include <vector>

namespace odoo::modules::help {

class HelpModule : public core::IModule {
public:
    explicit HelpModule(core::ModelFactory&     models,
                        core::ServiceFactory&   services,
                        core::ViewModelFactory& viewModels,
                        core::ViewFactory&      views);

    static constexpr const char* staticModuleName() { return "help"; }
    std::string              moduleName()   const override;
    std::string              version()      const override;
    std::vector<std::string> dependencies() const override;

    void registerModels()     override;
    void registerServices()   override;
    void registerViewModels() override;
    void registerViews()      override;
    void registerRoutes()     override;
    void initialize()         override;

private:
    core::ModelFactory&     models_;
    core::ServiceFactory&   services_;
    core::ViewModelFactory& viewModels_;
    core::ViewFactory&      views_;

    void ensureSchema_();
    void seedContent_();   ///< the shipped help articles
    void seedMenus_();
};

} // namespace odoo::modules::help
