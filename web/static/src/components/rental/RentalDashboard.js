/**
 * RentalDashboard.js — the rental landing page (docs/046 §2).
 *
 * One fetch of /rental/dashboard, which returns every panel's data in a
 * single cached payload. Not a dozen search_read calls assembled here —
 * that is the fastest route to a four-second paint and makes each panel
 * its own failure.
 *
 * Charts are inline SVG. No chart library: the app loads no bundler and
 * no CDN, and a 12-point bar chart does not justify either.
 *
 * FORM CHOICE — one departure from docs/046 §2, stated rather than
 * slipped in. The plan specified a 2-series LINE for revenue vs
 * expenses. This uses GROUPED BARS, because these are discrete monthly
 * totals: a line between September and October implies a value in
 * between, and there isn't one. Lines are right for a continuous
 * measure sampled over time; bars are right for per-period totals.
 *
 * Cumulative net is a SEPARATE view, not a third series. Income and
 * expense are flows; cumulative is a stock. Putting them on one scale
 * would be the dual-axis mistake wearing a disguise — so it is two
 * charts, per docs/046 §9.
 */

class RentalDashboard extends owl.Component {
    static template = owl.xml`
        <div class="rental-dash viz-root">

            <div class="rental-filters">
                <label class="dash-label">Horizon</label>
                <select t-model="state.months" t-on-change="reload">
                    <option value="6">6 months</option>
                    <option value="12">12 months</option>
                    <option value="24">24 months</option>
                </select>
                <span class="spacer"/>
                <button t-on-click="() => this.load(true)">Refresh</button>
            </div>

            <t t-if="state.loading">
                <div class="rental-empty">Loading dashboard…</div>
            </t>
            <t t-elif="state.error">
                <div class="rental-empty"><t t-esc="state.error"/></div>
            </t>
            <t t-else="">

                <!-- KPI row: five single values, each a stat tile.
                     A one-bar bar chart for a single number is the
                     classic mistake; occupancy gets a meter because it
                     is a ratio against a limit. -->
                <div class="kpi-row">
                    <div class="kpi">
                        <div class="kpi-label">Occupancy</div>
                        <div class="kpi-value"><t t-esc="d.occupancy.pct"/>%</div>
                        <div class="kpi-meter">
                            <div class="kpi-meter-fill"
                                 t-attf-style="width: {{d.occupancy.pct}}%"/>
                        </div>
                        <div class="kpi-sub">
                            <t t-esc="d.occupancy.occupied"/> of
                            <t t-esc="d.occupancy.lettable"/> lettable
                        </div>
                    </div>
                    <div class="kpi">
                        <div class="kpi-label">MRR</div>
                        <div class="kpi-value"><t t-esc="money(d.mrr)"/></div>
                        <div class="kpi-sub">recurring tenancies only</div>
                    </div>
                    <div class="kpi">
                        <div class="kpi-label">Outstanding</div>
                        <div class="kpi-value"><t t-esc="money(d.receivables.outstanding)"/></div>
                        <div class="kpi-sub">unpaid invoices</div>
                    </div>
                    <div class="kpi" t-att-class="d.receivables.overdue > 0 ? 'is-warn' : ''">
                        <div class="kpi-label">Overdue</div>
                        <div class="kpi-value"><t t-esc="money(d.receivables.overdue)"/></div>
                        <div class="kpi-sub">
                            <t t-if="d.receivables.overdue > 0">⚠ past due date</t>
                            <t t-else="">nothing past due</t>
                        </div>
                    </div>
                    <div class="kpi" t-att-class="d.noi_month &lt; 0 ? 'is-warn' : ''">
                        <div class="kpi-label">Net this month</div>
                        <div class="kpi-value"><t t-esc="money(d.noi_month)"/></div>
                        <div class="kpi-sub">income − expenses</div>
                    </div>
                </div>

                <!-- ============ CASHFLOW ============ -->
                <div class="panel">
                    <div class="panel-head">
                        <h3>Cashflow forecast</h3>
                        <div class="panel-tools">
                            <button t-att-class="state.cfView === 'monthly' ? 'on' : ''"
                                    t-on-click="() => this.setCfView('monthly')">Monthly</button>
                            <button t-att-class="state.cfView === 'cumulative' ? 'on' : ''"
                                    t-on-click="() => this.setCfView('cumulative')">Cumulative</button>
                            <button t-att-class="state.cfView === 'table' ? 'on' : ''"
                                    t-on-click="() => this.setCfView('table')">Table</button>
                        </div>
                    </div>

                    <!-- Legend is always present for 2 series, so identity
                         is never carried by colour alone. -->
                    <div class="rental-legend" t-if="state.cfView === 'monthly'">
                        <span class="item">
                            <span class="swatch" style="background: var(--s-income)"/>
                            <span>Income</span>
                        </span>
                        <span class="item">
                            <span class="swatch" style="background: var(--s-expense)"/>
                            <span>Expenses</span>
                        </span>
                    </div>

                    <t t-if="state.cfView === 'table'">
                        <div class="table-scroll">
                            <table class="rental-table">
                                <thead>
                                    <tr>
                                        <th>Month</th>
                                        <th class="num">Invoiced</th>
                                        <th class="num">Projected</th>
                                        <th class="num">Income</th>
                                        <th class="num">Expenses</th>
                                        <th class="num">Net</th>
                                        <th class="num">Cumulative</th>
                                    </tr>
                                </thead>
                                <tbody>
                                    <t t-foreach="series" t-as="r" t-key="r.month">
                                        <tr>
                                            <td t-esc="r.month"/>
                                            <td class="num" t-esc="money(r.receivable)"/>
                                            <td class="num" t-esc="money(r.projected_income)"/>
                                            <td class="num" t-esc="money(r.income)"/>
                                            <td class="num" t-esc="money(r.expense)"/>
                                            <td class="num" t-att-class="r.net &lt; 0 ? 'num neg' : 'num'"
                                                t-esc="money(r.net)"/>
                                            <td class="num" t-att-class="r.cumulative &lt; 0 ? 'num neg' : 'num'"
                                                t-esc="money(r.cumulative)"/>
                                        </tr>
                                    </t>
                                </tbody>
                            </table>
                        </div>
                    </t>

                    <t t-else="">
                        <div class="chart-wrap">
                            <svg t-att-viewBox="'0 0 ' + CW + ' ' + CH"
                                 preserveAspectRatio="xMidYMid meet"
                                 class="chart" role="img"
                                 t-att-aria-label="chartAria">
                                <!-- Recessive grid. -->
                                <t t-foreach="gridLines" t-as="g" t-key="g.v">
                                    <line class="grid" x1="52" t-att-x2="CW - 8"
                                          t-att-y1="g.y" t-att-y2="g.y"/>
                                    <text class="axis" x="46" t-att-y="g.y + 4"
                                          text-anchor="end" t-esc="g.label"/>
                                </t>
                                <!-- Zero line drawn distinctly: below it is
                                     the month you cannot pay for. -->
                                <line class="zero" x1="52" t-att-x2="CW - 8"
                                      t-att-y1="zeroY" t-att-y2="zeroY"/>

                                <t t-if="state.cfView === 'monthly'">
                                    <t t-foreach="bars" t-as="b" t-key="b.key">
                                        <rect t-att-x="b.x" t-att-y="b.y"
                                              t-att-width="b.w" t-att-height="b.h"
                                              rx="4" ry="4"
                                              t-att-fill="b.fill"
                                              t-on-mouseenter="ev => this.tip(ev, b.tip)"
                                              t-on-mousemove="ev => this.moveTip(ev)"
                                              t-on-mouseleave="hideTip"/>
                                    </t>
                                </t>
                                <t t-else="">
                                    <path class="cum-line" t-att-d="cumPath"/>
                                    <t t-foreach="cumPoints" t-as="p" t-key="p.key">
                                        <circle t-att-cx="p.x" t-att-cy="p.y" r="5"
                                                t-att-fill="p.fill"
                                                t-on-mouseenter="ev => this.tip(ev, p.tip)"
                                                t-on-mousemove="ev => this.moveTip(ev)"
                                                t-on-mouseleave="hideTip"/>
                                    </t>
                                </t>

                                <t t-foreach="xLabels" t-as="x" t-key="x.key">
                                    <text class="axis" t-att-x="x.x" t-att-y="CH - 6"
                                          text-anchor="middle" t-esc="x.label"/>
                                </t>
                            </svg>
                        </div>

                        <div class="chart-note">
                            <t t-if="state.cfView === 'cumulative'">
                                Running total of net cashflow. Below the zero line is a
                                month the projection does not cover.
                            </t>
                            <t t-else="">
                                Income is invoiced amounts plus projected rent; expenses are
                                budgeted recurring plus dated one-offs.
                            </t>
                        </div>
                    </t>

                    <details class="assumptions">
                        <summary>What this forecast assumes</summary>
                        <ul>
                            <t t-foreach="assumptions" t-as="a" t-key="a">
                                <li t-esc="a"/>
                            </t>
                        </ul>
                    </details>
                </div>

                <div class="panel-row">
                    <!-- ============ OCCUPANCY BY TYPE ============ -->
                    <div class="panel">
                        <div class="panel-head"><h3>Occupancy by unit type</h3></div>
                        <t t-if="!typeRows.length">
                            <div class="rental-empty">No units yet.</div>
                        </t>
                        <t t-else="">
                            <t t-foreach="typeRows" t-as="tr" t-key="tr.name">
                                <div class="stack-row">
                                    <div class="stack-label">
                                        <t t-esc="tr.name"/>
                                        <span class="muted"><t t-esc="tr.total"/></span>
                                    </div>
                                    <div class="stack">
                                        <t t-foreach="tr.segs" t-as="s" t-key="s.state">
                                            <div class="seg"
                                                 t-attf-style="width: {{s.pct}}%; background: var(--u-{{s.state}})"
                                                 t-att-title="s.label">
                                                <span t-if="s.pct > 12" t-esc="s.n"/>
                                            </div>
                                        </t>
                                    </div>
                                </div>
                            </t>
                            <div class="rental-legend">
                                <t t-foreach="occStates" t-as="s" t-key="s">
                                    <span class="item">
                                        <span class="swatch" t-attf-style="background: var(--u-{{s}})"/>
                                        <span t-esc="s"/>
                                    </span>
                                </t>
                            </div>
                        </t>
                    </div>

                    <!-- ============ RECEIVABLES AGEING ============ -->
                    <div class="panel">
                        <div class="panel-head"><h3>Receivables ageing</h3></div>
                        <!-- An ORDERED magnitude of badness, so a one-hue
                             ordinal ramp — not four categorical colours,
                             and not status red. The buckets are a scale,
                             not four identities. -->
                        <t t-foreach="ageRows" t-as="a" t-key="a.key">
                            <div class="age-row">
                                <div class="age-label" t-esc="a.label"/>
                                <div class="age-track">
                                    <div class="age-fill"
                                         t-attf-style="width: {{a.pct}}%; background: {{a.color}}"/>
                                </div>
                                <div class="age-amt" t-esc="money(a.amount)"/>
                            </div>
                        </t>
                        <t t-if="!ageTotal">
                            <div class="rental-empty">Nothing outstanding.</div>
                        </t>
                    </div>
                </div>

                <div class="panel-row">
                    <!-- ============ NEEDS ATTENTION ============ -->
                    <div class="panel">
                        <div class="panel-head"><h3>Needs attention</h3></div>
                        <table class="rental-table">
                            <tbody>
                                <t t-foreach="attentionRows" t-as="a" t-key="a.key">
                                    <tr>
                                        <td>
                                            <span t-if="a.n > 0" class="st-icon" t-esc="a.icon"/>
                                            <t t-esc="a.label"/>
                                        </td>
                                        <td class="num"
                                            t-att-class="a.n > 0 ? 'num attn' : 'num'"
                                            t-esc="a.n"/>
                                    </tr>
                                </t>
                            </tbody>
                        </table>
                    </div>

                    <!-- ============ ACTIVITY ============ -->
                    <div class="panel">
                        <div class="panel-head"><h3>Recent activity</h3></div>
                        <t t-if="!d.activity.length">
                            <div class="rental-empty">No events yet.</div>
                        </t>
                        <t t-else="">
                            <ul class="feed">
                                <t t-foreach="d.activity" t-as="e" t-key="e.at + e.summary">
                                    <li>
                                        <span class="feed-at" t-esc="e.at"/>
                                        <span class="feed-sum" t-esc="e.summary"/>
                                    </li>
                                </t>
                            </ul>
                        </t>
                    </div>
                </div>
            </t>

            <t t-if="state.tip.show">
                <div class="rental-tip"
                     t-attf-style="left: {{state.tip.x}}px; top: {{state.tip.y}}px">
                    <div class="t-code" t-esc="state.tip.title"/>
                    <t t-foreach="state.tip.rows" t-as="r" t-key="r.k">
                        <div class="t-row"><t t-esc="r.k"/>: <b t-esc="r.v"/></div>
                    </t>
                </div>
            </t>
        </div>
    `;

