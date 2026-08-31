/**
 * TimesheetGrid.js — Project → Timesheets (docs/100).
 *
 * A week at a time: one row per (project, task), one column per day, hours in
 * the cells. This is the shape people already know from every timesheet they
 * have ever filled in, and it is far faster to fill than a list of records —
 * logging five days of work is five keystrokes across a row, not five forms.
 *
 * The cells send an ABSOLUTE value, never a delta. `set_cell` is idempotent, so
 * a double-submit, a retry or a stray blur event cannot silently double
 * someone's day. That property is the whole reason the grid can save on blur
 * instead of behind an explicit Save button.
 */
class TimesheetGrid extends owl.Component {
    static template = owl.xml`
        <div class="tg-shell">

            <div class="tg-head">
                <h2 class="tg-title">Timesheets</h2>
                <div class="tg-week">
                    <button class="tg-btn" t-on-click="() => this.shiftWeek(-7)" title="Previous week">‹</button>
                    <span class="tg-week-l" t-esc="weekLabel"/>
                    <button class="tg-btn" t-on-click="() => this.shiftWeek(7)" title="Next week">›</button>
                    <button class="tg-btn ghost" t-on-click="thisWeek">This week</button>
                </div>
                <select class="tg-sel" t-on-change="onUser">
                    <option value="0">All users</option>
                    <t t-foreach="state.users" t-as="u" t-key="u.id">
                        <option t-att-value="u.id" t-att-selected="u.id === state.userId" t-esc="u.login"/>
                    </t>
                </select>
                <div class="tg-spacer"/>
                <span class="tg-total">Week total <b t-esc="fmtH(state.total)"/> h</span>
            </div>

            <t t-if="state.error"><div class="tg-error" t-esc="state.error"/></t>
            <t t-if="state.notice"><div class="tg-notice" t-esc="state.notice"/></t>

            <div class="tg-wrap">
                <table class="tg-table">
                    <thead>
                        <tr>
                            <th class="tg-c-proj">Project</th>
                            <th class="tg-c-task">Task</th>
                            <t t-foreach="state.days" t-as="d" t-key="d.date">
                                <th class="tg-c-day"
                                    t-att-class="{today: d.is_today, weekend: d.is_weekend}">
                                    <span class="tg-dow" t-esc="d.dow"/>
                                    <span class="tg-dat" t-esc="d.label"/>
                                </th>
                            </t>
                            <th class="tg-c-tot">Total</th>
                        </tr>
                    </thead>
                    <tbody>
                        <t t-foreach="state.rows" t-as="r" t-key="rowKey(r)">
                            <tr>
                                <td class="tg-c-proj" t-esc="r.project_name"/>
                                <td class="tg-c-task" t-esc="r.task_name || '(no task)'"/>
                                <t t-foreach="state.days" t-as="d" t-key="d.date">
                                    <td class="tg-c-day"
                                        t-att-class="{today: d.is_today, weekend: d.is_weekend}">
                                        <input class="tg-in"
                                               t-att-class="{filled: cell(r, d.date) > 0}"
                                               t-att-value="cellText(r, d.date)"
                                               t-att-disabled="state.busy"
                                               t-on-focus="(ev) => ev.target.select()"
                                               t-on-blur="(ev) => this.commit(r, d.date, ev)"
                                               t-on-keydown="(ev) => this.onKey(ev, r, d.date)"/>
                                    </td>
                                </t>
                                <td class="tg-c-tot" t-esc="fmtH(rowTotal(r))"/>
                            </tr>
                        </t>

                    </tbody>
                    <tfoot>
                        <tr>
                            <td class="tg-c-proj" colspan="2">Daily total</td>
                            <t t-foreach="state.days" t-as="d" t-key="d.date">
                                <td class="tg-c-day tg-foot"
                                    t-att-class="{today: d.is_today, weekend: d.is_weekend, over: colTotal(d.date) > 8}"
                                    t-esc="fmtH(colTotal(d.date))"/>
                            </t>
                            <td class="tg-c-tot tg-foot" t-esc="fmtH(state.total)"/>
                        </tr>
                    </tfoot>
                </table>

                <div class="tg-none" t-if="!state.rows.length and !state.loading">
                    Nothing logged this week. Add a project below to start.
                </div>

                <!-- Deliberately OUTSIDE the table. As a table row with a
                     colspan, this cell inherits the combined width of the
                     narrow day columns and crushes the button and hint into a
                     vertical strip. A plain row below the grid is immune to
                     the table's column sizing. -->
                <div class="tg-addbar">
                    <span class="tg-addlbl">Add a row</span>
                    <select class="tg-sel" t-on-change="onNewProject">
                        <option value="0">Choose a project…</option>
                        <t t-foreach="state.projects" t-as="p" t-key="p.id">
                            <option t-att-value="p.id" t-att-selected="p.id === state.newProject"
                                    t-esc="p.display_name || p.name"/>
                        </t>
                    </select>
                    <select class="tg-sel" t-att-disabled="!state.newProject" t-on-change="onNewTask">
                        <option value="0">(no task)</option>
                        <t t-foreach="state.newTasks" t-as="t" t-key="t.id">
                            <option t-att-value="t.id" t-att-selected="t.id === state.newTask"
                                    t-esc="t.name"/>
                        </t>
                    </select>
                    <button class="tg-btn" t-att-disabled="!state.newProject" t-on-click="addRow">Add row</button>
                    <span class="tg-hint">Type hours as 1.5, 1,5 or 1:30 — they save when you click away.</span>
                </div>
            </div>
        </div>`;

