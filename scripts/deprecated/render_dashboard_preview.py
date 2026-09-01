#!/usr/bin/env python3
"""
Render the rental dashboard from live data, using the real CSS.

The verification script checks wiring and arithmetic. It cannot check
LAYOUT — label collisions, bar proportions, whether a 12-month series
reflows. docs/046 §9: render it and look at it before calling it done.

The chart geometry here mirrors RentalDashboard.js deliberately; if the
two ever disagree, this preview is wrong and the component is right.

    python3 scripts/render_dashboard_preview.py [months] > /tmp/dash.html
"""
import html
import io
import json
import os
import sys
import urllib.request

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
BASE = os.environ.get("BASE", "http://127.0.0.1:8069")
CW, CH = 780, 240
PLOT_LEFT = 52
STATES = ["occupied", "available", "reserved", "maintenance"]

AGE = [("current", "Not yet due", "#86b6ef"), ("d0_30", "0–30 days", "#3987e5"),
       ("d31_60", "31–60 days", "#256abf"), ("d61_90", "61–90 days", "#104281"),
       ("d90_plus", "90+ days", "#0b2f5c")]

ATTN = [("overdue_60d", "Invoices overdue > 60 days", "⛔"),
        ("units_in_maintenance", "Units in maintenance", "⚠"),
        ("units_vacant", "Units vacant", "○"),
        ("walk_in_tenancies", "Walk-ins not auto-billed", "✎"),
        ("unallocated_payments", "Payments not allocated", "◐")]


def money(v):
    return f"{float(v or 0):,.2f}"


def short(v):
    n, a = float(v or 0), abs(float(v or 0))
    if a >= 1e6:
        return f"{n/1e6:.1f}M"
    if a >= 1e3:
        return f"{n/1e3:.0f}k"
    return f"{n:.0f}"


def chart_svg(series, mode):
    """Grouped bars (monthly) or a single cumulative line."""
    if not series:
        return "<p>No data</p>"
    vals = ([r["cumulative"] for r in series] if mode == "cumulative"
            else [v for r in series for v in (r["income"], r["expense"])])
    hi, lo = max(0, *vals), min(0, *vals)
    if hi == 0 and lo == 0:
        hi = 1
    hi *= 1.08
    if lo < 0:
        lo *= 1.08
    top, bottom = 12, CH - 26
    span = (hi - lo) or 1

    def y(v):
        return bottom - ((v - lo) / span) * (bottom - top)

    plot_w = CW - PLOT_LEFT - 8
    slot = plot_w / len(series)
    p = [f'<svg viewBox="0 0 {CW} {CH}" class="chart" '
         f'preserveAspectRatio="xMidYMid meet">']

    for i in range(5):
        v = lo + (hi - lo) * i / 4
        p.append(f'<line class="grid" x1="{PLOT_LEFT}" x2="{CW-8}" '
                 f'y1="{y(v):.1f}" y2="{y(v):.1f}"/>')
        p.append(f'<text class="axis" x="46" y="{y(v)+4:.1f}" '
                 f'text-anchor="end">{short(v)}</text>')
    p.append(f'<line class="zero" x1="{PLOT_LEFT}" x2="{CW-8}" '
             f'y1="{y(0):.1f}" y2="{y(0):.1f}"/>')

    if mode == "cumulative":
        pts = [(PLOT_LEFT + slot * i + slot / 2, y(r["cumulative"]), r)
               for i, r in enumerate(series)]
        d = " ".join(f"{'M' if i == 0 else 'L'}{x:.1f},{yy:.1f}"
                     for i, (x, yy, _) in enumerate(pts))
        p.append(f'<path class="cum-line" d="{d}"/>')
        for x, yy, r in pts:
            fill = "var(--st-critical)" if r["cumulative"] < 0 else "var(--s-income)"
            p.append(f'<circle cx="{x:.1f}" cy="{yy:.1f}" r="5" fill="{fill}">'
                     f'<title>{r["month"]}: {money(r["cumulative"])}</title></circle>')
    else:
        bw = max(4, min(18, (slot - 8) / 2))
        for i, r in enumerate(series):
            cx = PLOT_LEFT + slot * i + slot / 2
            for k, (val, fill) in enumerate(
                    [(r["income"], "var(--s-income)"),
                     (r["expense"], "var(--s-expense)")]):
                x = cx - bw - 1 + k * (bw + 2)
                y0, y1 = y(0), y(val)
                p.append(f'<rect x="{x:.1f}" y="{min(y0,y1):.1f}" width="{bw:.1f}" '
                         f'height="{max(1,abs(y1-y0)):.1f}" rx="4" ry="4" fill="{fill}">'
                         f'<title>{r["month"]} income {money(r["income"])} / '
                         f'expense {money(r["expense"])}</title></rect>')

    every = 3 if len(series) > 14 else (2 if len(series) > 8 else 1)
    for i, r in enumerate(series):
        if i % every:
            continue
        x = PLOT_LEFT + slot * i + slot / 2
        p.append(f'<text class="axis" x="{x:.1f}" y="{CH-6}" '
                 f'text-anchor="middle">{r["month"][2:]}</text>')
    p.append("</svg>")
    return "\n".join(p)


