// pinax_internal.h - the state as the interpreter sees it. Not a public header.
#ifndef PINAX_INTERNAL_H
#define PINAX_INTERNAL_H

#include "pinax.h"
#include "pinax_code.h"

#include <stdbool.h>

#define PNX_MAX_HOSTS 64
#define PNX_MAX_HANDLE_TYPES 16
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
    uint32_t kind, elem_bytes, capacity;
    uint32_t length; // cells in use: the capacity for an array or table, what was pushed for a list
    uint32_t head;   // a queue: the cell of its oldest value; a pool: its first free slot plus one
    uint32_t key;    // an index: its key field, PNX_FIELD(table, offset, extra)
    bool     key_is_str;
} pnx_global;

struct pinax_call {
    pinax_state *state;
};

// What a load replaces, kept so that a failed load can put it back.
typedef struct pnx_program {
    const pnx_head      *head;
    const pnx_wordrec   *words;
    const pnx_importrec *importrecs;
    const int64_t       *iconst;
    const pnx_sconst    *sconst;
    const uint32_t      *code;
    pnx_counts          *word_counts;
    const pnx_host     **imports;
    pnx_global          *globals;
    const uint8_t       *seg_const, *seg_global;
    void                *symbols;
    size_t               used;
    uint32_t             n_hosts;
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
    uint64_t charge; // bytes scanned by string words since the last CHECK; 16 of them count as one instruction
    uint32_t owed; // cost of the CHECK a run was suspended at
    uint32_t pc, int_base, str_base, depth, depth_peak;
    uint32_t top_word;
    bool     suspended;
    bool     loading;   // the suspended run is the code outside definitions of a load
    pnx_program before_load; // what that load would be undone to
    void    *symbols;   // the compiler's declaration tables, kept for the next load (pinax_compile.c)
    pinax_err   fail;
    const char *error_text;
    void (*diag)(void *user, pinax_str file, uint32_t line, uint32_t column, pinax_str message);
    void *diag_user;
    pinax_resolve_fn resolve; // gives the C functions that `extern` names
    void            *resolve_user;
    uint8_t hash_key[16]; // of the lookup indexes; inside the block, so images carry it

    pnx_handle_type  handle_types[PNX_MAX_HANDLE_TYPES];
    pnx_handle_slot *handles;
    uint32_t         n_handle_types, handle_slots, handle_free, handles_open, handles_peak;
    uint32_t         run_serial; // counts runs; see pnx_handle_slot.run
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
// Hash of key bytes for an index: the vendored proven's keyed SipHash-2-4.
uint64_t pnx_hash(const uint8_t key[16], const uint8_t *bytes, uint32_t length);
// The name of an error number as a view into the interpreter's static text.
extern const char pnx_static_text[];
void pnx_err_name(int64_t err, uint32_t *offset, uint32_t *length);
// Offset of the last `part` in `bytes`, or UINT32_MAX. An empty part is found at the end.
uint32_t pnx_find_last(const uint8_t *bytes, uint32_t length, const uint8_t *part, uint32_t part_length);
// Offset of the first `byte`, or UINT32_MAX.
uint32_t pnx_find_byte(const uint8_t *bytes, uint32_t length, uint8_t byte);

// The loaded program as it is now, and putting an earlier one back (a failed load).
pnx_program pnx_program_now(const pinax_state *state);
void pnx_program_restore(pinax_state *state, const pnx_program *program);
// Loads a container in place of the present program. The first `keep` globals keep their
// storage and what it holds: they are the globals of the program being extended.
pinax_err pnx_load_container(pinax_state *state, pinax_str bytecode, uint32_t keep);
// Bytes from the block, zeroed; NULL when it is full (pinax_needed() then tells how much).
void *pnx_take(pinax_state *state, size_t bytes, size_t align);
// The number of the source module called `name` (of `length` bytes), or UINT32_MAX.
// The host word that stands for the C function `name` of library `module` ("@lib"), asking
// the resolver for it when it is not there yet.
pinax_err pnx_extern_link(pinax_state *state, const char *module, const char *name, const char *signature,
                          uint32_t *index);
uint32_t pnx_module_number(const pinax_state *state, const char *name, size_t length);

// Bytes in the main task's region; NULL when it is full.
uint8_t *pnx_region_alloc(pinax_state *state, size_t bytes);

#endif // PINAX_INTERNAL_H
