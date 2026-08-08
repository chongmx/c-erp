#!/bin/bash
# =============================================================
# server.sh — control the c-erp systemd service.
#
# Unit: /etc/systemd/system/c-erp.service
#   User=chongmx, WorkingDirectory=<project root>, Restart=on-failure
#
# Usage:
#   ./scripts/server.sh --install    create the systemd unit (once)
#   ./scripts/server.sh --start      start now
#   ./scripts/server.sh --stop       stop now, INCLUDING a stray process
#   ./scripts/server.sh --restart    restart now (also clears a failed state)
#   ./scripts/server.sh --enable     start automatically at boot
#   ./scripts/server.sh --disable    do not start at boot
#   ./scripts/server.sh --status     what is it doing right now
#   ./scripts/server.sh --logs [-f]  recent logs; -f to follow live
#
# --enable/--disable only change boot behaviour. They do NOT start or stop
# a running service, which is systemd's own semantics and is deliberate:
# you can disable at boot without dropping your users mid-session.
#
# STRAY PROCESSES. A c-erp started by hand — `./build/c-erp &`, or a
# setsid/nohup that outlived its shell — is invisible to systemd, so
# `systemctl stop` reports success while the port stays bound and the old
# binary keeps serving. That is a genuinely confusing state, so --stop
# kills strays too and --status reports them loudly rather than showing a
# tidy "inactive" beside a listening port.
#
# Every state-changing action is verified afterwards rather than assumed —
# systemctl returning 0 only means the request was accepted.
#
# Override the unit name with CERP_SERVICE=other-name if you run more than
# one tenant on this host.
# =============================================================
set -euo pipefail

SERVICE=${CERP_SERVICE:-c-erp}
ACTION=

while [ $# -gt 0 ]; do
    case "$1" in
        --start|--stop|--restart|--enable|--disable|--status|--install)
            [ -z "$ACTION" ] || { echo "ERROR: pick one action, got --${ACTION} and $1" >&2; exit 2; }
            ACTION=${1#--}; shift ;;
        --reset)
            # Reserved. Caught explicitly so it fails loudly instead of falling
            # through to "Unknown argument", and so it can never be mistaken
            # for --restart once it does mean something.
            echo "ERROR: --reset is reserved and not implemented yet." >&2
            echo "       To restart the service, use --restart." >&2
            exit 2 ;;
        --logs)
            ACTION=logs; shift
            FOLLOW=
            if [ "${1:-}" = "-f" ]; then FOLLOW=1; shift; fi ;;
        -h|--help) sed -n '2,34p' "$0"; exit 0 ;;
        *) echo "Unknown argument: $1" >&2; exit 2 ;;
    esac
done

[ -n "$ACTION" ] || { sed -n '2,34p' "$0"; exit 2; }

command -v systemctl >/dev/null || { echo "ERROR: systemctl not found — this script is for the deployed server." >&2; exit 1; }

# A missing unit is fatal only for the actions that need systemd. --stop,
# --status and --install must still work without one: "there is no unit
# but something IS listening" is precisely the situation --stop exists to
# resolve, and refusing to run leaves the user with no way out.
unit_exists() { systemctl cat "$SERVICE" >/dev/null 2>&1; }
case "$ACTION" in
  start|restart|enable|disable)
    if ! unit_exists; then
        echo "ERROR: no systemd unit named '$SERVICE'." >&2
        echo "       Run: $0 --install" >&2
        exit 1
    fi ;;
esac

# ---- port, so we can confirm it is really serving ----------------------
here=$(cd "$(dirname "$0")/.." && pwd)
CFG="$here/config/system.cfg"
PORT=8069
if [ -f "$CFG" ]; then
    p=$(sed -nE 's/^[[:space:]]*http_port[[:space:]]*=[[:space:]]*([0-9]+).*/\1/p' "$CFG" | head -1)
    [ -n "$p" ] && PORT=$p
