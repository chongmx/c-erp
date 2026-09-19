/**
 * TaskForm.js — one ticket: Project → Tasks / the Task Board (issue tracker).
 *
 * A task is a ticket here: it has a key (CERP-12), a type, a priority, labels,
 * a reporter and watchers, and a single activity feed that holds both what
 * people SAID (comments) and what CHANGED (history, written by the server on
 * every write — see ProjectTaskViewModel).
 *
 * Screenshots are the point of a bug report, so they are one keystroke:
 * paste (or drop) an image into the description or a comment and it is
 * uploaded onto the ticket and referenced inline as
 *     ![screenshot-20260919-104500.png](/web/content/42)
 * which is rendered back as the image. Only /web/content/<id> is ever turned
 * into an <img>; anything else in the text stays text.
 *
 * Sidebar fields save on change, like Jira: there is no Save button to forget.
 * The title saves on Enter or when it loses focus; the description has an
 * explicit Save because it is long-form.
 *
 * Also defines window.TicketUI — the type/priority vocabulary and icons —
 * shared with TaskBoard.js, which loads after this file.
 */
const TicketUI = {
    types: [
        { value: 'task',    label: 'Task',    icon: '✓' },
        { value: 'bug',     label: 'Bug',     icon: '●' },
        { value: 'feature', label: 'Feature', icon: '+' },
        { value: 'chore',   label: 'Chore',   icon: '⚙' },
    ],
    priorities: [
        { value: 2,  label: 'Urgent', icon: '⇈' },
        { value: 1,  label: 'High',   icon: '↑' },
        { value: 0,  label: 'Normal', icon: '=' },
        { value: -1, label: 'Low',    icon: '↓' },
    ],
    type(v)     { return this.types.find(t => t.value === v) || this.types[0]; },
    priority(v) { return this.priorities.find(p => p.value === Number(v || 0)) || this.priorities[2]; },
    initials(name) {
        const parts = String(name || '').trim().split(/\s+/).filter(Boolean);
        if (!parts.length) return '?';
        return ((parts[0][0] || '') + (parts.length > 1 ? parts[parts.length - 1][0] : '')).toUpperCase();
    },
};
window.TicketUI = TicketUI;

const TF_IMAGE_RE = /^\/web\/content\/\d+$/;

/**
 * Ticket text — the description, a comment, the create-form preview.
 * Plain text, with three things made live:
 *   ![name](/web/content/<id>)   an uploaded image (only that URL shape)
 *   CERP-12                      a ticket key, when the prefix is a real project
 *   https://...                  a link
 * Everything goes through t-esc / t-att, so no text ever becomes markup.
 */
class TicketRich extends owl.Component {
    static props = ['text?', 'prefixes?', 'onImage?', 'onKey?'];
    static template = owl.xml`
        <t t-foreach="segments" t-as="sg" t-key="sg_index">
            <t t-if="sg.kind === 'img'">
                <img class="tf-inline-img" t-att-src="sg.src" t-att-alt="sg.alt"
                     t-on-click="() => this.image(sg.src)"/>
            </t>
            <t t-elif="sg.kind === 'key'">
                <a class="tf-keylink" href="#" t-on-click.prevent="() => this.key(sg.text)" t-esc="sg.text"/>
            </t>
            <t t-elif="sg.kind === 'url'">
                <a t-att-href="sg.text" target="_blank" rel="noopener noreferrer" t-esc="sg.text"/>
            </t>
            <t t-else=""><t t-esc="sg.text"/></t>
        </t>`;

    get segments() {
        const out = [];
        const re = /!\[([^\]\n]*)\]\(([^)\s]+)\)|\b([A-Z][A-Z0-9]{1,9}-\d+)\b|(https?:\/\/[^\s<>()]+)/g;
        const s = String(this.props.text || '');
        const prefixes = this.props.prefixes || [];
        let last = 0, m;
        while ((m = re.exec(s)) !== null) {
            let seg = null;
            if (m[2] !== undefined) {
                if (TF_IMAGE_RE.test(m[2])) seg = { kind: 'img', src: m[2], alt: m[1] || 'image' };
            } else if (m[3] !== undefined) {
                const prefix = m[3].slice(0, m[3].lastIndexOf('-'));
                if (prefixes.includes(prefix)) seg = { kind: 'key', text: m[3] };
            } else if (m[4] !== undefined) {
                seg = { kind: 'url', text: m[4] };
            }
            if (!seg) continue;
            if (m.index > last) out.push({ kind: 'text', text: s.slice(last, m.index) });
            out.push(seg);
            last = re.lastIndex;
        }
        if (last < s.length) out.push({ kind: 'text', text: s.slice(last) });
        return out;
    }
    image(src) { if (this.props.onImage) this.props.onImage(src); }
    key(k)     { if (this.props.onKey) this.props.onKey(k); }
}

