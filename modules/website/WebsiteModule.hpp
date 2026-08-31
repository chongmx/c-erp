#pragma once
// =============================================================
// modules/website/WebsiteModule.hpp — the CMS (docs/115)
//
// Adapted from the reference ERP's `website` addon: pages, a menu tree, publishing, SEO
// metadata, robots.txt and sitemap.xml.
//
// Two deliberate departures, both explained in docs/115:
//   * pages are served under /site/... because c-erp's ERP already owns "/";
//   * page content is typed BLOCKS rendered by the server, not author markup,
//     so the ordinary blocks have no XSS surface at all.
// =============================================================
#include "IModule.hpp"
#include "Factories.hpp"
#include <memory>
#include <string>
#include <vector>

namespace cerp::modules::website {

class WebsiteModule : public core::IModule {
public:
    static constexpr const char* staticModuleName() { return "website"; }

    explicit WebsiteModule(core::ModelFactory&, core::ServiceFactory&,
                           core::ViewModelFactory&, core::ViewFactory&);

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
    void seedContent_();
    void seedMenus_();
};

} // namespace cerp::modules::website
