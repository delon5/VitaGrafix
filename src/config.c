#include <vitasdk.h>
#include <taihen.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "io.h"
#include "config.h"
#include "main.h"
#include "log.h"

static vg_config_section_t g_config_section = CONFIG_SECTION_NONE;
static vg_config_t g_config = {0};
static vg_io_status_t g_config_status = {0};

// Incremented on every section header, so options can tell
// whether they are being (re)defined by a new section
static uint32_t g_config_section_id = 0;
// Section that last defined the IB list
static uint32_t g_config_ib_section_id = 0;

#define CONFIG_PATH_SIZE 128

const vg_res_t vg_config_framebuffer_resolutions[FRAMEBUFFER_RESOLUTION_COUNT] = {
    {960, 544},
    {720, 408},
    {640, 368},
    {480, 272},
};

#define OPTIONS_TOTAL 7
static const vg_config_parse_option_t _OPTIONS[OPTIONS_TOTAL] = {
    {"ENABLED", CONFIG_OPTION_FEATURE_STATE,              &g_config.enabled,      {NULL},                   NULL},
    {"OSD",     CONFIG_OPTION_FEATURE_STATE,              &g_config.osd_enabled,  {NULL},                   NULL},
    {"LOG",     CONFIG_OPTION_FEATURE_STATE,              &g_config.log_enabled,  {NULL},                   NULL},
    {"FB",      CONFIG_OPTION_FRAMEBUFFER_RESOLUTION,     &g_config.fb_enabled,   {(void *)&g_config.fb},   NULL},
    {"IB",      CONFIG_OPTION_INTERNAL_BUFFER_RESOLUTION, &g_config.ib_enabled,   {(void *)g_config.ib},    &g_config.ib_count},
    {"FPS",     CONFIG_OPTION_FRAMERATE,                  &g_config.fps_enabled,  {(void *)&g_config.fps},  NULL},
    {"MSAA",    CONFIG_OPTION_MSAA,                       &g_config.msaa_enabled, {(void *)&g_config.msaa}, NULL},
};

static bool vg_config_token_matches(const char line[], int pos, const char token[]) {
    size_t token_length = strlen(token);
    return !strncasecmp(&line[pos], token, token_length) && vg_io_is_line_end(line, pos + token_length);
}

static vg_io_status_t vg_config_parse_feature_state(const char line[], int pos, vg_feature_state_t *out) {
    while (isspace(line[pos])) { pos++; }

    // Enabled
    if (vg_config_token_matches(line, pos, "1")
            || vg_config_token_matches(line, pos, "on")
            || vg_config_token_matches(line, pos, "true")) {
        *out = FT_ENABLED;
        __ret_status(IO_OK, 0, 0);
    }
    // Disabled
    if (vg_config_token_matches(line, pos, "0")
            || vg_config_token_matches(line, pos, "off")
            || vg_config_token_matches(line, pos, "false")) {
        *out = FT_DISABLED;
        __ret_status(IO_OK, 0, 0);
    }

    __ret_status(IO_ERROR_PARSE_INVALID_TOKEN, 0, pos);
}

static vg_io_status_t vg_config_parse_dimension(const char line[], int *pos, uint16_t *out) {
    if (!isdigit(line[*pos]))
        __ret_status(IO_ERROR_PARSE_INVALID_TOKEN, 0, *pos);

    char *end = NULL;
    unsigned long value = strtoul(&line[*pos], &end, 10);
    if (value == 0 || value > UINT16_MAX)
        __ret_status(IO_ERROR_PARSE_INVALID_TOKEN, 0, *pos);

    *out = value;
    *pos = end - line;
    __ret_status(IO_OK, 0, 0);
}

static vg_io_status_t vg_config_parse_resolution_value(const char line[], int *pos, vg_res_t *res) {
    int resolution_pos = *pos;
    vg_io_status_t ret = vg_config_parse_dimension(line, pos, &res->width);
    if (ret.code != IO_OK)
        return ret;

    while (isspace(line[*pos])) { (*pos)++; }
    if (line[*pos] != 'x' && line[*pos] != 'X')
        __ret_status(IO_ERROR_PARSE_INVALID_TOKEN, 0, *pos);
    (*pos)++;

    while (isspace(line[*pos])) { (*pos)++; }
    ret = vg_config_parse_dimension(line, pos, &res->height);
    if (ret.code != IO_OK)
        return ret;

    vg_res_t original = *res;
    res->width &= ~3u;
    res->height &= ~3u;
    if (res->width == 0 || res->height == 0)
        __ret_status(IO_ERROR_PARSE_INVALID_TOKEN, 0, resolution_pos);
    if (original.width != res->width || original.height != res->height) {
        vg_log_printf("[CONFIG] Resolution %dx%d aligned down to %dx%d\n",
                original.width, original.height, res->width, res->height);
    }

    __ret_status(IO_OK, 0, 0);
}

