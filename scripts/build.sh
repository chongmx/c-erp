#!/bin/bash
# =============================================================
# scripts/build.sh — build the binaries, and run the admin console.
#
#   ./build/c-erp       the ERP server            (default target)
#   ./build/erp-admin   the IT admin console      (docs/073)
#
# Binaries stay in ./build (paths / systemd unit unchanged).
#
# BUILD
#   ./scripts/build.sh                 configure + build both
#   ./scripts/build.sh --server        build only ./build/c-erp
#   ./scripts/build.sh --admin         build only ./build/erp-admin
#   ./scripts/build.sh --clean         wipe ./build first (full rebuild)
#   ./scripts/build.sh --jobs 8        override parallelism (also -j 8)
#
# ADMIN CONSOLE
#   ./scripts/build.sh --run-admin     build it, start it, print the URL
#   ./scripts/build.sh --admin-url     re-print the URL of a running console
#   ./scripts/build.sh --stop-admin    stop it
#   ./scripts/build.sh --admin-port N  use port N instead of 8072
#
#   ./scripts/build.sh -h | --help     this help
#
# WHY --run-admin EXISTS. erp-admin is not a service and nothing starts it;
# it is EXCLUDE_FROM_ALL, so a plain `cmake --build ./build` does not even
# produce it. It also mints a NEW one-time token on every start and prints
# it once. Open it without `?token=…` and the page still loads while every
# panel fails with 401 — which looks exactly like "the console is broken".
# So this subcommand builds it, starts it, waits for the port, and hands you
# the complete URL. --admin-url gets it back after the console output has
# scrolled away.
#
# The console binds 127.0.0.1 ONLY and refuses anything else — that is
# deliberate (docs/073). From WSL that is not a barrier: Windows forwards
# localhost into the VM, so the printed URL opens in a Windows browser as-is.
# The "use an SSH tunnel" line the console prints is advice for a REMOTE
# server; on this machine you can ignore it.
# =============================================================
set -euo pipefail
cd "$(dirname "$0")/.."

JOBS="$(nproc 2>/dev/null || echo 4)"
CLEAN=
ACTION=build           # build | run-admin | stop-admin | admin-url
TARGETS=both           # both | server | admin
ADMIN_PORT=8072
ADMIN_LOG=log/erp-admin.log

set_action() {         # one action per invocation, like server.sh
    [ "$ACTION" = build ] || { echo "ERROR: pick one action, got --$ACTION and $1" >&2; exit 2; }
    ACTION=${1#--}
}

while [ $# -gt 0 ]; do
    case "$1" in
        --clean)       CLEAN=1; shift ;;
        --jobs)        JOBS="${2:?--jobs needs a number}"; shift 2 ;;
        -j)            JOBS="${2:?-j needs a number}"; shift 2 ;;
        --server)      TARGETS=server; shift ;;
        --admin)       TARGETS=admin;  shift ;;
        --run-admin|--stop-admin|--admin-url)
                       set_action "$1"; shift ;;
        --admin-port)  ADMIN_PORT="${2:?--admin-port needs a number}"; shift 2 ;;
        -h|--help)     sed -n '2,38p' "$0"; exit 0 ;;
        *)             echo "unknown option: $1" >&2
                       echo "try: $0 --help" >&2; exit 2 ;;
    esac
done

# ---- admin console helpers ---------------------------------------------
admin_pids() { pgrep -x erp-admin 2>/dev/null || true; }

# The token is only ever printed to stdout, so the log IS the record of it.
admin_token() { grep -o 'token=[a-f0-9]\+' "$ADMIN_LOG" 2>/dev/null | tail -1 | cut -d= -f2; }

admin_listening() { ss -tln 2>/dev/null | grep -q "127.0.0.1:${ADMIN_PORT}\b"; }

print_admin_url() {
    local tok; tok=$(admin_token)
    if [ -z "$tok" ]; then
        echo "ERROR: no token found in $ADMIN_LOG." >&2
        echo "       The console prints its token once, at startup. Restart it:" >&2
        echo "         $0 --stop-admin && $0 --run-admin" >&2
        return 1
    fi
    echo
    echo "  ERP Admin Console — open this in your browser:"
    echo
    echo "    http://localhost:${ADMIN_PORT}/?token=${tok}"
    echo
    echo "  The token changes every restart. Without it the page loads but every"
    echo "  panel returns 401. Re-print this URL any time with:"
    echo "    $0 --admin-url"
    echo
}

