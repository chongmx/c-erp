/**
 * HelpCenter.js — the in-app Help Centre (docs/101).
 *
 * Three panes and a tab bar:
 *
 *   [ tabs — one per ERP module, scrolls sideways as modules are added ]
 *   ┌──────────┬────────────────────────────┬──────────────┐
 *   │ contents │ the article                │ assistant    │
 *   │ (tree)   │                            │ (right rail) │
 *   └──────────┴────────────────────────────┴──────────────┘
 *
 * Both side rails are resizable by dragging their inner edge and collapsible
 * from their header; the widths persist per browser. The centre column is the
 * only thing that flexes, so neither rail can be squeezed out of existence by
 * a narrow window.
 *
 * The tab bar is a horizontal scroll container for the same reason the parts
 * catalogue's facet strip is: there will eventually be more modules than fit
 * across a screen, and wrapping them onto a second row would push the content
 * down every time the system grows.
 *
 * Markdown is rendered here rather than stored as HTML. The bodies have to stay
 * plain text so an assistant can retrieve and quote them; HTML in the database
 * would also mean trusting stored markup, which this renderer avoids by
 * escaping everything before it applies any formatting.
 */
class HelpCenter extends owl.Component {
    static template = owl.xml`
        <div class="hc-shell">

            <div class="hc-tabsrow">
                <button class="hc-scroll" t-att-disabled="!state.canScrollL"
                        t-on-click="() => this.scrollTabs(-1)" title="Scroll tabs left">‹</button>
                <div class="hc-tabs" t-ref="tabs" t-on-scroll="onTabScroll">
                    <t t-foreach="state.books" t-as="b" t-key="b.slug">
                        <button class="hc-tab"
                                t-att-class="{active: b.slug === state.book, empty: !b.count}"
                                t-on-click="() => this.openBook(b.slug)">
                            <span t-esc="b.label"/>
                            <span class="hc-tab-n" t-if="b.count" t-esc="b.count"/>
                            <span class="hc-tab-soon" t-if="!b.count">—</span>
                        </button>
                    </t>
                </div>
                <button class="hc-scroll" t-att-disabled="!state.canScrollR"
                        t-on-click="() => this.scrollTabs(1)" title="Scroll tabs right">›</button>
            </div>

            <div class="hc-body">

                <!-- left rail: contents tree.
                     NB: no runs of hyphens in these comments. OWL parses the
                     template as XML, where "- -" inside a comment is illegal
                     and kills the whole compile. -->

                <div class="hc-rail left" t-att-class="{collapsed: state.leftCollapsed}"
                     t-att-style="state.leftCollapsed ? '' : ('flex-basis:' + state.leftW + 'px')">
                    <div class="hc-rail-h">
                        <span class="hc-rail-t" t-if="!state.leftCollapsed">Contents</span>
                        <button class="hc-icon" t-att-title="state.leftCollapsed ? 'Show contents' : 'Hide contents'"
                                t-on-click="() => this.toggleRail('left')"
                                t-esc="state.leftCollapsed ? '»' : '«'"/>
                    </div>
                    <div class="hc-rail-b" t-if="!state.leftCollapsed">
                        <input class="hc-search" placeholder="Search this book…"
                               t-att-value="state.q" t-on-input="onSearch"/>

                        <t t-if="state.q.length > 1">
                            <div class="hc-hits">
                                <div class="hc-hits-n"><t t-esc="state.hits.length"/> result(s)</div>
                                <t t-foreach="state.hits" t-as="h" t-key="h.slug">
                                    <button class="hc-hit" t-att-class="{active: h.slug === state.slug}"
                                            t-on-click="() => this.open(h.slug)">
                                        <span class="hc-hit-t" t-esc="h.title"/>
                                        <span class="hc-hit-x" t-if="h.excerpt" t-esc="h.excerpt"/>
                                    </button>
                                </t>
                                <div class="hc-none" t-if="!state.hits.length">Nothing matched.</div>
                            </div>
                        </t>

                        <t t-else="">
                            <t t-foreach="state.tree" t-as="s" t-key="s.slug">
                                <div class="hc-sec">
                                    <button class="hc-sec-h" t-on-click="() => this.toggleSection(s.slug)">
                                        <span class="hc-caret" t-esc="isFolded(s.slug) ? '▸' : '▾'"/>
                                        <span t-esc="s.title"/>
                                        <span class="hc-sec-n" t-esc="s.articles.length"/>
                                    </button>
                                    <t t-if="!isFolded(s.slug)">
                                        <t t-foreach="s.articles" t-as="a" t-key="a.slug">
                                            <button class="hc-art" t-att-class="{active: a.slug === state.slug}"
                                                    t-on-click="() => this.open(a.slug)" t-esc="a.title"/>
                                        </t>
                                    </t>
                                </div>
                            </t>
                            <div class="hc-none" t-if="!state.tree.length and !state.loading">
                                No help written for this module yet.
                            </div>
                        </t>
                    </div>
                    <div class="hc-grip" t-if="!state.leftCollapsed"
                         t-on-mousedown="(ev) => this.startDrag(ev, 'left')"/>
                </div>

                <!-- centre: the article -->
                <div class="hc-main">
                    <t t-if="state.error"><div class="hc-error" t-esc="state.error"/></t>
                    <t t-if="state.article">
                        <div class="hc-crumb">
                            <span t-esc="bookLabel"/>
                            <t t-if="state.article.section_title">
                                <span class="hc-sep">›</span><span t-esc="state.article.section_title"/>
                            </t>
                        </div>
                        <h1 class="hc-h1" t-esc="state.article.title"/>
                        <div class="hc-md" t-out="rendered"/>
                    </t>
                    <t t-elif="!state.loading">
                        <div class="hc-welcome">
                            <h1 class="hc-h1">Help</h1>
                            <p t-if="state.tree.length">Choose a topic on the left to start reading.</p>
                            <p t-else="">
                                There is no help written for this module yet. Pick another tab above —
                                tabs with a dash have no articles.
                            </p>
                        </div>
                    </t>
                </div>

                <!-- right rail: assistant -->
                <div class="hc-rail right" t-att-class="{collapsed: state.rightCollapsed}"
                     t-att-style="state.rightCollapsed ? '' : ('flex-basis:' + state.rightW + 'px')">
                    <div class="hc-grip right" t-if="!state.rightCollapsed"
                         t-on-mousedown="(ev) => this.startDrag(ev, 'right')"/>
                    <div class="hc-rail-h">
                        <button class="hc-icon" t-att-title="state.rightCollapsed ? 'Show assistant' : 'Hide assistant'"
                                t-on-click="() => this.toggleRail('right')"
                                t-esc="state.rightCollapsed ? '«' : '»'"/>
                        <span class="hc-rail-t" t-if="!state.rightCollapsed">Assistant</span>
                    </div>
                    <div class="hc-rail-b" t-if="!state.rightCollapsed">
                        <div class="hc-ai-note">
                            Not connected yet. When it is, answers will be drawn from these
                            articles and cite the ones they came from.
                        </div>
                        <div class="hc-ai-box">
                            <textarea class="hc-ai-in" rows="3" disabled="disabled"
                                      placeholder="Ask a question about this screen…"/>
                            <button class="hc-btn" disabled="disabled">Ask</button>
                        </div>
                        <div class="hc-ai-rel" t-if="state.related.length">
                            <div class="hc-ai-h">Related articles</div>
                            <t t-foreach="state.related" t-as="r" t-key="r.slug">
                                <button class="hc-art" t-on-click="() => this.open(r.slug)" t-esc="r.title"/>
                            </t>
                            <div class="hc-ai-foot">
                                This is the same retrieval step the assistant will run before answering.
                            </div>
                        </div>
                    </div>
                </div>
            </div>
        </div>`;