static vg_io_status_t vg_config_parse_framebuffer_resolution(const char line[], int pos,
        vg_feature_state_t *ft, vg_res_t *res) {
    *ft = FT_ENABLED;
    while (isspace(line[pos])) { pos++; }

    // Disabled
    if (vg_config_token_matches(line, pos, "0")
            || vg_config_token_matches(line, pos, "off")
            || vg_config_token_matches(line, pos, "false")) {
        *ft = FT_DISABLED;
        __ret_status(IO_OK, 0, 0);
    }

    vg_io_status_t ret = vg_config_parse_resolution_value(line, &pos, res);
    if (ret.code != IO_OK)
        return ret;

    bool is_valid = false;
    for (int i = 0; i < FRAMEBUFFER_RESOLUTION_COUNT; i++) {
        if (res->width == vg_config_framebuffer_resolutions[i].width
                && res->height == vg_config_framebuffer_resolutions[i].height) {
            is_valid = true;
            break;
        }
    }
    if (!is_valid)
        __ret_status(IO_ERROR_PARSE_INVALID_TOKEN, 0, pos);
    if (!vg_io_is_line_end(line, pos))
        __ret_status(IO_ERROR_PARSE_INVALID_TOKEN, 0, pos);

    __ret_status(IO_OK, 0, 0);
}

static vg_io_status_t vg_config_parse_internal_buffer_resolution(const char line[], int pos,
        vg_feature_state_t *ft, vg_res_t *res, uint8_t *count) {
    *ft = FT_ENABLED;
    while (isspace(line[pos])) { pos++; }

    if (vg_config_token_matches(line, pos, "0")
            || vg_config_token_matches(line, pos, "off")
            || vg_config_token_matches(line, pos, "false")) {
        *ft = FT_DISABLED;
        __ret_status(IO_OK, 0, 0);
    }

    while (true) {
        if (*count >= MAX_RES_COUNT)
            __ret_status(IO_ERROR_PARSE_INVALID_TOKEN, 0, pos);

        vg_io_status_t ret = vg_config_parse_resolution_value(line, &pos, &res[*count]);
        if (ret.code != IO_OK)
            return ret;
        (*count)++;

        while (isspace(line[pos])) { pos++; }
        if (vg_io_is_line_end(line, pos))
            break;
        if (line[pos] != ',')
            __ret_status(IO_ERROR_PARSE_INVALID_TOKEN, 0, pos);
        pos++;
        while (isspace(line[pos])) { pos++; }
        if (vg_io_is_line_end(line, pos))
            __ret_status(IO_ERROR_PARSE_INVALID_TOKEN, 0, pos);
    }

    __ret_status(IO_OK, 0, 0);
}

static vg_io_status_t vg_config_parse_framerate(const char line[], int pos, vg_feature_state_t *ft, vg_fps_t *fps) {
    *ft = FT_ENABLED;
    while (isspace(line[pos])) { pos++; }

    // Disabled
    if (vg_config_token_matches(line, pos, "0")
            || vg_config_token_matches(line, pos, "off")
            || vg_config_token_matches(line, pos, "false")) {
        *ft = FT_DISABLED;
        __ret_status(IO_OK, 0, 0);
    }

    if (vg_config_token_matches(line, pos, "60")) {
        *fps = FPS_60;
        __ret_status(IO_OK, 0, 0);
    }
    if (vg_config_token_matches(line, pos, "30")) {
        *fps = FPS_30;
        __ret_status(IO_OK, 0, 0);
    }
    if (vg_config_token_matches(line, pos, "20")) {
        *fps = FPS_20;
        __ret_status(IO_OK, 0, 0);
    }

    __ret_status(IO_ERROR_PARSE_INVALID_TOKEN, 0, pos);
}