    setup() {
        this.CW = 780;      // SVG user units; the element scales to fit
        this.CH = 240;
        this.state = owl.useState({
            data: null, loading: true, error: '',
            months: '12', cfView: 'monthly',
            tip: { show: false, x: 0, y: 0, title: '', rows: [] },
        });
        owl.onWillStart(() => this.load());
    }

    async load(fresh) {
        this.state.loading = true;
        this.state.error = '';
        try {
            const qs = `months=${encodeURIComponent(this.state.months)}` +
                       (fresh ? '&fresh=1' : '');
            const res = await fetch(`/rental/dashboard?${qs}`, {
                credentials: 'same-origin',
            });
            if (!res.ok) throw new Error(`Dashboard unavailable (HTTP ${res.status})`);
            const data = await res.json();
            if (data.error) throw new Error(data.error);
            this.state.data = data;
        } catch (e) {
            this.state.error = (e && e.message) ? e.message : 'Could not load the dashboard.';
        } finally {
            this.state.loading = false;
        }
    }
    reload() { this.load(); }
    setCfView(v) { this.state.cfView = v; this.hideTip(); }

    get d()      { return this.state.data || {}; }
    get series() { return (this.d.cashflow && this.d.cashflow.series) || []; }
    get assumptions() { return (this.d.cashflow && this.d.cashflow.assumptions) || []; }
    get occStates()   { return ['occupied', 'available', 'reserved', 'maintenance']; }

