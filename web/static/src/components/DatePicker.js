/**
 * DatePicker — a fully theme-aware calendar popup component.
 * Props:
 *   value    {string}   YYYY-MM-DD or ''
 *   onSelect {function} called with the chosen YYYY-MM-DD string
 */
const DatePicker = (() => {
const { Component, useState, xml, onMounted, onWillUnmount } = owl;

class DatePicker extends Component {
    static template = xml`
        <div class="dp-wrap">
            <input class="form-input dp-input"
                   type="text"
                   readonly="readonly"
                   t-att-value="displayValue"
                   t-on-click.stop="togglePicker"
                   placeholder="Select date"/>
            <span class="dp-icon" t-on-click.stop="togglePicker">&#128197;</span>
            <t t-if="state.open">
                <div class="dp-popup" t-on-click.stop="onPopupClick">
                    <div class="dp-head">
                        <button class="dp-nav" data-dp-nav="prev">&#8249;</button>
                        <span class="dp-month-label" t-esc="monthLabel"/>
                        <button class="dp-nav" data-dp-nav="next">&#8250;</button>
                    </div>
                    <div class="dp-weekdays">
                        <span>Su</span><span>Mo</span><span>Tu</span>
                        <span>We</span><span>Th</span><span>Fr</span><span>Sa</span>
                    </div>
                    <div class="dp-grid">
                        <t t-foreach="calDays" t-as="d" t-key="d.key">
                            <button t-attf-class="dp-day{{d.outside?' dp-out':''}}{{d.today?' dp-today':''}}{{d.selected?' dp-sel':''}}"
                                    t-att-data-date="d.date"
                                    t-esc="d.label"/>
                        </t>
                    </div>
                </div>
            </t>
        </div>
    `;

    setup() {
        this.state = useState({ open: false, viewYear: 0, viewMonth: 0 });
        onMounted(() => {
            this._closeOnOutside = () => { this.state.open = false; };
            document.addEventListener('click', this._closeOnOutside);
        });
        onWillUnmount(() => {
            document.removeEventListener('click', this._closeOnOutside);
        });
    }

    // ── helpers ──────────────────────────────────────────────────

    _pad(n) { return String(n).padStart(2, '0'); }

    _isoDate(year, month, day) {
        return `${year}-${this._pad(month + 1)}-${this._pad(day)}`;
    }

    get displayValue() {
        const v = this.props.value;
        if (!v) return '';
        const [y, m, d] = v.split('-').map(Number);
        return new Date(y, m - 1, d)
            .toLocaleDateString('en-GB', { day: '2-digit', month: 'short', year: 'numeric' });
    }

    get monthLabel() {
        return new Date(this.state.viewYear, this.state.viewMonth)
            .toLocaleDateString('en', { month: 'long', year: 'numeric' });
    }

    get calDays() {
        const { viewYear: y, viewMonth: m } = this.state;
        const today    = new Date().toISOString().substring(0, 10);
        const selected = this.props.value || '';
        const firstDow = new Date(y, m, 1).getDay();          // 0=Sun
        const inMonth  = new Date(y, m + 1, 0).getDate();     // days in month
        const inPrev   = new Date(y, m, 0).getDate();         // days in prev month
        const days = [];

        // leading days from previous month
        for (let i = firstDow - 1; i >= 0; i--) {
            const day = inPrev - i;
            const ds  = this._isoDate(y, m - 1, day);
            days.push({ key: 'p' + ds, date: ds, label: day, outside: true,
                        today: ds === today, selected: ds === selected });
        }
        // current month
        for (let i = 1; i <= inMonth; i++) {
            const ds = this._isoDate(y, m, i);
            days.push({ key: ds, date: ds, label: i, outside: false,
                        today: ds === today, selected: ds === selected });
        }
        // trailing days
        const rem = 42 - days.length;
        for (let i = 1; i <= rem; i++) {
            const ds = this._isoDate(y, m + 1, i);
            days.push({ key: 'n' + ds, date: ds, label: i, outside: true,
                        today: ds === today, selected: ds === selected });
        }
        return days;
    }

    // ── event handlers ────────────────────────────────────────────

    togglePicker() {
        if (!this.state.open) {
            // seed view from current value or today
            const raw = this.props.value || new Date().toISOString().substring(0, 10);
            const [y, m] = raw.split('-').map(Number);
            this.state.viewYear  = y;
            this.state.viewMonth = m - 1;
        }
        this.state.open = !this.state.open;
    }

    // All clicks inside the popup route here (outside clicks close via document listener)
    onPopupClick(e) {
        const nav = e.target.closest('[data-dp-nav]');
        if (nav) {
            if (nav.dataset.dpNav === 'prev') {
                if (this.state.viewMonth === 0) { this.state.viewMonth = 11; this.state.viewYear--; }
                else this.state.viewMonth--;
            } else {
                if (this.state.viewMonth === 11) { this.state.viewMonth = 0; this.state.viewYear++; }
                else this.state.viewMonth++;
            }
            return;
        }
        const dayBtn = e.target.closest('[data-date]');
        if (dayBtn) {
            this.props.onSelect(dayBtn.dataset.date);
            this.state.open = false;
        }
    }
}

return DatePicker;
})();
