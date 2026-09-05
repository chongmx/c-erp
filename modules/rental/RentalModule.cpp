// =============================================================
// modules/rental/RentalModule.cpp
//
// PERF-E: every inner class and every method body lives here, so the
// heavy headers (pqxx, BaseModel, nlohmann) stay out of the .hpp and a
// change here recompiles one TU rather than the codebase.
// =============================================================
#include "RentalModule.hpp"
#include "RentalMigrations.hpp"
#include "RentalEvents.hpp"
#include "RentalBilling.hpp"
#include "RentalExpenses.hpp"
#include "RentalForecast.hpp"
#include "RentalCalendar.hpp"
#include "RentalDashboard.hpp"
#include "RentalDemo.hpp"
#include "SessionManager.hpp"

#include "BaseModel.hpp"
#include "BaseView.hpp"
#include "GenericViewModel.hpp"
#include "DbConnection.hpp"
#include "DecimalPrecision.hpp"
#include "MigrationRunner.hpp"

#include <drogon/drogon.h>
#include <nlohmann/json.hpp>
#include <pqxx/pqxx>
#include <memory>
#include <string>
#include <vector>
#include <set>

namespace cerp::modules::rental {

using namespace cerp::infrastructure;
using namespace cerp::core;

// ================================================================
// helpers
// ================================================================

/**
 * Normalise a many2one value to a plain id.
 *
 * The client may send either a bare integer or the reference ERP's `[id, "display"]`
 * pair depending on whether the value came from a form field or a
 * search_read result. Anything else — notably `false`, which is how the
 * client clears a many2one — becomes 0, meaning "not set".
 *
 * Same helper as AccountModule's; it is file-local there too rather than
 * living on BaseModel, so it is duplicated rather than reached into.
 */
static int m2oToId_(const nlohmann::json& v) {
    if (v.is_number_integer()) return v.get<int>();
    if (v.is_array() && !v.empty() && v[0].is_number_integer())
        return v[0].get<int>();
    return 0;
}

// ================================================================
// 1. MODELS
//
// Every money column is BIGINT micro-units (P2) and must be listed in
// markScaled(), or BaseModel will hand the raw micro-unit integer to the
// UI as if it were a major-unit amount.
//
// Every field the UI writes must appear in registerFields(). A column
// that exists in the DB but not here is silently DISCARDED by
// BaseModel::write() with no error anywhere — the tax_ids_json defect in
// docs/053 was exactly this, and no unit test could have seen it.
// ================================================================

// ---------------------------------------------------------------
// rental.unit.type
// ---------------------------------------------------------------
class RentalUnitType : public BaseModel<RentalUnitType> {
public:
    ODOO_MODEL("rental.unit.type", "rental_unit_type")

    std::string name, code, defaultPeriod = "monthly", taxIdsJson = "[]";
    double      defaultRate = 0.0;
    double      areaSqm = 0.0, volumeM3 = 0.0;
    int         companyId = 1;
    bool        active = true;

    explicit RentalUnitType(std::shared_ptr<DbConnection> db) : BaseModel(std::move(db)) {}

    void registerFields() {
        fieldRegistry_.add({"name",           FieldType::Char,     "Name", true});
        fieldRegistry_.add({"code",           FieldType::Char,     "Code", true});
        fieldRegistry_.add({"default_rate",   FieldType::Monetary, "Default Rate"});
        fieldRegistry_.add({"default_period", FieldType::Char,     "Billing Period"});
        fieldRegistry_.add({"tax_ids_json",   FieldType::Char,     "Taxes"});
        fieldRegistry_.add({"area_sqm",       FieldType::Float,    "Area (m²)"});
        fieldRegistry_.add({"volume_m3",      FieldType::Float,    "Volume (m³)"});
        fieldRegistry_.add({"company_id",     FieldType::Many2one, "Company", false, false, true, false, "res.company"});
        fieldRegistry_.add({"active",         FieldType::Boolean,  "Active"});
        // area/volume are physical measurements, NOT money — they stay
        // NUMERIC in the DB and must not be scaled.
        fieldRegistry_.markScaled({"default_rate"});
        fieldRegistry_.setPrecision(DecimalPrecision::kProductPrice, {"default_rate"});
    }

    void serializeFields(nlohmann::json& j) const override {
        j["name"]           = name;
        j["code"]           = code;
        j["default_rate"]   = defaultRate;
        j["default_period"] = defaultPeriod;
        j["tax_ids_json"]   = taxIdsJson.empty() ? "[]" : taxIdsJson;
        j["area_sqm"]       = areaSqm;
        j["volume_m3"]      = volumeM3;
        j["company_id"]     = companyId > 0 ? nlohmann::json(companyId) : nlohmann::json(false);
        j["active"]         = active;
    }

    void deserializeFields(const nlohmann::json& j) override {
        if (j.contains("name")           && j["name"].is_string())           name          = j["name"].get<std::string>();
        if (j.contains("code")           && j["code"].is_string())           code          = j["code"].get<std::string>();
        if (j.contains("default_rate")   && j["default_rate"].is_number())   defaultRate   = j["default_rate"].get<double>();
        if (j.contains("default_period") && j["default_period"].is_string()) defaultPeriod = j["default_period"].get<std::string>();
        if (j.contains("tax_ids_json")   && j["tax_ids_json"].is_string()) {
            taxIdsJson = j["tax_ids_json"].get<std::string>();
            if (taxIdsJson.empty()) taxIdsJson = "[]";
        }
        if (j.contains("area_sqm")       && j["area_sqm"].is_number())       areaSqm       = j["area_sqm"].get<double>();
        if (j.contains("volume_m3")      && j["volume_m3"].is_number())      volumeM3      = j["volume_m3"].get<double>();
        if (j.contains("company_id"))                                        companyId     = m2oToId_(j["company_id"]);
        if (j.contains("active")         && j["active"].is_boolean())        active        = j["active"].get<bool>();
    }

    nlohmann::json toJson() const override {
        nlohmann::json j; serializeFields(j);
        j["id"] = getId();
        j["display_name"] = name;
        return j;
    }
    void fromJson(const nlohmann::json& j) override { deserializeFields(j); }

    std::vector<std::string> validate() const override {
        std::vector<std::string> e;
        if (name.empty()) e.push_back("Name is required");
        if (code.empty()) e.push_back("Code is required");
        if (defaultPeriod != "monthly" && defaultPeriod != "quarterly" && defaultPeriod != "yearly")
            e.push_back("Billing period must be monthly, quarterly or yearly");
        return e;
    }
};

// ---------------------------------------------------------------
// rental.unit
//
// `state` is DERIVED from the contract lines. It is registered as a
// normal field so the UI can read and filter it, and so an operator can
// set maintenance/retired — but the billing and contract code recomputes
// it rather than trusting what is stored. A stored state that can
// disagree with the lines is the classic double-let bug.
// ---------------------------------------------------------------
class RentalUnit : public BaseModel<RentalUnit> {
public:
    ODOO_MODEL("rental.unit", "rental_unit")

    std::string code, name, site, zone, floor, state = "available", notes;
    int         typeId = 0, locationId = 0, companyId = 1;
    double      areaSqm = 0.0, volumeM3 = 0.0;
    bool        active = true;

    explicit RentalUnit(std::shared_ptr<DbConnection> db) : BaseModel(std::move(db)) {}

    void registerFields() {
        fieldRegistry_.add({"code",        FieldType::Char,     "Code", true});
        fieldRegistry_.add({"name",        FieldType::Char,     "Name"});
        fieldRegistry_.add({"type_id",     FieldType::Many2one, "Unit Type", false, false, true, false, "rental.unit.type"});
        fieldRegistry_.add({"site",        FieldType::Char,     "Site"});
        fieldRegistry_.add({"zone",        FieldType::Char,     "Zone"});
        fieldRegistry_.add({"floor",       FieldType::Char,     "Floor"});
        fieldRegistry_.add({"area_sqm",    FieldType::Float,    "Area (m²)"});
        fieldRegistry_.add({"volume_m3",   FieldType::Float,    "Volume (m³)"});
        fieldRegistry_.add({"state",       FieldType::Char,     "State"});
        fieldRegistry_.add({"location_id", FieldType::Many2one, "Stock Location", false, false, true, false, "stock.location"});
        fieldRegistry_.add({"notes",       FieldType::Text,     "Notes"});
        fieldRegistry_.add({"company_id",  FieldType::Many2one, "Company", false, false, true, false, "res.company"});
        fieldRegistry_.add({"active",      FieldType::Boolean,  "Active"});
        // No money on this model, so nothing to scale.
    }

    void serializeFields(nlohmann::json& j) const override {
        j["code"]        = code;
        j["name"]        = name;
        j["type_id"]     = typeId     > 0 ? nlohmann::json(typeId)     : nlohmann::json(false);
        j["site"]        = site;
        j["zone"]        = zone;
        j["floor"]       = floor;
        j["area_sqm"]    = areaSqm;
        j["volume_m3"]   = volumeM3;
        j["state"]       = state;
        j["location_id"] = locationId > 0 ? nlohmann::json(locationId) : nlohmann::json(false);
        j["notes"]       = notes;
        j["company_id"]  = companyId  > 0 ? nlohmann::json(companyId)  : nlohmann::json(false);
        j["active"]      = active;
    }

    void deserializeFields(const nlohmann::json& j) override {
        if (j.contains("code")      && j["code"].is_string())      code      = j["code"].get<std::string>();
        if (j.contains("name")      && j["name"].is_string())      name      = j["name"].get<std::string>();
        if (j.contains("type_id"))                                 typeId    = m2oToId_(j["type_id"]);
        if (j.contains("site")      && j["site"].is_string())      site      = j["site"].get<std::string>();
        if (j.contains("zone")      && j["zone"].is_string())      zone      = j["zone"].get<std::string>();
        if (j.contains("floor")     && j["floor"].is_string())     floor     = j["floor"].get<std::string>();
        if (j.contains("area_sqm")  && j["area_sqm"].is_number())  areaSqm   = j["area_sqm"].get<double>();
        if (j.contains("volume_m3") && j["volume_m3"].is_number()) volumeM3  = j["volume_m3"].get<double>();
        if (j.contains("state")     && j["state"].is_string())     state     = j["state"].get<std::string>();
        if (j.contains("location_id"))                             locationId= m2oToId_(j["location_id"]);
        if (j.contains("notes")     && j["notes"].is_string())     notes     = j["notes"].get<std::string>();
        if (j.contains("company_id"))                              companyId = m2oToId_(j["company_id"]);
        if (j.contains("active")    && j["active"].is_boolean())   active    = j["active"].get<bool>();
    }

