/**
 * DbBackups.js — in-app Database section (docs/075).
 *
 * Per-tenant snapshots for the current company only. Backup / export(download)
 * are one click; restore / import (destructive) require re-entering the admin
 * password. The server enforces admin-only + per-tenant isolation + audit — the
 * UI can never target another company's database.
 */
class DbBackups extends owl.Component {
    static template = owl.xml`
        <div class="db-screen">
            <h2>Database &amp; backups</h2>
            <p class="db-sub">Snapshots of <b><t t-esc="state.company || 'this company'"/></b> only.
               Restore and import are destructive and ask for your password.</p>
            <t t-if="state.error"><div class="db-error" t-esc="state.error"/></t>
            <t t-if="state.notice"><div class="db-notice" t-esc="state.notice"/></t>

            <div class="db-actions">
                <input class="db-label" placeholder="label (optional)" t-model="state.label"/>
                <button class="act" t-on-click="doBackup" t-att-disabled="state.busy">Create snapshot</button>
                <label class="db-import">
                    Import .dump<input type="file" accept=".dump" style="display:none" t-on-change="doImport"/>
                </label>
            </div>

            <table class="db-table">
                <thead><tr><th>Snapshot</th><th>Size</th><th>Created</th><th></th></tr></thead>
                <tbody>
                    <tr t-foreach="state.backups" t-as="b" t-key="b.file">
                        <td t-esc="b.file"/>
                        <td class="db-num" t-esc="fmtSize(b.size)"/>
                        <td class="muted" t-esc="b.date"/>
                        <td class="db-row-actions">
                            <a class="ghost" t-att-href="dl(b.file)">Download</a>
                            <button class="ghost" t-on-click="() => this.askRestore(b.file)">Restore</button>
                            <button class="db-del" t-on-click="() => this.doDelete(b.file)">Delete</button>
                        </td>
                    </tr>
                    <tr t-if="!state.backups.length"><td colspan="4" class="muted">No snapshots yet.</td></tr>
                </tbody>
            </table>

            <div t-if="state.confirm" class="db-confirm">
                <div class="db-confirm-card">
                    <h3>Restore “<t t-esc="state.confirm"/>”?</h3>
                    <p class="db-warn">This OVERWRITES the current database of <b t-esc="state.company"/>.
                       Enter your password to confirm.</p>
                    <input type="password" placeholder="your password" t-model="state.pw"/>
                    <div class="db-confirm-actions">
                        <button class="ghost" t-on-click="cancelRestore">Cancel</button>
                        <button class="danger" t-on-click="doRestore" t-att-disabled="state.busy">Restore now</button>
                    </div>
                    <pre t-if="state.restoreOut" t-esc="state.restoreOut"/>
                </div>
            </div>
        </div>
    `;

    setup() {
        this.state = owl.useState({
            company: '', backups: [], label: '', error: null, notice: null,
            busy: false, confirm: null, pw: '', restoreOut: '',
        });
        this.reload();
    }

    async reload() {
        try {
            const r = await RpcService.dbList();
            if (r && r.error) { this.state.error = r.error; return; }
            this.state.company = (r && r.company) || '';
            this.state.backups = (r && r.backups) || [];
            this.state.error = null;
        } catch (e) { this.state.error = e.message; }
    }

    fmtSize(bytes) {
        const n = Number(bytes) || 0;
        if (n > 1e9) return (n / 1e9).toFixed(1) + ' GB';
        if (n > 1e6) return (n / 1e6).toFixed(1) + ' MB';
        if (n > 1e3) return (n / 1e3).toFixed(0) + ' KB';
        return n + ' B';
    }
    dl(file) { return RpcService.dbDownloadUrl(file); }

    async doBackup() {
        this.state.busy = true; this.state.error = null; this.state.notice = 'Creating snapshot…';
        try {
            const r = await RpcService.dbBackup(this.state.label);
            if (r && r.ok) { this.state.notice = 'Snapshot created: ' + r.file; this.state.label = ''; await this.reload(); }
            else { this.state.error = (r && r.output) || 'backup failed'; this.state.notice = null; }
        } catch (e) { this.state.error = e.message; this.state.notice = null; }
        this.state.busy = false;
    }

    async doImport(ev) {
        const file = ev.target.files && ev.target.files[0];
        if (!file) return;
        this.state.busy = true; this.state.notice = 'Uploading ' + file.name + '…'; this.state.error = null;
        try {
            const r = await RpcService.dbUpload(file);
            if (r && r.ok) { this.state.notice = 'Imported ' + r.file + ' — use Restore to apply it.'; await this.reload(); }
            else { this.state.error = (r && r.error) || 'import failed'; this.state.notice = null; }
        } catch (e) { this.state.error = e.message; this.state.notice = null; }
        this.state.busy = false; ev.target.value = '';
    }

    askRestore(file) { this.state.confirm = file; this.state.pw = ''; this.state.restoreOut = ''; }
    cancelRestore()  { this.state.confirm = null; this.state.pw = ''; }
    async doRestore() {
        if (!this.state.pw) { this.state.restoreOut = 'Password required.'; return; }
        this.state.busy = true; this.state.restoreOut = 'Restoring…';
        try {
            const r = await RpcService.dbRestore(this.state.confirm, this.state.pw);
            if (r && r.error) { this.state.restoreOut = r.error; }
            else { this.state.restoreOut = 'Restore complete. Reloading…'; setTimeout(() => window.location.reload(), 1500); }
        } catch (e) { this.state.restoreOut = e.message; }
        this.state.busy = false;
    }

    async doDelete(file) {
        if (!confirm('Delete snapshot ' + file + '?')) return;
        try { await RpcService.dbDelete(file); await this.reload(); }
        catch (e) { this.state.error = e.message; }
    }
}