    setup() {
        this.state = owl.useState({
            weekStart: '', days: [], rows: [], colTotals: {}, total: 0,
            users: [], projects: [], newTasks: [],
            userId: 0, newProject: 0, newTask: 0,
            // Rows added in the browser that have no hours yet. They must
            // survive a reload, or picking a project and typing into it would
            // make the row vanish under the user's cursor.
            pending: [],
            loading: false, busy: false, error: '', notice: '',
        });
        owl.onWillStart(async () => {
            try {
                const [users, projects] = await Promise.all([
                    RpcService.call('res.users', 'search_read', [[]], { fields: ['login'], limit: 100 }),
                    RpcService.call('project.project', 'search_read', [[]],
                                    { fields: ['name', 'display_name'], limit: 200 }),
                ]);
                this.state.users = users || [];
                this.state.projects = projects || [];
            } catch (e) { /* pickers are a convenience */ }
            await this.reload();
        });
    }

    async reload() {
        this.state.loading = true;
        this.state.error = '';
        try {
            const args = {};
            if (this.state.weekStart) args.date = this.state.weekStart;
            if (this.state.userId) args.user_id = this.state.userId;
            const g = await RpcService.call('project.timesheet', 'grid', [args], {});
            this.state.weekStart = (g && g.week_start) || '';
            this.state.days = (g && g.days) || [];
            this.state.colTotals = (g && g.col_totals) || {};
            this.state.total = (g && g.total) || 0;

            const rows = (g && g.rows) || [];
            // Re-attach any browser-only rows the server has no hours for yet.
            const seen = new Set(rows.map(r => this.rowKey(r)));
            for (const p of this.state.pending)
                if (!seen.has(this.rowKey(p))) rows.push(Object.assign({}, p, { cells: {}, total: 0 }));
            this.state.rows = rows;
        } catch (e) {
            this.state.error = (e && e.message) || 'Could not load the timesheet.';
        } finally {
            this.state.loading = false;
        }
    }

    rowKey(r) { return (r.project_id || 0) + ':' + (r.task_id || 0); }
    cell(r, date) { return (r.cells && r.cells[date]) || 0; }
    cellText(r, date) { const v = this.cell(r, date); return v ? this.fmtH(v) : ''; }
    rowTotal(r) {
        return this.state.days.reduce((s, d) => s + this.cell(r, d.date), 0);
    }
    colTotal(date) {
        return this.state.rows.reduce((s, r) => s + this.cell(r, date), 0);
    }

    get weekLabel() {
        if (!this.state.days.length) return '';
        const a = this.state.days[0], b = this.state.days[this.state.days.length - 1];
        return a.label + ' – ' + b.label;
    }

