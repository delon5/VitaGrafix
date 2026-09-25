#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <strings.h>
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../src/interpreter/interpreter.h"
#include "../src/patch_gxp_core.h"

#define LINE_SIZE 4096

static const char *skip_ws(const char *text) {
    while (isspace((unsigned char)*text)) {
        text++;
    }
    return text;
}

static bool parse_patch_line(const char *line, uint8_t *segment, uint32_t *offset, const char **expression) {
    char *end;
    errno = 0;
    unsigned long parsed_segment = strtoul(line, &end, 10);
    if (errno || end == line || parsed_segment > UINT8_MAX || *end != ':') {
        return false;
    }

    line = end + 1;
    errno = 0;
    unsigned long parsed_offset = strtoul(line, &end, 0);
    if (errno || end == line || parsed_offset > UINT32_MAX || !isspace((unsigned char)*end)) {
        return false;
    }

    *expression = skip_ws(end);
    if (**expression == '\0') {
        return false;
    }

    *segment = parsed_segment;
    *offset = parsed_offset;
    return true;
}

static bool parse_resolution(const char *text, const char **end, uint16_t *width, uint16_t *height) {
    char *next;
    errno = 0;
    unsigned long parsed_width = strtoul(text, &next, 10);
    if (errno || next == text || parsed_width == 0 || parsed_width > UINT16_MAX || (*next != 'x' && *next != 'X')) {
        return false;
    }

    text = next + 1;
    errno = 0;
    unsigned long parsed_height = strtoul(text, &next, 10);
    if (errno || next == text || parsed_height == 0 || parsed_height > UINT16_MAX) {
        return false;
    }

    *width = parsed_width;
    *height = parsed_height;
    *end = next;
    return true;
}

static bool parse_ib_list(const char *text, intp_vg_context_t *context) {
    uint8_t count = 0;
    while (*text != '\0') {
        if (count >= INTP_VG_MAX_RES_COUNT) {
            return false;
        }

        const char *end;
        if (!parse_resolution(text, &end, &context->ib_width[count], &context->ib_height[count])) {
            return false;
        }
        count++;
        if (*end == '\0') {
            break;
        }
        if (*end != ',') {
            return false;
        }
        text = end + 1;
    }

    if (count == 0) {
        return false;
    }
    for (; count < INTP_VG_MAX_RES_COUNT; count++) {
        context->ib_width[count] = context->ib_width[count - 1];
        context->ib_height[count] = context->ib_height[count - 1];
    }
    return true;
}

static bool parse_fps(const char *text, intp_vg_context_t *context) {
    if (!strcmp(text, "60")) {
        context->vblank = 1;
        context->fps_limit = 60;
    } else if (!strcmp(text, "30")) {
        context->vblank = 2;
        context->fps_limit = 30;
    } else if (!strcmp(text, "20")) {
        context->vblank = 3;
        context->fps_limit = 20;
    } else {
        return false;
    }
    return true;
}

static bool parse_msaa(const char *text, intp_vg_context_t *context) {
    if (!strcmp(text, "0") || !strcmp(text, "1") || !strcmp(text, "2")) {
        context->msaa = text[0] - '0';
        context->msaa_enabled = context->msaa > 0;
        return true;
    }
    return false;
}

static void write_result(unsigned int line_number, uint8_t segment,
        uint32_t offset, intp_status_t status, const intp_value_t *value) {
    fprintf(stdout, "%05u %u:%08X ", line_number, segment, offset);
    if (status.code != INTP_STATUS_OK) {
        fprintf(stdout, "ERR %d %u\n", status.code, status.pos);
        return;
    }

    fprintf(stdout, "OK %d %u", value->type, value->size);
    for (uint8_t i = 0; i < value->size; i++) {
        fprintf(stdout, " %02X", value->data.raw[i]);
    }
    fputc('\n', stdout);
}

/*
 * Shader literal lines (gxp: / gxplit:), checked with the plugin's own parser
 */
static vg_gxp_table_t g_gxp;
static bool g_expect_stock = false;
static bool g_rules_only = false;

static void print_bytes(const uint8_t *b, uint8_t size, uint32_t gaps) {
    for (uint8_t i = 0; i < size; i++) {
        if (gaps & (1u << i))
            fprintf(stdout, " ??");
        else
            fprintf(stdout, " %02X", b[i]);
    }
}

