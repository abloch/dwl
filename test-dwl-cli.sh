#!/bin/bash
# Test suite for dwl-cli IPC commands.
#
# Starts a headless dwl compositor, exercises every dwl-cli command, and
# checks the resulting JSON state.  Output is TAP format so it works with
# prove(1), tap-runner, or any CI that understands TAP.
#
# Usage:
#   ./test-dwl-cli.sh                  # uses ./dwl and ./dwl-cli
#   DWL=/path/dwl DWLCLI=/path/dwl-cli ./test-dwl-cli.sh
#
# Requirements: WLR_BACKENDS=headless + WLR_RENDERER=pixman support in dwl.

set -uo pipefail

DWL=${DWL:-./dwl}
DWLCLI=${DWLCLI:-./dwl-cli}
SOCK=""
LOG="/tmp/dwl-test-$$.log"
PASS=0
FAIL=0
TEST_NUM=0
DWL_PID=""

die() { echo "FATAL: $*" >&2; exit 1; }

# ── Compositor lifecycle ────────────────────────────────────────────────────

start_compositor() {
    local rt="${XDG_RUNTIME_DIR:-/run/user/$(id -u)}"
    # Snapshot existing sockets so we can detect the new one.
    local before
    before=$(ls "$rt"/wayland-* 2>/dev/null | grep -v '\.lock$' | sort || true)

    WLR_BACKENDS=headless WLR_RENDERER=pixman \
        "$DWL" >"$LOG" 2>&1 &
    DWL_PID=$!

    # Wait for a new socket to appear in XDG_RUNTIME_DIR.
    for _ in $(seq 30); do
        local after new
        after=$(ls "$rt"/wayland-* 2>/dev/null | grep -v '\.lock$' | sort || true)
        new=$(comm -13 <(echo "$before") <(echo "$after") | head -1)
        if [ -n "$new" ] && [ -S "$new" ]; then
            SOCK=$(basename "$new")
            return 0
        fi
        sleep 0.15
    done
    die "compositor socket never appeared; see $LOG"
}

stop_compositor() {
    [ -n "$DWL_PID" ] && kill "$DWL_PID" 2>/dev/null || true
    rm -f "$LOG"
}

trap stop_compositor EXIT

# ── TAP helpers ─────────────────────────────────────────────────────────────

ok() {
    TEST_NUM=$((TEST_NUM + 1))
    echo "ok $TEST_NUM - $1"
    PASS=$((PASS + 1))
}

nok() {
    TEST_NUM=$((TEST_NUM + 1))
    echo "not ok $TEST_NUM - $1"
    echo "  # expected: $2"
    echo "  # got:      $3"
    FAIL=$((FAIL + 1))
}

check() {
    local desc="$1" expected="$2" actual="$3"
    if [ "$expected" = "$actual" ]; then
        ok "$desc"
    else
        nok "$desc" "$expected" "$actual"
    fi
}

check_contains() {
    local desc="$1" pattern="$2" actual="$3"
    if echo "$actual" | grep -qF "$pattern"; then
        ok "$desc"
    else
        nok "$desc" "output containing '$pattern'" "$actual"
    fi
}

check_exit_nonzero() {
    local desc="$1"
    shift
    if ! WAYLAND_DISPLAY=$SOCK "$DWLCLI" "$@" >/dev/null 2>&1; then
        ok "$desc"
    else
        nok "$desc" "non-zero exit" "exit 0"
    fi
}

# ── dwl-cli helpers ─────────────────────────────────────────────────────────

cli() {
    WAYLAND_DISPLAY=$SOCK "$DWLCLI" "$@" 2>/dev/null
}

cli_stderr() {
    WAYLAND_DISPLAY=$SOCK "$DWLCLI" "$@" 2>&1
}

