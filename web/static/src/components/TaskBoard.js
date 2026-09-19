/**
 * TaskBoard.js — Project → Task Board (docs/100).
 *
 * A kanban board over project.task. Columns are stages, cards are tasks, and
 * dragging a card between columns is the primary way work moves — so the drag
 * has to feel right, not merely function.
 *
 * Three things that make a board usable rather than just correct:
 *
 *  - The move is applied OPTIMISTICALLY. The card lands where you dropped it
 *    immediately and the server call follows; a board that waits for a round
 *    trip before the card moves feels broken. If the call fails the card
 *    springs back and the error is shown, which is the only honest way to do
 *    optimism.
 *  - Drop position matters, not just the column. `move_stage` takes an index,
 *    so a card dropped at the top of a column stays at the top.
 *  - Keyboard users get the same moves. Dragging is not an accessible
 *    interaction, so every card carries ← / → buttons that shift it a stage.
 */
class TaskBoard extends owl.Component {
    static template = owl.xml`
        <div class="tb-shell">

            <div class="tb-head">
                <h2 class="tb-title">Task Board</h2>
                <select class="tb-sel" t-on-change="onProject">
                    <option value="0">All projects</option>
                    <t t-foreach="state.projects" t-as="p" t-key="p.id">
                        <option t-att-value="p.id" t-att-selected="p.id === state.projectId"
                                t-esc="p.display_name || p.name"/>
                    </t>
                </select>
                <select class="tb-sel" t-on-change="onUser">
                    <option value="0">Everyone</option>
                    <t t-foreach="state.users" t-as="u" t-key="u.id">
                        <option t-att-value="u.id" t-att-selected="u.id === state.userId" t-esc="u.login"/>
                    </t>
                </select>
                <select class="tb-sel" data-tb="type" t-on-change="onType">
                    <option value="">All types</option>
                    <t t-foreach="ui.types" t-as="ty" t-key="ty.value">
                        <option t-att-value="ty.value" t-att-selected="ty.value === state.type"
                                t-esc="ty.icon + '  ' + ty.label"/>
                    </t>
                </select>
                <select class="tb-sel" data-tb="label" t-on-change="onTag">
                    <option value="0">All labels</option>
                    <t t-foreach="state.tags" t-as="g" t-key="g.id">
                        <option t-att-value="g.id" t-att-selected="g.id === state.tagId" t-esc="g.name"/>
                    </t>
                </select>
                <!-- Uncontrolled on purpose: the board re-renders when each
                     search returns, and re-binding the value mid-typing can
                     swallow a keystroke. The remembered query is set once. -->
                <input class="tb-sel tb-search" data-tb="search" placeholder="Search key or title…"
                       t-ref="search" t-on-input="onSearch"/>
                <button class="tb-btn primary" data-tb="create" t-on-click="createTicket">+ Create</button>
                <div class="tb-spacer"/>
                <t t-if="state.stats">
                    <span class="tb-stat"><b t-esc="state.stats.open"/> open</span>
                    <span class="tb-stat"><b t-esc="state.stats.closed"/> done</span>
                    <span class="tb-stat" t-if="state.stats.blocked">
                        <b class="tb-red" t-esc="state.stats.blocked"/> blocked
                    </span>
                    <span class="tb-stat">
                        <b t-esc="fmtH(state.stats.logged_hours)"/> /
                        <t t-esc="fmtH(state.stats.planned_hours)"/> h
                    </span>
                </t>
                <button class="tb-btn" t-on-click="reload">Refresh</button>
            </div>

            <t t-if="state.error"><div class="tb-error" t-esc="state.error"/></t>

            <div class="tb-cols">
                <t t-foreach="state.stages" t-as="st" t-key="st.id">
                    <div class="tb-col" t-att-class="{drop: state.dropStage === st.id, fold: st.fold}"
                         t-on-dragover="(ev) => this.onDragOver(ev, st.id)"
                         t-on-dragleave="() => this.onDragLeave(st.id)"
                         t-on-drop="(ev) => this.onDrop(ev, st.id, -1)">
                        <div class="tb-col-h">
                            <span class="tb-col-n" t-esc="st.name"/>
                            <span class="tb-col-c" t-esc="cardsIn(st.id).length"/>
                            <span class="tb-col-h2" t-if="hoursIn(st.id)" t-esc="fmtH(hoursIn(st.id)) + 'h'"/>
                        </div>
                        <div class="tb-cards">
                            <t t-foreach="cardsIn(st.id)" t-as="t" t-key="t.id">
                                <div class="tb-card"
                                     t-att-class="'ks-' + t.kanban_state + (state.dragId === t.id ? ' dragging' : '')"
                                     draggable="true"
                                     t-on-dragstart="(ev) => this.onDragStart(ev, t)"
                                     t-on-dragend="onDragEnd"
                                     t-on-drop.stop="(ev) => this.onDrop(ev, st.id, t_index)"
                                     t-att-data-key="t.key"
                                     t-on-click="() => this.open(t.id)">
                                    <div class="tb-card-k">
                                        <span t-attf-class="tf-type sm tf-type-{{ t.issue_type }}"
                                              t-att-title="ui.type(t.issue_type).label"
                                              t-esc="ui.type(t.issue_type).icon"/>
                                        <span class="tb-key" t-esc="t.key"/>
                                        <span t-if="t.priority" t-attf-class="tb-prio p{{ t.priority }}"
                                              t-att-title="ui.priority(t.priority).label + ' priority'"
                                              t-esc="ui.priority(t.priority).icon"/>
                                    </div>
                                    <div class="tb-card-t">
                                        <span t-esc="t.name"/>
                                    </div>
                                    <div class="tb-card-m" t-if="!state.projectId" t-esc="t.project_name"/>
                                    <div class="tb-tags" t-if="t.tags and t.tags.length">
                                        <t t-foreach="t.tags" t-as="g" t-key="g.id">
                                            <span class="tb-tag" t-esc="g.name"/>
                                        </t>
                                    </div>
                                    <div class="tb-card-f">
                                        <span class="tb-av" t-if="t.user_id" t-att-title="t.user_name"
                                              t-esc="ui.initials(t.user_name)"/>
                                        <span class="tb-due" t-if="t.date_deadline"
                                              t-att-class="{late: isLate(t)}" t-esc="t.date_deadline"/>
                                        <span class="tb-cnt" t-if="t.comment_count" title="Comments"
                                              t-esc="'💬 ' + t.comment_count"/>
                                        <span class="tb-cnt" t-if="t.attachment_count" title="Attachments"
                                              t-esc="'📎 ' + t.attachment_count"/>
                                        <span class="tb-hrs" t-if="t.planned_hours or t.logged_hours"
                                              t-esc="fmtH(t.logged_hours) + '/' + fmtH(t.planned_hours) + 'h'"/>
                                    </div>
                                    <div class="tb-bar" t-if="t.planned_hours">
                                        <span class="tb-bar-f" t-att-class="{over: t.logged_hours > t.planned_hours}"
                                              t-att-style="'width:' + pct(t) + '%'"/>
                                    </div>
                                    <!-- Dragging is not an accessible interaction, so the same
                                         move is always available from the keyboard. -->
                                    <div class="tb-nav" t-on-click.stop="() => {}">
                                        <button class="tb-mv" t-att-disabled="!prevStage(st.id)"
                                                title="Move to previous stage"
                                                t-on-click.stop="() => this.shift(t, -1)">‹</button>
                                        <button class="tb-mv" t-att-disabled="!nextStage(st.id)"
                                                title="Move to next stage"
                                                t-on-click.stop="() => this.shift(t, 1)">›</button>
                                    </div>
                                </div>
                            </t>

                            <div class="tb-empty" t-if="!cardsIn(st.id).length">Drop a task here</div>
                        </div>

                        <div class="tb-add" t-if="state.projectId">
                            <input class="tb-add-in" placeholder="+ New task…"
                                   t-att-value="state.newIn === st.id ? state.newName : ''"
                                   t-on-focus="() => this.focusAdd(st.id)"
                                   t-on-input="(ev) => { state.newName = ev.target.value; }"
                                   t-on-keydown="(ev) => this.onAddKey(ev, st.id)"/>
                        </div>
                    </div>
                </t>

                <div class="tb-none" t-if="!state.stages.length and !state.loading">
                    No stages defined. Add some under Project → Task Stages.
                </div>
            </div>
        </div>`;

