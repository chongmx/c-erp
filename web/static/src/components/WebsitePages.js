/**
 * WebsitePages.js — Settings → Website → Website Pages (docs/128).
 *
 * The generic form rendered `blocks_json` as a textarea, so a page was a wall
 * of JSON and the only way to see what it looked like was to save and go and
 * look at the site. This screen gives the same record two views of itself:
 *
 *   Preview   the blocks RENDERED, by the server, in the site's own stylesheet
 *   Source    the JSON, editable
 *
 * The preview is generated from what is in the editor RIGHT NOW, not from what
 * is saved — otherwise it would answer a different question than the one being
 * asked. It arrives as a whole document and goes into a sandboxed iframe: the
 * page carries the site's palette, and `sandbox` with no allow-scripts means
 * even a preview of an admin's raw-HTML block cannot run anything here.
 */

class WebsitePages extends owl.Component {
    static template = owl.xml`
        <div class="wp-wrap">
            <div class="wp-list">
                <div class="wp-list-head">
                    <span>Pages</span>
                    <button class="wp-mini" t-on-click="load" title="Reload">↻</button>
                </div>
                <t t-if="state.loading and !state.pages.length">
                    <div class="wp-note">Loading…</div>
                </t>
                <t t-foreach="state.pages" t-as="p" t-key="p.id">
                    <div t-attf-class="wp-item{{ state.current and state.current.id === p.id ? ' sel' : '' }}"
                         t-on-click="() => this.select(p.id)">
                        <div class="wp-item-t"><t t-esc="p.title || p.slug"/></div>
                        <div class="wp-item-s">
                            <span class="wp-slug">/<t t-esc="p.slug"/></span>
                            <t t-if="p.is_homepage"><span class="wp-tag">home</span></t>
                            <t t-if="!p.is_published"><span class="wp-tag wp-draft">draft</span></t>
                        </div>
                    </div>
                </t>
                <t t-if="!state.loading and !state.pages.length">
                    <div class="wp-note">No pages yet.</div>
                </t>
            </div>

            <div class="wp-main">
                <t t-if="!state.current">
                    <div class="wp-empty">Select a page to see it.</div>
                </t>
                <t t-else="">
                    <div class="wp-head">
                        <div>
                            <h2 t-esc="state.current.title || state.current.slug"/>
                            <p class="wp-sub">
                                <t t-esc="state.blockCount"/> block<t t-if="state.blockCount !== 1">s</t>
                                · <span class="wp-slug">/site/<t t-esc="state.current.slug"/></span>
                            </p>
                        </div>
                        <div class="wp-actions">
                            <a class="wp-mini" t-attf-href="/site/{{ state.current.slug }}"
                               target="_blank" rel="noopener">Open ↗</a>
                            <button class="wp-mini" t-on-click="refreshPreview">Refresh preview</button>
                            <button class="wp-save" t-on-click="save"
                                    t-att-disabled="state.saving or !state.dirty">
                                <t t-if="state.saving">Saving…</t>
                                <t t-else="">Save</t>
                            </button>
                        </div>
                    </div>

                    <div class="wp-tabs">
                        <button t-attf-class="wp-tab{{ state.tab === 'preview' ? ' active' : '' }}"
                                t-on-click="() => this.setTab('preview')">Preview</button>
                        <button t-attf-class="wp-tab{{ state.tab === 'source' ? ' active' : '' }}"
                                t-on-click="() => this.setTab('source')">Source</button>
                        <span class="wp-sp"></span>
                        <t t-if="state.jsonError">
                            <span class="wp-err" t-esc="state.jsonError"/>
                        </t>
                        <t t-elif="state.dirty">
                            <span class="wp-warn">Unsaved changes</span>
                        </t>
                        <t t-elif="state.message">
                            <span class="wp-ok" t-esc="state.message"/>
                        </t>
                    </div>

                    <div class="wp-pane">
                        <t t-if="state.tab === 'preview'">
                            <t t-if="state.previewError">
                                <div class="wp-note wp-note-err" t-esc="state.previewError"/>
                            </t>
                            <iframe class="wp-frame" title="Page preview" sandbox=""
                                    t-att-srcdoc="state.previewHtml"></iframe>
                        </t>
                        <t t-else="">
                            <textarea class="wp-src" spellcheck="false"
                                      t-att-value="state.source"
                                      t-on-input="onSource"></textarea>
                        </t>
                    </div>
                </t>
            </div>
        </div>
    `;

    state = owl.useState({
        pages: [], current: null,
        source: '', blockCount: 0,
        tab: 'preview', previewHtml: '', previewError: '', jsonError: '',
        loading: false, saving: false, dirty: false, message: '',
    });

