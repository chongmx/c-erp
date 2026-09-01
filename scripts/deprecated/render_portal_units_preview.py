#!/usr/bin/env python3
"""
Render the portal "My Units" page from live data, using the real CSS.

Provisions portal access for the demo customer, logs in as them, fetches
/portal/api/units, and renders the same markup portal.js produces.

    python3 scripts/render_portal_units_preview.py > /tmp/units.html
"""
import html
import io
import json
import os
import re
import subprocess
import sys
import urllib.error
import urllib.request

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
BASE = os.environ.get("BASE", "http://127.0.0.1:8069")


def psql(sql):
    env = dict(os.environ, PGPASSWORD="odoo")
    return subprocess.run(
        ["psql", "-q", "-h", "localhost", "-U", "odoo", "-d", "odoo", "-tAc", sql],
        capture_output=True, text=True, env=env).stdout.strip()


def post(path, payload, cookie=None):
    req = urllib.request.Request(
        BASE + path, data=json.dumps(payload).encode(),
        headers={"Content-Type": "application/json"})
    if cookie:
        req.add_header("Cookie", cookie)
    return urllib.request.urlopen(req)


def main():
    partner = psql("SELECT partner_id FROM rental_contract_line "
                   "WHERE state='active' ORDER BY id LIMIT 1")
    if not partner:
        sys.stderr.write("no active tenancy — run scripts/seed_rental_demo.sh first\n")
        return 1

    email = psql(f"SELECT COALESCE(email,'') FROM res_partner WHERE id={partner}")
    if not email:
        email = f"demo_tenant_{partner}@example.com"
        psql(f"UPDATE res_partner SET email='{email}' WHERE id={partner}")

    # Provision portal access through the admin action, which is the flow
    # a real operator follows — the password hash is PBKDF2 with a random
    # salt and cannot be written directly.
    admin = post("/web/session/authenticate",
                 {"jsonrpc": "2.0", "method": "call",
                  "params": {"db": "odoo", "login": "admin", "password": "admin"}})
    sid = re.search(r'"session_id":"([a-f0-9]+)"', admin.read().decode()).group(1)
    post("/web/dataset/call_kw",
         {"jsonrpc": "2.0", "method": "call",
          "params": {"model": "portal.partner", "method": "portal_reset_password",
                     "args": [[int(partner)]],
                     "kwargs": {"context": {"session_id": sid}}}})

    login = post("/portal/api/login", {"email": email, "password": "Welcome1"})
    cookie = login.headers.get("Set-Cookie", "").split(";")[0]
    if not cookie:
        sys.stderr.write("portal login failed\n")
        return 1

    req = urllib.request.Request(BASE + "/portal/api/units",
                                 headers={"Cookie": cookie})
    d = json.load(urllib.request.urlopen(req))

    # Reuse the real stylesheet from portal.html.
    ph = io.open(os.path.join(ROOT, "web/static/portal.html"), encoding="utf8").read()
    css = re.search(r"<style>(.*?)</style>", ph, re.S).group(1)

    s = d.get("summary", {})
    units = d.get("units", [])

    def money(v):
        return f"{float(v or 0):,.2f}"

    def date(v):
        return v or "—"

    overdue = float(s.get("overdue", 0))
    ob = (f'<div class="bal-overdue"><div class="k">⚠ Overdue</div>'
          f'<div class="v">{money(overdue)}</div></div>' if overdue > 0 else
          '<div class="bal-clear"><div class="k">✓ Overdue</div>'
          '<div class="v">None</div></div>')

    cards = []
    for u in units:
        per = (f'every {u["billing_months"]} months' if u["billing_months"] > 1
               else "per month")
        badge = ('<span class="uc-badge">Auto-billed</span>' if u["recurring"]
                 else '<span class="uc-badge manual">Billed manually</span>')
        rows = ""
        if u.get("zone"):
            rows += f'<dt>Zone</dt><dd>{html.escape(u["zone"])}</dd>'
        rows += f'<dt>Since</dt><dd>{date(u["since"])}</dd>'
        if u.get("until"):
            rows += f'<dt>Until</dt><dd>{date(u["until"])}</dd>'
        if u["recurring"] and u.get("next_period"):
            rows += f'<dt>Next period</dt><dd>{date(u["next_period"])}</dd>'
        rows += f'<dt>Status</dt><dd>{html.escape(u["state"])}</dd>'
        cards.append(f'''
          <div class="unit-card">
            <div class="uc-top">
              <div><div class="uc-code">{html.escape(u["code"] or "—")}</div>
                   <div class="uc-type">{html.escape(u["type"] or u["name"] or "")}</div></div>
              <div style="text-align:right">
                <div class="uc-rate">{money(u["net_rate"])}</div>
                <div class="uc-per">{per}</div></div>
            </div>
            {badge}
            <dl>{rows}</dl>
          </div>''')

    nxt = (f'Next payment due {s["next_due_date"]}' if s.get("next_due_date")
           else "Nothing outstanding")

    sys.stdout.write(f"""<title>Portal — My Units (preview)</title>
<style>{css}
body{{margin:0;background:#f1f5f9;font:14px/1.5 system-ui,-apple-system,'Segoe UI',sans-serif}}
.wrap{{max-width:1000px;margin:0 auto;padding:24px}}
.page-title{{font-size:22px;font-weight:600;color:#0f172a;margin-bottom:16px}}
.empty-state{{padding:28px;text-align:center;color:#64748b}}
</style>
<div class="wrap">
  <div class="page-title">My Units</div>
  <div class="bal-hero">
    <div class="bal-main">
      <div class="bal-label">Balance due</div>
      <div class="bal-value">{money(s.get('balance_due'))}</div>
      <div class="uc-per">{nxt}</div>
    </div>
    <div class="bal-side">
      {ob}
      <div><div class="k">Units</div><div class="v">{s.get('count', 0)}</div></div>
      <div><div class="k">Per month</div>
           <div class="v">{money(s.get('monthly_total'))}</div></div>
    </div>
  </div>
  <div class="unit-cards">{''.join(cards)}</div>
</div>""")
    sys.stderr.write(f"{len(units)} units, balance {money(s.get('balance_due'))}\n")
    return 0


if __name__ == "__main__":
    sys.exit(main())