    // ---- editing -----------------------------------------------------------
    onKey(ev, r, date) {
        if (ev.key === 'Enter') { ev.target.blur(); return; }
        if (ev.key === 'Escape') { ev.target.value = this.cellText(r, date); ev.target.blur(); }
    }

    /**
     * Parse and persist one cell. Accepts "1.5", "1,5" and "1:30" — people
     * write durations all three ways and rejecting two of them is a papercut
     * on the most repeated action in the screen.
     */
    parseHours(raw) {
        const s = String(raw == null ? '' : raw).trim().replace(',', '.');
        if (!s) return 0;
        const hm = s.match(/^(\d+):([0-5]?\d)$/);
        if (hm) return parseInt(hm[1], 10) + parseInt(hm[2], 10) / 60;
        if (!/^\d*\.?\d+$/.test(s)) return NaN;
        return parseFloat(s);
    }

    async commit(r, date, ev) {
        const hours = this.parseHours(ev.target.value);
        const current = this.cell(r, date);
        if (Number.isNaN(hours)) {
            ev.target.value = this.cellText(r, date);
            this.state.error = 'Enter hours as 1.5, 1,5 or 1:30.';
            return;
        }
        // Round to the minute; nobody means 0.3333333 hours.
        const rounded = Math.round(hours * 60) / 60;
        if (Math.abs(rounded - current) < 0.001) { ev.target.value = this.cellText(r, date); return; }

        this.state.busy = true;
        this.state.error = '';
        this.state.notice = '';
        try {
            await RpcService.call('project.timesheet', 'set_cell', [{
                project_id: r.project_id, task_id: r.task_id || 0,
                user_id: this.state.userId || 0, date, hours: rounded,
            }], {});
            // Reload rather than patch locally: the totals row, the row total
            // and the week total all move together, and the server already
            // computed them.
            await this.reload();
        } catch (e) {
            ev.target.value = this.cellText(r, date);
            this.state.error = (e && e.message) || 'The entry could not be saved.';
        } finally {
            this.state.busy = false;
        }
    }

    // ---- adding rows -------------------------------------------------------
    async onNewProject(ev) {
        this.state.newProject = parseInt(ev.target.value, 10) || 0;
        this.state.newTask = 0;
        this.state.newTasks = [];
        if (!this.state.newProject) return;
        try {
            this.state.newTasks = await RpcService.call('project.task', 'search_read',
                [[['project_id', '=', this.state.newProject]]], { fields: ['name'], limit: 200 }) || [];
        } catch (e) { /* a row with no task is still valid */ }
    }
    onNewTask(ev) { this.state.newTask = parseInt(ev.target.value, 10) || 0; }

    addRow() {
        if (!this.state.newProject) return;
        const proj = this.state.projects.find(p => p.id === this.state.newProject);
        const task = this.state.newTasks.find(t => t.id === this.state.newTask);
        const row = {
            project_id: this.state.newProject,
            project_name: (proj && (proj.display_name || proj.name)) || '',
            task_id: this.state.newTask || 0,
            task_name: (task && task.name) || '',
            cells: {}, total: 0,
        };
        if (this.state.rows.some(r => this.rowKey(r) === this.rowKey(row))) {
            this.state.notice = 'That row is already on the sheet.';
            return;
        }
        this.state.pending.push({ project_id: row.project_id, project_name: row.project_name,
                                  task_id: row.task_id, task_name: row.task_name });
        this.state.rows.push(row);
        this.state.notice = '';
    }

    // ---- week navigation ---------------------------------------------------
    shiftWeek(days) {
        const base = this.state.weekStart ? new Date(this.state.weekStart + 'T00:00:00') : new Date();
        base.setDate(base.getDate() + days);
        this.state.weekStart = base.toISOString().slice(0, 10);
        this.state.pending = [];      // a new week starts with a clean sheet
        return this.reload();
    }
    thisWeek() { this.state.weekStart = ''; this.state.pending = []; return this.reload(); }
    async onUser(ev) {
        this.state.userId = parseInt(ev.target.value, 10) || 0;
        this.state.pending = [];
        await this.reload();
    }

    fmtH(h) {
        const v = Number(h || 0);
        if (!v) return '0';
        return Number.isInteger(v) ? String(v) : v.toFixed(2).replace(/0$/, '');
    }
}
