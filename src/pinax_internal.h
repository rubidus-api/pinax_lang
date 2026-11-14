// pinax_internal.h - the state as the interpreter sees it. Not a public header.
#ifndef PINAX_INTERNAL_H
#define PINAX_INTERNAL_H

#include "pinax.h"
#include "pinax_code.h"

#include <stdbool.h>

#define PNX_MAX_HOSTS 96
// Register slots a task gets for every level of its call depth.
#define PNX_TASK_INTS 16
#define PNX_TASK_STRS 4
#define PNX_MAX_HANDLE_TYPES 16
#define PNX_MAX_STR_ARGS 8
static_assert(PNX_SEG_HOST + PNX_MAX_STR_ARGS <= PNX_SEG_KEPT, "every string argument of a run has a segment");
#define PNX_MAX_STR_RESULTS 4
#define PNX_MAX_WIDTHS 16 // integer arguments or results of a signature whose narrow types are checked (RFC-0025)
#define PNX_HOST_WIDTHS 8 // of those, the results a host word keeps the widths of

// A string value: bytes `offset .. offset + length` of a segment. Never an address, so the
// block can be copied or mapped elsewhere and the value still holds.
typedef struct pnx_scell {
    uint32_t segment;
    uint32_t length;
    uint64_t offset;
} pnx_scell;

#define PNX_SNEXT_DONE ((pnx_scell){PNX_SEG_DONE, 0, 0}) // a sequence that is used up (segment PNX_SEG_DONE)

// The pattern matcher of the module ptn (pinax_ptn.c). pnx_ptn_find looks for the first match
// at or after `from`: 1 found (caps set), 0 none, -1 a malformed pattern, -2 too many steps.
#define PNX_PTN_CAPS 9
#define PNX_PTN_MAX 255
typedef struct pnx_ptn_caps {
    uint32_t n, at, len;
    uint32_t start[PNX_PTN_CAPS], clen[PNX_PTN_CAPS];
} pnx_ptn_caps;
int  pnx_ptn_find(const uint8_t *s, uint32_t length, uint32_t from, const uint8_t *pat, uint32_t plen, pnx_ptn_caps *caps,
                  uint64_t *steps);
bool pnx_ptn_check(const uint8_t *pat, uint32_t plen, const char **why);

// The regular expressions of the module rex (pinax_rex.c). A program is bytes without
// addresses. pnx_rex_compile writes it into `out` and needs nothing else; pnx_rex_search needs
// pnx_rex_work() bytes of work (0: not a program). A search gives 2 places per group, group 0
// the whole match; PNX_REX_NONE for a group that took no part.
#define PNX_REX_NONE UINT32_MAX
uint32_t pnx_rex_compile(const uint8_t *pat, uint32_t plen, uint8_t *out, uint32_t cap, const char **why, uint32_t *at);
uint32_t pnx_rex_groups(const uint8_t *prog, uint32_t len);
size_t   pnx_rex_work(const uint8_t *prog, uint32_t len);
int      pnx_rex_search(const uint8_t *prog, uint32_t plen, const uint8_t *s, uint32_t len, uint32_t from, uint32_t *found,
                        void *work, uint64_t *steps);

// The module `sheet` (pinax_sheet.c, RFC-0042, RFC-0044): a book of sheets, each a table of rows and columns, in
// one buffer. `b` is the buffer's bytes and `cap` its capacity; every function checks that the bytes still hold a
// book. Words other than the book words work on the current sheet (STATE: there is none).
typedef enum { PNX_SH_OK, PNX_SH_FULL, PNX_SH_RANGE, PNX_SH_KIND, PNX_SH_NONE, PNX_SH_BAD, PNX_SH_OVERFLOW, PNX_SH_STATE } pnx_sheet_result;
pnx_sheet_result pnx_sheet_open(uint8_t *b, uint32_t cap, uint32_t nmax);
pnx_sheet_result pnx_sheet_create(uint8_t *b, uint32_t cap, const uint8_t *name, uint32_t n, int64_t rmax, int64_t cols, bool colmajor, int64_t *num);
pnx_sheet_result pnx_sheet_use(uint8_t *b, uint32_t cap, const uint8_t *name, uint32_t n);
pnx_sheet_result pnx_sheet_use_n(uint8_t *b, uint32_t cap, int64_t k);
pnx_sheet_result pnx_sheet_current(uint8_t *b, uint32_t cap, int64_t *out);
pnx_sheet_result pnx_sheet_sheets(uint8_t *b, uint32_t cap, int64_t *out);
pnx_sheet_result pnx_sheet_sheet_name(uint8_t *b, uint32_t cap, int64_t k, uint32_t *off, uint32_t *len);
pnx_sheet_result pnx_sheet_find_sheet(uint8_t *b, uint32_t cap, const uint8_t *name, uint32_t n, int64_t *num);
pnx_sheet_result pnx_sheet_drop(uint8_t *b, uint32_t cap, const uint8_t *name, uint32_t n);
pnx_sheet_result pnx_sheet_rename(uint8_t *b, uint32_t cap, int64_t k, const uint8_t *name, uint32_t n);
pnx_sheet_result pnx_sheet_copy_to(uint8_t *b, uint32_t cap, int64_t r1, int64_t c1, int64_t k, int64_t r2, int64_t c2);
pnx_sheet_result pnx_sheet_index(uint8_t *b, uint32_t cap, int64_t col);
pnx_sheet_result pnx_sheet_drop_index(uint8_t *b, uint32_t cap, int64_t col);
pnx_sheet_result pnx_sheet_indexed(uint8_t *b, uint32_t cap, int64_t col, int64_t *out);
pnx_sheet_result pnx_sheet_dim(uint8_t *b, uint32_t cap, int which, int64_t *out);
uint32_t pnx_sheet_rows(uint8_t *b, uint32_t cap);
uint32_t pnx_sheet_cols(uint8_t *b, uint32_t cap);
uint32_t pnx_sheet_rows_max(uint8_t *b, uint32_t cap);
pnx_sheet_result pnx_sheet_add_row(uint8_t *b, uint32_t cap, uint32_t *row);
pnx_sheet_result pnx_sheet_truncate_rows(uint8_t *b, uint32_t cap, uint32_t n);
pnx_sheet_result pnx_sheet_clear(uint8_t *b, uint32_t cap);
pnx_sheet_result pnx_sheet_widen(uint8_t *b, uint32_t cap, uint32_t cols);
pnx_sheet_result pnx_sheet_put_int(uint8_t *b, uint32_t cap, int64_t v, int64_t r, int64_t c);
pnx_sheet_result pnx_sheet_put_bool(uint8_t *b, uint32_t cap, int64_t v, int64_t r, int64_t c);
pnx_sheet_result pnx_sheet_put_date(uint8_t *b, uint32_t cap, int64_t v, int64_t r, int64_t c);
pnx_sheet_result pnx_sheet_get_date(uint8_t *b, uint32_t cap, int64_t r, int64_t c, int64_t *v);
pnx_sheet_result pnx_sheet_put_dec(uint8_t *b, uint32_t cap, int64_t v, uint32_t scale, int64_t r, int64_t c);
pnx_sheet_result pnx_sheet_put_str(uint8_t *b, uint32_t cap, const uint8_t *s, uint32_t n, int64_t r, int64_t c);
pnx_sheet_result pnx_sheet_clear_cell(uint8_t *b, uint32_t cap, int64_t r, int64_t c);
pnx_sheet_result pnx_sheet_copy_cell(uint8_t *b, uint32_t cap, int64_t r1, int64_t c1, int64_t r2, int64_t c2);
pnx_sheet_result pnx_sheet_kind(uint8_t *b, uint32_t cap, int64_t r, int64_t c, int64_t *kind);
pnx_sheet_result pnx_sheet_get_int(uint8_t *b, uint32_t cap, int64_t r, int64_t c, int64_t *v);
pnx_sheet_result pnx_sheet_get_bool(uint8_t *b, uint32_t cap, int64_t r, int64_t c, int64_t *v);
pnx_sheet_result pnx_sheet_get_dec(uint8_t *b, uint32_t cap, int64_t r, int64_t c, uint32_t scale, int64_t *v);
pnx_sheet_result pnx_sheet_get_str(uint8_t *b, uint32_t cap, int64_t r, int64_t c, uint32_t *off, uint32_t *len);
pnx_sheet_result pnx_sheet_find_int(uint8_t *b, uint32_t cap, bool across, int64_t key, int64_t line, int64_t from, int64_t *found, uint64_t *scanned);
pnx_sheet_result pnx_sheet_find_str(uint8_t *b, uint32_t cap, bool across, const uint8_t *s, uint32_t n, int64_t line, int64_t from,
                                    int64_t *found, uint64_t *scanned);
