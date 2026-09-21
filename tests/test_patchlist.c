/*
 * Applies a VitaGrafix patch file to a configuration on the host: every
 * '<seg>:<offset> <expression>' line is evaluated with the patch interpreter,
 * and every '>' hook directive is parsed and installed with the plugin's own
 * parser (src/patch_hook.c, compiled against the stub vitasdk/taihen headers
 * in tests/stubs, with taiHEN faked below). A directive is therefore checked
 * by exactly the code that will run it on hardware, and is reported with the
 * arguments it resolved to at this configuration.
 */
#include <errno.h>
#include <limits.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <vitasdk.h>
#include <taihen.h>

#include "../src/io.h"
#include "../src/config.h"
#include "../src/patch.h"
#include "../src/patch_hook.h"
#include "../src/main.h"
#include "../src/interpreter/interpreter.h"

// isspace() and friends are declared by ../src/main.h, as they are for the
// plugin sources; <ctype.h> must not be included on top of that

#define LINE_SIZE 4096

// ---------------------------------------------------------------- plugin glue

vg_main_t g_main;

static vg_config_t g_config;
vg_config_t *vg_config_get() { return &g_config; }

// The plugin's log, on stderr when VG_LOG is set in the environment. Off by
// default so that the tool's output stays exactly the report below.
static bool g_log_enabled = false;
void vg_log_printf(const char *format, ...) {
    if (!g_log_enabled)
        return;
    va_list args;
    va_start(args, format);
    vfprintf(stderr, format, args);
    va_end(args);
}

// src/io.c and src/patch_hook.c need these to link; nothing here calls them
SceUID sceIoOpen(const char *file, int flags, int mode) { (void)file; (void)flags; (void)mode; return -1; }
int sceIoClose(SceUID fd) { (void)fd; return 0; }
int sceIoRead(SceUID fd, void *data, SceSize size) { (void)fd; (void)data; (void)size; return 0; }
int sceIoWrite(SceUID fd, const void *data, SceSize size) { (void)fd; (void)data; (void)size; return size; }
long long sceIoLseek(SceUID fd, long long offset, int whence) { (void)fd; (void)offset; (void)whence; return 0; }
int sceIoMkdir(const char *dir, int mode) { (void)dir; (void)mode; return 0; }
int sceDisplayWaitVblankStartMulti(unsigned int vcount) { (void)vcount; return 0; }
int sceCtrlPeekBufferPositive(int port, SceCtrlData *pad_data, int count) { (void)port; (void)pad_data; return count; }
int sceCtrlPeekBufferPositive2(int port, SceCtrlData *pad_data, int count) { (void)port; (void)pad_data; return count; }

// ------------------------------------------------------------- fake taiHEN

// Hooks are recorded, never armed: the checker installs a directive so that
// the plugin's install time checks (duplicate address, conflicting arguments,
// slot exhaustion, target inside the segment) run over the whole file, but no
// hook body is ever entered here.
#define FAKE_HOOK_MAX 64
static struct _tai_hook_user g_fake_node[FAKE_HOOK_MAX];
static int g_fake_num = 0;

static SceUID fake_hook_new(tai_hook_ref_t *p_hook, const void *hook_func) {
    if (g_fake_num >= FAKE_HOOK_MAX)
        return -1;
    struct _tai_hook_user *node = &g_fake_node[g_fake_num];
    node->next = 0;
    node->func = (void *)hook_func;
    node->old = NULL;
    *p_hook = (tai_hook_ref_t)node;
    return 0x1000 + g_fake_num++;
}

SceUID taiHookFunctionImport(tai_hook_ref_t *p_hook, void *module, uint32_t library_nid,
        uint32_t func_nid, const void *hook_func) {
    (void)module; (void)library_nid; (void)func_nid;
    return fake_hook_new(p_hook, hook_func);
}

