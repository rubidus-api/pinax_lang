// pinax_internal.h - the state as the interpreter sees it. Not a public header.
#ifndef PINAX_INTERNAL_H
#define PINAX_INTERNAL_H

#include "pinax.h"
#include "pinax_code.h"

#include <stdbool.h>

#define PNX_MAX_HOSTS 64
#define PNX_MAX_STR_ARGS 8
#define PNX_MAX_STR_RESULTS 4

// A string value: bytes `offset .. offset + length` of a segment. Never an address, so the
// block can be copied or mapped elsewhere and the value still holds.
typedef struct pnx_scell {
    uint32_t segment;
    uint32_t length;
    uint64_t offset;
} pnx_scell;

#define PNX_SNEXT_DONE UINT64_MAX // offset of a sequence that is used up

typedef struct pnx_frame {
    uint32_t pc, int_base, str_base, reserved;
} pnx_frame;

typedef struct pnx_counts {
    uint8_t int_args, str_args, int_results, str_results;
    bool    fails;
} pnx_counts;

typedef struct pnx_host {
    char          module[16];
    char          name[32];
    char          signature[72];
    pinax_host_fn fn;
    void         *user;
    uint32_t      flags;
    pnx_counts    counts;
} pnx_host;

typedef struct pnx_global {
    uint8_t *ptr; // a buffer starts with its 64-bit length
    uint32_t kind, elem_bytes, capacity;
    uint32_t length; // cells in use: the capacity for an array or table, what was pushed for a list
    uint32_t key;    // an index: its key field, PNX_FIELD(table, offset, extra)
    bool     key_is_str;
} pnx_global;

struct pinax_call {
    pinax_state *state;
};

struct pinax_state {
    uint8_t *block;
    size_t   block_len, used, needed;

    int64_t   *ibank;
    pnx_scell *sbank;
    pnx_frame *ctrl;
    uint8_t   *region;
    pnx_host  *hosts;
    uint32_t   int_slots, str_slots, depth_max, region_len, region_used, n_hosts;

    // loaded code; all inside the block
    const pnx_head     *head;
    const pnx_wordrec  *words;
    const pnx_importrec *importrecs;
    const int64_t      *iconst;
    const pnx_sconst   *sconst;
    const uint32_t     *code;
    pnx_counts         *word_counts;
    const pnx_host    **imports;
    pnx_global         *globals;

    const uint8_t *segment[PNX_SEG_COUNT];
    uint32_t       host_segments; // segments past PNX_SEG_HOST in use for this run

    uint64_t budget, left, instructions;
    uint32_t owed; // cost of the CHECK a run was suspended at
    uint32_t pc, int_base, str_base, depth, depth_peak;
    uint32_t top_word;
    bool     suspended;
    pinax_err   fail;
    const char *error_text;
    void (*diag)(void *user, pinax_str file, uint32_t line, uint32_t column, pinax_str message);
    void *diag_user;
};

// The instruction table as data: operand kinds, shape, second word (pinax_code.h).
typedef struct pnx_opinfo {
    uint8_t a, b, c, shape, second, fails;
} pnx_opinfo;
extern const pnx_opinfo pnx_ops_info[PNX_OP_COUNT];

// Counts the int and str parameters and results of "(a int, b str gives r int or fail)".
bool pnx_signature_counts(const char *signature, pnx_counts *out);

// Runs from state->pc until the top word returns, faults, fails or the budget runs out.
pinax_err pnx_execute(pinax_state *state);

// Text helpers on the vendored proven (pinax_text.c).
// Decimal integer with an optional sign; the whole view must be the number.
bool pnx_parse_int(const uint8_t *bytes, uint32_t length, int64_t *out);
// Decimal text of `value` into `out`; returns its length (at most 20).
uint32_t pnx_format_int(int64_t value, uint8_t out[24]);
// Offset of the first `part` in `bytes`, or UINT32_MAX. An empty part is found at 0.
uint32_t pnx_find(const uint8_t *bytes, uint32_t length, const uint8_t *part, uint32_t part_length);
// Hash of key bytes for an index (the vendored proven's FNV-1a).
uint64_t pnx_hash(const uint8_t *bytes, uint32_t length);
// The name of an error number as a view into the interpreter's static text.
extern const char pnx_static_text[];
void pnx_err_name(int64_t err, uint32_t *offset, uint32_t *length);
// Offset of the first `byte`, or UINT32_MAX.
uint32_t pnx_find_byte(const uint8_t *bytes, uint32_t length, uint8_t byte);

// Forgets the loaded code and gives back the block from `mark` on.
void pnx_unload(pinax_state *state, size_t mark);

// Bytes in the main task's region; NULL when it is full.
uint8_t *pnx_region_alloc(pinax_state *state, size_t bytes);

#endif // PINAX_INTERNAL_H
