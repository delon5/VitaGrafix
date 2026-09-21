#!/usr/bin/env bash
#
# Checks what tests/test_patchlist reports for the '>' hook directives in
# patchlist_directives.txt: one line per directive, with the arguments it
# resolved to at that configuration, and the plugin's own refusal for every
# directive that would fail on hardware. The expected output is frozen below,
# so a change in the directive grammar, in what is refused, or in the report
# the patch verifier reads has to be made here on purpose.
set -u

SCRIPT_DIR=$(dirname "${BASH_SOURCE[0]}")
TEST_BIN="$SCRIPT_DIR/test_patchlist"
FIXTURE="$SCRIPT_DIR/patchlist_directives.txt"

make -C "$SCRIPT_DIR" test_patchlist >&2 || exit 2

failures=0

run_case() {
    local label=$1
    local expected=$2
    shift 2

    local actual status
    actual=$("$TEST_BIN" "$FIXTURE" "$@" 2>/dev/null)
    status=$?

    # The fixture holds directives that must be refused, so a run that reports
    # no error at all is itself a failure
    if [[ $status -ne 1 ]]; then
        printf '%s: expected exit 1 (errors reported), got %d\n' "$label" "$status"
        failures=$((failures + 1))
    fi

    if ! diff -u <(printf '%s\n' "$expected") <(printf '%s\n' "$actual"); then
        printf '%s: report does not match the expected output\n' "$label"
        failures=$((failures + 1))
    fi
}

read -r -d '' EXPECTED_FPS60 <<'EOF'
00006 HOOK ERR - - Hook directive outside of a [section] header. | >rateDivide(0:0x1000, 2, void)
00011 HOOK ERR 10 0 Hook directive outside of @FPS. | >sceDisplaySetFrameBuf_withWait()
00014 HOOK OK sceDisplaySetFrameBuf_withWait nid=0x7A410B64 install=no
00015 HOOK OK sceCtrlReadBufferPositive_peekPatched nid=0x67E7AB83 install=yes
00016 HOOK OK sceCtrlReadBufferPositive2_peekPatched nid=0xC4226A3E install=yes
00018 HOOK OK sceCtrlReadBufferPositive2_peekPatched nid=0xC4226A3E install=yes
00021 HOOK OK rateDivide 0:00001000 divisor=2 ret=0x1 thumb call args=4 install=yes
00022 HOOK OK rateDivide 0:00001010 divisor=2 ret=0x0 arm frame args=4 install=yes
00023 HOOK OK rateDivide 0:00001020 divisor=2 ret=0x7F thumb call args=2 install=yes
00025 HOOK OK rateDivide 0:00001000 divisor=2 ret=0x1 thumb call args=4 install=yes
00028 HOOK ERR 11 0 Conflicting hook directives for one address. | >rateDivide(0:0x1000, 3, ret=9)
00031 HOOK ERR 12 22 Rate divisor out of range, allowed: 1..16 | >rateDivide(0:0x1030, fps_limit / 90, void)
00032 HOOK ERR 12 22 Rate divisor out of range, allowed: 1..16 | >rateDivide(0:0x1030, 17, void)
00035 HOOK ERR 14 25 Hook target ABI is not supported. | >rateDivide(0:0x1030, 2, retfloat)
00036 HOOK ERR 14 37 Hook target ABI is not supported. | >rateDivide(0:0x1030, 2, ret=1, args=6)
00038 HOOK ERR 13 0 Hook target is not an instruction inside the module. | >rateDivide(0:0x1031, 2, void)
00039 HOOK ERR 13 0 Hook target is not an instruction inside the module. | >rateDivide(0:0x40000, 2, void)
00042 HOOK ERR 4 23 Invalid token. | >rateDivide(0:0x1030, 2)
00043 HOOK ERR 4 31 Invalid token. | >rateDivide(0:0x1030, 2, void, fast)
00044 HOOK ERR 4 37 Invalid token. | >rateDivide(0:0x1030, 2, void, call, frame)
00045 HOOK ERR 4 30 Invalid token. | >rateDivide(0:0x1030, 2, void) and more
00046 HOOK ERR 4 1 Invalid token. | >noSuchDirective()
00052 HOOK OK rateDivide 0:00001000 divisor=3 ret=0x9 thumb call args=4 install=yes
EOF

read -r -d '' EXPECTED_FPS30 <<'EOF'
00006 HOOK ERR - - Hook directive outside of a [section] header. | >rateDivide(0:0x1000, 2, void)
00011 HOOK ERR 10 0 Hook directive outside of @FPS. | >sceDisplaySetFrameBuf_withWait()
00014 HOOK OK sceDisplaySetFrameBuf_withWait nid=0x7A410B64 install=yes
00015 HOOK OK sceCtrlReadBufferPositive_peekPatched nid=0x67E7AB83 install=no
00016 HOOK OK sceCtrlReadBufferPositive2_peekPatched nid=0xC4226A3E install=no
00018 HOOK OK sceCtrlReadBufferPositive2_peekPatched nid=0xC4226A3E install=no
00021 HOOK OK rateDivide 0:00001000 divisor=1 ret=0x1 thumb call args=4 install=no
00022 HOOK OK rateDivide 0:00001010 divisor=2 ret=0x0 arm frame args=4 install=yes
00023 HOOK OK rateDivide 0:00001020 divisor=2 ret=0x7F thumb call args=2 install=yes
00025 HOOK OK rateDivide 0:00001000 divisor=1 ret=0x1 thumb call args=4 install=no
00028 HOOK OK rateDivide 0:00001000 divisor=3 ret=0x9 thumb call args=4 install=yes
00031 HOOK ERR 12 22 Rate divisor out of range, allowed: 1..16 | >rateDivide(0:0x1030, fps_limit / 90, void)
00032 HOOK ERR 12 22 Rate divisor out of range, allowed: 1..16 | >rateDivide(0:0x1030, 17, void)
00035 HOOK ERR 14 25 Hook target ABI is not supported. | >rateDivide(0:0x1030, 2, retfloat)
00036 HOOK ERR 14 37 Hook target ABI is not supported. | >rateDivide(0:0x1030, 2, ret=1, args=6)
00038 HOOK ERR 13 0 Hook target is not an instruction inside the module. | >rateDivide(0:0x1031, 2, void)
00039 HOOK ERR 13 0 Hook target is not an instruction inside the module. | >rateDivide(0:0x40000, 2, void)
00042 HOOK ERR 4 23 Invalid token. | >rateDivide(0:0x1030, 2)
00043 HOOK ERR 4 31 Invalid token. | >rateDivide(0:0x1030, 2, void, fast)
00044 HOOK ERR 4 37 Invalid token. | >rateDivide(0:0x1030, 2, void, call, frame)
00045 HOOK ERR 4 30 Invalid token. | >rateDivide(0:0x1030, 2, void) and more
00046 HOOK ERR 4 1 Invalid token. | >noSuchDirective()
00052 HOOK OK rateDivide 0:00001000 divisor=3 ret=0x9 thumb call args=4 install=yes
EOF

run_case "FPS=60" "$EXPECTED_FPS60" --fps 60 --seg 0:0x30000
run_case "FPS=30" "$EXPECTED_FPS30" --fps 30 --seg 0:0x30000

if [[ $failures -ne 0 ]]; then
    printf 'FAILED: %d case(s)\n' "$failures"
    exit 1
fi

printf 'PASSED: hook directives reported as expected at FPS=60 and FPS=30\n'