class TaskForm extends owl.Component {
    static components = { M2OSelect, TicketRich };
    static props = ['recordId?', 'defaults?', 'onBack?', '*'];
    static template = owl.xml`
        <div class="tf-shell" t-on-keydown="onShellKey">
            <div class="tf-top">
                <button class="tf-btn ghost" t-on-click="back">← Back</button>
                <button class="tf-btn ghost" t-on-click="openBoard">Board</button>
                <span class="tf-crumb" t-if="state.task">
                    <t t-esc="state.task.project_name"/> /
                    <b class="tf-key" t-esc="state.task.key"/>
                </span>
                <span class="tf-crumb" t-elif="state.isNew">New ticket</span>
                <div class="tf-spacer"/>
                <span class="tf-flash" t-if="state.flash" t-esc="state.flash"/>
                <button t-if="state.task" class="tf-btn tf-watch-btn"
                        t-att-class="{on: state.task.watching}" t-on-click="toggleWatch"
                        t-esc="state.task.watching ? 'Watching' : 'Watch'"/>
            </div>

            <div class="tf-error" t-if="state.error" t-esc="state.error"/>

            <t t-if="state.loading">
                <div class="tf-loading">Loading…</div>
            </t>

            <!-- ───────────── a new ticket ───────────── -->
            <t t-elif="state.isNew">
                <div class="tf-new">
                    <h2 class="tf-new-h">Create ticket</h2>
                    <div class="tf-new-row">
                        <label>Project</label>
                        <div class="tf-new-in">
                            <M2OSelect model="'project.project'" value="state.form.project_id"
                                       label="'Project'" onSelect.bind="onNewProject"/>
                        </div>
                    </div>
                    <div class="tf-new-row">
                        <label>Type</label>
                        <select class="tf-sel" data-tf="new-type" t-on-change="onNewType">
                            <t t-foreach="ui.types" t-as="ty" t-key="ty.value">
                                <option t-att-value="ty.value" t-att-selected="ty.value === state.form.issue_type"
                                        t-esc="ty.icon + '  ' + ty.label"/>
                            </t>
                        </select>
                    </div>
                    <div class="tf-new-row">
                        <label>Summary</label>
                        <input class="tf-in" data-tf="new-name" t-att-value="state.form.name"
                               placeholder="What needs doing, in one line" t-on-input="onNewName"/>
                    </div>
                    <div class="tf-new-row">
                        <label>Priority</label>
                        <select class="tf-sel" data-tf="new-priority" t-on-change="onNewPriority">
                            <t t-foreach="ui.priorities" t-as="pr" t-key="pr.value">
                                <option t-att-value="pr.value" t-att-selected="pr.value === state.form.priority"
                                        t-esc="pr.icon + '  ' + pr.label"/>
                            </t>
                        </select>
                    </div>
                    <div class="tf-new-row">
                        <label>Assignee</label>
                        <div class="tf-new-in">
                            <M2OSelect model="'res.users'" value="state.form.user_id"
                                       label="'Assignee'" onSelect.bind="onNewAssignee"/>
                        </div>
                    </div>
                    <div class="tf-new-row top">
                        <label>Description</label>
                        <div class="tf-new-in">
                            <textarea class="tf-textarea" data-draft="newDesc" data-tf="new-desc" rows="8"
                                      placeholder="Steps, expected, actual. Paste a screenshot to include it."
                                      t-att-value="state.drafts.newDesc"
                                      t-on-input="onDraft" t-on-paste="onPaste" t-on-drop="onDropText"
                                      t-on-dragover="onDragOverText"/>
                            <div class="tf-hint" t-if="state.uploading">Uploading <t t-esc="state.uploadName"/>…</div>
                            <div class="tf-preview" t-if="state.drafts.newDesc">
                                <TicketRich text="state.drafts.newDesc" prefixes="state.prefixes"
                                            onImage.bind="openLightbox" onKey.bind="openKey"/>
                            </div>
                        </div>
                    </div>
                    <div class="tf-new-actions">
                        <button class="btn btn-primary" data-tf="create" t-on-click="create"
                                t-att-disabled="state.busy">Create</button>
                        <button class="btn" t-on-click="back">Cancel</button>
                    </div>
                </div>
            </t>

            <!-- ───────────── an existing ticket ───────────── -->
            <t t-elif="state.task">
                <div class="tf-body">
                    <div class="tf-main">
                        <div class="tf-title-row">
                            <span t-attf-class="tf-type tf-type-{{ state.task.issue_type }}"
                                  t-att-title="ui.type(state.task.issue_type).label"
                                  t-esc="ui.type(state.task.issue_type).icon"/>
                            <input class="tf-title" data-tf="title" t-att-value="state.task.name"
                                   t-on-keydown="onTitleKey" t-on-change="onTitleChange"/>
                        </div>
                        <div class="tf-sub" t-if="state.task.parent_id">
                            Subtask of
                            <a class="tf-link" href="#" t-on-click.prevent="() => this.openTask(state.task.parent_id)"
                               t-esc="state.task.parent_key + ' ' + state.task.parent_name"/>
                        </div>

                        <!-- description -->
                        <section class="tf-sec">
                            <div class="tf-sec-h">
                                <span>Description</span>
                                <button t-if="!state.descEditing" class="tf-link" data-tf="edit-desc"
                                        t-on-click="editDesc">Edit</button>
                            </div>
                            <t t-if="state.descEditing">
                                <textarea class="tf-textarea" data-draft="desc" data-tf="desc" rows="10"
                                          t-att-value="state.drafts.desc" t-on-input="onDraft"
                                          t-on-paste="onPaste" t-on-drop="onDropText" t-on-dragover="onDragOverText"/>
                                <div class="tf-actions">
                                    <button class="btn btn-primary btn-sm" data-tf="save-desc" t-on-click="saveDesc">Save</button>
                                    <button class="btn btn-sm" t-on-click="cancelDesc">Cancel</button>
                                    <span class="tf-hint">Paste or drop a screenshot to put it here.</span>
                                </div>
                            </t>
                            <t t-elif="state.task.description">
                                <div class="tf-rich tf-desc" t-on-dblclick="editDesc">
                                    <TicketRich text="state.task.description" prefixes="state.prefixes"
                                                onImage.bind="openLightbox" onKey.bind="openKey"/>
                                </div>
                            </t>
                            <t t-else="">
                                <div class="tf-empty" t-on-click="editDesc">Add a description…</div>
                            </t>
                        </section>

                        <!-- attachments -->
                        <section class="tf-sec">
                            <div class="tf-sec-h">
                                <span>Attachments</span>
                                <span class="tf-count" t-esc="state.attachments.length"/>
                                <button class="tf-link" data-tf="attach" t-on-click="pickFiles">Attach</button>
                            </div>
                            <input type="file" class="tf-file" multiple="multiple" t-ref="fileInput"
                                   data-tf="file-input" t-on-change="onFilesChosen"/>
                            <div class="tf-thumbs" t-att-class="{over: state.dragOver}"
                                 t-on-dragover.prevent="() => { this.state.dragOver = true; }"
                                 t-on-dragleave="() => { this.state.dragOver = false; }"
                                 t-on-drop.prevent="onDropFiles">
                                <t t-foreach="state.attachments" t-as="a" t-key="a.id">
                                    <div class="tf-thumb" t-att-data-att="a.id">
                                        <t t-if="isImage(a)">
                                            <img class="tf-thumb-img" t-att-src="a.url" t-att-alt="a.name"
                                                 t-on-click="() => this.openLightbox(a.url)"/>
                                        </t>
                                        <t t-else="">
                                            <a class="tf-file-chip" t-att-href="a.url" target="_blank" rel="noopener"
                                               t-esc="a.name"/>
                                        </t>
                                        <div class="tf-thumb-f">
                                            <span class="tf-thumb-n" t-att-title="a.name" t-esc="a.name"/>
                                            <span class="tf-x" title="Remove" t-on-click.stop="() => this.removeAttachment(a)">✕</span>
                                        </div>
                                    </div>
                                </t>
                                <div class="tf-drop" t-if="!state.attachments.length">
                                    Drop files here, paste a screenshot, or use Attach.
                                </div>
                            </div>
                            <div class="tf-hint" t-if="state.uploading">Uploading <t t-esc="state.uploadName"/>…</div>
                        </section>

                        <!-- subtasks -->
                        <section class="tf-sec">
                            <div class="tf-sec-h">
                                <span>Subtasks</span>
                                <span class="tf-count" t-esc="state.task.subtasks.length"/>
                            </div>
                            <div class="tf-subtasks">
                                <t t-foreach="state.task.subtasks" t-as="s" t-key="s.id">
                                    <div class="tf-subtask" t-att-class="{closed: s.closed}"
                                         t-on-click="() => this.openTask(s.id)">
                                        <span t-attf-class="tf-type sm tf-type-{{ s.issue_type }}"
                                              t-esc="ui.type(s.issue_type).icon"/>
                                        <span class="tf-key" t-esc="s.key"/>
                                        <span class="tf-subtask-n" t-esc="s.name"/>
                                        <span class="tf-pill" t-esc="s.stage"/>
                                    </div>
                                </t>
                                <input class="tf-in tf-sub-add" data-tf="subtask" placeholder="+ Add a subtask and press Enter"
                                       t-att-value="state.drafts.subtask" data-draft="subtask"
                                       t-on-input="onDraft" t-on-keydown="onSubtaskKey"/>
                            </div>
                        </section>

                        <!-- activity -->
                        <section class="tf-sec">
                            <div class="tf-sec-h">
                                <span>Activity</span>
                                <span class="tf-tabs">
                                    <button t-att-class="{on: state.feed === 'all'}" t-on-click="() => this.setFeed('all')">All</button>
                                    <button t-att-class="{on: state.feed === 'comment'}" t-on-click="() => this.setFeed('comment')">Comments</button>
                                    <button t-att-class="{on: state.feed === 'tracking'}" t-on-click="() => this.setFeed('tracking')">History</button>
                                </span>
                            </div>
                            <div class="tf-feed">
                                <t t-foreach="feed" t-as="m" t-key="m.id">
                                    <div t-attf-class="tf-entry tf-{{ m.subtype }}" t-att-data-msg="m.id">
                                        <div class="tf-av" t-att-title="m.author_name" t-esc="ui.initials(m.author_name)"/>
                                        <div class="tf-entry-b">
                                            <div class="tf-entry-h">
                                                <b t-esc="m.author_name"/>
                                                <span class="tf-when" t-att-title="m.date" t-esc="when(m.date)"/>
                                                <span class="tf-muted" t-if="m.edited">(edited)</span>
                                                <t t-if="m.can_edit and state.editId !== m.id">
                                                    <button class="tf-link sm" t-on-click="() => this.startEdit(m)">Edit</button>
                                                    <button class="tf-link sm" t-on-click="() => this.deleteComment(m)">Delete</button>
                                                </t>
                                            </div>
                                            <t t-if="state.editId === m.id">
                                                <textarea class="tf-textarea" data-draft="edit" rows="4"
                                                          t-att-value="state.drafts.edit" t-on-input="onDraft"
                                                          t-on-paste="onPaste"/>
                                                <div class="tf-actions">
                                                    <button class="btn btn-primary btn-sm" t-on-click="saveEdit">Save</button>
                                                    <button class="btn btn-sm" t-on-click="cancelEdit">Cancel</button>
                                                </div>
                                            </t>
                                            <t t-elif="m.subtype === 'tracking'">
                                                <div class="tf-track">
                                                    <t t-foreach="lines(m.body)" t-as="ln" t-key="ln_index">
                                                        <div t-esc="ln"/>
                                                    </t>
                                                </div>
                                            </t>
                                            <t t-else="">
                                                <div class="tf-rich">
                                                    <TicketRich text="m.body" prefixes="state.prefixes"
                                                                onImage.bind="openLightbox" onKey.bind="openKey"/>
                                                </div>
                                            </t>
                                        </div>
                                    </div>
                                </t>
                                <div class="tf-empty" t-if="!feed.length">Nothing here yet.</div>
                            </div>
                            <div class="tf-compose">
                                <textarea class="tf-textarea" data-draft="comment" data-tf="comment" rows="3"
                                          placeholder="Add a comment… paste a screenshot to include it"
                                          t-ref="commentArea"
                                          t-att-value="state.drafts.comment" t-on-input="onDraft"
                                          t-on-paste="onPaste" t-on-drop="onDropText" t-on-dragover="onDragOverText"
                                          t-on-keydown="onCommentKey"/>
                                <div class="tf-actions">
                                    <button class="btn btn-primary btn-sm" data-tf="post" t-on-click="postComment"
                                            t-att-disabled="state.busy or !state.drafts.comment.trim()">Comment</button>
                                    <button class="btn btn-sm" data-tf="comment-image" t-on-click="pickCommentImage">Add image</button>
                                    <input type="file" class="tf-file" accept="image/*" t-ref="commentFile"
                                           data-tf="comment-file" t-on-change="onCommentFileChosen"/>
                                    <span class="tf-hint">Ctrl+Enter to send</span>
                                </div>
                            </div>
                        </section>
                    </div>

                    <!-- the sidebar: every field saves on change -->
                    <aside class="tf-side">
                        <div class="tf-field">
                            <label>Status</label>
                            <select class="tf-sel" data-tf="stage" t-on-change="onStage">
                                <option value="0" t-if="!state.task.stage_id">—</option>
                                <t t-foreach="state.task.stages" t-as="s" t-key="s.id">
                                    <option t-att-value="s.id" t-att-selected="s.id === state.task.stage_id" t-esc="s.name"/>
                                </t>
                            </select>
                        </div>
                        <div class="tf-field">
                            <label>Assignee</label>
                            <M2OSelect model="'res.users'" value="userValue('user')" label="'Assignee'"
                                       placeholder="'Unassigned'" onSelect.bind="onAssignee"/>
                            <button class="tf-link sm" data-tf="assign-me" t-on-click="assignToMe"
                                    t-if="state.task.user_id !== myUid">Assign to me</button>
                        </div>
                        <div class="tf-field">
                            <label>Reporter</label>
                            <M2OSelect model="'res.users'" value="userValue('reporter')" label="'Reporter'"
                                       onSelect.bind="onReporter"/>
                        </div>
                        <div class="tf-field">
                            <label>Type</label>
                            <select class="tf-sel" data-tf="type" t-on-change="onType">
                                <t t-foreach="ui.types" t-as="ty" t-key="ty.value">
                                    <option t-att-value="ty.value" t-att-selected="ty.value === state.task.issue_type"
                                            t-esc="ty.icon + '  ' + ty.label"/>
                                </t>
                            </select>
                        </div>
                        <div class="tf-field">
                            <label>Priority</label>
                            <select class="tf-sel" data-tf="priority" t-on-change="onPriority">
                                <t t-foreach="ui.priorities" t-as="pr" t-key="pr.value">
                                    <option t-att-value="pr.value" t-att-selected="pr.value === state.task.priority"
                                            t-esc="pr.icon + '  ' + pr.label"/>
                                </t>
                            </select>
                        </div>
                        <div class="tf-field">
                            <label>Labels</label>
                            <div class="tf-tags">
                                <t t-foreach="state.task.tags" t-as="g" t-key="g.id">
                                    <span class="tf-tag" t-att-data-tag="g.name">
                                        <t t-esc="g.name"/>
                                        <span class="tf-x" title="Remove label" t-on-click="() => this.removeTag(g)">✕</span>
                                    </span>
                                </t>
                                <input class="tf-tag-in" data-tf="tag" list="tf-tag-list" placeholder="Add label…"
                                       t-att-value="state.drafts.tag" data-draft="tag"
                                       t-on-input="onDraft" t-on-keydown="onTagKey" t-on-change="onTagPicked"/>
                                <datalist id="tf-tag-list">
                                    <t t-foreach="state.allTags" t-as="g" t-key="g.id">
                                        <option t-att-value="g.name"/>
                                    </t>
                                </datalist>
                            </div>
                        </div>
                        <div class="tf-field">
                            <label>Blocked</label>
                            <label class="tf-check">
                                <input type="checkbox" data-tf="blocked"
                                       t-att-checked="state.task.kanban_state === 'blocked'" t-on-change="onBlocked"/>
                                Flag as blocked
                            </label>
                        </div>
                        <div class="tf-field">
                            <label>Due date</label>
                            <input class="tf-in" type="date" data-tf="due" t-att-value="state.task.date_deadline"
                                   t-on-change="onDue"/>
                        </div>
                        <div class="tf-field">
                            <label>Estimate (h)</label>
                            <input class="tf-in" type="number" min="0" step="0.5" data-tf="estimate"
                                   t-att-value="state.task.planned_hours" t-on-change="onEstimate"/>
                            <span class="tf-muted" t-if="state.task.logged_hours">
                                <t t-esc="fmtH(state.task.logged_hours)"/> h logged
                            </span>
                        </div>
                        <div class="tf-field">
                            <label>Project</label>
                            <M2OSelect model="'project.project'" value="[state.task.project_id, state.task.project_name]"
                                       label="'Project'" onSelect.bind="onProject"/>
                        </div>
                        <div class="tf-field">
                            <label>Parent</label>
                            <M2OSelect model="'project.task'" value="parentValue" label="'Parent ticket'"
                                       placeholder="'None'" domain="parentDomain" onSelect.bind="onParent"/>
                        </div>
                        <div class="tf-field">
                            <label>Watchers <span class="tf-count" t-esc="state.task.watchers.length"/></label>
                            <div class="tf-watchers">
                                <t t-foreach="state.task.watchers" t-as="w" t-key="w.id">
                                    <span class="tf-av sm" t-att-title="w.name" t-esc="ui.initials(w.name)"/>
                                </t>
                            </div>
                        </div>
                        <div class="tf-meta">
                            <div>Created <t t-esc="when(state.task.create_date)"/></div>
                            <div>Updated <t t-esc="when(state.task.write_date)"/></div>
                            <div t-if="state.task.date_end">Closed <t t-esc="state.task.date_end"/></div>
                        </div>
                    </aside>
                </div>
            </t>

            <div class="tf-lightbox" t-if="state.lightbox" t-on-click="closeLightbox">
                <img t-att-src="state.lightbox" alt=""/>
            </div>
        </div>`;

