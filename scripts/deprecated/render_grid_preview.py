#!/usr/bin/env python3
"""
Render a static preview of the unit grid from real data.

The verification script checks wiring — assets served, RPC shapes, state
coverage. It cannot check LAYOUT. This produces the actual markup and CSS
so the grid can be looked at: label collisions, cell proportions, whether
45 units across five zones reflows sensibly.

    python3 scripts/render_grid_preview.py > /tmp/grid_preview.html
"""
import collections
import html
import io
import os
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)

GLYPH = {"occupied": "■", "available": "□", "reserved": "▤",
         "maintenance": "⚠", "retired": "✖"}
LABEL = {"occupied": "Occupied", "available": "Available", "reserved": "Reserved",
         "maintenance": "Maintenance", "retired": "Retired"}
ORDER = ["occupied", "available", "reserved", "maintenance", "retired"]

SQL = """
SELECT u.code, COALESCE(u.zone,''), COALESCE(t.name,''), u.state
  FROM rental_unit u
  LEFT JOIN rental_unit_type t ON t.id = u.type_id
 WHERE u.site = 'Demo Warehouse'
 ORDER BY u.zone, u.code
"""


def fetch():
    env = dict(os.environ, PGPASSWORD="odoo")
    out = subprocess.run(
        ["psql", "-q", "-h", "localhost", "-U", "odoo", "-d", "odoo",
         "-t", "-A", "-F", "|", "-c", SQL],
        capture_output=True, text=True, env=env).stdout
    return [l.split("|") for l in out.splitlines() if l.strip()]


def main():
    rows = fetch()
    if not rows:
        sys.stderr.write("no demo units — run scripts/seed_rental_demo.sh first\n")
        return 1

    css = io.open(os.path.join(ROOT, "web/static/src/components/rental/rental.css"),
                  encoding="utf8").read()

    zones = collections.OrderedDict()
    count = collections.Counter()
    for code, zone, tname, state in rows:
        zones.setdefault(zone or "Unzoned", []).append((code, tname, state))
        count[state] += 1

    # Retired is not lettable stock, so it stays out of the denominator —
    # the same rule the component applies.
    lettable = sum(count[k] for k in ORDER if k != "retired")
    pct = round(count["occupied"] / lettable * 100) if lettable else 0

    p = ["<title>Rental unit grid — preview</title>",
         "<style>%s\nbody{margin:0;font:14px/1.45 system-ui,-apple-system,"
         "'Segoe UI',sans-serif;background:var(--viz-surface);"
         "color:var(--ink-primary)}</style>" % css,
         '<div class="rental-grid-wrap viz-root">',
         '<div class="rental-summary">']

    for k in ORDER:
        if count[k]:
            p.append('<div><span class="n">%d</span>%s</div>' % (count[k], LABEL[k]))
    p.append('<div><span class="n">%d%%</span>Occupancy</div></div>' % pct)

    p.append('<div class="rental-legend">')
    for k in ORDER:
        p.append('<span class="item"><span class="swatch" '
                 'style="background:var(--u-%s)"></span><span>%s</span>'
                 '<span>%s</span></span>' % (k, GLYPH[k], LABEL[k]))
    p.append("</div>")

    for zone, units in zones.items():
        p.append('<div class="rental-zone"><h3>%s — %d units</h3>'
                 '<div class="rental-grid">' % (html.escape(zone), len(units)))
        for code, tname, state in units:
            p.append('<button class="rental-cell is-%s" title="%s">'
                     '<span class="top"><span class="code">%s</span>'
                     '<span class="glyph">%s</span></span>'
                     '<span class="label">%s</span></button>'
                     % (state, html.escape(tname), html.escape(code),
                        GLYPH.get(state, "?"), state))
        p.append("</div></div>")

    p.append("</div>")
    sys.stdout.write("\n".join(p))
    sys.stderr.write("%d units, %d%% occupancy, %d zones\n"
                     % (len(rows), pct, len(zones)))
    return 0


if __name__ == "__main__":
    sys.exit(main())
