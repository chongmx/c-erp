#pragma once
// ============================================================
// core/MoneyMigrations.hpp
//
// Schema migrations for P2 — money as int64 micro-units, configurable
// precision, and multi-currency. (docs/047 §5, docs/048)
//
// Version range 900–999, extending the table in docs/036 §3 which
// previously stopped at 799.
//
// Registered from BaseModule so it runs before the modules whose tables
// it rewrites. MigrationRunner applies in ascending version order inside
// one transaction each, and halts startup on failure.
//
// ORDERING MATTERS
//   901  reference data (decimal_precision, currency rate/base, FX account)
//   910  account_move_line   ← lines before headers: headers are recomputed
//   911  account_move           from the lines afterwards
//   912  account_payment
//   920  sale_order / sale_order_line
//   930  purchase_order / purchase_order_line
//   940  stock_move
//   950  product_product
//   960  mrp_bom_line
//
// VALUE PRESERVATION
//   `USING ROUND(col * 1000000)::BIGINT` converts in place, losslessly at
//   the row counts involved. docs/047 §2.2 recorded that price data was
//   disposable; it turns out it does not need to be discarded.
// ============================================================
#include <string>

namespace cerp::infrastructure { class MigrationRunner; }

namespace cerp::core {

/// Register every P2 migration on the runner.
void registerMoneyMigrations(cerp::infrastructure::MigrationRunner& runner);

} // namespace cerp::core