    setup() {
        this.ui = TicketUI;
        this.fileInput = owl.useRef('fileInput');
        this.commentFile = owl.useRef('commentFile');
        this.commentArea = owl.useRef('commentArea');
        const d = this.props.defaults || {};
        this.state = owl.useState({
            loading: false, error: '', flash: '', busy: false,
            isNew: !this.props.recordId,
            task: null, activity: [], attachments: [], allTags: [], prefixes: [],
            feed: 'all', descEditing: false, editId: 0,
            drafts: { comment: '', desc: '', newDesc: '', edit: '', tag: '', subtask: '' },
            form: {
                project_id: d.project_id || this.lastProject() || 0,
                issue_type: d.issue_type || 'task', priority: 0, user_id: 0, name: '',
            },
            pendingAttachments: [],
            uploading: false, uploadName: '', dragOver: false, lightbox: '',
        });
        this.taskId = this.props.recordId || 0;
        owl.onWillStart(async () => {
            await this.loadVocabulary();
            if (this.taskId) await this.load();
        });
    }

    get myUid() { return (RpcService.getSession && RpcService.getSession().uid) || 0; }

    // ---- loading -------------------------------------------------------------
    async loadVocabulary() {
        try {
            const [tags, projects] = await Promise.all([
                RpcService.call('project.tag', 'search_read', [[['active', '=', true]]],
                                { fields: ['name'], limit: 500 }),
                RpcService.call('project.project', 'search_read', [[]],
                                { fields: ['task_prefix'], limit: 500 }),
            ]);
            this.state.allTags = tags || [];
            this.state.prefixes = (projects || []).map(p => p.task_prefix).filter(Boolean);
        } catch (e) { /* suggestions are a convenience */ }
    }