    money(v) {
        const n = Number(v || 0);
        return n.toLocaleString(undefined, { minimumFractionDigits: 2,
                                             maximumFractionDigits: 2 });
    }

    // --- chart geometry -------------------------------------------
    // Computed here rather than in the template so the maths is
    // testable by reading it, and the template stays declarative.

    get chartVals() {
        const s = this.series;
        if (this.state.cfView === 'cumulative') return s.map(r => r.cumulative);
        return s.flatMap(r => [r.income, r.expense]);
    }

    get scale() {
        const vals = this.chartVals;
        let hi = Math.max(0, ...vals);
        let lo = Math.min(0, ...vals);
        if (hi === 0 && lo === 0) hi = 1;
        // A little headroom so the tallest bar is not flush with the top.
        hi = hi * 1.08;
        if (lo < 0) lo = lo * 1.08;
        const top = 12, bottom = this.CH - 26;
        const span = (hi - lo) || 1;
        return {
            hi, lo, top, bottom,
            y: v => bottom - ((v - lo) / span) * (bottom - top),
        };
    }

    get zeroY() { return this.scale.y(0); }

    get gridLines() {
        const sc = this.scale;
        const out = [];
        for (let i = 0; i <= 4; i++) {
            const v = sc.lo + ((sc.hi - sc.lo) * i) / 4;
            out.push({ v, y: sc.y(v), label: this.shortMoney(v) });
        }
        return out;
    }