    nlohmann::json toJson() const override {
        nlohmann::json j; serializeFields(j);
        j["id"] = getId();
        j["display_name"] = name.empty() ? code : (code + " — " + name);
        return j;
    }
    void fromJson(const nlohmann::json& j) override { deserializeFields(j); }

    std::vector<std::string> validate() const override {
        std::vector<std::string> e;
        if (code.empty()) e.push_back("Code is required");
        static const std::vector<std::string> kStates =
            {"available","reserved","occupied","maintenance","retired"};
        bool ok = false;
        for (const auto& s : kStates) if (state == s) ok = true;
        if (!ok) e.push_back("Invalid unit state");
        return e;
    }
};

// ---------------------------------------------------------------
// rental.contract
// ---------------------------------------------------------------
class RentalContract : public BaseModel<RentalContract> {
public:
    ODOO_MODEL("rental.contract", "rental_contract")

    std::string name, state = "draft", dateStart, dateCancelled,
                billingPeriod = "monthly", depositState = "none", notes;
    // Derived from billingPeriod by trigger (migration 816), except for the
    // 'custom' preset where these two ARE the user's choice. 0 / "" mean the
    // period has no interval at all — one-off and on-demand.
    int         billingInterval = 1;
    std::string billingUnit     = "month";
    int    partnerId = 0, paymentTermId = 0, currencyId = 0,
           journalId = 0, companyId = 1, billingLeadDays = 7;
    double depositAmount = 0.0;
    bool   active = true;

    explicit RentalContract(std::shared_ptr<DbConnection> db) : BaseModel(std::move(db)) {}

    void registerFields() {
        fieldRegistry_.add({"name",              FieldType::Char,     "Reference"});
        fieldRegistry_.add({"partner_id",        FieldType::Many2one, "Customer", true, false, true, false, "res.partner"});
        {
            core::FieldDef st{"state", FieldType::Selection, "Status"};
            st.selection = { {"draft", "Draft"}, {"active", "Active"},
                             {"cancelled", "Cancelled"}, {"closed", "Closed"} };
            fieldRegistry_.add(st);
        }
        fieldRegistry_.add({"date_start",        FieldType::Date,     "Start Date"});
        fieldRegistry_.add({"date_cancelled",    FieldType::Date,     "Cancelled On"});
        // A selection, not free text: these nine values are the whole vocabulary
        // of the CHECK constraint in migration 816, and the client renders a
        // real combobox for a selection field instead of a text box the user
        // has to guess the spelling into.
        {
            core::FieldDef bp{"billing_period", FieldType::Selection, "Billing Period"};
            bp.selection = {
                {"daily",     "Daily"},
                {"weekly",    "Weekly"},
                {"monthly",   "Monthly"},
                {"quarterly", "Quarterly (3 months)"},
                {"biannual",  "Every 6 months"},
                {"yearly",    "Yearly"},
                {"custom",    "Custom — every X…"},
                {"oneoff",    "One off"},
                {"ondemand",  "On demand"},
            };
            fieldRegistry_.add(bp);
        }
        fieldRegistry_.add({"billing_interval",  FieldType::Integer,  "Every"});
        {
            core::FieldDef bu{"billing_unit", FieldType::Selection, "Period Unit"};
            bu.selection = {
                {"day",   "Day(s)"},
                {"week",  "Week(s)"},
                {"month", "Month(s)"},
                {"year",  "Year(s)"},
            };
            fieldRegistry_.add(bu);
        }
        fieldRegistry_.add({"billing_lead_days", FieldType::Integer,  "Invoice Lead Days"});
        fieldRegistry_.add({"payment_term_id",   FieldType::Many2one, "Payment Terms", false, false, true, false, "account.payment.term"});
        fieldRegistry_.add({"deposit_amount",    FieldType::Monetary, "Deposit"});
        // Both of these are CHECK-constrained to a fixed list, so they are
        // selections: as Char the form drew a text box and the user had to
        // type "forfeited" correctly to change a deposit's state.
        {
            core::FieldDef ds{"deposit_state", FieldType::Selection, "Deposit Status"};
            ds.selection = { {"none", "Not held"}, {"held", "Held"},
                             {"refunded", "Refunded"}, {"forfeited", "Forfeited"} };
            fieldRegistry_.add(ds);
        }
        fieldRegistry_.add({"currency_id",       FieldType::Many2one, "Currency", false, false, true, false, "res.currency"});
        fieldRegistry_.add({"journal_id",        FieldType::Many2one, "Journal", false, false, true, false, "account.journal"});
        fieldRegistry_.add({"notes",             FieldType::Text,     "Notes"});
        fieldRegistry_.add({"company_id",        FieldType::Many2one, "Company", false, false, true, false, "res.company"});
        fieldRegistry_.add({"active",            FieldType::Boolean,  "Active"});
        // The units let under this contract.
        //
        // Without this the contract was a header with nothing to rent: the form
        // showed a customer, dates and a deposit, and there was no way at all —
        // no menu, no action, no lines section — to say WHICH unit the tenant
        // has. The generic form renders a one2many as an editable line table,
        // so declaring it here is the whole feature.
        //
        // store=false, searchable=false: it is not a column. The inverse is
        // rental_contract_line.contract_id.
        fieldRegistry_.add({"line_ids", FieldType::One2many, "Units",
                            false, false, false, false,
                            "rental.contract.line", "contract_id"});
        fieldRegistry_.markScaled({"deposit_amount"});
    }

    void serializeFields(nlohmann::json& j) const override {
        j["name"]              = name;
        j["partner_id"]        = partnerId     > 0 ? nlohmann::json(partnerId)     : nlohmann::json(false);
        j["state"]             = state;
        j["date_start"]        = dateStart.empty()     ? nlohmann::json(nullptr) : nlohmann::json(dateStart);
        j["date_cancelled"]    = dateCancelled.empty() ? nlohmann::json(nullptr) : nlohmann::json(dateCancelled);
        j["billing_period"]    = billingPeriod;
        // NULL, not 0/"" — one-off and on-demand have no interval, and a 0 in
        // an interval column would look like a period of "every zero months".
        j["billing_interval"]  = billingInterval > 0 ? nlohmann::json(billingInterval)
                                                     : nlohmann::json(nullptr);
        j["billing_unit"]      = billingUnit.empty() ? nlohmann::json(nullptr)
                                                     : nlohmann::json(billingUnit);
        j["billing_lead_days"] = billingLeadDays;
        j["payment_term_id"]   = paymentTermId > 0 ? nlohmann::json(paymentTermId) : nlohmann::json(false);
        j["deposit_amount"]    = depositAmount;
        j["deposit_state"]     = depositState;
        j["currency_id"]       = currencyId    > 0 ? nlohmann::json(currencyId)    : nlohmann::json(false);
        j["journal_id"]        = journalId     > 0 ? nlohmann::json(journalId)     : nlohmann::json(false);
        j["notes"]             = notes;
        j["company_id"]        = companyId     > 0 ? nlohmann::json(companyId)     : nlohmann::json(false);
        j["active"]            = active;
    }

    void deserializeFields(const nlohmann::json& j) override {
        if (j.contains("name")           && j["name"].is_string())           name          = j["name"].get<std::string>();
        if (j.contains("partner_id"))                                        partnerId     = m2oToId_(j["partner_id"]);
        if (j.contains("state")          && j["state"].is_string())          state         = j["state"].get<std::string>();
        if (j.contains("date_start")     && j["date_start"].is_string())     dateStart     = j["date_start"].get<std::string>();
        if (j.contains("date_cancelled") && j["date_cancelled"].is_string()) dateCancelled = j["date_cancelled"].get<std::string>();
        if (j.contains("billing_period") && j["billing_period"].is_string()) billingPeriod = j["billing_period"].get<std::string>();
        if (j.contains("billing_interval") && j["billing_interval"].is_number())
            billingInterval = j["billing_interval"].get<int>();
        if (j.contains("billing_unit")   && j["billing_unit"].is_string())   billingUnit   = j["billing_unit"].get<std::string>();
        // Mirror the trigger so a caller that reads the record straight back
        // sees the same period it would get from the database. The trigger is
        // still the authority — this only keeps the in-memory object honest.
        applyPeriodPreset_();
        if (j.contains("billing_lead_days") && j["billing_lead_days"].is_number())
            billingLeadDays = j["billing_lead_days"].get<int>();
        if (j.contains("payment_term_id"))                                   paymentTermId = m2oToId_(j["payment_term_id"]);
        if (j.contains("deposit_amount") && j["deposit_amount"].is_number()) depositAmount = j["deposit_amount"].get<double>();
        if (j.contains("deposit_state")  && j["deposit_state"].is_string())  depositState  = j["deposit_state"].get<std::string>();
        if (j.contains("currency_id"))                                       currencyId    = m2oToId_(j["currency_id"]);
        if (j.contains("journal_id"))                                        journalId     = m2oToId_(j["journal_id"]);
        if (j.contains("notes")          && j["notes"].is_string())          notes         = j["notes"].get<std::string>();
        if (j.contains("company_id"))                                        companyId     = m2oToId_(j["company_id"]);
        if (j.contains("active")         && j["active"].is_boolean())        active        = j["active"].get<bool>();
    }

    nlohmann::json toJson() const override {
        nlohmann::json j; serializeFields(j);
        j["id"] = getId();
        j["display_name"] = name;
        return j;
    }
    void fromJson(const nlohmann::json& j) override { deserializeFields(j); }

