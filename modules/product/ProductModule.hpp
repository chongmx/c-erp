#pragma once
// =============================================================
// modules/product/ProductModule.hpp
//
// Phase 8 — Product Catalog
//
// Models:  product.category  (table: product_category)
//          product.product   (table: product_product)
//
// Simplification: single-model — no template/variant split.
// Seeds:  3 product categories (All, Goods, Services)
// Menus:  Adds Products direct link (id=51) under the Products
//         app tile (id=50, created by UomModule) and Categories
//         leaf (id=54) under Configuration section (id=52).
// =============================================================
#include "IModule.hpp"
#include "Factories.hpp"
#include <memory>
#include <string>
#include <vector>

namespace odoo::modules::product {

// ================================================================
// MODULE
// ================================================================

class ProductModule : public core::IModule {
public:
    explicit ProductModule(core::ModelFactory&     models,
                           core::ServiceFactory&   services,
                           core::ViewModelFactory& viewModels,
                           core::ViewFactory&      views);

    static constexpr const char* staticModuleName() { return "product"; }
    std::string moduleName() const override;

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
    void seedCategories_();
    void seedMenus_();
};

} // namespace odoo::modules::product
