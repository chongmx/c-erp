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
                                     t-on-click="() => this.open(t.id)">
                                    <div class="tb-card-t">
                                        <span class="tb-prio" t-if="t.priority" title="High priority">★</span>
                                        <span t-esc="t.name"/>
                                    </div>
                                    <div class="tb-card-m" t-if="!state.projectId" t-esc="t.project_name"/>
                                    <div class="tb-card-f">
                                        <span class="tb-who" t-if="t.user_login" t-esc="t.user_login"/>
                                        <span class="tb-due" t-if="t.date_deadline"
                                              t-att-class="{late: isLate(t)}" t-esc="t.date_deadline"/>
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
        this.state = owl.useState({
            projects: [], users: [], stages: [], tasks: [],
            projectId: 0, userId: 0, stats: null,
            dragId: 0, dropStage: 0,
            newIn: 0, newName: '',
            loading: false, error: '', busy: false,
        });
        owl.onWillStart(async () => {
            try {
                const [projects, users] = await Promise.all([
                    RpcService.call('project.project', 'search_read', [[]],
                                    { fields: ['name', 'display_name'], limit: 200 }),
                    RpcService.call('res.users', 'search_read', [[]],
                                    { fields: ['login'], limit: 100 }),
                ]);
                this.state.projects = projects || [];
                this.state.users = users || [];
            } catch (e) { /* the pickers are a convenience; the board still loads */ }
            await this.reload();
        });
    }

    get filter() {
        const f = {};
        if (this.state.projectId) f.project_id = this.state.projectId;
        if (this.state.userId) f.user_id = this.state.userId;
        return f;
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
    async onProject(ev) { this.state.projectId = parseInt(ev.target.value, 10) || 0; await this.reload(); }
    async onUser(ev)    { this.state.userId    = parseInt(ev.target.value, 10) || 0; await this.reload(); }

    fmtH(h) {
        const v = Number(h || 0);
        return Number.isInteger(v) ? String(v) : v.toFixed(1);
    }
    open(id) { window.location.hash = '#action=tasks&view=form&id=' + id; }
}
