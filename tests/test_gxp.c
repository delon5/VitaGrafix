/*
 * Unit tests for the shader literal patches (src/patch_gxp_core.c).
 * Synthetic GXP programs only: no game data.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../src/interpreter/interpreter.h"
#include "../src/patch_gxp_core.h"

static int g_tests, g_failed;

#define CHECK(cond, ...) do { \
    g_tests++; \
    if (!(cond)) { \
        g_failed++; \
        printf("FAIL %s:%d: ", __FILE__, __LINE__); \
        printf(__VA_ARGS__); \
        printf("\n"); \
    } \
} while (0)

static void set_ib(uint16_t w, uint16_t h) {
    intp_vg_context_t c = {0};
    c.fb_width = 960;
    c.fb_height = 544;
    for (int i = 0; i < INTP_VG_MAX_RES_COUNT; i++) {
        c.ib_width[i] = w;
        c.ib_height[i] = h;
    }
    c.vblank = 1;
    c.fps_limit = 60;
    c.msaa = 2;
    intp_set_vg_context(&c);
}

static void wr32(uint8_t *p, uint32_t v) { memcpy(p, &v, 4); }
static uint32_t rd32(const uint8_t *p) { uint32_t v; memcpy(&v, p, 4); return v; }

/*
 * A synthetic program: magic, size at +8, filler, and a literal table of
 * (register, value) pairs at 'table' (the offset field at 0x74 is relative to itself)
 */
static uint8_t *make_prog(uint32_t size, uint32_t table, const uint32_t *regs, const uint32_t *vals, uint32_t n) {
    uint8_t *p = malloc(size);
    for (uint32_t i = 0; i < size; i++)
        p[i] = (uint8_t)(i * 7 + 3);
    memcpy(p, "GXP\0", 4);
    p[4] = 1;
    p[5] = 5;
    wr32(p + 8, size);
    wr32(p + VG_GXP_LIT_COUNT, n);
    wr32(p + VG_GXP_LIT_OFFSET, table - VG_GXP_LIT_OFFSET);
    for (uint32_t i = 0; i < n; i++) {
        wr32(p + table + 8 * i, regs[i]);
        wr32(p + table + 8 * i + 4, vals[i]);
    }
    return p;
}

static vg_gxp_parse_t parse(const char *line, vg_gxp_line_t *l, uint32_t *pos) {
    intp_status_t ist;
    return vg_gxp_parse_line(line, l, pos, &ist);
}

static bool add(vg_gxp_table_t *t, const char *line) {
    vg_gxp_line_t l;
    uint32_t pos;
    vg_gxp_parse_t pr = parse(line, &l, &pos);
    if (pr != VG_GXP_PARSE_OK && pr != VG_GXP_PARSE_UNCHANGED)
        return false;
    if (l.kind == VG_GXP_LINE_RULE && pr == VG_GXP_PARSE_UNCHANGED)
        return true;
    return vg_gxp_table_add(t, &l) == VG_GXP_ADD_OK;
}

static void test_fnv(void) {
    CHECK(vg_gxp_fnv1a("", 0) == 0x811C9DC5, "fnv empty");
    CHECK(vg_gxp_fnv1a("a", 1) == 0xE40C292C, "fnv a");
    CHECK(vg_gxp_fnv1a("foobar", 6) == 0xBF9CF968, "fnv foobar");
    CHECK(vg_gxp_fnv1a("GXP\0", 4) == vg_gxp_fnv1a("GXP", 4), "fnv includes the NUL");
}