    async load() {
        this.state.loading = !this.state.task;
        this.state.error = '';
        try {
            const [task, activity, files] = await Promise.all([
                RpcService.call('project.task', 'task_detail', [{ id: this.taskId }], {}),
                RpcService.call('project.task', 'activity', [{ task_id: this.taskId }], {}),
                RpcService.call('ir.attachment', 'search_read',
                    [[['res_model', '=', 'project.task'], ['res_id', '=', this.taskId]]], { limit: 200 }),
            ]);
            this.state.task = task;
            this.state.activity = activity || [];
            this.state.attachments = files || [];
            this.state.isNew = false;
        } catch (e) {
            this.state.error = (e && e.message) || 'Could not load the ticket.';
        } finally {
            this.state.loading = false;
        }
    }

    get feed() {
        const f = this.state.feed;
        return f === 'all' ? this.state.activity : this.state.activity.filter(m => m.subtype === f);
    }
    setFeed(f) { this.state.feed = f; }

    // ---- saving --------------------------------------------------------------
    /** Write, then reload: the server writes the history line, so the feed
     *  only tells the truth after a re-read. */
    async save(vals) {
        if (!this.taskId) return;
        this.state.error = '';
        try {
            await RpcService.call('project.task', 'write', [[this.taskId], vals], {});
            await this.load();
            this.flash('Saved');
        } catch (e) {
            this.state.error = (e && e.message) || 'Could not save.';
            await this.load();
        }
    }
    flash(msg) {
        this.state.flash = msg;
        clearTimeout(this._flashT);
        this._flashT = setTimeout(() => { this.state.flash = ''; }, 1500);
    }

