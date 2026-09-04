#!/bin/bash
# =============================================================
# scripts/deploy.sh — build the binaries in Docker, ship them to the host.
#
# WHY WE CROSS-BUILD. easylockerspace has 2 cores and 964 MiB of RAM, backed by
# a 6 GiB swapfile. Measured 2026-09-01: the largest single translation unit
# peaks at 1604 MiB — more than the host's entire RAM — so the server cannot
# compile that file at any parallelism, only page it through swap. It is not
# OOM-killed (the swap sees to that); it just takes hours. The same build here
# takes 667 s on 8 cores. Dockerfile.build reproduces the host's toolchain
# exactly (Debian 13 / glibc 2.41 / gcc 14.2) so the binary produced here runs
# there — verified: identical GLIBC 2.38 / GLIBCXX 3.4.32 and the same 28
# shared libraries as the binary already running on the host.
#
# USAGE
#   ./scripts/deploy.sh                 build both inside Docker & deploy
#   ./scripts/deploy.sh --server        build & deploy c-erp only
#   ./scripts/deploy.sh --admin         build & deploy erp-admin only
#   ./scripts/deploy.sh --clean         wipe ./build first, rebuild, deploy
#   ./scripts/deploy.sh --restart       sync, then restart c-erp via server.sh
#   ./scripts/deploy.sh --status        show remote status after deploy
#   ./scripts/deploy.sh --host HOST     override SSH host (default: easylockerspace)
#   ./scripts/deploy.sh --rebuild-image force a rebuild of the builder image
#   ./scripts/deploy.sh --no-abi-check  ship even if the ABI check objects
#   ./scripts/deploy.sh --dry-run       build and check, but do not ship
#
# All build flags (--server, --admin, --clean, -j N) go through to build.sh.
# =============================================================
set -euo pipefail
cd "$(dirname "$0")/.."

REMOTE_HOST="${GCP_HOST:-easylockerspace}"
REMOTE_DIR="${GCP_DIR:-~/code/c-erp}"
DOCKER_IMAGE="${DOCKER_IMAGE:-gcp-builder}"
# The cross-build gets its OWN directory.
#
# It used to build into ./build, which tests/run.sh also owns. Whichever ran
# last left its cmake cache there and broke the other: run.sh would die with
#   "The current CMakeCache.txt directory ... is different than /workspace/build"
# and report `erp_tests (build)` as a test failure that had nothing to do with
# any test. That cost three full suite runs before it was worth fixing.
BUILD_DIR="${BUILD_DIR:-build-docker}"
CCACHE_DIR="${CCACHE_DIR:-$HOME/.cache/cerp-ccache}"

RESTART_SERVICE=0
CHECK_STATUS=0
REBUILD_IMAGE=0
ABI_CHECK=1
DRY_RUN=0
BUILD_ARGS=()

while [ $# -gt 0 ]; do
    case "$1" in
        --host)          REMOTE_HOST="$2"; shift 2 ;;
        --dir)           REMOTE_DIR="$2"; shift 2 ;;
        --restart)       RESTART_SERVICE=1; shift ;;
        --status)        CHECK_STATUS=1; shift ;;
        --rebuild-image) REBUILD_IMAGE=1; shift ;;
        --no-abi-check)  ABI_CHECK=0; shift ;;
        --dry-run)       DRY_RUN=1; shift ;;
        -h|--help)
            sed -n '2,23p' "$0" | sed 's/^# \{0,1\}//'
            exit 0
            ;;
        *)
            BUILD_ARGS+=("$1")
            shift
            ;;
    esac
done

# -------------------------------------------------------------
# Step 0 — the builder image must exist, and be newer than the Dockerfile.
#
# Skipping this is how a stale image survives a Dockerfile fix: the build then
# fails for a reason that was corrected days ago, or worse succeeds against the
# wrong library set and produces a binary that dies on the host.
# -------------------------------------------------------------
# Can we talk to the daemon at all? Ask before interpreting any docker output.
#
# `docker image inspect` fails the same way whether the image is missing or the
# socket is unreadable, and swallowing the reason turned "permission denied"
# into "image not found — building it". The script then tried to BUILD, which
# failed on the same permission error one step later behind a much noisier
# message. Diagnose the cause once, here, and say what to do about it.
if ! docker_err=$(docker version --format '{{.Server.Version}}' 2>&1); then
    echo "ERROR: cannot talk to the Docker daemon." >&2
    echo "  $docker_err" | head -2 >&2
    if printf '%s' "$docker_err" | grep -qi 'permission denied'; then
        echo >&2
        echo "  You ARE in the docker group ($(getent group docker 2>/dev/null || echo 'group missing'))," >&2
        echo "  but a session only picks up group membership when it is CREATED." >&2
        echo "  A WSL login started before the group was added keeps the old" >&2
        echo "  credentials, and new terminal tabs re-attach to that same session." >&2
        echo >&2
        echo "  Fix, most reliable first:" >&2
        echo "    wsl --shutdown      (from Windows PowerShell), then reopen the terminal" >&2
        echo "    newgrp docker       (this shell only, no restart)" >&2
    fi
    exit 1