static void test_parser(void) {
    vg_gxp_line_t l;
    uint32_t pos;

    set_ib(960, 544);
    vg_gxp_parse_t pr = parse("gxp:842cd7e0:0x1C4 fl32(half(1.0/720)) => fl32(half(1.0/min(ib_w, 960)))", &l, &pos);
    CHECK(pr == VG_GXP_PARSE_OK && l.kind == VG_GXP_LINE_PATCH && l.patch.hash == 0x842CD7E0 && l.patch.offset == 0x1C4
          && l.patch.size == 4 && rd32(l.patch.stock) == 0x3AB60000 && rd32(l.patch.data) == 0x3A888000 && l.patch.active,
          "accepts the LBP line at 960x544 (pr %d)", pr);
    set_ib(720, 408);
    pr = parse("gxp:842cd7e0:0x1C4 fl32(half(1.0/720)) => fl32(half(1.0/min(ib_w, 960)))", &l, &pos);
    CHECK(pr == VG_GXP_PARSE_UNCHANGED && !l.patch.active, "stock at 720x408 (pr %d)", pr);
    set_ib(1280, 720);
    pr = parse("gxp:842CD7E0:0x1C4 fl32(half(1.0/720)) => fl32(half(1.0/min(ib_w, 960)))", &l, &pos);
    CHECK(pr == VG_GXP_PARSE_OK && rd32(l.patch.data) == 0x3A888000, "clamped at 1280x720");
    set_ib(960, 544);

    pr = parse("gxplit: fl32(1.0/720).fl32(1.0/408) => fl32(1.0/min(ib_w, 960)).fl32(1.0/min(ib_h, 544))", &l, &pos);
    CHECK(pr == VG_GXP_PARSE_OK && l.kind == VG_GXP_LINE_RULE && l.rule.words == 2 && l.rule.stock[0] == 0x3AB60B61
          && l.rule.stock[1] == 0x3B20A0A1 && l.rule.data[0] == 0x3A888889 && l.rule.data[1] == 0x3AF0F0F1, "rule of 2 words");
    pr = parse("gxplit: fl16(1.0/720).fl16(1.0/408) => fl16(1.0/min(ib_w, 960)).fl16(1.0/min(ib_h, 544))", &l, &pos);
    CHECK(pr == VG_GXP_PARSE_OK && l.rule.words == 1 && l.rule.stock[0] == 0x190515B0 && l.rule.data[0] == 0x17881444,
          "packed half rule");
    pr = parse("GXP:0350976F:0x324 fl16(1.0/720).fl16(1.0/408) => fl16(1.0/960).fl16(1.0/544)  # comment", &l, &pos);
    CHECK(pr == VG_GXP_PARSE_OK, "keyword case and a trailing comment (pr %d)", pr);

    struct { const char *line; vg_gxp_parse_t want; } bad[] = {
        {"gxp:842CD7E0:0x1C4 fl32(1.0)", VG_GXP_PARSE_SYNTAX},                               // no =>
        {"gxp:842CD7E0:0x1C4 fl32(1.0) => fl32(2.0) => fl32(3.0)", VG_GXP_PARSE_SYNTAX},     // two =>
        {"gxp:842CD7E:0x1C4 fl32(1.0) => fl32(2.0)", VG_GXP_PARSE_SYNTAX},                   // 7 digits
        {"gxp:842CD7E00:0x1C4 fl32(1.0) => fl32(2.0)", VG_GXP_PARSE_SYNTAX},                 // 9 digits
        {"gxp:842CD7EG:0x1C4 fl32(1.0) => fl32(2.0)", VG_GXP_PARSE_SYNTAX},                  // not hex
        {"gxp:00000000:0x1C4 fl32(1.0) => fl32(2.0)", VG_GXP_PARSE_SYNTAX},                  // hash 0
        {"gxp:842CD7E0:8 fl32(1.0) => fl32(2.0)", VG_GXP_PARSE_SYNTAX},                      // header
        {"gxp:842CD7E0:-4 fl32(1.0) => fl32(2.0)", VG_GXP_PARSE_SYNTAX},
        {"gxp:842CD7E0:  0x1C4 fl32(1.0) => fl32(2.0)", VG_GXP_PARSE_SYNTAX},
        {"gxp:842CD7E0:0x1C4fl32(1.0) => fl32(2.0)", VG_GXP_PARSE_SYNTAX},                  // no blank after offset
        {"gxp:842CD7E0:0xFFFFF fl32(1.0) => fl32(2.0)", VG_GXP_PARSE_SYNTAX},               // past 1 MiB
        {"gxp:842CD7E0:0x1C4 fl32(1.0) => fl16(2.0)", VG_GXP_PARSE_SIZE},
        {"gxp:842CD7E0:0x1C4 fl32(1.0/720).??(4) => ??(4).fl32(1.0)", VG_GXP_PARSE_GAPS},
        {"gxp:842CD7E0:0x1C4 ??(4) => ??(4)", VG_GXP_PARSE_GAPS},
        {"gxp:842CD7E0:0x1C4 fl32(1.0/720), => fl32(1.0)", VG_GXP_PARSE_SYNTAX},             // trailing , left
        {"gxp:842CD7E0:0x1C4 fl32(1.0/720)$ => fl32(1.0)", VG_GXP_PARSE_SYNTAX},             // trailing $ left
        {"gxp:842CD7E0:0x1C4 fl32(1.0/720) => fl32(1.0),", VG_GXP_PARSE_SYNTAX},             // trailing , right
        {"gxp:842CD7E0:0x1C4 fl32(1.0/720) => fl32(1.0)$", VG_GXP_PARSE_SYNTAX},             // trailing $ right
        {"gxp:842CD7E0:0x1C4 fl32(1.0/nope) => fl32(1.0)", VG_GXP_PARSE_EVAL},
        {"gxp:842CD7E0:0x1C4 fl32(1.0) => fl32(1.0/nope)", VG_GXP_PARSE_EVAL},
        {"gxplit: fl32(1.0).fl16(1.0) => fl32(2.0).fl16(2.0)", VG_GXP_PARSE_SIZE},          // 6 bytes
        {"gxplit: fl32(1.0).??(4) => fl32(2.0).??(4)", VG_GXP_PARSE_GAPS},
        {"gxplit: fl32(1).fl32(2).fl32(3).fl32(4).fl32(5).fl32(6).fl32(7).fl32(8).fl32(9) => fl32(1)", VG_GXP_PARSE_EVAL},
        {"gxpx: fl32(1.0) => fl32(2.0)", VG_GXP_PARSE_SYNTAX},
    };
    for (unsigned i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
        pr = parse(bad[i].line, &l, &pos);
        CHECK(pr == bad[i].want, "rejects '%s': got %d, want %d (pos %u)", bad[i].line, pr, bad[i].want, pos);
    }

    // The stock side ends before column VG_GXP_MAX_LEFT
    char longline[400];
    int n = snprintf(longline, sizeof(longline), "gxp:842CD7E0:0x1C4 fl32(1.0");
    while (n < 200)
        n += snprintf(longline + n, sizeof(longline) - n, "+0");
    snprintf(longline + n, sizeof(longline) - n, ") => fl32(2.0)");
    pr = parse(longline, &l, &pos);
    CHECK(pr == VG_GXP_PARSE_SYNTAX && pos >= VG_GXP_MAX_LEFT, "=> past column 128 (pr %d pos %u)", pr, pos);

    // Error positions are relative to the whole line
    pr = parse("gxp:842CD7E0:0x1C4 fl32(1.0) => fl32(nope)", &l, &pos);
    CHECK(pr == VG_GXP_PARSE_EVAL && pos >= 37, "right-side error position %u", pos);
}