static vg_io_status_t vg_config_parse_msaa(const char line[], int pos, vg_feature_state_t *ft, vg_msaa_t *msaa) {
    *ft = FT_ENABLED;
    while (isspace(line[pos])) { pos++; }

    // Disabled
    if (vg_config_token_matches(line, pos, "0")
            || vg_config_token_matches(line, pos, "off")
            || vg_config_token_matches(line, pos, "false")) {
        *ft = FT_DISABLED;
        __ret_status(IO_OK, 0, 0);
    }

    // Both the bare number and the documented '4x' / '2x' / '1x' spellings
    if (vg_config_token_matches(line, pos, "4") || vg_config_token_matches(line, pos, "4x")) {
        *msaa = MSAA_4X;
        __ret_status(IO_OK, 0, 0);
    }
    if (vg_config_token_matches(line, pos, "2") || vg_config_token_matches(line, pos, "2x")) {
        *msaa = MSAA_2X;
        __ret_status(IO_OK, 0, 0);
    }
    if (vg_config_token_matches(line, pos, "1") || vg_config_token_matches(line, pos, "1x")) {
        *msaa = MSAA_NONE;
        __ret_status(IO_OK, 0, 0);
    }

    __ret_status(IO_ERROR_PARSE_INVALID_TOKEN, 0, pos);
}

static vg_io_status_t vg_config_parse_option(const char line[]) {
    vg_io_status_t ret = {IO_ERROR_PARSE_INVALID_TOKEN, 0, 0};
    int pos = 0;

    for (int i = 0; i < OPTIONS_TOTAL; i++) {
        size_t len = strlen(_OPTIONS[i].name);
        int pos_rhs = pos + len;

        if (!strncasecmp(&line[pos], _OPTIONS[i].name, len)) {
            // Ignore [MAIN] if game-specific option is already set
            if (g_config_section == CONFIG_SECTION_MAIN && *(_OPTIONS[i].ft_state) != FT_UNSPECIFIED) {
                __ret_status(IO_OK, 0, 0);
            }

            while (line[pos_rhs] != '\0' && isspace(line[pos_rhs])) { pos_rhs++; }

            // Check '=' char
            if (line[pos_rhs] != '=') {
                ret.pos_line = pos_rhs;
                return ret;
            }
            pos_rhs++;

            switch (_OPTIONS[i].type) {
                case CONFIG_OPTION_FEATURE_STATE:
                    return vg_config_parse_feature_state(line, pos_rhs, _OPTIONS[i].ft_state);
                case CONFIG_OPTION_FRAMEBUFFER_RESOLUTION:
                    return vg_config_parse_framebuffer_resolution(line, pos_rhs,
                            _OPTIONS[i].ft_state, _OPTIONS[i].res);
                case CONFIG_OPTION_INTERNAL_BUFFER_RESOLUTION:
                    // A new section replaces the IB list instead of appending to it,
                    // otherwise [MAIN] IB + game IB would become a multi-res list
                    if (g_config_ib_section_id != g_config_section_id) {
                        *(_OPTIONS[i].count) = 0;
                        g_config_ib_section_id = g_config_section_id;
                    }
                    return vg_config_parse_internal_buffer_resolution(line, pos_rhs,
                            _OPTIONS[i].ft_state, _OPTIONS[i].res, _OPTIONS[i].count);
                case CONFIG_OPTION_FRAMERATE:
                    return vg_config_parse_framerate(line, pos_rhs, _OPTIONS[i].ft_state, _OPTIONS[i].fps);
                case CONFIG_OPTION_MSAA:
                    return vg_config_parse_msaa(line, pos_rhs, _OPTIONS[i].ft_state, _OPTIONS[i].msaa);
            }
        }
    }

    return ret;
}

static vg_io_status_t vg_config_parse_line(const char line[]) {
    vg_io_status_t ret = {IO_OK, 0, 0};

    // Check for a new section
    if (line[0] == '[') {
        g_config_section_id++;

        // [MAIN]
        if (!strncasecmp(line, "[MAIN]", 6) && vg_io_is_line_end(line, 6)) {
            g_config_section = CONFIG_SECTION_MAIN;
            return ret;
        }

        // [TITLEID,SELF,NID]
        vg_io_section_header_t header;
        ret = vg_io_parse_section_header(line, &header);
        if (ret.code != IO_OK) {
            return ret;
        }

        g_config_section = vg_main_match_current_module(header.titleid, header.self, header.nid, false) == MODULE_MATCH
            ? CONFIG_SECTION_GAME : CONFIG_SECTION_NONE;
        return ret;
    }

    // Parse option
    if (g_config_section != CONFIG_SECTION_NONE) {
        return vg_config_parse_option(line);
    }

    return ret;
}

