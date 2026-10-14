// pinax_internal.h - the state as the interpreter sees it. Not a public header.
#ifndef PINAX_INTERNAL_H
#define PINAX_INTERNAL_H

#include "pinax.h"
#include "pinax_code.h"

#include <stdbool.h>

#define PNX_MAX_HOSTS 64
// Register slots a task gets for every level of its call depth.
#define PNX_TASK_INTS 16
#define PNX_TASK_STRS 4
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

// What the last ptn.match of a run found, for ptn.capture: the whole match and the captures.
typedef struct pnx_ptn_found {
    pnx_scell cap[1 + PNX_PTN_CAPS];
    uint32_t  n; // 0: no match yet; else 1 + the captures
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
} pnx_global;

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
    const uint8_t       *lines, *localrecs;
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
    uint32_t fault_pc;             // where the last run ended with a fault: the code word after the instruction
    const uint8_t *lines;          // pnx_linerec records, head->n_lines of them (not aligned)
    char     where[112];           // "module:line: error" of the last fault, for pinax_error_text()
    uint32_t fault_ibase, fault_sbase; // the register windows of the word that faulted
    const uint8_t *localrecs;      // pnx_localrec records, head->n_locals of them (not aligned)
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
    pnx_program before_load; // what that load would be undone to
    void    *symbols;   // the compiler's declaration tables, kept for the next load (pinax_compile.c)
    pinax_err   fail;
    const char *error_text;
    void (*diag)(void *user, pinax_str file, uint32_t line, uint32_t column, pinax_str message);
    void *diag_user;
    pinax_resolve_fn resolve; // gives the C functions that `extern` names
    // what the last pinax_eval() left: its types (the compiler's numbers) and values
    uint32_t  eval_n;
    uint16_t  eval_type[16];
    int64_t   eval_int[16];
    pinax_str eval_str[16];
    void            *resolve_user;
    uint8_t hash_key[16]; // of the lookup indexes; inside the block, so images carry it

    pnx_handle_type  handle_types[PNX_MAX_HANDLE_TYPES];
    pnx_handle_slot *handles;
    uint32_t         n_handle_types, handle_slots, handle_free, handles_open, handles_peak;
    uint32_t         run_serial; // counts runs; see pnx_handle_slot.run
    pnx_ptn_found    ptn;        // the main run's last ptn.match
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
// The words of the modules hash and encode, over proven_c_lib. pnx_encode writes at most `cap`
// bytes to `out`: 0 done (`*written` set), 1 not enough room, 2 the input is not of the form.
uint64_t pnx_hash_kind(uint32_t kind, const uint8_t *bytes, uint32_t length);
int      pnx_encode(uint32_t kind, const uint8_t *in, uint32_t length, uint8_t *out, uint64_t cap, uint64_t *written);
// The name of an error number as a view into the interpreter's static text.

extern const char pnx_static_text[];
extern const uint32_t pnx_static_len; // bytes of pnx_static_text

// Images (D0036), from pinax_compile.c: the kept declaration tables at `symbols` were made at
// `from` and now lie `by` bytes further on; and the sizes of the compiler's kept structs,
// mixed into the layout hash of an image.
void     pnx_symbols_move(void *symbols, const void *from, ptrdiff_t by);
uint64_t pnx_symbols_layout(uint64_t hash);
void pnx_err_name(int64_t err, uint32_t *offset, uint32_t *length);
bool pnx_tasks_alive(const pinax_state *state); // a task was spawned and has not ended
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
// Ends every task and puts the main run back in place: after a fault inside a task.
void pnx_tasks_abort(pinax_state *state);
// The name of source module `number`; empty when it has none.
pinax_str pnx_module_name(const pinax_state *state, uint32_t number);
uint32_t pnx_module_number(const pinax_state *state, const char *name, size_t length);

// Bytes in the main task's region; NULL when it is full.
uint8_t *pnx_region_alloc(pinax_state *state, size_t bytes);

#endif // PINAX_INTERNAL_H