static bool handle_gxp_line(unsigned int line_number, const char *line) {
    vg_gxp_line_t l;
    uint32_t pos = 0;
    intp_status_t ist = {0};
    vg_gxp_parse_t pr = vg_gxp_parse_line(line, &l, &pos, &ist);
    bool rule = !strncasecmp(line, "gxplit:", 7);

    if (rule)
        fprintf(stdout, "%05u gxplit ", line_number);
    else
        fprintf(stdout, "%05u gxp:%08X:%08X ", line_number, l.patch.hash, l.patch.offset);

    if (pr != VG_GXP_PARSE_OK && pr != VG_GXP_PARSE_UNCHANGED) {
        fprintf(stdout, "ERR %d %u\n", pr == VG_GXP_PARSE_EVAL ? (int)ist.code : 100 + (int)pr, pos);
        return false;
    }

    if (rule) {
        fprintf(stdout, "OK %u", 4 * l.rule.words);
        for (uint8_t w = 0; w < l.rule.words; w++)
            fprintf(stdout, " %08X", l.rule.data[w]);
        fprintf(stdout, " stock");
        for (uint8_t w = 0; w < l.rule.words; w++)
            fprintf(stdout, " %08X", l.rule.stock[w]);
    } else {
        fprintf(stdout, "OK %u", l.patch.size);
        print_bytes(l.patch.data, l.patch.size, l.patch.gap_mask);
        fprintf(stdout, " stock");
        print_bytes(l.patch.stock, l.patch.size, l.patch.gap_mask);
    }
    fputc('\n', stdout);

    bool ok = true;
    if (g_expect_stock && pr != VG_GXP_PARSE_UNCHANGED) {
        fprintf(stdout, "%05u ERR stock: the new bytes differ from the stock bytes at this resolution\n", line_number);
        ok = false;
    }
    if (rule && pr == VG_GXP_PARSE_UNCHANGED)
        return ok;   // the plugin queues nothing for it
    if (!rule && g_rules_only)
        return ok;

    l.rule.ordinal = (uint16_t)line_number;
    l.patch.ordinal = (uint16_t)line_number;
    vg_gxp_add_t ar = vg_gxp_table_add(&g_gxp, &l);
    if (ar == VG_GXP_ADD_CHAIN) {
        // The plugin drops such a rule at this resolution and goes on
        fprintf(stdout, "%05u gxplit skipped: at this resolution it writes a word a rule looks for\n", line_number);
        return ok;
    }
    if (ar != VG_GXP_ADD_OK) {
        fprintf(stdout, "%05u ERR %s\n", line_number, ar == VG_GXP_ADD_OVERLAP ? "overlap" : "full");
        return false;
    }
    return ok;
}

static uint8_t *load_file(const char *path, uint32_t *size) {
    FILE *f = fopen(path, "rb");
    if (!f)
        return NULL;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (n <= 0 || n > VG_GXP_MAX_SIZE) {
        fclose(f);
        return NULL;
    }
    uint8_t *b = malloc((size_t)n);
    if (b && fread(b, 1, (size_t)n, f) != (size_t)n) {
        free(b);
        b = NULL;
    }
    fclose(f);
    *size = (uint32_t)n;
    return b;
}

static bool named(uint32_t hash) {
    for (uint16_t i = 0; i < g_gxp.count; i++) {
        if (g_gxp.e[i].hash == hash)
            return true;
    }
    return false;
}