void vg_config_set_unspecified_to_defaults() {
    if (g_config.enabled == FT_UNSPECIFIED) {
        g_config.enabled = FT_ENABLED;
    }

    if (g_config.osd_enabled == FT_UNSPECIFIED) {
        g_config.osd_enabled = FT_ENABLED;
    }

    if (g_config.log_enabled == FT_UNSPECIFIED) {
        g_config.log_enabled = FT_ENABLED;
    }

    if (g_config.fb_enabled == FT_UNSPECIFIED) {
        g_config.fb_enabled = FT_DISABLED;
    }

    if (g_config.ib_enabled == FT_UNSPECIFIED) {
        g_config.ib_enabled = FT_DISABLED;
    }

    // A disabled resolution option still has to evaluate to the native size:
    // patches reference fb_w/ib_w from other option blocks, and an explicit
    // "FB = off" would otherwise leave the zero-initialised 0x0 behind.
    if (g_config.fb_enabled != FT_ENABLED || g_config.fb.width == 0 || g_config.fb.height == 0) {
        g_config.fb.width = 960;
        g_config.fb.height = 544;
    }

    if (g_config.ib_enabled != FT_ENABLED || g_config.ib_count == 0
            || g_config.ib[0].width == 0 || g_config.ib[0].height == 0) {
        g_config.ib[0].width = 960;
        g_config.ib[0].height = 544;
        g_config.ib_count = 1;
    }

    if (g_config.fps_enabled == FT_UNSPECIFIED) {
        g_config.fps_enabled = FT_DISABLED;
    }

    if (g_config.msaa_enabled == FT_UNSPECIFIED) {
        g_config.msaa_enabled = FT_DISABLED;
    }
}

void vg_config_propagate_ib() {
    if (g_config.ib_count == 0) {
        return;
    }
    for (uint8_t i = g_config.ib_count; i < MAX_RES_COUNT; i++) {
        g_config.ib[i].width = g_config.ib[i - 1].width;
        g_config.ib[i].height = g_config.ib[i - 1].height;
    }
}

// Saving the in-game menu's settings
//
// Each title's settings live in config/<TITLEID>.txt, which is used instead of config.txt
// when it exists (config.txt is the fallback, like patchlist.txt for the patch folder).
// VitaGrafixConfigurator writes the same files. A save puts the menu's options into the
// last part of that file the running game reads (the lines before the first section
// header, or a matching section), where they are, so nothing else in the file changes
// meaning. A new file starts with what the game used from config.txt that the menu does
// not set (LOG). The file I/O runs on a worker thread, away from the game's display thread.

#define CONFIG_SAVE_OPTIONS_SIZE 512
#define CONFIG_SAVE_READ_SIZE    512
#define CONFIG_TEMP_SUFFIX       ".vgtmp"
#define CONFIG_OLD_SUFFIX        ".vgold"
#define CONFIG_ERROR_NOT_FOUND   ((SceUID)0x80010002) // ENOENT

static const char *const g_config_menu_options[FEATURE_INVALID] = {"FB", "IB", "FPS", "MSAA"};

static char g_config_save_options[CONFIG_SAVE_OPTIONS_SIZE];
static int g_config_save_options_length = 0;
static uint32_t g_config_save_supported = 0; // bit per vg_feature_t written by the section
static bool g_config_save_log_disabled = false;
static volatile vg_config_save_state_t g_config_save_state = CONFIG_SAVE_IDLE;

typedef struct {
    SceUID fd;
    char buffer[CONFIG_SAVE_READ_SIZE];
    int length;
    int pos;
} vg_config_reader_t;

typedef struct {
    SceUID fd;
    bool has_data;
    char last_char;
} vg_config_writer_t;

static bool vg_config_buffer_append(char buffer[], size_t size, int *length, const char format[], ...) {
    if (*length < 0 || (size_t)*length >= size) {
        return false;
    }

    va_list args;
    va_start(args, format);
    int written = vsnprintf(buffer + *length, size - *length, format, args);
    va_end(args);
    if (written < 0 || written >= (int)(size - *length)) {
        return false;
    }

    *length += written;
    return true;
}