fi

is_active()  { [ "$(systemctl is-active  "$SERVICE" 2>/dev/null || true)" = active ]; }
is_enabled() { [ "$(systemctl is-enabled "$SERVICE" 2>/dev/null || true)" = enabled ]; }
listening()  { ss -tln 2>/dev/null | grep -q ":${PORT}\b"; }

main_pid()   { systemctl show -p MainPID --value "$SERVICE" 2>/dev/null || echo 0; }

# Every c-erp process that systemd does NOT own.
#
# pgrep -x matches the executable name exactly. Never `pgrep -f c-erp`
# here: that pattern also matches this script's own command line, and a
# script that kills itself reports a mysterious exit 143.
strays() {
    local owned; owned=$(main_pid)
    local p out=
    for p in $(pgrep -x c-erp 2>/dev/null || true); do
        [ "$p" = "$owned" ] && continue
        out="$out $p"
    done
    echo "${out# }"
}

# Poll rather than sleep a fixed amount: binding is not instantaneous, and a
# fixed sleep is either too slow or occasionally too short.
wait_for() {                     # wait_for <up|down> <seconds>
    local want=$1 limit=$2 i=0
    while [ "$i" -lt "$limit" ]; do
        if [ "$want" = up ]   && is_active && listening; then return 0; fi
        if [ "$want" = down ] && ! is_active && ! listening; then return 0; fi
        sleep 1; i=$((i + 1))
    done
    return 1
}

report() {
    # `systemctl is-active/is-enabled` PRINT the state and ALSO exit non-zero
    # when it is not active/enabled. A `|| echo default` fallback therefore
    # appends a second line instead of substituting. Capture, then fall back
    # only when the output was genuinely empty.
    local act ena
    act=$(systemctl is-active  "$SERVICE" 2>/dev/null) || true
    ena=$(systemctl is-enabled "$SERVICE" 2>/dev/null) || true
    [ -n "$act" ] || act=inactive
    [ -n "$ena" ] || ena=disabled
    printf 'service  : %s\n' "$SERVICE"
    printf 'active   : %s\n' "$act"
    printf 'at boot  : %s\n' "$ena"
    printf 'port %-5s: %s\n' "$PORT" "$(listening && echo listening || echo 'not listening')"
    local pid; pid=$(main_pid)
    [ "${pid:-0}" != 0 ] && printf 'main pid : %s\n' "$pid"
    local s; s=$(strays)
    if [ -n "$s" ]; then
        printf 'STRAY    : %s  (started by hand, not managed by systemd)\n' "$s"
        printf '           `%s --stop` will end these too.\n' "$0"
    fi
}

case "$ACTION" in
  start)
    if is_active; then echo "Already running."; report; exit 0; fi
    sudo systemctl start "$SERVICE"
    if wait_for up 20; then echo "Started."; report
    else echo "FAILED: did not come up within 20s." >&2; report
         sudo systemctl status "$SERVICE" --no-pager -l | head -20 >&2; exit 1; fi
    ;;

  stop)
    stopped_something=
    if unit_exists && is_active; then
        sudo systemctl stop "$SERVICE"
        stopped_something=1
    fi

    # Then the strays. TERM first so the process can close its DB pool and
    # its listening socket cleanly; KILL only for anything still standing.
    s=$(strays)
    if [ -n "$s" ]; then
        echo "Stopping stray process(es): $s"
        # shellcheck disable=SC2086
        kill $s 2>/dev/null || true
        for _ in 1 2 3 4 5 6 7 8 9 10; do
            [ -z "$(strays)" ] && break
            sleep 1
        done
        s=$(strays)
        if [ -n "$s" ]; then
            echo "Still up after SIGTERM; sending SIGKILL to: $s"
            # shellcheck disable=SC2086
            kill -9 $s 2>/dev/null || true
            sleep 1
        fi
        stopped_something=1
    fi

    if [ -z "$stopped_something" ]; then echo "Already stopped."; report; exit 0; fi

    # The PORT is the real verdict, not systemd's opinion: something else
    # holding it means the stop did not achieve what the user asked for.
    if wait_for down 20 && [ -z "$(strays)" ]; then
        echo "Stopped."; report
    else
        echo "FAILED: something is still running or holding port ${PORT}." >&2
        report
        ss -tlnp 2>/dev/null | grep ":${PORT}\b" >&2 || true
        exit 1
    fi
    ;;

  install)
    UNIT=/etc/systemd/system/${SERVICE}.service
    if [ -e "$UNIT" ]; then
        echo "Unit already exists: $UNIT"
        echo "Edit it by hand, or remove it first if you want it regenerated."
        exit 0
    fi
    [ -x "$here/build/c-erp" ] || {
        echo "ERROR: $here/build/c-erp not found or not executable." >&2
        echo "       Build first: cmake --build ./build" >&2
        exit 1; }

    echo "Writing $UNIT"
    # WorkingDirectory matters: main.cpp resolves config/system.cfg
    # relative to the current directory, so a unit without it starts a
    # server with no configuration and fails in a confusing way.
    sudo tee "$UNIT" >/dev/null <<UNITEOF
