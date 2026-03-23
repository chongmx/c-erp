'use strict';

// ================================================================
// Portal frontend — vanilla JS, no external dependencies
// All API calls go to /portal/api/* endpoints.
// ================================================================

const Portal = (() => {

    let _user = null; // { partner_id, name, email }

    // ── API helper ────────────────────────────────────────────────
    async function api(method, path, body) {
        const opts = {
            method,
            credentials: 'include',
            headers: { 'Content-Type': 'application/json' },
        };
        if (body !== undefined) opts.body = JSON.stringify(body);
        const res = await fetch('/portal/api' + path, opts);
        const data = await res.json();
        if (!res.ok) throw new Error(data.error || 'Request failed');
        return data;
    }

    async function uploadFile(path, formData) {
        const res = await fetch('/portal/api' + path, {
            method: 'POST',
            credentials: 'include',
            body: formData, // no Content-Type header — browser sets multipart boundary
        });
        const data = await res.json();
        if (!res.ok) throw new Error(data.error || 'Upload failed');
        return data;
    }

    // ── Toast ─────────────────────────────────────────────────────
    function toast(msg) {
        const el = document.getElementById('toast');
        el.textContent = msg;
        el.classList.add('show');
        setTimeout(() => el.classList.remove('show'), 3000);
    }

    // ── Formatters ────────────────────────────────────────────────
    function fmtAmount(v) {
        const n = parseFloat(v) || 0;
        return n.toLocaleString('en-US', { minimumFractionDigits: 2, maximumFractionDigits: 2 });
    }
    function fmtDate(v) {
        if (!v || v === false) return '—';
        return String(v).substring(0, 10);
    }
    function paymentBadge(ps) {
        const map = {
            'paid':         ['paid',    'Paid'],
            'in_payment':   ['paid',    'In Payment'],
            'partial':      ['partial', 'Partial'],
            'not_paid':     ['notpaid', 'Not Paid'],
            'reversed':     ['draft',   'Reversed'],
        };
        const [cls, lbl] = map[ps] || ['draft', ps || '—'];
        return `<span class="badge badge-${cls}">${lbl}</span>`;
    }
    function stateBadge(s) {
        const map = { posted: 'posted', draft: 'draft', cancel: 'draft' };
        const lbl = { posted: 'Confirmed', draft: 'Draft', cancel: 'Cancelled' };
        return `<span class="badge badge-${map[s]||'draft'}">${lbl[s]||s}</span>`;
    }

    // ── Login ──────────────────────────────────────────────────────
    async function login() {
        const email    = document.getElementById('login-email').value.trim();
        const password = document.getElementById('login-pass').value;
        const errEl    = document.getElementById('login-error');
        const btn      = document.getElementById('login-btn');

        errEl.style.display = 'none';
        btn.disabled = true;
        btn.textContent = 'Signing in…';

        try {
            const data = await api('POST', '/login', { email, password });
            _user = { name: data.name, email: data.email };
            showApp();
        } catch (e) {
            errEl.textContent = e.message || 'Invalid email or password.';
            errEl.style.display = 'block';
        } finally {
            btn.disabled = false;
            btn.textContent = 'Sign In';
        }
    }

    // Allow Enter key on login form
    document.addEventListener('DOMContentLoaded', () => {
        ['login-email', 'login-pass'].forEach(id => {
            document.getElementById(id).addEventListener('keydown', e => {
                if (e.key === 'Enter') login();
            });
        });
        // Check if already logged in
        api('GET', '/me').then(data => {
            _user = data;
            showApp();
        }).catch(() => {
            document.getElementById('login-screen').style.display = 'flex';
        });
    });

    async function logout() {
        await api('POST', '/logout').catch(() => {});
        _user = null;
        document.getElementById('portal-app').style.display = 'none';
        document.getElementById('login-screen').style.display = 'flex';
        document.getElementById('login-email').value = '';
        document.getElementById('login-pass').value = '';
    }

    // ── App shell ─────────────────────────────────────────────────
    function showApp() {
        document.getElementById('login-screen').style.display = 'none';
        document.getElementById('portal-app').style.display = 'block';
        document.getElementById('user-name').textContent = _user?.name || '';
        showSection('invoices');
    }

    function showSection(name) {
        ['invoices', 'orders', 'deliveries', 'products'].forEach(s => {
            document.getElementById('section-' + s).style.display = s === name ? '' : 'none';
            const nav = document.querySelector(`.nav-item[data-section="${s}"]`);
            if (nav) nav.classList.toggle('active', s === name);
        });
        if (name === 'invoices')   loadInvoices();
        if (name === 'orders')     loadOrders();
        if (name === 'deliveries') loadDeliveries();
        if (name === 'products')   loadProducts();
    }

    // ── Invoices ──────────────────────────────────────────────────
    let _invoices = [];

    async function loadInvoices() {
        const tbody = document.getElementById('invoice-tbody');
        tbody.innerHTML = '<tr><td colspan="6" class="empty-state">Loading…</td></tr>';
        try {
            _invoices = await api('GET', '/invoices');
            renderInvoices();
        } catch (e) {
            tbody.innerHTML = `<tr><td colspan="6" class="empty-state">${e.message}</td></tr>`;
        }
    }

    function renderInvoices() {
        const tbody = document.getElementById('invoice-tbody');
        if (!_invoices.length) {
            tbody.innerHTML = '<tr><td colspan="6" class="empty-state"><div class="icon">📄</div>No invoices yet.</td></tr>';
            return;
        }
        tbody.innerHTML = _invoices.map(inv => {
            const proofHtml = (inv.proofs || []).map(p =>
                `<div class="proof-item">📎 ${escHtml(p.filename)} <span style="color:#94a3b8">${fmtDate(p.upload_date)}</span></div>`
            ).join('');

            return `<tr>
              <td><a href="#" style="color:#2563eb;font-weight:600" onclick="Portal.openDetail(${inv.id});return false">${escHtml(inv.name||'/')}</a></td>
              <td>${fmtDate(inv.invoice_date)}</td>
              <td style="font-weight:600">${fmtAmount(inv.amount_total)}</td>
              <td>${stateBadge(inv.state)}</td>
              <td>${paymentBadge(inv.payment_state)}</td>
              <td>
                <div style="display:flex;gap:6px;flex-wrap:wrap">
                  <button class="btn-sm primary" onclick="Portal.openDetail(${inv.id})">View</button>
                  <label class="btn-sm success" style="cursor:pointer">
                    Upload Proof
                    <input type="file" accept="image/*,.pdf" style="display:none"
                           onchange="Portal.uploadProof(${inv.id}, this)">
                  </label>
                </div>
                ${proofHtml}
              </td>
            </tr>`;
        }).join('');
    }

    // ── Invoice Detail ────────────────────────────────────────────
    async function openDetail(invoiceId) {
        try {
            const inv = await api('GET', `/invoice/${invoiceId}/detail`);
            const detail = document.getElementById('invoice-detail');
            const body   = document.getElementById('detail-body');
            document.getElementById('detail-title').textContent = inv.name || 'Invoice';

            const linesHtml = inv.lines.length
                ? `<table style="margin-top:16px">
                    <thead><tr><th>Description</th><th style="text-align:right">Qty</th><th style="text-align:right">Unit Price</th><th style="text-align:right">Subtotal</th></tr></thead>
                    <tbody>${inv.lines.map(l => `
                      <tr>
                        <td>${escHtml(l.name||'')}</td>
                        <td style="text-align:right">${parseFloat(l.quantity)||0}</td>
                        <td style="text-align:right">${fmtAmount(l.price_unit)}</td>
                        <td style="text-align:right">${fmtAmount(l.subtotal)}</td>
                      </tr>`).join('')}
                    </tbody></table>`
                : '<p class="text-muted" style="margin-top:16px">No line items.</p>';

            body.innerHTML = `
              <div class="info-row"><span class="info-label">Reference</span><span class="info-value">${escHtml(inv.name||'/')}</span></div>
              <div class="info-row"><span class="info-label">Date</span><span class="info-value">${fmtDate(inv.invoice_date)}</span></div>
              <div class="info-row"><span class="info-label">Status</span><span class="info-value">${stateBadge(inv.state)}</span></div>
              <div class="info-row"><span class="info-label">Payment</span><span class="info-value">${paymentBadge(inv.payment_state)}</span></div>
              <div class="info-row"><span class="info-label">Total</span><span class="info-value" style="font-size:18px;font-weight:700">${fmtAmount(inv.amount_total)}</span></div>
              ${linesHtml}
              <div style="margin-top:24px;display:flex;gap:10px">
                <button class="btn-sm primary" onclick="Portal.printInvoice(${invoiceId})">🖨 Print / Save PDF</button>
                <label class="btn-sm success" style="cursor:pointer">
                  Upload Proof
                  <input type="file" accept="image/*,.pdf" style="display:none"
                         onchange="Portal.uploadProof(${invoiceId}, this)">
                </label>
              </div>`;

            document.getElementById('overlay').style.display = 'block';
            detail.style.display = 'block';
        } catch (e) {
            toast('Could not load invoice: ' + e.message);
        }
    }

    function closeDetail() {
        document.getElementById('invoice-detail').style.display = 'none';
        document.getElementById('overlay').style.display = 'none';
    }

    function printInvoice(invoiceId) {
        window.open(`/portal/api/invoice/${invoiceId}/print`, '_blank');
    }

    // ── Upload proof ──────────────────────────────────────────────
    async function uploadProof(invoiceId, input) {
        const file = input.files[0];
        if (!file) return;
        const fd = new FormData();
        fd.append('file', file);
        try {
            await uploadFile(`/invoice/${invoiceId}/proof`, fd);
            toast('Payment proof uploaded successfully.');
            await loadInvoices();
        } catch (e) {
            toast('Upload failed: ' + e.message);
        }
        input.value = '';
    }

    // ── Orders ────────────────────────────────────────────────────
    let _orders = [];

    async function loadOrders() {
        const tbody = document.getElementById('order-tbody');
        tbody.innerHTML = '<tr><td colspan="5" class="empty-state">Loading…</td></tr>';
        try {
            _orders = await api('GET', '/orders');
            renderOrders();
        } catch (e) {
            tbody.innerHTML = `<tr><td colspan="5" class="empty-state">${e.message}</td></tr>`;
        }
    }

    function orderStateBadge(s) {
        const map = { sale: 'posted', done: 'posted', draft: 'draft', cancel: 'draft', sent: 'partial' };
        const lbl = { sale: 'Confirmed', done: 'Done', draft: 'Draft', cancel: 'Cancelled', sent: 'Sent' };
        return `<span class="badge badge-${map[s]||'draft'}">${lbl[s]||s}</span>`;
    }

    function renderOrders() {
        const tbody = document.getElementById('order-tbody');
        if (!_orders.length) {
            tbody.innerHTML = '<tr><td colspan="5" class="empty-state"><div class="icon">📋</div>No orders yet.</td></tr>';
            return;
        }
        tbody.innerHTML = _orders.map(o => `<tr>
          <td><a href="#" style="color:#2563eb;font-weight:600" onclick="Portal.openOrderDetail(${o.id});return false">${escHtml(o.name||'/')}</a></td>
          <td>${fmtDate(o.date_order)}</td>
          <td style="font-weight:600">${fmtAmount(o.amount_total)}</td>
          <td>${orderStateBadge(o.state)}</td>
          <td><button class="btn-sm primary" onclick="Portal.openOrderDetail(${o.id})">View</button>
              <button class="btn-sm secondary" onclick="Portal.printOrder(${o.id})">Print</button></td>
        </tr>`).join('');
    }

    async function openOrderDetail(orderId) {
        try {
            const o = await api('GET', `/order/${orderId}/detail`);
            const detail = document.getElementById('invoice-detail');
            const body   = document.getElementById('detail-body');
            document.getElementById('detail-title').textContent = o.name || 'Order';
            const linesHtml = o.lines.length
                ? `<table style="margin-top:16px">
                    <thead><tr><th>Description</th><th>UOM</th><th style="text-align:right">Qty</th><th style="text-align:right">Unit Price</th><th style="text-align:right">Subtotal</th></tr></thead>
                    <tbody>${o.lines.map(l => `<tr>
                      <td>${escHtml(l.name||'')}</td>
                      <td>${escHtml(l.uom||'')}</td>
                      <td style="text-align:right">${parseFloat(l.quantity)||0}</td>
                      <td style="text-align:right">${fmtAmount(l.price_unit)}</td>
                      <td style="text-align:right">${fmtAmount(l.subtotal)}</td>
                    </tr>`).join('')}</tbody></table>`
                : '<p class="text-muted" style="margin-top:16px">No line items.</p>';
            body.innerHTML = `
              <div class="info-row"><span class="info-label">Reference</span><span class="info-value">${escHtml(o.name||'/')}</span></div>
              <div class="info-row"><span class="info-label">Date</span><span class="info-value">${fmtDate(o.date_order)}</span></div>
              <div class="info-row"><span class="info-label">Status</span><span class="info-value">${orderStateBadge(o.state)}</span></div>
              <div class="info-row"><span class="info-label">Total</span><span class="info-value" style="font-size:18px;font-weight:700">${fmtAmount(o.amount_total)}</span></div>
              ${linesHtml}
              <div style="margin-top:24px">
                <button class="btn-sm primary" onclick="Portal.printOrder(${orderId})">🖨 Print / Save PDF</button>
              </div>`;
            document.getElementById('overlay').style.display = 'block';
            detail.style.display = 'block';
        } catch (e) {
            toast('Could not load order: ' + e.message);
        }
    }

    function printOrder(orderId) {
        window.open(`/portal/api/order/${orderId}/print`, '_blank');
    }

    // ── Deliveries ────────────────────────────────────────────────
    let _deliveries = [];

    async function loadDeliveries() {
        const tbody = document.getElementById('delivery-tbody');
        tbody.innerHTML = '<tr><td colspan="5" class="empty-state">Loading…</td></tr>';
        try {
            _deliveries = await api('GET', '/deliveries');
            renderDeliveries();
        } catch (e) {
            tbody.innerHTML = `<tr><td colspan="5" class="empty-state">${e.message}</td></tr>`;
        }
    }

    function deliveryStateBadge(s) {
        const map = { done: 'posted', confirmed: 'partial', assigned: 'partial', draft: 'draft', cancel: 'draft' };
        const lbl = { done: 'Done', confirmed: 'Confirmed', assigned: 'Ready', draft: 'Draft', cancel: 'Cancelled' };
        return `<span class="badge badge-${map[s]||'draft'}">${lbl[s]||s}</span>`;
    }

    function renderDeliveries() {
        const tbody = document.getElementById('delivery-tbody');
        if (!_deliveries.length) {
            tbody.innerHTML = '<tr><td colspan="5" class="empty-state"><div class="icon">🚚</div>No deliveries yet.</td></tr>';
            return;
        }
        tbody.innerHTML = _deliveries.map(d => `<tr>
          <td><a href="#" style="color:#2563eb;font-weight:600" onclick="Portal.openDeliveryDetail(${d.id});return false">${escHtml(d.name||'/')}</a></td>
          <td>${escHtml(d.origin||'—')}</td>
          <td>${fmtDate(d.scheduled_date)}</td>
          <td>${deliveryStateBadge(d.state)}</td>
          <td><button class="btn-sm primary" onclick="Portal.openDeliveryDetail(${d.id})">View</button>
              <button class="btn-sm secondary" onclick="Portal.printDelivery(${d.id})">Print</button></td>
        </tr>`).join('');
    }

    async function openDeliveryDetail(pickId) {
        try {
            const d = await api('GET', `/delivery/${pickId}/detail`);
            const detail = document.getElementById('invoice-detail');
            const body   = document.getElementById('detail-body');
            document.getElementById('detail-title').textContent = d.document_title || d.name || 'Delivery';
            const linesHtml = d.lines.length
                ? `<table style="margin-top:16px">
                    <thead><tr><th>Product</th><th>UOM</th><th style="text-align:right">Demand</th><th style="text-align:right">Done</th></tr></thead>
                    <tbody>${d.lines.map(l => `<tr>
                      <td>${escHtml(l.name||'')}</td>
                      <td>${escHtml(l.uom||'')}</td>
                      <td style="text-align:right">${parseFloat(l.demand)||0}</td>
                      <td style="text-align:right">${parseFloat(l.done)||0}</td>
                    </tr>`).join('')}</tbody></table>`
                : '<p class="text-muted" style="margin-top:16px">No moves.</p>';
            body.innerHTML = `
              <div class="info-row"><span class="info-label">Reference</span><span class="info-value">${escHtml(d.name||'/')}</span></div>
              <div class="info-row"><span class="info-label">Origin</span><span class="info-value">${escHtml(d.origin||'—')}</span></div>
              <div class="info-row"><span class="info-label">Date</span><span class="info-value">${fmtDate(d.scheduled_date)}</span></div>
              <div class="info-row"><span class="info-label">Status</span><span class="info-value">${deliveryStateBadge(d.state)}</span></div>
              ${linesHtml}
              <div style="margin-top:24px">
                <button class="btn-sm primary" onclick="Portal.printDelivery(${pickId})">🖨 Print / Save PDF</button>
              </div>`;
            document.getElementById('overlay').style.display = 'block';
            detail.style.display = 'block';
        } catch (e) {
            toast('Could not load delivery: ' + e.message);
        }
    }

    function printDelivery(pickId) {
        window.open(`/portal/api/delivery/${pickId}/print`, '_blank');
    }

    // ── Products / Storage Plans ──────────────────────────────────
    async function loadProducts() {
        const el = document.getElementById('product-list');
        el.innerHTML = '<div class="empty-state"><div class="icon">📦</div>Loading…</div>';
        try {
            const products = await api('GET', '/products');
            if (!products.length) {
                el.innerHTML = '<div class="empty-state"><div class="icon">📦</div>No storage plans configured yet. Contact your account manager.</div>';
                return;
            }
            el.innerHTML = products.map(p => `
              <div class="product-card">
                <div>
                  <div class="product-name">${escHtml(p.name)}</div>
                </div>
                <div style="display:flex;align-items:center;gap:20px">
                  <div class="product-price">${fmtAmount(p.price_unit)}</div>
                  <button class="btn-sm primary" onclick="Portal.requestInvoice(${p.product_id}, '${escAttr(p.name)}')">
                    Request Invoice
                  </button>
                </div>
              </div>`).join('');
        } catch (e) {
            el.innerHTML = `<div class="empty-state">${e.message}</div>`;
        }
    }

    async function requestInvoice(productId, productName) {
        if (!confirm(`Request an invoice for "${productName}" for this month?`)) return;
        try {
            const data = await api('POST', '/request', { product_id: productId });
            toast('Invoice request submitted. Reference #' + data.invoice_id);
            showSection('invoices');
        } catch (e) {
            toast('Error: ' + e.message);
        }
    }

    // ── Utilities ─────────────────────────────────────────────────
    function escHtml(s) {
        return String(s).replace(/&/g,'&amp;').replace(/</g,'&lt;').replace(/>/g,'&gt;').replace(/"/g,'&quot;');
    }
    function escAttr(s) {
        return String(s).replace(/'/g,"\\'").replace(/"/g,'&quot;');
    }

    // Public API
    return { login, logout, showSection, openDetail, closeDetail,
             printInvoice, uploadProof, requestInvoice,
             openOrderDetail, printOrder,
             openDeliveryDetail, printDelivery };
})();