static void test_table(void) {
    vg_gxp_table_t t;
    vg_gxp_table_reset(&t);
    set_ib(960, 544);
    CHECK(add(&t, "gxp:11111111:0x1C4 fl32(1.0) => fl32(2.0)"), "first");
    CHECK(!add(&t, "gxp:11111111:0x1C6 fl16(1.0) => fl16(2.0)"), "overlap 0x1C4+4 / 0x1C6+2 rejected");
    CHECK(add(&t, "gxp:11111111:0x1C8 fl32(1.0) => fl32(2.0)"), "adjacent accepted");
    CHECK(add(&t, "gxp:22222222:0x1C4 fl32(1.0) => fl32(2.0)"), "same offset, other program");
    CHECK(!add(&t, "gxp:11111111:0x1C4 fl32(3.0) => fl32(3.0)"), "an unchanged line still counts for overlap");

    char line[128];
    vg_gxp_table_reset(&t);
    for (int i = 0; i < VG_GXP_MAX_PATCHES; i++) {
        snprintf(line, sizeof(line), "gxp:%08X:0x20 fl32(1.0) => fl32(2.0)", 0x100 + i);
        add(&t, line);
    }
    CHECK(t.count == VG_GXP_MAX_PATCHES, "table filled");
    vg_gxp_line_t l;
    uint32_t pos;
    parse("gxp:0000FFFF:0x20 fl32(1.0) => fl32(2.0)", &l, &pos);
    CHECK(vg_gxp_table_add(&t, &l) == VG_GXP_ADD_FULL, "entry 129 is FULL");

    vg_gxp_table_reset(&t);
    for (int i = 0; i < VG_GXP_MAX_RULES; i++) {
        snprintf(line, sizeof(line), "gxplit: uint32(%d) => uint32(%d)", 1000 + i, 2000 + i);
        add(&t, line);
    }
    parse("gxplit: uint32(5000) => uint32(6000)", &l, &pos);
    CHECK(vg_gxp_table_add(&t, &l) == VG_GXP_ADD_FULL, "rule 9 is FULL");

    vg_gxp_table_reset(&t);
    CHECK(add(&t, "gxplit: uint32(1).uint32(2) => uint32(3).uint32(4)"), "rule a");
    parse("gxplit: uint32(4) => uint32(9)", &l, &pos);
    CHECK(vg_gxp_table_add(&t, &l) == VG_GXP_ADD_CHAIN, "a rule looking for another rule's new words");
    parse("gxplit: uint32(7) => uint32(1)", &l, &pos);
    CHECK(vg_gxp_table_add(&t, &l) == VG_GXP_ADD_CHAIN, "a rule writing another rule's stock words");

    vg_gxp_table_reset(&t);
    parse("gxplit: uint32(1).uint32(2) => uint32(3).uint32(1)", &l, &pos);
    CHECK(vg_gxp_table_add(&t, &l) == VG_GXP_ADD_CHAIN, "a rule writing a word it looks for itself");

    // At ib 720x544 only 1/H changes: the three LBP rules must still be accepted together
    set_ib(720, 544);
    vg_gxp_table_reset(&t);
    CHECK(add(&t, "gxplit: fl32(1.0/720).fl32(1.0/408) => fl32(1.0/min(ib_w, 960)).fl32(1.0/min(ib_h, 544))")
          && add(&t, "gxplit: fl32(half(1.0/720)).fl32(1.0/408) => fl32(half(1.0/min(ib_w, 960))).fl32(1.0/min(ib_h, 544))")
          && add(&t, "gxplit: fl16(1.0/720).fl16(1.0/408) => fl16(1.0/min(ib_w, 960)).fl16(1.0/min(ib_h, 544))")
          && t.rule_count == 3, "LBP rules at 720x544 (%u rules)", t.rule_count);
    set_ib(960, 544);

    // Seal: sorted, programs counted, survey words deduplicated
    vg_gxp_table_reset(&t);
    add(&t, "gxp:33333333:0x28 fl32(1.0/720) => fl32(1.0/960)");
    add(&t, "gxp:11111111:0x30 fl32(1.0/408) => fl32(1.0/544)");
    add(&t, "gxp:11111111:0x20 fl32(1.0/720) => fl32(1.0/960)");
    add(&t, "gxplit: fl32(1.0/720).fl32(1.0/408) => fl32(1.0/960).fl32(1.0/544)");
    vg_gxp_table_seal(&t);
    CHECK(t.e[0].hash == 0x11111111 && t.e[0].offset == 0x20 && t.e[1].offset == 0x30 && t.e[2].hash == 0x33333333,
          "sorted by hash and offset");
    CHECK(t.programs == 2 && t.e[0].prog == 0 && t.e[1].prog == 0 && t.e[2].prog == 1, "program slots");
    CHECK(t.survey_count == 2, "survey words deduplicated (%u)", t.survey_count);
}