uint32_t         pnx_sheet_sel_len(uint8_t *b, uint32_t cap);
pnx_sheet_result pnx_sheet_sel_row(uint8_t *b, uint32_t cap, int64_t k, int64_t *row);
pnx_sheet_result pnx_sheet_select_all(uint8_t *b, uint32_t cap);
pnx_sheet_result pnx_sheet_narrow(uint8_t *b, uint32_t cap, bool text, int64_t col, int64_t op, int64_t ikey, const uint8_t *s, uint32_t n, uint64_t *scanned);
pnx_sheet_result pnx_sheet_where(uint8_t *b, uint32_t cap, bool text, int64_t col, int64_t op, int64_t ikey, const uint8_t *s, uint32_t n, uint64_t *scanned);
pnx_sheet_result pnx_sheet_total(uint8_t *b, uint32_t cap, int what, int over, int64_t line, int64_t *out, uint64_t *scanned);
pnx_sheet_result pnx_sheet_order_by(uint8_t *b, uint32_t cap, int64_t col, bool desc, uint64_t *scanned);
pnx_sheet_result pnx_sheet_compact(uint8_t *b, uint32_t cap);
// The files (pinax_sheetio.c, RFC-0045). `charge` takes the bytes worked on.
pnx_sheet_result pnx_sheet_load_csv(uint8_t *b, uint32_t cap, uint8_t *src, uint64_t n, int mode, int64_t *added, uint64_t *charge);
pnx_sheet_result pnx_sheet_load_xlsx(uint8_t *b, uint32_t cap, uint8_t *src, uint64_t n, uint64_t srccap, int mode, int64_t sheet,
                                     int64_t *added, uint64_t *charge);
pnx_sheet_result pnx_sheet_save_csv(uint8_t *b, uint32_t cap, uint8_t *dst, uint64_t dstcap, int64_t *len, uint64_t *charge);
pnx_sheet_result pnx_sheet_save_xlsx(uint8_t *b, uint32_t cap, uint8_t *dst, uint64_t dstcap, int64_t *len, uint64_t *charge);


// The words of `sheet` (RFC-0042): id, name, what they take (in stack order: i int, b bool, s str, d a dec(N)
// whose scale the compiler appends, n a constant scale), what they give (i, b, s, D a dec(N) of the scale n),
// and flags: F can fail, R reads the sheet only. The ints go in the registers from A in the order written, the
// str in register B; the result takes A (or B).
#define PNX_SHEET_WORDS(X)                                                                          \
    X(OPEN_BOOK, "open_book", "i", "", "F") X(CREATE, "create", "sii", "i", "F")                    \
    X(CREATE_COLS, "create_cols", "sii", "i", "F") X(USE, "activate", "s", "", "F")                      \
    X(USE_N, "activate_n", "i", "", "F") X(CURRENT, "current", "", "i", "R")                             \
    X(SHEETS, "sheets", "", "i", "R") X(SHEET_NAME, "sheet_name", "i", "s", "FR")                   \
    X(FIND_SHEET, "find_sheet", "s", "i", "FR") X(DROP, "remove", "s", "", "F")                       \
    X(RENAME, "rename", "is", "", "F") X(COPY_TO, "copy_to", "iiiii", "", "F")                      \
    X(INDEX, "index", "i", "", "F") X(DROP_INDEX, "unindex", "i", "", "")                        \
    X(INDEXED, "indexed", "i", "b", "R")                                                            \
    X(ROWS, "rows", "", "i", "R") X(COLS, "cols", "", "i", "R")                                     \
    X(ROWS_MAX, "rows_max", "", "i", "R") X(ADD_ROW, "add_row", "", "i", "F")                       \
    X(TRUNCATE_ROWS, "truncate_rows", "i", "", "") X(WIDEN, "widen", "i", "", "F")                  \
    X(CLEAR, "clear", "", "", "") X(COMPACT, "compact", "", "", "")                                 \
    X(PUT_INT, "put_int", "iii", "", "F") X(PUT_BOOL, "put_bool", "bii", "", "F") X(PUT_DATE, "put_date", "iii", "", "F")                   \
    X(PUT_DEC, "put_dec", "dii", "", "F") X(PUT_STR, "put_str", "sii", "", "F")                     \
    X(CLEAR_CELL, "clear_cell", "ii", "", "") X(COPY_CELL, "copy_cell", "iiii", "", "")             \
    X(KIND, "kind", "ii", "i", "R") X(IS_EMPTY, "is_empty", "ii", "b", "R")                         \
    X(GET_INT, "int", "ii", "i", "FR") X(GET_BOOL, "bool", "ii", "b", "FR") X(GET_DATE, "date", "ii", "i", "FR")                         \
    X(GET_DEC, "dec", "iin", "D", "FR") X(GET_STR, "str", "ii", "s", "FR")                          \
    X(FIND_ROW_INT, "find_row_int", "ii", "i", "FR") X(FIND_ROW_STR, "find_row_str", "si", "i", "FR") \
    X(FIND_COL_INT, "find_col_int", "ii", "i", "FR") X(FIND_COL_STR, "find_col_str", "si", "i", "FR") \
    X(FIND_ROW_FROM_INT, "find_row_from_int", "iii", "i", "FR")                                     \
    X(FIND_ROW_FROM_STR, "find_row_from_str", "sii", "i", "FR")                                     \
    X(FIND_COL_FROM_INT, "find_col_from_int", "iii", "i", "FR")                                     \
    X(FIND_COL_FROM_STR, "find_col_from_str", "sii", "i", "FR")                                     \
    X(WHERE_INT, "where_int", "iii", "", "") X(WHERE_STR, "where_str", "iis", "", "")               \
    X(NARROW_INT, "narrow_int", "iii", "", "") X(NARROW_STR, "narrow_str", "iis", "", "")           \
    X(SELECT_ALL, "select_all", "", "", "") X(SEL_LEN, "sel_len", "", "i", "R")                     \
    X(SEL_ROW, "sel_row", "i", "i", "R")                                                            \
    X(SUM_COL, "sum_col", "i", "i", "R") X(MIN_COL, "min_col", "i", "i", "FR")                      \
    X(MAX_COL, "max_col", "i", "i", "FR") X(COUNT_COL, "count_col", "i", "i", "R")                  \
    X(SUM_ROW, "sum_row", "i", "i", "R") X(MIN_ROW, "min_row", "i", "i", "FR")                      \
    X(MAX_ROW, "max_row", "i", "i", "FR") X(COUNT_ROW, "count_row", "i", "i", "R")                  \
    X(SUM_SEL, "sum_sel", "i", "i", "R") X(MIN_SEL, "min_sel", "i", "i", "FR")                      \
    X(MAX_SEL, "max_sel", "i", "i", "FR") X(COUNT_SEL, "count_sel", "i", "i", "R")                  \
    X(ORDER_BY, "order_by", "i", "", "F") X(ORDER_BY_DESC, "order_by_desc", "i", "", "F")                \
    X(LOAD_CSV, "load_csv", "i", "i", "FO") X(LOAD_XLSX, "load_xlsx", "ii", "i", "FO")              \
    X(SAVE_CSV, "save_csv", "", "i", "FO") X(SAVE_XLSX, "save_xlsx", "", "i", "FO")