    std::vector<std::string> validate() const override {
        std::vector<std::string> e;
        if (partnerId <= 0) e.push_back("Customer is required");
        if (billingLeadDays < 0 || billingLeadDays > 90)
            e.push_back("Invoice lead days must be between 0 and 90");
        static const std::set<std::string> kPeriods = {
            "daily", "weekly", "monthly", "quarterly", "biannual",
            "yearly", "custom", "oneoff", "ondemand"};
        if (!kPeriods.count(billingPeriod))
            e.push_back("Billing period must be one of daily, weekly, monthly, "
                        "quarterly, biannual, yearly, custom, oneoff, ondemand");
        // Only 'custom' lets the user choose the numbers, so only 'custom' can
        // get them wrong. A rejected value here is a 400 with this sentence
        // rather than a raw CHECK-constraint violation from PostgreSQL.
        if (billingPeriod == "custom") {
            if (billingInterval < 1 || billingInterval > 366)
                e.push_back("Every X must be between 1 and 366");
            static const std::set<std::string> kUnits = {"day", "week", "month", "year"};
            if (!kUnits.count(billingUnit))
                e.push_back("Period unit must be day, week, month or year");
        }
        return e;
    }

private:
    /// The preset -> (interval, unit) table, mirroring migration 816's trigger.
    void applyPeriodPreset_() {
        if (billingPeriod == "daily")          { billingInterval = 1; billingUnit = "day";   }
        else if (billingPeriod == "weekly")    { billingInterval = 1; billingUnit = "week";  }
        else if (billingPeriod == "monthly")   { billingInterval = 1; billingUnit = "month"; }
        else if (billingPeriod == "quarterly") { billingInterval = 3; billingUnit = "month"; }
        else if (billingPeriod == "biannual")  { billingInterval = 6; billingUnit = "month"; }
        else if (billingPeriod == "yearly")    { billingInterval = 1; billingUnit = "year";  }
        else if (billingPeriod == "custom")    { /* the user's own numbers stand */ }
        else                                   { billingInterval = 0; billingUnit.clear(); }
    }
};

// ---------------------------------------------------------------
// rental.contract.line — where the per-unit dates live
// ---------------------------------------------------------------
class RentalContractLine : public BaseModel<RentalContractLine> {
public:
    ODOO_MODEL("rental.contract.line", "rental_contract_line")

    std::string dateStart, dateEnd, nextPeriodStart, invoicedThrough,
                prorationPolicy = "full_period", state = "pending", taxIdsJson = "[]",
                billingMode = "manual";
    int    contractId = 0, partnerId = 0, unitId = 0, billingAnchorDay = 1,
           billingMonths = 1, billingLeadDays = 7, companyId = 1;
    // 0 / "" mean "inherit the contract's period" and store as NULL
    // (migration 816). A line only carries these when it deliberately bills at
    // a different cadence from its contract.
    int         billingInterval = 0;
    std::string billingUnit;
    double unitPrice = 0.0, discountPct = 0.0;

    explicit RentalContractLine(std::shared_ptr<DbConnection> db) : BaseModel(std::move(db)) {}

    void registerFields() {
        // contract_id is OPTIONAL (migration 812): a walk-in has no
        // contract, so the customer lives on the line itself.
        fieldRegistry_.add({"contract_id",        FieldType::Many2one, "Contract", false, false, true, false, "rental.contract"});
        fieldRegistry_.add({"partner_id",         FieldType::Many2one, "Customer", true,  false, true, false, "res.partner"});
        fieldRegistry_.add({"unit_id",            FieldType::Many2one, "Unit", false, false, true, false, "rental.unit"});
        fieldRegistry_.add({"date_start",         FieldType::Date,     "Start Date", true});
        fieldRegistry_.add({"date_end",           FieldType::Date,     "End Date"});
        fieldRegistry_.add({"unit_price",         FieldType::Monetary, "Rate"});
        fieldRegistry_.add({"discount_pct",       FieldType::Float,    "Discount %"});
        fieldRegistry_.add({"tax_ids_json",       FieldType::Char,     "Taxes"});
        fieldRegistry_.add({"billing_anchor_day", FieldType::Integer,  "Billing Day"});
        // Migration 812. A column that exists in the DB but not here is
        // silently DISCARDED by write() — the tax_ids_json defect in
        // docs/053, and the reason every new column is registered in the
        // same commit that adds it.
        // Every one of these four is fenced by a CHECK constraint, so its
        // vocabulary is already fixed — registering it as Char only hid that
        // from the client. A line's Billing and Status then rendered as free
        // TEXT BOXES in the contract's line table: the user had to type
        // "recurring" and "active" letter-perfect, and anything else was
        // refused on save by a constraint, from a form that never showed the
        // valid values. Same argument as billing_period on the contract above.
        {
            core::FieldDef bm{"billing_mode", FieldType::Selection, "Billing"};
            bm.selection = { {"recurring", "Recurring"}, {"oneoff", "One off"},
                             {"ondemand", "On demand"},  {"manual", "Manual"} };
            fieldRegistry_.add(bm);
        }
        fieldRegistry_.add({"billing_months",     FieldType::Integer,  "Every (months)"});
        // docs/architecture/modules.md "The billing period": (interval, unit) expresses every period in one shape —
        // daily/weekly/monthly/quarterly/biannual/yearly are just presets over
        // it, and "every X <unit>" is the same field pair with an arbitrary X.
        fieldRegistry_.add({"billing_interval",   FieldType::Integer,  "Every"});
        {
            core::FieldDef bu{"billing_unit", FieldType::Selection, "Period"};
            bu.selection = { {"day", "Day"}, {"week", "Week"},
                             {"month", "Month"}, {"year", "Year"} };
            fieldRegistry_.add(bu);
        }
        fieldRegistry_.add({"billing_lead_days",  FieldType::Integer,  "Invoice Lead Days"});
        fieldRegistry_.add({"next_period_start",  FieldType::Date,     "Next Period"});
        fieldRegistry_.add({"invoiced_through",   FieldType::Date,     "Invoiced Through"});
        {
            core::FieldDef pp{"proration_policy", FieldType::Selection, "Proration"};
            pp.selection = { {"full_period",      "Charge the full period"},
                             {"prorate_days",     "Prorate by days"},
                             {"start_next_cycle", "Start next cycle"} };
            fieldRegistry_.add(pp);
        }
        {
            core::FieldDef st{"state", FieldType::Selection, "Status"};
            st.selection = { {"pending", "Pending"}, {"active", "Active"},
                             {"ended", "Ended"},     {"cancelled", "Cancelled"} };
            fieldRegistry_.add(st);
        }
        fieldRegistry_.add({"company_id",         FieldType::Many2one, "Company", false, false, true, false, "res.company"});
        // discount_pct is BIGINT micro-units like every other scaled value,
        // so 12.5% stores as 12500000 — not a NUMERIC percentage.
        fieldRegistry_.markScaled({"unit_price", "discount_pct"});
        fieldRegistry_.setPrecision(DecimalPrecision::kProductPrice, {"unit_price"});
        fieldRegistry_.setPrecision(DecimalPrecision::kDiscount,     {"discount_pct"});
    }

    void serializeFields(nlohmann::json& j) const override {
        j["contract_id"]        = contractId > 0 ? nlohmann::json(contractId) : nlohmann::json(false);
        j["partner_id"]         = partnerId  > 0 ? nlohmann::json(partnerId)  : nlohmann::json(false);
        j["unit_id"]            = unitId     > 0 ? nlohmann::json(unitId)     : nlohmann::json(false);
        j["date_start"]         = dateStart.empty()       ? nlohmann::json(nullptr) : nlohmann::json(dateStart);
        j["date_end"]           = dateEnd.empty()         ? nlohmann::json(nullptr) : nlohmann::json(dateEnd);
        j["unit_price"]         = unitPrice;
        j["discount_pct"]       = discountPct;
        j["tax_ids_json"]       = taxIdsJson.empty() ? "[]" : taxIdsJson;
        j["billing_anchor_day"] = billingAnchorDay;
        j["billing_mode"]       = billingMode;
        j["billing_months"]     = billingMonths;
        // NULL = follow the contract. Writing 'month' here instead would make
        // every line an explicit monthly override and the contract's own
        // billing period could never take effect (migration 816).
        j["billing_interval"]   = billingInterval > 0 ? nlohmann::json(billingInterval)
                                                      : nlohmann::json(nullptr);
        j["billing_unit"]       = billingUnit.empty() ? nlohmann::json(nullptr)
                                                      : nlohmann::json(billingUnit);
        j["billing_lead_days"]  = billingLeadDays;
        j["next_period_start"]  = nextPeriodStart.empty() ? nlohmann::json(nullptr) : nlohmann::json(nextPeriodStart);
        j["invoiced_through"]   = invoicedThrough.empty() ? nlohmann::json(nullptr) : nlohmann::json(invoicedThrough);
        j["proration_policy"]   = prorationPolicy;
        j["state"]              = state;
        j["company_id"]         = companyId > 0 ? nlohmann::json(companyId) : nlohmann::json(false);
    }

