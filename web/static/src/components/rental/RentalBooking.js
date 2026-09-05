/**
 * RentalBooking.js — the booking calendar: what is let, when, and by whom.
 *
 * Three views of the same month, chosen from the sidebar:
 *
 *   All units    every unit, one day-strip each, grouped by type
 *   A type       the same, narrowed — the "how is this category doing" view
 *   One unit     a month grid, its status, and the bookings behind it
 *
 * The strip is the point. Each box is a day; filled means let. Reading a row
 * left to right tells you at a glance whether a unit is solid, patchy or
 * sitting empty — which a percentage alone never does. Four units at 70% can
 * be four half-used lockers or three full ones and a dead one, and those are
 * completely different problems.
 *
 * Occupancy is NOT stored. The server derives it from the contract lines that
 * already drive billing (RentalCalendar.cpp), so what this draws is what will
 * be invoiced. There is no second source of truth to drift.
 *
 * Booking: click a free day, click another, press Book. That creates a real
 * rental.contract.line — the same record the contract form creates — so the
 * billing run, the unit-state derivation and the invoice link all keep working
 * with no knowledge of this screen.
 */
class RentalBooking extends owl.Component {
    // OWL resolves sub-components at first render: a missing entry here throws
    // only when a user opens this exact screen.
    static components = { M2OSelect };