SceUID taiHookFunctionOffset(tai_hook_ref_t *p_hook, SceUID modid, int segidx, uint32_t offset,
        int thumb, const void *hook_func) {
    (void)modid; (void)segidx; (void)offset; (void)thumb;
    return fake_hook_new(p_hook, hook_func);
}

int taiHookRelease(SceUID tai_uid, tai_hook_ref_t hook) { (void)tai_uid; (void)hook; return 0; }

// ---------------------------------------------------------------- the checker

// Feature blocks, spelled as src/patch.c's _FEATURE_TOKENS spells them. Only
// the block a '>' line sits under decides whether the plugin parses it at all,
// and a directive outside @FPS is refused by vg_hook_parse_request().
static const vg_patch_feature_token_t _FEATURE_TOKENS[FEATURE_INVALID] = {
    {"@FB",   FEATURE_FB},
    {"@IB",   FEATURE_IB},
    {"@FPS",  FEATURE_FPS},
    {"@MSAA", FEATURE_MSAA}
};

static vg_feature_t parse_feature(const char *line) {
    for (int i = 0; i < FEATURE_INVALID; i++) {
        size_t length = strlen(_FEATURE_TOKENS[i].name);
        if (!strncasecmp(line, _FEATURE_TOKENS[i].name, length) && vg_io_is_line_end(line, length)) {
            return _FEATURE_TOKENS[i].type;
        }
    }
    return FEATURE_INVALID;
}

/**
 * Only one section of a patch file is ever live: the plugin applies the one
 * whose module matches, so hooks installed for the previous section must not
 * be held against the next one.
 */
static void reset_hook_state() {
    g_fake_num = 0;
    g_main.rate_hook_num = 0;
    memset(g_main.rate_hook, 0, sizeof(g_main.rate_hook));
    for (int i = 0; i < MAX_HOOK_NUM; i++) {
        g_main.hook[i] = -1;
        g_main.hook_ref[i] = 0;
    }
    g_main.frame = 0;
}

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

/**
 * '--seg INDEX:SIZE': the memsz of one segment of the module the patch file is
 * for. src/patch_hook.c refuses a hook target that is not inside its segment,
 * and without this it has no module info to check against.
 */
