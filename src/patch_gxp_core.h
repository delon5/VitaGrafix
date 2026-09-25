#ifndef _PATCH_GXP_CORE_H_
#define _PATCH_GXP_CORE_H_

/*
 * Shader literal patches: the parts that do not depend on the Vita SDK
 * (line parsing, the patch table, matching a GXP program against it and
 * writing the new bytes). Built into the plugin and into the host tests.
 *
 *   gxp:<HASH>:<OFFSET> <stock-expr> => <new-expr>
 *   gxplit: <stock-expr> => <new-expr>
 *
 * HASH is the FNV-1a (32-bit) of the whole GXP program as the game registers
 * it. A gxp line rewrites bytes at OFFSET of that one program; a gxplit line
 * rewrites runs of consecutive literal-table entries holding the stock words
 * in any program no gxp line names. The stock side is always checked first.
 */

#include <stdint.h>
#include <stdbool.h>

#include "interpreter/interpreter.h"

#define VG_GXP_MAX_PATCHES    128        // gxp lines, including those that are stock at this config
#define VG_GXP_MAX_RULES      8          // gxplit lines
#define VG_GXP_MAX_BYTES      32         // bytes per line (== MAX_VALUE_SIZE)
#define VG_GXP_MAX_WORDS      (VG_GXP_MAX_BYTES / 4)
#define VG_GXP_MAX_LEFT       128        // the "=>" must be before this column
#define VG_GXP_MIN_OFFSET     12         // magic, version and size are never patched
#define VG_GXP_MIN_SIZE       0x98       // smallest header that holds the literal table fields
#define VG_GXP_MAX_SIZE       0x100000
#define VG_GXP_MAX_RULE_SITES 16         // literal runs rewritten in one program by rules
#define VG_GXP_MAX_SURVEY     32         // stock words looked for in programs nothing matched

#define VG_GXP_LIT_COUNT      0x70       // u32: number of (register, value) literal entries
#define VG_GXP_LIT_OFFSET     0x74       // u32: offset of the entries, relative to this field

typedef struct {
    uint32_t hash;
    uint32_t offset;
    uint8_t  size;
    bool     active;                     // false: the new bytes equal the stock bytes at this config
    uint8_t  stock[VG_GXP_MAX_BYTES];
    uint8_t  data[VG_GXP_MAX_BYTES];
    uint32_t gap_mask;                   // bit i set: byte i is a ?? gap (neither checked nor written)
    uint16_t ordinal;                    // n-th shader line of the file, for the log
    uint8_t  prog;                       // program slot (distinct hash) after sealing
} vg_gxp_patch_t;

typedef struct {
    uint8_t  words;
    uint32_t stock[VG_GXP_MAX_WORDS];
    uint32_t data[VG_GXP_MAX_WORDS];
    uint16_t ordinal;
} vg_gxp_rule_t;

typedef struct {
    vg_gxp_patch_t e[VG_GXP_MAX_PATCHES];
    uint16_t count;
    uint16_t active;                     // entries with active set
    vg_gxp_rule_t r[VG_GXP_MAX_RULES];
    uint8_t  rule_count;
    uint32_t survey[VG_GXP_MAX_SURVEY];
    uint8_t  survey_count;
    uint16_t programs;                   // distinct hashes among the active entries
    bool     sealed;
} vg_gxp_table_t;

typedef enum {
    VG_GXP_LINE_PATCH,
    VG_GXP_LINE_RULE
} vg_gxp_line_kind_t;

typedef struct {
    vg_gxp_line_kind_t kind;
    vg_gxp_patch_t patch;
    vg_gxp_rule_t rule;
    bool approximated;                   // an encoder rounded a value (see intp_value_t)
} vg_gxp_line_t;

typedef enum {
    VG_GXP_PARSE_OK,
    VG_GXP_PARSE_UNCHANGED,              // valid, but the new bytes equal the stock bytes
    VG_GXP_PARSE_SYNTAX,
    VG_GXP_PARSE_EVAL,                   // interpreter error, see the intp_status_t
    VG_GXP_PARSE_SIZE,                   // sides of different or unsupported size
    VG_GXP_PARSE_GAPS                    // ?? gaps that differ, or nothing but gaps
} vg_gxp_parse_t;

typedef enum {
    VG_GXP_ADD_OK,
    VG_GXP_ADD_FULL,
    VG_GXP_ADD_OVERLAP,                  // overlaps another line of the same program
    VG_GXP_ADD_CHAIN                     // a rule's new words are another rule's stock words
} vg_gxp_add_t;

typedef enum {
    GXP_NOT_PROGRAM,
    GXP_NO_MATCH,
    GXP_MATCHED,
    GXP_STOCK_MISMATCH,
    GXP_OUT_OF_RANGE,
    GXP_ALREADY_PATCHED,
    GXP_RULE_MATCHED,
    GXP_RULE_CONFLICT,
    GXP_RULE_TOO_MANY,
    GXP_SURVEY
} vg_gxp_status_t;

typedef struct {
    uint8_t  rule;
    uint32_t offset;                     // byte offset of the first entry of the run
} vg_gxp_site_t;

typedef struct {
    vg_gxp_status_t status;
    uint32_t hash;
    uint32_t size;
    uint16_t first, count;               // table entries of this hash (gxp lines)
    uint16_t sites, bytes;               // what a commit writes
    uint32_t bad_offset;                 // STOCK_MISMATCH / OUT_OF_RANGE
    uint8_t  nsite;                      // rule runs
    vg_gxp_site_t site[VG_GXP_MAX_RULE_SITES];
    uint32_t survey_word, survey_offset; // SURVEY
} vg_gxp_result_t;

uint32_t vg_gxp_fnv1a(const void *data, uint32_t size);
bool vg_gxp_program_size(const void *prog, uint32_t *size);
bool vg_gxp_literal_table(const void *prog, uint32_t size, uint32_t *offset, uint32_t *count);

vg_gxp_parse_t vg_gxp_parse_line(const char *line, vg_gxp_line_t *out, uint32_t *errpos, intp_status_t *ist);

void vg_gxp_table_reset(vg_gxp_table_t *t);
vg_gxp_add_t vg_gxp_table_add(vg_gxp_table_t *t, const vg_gxp_line_t *line);
void vg_gxp_table_seal(vg_gxp_table_t *t);

vg_gxp_status_t vg_gxp_match(const vg_gxp_table_t *t, const void *prog, vg_gxp_result_t *r);
vg_gxp_status_t vg_gxp_recheck(const vg_gxp_table_t *t, const void *prog, vg_gxp_result_t *r);
void vg_gxp_commit(const vg_gxp_table_t *t, void *prog, const vg_gxp_result_t *r);

#endif