    setup() {
        this.ui = window.TicketUI;
        // The filters come back as you left them: open a ticket, press Back,
        // and you are looking at the same board, not "All projects" again.
        const saved = this.loadFilters();
        this.state = owl.useState({
            projects: [], users: [], stages: [], tasks: [], tags: [],
            projectId: saved.projectId || 0, userId: saved.userId || 0,
            type: saved.type || '', tagId: saved.tagId || 0, q: saved.q || '',
            stats: null,
            dragId: 0, dropStage: 0,
            newIn: 0, newName: '',
            loading: false, error: '', busy: false,
        });
        this.searchRef = owl.useRef('search');
        owl.onMounted(() => { if (this.searchRef.el) this.searchRef.el.value = this.state.q; });
        owl.onWillStart(async () => {
            try {
                const [projects, users, tags] = await Promise.all([
                    RpcService.call('project.project', 'search_read', [[]],
                                    { fields: ['name', 'display_name'], limit: 200 }),
                    RpcService.call('res.users', 'search_read', [[]],
                                    { fields: ['login'], limit: 100 }),
                    RpcService.call('project.tag', 'search_read', [[['active', '=', true]]],
                                    { fields: ['name'], limit: 500 }),
                ]);
                this.state.projects = projects || [];
                this.state.users = users || [];
                this.state.tags = (tags || []).sort((a, b) => a.name.localeCompare(b.name));
                // A remembered project that no longer exists would show an
                // empty board with no way to tell why.
                if (this.state.projectId && !this.state.projects.some(p => p.id === this.state.projectId))
                    this.state.projectId = 0;
            } catch (e) { /* the pickers are a convenience; the board still loads */ }
            await this.reload();
        });
    }

