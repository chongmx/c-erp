/**
 * website-editor.js — in-place block editing (docs/117)
 *
 * Served only to a page rendered for somebody who may edit it: the server
 * emits the <script> tag and window.__WSITE_EDIT only when the session carries
 * the right group. A visitor's HTML does not mention this file.
 *
 * That is NOT the security control — POST /site/api/page/<id>/blocks re-checks
 * the session and the group server-side, because hiding a button has never
 * stopped anybody. This file is the convenience half.
 *
 * THE RULE THAT MATTERS HERE:
 *
 *   Text is read with textContent, NEVER innerHTML.
 *
 * The page is edited in contenteditable elements, so the browser will happily
 * let somebody paste markup into one. Reading textContent means whatever they
 * paste becomes a STRING — the server stores a string, and the renderer
 * escapes it on the way out. The block model from docs/115 survives editing
 * intact: there is still exactly one way markup reaches a page, and it is the
 * admin-only html block.
 */
(function () {
    'use strict';
    var CFG = window.__WSITE_EDIT;
    if (!CFG || !CFG.page_id) return;          // not an editable render

    var editing = false, dirty = false, original = null;

    // ---- styles, injected so the public page carries none of this ----
    var css = document.createElement('style');
    css.textContent =
        '.wse-bar{position:fixed;bottom:0;left:0;right:0;z-index:9999;display:flex;' +
        'gap:10px;align-items:center;padding:10px 16px;background:#16202a;color:#e7eef4;' +
        'font:14px ui-sans-serif,system-ui,sans-serif;box-shadow:0 -2px 14px rgba(0,0,0,.25)}' +
        '.wse-bar button{font:inherit;font-weight:600;padding:7px 14px;border-radius:6px;' +
        'border:1px solid #3a4a58;background:#22303e;color:#e7eef4;cursor:pointer}' +
        '.wse-bar button.pri{background:#0a6f7d;border-color:#0a6f7d}' +
        '.wse-bar button:disabled{opacity:.5;cursor:default}' +
        '.wse-bar .sp{flex:1}.wse-bar .msg{font-size:13px;color:#9fb2c0}' +
        'body.wse-on{padding-bottom:66px}' +
        'body.wse-on [data-wse]{position:relative;outline:1px dashed #9ac7d0;' +
        'outline-offset:6px;border-radius:2px}' +
        'body.wse-on [data-wse]:hover{outline-color:#0a6f7d}' +
        'body.wse-on [contenteditable="true"]:focus{outline:2px solid #0a6f7d;' +
        'outline-offset:4px;background:rgba(10,111,125,.05)}' +
        '.wse-tools{position:absolute;top:-14px;right:-6px;display:none;gap:3px;z-index:20}' +
        'body.wse-on [data-wse]:hover .wse-tools{display:flex}' +
        '.wse-tools button{font:12px ui-sans-serif,system-ui,sans-serif;line-height:1;' +
        'padding:4px 7px;border:1px solid #c9d4dd;background:#fff;color:#16202a;' +
        'border-radius:4px;cursor:pointer}' +
        '.wse-tools button:hover{background:#0a6f7d;color:#fff;border-color:#0a6f7d}' +
        '.wse-add{display:grid;gap:7px}' +
        /* theme controls (docs/121), now a tab in the sidebar (docs/122) */
        '.wse-themes{display:grid;grid-template-columns:1fr 1fr;gap:7px;margin-bottom:15px}' +
        '.wse-theme{display:flex;align-items:center;gap:8px;padding:7px 9px;border-radius:7px;' +
        'border:1px solid #33445a;background:#1c2836;cursor:pointer;font:inherit;' +
        'color:inherit;text-align:left}' +
        '.wse-theme.sel{border-color:#4cc9c0;box-shadow:0 0 0 1px #4cc9c0}' +
        '.wse-sw{width:22px;height:22px;border-radius:5px;flex:none;overflow:hidden;' +
        'border:1px solid rgba(255,255,255,.2);position:relative}' +
        '.wse-sw i{position:absolute;right:0;top:0;bottom:0;width:42%}' +
        '.wse-row{display:flex;align-items:center;justify-content:space-between;' +
        'gap:10px;margin-bottom:11px}' +
        '.wse-row label{font-size:13px;color:#9fb2c0}' +
        '.wse-pane input[type=color]{width:46px;height:28px;padding:0;cursor:pointer;' +
        'border:1px solid #33445a;background:none;border-radius:5px}' +
        '.wse-pane select{font:inherit;background:#1c2836;color:#e7eef4;' +
        'border:1px solid #33445a;border-radius:5px;padding:5px 7px}' +
        /* ---- edit-mode chrome: a top bar and a right sidebar (docs/122) ---- */
        '.wse-top{position:fixed;top:0;left:0;right:0;height:48px;z-index:9998;display:none;' +
        'align-items:center;gap:12px;padding:0 16px;background:#16202a;color:#e7eef4;' +
        'font:14px ui-sans-serif,system-ui,sans-serif;box-shadow:0 2px 12px rgba(0,0,0,.3)}' +
        'body.wse-on .wse-top{display:flex}' +
        /* editing has its own bar; two of them is one too many */
        'body.wse-on .wse-bar{display:none}' +
        '.wse-top .wse-page{color:#9fb2c0;font-size:13px}' +
        '.wse-top .sp{flex:1}' +
        '.wse-top button{font:inherit;font-weight:600;padding:7px 15px;border-radius:6px;' +
        'border:1px solid #3a4a58;background:#22303e;color:#e7eef4;cursor:pointer}' +
        '.wse-top button.pri{background:#0a6f7d;border-color:#0a6f7d}' +
        '.wse-top button:disabled{opacity:.5;cursor:default}' +
        '.wse-side{position:fixed;top:48px;right:0;bottom:0;width:320px;z-index:9997;' +
        'display:none;flex-direction:column;background:#16202a;color:#e7eef4;' +
        'border-left:1px solid #2b3a49;font:14px ui-sans-serif,system-ui,sans-serif}' +
        'body.wse-on .wse-side{display:flex}' +
        'body.wse-on{padding-top:48px;padding-right:320px;padding-bottom:0}' +
        '.wse-tabs{display:flex;flex:none;border-bottom:1px solid #2b3a49}' +
        '.wse-tab{flex:1;font:inherit;font-weight:600;font-size:13px;padding:11px 4px;' +
        'background:none;border:0;border-bottom:2px solid transparent;color:#9fb2c0;cursor:pointer}' +
        '.wse-tab:hover{color:#e7eef4}' +
        '.wse-tab.active{color:#e7eef4;border-bottom-color:#4cc9c0}' +
        '.wse-pane{flex:1;overflow-y:auto;padding:15px}' +
        '.wse-pane[hidden]{display:none}' +
        '.wse-hint{color:#8ea0af;font-size:13px;line-height:1.55;margin:0}' +
        '.wse-grid{display:grid;grid-template-columns:1fr 1fr;gap:7px}' +
        '.wse-grid button{font:inherit;font-size:13px;font-weight:600;padding:9px 6px;' +
        'border:1px solid #33445a;background:#1c2836;color:#e7eef4;border-radius:7px;cursor:pointer}' +
        '.wse-grid button:hover{border-color:#4cc9c0;color:#fff}' +
        '.wse-fld{display:grid;gap:5px;margin-bottom:13px}' +
        '.wse-fld>label{font-size:12px;font-weight:650;color:#9fb2c0}' +
        '.wse-fld input[type=text],.wse-fld textarea,.wse-fld select{font:inherit;font-size:13px;' +
        'width:100%;padding:7px 9px;border:1px solid #33445a;border-radius:6px;' +
        'background:#0f1720;color:#e7eef4}' +
        '.wse-fld textarea{min-height:70px;resize:vertical;line-height:1.5}' +
        '.wse-fld input:focus,.wse-fld textarea:focus,.wse-fld select:focus{outline:none;' +
        'border-color:#4cc9c0;box-shadow:0 0 0 2px rgba(76,201,192,.25)}' +
        '.wse-check{display:flex;align-items:center;gap:8px;font-size:13px;color:#cfdae3}' +
        '.wse-sec{margin:0 0 10px;font-size:11px;letter-spacing:.1em;text-transform:uppercase;' +
        'color:#7f93a3;font-weight:700}' +
        '.wse-item{border:1px solid #2b3a49;border-radius:8px;padding:12px;margin-bottom:11px;' +
        'background:#1a2531}' +
        '.wse-item-top{display:flex;align-items:center;justify-content:space-between;' +
        'margin-bottom:9px}' +
        '.wse-item-top strong{font-size:12px;color:#9fb2c0;letter-spacing:.04em}' +
        '.wse-mini{font:inherit;font-size:12px;padding:3px 8px;border-radius:5px;' +
        'border:1px solid #3a4a58;background:#22303e;color:#e7eef4;cursor:pointer}' +
        '.wse-mini:hover{border-color:#4cc9c0}' +
        '.wse-mini.danger:hover{border-color:#e06c75;color:#ffb3b8}' +
        '.wse-add-item{width:100%;font:inherit;font-size:13px;font-weight:600;padding:8px;' +
        'border:1px dashed #3a4a58;background:none;color:#9fb2c0;border-radius:7px;cursor:pointer}' +
        '.wse-add-item:hover{border-color:#4cc9c0;color:#e7eef4}' +
        /* the selected block, echoed on the page itself */
        'body.wse-on [data-wse].sel{outline:2px solid #4cc9c0;outline-offset:6px}' +
        'body.wse-on [data-wse].sel:hover{outline-color:#4cc9c0}' +
        /* A full-bleed block escapes its wrapper: the hero carries
           `margin:0 -22px` so it runs past the text column, which left it 22px
           wider than the [data-wse] box the outline is drawn on. The outline
           then sat 16px INSIDE the hero on both sides — and since the hero is
           positioned, it painted over those two edges, so the selection showed
           as a pair of horizontal lines with no sides.
           Moving the bleed onto the WRAPPER keeps the rendered position
           identical and makes the box actually wrap its contents. */
        'body.wse-on [data-wse].wse-bleed{margin-left:-22px;margin-right:-22px}' +
        'body.wse-on .wse-bleed>.w-hero{margin-left:0;margin-right:0}' +
        '@media(max-width:640px){body.wse-on [data-wse].wse-bleed{' +
        'margin-left:0;margin-right:0}}';
    document.head.appendChild(css);

    // ---- the bar shown while VIEWING ----
    var bar = document.createElement('div');
    bar.className = 'wse-bar';
    bar.innerHTML =
        '<strong>Website</strong><span class="msg" id="wse-msg">Viewing</span>' +
        '<span class="sp"></span>' +
        '<button id="wse-theme">Theme</button>' +
        '<button id="wse-edit" class="pri">Edit page</button>';
    document.body.appendChild(bar);

    // ---- the bar shown while EDITING (docs/122) ----
    // Save and Discard live here rather than in the bottom bar, so the page is
    // framed by its controls the way an editor is and not by a strip that
    // covers the last block.
    var topbar = document.createElement('div');
    topbar.className = 'wse-top';
    topbar.innerHTML =
        '<strong>Editing</strong>' +
        '<span class="wse-page" id="wse-page"></span>' +
        '<span class="msg" id="wse-msg2"></span>' +
        '<span class="sp"></span>' +
        '<button id="wse-cancel">Discard</button>' +
        '<button id="wse-save" class="pri">Save</button>';
    document.body.appendChild(topbar);

    var $ = function (id) { return document.getElementById(id); };
    // One message, two places to show it — whichever bar is on screen.
    var msg = function (t) {
        $('wse-msg').textContent = t;
        $('wse-msg2').textContent = t;
    };

    // ------------------------------------------------------------------
    // THE THEME PANEL (docs/121)
    //
    // Same standing as the rest of this file: it is a convenience, not a
    // control. POST /site/api/theme re-checks the session's groups and
    // re-validates every colour, so forcing this panel open in a browser that
    // is not allowed to use it gets a 403 and nothing else.
    //
    // Colours coming back from the server are still checked against a hex
    // pattern before being written into a style property — the server
    // validates on the way in, and this validates on the way out, because a
    // value that reaches `style.background` unchecked is a sink either way.
    // ------------------------------------------------------------------
    var HEX = /^#[0-9a-f]{6}$/i;
    var themeState = null;

    // ---- the sidebar (docs/122) ----
    // Three tabs. Blocks adds and reorders; Customize edits the SELECTED
    // block's fields; Theme is the palette. The Customize tab is the one that
    // earns the sidebar: the in-page editor can only ever offer the fields it
    // draws, so a button's link, an image's alt text or a plan's "highlight"
    // flag had no way of being changed at all.
    var side = document.createElement('aside');
    side.className = 'wse-side';
    side.innerHTML =
        '<div class="wse-tabs">' +
          '<button class="wse-tab active" data-tab="blocks" id="wse-tab-blocks">Blocks</button>' +
          '<button class="wse-tab" data-tab="custom" id="wse-tab-custom">Customize</button>' +
          '<button class="wse-tab" data-tab="theme" id="wse-tab-theme">Theme</button>' +
        '</div>' +
        '<div class="wse-pane" id="wse-pane-blocks"></div>' +
        '<div class="wse-pane" id="wse-pane-custom" hidden></div>' +
        '<div class="wse-pane" id="wse-pane-theme" hidden>' +
          '<p class="wse-sec">Preset</p>' +
          '<div class="wse-themes" id="wse-themes"></div>' +
          '<div class="wse-row"><label for="wse-accent">Accent colour</label>' +
          '<input type="color" id="wse-accent"></div>' +
          '<div class="wse-row"><label for="wse-onacc">Text on accent</label>' +
          '<select id="wse-onacc">' +
          '<option value="">Automatic</option>' +
          '<option value="#ffffff">White</option>' +
          '<option value="#111318">Dark</option></select></div>' +
          '<div class="wse-row"><label for="wse-dark">Dark mode</label>' +
          '<select id="wse-dark">' +
          '<option value="auto">Follow visitor</option>' +
          '<option value="off">Always light</option>' +
          '<option value="on">Always dark</option></select></div>' +
          '<button id="wse-theme-apply" class="wse-add-item" ' +
          'style="border-style:solid;border-color:#0a6f7d;background:#0a6f7d;color:#fff">' +
          'Apply theme</button>' +
        '</div>';
    document.body.appendChild(side);

    function tab(name) {
        side.querySelectorAll('.wse-tab').forEach(function (t) {
            t.classList.toggle('active', t.dataset.tab === name);
        });
        ['blocks', 'custom', 'theme'].forEach(function (n) {
            $('wse-pane-' + n).hidden = (n !== name);
        });
        if (name === 'theme' && !themeState) openTheme();
    }
    side.querySelector('.wse-tabs').addEventListener('click', function (ev) {
        var t = ev.target.closest('.wse-tab');
        if (t) tab(t.dataset.tab);
    });

    function drawThemes() {
        var host = $('wse-themes');
        host.textContent = '';
        (themeState.presets || []).forEach(function (p) {
            var b = document.createElement('button');
            b.className = 'wse-theme' + (p.key === themeState.theme ? ' sel' : '');
            b.dataset.theme = p.key;
            var sw = document.createElement('span');
            sw.className = 'wse-sw';
            if (HEX.test(p.bg)) sw.style.background = p.bg;
            var stripe = document.createElement('i');
            if (HEX.test(p.accent)) stripe.style.background = p.accent;
            sw.appendChild(stripe);
            var name = document.createElement('span');
            name.textContent = p.label;          // textContent, never innerHTML
            b.appendChild(sw); b.appendChild(name);
            b.addEventListener('click', function () {
                themeState.theme = p.key;
                if (HEX.test(p.accent)) { themeState.accent = p.accent; $('wse-accent').value = p.accent; }
                drawThemes();
            });
            host.appendChild(b);
        });
    }

    function openTheme() {
        fetch('/site/api/theme', { headers: { 'Accept': 'application/json' } })
            .then(function (r) {
                if (!r.ok) throw new Error(r.status === 403
                    ? 'You do not have permission to change the theme.'
                    : 'Could not load the theme.');
                return r.json();
            })
            .then(function (d) {
                themeState = d;
                drawThemes();
                if (HEX.test(d.accent)) $('wse-accent').value = d.accent;
                $('wse-onacc').value = d.on_accent_override || '';
                $('wse-dark').value = d.dark_mode || 'auto';
            })
            .catch(function (e) { msg(e.message); });
    }

    function applyTheme() {
        // Applying a theme reloads, because the palette is in the page's own
        // <style>. Unsaved block edits would go with it, so ask first.
        if (dirty && !window.confirm(
                'Applying a theme reloads the page and your unsaved edits will be lost. Continue?'))
            return;
        var btn = $('wse-theme-apply');
        btn.disabled = true;
        msg('Applying theme…');
        fetch('/site/api/theme', {
            method: 'POST',
            headers: { 'Content-Type': 'application/json' },
            body: JSON.stringify({
                theme:     themeState.theme,
                accent:    $('wse-accent').value,
                on_accent: $('wse-onacc').value,   // "" = back to computed
                dark_mode: $('wse-dark').value
            })
        }).then(function (r) {
            return r.json().catch(function () { return {}; })
                .then(function (j) { return { ok: r.ok, j: j }; });
        }).then(function (o) {
            btn.disabled = false;
            if (!o.ok) { msg(o.j.error || 'The theme was not saved.'); return; }
            location.reload();                    // the palette is in the page's <style>
        }).catch(function () {
            btn.disabled = false;
            msg('The theme was not saved.');
        });
    }

    // ------------------------------------------------------------------
    // THE BLOCK SCHEMA (docs/122)
    //
    // What each block type is made of, so the Customize tab can be generated
    // rather than hand-written twelve times. This is the answer to the real
    // limitation of in-place editing: you can only click on what is rendered,
    // so a link, an alt text, a coordinate or a boolean had NO editable
    // surface at all. Every field a block owns is listed here.
    //
    // `t` is the control: text | area | select | bool | lines.
    // `lines` is a textarea that round-trips to an array of strings.
    // ------------------------------------------------------------------
    var SCHEMA = {
        hero: { label: 'Hero', fields: [
            { f: 'eyebrow',     l: 'Eyebrow',            t: 'text' },
            { f: 'headline',    l: 'Headline',           t: 'text' },
            { f: 'subheadline', l: 'Sub-headline',       t: 'area' },
            { f: 'cta_text',    l: 'Button label',       t: 'text' },
            { f: 'cta_href',    l: 'Button link',        t: 'text' },
            { f: 'alt_text',    l: 'Second button',      t: 'text' },
            { f: 'alt_href',    l: 'Second button link', t: 'text' } ] },
        heading: { label: 'Heading', fields: [
            { f: 'level', l: 'Level', t: 'select',
              o: [['1', 'Page title (H1)'], ['2', 'Section (H2)'], ['3', 'Sub-section (H3)']] },
            { f: 'text',  l: 'Text',  t: 'text' } ] },
        text:    { label: 'Text',    fields: [ { f: 'text', l: 'Body', t: 'area' } ] },
        image:   { label: 'Image',   fields: [
            { f: 'src',     l: 'Image',     t: 'image' },
            { f: 'alt',     l: 'Alt text (what a screen reader says)', t: 'text' },
            { f: 'caption', l: 'Caption',   t: 'text' } ] },
        button:  { label: 'Button',  fields: [
            { f: 'text', l: 'Label',    t: 'text' },
            { f: 'href', l: 'Links to', t: 'text' } ] },
        divider: { label: 'Divider', fields: [] },
        map:     { label: 'Map',     fields: [
            { f: 'query', l: 'Place name', t: 'text' },
            { f: 'lat',   l: 'Latitude',   t: 'text' },
            { f: 'lon',   l: 'Longitude',  t: 'text' } ],
            hint: 'Without a latitude and longitude the block renders as a link rather than an embedded map.' },
        form:    { label: 'Form',    fields: [ { f: 'slug', l: 'Form', t: 'text' } ] },
        html:    { label: 'Raw HTML', admin: true, fields: [ { f: 'html', l: 'HTML', t: 'area' } ],
                   hint: 'Administrators only. The server refuses this block from anyone else.' },
        columns: { label: 'Columns', item: 'Column', fields: [], itemFields: [
            { f: 'title', l: 'Title', t: 'text' },
            { f: 'text',  l: 'Text',  t: 'area' } ] },
        steps:   { label: 'Steps',   item: 'Step',   fields: [], itemFields: [
            { f: 'title', l: 'Title', t: 'text' },
            { f: 'text',  l: 'Text',  t: 'area' } ] },
        faq:     { label: 'FAQ',     item: 'Question', fields: [], itemFields: [
            { f: 'q', l: 'Question', t: 'text' },
            { f: 'a', l: 'Answer',   t: 'area' } ] },
        references: { label: 'References', item: 'Reference', fields: [], itemFields: [
            { f: 'name', l: 'Name',     t: 'text' },
            { f: 'note', l: 'Note',     t: 'text' },
            { f: 'logo', l: 'Logo URL', t: 'text' } ] },
        video:   { label: 'Video',   fields: [
            { f: 'src',     l: 'Video',   t: 'image' },
            { f: 'poster',  l: 'Poster image (uploaded video only)', t: 'image' },
            { f: 'caption', l: 'Caption', t: 'text' } ],
            hint: 'Paste a YouTube or Vimeo link, or upload an MP4 / WebM (up to 24 MB). '
                + 'Longer clips belong on a provider — the embed costs you nothing to host.' },
        quote:   { label: 'Quote',   fields: [
            { f: 'text',   l: 'Quote',  t: 'area' },
            { f: 'author', l: 'Who said it', t: 'text' },
            { f: 'role',   l: 'Their role or company', t: 'text' } ] },
        cta:     { label: 'Call to action', fields: [
            { f: 'headline', l: 'Headline',     t: 'text' },
            { f: 'text',     l: 'Supporting line', t: 'text' },
            { f: 'cta_text', l: 'Button label', t: 'text' },
            { f: 'cta_href', l: 'Button link',  t: 'text' } ] },
        spacer:  { label: 'Spacer',  fields: [
            { f: 'size', l: 'Height', t: 'select',
              o: [['small', 'Small'], ['medium', 'Medium'], ['large', 'Large']] } ] },
        gallery: { label: 'Gallery', item: 'Picture', fields: [], itemFields: [
            { f: 'src',     l: 'Image',   t: 'image' },
            { f: 'alt',     l: 'Alt text', t: 'text' },
            { f: 'caption', l: 'Caption', t: 'text' } ] },
        stats:   { label: 'Numbers', item: 'Number', fields: [], itemFields: [
            { f: 'value', l: 'Value', t: 'text' },
            { f: 'label', l: 'Label', t: 'text' } ] },
        table:   { label: 'Table', item: 'Row',
            fields: [ { f: 'header', l: 'First row is a header', t: 'bool' } ],
            itemFields: [ { f: 'cells', l: 'Cells, one per line', t: 'lines' } ],
            hint: 'Each row is one cell per line. Keep the number of lines the same '
                + 'in every row and the columns line up.' },
        pricing: { label: 'Pricing', item: 'Plan', fields: [], itemFields: [
            { f: 'name',     l: 'Name',            t: 'text' },
            { f: 'size',     l: 'Size / subtitle', t: 'text' },
            { f: 'price',    l: 'Price',           t: 'text' },
            { f: 'period',   l: 'Period',          t: 'text' },
            { f: 'featured', l: 'Highlight this plan', t: 'bool' },
            { f: 'badge',    l: 'Badge text',      t: 'text' },
            { f: 'features', l: 'Features, one per line', t: 'lines' },
            { f: 'cta_text', l: 'Button label',    t: 'text' },
            { f: 'cta_href', l: 'Button link',     t: 'text' } ] }
    };

    // ---- the block palette ----
    var PALETTE = [
        ['hero', 'Hero'], ['heading', 'Heading'], ['text', 'Text'],
        ['image', 'Image'], ['gallery', 'Gallery'], ['video', 'Video'],
        ['pricing', 'Units'], ['stats', 'Numbers'], ['quote', 'Quote'],
        ['steps', 'Steps'], ['faq', 'FAQ'], ['table', 'Table'],
        ['cta', 'Call to action'], ['columns', 'Columns'], ['button', 'Button'],
        ['references', 'References'], ['map', 'Map'],
        ['divider', 'Divider'], ['spacer', 'Spacer']
    ];
    // The palette lives in the sidebar's Blocks pane. It keeps the .wse-add
    // class and its data-add buttons: those are the contract the tests drive.
    var adder = document.createElement('div');
    adder.className = 'wse-add wse-grid';
    adder.innerHTML = PALETTE.map(function (p) {
        return '<button data-add="' + p[0] + '">' + p[1] + '</button>';
    }).join('');

    var main = document.querySelector('main');
    if (!main) return;

    // ------------------------------------------------------------------
    // The model. The editor edits DATA and re-renders, rather than editing
    // the rendered HTML and trying to read it back — the same reason the
    // server renders blocks in the first place.
    // ------------------------------------------------------------------
    var blocks = [];

    function esc(s) {
        return String(s == null ? '' : s)
            .replace(/&/g, '&amp;').replace(/</g, '&lt;').replace(/>/g, '&gt;')
            .replace(/"/g, '&quot;').replace(/'/g, '&#39;');
    }

    // Read the CURRENT page back into blocks by asking the server for them —
    // parsing the rendered HTML would be guessing.
    function load() {
        return fetch('/site/api/page/' + CFG.page_id + '/blocks', {
            headers: { 'Accept': 'application/json' }
        }).then(function (r) { return r.ok ? r.json() : { blocks: [] }; })
          .then(function (d) { blocks = Array.isArray(d.blocks) ? d.blocks : []; })
          .catch(function () { blocks = []; });
    }

    function blank(type) {
        switch (type) {
            case 'heading':    return { type: 'heading', level: '2', text: 'New heading' };
            case 'text':       return { type: 'text', text: 'Write something here.' };
            case 'image':      return { type: 'image', src: '', alt: '' };
            case 'button':     return { type: 'button', text: 'Learn more', href: '/site' };
            case 'divider':    return { type: 'divider' };
            case 'columns':    return { type: 'columns', items: [
                                   { title: 'First', text: 'Describe it.' },
                                   { title: 'Second', text: 'Describe it.' }] };
            case 'references': return { type: 'references', items: [{ name: 'A customer', note: '' }] };
            case 'map':        return { type: 'map', query: 'Kuala Lumpur' };
            case 'hero':       return { type: 'hero', eyebrow: '', headline: 'A headline',
                                        subheadline: 'A sentence that explains it.',
                                        cta_text: 'Get started', cta_href: '/site/contact' };
            case 'pricing':    return { type: 'pricing', items: [
                                   { name: 'Unit type', size: '', price: '', period: '/month',
                                     features: ['A feature'], cta_text: 'Enquire',
                                     cta_href: '/site/contact' }] };
            case 'steps':      return { type: 'steps', items: [
                                   { title: 'First step', text: 'What happens.' },
                                   { title: 'Second step', text: 'What happens next.' }] };
            case 'faq':        return { type: 'faq', items: [
                                   { q: 'A question?', a: 'The answer.' }] };
            case 'video':      return { type: 'video', src: '', caption: '', poster: '' };
            case 'gallery':    return { type: 'gallery', items: [
                                   { src: '', alt: '', caption: '' }] };
            case 'quote':      return { type: 'quote', text: 'Something a customer said.',
                                        author: '', role: '' };
            case 'stats':      return { type: 'stats', items: [
                                   { value: '250', label: 'Units on site' },
                                   { value: '7 days', label: 'Access every week' }] };
            case 'cta':        return { type: 'cta', headline: 'Ready when you are',
                                        text: 'A sentence that gets them to act.',
                                        cta_text: 'Get in touch', cta_href: '/site/contact' };
            case 'table':      return { type: 'table', header: true, items: [
                                   { cells: ['', '', ''] },
                                   { cells: ['', '', ''] }] };
            case 'spacer':     return { type: 'spacer', size: 'medium' };
            default:           return null;
        }
    }

    // Render the editable view. Every editable region is tagged with the block
    // index and the field it maps to, so reading back is a lookup rather than
    // an interpretation of the markup.
    function draw() {
        var out = '';
        blocks.forEach(function (b, i) {
            var tools =
                '<span class="wse-tools">' +
                '<button data-up="' + i + '" title="Move up">&#8593;</button>' +
                '<button data-down="' + i + '" title="Move down">&#8595;</button>' +
                '<button data-del="' + i + '" title="Delete">&times;</button></span>';
            var body = '';
            switch (b.type) {
                case 'heading':
                    // The level is interpolated as a TAG NAME, so it is the one
                    // value here that escaping cannot make safe — it has to be
                    // constrained instead. The server clamps it on render too,
                    // so a stored oddity never reaches the public page; this
                    // stops it reaching the editor's own DOM either.
                    var lvl = /^[123]$/.test(String(b.level)) ? String(b.level) : '2';
                    body = '<h' + lvl + ' class="w-h" contenteditable="true" ' +
                           'data-i="' + i + '" data-f="text">' + esc(b.text) + '</h' + lvl + '>';
                    break;
                case 'text':
                    body = '<p class="w-p" contenteditable="true" data-i="' + i +
                           '" data-f="text" style="white-space:pre-wrap">' + esc(b.text) + '</p>';
                    break;
                case 'divider':
                    body = '<hr class="w-hr"/>';
                    break;
                case 'button':
                    body = '<p class="w-btn-wrap"><span class="w-btn" contenteditable="true" ' +
                           'data-i="' + i + '" data-f="text">' + esc(b.text) + '</span>' +
                           ' <small contenteditable="true" data-i="' + i + '" data-f="href">' +
                           esc(b.href) + '</small></p>';
                    break;
                case 'image':
                    body = '<figure class="w-fig">' +
                           (b.src ? '<img src="' + esc(b.src) + '" alt="' + esc(b.alt) + '"/>' : '') +
                           '<figcaption>URL: <span contenteditable="true" data-i="' + i +
                           '" data-f="src">' + esc(b.src) + '</span></figcaption></figure>';
                    break;
                case 'map':
                    body = '<p class="w-p">Map of <span contenteditable="true" data-i="' + i +
                           '" data-f="query">' + esc(b.query) + '</span></p>';
                    break;
                case 'columns':
                    body = '<div class="w-cols">' + (b.items || []).map(function (c, k) {
                        return '<div class="w-col">' +
                          '<h3 class="w-col-h" contenteditable="true" data-i="' + i +
                          '" data-k="' + k + '" data-f="title">' + esc(c.title) + '</h3>' +
                          '<p class="w-p" contenteditable="true" data-i="' + i +
                          '" data-k="' + k + '" data-f="text">' + esc(c.text) + '</p></div>';
                    }).join('') + '</div>';
                    break;
                case 'hero':
                    body = '<section class="w-hero">' +
                      '<p class="w-hero-eyebrow" contenteditable="true" data-i="' + i +
                      '" data-f="eyebrow">' + esc(b.eyebrow) + '</p>' +
                      '<h1 class="w-hero-h" contenteditable="true" data-i="' + i +
                      '" data-f="headline">' + esc(b.headline) + '</h1>' +
                      '<p class="w-hero-sub" contenteditable="true" data-i="' + i +
                      '" data-f="subheadline">' + esc(b.subheadline) + '</p>' +
                      '<p class="w-hero-cta"><span class="w-btn" contenteditable="true" data-i="' + i +
                      '" data-f="cta_text">' + esc(b.cta_text) + '</span></p></section>';
                    break;
                case 'pricing':
                    body = '<div class="w-plans">' + (b.items || []).map(function (c, k) {
                        return '<div class="w-plan">' +
                          '<h3 class="w-plan-name" contenteditable="true" data-i="' + i +
                          '" data-k="' + k + '" data-f="name">' + esc(c.name) + '</h3>' +
                          '<p class="w-plan-size" contenteditable="true" data-i="' + i +
                          '" data-k="' + k + '" data-f="size">' + esc(c.size) + '</p>' +
                          '<p class="w-plan-price" contenteditable="true" data-i="' + i +
                          '" data-k="' + k + '" data-f="price">' + esc(c.price) + '</p>' +
                          '<ul class="w-plan-feats">' + (c.features || []).map(function (f) {
                              return '<li>' + esc(f) + '</li>'; }).join('') + '</ul>' +
                          '<p class="w-plan-cta"><span class="w-btn" contenteditable="true" data-i="' + i +
                          '" data-k="' + k + '" data-f="cta_text">' + esc(c.cta_text) + '</span></p>' +
                          '</div>';
                    }).join('') + '</div>';
                    break;
                case 'steps':
                    body = '<ol class="w-steps">' + (b.items || []).map(function (c, k) {
                        return '<li class="w-step">' +
                          '<h3 class="w-step-h" contenteditable="true" data-i="' + i +
                          '" data-k="' + k + '" data-f="title">' + esc(c.title) + '</h3>' +
                          '<p class="w-p" contenteditable="true" data-i="' + i +
                          '" data-k="' + k + '" data-f="text">' + esc(c.text) + '</p></li>';
                    }).join('') + '</ol>';
                    break;
                case 'faq':
                    body = '<div class="w-faq">' + (b.items || []).map(function (c, k) {
                        return '<div class="w-faq-i" style="padding:12px 0">' +
                          '<div style="font-weight:620" contenteditable="true" data-i="' + i +
                          '" data-k="' + k + '" data-f="q">' + esc(c.q) + '</div>' +
                          '<p class="w-p" contenteditable="true" data-i="' + i +
                          '" data-k="' + k + '" data-f="a">' + esc(c.a) + '</p></div>';
                    }).join('') + '</div>';
                    break;
                case 'references':
                    body = '<ul class="w-refs">' + (b.items || []).map(function (c, k) {
                        return '<li class="w-ref">' +
                          '<span class="w-ref-name" contenteditable="true" data-i="' + i +
                          '" data-k="' + k + '" data-f="name">' + esc(c.name) + '</span>' +
                          '<span class="w-ref-note" contenteditable="true" data-i="' + i +
                          '" data-k="' + k + '" data-f="note">' + esc(c.note) + '</span></li>';
                    }).join('') + '</ul>';
                    break;
                case 'quote':
                    body = '<blockquote class="w-quote"><p contenteditable="true" data-i="' + i +
                      '" data-f="text">' + esc(b.text) + '</p><footer>' +
                      '<span class="w-quote-who" contenteditable="true" data-i="' + i +
                      '" data-f="author">' + esc(b.author) + '</span>' +
                      '<span class="w-quote-role" contenteditable="true" data-i="' + i +
                      '" data-f="role">' + esc(b.role) + '</span></footer></blockquote>';
                    break;
                case 'cta':
                    body = '<aside class="w-cta"><div class="w-cta-t">' +
                      '<p class="w-cta-h" contenteditable="true" data-i="' + i +
                      '" data-f="headline">' + esc(b.headline) + '</p>' +
                      '<p class="w-cta-s" contenteditable="true" data-i="' + i +
                      '" data-f="text">' + esc(b.text) + '</p></div>' +
                      '<span class="w-btn" contenteditable="true" data-i="' + i +
                      '" data-f="cta_text">' + esc(b.cta_text) + '</span></aside>';
                    break;
                case 'stats':
                    body = '<dl class="w-stats">' + (b.items || []).map(function (c, k) {
                        return '<div class="w-stat">' +
                          '<dt class="w-stat-v" contenteditable="true" data-i="' + i +
                          '" data-k="' + k + '" data-f="value">' + esc(c.value) + '</dt>' +
                          '<dd class="w-stat-l" contenteditable="true" data-i="' + i +
                          '" data-k="' + k + '" data-f="label">' + esc(c.label) + '</dd></div>';
                    }).join('') + '</dl>';
                    break;
                case 'gallery':
                    body = '<div class="w-gal">' + (b.items || []).map(function (c) {
                        return '<figure class="w-gal-i">' +
                          (c.src ? '<img src="' + esc(c.src) + '" alt="' + esc(c.alt) + '"/>'
                                 : '<div style="height:170px;border:1px dashed #9ac7d0;' +
                                   'border-radius:10px;display:flex;align-items:center;' +
                                   'justify-content:center;font-size:13px;color:#5d6f7e">' +
                                   'Pick an image in the sidebar</div>') +
                          '</figure>';
                    }).join('') + '</div>';
                    break;
                case 'video':
                    // Rendered as a placeholder rather than a live embed: an
                    // iframe inside the editable page swallows clicks, so the
                    // block underneath could not be selected.
                    body = '<div class="w-video"><div class="w-video-frame" ' +
                      'style="display:flex;align-items:center;justify-content:center;' +
                      'padding-top:0;height:200px;font-size:13px;color:#5d6f7e">' +
                      (b.src ? esc(b.src) : 'Add a video link or upload one in the sidebar') +
                      '</div></div>';
                    break;
                case 'table':
                    body = '<div class="w-table-wrap"><table class="w-table"><tbody>' +
                      (b.items || []).map(function (r, k) {
                        return '<tr>' + (r.cells || []).map(function (c, ci) {
                            return '<td contenteditable="true" data-i="' + i + '" data-k="' + k +
                                   '" data-f="cell' + ci + '">' + esc(c) + '</td>';
                        }).join('') + '</tr>';
                      }).join('') + '</tbody></table></div>';
                    break;
                case 'spacer':
                    body = '<div class="w-sp-m" style="border:1px dashed #9ac7d0;' +
                           'border-radius:6px;display:flex;align-items:center;' +
                           'justify-content:center;font-size:12px;color:#5d6f7e">' +
                           'Spacer (' + esc(b.size || 'medium') + ')</div>';
                    break;
                default:
                    // A block this editor does not know (an html block, say) is
                    // shown as read-only and passed through untouched on save.
                    body = '<div class="w-raw"><em>(' + esc(b.type) +
                           ' block — edit it in Settings)</em></div>';
            }
            // Blocks whose rendered box is wider than the text column need the
            // bleed on the wrapper, or the selection outline cuts across them.
            var bleed = (b.type === 'hero') ? ' class="wse-bleed"' : '';
            out += '<div data-wse="' + i + '"' + bleed + '>' + tools + body + '</div>';
        });
        main.innerHTML = out;
        // The palette now lives in the sidebar, so the page is only the page.
        if (selected >= blocks.length) selected = blocks.length - 1;
        markSelection();
        drawOutline();
    }

    // ------------------------------------------------------------------
    // SELECTION + THE CUSTOMIZE PANE (docs/122)
    // ------------------------------------------------------------------
    var selected = -1;

    function markSelection() {
        main.querySelectorAll('[data-wse]').forEach(function (n) {
            n.classList.toggle('sel', parseInt(n.dataset.wse, 10) === selected);
        });
    }

    function select(i, scroll) {
        selected = i;
        markSelection();
        drawCustom();
        if (scroll) {
            var n = main.querySelector('[data-wse="' + i + '"]');
            if (n) n.scrollIntoView({ block: 'center', behavior: 'smooth' });
        }
    }

    // Build one labelled control. Values go in as `.value`, never as markup,
    // and come back out as strings — the same discipline as the harvest.
    function control(spec, value, onChange) {
        var wrap = document.createElement('div');
        wrap.className = 'wse-fld';

        if (spec.t === 'bool') {
            var line = document.createElement('label');
            line.className = 'wse-check';
            var cb = document.createElement('input');
            cb.type = 'checkbox';
            cb.checked = value === true || value === 'true';
            cb.addEventListener('change', function () { onChange(cb.checked); });
            var cap = document.createElement('span');
            cap.textContent = spec.l;
            line.appendChild(cb); line.appendChild(cap);
            wrap.appendChild(line);
            return wrap;
        }

        var lab = document.createElement('label');
        lab.textContent = spec.l;
        wrap.appendChild(lab);

        // The media control (docs/124): a preview, an upload, and the library.
        // Before this an image block could only take a URL typed by hand, so
        // every picture on the site had to be hosted somewhere else.
        if (spec.t === 'image') {
            var preview = document.createElement('img');
            preview.style.cssText = 'max-width:100%;border-radius:6px;display:none;' +
                                    'margin-bottom:8px;background:#0f1720';
            var show = function (url) {
                if (url) { preview.src = url; preview.style.display = 'block'; }
                else preview.style.display = 'none';
            };
            show(value);
            wrap.appendChild(preview);

            var url = document.createElement('input');
            url.type = 'text';
            url.value = value == null ? '' : String(value);
            url.placeholder = 'https://… or upload below';
            url.addEventListener('input', function () { onChange(url.value); show(url.value); });
            wrap.appendChild(url);

            var row = document.createElement('div');
            row.style.cssText = 'display:flex;gap:6px;margin-top:7px';
            var file = document.createElement('input');
            file.type = 'file';
            file.accept = 'image/png,image/jpeg,image/gif,image/webp';
            file.style.display = 'none';
            var pick = document.createElement('button');
            pick.className = 'wse-mini';
            pick.textContent = 'Upload…';
            pick.addEventListener('click', function () { file.click(); });
            var browse = document.createElement('button');
            browse.className = 'wse-mini';
            browse.textContent = 'Library';
            row.appendChild(pick); row.appendChild(browse); row.appendChild(file);
            wrap.appendChild(row);

            var gallery = document.createElement('div');
            gallery.style.cssText = 'display:none;grid-template-columns:1fr 1fr 1fr;' +
                                    'gap:6px;margin-top:9px';
            wrap.appendChild(gallery);

            var choose = function (u) { url.value = u; onChange(u); show(u); draw(); };

            file.addEventListener('change', function () {
                var f = file.files && file.files[0];
                if (!f) return;
                pick.disabled = true; pick.textContent = 'Uploading…';
                fetch('/site/api/media?name=' + encodeURIComponent(f.name),
                      { method: 'POST', body: f })
                    .then(function (r) {
                        return r.json().catch(function () { return {}; })
                                .then(function (j) { return { ok: r.ok, j: j }; });
                    })
                    .then(function (o) {
                        pick.disabled = false; pick.textContent = 'Upload…';
                        file.value = '';
                        // The server refuses SVG and anything that is not
                        // really an image; say so rather than failing quietly.
                        if (!o.ok) { msg(o.j.error || 'That image was not accepted.'); return; }
                        choose(o.j.url);
                        msg('Image uploaded');
                    })
                    .catch(function () {
                        pick.disabled = false; pick.textContent = 'Upload…';
                        msg('The upload did not reach the server.');
                    });
            });

            browse.addEventListener('click', function () {
                if (gallery.style.display === 'grid') { gallery.style.display = 'none'; return; }
                fetch('/site/api/media', { headers: { 'Accept': 'application/json' } })
                    .then(function (r) { return r.ok ? r.json() : { images: [] }; })
                    .then(function (d) {
                        gallery.textContent = '';
                        (d.images || []).forEach(function (im) {
                            var t = document.createElement('img');
                            t.src = im.url;
                            t.alt = im.name;          // author text, set as a property
                            t.title = im.name;
                            t.style.cssText = 'width:100%;height:58px;object-fit:cover;' +
                                              'border-radius:5px;cursor:pointer;' +
                                              'border:1px solid #33445a';
                            t.addEventListener('click', function () {
                                choose(im.url);
                                gallery.style.display = 'none';
                            });
                            gallery.appendChild(t);
                        });
                        if (!(d.images || []).length) {
                            var none = document.createElement('p');
                            none.className = 'wse-hint';
                            none.style.gridColumn = '1/-1';
                            none.textContent = 'Nothing uploaded yet.';
                            gallery.appendChild(none);
                        }
                        gallery.style.display = 'grid';
                    })
                    .catch(function () { msg('Could not load the image library.'); });
            });
            return wrap;
        }

        var input;
        if (spec.t === 'select') {
            input = document.createElement('select');
            (spec.o || []).forEach(function (o) {
                var opt = document.createElement('option');
                opt.value = o[0];
                opt.textContent = o[1];
                input.appendChild(opt);
            });
            input.value = value == null ? '' : String(value);
            input.addEventListener('change', function () { onChange(input.value); });
        } else if (spec.t === 'area' || spec.t === 'lines') {
            input = document.createElement('textarea');
            input.value = spec.t === 'lines'
                ? (Array.isArray(value) ? value.join('\n') : (value == null ? '' : String(value)))
                : (value == null ? '' : String(value));
            input.addEventListener('input', function () {
                onChange(spec.t === 'lines'
                    ? input.value.split('\n').map(function (s) { return s.trim(); })
                          .filter(function (s) { return s.length; })
                    : input.value);
            });
        } else {
            input = document.createElement('input');
            input.type = 'text';
            input.value = value == null ? '' : String(value);
            input.addEventListener('input', function () { onChange(input.value); });
        }
        lab.htmlFor = input.id = 'wse-f-' + Math.random().toString(36).slice(2, 9);
        wrap.appendChild(input);
        return wrap;
    }

    function miniButton(text, cls, fn) {
        var b = document.createElement('button');
        b.className = 'wse-mini' + (cls ? ' ' + cls : '');
        b.textContent = text;
        b.addEventListener('click', fn);
        return b;
    }

    function touched() { dirty = true; msg('Unsaved changes'); }

    function drawCustom() {
        var pane = $('wse-pane-custom');
        pane.textContent = '';

        var b = blocks[selected];
        if (!b) {
            var hint = document.createElement('p');
            hint.className = 'wse-hint';
            hint.textContent = 'Click any block on the page to change its settings — '
                             + 'including the things you cannot click on, like a button’s '
                             + 'link or an image’s alt text.';
            pane.appendChild(hint);
            return;
        }

        var spec = SCHEMA[b.type] || { label: b.type, fields: [] };
        var head = document.createElement('p');
        head.className = 'wse-sec';
        head.textContent = spec.label + ' — block ' + (selected + 1) + ' of ' + blocks.length;
        pane.appendChild(head);

        if (spec.hint) {
            var h = document.createElement('p');
            h.className = 'wse-hint';
            h.style.marginBottom = '13px';
            h.textContent = spec.hint;
            pane.appendChild(h);
        }

        // Block-level fields.
        (spec.fields || []).forEach(function (f) {
            pane.appendChild(control(f, b[f.f], function (v) {
                b[f.f] = v; touched(); draw();
            }));
        });

        // Repeating items, each with its own reorder and delete.
        if (spec.itemFields) {
            if (!Array.isArray(b.items)) b.items = [];
            var sec = document.createElement('p');
            sec.className = 'wse-sec';
            sec.textContent = spec.item + 's';
            pane.appendChild(sec);

            b.items.forEach(function (it, k) {
                var box = document.createElement('div');
                box.className = 'wse-item';
                var top = document.createElement('div');
                top.className = 'wse-item-top';
                var name = document.createElement('strong');
                name.textContent = spec.item + ' ' + (k + 1);
                var tools = document.createElement('span');
                tools.appendChild(miniButton('↑', '', function () {
                    if (k === 0) return;
                    b.items.splice(k - 1, 0, b.items.splice(k, 1)[0]);
                    touched(); draw(); drawCustom();
                }));
                tools.appendChild(miniButton('↓', '', function () {
                    if (k >= b.items.length - 1) return;
                    b.items.splice(k + 1, 0, b.items.splice(k, 1)[0]);
                    touched(); draw(); drawCustom();
                }));
                tools.appendChild(miniButton('✕', 'danger', function () {
                    b.items.splice(k, 1);
                    touched(); draw(); drawCustom();
                }));
                top.appendChild(name); top.appendChild(tools);
                box.appendChild(top);
                spec.itemFields.forEach(function (f) {
                    box.appendChild(control(f, it[f.f], function (v) {
                        it[f.f] = v; touched(); draw();
                    }));
                });
                pane.appendChild(box);
            });

            var add = document.createElement('button');
            add.className = 'wse-add-item';
            add.textContent = '+ Add ' + spec.item.toLowerCase();
            add.addEventListener('click', function () {
                var blankItem = {};
                spec.itemFields.forEach(function (f) {
                    blankItem[f.f] = f.t === 'bool' ? false : (f.t === 'lines' ? [] : '');
                });
                b.items.push(blankItem);
                touched(); draw(); drawCustom();
            });
            pane.appendChild(add);
        }
    }

    // The page outline. A long page is hard to navigate by scrolling, and
    // nothing else here tells you what the page is MADE of at a glance.
    function drawOutline() {
        var host = $('wse-outline');
        if (!host) return;
        host.textContent = '';
        blocks.forEach(function (b, i) {
            var row = document.createElement('button');
            row.className = 'wse-add-item';
            row.style.textAlign = 'left';
            row.style.marginBottom = '5px';
            if (i === selected) { row.style.borderColor = '#4cc9c0'; row.style.color = '#e7eef4'; }
            var label = (SCHEMA[b.type] && SCHEMA[b.type].label) || b.type;
            var first = b.headline || b.text || b.name || b.query ||
                        (Array.isArray(b.items) && b.items[0] &&
                         (b.items[0].title || b.items[0].name || b.items[0].q)) || '';
            row.textContent = (i + 1) + '. ' + label + (first ? ' — ' + String(first).slice(0, 26) : '');
            row.addEventListener('click', function () { select(i, true); tab('custom'); });
            host.appendChild(row);
        });
    }

    // THE READ-BACK. textContent, never innerHTML.
    function harvest() {
        main.querySelectorAll('[contenteditable="true"]').forEach(function (el) {
            var i = parseInt(el.dataset.i, 10);
            var f = el.dataset.f;
            if (isNaN(i) || !f || !blocks[i]) return;
            var v = el.textContent;          // <- the whole security argument
            if (el.dataset.k !== undefined) {
                var k = parseInt(el.dataset.k, 10);
                var item = blocks[i].items && blocks[i].items[k];
                if (!item) return;
                // A table row's cells are an ARRAY, so "cell3" addresses an
                // index rather than naming a field. Without this the read-back
                // would quietly write items[k].cell3 and lose the edit.
                var m = /^cell(\d+)$/.exec(f);
                if (m) {
                    if (!Array.isArray(item.cells)) item.cells = [];
                    item.cells[parseInt(m[1], 10)] = v;
                } else {
                    item[f] = v;
                }
            } else {
                blocks[i][f] = v;
            }
        });
    }

    // Fill the Blocks pane once: the palette, then the outline.
    (function buildBlocksPane() {
        var pane = $('wse-pane-blocks');
        var h1 = document.createElement('p');
        h1.className = 'wse-sec';
        h1.textContent = 'Add a block';
        pane.appendChild(h1);
        pane.appendChild(adder);
        var h2 = document.createElement('p');
        h2.className = 'wse-sec';
        h2.style.marginTop = '18px';
        h2.textContent = 'This page';
        pane.appendChild(h2);
        var outline = document.createElement('div');
        outline.id = 'wse-outline';
        pane.appendChild(outline);
    })();

    function enter(openTab) {
        editing = true; dirty = false;
        selected = -1;
        original = JSON.stringify(blocks);
        document.body.classList.add('wse-on');
        $('wse-edit').style.display = 'none';
        $('wse-page').textContent = document.title.split(' · ')[0] || '';
        msg('Click any text to edit it, or any block to change its settings');
        draw();
        drawCustom();
        tab(openTab || 'blocks');
    }

    function leave(reload) {
        editing = false;
        selected = -1;
        document.body.classList.remove('wse-on');
        if (reload) { window.location.reload(); return; }
        $('wse-edit').style.display = '';
    }

    $('wse-edit').addEventListener('click', function () {
        load().then(function () { enter('blocks'); });
    });

    // Theme is an edit, so it opens the editor on its own tab rather than
    // floating over a page that is not in edit mode.
    $('wse-theme').addEventListener('click', function () {
        load().then(function () { enter('theme'); });
    });
    $('wse-theme-apply').addEventListener('click', applyTheme);

    // Adding a block: the palette is in the sidebar now, so its clicks no
    // longer pass through main's handler.
    adder.addEventListener('click', function (ev) {
        var b = ev.target.closest('button[data-add]');
        if (!b || !editing) return;
        harvest();
        var nb = blank(b.dataset.add);
        if (!nb) return;
        // Insert AFTER the selected block rather than always at the end —
        // appending and then clicking "up" eleven times is not editing.
        var at = (selected >= 0 && selected < blocks.length) ? selected + 1 : blocks.length;
        blocks.splice(at, 0, nb);
        dirty = true;
        draw();
        select(at, true);
        tab('custom');
    });

    $('wse-cancel').addEventListener('click', function () {
        if (dirty && !window.confirm('Discard your changes?')) return;
        leave(true);
    });

    $('wse-save').addEventListener('click', function () {
        harvest();
        $('wse-save').disabled = true;
        msg('Saving…');
        fetch('/site/api/page/' + CFG.page_id + '/blocks', {
            method: 'POST',
            headers: { 'Content-Type': 'application/json' },
            body: JSON.stringify({ blocks: blocks })
        }).then(function (r) {
            return r.json().catch(function () { return {}; }).then(function (d) {
                return { ok: r.ok, d: d };
            });
        }).then(function (o) {
            $('wse-save').disabled = false;
            if (!o.ok || o.d.error) { msg(o.d.error || 'Could not save.'); return; }
            msg('Saved');
            dirty = false;
            setTimeout(function () { leave(true); }, 400);
        }).catch(function () {
            $('wse-save').disabled = false;
            msg('Could not reach the server.');
        });
    });

    main.addEventListener('input', function () { if (editing) { dirty = true; msg('Unsaved changes'); } });

    main.addEventListener('click', function (ev) {
        if (!editing) return;
        var b = ev.target.closest('button');

        // Not a tool button — treat it as selecting the block that was clicked,
        // so the Customize tab always describes what the user is looking at.
        if (!b) {
            var host = ev.target.closest('[data-wse]');
            if (!host) return;
            var idx = parseInt(host.dataset.wse, 10);
            if (!isNaN(idx) && idx !== selected) { select(idx, false); tab('custom'); }
            return;
        }

        var i = parseInt(b.dataset.up || b.dataset.down || b.dataset.del, 10);
        if (isNaN(i)) return;
        harvest();
        if (b.dataset.del !== undefined) {
            if (!window.confirm('Delete this block?')) return;
            blocks.splice(i, 1);
            if (selected === i) selected = -1;
            else if (selected > i) selected--;
        } else if (b.dataset.up !== undefined && i > 0) {
            blocks.splice(i - 1, 0, blocks.splice(i, 1)[0]);
            if (selected === i) selected = i - 1;
        } else if (b.dataset.down !== undefined && i < blocks.length - 1) {
            blocks.splice(i + 1, 0, blocks.splice(i, 1)[0]);
            if (selected === i) selected = i + 1;
        }
        dirty = true;
        draw();
        drawCustom();
    });

    // Keyboard. Ctrl/Cmd+S saves, Escape deselects — an editor that needs the
    // mouse for everything is slower than the form it replaced.
    document.addEventListener('keydown', function (ev) {
        if (!editing) return;
        if ((ev.ctrlKey || ev.metaKey) && (ev.key === 's' || ev.key === 'S')) {
            ev.preventDefault();
            $('wse-save').click();
        } else if (ev.key === 'Escape' && selected >= 0) {
            select(-1, false);
        }
    });

    window.addEventListener('beforeunload', function (e) {
        if (editing && dirty) { e.preventDefault(); e.returnValue = ''; }
    });
})();