enum {
#define X(id, name, in, out, flags) PNX_SHW_##id,
    PNX_SHEET_WORDS(X)
#undef X
    PNX_SHW_COUNT
};
typedef struct {
    const char *name, *in, *out, *flags;
} pnx_shword;
static const pnx_shword pnx_shwords[PNX_SHW_COUNT] = {
#define X(id, name, in, out, flags) [PNX_SHW_##id] = {name, in, out, flags},
    PNX_SHEET_WORDS(X)
#undef X
};
static inline bool pnx_shhas(const char *s, char c) {
    for (; *s; ++s)
        if (*s == c)
            return true;
    return false;
}
static inline uint32_t pnx_shlen(const char *s) {
    uint32_t n = 0;
    while (s[n])
        ++n;
    return n;
}
// Registers a word of sheet reads and writes: ints from A, a str at B.
static inline void pnx_sheet_regs(const pnx_shword *w, uint32_t *int_in, uint32_t *int_out, bool *str_in, bool *str_out) {
    uint32_t ni = 0;
    *str_in = false;
    for (const char *p = w->in; *p; ++p) {
        if (*p == 's')
            *str_in = true;
        else
            ni += *p == 'd' ? 2u : 1u; // a dec value and its scale
    }
    *int_in = ni;
    *int_out = (w->out[0] == 'i' || w->out[0] == 'b' || w->out[0] == 'D') ? 1u : 0u;
    *str_out = w->out[0] == 's';
}

// The text formats of json and csv (pinax_json.c), read a token or a field at a time.
enum {
    PNX_JSON_OBJECT = 1, PNX_JSON_OBJECT_END, PNX_JSON_ARRAY, PNX_JSON_ARRAY_END, PNX_JSON_KEY, PNX_JSON_STRING,
    PNX_JSON_NUMBER, PNX_JSON_BOOLEAN, PNX_JSON_NULL, PNX_JSON_KINDS,
    PNX_JSON_DEC = 32, // JPUTI only: a dec(N), N = B - PNX_JSON_DEC (RFC-0019); CPUTI writes one with B = 3 + N
};
int      pnx_json_next(const uint8_t *s, uint32_t n, uint32_t *start, uint32_t *len, bool *escaped, uint32_t *past);
int      pnx_json_skip(const uint8_t *s, uint32_t n, uint32_t *past);
uint32_t pnx_json_unescape(const uint8_t *in, uint32_t n, uint8_t *out);
uint32_t pnx_json_quote(const uint8_t *in, uint32_t n, uint8_t *out, uint64_t cap);
// RFC-0046. The writers give the length written, PNX_TXT_FULL when the output does not fit, PNX_TXT_BAD when the input is not what they read.
#define PNX_TXT_FULL UINT32_MAX
#define PNX_TXT_BAD (UINT32_MAX - 1u)
bool     pnx_json_valid(const uint8_t *s, uint32_t n);
int      pnx_json_at(const uint8_t *s, uint32_t n, const uint8_t *path, uint32_t pn, int *kind, uint32_t *start, uint32_t *len, bool *escaped);
uint32_t pnx_json_minify(const uint8_t *s, uint32_t n, uint8_t *out, uint64_t cap);
uint32_t pnx_json_pretty(const uint8_t *s, uint32_t n, uint32_t indent, uint8_t *out, uint64_t cap);
int      pnx_csv_field(const uint8_t *s, uint32_t n, uint32_t *start, uint32_t *len, bool *doubled, uint32_t *past, bool *last);
uint32_t pnx_csv_quote(const uint8_t *in, uint32_t n, uint8_t *out, uint64_t cap);

// The text formats toml and md (pinax_toml.c, pinax_md.c; RFC-0046), read without a tree.
enum { PNX_TOML_TABLE = 1, PNX_TOML_TABLES, PNX_TOML_STRING, PNX_TOML_INT, PNX_TOML_FLOAT, PNX_TOML_BOOL, PNX_TOML_DATETIME,
       PNX_TOML_ARRAY, PNX_TOML_INLINE };
int      pnx_toml_next(const uint8_t *s, uint32_t n, int *kind, uint32_t *key_off, uint32_t *key_len, uint32_t *val_off,
                       uint32_t *val_len, uint32_t *past);
bool     pnx_toml_valid(const uint8_t *s, uint32_t n);
size_t   pnx_toml_check_room(uint32_t n);
int      pnx_toml_check(const uint8_t *s, uint32_t n, uint8_t *mem, size_t memlen);
size_t   pnx_toml_write_room(uint32_t n, uint32_t extra);
// toml.set and toml.remove: 0 and the length written, 1 format, 2 full, 3 not found
int      pnx_toml_set(const uint8_t *s, uint32_t n, const uint8_t *path, uint32_t pn, int kind, const uint8_t *val, uint32_t vn,
                      uint8_t *out, uint64_t cap, uint32_t *outlen, uint8_t *mem, size_t memlen);
int      pnx_toml_remove(const uint8_t *s, uint32_t n, const uint8_t *path, uint32_t pn, uint8_t *out, uint64_t cap, uint32_t *outlen,
                         uint8_t *mem, size_t memlen);