    onTitleKey(ev) {
        if (ev.key === 'Enter') { ev.preventDefault(); ev.target.blur(); }
        if (ev.key === 'Escape') { ev.target.value = this.state.task.name; ev.target.blur(); }
    }
    async onTitleChange(ev) {
        const name = ev.target.value.trim();
        if (!name) { ev.target.value = this.state.task.name; return; }
        if (name !== this.state.task.name) await this.save({ name });
    }
    editDesc() {
        this.state.drafts.desc = this.state.task.description || '';
        this.state.descEditing = true;
    }
    cancelDesc() { this.state.descEditing = false; }
    async saveDesc() {
        this.state.descEditing = false;
        if (this.state.drafts.desc !== (this.state.task.description || ''))
            await this.save({ description: this.state.drafts.desc });
    }

    async onStage(ev)    { const v = parseInt(ev.target.value, 10) || 0; if (v) await this.save({ stage_id: v }); }
    async onType(ev)     { await this.save({ issue_type: ev.target.value }); }
    async onPriority(ev) { await this.save({ priority: parseInt(ev.target.value, 10) || 0 }); }
    async onBlocked(ev)  { await this.save({ kanban_state: ev.target.checked ? 'blocked' : 'normal' }); }
    async onDue(ev)      { await this.save({ date_deadline: ev.target.value || false }); }
    async onEstimate(ev) {
        const h = Number(ev.target.value);
        await this.save({ planned_hours: Number.isFinite(h) && h >= 0 ? h : 0 });
    }
    async onAssignee(id) { if ((id || 0) !== (this.state.task.user_id || 0)) await this.save({ user_id: id || false }); }
    async onReporter(id) { if ((id || 0) !== (this.state.task.reporter_id || 0)) await this.save({ reporter_id: id || false }); }
    async assignToMe()   { if (this.myUid) await this.save({ user_id: this.myUid }); }
    async onProject(id) {
        if (!id || id === this.state.task.project_id) return;
        // Moving a ticket gives it the new project's next key — say so.
        if (!window.confirm('Move this ticket to the other project? It will get a new key there.')) {
            await this.load();
            return;
        }
        await this.save({ project_id: id });
    }
    async onParent(id) { if ((id || 0) !== (this.state.task.parent_id || 0)) await this.save({ parent_id: id || false }); }