    setup() { this.load(); }

    // ---- data ----------------------------------------------------
    async rpc(model, method, args) {
        const res = await fetch('/web/dataset/call_kw', {
            method: 'POST',
            credentials: 'same-origin',
            headers: { 'Content-Type': 'application/json' },
            body: JSON.stringify({ jsonrpc: '2.0', method: 'call',
                params: { model, method, args, kwargs: {} } }),
        });
        const data = await res.json();
        if (data.error) throw new Error(data.error.data?.message || data.error.message || 'Request failed');
        return data.result;
    }

    async load() {
        this.state.loading = true;
        try {
            const rows = await this.rpc('website.page', 'search_read', [
                [], ['id', 'slug', 'title', 'is_published', 'is_homepage'],
            ]);
            this.state.pages = Array.isArray(rows) ? rows : [];
            if (this.state.current) {
                const still = this.state.pages.find(p => p.id === this.state.current.id);
                if (!still) this.state.current = null;
            }
        } catch (e) {
            this.state.previewError = e.message;
        } finally {
            this.state.loading = false;
        }
    }

    async select(id) {
        if (this.state.dirty &&
            !window.confirm('You have unsaved changes. Discard them?')) return;
        const page = this.state.pages.find(p => p.id === id);
        if (!page) return;
        this.state.current = page;
        this.state.dirty = false;
        this.state.message = '';
        this.state.jsonError = '';
        try {
            const rows = await this.rpc('website.page', 'read', [[id], ['blocks_json']]);
            const raw = (rows && rows[0] && rows[0].blocks_json) || '[]';
            let parsed = [];
            try { parsed = JSON.parse(raw); } catch { parsed = []; }
            // Pretty-printed: the stored form is one long line, which is the
            // reason the field was unreadable in the first place.
            this.state.source = JSON.stringify(parsed, null, 2);
            this.state.blockCount = Array.isArray(parsed) ? parsed.length : 0;
        } catch (e) {
            this.state.source = '[]';
            this.state.jsonError = e.message;
        }
        this.refreshPreview();
    }

    // ---- editing -------------------------------------------------
    onSource(ev) {
        this.state.source = ev.target.value;
        this.state.dirty = true;
        this.state.message = '';
        this.state.jsonError = this.parseError(this.state.source);
        if (!this.state.jsonError) {
            const b = JSON.parse(this.state.source);
            this.state.blockCount = Array.isArray(b) ? b.length : 0;
        }
    }

    parseError(text) {
        try {
            const v = JSON.parse(text);
            if (!Array.isArray(v)) return 'The blocks must be a JSON array.';
            return '';
        } catch (e) {
            return e.message;
        }
    }

    setTab(tab) {
        this.state.tab = tab;
        if (tab === 'preview') this.refreshPreview();
    }

    // ---- the preview ---------------------------------------------
    async refreshPreview() {
        if (!this.state.current) return;
        const err = this.parseError(this.state.source);
        if (err) { this.state.previewError = err; return; }
        this.state.previewError = '';
        try {
            const res = await fetch('/site/api/preview', {
                method: 'POST',
                credentials: 'same-origin',
                headers: { 'Content-Type': 'application/json' },
                body: JSON.stringify({
                    blocks: JSON.parse(this.state.source),
                    title: this.state.current.title || this.state.current.slug,
                }),
            });
            const data = await res.json().catch(() => ({}));
            if (!res.ok) throw new Error(data.error || `Preview failed (HTTP ${res.status})`);
            this.state.previewHtml = data.html || '';
        } catch (e) {
            this.state.previewError = e.message;
            this.state.previewHtml = '';
        }
    }

    // ---- saving --------------------------------------------------
    async save() {
        if (!this.state.current) return;
        const err = this.parseError(this.state.source);
        if (err) { this.state.jsonError = err; this.state.tab = 'source'; return; }
        this.state.saving = true;
        this.state.message = '';
        try {
            // Through the website endpoint, not a raw write: it is the thing
            // that checks the group, refuses unknown block types and keeps a
            // revision, and a second way in would bypass all three.
            const res = await fetch(`/site/api/page/${this.state.current.id}/blocks`, {
                method: 'POST',
                credentials: 'same-origin',
                headers: { 'Content-Type': 'application/json' },
                body: JSON.stringify({ blocks: JSON.parse(this.state.source) }),
            });
            const data = await res.json().catch(() => ({}));
            if (!res.ok) throw new Error(data.error || `Save failed (HTTP ${res.status})`);
            this.state.dirty = false;
            this.state.message = 'Saved';
            this.refreshPreview();
        } catch (e) {
            this.state.jsonError = e.message;
        } finally {
            this.state.saving = false;
        }
    }
}