int      pnx_toml_string(const uint8_t *s, uint32_t n, uint32_t *off, uint32_t *len);
uint32_t pnx_toml_decode(const uint8_t *in, uint32_t n, uint8_t *out);
int      pnx_toml_at(const uint8_t *s, uint32_t n, const uint8_t *path, uint32_t pn, int *kind, uint32_t *off, uint32_t *len);
enum { PNX_MD_HEADING = 1, PNX_MD_PARAGRAPH, PNX_MD_CODE, PNX_MD_QUOTE, PNX_MD_LIST, PNX_MD_RULE, PNX_MD_TABLE };
int      pnx_md_next(const uint8_t *s, uint32_t n, int *kind, int *level, uint32_t *off, uint32_t *len, uint32_t *past);
uint32_t pnx_md_html(const uint8_t *s, uint32_t n, uint8_t *out, uint64_t cap);
uint32_t pnx_md_inline(const uint8_t *s, uint32_t n, uint8_t *out, uint64_t cap);

// The words of json (more than json.next), toml and md that read a text and give values or write a buffer
// (RFC-0046): id, module (8 json, 11 toml, 12 md), name, what they take in stack order (i int or bool, s str), what
// they give, flags: F can fail, W writes the buffer that comes last. The ints go in the registers from A in the
// order written, the strs from B.
#define PNX_TEXT_WORDS(X)                                                                           \
    X(JSON_VALID, 8, "valid", "s", "b", "") X(JSON_AT, 8, "at", "ss", "is", "F")                    \
    X(JSON_MINIFY, 8, "minify", "s", "", "FW") X(JSON_PRETTY, 8, "pretty", "si", "", "FW")          \
    X(TOML_VALID, 11, "valid", "s", "b", "") X(TOML_AT, 11, "at", "ss", "is", "F")                  \
    X(TOML_NEXT, 11, "next", "s", "isss", "F") X(TOML_CHECK, 11, "check", "s", "", "F")            \
    X(TOML_SET, 11, "set", "sssi", "", "FW") X(TOML_REMOVE, 11, "remove", "ss", "", "FW")           \
    X(MD_NEXT, 12, "next", "s", "iiss", "F") X(MD_HTML, 12, "html", "s", "", "FW")                  \
    X(MD_INLINE, 12, "inline", "s", "", "FW")

enum {
#define X(id, module, name, in, out, flags) PNX_TXW_##id,
    PNX_TEXT_WORDS(X)
#undef X
    PNX_TXW_COUNT
};
typedef struct {
    uint8_t     module;
    const char *name, *in, *out, *flags;
} pnx_txword;
static const pnx_txword pnx_txwords[PNX_TXW_COUNT] = {
#define X(id, module, name, in, out, flags) [PNX_TXW_##id] = {module, name, in, out, flags},
    PNX_TEXT_WORDS(X)
#undef X
};
static inline bool pnx_txhas(const char *s, char c) {
    for (; *s; ++s)
        if (*s == c)
            return true;
    return false;
}
// The registers of a text word: ints (and bools) in and out from A, strs in and out from B.
static inline void pnx_text_regs(const pnx_txword *w, uint32_t *ni, uint32_t *ri, uint32_t *si, uint32_t *ro) {
    *ni = *ri = *si = *ro = 0;
    for (const char *p = w->in; *p; ++p)
        *(*p == 's' ? si : ni) += 1;
    for (const char *p = w->out; *p; ++p)
        *(*p == 's' ? ro : ri) += 1;
}

// What the last ptn.match of a run found, for ptn.capture: the whole match and the captures.
typedef struct pnx_ptn_found {
    pnx_scell cap[1 + PNX_PTN_CAPS];
    uint32_t  n;       // 0: no match yet; else 1 + the captures
    pnx_scell skipped; // rex.replace_by: the code point kept after an empty match (XCAPI 10)
} pnx_ptn_found;

typedef struct pnx_frame {
    uint32_t pc, int_base, str_base, reserved;
} pnx_frame;

typedef struct pnx_counts {
    uint8_t int_args, str_args, int_results, str_results;
    bool    fails;
    bool    yields; // may yield (RFC-0005 section 3.5), worked out when the code is loaded
} pnx_counts;

typedef struct pnx_host {
    char          module[16];
    char          name[32];
    char          signature[72];
    pinax_host_fn fn;
    void         *user;
    uint32_t      flags;
    pnx_counts    counts;
    // the narrow types of the results (RFC-0025): per integer result, a width code plus one, or 0
    // where the value is an int, a u64 or anything else that takes 64 bits. Arguments need no
    // check: the compiler typed them. A narrow result beyond the eighth refuses the registration.
    uint8_t       res_width[PNX_HOST_WIDTHS];
    // a C function named by `extern`: how its arguments are laid out for the call
    pinax_extern_fn ext;
    uint8_t       ext_n, ext_result; // parameters; the result: PNX_XA_NONE, PNX_XA_INT or a handle
    uint8_t       ext_kind[8];
} pnx_host;

// Kinds of an extern parameter. A handle is PNX_XA_HANDLE plus its type number, and
// PNX_XA_OWNED more when the function takes the object over.
enum { PNX_XA_INT = 0, PNX_XA_STR = 1, PNX_XA_CSTR = 2, PNX_XA_NONE = 3, PNX_XA_HANDLE = 0x10, PNX_XA_OWNED = 0x40 };

// A kind of host object scripts can hold (pinax_handle_type).
typedef struct pnx_handle_type {
    char  name[32];
    void (*close)(void *object, void *user);
    void *user;
} pnx_handle_type;

// One slot of the handle table. A handle is the slot number plus one in the low 32 bits and
// the slot's generation in the high 32; the generation is odd while the slot is in use.
typedef struct pnx_handle_slot {
    void    *object;
    uint32_t generation, type; // type: number of its pnx_handle_type plus one
    uint32_t next_free;        // free list: slot number plus one
    uint32_t run;              // the run that opened it, so that a fault can close it
} pnx_handle_slot;

typedef struct pnx_global {
    uint8_t *ptr; // a buffer starts with its 64-bit length
    uint8_t *own; // its storage in the block; ptr differs while the host has bound it
    uint32_t kind, elem_bytes, capacity;
    uint32_t length; // cells in use: the capacity for an array or table, what was pushed for a list
    uint32_t head;   // a queue: the cell of its oldest value; a pool: its first free slot plus one
    uint32_t key;    // an index: its key field, PNX_FIELD(table, offset, extra)
    bool     key_is_str;
    bool     shared; // declared `shared`: another state's storage, or none until pinax_share() gives it
    uint8_t  width;  // an array, list or queue of values: the width code of a cell (PNX_W_64: 8 bytes)
    bool     work;   // the work area of SORT and SORTBY (PNX_GLOBAL_WORK)
} pnx_global;

// The instructions that change the global they name (in BX, C, a wide form's second word or a
// field's). None may name a shared global (RFC-0020 section 4); LCOPYR and SORTBY write their BX only.
static inline bool pnx_writes_global(uint32_t op) {
    switch (op) {
    case PNX_SETG: case PNX_ASET: case PNX_ASETW: case PNX_GCLEAR: case PNX_LPUSH: case PNX_LPUSHS: case PNX_LPOP:
    case PNX_LPOPS: case PNX_FSET: case PNX_FSETS: case PNX_ADDROW: case PNX_IXPUT: case PNX_IXPUTO: case PNX_IXDEL: case PNX_BCLEAR:
    case PNX_BPUT: case PNX_BPUTS: case PNX_BPUTW: case PNX_BPUTSW: case PNX_BCAT: case PNX_BCATI: case PNX_BENC:
    case PNX_BENCW: case PNX_BTRUNC: case PNX_JPUTI: case PNX_JPUTIW: case PNX_JPUTS: case PNX_JPUTSW: case PNX_CPUTS:
    case PNX_CPUTSW: case PNX_CPUTI: case PNX_CPUTIW: case PNX_LCOPYR: case PNX_QPUT: case PNX_QTAKE: case PNX_QWTAKE:
    case PNX_QWPUT: case PNX_PALLOC: case PNX_PFREE: case PNX_SORT: case PNX_SORTBY: case PNX_SHEET: case PNX_SHEETF: case PNX_TXTW: case PNX_TXTWF: return true;
    default: return false;
    }
}