    shortMoney(v) {
        const n = Number(v || 0);
        const a = Math.abs(n);
        if (a >= 1e6) return (n / 1e6).toFixed(1) + 'M';
        if (a >= 1e3) return (n / 1e3).toFixed(0) + 'k';
        return n.toFixed(0);
    }

    get plotLeft()  { return 52; }
    get plotWidth() { return this.CW - this.plotLeft - 8; }

    get bars() {
        const s = this.series, sc = this.scale;
        if (!s.length) return [];
        const slot = this.plotWidth / s.length;
        // 2px surface gap between adjacent fills; bars stay thin.
        const bw = Math.max(4, Math.min(18, (slot - 8) / 2));
        const out = [];
        s.forEach((r, i) => {
            const cx = this.plotLeft + slot * i + slot / 2;
            [['income', r.income, 'var(--s-income)'],
             ['expense', r.expense, 'var(--s-expense)']].forEach(([kind, val, fill], k) => {
                const x = cx - bw - 1 + k * (bw + 2);
                const y0 = sc.y(0), y1 = sc.y(val);
                out.push({
                    key: r.month + kind,
                    x, w: bw,
                    y: Math.min(y0, y1),
                    h: Math.max(1, Math.abs(y1 - y0)),
                    fill,
                    tip: {
                        title: r.month,
                        rows: [
                            { k: 'Income',   v: this.money(r.income) },
                            { k: 'Expenses', v: this.money(r.expense) },
                            { k: 'Net',      v: this.money(r.net) },
                        ],
                    },
                });
            });
        });
        return out;
    }

    get cumPoints() {
        const s = this.series, sc = this.scale;
        if (!s.length) return [];
        const slot = this.plotWidth / s.length;
        return s.map((r, i) => ({
            key: r.month,
            x: this.plotLeft + slot * i + slot / 2,
            y: sc.y(r.cumulative),
            // Colour follows the VALUE's meaning here, not a series
            // identity: a negative cumulative is the thing the reader
            // must act on.
            fill: r.cumulative < 0 ? 'var(--st-critical)' : 'var(--s-income)',
            tip: {
                title: r.month,
                rows: [
                    { k: 'Net this month', v: this.money(r.net) },
                    { k: 'Cumulative',     v: this.money(r.cumulative) },
                ],
            },
        }));
    }