// Checks every gxp line against DIR/<HASH>.gxp and runs the rules over every other program there
static unsigned int check_gxp_dir(const char *dir) {
    unsigned int errors = 0;
    char path[4096];
    vg_gxp_table_seal(&g_gxp);

    for (uint16_t i = 0; i < g_gxp.count; i++) {
        if (i && g_gxp.e[i].hash == g_gxp.e[i - 1].hash)
            continue;
        uint32_t hash = g_gxp.e[i].hash, size;
        snprintf(path, sizeof(path), "%s/%08X.gxp", dir, hash);
        uint8_t *b = load_file(path, &size);
        if (!b) {
            fprintf(stdout, "gxp:%08X ERR gxp-missing\n", hash);
            errors++;
            continue;
        }
        uint32_t psize;
        if (size < 12 || !vg_gxp_program_size(b, &psize) || psize > size) {
            fprintf(stdout, "gxp:%08X ERR gxp-short (not a whole GXP program)\n", hash);
            errors++;
            free(b);
            continue;
        }
        // Stock bytes and range of every line of this program, including those unchanged at this --ib
        bool bad = false;
        for (uint16_t k = i; k < g_gxp.count && g_gxp.e[k].hash == hash && !bad; k++) {
            const vg_gxp_patch_t *e = &g_gxp.e[k];
            if ((uint64_t)e->offset + e->size > psize) {
                fprintf(stdout, "gxp:%08X ERR gxp-range +0x%X\n", hash, e->offset);
                bad = true;
                break;
            }
            for (uint8_t x = 0; x < e->size; x++) {
                if (!(e->gap_mask & (1u << x)) && b[e->offset + x] != e->stock[x]) {
                    fprintf(stdout, "gxp:%08X ERR gxp-stock +0x%X\n", hash, e->offset + x);
                    bad = true;
                    break;
                }
            }
        }
        if (bad) {
            errors++;
            free(b);
            continue;
        }
        vg_gxp_result_t r;
        vg_gxp_status_t st = vg_gxp_match(&g_gxp, b, &r);
        if (st == GXP_NOT_PROGRAM || r.hash != hash) {
            fprintf(stdout, "gxp:%08X ERR gxp-hash (file hashes to %08X)\n", hash, r.hash);
            errors++;
        } else if (st == GXP_OUT_OF_RANGE) {
            fprintf(stdout, "gxp:%08X ERR gxp-range +0x%X\n", hash, r.bad_offset);
            errors++;
        } else if (st == GXP_STOCK_MISMATCH) {
            fprintf(stdout, "gxp:%08X ERR gxp-stock +0x%X\n", hash, r.bad_offset);
            errors++;
        } else if (st == GXP_MATCHED) {
            vg_gxp_commit(&g_gxp, b, &r);
            fprintf(stdout, "gxp:%08X applied %u sites %u bytes\n", hash, r.sites, r.bytes);
        } else {
            fprintf(stdout, "gxp:%08X stock at this resolution\n", hash);
        }
        free(b);
    }

    DIR *d = opendir(dir);
    if (!d) {
        perror(dir);
        return errors + 1;
    }
    struct dirent *de;
    while ((de = readdir(d)) != NULL) {
        size_t n = strlen(de->d_name);
        if (n != 12 || strcasecmp(de->d_name + 8, ".gxp"))
            continue;
        uint32_t hash = (uint32_t)strtoul(de->d_name, NULL, 16), size;
        if (named(hash))
            continue;
        snprintf(path, sizeof(path), "%s/%s", dir, de->d_name);
        uint8_t *b = load_file(path, &size);
        if (!b)
            continue;
        uint32_t psize;
        if (size < 12 || !vg_gxp_program_size(b, &psize) || psize > size) {
            free(b);
            continue;
        }
        vg_gxp_result_t r;
        vg_gxp_status_t st = vg_gxp_match(&g_gxp, b, &r);
        if (st == GXP_RULE_MATCHED) {
            for (uint8_t s = 0; s < r.nsite; s++)
                fprintf(stdout, "gxplit-site %u %08X +0x%X\n", g_gxp.r[r.site[s].rule].ordinal, r.hash,
                        r.site[s].offset + 4);
        } else if (st == GXP_SURVEY) {
            fprintf(stdout, "survey %08X %08X +0x%X\n", r.hash, r.survey_word, r.survey_offset);
        } else if (st == GXP_RULE_CONFLICT || st == GXP_RULE_TOO_MANY) {
            fprintf(stdout, "gxplit %08X ERR %s\n", r.hash, st == GXP_RULE_CONFLICT ? "conflict" : "too-many");
            errors++;
        }
        free(b);
    }
    closedir(d);
    return errors;
}