static bool vg_config_format_options() {
    char *buffer = g_config_save_options;
    const size_t size = sizeof(g_config_save_options);
    int *length = &g_config_save_options_length;
    const char *off = "off";

    *length = 0;
    g_config_save_supported = 0;
    g_config_save_log_disabled = g_config.log_enabled == FT_DISABLED;

    if (g_config.fb_enabled != FT_UNSUPPORTED) {
        g_config_save_supported |= 1 << FEATURE_FB;
        if (g_config.fb_enabled == FT_ENABLED) {
            if (!vg_config_buffer_append(buffer, size, length, "FB=%dx%d\n", g_config.fb.width, g_config.fb.height)) {
                return false;
            }
        } else if (!vg_config_buffer_append(buffer, size, length, "FB=%s\n", off)) {
            return false;
        }
    }

    if (g_config.ib_enabled != FT_UNSUPPORTED) {
        g_config_save_supported |= 1 << FEATURE_IB;
        if (g_config.ib_enabled == FT_ENABLED && g_config.ib_count > 0) {
            if (!vg_config_buffer_append(buffer, size, length, "IB=")) {
                return false;
            }
            for (uint8_t i = 0; i < g_config.ib_count; i++) {
                if (!vg_config_buffer_append(buffer, size, length, "%s%dx%d",
                        i > 0 ? "," : "", g_config.ib[i].width, g_config.ib[i].height)) {
                    return false;
                }
            }
            if (!vg_config_buffer_append(buffer, size, length, "\n")) {
                return false;
            }
        } else if (!vg_config_buffer_append(buffer, size, length, "IB=%s\n", off)) {
            return false;
        }
    }

    if (g_config.fps_enabled != FT_UNSUPPORTED) {
        g_config_save_supported |= 1 << FEATURE_FPS;
        if (g_config.fps_enabled == FT_ENABLED) {
            if (!vg_config_buffer_append(buffer, size, length, "FPS=%s\n",
                    g_config.fps == FPS_20 ? "20" : g_config.fps == FPS_30 ? "30" : "60")) {
                return false;
            }
        } else if (!vg_config_buffer_append(buffer, size, length, "FPS=%s\n", off)) {
            return false;
        }
    }

    if (g_config.msaa_enabled != FT_UNSUPPORTED) {
        g_config_save_supported |= 1 << FEATURE_MSAA;
        if (g_config.msaa_enabled == FT_ENABLED) {
            if (!vg_config_buffer_append(buffer, size, length, "MSAA=%s\n",
                    g_config.msaa == MSAA_NONE ? "1" : g_config.msaa == MSAA_2X ? "2" : "4")) {
                return false;
            }
        } else if (!vg_config_buffer_append(buffer, size, length, "MSAA=%s\n", off)) {
            return false;
        }
    }

    return true;
}

// Reads one line with its line break. Returns its length, 0 at the end of the file,
// or -1 on a read error or a line longer than the config parser accepts.
static int vg_config_read_line(vg_config_reader_t *reader, char line[], int size) {
    int length = 0;

    while (true) {
        if (reader->pos >= reader->length) {
            reader->length = sceIoRead(reader->fd, reader->buffer, sizeof(reader->buffer));
            reader->pos = 0;
            if (reader->length < 0) {
                return -1;
            }
            if (reader->length == 0) {
                break;
            }
        }

        char c = reader->buffer[reader->pos++];
        if (length >= size - 1) {
            return -1;
        }
        line[length++] = c;
        if (c == '\n') {
            break;
        }
    }

    line[length] = '\0';
    return length;
}

static bool vg_config_rewind(vg_config_reader_t *reader) {
    reader->length = 0;
    reader->pos = 0;
    return sceIoLseek(reader->fd, 0, SCE_SEEK_SET) == 0;
}

static bool vg_config_write(vg_config_writer_t *writer, const char data[], int length) {
    if (length <= 0) {
        return true;
    }
    if (sceIoWrite(writer->fd, data, length) != length) {
        return false;
    }

    writer->has_data = true;
    writer->last_char = data[length - 1];
    return true;
}

// Writes a whole line (bytes as read, so even a NUL in it is kept), ending it if it was not
static bool vg_config_write_line(vg_config_writer_t *writer, const char line[], int length) {
    return vg_config_write(writer, line, length)
        && (line[length - 1] == '\n' || vg_config_write(writer, "\n", 1));
}

// Is this line one of the options the menu writes (FB, IB, ... for a supported feature)?
static bool vg_config_is_saved_option(const char line[], uint32_t supported) {
    for (int i = 0; i < FEATURE_INVALID; i++) {
        size_t length = strlen(g_config_menu_options[i]);
        if (!(supported & (1 << i)) || strncasecmp(line, g_config_menu_options[i], length)) {
            continue;
        }

        const char *rhs = line + length;
        while (isspace(*rhs)) { rhs++; }
        if (*rhs == '=') {
            return true;
        }
    }

    return false;
}

// Does the running game read the options under this section header?
static bool vg_config_header_matches(const char line[]) {
    // What the parser sees: the line up to its comment or a stray CR
    char header[IO_CHUNK_SIZE + 1];
    size_t length = strcspn(line, "#\r");
    if (length > IO_CHUNK_SIZE) {
        length = IO_CHUNK_SIZE;
    }
    memcpy(header, line, length);
    header[length] = '\0';

    if (!strncasecmp(header, "[MAIN]", 6) && vg_io_is_line_end(header, 6)) {
        return false; // only fills in what the game's sections leave unset
    }

    vg_io_section_header_t parsed;
    return vg_io_parse_section_header(header, &parsed).code == IO_OK
        && vg_main_match_current_module(parsed.titleid, parsed.self, parsed.nid, false) == MODULE_MATCH;
}