fi

image_created=$(docker image inspect "$DOCKER_IMAGE" --format '{{.Created}}' 2>/dev/null || true)

if [ -z "$image_created" ]; then
    echo "[deploy] builder image '$DOCKER_IMAGE' not found — building it."
    REBUILD_IMAGE=1
elif [ "$REBUILD_IMAGE" -eq 0 ]; then
    img_epoch=$(date -d "$image_created" +%s 2>/dev/null || echo 0)
    dkf_epoch=$(stat -c %Y Dockerfile.build 2>/dev/null || echo 0)
    if [ "$dkf_epoch" -gt "$img_epoch" ]; then
        echo "[deploy] Dockerfile.build is newer than the image — rebuilding it."
        REBUILD_IMAGE=1
    fi
fi

if [ "$REBUILD_IMAGE" -eq 1 ]; then
    echo "======================================================="
    echo "[deploy] Step 0: Building the builder image ($DOCKER_IMAGE)..."
    echo "======================================================="
    docker build -f Dockerfile.build -t "$DOCKER_IMAGE" .
fi

echo "======================================================="
echo "[deploy] Step 1: Compiling inside Docker container ($DOCKER_IMAGE)..."
echo "======================================================="

# Run as the host uid:gid so ./build stays yours, not root's. The image sets
# HOME=/tmp because that uid has no passwd entry, and both git (for the
# vendored submodules) and cmake insist on a writable home.
mkdir -p "$CCACHE_DIR"
docker run --rm \
    -u "$(id -u):$(id -g)" \
    -v "$(pwd):/workspace" \
    -v "$CCACHE_DIR:/tmp/ccache" \
    "$DOCKER_IMAGE" \
    ./scripts/build.sh "${BUILD_ARGS[@]}"

# Locate built binaries in local ./build/
BINARIES=()
[ -f "./$BUILD_DIR/c-erp" ]     && BINARIES+=("./$BUILD_DIR/c-erp")
[ -f "./$BUILD_DIR/erp-admin" ] && BINARIES+=("./$BUILD_DIR/erp-admin")

