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
00031 HOOK ERR 11 0 Conflicting hook directives for one address. | >rateDivide(0:0x1000, 3, ret=9)
00032 HOOK ERR 11 0 Conflicting hook directives for one address. | >rateDivide(0:0x1000, 1 + (fps_limit / 60), ret=1, arm)
00033 HOOK ERR 11 0 Conflicting hook directives for one address. | >rateDivide(0:0x1000, 1 + (fps_limit / 60), ret=1, args=2)
00036 HOOK ERR 12 22 Rate divisor out of range, allowed: 1..16 | >rateDivide(0:0x1030, fps_limit / 90, void)
00037 HOOK ERR 12 22 Rate divisor out of range, allowed: 1..16 | >rateDivide(0:0x1030, 17, void)
00040 HOOK ERR 14 25 Hook target ABI is not supported. | >rateDivide(0:0x1030, 2, retfloat)
00041 HOOK ERR 14 37 Hook target ABI is not supported. | >rateDivide(0:0x1030, 2, ret=1, args=6)
00043 HOOK ERR 13 0 Hook target is not an instruction inside the module. | >rateDivide(0:0x1031, 2, void)
00044 HOOK ERR 13 0 Hook target is not an instruction inside the module. | >rateDivide(0:0x40000, 2, void)
00046 HOOK ERR 13 0 Hook target is not an instruction inside the module. | >rateDivide(1:0x1000, 2, void)
00049 HOOK ERR 4 23 Invalid token. | >rateDivide(0:0x1030, 2)
00050 HOOK ERR 4 31 Invalid token. | >rateDivide(0:0x1030, 2, void, fast)
00051 HOOK ERR 4 37 Invalid token. | >rateDivide(0:0x1030, 2, void, call, frame)
00052 HOOK ERR 4 30 Invalid token. | >rateDivide(0:0x1030, 2, void) and more
00053 HOOK ERR 4 1 Invalid token. | >noSuchDirective()
00059 HOOK OK rateDivide 0:00001000 divisor=3 ret=0x9 thumb call args=4 install=yes
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
00031 HOOK OK rateDivide 0:00001000 divisor=3 ret=0x9 thumb call args=4 install=yes
00032 HOOK OK rateDivide 0:00001000 divisor=1 ret=0x1 arm call args=4 install=no
00033 HOOK OK rateDivide 0:00001000 divisor=1 ret=0x1 thumb call args=2 install=no
00036 HOOK ERR 12 22 Rate divisor out of range, allowed: 1..16 | >rateDivide(0:0x1030, fps_limit / 90, void)
00037 HOOK ERR 12 22 Rate divisor out of range, allowed: 1..16 | >rateDivide(0:0x1030, 17, void)
00040 HOOK ERR 14 25 Hook target ABI is not supported. | >rateDivide(0:0x1030, 2, retfloat)
00041 HOOK ERR 14 37 Hook target ABI is not supported. | >rateDivide(0:0x1030, 2, ret=1, args=6)
00043 HOOK ERR 13 0 Hook target is not an instruction inside the module. | >rateDivide(0:0x1031, 2, void)
00044 HOOK ERR 13 0 Hook target is not an instruction inside the module. | >rateDivide(0:0x40000, 2, void)
00046 HOOK ERR 13 0 Hook target is not an instruction inside the module. | >rateDivide(1:0x1000, 2, void)
00049 HOOK ERR 4 23 Invalid token. | >rateDivide(0:0x1030, 2)
00050 HOOK ERR 4 31 Invalid token. | >rateDivide(0:0x1030, 2, void, fast)
00051 HOOK ERR 4 37 Invalid token. | >rateDivide(0:0x1030, 2, void, call, frame)
00052 HOOK ERR 4 30 Invalid token. | >rateDivide(0:0x1030, 2, void) and more
00053 HOOK ERR 4 1 Invalid token. | >noSuchDirective()
00059 HOOK OK rateDivide 0:00001000 divisor=3 ret=0x9 thumb call args=4 install=yes
EOF