[Unit]
Description=c-erp application server
Documentation=file://$here/CLAUDE.md
After=network-online.target postgresql.service
Wants=network-online.target

[Service]
Type=simple
User=$(id -un)
Group=$(id -gn)
WorkingDirectory=$here
ExecStart=$here/build/c-erp
Restart=on-failure
RestartSec=5
# Give a slow first boot room: the migration runner can take a while on a
# fresh database, and being killed part-way through is the worst moment.
TimeoutStartSec=120
KillSignal=SIGTERM
TimeoutStopSec=30

[Install]
WantedBy=multi-user.target
UNITEOF

    sudo systemctl daemon-reload
    if unit_exists; then
        echo "Installed. Next:"
        echo "  $0 --start      run it now"
        echo "  $0 --enable     also start it at boot"
        report
    else
        echo "FAILED: systemd still does not see '$SERVICE'." >&2
        exit 1
    fi
    ;;

  restart)
    # reset-failed first: after repeated crashes systemd rate-limits restarts
    # and a plain `restart` is refused until the failure counter is cleared.
    sudo systemctl reset-failed "$SERVICE" 2>/dev/null || true
    sudo systemctl restart "$SERVICE"
    if wait_for up 25; then echo "Restarted."; report
    else echo "FAILED: did not come back within 25s." >&2; report
         sudo systemctl status "$SERVICE" --no-pager -l | head -20 >&2; exit 1; fi
    ;;

  enable)
    sudo systemctl enable "$SERVICE"
    if is_enabled; then echo "Enabled at boot (current running state unchanged)."; report
    else echo "FAILED: still reports $(systemctl is-enabled "$SERVICE" 2>&1)." >&2; exit 1; fi
    ;;

  disable)
    sudo systemctl disable "$SERVICE"
    if ! is_enabled; then echo "Disabled at boot (current running state unchanged)."; report
    else echo "FAILED: still enabled." >&2; exit 1; fi
    ;;

  status)
    report; echo
    if unit_exists; then
        sudo systemctl status "$SERVICE" --no-pager -l | head -20
    else
        echo "(no systemd unit installed — run: $0 --install)"
    fi
    ;;

  logs)
    if [ -n "${FOLLOW:-}" ]; then
        echo "Following ${SERVICE} (Ctrl-C to stop). App log: $here/log/system.log"
        sudo journalctl -u "$SERVICE" -f
    else
        sudo journalctl -u "$SERVICE" -n 50 --no-pager
        echo
        echo "--- tail of log/system.log ---"
        tail -20 "$here/log/system.log" 2>/dev/null || echo "(no log/system.log yet)"
    fi
    ;;
esac
