#include <stdlib.h>
#include <string.h>
#include <strings.h>
#ifndef BUILD_VITAGRAFIX
#include <ctype.h>
#endif

#include "patch_gxp_core.h"

static uint32_t rd32(const uint8_t *p) {
    uint32_t v;
    memcpy(&v, p, sizeof(v));
    return v;
}

static void wr32(uint8_t *p, uint32_t v) {
    memcpy(p, &v, sizeof(v));
}

uint32_t vg_gxp_fnv1a(const void *data, uint32_t size) {
    const uint8_t *p = (const uint8_t *)data;
    uint32_t h = 0x811C9DC5u;
    for (uint32_t i = 0; i < size; i++) {
        h ^= p[i];
        h *= 0x01000193u;
    }
    return h ? h : 1;   // 0 is never a valid hash
}

bool vg_gxp_program_size(const void *prog, uint32_t *size) {
    const uint8_t *p = (const uint8_t *)prog;
    if (p[0] != 'G' || p[1] != 'X' || p[2] != 'P' || p[3] != 0)
        return false;
    uint32_t s = rd32(p + 8);
    if (s < VG_GXP_MIN_SIZE || s > VG_GXP_MAX_SIZE)
        return false;
    *size = s;
    return true;
}

bool vg_gxp_literal_table(const void *prog, uint32_t size, uint32_t *offset, uint32_t *count) {
    const uint8_t *p = (const uint8_t *)prog;
    if (size < VG_GXP_LIT_OFFSET + 4)
        return false;
    uint64_t n = rd32(p + VG_GXP_LIT_COUNT);
    uint64_t off = (uint64_t)VG_GXP_LIT_OFFSET + rd32(p + VG_GXP_LIT_OFFSET);
    if (n == 0 || off + 8 * n > size)
        return false;
    *offset = (uint32_t)off;
    *count = (uint32_t)n;
    return true;
}

/*
 * Line parsing
 */
static bool line_end(const char *s, uint32_t pos) {
    while (isspace((unsigned char)s[pos]))
        pos++;
    return s[pos] == '\0' || s[pos] == '#';
}