struct pinax_call {
    pinax_state *state;
    uint64_t     token; // of the operation the host word started and left pending; 0: none
};

// A run that waits for a host operation: where the results go when pinax_complete() brings them.
typedef struct pnx_wait {
    uint64_t        token; // 0: not waiting
    const pnx_host *host;
    uint32_t        ia, sa; // the first argument slots of the call, from the start of the banks
    bool            fails;  // the call was made by HCALLF: a failure word follows
} pnx_wait;

// What a run works in: its registers, its control stack, its part of the region and where it
// stands. The main run has one; every task has its own.
typedef struct pnx_context {
    int64_t   *ibank;
    pnx_scell *sbank;
    pnx_frame *ctrl;
    uint32_t   int_slots, str_slots, depth_max, region_used, region_len;
    uint32_t   region_floor; // RCUT goes no lower: the start of this run's part of the region
    uint32_t   pc, int_base, str_base, depth, depth_floor;
    uint32_t   resume_dst; // the register that learns whether the task it resumed has ended; UINT32_MAX: none
    bool       ends_task;  // returning at the floor ends a task, not the run
} pnx_context;

enum { PNX_TASK_FREE, PNX_TASK_READY, PNX_TASK_RUNNING, PNX_TASK_WAITING };

typedef struct pnx_task {
    pnx_context      ctx;    // where it stands while it is not the one running
    struct pnx_task *parent; // who resumed it (NULL: the main run), while it runs
    uint32_t         generation, in_host;
    uint8_t          state;
    bool             cancelled, yielded;
    bool             blocked;    // it yielded for a queue, when the queue count was blocked_at
    bool             apart;      // its set is declared `apart`: a fault ends this task only (RFC-0029)
    int64_t          tag;        // what the task named itself with (task_tag, RFC-0029 P-L6); 0 at spawn
    // its turn limit (RFC-0029 P-L3; 0: none). While it runs, the run's `left` is the smaller of what is
    // left of its turn and of the run: `given` when that was settled, `outer` the run's then, `rest` the
    // turn's then, `cut` when the turn was the smaller
    uint64_t         turns, rest, outer, given;
    bool             cut;
    uint64_t         blocked_at;
    pnx_wait         wait;       // the host operation it waits for
    pinax_err        wake_fail;  // what the operation failed with, for the failure word it goes on at
    pinax_err        wake_fault; // or the fault it ended with
    // its own memory, inside the block
    int64_t   *ibank;
    pnx_scell *sbank;
    pnx_frame *ctrl;
    uint32_t   int_slots, str_slots, depth_max, region_base, region_bytes;
    pnx_ptn_found ptn; // its last ptn.match
} pnx_task;

// The parts of a program, each a segment of the block with a capacity (RFC-0024): a load appends
// to them; a full one is taken again at twice the size, and the old one is dead room until the
// next compaction. Their counts are the state's `counts` (a pnx_head).
enum {
    PNX_PART_WORDS,      // pnx_wordrec
    PNX_PART_GLOBALRECS, // pnx_globalrec
    PNX_PART_IMPORTRECS, // pnx_importrec
    PNX_PART_ICONST,     // int64_t
    PNX_PART_SCONST,     // pnx_sconst
    PNX_PART_CODE,       // uint32_t
    PNX_PART_STRBYTES,   // uint8_t (segment[PNX_SEG_CONST])
    PNX_PART_LINES,      // pnx_linerec, unaligned
    PNX_PART_LOCALS,     // pnx_localrec, unaligned
    PNX_PART_ERRS,       // pnx_sconst: the name of each declared failure code, in strbytes (RFC-0037)
    PNX_PART_WCOUNTS,    // pnx_counts, one per word
    PNX_PART_PROVEN,     // uint8_t, one per code word
    PNX_PART_IMPORTS,    // const pnx_host *, one per import
    PNX_PART_GLOBALS,    // pnx_global, one per global
    PNX_PARTS
};

// The compiler's declaration tables, kept for the next load (RFC-0026): each a segment of the
// block with a count and a capacity that a load appends to in place, as the program's parts are;
// a full one is taken again at twice the size and the old one is dead room. Their items are the
// compiler's own (pinax_compile.c). The names they hold lie in the name chunks, which do not move
// while the state is in use: a full chunk is followed by a bigger one. The six name indexes hold
// entry numbers (`n` entries indexed, `cap` slots, and one slot more that counts the slots filled).
// Kept here by value, so that a load that fails puts them back with the rest of the program.
enum {
    PNX_SYM_WORDS, PNX_SYM_CONSTS, PNX_SYM_GLOBALS, PNX_SYM_RECORDS, PNX_SYM_FIELDS, PNX_SYM_BTYPES,
    PNX_SYM_DTYPES, PNX_SYM_SRCMODS, PNX_SYM_EXTERNS, PNX_SYM_RTYPES,
    PNX_SYM_TABLES,
    PNX_SYM_IX = PNX_SYM_TABLES, // the indexes of words, consts, globals, records, externs, dtypes
    PNX_SYM_OPENED = PNX_SYM_IX + 6, // the session's modules: the name a line wrote, and the module
    PNX_SYMS
};
#define PNX_NAME_CHUNKS 24

typedef struct pnx_symbols {
    void       *ptr[PNX_SYMS];
    uint32_t    n[PNX_SYMS], cap[PNX_SYMS];
    uint8_t    *chunk[PNX_NAME_CHUNKS];
    uint32_t    chunk_used[PNX_NAME_CHUNKS], chunk_cap[PNX_NAME_CHUNKS];
    uint32_t    n_chunks;
    bool        kept; // a source was loaded: the tables are the compiler's
    // the session of pinax_eval(): its module number (UINT32_MAX before the first line); the
    // modules its lines opened are the part PNX_SYM_OPENED
    uint32_t    session;
    // the session's search order (RFC-0040 part C): numbers of entries of PNX_SYM_OPENED
    uint8_t     n_search, search[16];
} pnx_symbols;

// What a load changes, kept so that a failed load can put it back: the counts by value, the
// parts and their capacities, and the rest of what points at the program.
typedef struct pnx_program {
    const pnx_head *head; // NULL, or the state's own counts
    pnx_head        counts;
    void           *parts[PNX_PARTS];
    uint32_t        caps[PNX_PARTS];
    pnx_global     *globals; // parts[PNX_PART_GLOBALS], for the readers that name it
    const uint8_t  *seg_const, *seg_global;
    pnx_symbols     symbols;
    size_t          used;
    uint32_t        n_hosts;
} pnx_program;