    static template = owl.xml`
        <div class="rbk-wrap viz-root">

            <!-- ── toolbar ───────────────────────────────────────────── -->
            <div class="rbk-toolbar">
                <div class="rbk-month">
                    <button class="rbk-nav" t-on-click="prevMonth" title="Previous month">‹</button>
                    <span class="rbk-month-label" t-esc="monthLabel"/>
                    <button class="rbk-nav" t-on-click="nextMonth" title="Next month">›</button>
                    <button class="rbk-today" t-on-click="thisMonth">Today</button>
                </div>
                <div class="rbk-spacer"/>
                <div class="rbk-headline" t-if="state.data">
                    <span class="rbk-pct" t-esc="pct(state.data.totals.pct)"/>
                    <span class="rbk-pct-cap">occupied this month</span>
                    <span class="rbk-sub">
                        <t t-esc="state.data.totals.let_days"/> let-days of
                        <t t-esc="state.data.totals.possible"/>
                    </span>
                </div>
                <button class="rbk-nav" t-on-click="load" title="Refresh">⟳</button>
            </div>

            <t t-if="state.error">
                <div class="rbk-error" t-esc="state.error"/>
            </t>

            <div class="rbk-body">

                <!-- ── sidebar: the assets ───────────────────────────── -->
                <aside class="rbk-side">
                    <div t-attf-class="rbk-side-row rbk-side-all{{ state.sel.kind === 'all' ? ' sel' : '' }}"
                         data-pick="all" t-on-click="() => this.pickAll()">
                        <span class="rbk-side-name">All units</span>
                        <span class="rbk-side-count" t-if="state.data"
                              t-esc="state.data.totals.units"/>
                    </div>

                    <t t-foreach="types" t-as="ty" t-key="ty.id">
                        <div t-attf-class="rbk-side-row rbk-side-type{{ state.sel.kind === 'type' and state.sel.id === ty.id ? ' sel' : '' }}"
                             t-att-data-type="ty.id" t-on-click="() => this.pickType(ty.id)">
                            <span class="rbk-side-name" t-esc="ty.name"/>
                            <span t-attf-class="rbk-chip {{ chipClass(ty.pct) }}"
                                  t-esc="pct(ty.pct)"/>
                        </div>
                        <t t-foreach="unitsOfType(ty.id)" t-as="u" t-key="u.id">
                            <div t-attf-class="rbk-side-row rbk-side-unit{{ state.sel.kind === 'unit' and state.sel.id === u.id ? ' sel' : '' }}"
                                 t-att-data-unit="u.id" t-on-click="() => this.pickUnit(u.id)">
                                <span t-attf-class="rbk-dot rbk-dot-{{ u.state }}"/>
                                <span class="rbk-side-name" t-esc="u.code"/>
                                <span class="rbk-side-days" t-esc="u.let_days + '/' + state.data.days"/>
                            </div>
                        </t>
                    </t>
                </aside>

                <!-- ── main ──────────────────────────────────────────── -->
                <section class="rbk-main">
                    <t t-if="state.loading">
                        <div class="rental-empty">Loading the calendar…</div>
                    </t>
                    <t t-elif="!state.data">
                        <div class="rental-empty">No calendar to show.</div>
                    </t>
                    <t t-elif="!state.data.units.length">
                        <div class="rental-empty">
                            No units yet. Add one under Rental → Units, then book it here.
                        </div>
                    </t>

                    <!-- STRIPS: all units, or one category -->
                    <t t-elif="state.sel.kind !== 'unit'">
                        <div class="rbk-strip-head">
                            <h3 class="rbk-title" t-esc="stripTitle"/>
                            <div class="rbk-ruler">
                                <t t-foreach="dayNumbers" t-as="d" t-key="d">
                                    <span t-attf-class="rbk-rule{{ d % 5 === 0 ? ' mark' : '' }}"
                                          t-esc="d % 5 === 0 ? d : ''"/>
                                </t>
                            </div>
                        </div>

                        <div class="rbk-strips">
                            <t t-foreach="visibleUnits" t-as="u" t-key="u.id">
                                <div class="rbk-strip-row" t-att-data-strip="u.id">
                                    <div class="rbk-strip-label" t-on-click="() => this.pickUnit(u.id)">
                                        <span class="rbk-strip-code" t-esc="u.code"/>
                                        <span class="rbk-strip-name" t-esc="u.name"/>
                                    </div>
                                    <div class="rbk-boxes">
                                        <t t-foreach="u.days" t-as="d" t-key="d_index">
                                            <span t-attf-class="rbk-box{{ d ? ' let' : '' }}{{ isToday(d_index) ? ' today' : '' }}"
                                                  t-att-data-unit-day="u.id + ':' + dayIso(d_index)"
                                                  t-att-title="boxTitle(u, d_index)"/>
                                        </t>
                                    </div>
                                    <div class="rbk-strip-num">
                                        <span class="rbk-strip-days"
                                              t-esc="u.let_days + '/' + state.data.days"/>
                                        <span t-attf-class="rbk-chip {{ chipClass(u.pct) }}"
                                              t-esc="pct(u.pct)"/>
                                    </div>
                                </div>
                            </t>
                        </div>
                    </t>

                    <!-- ONE UNIT: the month grid and what is behind it -->
                    <t t-else="">
                        <t t-set="u" t-value="selectedUnit"/>
                        <t t-if="u">
                            <div class="rbk-unit-head">
                                <div>
                                    <h3 class="rbk-title">
                                        <t t-esc="u.code"/>
                                        <span class="rbk-unit-name" t-esc="u.name"/>
                                    </h3>
                                    <div class="rbk-unit-meta">
                                        <span t-attf-class="rbk-badge rbk-dot-{{ u.state }}"
                                              t-esc="u.state"/>
                                        <span class="rbk-unit-type" t-esc="u.type_name"/>
                                        <span class="rbk-unit-days">
                                            <t t-esc="u.let_days"/>/<t t-esc="state.data.days"/> days let
                                        </span>
                                        <span t-attf-class="rbk-chip {{ chipClass(u.pct) }}"
                                              t-esc="pct(u.pct)"/>
                                    </div>
                                </div>
                            </div>

                            <div class="rbk-cal">
                                <t t-foreach="weekdayNames" t-as="w" t-key="w">
                                    <div class="rbk-cal-wd" t-esc="w"/>
                                </t>
                                <t t-foreach="leadingBlanks" t-as="b" t-key="'b' + b">
                                    <div class="rbk-cal-blank"/>
                                </t>
                                <t t-foreach="u.days" t-as="d" t-key="d_index">
                                    <div t-attf-class="rbk-cal-day{{ d ? ' let' : ' free' }}{{ isToday(d_index) ? ' today' : '' }}{{ inSelection(d_index) ? ' picked' : '' }}"
                                         t-att-data-day="dayIso(d_index)"
                                         t-att-title="boxTitle(u, d_index)"
                                         t-on-click="() => this.onDayClick(d_index)">
                                        <span class="rbk-cal-num" t-esc="d_index + 1"/>
                                        <span class="rbk-cal-who" t-esc="whoOn(u, d_index)"/>
                                    </div>
                                </t>
                            </div>

                            <!-- the selection banner: what pressing Book will do -->
                            <div class="rbk-selbar" t-if="state.pick.from">
                                <span class="rbk-selbar-text">
                                    <t t-esc="state.pick.from"/>
                                    <t t-if="state.pick.to and state.pick.to !== state.pick.from">
                                        → <t t-esc="state.pick.to"/>
                                    </t>
                                    <span class="rbk-selbar-n" t-esc="selectionLength + ' day(s)'"/>
                                </span>
                                <button class="btn btn-primary" data-book="open"
                                        t-on-click="openBooking">Book these dates</button>
                                <button class="btn" t-on-click="clearPick">Clear</button>
                            </div>
                            <div class="rbk-hint" t-else="">
                                Click a free day, then another, to choose a period to book.
                            </div>

                            <div class="rbk-bookings" t-if="u.bookings.length">
                                <div class="rbk-bookings-title">Bookings touching this month</div>
                                <table class="rbk-bk-table">
                                    <thead>
                                        <tr><th>Customer</th><th>From</th><th>To</th>
                                            <th>Status</th><th>Contract</th></tr>
                                    </thead>
                                    <tbody>
                                        <t t-foreach="u.bookings" t-as="bk" t-key="bk.line_id">
                                            <tr>
                                                <td t-esc="bk.partner || '—'"/>
                                                <td t-esc="bk.from"/>
                                                <td t-esc="bk.to || 'open-ended'"/>
                                                <td><span t-attf-class="rbk-badge rbk-line-{{ bk.state }}"
                                                          t-esc="bk.state"/></td>
                                                <td t-esc="bk.contract || '—'"/>
                                            </tr>
                                        </t>
                                    </tbody>
                                </table>
                            </div>
                        </t>
                    </t>
                </section>
            </div>

            <!-- ── the booking dialog ────────────────────────────────── -->
            <div class="m2o-modal-back" t-if="state.dlg" t-on-click="closeBooking">
                <div class="m2o-modal" t-on-click.stop="() => {}">
                    <div class="m2o-modal-head">
                        <span>Book <t t-esc="state.dlg.unitCode"/></span>
                        <button class="m2o-x" t-on-click="closeBooking">×</button>
                    </div>
                    <div class="rbk-dlg">
                        <label>Customer
                            <M2OSelect model="'res.partner'" label="'Customer'"
                                       placeholder="'Search a customer…'"
                                       value="state.dlg.partner_id"
                                       onSelect="(id) => { this.state.dlg.partner_id = id; }"/>
                        </label>
                        <div class="rbk-dlg-two">
                            <label>From
                                <input class="form-input" type="date" data-bk="date_start"
                                       t-att-value="state.dlg.date_start"
                                       t-on-input="ev => { this.state.dlg.date_start = ev.target.value; }"/>
                            </label>
                            <label>To
                                <input class="form-input" type="date" data-bk="date_end"
                                       t-att-value="state.dlg.date_end"
                                       t-on-input="ev => { this.state.dlg.date_end = ev.target.value; }"/>
                            </label>
                        </div>
                        <div class="rbk-dlg-two">
                            <label>Rate
                                <input class="form-input" type="number" step="0.01" data-bk="unit_price"
                                       t-att-value="state.dlg.unit_price"
                                       t-on-input="ev => { this.state.dlg.unit_price = ev.target.value; }"/>
                            </label>
                            <label>Billing
                                <select class="form-input" data-bk="billing_mode"
                                        t-on-change="ev => { this.state.dlg.billing_mode = ev.target.value; }">
                                    <option value="oneoff" t-att-selected="state.dlg.billing_mode === 'oneoff' ? true : undefined">One off</option>
                                    <option value="recurring" t-att-selected="state.dlg.billing_mode === 'recurring' ? true : undefined">Recurring</option>
                                </select>
                            </label>
                        </div>
                        <p class="rbk-dlg-note">
                            Leave <strong>To</strong> empty to let this unit until it is
                            terminated. A dated booking is billed once; an open-ended one
                            recurs.
                        </p>
                        <div class="rbk-dlg-err error" t-if="state.dlg.error" t-esc="state.dlg.error"/>
                    </div>
                    <div class="m2o-modal-foot">
                        <button t-on-click="closeBooking">Cancel</button>
                        <span/>
                        <button class="btn btn-primary" data-book="confirm"
                                t-att-disabled="state.dlg.saving ? true : undefined"
                                t-on-click="confirmBooking">Book</button>
                    </div>
                </div>
            </div>
        </div>`;