// An 801-byte program with (1/720 half-widened, 1/408) at +0x1C4 / +0x1CC, like 842CD7E0
static uint8_t *prog801(void) {
    uint32_t regs[] = {0xC, 0xD, 0xE, 0xF};
    uint32_t vals[] = {0x3AB60000, 0x3B20A0A1, 0x3F800000, 0x40000000};
    return make_prog(801, 0x1C0, regs, vals, 4);
}

static void table_for(vg_gxp_table_t *t, uint32_t hash, bool second_wrong) {
    char a[128], b[128];
    vg_gxp_table_reset(t);
    snprintf(a, sizeof(a), "gxp:%08X:0x1C4 fl32(half(1.0/720)) => fl32(half(1.0/min(ib_w, 960)))", hash);
    snprintf(b, sizeof(b), "gxp:%08X:0x1CC fl32(%s) => fl32(1.0/min(ib_h, 544))", hash, second_wrong ? "1.0/400" : "1.0/408");
    add(t, a);
    add(t, b);
    vg_gxp_table_seal(t);
}

static void test_apply(void) {
    set_ib(960, 544);
    uint8_t *p = prog801();
    uint32_t hash = vg_gxp_fnv1a(p, 801);
    vg_gxp_table_t t;
    table_for(&t, hash, false);

    uint8_t *saved = malloc(801);
    memcpy(saved, p, 801);
    vg_gxp_result_t r;
    vg_gxp_status_t st = vg_gxp_match(&t, p, &r);
    CHECK(st == GXP_MATCHED && r.sites == 2 && r.bytes == 8 && r.hash == hash, "matched (%d, %u sites)", st, r.sites);
    CHECK(vg_gxp_recheck(&t, p, &r) == GXP_MATCHED, "recheck still stock");
    vg_gxp_commit(&t, p, &r);
    int changed = 0;
    for (int i = 0; i < 801; i++)
        changed += p[i] != saved[i];
    CHECK(changed <= 8 && rd32(p + 0x1C4) == 0x3A888000 && rd32(p + 0x1CC) == 0x3AF0F0F1, "only the 8 bytes change");

    st = vg_gxp_match(&t, p, &r);
    CHECK(st == GXP_NO_MATCH, "a patched buffer no longer matches (%d)", st);
    uint8_t *fresh = prog801();
    CHECK(vg_gxp_match(&t, fresh, &r) == GXP_MATCHED, "a fresh copy matches again");

    // Race: another registration wrote the same bytes between match and commit
    memcpy(fresh + 0x1C4, p + 0x1C4, 4);
    memcpy(fresh + 0x1CC, p + 0x1CC, 4);
    CHECK(vg_gxp_recheck(&t, fresh, &r) == GXP_ALREADY_PATCHED, "already patched");
    wr32(fresh + 0x1C4, 0x12345678);
    CHECK(vg_gxp_recheck(&t, fresh, &r) == GXP_STOCK_MISMATCH, "changed to something else");
    free(fresh);

    // All or nothing: one wrong stock line blocks the program
    uint8_t *q = prog801();
    table_for(&t, vg_gxp_fnv1a(q, 801), true);
    memcpy(saved, q, 801);
    st = vg_gxp_match(&t, q, &r);
    CHECK(st == GXP_STOCK_MISMATCH && r.bad_offset >= 0x1CC && r.bad_offset < 0x1D0 && !memcmp(q, saved, 801),
          "stock mismatch, bad offset 0x%X, nothing written", r.bad_offset);

    // Out of range
    vg_gxp_table_reset(&t);
    char line[128];
    snprintf(line, sizeof(line), "gxp:%08X:0x320 uint16(0x1234) => uint16(0x4321)", vg_gxp_fnv1a(q, 801));
    add(&t, line);
    vg_gxp_table_seal(&t);
    CHECK(vg_gxp_match(&t, q, &r) == GXP_OUT_OF_RANGE, "out of range");
    free(q);

    // Gap: the register bytes between the two values are neither checked nor written
    uint8_t *g = prog801();
    wr32(g + 0x1C8, 0x77);   // register index changed
    uint32_t gh = vg_gxp_fnv1a(g, 801);
    vg_gxp_table_reset(&t);
    snprintf(line, sizeof(line), "gxp:%08X:0x1C4 fl32(half(1.0/720)).??(4).fl32(1.0/408) => "
             "fl32(half(1.0/960)).??(4).fl32(1.0/544)", gh);
    CHECK(add(&t, line), "gap line accepted");
    vg_gxp_table_seal(&t);
    st = vg_gxp_match(&t, g, &r);
    CHECK(st == GXP_MATCHED && r.bytes == 8, "gap: matched with 8 bytes (%d, %u)", st, r.bytes);
    vg_gxp_commit(&t, g, &r);
    CHECK(rd32(g + 0x1C8) == 0x77 && rd32(g + 0x1C4) == 0x3A888000, "gap bytes untouched");
    free(g);

    free(p);
    free(saved);
}