struct pinax_state {
    uint8_t *block;
    size_t   block_len, used, needed;

    int64_t   *ibank;
    pnx_scell *sbank;
    pnx_frame *ctrl;
    uint8_t   *region;
    pnx_host  *hosts;
    uint32_t   int_slots, str_slots, depth_max, region_len, region_used, n_hosts;
    uint32_t   region_floor; // of the context running now (pnx_context)

    // loaded code; all inside the block. `head` is NULL until a program is loaded, then `&counts`:
    // the counts of the parts, which loads append to (RFC-0024, PNX_PART_*).
    const pnx_head     *head;
    pnx_head            counts;
    uint32_t            part_cap[PNX_PARTS];
    const pnx_wordrec  *words;
    const pnx_globalrec *globalrecs;
    const pnx_importrec *importrecs;
    const int64_t      *iconst;
    const pnx_sconst   *sconst;
    const uint32_t     *code;
    pnx_counts         *word_counts;
    uint8_t            *proven; // per code word: why its check cannot fire (RFC-0023), 0 when not proven
    const pnx_host    **imports;
    pnx_global         *globals;

    const uint8_t *segment[PNX_SEG_COUNT];
    uint32_t       host_len[PNX_SEG_KEPT - PNX_SEG_HOST]; // the bytes of each string argument of this run
    uint32_t       host_segments; // segments past PNX_SEG_HOST in use for this run

    uint64_t budget, left, instructions;
    uint64_t charge; // bytes scanned by string words since the last CHECK; 16 of them count as one instruction
    uint32_t owed; // cost of the CHECK a run was suspended at
    uint32_t pc, int_base, str_base, depth, depth_peak;
    uint32_t fault_pc;             // where the last run ended with a fault: the code word after the instruction
    const uint8_t *lines;          // pnx_linerec records, head->n_lines of them (not aligned)
    char     where[112];           // "module:line: error" of the last fault, for pinax_error_text()
    char     task_where[112];      // the same of the last task of an `apart` set that faulted (RFC-0029)
    uint64_t task_faults;          // how many such tasks ended with a fault
    bool     task_fault_kept;      // this run had one: its locals stay readable after the run ends well
    uint32_t fault_ibase, fault_sbase; // the register windows of the word that faulted
    int64_t   *fault_ibank;            // in the banks of the run it faulted in (a task's, perhaps),
    pnx_scell *fault_sbank;            // until fault_locals() has read them
    uint32_t   fault_int_slots, fault_str_slots;
    const uint8_t *localrecs;      // pnx_localrec records, head->n_locals of them (not aligned)
    const pnx_sconst *errs;        // the names of the declared failure codes, head->n_errs of them
    // the locals of the word of the last fault, copied when it happened: names, types and the
    // bytes of str values lie in fault_pool
    struct pnx_faultlocal {
        uint16_t name, type, text, text_len;
        uint8_t  name_len, type_len, kind;
        int64_t  i;
    } fault_locals[16];
    uint32_t fault_n, fault_used;
    uint8_t  fault_pool[1024];
    uint32_t depth_floor;          // the depth at which the running word returns to the host
    // tasks: the one running (NULL: the main run), and where the main run stands meanwhile
    pnx_task   *task;
    pnx_context main;
    pnx_wait    main_wait;  // the main run waits for a host operation
    uint64_t    qturn;      // counts the changes of every queue: a task blocked since the last one cannot go
    uint32_t    gsel;       // the global GSEL chose for the instruction after it (RFC-0018)
    uint32_t    sort_work;  // the global SORT last took its work area from (checked at each use)
    pinax_err   main_fault; // that operation failed and the word that called it cannot fail
    uint64_t    token_serial;
    bool        waiting;    // the suspended run ended with PINAX_WAITING
    uint32_t    resume_dst;
    bool        ends_task;
    uint32_t int_peak, str_peak, region_peak;
    // the host word running now (NULL: none) and where its arguments lie: a word it runs
    // gets its window above them. `nest` counts the runs started from inside host words.
    const pnx_host *host_cur;
    int64_t        *host_ia;
    pnx_scell      *host_sa;
    uint32_t        nest;
    uint32_t top_word;
    bool     suspended;
    bool     loading;   // the suspended run is the code outside definitions of a load
    bool     from_image; // opened from an image: host words are given again by name (D0036)
    bool     frozen;     // lent a global with pinax_share(): it neither runs nor loads again
    bool     load_runs;  // the code of a load runs: shared globals not given yet read as empty
    // exported code (RFC-0028): the table the host attached, and whether it is in use for the program
    // as it stands (native_code and native_words: the program it was checked against; another length
    // checks it again). native_prefix: the code words it covers while it is in use, else 0 (RFC-0039);
    // the interpreter gives a run back to it at a call or a return to a place below that.
    const pinax_native *native;
    bool                native_bound;
    uint32_t            native_code, native_words;
    uint32_t            native_prefix;
    uint64_t            native_runs;  // runs that went through it (for the tests)
    uint64_t            native_leaves; // times it gave the run to the interpreter past its code (for the tests)
    uint64_t            native_backs;  // times the interpreter gave the run back to it (for the tests)
    bool                export_only; // pnx export-c: a load compiles and checks its code and runs none of it
    pnx_program before_load; // what that load would be undone to
    // redefine: the record of the word whose code a load traded, as it was, until the load is final
    // (a failed load, or one whose code faults, puts it back: pnx_program_restore)
    pnx_wordrec undo_word;
    uint32_t    undo_old, undo_new;
    bool        undo_pending;
    pnx_symbols symbols; // the compiler's declaration tables, kept for the next load (pinax_compile.c)
    pinax_err   fail;
    const char *error_text;
    void (*diag)(void *user, pinax_str file, uint32_t line, uint32_t column, pinax_str message);
    void *diag_user;
    void (*barrier)(void *user); // pinax_on_barrier: a dot, the end of a session line or of load code
    void *barrier_user;
    pinax_resolve_fn resolve; // gives the C functions that `extern` names
    // the session stack (RFC-0040 part A): what the lines of pinax_eval() left, the deepest
    // first; its types (the compiler's numbers) and values, a str's bytes copied here
    uint32_t  eval_n;
    uint16_t  eval_type[16];
    int64_t   eval_int[16];
    uint8_t   eval_len[16];
    uint8_t   eval_bytes[16][255];
    // a line that was suspended: what it will leave, kept when it ends (pinax_resume)
    bool      eval_pending;
    uint32_t  eval_next_n;
    uint16_t  eval_next_type[16];
    uint16_t  eval_next_text; // bit k: value k is a str
    void            *resolve_user;
    uint8_t hash_key[16]; // of the lookup indexes; inside the block, so images carry it

    pnx_handle_type  handle_types[PNX_MAX_HANDLE_TYPES];
    pnx_handle_slot *handles;
    uint32_t         n_handle_types, handle_slots, handle_free, handles_open, handles_peak;
    uint32_t         run_serial; // counts runs; see pnx_handle_slot.run
    pnx_ptn_found    ptn;        // the main run's last ptn.match
    // cold, and last so that they move none of the fields a run reads (RFC-0029)
    bool                turn_limits;   // a set of tasks has a turn limit: `left` is counted even with no budget
    uint64_t            counted;       // budget or turn_limits, set when a run starts: `left` is counted
    pinax_task_fault_fn on_task_fault; // told of each fault of an `apart` task as it happens (P-L6)
    void               *on_task_fault_user;
    struct pnx_capacity {              // the capacities the host gave for `host` (pinax_capacity, P-L2)
        char     name[48];             // a module's name, a point and a global's name
        uint32_t value;
    } capacities[16];
    uint32_t n_capacities;
};