    setup() {
        this.tabsRef = owl.useRef('tabs');
        this.state = owl.useState({
            books: [], book: '', tree: [], folded: {},
            slug: '', article: null, related: [], hits: [], q: '',
            leftW: this.readW('hcLeftW', 260), rightW: this.readW('hcRightW', 300),
            leftCollapsed: localStorage.getItem('hcLeftC') === '1',
            rightCollapsed: localStorage.getItem('hcRightC') === '1',
            canScrollL: false, canScrollR: false,
            loading: false, error: '',
        });
        // Bound once so the same reference can be removed on drag end.
        this._onMove = (ev) => this.onDrag(ev);
        this._onUp = () => this.endDrag();

        owl.onWillStart(async () => {
            try {
                this.state.books = await RpcService.call('help.article', 'books', [{}], {}) || [];
            } catch (e) {
                this.state.error = (e && e.message) || 'Could not load the help index.';
            }
            const deep = this.readDeepLink();
            const first = (this.state.books.find(b => b.count) || this.state.books[0] || {}).slug || '';
            await this.openBook(deep.book || first, deep.slug);
        });
        owl.onMounted(() => this.measureTabs());
        owl.onWillUnmount(() => {
            window.removeEventListener('mousemove', this._onMove);
            window.removeEventListener('mouseup', this._onUp);
        });
    }