    void deserializeFields(const nlohmann::json& j) override {
        if (j.contains("contract_id"))                                          contractId      = m2oToId_(j["contract_id"]);
        if (j.contains("partner_id"))                                           partnerId       = m2oToId_(j["partner_id"]);
        if (j.contains("unit_id"))                                              unitId          = m2oToId_(j["unit_id"]);
        if (j.contains("date_start")        && j["date_start"].is_string())        dateStart       = j["date_start"].get<std::string>();
        if (j.contains("date_end")          && j["date_end"].is_string())          dateEnd         = j["date_end"].get<std::string>();
        if (j.contains("unit_price")        && j["unit_price"].is_number())        unitPrice       = j["unit_price"].get<double>();
        if (j.contains("discount_pct")      && j["discount_pct"].is_number())      discountPct     = j["discount_pct"].get<double>();
        if (j.contains("tax_ids_json")      && j["tax_ids_json"].is_string()) {
            taxIdsJson = j["tax_ids_json"].get<std::string>();
            if (taxIdsJson.empty()) taxIdsJson = "[]";
        }
        if (j.contains("billing_anchor_day") && j["billing_anchor_day"].is_number()) billingAnchorDay = j["billing_anchor_day"].get<int>();
        if (j.contains("billing_mode")      && j["billing_mode"].is_string())      billingMode     = j["billing_mode"].get<std::string>();
        if (j.contains("billing_months")    && j["billing_months"].is_number())    billingMonths   = j["billing_months"].get<int>();
        if (j.contains("billing_interval")  && j["billing_interval"].is_number())  billingInterval = j["billing_interval"].get<int>();
        if (j.contains("billing_unit")      && j["billing_unit"].is_string())      billingUnit     = j["billing_unit"].get<std::string>();
        // Keep billing_months in step for month-based periods: the billing run
        // and every report still read it, and two fields disagreeing about the
        // same period is how a tenancy silently bills at the wrong cadence.
        // Only when this line HAS its own period — an inheriting line (unit
        // empty) must not have its billing_months rewritten to zero.
        if (billingInterval > 0) {
            if (billingUnit == "month")      billingMonths = billingInterval;
            else if (billingUnit == "year")  billingMonths = billingInterval * 12;
        }
        if (j.contains("billing_lead_days") && j["billing_lead_days"].is_number()) billingLeadDays = j["billing_lead_days"].get<int>();
        if (j.contains("next_period_start") && j["next_period_start"].is_string()) nextPeriodStart = j["next_period_start"].get<std::string>();
        if (j.contains("invoiced_through")  && j["invoiced_through"].is_string())  invoicedThrough = j["invoiced_through"].get<std::string>();
        if (j.contains("proration_policy")  && j["proration_policy"].is_string())  prorationPolicy = j["proration_policy"].get<std::string>();
        if (j.contains("state")             && j["state"].is_string())             state           = j["state"].get<std::string>();
        if (j.contains("company_id"))                                              companyId       = m2oToId_(j["company_id"]);
    }

    nlohmann::json toJson() const override {
        nlohmann::json j; serializeFields(j);
        j["id"] = getId();
        j["display_name"] = dateStart;
        return j;
    }
    void fromJson(const nlohmann::json& j) override { deserializeFields(j); }

    std::vector<std::string> validate() const override {
        std::vector<std::string> e;
        // The CUSTOMER is required; the contract is not. That inversion is
        // the whole point of migration 812 — a walk-in rents a unit with
        // no contract in sight.
        // A line under a contract takes the contract's customer (migration
        // 818's trigger fills it), which is why the contract form's line grid
        // has no Customer column. A walk-in line has no contract, so there the
        // customer really is required.
        if (partnerId <= 0 && contractId <= 0)
            e.push_back("Customer is required");
        if (dateStart.empty()) e.push_back("Start date is required");
        if (billingAnchorDay < 1 || billingAnchorDay > 31)
            e.push_back("Billing day must be between 1 and 31");
        if (billingMode != "manual" && billingMode != "recurring"
            && billingMode != "oneoff" && billingMode != "ondemand")
            e.push_back("Billing must be one of: manual, recurring, oneoff, ondemand");
        if (!billingUnit.empty() && billingUnit != "day" && billingUnit != "week"
            && billingUnit != "month" && billingUnit != "year")
            e.push_back("Billing period must be day, week, month or year");
        // 366 is the CHECK's ceiling; a friendly message beats a constraint
        // violation the user cannot act on.
        //
        // 0 is not out of range — it is the line saying "I have no period of my
        // own, use the contract's" (migration 816). Rejecting it here made a
        // line that inherits impossible to create at all, which is the normal
        // case: most lines follow their contract.
        if (billingInterval != 0 && (billingInterval < 1 || billingInterval > 366))
            e.push_back("Billing interval must be between 1 and 366");
        // Half a period is not a period: an interval without a unit, or a unit
        // without an interval, resolves differently on the line and in the
        // billing query.
        if ((billingInterval == 0) != billingUnit.empty())
            e.push_back("Set both 'every' and its unit, or neither to follow the contract");
        if (billingMonths < 1 || billingMonths > 12)
            e.push_back("Billing interval must be between 1 and 12 months");
        return e;
    }
};

// ---------------------------------------------------------------
// rental.expense.category
// ---------------------------------------------------------------
class RentalExpenseCategory : public BaseModel<RentalExpenseCategory> {
public:
    ODOO_MODEL("rental.expense.category", "rental_expense_category")

    std::string name;
    int  accountId = 0, companyId = 1;
    bool isOperating = true, active = true;

    explicit RentalExpenseCategory(std::shared_ptr<DbConnection> db) : BaseModel(std::move(db)) {}

    void registerFields() {
        fieldRegistry_.add({"name",         FieldType::Char,     "Name", true});
        fieldRegistry_.add({"account_id",   FieldType::Many2one, "Expense Account", false, false, true, false, "account.account"});
        fieldRegistry_.add({"is_operating", FieldType::Boolean,  "Operating Expense"});
        fieldRegistry_.add({"company_id",   FieldType::Many2one, "Company", false, false, true, false, "res.company"});
        fieldRegistry_.add({"active",       FieldType::Boolean,  "Active"});
    }
    void serializeFields(nlohmann::json& j) const override {
        j["name"]         = name;
        j["account_id"]   = accountId > 0 ? nlohmann::json(accountId) : nlohmann::json(false);
        j["is_operating"] = isOperating;
        j["company_id"]   = companyId > 0 ? nlohmann::json(companyId) : nlohmann::json(false);
        j["active"]       = active;
    }
    void deserializeFields(const nlohmann::json& j) override {
        if (j.contains("name")         && j["name"].is_string())          name        = j["name"].get<std::string>();
        if (j.contains("account_id"))                                     accountId   = m2oToId_(j["account_id"]);
        if (j.contains("is_operating") && j["is_operating"].is_boolean()) isOperating = j["is_operating"].get<bool>();
        if (j.contains("company_id"))                                     companyId   = m2oToId_(j["company_id"]);
        if (j.contains("active")       && j["active"].is_boolean())       active      = j["active"].get<bool>();
    }
    nlohmann::json toJson() const override {
        nlohmann::json j; serializeFields(j);
        j["id"] = getId(); j["display_name"] = name;
        return j;
    }
    void fromJson(const nlohmann::json& j) override { deserializeFields(j); }
    std::vector<std::string> validate() const override {
        std::vector<std::string> e;
        if (name.empty()) e.push_back("Name is required");
        return e;
    }
};

// ---------------------------------------------------------------
// rental.expense
// ---------------------------------------------------------------
class RentalExpense : public BaseModel<RentalExpense> {
public:
    ODOO_MODEL("rental.expense", "rental_expense")

    std::string date, name, state = "draft", recurrenceInterval,
                recurrenceNextDate, recurrenceEndDate;
    int    categoryId = 0, partnerId = 0, unitId = 0, contractId = 0,
           accountId = 0, moveId = 0, recurrenceParentId = 0, companyId = 1;
    double amount = 0.0;
    bool   isRecurring = false;

    explicit RentalExpense(std::shared_ptr<DbConnection> db) : BaseModel(std::move(db)) {}

    void registerFields() {
        fieldRegistry_.add({"date",                 FieldType::Date,     "Date", true});
        fieldRegistry_.add({"name",                 FieldType::Char,     "Description", true});
        fieldRegistry_.add({"category_id",          FieldType::Many2one, "Category", false, false, true, false, "rental.expense.category"});
        fieldRegistry_.add({"amount",               FieldType::Monetary, "Amount"});
        fieldRegistry_.add({"partner_id",           FieldType::Many2one, "Vendor", false, false, true, false, "res.partner"});
        fieldRegistry_.add({"unit_id",              FieldType::Many2one, "Unit", false, false, true, false, "rental.unit"});
        fieldRegistry_.add({"contract_id",          FieldType::Many2one, "Recharge To", false, false, true, false, "rental.contract"});
        fieldRegistry_.add({"account_id",           FieldType::Many2one, "Account", false, false, true, false, "account.account"});
        fieldRegistry_.add({"state",                FieldType::Char,     "Status"});
        fieldRegistry_.add({"move_id",              FieldType::Many2one, "Vendor Bill", false, false, true, false, "account.move"});
        fieldRegistry_.add({"is_recurring",         FieldType::Boolean,  "Recurring"});
        fieldRegistry_.add({"recurrence_interval",  FieldType::Char,     "Every"});
        fieldRegistry_.add({"recurrence_next_date", FieldType::Date,     "Next Generation"});
        fieldRegistry_.add({"recurrence_end_date",  FieldType::Date,     "Until"});
        fieldRegistry_.add({"recurrence_parent_id", FieldType::Many2one, "Generated From", false, false, true, false, "rental.expense"});
        fieldRegistry_.add({"company_id",           FieldType::Many2one, "Company", false, false, true, false, "res.company"});
        // Receipts attach through ir.attachment's polymorphic (res_model,
        // res_id) link, not a column here — the placeholder attachment_id was
        // dropped in migration 814 (docs/092). Historical note follows:
        // attachment_id existed in the DB but was deliberately NOT registered:
        // there is no ir.attachment model yet, so nothing can write it, and
        // registering a field with no model behind it invites a UI that
        // pretends receipts can be uploaded (docs/054 §7).
        fieldRegistry_.markScaled({"amount"});
    }