// The instruction table as data: operand kinds, shape, second word (pinax_code.h).
typedef struct pnx_opinfo {
    uint8_t a, b, c, shape, second, fails;
    uint8_t known;    // 0: this number is no instruction
    char    roles[4]; // what it does with the registers named by A, B, C (pinax_code.h)
} pnx_opinfo;
extern const pnx_opinfo pnx_ops_info[PNX_OP_COUNT];

// Counts the int and str parameters and results of "(a int, b str gives r int or fail)".
bool pnx_signature_counts(const char *signature, pnx_counts *out);
// The narrow types of a signature (RFC-0025): for the first PNX_MAX_WIDTHS integer arguments and
// results, a width code plus one where the type is u8 u16 u32 i8 i16 i32, else 0. False when a
// narrow type lies beyond that many.
bool pnx_signature_widths(const char *signature, uint8_t args[PNX_MAX_WIDTHS], uint8_t results[PNX_MAX_WIDTHS]);
// Whether `value` is one of the width that `code_plus_one` names (0: every value).
static inline bool pnx_width_holds(int64_t value, uint8_t code_plus_one) {
    switch (code_plus_one) {
    case 0: return true;
    case PNX_W_U8 + 1: return value >= 0 && value <= 255;
    case PNX_W_U16 + 1: return value >= 0 && value <= 65535;
    case PNX_W_U32 + 1: return value >= 0 && value <= 4294967295ll;
    case PNX_W_I8 + 1: return value >= -128 && value <= 127;
    case PNX_W_I16 + 1: return value >= -32768 && value <= 32767;
    case PNX_W_I32 + 1: return value >= -2147483648ll && value <= 2147483647ll;
    default: return true;
    }
}

// Runs from state->pc until the top word returns, faults, fails or the budget runs out.
pinax_err pnx_execute(pinax_state *state);
// The same for a state whose exported code covers a prefix (RFC-0039, pnx_leave_native): a call or a
// return to a place below native_prefix gives the run back (PNX_STEP_ON, the place in the state).
pinax_err pnx_execute_prefix(pinax_state *state);
// One instruction of the run that stands in the state (RFC-0028): PNX_STEP_ON when it went on, with the
// pc, bases, depth and budget put back into the state; otherwise how the run ended, stopped or waits.
#define PNX_STEP_ON ((pinax_err)0x7ffffff1)
pinax_err pnx_step(pinax_state *state);
// The hash an exported program carries: its first code_words of code, where each of its first `words`
// words starts, and the layout of this build.
uint64_t pnx_native_hash(const pinax_state *st, uint32_t code_words, uint32_t words);
// Runs the code from where the run stands: through the attached exported code when the program begins
// with the code it covers.
pinax_err pnx_run_code(pinax_state *st);
// Exported code at a place past the code it covers (RFC-0039): pnx_execute_prefix runs from there until
// a call or a return goes back below native_prefix (PNX_STEP_ON, the place in the state) or the run ends.
pinax_err pnx_leave_native(pinax_state *st);
// The program's code as C (pinax_export.c): `name` is the table's name. 0 when `cap` is too small.
size_t pnx_export_c(const pinax_state *st, const char *name, char *out, size_t cap);
// A fault in a task of an `apart` set (RFC-0029 P-L1): noted (pnx_note_task_fault), the task ended, the
// state left in the context of whoever resumed it. False when the fault ends the run as before.
bool pnx_fault_apart(pinax_state *st, pinax_err result, uint32_t at, uint32_t ibase, uint32_t sbase);
void pnx_note_task_fault(pinax_state *st, pinax_err err);
// The run's `left` ran out at a CHECK (RFC-0029 P-L3): true when it was a task's turn that ran out, and
// the task faults with cancelled; otherwise the budget ran out, each task with a turn limit keeps what is
// left of its turn, and the run suspends. What `left` still held (less than the CHECK's cost) is dropped
// either way, as a suspension drops it. pnx_turns_again settles `left` again when the run resumes.
bool     pnx_turn_out(pinax_state *st);
// Exported code's two ways out (RFC-0028, RFC-0029): at a fault, and where `left` ran out. Each gives
// PNX_STEP_ON when the run goes on from where the state says (a task of an `apart` set ended alone):
// pnx_run_code then enters the exported code again. So that code never reloads `left` itself.
pinax_err pnx_exec_fault(pinax_state *st, pinax_err result, uint32_t at, uint32_t ibase, uint32_t sbase,
                         const int64_t *R, uint32_t floor, uint64_t left, uint64_t count);
// pnx_exec_out takes six arguments, as pnx_exec_suspend does, and the depth floor from the state (where
// exported code keeps it right): two more arguments, or storing them first, cost the hot loop of exported
// code 16 to 20 percent (measured, int_loop).
pinax_err pnx_exec_out(pinax_state *st, uint32_t at, uint32_t ibase, uint32_t sbase, uint32_t depth, uint64_t count);
// HCALL and HCALLF for exported code (RFC-0028 section 9, D0066): what pnx_step does for them, without the
// interpreter's dispatch. The run's place is in the state before and after, as for pnx_step.
pinax_err pnx_step_host(pinax_state *st);
uint64_t pnx_turns_again(pinax_state *st, uint64_t left);
// The two ways out of a run that pnx_execute, pnx_step and exported code share (pinax_vm.c).
pinax_err pnx_exec_end(pinax_state *st, pinax_err result, uint32_t at, uint32_t ibase, uint32_t sbase,
                       const int64_t *R, uint32_t floor, uint64_t left, uint64_t count);
pinax_err pnx_exec_suspend(pinax_state *st, uint32_t at, uint32_t ibase, uint32_t sbase, uint32_t depth,
                           uint64_t count);

// RFC-0023: what the loader proved of an instruction's run-time check (state->proven).
enum {
    PNX_PROVEN_NONE,
    PNX_PROVEN_INDEX,       // the index lies in [0, capacity - 1] of an always-full array
    PNX_PROVEN_INDEX_AGAIN, // the index passed this collection's check earlier in the word
    PNX_PROVEN_ROW,         // the row lies in [1, capacity] of an always-full table
    PNX_PROVEN_ROW_AGAIN,   // the row passed this table's check (a pool's generation too) earlier
    PNX_PROVEN_NO_OVERFLOW, // both operands lie in ranges whose result fits an int
};
bool        pnx_checked_op(uint32_t op); // the instructions whose check the proofs cover
const char *pnx_proven_text(uint8_t reason);
void        pnx_checks_report(const pinax_state *state, void (*put)(void *user, const char *text, size_t len), void *user);