case "$ACTION" in
  stop-admin)
    pids=$(admin_pids)
    if [ -z "$pids" ]; then echo "Admin console is not running."; exit 0; fi
    echo "Stopping erp-admin: $pids"
    # shellcheck disable=SC2086
    kill $pids 2>/dev/null || true
    for _ in 1 2 3 4 5; do [ -z "$(admin_pids)" ] && break; sleep 1; done
    if [ -n "$(admin_pids)" ]; then
        # shellcheck disable=SC2086
        kill -9 $(admin_pids) 2>/dev/null || true; sleep 1
    fi
    [ -z "$(admin_pids)" ] && echo "Stopped." || { echo "FAILED: still running." >&2; exit 1; }
    exit 0 ;;

  admin-url)
    if [ -z "$(admin_pids)" ]; then
        echo "Admin console is not running. Start it with: $0 --run-admin" >&2
        exit 1
    fi
    print_admin_url
    exit 0 ;;
esac

# ---- build --------------------------------------------------------------
if [ -n "$CLEAN" ]; then
    echo "[build] --clean: removing ./build"
    rm -rf ./build
fi

# Configure only when there is no cache to reuse. `cmake --build` re-runs the
# generator by itself when CMakeLists.txt changes, so an unconditional
# `cmake -B` bought nothing and cost ~65s of configure on every invocation —
# painful for `--run-admin`, whose whole point is getting to the console fast.
# --clean removes ./build, so this still reconfigures when it should.
if [ -f build/CMakeCache.txt ]; then
    echo "[build] reusing existing cmake cache (delete ./build or use --clean to reconfigure)"
else
    echo "[build] configuring (cmake -B ./build) ..."
    cmake -B ./build
fi

build_one() {
    echo "[build] building $1 (-j $JOBS) ..."
    cmake --build ./build --target "$1" -j "$JOBS"
}

# --run-admin only needs the console; building the server too would make the
# common "just let me look at the admin panel" case slow for no reason.
if [ "$ACTION" = run-admin ]; then
    build_one erp-admin
else
    case "$TARGETS" in
        both)   build_one c-erp; build_one erp-admin ;;
        server) build_one c-erp ;;
        admin)  build_one erp-admin ;;
    esac
fi

echo
echo "[build] done:"
rc=0
case "${ACTION}:${TARGETS}" in
    run-admin:*) want="build/erp-admin" ;;
    *:server)    want="build/c-erp" ;;
    *:admin)     want="build/erp-admin" ;;
    *)           want="build/c-erp build/erp-admin" ;;
esac
for b in $want; do
    if [ -x "$b" ]; then
        printf "  %-18s %s\n" "$b" "$(du -h "$b" 2>/dev/null | cut -f1)"
    else
        echo "  MISSING: $b"; rc=1
    fi
done
[ "$rc" -eq 0 ] || exit $rc

# ---- start the console, if that is what was asked for -------------------
if [ "$ACTION" = run-admin ]; then
    echo
    if [ -n "$(admin_pids)" ]; then
        echo "[admin] already running (pid $(admin_pids | tr '\n' ' ')) — reusing it."
        echo "        Use '$0 --stop-admin' first if you want a fresh token."
        print_admin_url
        exit 0
    fi

    mkdir -p "$(dirname "$ADMIN_LOG")"
    echo "[admin] starting on 127.0.0.1:${ADMIN_PORT} (log: $ADMIN_LOG) ..."
    # setsid + detached: the console must outlive this script, and its stdout
    # is the only place the token is ever written.
    ( setsid ./build/erp-admin --port "$ADMIN_PORT" > "$ADMIN_LOG" 2>&1 < /dev/null & )

    # Poll for the port rather than sleeping a guessed amount.
    for _ in $(seq 1 20); do admin_listening && break; sleep 1; done
    if ! admin_listening; then
        echo "FAILED: nothing listening on 127.0.0.1:${ADMIN_PORT} after 20s." >&2
        echo "--- $ADMIN_LOG ---" >&2
        tail -20 "$ADMIN_LOG" >&2 2>/dev/null || true
        exit 1
    fi
    print_admin_url
fi
exit 0