    void serializeFields(nlohmann::json& j) const override {
        j["date"]                 = date.empty() ? nlohmann::json(nullptr) : nlohmann::json(date);
        j["name"]                 = name;
        j["category_id"]          = categoryId         > 0 ? nlohmann::json(categoryId)         : nlohmann::json(false);
        j["amount"]               = amount;
        j["partner_id"]           = partnerId          > 0 ? nlohmann::json(partnerId)          : nlohmann::json(false);
        j["unit_id"]              = unitId             > 0 ? nlohmann::json(unitId)             : nlohmann::json(false);
        j["contract_id"]          = contractId         > 0 ? nlohmann::json(contractId)         : nlohmann::json(false);
        j["account_id"]           = accountId          > 0 ? nlohmann::json(accountId)          : nlohmann::json(false);
        j["state"]                = state;
        j["move_id"]              = moveId             > 0 ? nlohmann::json(moveId)             : nlohmann::json(false);
        j["is_recurring"]         = isRecurring;
        j["recurrence_interval"]  = recurrenceInterval.empty() ? nlohmann::json(nullptr) : nlohmann::json(recurrenceInterval);
        j["recurrence_next_date"] = recurrenceNextDate.empty() ? nlohmann::json(nullptr) : nlohmann::json(recurrenceNextDate);
        j["recurrence_end_date"]  = recurrenceEndDate.empty()  ? nlohmann::json(nullptr) : nlohmann::json(recurrenceEndDate);
        j["recurrence_parent_id"] = recurrenceParentId > 0 ? nlohmann::json(recurrenceParentId) : nlohmann::json(false);
        j["company_id"]           = companyId          > 0 ? nlohmann::json(companyId)          : nlohmann::json(false);
    }

    void deserializeFields(const nlohmann::json& j) override {
        if (j.contains("date")         && j["date"].is_string())         date       = j["date"].get<std::string>();
        if (j.contains("name")         && j["name"].is_string())         name       = j["name"].get<std::string>();
        if (j.contains("category_id"))                                   categoryId = m2oToId_(j["category_id"]);
        if (j.contains("amount")       && j["amount"].is_number())       amount     = j["amount"].get<double>();
        if (j.contains("partner_id"))                                    partnerId  = m2oToId_(j["partner_id"]);
        if (j.contains("unit_id"))                                       unitId     = m2oToId_(j["unit_id"]);
        if (j.contains("contract_id"))                                   contractId = m2oToId_(j["contract_id"]);
        if (j.contains("account_id"))                                    accountId  = m2oToId_(j["account_id"]);
        if (j.contains("state")        && j["state"].is_string())        state      = j["state"].get<std::string>();
        if (j.contains("move_id"))                                       moveId     = m2oToId_(j["move_id"]);
        if (j.contains("is_recurring") && j["is_recurring"].is_boolean()) isRecurring = j["is_recurring"].get<bool>();
        if (j.contains("recurrence_interval")  && j["recurrence_interval"].is_string())  recurrenceInterval = j["recurrence_interval"].get<std::string>();
        if (j.contains("recurrence_next_date") && j["recurrence_next_date"].is_string()) recurrenceNextDate = j["recurrence_next_date"].get<std::string>();
        if (j.contains("recurrence_end_date")  && j["recurrence_end_date"].is_string())  recurrenceEndDate  = j["recurrence_end_date"].get<std::string>();
        if (j.contains("recurrence_parent_id"))                          recurrenceParentId = m2oToId_(j["recurrence_parent_id"]);
        if (j.contains("company_id"))                                    companyId  = m2oToId_(j["company_id"]);
    }

    nlohmann::json toJson() const override {
        nlohmann::json j; serializeFields(j);
        j["id"] = getId(); j["display_name"] = name;
        return j;
    }
    void fromJson(const nlohmann::json& j) override { deserializeFields(j); }

    std::vector<std::string> validate() const override {
        std::vector<std::string> e;
        if (name.empty()) e.push_back("Description is required");
        if (date.empty()) e.push_back("Date is required");
        if (isRecurring && recurrenceInterval.empty())
            e.push_back("A recurring expense needs an interval");
        return e;
    }
};

// ---------------------------------------------------------------
// rental.event — read-only from the UI's point of view
// ---------------------------------------------------------------
class RentalEvent : public BaseModel<RentalEvent> {
public:
    ODOO_MODEL("rental.event", "rental_event")

    std::string occurredAt, eventType, summary, refModel;
    int contractId = 0, lineId = 0, unitId = 0, partnerId = 0,
        userId = 0, refId = 0, companyId = 1;

    explicit RentalEvent(std::shared_ptr<DbConnection> db) : BaseModel(std::move(db)) {}

    void registerFields() {
        fieldRegistry_.add({"occurred_at", FieldType::Datetime, "When"});
        fieldRegistry_.add({"event_type",  FieldType::Char,     "Event"});
        fieldRegistry_.add({"contract_id", FieldType::Many2one, "Contract", false, false, true, false, "rental.contract"});
        fieldRegistry_.add({"line_id",     FieldType::Integer,  "Line"});
        fieldRegistry_.add({"unit_id",     FieldType::Many2one, "Unit", false, false, true, false, "rental.unit"});
        fieldRegistry_.add({"partner_id",  FieldType::Many2one, "Customer", false, false, true, false, "res.partner"});
        fieldRegistry_.add({"user_id",     FieldType::Many2one, "User", false, false, true, false, "res.users"});
        fieldRegistry_.add({"summary",     FieldType::Char,     "Summary"});
        fieldRegistry_.add({"ref_model",   FieldType::Char,     "Ref Model"});
        fieldRegistry_.add({"ref_id",      FieldType::Integer,  "Ref Id"});
        fieldRegistry_.add({"company_id",  FieldType::Many2one, "Company", false, false, true, false, "res.company"});
        // `detail` (JSONB) is not registered: BaseModel has no JSONB field
        // type, and the feed reads summary. The column is populated by
        // RentalEvents::emit for forensic queries.
    }

    void serializeFields(nlohmann::json& j) const override {
        j["occurred_at"] = occurredAt;
        j["event_type"]  = eventType;
        j["contract_id"] = contractId > 0 ? nlohmann::json(contractId) : nlohmann::json(false);
        j["line_id"]     = lineId;
        j["unit_id"]     = unitId     > 0 ? nlohmann::json(unitId)     : nlohmann::json(false);
        j["partner_id"]  = partnerId  > 0 ? nlohmann::json(partnerId)  : nlohmann::json(false);
        j["user_id"]     = userId     > 0 ? nlohmann::json(userId)     : nlohmann::json(false);
        j["summary"]     = summary;
        j["ref_model"]   = refModel;
        j["ref_id"]      = refId;
        j["company_id"]  = companyId  > 0 ? nlohmann::json(companyId)  : nlohmann::json(false);
    }

    void deserializeFields(const nlohmann::json& j) override {
        if (j.contains("occurred_at") && j["occurred_at"].is_string()) occurredAt = j["occurred_at"].get<std::string>();
        if (j.contains("event_type")  && j["event_type"].is_string())  eventType  = j["event_type"].get<std::string>();
        if (j.contains("contract_id"))                                 contractId = m2oToId_(j["contract_id"]);
        if (j.contains("line_id")     && j["line_id"].is_number())     lineId     = j["line_id"].get<int>();
        if (j.contains("unit_id"))                                     unitId     = m2oToId_(j["unit_id"]);
        if (j.contains("partner_id"))                                  partnerId  = m2oToId_(j["partner_id"]);
        if (j.contains("user_id"))                                     userId     = m2oToId_(j["user_id"]);
        if (j.contains("summary")     && j["summary"].is_string())     summary    = j["summary"].get<std::string>();
        if (j.contains("ref_model")   && j["ref_model"].is_string())   refModel   = j["ref_model"].get<std::string>();
        if (j.contains("ref_id")      && j["ref_id"].is_number())      refId      = j["ref_id"].get<int>();
        if (j.contains("company_id"))                                  companyId  = m2oToId_(j["company_id"]);
    }

    nlohmann::json toJson() const override {
        nlohmann::json j; serializeFields(j);
        j["id"] = getId(); j["display_name"] = summary;
        return j;
    }
    void fromJson(const nlohmann::json& j) override { deserializeFields(j); }
    std::vector<std::string> validate() const override { return {}; }
};

// ================================================================
// 2. VIEWS
// ================================================================

namespace {

class SimpleView : public core::BaseView {
public:
    SimpleView(std::string vn, std::string mn, std::string vt,
               std::string archStr, nlohmann::json flds)
        : vn_(std::move(vn)), mn_(std::move(mn)), vt_(std::move(vt))
        , arch_(std::move(archStr)), fields_(std::move(flds)) {}

