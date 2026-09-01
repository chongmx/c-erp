#!/usr/bin/env python3
"""One-off: give the verify scripts a cookie jar for the /rental/ routes.

Those routes now authenticate, so the scripts that call them must too.
Each already signs in for JSON-RPC; this captures the session COOKIE from
that same call (-c) and presents it on the rental requests (-b).
"""
import io
import re
import sys

JOBS = {
    "scripts/verify_rental_billing.sh":   "vrb",
    "scripts/verify_rental_cashflow.sh":  "vrc",
    "scripts/verify_rental_dashboard.sh": "vrd2",
    "scripts/verify_rental_portal.sh":    "vrp",
}

for path, tag in JOBS.items():
    s = io.open(path, encoding="utf8").read()
    jar = f"/tmp/{tag}_cookie.txt"

    if "_cookie.txt" in s:
        print(f"{path}: already patched")
        continue

    # 1. Capture the cookie on whichever authenticate call the script makes.
    before = s
    s = re.sub(
        r'curl -s (-X POST "\$BASE/web/session/authenticate")',
        rf'curl -s -c {jar} \1', s, count=1)
    if s == before:
        print(f"{path}: NO authenticate call matched — check by hand", file=sys.stderr)
        sys.exit(1)

    # 2. Present it on every /rental/ request.
    s = re.sub(r'curl -s (-o /dev/null )?(-w [^ ]+ )?-X POST "\$BASE/rental/',
               lambda m: (f'curl -s {m.group(1) or ""}{m.group(2) or ""}'
                          f'-b {jar} -X POST "$BASE/rental/'), s)
    s = re.sub(r'curl -s "\$BASE/rental/',
               f'curl -s -b {jar} "$BASE/rental/', s)

    io.open(path, "w", encoding="utf8").write(s)
    n = s.count(jar) - 1
    print(f"{path}: cookie jar added, {n} rental call(s) authenticated")