    readW(key, dflt) {
        const v = parseInt(localStorage.getItem(key) || '', 10);
        return (v && v >= 160 && v <= 640) ? v : dflt;
    }

    // ---- deep links --------------------------------------------------------
    // The shell owns location.hash, so the article is appended as an extra
    // parameter and written back with replaceState — that updates the address
    // bar without firing hashchange and sending the router somewhere else.
    readDeepLink() {
        const h = window.location.hash || '';
        const m = h.match(/[#&]help=([a-z0-9-]+)/i);
        const b = h.match(/[#&]helpbook=([a-z0-9-]+)/i);
        return { slug: m ? m[1] : '', book: b ? b[1] : '' };
    }
    writeDeepLink() {
        let h = (window.location.hash || '#action=help')
            .replace(/([#&])help=[a-z0-9-]*/i, '$1')
            .replace(/([#&])helpbook=[a-z0-9-]*/i, '$1')
            .replace(/&&+/g, '&').replace(/&$/, '');
        if (this.state.book) h += (h.includes('&') || h.length > 1 ? '&' : '') + 'helpbook=' + this.state.book;
        if (this.state.slug) h += '&help=' + this.state.slug;
        try { window.history.replaceState(null, '', h); } catch (e) { /* not fatal */ }
    }

    get bookLabel() {
        const b = this.state.books.find(x => x.slug === this.state.book);
        return b ? b.label : this.state.book;
    }

    // ---- navigation --------------------------------------------------------
    async openBook(slug, wantSlug) {
        if (!slug) return;
        this.state.book = slug;
        this.state.q = '';
        this.state.hits = [];
        this.state.loading = true;
        this.state.error = '';
        try {
            this.state.tree = await RpcService.call('help.article', 'tree', [{ book: slug }], {}) || [];
        } catch (e) {
            this.state.tree = [];
            this.state.error = (e && e.message) || 'Could not load this book.';
        } finally {
            this.state.loading = false;
        }
        // Land on something readable rather than an empty pane.
        const firstArticle = (this.state.tree.find(s => s.articles.length) || { articles: [] }).articles[0];
        const target = wantSlug || (firstArticle && firstArticle.slug);
        if (target) await this.open(target);
        else { this.state.article = null; this.state.slug = ''; this.state.related = []; this.writeDeepLink(); }
        this.measureTabs();
    }

    async open(slug) {
        this.state.loading = true;
        this.state.error = '';
        try {
            const a = await RpcService.call('help.article', 'article', [{ slug }], {});
            this.state.article = a;
            this.state.slug = slug;
            if (a && a.book && a.book !== this.state.book) {
                // A search hit or related link can point into another book.
                this.state.book = a.book;
                this.state.tree = await RpcService.call('help.article', 'tree', [{ book: a.book }], {}) || [];
            }
            this.writeDeepLink();
            try {
                this.state.related = await RpcService.call('help.article', 'related', [{ slug }], {}) || [];
            } catch (e) { this.state.related = []; }
            const main = document.querySelector('.hc-main');
            if (main) main.scrollTop = 0;
        } catch (e) {
            this.state.error = (e && e.message) || 'That article could not be opened.';
        } finally {
            this.state.loading = false;
        }
    }

    toggleSection(slug) { this.state.folded[slug] = !this.state.folded[slug]; }
    isFolded(slug) { return !!this.state.folded[slug]; }

    async onSearch(ev) {
        this.state.q = ev.target.value;
        if (this.state.q.trim().length < 2) { this.state.hits = []; return; }
        try {
            this.state.hits = await RpcService.call('help.article', 'search',
                [{ q: this.state.q.trim(), book: this.state.book }], {}) || [];
        } catch (e) { this.state.hits = []; }
    }

    // ---- tab bar scrolling -------------------------------------------------
    measureTabs() {
        const el = this.tabsRef.el;
        if (!el) return;
        this.state.canScrollL = el.scrollLeft > 2;
        this.state.canScrollR = el.scrollLeft + el.clientWidth < el.scrollWidth - 2;
    }
    onTabScroll() { this.measureTabs(); }
    scrollTabs(dir) {
        const el = this.tabsRef.el;
        if (!el) return;
        el.scrollLeft += dir * Math.max(160, el.clientWidth * 0.7);
        this.measureTabs();
    }

    // ---- rail sizing -------------------------------------------------------
    toggleRail(side) {
        if (side === 'left') {
            this.state.leftCollapsed = !this.state.leftCollapsed;
            localStorage.setItem('hcLeftC', this.state.leftCollapsed ? '1' : '0');
        } else {
            this.state.rightCollapsed = !this.state.rightCollapsed;
            localStorage.setItem('hcRightC', this.state.rightCollapsed ? '1' : '0');
        }
        this.measureTabs();
    }

    startDrag(ev, side) {
        ev.preventDefault();
        this._drag = { side, x: ev.clientX, w: side === 'left' ? this.state.leftW : this.state.rightW };
        window.addEventListener('mousemove', this._onMove);
        window.addEventListener('mouseup', this._onUp);
        document.body.classList.add('hc-resizing');
    }
    onDrag(ev) {
        if (!this._drag) return;
        const d = ev.clientX - this._drag.x;
        // The right rail grows as the pointer moves LEFT, so its delta inverts.
        const raw = this._drag.side === 'left' ? this._drag.w + d : this._drag.w - d;
        const w = Math.max(180, Math.min(560, raw));
        if (this._drag.side === 'left') this.state.leftW = w; else this.state.rightW = w;
    }
    endDrag() {
        if (!this._drag) return;
        localStorage.setItem(this._drag.side === 'left' ? 'hcLeftW' : 'hcRightW',
                             String(this._drag.side === 'left' ? this.state.leftW : this.state.rightW));
        this._drag = null;
        window.removeEventListener('mousemove', this._onMove);
        window.removeEventListener('mouseup', this._onUp);
        document.body.classList.remove('hc-resizing');
    }

    // ---- markdown ----------------------------------------------------------
    get rendered() {
        return HelpCenter.markdown((this.state.article && this.state.article.body) || '');
    }

    /**
     * A small, deliberately limited markdown renderer.
     *
     * Everything is HTML-escaped FIRST and formatting applied to the escaped
     * text afterwards, so no stored content can inject markup — the bodies are
     * data, and data never becomes tags.
     */
    static markdown(src) {
        const esc = (s) => String(s)
            .replace(/&/g, '&amp;').replace(/</g, '&lt;').replace(/>/g, '&gt;')
            .replace(/"/g, '&quot;');

        // Inline: code first, so ** inside `code` is not treated as bold.
        const inline = (s) => {
            const codes = [];
            let t = esc(s).replace(/`([^`]+)`/g, (m, c) => {
                codes.push(c);
                return ' CODE' + (codes.length - 1) + ' ';
            });
            t = t.replace(/\*\*([^*]+)\*\*/g, '<strong>$1</strong>')
                 .replace(/(^|[^*])\*([^*\n]+)\*/g, '$1<em>$2</em>')
                 // Only http(s) links are linkified — javascript: URLs must never
                 // survive into an href.
                 .replace(/\[([^\]]+)\]\((https?:[^)\s]+)\)/g,
                          '<a href="$2" target="_blank" rel="noopener noreferrer">$1</a>');
            return t.replace(/ CODE(\d+) /g, (m, i) => '<code>' + codes[+i] + '</code>');
        };

        const lines = String(src).replace(/\r\n/g, '\n').split('\n');
        const out = [];
        let i = 0;

        const closeList = (stack) => { while (stack.length) out.push('</' + stack.pop() + '>'); };
        const listStack = [];

        while (i < lines.length) {
            const line = lines[i];

            // fenced code
            if (/^```/.test(line)) {
                closeList(listStack);
                const buf = [];
                i++;
                while (i < lines.length && !/^```/.test(lines[i])) buf.push(lines[i++]);
                i++;
                out.push('<pre><code>' + esc(buf.join('\n')) + '</code></pre>');
                continue;
            }
            // indented code (4 spaces), used for diagrams and literal examples
            if (/^ {4}\S/.test(line)) {
                closeList(listStack);
                const buf = [];
                while (i < lines.length && (/^ {4}/.test(lines[i]) || lines[i].trim() === '')) {
                    if (lines[i].trim() === '' &&
                        !(i + 1 < lines.length && /^ {4}\S/.test(lines[i + 1]))) break;
                    buf.push(lines[i].replace(/^ {4}/, ''));
                    i++;
                }
                out.push('<pre><code>' + esc(buf.join('\n')) + '</code></pre>');
                continue;
            }
            // table
            if (/^\s*\|/.test(line) && i + 1 < lines.length && /^\s*\|[\s:|-]+\|?\s*$/.test(lines[i + 1])) {
                closeList(listStack);
                const cells = (r) => r.trim().replace(/^\||\|$/g, '').split('|').map(c => c.trim());
                const head = cells(line);
                i += 2;
                const rows = [];
                while (i < lines.length && /^\s*\|/.test(lines[i])) rows.push(cells(lines[i++]));
                out.push('<div class="hc-tablewrap"><table><thead><tr>' +
                    head.map(h => '<th>' + inline(h) + '</th>').join('') +
                    '</tr></thead><tbody>' +
                    rows.map(r => '<tr>' + r.map(c => '<td>' + inline(c) + '</td>').join('') + '</tr>').join('') +
                    '</tbody></table></div>');
                continue;
            }
            // heading
            const h = line.match(/^(#{1,4})\s+(.*)$/);
            if (h) {
                closeList(listStack);
                const n = h[1].length + 1;   // # is the article title, so # -> h2
                out.push('<h' + n + '>' + inline(h[2]) + '</h' + n + '>');
                i++; continue;
            }
            // blockquote
            if (/^>\s?/.test(line)) {
                closeList(listStack);
                const buf = [];
                while (i < lines.length && /^>\s?/.test(lines[i])) buf.push(lines[i++].replace(/^>\s?/, ''));
                out.push('<blockquote>' + inline(buf.join(' ')) + '</blockquote>');
                continue;
            }
            // horizontal rule
            if (/^\s*---+\s*$/.test(line)) { closeList(listStack); out.push('<hr/>'); i++; continue; }
            // lists
            const ul = line.match(/^\s*[-*]\s+(.*)$/);
            const ol = line.match(/^\s*\d+\.\s+(.*)$/);
            if (ul || ol) {
                const want = ul ? 'ul' : 'ol';
                if (listStack[listStack.length - 1] !== want) { closeList(listStack); out.push('<' + want + '>'); listStack.push(want); }
                let text = (ul || ol)[1];
                i++;
                // Lazy continuation: a wrapped bullet is indented by a couple of
                // spaces and belongs to the item above it. Without this the
                // continuation closes the list and becomes a stray paragraph —
                // which is exactly what it looked like on screen. Kept to 1-3
                // spaces so a 4-space indent is still a code block.
                while (i < lines.length && lines[i].trim() &&
                       /^ {1,3}\S/.test(lines[i]) &&
                       !/^\s*(?:[-*]\s|\d+\.\s)/.test(lines[i])) {
                    text += ' ' + lines[i].trim();
                    i++;
                }
                out.push('<li>' + inline(text) + '</li>');
                continue;
            }
            // blank
            if (!line.trim()) { closeList(listStack); i++; continue; }
            // paragraph — join following non-blank, non-special lines
            const buf = [line];
            i++;
            while (i < lines.length && lines[i].trim() &&
                   !/^(#{1,4}\s|```|>\s?|\s*[-*]\s|\s*\d+\.\s|\s*\|)/.test(lines[i]) &&
                   !/^ {4}\S/.test(lines[i]) && !/^\s*---+\s*$/.test(lines[i])) {
                buf.push(lines[i++]);
            }
            closeList(listStack);
            out.push('<p>' + inline(buf.join(' ')) + '</p>');
        }
        closeList(listStack);
        return owl.markup(out.join('\n'));
    }
}