static void test_rules(void) {
    set_ib(960, 544);
    vg_gxp_table_t t;
    vg_gxp_table_reset(&t);
    add(&t, "gxplit: fl32(1.0/720).fl32(1.0/408) => fl32(1.0/min(ib_w, 960)).fl32(1.0/min(ib_h, 544))");
    add(&t, "gxplit: fl16(1.0/720).fl16(1.0/408) => fl16(1.0/min(ib_w, 960)).fl16(1.0/min(ib_h, 544))");
    vg_gxp_table_seal(&t);
    vg_gxp_result_t r;

    uint32_t regs[] = {4, 5, 6, 7};
    uint32_t vals[] = {0x3935385C, 0x3AB60B61, 0x3B20A0A1, 0x190515B0};
    uint8_t *p = make_prog(600, 0x200, regs, vals, 4);
    vg_gxp_status_t st = vg_gxp_match(&t, p, &r);
    CHECK(st == GXP_RULE_MATCHED && r.nsite == 2, "two runs (%d, %u)", st, r.nsite);
    CHECK(vg_gxp_recheck(&t, p, &r) == GXP_RULE_MATCHED, "rule recheck");
    vg_gxp_commit(&t, p, &r);
    CHECK(rd32(p + 0x200 + 8 + 4) == 0x3A888889 && rd32(p + 0x200 + 16 + 4) == 0x3AF0F0F1
          && rd32(p + 0x200 + 24 + 4) == 0x17881444 && rd32(p + 0x200 + 4) == 0x3935385C
          && rd32(p + 0x200 + 8) == 5, "rule words written, registers and others untouched");
    free(p);

    uint32_t regs2[] = {4, 6};   // not consecutive registers
    uint32_t vals2[] = {0x3AB60B61, 0x3B20A0A1};
    p = make_prog(600, 0x200, regs2, vals2, 2);
    CHECK(vg_gxp_match(&t, p, &r) == GXP_SURVEY, "non-consecutive registers: survey only");
    free(p);

    uint32_t regs3[] = {4, 5, 6};   // values not adjacent
    uint32_t vals3[] = {0x3AB60B61, 0x3F800000, 0x3B20A0A1};
    p = make_prog(600, 0x200, regs3, vals3, 3);
    CHECK(vg_gxp_match(&t, p, &r) == GXP_SURVEY, "non-adjacent values: survey only");
    free(p);

    // Conflicting runs
    vg_gxp_table_t c;
    vg_gxp_table_reset(&c);
    add(&c, "gxplit: uint32(1).uint32(2) => uint32(10).uint32(20)");
    add(&c, "gxplit: uint32(2).uint32(3) => uint32(30).uint32(40)");
    vg_gxp_table_seal(&c);
    uint32_t regs4[] = {0, 1, 2};
    uint32_t vals4[] = {1, 2, 3};
    p = make_prog(600, 0x200, regs4, vals4, 3);
    uint8_t saved[600];
    memcpy(saved, p, 600);
    CHECK(vg_gxp_match(&c, p, &r) == GXP_RULE_CONFLICT && !memcmp(p, saved, 600), "conflict, nothing written");
    free(p);

    // Too many runs
    uint32_t regs5[40], vals5[40];
    for (int i = 0; i < 40; i++) {
        regs5[i] = i;
        vals5[i] = i % 2 ? 0x3B20A0A1 : 0x3AB60B61;
    }
    p = make_prog(1000, 0x200, regs5, vals5, 40);
    CHECK(vg_gxp_match(&t, p, &r) == GXP_RULE_TOO_MANY, "17+ runs");
    free(p);

    // A named program is never scanned by rules
    p = make_prog(600, 0x200, regs, vals, 4);
    vg_gxp_table_t n;
    vg_gxp_table_reset(&n);
    add(&n, "gxplit: fl32(1.0/720).fl32(1.0/408) => fl32(1.0/min(ib_w, 960)).fl32(1.0/min(ib_h, 544))");
    char line[128];
    snprintf(line, sizeof(line), "gxp:%08X:0x10 uint8(%u) => uint8(%u)", vg_gxp_fnv1a(p, 600), p[0x10], p[0x10] ^ 1);
    add(&n, line);
    vg_gxp_table_seal(&n);
    CHECK(vg_gxp_match(&n, p, &r) == GXP_MATCHED && r.nsite == 0, "named program: its line, not the rule");
    free(p);

    // Damaged literal tables are not scanned (and not over-read)
    p = make_prog(600, 0x200, regs, vals, 4);
    wr32(p + VG_GXP_LIT_COUNT, 0x10000000);
    CHECK(vg_gxp_match(&t, p, &r) == GXP_NO_MATCH, "huge literal count");
    wr32(p + VG_GXP_LIT_COUNT, 4);
    wr32(p + VG_GXP_LIT_OFFSET, 0xFFFFFFF0);
    CHECK(vg_gxp_match(&t, p, &r) == GXP_NO_MATCH, "table offset past the end");
    free(p);
}