    get filter() {
        const f = {};
        if (this.state.projectId) f.project_id = this.state.projectId;
        if (this.state.userId) f.user_id = this.state.userId;
        if (this.state.type) f.issue_type = this.state.type;
        if (this.state.tagId) f.tag_id = this.state.tagId;
        if (this.state.q.trim()) f.q = this.state.q.trim();
        return f;
    }
    loadFilters() {
        try { return JSON.parse(window.localStorage.getItem('tb.filters') || '{}') || {}; }
        catch (e) { return {}; }
    }
    saveFilters() {
        const s = this.state;
        try {
            window.localStorage.setItem('tb.filters', JSON.stringify(
                { projectId: s.projectId, userId: s.userId, type: s.type, tagId: s.tagId, q: s.q }));
        } catch (e) { /* a convenience, never required */ }
    }

    async reload() {
        this.state.loading = true;
        this.state.error = '';
        try {
            const [board, stats] = await Promise.all([
                RpcService.call('project.task', 'board', [this.filter], {}),
                RpcService.call('project.project', 'stats',
                                [this.state.projectId ? { project_id: this.state.projectId } : {}], {}),
            ]);
            this.state.stages = (board && board.stages) || [];
            this.state.tasks = (board && board.tasks) || [];
            this.state.stats = stats || null;
        } catch (e) {
            this.state.error = (e && e.message) || 'Could not load the board.';
        } finally {
            this.state.loading = false;
        }
    }

    // ---- reading the board -------------------------------------------------
    cardsIn(stageId) {
        return this.state.tasks
            .filter(t => (t.stage_id || 0) === stageId)
            .sort((a, b) => (a.sequence - b.sequence) || (a.id - b.id));
    }
    hoursIn(stageId) {
        return this.cardsIn(stageId).reduce((s, t) => s + (t.planned_hours || 0), 0);
    }
    pct(t) {
        if (!t.planned_hours) return 0;
        return Math.min(100, Math.round((t.logged_hours / t.planned_hours) * 100));
    }
    isLate(t) {
        if (!t.date_deadline) return false;
        const st = this.state.stages.find(s => s.id === (t.stage_id || 0));
        if (st && st.is_closed) return false;   // a finished task is never late
        return t.date_deadline < new Date().toISOString().slice(0, 10);
    }
    stageIndex(id) { return this.state.stages.findIndex(s => s.id === id); }
    prevStage(id) { const i = this.stageIndex(id); return i > 0 ? this.state.stages[i - 1] : null; }
    nextStage(id) {
        const i = this.stageIndex(id);
        return (i >= 0 && i < this.state.stages.length - 1) ? this.state.stages[i + 1] : null;
    }

    // ---- drag and drop -----------------------------------------------------
    onDragStart(ev, task) {
        this.state.dragId = task.id;
        // Firefox refuses to start a drag without data on the transfer.
        try {
            ev.dataTransfer.setData('text/plain', String(task.id));
            ev.dataTransfer.effectAllowed = 'move';
        } catch (e) { /* not fatal */ }
    }
    onDragEnd() { this.state.dragId = 0; this.state.dropStage = 0; }
    onDragOver(ev, stageId) {
        ev.preventDefault();                       // required to allow a drop
        try { ev.dataTransfer.dropEffect = 'move'; } catch (e) { /* not fatal */ }
        this.state.dropStage = stageId;
    }
    onDragLeave(stageId) { if (this.state.dropStage === stageId) this.state.dropStage = 0; }