static int hexval(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

// Evaluates line[*pos..] up to the end of 'line' and requires nothing but blanks (or a comment) after it
static vg_gxp_parse_t eval_side(const char *line, uint32_t *pos, intp_value_t *value, uint32_t *errpos,
                                intp_status_t *ist) {
    memset(value, 0, sizeof(*value));
    intp_status_t st = intp_evaluate(line, pos, value);
    if (st.code != INTP_STATUS_OK) {
        if (ist)
            *ist = st;
        *errpos = st.pos;
        return VG_GXP_PARSE_EVAL;
    }
    // intp_evaluate stops without an error at ',' or '$': anything left over is an error here
    if (!line_end(line, *pos)) {
        *errpos = *pos;
        return VG_GXP_PARSE_SYNTAX;
    }
    return VG_GXP_PARSE_OK;
}

#define VG_GXP_MAX_LINE 256

vg_gxp_parse_t vg_gxp_parse_line(const char *src, vg_gxp_line_t *out, uint32_t *errpos, intp_status_t *ist) {
    uint32_t pos;
    memset(out, 0, sizeof(*out));
    *errpos = 0;

    // Work on a copy that ends at a comment ('#' is never part of an expression)
    char line[VG_GXP_MAX_LINE];
    size_t len = strcspn(src, "#");
    if (len >= sizeof(line)) {
        *errpos = sizeof(line) - 1;
        return VG_GXP_PARSE_SYNTAX;
    }
    memcpy(line, src, len);
    line[len] = '\0';

    if (!strncasecmp(line, "gxplit:", 7)) {
        out->kind = VG_GXP_LINE_RULE;
        pos = 7;
    } else if (!strncasecmp(line, "gxp:", 4)) {
        out->kind = VG_GXP_LINE_PATCH;
        pos = 4;

        // Exactly 8 hex digits, then ':'
        uint32_t hash = 0;
        for (int i = 0; i < 8; i++) {
            int v = hexval(line[pos + i]);
            if (v < 0) {
                *errpos = pos + i;
                return VG_GXP_PARSE_SYNTAX;
            }
            hash = (hash << 4) | (uint32_t)v;
        }
        if (line[pos + 8] != ':' || hash == 0) {
            *errpos = line[pos + 8] != ':' ? pos + 8 : pos;
            return VG_GXP_PARSE_SYNTAX;
        }
        pos += 9;

        // Offset: starts with a digit, ends at a blank
        if (!isdigit((unsigned char)line[pos])) {
            *errpos = pos;
            return VG_GXP_PARSE_SYNTAX;
        }
        char *next = NULL;
        unsigned long long off = strtoull(&line[pos], &next, 0);
        if (next == &line[pos] || !isspace((unsigned char)*next) || off < VG_GXP_MIN_OFFSET
                || off > VG_GXP_MAX_SIZE) {
            *errpos = next && next != &line[pos] && !isspace((unsigned char)*next) ? (uint32_t)(next - line) : pos;
            return VG_GXP_PARSE_SYNTAX;
        }
        out->patch.hash = hash;
        out->patch.offset = (uint32_t)off;
        pos = (uint32_t)(next - line);
    } else {
        return VG_GXP_PARSE_SYNTAX;
    }

    while (isspace((unsigned char)line[pos]))
        pos++;

    // A single "=>", before VG_GXP_MAX_LEFT
    const char *arrow = strstr(&line[pos], "=>");
    if (!arrow) {
        *errpos = (uint32_t)strlen(line);
        return VG_GXP_PARSE_SYNTAX;
    }
    uint32_t apos = (uint32_t)(arrow - line);
    if (apos >= VG_GXP_MAX_LEFT) {
        *errpos = apos;
        return VG_GXP_PARSE_SYNTAX;
    }
    if (strstr(arrow + 2, "=>")) {
        *errpos = (uint32_t)(strstr(arrow + 2, "=>") - line);
        return VG_GXP_PARSE_SYNTAX;
    }

    // Stock side, on a copy that ends at "=>" (the interpreter cannot stop at '='); positions stay line-relative
    char left[VG_GXP_MAX_LEFT + 1];
    memcpy(left, line, apos);
    left[apos] = '\0';
    intp_value_t stock, data;
    uint32_t p = pos;
    vg_gxp_parse_t ret = eval_side(left, &p, &stock, errpos, ist);
    if (ret != VG_GXP_PARSE_OK)
        return ret;

    // New side
    p = apos + 2;
    while (isspace((unsigned char)line[p]))
        p++;
    ret = eval_side(line, &p, &data, errpos, ist);
    if (ret != VG_GXP_PARSE_OK)
        return ret;

    out->approximated = stock.approximated || data.approximated;

    uint8_t size = stock.size;
    if (size == 0 || size > VG_GXP_MAX_BYTES || data.size != size) {
        *errpos = apos;
        return VG_GXP_PARSE_SIZE;
    }
    uint32_t gaps = 0;
    for (uint8_t i = 0; i < size; i++) {
        if (stock.unk[i] != data.unk[i]) {
            *errpos = apos;
            return VG_GXP_PARSE_GAPS;
        }
        if (stock.unk[i])
            gaps |= 1u << i;
    }
    if (gaps == (size == 32 ? 0xFFFFFFFFu : (1u << size) - 1)) {
        *errpos = pos;
        return VG_GXP_PARSE_GAPS;
    }

    bool changed = false;
    for (uint8_t i = 0; i < size; i++) {
        if (!(gaps & (1u << i)) && stock.data.raw[i] != data.data.raw[i])
            changed = true;
    }

    if (out->kind == VG_GXP_LINE_RULE) {
        if (size % 4) {
            *errpos = apos;
            return VG_GXP_PARSE_SIZE;
        }
        if (gaps) {
            *errpos = pos;
            return VG_GXP_PARSE_GAPS;
        }
        out->rule.words = size / 4;
        for (uint8_t w = 0; w < out->rule.words; w++) {
            out->rule.stock[w] = rd32(&stock.data.raw[4 * w]);
            out->rule.data[w] = rd32(&data.data.raw[4 * w]);
        }
    } else {
        if ((uint64_t)out->patch.offset + size > VG_GXP_MAX_SIZE) {
            *errpos = 13;   // the offset
            return VG_GXP_PARSE_SYNTAX;
        }
        out->patch.size = size;
        out->patch.gap_mask = gaps;
        out->patch.active = changed;
        memcpy(out->patch.stock, stock.data.raw, size);
        memcpy(out->patch.data, data.data.raw, size);
    }

    return changed ? VG_GXP_PARSE_OK : VG_GXP_PARSE_UNCHANGED;
}

/*
 * Table
 */
void vg_gxp_table_reset(vg_gxp_table_t *t) {
    memset(t, 0, sizeof(*t));
}

// true when a word that rule w changes is a word rule s looks for (s may be w itself)
static bool rule_feeds(const vg_gxp_rule_t *w, const vg_gxp_rule_t *s) {
    for (uint8_t i = 0; i < w->words; i++) {
        if (w->data[i] == w->stock[i])
            continue;   // unchanged word
        for (uint8_t j = 0; j < s->words; j++) {
            if (w->data[i] == s->stock[j])
                return true;
        }
    }
    return false;
}

// true when the stock runs of a and b could claim the same literal entry: at some shift the
// overlapping words are equal (a == b included)
static bool rules_overlap(const vg_gxp_rule_t *a, const vg_gxp_rule_t *b) {
    for (int s = -(int)b->words + 1; s < (int)a->words; s++) {
        bool same = true;
        for (int j = 0; j < b->words && same; j++) {
            int i = s + j;
            if (i >= 0 && i < a->words)
                same = a->stock[i] == b->stock[j];
        }
        if (same)
            return true;
    }
    return false;
}

vg_gxp_add_t vg_gxp_table_add(vg_gxp_table_t *t, const vg_gxp_line_t *line) {
    if (line->kind == VG_GXP_LINE_RULE) {
        const vg_gxp_rule_t *n = &line->rule;
        if (t->rule_count >= VG_GXP_MAX_RULES)
            return VG_GXP_ADD_FULL;
        // Two rules that can claim the same entries would cancel each other on every program
        for (uint8_t i = 0; i < t->rule_count; i++) {
            if (rules_overlap(n, &t->r[i]))
                return VG_GXP_ADD_OVERLAP;
        }
        // A word a rule writes must never be a word a rule looks for, or a buffer registered
        // again could be rewritten twice
        if (rule_feeds(n, n))
            return VG_GXP_ADD_CHAIN;
        for (uint8_t i = 0; i < t->rule_count; i++) {
            if (rule_feeds(n, &t->r[i]) || rule_feeds(&t->r[i], n))
                return VG_GXP_ADD_CHAIN;
        }
        t->r[t->rule_count++] = *n;
        return VG_GXP_ADD_OK;
    }

    const vg_gxp_patch_t *n = &line->patch;
    if (t->count >= VG_GXP_MAX_PATCHES)
        return VG_GXP_ADD_FULL;
    // Lines of one program may not overlap, whether or not they change anything at this config
    for (uint16_t i = 0; i < t->count; i++) {
        const vg_gxp_patch_t *o = &t->e[i];
        if (o->hash == n->hash && n->offset < o->offset + o->size && o->offset < n->offset + n->size)
            return VG_GXP_ADD_OVERLAP;
    }
    t->e[t->count++] = *n;
    if (n->active)
        t->active++;
    return VG_GXP_ADD_OK;
}

static void survey_add(vg_gxp_table_t *t, uint32_t w) {
    for (uint8_t i = 0; i < t->survey_count; i++) {
        if (t->survey[i] == w)
            return;
    }
    if (t->survey_count < VG_GXP_MAX_SURVEY)
        t->survey[t->survey_count++] = w;
}

void vg_gxp_table_seal(vg_gxp_table_t *t) {
    // Insertion sort by (hash, offset)
    for (uint16_t i = 1; i < t->count; i++) {
        vg_gxp_patch_t x = t->e[i];
        int j = i - 1;
        while (j >= 0 && (t->e[j].hash > x.hash || (t->e[j].hash == x.hash && t->e[j].offset > x.offset))) {
            t->e[j + 1] = t->e[j];
            j--;
        }
        t->e[j + 1] = x;
    }

    // Program slots, counted over the entries that change something
    t->programs = 0;
    uint32_t last = 0;
    bool have = false;
    for (uint16_t i = 0; i < t->count; i++) {
        if (!have || t->e[i].hash != last) {
            last = t->e[i].hash;
            have = true;
            bool any = false;
            for (uint16_t k = i; k < t->count && t->e[k].hash == last; k++)
                any |= t->e[k].active;
            if (any)
                t->programs++;
        }
        t->e[i].prog = (uint8_t)(t->programs ? t->programs - 1 : 0);
    }

    // Words to report when a program holds them but nothing matched
    t->survey_count = 0;
    for (uint8_t i = 0; i < t->rule_count; i++) {
        for (uint8_t w = 0; w < t->r[i].words; w++)
            survey_add(t, t->r[i].stock[w]);
    }
    for (uint16_t i = 0; i < t->count; i++) {
        const vg_gxp_patch_t *e = &t->e[i];
        for (uint8_t b = 0; b + 4 <= e->size; b += 4) {
            if ((e->offset + b) % 4 == 0 && !(e->gap_mask & (0xFu << b)))
                survey_add(t, rd32(&e->stock[b]));
        }
    }
    t->sealed = true;
}

/*
 * Matching (read-only)
 */
static int find_first(const vg_gxp_table_t *t, uint32_t hash) {
    int lo = 0, hi = (int)t->count;
    while (lo < hi) {
        int mid = (lo + hi) / 2;
        if (t->e[mid].hash < hash)
            lo = mid + 1;
        else
            hi = mid;
    }
    return lo < t->count && t->e[lo].hash == hash ? lo : -1;
}

static bool bytes_equal(const uint8_t *prog, const vg_gxp_patch_t *e, const uint8_t *ref, uint32_t *bad) {
    for (uint8_t b = 0; b < e->size; b++) {
        if (e->gap_mask & (1u << b))
            continue;
        if (prog[e->offset + b] != ref[b]) {
            if (bad)
                *bad = e->offset + b;
            return false;
        }
    }
    return true;
}

static uint8_t nongap_bytes(const vg_gxp_patch_t *e) {
    uint8_t n = 0;
    for (uint8_t b = 0; b < e->size; b++) {
        if (!(e->gap_mask & (1u << b)))
            n++;
    }
    return n;
}

static vg_gxp_status_t match_rules(const vg_gxp_table_t *t, const uint8_t *p, vg_gxp_result_t *r) {
    uint32_t toff, tcount;
    if (!vg_gxp_literal_table(p, r->size, &toff, &tcount))
        return GXP_NO_MATCH;

    uint32_t claimed_lo[VG_GXP_MAX_RULE_SITES], claimed_hi[VG_GXP_MAX_RULE_SITES];
    uint32_t total = 0;
    for (uint32_t i = 0; i < tcount; i++) {
        for (uint8_t k = 0; k < t->rule_count; k++) {
            const vg_gxp_rule_t *rule = &t->r[k];
            if (i + rule->words > tcount)
                continue;
            uint32_t reg0 = rd32(p + toff + 8 * i);
            bool ok = true;
            for (uint8_t w = 0; w < rule->words && ok; w++) {
                const uint8_t *ent = p + toff + 8 * (i + w);
                ok = rd32(ent) == reg0 + w && rd32(ent + 4) == rule->stock[w];
            }
            if (!ok)
                continue;
            // Every run is found on the unmodified bytes; two runs sharing an entry write nothing
            for (uint32_t s = 0; s < total && s < VG_GXP_MAX_RULE_SITES; s++) {
                if (i < claimed_hi[s] && claimed_lo[s] < i + rule->words) {
                    r->status = GXP_RULE_CONFLICT;
                    r->bad_offset = toff + 8 * i;
                    return GXP_RULE_CONFLICT;
                }
            }
            if (total >= VG_GXP_MAX_RULE_SITES) {
                r->status = GXP_RULE_TOO_MANY;
                return GXP_RULE_TOO_MANY;
            }
            claimed_lo[total] = i;
            claimed_hi[total] = i + rule->words;
            r->site[total].rule = k;
            r->site[total].offset = toff + 8 * i;
            total++;
        }
    }
    if (!total)
        return GXP_NO_MATCH;
    r->nsite = (uint8_t)total;
    r->sites = (uint16_t)total;
    r->bytes = 0;
    for (uint8_t s = 0; s < r->nsite; s++)
        r->bytes += 4 * t->r[r->site[s].rule].words;
    r->status = GXP_RULE_MATCHED;
    return GXP_RULE_MATCHED;
}

static vg_gxp_status_t survey(const vg_gxp_table_t *t, const uint8_t *p, vg_gxp_result_t *r) {
    uint32_t toff, tcount;
    if (!t->survey_count || !vg_gxp_literal_table(p, r->size, &toff, &tcount))
        return GXP_NO_MATCH;
    for (uint32_t i = 0; i < tcount; i++) {
        uint32_t v = rd32(p + toff + 8 * i + 4);
        for (uint8_t k = 0; k < t->survey_count; k++) {
            if (v == t->survey[k]) {
                r->survey_word = v;
                r->survey_offset = toff + 8 * i + 4;
                r->status = GXP_SURVEY;
                return GXP_SURVEY;
            }
        }
    }
    return GXP_NO_MATCH;
}

vg_gxp_status_t vg_gxp_match(const vg_gxp_table_t *t, const void *prog, vg_gxp_result_t *r) {
    const uint8_t *p = (const uint8_t *)prog;
    memset(r, 0, sizeof(*r));
    r->status = GXP_NOT_PROGRAM;
    if (!prog || !vg_gxp_program_size(prog, &r->size))
        return GXP_NOT_PROGRAM;
    r->hash = vg_gxp_fnv1a(prog, r->size);
    r->status = GXP_NO_MATCH;

    int first = find_first(t, r->hash);
    if (first >= 0) {
        // A named program: its lines decide, all or nothing; rules never look at it
        r->first = (uint16_t)first;
        uint16_t n = 0;
        while (first + n < t->count && t->e[first + n].hash == r->hash)
            n++;
        r->count = n;
        for (uint16_t i = 0; i < n; i++) {
            const vg_gxp_patch_t *e = &t->e[first + i];
            if (!e->active)
                continue;
            if ((uint64_t)e->offset + e->size > r->size) {
                r->bad_offset = e->offset;
                r->status = GXP_OUT_OF_RANGE;
                return r->status;
            }
            if (!bytes_equal(p, e, e->stock, &r->bad_offset)) {
                r->status = GXP_STOCK_MISMATCH;
                return r->status;
            }
            r->sites++;
            r->bytes += nongap_bytes(e);
        }
        r->status = r->sites ? GXP_MATCHED : GXP_NO_MATCH;
        return r->status;
    }

    if (t->rule_count) {
        vg_gxp_status_t st = match_rules(t, p, r);
        if (st != GXP_NO_MATCH)
            return st;
    }
    return survey(t, p, r);
}

vg_gxp_status_t vg_gxp_recheck(const vg_gxp_table_t *t, const void *prog, vg_gxp_result_t *r) {
    const uint8_t *p = (const uint8_t *)prog;
    uint32_t stock = 0, patched = 0, sites = 0;

    if (r->status == GXP_MATCHED) {
        for (uint16_t i = 0; i < r->count; i++) {
            const vg_gxp_patch_t *e = &t->e[r->first + i];
            if (!e->active)
                continue;
            sites++;
            if (bytes_equal(p, e, e->stock, &r->bad_offset))
                stock++;
            else if (bytes_equal(p, e, e->data, NULL))
                patched++;
        }
        if (stock == sites)
            return GXP_MATCHED;
        return patched == sites ? GXP_ALREADY_PATCHED : GXP_STOCK_MISMATCH;
    }

    if (r->status == GXP_RULE_MATCHED) {
        for (uint8_t s = 0; s < r->nsite; s++) {
            const vg_gxp_rule_t *rule = &t->r[r->site[s].rule];
            bool is_stock = true, is_data = true;
            for (uint8_t w = 0; w < rule->words; w++) {
                uint32_t v = rd32(p + r->site[s].offset + 8 * w + 4);
                is_stock &= v == rule->stock[w];
                is_data &= v == rule->data[w];
            }
            sites++;
            if (is_stock)
                stock++;
            else if (is_data)
                patched++;
            else
                r->bad_offset = r->site[s].offset + 4;
        }
        if (stock == sites)
            return GXP_RULE_MATCHED;
        return patched == sites ? GXP_ALREADY_PATCHED : GXP_STOCK_MISMATCH;
    }

    return r->status;
}

void vg_gxp_commit(const vg_gxp_table_t *t, void *prog, const vg_gxp_result_t *r) {
    uint8_t *p = (uint8_t *)prog;
    if (r->status == GXP_MATCHED) {
        for (uint16_t i = 0; i < r->count; i++) {
            const vg_gxp_patch_t *e = &t->e[r->first + i];
            if (!e->active)
                continue;
            for (uint8_t b = 0; b < e->size; b++) {
                if (!(e->gap_mask & (1u << b)))
                    p[e->offset + b] = e->data[b];
            }
        }
    } else if (r->status == GXP_RULE_MATCHED) {
        for (uint8_t s = 0; s < r->nsite; s++) {
            const vg_gxp_rule_t *rule = &t->r[r->site[s].rule];
            for (uint8_t w = 0; w < rule->words; w++)
                wr32(p + r->site[s].offset + 8 * w + 4, rule->data[w]);
        }
    }
}
