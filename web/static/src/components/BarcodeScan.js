/**
 * BarcodeScan.js — Inventory → Barcode.
 *
 * A scan screen: type or scan a barcode, and it resolves what the code is
 * (product / location / lot) via stock.quant.resolve_barcode. The input keeps
 * focus so a hardware scanner (which types + Enter) drives it hands-free.
 */
class BarcodeScan extends owl.Component {
    static template = owl.xml`
        <div class="bc-wrap">
            <div class="bc-head">
                <h2>Barcode</h2>
                <p>Scan or type a barcode — products, locations and lots are recognised.</p>
            </div>
            <div class="bc-scanbar">
                <input class="bc-input" t-ref="scan"
                       placeholder="Scan a barcode…"
                       t-model="state.code"
                       t-on-keydown="onKey"/>
                <button class="btn btn-primary" t-on-click="onScan">Scan</button>
            </div>
            <t t-if="state.error"><div class="bc-note bc-err" t-esc="state.error"/></t>
            <t t-if="state.last">
                <div t-attf-class="bc-result bc-{{state.last.type}}">
                    <span class="bc-type" t-esc="state.last.type"/>
                    <span class="bc-name" t-esc="state.last.name || '(unknown code)'"/>
                    <span class="bc-id" t-if="state.last.id" t-esc="'#' + state.last.id"/>
                </div>
            </t>
            <div class="bc-card" t-if="state.history.length">
                <h3>Recent scans</h3>
                <table class="bc-table"><tbody>
                    <t t-foreach="state.history" t-as="h" t-key="h.key">
                        <tr>
                            <td class="bc-type" t-esc="h.type"/>
                            <td t-esc="h.name || '(unknown)'"/>
                            <td class="bc-code" t-esc="h.code"/>
                        </tr>
                    </t>
                </tbody></table>
            </div>
        </div>`;

    setup() {
        this.state = owl.useState({ code: '', last: null, error: '', history: [] });
        this.scanRef = owl.useRef('scan');
        owl.onMounted(() => { if (this.scanRef.el) this.scanRef.el.focus(); });
    }

    onKey(ev) { if (ev.key === 'Enter') { ev.preventDefault(); this.onScan(); } }

    async onScan() {
        const code = (this.state.code || '').trim();
        if (!code) return;
        this.state.error = '';
        try {
            const res = await RpcService.call('stock.quant', 'resolve_barcode', [{ barcode: code }]);
            this.state.last = res;
            this.state.history.unshift({ key: Date.now() + '-' + code, code, type: res.type, name: res.name });
            if (this.state.history.length > 20) this.state.history.pop();
        } catch (e) {
            this.state.error = (e && e.message) || 'Scan failed';
        }
        this.state.code = '';
        if (this.scanRef.el) this.scanRef.el.focus();
    }
}