if [ ${#BINARIES[@]} -eq 0 ]; then
    echo "ERROR: No compiled binaries found in ./build to deploy." >&2
    exit 1
fi

# -------------------------------------------------------------
# Step 2 — the ABI guard.
#
# This is the whole reason the build box has to match the host. A binary linked
# against a newer glibc fails at exec with
#     /lib/.../libc.so.6: version `GLIBC_2.4x' not found
# which reads like a corrupt upload rather than a toolchain mismatch. Check it
# BEFORE the binary lands, so a bad build never replaces a working one.
#
# Compares the highest symbol version each binary REQUIRES against the highest
# the host PROVIDES, read live over ssh rather than hardcoded here — a number
# baked into this file would rot the next time the host is upgraded.
# -------------------------------------------------------------
if [ "$ABI_CHECK" -eq 1 ]; then
    echo
    echo "======================================================="
    echo "[deploy] Step 2: ABI check against $REMOTE_HOST..."
    echo "======================================================="

    host_abi=$(ssh "$REMOTE_HOST" 'bash -s' <<'PROBE'
libc=$(ldd --version | head -1 | grep -oE '[0-9]+\.[0-9]+$')
so=$(/sbin/ldconfig -p | grep -m1 'libstdc++\.so\.6 ' | awk '{print $NF}')
cxx=$(strings -a "$so" | grep -oE 'GLIBCXX_[0-9.]+' | sed 's/GLIBCXX_//' | sort -V | tail -1)
echo "$libc $cxx"
PROBE
    ) || { echo "ERROR: could not read the host ABI over ssh." >&2; exit 1; }

    host_glibc=${host_abi%% *}
    host_glibcxx=${host_abi##* }
    echo "  host provides:  glibc $host_glibc, GLIBCXX $host_glibcxx"

    newest() { printf '%s\n' "$@" | sort -V | tail -1; }
    abi_fail=0

    for b in "${BINARIES[@]}"; do
        need_glibc=$(objdump -T "$b" 2>/dev/null | grep -oE 'GLIBC_[0-9.]+' | sed 's/GLIBC_//' | sort -V | tail -1)
        need_cxx=$(objdump -T "$b" 2>/dev/null | grep -oE 'GLIBCXX_[0-9.]+' | sed 's/GLIBCXX_//' | sort -V | tail -1)
        printf '  %-20s needs glibc %-8s GLIBCXX %s\n' \
               "$(basename "$b")" "${need_glibc:-none}" "${need_cxx:-none}"

        if [ -n "$need_glibc" ] && [ "$(newest "$need_glibc" "$host_glibc")" != "$host_glibc" ]; then
            echo "    FAIL  needs glibc $need_glibc, host provides $host_glibc" >&2
            abi_fail=1
        fi
        if [ -n "$need_cxx" ] && [ "$(newest "$need_cxx" "$host_glibcxx")" != "$host_glibcxx" ]; then
            echo "    FAIL  needs GLIBCXX $need_cxx, host provides $host_glibcxx" >&2
            abi_fail=1
        fi
    done

    if [ "$abi_fail" -eq 1 ]; then
        echo >&2
        echo "ERROR: this binary would not start on $REMOTE_HOST — refusing to ship." >&2
        echo "       The builder image has drifted from the host. Re-measure:" >&2
        echo "         ssh $REMOTE_HOST 'ldd --version; gcc --version; cat /etc/os-release'" >&2
        echo "       align Dockerfile.build's base image to it, then --rebuild-image." >&2
        echo "       --no-abi-check overrides this, if you know why." >&2
        exit 1
    fi
    echo "  ok — both binaries stay within what the host provides."
fi

# -------------------------------------------------------------
# Step 3 — source drift warning.
#
# This script ships BINARIES ONLY. web/static/ (the OWL frontend), config/ and
# db/ reach the host by `git pull`, so a deploy carrying frontend changes is
# only half done here. Say so rather than let it puzzle you later: the host has
# already run an Aug-09 binary against Aug-31 assets.
# -------------------------------------------------------------
local_rev=$(git rev-parse --short HEAD 2>/dev/null || echo unknown)
remote_rev=$(ssh "$REMOTE_HOST" "cd $REMOTE_DIR 2>/dev/null && git rev-parse --short HEAD" 2>/dev/null || echo unknown)
if [ "$local_rev" != "$remote_rev" ]; then
    echo
    echo "  NOTE  source revision differs — local $local_rev, remote $remote_rev."
    echo "        This script ships binaries only; web/static, config and db come"
    echo "        from git on the host. If this deploy includes frontend changes:"
    echo "            ssh $REMOTE_HOST 'cd $REMOTE_DIR && git pull'"
fi

# Matching revisions are NOT enough. A dirty working tree compiles uncommitted
# backend changes into the binary while the host, which only ever sees commits,
# keeps serving the committed frontend. Both HEADs then read identical and the
# check above says nothing — yet the two halves are paired with different code.
# This is the case that actually bites, so name the files.
dirty_src=$(git status --porcelain -- modules core main.cpp 2>/dev/null | wc -l)
dirty_web=$(git status --porcelain -- web 2>/dev/null | wc -l)
if [ "${dirty_src:-0}" -gt 0 ] || [ "${dirty_web:-0}" -gt 0 ]; then
    echo
    echo "  WARNING  the working tree is dirty — this binary contains code that is"
    echo "           in no commit ($dirty_src backend file(s) changed)."
    if [ "${dirty_web:-0}" -gt 0 ]; then
        echo "           $dirty_web uncommitted web/ file(s) will NOT reach the host,"
        echo "           because it gets web/static by git pull. The deployed backend"
        echo "           and the served frontend are therefore built from DIFFERENT"
        echo "           source. Commit and push, then pull on the host:"
        git status --porcelain -- web 2>/dev/null | sed 's/^/             /'
    fi
    echo "           Deploying from a clean, pushed tree avoids this entirely."
fi

if [ "$DRY_RUN" -eq 1 ]; then
    echo
    echo "[deploy] --dry-run: built and checked, nothing shipped."
    exit 0
fi

echo
echo "======================================================="
echo "[deploy] Step 4: Deploying binaries to GCP ($REMOTE_HOST)..."
echo "======================================================="

ssh "$REMOTE_HOST" "mkdir -p $REMOTE_DIR/build"

# rsync needs to exist on BOTH ends. A fresh Debian host has no rsync, and the
# failure is opaque — the local rsync reports "connection unexpectedly closed"
# and exits 127, which reads like a network fault rather than a missing package.
# Check first, and fall back to scp, which only needs sshd.
if ssh "$REMOTE_HOST" 'command -v rsync' >/dev/null 2>&1; then
    rsync -avzP "${BINARIES[@]}" "$REMOTE_HOST:$REMOTE_DIR/build/"
else
    echo "  NOTE  no rsync on $REMOTE_HOST — falling back to scp."
    echo "        (install it there for resumable, delta transfers: sudo apt-get install rsync)"
    scp "${BINARIES[@]}" "$REMOTE_HOST:$REMOTE_DIR/build/"
fi

ssh "$REMOTE_HOST" "chmod +x $REMOTE_DIR/build/* 2>/dev/null || true"

if [ "$RESTART_SERVICE" -eq 1 ]; then
    echo
    echo "======================================================="
    echo "[deploy] Step 5: Restarting remote c-erp service via server.sh..."
    echo "======================================================="
    ssh "$REMOTE_HOST" "cd $REMOTE_DIR && ./scripts/server.sh --restart"
fi

if [ "$CHECK_STATUS" -eq 1 ]; then
    echo
    echo "======================================================="
    echo "[deploy] Remote Server Status:"
    echo "======================================================="
    ssh "$REMOTE_HOST" "cd $REMOTE_DIR && ./scripts/server.sh --status"
fi

echo
echo "[deploy] Deployment complete."