    userValue(which) {
        const t = this.state.task;
        const id = which === 'user' ? t.user_id : t.reporter_id;
        const name = which === 'user' ? t.user_name : t.reporter_name;
        return id ? [id, name] : 0;
    }
    get parentValue() {
        const t = this.state.task;
        return t.parent_id ? [t.parent_id, t.parent_key + ' ' + t.parent_name] : 0;
    }
    get parentDomain() { return [['id', '!=', this.taskId], ['active', '=', true]]; }

    async toggleWatch() {
        try {
            await RpcService.call('project.task', 'watch',
                                  [{ task_id: this.taskId, watch: !this.state.task.watching }], {});
            await this.load();
        } catch (e) { this.state.error = (e && e.message) || 'Could not change watching.'; }
    }

    // ---- labels --------------------------------------------------------------
    async setTags(tags) {
        try {
            await RpcService.call('project.task', 'set_tags', [{ task_id: this.taskId, tags }], {});
            this.state.drafts.tag = '';
            await Promise.all([this.load(), this.loadVocabulary()]);
            this.flash('Saved');
        } catch (e) { this.state.error = (e && e.message) || 'Could not change the labels.'; }
    }
    async addTag(name) {
        const n = (name || '').trim();
        if (!n) return;
        const have = this.state.task.tags.map(g => g.name);
        if (have.some(h => h.toLowerCase() === n.toLowerCase())) { this.state.drafts.tag = ''; return; }
        await this.setTags([...have, n]);
    }
    async removeTag(g) { await this.setTags(this.state.task.tags.filter(x => x.id !== g.id).map(x => x.id)); }
    async onTagKey(ev) {
        if (ev.key === 'Enter' || ev.key === ',') { ev.preventDefault(); await this.addTag(ev.target.value); }
    }
    /** A pick from the datalist arrives as a change with the full name. */
    async onTagPicked(ev) {
        const v = ev.target.value;
        if (this.state.allTags.some(g => g.name === v)) await this.addTag(v);
    }

    // ---- subtasks ------------------------------------------------------------
    async onSubtaskKey(ev) {
        if (ev.key !== 'Enter') return;
        const name = (this.state.drafts.subtask || '').trim();
        if (!name) return;
        try {
            await RpcService.call('project.task', 'create', [{
                name, project_id: this.state.task.project_id, parent_id: this.taskId }], {});
            this.state.drafts.subtask = '';
            await this.load();
        } catch (e) { this.state.error = (e && e.message) || 'Could not add the subtask.'; }
    }