    setup() {
        this.state = owl.useState({
            month:   '',              // YYYY-MM; empty = server picks today's
            data:    null,
            loading: true,
            error:   '',
            sel:     { kind: 'all', id: 0 },
            pick:    { from: '', to: '' },
            dlg:     null,
        });
        owl.onWillStart(() => this.load());
    }

    // ---- data -------------------------------------------------------------
    async load() {
        this.state.loading = true;
        this.state.error   = '';
        try {
            const qs = this.state.month ? ('?month=' + encodeURIComponent(this.state.month)) : '';
            const r  = await fetch('/rental/calendar' + qs, { credentials: 'same-origin' });
            const j  = await r.json();
            if (!r.ok) throw new Error(j.error || 'Could not load the calendar.');
            this.state.data  = j;
            this.state.month = j.month;
        } catch (e) {
            this.state.error = (e && e.message) || String(e);
            this.state.data  = null;
        } finally {
            this.state.loading = false;
        }
    }

    shiftMonth(delta) {
        const [y, m] = (this.state.month || '').split('-').map(Number);
        if (!y || !m) return this.load();
        const d = new Date(Date.UTC(y, m - 1 + delta, 1));
        this.state.month = d.toISOString().slice(0, 7);
        this.clearPick();
        return this.load();
    }
    prevMonth() { return this.shiftMonth(-1); }
    nextMonth() { return this.shiftMonth(1); }
    thisMonth() { this.state.month = ''; this.clearPick(); return this.load(); }