// Text helpers on the vendored proven (pinax_text.c).
// Decimal integer with an optional sign; the whole view must be the number.
bool pnx_parse_int(const uint8_t *bytes, uint32_t length, int64_t *out);
// Decimal text of `value` into `out`; returns its length (at most 20).
uint32_t pnx_format_int(int64_t value, uint8_t out[24]);
uint32_t pnx_format_uint(uint64_t value, uint8_t out[24]); // a u64 (RFC-0025): the bits as an unsigned number
// dec(N) (RFC-0019): `value` scaled by 10^scale as text ("-0.05"), and back. Reading takes an
// optional sign, digits, and a point followed by at most `scale` digits.
uint32_t pnx_format_dec(int64_t value, uint32_t scale, uint8_t out[24]);
bool     pnx_parse_dec(const uint8_t *bytes, uint32_t length, uint32_t scale, int64_t *out);
extern const int64_t pnx_pow10[19];
// Offset of the first `part` in `bytes`, or UINT32_MAX. An empty part is found at 0.
uint32_t pnx_find(const uint8_t *bytes, uint32_t length, const uint8_t *part, uint32_t part_length);
// Hash of key bytes for an index: the vendored proven's keyed SipHash-2-4.
uint64_t pnx_hash(const uint8_t key[16], const uint8_t *bytes, uint32_t length);
// The words of the modules hash and encode, over proven_c_lib. pnx_encode writes at most `cap`
// bytes to `out`: 0 done (`*written` set), 1 not enough room, 2 the input is not of the form.
uint64_t pnx_hash_kind(uint32_t kind, const uint8_t *bytes, uint32_t length);
int      pnx_encode(uint32_t kind, const uint8_t *in, uint32_t length, uint8_t *out, uint64_t cap, uint64_t *written);
// The name of an error number as a view into the interpreter's static text.

extern const char pnx_static_text[];
extern const uint32_t pnx_static_len; // bytes of pnx_static_text

// From pinax_compile.c: the bytes of the storage of kept table `table` (PNX_SYM_*), as a piece of
// the block; and after the block moved (compaction, an image), every pointer of the kept tables
// is given to `where`, which says where it lies now (NULL: nowhere it may point).
size_t   pnx_symbols_bytes(const pinax_state *state, unsigned table);
bool     pnx_symbols_relocate(pinax_state *state, void *(*where)(void *user, const void *p), void *user);
// After a load that declared: the live pieces of the program move down over the copies a load
// left behind, so that the block holds one (RFC-0024). Does nothing while a run, a load, a host
// word or a task is in the middle of something.
void pnx_compact(pinax_state *state);
// A load is final (its code ran through): what it traded stays, and the block is compacted.
void pnx_load_done(pinax_state *state);
uint64_t pnx_symbols_layout(uint64_t hash);
void pnx_err_name(int64_t err, uint32_t *offset, uint32_t *length);
// The name of an error number: a declared failure code's (RFC-0037), or pnx_err_name's.
pinax_str pnx_err_label(const pinax_state *state, int64_t err);
bool pnx_tasks_alive(const pinax_state *state); // a task was spawned and has not ended
// The session stack takes what a line left (st->eval_next_*): ints in `ri`, strs in `rs`, each
// bank in the order of the stack. Refused when a str is longer than 255 bytes; then it stays.
pinax_err pnx_session_keep(pinax_state *state, const int64_t *ri, const pinax_str *rs);
// The words whose code calls `word` or takes it as a value (not itself, not the code outside
// definitions): how many, the first `cap` of them in `out`.
uint32_t pnx_callers_of(const pinax_state *state, uint32_t word, uint32_t *out, uint32_t cap);
// The effects of a word in full (RFC-0040 part G): pinax_effects, and which globals it reads and
// writes (the first 32 of each; `chosen`: one a GSEL chooses, which is not known here).
typedef struct pnx_effects {
    pinax_effects pub;
    uint32_t      n_reads, n_writes;
    uint16_t      reads[32], writes[32];
    bool          chosen_read, chosen_write;
} pnx_effects;
pinax_err pnx_word_effects(pinax_state *state, uint32_t word, pnx_effects *out);
// pnx see --effects: a line for each word of module `module`, "name: none" or what it may do.
void pnx_effects_report(pinax_state *state, uint32_t module, void (*put)(void *user, const char *text, size_t len),
                        void *user);
// Offset of the last `part` in `bytes`, or UINT32_MAX. An empty part is found at the end.
uint32_t pnx_find_last(const uint8_t *bytes, uint32_t length, const uint8_t *part, uint32_t part_length);
// Offset of the first `byte`, or UINT32_MAX.
uint32_t pnx_find_byte(const uint8_t *bytes, uint32_t length, uint8_t byte);

// The loaded program as it is now, and putting an earlier one back (a failed load).
pnx_program pnx_program_now(const pinax_state *state);
void pnx_program_restore(pinax_state *state, const pnx_program *program);
// Loads a container: appends what it carries to the program the state holds (which must be the
// one the container continues), checks the new words, and undoes everything on a refusal.
pinax_err pnx_load_container(pinax_state *state, pinax_str bytecode);
// The program of the state as one container made for an empty state (base zero), for tests and
// the fuzzer: the bytes written, or 0 when `cap` is too small or the state holds no program.
size_t pnx_program_bytes(const pinax_state *state, unsigned char *out, size_t cap);
typedef struct span {
    uint32_t begin, end; // instruction indexes of one word
} span;

// The code of word `index`: [entry, entry + length).
static inline span pnx_word_span(const pinax_state *state, uint32_t index) {
    const pnx_wordrec *w = &state->words[index];
    return (span){w->entry, w->entry + w->length};
}
// Bytes from the block, zeroed; NULL when it is full (pinax_needed() then tells how much).
void *pnx_take(pinax_state *state, size_t bytes, size_t align);
// The number of the source module called `name` (of `length` bytes), or UINT32_MAX.
// The host word that stands for the C function `name` of library `module` ("@lib"), asking
// the resolver for it when it is not there yet.
pinax_err pnx_extern_link(pinax_state *state, const char *module, const char *name, const char *signature,
                          uint32_t *index);
// Ends every task and puts the main run back in place: after a fault inside a task.
void pnx_tasks_abort(pinax_state *state);
// The banks the fault_ibase and fault_sbase of a fault are in: those of the run now.
void pnx_fault_banks(pinax_state *state);
// Where the run stands now, and back to it. A word run from a host word keeps it, so that
// the run goes back there however that word ended.
void pnx_ctx_save(const pinax_state *state, pnx_context *context);
// That word ended badly while a task it resumed ran: the tasks entered since `task` was the
// one running end, and the run is put back at `context`.
void pnx_tasks_unwind(pinax_state *state, pnx_task *task, const pnx_context *context);
// The name of source module `number`; empty when it has none.
pinax_str pnx_module_name(const pinax_state *state, uint32_t number);
uint32_t pnx_module_number(const pinax_state *state, const char *name, size_t length);

// Bytes in the main task's region; NULL when it is full.
uint8_t *pnx_region_alloc(pinax_state *state, size_t bytes);
// What a run took from the region since `mark` is given back: the one way back, so that a
// record of the region, if one is ever kept, is kept here.
static inline void pnx_region_give_back(pinax_state *state, uint32_t mark) {
    state->region_used = mark;
}

#endif // PINAX_INTERNAL_H