    std::string viewName()  const override { return vn_; }
    std::string modelName() const override { return mn_; }
    std::string viewType()  const override { return vt_; }
    std::string arch()      const override { return arch_; }
    nlohmann::json fields() const override { return fields_; }
    nlohmann::json render(const nlohmann::json&) const override { return {}; }

private:
    std::string vn_, mn_, vt_, arch_;
    nlohmann::json fields_;
};

} // namespace

// ================================================================
// 3. MODULE
// ================================================================

RentalModule::RentalModule(ModelFactory&     models,
                           ServiceFactory&   services,
                           ViewModelFactory& viewModels,
                           ViewFactory&      views)
    : models_(models), services_(services), viewModels_(viewModels), views_(views) {}

std::string RentalModule::moduleName() const { return "rental"; }
std::string RentalModule::version()    const { return "19.0.1.0.0"; }

std::vector<std::string> RentalModule::dependencies() const {
    // account: invoices, taxes, payment allocation.
    // product is NOT a dependency — a rental unit is not a product, and
    // making it one would drag stock valuation into a lettings business.
    return {"base", "account"};
}

void RentalModule::registerModels() {
    auto db = services_.db();
    models_.registerCreator("rental.unit.type",        [db]{ return std::make_shared<RentalUnitType>(db); });
    models_.registerCreator("rental.unit",             [db]{ return std::make_shared<RentalUnit>(db); });
    models_.registerCreator("rental.contract",         [db]{ return std::make_shared<RentalContract>(db); });
    models_.registerCreator("rental.contract.line",    [db]{ return std::make_shared<RentalContractLine>(db); });
    models_.registerCreator("rental.expense.category", [db]{ return std::make_shared<RentalExpenseCategory>(db); });
    models_.registerCreator("rental.expense",          [db]{ return std::make_shared<RentalExpense>(db); });
    models_.registerCreator("rental.event",            [db]{ return std::make_shared<RentalEvent>(db); });
}

void RentalModule::registerServices() {}

void RentalModule::registerViewModels() {
    auto db = services_.db();
    // GenericViewModel throughout for now. Per docs/040 §1.2 the module
    // must INHERIT the audited, rule-enforcing, OCC-guarded path rather
    // than hand-rolling ViewModels — Container::verifyViewModelCompliance_
    // enforces that at boot, and an allowlist entry is not being added.
    //
    // Contract and line get custom ViewModels in phase 4, when they need
    // lifecycle actions; those will still derive from the audited base.
    viewModels_.registerCreator("rental.unit.type",        [db]{ return std::make_shared<GenericViewModel<RentalUnitType>>(db); });
    viewModels_.registerCreator("rental.unit",             [db]{ return std::make_shared<GenericViewModel<RentalUnit>>(db); });
    viewModels_.registerCreator("rental.contract",         [db]{ return std::make_shared<GenericViewModel<RentalContract>>(db); });
    viewModels_.registerCreator("rental.contract.line",    [db]{ return std::make_shared<GenericViewModel<RentalContractLine>>(db); });
    viewModels_.registerCreator("rental.expense.category", [db]{ return std::make_shared<GenericViewModel<RentalExpenseCategory>>(db); });
    viewModels_.registerCreator("rental.expense",          [db]{ return std::make_shared<GenericViewModel<RentalExpense>>(db); });
    viewModels_.registerCreator("rental.event",            [db]{ return std::make_shared<GenericViewModel<RentalEvent>>(db); });
}

void RentalModule::registerViews() {
    views_.registerCreator("rental.unit.list", []{
        return std::make_shared<SimpleView>("rental.unit.list", "rental.unit", "list",
            "<list string=\"Units\">"
            "<field name=\"code\"/><field name=\"name\"/><field name=\"type_id\"/>"
            "<field name=\"zone\"/><field name=\"state\"/>"
            "</list>",
            nlohmann::json{
                {"code",    {{"type","char"},     {"string","Code"}}},
                {"name",    {{"type","char"},     {"string","Name"}}},
                {"type_id", {{"type","many2one"}, {"string","Unit Type"}}},
                {"zone",    {{"type","char"},     {"string","Zone"}}},
                {"state",   {{"type","char"},     {"string","State"}}},
            });
    });
    views_.registerCreator("rental.unit.form", []{
        return std::make_shared<SimpleView>("rental.unit.form", "rental.unit", "form",
            "<form string=\"Unit\">"
            "<field name=\"code\"/><field name=\"name\"/><field name=\"type_id\"/>"
            "<field name=\"site\"/><field name=\"zone\"/><field name=\"floor\"/>"
            "<field name=\"area_sqm\"/><field name=\"volume_m3\"/>"
            "<field name=\"state\"/><field name=\"notes\"/>"
            "</form>",
            nlohmann::json{
                {"code",      {{"type","char"},     {"string","Code"}}},
                {"name",      {{"type","char"},     {"string","Name"}}},
                {"type_id",   {{"type","many2one"}, {"string","Unit Type"}}},
                {"site",      {{"type","char"},     {"string","Site"}}},
                {"zone",      {{"type","char"},     {"string","Zone"}}},
                {"floor",     {{"type","char"},     {"string","Floor"}}},
                {"area_sqm",  {{"type","float"},    {"string","Area (m²)"}}},
                {"volume_m3", {{"type","float"},    {"string","Volume (m³)"}}},
                {"state",     {{"type","char"},     {"string","State"}}},
                {"notes",     {{"type","text"},     {"string","Notes"}}},
            });
    });
    views_.registerCreator("rental.unit.type.list", []{
        return std::make_shared<SimpleView>("rental.unit.type.list", "rental.unit.type", "list",
            "<list string=\"Unit Types\">"
            "<field name=\"name\"/><field name=\"code\"/>"
            "<field name=\"default_rate\"/><field name=\"default_period\"/>"
            "</list>",
            nlohmann::json{
                {"name",           {{"type","char"},     {"string","Name"}}},
                {"code",           {{"type","char"},     {"string","Code"}}},
                {"default_rate",   {{"type","monetary"}, {"string","Default Rate"}}},
                {"default_period", {{"type","char"},     {"string","Period"}}},
            });
    });
    // The contract form.
    //
    // Without a registered form view the arch is literally "<form/>", so the
    // client had no declared order and fell back to whatever order the fields
    // JSON happened to arrive in — alphabetical. The screen opened with
    // "Active", buried the Customer below a full-width Notes box, and showed
    // "Company" (the multi-company OWNER of the row) directly above
    // "Customer", which reads as though a contract has two companies.
    //
    // So: identity first, then the billing terms, then money, then the notes,
    // and the unit lines last. company_id is NOT here — it is stamped from the
    // session on create (docs/094) and is not the user's to choose.
    views_.registerCreator("rental.contract.form", []{
        return std::make_shared<SimpleView>("rental.contract.form", "rental.contract", "form",
            "<form string=\"Contract\">"
            // Who and when
            "<field name=\"name\"/><field name=\"partner_id\"/>"
            "<field name=\"date_start\"/><field name=\"state\"/>"
            // How often it bills
            "<field name=\"billing_period\"/>"
            "<field name=\"billing_interval\"/><field name=\"billing_unit\"/>"
            "<field name=\"billing_lead_days\"/>"
            // Money
            "<field name=\"payment_term_id\"/><field name=\"journal_id\"/>"
            "<field name=\"currency_id\"/>"
            "<field name=\"deposit_amount\"/><field name=\"deposit_state\"/>"
            // Ending, then the free text, then the units
            "<field name=\"date_cancelled\"/><field name=\"active\"/>"
            "<field name=\"notes\"/>"
            "<field name=\"line_ids\"/>"
            "</form>",
            nlohmann::json{
                {"name",           {{"type","char"},     {"string","Reference"}}},
                {"partner_id",     {{"type","many2one"}, {"string","Customer"},
                                    {"relation","res.partner"}}},
                {"date_start",     {{"type","date"},     {"string","Start Date"}}},
                {"state",          {{"type","selection"},{"string","Status"},
                                    {"selection", nlohmann::json::array({
                                        nlohmann::json::array({"draft","Draft"}),
                                        nlohmann::json::array({"active","Active"}),
                                        nlohmann::json::array({"cancelled","Cancelled"}),
                                        nlohmann::json::array({"closed","Closed"})})}}},
                {"billing_period", {{"type","selection"},{"string","Billing Period"},
                                    {"selection", nlohmann::json::array({
                                        nlohmann::json::array({"daily","Daily"}),
                                        nlohmann::json::array({"weekly","Weekly"}),
                                        nlohmann::json::array({"monthly","Monthly"}),
                                        nlohmann::json::array({"quarterly","Quarterly (3 months)"}),
                                        nlohmann::json::array({"biannual","Every 6 months"}),
                                        nlohmann::json::array({"yearly","Yearly"}),
                                        nlohmann::json::array({"custom","Custom — every X…"}),
                                        nlohmann::json::array({"oneoff","One off"}),
                                        nlohmann::json::array({"ondemand","On demand"})})}}},
                {"billing_interval",{{"type","integer"}, {"string","Every"}}},
                {"billing_unit",   {{"type","selection"},{"string","Period Unit"},
                                    {"selection", nlohmann::json::array({
                                        nlohmann::json::array({"day","Day(s)"}),
                                        nlohmann::json::array({"week","Week(s)"}),
                                        nlohmann::json::array({"month","Month(s)"}),
                                        nlohmann::json::array({"year","Year(s)"})})}}},
                {"billing_lead_days",{{"type","integer"},{"string","Invoice Lead Days"}}},
                {"payment_term_id",{{"type","many2one"}, {"string","Payment Terms"},
                                    {"relation","account.payment.term"}}},
                {"journal_id",     {{"type","many2one"}, {"string","Journal"},
                                    {"relation","account.journal"}}},
                {"currency_id",    {{"type","many2one"}, {"string","Currency"},
                                    {"relation","res.currency"}}},
                {"deposit_amount", {{"type","monetary"}, {"string","Deposit"}}},
                {"deposit_state",  {{"type","selection"},{"string","Deposit Status"},
                                    {"selection", nlohmann::json::array({
                                        nlohmann::json::array({"none","Not held"}),
                                        nlohmann::json::array({"held","Held"}),
                                        nlohmann::json::array({"refunded","Refunded"}),
                                        nlohmann::json::array({"forfeited","Forfeited"})})}}},
                {"date_cancelled", {{"type","date"},     {"string","Cancelled On"}}},
                {"active",         {{"type","boolean"},  {"string","Active"}}},
                {"notes",          {{"type","text"},     {"string","Notes"}}},
                {"line_ids",       {{"type","one2many"}, {"string","Units"},
                                    {"relation","rental.contract.line"},
                                    {"relation_field","contract_id"}}},
            });
    });
    views_.registerCreator("rental.contract.list", []{
        return std::make_shared<SimpleView>("rental.contract.list", "rental.contract", "list",
            "<list string=\"Contracts\">"
            "<field name=\"name\"/><field name=\"partner_id\"/>"
            "<field name=\"date_start\"/><field name=\"billing_period\"/><field name=\"state\"/>"
            "</list>",
            nlohmann::json{
                {"name",           {{"type","char"},     {"string","Reference"}}},
                {"partner_id",     {{"type","many2one"}, {"string","Customer"}}},
                {"date_start",     {{"type","date"},     {"string","Start"}}},
                {"billing_period", {{"type","char"},     {"string","Period"}}},
                {"state",          {{"type","char"},     {"string","Status"}}},
            });
    });
    views_.registerCreator("rental.expense.list", []{
        return std::make_shared<SimpleView>("rental.expense.list", "rental.expense", "list",
            "<list string=\"Expenses\">"
            "<field name=\"date\"/><field name=\"name\"/><field name=\"category_id\"/>"
            "<field name=\"amount\"/><field name=\"is_recurring\"/><field name=\"state\"/>"
            "</list>",
            nlohmann::json{
                {"date",         {{"type","date"},     {"string","Date"}}},
                {"name",         {{"type","char"},     {"string","Description"}}},
                {"category_id",  {{"type","many2one"}, {"string","Category"}}},
                {"amount",       {{"type","monetary"}, {"string","Amount"}}},
                {"is_recurring", {{"type","boolean"},  {"string","Recurring"}}},
                {"state",        {{"type","char"},     {"string","Status"}}},
            });
    });
}

void RentalModule::registerRoutes() {
    auto db = services_.db();
    // SEC-28: captured ONCE, outside the lambda.
    const bool devMode = services_.devMode();
    auto sessions = services_.sessions();

    // Every route below mutates or discloses business data, so every one
    // of them authenticates.
    //
    // The first cut of these routes had NO auth at all: /rental/billing/run
    // would create invoices, and /rental/dashboard would disclose MRR and
    // receivables, to anyone who could reach the port. Loopback binding
    // and nginx are not access control — they decide who can knock, not
    // who gets in. Same checkAuth shape ReportModule uses.
    auto checkAuth = [sessions](const drogon::HttpRequestPtr& req) -> bool {
        if (!sessions) return false;
        const std::string sid = req->getCookie(SessionManager::cookieName());
        if (sid.empty()) return false;
        auto s = sessions->get(sid);
        return s.has_value() && s->isAuthenticated();
    };
    auto unauthorized = []() -> drogon::HttpResponsePtr {
        auto r = drogon::HttpResponse::newHttpResponse();
        r->setStatusCode(drogon::k401Unauthorized);
        r->setContentTypeCode(drogon::CT_APPLICATION_JSON);
        r->setBody(nlohmann::json{{"error", "Not authenticated"}}.dump());
        return r;
    };

    // "Generate invoices now" — the SAME RentalBilling::run the cron
    // calls, differing only by the as-of date. A manual path with its own
    // implementation is how double-billing gets discovered in production.
    drogon::app().registerHandler(
        "/rental/billing/run",
        [db, devMode, checkAuth, unauthorized](
            const drogon::HttpRequestPtr& req,
            std::function<void(const drogon::HttpResponsePtr&)>&& cb) {
            if (!checkAuth(req)) { cb(unauthorized()); return; }
            auto json = [&cb](int code, const nlohmann::json& body) {
                auto r = drogon::HttpResponse::newHttpResponse();
                r->setStatusCode(static_cast<drogon::HttpStatusCode>(code));
                r->setContentTypeCode(drogon::CT_APPLICATION_JSON);
                r->setBody(body.dump());
                cb(r);
            };
            try {
                // The date is echoed into SQL only as a bound parameter and
                // is validated as a date by PostgreSQL itself; it is never
                // concatenated (SEC-30).
                std::string asOf;
                auto p = req->getOptionalParameter<std::string>("date");
                if (p) asOf = *p;

                const auto r = RentalBilling::run(db, asOf);
                json(200, nlohmann::json{
                    {"invoices_created", r.invoicesCreated},
                    {"lines_billed",     r.linesBilled},
                    {"groups_skipped",   r.groupsSkipped},
                    {"groups_failed",    r.groupsFailed},
                    {"move_ids",         r.moveIds},
                    {"errors",           devMode ? r.errors : std::vector<std::string>{}}
                });
            } catch (const PoolExhaustedException& e) {
                LOG_ERROR << "[rental/billing] pool: " << e.what();
                json(503, {{"error", "The server is temporarily overloaded. Please retry."}});
            } catch (const std::exception& e) {
                LOG_ERROR << "[rental/billing/run] " << e.what();
                json(500, {{"error", devMode ? e.what() : "An internal error occurred"}});
            }
        },
        {drogon::Post});

    // Generate the dated occurrences of recurring expenses. Same shape as
    // the billing action: one code path, shared with the cron.
    drogon::app().registerHandler(
        "/rental/expenses/generate",
        [db, devMode, checkAuth, unauthorized](
            const drogon::HttpRequestPtr& req,
            std::function<void(const drogon::HttpResponsePtr&)>&& cb) {
            if (!checkAuth(req)) { cb(unauthorized()); return; }
            auto json = [&cb](int code, const nlohmann::json& body) {
                auto r = drogon::HttpResponse::newHttpResponse();
                r->setStatusCode(static_cast<drogon::HttpStatusCode>(code));
                r->setContentTypeCode(drogon::CT_APPLICATION_JSON);
                r->setBody(body.dump());
                cb(r);
            };
            try {
                std::string asOf;
                auto p = req->getOptionalParameter<std::string>("date");
                if (p) asOf = *p;
                const auto r = RentalExpenses::generate(db, asOf);
                json(200, nlohmann::json{
                    {"generated", r.generated},
                    {"skipped",   r.skipped},
                    {"failed",    r.failed},
                    {"errors",    devMode ? r.errors : std::vector<std::string>{}}
                });
            } catch (const PoolExhaustedException& e) {
                LOG_ERROR << "[rental/expenses] pool: " << e.what();
                json(503, {{"error", "The server is temporarily overloaded. Please retry."}});
            } catch (const std::exception& e) {
                LOG_ERROR << "[rental/expenses/generate] " << e.what();
                json(500, {{"error", devMode ? e.what() : "An internal error occurred"}});
            }
        },
        {drogon::Post});

    // Cashflow forecast. One endpoint returning the whole payload rather
    // than N search_read calls assembled in the browser — docs/040 §3.4.
    drogon::app().registerHandler(
        "/rental/cashflow",
        [db, devMode, checkAuth, unauthorized](
            const drogon::HttpRequestPtr& req,
            std::function<void(const drogon::HttpResponsePtr&)>&& cb) {
            if (!checkAuth(req)) { cb(unauthorized()); return; }
            auto json = [&cb](int code, const nlohmann::json& body) {
                auto r = drogon::HttpResponse::newHttpResponse();
                r->setStatusCode(static_cast<drogon::HttpStatusCode>(code));
                r->setContentTypeCode(drogon::CT_APPLICATION_JSON);
                r->setBody(body.dump());
                cb(r);
            };
            try {
                int months = 12;
                if (auto m = req->getOptionalParameter<std::string>("months")) {
                    // SEC-30: never trust that a parse succeeded means the
                    // input was numeric; the value is clamped downstream
                    // regardless.
                    try { months = std::stoi(*m); } catch (...) { months = 12; }
                }
                std::string from;
                if (auto f = req->getOptionalParameter<std::string>("from")) from = *f;

                json(200, RentalForecast::cashflow(db, months, from));
            } catch (const PoolExhaustedException& e) {
                LOG_ERROR << "[rental/cashflow] pool: " << e.what();
                json(503, {{"error", "The server is temporarily overloaded. Please retry."}});
            } catch (const std::exception& e) {
                LOG_ERROR << "[rental/cashflow] " << e.what();
                json(500, {{"error", devMode ? e.what() : "An internal error occurred"}});
            }
        },
        {drogon::Get});

    // ----------------------------------------------------------
    // The Booking screen (docs: rental booking calendar).
    //
    // Two routes, both authenticated like everything else here:
    //   GET  /rental/calendar?month=YYYY-MM[&type_id=N]   day-level occupancy
    //   POST /rental/booking/create?unit_id=..&partner_id=..&date_start=..
    //
    // Parameters travel as query/form values, not a JSON body, because that
    // is the shape every other route in this module already uses
    // (/rental/billing/run?date=). One idiom is worth more than a marginally
    // tidier payload.
    // ----------------------------------------------------------
    auto companyOf = [sessions](const drogon::HttpRequestPtr& req) -> int {
        if (!sessions) return 0;
        auto s = sessions->get(req->getCookie(SessionManager::cookieName()));
        return (s.has_value() && s->isAuthenticated()) ? s->companyId : 0;
    };

    drogon::app().registerHandler(
        "/rental/calendar",
        [db, devMode, checkAuth, unauthorized, companyOf](
            const drogon::HttpRequestPtr& req,
            std::function<void(const drogon::HttpResponsePtr&)>&& cb) {
            if (!checkAuth(req)) { cb(unauthorized()); return; }
            auto json = [&cb](int code, const nlohmann::json& body) {
                auto r = drogon::HttpResponse::newHttpResponse();
                r->setStatusCode(static_cast<drogon::HttpStatusCode>(code));
                r->setContentTypeCode(drogon::CT_APPLICATION_JSON);
                r->setBody(body.dump());
                cb(r);
            };
            try {
                std::string ym;
                if (auto m = req->getOptionalParameter<std::string>("month")) ym = *m;
                int typeId = 0;
                if (auto t = req->getOptionalParameter<std::string>("type_id")) {
                    try { typeId = std::stoi(*t); } catch (...) { typeId = 0; }
                }
                json(200, RentalCalendar::month(db, ym, companyOf(req), typeId));
            } catch (const PoolExhaustedException& e) {
                LOG_ERROR << "[rental/calendar] pool: " << e.what();
                json(503, {{"error", "The server is temporarily overloaded. Please retry."}});
            } catch (const std::exception& e) {
                LOG_ERROR << "[rental/calendar] " << e.what();
                json(500, {{"error", devMode ? e.what() : "An internal error occurred"}});
            }
        },
        {drogon::Get});

    drogon::app().registerHandler(
        "/rental/booking/create",
        [db, devMode, checkAuth, unauthorized, companyOf](
            const drogon::HttpRequestPtr& req,
            std::function<void(const drogon::HttpResponsePtr&)>&& cb) {
            if (!checkAuth(req)) { cb(unauthorized()); return; }
            auto json = [&cb](int code, const nlohmann::json& body) {
                auto r = drogon::HttpResponse::newHttpResponse();
                r->setStatusCode(static_cast<drogon::HttpStatusCode>(code));
                r->setContentTypeCode(drogon::CT_APPLICATION_JSON);
                r->setBody(body.dump());
                cb(r);
            };
            auto p = [&req](const char* k) -> std::string {
                auto v = req->getOptionalParameter<std::string>(k);
                return v ? *v : std::string();
            };
            auto num = [&p](const char* k, int dflt) {
                const auto s = p(k);
                if (s.empty()) return dflt;
                try { return std::stoi(s); } catch (...) { return dflt; }
            };
            try {
                RentalCalendar::BookRequest br;
                br.unitId     = num("unit_id", 0);
                br.partnerId  = num("partner_id", 0);
                br.contractId = num("contract_id", 0);
                br.dateStart  = p("date_start");
                br.dateEnd    = p("date_end");
                br.billingMode = p("billing_mode");
                const auto price = p("unit_price");
                if (!price.empty()) {
                    try { br.unitPrice = std::stod(price); } catch (...) { br.unitPrice = -1.0; }
                }
                br.companyId = companyOf(req);
                json(200, RentalCalendar::book(db, br));
            } catch (const ValidationError& e) {
                // The operator's own mistake — overlapping dates, no customer,
                // a retired unit. 400 with the reason, always passed through:
                // these messages are written for the screen and contain no
                // schema detail (SEC-28).
                json(400, {{"error", e.what()}});
            } catch (const PoolExhaustedException& e) {
                LOG_ERROR << "[rental/booking] pool: " << e.what();
                json(503, {{"error", "The server is temporarily overloaded. Please retry."}});
            } catch (const std::exception& e) {
                LOG_ERROR << "[rental/booking/create] " << e.what();
                json(500, {{"error", devMode ? e.what() : "An internal error occurred"}});
            }
        },
        {drogon::Post});

    // The dashboard: ONE endpoint returning the whole payload, cached
    // 60 s. Not a dozen search_read calls assembled in the browser —
    // docs/040 §3.4.
    drogon::app().registerHandler(
        "/rental/dashboard",
        [db, devMode, checkAuth, unauthorized](
            const drogon::HttpRequestPtr& req,
            std::function<void(const drogon::HttpResponsePtr&)>&& cb) {
            if (!checkAuth(req)) { cb(unauthorized()); return; }
            auto json = [&cb](int code, const nlohmann::json& body) {
                auto r = drogon::HttpResponse::newHttpResponse();
                r->setStatusCode(static_cast<drogon::HttpStatusCode>(code));
                r->setContentTypeCode(drogon::CT_APPLICATION_JSON);
                r->setBody(body.dump());
                cb(r);
            };
            try {
                int months = 12;
                if (auto m = req->getOptionalParameter<std::string>("months")) {
                    try { months = std::stoi(*m); } catch (...) { months = 12; }
                }
                const bool fresh =
                    req->getOptionalParameter<std::string>("fresh").has_value();
                json(200, RentalDashboard::build(db, months, fresh));
            } catch (const PoolExhaustedException& e) {
                LOG_ERROR << "[rental/dashboard] pool: " << e.what();
                json(503, {{"error", "The server is temporarily overloaded. Please retry."}});
            } catch (const std::exception& e) {
                LOG_ERROR << "[rental/dashboard] " << e.what();
                json(500, {{"error", devMode ? e.what() : "An internal error occurred"}});
            }
        },
        {drogon::Get});

    // ----------------------------------------------------------
    // Demo data — Settings -> Technical -> Demo Data.
    //
    // /clear is DESTRUCTIVE, so three things hold:
    //   * it authenticates, like everything else here
    //   * /status reports exactly what exists, so the UI can show what is
    //     about to be removed instead of asking for blind confirmation
    //   * what counts as demo data is defined once in RentalDemo, so seed
    //     and clear can never disagree about what they own
    // ----------------------------------------------------------
    auto demoRoute = [db, devMode, checkAuth, unauthorized](
        const char* what,
        std::function<nlohmann::json(std::shared_ptr<DbConnection>)> fn) {
        return [db, devMode, checkAuth, unauthorized, what, fn](
            const drogon::HttpRequestPtr& req,
            std::function<void(const drogon::HttpResponsePtr&)>&& cb) {
            auto json = [&cb](int code, const nlohmann::json& body) {
                auto r = drogon::HttpResponse::newHttpResponse();
                r->setStatusCode(static_cast<drogon::HttpStatusCode>(code));
                r->setContentTypeCode(drogon::CT_APPLICATION_JSON);
                r->setBody(body.dump());
                cb(r);
            };
            if (!checkAuth(req)) { cb(unauthorized()); return; }
            try {
                json(200, fn(db));
            } catch (const PoolExhaustedException& e) {
                LOG_ERROR << "[rental/demo] pool: " << e.what();
                json(503, {{"error", "The server is temporarily overloaded. Please retry."}});
            } catch (const std::exception& e) {
                LOG_ERROR << "[rental/demo/" << what << "] " << e.what();
                json(500, {{"error", devMode ? e.what() : "An internal error occurred"}});
            }
        };
    };

    drogon::app().registerHandler("/rental/demo/status",
        demoRoute("status", &RentalDemo::status), {drogon::Get});
    // POST for both mutations: a GET that changes data can be triggered by
    // a link, a prefetch or a crawler.
    drogon::app().registerHandler("/rental/demo/seed",
        demoRoute("seed",  &RentalDemo::seed),  {drogon::Post});
    drogon::app().registerHandler("/rental/demo/clear",
        demoRoute("clear", &RentalDemo::clear), {drogon::Post});
}

void RentalModule::registerMigrations(cerp::infrastructure::MigrationRunner& runner) {
    registerRentalMigrations(runner);
}

void RentalModule::initialize() {
    seedActions_();
    seedMenus_();

    // Bind the cron handler, then activate the job. Migration 810 leaves
    // it inactive on purpose — a job with no handler logs "no handler"
    // every tick. Activation belongs here, where the handler now exists.
    RentalBilling::registerCron(services_.db());
    RentalExpenses::registerCron(services_.db());
    {
        auto conn = services_.db()->acquire();
        pqxx::work txn{conn.get()};
        txn.exec("UPDATE ir_cron SET active = TRUE, write_date = now() "
                 " WHERE code IN ('rental.billing','rental.expenses') AND NOT active");
        txn.commit();
    }
}

// ----------------------------------------------------------
// Actions and menus
// ----------------------------------------------------------
void RentalModule::seedActions_() {
    auto conn = services_.db()->acquire();
    pqxx::work txn{conn.get()};
    txn.exec(R"SQL(
        INSERT INTO ir_act_window (id, name, res_model, view_mode) VALUES
            -- res_model is the CUSTOM_VIEWS key, not a real model: the
            -- dashboard is a view over several models and has no table of
            -- its own. ActionView dispatches on the string.
            (39, 'Dashboard',  'rental.dashboard',       'list'),
            (40, 'Units',      'rental.unit',            'list,form'),
            (41, 'Unit Types', 'rental.unit.type',       'list,form'),
            (42, 'Contracts',  'rental.contract',        'list,form'),
            (43, 'Expenses',   'rental.expense',         'list,form'),
            (44, 'Categories', 'rental.expense.category','list,form'),
            (45, 'Events',     'rental.event',           'list'),
            (46, 'Demo Data',  'rental.demo.data',       'list'),
            -- 127, not 47: the rental block 39-46 is full and 47 is
            -- Barcode. tests/integration/core/menu-ids prints the next
            -- free id in each space, which is how this one was chosen.
            (127, 'Booking',   'rental.booking',        'list')
        ON CONFLICT (id) DO UPDATE
            SET res_model = EXCLUDED.res_model,
                view_mode = EXCLUDED.view_mode
    )SQL");
    txn.exec("SELECT setval('ir_act_window_id_seq', (SELECT MAX(id) FROM ir_act_window), true)");
    txn.commit();
}

void RentalModule::seedMenus_() {
    auto conn = services_.db()->acquire();
    pqxx::work txn{conn.get()};

    // 300-series: the existing menu ids run to 203, so this leaves room
    // for the modules in between to grow without a collision.
    txn.exec(R"SQL(
        INSERT INTO ir_ui_menu (id, name, parent_id, sequence, action_id, web_icon) VALUES
            (300, 'Rental', NULL, 45, NULL, 'rental')
        ON CONFLICT (id) DO NOTHING
    )SQL");
    txn.exec(R"SQL(
        INSERT INTO ir_ui_menu (id, name, parent_id, sequence, action_id) VALUES
            (310, 'Operations',    300, 10, NULL),
            (320, 'Configuration', 300, 90, NULL)
        ON CONFLICT (id) DO NOTHING
    )SQL");
    txn.exec(R"SQL(
        INSERT INTO ir_ui_menu (id, name, parent_id, sequence, action_id) VALUES
            (309, 'Dashboard',  310,  5, 39),
            (315, 'Booking',    310,  7, 127),
            (311, 'Units',      310, 10, 40),
            (312, 'Contracts',  310, 20, 42),
            (313, 'Expenses',   310, 30, 43),
            (314, 'Events',     310, 40, 45),
            (321, 'Unit Types', 320, 10, 41),
            (322, 'Expense Categories', 320, 20, 44)
        ON CONFLICT (id) DO UPDATE SET action_id = EXCLUDED.action_id
    )SQL");
    // Demo Data lives under Settings -> Technical (menu 101), not under
    // Rental: it is an administrative tool for evaluating the module, not
    // part of running a facility. Putting a "delete everything" button in
    // the operator's daily navigation is asking for it to be pressed.
    txn.exec(R"SQL(
        INSERT INTO ir_ui_menu (id, name, parent_id, sequence, action_id) VALUES
            (330, 'Demo Data', 415, 30, 46)
        ON CONFLICT (id) DO UPDATE SET action_id = EXCLUDED.action_id,
                                       parent_id = EXCLUDED.parent_id
    )SQL");

    txn.exec("SELECT setval('ir_ui_menu_id_seq', (SELECT MAX(id) FROM ir_ui_menu), true)");
    txn.commit();
}

} // namespace cerp::modules::rental