static bool vg_config_recover_title_file(const char path[]);

static bool vg_config_write_title_file() {
    char path[CONFIG_PATH_SIZE];
    char temp_path[CONFIG_PATH_SIZE + sizeof(CONFIG_TEMP_SUFFIX)];
    char old_path[CONFIG_PATH_SIZE + sizeof(CONFIG_OLD_SUFFIX)];
    snprintf(path, sizeof(path), "%s%s.txt", CONFIG_DIR, g_main.titleid);
    snprintf(temp_path, sizeof(temp_path), "%s" CONFIG_TEMP_SUFFIX, path);
    snprintf(old_path, sizeof(old_path), "%s" CONFIG_OLD_SUFFIX, path);

    char line[IO_CHUNK_SIZE + 1];
    vg_config_reader_t reader;
    reader.length = 0;
    reader.pos = 0;
    vg_config_writer_t writer = {-1, false, '\0'};

    sceIoMkdir(CONFIG_DIR, 0777);

    // A save that stopped halfway is finished first, so its files are not lost
    reader.fd = sceIoOpen(path, SCE_O_RDONLY, 0777);
    if (reader.fd < 0 && vg_config_recover_title_file(path)) {
        reader.fd = sceIoOpen(path, SCE_O_RDONLY, 0777);
    }
    if (reader.fd < 0 && reader.fd != CONFIG_ERROR_NOT_FOUND) {
        return false; // there, but unreadable: do not replace it
    }
    bool had_file = reader.fd >= 0;

    // First pass: the last part of the file the game reads. The lines before the
    // first header always count (the file is this title's own). There, the options go
    // where the first of them is, else at the end of those lines.
    int block_count = 0;
    int target_block = 0;
    int line_count = 0;
    int first_option_line = -1;
    int first_header_line = -1;
    while (had_file) {
        int length = vg_config_read_line(&reader, line, sizeof(line));
        if (length < 0) {
            goto SAVE_FAILURE;
        }
        if (length == 0) {
            break;
        }

        const char *text = line;
        while (isspace(*text)) { text++; }
        if (*text == '[') {
            block_count++;
            if (first_header_line < 0) {
                first_header_line = line_count;
            }
            if (vg_config_header_matches(text)) {
                target_block = block_count;
            }
        } else if (block_count == 0 && first_option_line < 0
                && vg_config_is_saved_option(text, g_config_save_supported)) {
            first_option_line = line_count;
        }
        line_count++;
    }
    int insert_line = first_option_line >= 0 ? first_option_line : first_header_line;

    if (had_file && !vg_config_rewind(&reader)) {
        goto SAVE_FAILURE;
    }

    sceIoRemove(temp_path);
    writer.fd = sceIoOpen(temp_path, SCE_O_WRONLY | SCE_O_CREAT | SCE_O_TRUNC, 0777);
    if (writer.fd < 0) {
        goto SAVE_FAILURE;
    }

    // A new file replaces config.txt for this title: keep what the game used from it
    // that the menu does not set
    if (!had_file && g_config_save_log_disabled && !vg_config_write(&writer, "LOG=off\n", 8)) {
        goto SAVE_FAILURE;
    }

    // Second pass: copy, with the target part's options replaced where they are
    int block = 0;
    int line_number = 0;
    bool options_written = false;
    while (had_file) {
        int length = vg_config_read_line(&reader, line, sizeof(line));
        if (length < 0) {
            goto SAVE_FAILURE;
        }
        if (length == 0) {
            break;
        }

        const char *text = line;
        while (isspace(*text)) { text++; }

        if (target_block == 0 && line_number++ == insert_line) {
            if (!vg_config_write(&writer, g_config_save_options, g_config_save_options_length)) {
                goto SAVE_FAILURE;
            }
            options_written = true;
        }

        if (*text == '[') {
            block++;
            if (!vg_config_write_line(&writer, line, length)) {
                goto SAVE_FAILURE;
            }
            if (block == target_block) {
                if (!vg_config_write(&writer, g_config_save_options, g_config_save_options_length)) {
                    goto SAVE_FAILURE;
                }
                options_written = true;
            }
            continue;
        }

        if (block == target_block && vg_config_is_saved_option(text, g_config_save_supported)) {
            continue;
        }

        if (!vg_config_write_line(&writer, line, length)) {
            goto SAVE_FAILURE;
        }
    }

    // A file with nothing but other lines before its end (or a new file)
    if (!options_written && !vg_config_write(&writer, g_config_save_options, g_config_save_options_length)) {
        goto SAVE_FAILURE;
    }

    if (reader.fd >= 0) {
        sceIoClose(reader.fd);
        reader.fd = -1;
    }

    int close_result = sceIoClose(writer.fd);
    writer.fd = -1;
    if (close_result < 0) {
        goto SAVE_FAILURE;
    }

    // Rename cannot replace a file, so the old one moves aside first. If this stops
    // halfway, vg_config_parse (or the next save) finishes the swap.
    sceIoRemove(old_path);
    if (had_file && sceIoRename(path, old_path) < 0) {
        goto SAVE_FAILURE;
    }
    if (sceIoRename(temp_path, path) < 0) {
        if (had_file) {
            sceIoRename(old_path, path);
        }
        goto SAVE_FAILURE;
    }
    sceIoRemove(old_path);

    return true;

SAVE_FAILURE:
    if (reader.fd >= 0) {
        sceIoClose(reader.fd);
    }
    if (writer.fd >= 0) {
        sceIoClose(writer.fd);
    }
    sceIoRemove(temp_path);
    return false;
}

