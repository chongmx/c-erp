# Executed Plan 3 — Phases 13–17g, 27, A3, Stock+, MRP-BOM, Security Hardening (2026)

Reference history:
- executed-plan-1.md — Phases 1–8 (infra, base, auth, IR, account, UOM, product, frontend)
- executed-plan-2.md — Phases 9–12 (sale, purchase, HR, stock-seed)

This file covers everything completed up to 2026-03-23.

---

## Phase 13 — ir_config_parameter

- Table: ir_config_parameter (key/value store)
- IrConfigParameterViewModel — CRUD
- Seeded system parameters (web.base.url, etc.)
- Doc: 010-phase13-14-progress.md

---

## Phase 14 — auth_signup (Partial)

- res_users_signup_token table
- Email token flow skeleton written
- BLOCKED on ir.mail_server (SMTP) — not wired
- Auth signup form in portal — untested
- Status: code written, NOT tested
- Doc: 010-phase13-14-progress.md

---

## Phase 15 — Stock MVP

- 4 tables: stock_location, stock_picking_type, stock_picking, stock_move
- StockPickingViewModel: action_confirm, action_assign, button_validate
  - button_validate updates sale_order_line.qty_delivered and purchase_order_line.qty_received
- StockMoveViewModel: GenericViewModel<StockMove>
- Sequences: WH/IN/YYYY/NNNN, WH/OUT/YYYY/NNNN
- Seeded: default warehouse, 3 op types, virtual locations
- Doc: 011-stock-module-progress.md

---

## Phase 16 — Stock ↔ Sale/Purchase Auto-Picking

- SaleOrderViewModel.action_confirm: auto-creates stock_picking + stock_move rows (delivery)
- PurchaseOrderViewModel.button_confirm: auto-creates stock_picking + stock_move rows (receipt)
- stock_picking.sale_id / purchase_id FK back-references
- Doc: 012-stock-sale-purchase-links-progress.md

---

## Phase 17a — OWL One2many Order Lines (SO/PO)

- SaleOrderFormView: editable order_line table with add/delete
- PurchaseOrderFormView: editable order_line table with add/delete
- Event delegation pattern (OWL 2 IIFE) for t-foreach handlers
- data-line-field / data-key / data-del-line / data-add-line attributes
- Doc: 013-order-lines-progress.md

---

## Phase 17b — TransferFormView

- Full transfer form: header fields, move lines table, status bar, action buttons
- action_confirm, action_assign, button_validate wired
- Chatter stub
- Doc: 014-transfer-invoice-po-forms-progress.md

---

## Phase 17c — SaleOrderFormView Stat Buttons

- Delivery count stat button: navigates to filtered stock.picking list
- Invoice count stat button: navigates to filtered account.move list
- Doc: 013-order-lines-progress.md

---

## Phase 17d — InvoiceFormView + PurchaseOrderFormView

- InvoiceFormView: full invoice with line editing, post/cancel/reset buttons
- PurchaseOrderFormView: full PO form with order lines
- Doc: 014-transfer-invoice-po-forms-progress.md

---

## Phase 17g — Transfer + Product Form Small Fixes

- Product form tabs: Sales / Purchase / Inventory / Accounting (structure + stub fields)
- Transfer form: scheduled_date, origin, note fields
- Fixed product.product fields: sale_ok, purchase_ok, type, uom_id
- Doc: 017-phase17g-form-fixes-progress.md

---

## Phase 27 — Chatter / Audit Log

- mail_message table (res_model, res_id, author_id, body, subtype, date)
- MailMessageViewModel: search_read + create
- postLog() helper called on all state transitions:
  SaleOrder, PurchaseOrder, AccountMove, AccountPayment, StockPicking
- Frontend ChatterPanel component in all 5 core form views
- MailHelpers.hpp: postLog() free function used across modules
- Doc: 018-phase27-chatter-progress.md

---

## Phase A3 — Product Form Tabs (Full)

- Sales tab: invoice_policy, description_sale fields
- Purchase tab: description_purchase field (product_supplierinfo deferred to A3b)
- Inventory tab: weight, volume, tracking (none/lot/serial)
- Accounting tab: income/expense account stubs
- Product stat widgets: On Hand, In Transfer, Forecasted
- Doc: 020-session-2026-03-23-progress.md

---

## Phase Stock+ — Warehouse + Location + Internal Transfer Views

- StockWarehouse model + view (Warehouses menu)
- Warehouse/Location configuration views in Inventory → Configuration
- Internal Transfers (picking_type_id=3) separate menu entry
- Receipts (id=1), Deliveries (id=2) action windows with domain filters
- Move History: Inventory → Reporting → Move History (stock.move list)
- Doc: 020-session-2026-03-23-progress.md

---

## Phase 26-BOM — MRP Bills of Materials