    async onDrop(ev, stageId, index) {
        ev.preventDefault();
        ev.stopPropagation();
        const id = this.state.dragId || parseInt(ev.dataTransfer.getData('text/plain'), 10);
        this.state.dragId = 0;
        this.state.dropStage = 0;
        if (!id) return;
        await this.move(id, stageId, index);
    }

    async shift(task, dir) {
        const target = dir < 0 ? this.prevStage(task.stage_id || 0) : this.nextStage(task.stage_id || 0);
        if (target) await this.move(task.id, target.id, -1);
    }

    /**
     * Apply the move locally first, then persist. On failure the previous
     * position is restored — optimism without a rollback is just a lie.
     */
    async move(taskId, stageId, index) {
        const task = this.state.tasks.find(t => t.id === taskId);
        if (!task) return;
        const before = { stage_id: task.stage_id, sequence: task.sequence };
        if (before.stage_id === stageId && index < 0) return;

        const column = this.cardsIn(stageId).filter(t => t.id !== taskId);
        const at = index < 0 ? column.length : Math.min(index, column.length);
        column.splice(at, 0, task);
        column.forEach((t, i) => { t.sequence = (i + 1) * 10; });
        task.stage_id = stageId;

        this.state.busy = true;
        this.state.error = '';
        try {
            await RpcService.call('project.task', 'move_stage',
                                  [{ task_id: taskId, stage_id: stageId, index: at }], {});
            // Stage changes can close a task, which changes the counters.
            this.state.stats = await RpcService.call(
                'project.project', 'stats',
                [this.state.projectId ? { project_id: this.state.projectId } : {}], {});
        } catch (e) {
            task.stage_id = before.stage_id;
            task.sequence = before.sequence;
            this.state.error = (e && e.message) || 'The task could not be moved.';
        } finally {
            this.state.busy = false;
        }
    }

    // ---- quick add ---------------------------------------------------------
    focusAdd(stageId) {
        if (this.state.newIn !== stageId) { this.state.newIn = stageId; this.state.newName = ''; }
    }
    async onAddKey(ev, stageId) {
        if (ev.key === 'Escape') { this.state.newIn = 0; this.state.newName = ''; return; }
        if (ev.key !== 'Enter') return;
        const name = (this.state.newName || '').trim();
        if (!name || !this.state.projectId) return;
        this.state.error = '';
        try {
            await RpcService.call('project.task', 'create',
                [{ name, project_id: this.state.projectId, stage_id: stageId,
                   user_id: this.state.userId || false }], {});
            this.state.newName = '';
            await this.reload();
        } catch (e) {
            this.state.error = (e && e.message) || 'The task could not be created.';
        }
    }

    // ---- top controls ------------------------------------------------------
    async onProject(ev) { this.state.projectId = parseInt(ev.target.value, 10) || 0; await this.refilter(); }
    async onUser(ev)    { this.state.userId    = parseInt(ev.target.value, 10) || 0; await this.refilter(); }
    async onType(ev)    { this.state.type      = ev.target.value || '';              await this.refilter(); }
    async onTag(ev)     { this.state.tagId     = parseInt(ev.target.value, 10) || 0; await this.refilter(); }
    onSearch(ev) {
        this.state.q = ev.target.value;
        clearTimeout(this._qT);
        const wait = (window.UI_TIMING && window.UI_TIMING.debounce) || 150;
        this._qT = setTimeout(() => this.refilter(), wait);
    }
    async refilter() { this.saveFilters(); await this.reload(); }

    /** A new ticket, in the project being looked at. */
    createTicket() {
        if (window.ErpNav && window.ErpNav.openRecord)
            window.ErpNav.openRecord('project.task', 'new',
                                     { from: 'board', project_id: this.state.projectId || 0 });
    }

    fmtH(h) {
        const v = Number(h || 0);
        return Number.isInteger(v) ? String(v) : v.toFixed(1);
    }
    /**
     * Open a record in the shell. Falls back to a warning rather than pretending
     * it worked — the previous `location.hash` version failed silently because
     * this app has no hash router at all.
     */
    openRecord(model, id, defaults) {
        if (window.ErpNav && window.ErpNav.openRecord) return window.ErpNav.openRecord(model, id, defaults);
        console.warn('Cannot navigate: the shell is not mounted.');
        return false;
    }

    // from: 'board' makes the ticket's Back return here rather than to the list.
    open(id) { return this.openRecord('project.task', id, { from: 'board' }); }
}