static void test_survey_and_programs(void) {
    set_ib(960, 544);
    vg_gxp_table_t t;
    vg_gxp_table_reset(&t);
    add(&t, "gxp:12345678:0x20 fl32(1.0/720) => fl32(1.0/min(ib_w, 960))");
    vg_gxp_table_seal(&t);
    vg_gxp_result_t r;
    uint32_t regs[] = {3};
    uint32_t vals[] = {0x3AB60B61};
    uint8_t *p = make_prog(400, 0x100, regs, vals, 1);
    CHECK(vg_gxp_match(&t, p, &r) == GXP_SURVEY && r.survey_word == 0x3AB60B61 && r.survey_offset == 0x104,
          "survey word and offset");
    free(p);

    uint8_t *small = malloc(12);
    memcpy(small, "GXP\0", 4);
    wr32(small + 8, 0x10);
    CHECK(vg_gxp_match(&t, small, &r) == GXP_NOT_PROGRAM, "size 0x10");
    wr32(small + 8, 0x200000);
    CHECK(vg_gxp_match(&t, small, &r) == GXP_NOT_PROGRAM, "size 2 MiB");
    small[0] = 'X';
    CHECK(vg_gxp_match(&t, small, &r) == GXP_NOT_PROGRAM, "bad magic");
    free(small);
    CHECK(vg_gxp_match(&t, NULL, &r) == GXP_NOT_PROGRAM, "NULL");
}