// A save stopped after moving the old file aside: the new one was complete by then
static bool vg_config_recover_title_file(const char path[]) {
    char temp_path[CONFIG_PATH_SIZE + sizeof(CONFIG_TEMP_SUFFIX)];
    char old_path[CONFIG_PATH_SIZE + sizeof(CONFIG_OLD_SUFFIX)];
    snprintf(temp_path, sizeof(temp_path), "%s" CONFIG_TEMP_SUFFIX, path);
    snprintf(old_path, sizeof(old_path), "%s" CONFIG_OLD_SUFFIX, path);

    SceIoStat stat;
    if (sceIoGetstat(old_path, &stat) < 0) {
        return false;
    }

    if (sceIoRename(temp_path, path) >= 0) {
        sceIoRemove(old_path);
    } else if (sceIoRename(old_path, path) < 0) {
        return false;
    }

    vg_log_printf("[CONFIG] Recovered %s from an interrupted save\n", path);
    return true;
}

static int vg_config_save_thread(SceSize args, void *argp) {
    bool ok = vg_config_write_title_file();

    __sync_synchronize();
    g_config_save_state = ok ? CONFIG_SAVE_OK : CONFIG_SAVE_FAILED;
    return sceKernelExitDeleteThread(0);
}

bool vg_config_save_start() {
    if (g_config_save_state == CONFIG_SAVE_RUNNING) {
        return false;
    }

    // Snapshot the settings here, the worker only does the file I/O
    if (!vg_config_format_options()) {
        return false;
    }

    g_config_save_state = CONFIG_SAVE_RUNNING;
    __sync_synchronize();

    SceUID thread = sceKernelCreateThread("VitaGrafixSave", vg_config_save_thread, 0x10000100, 0x4000, 0, 0, NULL);
    if (thread < 0 || sceKernelStartThread(thread, 0, NULL) < 0) {
        if (thread >= 0) {
            sceKernelDeleteThread(thread);
        }
        g_config_save_state = CONFIG_SAVE_FAILED;
        return false;
    }

    return true;
}

vg_config_save_state_t vg_config_save_get_state() {
    return g_config_save_state;
}

void vg_config_save_wait() {
    for (int i = 0; i < 200 && g_config_save_state == CONFIG_SAVE_RUNNING; i++) {
        sceKernelDelayThread(10000);
    }
}

// Where the last parse failed, for the error screen
static char g_config_error_path[CONFIG_PATH_SIZE] = "";

const char *vg_config_get_error_path() {
    return g_config_error_path;
}

