#pragma once

/**
 * @file Groups.hpp
 * @brief Named constants for all built-in res_groups IDs.
 *
 * Use these constants instead of bare integers when checking group membership:
 *
 *   if (session.hasGroup(Groups::ACCOUNT_MANAGER)) { ... }
 *   if (session.hasAnyGroup({Groups::SALES_USER, Groups::SALES_MANAGER})) { ... }
 *   if (session.isAdmin) { ... }   // shortcut for hasGroup(BASE_ADMIN)
 *
 * Group IDs are seeded in AuthModule::seedGroups_() and must stay in sync with
 * the values in this file.
 */
namespace Groups {

    // ── Base ──────────────────────────────────────────────────────────────────
    /// Public / anonymous users (portal visitors, share=TRUE).
    /// No login required. Read-only access to public documents only.
    constexpr int BASE_PUBLIC   = 1;

    /// Standard internal employees.
    /// Can log in and access the modules they are assigned to.
    /// Every non-admin employee should be in this group.
    constexpr int BASE_INTERNAL = 2;

    /// System administrator.
    /// Full access to all modules, settings, and technical menus.
    /// Implies all lower-level permissions.
    constexpr int BASE_ADMIN    = 3;

    // ── Settings ─────────────────────────────────────────────────────────────
    /// Can access the Settings application and change company/system parameters.
    /// Does NOT grant full Administrator rights.
    constexpr int SETTINGS_CONFIGURATION = 4;

    // ── Accounting ────────────────────────────────────────────────────────────
    /// Billing — basic invoicing access.
    /// Can create, edit, and validate customer invoices and vendor bills.
    /// Cannot access journal entries, reconciliation, or accounting reports.
    constexpr int ACCOUNT_BILLING   = 5;

    /// Accountant — full accounting access.
    /// Can manage journal entries, bank reconciliation, closing periods,
    /// tax reports, and all accounting reports.
    /// Implies ACCOUNT_BILLING privileges.
    constexpr int ACCOUNT_MANAGER   = 6;

    // ── Sales ─────────────────────────────────────────────────────────────────
    /// Sales User — can manage their own quotations and sales orders.
    /// Cannot see other users' orders or access sales reporting.
    constexpr int SALES_USER    = 7;

    /// Sales Manager — full sales access.
    /// Can see all sales orders from all users, manage sales teams,
    /// apply manual discounts, and view sales reports.
    /// Implies SALES_USER privileges.
    constexpr int SALES_MANAGER = 8;

    // ── Purchase ─────────────────────────────────────────────────────────────
    /// Purchase User — can create and manage their own RFQs and purchase orders.
    constexpr int PURCHASE_USER    = 9;

    /// Purchase Manager — full purchase access.
    /// Can see all POs, manage vendor pricelists, approve large orders,
    /// and view purchase reports.
    /// Implies PURCHASE_USER privileges.
    constexpr int PURCHASE_MANAGER = 10;

    // ── Inventory ─────────────────────────────────────────────────────────────
    /// Inventory User — can process receipts, deliveries, and internal transfers.
    /// Cannot perform inventory adjustments or view stock valuation.
    constexpr int INVENTORY_USER    = 11;

    /// Inventory Manager — full warehouse access.
    /// Can perform inventory adjustments, view stock valuation, configure
    /// warehouses and locations, and manage reordering rules.
    /// Implies INVENTORY_USER privileges.
    constexpr int INVENTORY_MANAGER = 12;

    // ── Manufacturing ─────────────────────────────────────────────────────────
    /// Manufacturing User — can create and process manufacturing orders.
    constexpr int MRP_USER    = 13;

    /// Manufacturing Manager — full manufacturing access.
    /// Can manage Bills of Materials, work centres, routings, planning,
    /// and all manufacturing reports.
    /// Implies MRP_USER privileges.
    constexpr int MRP_MANAGER = 14;

    // ── Human Resources ───────────────────────────────────────────────────────
    /// HR Employee — can view and update their own employee profile.
    constexpr int HR_EMPLOYEE = 15;

    /// HR Manager — full HR access.
    /// Can manage all employees, contracts, departments, and HR reports.
    /// Implies HR_EMPLOYEE privileges.
    constexpr int HR_MANAGER  = 16;

} // namespace Groups