    // ---- comments ------------------------------------------------------------
    onDraft(ev) {
        const k = ev.target.dataset.draft;
        if (k) this.state.drafts[k] = ev.target.value;
    }
    async onCommentKey(ev) {
        if (ev.key === 'Enter' && (ev.ctrlKey || ev.metaKey)) { ev.preventDefault(); await this.postComment(); }
    }
    async postComment() {
        const body = (this.state.drafts.comment || '').trim();
        if (!body || this.state.busy) return;
        this.state.busy = true;
        try {
            await RpcService.call('project.task', 'post_comment', [{ task_id: this.taskId, body }], {});
            this.state.drafts.comment = '';
            await this.load();
        } catch (e) {
            this.state.error = (e && e.message) || 'Could not post the comment.';
        } finally { this.state.busy = false; }
    }
    startEdit(m) { this.state.drafts.edit = m.body; this.state.editId = m.id; }
    cancelEdit() { this.state.editId = 0; }
    async saveEdit() {
        const id = this.state.editId;
        try {
            await RpcService.call('project.task', 'edit_comment',
                                  [{ message_id: id, body: this.state.drafts.edit }], {});
            this.state.editId = 0;
            await this.load();
        } catch (e) { this.state.error = (e && e.message) || 'Could not save the comment.'; }
    }
    async deleteComment(m) {
        if (!window.confirm('Delete this comment?')) return;
        try {
            await RpcService.call('project.task', 'delete_comment', [{ message_id: m.id }], {});
            await this.load();
        } catch (e) { this.state.error = (e && e.message) || 'Could not delete the comment.'; }
    }