vg_io_status_t vg_config_parse() {
    g_config = (vg_config_t){0};

    g_config.enabled      = FT_UNSPECIFIED;
    g_config.osd_enabled  = FT_UNSPECIFIED;
    g_config.log_enabled  = FT_UNSPECIFIED;
    g_config.fb_enabled   = FT_UNSPECIFIED;
    g_config.ib_enabled   = FT_UNSPECIFIED;
    g_config.fps_enabled  = FT_UNSPECIFIED;
    g_config.msaa_enabled = FT_UNSPECIFIED;
    g_config_section_id    = 0;
    g_config_ib_section_id = 0;

    g_config.ib[0] = (vg_res_t){960, 544};
    g_config.ib_count = 1;
    g_config.fps = FPS_60;
    g_config.msaa = MSAA_4X;

    char path[CONFIG_PATH_SIZE];
    snprintf(path, sizeof(path), "%s%s.txt", CONFIG_DIR, g_main.titleid);

    // The title's own file (config/<TITLEID>.txt, written by the in-game menu and by
    // VitaGrafixConfigurator) is used instead of config.txt, which is the fallback, the
    // way the patch folder is preferred over patchlist.txt. The file is specific to this
    // title, so options may be listed without a section header (they are the game's section).
    g_config_section = CONFIG_SECTION_GAME;
    g_config_section_id = 1;
    g_config_status = vg_io_parse(path, vg_config_parse_line, false);
    if (g_config_status.code == IO_ERROR_OPEN_FAILED && vg_config_recover_title_file(path)) {
        g_config_status = vg_io_parse(path, vg_config_parse_line, false);
    }
    const char *parsed_path = path;

    // If does not exist, parse global config.txt. It is only read, never written or
    // created: without one (and without the title's file) the defaults apply.
    if (g_config_status.code == IO_ERROR_OPEN_FAILED) {
        SceIoStat stat;
        g_config_section = CONFIG_SECTION_NONE;
        g_config_section_id = 0;
        g_config_status = sceIoGetstat(CONFIG_PATH, &stat) < 0
            ? (vg_io_status_t){IO_OK, 0, 0}
            : vg_io_parse(CONFIG_PATH, vg_config_parse_line, false);
        parsed_path = CONFIG_PATH;
    }

    if (g_config_status.code != IO_OK) {
        snprintf(g_config_error_path, sizeof(g_config_error_path), "%s", parsed_path);
    }

    // Set unset options to their default values
    vg_config_set_unspecified_to_defaults();

    // Propagate last specified IB res. (for multires patches)
    vg_config_propagate_ib();

#ifdef ENABLE_VERBOSE_LOGGING
    vg_log_printf("[CONFIG] Config:\n");
    vg_log_printf("[CONFIG] ENABLED: %d\n", g_config.enabled);
    vg_log_printf("[CONFIG] OSD: %d\n", g_config.osd_enabled);
    vg_log_printf("[CONFIG] LOG: %d\n", g_config.log_enabled);
    vg_log_printf("[CONFIG] FB: %d %dx%d\n", g_config.fb_enabled, g_config.fb.width, g_config.fb.height);
    vg_log_printf("[CONFIG] IB: %d #%d 1st:%dx%d\n", g_config.ib_enabled, g_config.ib_count, g_config.ib[0].width, g_config.ib[0].height);
    vg_log_printf("[CONFIG] FPS: %d %d\n", g_config.fps_enabled, g_config.fps);
    vg_log_printf("[CONFIG] MSAA: %d %d\n", g_config.msaa_enabled, g_config.msaa);
#endif

    return g_config_status;
}

bool vg_config_is_feature_enabled(vg_feature_t feature) {
    if (!g_config.enabled) {
        return false;
    }

    switch (feature) {
        case FEATURE_FB:   return g_config.fb_enabled == FT_ENABLED;
        case FEATURE_IB:   return g_config.ib_enabled == FT_ENABLED;
        case FEATURE_FPS:  return g_config.fps_enabled == FT_ENABLED;
        case FEATURE_MSAA: return g_config.msaa_enabled == FT_ENABLED;
        default: return false;
    }

    return false;
}

bool vg_config_is_feature_supported(vg_feature_t feature) {
    if (!g_config.enabled) {
        return false;
    }

    switch (feature) {
        case FEATURE_FB:   return g_config.fb_enabled != FT_UNSUPPORTED;
        case FEATURE_IB:   return g_config.ib_enabled != FT_UNSUPPORTED;
        case FEATURE_FPS:  return g_config.fps_enabled != FT_UNSUPPORTED;
        case FEATURE_MSAA: return g_config.msaa_enabled != FT_UNSUPPORTED;
        default: return false;
    }

    return false;
}

vg_config_t *vg_config_get() {
    return &g_config;
}

const vg_io_status_t *vg_config_get_status() {
    return &g_config_status;
}

void vg_config_apply_patch_capabilities(vg_feature_state_t states[]) {
    if (states[FEATURE_FB] == FT_UNSUPPORTED)   g_config.fb_enabled = FT_UNSUPPORTED;
    if (states[FEATURE_IB] == FT_UNSUPPORTED)   g_config.ib_enabled = FT_UNSUPPORTED;
    if (states[FEATURE_FPS] == FT_UNSUPPORTED)  g_config.fps_enabled = FT_UNSUPPORTED;
    if (states[FEATURE_MSAA] == FT_UNSUPPORTED) g_config.msaa_enabled = FT_UNSUPPORTED;
}