    // ---- selection --------------------------------------------------------
    pickAll()      { this.state.sel = { kind: 'all',  id: 0 };  this.clearPick(); }
    pickType(id)   { this.state.sel = { kind: 'type', id };     this.clearPick(); }
    pickUnit(id)   { this.state.sel = { kind: 'unit', id };     this.clearPick(); }

    get types() { return (this.state.data && this.state.data.types) || []; }

    unitsOfType(typeId) {
        return ((this.state.data && this.state.data.units) || [])
            .filter(u => u.type_id === typeId);
    }

    get visibleUnits() {
        const all = (this.state.data && this.state.data.units) || [];
        if (this.state.sel.kind === 'type')
            return all.filter(u => u.type_id === this.state.sel.id);
        return all;
    }

    get selectedUnit() {
        return ((this.state.data && this.state.data.units) || [])
            .find(u => u.id === this.state.sel.id) || null;
    }

    get stripTitle() {
        if (this.state.sel.kind === 'type') {
            const t = this.types.find(x => x.id === this.state.sel.id);
            return t ? t.name : 'Units';
        }
        return 'All units';
    }

    // ---- day helpers ------------------------------------------------------
    get monthLabel() {
        const m = this.state.month || (this.state.data && this.state.data.month) || '';
        if (!m) return '';
        const [y, mm] = m.split('-').map(Number);
        const names = ['January','February','March','April','May','June','July',
                       'August','September','October','November','December'];
        return (names[mm - 1] || m) + ' ' + y;
    }

    get dayNumbers() {
        const n = (this.state.data && this.state.data.days) || 0;
        return Array.from({ length: n }, (_, i) => i + 1);
    }

    /** ISO date for a zero-based day index in the shown month. */
    dayIso(idx) {
        const m = (this.state.data && this.state.data.from) || '';
        if (!m) return '';
        return m.slice(0, 8) + String(idx + 1).padStart(2, '0');
    }

    isToday(idx) {
        return !!(this.state.data && this.dayIso(idx) === this.state.data.today);
    }