# Extract fields from a fresh status snapshot.
status_json()    { cli status; }
tag_count()      { status_json | python3 -c "import sys,json; print(json.load(sys.stdin)['manager']['tag_count'])"; }
layouts_list()   { status_json | python3 -c "import sys,json; print(','.join(json.load(sys.stdin)['manager']['layouts']))"; }
active_tags()    { status_json | python3 -c "
import sys,json
o=json.load(sys.stdin)['outputs'][0]
print(','.join(str(t['index']) for t in o['tags'] if t['state'] & 1))"; }
layout_index()   { status_json | python3 -c "import sys,json; print(json.load(sys.stdin)['outputs'][0]['layout']['index'])"; }
layout_symbol()  { status_json | python3 -c "import sys,json; print(json.load(sys.stdin)['outputs'][0]['layout']['symbol'])"; }
output_name()    { status_json | python3 -c "import sys,json; print(json.load(sys.stdin)['outputs'][0]['name'])"; }

# ── Setup ───────────────────────────────────────────────────────────────────

[ -x "$DWL" ]    || die "$DWL not found or not executable"
[ -x "$DWLCLI" ] || die "$DWLCLI not found or not executable"

start_compositor
echo "TAP version 13"
echo "# compositor started on WAYLAND_DISPLAY=$SOCK"

# ── Manager metadata ────────────────────────────────────────────────────────

check "manager: tag_count is 9"                "9"           "$(tag_count)"
check "manager: layouts list matches config"   "[]=,><>,[M]" "$(layouts_list)"

# ── Initial compositor state ─────────────────────────────────────────────────

check "initial: tag index 1 is active"         "1"   "$(active_tags)"
check "initial: layout index is 0"             "0"   "$(layout_index)"
check "initial: layout symbol is []="          "[]=" "$(layout_symbol)"
check "initial: output is HEADLESS-1"          "HEADLESS-1" "$(output_name)"

# ── status JSON structure ────────────────────────────────────────────────────

check_contains "status: has 'manager' object"   '"tag_count"'   "$(status_json)"
check_contains "status: has 'outputs' array"    '"HEADLESS-1"'  "$(status_json)"
check_contains "status: has 'toplevels' array"  '"toplevels"'   "$(status_json)"

# ── view: tag mask syntax ────────────────────────────────────────────────────
# parse_tagmask accepts decimal, 0x hex, t<N> (1-indexed), and comma lists.

cli view t3
check "view t3: activates tag index 3"              "3" "$(active_tags)"

cli view t1
check "view t1: activates tag index 1"              "1" "$(active_tags)"

cli view 4           # 1-indexed tag 4 = 1<<3 = tag index 4
check "view 4 (decimal): activates tag index 4"     "4" "$(active_tags)"

cli view 0x1         # hex bitmask 1 = 1<<0 = tag index 1
check "view 0x1 (hex): activates tag index 1"       "1" "$(active_tags)"

cli view t9          # 1<<8 = tag index 9
check "view t9: activates last tag (index 9)"       "9" "$(active_tags)"

cli view 1,3         # comma list: tags 1 and 3 simultaneously
check "view 1,3 (comma list): activates tags 1 and 3"   "1,3" "$(active_tags)"

cli view 0x5         # hex bitmask 0x5 = bits 0,2 = tags 1 and 3
check "view 0x5 (hex multi-tag): activates tags 1 and 3" "1,3" "$(active_tags)"

# ── view: invalid masks ───────────────────────────────────────────────────────

cli view t3          # set known baseline
check_contains "view invalid mask: prints error"    \
    "invalid tagmask"  "$(cli_stderr view notamask)"
check "view invalid mask: state unchanged"          "3" "$(active_tags)"
check_contains "view invalid comma (tag 0): prints error" \
    "invalid tagmask"  "$(cli_stderr view 1,0)"
check_contains "view invalid comma (tag 32): prints error" \
    "invalid tagmask"  "$(cli_stderr view 1,32)"
check "view invalid comma list: state unchanged"    "3" "$(active_tags)"

# ── view-toggle: alternate tagset semantics ──────────────────────────────────
# view-toggle switches to the alt tagset and sets it to the given mask.
# A repeated call with the same mask hits the early-return guard and is a no-op.

cli view t3
cli view-toggle t1      # alt tagset ← mask 1 (index 1), seltags flips
check "view-toggle t1 from t3: alt tagset has index 1" "1" "$(active_tags)"

cli view-toggle t1      # same mask == current tagset → early return, no change
check "view-toggle same mask twice: is a no-op"         "1" "$(active_tags)"

cli view t3             # restore via direct view (no seltags toggle)
check "view t3 after toggle: tag index 3 is active"     "3" "$(active_tags)"

cli view-toggle t3      # toggle back to alt tagset; mask == current → no-op
check "view-toggle t3 when already on t3: no-op"        "3" "$(active_tags)"

# ── layout cycling ────────────────────────────────────────────────────────────

cli layout 1
check "layout 1: index is 1"       "1"    "$(layout_index)"
check "layout 1: symbol is ><>"    "><>"  "$(layout_symbol)"

cli layout 2
check "layout 2: index is 2"       "2"    "$(layout_index)"
check "layout 2: symbol is [M]"    "[M]"  "$(layout_symbol)"

cli layout 0
check "layout 0: index restored to 0"    "0"    "$(layout_index)"
check "layout 0: symbol restored to []=" "[]="  "$(layout_symbol)"

# Out-of-range index: compositor silently ignores it (no protocol error).
LAYOUT_BEFORE=$(layout_index)
cli layout 99 2>/dev/null || true
check "layout 99 (out of range): layout unchanged"  "$LAYOUT_BEFORE" "$(layout_index)"

# ── layout: sequential round-trip ────────────────────────────────────────────
# Cycle through all three layouts and back to verify each transition is clean.

for IDX in 0 1 2 0; do
    cli layout "$IDX"
    check "layout cycle: index $IDX" "$IDX" "$(layout_index)"
done

# ── --output flag ─────────────────────────────────────────────────────────────

check "--output HEADLESS-1 status: correct output name" \
    "HEADLESS-1" \
    "$(cli --output HEADLESS-1 status | python3 -c "import sys,json; print(json.load(sys.stdin)['outputs'][0]['name'])")"

check_exit_nonzero "--output UNKNOWN: exits non-zero" \
    --output UNKNOWN view t1

# ── focus: unknown identifier is silently ignored ─────────────────────────────
# No client is connected, so every identifier is unknown.  The compositor
# iterates the client list, finds nothing, and returns without error.

cli view t1              # set known baseline
cli focus "deadbeef00000000000000000000000000" 2>/dev/null || true
check "focus unknown id: state unchanged" "1" "$(active_tags)"

# ── urgent: with no client, silently ignored ──────────────────────────────────

cli urgent "deadbeef00000000000000000000000000" 1 2>/dev/null || true
ok "urgent with unknown id: completes without crash"

# ── client-tags: bogus id is silently ignored ─────────────────────────────────

cli client-tags set    "deadbeef00000000000000000000000000" t1    2>/dev/null || true
cli client-tags add    "deadbeef00000000000000000000000000" t1    2>/dev/null || true
cli client-tags toggle "deadbeef00000000000000000000000000" t1    2>/dev/null || true
cli client-tags remove "deadbeef00000000000000000000000000" t1    2>/dev/null || true
ok "client-tags (set/add/toggle/remove) with unknown id: completes without crash"

# Comma-list syntax accepted by all client-tags subcommands.
cli client-tags set    "deadbeef00000000000000000000000000" 1,3   2>/dev/null || true
cli client-tags add    "deadbeef00000000000000000000000000" 1,3   2>/dev/null || true
cli client-tags toggle "deadbeef00000000000000000000000000" 1,3   2>/dev/null || true
cli client-tags remove "deadbeef00000000000000000000000000" 1,3   2>/dev/null || true
ok "client-tags accepts comma-list tag syntax: completes without crash"

# ── Error / usage cases ───────────────────────────────────────────────────────

check_exit_nonzero "no command: exits non-zero"
check_exit_nonzero "unknown command: exits non-zero" bogus
check_exit_nonzero "view no arg: exits non-zero" view
check_exit_nonzero "view-toggle no arg: exits non-zero" view-toggle
check_exit_nonzero "layout no arg: exits non-zero" layout
check_exit_nonzero "focus no arg: exits non-zero" focus
check_exit_nonzero "urgent no arg: exits non-zero" urgent
check_exit_nonzero "client-tags no arg: exits non-zero" client-tags

check_contains "view no arg: prints usage hint"        "view requires"        "$(cli_stderr view)"
check_contains "view-toggle no arg: prints usage hint" "view-toggle requires" "$(cli_stderr view-toggle)"
check_contains "layout no arg: prints usage hint"      "layout requires"      "$(cli_stderr layout)"
check_contains "focus no arg: prints usage hint"       "focus requires"       "$(cli_stderr focus)"
check_contains "urgent no arg: prints usage hint"      "urgent requires"      "$(cli_stderr urgent)"
check_contains "client-tags no arg: prints usage hint" "client-tags requires" "$(cli_stderr client-tags)"
check_contains "client-tags bad op: prints usage hint" "set, add, toggle, or remove" \
    "$(cli_stderr client-tags badop deadbeef t1)"

# ── Summary ───────────────────────────────────────────────────────────────────

echo "1..$TEST_NUM"
echo "# $PASS passed, $FAIL failed"
[ "$FAIL" -eq 0 ]