- mrp_bom + mrp_bom_line tables
- MrpBomViewModel: CRUD + One2many bom lines
- BomFormView: header + line table
- Manufacturing app menu: Bills of Materials leaf
- BOM stat widget on ProductFormView
- Doc: 020-session-2026-03-23-progress.md

---

## Security Audit v1/v2 — Portal + Core

Docs: 021-security-fixes-progress.md, 022-portal-dle-decimal-progress.md, 027-security-perf-portal-audit.md

### SEC-16 [HIGH] Path traversal in payment proof upload — FIXED
- Basename extraction (strip / and \), extension allowlist (.pdf/.jpg/.jpeg/.png), size limit 10MB

### SEC-17 [HIGH] e.what() leaked to portal clients — FIXED
- devMode/secureCookies added to ServiceFactory + Container
- All 14 portal catch blocks now conditional: devMode ? e.what() : "An internal error occurred"

### SEC-18 [MEDIUM] Portal cookie missing Secure flag — FIXED
- if (secureCookies) cookie.setSecure(true) added to portal login handler

### SEC-19 [MEDIUM] No file size/type validation on upload — FIXED
- Combined with SEC-16 fix

### PERF-01 [HIGH] Single std::mutex serializing all session lookups — FIXED
- SessionManager upgraded to std::shared_mutex + two-phase get() with 60s kTouchInterval_

### PERF-02 [HIGH] FieldRegistry rebuilt per request — FIXED
- inline static FieldRegistry + std::call_once in BaseModel<TDerived>

---

## Security Audit v3 — Authorization + WebSocket (2026-03-23)

Doc: 028-security-perf-audit-2.md

### SEC-23 [HIGH] No authorization on res.users write endpoints — FIXED
- callerSession_() helper resolves session via shared SessionManager
- handleCreate + handleUnlink: require isAdmin || SETTINGS_CONFIGURATION group
- handleSearchRead: require isAdmin || SETTINGS_CONFIGURATION
- handleWrite: non-admin can only write own record; groups_id and password blocked
- Self-deletion guard in handleUnlink

### SEC-04 [HIGH] No model-level access check in dispatcher — FIXED
- checkModelAccess_(model, session) in JsonRpcDispatcher::handleCallKw_()
- Static map: account.move→ACCOUNT_BILLING, hr.employee→HR_EMPLOYEE, stock.*→INVENTORY_USER, etc.

### SEC-24/PERF-09 [MEDIUM] PortalSessionManager plain std::mutex — FIXED
- Upgraded to std::shared_mutex + two-phase get() + kTouchInterval_=60s

### SEC-11 [MEDIUM] Unauthenticated WebSocket endpoint — FIXED
- BusController::handleNewConnection validates session_id cookie
- WebSocketServer takes shared_ptr<SessionManager>; Container reordered

### PERF-07 Rate limiter prune on every call — FIXED
- lastPrune_ timestamp; prune() throttled to once per kWindowSeconds

### PERF-08 ostringstream for hex generation — FIXED
- Direct lookup table kHex[] in both SessionManager and PortalSessionManager

### Bug: SEC-23 always denied admin — FIXED
- AuthModule::registerViewModels() created a new empty SessionManager per request
- ServiceFactory now carries shared_ptr<SessionManager> (sf.sessions())
- All ViewModel creators that need the session store capture sf.sessions()

---

## User & Group Management UI

Doc: 024-user-groups-progress.md, 025-user-group-management-ui-progress.md

- Groups.hpp: 16 named group constants (BASE_PUBLIC through HR_MANAGER)
- AuthModule::seedGroups_(): idempotent group seeding with ON CONFLICT
- Session::groupIds populated on authenticate; hasGroup() / hasAnyGroup() helpers
- Settings → Users: full list view with company filter, group filter, activation toggle
- Settings → Groups: list view
- UsersFormView: group multi-checkbox assignment
- Portal Users filter fix: qualified rp.company_id to avoid SQL ambiguity
- company_id backfill migration: matches company_name text to res_partner IS_COMPANY

---

## Inventory — Move History Fixes (2026-03-23)

- Removed LEFT JOIN product_template (table does not exist in this codebase)
- Changed pt.name → pp.name (product name on product_product directly)
- Added StockMoveListView: 8 columns, excludes Description/name column
- Registered as "stock.move.list"

---

## Build Warning Fixes

- AuthViewModel.hpp: all exec_params( → exec( (9 instances)
- AuthModule.hpp: all exec_params( → exec( (9 instances)

---

## Deferred from This Period

| Item | Status |
|------|--------|
| PERF-03 (single TU compile) | Valid, architectural — deferred |
| PERF-04 (fields_get/get_views caching) | Valid — deferred |
| PERF-05 (JSON deep-copy) | Minor — deferred |
| PERF-06 (portal sequential SQL) | Valid — deferred |
| Phase 14 auth_signup | Blocked on SMTP |
| Phase 17e PDF reports | Planned next |
| Phase A3b vendor pricelist | Planned next |
| Phase A2 register payment | Planned next |