def main():
    months = sys.argv[1] if len(sys.argv) > 1 else "12"
    d = json.load(urllib.request.urlopen(
        f"{BASE}/rental/dashboard?months={months}&fresh=1"))
    css = io.open(os.path.join(ROOT, "web/static/src/components/rental/rental.css"),
                  encoding="utf8").read()
    s = d["cashflow"]["series"]
    occ = d["occupancy"]

    p = ["<title>Rental dashboard — preview</title>",
         f"<style>{css}\nbody{{margin:0;font:14px/1.45 system-ui,-apple-system,"
         "'Segoe UI',sans-serif;background:var(--viz-surface);"
         "color:var(--ink-primary)}</style>",
         '<div class="rental-dash viz-root">']

    warn = " is-warn" if float(d["receivables"]["overdue"]) > 0 else ""
    nwarn = " is-warn" if float(d["noi_month"]) < 0 else ""
    p.append(f'''<div class="kpi-row">
  <div class="kpi"><div class="kpi-label">Occupancy</div>
    <div class="kpi-value">{occ["pct"]}%</div>
    <div class="kpi-meter"><div class="kpi-meter-fill" style="width:{occ["pct"]}%"></div></div>
    <div class="kpi-sub">{occ["occupied"]} of {occ["lettable"]} lettable</div></div>
  <div class="kpi"><div class="kpi-label">MRR</div>
    <div class="kpi-value">{money(d["mrr"])}</div>
    <div class="kpi-sub">recurring tenancies only</div></div>
  <div class="kpi"><div class="kpi-label">Outstanding</div>
    <div class="kpi-value">{money(d["receivables"]["outstanding"])}</div>
    <div class="kpi-sub">unpaid invoices</div></div>
  <div class="kpi{warn}"><div class="kpi-label">Overdue</div>
    <div class="kpi-value">{money(d["receivables"]["overdue"])}</div>
    <div class="kpi-sub">⚠ past due date</div></div>
  <div class="kpi{nwarn}"><div class="kpi-label">Net this month</div>
    <div class="kpi-value">{money(d["noi_month"])}</div>
    <div class="kpi-sub">income − expenses</div></div>
</div>''')

    for mode, title in (("monthly", "Cashflow forecast — monthly"),
                        ("cumulative", "Cashflow forecast — cumulative net")):
        legend = ('<div class="rental-legend">'
                  '<span class="item"><span class="swatch" style="background:var(--s-income)"></span>'
                  '<span>Income</span></span>'
                  '<span class="item"><span class="swatch" style="background:var(--s-expense)"></span>'
                  '<span>Expenses</span></span></div>') if mode == "monthly" else ""
        note = ("Running total of net cashflow. Below the zero line is a month the "
                "projection does not cover." if mode == "cumulative" else
                "Income is invoiced amounts plus projected rent; expenses are "
                "budgeted recurring plus dated one-offs.")
        p.append(f'<div class="panel"><div class="panel-head"><h3>{title}</h3></div>'
                 f'{legend}<div class="chart-wrap">{chart_svg(s, mode)}</div>'
                 f'<div class="chart-note">{note}</div></div>')

    rows = "".join(
        f'<tr><td>{r["month"]}</td><td class="num">{money(r["receivable"])}</td>'
        f'<td class="num">{money(r["projected_income"])}</td>'
        f'<td class="num">{money(r["income"])}</td>'
        f'<td class="num">{money(r["expense"])}</td>'
        f'<td class="num{" neg" if r["net"] < 0 else ""}">{money(r["net"])}</td>'
        f'<td class="num{" neg" if r["cumulative"] < 0 else ""}">{money(r["cumulative"])}</td></tr>'
        for r in s)
    p.append('<div class="panel"><div class="panel-head"><h3>Cashflow — table view</h3></div>'
             '<div class="table-scroll"><table class="rental-table"><thead><tr>'
             '<th>Month</th><th class="num">Invoiced</th><th class="num">Projected</th>'
             '<th class="num">Income</th><th class="num">Expenses</th>'
             '<th class="num">Net</th><th class="num">Cumulative</th></tr></thead>'
             f'<tbody>{rows}</tbody></table></div></div>')

    p.append('<div class="panel-row">')
    stacks = []
    for name in sorted(occ.get("by_type", {})):
        counts = occ["by_type"][name]
        total = sum(counts.get(k, 0) for k in STATES) + counts.get("retired", 0)
        segs = "".join(
            f'<div class="seg" style="width:{counts[st]*100/total:.1f}%;'
            f'background:var(--u-{st})" title="{st}: {counts[st]}">'
            f'{counts[st] if counts[st]*100/total > 12 else ""}</div>'
            for st in STATES if counts.get(st, 0) > 0)
        stacks.append(f'<div class="stack-row"><div class="stack-label">'
                      f'{html.escape(name)}<span class="muted">{total}</span></div>'
                      f'<div class="stack">{segs}</div></div>')
    legend = "".join(f'<span class="item"><span class="swatch" '
                     f'style="background:var(--u-{st})"></span><span>{st}</span></span>'
                     for st in STATES)
    p.append('<div class="panel"><div class="panel-head"><h3>Occupancy by unit type</h3></div>'
             + "".join(stacks) + f'<div class="rental-legend">{legend}</div></div>')

    mx = max([float(d["ageing"].get(k, 0)) for k, _, _ in AGE] + [1])
    age_rows = "".join(
        f'<div class="age-row"><div class="age-label">{lbl}</div>'
        f'<div class="age-track"><div class="age-fill" '
        f'style="width:{float(d["ageing"].get(k,0))*100/mx:.1f}%;background:{col}"></div></div>'
        f'<div class="age-amt">{money(d["ageing"].get(k,0))}</div></div>'
        for k, lbl, col in AGE)
    p.append('<div class="panel"><div class="panel-head"><h3>Receivables ageing</h3></div>'
             + age_rows + '</div>')
    p.append('</div>')

    p.append('<div class="panel-row">')
    arows = "".join(
        f'<tr><td>{(icon + " ") if d["attention"].get(k,0) else ""}{lbl}</td>'
        f'<td class="num{" attn" if d["attention"].get(k,0) else ""}">'
        f'{d["attention"].get(k,0)}</td></tr>'
        for k, lbl, icon in ATTN)
    p.append('<div class="panel"><div class="panel-head"><h3>Needs attention</h3></div>'
             f'<table class="rental-table"><tbody>{arows}</tbody></table></div>')
    feed = "".join(f'<li><span class="feed-at">{html.escape(e["at"])}</span>'
                   f'<span class="feed-sum">{html.escape(e["summary"])}</span></li>'
                   for e in d.get("activity", [])[:10])
    p.append('<div class="panel"><div class="panel-head"><h3>Recent activity</h3></div>'
             + (f'<ul class="feed">{feed}</ul>' if feed
                else '<div class="rental-empty">No events yet.</div>') + '</div>')
    p.append('</div></div>')

    sys.stdout.write("\n".join(p))
    sys.stderr.write(f"{len(s)} months, occupancy {occ['pct']}%, "
                     f"MRR {money(d['mrr'])}\n")
    return 0


if __name__ == "__main__":
    sys.exit(main())