static void test_lookup(void) {
    set_ib(960, 544);
    vg_gxp_table_t t;
    vg_gxp_table_reset(&t);
    uint32_t hashes[40];
    srand(1234);
    for (int i = 0; i < 40; i++)
        hashes[i] = ((uint32_t)rand() << 16) ^ (uint32_t)rand() ^ 1;
    char line[128];
    for (int k = 0; k < 80; k++) {
        int i = (k * 17) % 40, which = k / 40;
        snprintf(line, sizeof(line), "gxp:%08X:0x%X uint16(0x1111) => uint16(0x2222)", hashes[i], 0x40 + 0x10 * which);
        add(&t, line);
    }
    vg_gxp_table_seal(&t);
    int ok = 0;
    for (int i = 0; i < 40; i++) {
        int n = 0;
        for (int k = 0; k < t.count; k++)
            n += t.e[k].hash == hashes[i];
        ok += n == 2;
    }
    CHECK(ok == 40 && t.programs == 40, "each program has exactly its own 2 entries (%d, %u)", ok, t.programs);
}

int main(void) {
    test_fnv();
    test_parser();
    test_table();
    test_apply();
    test_rules();
    test_survey_and_programs();
    test_lookup();
    printf("%d out of %d tests succeeded!\n", g_tests - g_failed, g_tests);
    return g_failed ? 1 : 0;
}