int main(int argc, char *argv[]) {
    if (argc < 2) {
        fprintf(stderr, "Usage: %s <patchlist.txt> [--fb WIDTHxHEIGHT|off] [--ib WIDTHxHEIGHT[,WIDTHxHEIGHT...]|off] [--fps 20|30|60] [--msaa 0|1|2]\n"
                        "       [--expect-stock] [--gxp-dir DIR [--rules-only]]\n", argv[0]);
        return 2;
    }

    intp_vg_context_t context = {0};
    context.fb_width = 960;
    context.fb_height = 544;
    for (int i = 0; i < INTP_VG_MAX_RES_COUNT; i++) {
        context.ib_width[i] = 960;
        context.ib_height[i] = 544;
    }
    context.vblank = 1;
    context.fps_limit = 60;
    context.msaa = 2;
    context.msaa_enabled = true;

    const char *gxp_dir = NULL;
    for (int i = 2; i < argc; i += 2) {
        if (!strcmp(argv[i], "--expect-stock")) {
            g_expect_stock = true;
            i--;
            continue;
        }
        if (!strcmp(argv[i], "--rules-only")) {
            g_rules_only = true;
            i--;
            continue;
        }
        if (i + 1 >= argc) {
            fprintf(stderr, "Missing value for %s.\n", argv[i]);
            return 2;
        }

        if (!strcmp(argv[i], "--fb")) {
            // A disabled option still evaluates to the native size, as the plugin does
            if (!strcmp(argv[i + 1], "off")) {
                context.fb_width = 960;
                context.fb_height = 544;
            } else {
                const char *end;
                if (!parse_resolution(argv[i + 1], &end, &context.fb_width, &context.fb_height)
                        || *end != '\0') {
                    fprintf(stderr, "Invalid framebuffer resolution: %s\n", argv[i + 1]);
                    return 2;
                }
            }
        } else if (!strcmp(argv[i], "--ib")) {
            if (!strcmp(argv[i + 1], "off")) {
                for (int j = 0; j < INTP_VG_MAX_RES_COUNT; j++) {
                    context.ib_width[j] = 960;
                    context.ib_height[j] = 544;
                }
            } else if (!parse_ib_list(argv[i + 1], &context)) {
                fprintf(stderr, "Invalid internal-buffer resolution list: %s\n", argv[i + 1]);
                return 2;
            }
        } else if (!strcmp(argv[i], "--fps")) {
            if (!parse_fps(argv[i + 1], &context)) {
                fprintf(stderr, "Invalid frame rate: %s\n", argv[i + 1]);
                return 2;
            }
        } else if (!strcmp(argv[i], "--gxp-dir")) {
            gxp_dir = argv[i + 1];
        } else if (!strcmp(argv[i], "--msaa")) {
            if (!parse_msaa(argv[i + 1], &context)) {
                fprintf(stderr, "Invalid MSAA value: %s\n", argv[i + 1]);
                return 2;
            }
        } else {
            fprintf(stderr, "Unknown option: %s\n", argv[i]);
            return 2;
        }
    }

    FILE *input = fopen(argv[1], "r");
    if (input == NULL) {
        perror(argv[1]);
        return 2;
    }

    intp_set_vg_context(&context);

    char line[LINE_SIZE];
    unsigned int line_number = 0;
    unsigned int patch_count = 0;
    unsigned int error_count = 0;
    while (fgets(line, sizeof(line), input) != NULL) {
        line_number++;

        size_t length = strlen(line);
        if (length == sizeof(line) - 1 && line[length - 1] != '\n') {
            fprintf(stderr, "%s:%u: line is too long\n", argv[1], line_number);
            error_count++;
            int character;
            while ((character = fgetc(input)) != '\n' && character != EOF) {}
            continue;
        }

        line[strcspn(line, "\r\n#")] = '\0';
        const char *patch_line = skip_ws(line);
        if (*patch_line == '[' && !gxp_dir)
            vg_gxp_table_reset(&g_gxp);   // each section has its own shader lines (--gxp-dir checks one game)
        if (!strncasecmp(patch_line, "gxp:", 4) || !strncasecmp(patch_line, "gxplit:", 7)) {
            patch_count++;
            if (!handle_gxp_line(line_number, patch_line))
                error_count++;
            continue;
        }
        uint8_t segment;
        uint32_t offset;
        const char *expression;
        if (!parse_patch_line(patch_line, &segment, &offset, &expression)) {
            continue;
        }

        intp_value_t value = {0};
        uint32_t position = 0;
        intp_status_t status = intp_evaluate(expression, &position, &value);
        write_result(line_number, segment, offset, status, &value);
        patch_count++;
        if (status.code != INTP_STATUS_OK) {
            error_count++;
        }
    }

    if (ferror(input)) {
        perror(argv[1]);
        error_count++;
    }
    fclose(input);

    if (gxp_dir)
        error_count += check_gxp_dir(gxp_dir);

    fprintf(stderr, "%u patch expressions written (%u errors)\n", patch_count, error_count);
    return error_count == 0 ? 0 : 1;
}