    /** Which weekday the 1st falls on, so the grid lines up. */
    get leadingBlanks() {
        const from = (this.state.data && this.state.data.from) || '';
        if (!from) return [];
        const wd = new Date(from + 'T00:00:00Z').getUTCDay();   // 0 = Sunday
        return Array.from({ length: (wd + 6) % 7 }, (_, i) => i);  // week starts Monday
    }

    get weekdayNames() { return ['Mon','Tue','Wed','Thu','Fri','Sat','Sun']; }

    /** The booking covering a day, if any — used for the cell's tooltip. */
    bookingOn(u, idx) {
        const iso = this.dayIso(idx);
        return (u.bookings || []).find(b =>
            b.from <= iso && (!b.to || iso <= b.to)) || null;
    }

    whoOn(u, idx) {
        const b = this.bookingOn(u, idx);
        return b ? (b.partner || '—') : '';
    }

    boxTitle(u, idx) {
        const iso = this.dayIso(idx);
        const b   = this.bookingOn(u, idx);
        return b ? `${iso} · ${b.partner || 'let'}` : `${iso} · free`;
    }

    // ---- picking a range to book -----------------------------------------
    onDayClick(idx) {
        const iso = this.dayIso(idx);
        const p   = this.state.pick;
        if (!p.from || (p.from && p.to)) {
            this.state.pick = { from: iso, to: '' };
        } else if (iso < p.from) {
            this.state.pick = { from: iso, to: p.from };
        } else {
            this.state.pick = { from: p.from, to: iso };
        }
    }

    inSelection(idx) {
        const iso = this.dayIso(idx);
        const p   = this.state.pick;
        if (!p.from) return false;
        const to = p.to || p.from;
        return iso >= p.from && iso <= to;
    }

    get selectionLength() {
        const p = this.state.pick;
        if (!p.from) return 0;
        const a = new Date(p.from + 'T00:00:00Z');
        const b = new Date((p.to || p.from) + 'T00:00:00Z');
        return Math.round((b - a) / 86400000) + 1;
    }

    clearPick() { this.state.pick = { from: '', to: '' }; }

    // ---- the dialog -------------------------------------------------------
    openBooking() {
        const u = this.selectedUnit;
        if (!u || !this.state.pick.from) return;
        this.state.dlg = {
            unit_id:      u.id,
            unitCode:     u.code,
            partner_id:   0,
            date_start:   this.state.pick.from,
            date_end:     this.state.pick.to || this.state.pick.from,
            unit_price:   '',
            billing_mode: 'oneoff',
            error:        '',
            saving:       false,
        };
    }
    closeBooking() { this.state.dlg = null; }

    async confirmBooking() {
        const d = this.state.dlg;
        if (!d || d.saving) return;
        if (!d.partner_id) { d.error = 'Choose a customer for this booking.'; return; }
        d.saving = true; d.error = '';
        try {
            const body = new URLSearchParams({
                unit_id:      String(d.unit_id),
                partner_id:   String(d.partner_id),
                date_start:   d.date_start || '',
                date_end:     d.date_end || '',
                billing_mode: d.billing_mode || '',
            });
            if (d.unit_price !== '') body.set('unit_price', String(d.unit_price));
            const r = await fetch('/rental/booking/create', {
                method: 'POST',
                credentials: 'same-origin',
                headers: { 'Content-Type': 'application/x-www-form-urlencoded' },
                body: body.toString(),
            });
            const j = await r.json();
            if (!r.ok) throw new Error(j.error || 'The booking was refused.');
            this.state.dlg = null;
            this.clearPick();
            await this.load();
        } catch (e) {
            d.error  = (e && e.message) || String(e);
            d.saving = false;
        }
    }

    // ---- formatting -------------------------------------------------------
    pct(v) { return Math.round(v || 0) + '%'; }

    /** Occupancy bands, so a glance carries meaning without reading digits. */
    chipClass(v) {
        const n = v || 0;
        if (n >= 85) return 'rbk-chip-hot';
        if (n >= 40) return 'rbk-chip-ok';
        if (n > 0)   return 'rbk-chip-low';
        return 'rbk-chip-idle';
    }
}