# No '--seg' at all: the plugin cannot bound check a hook target without
# module segment info, and refuses every one rather than arming a branch into
# code it has not checked. sceKernelGetModuleInfo() failing on hardware lands
# here, and so does a patch author who did not pass the segment sizes.
read -r -d '' EXPECTED_NO_SEG <<'EOF'
00006 HOOK ERR - - Hook directive outside of a [section] header. | >rateDivide(0:0x1000, 2, void)
00011 HOOK ERR 10 0 Hook directive outside of @FPS. | >sceDisplaySetFrameBuf_withWait()
00014 HOOK OK sceDisplaySetFrameBuf_withWait nid=0x7A410B64 install=no
00015 HOOK OK sceCtrlReadBufferPositive_peekPatched nid=0x67E7AB83 install=yes
00016 HOOK OK sceCtrlReadBufferPositive2_peekPatched nid=0xC4226A3E install=yes
00018 HOOK OK sceCtrlReadBufferPositive2_peekPatched nid=0xC4226A3E install=yes
00021 HOOK ERR 15 0 No module segment info, hook target cannot be checked. | >rateDivide(0:0x1000, 1 + (fps_limit / 60), ret=1)
00022 HOOK ERR 15 0 No module segment info, hook target cannot be checked. | >rateDivide(0:0x1010, 2, void, arm, frame)
00023 HOOK ERR 15 0 No module segment info, hook target cannot be checked. | >rateDivide(0:0x1020, 2, ret=0x7F, thumb, call, args=2)
00025 HOOK ERR 15 0 No module segment info, hook target cannot be checked. | >rateDivide(0:0x1000, 1 + (fps_limit / 60), ret=1)
00031 HOOK ERR 15 0 No module segment info, hook target cannot be checked. | >rateDivide(0:0x1000, 3, ret=9)
00032 HOOK ERR 15 0 No module segment info, hook target cannot be checked. | >rateDivide(0:0x1000, 1 + (fps_limit / 60), ret=1, arm)
00033 HOOK ERR 15 0 No module segment info, hook target cannot be checked. | >rateDivide(0:0x1000, 1 + (fps_limit / 60), ret=1, args=2)
00036 HOOK ERR 12 22 Rate divisor out of range, allowed: 1..16 | >rateDivide(0:0x1030, fps_limit / 90, void)
00037 HOOK ERR 12 22 Rate divisor out of range, allowed: 1..16 | >rateDivide(0:0x1030, 17, void)
00040 HOOK ERR 14 25 Hook target ABI is not supported. | >rateDivide(0:0x1030, 2, retfloat)
00041 HOOK ERR 14 37 Hook target ABI is not supported. | >rateDivide(0:0x1030, 2, ret=1, args=6)
00043 HOOK ERR 13 0 Hook target is not an instruction inside the module. | >rateDivide(0:0x1031, 2, void)
00044 HOOK ERR 15 0 No module segment info, hook target cannot be checked. | >rateDivide(0:0x40000, 2, void)
00046 HOOK ERR 15 0 No module segment info, hook target cannot be checked. | >rateDivide(1:0x1000, 2, void)
00049 HOOK ERR 4 23 Invalid token. | >rateDivide(0:0x1030, 2)
00050 HOOK ERR 4 31 Invalid token. | >rateDivide(0:0x1030, 2, void, fast)
00051 HOOK ERR 4 37 Invalid token. | >rateDivide(0:0x1030, 2, void, call, frame)
00052 HOOK ERR 4 30 Invalid token. | >rateDivide(0:0x1030, 2, void) and more
00053 HOOK ERR 4 1 Invalid token. | >noSuchDirective()
00059 HOOK ERR 15 0 No module segment info, hook target cannot be checked. | >rateDivide(0:0x1000, 3, ret=9)
EOF

run_case "FPS=60" "$EXPECTED_FPS60" --fps 60 --seg 0:0x30000 --seg 1:0x8000:6
run_case "FPS=30" "$EXPECTED_FPS30" --fps 30 --seg 0:0x30000 --seg 1:0x8000:6
run_case "no --seg" "$EXPECTED_NO_SEG" --fps 60

if [[ $failures -ne 0 ]]; then
    printf 'FAILED: %d case(s)\n' "$failures"
    exit 1
fi

printf 'PASSED: hook directives reported as expected at FPS=60, FPS=30 and without segment info\n'