static bool parse_segment_size(const char *text) {
    char *next;
    errno = 0;
    unsigned long index = strtoul(text, &next, 10);
    if (errno || next == text || *next != ':'
            || index >= sizeof(g_main.sce_info.segments) / sizeof(g_main.sce_info.segments[0])) {
        return false;
    }

    text = next + 1;
    errno = 0;
    unsigned long long size = strtoull(text, &next, 0);
    if (errno || next == text || *next != '\0' || size == 0 || size > UINT32_MAX) {
        return false;
    }

    g_main.sce_info.segments[index].memsz = size;
    return true;
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

static void write_hook_error(unsigned int line_number, vg_io_status_t status, const char *line) {
    fprintf(stdout, "%05u HOOK ERR %d %u %s | %s\n", line_number, status.code, status.pos_line,
                vg_io_status_code_to_string(status.code), line);
}

/**
 * Reports a directive by what it resolved to at this configuration - the
 * request the plugin would act on, not the text of the line - and whether the
 * plugin would install it here.
 */
static void write_hook_result(unsigned int line_number, const vg_hook_request_t *request,
            uint8_t shall_hook) {
    fprintf(stdout, "%05u HOOK OK %s ", line_number, request->name);

    if (request->kind == HOOK_KIND_RATE_DIVIDE) {
        fprintf(stdout, "%u:%08X divisor=%u ret=0x%X %s %s args=%u ",
                    request->segment, request->offset, request->divisor, request->ret_value,
                    request->thumb ? "thumb" : "arm",
                    request->frame_counted ? "frame" : "call", request->arg_num);
    } else {
        fprintf(stdout, "nid=0x%08X ", request->import_nid);
    }

    fprintf(stdout, "install=%s\n", shall_hook ? "yes" : "no");
}

/**
 * Parses and installs one '>' directive with the plugin's own parser, so that
 * the checker cannot disagree with the plugin about what a directive means.
 * Returns false when the directive would fail on hardware.
 */
static bool check_hook_directive(unsigned int line_number, const char *line,
            vg_feature_t feature, bool in_section) {
    // The plugin only looks at lines inside the section of the module it is
    // running in, and only under a feature block. A directive outside either
    // is never parsed, never installed and never reported - dead text that
    // looks like a patch, so the checker refuses it.
    if (!in_section) {
        fprintf(stdout, "%05u HOOK ERR - - Hook directive outside of a [section] header. | %s\n",
                    line_number, line);
        return false;
    }

    vg_hook_request_t request = {0};
    uint8_t shall_hook = 0;

    vg_io_status_t status = vg_hook_parse_request(line, feature, &request, &shall_hook);
    if (status.code == IO_OK) {
        // Install it as well (against the fake taiHEN above): a duplicate
        // address, two lines that disagree about one address, more directives
        // than there are slots and a target outside its segment are all
        // decided here rather than at parse time
        status = vg_hook_apply_request(&request, shall_hook);
    }

    if (status.code != IO_OK) {
        write_hook_error(line_number, status, line);
        return false;
    }

    write_hook_result(line_number, &request, shall_hook);
    return true;
}

int main(int argc, char *argv[]) {
    if (argc < 2) {
        fprintf(stderr, "Usage: %s <patchlist.txt> [--fb WIDTHxHEIGHT|off] [--ib WIDTHxHEIGHT[,WIDTHxHEIGHT...]|off] [--fps 20|30|60] [--msaa 0|1|2] [--seg INDEX:SIZE]\n", argv[0]);
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

    // The configuration the hook directives are resolved against. Every
    // feature is on: a directive under a disabled block is not parsed by the
    // plugin at all, which is exactly how a typo in one stays invisible until
    // a user turns that option on, so the checker always parses it and reports
    // whether this configuration would install it.
    g_config.enabled = FT_ENABLED;
    g_config.fb_enabled = FT_ENABLED;
    g_config.ib_enabled = FT_ENABLED;
    g_config.fps_enabled = FT_ENABLED;
    g_config.msaa_enabled = FT_ENABLED;
    g_config.fps = FPS_60;
    g_log_enabled = getenv("VG_LOG") != NULL;
    reset_hook_state();

    for (int i = 2; i < argc; i += 2) {
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
            g_config.fps = context.fps_limit == 60 ? FPS_60 : context.fps_limit == 30 ? FPS_30 : FPS_20;
        } else if (!strcmp(argv[i], "--seg")) {
            // Segment sizes of the module being patched, so that the plugin's
            // check of a hook target against its segment runs here too
            if (!parse_segment_size(argv[i + 1])) {
                fprintf(stderr, "Invalid segment size: %s\n", argv[i + 1]);
                return 2;
            }
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
    unsigned int hook_count = 0;
    unsigned int error_count = 0;
    // What the plugin tracks while it reads a patch file: the section it is
    // in, and the feature block the current line sits under
    bool in_section = false;
    vg_feature_t feature = FEATURE_INVALID;
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

        if (patch_line[0] == '[') {
            // A new section is a different build of the game, and only one of
            // them is ever the one being patched
            in_section = true;
            feature = FEATURE_INVALID;
            reset_hook_state();
            continue;
        }

        if (patch_line[0] == '@') {
            feature = parse_feature(patch_line);
            continue;
        }

        if (patch_line[0] == '>') {
            hook_count++;
            if (!check_hook_directive(line_number, patch_line, feature, in_section)) {
                error_count++;
            }
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

    fprintf(stderr, "%u patch expressions, %u hook directives written (%u errors)\n",
                patch_count, hook_count, error_count);
    return error_count == 0 ? 0 : 1;
}