    // ---- files and screenshots -------------------------------------------------
    /** Upload one file. On an existing ticket it is attached to the ticket;
     *  on a new one it is held unlinked and attached once the ticket exists. */
    async uploadFile(file, name) {
        this.state.uploading = true;
        this.state.uploadName = name || file.name;
        try {
            const form = new FormData();
            form.append('file', file, name || file.name);
            form.append('name', name || file.name);
            if (this.taskId) {
                form.append('res_model', 'project.task');
                form.append('res_id', String(this.taskId));
            }
            const resp = await fetch('/web/attachment/upload', {
                method: 'POST', body: form, credentials: 'same-origin',
            });
            const out = await resp.json().catch(() => ({}));
            if (!resp.ok) throw new Error(out.error || ('Upload failed (' + resp.status + ')'));
            if (!this.taskId) this.state.pendingAttachments.push(out.id);
            return out;
        } finally {
            this.state.uploading = false;
            this.state.uploadName = '';
        }
    }
    /** A clipboard image is called "image.png" by every browser; give it a
     *  name that says when it was taken, with the extension its type implies. */
    screenshotName(file) {
        const ext = file.type === 'image/jpeg' ? '.jpg' : file.type === 'image/gif' ? '.gif' : '.png';
        if (file.name && file.name !== 'image.png' && /\.(png|jpe?g|gif)$/i.test(file.name)) return file.name;
        const d = new Date();
        const p = (n) => String(n).padStart(2, '0');
        return 'screenshot-' + d.getFullYear() + p(d.getMonth() + 1) + p(d.getDate()) + '-' +
               p(d.getHours()) + p(d.getMinutes()) + p(d.getSeconds()) + ext;
    }
    /** Put ![name](/web/content/id) at the caret of a draft. */
    insertImage(draftKey, el, out) {
        const token = '![' + (out.name || 'image') + '](/web/content/' + out.id + ')';
        const cur = this.state.drafts[draftKey] || '';
        const a = el && Number.isInteger(el.selectionStart) ? el.selectionStart : cur.length;
        const b = el && Number.isInteger(el.selectionEnd) ? el.selectionEnd : a;
        const before = cur.slice(0, a);
        const sep = before && !before.endsWith('\n') ? '\n' : '';
        this.state.drafts[draftKey] = before + sep + token + '\n' + cur.slice(b);
    }
    async insertFiles(files, el) {
        const key = el && el.dataset ? el.dataset.draft : '';
        this.state.error = '';
        for (const f of files) {
            try {
                const isImg = /^image\//.test(f.type);
                const out = await this.uploadFile(f, isImg ? this.screenshotName(f) : f.name);
                if (isImg && key) this.insertImage(key, el, out);
            } catch (e) {
                this.state.error = (e && e.message) || 'Upload failed.';
            }
        }
        if (this.taskId) await this.reloadAttachments();
    }
    async onPaste(ev) {
        const items = Array.from((ev.clipboardData && ev.clipboardData.items) || []);
        const files = items.filter(i => i.kind === 'file').map(i => i.getAsFile()).filter(Boolean);
        if (!files.length) return;            // an ordinary text paste
        ev.preventDefault();
        await this.insertFiles(files, ev.target);
    }
    onDragOverText(ev) {
        if (ev.dataTransfer && Array.from(ev.dataTransfer.types || []).includes('Files')) ev.preventDefault();
    }
    async onDropText(ev) {
        const files = Array.from((ev.dataTransfer && ev.dataTransfer.files) || []);
        if (!files.length) return;
        ev.preventDefault();
        await this.insertFiles(files, ev.target);
    }
    pickCommentImage() { if (this.commentFile.el) this.commentFile.el.click(); }
    async onCommentFileChosen(ev) {
        const files = Array.from(ev.target.files || []);
        const area = this.commentArea.el || null;
        ev.target.value = '';
        await this.insertFiles(files, area);
    }
    pickFiles() { if (this.fileInput.el) this.fileInput.el.click(); }
    async onFilesChosen(ev) {
        const files = Array.from(ev.target.files || []);
        ev.target.value = '';
        await this.insertFiles(files, null);
    }
    async onDropFiles(ev) {
        this.state.dragOver = false;
        await this.insertFiles(Array.from((ev.dataTransfer && ev.dataTransfer.files) || []), null);
    }
    async reloadAttachments() {
        try {
            this.state.attachments = await RpcService.call('ir.attachment', 'search_read',
                [[['res_model', '=', 'project.task'], ['res_id', '=', this.taskId]]], { limit: 200 }) || [];
            if (this.state.task) this.state.task.attachment_count = this.state.attachments.length;
        } catch (e) { /* the list refreshes on the next load */ }
    }
    async removeAttachment(a) {
        if (!window.confirm('Remove "' + a.name + '"? A comment that shows it will show a broken image.')) return;
        try {
            await RpcService.call('ir.attachment', 'unlink', [[a.id]], {});
            await this.reloadAttachments();
        } catch (e) { this.state.error = (e && e.message) || 'Could not remove the file.'; }
    }
    isImage(a) { return /^image\//.test(a.mimetype || ''); }
    openLightbox(url) { this.state.lightbox = url; }
    closeLightbox() { this.state.lightbox = ''; }
    onShellKey(ev) { if (ev.key === 'Escape' && this.state.lightbox) this.closeLightbox(); }

    // ---- a new ticket --------------------------------------------------------
    lastProject() {
        try { return parseInt(window.localStorage.getItem('tf.lastProject') || '0', 10) || 0; }
        catch (e) { return 0; }
    }
    onNewProject(id)  { this.state.form.project_id = id || 0; }
    onNewAssignee(id) { this.state.form.user_id = id || 0; }
    onNewType(ev)     { this.state.form.issue_type = ev.target.value; }
    onNewPriority(ev) { this.state.form.priority = parseInt(ev.target.value, 10) || 0; }
    onNewName(ev)     { this.state.form.name = ev.target.value; }
    async create() {
        const f = this.state.form;
        if (!f.project_id) { this.state.error = 'Choose a project.'; return; }
        if (!f.name.trim()) { this.state.error = 'Give the ticket a summary.'; return; }
        this.state.busy = true;
        this.state.error = '';
        try {
            const vals = { name: f.name.trim(), project_id: f.project_id, issue_type: f.issue_type,
                           priority: f.priority, description: this.state.drafts.newDesc || '' };
            if (f.user_id) vals.user_id = f.user_id;
            const id = await RpcService.call('project.task', 'create', [vals], {});
            // Screenshots pasted before the ticket existed were stored unlinked.
            if (this.state.pendingAttachments.length)
                await RpcService.call('ir.attachment', 'write',
                    [this.state.pendingAttachments, { res_model: 'project.task', res_id: id }], {});
            try { window.localStorage.setItem('tf.lastProject', String(f.project_id)); } catch (e) { /* optional */ }
            this.taskId = id;
            this.state.pendingAttachments = [];
            await this.load();
            this.flash('Created ' + ((this.state.task && this.state.task.key) || ''));
        } catch (e) {
            this.state.error = (e && e.message) || 'Could not create the ticket.';
        } finally { this.state.busy = false; }
    }

    // ---- rendering helpers ---------------------------------------------------
    lines(body) { return String(body || '').split('\n').filter(Boolean); }
    when(iso) {
        if (!iso) return '';
        const d = new Date(/Z$|[+-]\d\d:?\d\d$/.test(iso) ? iso : iso + 'Z');
        if (isNaN(d.getTime())) return iso;
        const sec = (Date.now() - d.getTime()) / 1000;
        if (sec < 60) return 'just now';
        if (sec < 3600) return Math.floor(sec / 60) + ' min ago';
        if (sec < 86400) return Math.floor(sec / 3600) + ' h ago';
        const p = (n) => String(n).padStart(2, '0');
        return d.getFullYear() + '-' + p(d.getMonth() + 1) + '-' + p(d.getDate()) + ' ' +
               p(d.getHours()) + ':' + p(d.getMinutes());
    }
    fmtH(h) { const v = Number(h || 0); return Number.isInteger(v) ? String(v) : v.toFixed(1); }

    // ---- navigation ------------------------------------------------------------
    back() {
        const d = this.props.defaults || {};
        if (d.from === 'board') return this.openBoard();
        if (this.props.onBack) this.props.onBack();
    }
    openBoard() {
        if (window.ErpNav && window.ErpNav.openRecord) window.ErpNav.openRecord('project.board', 0);
    }
    openTask(id) {
        if (id && window.ErpNav && window.ErpNav.openRecord)
            window.ErpNav.openRecord('project.task', id, this.props.defaults || null);
    }
    async openKey(key) {
        try {
            const r = await RpcService.call('project.task', 'search_read', [[['key', '=', key]]],
                                            { fields: ['id'], limit: 1 });
            if (r && r.length) this.openTask(r[0].id);
            else this.state.error = 'No ticket ' + key + '.';
        } catch (e) { this.state.error = (e && e.message) || ('Could not open ' + key); }
    }
}

window.TaskForm = TaskForm;