    get cumPath() {
        const p = this.cumPoints;
        if (!p.length) return '';
        return p.map((q, i) => `${i ? 'L' : 'M'}${q.x.toFixed(1)},${q.y.toFixed(1)}`).join(' ');
    }

    get xLabels() {
        const s = this.series;
        if (!s.length) return [];
        const slot = this.plotWidth / s.length;
        // Thin the labels rather than let them collide.
        const every = s.length > 14 ? 3 : (s.length > 8 ? 2 : 1);
        return s.map((r, i) => ({ key: r.month, i, x: this.plotLeft + slot * i + slot / 2,
                                  label: r.month.slice(2) }))
                .filter(o => o.i % every === 0);
    }

    get chartAria() {
        const t = (this.d.cashflow && this.d.cashflow.totals) || {};
        return this.state.cfView === 'cumulative'
            ? `Cumulative net cashflow over ${this.series.length} months`
            : `Monthly income and expenses over ${this.series.length} months. ` +
              `Total income ${this.money(t.income)}, total expenses ${this.money(t.expense)}.`;
    }

    // --- occupancy ------------------------------------------------
    get typeRows() {
        const byType = (this.d.occupancy && this.d.occupancy.by_type) || {};
        return Object.keys(byType).sort().map(name => {
            const counts = byType[name] || {};
            const total = this.occStates.reduce((a, s) => a + (counts[s] || 0), 0)
                        + (counts.retired || 0);
            const segs = this.occStates
                .filter(s => (counts[s] || 0) > 0)
                .map(s => ({
                    state: s, n: counts[s],
                    pct: total ? (counts[s] * 100) / total : 0,
                    label: `${s}: ${counts[s]}`,
                }));
            return { name, total, segs };
        });
    }

    // --- ageing ---------------------------------------------------
    get ageRows() {
        const a = this.d.ageing || {};
        // One hue, light -> dark: an ordinal ramp, from docs/046 §3.
        const defs = [
            { key: 'current',  label: 'Not yet due', color: '#86b6ef' },
            { key: 'd0_30',    label: '0–30 days',   color: '#3987e5' },
            { key: 'd31_60',   label: '31–60 days',  color: '#256abf' },
            { key: 'd61_90',   label: '61–90 days',  color: '#104281' },
            { key: 'd90_plus', label: '90+ days',    color: '#0b2f5c' },
        ];
        const max = Math.max(1, ...defs.map(d => Number(a[d.key] || 0)));
        return defs.map(d => ({
            ...d,
            amount: Number(a[d.key] || 0),
            pct: (Number(a[d.key] || 0) * 100) / max,
        }));
    }
    get ageTotal() {
        return this.ageRows.reduce((s, r) => s + r.amount, 0);
    }

    // --- attention ------------------------------------------------
    get attentionRows() {
        const a = this.d.attention || {};
        // Status is icon + label, never a bare colour dot.
        return [
            { key: 'overdue',  label: 'Invoices overdue > 60 days', n: a.overdue_60d || 0,          icon: '⛔' },
            { key: 'maint',    label: 'Units in maintenance',       n: a.units_in_maintenance || 0, icon: '⚠' },
            { key: 'vacant',   label: 'Units vacant',               n: a.units_vacant || 0,         icon: '○' },
            { key: 'walkin',   label: 'Walk-ins not auto-billed',   n: a.walk_in_tenancies || 0,    icon: '✎' },
            { key: 'unalloc',  label: 'Payments not allocated',     n: a.unallocated_payments || 0, icon: '◐' },
        ];
    }

    // --- tooltip --------------------------------------------------
    tip(ev, t) {
        this.state.tip = { show: true, x: 0, y: 0, title: t.title, rows: t.rows };
        this.moveTip(ev);
    }
    moveTip(ev) {
        if (!this.state.tip.show) return;
        const pad = 14, w = 220, h = 110;
        let x = ev.clientX + pad, y = ev.clientY + pad;
        if (x + w > window.innerWidth)  x = ev.clientX - w - pad;
        if (y + h > window.innerHeight) y = ev.clientY - h - pad;
        this.state.tip.x = Math.max(4, x);
        this.state.tip.y = Math.max(4, y);
    }
    hideTip() { this.state.tip.show = false; }
}
