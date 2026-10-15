// pinax_compile.c - the one-pass compiler (RFC-0013 S2): tokens in, a format 0 bytecode
// container out, which then goes through the same loader as hand-written bytecode (J1).
//
// How registers are chosen. The stack effect of every word is static, so each stack position
// has a fixed register: a word's window is its parameters and local variables first, then one
// register per stack position, separately for the I and the S bank. The number of locals is
// only known when the word ends, so code is first written with virtual numbers (a local is its
// number, stack position p is 128 + p) and rewritten once at the end of the word, driven by
// the operand kinds of the instruction table.
//
// Values on the compile-time stack are lazy where that is free: a literal and a read of a
// local are only remembered, and a comparison stays a pending condition so that `if`, `while`
// and `until` can turn it into one compare-and-branch. Everything is put into its register at
// the points where control flow meets.
//
// Working memory comes from the unused tail of the host's block and is given back when the
// load ends (RFC-0013 Q7).
#include "pinax_internal.h"
#include "pinax_lex.h"

// A type is a number. The built-in ones are fixed; declared ones (rows of a table, distinct
// types) are entries of the compiler's type table. The bank a value lives in is part of the
// number, so that it can be asked without the table.
typedef uint16_t tid;
#define TY_SBANK 0x8000u
// TY_STEM and TY_COLL are not values: a stem (lo hi range) and the name of an array or list
// exist only at compile time and are taken by the word that follows.
enum { TY_NONE, TY_INT, TY_BOOL, TY_ERR, TY_STEM, TY_COLL, TY_STR = TY_SBANK | 6, TY_MARK = 7,
       TY_BUF = 8, // a buffer handed to a host word: only in the signatures of host words
       TY_REGEX = TY_SBANK | 9, // a program of rex (RFC-0011): a view of its bytes
       TY_FIRST_DECLARED = 16 };
enum { BANK_I, BANK_S, BANK_NONE };
enum { E_SLOT, E_LOCAL, E_CONST, E_COND };
enum { B_IF, B_LOOP, B_ORELSE, B_CASE, B_OF, B_DO };
enum {
    STEM_RANGE, STEM_BYTES, STEM_TIMES, STEM_COLL, STEM_WORDS, STEM_ROWS, STEM_STRS,
    STEM_LINES, STEM_SPLIT, STEM_SPLIT_BYTE, // like STEM_WORDS, these take their next piece off the string
    STEM_CHARS, STEM_PTN, // ptn.matches: the string and its pattern, like STEM_SPLIT
    STEM_REX, STEM_RSPLIT, // rex.matches and rex.split: the string and its program
    STEM_CSV // csv.fields: like STEM_WORDS
};
enum { GK_SCALAR, GK_ARRAY, GK_LIST, GK_BUFFER, GK_INDEX, GK_QUEUE, GK_POOL, GK_TASKS };
enum { L_PLAIN, L_OWNS, L_MOVED }; // what a local holds of a value that must be given exactly once
#define FWD_MARK 0x80000000u // in a call: the number of a forward declaration, patched at the end
#define TY_DISTINCT 0x4000u  // declared by `type`: this plus its number
#define TY_BLOCK 0x2000u     // word (effect): this plus the number of the effect in comp.btypes
#define TY_HANDLE 0x1800u    // a handle type of the host: this plus its number
#define MAX_NESTED 4         // value blocks inside value blocks
enum { USE_EACH, USE_FOLD, USE_TIMES, USE_COUNT, USE_ANY, USE_ALL, USE_FIRST, USE_EACH_INDEX, USE_MAP, USE_FILTER,
       USE_WITH, USE_BY }; // the last two: rex.with_match and rex.replace_by

#define MAX_STACK 64
#define PNX_REX_BUF (16u + 4096u * 8u + 1024u * 32u) // the largest program
#define MAX_BLOCKS 32
#define MAX_DEFERS 16
#define MAX_LOCALS 96
#define MAX_PARAMS 16
#define MAX_MODULES 16
#define MAX_CASE 32
#define SLOT0 128u // virtual register of stack position 0
#define NONE UINT32_MAX

typedef struct name {
    const uint8_t *text;
    uint32_t       length;
} name;

typedef struct entry {
    tid     type;
    uint8_t kind;
    uint8_t reg;     // E_LOCAL: the local's register
    uint8_t op, a, b; // E_COND: branch instruction and its two fields (b: register or immediate)
                      // a stem: op is its kind
    bool    hidden;  // an operand of a running block: code cannot reach it
    bool    owned;   // a value that must be given on (RFC-0008 section 5.3), fresh on the stack
    uint8_t src;     // read from this local (its number plus one): lent, or given when it reaches a taker
    uint8_t born;    // a str: the marks made before it (fn.marks then); a mark: its own number
    uint16_t lit;    // pushed by a string literal or const: its constant number plus one
    int64_t value;   // E_CONST
} entry;

// The names written in a signature. They are wanted while the source that wrote them is
// compiled (to bind inputs, to print the signature); the tables kept for later loads drop them.
typedef struct signames {
    name in[MAX_PARAMS], out[MAX_PARAMS];
} signames;

typedef struct sig {
    uint8_t n_in, n_out;
    bool    fails, named;
    tid     in[MAX_PARAMS], out[MAX_PARAMS];
    bool    in_owned[MAX_PARAMS], out_owned[MAX_PARAMS];
    signames *names; // NULL: none written, or dropped
} sig;

static name sig_in_name(const sig *s, uint32_t k) {
    return s->names ? s->names->in[k] : (name){0};
}

typedef struct wordsym {
    name     name;
    sig      sig;
    uint32_t entry;
    uint16_t cells[2];
    uint16_t module;   // the source module it was declared in (set when the load is done)
    bool     exported; // visible to other modules as module.name
} wordsym;

typedef struct local {
    name    name;
    tid     type;
    uint8_t reg;
    uint8_t state; // L_PLAIN, L_OWNS, L_MOVED
    bool    stored; // `to` stored into it after it was made (a loop's element, see end_do)
    uint8_t born;   // as entry.born, for the value it holds
    bool    stale;  // a str made after a mark that was released since: it is not read
    bool    deferred; // its owned value is given by a defer when the block is left: lent only
    uint32_t drec;  // its debug record in fn.dlocals, or NONE
} local;

// defer ... end (RFC-0017 section 3.6): code run where its block is left. The body is read
// again at every place that leaves the block, from its first token.
typedef struct deferred {
    uint32_t at, line, line_start; // where the first token of the body starts
    uint32_t block; // the block it belongs to: n_blocks when it was written (0: the word's body)
} deferred;

// Debug information of a local: the code words of its body where its register holds it.
typedef struct dlocal {
    uint32_t from, to; // `to` is NONE while its scope is open
    uint8_t  reg;
    tid      type;
    name     name;
} dlocal;

// What must agree, or is merged, where paths meet: what a local owns, and when its value was
// made (RFC-0009 section 3.5: a str made after a mark is not used after its release).
typedef struct lstate {
    uint8_t state, born;
    bool    stale;
} lstate;

// The types of the stack at one moment; `hidden` marks the entries that belong to a running
// block and not to the code.
typedef struct shot {
    tid      types[MAX_STACK];
    uint8_t  born[MAX_STACK];
    uint64_t hidden, owned;
    uint8_t  depth;
} shot;

typedef struct block {
    uint8_t  kind;
    bool     has_else, has_out, targeted;
    name     name;       // loop name
    uint32_t head;       // loop: first instruction
    uint32_t exits;      // chain of jumps to the end
    uint32_t else_jump;  // if: the jump taken when the condition is false
    uint32_t locals_mark;
    uint8_t  nloc_mark[2];
    shot     in, out; // the stack when the block began; what a finished branch left
    lstate   owns_in[MAX_LOCALS], owns_out[MAX_LOCALS]; // and the state of the locals then
    uint32_t releases; // fn.releases when the block began
    // case
    uint32_t next_test;   // jump of the last test, taken when no value matched
    uint8_t  subject;
    tid      subject_type;
    bool     has_otherwise;
    uint32_t n_values;
    int64_t  values[MAX_CASE]; // integers, or string constant numbers
    // do block of a combining word
    uint8_t  use, stem, cur, lim, acc, n_hidden;
    uint32_t dst, full;  // map_into, filter_into: the list filled, and the jumps taken when it is full
    uint8_t  fill, room; // ... into an array: the cells written so far, and the array's length
    name     dst_name;
    uint8_t  val, idx;   // first: the register of the element found; each_index: the counter
    uint32_t elem_local; // the named local the element of a range goes to, or NONE
    tid      element;
    tid      acc_type;
    uint32_t floor_mark;
    name     user;       // the combining word that follows the block
} block;

typedef struct vec {
    void    *ptr;
    uint32_t n, cap;
} vec;

// One body being compiled: a word, or the code outside definitions.
typedef struct fn {
    entry    stack[MAX_STACK];
    uint32_t depth;
    block    blocks[MAX_BLOCKS];
    uint32_t n_blocks;
    local    locals[MAX_LOCALS];
    uint32_t n_locals;
    uint8_t  nloc[2], maxloc[2], maxslot[2];
    uint32_t floor; // stack entries below this belong to a running block
    uint8_t  marks; // the marks made so far, in the order of the source (entry.born)
    uint32_t releases; // the releases compiled so far
    bool     dead; // the code here cannot be reached
    sig      own_sig; // a value block keeps its effect here
    vec      code; // of uint32_t
    vec      lines; // of uint32_t: the source line of each code word
    vec      dlocals; // of dlocal
    uint32_t start, last_check;
    deferred defers[MAX_DEFERS];
    uint32_t n_defers;
    // The call just emitted, when what it gives is what this word gives: a return right after
    // it becomes a tail call, which reuses the window (RFC-0001 section 5.1).
    uint32_t tail_at, tail_word, tail_base[2], tail_in[2], tail_depth;
    uint32_t def_at; // instruction that just wrote def_reg, for retargeting a store
    uint8_t  def_reg, def_bank;
    bool     is_top;
    const sig *sig;
} fn;

typedef struct constant {
    name     name;
    tid      type;
    int64_t  value; // integer, or the string constant number
    uint16_t module;   // the source module it was declared in (set when the load is done)
    bool     exported; // visible to other modules as module.name
} constant;

typedef struct globalsym {
    name     name;
    tid      type;     // of the scalar, or of one cell; for a table of records, its row type
    uint8_t  kind;
    uint8_t  strcap;   // cells are str(N): N. Stored as rows of one length byte and N bytes
    bool     forward;  // announced by `forward`, not declared yet
    bool     bound;    // `bound`: the host gives the storage with pinax_bind()
    uint32_t capacity; // cells of an array or list
    uint32_t record;   // NONE, or the record a table holds
    uint32_t rows;     // rows written by `row` items so far
    uint32_t table, key; // an index: its table (a global) and its key field (in comp.fields)
    uint16_t module;   // the source module it was declared in (set when the load is done)
    bool     exported; // visible to other modules as module.name
} globalsym;

// A field of a record: where it lies and how it is read. `extra` is the width code, or the
// capacity N of a str(N).
typedef struct field {
    name     name;
    tid      type;
    uint16_t offset;
    uint8_t  extra;
    bool     is_str;
    bool     varint; // packed as a varint (RFC-0009 section 4.4); held as 64 bits
} field;

typedef struct record {
    name     name;
    uint32_t first, count; // its fields in comp.fields
    uint32_t size;
    uint16_t module;   // the source module it was declared in (set when the load is done)
    bool     exported; // visible to other modules as module.name
    bool     big_endian; // packed big-endian (only bytes.pack and unpack; memory stays as it is)
} record;

// extern NAME ( effect ) from "lib" : a C function the host's resolver gave
typedef struct externsym {
    name     name;
    uint32_t host; // its host word, in state->hosts
    uint16_t module;
    bool     exported;
} externsym;

// forward word NAME ( effect ) : a word called before it is defined
typedef struct fwd {
    name     name;
    sig      sig;
    uint32_t index; // its word number once defined, NONE before
    bool     exported;
} fwd;

// type NAME base : the same representation, another type
typedef struct dtype {
    name name;
    tid  base;
    uint16_t module;   // the source module it was declared in (set when the load is done)
    bool     exported; // visible to other modules as module.name
} dtype;

// The finished code of a value block, kept until the word around it has been stored: words lie
// in the container in the order of their numbers.
typedef struct pending_code {
    uint32_t  word, n;
    uint32_t *code, *lines;
} pending_code;

// A `truncate` of a list (RFC-0017 section 3.2): nothing may keep a row of that list.
typedef struct trunc_site {
    uint32_t global, line, column;
} trunc_site;

typedef struct comp {
    pinax_state *st;
    const char  *file;
    pnx_lexer    lexer, *lx;
    pnx_token    tok;
    uint8_t     *arena, *arena_end;
    fn          *f, *word_fn, *top_fn;
    vec          words;    // wordsym
    vec          imports;  // uint32_t: index into st->hosts
    vec          iconst;   // int64_t
    vec          sconst;   // pnx_sconst
    vec          strbytes; // uint8_t
    vec          consts;   // constant
    vec          globals;  // globalsym
    vec          records;  // record
    vec          fields;   // field
    vec          fwds;     // fwd
    vec          btypes;   // sig: the effects that are block types
    vec          pending;  // pending_code: value blocks compiled inside the body now being compiled
    fn          *block_fn[MAX_NESTED];
    uint32_t     nested;   // value blocks open
    uint32_t     def_index; // the word a definition just made
    // Several loads make one program (RFC-0014). What earlier loads declared lies below these
    // counts and belongs to other modules: a bare name finds only what lies above them.
    struct counts {
        uint32_t words, consts, globals, records, dtypes, externs;
    } base;
    vec          externs;   // externsym
    // pinax_eval(): every line belongs to one module, the session, so a line sees what the
    // lines before it declared and the modules they opened
    bool         eval;
    bool         unfinished; // the text ended inside a definition or a block: more is to come
    bool         see;        // list the stack after every line of the definitions that follow
    uint32_t     see_line;
    sig          eval_sig;   // what the line leaves on the stack: the results of its code
    // redefine word NAME ... : the session's word that is replaced and the new word whose code
    // takes its place (RFC-0006 S11); NONE when the text redefines nothing
    uint32_t     redefine_old, redefine_new;
    bool         redefining; // `redefine` stands before the definition being read
    uint32_t    *entries;    // after a redefinition: where every word's code begins now
    uint16_t     redefine_cells[2]; // and the registers of the new code, which the old word gets
    bool         in_extern; // a signature of an extern is being read: cstr is a type there
    bool         in_host;   // a signature of a host word is being read: buffer is a type there
    bool         drop_names; // the names in the signature being read are not wanted
    uint32_t     cstr_mask; // and these of its inputs were written cstr
    uint16_t     module;    // the number of the module being compiled
    vec          srcmods;   // name: the modules loaded so far, this one last
    bool         exporting; // `export` stands before the item being compiled
    uint32_t     path_global; // to module.name : the global the path names, for store_local
    vec          truncs;    // trunc_site: lists cut back by truncate, checked when the source is read
    vec          dtypes;   // dtype
    vec          localrecs; // pnx_localrec: the locals of the words of this load
    uint8_t     *rx_buf;    // where rx compiles a constant pattern, PNX_REX_BUF bytes
    bool         data_only; // PINAX_LOAD_DATA: declarations, literals, stores and rows only
    bool         sink;      // the word being compiled may take an owned value off the stack
    bool         in_defer;  // the body of a defer is being read
    name         modules[MAX_MODULES]; // the names the source writes: `use rex as rx` opens rx
    name         module_real[MAX_MODULES]; // and the modules they stand for
    uint32_t     n_modules;
    pinax_err    err;
    const char  *message;
    uint32_t     line, column;
} comp;

// ---- errors and memory ------------------------------------------------------------------

static bool error_at(comp *c, pinax_err err, uint32_t line, uint32_t column, const char *message) {
    if (c->err == PINAX_OK) {
        if (err == PINAX_ERR_SYNTAX && c->tok.kind == PNX_T_END)
            c->unfinished = true; // the text ran out; pinax_eval() says so
        c->err = err;
        c->message = message;
        c->line = line;
        c->column = column;
    }
    return false;
}

static bool error(comp *c, pinax_err err, const char *message) {
    return error_at(c, err, c->tok.line, c->tok.column, message);
}

static bool syntax(comp *c, const char *message) {
    return error(c, PINAX_ERR_SYNTAX, message);
}

static bool type_error(comp *c, const char *message) {
    return error(c, PINAX_ERR_TYPE, message);
}

static void *arena_alloc(comp *c, size_t bytes) {
    bytes = (bytes + 7u) & ~(size_t)7u;
    if (bytes > (size_t)(c->arena_end - c->arena)) {
        c->st->needed = c->st->block_len + bytes; // a lower bound of what would have been enough
        error(c, PINAX_ERR_NO_ROOM, "the block is too small to compile this source");
        return NULL;
    }
    void *p = c->arena;
    c->arena += bytes;
    __builtin_memset(p, 0, bytes);
    return p;
}

// Room for one more element; the old storage is left behind in the arena.
static void *vec_push(comp *c, vec *v, size_t elem) {
    if (v->n == v->cap) {
        const uint32_t cap = v->cap ? v->cap * 2 : 32;
        void *p = arena_alloc(c, (size_t)cap * elem);
        if (!p)
            return NULL;
        if (v->n)
            __builtin_memcpy(p, v->ptr, (size_t)v->n * elem);
        v->ptr = p;
        v->cap = cap;
    }
    return (uint8_t *)v->ptr + (size_t)v->n++ * elem;
}

// ---- tokens -------------------------------------------------------------------------------

static bool advance(comp *c) {
    if (!pnx_lex_next(c->lx, &c->tok))
        return error_at(c, PINAX_ERR_SYNTAX, c->lx->error_line, c->lx->error_column, c->lx->error);
    return true;
}

static bool is_word(const comp *c, pnx_word_id word) {
    return c->tok.kind == PNX_T_RESERVED && c->tok.word == word;
}

static name tok_name(const comp *c) {
    return (name){c->lx->source + c->tok.start, c->tok.length};
}

// A path that names `word` of some module: "str.join_into", or the same under another name.
static bool ends_with_word(name n, const char *word) {
    const size_t len = __builtin_strlen(word);
    return n.length > len && n.text[n.length - len - 1] == '.' && __builtin_memcmp(n.text + n.length - len, word, len) == 0;
}

static bool name_eq(name a, name b) {
    return a.length == b.length && __builtin_memcmp(a.text, b.text, a.length) == 0;
}

static bool name_is(name a, const char *text) {
    const size_t n = __builtin_strlen(text);
    return a.length == n && __builtin_memcmp(a.text, text, n) == 0;
}

static name cname(const char *text) {
    return (name){(const uint8_t *)text, (uint32_t)__builtin_strlen(text)};
}

// ---- types and signatures ---------------------------------------------------------------

static int bank_of(tid type) {
    return type == TY_STEM || type == TY_COLL ? BANK_NONE : (type & TY_SBANK) ? BANK_S : BANK_I;
}

// The row type of the table that is global `index`.
static tid row_type(uint32_t index) {
    return (tid)(TY_FIRST_DECLARED + index);
}

static bool is_row(tid type) {
    return type >= TY_FIRST_DECLARED && type < TY_HANDLE;
}

static bool is_handle(tid type) {
    return type >= TY_HANDLE && type < TY_BLOCK;
}

static bool is_block(tid type) {
    return type >= TY_BLOCK && type < TY_DISTINCT;
}

// The type a distinct type is made of; any other type is its own base.
static tid base_of(const comp *c, tid type) {
    if ((type & ~TY_SBANK) >= TY_DISTINCT && (type & ~TY_SBANK) - TY_DISTINCT < c->dtypes.n)
        return ((const dtype *)c->dtypes.ptr + ((type & ~TY_SBANK) - TY_DISTINCT))->base;
    return type;
}

static uint32_t find_global(const comp *c, name n);

static name type_name(const comp *c, tid type) {
    if (is_row(type))
        return ((const globalsym *)c->globals.ptr + (type - TY_FIRST_DECLARED))->name;
    if (is_handle(type))
        return cname(c->st->handle_types[type - TY_HANDLE].name);
    if (base_of(c, type) != type)
        return ((const dtype *)c->dtypes.ptr + ((type & ~TY_SBANK) - TY_DISTINCT))->name;
    return cname(type == TY_INT ? "int" : type == TY_BOOL ? "bool" : type == TY_ERR ? "err" : type == TY_MARK ? "mark"
                 : type == TY_BUF ? "buffer" : type == TY_REGEX ? "regex" : "str");
}

static tid basic_type(name n) {
    if (name_is(n, "int"))
        return TY_INT;
    if (name_is(n, "bool"))
        return TY_BOOL;
    if (name_is(n, "str"))
        return TY_STR;
    if (name_is(n, "err"))
        return TY_ERR;
    if (name_is(n, "mark"))
        return TY_MARK;
    return TY_NONE;
}

// A type as written in a signature or a declaration: a basic type, or the name of a table of
// records, which is the type of its rows.
static bool source_used(const comp *c, uint32_t m);
static uint32_t find_extern(const comp *c, name n);

static bool module_used(const comp *c, name module);

static tid type_of_name(const comp *c, name n) {
    const tid basic = basic_type(n);
    if (basic != TY_NONE)
        return basic;
    if (name_is(n, "regex") && module_used(c, cname("rex"))) // the type of the module rex, seen where it is used
        return TY_REGEX;
    for (uint32_t k = c->eval ? 0 : c->base.dtypes; k < c->dtypes.n; ++k) {
        const dtype *d = (const dtype *)c->dtypes.ptr + k;
        if ((k >= c->base.dtypes || d->module == c->module) && name_eq(d->name, n))
            return (tid)((TY_DISTINCT + k) | (d->base & TY_SBANK));
    }
    for (uint32_t k = 0; k < c->st->n_handle_types; ++k) { // a type the host gave
        if (name_is(n, c->st->handle_types[k].name))
            return (tid)(TY_HANDLE + k);
    }
    const uint32_t g = find_global(c, n);
    if (g != NONE) {
        const globalsym *sym = (const globalsym *)c->globals.ptr + g;
        if (sym->kind != GK_SCALAR && (sym->forward || sym->record != NONE))
            return row_type(g);
        if (sym->kind == GK_TASKS) // a task of that set
            return row_type(g);
    }
    // a type or a table of records that a module in use exports
    for (uint32_t k = 0; k < c->base.dtypes; ++k) {
        const dtype *d = (const dtype *)c->dtypes.ptr + k;
        if (d->exported && name_eq(d->name, n) && source_used(c, d->module))
            return (tid)((TY_DISTINCT + k) | (d->base & TY_SBANK));
    }
    for (uint32_t k = 0; k < c->base.globals; ++k) {
        const globalsym *sym = (const globalsym *)c->globals.ptr + k;
        if (sym->exported && sym->kind != GK_SCALAR && sym->record != NONE && name_eq(sym->name, n) &&
            source_used(c, sym->module))
            return row_type(k);
    }
    return TY_NONE;
}

// The type of blocks with effect `s`: one number per effect (names of parameters do not count).
static bool block_type(comp *c, const sig *s, tid *out) {
    const sig *all = c->btypes.ptr;
    for (uint32_t k = 0; k < c->btypes.n; ++k) {
        bool same = all[k].n_in == s->n_in && all[k].n_out == s->n_out && all[k].fails == s->fails;
        for (uint32_t j = 0; same && j < s->n_in; ++j)
            same = all[k].in[j] == s->in[j] && all[k].in_owned[j] == s->in_owned[j];
        for (uint32_t j = 0; same && j < s->n_out; ++j)
            same = all[k].out[j] == s->out[j] && all[k].out_owned[j] == s->out_owned[j];
        if (same) {
            *out = (tid)(TY_BLOCK + k);
            return true;
        }
    }
    if (c->btypes.n >= TY_DISTINCT - TY_BLOCK)
        return syntax(c, "too many different block types");
    sig *slot = vec_push(c, &c->btypes, sizeof(sig));
    if (!slot)
        return false;
    *slot = *s;
    slot->names = NULL;
    *out = (tid)(TY_BLOCK + c->btypes.n - 1);
    return true;
}

// "(" effect ")" ; the current token is the "(".
static bool parse_sig(comp *c, sig *s) {
    __builtin_memset(s, 0, sizeof *s);
    if (c->tok.kind != PNX_T_LPAREN)
        return syntax(c, "a signature is expected here: ( inputs gives results )");
    if (!advance(c))
        return false;
    bool outputs = false, any_named = false, any_bare = false;
    for (;;) {
        if (c->tok.kind == PNX_T_RPAREN)
            break;
        if (is_word(c, PNX_KW_gives)) {
            if (outputs)
                return syntax(c, "gives is written once in a signature");
            outputs = true;
            if (!advance(c))
                return false;
            continue;
        }
        if (is_word(c, PNX_KW_or)) {
            if (!advance(c))
                return false;
            if (!is_word(c, PNX_KW_fail))
                return syntax(c, "or is followed by fail in a signature");
            if (!advance(c))
                return false;
            if (c->tok.kind != PNX_T_RPAREN)
                return syntax(c, "or fail ends the signature");
            s->fails = true;
            break;
        }
        // [ name ] [ owned ] type ; a type is a name or word ( effect )
        bool owned = false;
        name param = {0}, type = {0};
        tid  t = TY_NONE;
        if (c->tok.kind == PNX_T_NAME) {
            type = tok_name(c);
            if (!advance(c))
                return false;
            if (c->tok.kind == PNX_T_NAME || is_word(c, PNX_KW_owned) || is_word(c, PNX_KW_word) ||
                (c->in_host && is_word(c, PNX_KW_buffer))) {
                param = type; // it was the name of the parameter
                type = (name){0};
            }
        }
        if (!type.length) {
            if (is_word(c, PNX_KW_owned)) {
                owned = true;
                if (!advance(c))
                    return false;
            }
            if (is_word(c, PNX_KW_word)) { // a block: word ( effect )
                sig inner;
                if (!advance(c) || !parse_sig(c, &inner) || !block_type(c, &inner, &t))
                    return false;
            } else if (c->tok.kind == PNX_T_NAME) {
                type = tok_name(c);
                if (!advance(c))
                    return false;
            } else if (c->in_host && !outputs && !owned && is_word(c, PNX_KW_buffer)) {
                t = TY_BUF;
                if (!advance(c))
                    return false;
            } else {
                return syntax(c, "a parameter is a type, or a name and a type");
            }
        }
        if (type.length)
            t = type_of_name(c, type);
        if (t == TY_NONE && c->in_extern && !outputs && !owned && name_is(type, "cstr")) {
            t = TY_STR; // handed to the C function as a NUL-terminated copy
            c->cstr_mask |= 1u << s->n_in;
        }
        if (t == TY_NONE)
            return type_error(c, name_is(type, "float") ? "float is not available: this build has no F bank"
                                                        : "this type is not known here");
        uint8_t *count = outputs ? &s->n_out : &s->n_in;
        if (*count == MAX_PARAMS)
            return syntax(c, "too many parameters");
        if (param.length && !s->names && !c->drop_names) {
            s->names = arena_alloc(c, sizeof(signames));
            if (!s->names)
                return false;
        }
        if (outputs) {
            s->out[*count] = t;
            s->out_owned[*count] = owned;
            if (s->names)
                s->names->out[*count] = param;
        } else {
            s->in[*count] = t;
            s->in_owned[*count] = owned;
            if (s->names)
                s->names->in[*count] = param;
            if (param.length)
                any_named = true;
            else
                any_bare = true;
        }
        ++*count;
        if (c->tok.kind == PNX_T_COMMA) {
            if (!advance(c))
                return false;
            if (c->tok.kind != PNX_T_NAME && !is_word(c, PNX_KW_owned) && !is_word(c, PNX_KW_word))
                return syntax(c, "a parameter is expected after the comma");
        } else if (c->tok.kind != PNX_T_RPAREN && !is_word(c, PNX_KW_gives) && !is_word(c, PNX_KW_or)) {
            return syntax(c, "parameters are separated by commas");
        }
    }
    if (any_named && any_bare)
        return type_error(c, "inputs are all named or all bare types, not a mix");
    s->named = any_named;
    return advance(c); // past the ")"
}

// The signature as the host writes it: "(last int gives total int)".
static bool sig_text(const comp *c, const sig *s, char *out, size_t cap) {
    size_t n = 0;
#define PUT(text, len)                                                                             \
    do {                                                                                           \
        if (n + (len) >= cap)                                                                      \
            return false;                                                                          \
        __builtin_memcpy(out + n, (text), (len));                                                  \
        n += (len);                                                                                \
    } while (0)
    PUT("(", 1);
    for (uint32_t side = 0; side < 2; ++side) {
        const uint8_t count = side ? s->n_out : s->n_in;
        if (side && count) {
            if (s->n_in)
                PUT(" ", 1);
            PUT("gives ", 6);
        }
        for (uint32_t k = 0; k < count; ++k) {
            const name pn = !s->names ? (name){0} : side ? s->names->out[k] : s->names->in[k];
            const tid kt = side ? s->out[k] : s->in[k];
            if (is_block(kt)) { // word (effect), written out
                char inner[72];
                if (!sig_text(c, (const sig *)c->btypes.ptr + (kt - TY_BLOCK), inner, sizeof inner))
                    return false;
                if (k)
                    PUT(", ", 2);
                if (pn.length) {
                    PUT(pn.text, pn.length);
                    PUT(" ", 1);
                }
                if (side ? s->out_owned[k] : s->in_owned[k])
                    PUT("owned ", 6);
                PUT("word ", 5);
                PUT(inner, __builtin_strlen(inner));
                continue;
            }
            const name tn = type_name(c, kt);
            if (k)
                PUT(", ", 2);
            if (pn.length) {
                PUT(pn.text, pn.length);
                PUT(" ", 1);
            }
            if (side ? s->out_owned[k] : s->in_owned[k])
                PUT("owned ", 6);
            if (!side && c->in_extern && (c->cstr_mask >> k & 1u))
                PUT("c", 1);
            PUT(tn.text, tn.length);
        }
    }
    if (s->fails) {
        if (s->n_in || s->n_out)
            PUT(" ", 1);
        PUT("or fail", 7);
    }
    PUT(")", 1);
#undef PUT
    out[n] = '\0';
    return true;
}

// The signature of a registered host word, read with the same parser.
static bool host_sig(comp *c, const pnx_host *h, sig *s) {
    pnx_lexer temp, *saved_lx = c->lx;
    const pnx_token saved_tok = c->tok;
    pnx_lex_init(&temp, (const uint8_t *)h->signature, __builtin_strlen(h->signature));
    c->lx = &temp;
    c->in_extern = h->module[0] == '@';
    c->in_host = !c->in_extern;
    c->drop_names = true; // a call needs the types only
    const bool ok = advance(c) && parse_sig(c, s);
    c->in_extern = c->in_host = c->drop_names = false;
    c->lx = saved_lx;
    c->tok = saved_tok;
    if (!ok) { // registration accepted it, so this is a type the compiler does not have yet
        c->err = PINAX_OK;
        return type_error(c, "this host word uses a type the compiler does not support yet");
    }
    return true;
}

// ---- code ---------------------------------------------------------------------------------

static uint32_t *code_at(fn *f, uint32_t index) {
    return (uint32_t *)f->code.ptr + index;
}

static uint32_t emit(comp *c, uint32_t word) {
    fn *f = c->f;
    uint32_t *slot = vec_push(c, &f->code, sizeof(uint32_t));
    uint32_t *line = slot ? vec_push(c, &f->lines, sizeof(uint32_t)) : NULL;
    if (!line)
        return NONE;
    *slot = word;
    *line = c->tok.line;
    f->def_at = NONE;
    return f->code.n - 1;
}

// An instruction whose A field is the register it writes and nothing else.
static uint32_t emit_def(comp *c, uint32_t word, int bank) {
    const uint32_t at = emit(c, word);
    if (at != NONE) {
        c->f->def_at = at;
        c->f->def_reg = (uint8_t)(word >> 8);
        c->f->def_bank = (uint8_t)bank;
    }
    return at;
}

// An instruction that names a global in C (AGET, ROW, BPUT, IXGET, TSPAWN, ...). A global
// numbered above 255 does not fit there: the wide form carries it in a second word.
static uint32_t wide_of(uint32_t op) {
    switch (op) {
    case PNX_AGET: return PNX_AGETW;
    case PNX_ASET: return PNX_ASETW;
    case PNX_ROW: return PNX_ROWW;
    case PNX_PALIVE: return PNX_PALIVEW;
    case PNX_IXGET: return PNX_IXGETW;
    case PNX_IXGETS: return PNX_IXGETSW;
    case PNX_BPUT: return PNX_BPUTW;
    case PNX_BENC: return PNX_BENCW;
    case PNX_JPUTI: return PNX_JPUTIW;
    case PNX_JPUTS: return PNX_JPUTSW;
    case PNX_CPUTS: return PNX_CPUTSW;
    case PNX_CPUTI: return PNX_CPUTIW;
    case PNX_BPUTS: return PNX_BPUTSW;
    case PNX_TSPAWN: return PNX_TSPAWNW;
    case PNX_TRESUME: return PNX_TRESUMEW;
    default: return PNX_TDONEW;
    }
}
static uint32_t emit_g(comp *c, uint32_t op, uint32_t a, uint32_t b, uint32_t global) {
    if (global <= 255)
        return emit(c, PNX_ABC(op, a, b, global));
    const uint32_t at = emit(c, PNX_ABC(wide_of(op), a, b, 0));
    return at == NONE || emit(c, global) == NONE ? NONE : at;
}
// The same for an instruction whose result a store may retarget (emit_def); the wide form is
// not retargeted.
static uint32_t emit_g_def(comp *c, uint32_t op, uint32_t a, uint32_t b, uint32_t global, int bank) {
    return global <= 255 ? emit_def(c, PNX_ABC(op, a, b, global), bank) : emit_g(c, op, a, b, global);
}

// A forward jump whose target is not known yet. `chain` links the jumps to the same place.
static bool jump_later(comp *c, uint32_t *chain) {
    const uint32_t at = emit(c, PNX_AX(PNX_JMP, *chain == NONE ? 0xffffffu : *chain));
    if (at == NONE)
        return false;
    if (at >= 0xffffffu)
        return syntax(c, "the word is too long");
    *chain = at;
    return true;
}

static void land(comp *c, uint32_t *chain) {
    fn *f = c->f;
    for (uint32_t at = *chain; at != NONE;) {
        const uint32_t next = *code_at(f, at) >> 8;
        *code_at(f, at) = PNX_AX(PNX_JMP, f->code.n - (at + 1));
        at = next == 0xffffffu ? NONE : next;
    }
    *chain = NONE;
    f->def_at = NONE;
    f->tail_at = NONE; // another path arrives here
}

static uint32_t add_iconst(comp *c, int64_t value) {
    int64_t *all = c->iconst.ptr;
    for (uint32_t k = 0; k < c->iconst.n; ++k) {
        if (all[k] == value)
            return k;
    }
    int64_t *slot = vec_push(c, &c->iconst, sizeof(int64_t));
    if (!slot)
        return NONE;
    *slot = value;
    return c->iconst.n - 1;
}

static bool load_int(comp *c, uint32_t reg, int64_t value) {
    if (value >= INT16_MIN && value <= INT16_MAX)
        return emit_def(c, PNX_ABX(PNX_LOADI, reg, (uint16_t)(int16_t)value), BANK_I) != NONE;
    const uint32_t k = add_iconst(c, value);
    if (k == NONE)
        return false;
    if (k > 0xffffu)
        return syntax(c, "too many integer constants");
    return emit_def(c, PNX_ABX(PNX_LOADK, reg, k), BANK_I) != NONE;
}

// ---- the compile-time stack ---------------------------------------------------------------

// Virtual register of stack entry `index`: its position among the entries of the same bank.
static uint32_t slot_of(comp *c, uint32_t index) {
    fn *f = c->f;
    const int bank = bank_of(f->stack[index].type);
    uint32_t pos = 0;
    for (uint32_t k = 0; k < index; ++k) {
        if (bank_of(f->stack[k].type) == bank)
            ++pos;
    }
    if (pos + 1 > f->maxslot[bank])
        f->maxslot[bank] = (uint8_t)(pos + 1);
    return SLOT0 + pos;
}

// Virtual register the next pushed value of this bank would get.
static uint32_t top_slot(comp *c, int bank, uint32_t extra) {
    fn *f = c->f;
    uint32_t pos = 0;
    for (uint32_t k = 0; k < f->depth; ++k) {
        if (bank_of(f->stack[k].type) == bank)
            ++pos;
    }
    if (pos + extra > f->maxslot[bank])
        f->maxslot[bank] = (uint8_t)(pos + extra);
    return SLOT0 + pos;
}

// Opens the debug record of a named local: from code word `from` on, its register holds it.
static bool debug_local(comp *c, local *l, uint32_t from) {
    l->drec = NONE;
    if (!l->name.length)
        return true;
    dlocal *d = vec_push(c, &c->f->dlocals, sizeof(dlocal));
    if (!d)
        return false;
    *d = (dlocal){.from = from, .to = NONE, .reg = l->reg, .type = l->type, .name = l->name};
    l->drec = c->f->dlocals.n - 1;
    return true;
}

// A value pushed is new: made after every mark so far. Copies of a value keep its own `born`.
static bool push(comp *c, entry e) {
    fn *f = c->f;
    if (f->depth == MAX_STACK)
        return syntax(c, "the stack is deeper than 64 values here");
    e.born = f->marks;
    f->stack[f->depth++] = e;
    return true;
}

static bool push_slot(comp *c, tid type) {
    if (!push(c, (entry){.type = type, .kind = E_SLOT}))
        return false;
    (void)slot_of(c, c->f->depth - 1); // counts the register
    return true;
}

static bool cond_jump(comp *c, const entry *e, uint32_t skip) {
    return emit(c, PNX_ABC(e->op, e->a, e->b, skip)) != NONE;
}

// Puts the value of a pending condition into `reg` as 0 or 1.
static bool cond_value(comp *c, const entry *e, uint32_t reg) {
    return cond_jump(c, e, 2) && emit(c, PNX_ABX(PNX_LOADI, reg, 0)) != NONE &&
           emit(c, PNX_AX(PNX_JMP, 1)) != NONE && emit(c, PNX_ABX(PNX_LOADI, reg, 1)) != NONE;
}

// Makes entry `index` live in its own register.
static bool settle(comp *c, uint32_t index) {
    entry *e = &c->f->stack[index];
    if (e->kind == E_SLOT || bank_of(e->type) == BANK_NONE)
        return true;
    const uint32_t reg = slot_of(c, index);
    bool ok;
    switch (e->kind) {
    case E_LOCAL:
        ok = emit_def(c, PNX_ABC(bank_of(e->type) == BANK_S ? PNX_MOVS : PNX_MOVI, reg, e->reg, 0), bank_of(e->type)) != NONE;
        break;
    case E_CONST: ok = load_int(c, reg, e->value); break;
    default: ok = cond_value(c, e, reg); break;
    }
    e->kind = E_SLOT;
    return ok;
}

static bool settle_all(comp *c) {
    for (uint32_t k = 0; k < c->f->depth; ++k) {
        if (c->f->stack[k].type == TY_COLL)
            return type_error(c, "the name of an array or list is used at once by a word (idx, len, each ...)");
        if (!settle(c, k))
            return false;
    }
    return true;
}

// Register to read entry `index` from. Literals and conditions are put into their register
// first; a local is read where it lives.
static bool reg_of(comp *c, uint32_t index, uint32_t *reg) {
    entry *e = &c->f->stack[index];
    if (e->kind == E_LOCAL) {
        *reg = e->reg;
        return true;
    }
    if (!settle(c, index))
        return false;
    *reg = slot_of(c, index);
    return true;
}

static bool need(comp *c, uint32_t count) {
    const bool sink = c->sink;
    c->sink = false;
    if (c->f->depth - c->f->floor < count)
        return type_error(c, "the stack does not hold enough values for this word");
    for (uint32_t k = 0; k < count; ++k) {
        const tid type = c->f->stack[c->f->depth - 1 - k].type;
        if (c->f->stack[c->f->depth - 1 - k].owned && !sink)
            return type_error(c, "an owned value is kept in a local or given on at once; this word would lose it");
        if (type == TY_STEM)
            return type_error(c, "a stem is not a value: a combining word takes it at once");
        if (type == TY_COLL)
            return type_error(c, "the name of an array or list is not a value; a word like idx or len takes it");
    }
    return true;
}

// Entry `index` is given to something that takes over an owned value. True when it carried
// one: a fresh owned value, or the value of a local that held one, which is then spent.
static bool give(comp *c, uint32_t index) {
    fn *f = c->f;
    entry *e = &f->stack[index];
    if (e->owned) {
        e->owned = false;
        return true;
    }
    if (e->src && f->locals[e->src - 1].state == L_OWNS) {
        if (f->locals[e->src - 1].deferred && !c->in_defer)
            return type_error(c, "a defer gives this owned value when the block is left; here it can only be lent");
        f->locals[e->src - 1].state = L_MOVED;
        return true;
    }
    return false;
}

// Where a word is left, nothing owned may stay behind.
static bool nothing_owned(comp *c) {
    fn *f = c->f;
    for (uint32_t k = 0; k < f->n_locals; ++k) {
        if (f->locals[k].state == L_OWNS)
            return type_error(c, "an owned value in a local is not given on every path (close, free or store it)");
    }
    for (uint32_t k = 0; k < f->depth; ++k) {
        if (f->stack[k].owned)
            return type_error(c, "an owned value on the stack is not given on every path");
    }
    return true;
}

// The array or list named on top of the stack; pops the name.
static bool need_coll(comp *c, const globalsym **out, uint32_t *index) {
    fn *f = c->f;
    if (f->depth == f->floor || f->stack[f->depth - 1].type != TY_COLL)
        return type_error(c, "this word takes the name of an array or list last: i names idx");
    *index = (uint32_t)f->stack[f->depth - 1].value;
    *out = (const globalsym *)c->globals.ptr + *index;
    --f->depth;
    return true;
}

// ---- conditions ---------------------------------------------------------------------------

static void cond_negate(entry *e) {
    uint8_t t;
    switch (e->op) {
    case PNX_JEQ: e->op = PNX_JNE; break;
    case PNX_JNE: e->op = PNX_JEQ; break;
    case PNX_JLT: e->op = PNX_JLE; t = e->a; e->a = e->b; e->b = t; break; // not a<b  is  b<=a
    case PNX_JLE: e->op = PNX_JLT; t = e->a; e->a = e->b; e->b = t; break;
    case PNX_JEQI: e->op = PNX_JNEI; break;
    case PNX_JNEI: e->op = PNX_JEQI; break;
    case PNX_JLTI: e->op = PNX_JGEI; break;
    case PNX_JSEQ: e->op = PNX_JSNE; break;
    case PNX_JSNE: e->op = PNX_JSEQ; break;
    default: e->op = PNX_JLTI; break;
    }
}

// Pops the top value, which must be a bool, as a condition that is true when the value is.
static bool pop_cond(comp *c, entry *out) {
    fn *f = c->f;
    if (!need(c, 1))
        return false;
    entry *e = &f->stack[f->depth - 1];
    if (e->type != TY_BOOL)
        return type_error(c, "a bool is expected here");
    if (e->kind == E_COND) {
        *out = *e;
    } else {
        uint32_t reg;
        if (!reg_of(c, f->depth - 1, &reg))
            return false;
        *out = (entry){.type = TY_BOOL, .kind = E_COND, .op = PNX_JNEI, .a = (uint8_t)reg, .b = 0};
    }
    --f->depth;
    return true;
}

// ---- names --------------------------------------------------------------------------------

static local *find_local(fn *f, name n) {
    for (uint32_t k = f->n_locals; k-- > 0;) {
        if (f->locals[k].name.length && name_eq(f->locals[k].name, n))
            return &f->locals[k];
    }
    return NULL;
}

static wordsym *find_word(comp *c, name n, uint32_t *index) {
    wordsym *all = c->words.ptr;
    for (uint32_t k = c->eval ? 0 : c->base.words; k < c->words.n; ++k) {
        if ((k >= c->base.words || all[k].module == c->module) && name_eq(all[k].name, n)) {
            if (index)
                *index = k;
            return &all[k];
        }
    }
    return NULL;
}

static fwd *find_fwd(const comp *c, name n, uint32_t *index) {
    fwd *all = c->fwds.ptr;
    for (uint32_t k = 0; k < c->fwds.n; ++k) {
        if (name_eq(all[k].name, n)) {
            if (index)
                *index = k;
            return &all[k];
        }
    }
    return NULL;
}

// The module a path prefix stands for: `use io` opens io, `use rex as rx` opens rx for rex.
// False when no `use` opened that prefix.
static bool module_of(const comp *c, name prefix, name *real) {
    for (uint32_t k = 0; k < c->n_modules; ++k) {
        if (name_eq(c->modules[k], prefix)) {
            *real = c->module_real[k];
            return true;
        }
    }
    return false;
}

// Whether a `use` opened module `module` under any name.
static bool module_used(const comp *c, name module) {
    for (uint32_t k = 0; k < c->n_modules; ++k) {
        if (name_eq(c->module_real[k], module))
            return true;
    }
    return false;
}

// The number of the loaded source module called `n`, or NONE. The module being compiled is
// not among them: it cannot use itself.
static uint32_t source_module(const comp *c, name n) {
    const name *all = c->srcmods.ptr;
    for (uint32_t k = 0; k < c->srcmods.n; ++k) {
        if (k != c->module && all[k].length && name_eq(all[k], n))
            return k;
    }
    return NONE;
}

// Whether this source opened source module number `m` with `use`.
static bool source_used(const comp *c, uint32_t m) {
    return m != c->module && m < c->srcmods.n && ((const name *)c->srcmods.ptr)[m].length &&
           module_used(c, ((const name *)c->srcmods.ptr)[m]);
}

// The host word `word` of module `module`: its index into state->hosts, or NONE.
static uint32_t find_host(const comp *c, name module, name word) {
    for (uint32_t k = 0; k < c->st->n_hosts; ++k) {
        const pnx_host *h = &c->st->hosts[k];
        if (name_eq(cname(h->name), word) && name_eq(cname(h->module), module))
            return k;
    }
    return NONE;
}

typedef struct core_word {
    const char *name;
    uint8_t     kind, op;
} core_word;

enum { C_STACK, C_ARITH, C_COMPARE, C_BOOL, C_NOT, C_NEG, C_UNARY, C_UCMP, C_CHK, C_CLAMP, C_SELECT };
enum { ST_DUP, ST_DROP, ST_SWAP, ST_OVER, ST_NIP };
enum { CMP_EQ, CMP_NE, CMP_LT, CMP_LE, CMP_GT, CMP_GE };

static const core_word core_words[] = {
    {"dup", C_STACK, ST_DUP},     {"drop", C_STACK, ST_DROP},   {"swap", C_STACK, ST_SWAP},
    {"over", C_STACK, ST_OVER},   {"nip", C_STACK, ST_NIP},
    {"add", C_ARITH, PNX_ADD},    {"sub", C_ARITH, PNX_SUB},    {"mul", C_ARITH, PNX_MUL},
    {"div", C_ARITH, PNX_DIV},    {"mod", C_ARITH, PNX_MOD},    {"neg", C_NEG, PNX_NEG},
    {"wrap_add", C_ARITH, PNX_WADD}, {"wrap_sub", C_ARITH, PNX_WSUB}, {"wrap_mul", C_ARITH, PNX_WMUL},
    {"bit_and", C_ARITH, PNX_AND}, {"bit_or", C_ARITH, PNX_OR}, {"bit_xor", C_ARITH, PNX_XOR},
    {"shl", C_ARITH, PNX_SHL},    {"shr", C_ARITH, PNX_SHR},    {"u_shr", C_ARITH, PNX_USHR},
    {"eq", C_COMPARE, CMP_EQ},    {"ne", C_COMPARE, CMP_NE},    {"lt", C_COMPARE, CMP_LT},
    {"le", C_COMPARE, CMP_LE},    {"gt", C_COMPARE, CMP_GT},    {"ge", C_COMPARE, CMP_GE},
    {"and", C_BOOL, PNX_AND},     {"or", C_BOOL, PNX_OR},       {"not", C_NOT, 0},
    {"sat_add", C_ARITH, PNX_SATADD}, {"sat_sub", C_ARITH, PNX_SATSUB}, {"sat_mul", C_ARITH, PNX_SATMUL},
    {"u_div", C_ARITH, PNX_UDIV}, {"u_mod", C_ARITH, PNX_UMOD}, {"rotl", C_ARITH, PNX_ROTL},
    {"rotr", C_ARITH, PNX_ROTR},  {"mul_hi", C_ARITH, PNX_MULHI}, {"u_mul_hi", C_ARITH, PNX_UMULHI},
    {"floor_div", C_ARITH, PNX_FDIV}, {"floor_mod", C_ARITH, PNX_FMOD},
    {"abs", C_UNARY, PNX_ABS},    {"bit_not", C_UNARY, PNX_BNOT}, {"count_ones", C_UNARY, PNX_POPC},
    {"leading_zeros", C_UNARY, PNX_CLZ}, {"trailing_zeros", C_UNARY, PNX_CTZ}, {"byte_swap", C_UNARY, PNX_BSWAP},
    {"u_lt", C_UCMP, CMP_LT},     {"u_le", C_UCMP, CMP_LE},     {"u_gt", C_UCMP, CMP_GT},
    {"u_ge", C_UCMP, CMP_GE},
    {"chk_add", C_CHK, PNX_CHKADD}, {"chk_sub", C_CHK, PNX_CHKSUB}, {"chk_mul", C_CHK, PNX_CHKMUL},
    {"chk_div", C_CHK, PNX_CHKDIV},
    {"clamp", C_CLAMP, 0},        {"select", C_SELECT, 0},
};

static const core_word *find_core(name n) {
    for (size_t k = 0; k < sizeof core_words / sizeof core_words[0]; ++k) {
        if (name_is(n, core_words[k].name))
            return &core_words[k];
    }
    return NULL;
}

static const constant *find_const(const comp *c, name n) {
    const constant *all = c->consts.ptr;
    for (uint32_t k = c->eval ? 0 : c->base.consts; k < c->consts.n; ++k) {
        if ((k >= c->base.consts || all[k].module == c->module) && name_eq(all[k].name, n))
            return &all[k];
    }
    return NULL;
}

static uint32_t find_global(const comp *c, name n) {
    const globalsym *all = c->globals.ptr;
    for (uint32_t k = c->eval ? 0 : c->base.globals; k < c->globals.n; ++k) {
        if ((k >= c->base.globals || all[k].module == c->module) && name_eq(all[k].name, n))
            return k;
    }
    return NONE;
}

static uint32_t find_record(const comp *c, name n) {
    const record *all = c->records.ptr;
    for (uint32_t k = c->eval ? 0 : c->base.records; k < c->records.n; ++k) {
        if ((k >= c->base.records || all[k].module == c->module) && name_eq(all[k].name, n))
            return k;
    }
    return NONE;
}

// The extern of this source called `n`: its number in comp.externs, or NONE.
static uint32_t find_extern(const comp *c, name n) {
    const externsym *all = c->externs.ptr;
    for (uint32_t k = c->eval ? 0 : c->base.externs; k < c->externs.n; ++k) {
        if ((k >= c->base.externs || all[k].module == c->module) && name_eq(all[k].name, n))
            return k;
    }
    return NONE;
}

// The record a field path begins with: one of this source, or one a module in use exports.
static uint32_t find_record_any(const comp *c, name n) {
    const uint32_t own = find_record(c, n);
    if (own != NONE)
        return own;
    const record *all = c->records.ptr;
    for (uint32_t k = 0; k < c->base.records; ++k) {
        if (all[k].exported && name_eq(all[k].name, n) && source_used(c, all[k].module))
            return k;
    }
    return NONE;
}

// Field `n` of record `r`, or NULL.
static const field *find_field(const comp *c, uint32_t r, name n) {
    const record *rec = (const record *)c->records.ptr + r;
    const field *all = (const field *)c->fields.ptr + rec->first;
    for (uint32_t k = 0; k < rec->count; ++k) {
        if (name_eq(all[k].name, n))
            return &all[k];
    }
    return NULL;
}

// Words the compiler expands itself: stems, combining words, the basic string words.
enum {
    BW_RANGE, BW_SUM, BW_EACH, BW_FOLD, BW_TIMES, BW_LEN, BW_IS_EMPTY, BW_BYTE_AT, BW_BYTES,
    BW_IDX, BW_CAPACITY, BW_IS_FULL, BW_CLEAR, BW_PUSH, BW_POP, BW_FIND, BW_TO_INT, BW_TO_STR, BW_ASSERT,
    BW_WORDS, BW_ERR_TEXT, BW_ADD_ROW, BW_IS_NONE, BW_NONE, BW_ROW_INDEX, BW_MAX, BW_MIN,
    BW_APPEND, BW_APPEND_BYTE, BW_APPEND_INT, BW_CONTENTS, BW_TAKE, BW_SKIP, BW_TAKE_LAST, BW_SKIP_LAST,
    BW_STARTS_WITH, BW_ENDS_WITH, BW_CONTAINS, BW_LOOKUP, BW_INSERT, BW_REMOVE, BW_REPLACE, BW_PUT, BW_PEEK,
    BW_ALLOC, BW_FREE, BW_IS_ALIVE, BW_COUNT, BW_ANY, BW_ALL, BW_FIRST, BW_EACH_INDEX, BW_MAP_INTO, BW_FILTER_INTO, BW_MINIMUM, BW_MAXIMUM,
    BW_SPAWN, BW_YIELD, BW_RESUME, BW_IS_DONE, BW_CANCEL, BW_RUN_ALL,
    BW_SLICE, BW_CUT, BW_TRY_CUT, BW_TRIM, BW_TRIM_LEFT, BW_TRIM_RIGHT, BW_LINES, BW_SPLIT, BW_FIND_LAST, BW_COMPARE,
    BW_HEX, BW_BIN, BW_HEX_PAD, BW_ZERO_PAD, BW_PAD_LEFT, BW_PAD_RIGHT, BW_CALL, BW_MARK, BW_RELEASE, BW_REGION_FREE,
    BW_SPAN, BW_CHARS, BW_JOIN_INTO, BW_UPPER, BW_LOWER, BW_REPEAT, BW_TO_INT_BASE, BW_IS_UTF8, BW_CHAR_COUNT,
    BW_REPLACE_INTO, BW_APPEND_HEX, BW_CRC32, BW_FNV1A, BW_HASH64, BW_BASE64_ENCODE, BW_BASE64_DECODE,
    BW_HEX_ENCODE, BW_HEX_DECODE, BW_URL_ENCODE, BW_URL_DECODE, BW_TRUNCATE, BW_PTN_IS_MATCH, BW_PTN_MATCH,
    BW_PTN_CAPTURE, BW_PTN_MATCHES, BW_PTN_REPLACE, BW_RX, BW_RX_COMPILE, BW_RX_IS_MATCH, BW_RX_MATCH, BW_RX_CUT,
    BW_RX_MATCHES, BW_RX_SPLIT, BW_RX_REPLACE, BW_RX_REPLACE_INTO, BW_WAIT_TAKE, BW_WAIT_PUT
};
static const struct builtin {
    const char *name;
    uint8_t     id;
    uint8_t     module; // 0: core, written bare; 1: module str; 2: module fmt - written str.word, fmt.word
} builtins[] = {
    {"range", BW_RANGE, 0},     {"sum", BW_SUM, 0},        {"each", BW_EACH, 0},
    {"fold", BW_FOLD, 0},       {"times", BW_TIMES, 0},    {"len", BW_LEN, 0},
    {"is_empty", BW_IS_EMPTY, 0}, {"byte_at", BW_BYTE_AT, 1}, {"bytes", BW_BYTES, 1},
    {"idx", BW_IDX, 0},         {"capacity", BW_CAPACITY, 0}, {"is_full", BW_IS_FULL, 0},
    {"clear", BW_CLEAR, 0},     {"push", BW_PUSH, 0},      {"pop", BW_POP, 0},
    {"find", BW_FIND, 1},        {"to_int", BW_TO_INT, 1},   {"to_str", BW_TO_STR, 1},
    {"assert", BW_ASSERT, 0},   {"words", BW_WORDS, 1},     {"err_text", BW_ERR_TEXT, 0},
    {"add_row", BW_ADD_ROW, 0}, {"is_none", BW_IS_NONE, 0}, {"none", BW_NONE, 0},
    {"row_index", BW_ROW_INDEX, 0}, {"max", BW_MAX, 0},    {"min", BW_MIN, 0},
    {"append", BW_APPEND, 0},   {"append_byte", BW_APPEND_BYTE, 0}, {"append_int", BW_APPEND_INT, 0},
    {"contents", BW_CONTENTS, 0}, {"take", BW_TAKE, 0},    {"skip", BW_SKIP, 1},
    {"take_last", BW_TAKE_LAST, 1}, {"skip_last", BW_SKIP_LAST, 1},
    {"starts_with", BW_STARTS_WITH, 1}, {"ends_with", BW_ENDS_WITH, 1}, {"contains", BW_CONTAINS, 1},
    {"lookup", BW_LOOKUP, 0},   {"insert", BW_INSERT, 0},  {"remove", BW_REMOVE, 0},
    {"replace", BW_REPLACE, 1},  {"put", BW_PUT, 0},        {"peek", BW_PEEK, 0},
    {"alloc", BW_ALLOC, 0},     {"free", BW_FREE, 0},      {"is_alive", BW_IS_ALIVE, 0},
    {"count", BW_COUNT, 0},     {"any", BW_ANY, 0},        {"all", BW_ALL, 0},
    {"first", BW_FIRST, 0},     {"each_index", BW_EACH_INDEX, 0},
    {"map_into", BW_MAP_INTO, 0}, {"filter_into", BW_FILTER_INTO, 0},
    {"spawn", BW_SPAWN, 0},     {"yield", BW_YIELD, 0},    {"resume", BW_RESUME, 0},
    {"is_done", BW_IS_DONE, 0}, {"cancel", BW_CANCEL, 0},  {"run_all", BW_RUN_ALL, 0},
    {"wait_take", BW_WAIT_TAKE, 0}, {"wait_put", BW_WAIT_PUT, 0},
    {"minimum", BW_MINIMUM, 0}, {"maximum", BW_MAXIMUM, 0},
    {"slice", BW_SLICE, 1},         {"cut", BW_CUT, 1},            {"try_cut", BW_TRY_CUT, 1},
    {"trim", BW_TRIM, 1},           {"trim_left", BW_TRIM_LEFT, 1}, {"trim_right", BW_TRIM_RIGHT, 1},
    {"lines", BW_LINES, 1},         {"split", BW_SPLIT, 1},        {"find_last", BW_FIND_LAST, 1},
    {"compare", BW_COMPARE, 1},     {"pad_left", BW_PAD_LEFT, 1},  {"pad_right", BW_PAD_RIGHT, 1},
    {"hex", BW_HEX, 2},             {"bin", BW_BIN, 2},            {"hex_pad", BW_HEX_PAD, 2},
    {"zero_pad", BW_ZERO_PAD, 2},   {"call", BW_CALL, 0},          {"mark", BW_MARK, 0},
    {"release", BW_RELEASE, 0},     {"region_free", BW_REGION_FREE, 0},
    {"span", BW_SPAN, 1},           {"chars", BW_CHARS, 1},        {"join_into", BW_JOIN_INTO, 1},
    {"upper", BW_UPPER, 1},         {"lower", BW_LOWER, 1},        {"repeat", BW_REPEAT, 1},
    {"to_int_base", BW_TO_INT_BASE, 1}, {"is_utf8", BW_IS_UTF8, 1}, {"char_count", BW_CHAR_COUNT, 1},
    {"replace_into", BW_REPLACE_INTO, 1}, {"append_hex", BW_APPEND_HEX, 2},
    {"crc32", BW_CRC32, 3},         {"fnv1a", BW_FNV1A, 3},        {"hash64", BW_HASH64, 3},
    {"base64_encode", BW_BASE64_ENCODE, 4}, {"base64_decode", BW_BASE64_DECODE, 4},
    {"hex_encode", BW_HEX_ENCODE, 4}, {"hex_decode", BW_HEX_DECODE, 4},
    {"url_encode", BW_URL_ENCODE, 4}, {"url_decode", BW_URL_DECODE, 4},
    {"truncate", BW_TRUNCATE, 0},
    {"is_match", BW_PTN_IS_MATCH, 6}, {"match", BW_PTN_MATCH, 6},   {"capture", BW_PTN_CAPTURE, 6},
    {"matches", BW_PTN_MATCHES, 6}, {"replace", BW_PTN_REPLACE, 6},
    {"rx", BW_RX, 7},               {"compile", BW_RX_COMPILE, 7}, {"is_match", BW_RX_IS_MATCH, 7},
    {"match", BW_RX_MATCH, 7},      {"cut_match", BW_RX_CUT, 7},   {"matches", BW_RX_MATCHES, 7},
    {"split", BW_RX_SPLIT, 7},      {"replace", BW_RX_REPLACE, 7}, {"replace_into", BW_RX_REPLACE_INTO, 7},
    {"len", BW_LEN, 1},             {"is_empty", BW_IS_EMPTY, 1},  {"take", BW_TAKE, 1},
};

// A word of the core (module 0, written bare) or of a standard module (1: str, 2: fmt, written
// with the module: str.len). `len`, `is_empty` and `take` exist twice: bare for collections
// and queues, in `str` for strings.
// The standard modules, part of the language: their number in the builtin table, or 0.
static uint8_t standard_module(name n) {
    return name_is(n, "str") ? 1 : name_is(n, "fmt") ? 2 : name_is(n, "hash") ? 3 : name_is(n, "encode") ? 4
         : name_is(n, "bytes") ? 5 : name_is(n, "ptn") ? 6 : name_is(n, "rex") ? 7 : name_is(n, "json") ? 8
         : name_is(n, "csv") ? 9 : 0;
}

static const struct builtin *find_builtin(name n, uint8_t module) {
    for (size_t k = 0; k < sizeof builtins / sizeof builtins[0]; ++k) {
        if (builtins[k].module == module && name_is(n, builtins[k].name))
            return &builtins[k];
    }
    return NULL;
}

// A name every part of the source sees: a word, a constant, a global, a type, a core word.
static bool name_is_global(comp *c, name n) {
    return find_word(c, n, NULL) || find_fwd(c, n, NULL) || find_extern(c, n) != NONE || find_core(n) || type_of_name(c, n) != TY_NONE ||
           name_is(n, "float") || name_is(n, "raw") ||
           find_record(c, n) != NONE ||
           name_is(n, "err") || find_const(c, n) || find_global(c, n) != NONE || find_builtin(n, 0);
}

// No shadowing (RFC-0008 section 3.3): a new local or loop name must not be a visible name.
static bool name_is_free(comp *c, name n) {
    if (find_local(c->f, n) || name_is_global(c, n))
        return type_error(c, "this name is already visible here; a new name may not hide it");
    for (uint32_t k = 0; k < c->f->n_blocks; ++k) {
        if (c->f->blocks[k].name.length && name_eq(c->f->blocks[k].name, n))
            return type_error(c, "this name is already visible here; a new name may not hide it");
    }
    return true;
}

// ---- words of the core --------------------------------------------------------------------

static bool move_slot(comp *c, tid type, uint32_t to, uint32_t from) {
    return emit_def(c, PNX_ABC(bank_of(type) == BANK_S ? PNX_MOVS : PNX_MOVI, to, from, 0), bank_of(type)) != NONE;
}

static bool stack_word(comp *c, int which) {
    fn *f = c->f;
    if (!need(c, which == ST_DUP || which == ST_DROP ? 1 : 2))
        return false;
    entry *top = &f->stack[f->depth - 1];
    switch (which) {
    case ST_DROP:
        --f->depth;
        return true;
    case ST_DUP:
    case ST_OVER: {
        const uint32_t from = f->depth - (which == ST_DUP ? 1u : 2u);
        entry copy = f->stack[from];
        if (copy.kind == E_SLOT) {
            const uint32_t source = slot_of(c, from);
            if (!push_slot(c, copy.type))
                return false;
            f->stack[f->depth - 1].born = copy.born;
            return move_slot(c, copy.type, slot_of(c, f->depth - 1), source);
        }
        if (!push(c, copy))
            return false;
        f->stack[f->depth - 1].born = copy.born;
        return true;
    }
    case ST_NIP: {
        entry *under = top - 1;
        if (top->kind == E_SLOT && bank_of(top->type) == bank_of(under->type)) {
            const uint32_t from = slot_of(c, f->depth - 1), to = slot_of(c, f->depth - 2);
            if (!move_slot(c, top->type, to, from))
                return false;
        }
        *under = *top;
        --f->depth;
        return true;
    }
    default: { // ST_SWAP
        entry *under = top - 1;
        if (bank_of(top->type) == bank_of(under->type) && (top->kind == E_SLOT || under->kind == E_SLOT)) {
            // Both through registers: the upper value goes one above, the lower moves up, the
            // upper comes down. Entries that are not in a register yet are put there first.
            if (!settle(c, f->depth - 2) || !settle(c, f->depth - 1))
                return false;
            const uint32_t lo = slot_of(c, f->depth - 2), hi = slot_of(c, f->depth - 1);
            const uint32_t spare = top_slot(c, bank_of(top->type), 1);
            const tid      t = top->type, u = under->type;
            if (!move_slot(c, t, spare, hi) || !move_slot(c, u, hi, lo) || !move_slot(c, t, lo, spare))
                return false;
        }
        const entry e = *top;
        *top = *under;
        *under = e;
        return true;
    }
    }
}

#define OP_SC_OF(w) ((int32_t)(int8_t)((w) >> 24))

static bool fits8(int64_t v) {
    return v >= INT8_MIN && v <= INT8_MAX;
}

static bool arith_word(comp *c, uint32_t op) {
    fn *f = c->f;
    if (!need(c, 2))
        return false;
    entry *a = &f->stack[f->depth - 2], *b = &f->stack[f->depth - 1];
    // A declared type D over int (RFC-0017 section 3.1): D and D are added and subtracted; D is
    // multiplied by a plain int from either side, divided by one and taken modulo one.
    const bool da = a->type != TY_INT && base_of(c, a->type) == TY_INT;
    const bool db = b->type != TY_INT && base_of(c, b->type) == TY_INT;
    const bool scaled = (op == PNX_MUL && ((da && b->type == TY_INT) || (db && a->type == TY_INT))) ||
                        ((op == PNX_DIV || op == PNX_MOD) && da && b->type == TY_INT);
    const tid result = db && !da ? b->type : a->type;
    if (!scaled && (a->type != b->type || base_of(c, a->type) != TY_INT ||
                    (a->type != TY_INT && op != PNX_ADD && op != PNX_SUB)))
        return type_error(c, (da || db) && (a->type == b->type || a->type == TY_INT || b->type == TY_INT)
                                 ? "a declared type is added to and subtracted from its own type, and multiplied, divided "
                                   "or taken modulo by an int; for other arithmetic take raw"
                                 : "this word takes two int values, or two of the same declared type");
    const uint32_t dst = slot_of(c, f->depth - 2);
    uint32_t ra, rb;
    // a small constant on top goes into the instruction (C, a signed byte)
    const uint32_t with_imm = op == PNX_MUL ? PNX_MULI : op == PNX_DIV ? PNX_DIVI : op == PNX_MOD ? PNX_MODI
                            : op == PNX_AND ? PNX_ANDI : op == PNX_OR  ? PNX_ORI  : op == PNX_XOR ? PNX_XORI
                            : op == PNX_SHL ? PNX_SHLI : op == PNX_SHR ? PNX_SHRI : op == PNX_USHR ? PNX_USHRI
                            : op == PNX_WMUL ? PNX_WMULI : NONE;
    if ((op == PNX_ADD || op == PNX_SUB) && b->kind == E_CONST && fits8(op == PNX_ADD ? b->value : -b->value)) {
        const int64_t imm = op == PNX_ADD ? b->value : -b->value;
        if (!reg_of(c, f->depth - 2, &ra) || emit_def(c, PNX_ABC(PNX_ADDI, dst, ra, (uint8_t)(int8_t)imm), BANK_I) == NONE)
            return false;
    } else if (with_imm != NONE && b->kind == E_CONST && fits8(b->value) &&
               !((op == PNX_DIV || op == PNX_MOD) && b->value == 0)) { // a zero divisor keeps its fault at run time
        if (!reg_of(c, f->depth - 2, &ra) ||
            emit_def(c, PNX_ABC(with_imm, dst, ra, (uint8_t)(int8_t)b->value), BANK_I) == NONE)
            return false;
    } else {
        if (!reg_of(c, f->depth - 2, &ra) || !reg_of(c, f->depth - 1, &rb) ||
            emit_def(c, PNX_ABC(op, dst, ra, rb), BANK_I) == NONE)
            return false;
    }
    f->depth -= 2;
    return push_slot(c, result);
}

static bool compare_word(comp *c, int which) {
    fn *f = c->f;
    if (!need(c, 2))
        return false;
    entry *a = &f->stack[f->depth - 2], *b = &f->stack[f->depth - 1];
    if (a->type != b->type)
        return type_error(c, "the two values compared must have the same type");
    if (base_of(c, a->type) != TY_INT && a->type != TY_STR && which != CMP_EQ && which != CMP_NE)
        return type_error(c, "values of this type are compared with eq and ne only");
    entry e = {.type = TY_BOOL, .kind = E_COND};
    uint32_t ra, rb;
    if (a->type == TY_STR && which != CMP_EQ && which != CMP_NE) { // byte order: compare, then test the sign
        if (!reg_of(c, f->depth - 2, &ra) || !reg_of(c, f->depth - 1, &rb))
            return false;
        f->depth -= 2;
        const uint32_t dst = top_slot(c, BANK_I, 1);
        if (emit(c, PNX_ABC(PNX_SCMP, dst, ra, rb)) == NONE)
            return false;
        e.op = which == CMP_LT || which == CMP_LE ? PNX_JLTI : PNX_JGEI;
        e.a = (uint8_t)dst;
        e.b = which == CMP_LE || which == CMP_GT ? 1 : 0; // < 0 ; < 1 ; >= 1 ; >= 0
        return push(c, e);
    }
    if (a->type == TY_STR) {
        if (!reg_of(c, f->depth - 2, &ra) || !reg_of(c, f->depth - 1, &rb))
            return false;
        e.op = which == CMP_EQ ? PNX_JSEQ : PNX_JSNE;
        e.a = (uint8_t)ra;
        e.b = (uint8_t)rb;
        f->depth -= 2;
        return push(c, e);
    }
    // a small literal on the right: the immediate forms
    int64_t imm = b->kind == E_CONST ? b->value : 0;
    bool    immediate = b->kind == E_CONST;
    if (immediate && (which == CMP_LE || which == CMP_GT)) {
        if (imm == INT64_MAX)
            immediate = false;
        else
            ++imm; // a <= k  is  a < k+1 ;  a > k  is  a >= k+1
    }
    if (immediate && fits8(imm)) {
        if (!reg_of(c, f->depth - 2, &ra))
            return false;
        e.op = which == CMP_EQ ? PNX_JEQI : which == CMP_NE ? PNX_JNEI
             : (which == CMP_LT || which == CMP_LE) ? PNX_JLTI : PNX_JGEI;
        e.a = (uint8_t)ra;
        e.b = (uint8_t)(int8_t)imm;
    } else {
        if (!reg_of(c, f->depth - 2, &ra) || !reg_of(c, f->depth - 1, &rb))
            return false;
        const bool swap = which == CMP_GT || which == CMP_GE; // a > b is b < a ; a >= b is b <= a
        e.op = which == CMP_EQ ? PNX_JEQ : which == CMP_NE ? PNX_JNE
             : (which == CMP_LT || which == CMP_GT) ? PNX_JLT : PNX_JLE;
        e.a = (uint8_t)(swap ? rb : ra);
        e.b = (uint8_t)(swap ? ra : rb);
    }
    f->depth -= 2;
    return push(c, e);
}

static bool bool_word(comp *c, uint32_t op) {
    fn *f = c->f;
    if (!need(c, 2))
        return false;
    if (f->stack[f->depth - 2].type != TY_BOOL || f->stack[f->depth - 1].type != TY_BOOL)
        return type_error(c, "and, or take two bool values; the bit words are bit_and, bit_or");
    const uint32_t dst = slot_of(c, f->depth - 2);
    uint32_t ra, rb;
    if (!reg_of(c, f->depth - 2, &ra) || !reg_of(c, f->depth - 1, &rb) ||
        emit_def(c, PNX_ABC(op, dst, ra, rb), BANK_I) == NONE)
        return false;
    f->depth -= 2;
    return push_slot(c, TY_BOOL);
}

static bool guard_op(comp *c, tid out);

// Like the built-in words below, this ends with the token after the word current.
static bool core_plain(comp *c, const core_word *w);

static bool core(comp *c, const core_word *w) {
    fn *f = c->f;
    uint32_t ra, rb;
    switch (w->kind) {
    case C_UNARY: { // int gives int
        if (!need(c, 1))
            return false;
        if (f->stack[f->depth - 1].type != TY_INT &&
            !(w->op == PNX_ABS && base_of(c, f->stack[f->depth - 1].type) == TY_INT)) // abs keeps a declared type
            return type_error(c, "this word takes an int");
        const uint32_t dst = slot_of(c, f->depth - 1);
        if (!reg_of(c, f->depth - 1, &ra) || emit_def(c, PNX_ABC(w->op, dst, ra, 0), BANK_I) == NONE)
            return false;
        f->stack[f->depth - 1].kind = E_SLOT;
        f->stack[f->depth - 1].src = 0;
        return advance(c);
    }
    case C_UCMP:  // the two values compared as unsigned numbers
    case C_CHK: { // the arithmetic that fails instead of faulting
        if (!need(c, 2))
            return false;
        if (f->stack[f->depth - 1].type != TY_INT || f->stack[f->depth - 2].type != TY_INT)
            return type_error(c, "this word takes two int values");
        if (!settle(c, f->depth - 2) || !settle(c, f->depth - 1))
            return false;
        ra = slot_of(c, f->depth - 2);
        rb = slot_of(c, f->depth - 1);
        f->depth -= 2;
        if (w->kind == C_CHK)
            return settle_all(c) && emit(c, PNX_ABC(w->op, ra, ra, rb)) != NONE && guard_op(c, TY_INT);
        const bool swap = w->op == CMP_GT || w->op == CMP_GE; // a > b is b < a
        const uint32_t op = w->op == CMP_LT || w->op == CMP_GT ? PNX_ULT : PNX_ULE;
        return emit_def(c, PNX_ABC(op, ra, swap ? rb : ra, swap ? ra : rb), BANK_I) != NONE && push_slot(c, TY_BOOL) &&
               advance(c);
    }
    case C_CLAMP: { // n lo hi clamp : lo when n is below it, hi when above
        if (!need(c, 3))
            return false;
        for (uint32_t k = 1; k <= 3; ++k) {
            if (f->stack[f->depth - k].type != TY_INT)
                return type_error(c, "clamp takes three int values: n lo hi clamp");
        }
        uint32_t lo, hi;
        if (!settle(c, f->depth - 3) || !reg_of(c, f->depth - 2, &lo) || !reg_of(c, f->depth - 1, &hi))
            return false;
        const uint32_t dst = slot_of(c, f->depth - 3);
        if (emit(c, PNX_ABC(PNX_JLE, lo, dst, 1)) == NONE || emit(c, PNX_ABC(PNX_MOVI, dst, lo, 0)) == NONE ||
            emit(c, PNX_ABC(PNX_JLE, dst, hi, 1)) == NONE || emit(c, PNX_ABC(PNX_MOVI, dst, hi, 0)) == NONE)
            return false;
        f->depth -= 2;
        f->def_at = NONE;
        return advance(c);
    }
    case C_SELECT: { // flag a b select : a when the flag is true, else b; no branch in the source
        if (!need(c, 3))
            return false;
        const tid type = f->stack[f->depth - 1].type;
        if (f->stack[f->depth - 3].type != TY_BOOL || f->stack[f->depth - 2].type != type)
            return type_error(c, "select takes a bool and two values of one type: flag a b select");
        uint32_t flag;
        if (!settle(c, f->depth - 3) || !settle(c, f->depth - 2) || !reg_of(c, f->depth - 1, &rb))
            return false;
        flag = slot_of(c, f->depth - 3);
        ra = slot_of(c, f->depth - 2);
        if (bank_of(type) == BANK_I) { // the result takes the flag's register
            if (emit(c, PNX_ABC(PNX_JEQI, flag, 0, 2)) == NONE || emit(c, PNX_ABC(PNX_MOVI, flag, ra, 0)) == NONE ||
                emit(c, PNX_AX(PNX_JMP, 1)) == NONE || emit(c, PNX_ABC(PNX_MOVI, flag, rb, 0)) == NONE)
                return false;
        } else if (emit(c, PNX_ABC(PNX_JNEI, flag, 0, 1)) == NONE || emit(c, PNX_ABC(PNX_MOVS, ra, rb, 0)) == NONE) {
            return false; // the result stays in a's register
        }
        f->depth -= 3;
        return push_slot(c, type) && advance(c);
    }
    default: break;
    }
    return core_plain(c, w) && advance(c);
}

static bool core_plain(comp *c, const core_word *w) {
    fn *f = c->f;
    switch (w->kind) {
    case C_STACK: return stack_word(c, w->op);
    case C_ARITH: return arith_word(c, w->op);
    case C_COMPARE: return compare_word(c, w->op);
    case C_BOOL: return bool_word(c, w->op);
    case C_NOT: {
        entry e;
        if (!pop_cond(c, &e))
            return false;
        cond_negate(&e);
        return push(c, e);
    }
    default: { // C_NEG
        if (!need(c, 1))
            return false;
        if (base_of(c, f->stack[f->depth - 1].type) != TY_INT)
            return type_error(c, "neg takes an int, or a value of a declared type over int");
        const uint32_t dst = slot_of(c, f->depth - 1);
        uint32_t ra;
        if (!reg_of(c, f->depth - 1, &ra) || emit_def(c, PNX_ABC(PNX_NEG, dst, ra, 0), BANK_I) == NONE)
            return false;
        f->stack[f->depth - 1].kind = E_SLOT;
        return true;
    }
    }
}

// ---- calls --------------------------------------------------------------------------------

static bool check_before_call(comp *c) {
    fn *f = c->f;
    uint32_t n = f->code.n - f->last_check + 2;
    if (n > 0xffffu)
        n = 0xffffu;
    if (emit(c, PNX_ABX(PNX_CHECK, 0, n)) == NONE)
        return false;
    f->last_check = f->code.n;
    return true;
}

// Checks the arguments on the stack against `s`, puts them into their registers and reports
// the first register of each bank. Leaves the arguments popped.
static bool released(comp *c, uint8_t serial);

static bool call_args(comp *c, const sig *s, uint32_t base[2]) {
    fn *f = c->f;
    for (uint32_t k = 0; k < s->n_in && f->depth - f->floor >= s->n_in; ++k) {
        if (s->in[k] == TY_BUF) { // a buffer goes to a host word as its number
            entry *e = &f->stack[f->depth - s->n_in + k];
            if (e->type != TY_COLL || ((const globalsym *)c->globals.ptr + e->value)->kind != GK_BUFFER)
                return type_error(c, "this host word takes a buffer here");
            *e = (entry){.type = TY_BUF, .kind = E_CONST, .value = e->value};
        }
    }
    c->sink = true;
    if (!need(c, s->n_in))
        return false;
    const uint32_t first = f->depth - s->n_in;
    for (uint32_t k = 0; k < s->n_in; ++k) {
        if (f->stack[first + k].type != s->in[k])
            return type_error(c, "an argument has another type than the word's signature asks for");
        if (s->in_owned[k] ? !give(c, first + k) : f->stack[first + k].owned)
            return type_error(c, s->in_owned[k] ? "this parameter takes an owned value; what is given here is not one"
                                                : "an owned value is lent from a local, not straight from the stack");
    }
    for (uint32_t k = first; k < f->depth; ++k) {
        if (!settle(c, k))
            return false;
    }
    f->depth = first;
    for (uint32_t k = 0; k < s->n_in; ++k) { // the word may release a mark given to it
        if (s->in[k] == TY_MARK && s->in_owned[k] && !released(c, f->stack[first + k].born))
            return false;
    }
    base[BANK_I] = top_slot(c, BANK_I, 0);
    base[BANK_S] = top_slot(c, BANK_S, 0);
    return true;
}

static void count_sig(const sig *s, uint32_t in[2], uint32_t out[2]) {
    in[0] = in[1] = out[0] = out[1] = 0;
    for (uint32_t k = 0; k < s->n_in; ++k)
        ++in[bank_of(s->in[k])];
    for (uint32_t k = 0; k < s->n_out; ++k)
        ++out[bank_of(s->out[k])];
}

// Pushes the results of a call. A host word leaves its results after its arguments; the stack
// wants them where the arguments were.
static bool push_results(comp *c, const sig *s, const uint32_t base[2], const uint32_t in[2], bool host) {
    uint32_t seen[2] = {0, 0};
    for (uint32_t k = 0; k < s->n_out; ++k) { // a mark given back is a new one: made after the call's values
        if (s->out[k] == TY_MARK) {
            if (c->f->marks == UINT8_MAX)
                return syntax(c, "a word makes at most 255 marks");
            ++c->f->marks;
            break;
        }
    }
    for (uint32_t k = 0; k < s->n_out; ++k) {
        const int bank = bank_of(s->out[k]);
        if (!push_slot(c, s->out[k]))
            return false;
        c->f->stack[c->f->depth - 1].owned = s->out_owned[k];
        if (host && in[bank] && !move_slot(c, s->out[k], base[bank] + seen[bank], base[bank] + in[bank] + seen[bank]))
            return false;
        ++seen[bank];
    }
    return true;
}

static bool emit_return(comp *c);
static bool emit_defers(comp *c, uint32_t from);
static bool defers_from(const fn *f, uint32_t from);
static void save_owns(const fn *f, lstate *out);
static void restore_owns(fn *f, const lstate *from, uint32_t count);
static block *open_block(comp *c, uint8_t kind);
static void snapshot(const fn *f, shot *out);
static bool name_is_free(comp *c, name n);

// The word after a call that can fail (RFC-0008 section 7.2). The call has been emitted; the
// next instruction is the one that runs on failure and is skipped on success. The current
// token is the guard.
static bool guard(comp *c, const sig *s, const uint32_t base[2], const uint32_t in[2], bool host) {
    fn *f = c->f;
    if (c->in_defer && (is_word(c, PNX_KW_or_fail) || is_word(c, PNX_KW_or_return)))
        return syntax(c, "a defer does not leave the word; a failure in it is taken with or_else");
    if (is_word(c, PNX_KW_or_fail)) {
        if (f->is_top || !f->sig->fails)
            return type_error(c, "or_fail passes the failure on, so this word needs `or fail` in its signature");
        if (!f->n_defers)
            return nothing_owned(c) && emit(c, PNX_ABC(PNX_RETF, 0, 0, 0)) != NONE &&
                   push_results(c, s, base, in, host) && advance(c);
        // with defers: on failure keep the error, run the defers, then fail with it
        uint32_t handler = NONE, resume = NONE;
        const uint32_t before = f->depth;
        if (!jump_later(c, &handler) || !push_results(c, s, base, in, host) || !jump_later(c, &resume))
            return false;
        const uint32_t after = f->depth;
        entry results[MAX_STACK]; // the stub below works where the results lie on the other path
        __builtin_memcpy(results, f->stack + before, (after - before) * sizeof(entry));
        land(c, &handler);
        f->depth = before;
        lstate states[MAX_LOCALS];
        save_owns(f, states);
        if (!push_slot(c, TY_ERR))
            return false;
        const uint32_t keep = slot_of(c, f->depth - 1);
        if (emit(c, PNX_ABX(PNX_GETFAIL, keep, 0)) == NONE || !emit_defers(c, 0))
            return false;
        --f->depth;
        if (!nothing_owned(c) || emit(c, PNX_ABC(PNX_FAILR, keep, 0, 0)) == NONE)
            return false;
        restore_owns(f, states, f->n_locals);
        __builtin_memcpy(f->stack + before, results, (after - before) * sizeof(entry));
        f->depth = after;
        land(c, &resume);
        return advance(c);
    }
    const bool returns = is_word(c, PNX_KW_or_return);
    if (!returns && !is_word(c, PNX_KW_or_else))
        return type_error(c, "a word that can fail is followed by or_fail, or_return or or_else");
    if (returns && f->is_top)
        return syntax(c, "or_return belongs inside a definition");
    uint32_t handler = NONE, resume = NONE;
    const uint32_t before = f->depth;
    if (!jump_later(c, &handler) || !push_results(c, s, base, in, host))
        return false;
    if (returns) { // on failure: return with the stack as it was without the results
        const uint32_t after = f->depth;
        if (!jump_later(c, &resume))
            return false;
        land(c, &handler);
        f->depth = before;
        uint8_t states[MAX_LOCALS]; // the return may give owned locals away; the path that goes on keeps them
        for (uint32_t k = 0; k < f->n_locals; ++k)
            states[k] = f->locals[k].state;
        if (!emit_return(c))
            return false;
        for (uint32_t k = 0; k < f->n_locals; ++k)
            f->locals[k].state = states[k];
        f->depth = after;
        land(c, &resume);
        return advance(c);
    }
    // or_else [ ( e ) ] body end : the body runs on failure and leaves what the word would have
    block *b = open_block(c, B_ORELSE);
    if (!b)
        return false;
    snapshot(f, &b->out);
    b->has_out = true;
    b->in.depth = (uint8_t)before;
    f->depth = before;
    if (!jump_later(c, &b->exits))
        return false;
    land(c, &handler);
    if (!advance(c))
        return false;
    if (c->tok.kind == PNX_T_LPAREN) {
        if (!advance(c))
            return false;
        if (c->tok.kind != PNX_T_NAME)
            return syntax(c, "a name for the error number is expected: or_else (e)");
        const name n = tok_name(c);
        if (!name_is_free(c, n))
            return false;
        if (f->n_locals == MAX_LOCALS || f->nloc[BANK_I] >= SLOT0 - 1)
            return syntax(c, "too many local variables in this word");
        local *l = &f->locals[f->n_locals++];
        *l = (local){.name = n, .type = TY_ERR, .reg = f->nloc[BANK_I]++};
        if (f->nloc[BANK_I] > f->maxloc[BANK_I])
            f->maxloc[BANK_I] = f->nloc[BANK_I];
        if (emit(c, PNX_ABX(PNX_GETFAIL, l->reg, 0)) == NONE || !debug_local(c, l, f->code.n) || !advance(c))
            return false;
        if (c->tok.kind != PNX_T_RPAREN)
            return syntax(c, "the error name is one name in parentheses");
        return advance(c);
    }
    return true;
}

// Both call functions end with the token after the word (and after its guard) current.
// Calls a word of the source: `index` is its number, or FWD_MARK and the number of its forward
// declaration.
static bool call_word(comp *c, const sig *sp, uint32_t index) {
    const sig s = *sp;
    uint32_t base[2], in[2], out[2];
    if (!call_args(c, &s, base) || (s.fails && !settle_all(c)))
        return false;
    count_sig(&s, in, out);
    (void)top_slot(c, BANK_I, in[0] > out[0] ? in[0] : out[0]);
    (void)top_slot(c, BANK_S, in[1] > out[1] ? in[1] : out[1]);
    if (!check_before_call(c) || emit(c, PNX_ABC(PNX_CALL, base[BANK_I], base[BANK_S], 0)) == NONE ||
        emit(c, index) == NONE || !advance(c))
        return false;
    if (s.fails)
        return guard(c, &s, base, in, false);
    if (!push_results(c, &s, base, in, false))
        return false;
    fn *f = c->f;
    bool same = !f->is_top && !f->sig->fails && f->sig->n_out == s.n_out;
    for (uint32_t k = 0; same && k < s.n_out; ++k)
        same = f->sig->out[k] == s.out[k] && f->sig->out_owned[k] == s.out_owned[k];
    if (same) {
        f->tail_at = f->code.n - 2;
        f->tail_word = index;
        f->tail_base[0] = base[0];
        f->tail_base[1] = base[1];
        f->tail_in[0] = in[0];
        f->tail_in[1] = in[1];
        f->tail_depth = f->depth;
    }
    return true;
}

static bool call_host(comp *c, uint32_t host) {
    sig s;
    if (!host_sig(c, &c->st->hosts[host], &s))
        return false;
    uint32_t import = NONE;
    const uint32_t *all = c->imports.ptr;
    for (uint32_t k = 0; k < c->imports.n; ++k) {
        if (all[k] == host)
            import = k;
    }
    if (import == NONE) {
        uint32_t *slot = vec_push(c, &c->imports, sizeof(uint32_t));
        if (!slot)
            return false;
        *slot = host;
        import = c->imports.n - 1;
    }
    uint32_t base[2], in[2], out[2];
    if (!call_args(c, &s, base) || (s.fails && !settle_all(c)))
        return false;
    count_sig(&s, in, out);
    (void)top_slot(c, BANK_I, in[0] + out[0]);
    (void)top_slot(c, BANK_S, in[1] + out[1]);
    if (!check_before_call(c) ||
        emit(c, PNX_ABC(s.fails ? PNX_HCALLF : PNX_HCALL, base[BANK_I], base[BANK_S], 0)) == NONE ||
        emit(c, import) == NONE || !advance(c))
        return false;
    return s.fails ? guard(c, &s, base, in, true) : push_results(c, &s, base, in, true);
}

// ---- storing ------------------------------------------------------------------------------

// Pops the top value into register `reg` of its bank.
static bool write_reg(comp *c, uint32_t reg) {
    fn *f = c->f;
    const uint32_t top = f->depth - 1;
    const entry    e = f->stack[top];
    const int      bank = bank_of(e.type);
    bool ok = true;
    switch (e.kind) {
    case E_SLOT: {
        const uint32_t from = slot_of(c, top);
        if (f->def_at != NONE && f->def_bank == bank && f->def_reg == from) {
            uint32_t *w = code_at(f, f->def_at); // the instruction that made the value writes the target
            *w = (*w & ~0xff00u) | reg << 8;
            f->def_at = NONE;
        } else {
            ok = move_slot(c, e.type, reg, from);
        }
        break;
    }
    case E_LOCAL:
        if (e.reg != reg)
            ok = move_slot(c, e.type, reg, e.reg);
        break;
    case E_CONST: ok = load_int(c, reg, e.value); break;
    default: ok = cond_value(c, &e, reg); break;
    }
    f->def_at = NONE;
    --f->depth;
    return ok;
}

static bool store_local(comp *c, name n) {
    fn *f = c->f;
    c->sink = true;
    if (!need(c, 1))
        return false;
    const uint32_t top = f->depth - 1;
    entry    e = f->stack[top];
    const int bank = bank_of(e.type);
    // `to NAME` stores into a local; a global is written with `set NAME` (D0042), which comes here
    // with path_global given
    const uint32_t g = c->path_global;
    if (g == NONE && !find_local(f, n) && find_global(c, n) != NONE)
        return type_error(c, "this name is a global; a global is written with set: value set name");
    if (g != NONE && ((const globalsym *)c->globals.ptr + g)->kind != GK_SCALAR) { // value index to names
        const globalsym *sym = (const globalsym *)c->globals.ptr + g;
        uint32_t rv, ri;
        if (!need(c, 2))
            return false;
        if (sym->record != NONE)
            return type_error(c, "a table of records is written field by field: value row to record.field");
        if (f->stack[top].type != TY_INT || f->stack[top - 1].type != sym->type)
            return type_error(c, "a cell is stored as: value index to names, with the value of the cell's type");
        if (!reg_of(c, top - 1, &rv) || !reg_of(c, top, &ri))
            return false;
        if (sym->strcap) { // the bytes are copied into the cell
            const uint32_t row = top_slot(c, BANK_I, 1);
            if (emit_g(c, PNX_ROW, row, ri, g) == NONE || emit(c, PNX_ABC(PNX_FSETS, rv, row, 0)) == NONE ||
                emit(c, PNX_FIELD(g, 0, sym->strcap)) == NONE)
                return false;
        } else if (emit_g(c, PNX_ASET, rv, ri, g) == NONE) {
            return false;
        }
        f->depth -= 2;
        return true;
    }
    if (g != NONE) { // a scalar global
        const globalsym *sym = (const globalsym *)c->globals.ptr + g;
        uint32_t reg;
        if (sym->type != e.type)
            return type_error(c, "this global has another type than the value stored into it");
        if (!reg_of(c, top, &reg))
            return false;
        (void)give(c, top); // storing into a global hands an owned value over
        if (sym->strcap) {
            const uint32_t one = top_slot(c, BANK_I, 1);
            if (emit(c, PNX_ABX(PNX_LOADI, one, 1)) == NONE || emit(c, PNX_ABC(PNX_FSETS, reg, one, 0)) == NONE ||
                emit(c, PNX_FIELD(g, 0, sym->strcap)) == NONE)
                return false;
        } else if (emit(c, PNX_ABX(PNX_SETG, reg, g)) == NONE) {
            return false;
        }
        --f->depth;
        return true;
    }
    local *l = find_local(f, n);
    if (!l) {
        if (f->is_top)
            return type_error(c, "outside a definition `to` makes no new local; a global is written with set");
        if (!name_is_free(c, n))
            return false;
        if (f->n_locals == MAX_LOCALS || f->nloc[bank] >= SLOT0 - 1)
            return syntax(c, "too many local variables in this word");
        l = &f->locals[f->n_locals++];
        *l = (local){.name = n, .type = e.type, .reg = f->nloc[bank]++, .drec = NONE};
        if (f->nloc[bank] > f->maxloc[bank])
            f->maxloc[bank] = f->nloc[bank];
        l->state = give(c, top) ? L_OWNS : L_PLAIN;
    } else {
        if (l->type != e.type)
            return type_error(c, "this local variable has another type than the value stored into it");
        const bool same = f->stack[top].src == (uint8_t)(l - f->locals + 1);
        if (!same) {
            if (l->state == L_OWNS)
                return type_error(c, "this local still holds an owned value; give that on first");
            l->state = give(c, top) ? L_OWNS : L_PLAIN;
        }
        l->stored = true;
        // values on the stack that were read from this local must keep the old value
        for (uint32_t k = 0; k < top; ++k) {
            if (f->stack[k].kind == E_LOCAL && bank_of(f->stack[k].type) == bank && f->stack[k].reg == l->reg &&
                !settle(c, k))
                return false;
        }
    }
    l->born = e.born;
    l->stale = false;
    if (!write_reg(c, l->reg))
        return false;
    return l->drec != NONE || debug_local(c, l, f->code.n); // a new local holds its value from here
}

// record.field : reads the field of the row on top of the stack, or (after `to`) stores the
// value below the row into it. The current token is the path.
static bool field_access(comp *c, bool write) {
    fn *f = c->f;
    const name path = tok_name(c);
    uint32_t dot = 0;
    while (path.text[dot] != '.')
        ++dot;
    const uint32_t r = find_record_any(c, (name){path.text, dot});
    if (r == NONE)
        return error(c, PINAX_ERR_NOT_FOUND, "this path names neither a module in use nor a record");
    const field *fd = find_field(c, r, (name){path.text + dot + 1, path.length - dot - 1});
    if (!fd)
        return error(c, PINAX_ERR_NOT_FOUND, "the record has no field of this name");
    c->sink = write;
    if (!need(c, write ? 2 : 1))
        return false;
    const uint32_t top = f->depth - 1;
    const tid      row = f->stack[top].type;
    if (f->stack[top].owned)
        return type_error(c, "an owned row is kept in a local before its fields are used");
    if (!is_row(row) || ((const globalsym *)c->globals.ptr + (row - TY_FIRST_DECLARED))->record != r)
        return type_error(c, "a field is reached through a row of a table of that record: row record.field");
    const uint32_t table = row - TY_FIRST_DECLARED;
    const uint32_t where = PNX_FIELD(table, fd->offset, fd->extra);
    uint32_t rr, rv;
    if (write) {
        if (f->stack[top - 1].type != fd->type)
            return type_error(c, "the value has another type than the field");
        if (!reg_of(c, top - 1, &rv) || !reg_of(c, top, &rr) ||
            emit(c, PNX_ABC(fd->is_str ? PNX_FSETS : PNX_FSET, rv, rr, 0)) == NONE || emit(c, where) == NONE)
            return false;
        (void)give(c, top - 1); // storing into a field hands an owned value over
        f->depth -= 2;
        return true;
    }
    if (!reg_of(c, top, &rr))
        return false;
    --f->depth;
    if (!push_slot(c, fd->type))
        return false;
    const uint32_t at = emit_def(c, PNX_ABC(fd->is_str ? PNX_FGETS : PNX_FGET, slot_of(c, f->depth - 1), rr, 0),
                                 bank_of(fd->type));
    return at != NONE && emit(c, where) != NONE;
}

// to name | to ( a , b , c )
static bool store(comp *c) {
    if (!advance(c))
        return false;
    if (c->tok.kind == PNX_T_NAME) {
        const name n = tok_name(c);
        return store_local(c, n) && advance(c);
    }
    if (c->tok.kind == PNX_T_LPAREN) {
        name     names[MAX_PARAMS];
        uint32_t count = 0;
        if (!advance(c))
            return false;
        for (;;) {
            if (c->tok.kind != PNX_T_NAME || count == MAX_PARAMS)
                return syntax(c, "a list of names is expected: to (a, b)");
            names[count++] = tok_name(c);
            if (!advance(c))
                return false;
            if (c->tok.kind == PNX_T_RPAREN)
                break;
            if (c->tok.kind != PNX_T_COMMA)
                return syntax(c, "names are separated by commas");
            if (!advance(c))
                return false;
        }
        for (uint32_t k = count; k-- > 0;) { // the rightmost name takes the top value
            if (!store_local(c, names[k]))
                return false;
        }
        return advance(c);
    }
    if (c->tok.kind == PNX_T_PATH) {
        const name path = tok_name(c);
        uint32_t dot = 0;
        while (path.text[dot] != '.')
            ++dot;
        name real;
        const uint32_t m = module_of(c, (name){path.text, dot}, &real) ? source_module(c, real) : NONE;
        if (m != NONE)
            return type_error(c, "a global of a module is written with set: value set module.name");
        return field_access(c, true) && advance(c);
    }
    return syntax(c, "to is followed by a name or a list of names");
}

// set NAME | set module.NAME : stores into a declared global (D0042); `to` stores into locals
static bool set_global(comp *c) {
    if (!advance(c))
        return false;
    if (c->tok.kind == PNX_T_NAME) {
        const name n = tok_name(c);
        const uint32_t g = find_global(c, n);
        if (g == NONE)
            return error(c, find_local(c->f, n) ? PINAX_ERR_TYPE : PINAX_ERR_NOT_FOUND,
                         find_local(c->f, n) ? "set writes a global; a local is written with to"
                                             : "set writes a declared global; there is none of this name");
        c->path_global = g;
        const bool ok = store_local(c, n);
        c->path_global = NONE;
        return ok && advance(c);
    }
    if (c->tok.kind == PNX_T_PATH) {
        const name path = tok_name(c);
        uint32_t dot = 0;
        while (path.text[dot] != '.')
            ++dot;
        name real;
        const uint32_t m = module_of(c, (name){path.text, dot}, &real) ? source_module(c, real) : NONE;
        if (m == NONE)
            return error(c, PINAX_ERR_NOT_FOUND, "set takes a global, or a global a module in use exports");
        const name n = {path.text + dot + 1, path.length - dot - 1};
        const globalsym *all = c->globals.ptr;
        for (uint32_t k = 0; k < c->base.globals; ++k) {
            if (all[k].module == m && all[k].exported && name_eq(all[k].name, n)) {
                c->path_global = k;
                const bool ok = store_local(c, n);
                c->path_global = NONE;
                return ok && advance(c);
            }
        }
        return error(c, PINAX_ERR_NOT_FOUND, "the module exports no global of this name");
    }
    return syntax(c, "set is followed by the name of a global");
}

// ---- control ------------------------------------------------------------------------------

static void snapshot(const fn *f, shot *out) {
    out->hidden = out->owned = 0;
    for (uint32_t k = 0; k < f->depth; ++k) {
        out->types[k] = f->stack[k].type;
        out->born[k] = f->stack[k].born;
        if (f->stack[k].hidden)
            out->hidden |= (uint64_t)1 << k;
        if (f->stack[k].owned)
            out->owned |= (uint64_t)1 << k;
    }
    out->depth = (uint8_t)f->depth;
}

static void restore(fn *f, const shot *from) {
    for (uint32_t k = 0; k < from->depth; ++k)
        f->stack[k] = (entry){.type = from->types[k], .kind = E_SLOT, .hidden = (from->hidden >> k & 1u) != 0,
                              .owned = (from->owned >> k & 1u) != 0, .born = from->born[k]};
    f->depth = from->depth;
}

// Where two paths meet, a value counts as made as late as on either path, and a mark as
// early: what a release then refuses covers both.
static uint8_t merged_born(tid type, uint8_t a, uint8_t b) {
    return type == TY_MARK ? (a < b ? a : b) : (a > b ? a : b);
}

// The stacks of two paths agree; when they do, the two are merged into both.
static bool same_stack(fn *f, shot *as) {
    if (f->depth != as->depth)
        return false;
    for (uint32_t k = 0; k < as->depth; ++k) {
        if (f->stack[k].type != as->types[k] || f->stack[k].owned != ((as->owned >> k & 1u) != 0))
            return false;
    }
    for (uint32_t k = 0; k < as->depth; ++k)
        f->stack[k].born = as->born[k] = merged_born(as->types[k], f->stack[k].born, as->born[k]);
    return true;
}

// The state of the locals (what they own) is part of what must agree where paths meet.
static void save_owns(const fn *f, lstate *out) {
    for (uint32_t k = 0; k < f->n_locals; ++k)
        out[k] = (lstate){f->locals[k].state, f->locals[k].born, f->locals[k].stale};
}

static void restore_owns(fn *f, const lstate *from, uint32_t count) {
    for (uint32_t k = 0; k < count && k < f->n_locals; ++k) {
        f->locals[k].state = from[k].state;
        f->locals[k].born = from[k].born;
        f->locals[k].stale = from[k].stale;
    }
}

// The locals that existed when block `b` began hold what `as` says, and those made inside it
// hold nothing owned any more. When they agree, when their values were made is merged into both.
static bool same_owns(comp *c, const block *b, lstate *as) {
    fn *f = c->f;
    for (uint32_t k = as ? 0 : b->locals_mark; k < f->n_locals; ++k) {
        if (k < b->locals_mark ? f->locals[k].state != as[k].state : f->locals[k].state == L_OWNS)
            return type_error(c, k < b->locals_mark
                                     ? "an owned value is given on one path and kept on another; every path must do the same"
                                     : "an owned value made in this block is not given before the block ends");
    }
    for (uint32_t k = 0; as && k < b->locals_mark && k < f->n_locals; ++k) {
        local *l = &f->locals[k];
        l->born = as[k].born = merged_born(l->type, l->born, as[k].born);
        l->stale = as[k].stale = l->stale || as[k].stale;
    }
    return true;
}

// The end of one turn of a loop that has a release inside it: a str kept from turn to turn
// must not be made anew in the turn, since the next turn's release was checked against the
// value from before the loop. The head's state is in `out` and `owns_out`, which a loop does
// not use otherwise (`in` has taken in the states of the exits by now).
static bool same_turn(comp *c, const block *b) {
    const fn *f = c->f;
    if (f->releases == b->releases)
        return true;
    for (uint32_t k = 0; k < b->out.depth; ++k) {
        if (bank_of(b->out.types[k]) == BANK_S && f->stack[k].born != b->out.born[k])
            return type_error(c, "a str kept from turn to turn of this loop is made anew after a release in it; "
                                 "the next turn could read bytes the release gave back");
    }
    for (uint32_t k = 0; k < b->locals_mark && k < f->n_locals; ++k) {
        const local *l = &f->locals[k];
        if (bank_of(l->type) == BANK_S && (l->born != b->owns_out[k].born || l->stale != b->owns_out[k].stale))
            return type_error(c, "a str local kept from turn to turn of this loop is made anew after a release in "
                                 "it; the next turn could read bytes the release gave back");
    }
    return true;
}

// A release, or a mark given to a word that may release it: the str values made after mark
// number `serial` are gone. One still on the stack is refused; a local holding one is stale.
static bool released(comp *c, uint8_t serial) {
    fn *f = c->f;
    ++f->releases;
    for (uint32_t k = 0; k < f->depth; ++k) {
        if (bank_of(f->stack[k].type) == BANK_S && f->stack[k].born >= serial)
            return type_error(c, "a str made after this mark is still on the stack; the release gives its bytes back");
    }
    for (uint32_t k = 0; k < f->n_locals; ++k) {
        if (bank_of(f->locals[k].type) == BANK_S && f->locals[k].born >= serial)
            f->locals[k].stale = true;
    }
    return true;
}

static block *open_block(comp *c, uint8_t kind) {
    fn *f = c->f;
    if (f->n_blocks == MAX_BLOCKS) {
        syntax(c, "blocks are nested too deeply");
        return NULL;
    }
    block *b = &f->blocks[f->n_blocks++];
    __builtin_memset(b, 0, sizeof *b);
    b->kind = kind;
    b->exits = b->else_jump = b->next_test = NONE;
    b->locals_mark = f->n_locals;
    b->nloc_mark[0] = f->nloc[0];
    b->nloc_mark[1] = f->nloc[1];
    snapshot(f, &b->in);
    save_owns(f, b->owns_in);
    b->releases = f->releases;
    if (kind == B_LOOP || kind == B_DO) { // the head as it was: `in` takes in what leaves the loop
        b->out = b->in;
        __builtin_memcpy(b->owns_out, b->owns_in, sizeof b->owns_out);
    }
    return b;
}

// Locals made inside a block end with it.
static void close_scope(fn *f, const block *b) {
    while (f->n_defers && f->defers[f->n_defers - 1].block > (uint32_t)(b - f->blocks))
        --f->n_defers;
    for (uint32_t k = b->locals_mark; k < f->n_locals; ++k) {
        if (f->locals[k].drec != NONE)
            ((dlocal *)f->dlocals.ptr)[f->locals[k].drec].to = f->code.n;
    }
    f->n_locals = b->locals_mark;
    f->nloc[0] = b->nloc_mark[0];
    f->nloc[1] = b->nloc_mark[1];
}

// The condition on top of the stack decides: when it is true the next instruction, a far jump
// to `chain`, is skipped. With `negate` the jump is taken when the condition is true.
static bool branch_unless(comp *c, bool negate, uint32_t *chain) {
    entry e;
    if (!pop_cond(c, &e))
        return false;
    if (negate)
        cond_negate(&e);
    // the condition was read from registers at and above its stack position, which stay
    // untouched while the values below are put into theirs
    return settle_all(c) && cond_jump(c, &e, 1) && jump_later(c, chain);
}

static bool do_if(comp *c) {
    uint32_t chain = NONE;
    if (!branch_unless(c, false, &chain))
        return false;
    block *b = open_block(c, B_IF);
    if (!b)
        return false;
    b->else_jump = chain;
    return advance(c);
}

static bool do_else(comp *c) {
    fn *f = c->f;
    block *b = f->n_blocks ? &f->blocks[f->n_blocks - 1] : NULL;
    if (!b || b->kind != B_IF || b->has_else)
        return syntax(c, "else without an open if");
    if (!f->dead) {
        if (!settle_all(c))
            return false;
        snapshot(f, &b->out);
        save_owns(f, b->owns_out);
        for (uint32_t k = b->locals_mark; k < f->n_locals; ++k) {
            if (f->locals[k].state == L_OWNS)
                return type_error(c, "an owned value made in this block is not given before the block ends");
        }
        b->has_out = true;
        if (!jump_later(c, &b->exits))
            return false;
    }
    land(c, &b->else_jump);
    close_scope(f, b);
    restore(f, &b->in);
    restore_owns(f, b->owns_in, b->locals_mark);
    f->dead = false;
    b->has_else = true;
    return advance(c);
}

static bool end_if(comp *c, block *b) {
    fn *f = c->f;
    if (!f->dead) {
        if (!settle_all(c))
            return false;
        if (b->has_out ? !same_stack(f, &b->out)
                       : !b->has_else && !same_stack(f, &b->in))
            return type_error(c, b->has_else ? "the two branches of this if leave different stacks"
                                             : "an if without else must leave the stack as it found it");
        if (b->has_out ? !same_owns(c, b, b->owns_out) : !b->has_else ? !same_owns(c, b, b->owns_in)
                                                                       : !same_owns(c, b, NULL))
            return false;
    } else if (b->has_out) {
        restore(f, &b->out);
        restore_owns(f, b->owns_out, b->locals_mark);
        f->dead = false;
    } else if (!b->has_else) {
        restore(f, &b->in);
        restore_owns(f, b->owns_in, b->locals_mark);
        f->dead = false;
    }
    land(c, &b->else_jump);
    land(c, &b->exits);
    close_scope(f, b);
    --f->n_blocks;
    return true;
}

// loop [ ( name ) ]
static bool do_loop(comp *c) {
    fn *f = c->f;
    name n = {0};
    if (!advance(c))
        return false;
    if (c->tok.kind == PNX_T_LPAREN) {
        if (!advance(c))
            return false;
        if (c->tok.kind != PNX_T_NAME)
            return syntax(c, "a loop name is expected: loop (name)");
        n = tok_name(c);
        if (!name_is_free(c, n) || !advance(c))
            return false;
        if (c->tok.kind != PNX_T_RPAREN)
            return syntax(c, "a loop name is one name in parentheses");
        if (!advance(c))
            return false;
    }
    if (!settle_all(c))
        return false;
    block *b = open_block(c, B_LOOP);
    if (!b)
        return false;
    b->name = n;
    b->head = f->code.n;
    f->def_at = NONE;
    return true;
}

// The loop that while, until or break leaves: the innermost one, or the one named in
// parentheses after the word. The current token is the word itself.
static bool leave_target(comp *c, block **out) {
    fn *f = c->f;
    const pnx_token at = c->tok;
    if (!advance(c))
        return false;
    name n = {0};
    if (c->tok.kind == PNX_T_LPAREN) {
        if (!advance(c))
            return false;
        if (c->tok.kind != PNX_T_NAME)
            return syntax(c, "a loop name is expected in the parentheses");
        n = tok_name(c);
        if (!advance(c))
            return false;
        if (c->tok.kind != PNX_T_RPAREN)
            return syntax(c, "a loop name is one name in parentheses");
        if (!advance(c))
            return false;
    }
    for (uint32_t k = f->n_blocks; k-- > 0;) {
        block *b = &f->blocks[k];
        if (b->kind == B_DO)
            return error_at(c, PINAX_ERR_SYNTAX, at.line, at.column,
                            "a loop is not left from inside a do block; the block is its own loop");
        if (b->kind == B_LOOP && (!n.length || (b->name.length && name_eq(b->name, n)))) {
            *out = b;
            return true;
        }
    }
    return error_at(c, PINAX_ERR_SYNTAX, at.line, at.column,
                    n.length ? "no loop of this name encloses this place" : "while, until and break belong inside a loop");
}

static bool do_leave(comp *c, pnx_word_id which) {
    fn *f = c->f;
    const pnx_token at = c->tok;
    // The condition must be taken before the loop name is read, but the name decides which
    // stack it is checked against; so the condition is popped first and kept.
    entry cond = {0};
    if (which != PNX_KW_break && !pop_cond(c, &cond))
        return false;
    block *target;
    if (!leave_target(c, &target))
        return false;
    if (which == PNX_KW_while)
        cond_negate(&cond); // leave when the condition is false
    if (!settle_all(c))
        return false;
    if (!same_stack(f, &target->in))
        return error_at(c, PINAX_ERR_TYPE, at.line, at.column,
                        "where a loop is left the stack must be as it was when the loop began");
    target->targeted = true;
    const uint32_t from = (uint32_t)(target - f->blocks) + 1;
    if (defers_from(f, from)) { // the defers inside the loop run on the way out
        lstate states[MAX_LOCALS];
        save_owns(f, states);
        uint32_t stub = NONE, stay = NONE;
        if (which != PNX_KW_break) { // stay: skip the jump out
            cond_negate(&cond);
            if (!cond_jump(c, &cond, 1) || !jump_later(c, &stub) || !jump_later(c, &stay))
                return false;
            land(c, &stub);
        }
        if (!emit_defers(c, from) || !same_owns(c, target, target->owns_in) || !jump_later(c, &target->exits))
            return false;
        restore_owns(f, states, f->n_locals);
        if (which == PNX_KW_break) {
            f->dead = true;
            return true;
        }
        land(c, &stay);
        return true;
    }
    if (!same_owns(c, target, target->owns_in))
        return false;
    if (which == PNX_KW_break) {
        f->dead = true;
        return jump_later(c, &target->exits);
    }
    // skip the far jump unless the loop is to be left
    cond_negate(&cond);
    return cond_jump(c, &cond, 1) && jump_later(c, &target->exits);
}

static bool end_loop(comp *c, block *b) {
    fn *f = c->f;
    if (!f->dead && !emit_defers(c, (uint32_t)(b - f->blocks) + 1)) // the end of one turn leaves the body
        return false;
    if (!f->dead) {
        if (!settle_all(c))
            return false;
        if (f->depth == b->out.depth && !same_turn(c, b))
            return false;
        if (!same_stack(f, &b->in))
            return type_error(c, "one turn of a loop must leave the stack as it found it");
        if (!same_owns(c, b, b->owns_in))
            return false;
        uint32_t n = f->code.n - b->head + 2;
        if (n > 0xffffu)
            n = 0xffffu;
        if (emit(c, PNX_ABX(PNX_CHECK, 0, n)) == NONE)
            return false;
        f->last_check = f->code.n;
        const uint32_t at = emit(c, 0);
        if (at == NONE)
            return false;
        *code_at(f, at) = PNX_AX(PNX_JMP, (uint32_t)((int32_t)b->head - (int32_t)(at + 1)));
    }
    land(c, &b->exits);
    close_scope(f, b);
    restore(f, &b->in);
    restore_owns(f, b->owns_in, b->locals_mark);
    f->dead = !b->targeted; // a loop nothing leaves never ends (D0022)
    --f->n_blocks;
    return true;
}

// Moves the results into the first registers of the window and returns. The results are the
// values the code can reach; operands of running blocks lie between them and do not count.
static bool emit_return(comp *c) {
    fn *f = c->f;
    if (f->n_defers && !emit_defers(c, 0)) // every block is left
        return false;
    const sig *s = f->sig;
    uint32_t at[MAX_PARAMS], n = 0, count[2] = {0, 0};
    for (uint32_t k = 0; k < f->depth; ++k) {
        if (f->stack[k].hidden)
            continue;
        if (n == s->n_out)
            return type_error(c, "the stack does not hold exactly the results of the signature here");
        if (f->stack[k].type != s->out[n])
            return type_error(c, "a result has another type than the signature says");
        if (s->out_owned[n] ? !give(c, k) : f->stack[k].owned)
            return type_error(c, s->out_owned[n] ? "this result is owned; the value returned is not"
                                                 : "an owned value is returned through a result marked owned");
        ++count[bank_of(s->out[n])];
        at[n++] = k;
    }
    if (n != s->n_out)
        return type_error(c, "the stack does not hold exactly the results of the signature here");
    if (!nothing_owned(c))
        return false;
    // a call in tail position: nothing was emitted since, and its results are the results
    bool tail = f->tail_at != NONE && f->tail_at + 2 == f->code.n && f->tail_depth == f->depth;
    for (uint32_t k = 0; tail && k < n; ++k)
        tail = at[k] == f->depth - n + k && f->stack[at[k]].kind == E_SLOT;
    if (tail) {
        f->lines.n = f->tail_at;
        f->code.n = f->tail_at; // the call is taken back; its arguments move to the front of the window
        for (uint32_t k = 0; k < f->tail_in[0]; ++k) {
            if (emit(c, PNX_ABC(PNX_MOVI, k, f->tail_base[0] + k, 0)) == NONE)
                return false;
        }
        for (uint32_t k = 0; k < f->tail_in[1]; ++k) {
            if (emit(c, PNX_ABC(PNX_MOVS, k, f->tail_base[1] + k, 0)) == NONE)
                return false;
        }
        const uint32_t word = f->tail_word;
        f->tail_at = NONE;
        return emit(c, PNX_ABC(PNX_TCALL, 0, 0, 0)) != NONE && emit(c, word) != NONE;
    }
    // One result of a bank goes straight from where it is. Several are first all put into
    // their stack registers, which lie above the targets, and then moved down in rising order.
    for (uint32_t k = 0; k < n; ++k) {
        if (count[bank_of(f->stack[at[k]].type)] > 1 && !settle(c, at[k]))
            return false;
    }
    uint32_t pos[2] = {0, 0};
    for (uint32_t k = 0; k < n; ++k) {
        entry    *e = &f->stack[at[k]];
        const uint32_t target = pos[bank_of(e->type)]++;
        if (e->kind == E_LOCAL) {
            if (e->reg != target && !move_slot(c, e->type, target, e->reg))
                return false;
        } else if (e->kind == E_CONST) {
            if (!load_int(c, target, e->value))
                return false;
        } else {
            if (!settle(c, at[k]) || !move_slot(c, e->type, target, slot_of(c, at[k])))
                return false;
        }
    }
    f->def_at = NONE;
    return emit(c, PNX_ABC(s->fails ? PNX_RETOK : PNX_RET, 0, 0, 0)) != NONE;
}

static bool end_orelse(comp *c, block *b) {
    fn *f = c->f;
    if (!f->dead) {
        if (!settle_all(c))
            return false;
        if (!same_stack(f, &b->out))
            return type_error(c, "the body of or_else must leave what the word would have given, or leave");
        if (!same_owns(c, b, b->owns_in))
            return false;
    }
    restore(f, &b->out);
    restore_owns(f, b->owns_in, b->locals_mark);
    f->dead = false;
    land(c, &b->exits);
    close_scope(f, b);
    --f->n_blocks;
    return true;
}

// ---- case ---------------------------------------------------------------------------------

static bool add_string(comp *c, const pnx_token *t, uint32_t *index);

// value case  v of ... end  ( v , v ) of ... end  otherwise ... end  end
static bool do_case(comp *c) {
    fn *f = c->f;
    if (!need(c, 1))
        return false;
    const tid type = f->stack[f->depth - 1].type;
    if (type != TY_INT && type != TY_STR && type != TY_ERR)
        return type_error(c, "case compares an int, an err or a str");
    if (!settle(c, f->depth - 1))
        return false;
    const uint32_t subject = slot_of(c, f->depth - 1);
    --f->depth;
    if (!settle_all(c))
        return false;
    block *b = open_block(c, B_CASE);
    if (!b)
        return false;
    b->subject = (uint8_t)subject;
    b->subject_type = type;
    return advance(c);
}

static bool err_constant(name n, int64_t *value);

// One value of a case branch; the current token is the value.
static bool case_value(comp *c, block *b, int64_t *value) {
    tid type = TY_NONE;
    const name n = tok_name(c);
    switch (c->tok.kind) {
    case PNX_T_INT:
    case PNX_T_CHAR:
        type = TY_INT;
        *value = c->tok.integer;
        break;
    case PNX_T_STRING:
    case PNX_T_RAW:
    case PNX_T_TEXT: {
        uint32_t index;
        if (!add_string(c, &c->tok, &index))
            return false;
        type = TY_STR;
        *value = index;
        break;
    }
    case PNX_T_NAME: {
        const constant *k = find_const(c, n);
        if (!k)
            return type_error(c, "a case value is a literal or the name of a const");
        type = k->type;
        *value = k->value;
        break;
    }
    case PNX_T_PATH:
        if (n.length > 4 && __builtin_memcmp(n.text, "err.", 4) == 0 &&
            err_constant((name){n.text + 4, n.length - 4}, value)) {
            type = TY_ERR;
            break;
        }
        return type_error(c, "a case value is a literal or the name of a const");
    default: return syntax(c, "a case value is expected here");
    }
    if (type != b->subject_type)
        return type_error(c, "a case value has another type than the value compared");
    for (uint32_t k = 0; k < b->n_values; ++k) {
        bool same = b->values[k] == *value;
        if (type == TY_STR && !same) {
            const pnx_sconst *all = c->sconst.ptr;
            const pnx_sconst x = all[b->values[k]], y = all[*value];
            same = x.length == y.length &&
                   __builtin_memcmp((const uint8_t *)c->strbytes.ptr + x.offset,
                                    (const uint8_t *)c->strbytes.ptr + y.offset, x.length) == 0;
        }
        if (same)
            return type_error(c, "this value is already in an earlier branch of the case");
    }
    if (b->n_values == MAX_CASE)
        return syntax(c, "a case has at most 32 values");
    b->values[b->n_values++] = *value;
    return advance(c);
}

// Emits the test of one value. The last test of a branch jumps to the next branch when it
// does not match; the earlier ones jump into the body when they do.
static bool case_test(comp *c, block *b, int64_t value, bool last, uint32_t *body) {
    const uint32_t subject = b->subject;
    if (b->subject_type == TY_STR) {
        const uint32_t scratch = top_slot(c, BANK_S, 2) + 1; // the subject's own register is the top one
        if (emit(c, PNX_ABX(PNX_LOADKS, scratch, (uint32_t)value)) == NONE ||
            emit(c, PNX_ABC(last ? PNX_JSEQ : PNX_JSNE, subject, scratch, 1)) == NONE)
            return false;
    } else if (fits8(value)) {
        if (emit(c, PNX_ABC(last ? PNX_JEQI : PNX_JNEI, subject, (uint8_t)(int8_t)value, 1)) == NONE)
            return false;
    } else {
        const uint32_t scratch = top_slot(c, BANK_I, 2) + 1;
        if (!load_int(c, scratch, value) || emit(c, PNX_ABC(last ? PNX_JEQ : PNX_JNE, subject, scratch, 1)) == NONE)
            return false;
    }
    return jump_later(c, last ? &b->next_test : body);
}

// A token directly inside case ... end: the values of a branch, otherwise, or the end.
static bool case_item(comp *c) {
    fn *f = c->f;
    block *b = &f->blocks[f->n_blocks - 1];
    if (is_word(c, PNX_KW_end)) {
        land(c, &b->next_test);
        if (!b->has_otherwise && emit(c, PNX_ABX(PNX_FAULT, 0, PINAX_ERR_RANGE)) == NONE)
            return false; // no branch matched: a fault (RFC-0008 section 5.7)
        if (b->has_out) {
            restore(f, &b->out);
            restore_owns(f, b->owns_out, b->locals_mark);
            f->dead = false;
        } else {
            f->dead = true;
        }
        land(c, &b->exits);
        --f->n_blocks;
        return advance(c);
    }
    if (b->has_otherwise)
        return syntax(c, "otherwise is the last branch of a case");
    if (is_word(c, PNX_KW_otherwise)) {
        land(c, &b->next_test);
        b->has_otherwise = true;
        return open_block(c, B_OF) != NULL && advance(c);
    }
    int64_t  values[MAX_CASE];
    uint32_t count = 0;
    if (c->tok.kind == PNX_T_LPAREN) {
        if (!advance(c))
            return false;
        for (;;) {
            if (count == MAX_CASE)
                return syntax(c, "a case has at most 32 values");
            if (!case_value(c, b, &values[count++]))
                return false;
            if (c->tok.kind == PNX_T_RPAREN)
                break;
            if (c->tok.kind != PNX_T_COMMA)
                return syntax(c, "case values are separated by commas");
            if (!advance(c))
                return false;
        }
        if (!advance(c))
            return false;
    } else if (!case_value(c, b, &values[count++])) {
        return false;
    }
    if (!is_word(c, PNX_KW_of))
        return syntax(c, "the values of a case branch are followed by of");
    land(c, &b->next_test);
    uint32_t body = NONE;
    for (uint32_t k = 0; k < count; ++k) {
        if (!case_test(c, b, values[k], k + 1 == count, &body))
            return false;
    }
    land(c, &body);
    return open_block(c, B_OF) != NULL && advance(c);
}

static bool end_of(comp *c, block *b) {
    fn *f = c->f;
    block *parent = b - 1;
    if (!f->dead) {
        if (!settle_all(c))
            return false;
        if (parent->has_out) {
            if (!same_stack(f, &parent->out))
                return type_error(c, "the branches of this case leave different stacks");
            if (!same_owns(c, b, parent->owns_out))
                return false;
        } else {
            if (!same_owns(c, b, NULL))
                return false;
            snapshot(f, &parent->out);
            save_owns(f, parent->owns_out);
            parent->has_out = true;
        }
        if (!jump_later(c, &parent->exits))
            return false;
    }
    close_scope(f, b);
    --f->n_blocks;
    restore(f, &parent->in);
    restore_owns(f, parent->owns_in, parent->locals_mark);
    f->dead = false;
    return true;
}

// ---- cat ----------------------------------------------------------------------------------

static bool body_token(comp *c);

// cat ( item , item ... ) : every item is code that leaves one value; the values are joined
// into one string in the region.
static bool do_cat(comp *c) {
    fn *f = c->f;
    if (!advance(c))
        return false;
    if (c->tok.kind != PNX_T_LPAREN)
        return syntax(c, "cat is followed by its items in parentheses: cat (a, b)");
    if (!advance(c))
        return false;
    const uint32_t first = f->depth, floor = f->floor, blocks = f->n_blocks;
    for (;;) {
        const uint32_t before = f->depth;
        f->floor = before;
        while (c->tok.kind != PNX_T_COMMA && c->tok.kind != PNX_T_RPAREN) {
            if (c->tok.kind == PNX_T_END)
                return syntax(c, "the items of cat are not closed by )");
            if (!body_token(c))
                return false;
            if (f->n_blocks < blocks)
                return syntax(c, "this end closes something opened outside the cat");
        }
        f->floor = floor;
        if (f->n_blocks != blocks)
            return syntax(c, "something opened inside this item of cat is not closed");
        if (f->dead || f->depth != before + 1)
            return type_error(c, "each item of cat leaves exactly one value");
        const tid type = f->stack[before].type;
        if (type != TY_STR && type != TY_INT && type != TY_BOOL)
            return type_error(c, "cat joins str, int and bool values");
        if (!settle(c, before))
            return false;
        if (c->tok.kind == PNX_T_RPAREN)
            break;
        if (!advance(c))
            return false;
    }
    const uint32_t scratch = top_slot(c, BANK_S, 1);
    if (emit(c, PNX_ABC(PNX_RMARK, scratch, 0, 0)) == NONE)
        return false;
    for (uint32_t k = first; k < f->depth; ++k) {
        const tid type = f->stack[k].type;
        const uint32_t op = type == TY_STR ? PNX_RCATS : type == TY_INT ? PNX_RCATI : PNX_RCATB;
        if (emit(c, PNX_ABC(op, scratch, slot_of(c, k), 0)) == NONE)
            return false;
    }
    f->depth = first;
    if (!push_slot(c, TY_STR) || !move_slot(c, TY_STR, slot_of(c, f->depth - 1), scratch))
        return false;
    return advance(c);
}

// ---- stems and the blocks of combining words ----------------------------------------------

// Finds the word after the end that closes the block opened by the current `do`.
static bool value_block(comp *c, const sig *bs, uint32_t line, uint32_t column);

static bool scan_user(comp *c, name *out, name *dst) {
    *dst = (name){0};
    pnx_lexer lx = *c->lx;
    pnx_token t;
    for (uint32_t depth = 1; depth;) {
        if (!pnx_lex_next(&lx, &t))
            return error_at(c, PINAX_ERR_SYNTAX, lx.error_line, lx.error_column, lx.error);
        if (t.kind == PNX_T_END) {
            c->unfinished = true;
            return syntax(c, "this block is not closed by end");
        }
        if (t.kind != PNX_T_RESERVED)
            continue;
        switch (t.word) {
        case PNX_KW_do: case PNX_KW_if: case PNX_KW_loop: case PNX_KW_case:
        case PNX_KW_of: case PNX_KW_otherwise: case PNX_KW_or_else: case PNX_KW_defer: ++depth; break;
        case PNX_KW_end: --depth; break;
        default: break;
        }
    }
    if (!pnx_lex_next(&lx, &t))
        return error_at(c, PINAX_ERR_SYNTAX, lx.error_line, lx.error_column, lx.error);
    // anything but a combining word after the end makes the block a value
    *out = t.kind == PNX_T_NAME ? (name){lx.source + t.start, t.length} : (name){0};
    if (t.kind == PNX_T_PATH) { // rex.with_match and rex.replace_by take a block too
        const name path = {lx.source + t.start, t.length};
        if (ends_with_word(path, "with_match") || ends_with_word(path, "replace_by"))
            *out = path;
        return true;
    }
    if (t.kind == PNX_T_NAME && pnx_lex_next(&lx, &t) && t.kind == PNX_T_NAME) {
        // ... end names map_into : the list to fill stands between the block and its word
        const name next = {lx.source + t.start, t.length};
        if (name_is(next, "map_into") || name_is(next, "filter_into")) {
            *dst = *out;
            *out = next;
        }
    }
    return true;
}

// Turns the stem on top of the stack into the registers of a loop: `cur` runs up to `lim`.
// The operands stay on the stack, marked as belonging to the block. Returns their count.
static bool guard_op(comp *c, tid out);
static bool found_or_fail(comp *c, tid type, uint32_t value, uint32_t flag);

static bool stem_begin(comp *c, uint8_t *kind, uint8_t *cur, uint8_t *lim, uint16_t *src, uint8_t *count,
                       tid *element) {
    fn *f = c->f;
    const entry stem = f->stack[--f->depth];
    *kind = stem.op;
    *src = 0;
    *element = TY_INT;
    if (stem.type == TY_COLL) { // an array or list: an index and the length
        const globalsym *g = (const globalsym *)c->globals.ptr + stem.value;
        if (g->kind != GK_ARRAY && g->kind != GK_LIST)
            return type_error(c, "only an array or a list is a stem (not a queue, a pool, a buffer or an index)");
        if (!push_slot(c, TY_INT) || !push_slot(c, TY_INT))
            return false;
        f->stack[f->depth - 2].hidden = f->stack[f->depth - 1].hidden = true;
        *kind = g->record != NONE ? STEM_ROWS : g->strcap ? STEM_STRS : STEM_COLL;
        *src = (uint16_t)stem.value;
        *cur = (uint8_t)slot_of(c, f->depth - 2);
        *lim = (uint8_t)slot_of(c, f->depth - 1);
        *count = 2;
        *element = g->type;
        return emit(c, PNX_ABX(PNX_LOADI, *cur, 0)) != NONE && emit(c, PNX_ABX(PNX_GLEN, *lim, (uint32_t)stem.value)) != NONE;
    }
    if (stem.op == STEM_WORDS || stem.op == STEM_LINES || stem.op == STEM_SPLIT_BYTE || stem.op == STEM_CHARS ||
        stem.op == STEM_CSV) {
        // the string itself is the rest still to read; for a byte separator, `cur` is the byte
        f->stack[f->depth - 1].hidden = true;
        *src = (uint16_t)slot_of(c, f->depth - 1);
        *cur = (uint8_t)stem.value;
        *lim = 0;
        *count = 1;
        *element = TY_STR;
        return true;
    }
    if (stem.op == STEM_REX || stem.op == STEM_RSPLIT) { // the whole string, its program, and the place reached
        f->stack[f->depth - 2].hidden = f->stack[f->depth - 1].hidden = true;
        *src = (uint8_t)slot_of(c, f->depth - 2);
        *lim = (uint8_t)slot_of(c, f->depth - 1);
        if (!push_slot(c, TY_INT))
            return false;
        f->stack[f->depth - 1].hidden = true;
        *cur = (uint8_t)slot_of(c, f->depth - 1);
        *count = 3;
        *element = TY_STR;
        return emit(c, PNX_ABX(PNX_LOADI, *cur, 0)) != NONE;
    }
    if (stem.op == STEM_SPLIT || stem.op == STEM_PTN) { // the string and, above it, the separator
        f->stack[f->depth - 2].hidden = f->stack[f->depth - 1].hidden = true;
        *src = (uint8_t)slot_of(c, f->depth - 2);
        *lim = (uint8_t)slot_of(c, f->depth - 1);
        *cur = 0;
        *count = 2;
        *element = TY_STR;
        return true;
    }
    if (stem.op == STEM_RANGE) {
        f->stack[f->depth - 2].hidden = f->stack[f->depth - 1].hidden = true;
        *cur = (uint8_t)slot_of(c, f->depth - 2);
        *lim = (uint8_t)slot_of(c, f->depth - 1);
        *count = 2;
        return true;
    }
    // bytes of a string: an index and the length above the string
    f->stack[f->depth - 1].hidden = true;
    *src = (uint16_t)slot_of(c, f->depth - 1);
    if (!push_slot(c, TY_INT) || !push_slot(c, TY_INT))
        return false;
    f->stack[f->depth - 2].hidden = f->stack[f->depth - 1].hidden = true;
    *cur = (uint8_t)slot_of(c, f->depth - 2);
    *lim = (uint8_t)slot_of(c, f->depth - 1);
    *count = 3;
    return emit(c, PNX_ABX(PNX_LOADI, *cur, 0)) != NONE && emit(c, PNX_ABC(PNX_SLEN, *lim, *src, 0)) != NONE;
}

// Stems that hand out pieces of a string until none is left.
static bool stem_takes(uint8_t kind) {
    return kind == STEM_WORDS || kind == STEM_LINES || kind == STEM_SPLIT || kind == STEM_SPLIT_BYTE ||
           kind == STEM_CHARS || kind == STEM_PTN || kind == STEM_REX || kind == STEM_RSPLIT || kind == STEM_CSV;
}

// Takes the next piece into `reg`; the instruction after it runs when there is none.
static bool stem_take(comp *c, uint8_t kind, uint32_t reg, uint32_t src, uint32_t cur, uint32_t lim) {
    if ((kind == STEM_REX || kind == STEM_RSPLIT) &&
        emit(c, PNX_ABC(kind == STEM_REX ? PNX_XNEXT : PNX_XSPLIT, cur, reg, src)) == NONE)
        return false;
    if (kind == STEM_REX || kind == STEM_RSPLIT)
        return emit(c, lim) != NONE;
    return emit(c, kind == STEM_WORDS   ? PNX_ABC(PNX_SWORD, src, reg, 0)
                   : kind == STEM_CHARS ? PNX_ABC(PNX_SCHAR, src, reg, 0)
                   : kind == STEM_CSV   ? PNX_ABC(PNX_CNEXT, src, reg, 0)
                   : kind == STEM_PTN   ? PNX_ABC(PNX_PNEXT, src, reg, lim)
                   : kind == STEM_REX || kind == STEM_RSPLIT
                       ? PNX_ABC(kind == STEM_REX ? PNX_XNEXT : PNX_XSPLIT, cur, reg, src) // the program: next word
                   : kind == STEM_LINES ? PNX_ABC(PNX_SLINE, src, reg, 0)
                   : kind == STEM_SPLIT ? PNX_ABC(PNX_SNEXTS, src, reg, lim)
                                        : PNX_ABC(PNX_SNEXT, src, reg, cur)) != NONE;
}

// Puts the current element of the stem into `reg`.
static bool stem_element(comp *c, uint8_t kind, uint32_t reg, uint32_t cur, uint32_t src) {
    if (kind == STEM_STRS) { // the text of cell cur: its row, then the view
        const uint32_t row = top_slot(c, BANK_I, 1);
        const globalsym *g = (const globalsym *)c->globals.ptr + src;
        return emit(c, PNX_ABC(PNX_ADDI, row, cur, 1)) != NONE && emit(c, PNX_ABC(PNX_FGETS, reg, row, 0)) != NONE &&
               emit(c, PNX_FIELD(src, 0, g->strcap)) != NONE;
    }
    if (kind == STEM_COLL)
        return emit_g(c, PNX_AGET, reg, cur, src) != NONE;
    return emit(c, kind == STEM_RANGE  ? PNX_ABC(PNX_MOVI, reg, cur, 0)
                   : kind == STEM_ROWS ? PNX_ABC(PNX_ADDI, reg, cur, 1) // the row of index cur
                                       : PNX_ABC(PNX_SBYTE, reg, src, cur)) != NONE;
}

static bool loop_back(comp *c, uint32_t head, uint32_t cur, int step) {
    fn *f = c->f;
    if (step && emit(c, PNX_ABC(PNX_ADDI, cur, cur, (uint8_t)(int8_t)step)) == NONE)
        return false;
    uint32_t n = f->code.n - head + 2;
    if (n > 0xffffu)
        n = 0xffffu;
    if (emit(c, PNX_ABX(PNX_CHECK, 0, n)) == NONE)
        return false;
    f->last_check = f->code.n;
    const uint32_t at = emit(c, 0);
    if (at == NONE)
        return false;
    // Every caller begins its loop with a test that skips one word (JLT cur lim, JGEI cur 1),
    // then a jump out to the place right after this one. Instead of jumping back to that test,
    // test here and go on past it: one instruction less a turn (the loop is rotated).
    const uint32_t first = *code_at(f, head), op = first & 0xffu;
    const int32_t  back = (int32_t)(head + 2) - (int32_t)(at + 1);
    if ((op == PNX_JLT || op == PNX_JGEI) && first >> 24 == 1 && back >= INT8_MIN &&
        (*code_at(f, head + 1) & 0xffu) == PNX_JMP)
        *code_at(f, at) = PNX_ABC(op, (first >> 8) & 0xffu, (first >> 16) & 0xffu, (uint8_t)(int8_t)back);
    else
        *code_at(f, at) = PNX_AX(PNX_JMP, (uint32_t)((int32_t)head - (int32_t)(at + 1)));
    return true;
}

static local *new_local(comp *c, name n, tid type) {
    fn *f = c->f;
    const int bank = bank_of(type);
    if (n.length && !name_is_free(c, n))
        return NULL;
    if (f->n_locals == MAX_LOCALS || f->nloc[bank] >= SLOT0 - 1) {
        syntax(c, "too many local variables in this word");
        return NULL;
    }
    local *l = &f->locals[f->n_locals++];
    *l = (local){.name = n, .type = type, .reg = f->nloc[bank]++, .born = f->marks};
    if (f->nloc[bank] > f->maxloc[bank])
        f->maxloc[bank] = f->nloc[bank];
    return debug_local(c, l, f->code.n) ? l : NULL;
}

// s re do ( whole str, group str ... ) body end rex.with_match (or fail): the leftmost match,
// once; s re buf do ( whole str, ... gives str ) body end rex.replace_by (gives n int or fail):
// every match replaced in the buffer by what the block gives (RFC-0011 sections 4.2 and 4.3).
static bool rex_block(comp *c, name user, sig *bs, uint32_t line, uint32_t column) {
    fn *f = c->f;
    const bool by = ends_with_word(user, "replace_by");
    name real;
    const name module = {user.text, (uint32_t)(user.length - sizeof "with_match")}; // both words are 10 bytes
    if (!module_of(c, module, &real) || !name_is(real, "rex"))
        return error_at(c, PINAX_ERR_NOT_FOUND, line, column, "with_match and replace_by are words of rex");
    const globalsym *g = NULL;
    uint32_t         gi = 0;
    if (by && !need_coll(c, &g, &gi))
        return false;
    if (!need(c, 2) || f->stack[f->depth - 2].type != TY_STR || f->stack[f->depth - 1].type != TY_REGEX ||
        (g && g->kind != GK_BUFFER))
        return error_at(c, PINAX_ERR_TYPE, line, column,
                        by ? "replace_by takes a str, a regex and a buffer before its block: s re buf do ... end rex.replace_by"
                           : "with_match takes a str and a regex before its block: s re do ... end rex.with_match");
    if (bs->fails || bs->n_in < 1 || bs->n_in > 1 + PNX_PTN_CAPS || bs->n_out != (by ? 1 : 0) ||
        (by && bs->out[0] != TY_STR))
        return error_at(c, PINAX_ERR_TYPE, line, column,
                        by ? "the block of replace_by is (whole str, group str ... gives str)"
                           : "the block of with_match is (whole str, group str ...) and gives nothing");
    for (uint32_t k = 0; k < bs->n_in; ++k) {
        if (bs->in[k] != TY_STR || (k > 0 && !sig_in_name(bs, k).length))
            return error_at(c, PINAX_ERR_TYPE, line, column, "the whole match and the groups are str parameters, the groups named");
    }
    if (!settle(c, f->depth - 2) || !settle(c, f->depth - 1))
        return false;
    const uint32_t rs = slot_of(c, f->depth - 2), rre = slot_of(c, f->depth - 1);
    f->stack[f->depth - 2].hidden = f->stack[f->depth - 1].hidden = true;
    uint8_t hidden = 2;
    // the flag that says the word did its work; for replace_by the count, the place reached and
    // the text between
    if (!push_slot(c, TY_INT) || (by && (!push_slot(c, TY_INT) || !push_slot(c, TY_INT) || !push_slot(c, TY_STR))))
        return false;
    hidden += by ? 4 : 1;
    for (uint32_t k = 1; k <= (by ? 4u : 1u); ++k)
        f->stack[f->depth - k].hidden = true;
    // the count lowest: it moves down to where the result goes, under the flag
    const uint32_t count = by ? slot_of(c, f->depth - 4) : 0, place = by ? slot_of(c, f->depth - 3) : 0,
                   ok = slot_of(c, f->depth - (by ? 2 : 1)), between = by ? slot_of(c, f->depth - 1) : 0;
    if (emit(c, PNX_ABX(PNX_LOADI, ok, by ? 1 : 0)) == NONE ||
        (by && (emit(c, PNX_ABX(PNX_LOADI, count, 0)) == NONE || emit(c, PNX_ABX(PNX_LOADI, place, 0)) == NONE)) ||
        !settle_all(c))
        return false;
    block *b = open_block(c, B_DO);
    if (!b)
        return false;
    b->use = by ? USE_BY : USE_WITH;
    b->n_hidden = hidden;
    b->user = user;
    b->acc = (uint8_t)ok;
    b->cur = (uint8_t)count;
    b->val = (uint8_t)between;
    b->lim = (uint8_t)rre;
    b->stem = (uint8_t)place; // replace_by: the place reached
    b->idx = (uint8_t)rs;
    b->dst = gi;
    b->full = NONE;
    b->floor_mark = f->floor;
    b->elem_local = NONE;
    f->floor = f->depth;
    b->head = f->code.n;
    f->def_at = NONE;
    uint32_t reg;
    if (sig_in_name(bs, 0).length) {
        const local *l = new_local(c, sig_in_name(bs, 0), TY_STR);
        if (!l)
            return false;
        reg = l->reg;
    } else {
        if (!push_slot(c, TY_STR))
            return false;
        reg = slot_of(c, f->depth - 1);
    }
    if (by) { // the text before the match into the buffer; none left: to the end
        if (emit(c, PNX_ABC(PNX_XSTEP, place, between, rs)) == NONE || emit(c, rre) == NONE || !jump_later(c, &b->exits) ||
            emit(c, PNX_ABX(PNX_BCAT, between, gi)) == NONE || !jump_later(c, &b->full) ||
            emit(c, PNX_ABC(PNX_XCAPI, reg, 0, 0)) == NONE)
            return false;
    } else if (emit(c, PNX_ABC(PNX_XMATCH, reg, rs, rre)) == NONE || !jump_later(c, &b->exits) ||
               emit(c, PNX_ABX(PNX_LOADI, ok, 1)) == NONE) {
        return false;
    }
    for (uint32_t k = 1; k < bs->n_in; ++k) {
        const local *l = new_local(c, sig_in_name(bs, k), TY_STR);
        if (!l || emit(c, PNX_ABC(PNX_XCAPI, l->reg, 0, k)) == NONE)
            return false;
    }
    for (uint32_t k = b->locals_mark; k < f->n_locals; ++k) { // they hold their values from here
        if (f->locals[k].drec != NONE)
            ((dlocal *)f->dlocals.ptr)[f->locals[k].drec].from = f->code.n;
    }
    f->def_at = NONE;
    return true; // the current token is the first of the body
}

static bool end_rex_block(comp *c, block *b) {
    fn *f = c->f;
    const bool by = b->use == USE_BY;
    if (!f->dead) {
        uint32_t made = 0;
        if (by) { // the text the block made
            if (f->depth != (uint32_t)b->in.depth + 1 || f->stack[f->depth - 1].type != TY_STR)
                return type_error(c, "the block of replace_by leaves one str: what the match becomes");
            if (!settle(c, f->depth - 1))
                return false;
            made = slot_of(c, f->depth - 1);
            --f->depth;
        }
        if (!settle_all(c))
            return false;
        if (f->depth == b->out.depth && !same_turn(c, b))
            return false;
        if (!same_stack(f, &b->in))
            return type_error(c, "this block must leave the stack as it found it");
        if (!same_owns(c, b, b->owns_in))
            return false;
        if (by) { // the new text, the code point kept after an empty match, once more
            const uint32_t kept = top_slot(c, BANK_S, 1);
            if (emit(c, PNX_ABX(PNX_BCAT, made, b->dst)) == NONE || !jump_later(c, &b->full) ||
                emit(c, PNX_ABC(PNX_XCAPI, kept, 0, 10)) == NONE || emit(c, PNX_ABX(PNX_BCAT, kept, b->dst)) == NONE ||
                !jump_later(c, &b->full) || emit(c, PNX_ABC(PNX_ADDI, b->cur, b->cur, 1)) == NONE ||
                !loop_back(c, b->head, b->cur, 0))
                return false;
        }
    }
    land(c, &b->exits);
    if (by) { // what follows the last match, then the end
        uint32_t over = NONE;
        const uint32_t tail = top_slot(c, BANK_S, 1);
        if (emit(c, PNX_ABC(PNX_SSKIP, tail, b->idx, b->stem)) == NONE || emit(c, PNX_ABX(PNX_BCAT, tail, b->dst)) == NONE || !jump_later(c, &b->full) || !jump_later(c, &over))
            return false;
        land(c, &b->full);
        if (emit(c, PNX_ABX(PNX_LOADI, b->acc, 0)) == NONE)
            return false;
        land(c, &over);
    }
    close_scope(f, b);
    f->floor = b->floor_mark;
    const block done = *b;
    --f->n_blocks;
    restore(f, &done.in);
    restore_owns(f, done.owns_in, done.locals_mark);
    f->dead = false;
    f->depth -= done.n_hidden;
    if (!advance(c)) // past the end
        return false;
    if (c->tok.kind != PNX_T_PATH || !name_eq(tok_name(c), done.user))
        return syntax(c, "the word that uses the block is expected right after its end");
    if (!settle_all(c))
        return false;
    f->def_at = NONE;
    if (!by)
        return emit(c, PNX_ABX(PNX_SETFAIL, 0, PINAX_ERR_NOT_FOUND)) != NONE &&
               emit(c, PNX_ABC(PNX_JNEI, done.acc, 0, 1)) != NONE && guard_op(c, TY_NONE);
    const uint32_t to = top_slot(c, BANK_I, 1);
    return move_slot(c, TY_INT, to, done.cur) && emit(c, PNX_ABX(PNX_SETFAIL, 0, PINAX_ERR_FULL)) != NONE &&
           emit(c, PNX_ABC(PNX_JNEI, done.acc, 0, 1)) != NONE && guard_op(c, TY_INT);
}

// stem do [ ( params ) ] body end each | fold      count do body end times
static bool do_block(comp *c) {
    fn *f = c->f;
    name user, dst_name;
    if (!scan_user(c, &user, &dst_name))
        return false;
    const pnx_token at = c->tok;
    sig bs = {0};
    if (!advance(c))
        return false;
    if (c->tok.kind == PNX_T_LPAREN && !parse_sig(c, &bs))
        return false;
    if (user.length && (ends_with_word(user, "with_match") || ends_with_word(user, "replace_by")))
        return rex_block(c, user, &bs, at.line, at.column);
    static const char *const users[] = {"times", "each",       "fold",     "count",      "any",
                                        "all",   "first",      "each_index", "map_into", "filter_into"};
    bool expanded = false;
    for (size_t k = 0; k < sizeof users / sizeof users[0]; ++k)
        expanded = expanded || name_is(user, users[k]);
    if (!expanded)
        return value_block(c, &bs, at.line, at.column);
    if (bs.fails)
        return error_at(c, PINAX_ERR_SYNTAX, at.line, at.column, "a block that can fail is not supported yet");
    const bool has_stem = f->depth > f->floor && bank_of(f->stack[f->depth - 1].type) == BANK_NONE;
    uint8_t  use, kind = STEM_TIMES, cur = 0, lim = 0, hidden = 0;
    uint16_t src = 0;
    tid     element = TY_INT;
    if (name_is(user, "times")) {
        use = USE_TIMES;
        if (has_stem || !need(c, 1) || f->stack[f->depth - 1].type != TY_INT)
            return error_at(c, PINAX_ERR_TYPE, at.line, at.column, "times takes a count: n do ... end times");
        if (bs.n_in || bs.n_out)
            return error_at(c, PINAX_ERR_TYPE, at.line, at.column, "the block of times takes and gives nothing");
        if (!settle(c, f->depth - 1))
            return false;
        f->stack[f->depth - 1].hidden = true;
        cur = (uint8_t)slot_of(c, f->depth - 1);
        hidden = 1;
    } else if (name_is(user, "each") || name_is(user, "fold") || name_is(user, "count") || name_is(user, "any") ||
               name_is(user, "all") || name_is(user, "first") || name_is(user, "each_index") ||
               name_is(user, "map_into") || name_is(user, "filter_into")) {
        use = name_is(user, "map_into") ? USE_MAP : name_is(user, "filter_into") ? USE_FILTER
            : name_is(user, "each") ? USE_EACH : name_is(user, "fold") ? USE_FOLD : name_is(user, "count") ? USE_COUNT
            : name_is(user, "any") ? USE_ANY : name_is(user, "all") ? USE_ALL : name_is(user, "first") ? USE_FIRST
                                                                                                    : USE_EACH_INDEX;
        if (!has_stem)
            return error_at(c, PINAX_ERR_TYPE, at.line, at.column,
                            "this combining word needs a stem before its block (for example lo hi range)");
        if (!stem_begin(c, &kind, &cur, &lim, &src, &hidden, &element))
            return false;
    } else {
        return error_at(c, PINAX_ERR_SYNTAX, at.line, at.column,
                        "this block is used by a word the compiler does not expand yet");
    }
    // rex.matches: after the whole match the block may take the groups, named str parameters
    // (RFC-0011 section 4.2); here they are left out of the checks and bound below
    const sig full = bs;
    uint32_t  groups = 0;
    if (kind == STEM_REX && bs.n_in > (use == USE_FOLD ? 2u : 1u)) {
        const uint32_t first = use == USE_FOLD ? 2 : 1;
        groups = bs.n_in - first;
        if (groups > PNX_PTN_CAPS || use == USE_EACH_INDEX || use == USE_MAP || use == USE_FILTER)
            return error_at(c, PINAX_ERR_TYPE, at.line, at.column,
                            "the block of rex.matches takes the whole match and at most nine groups (each, fold, count ...)");
        for (uint32_t k = first; k < bs.n_in; ++k) {
            if (bs.in[k] != TY_STR || !sig_in_name(&bs, k).length)
                return error_at(c, PINAX_ERR_TYPE, at.line, at.column, "the groups are named str parameters");
        }
        bs.n_in = (uint8_t)first;
    }
    uint32_t init = NONE, dst = NONE;
    if (use == USE_MAP || use == USE_FILTER) { // the list the results are pushed into
        dst = find_global(c, dst_name);
        const globalsym *g = dst == NONE ? NULL : (const globalsym *)c->globals.ptr + dst;
        // filter_into a list of records: the rows that pass are copied, from a list or an array
        // of the same record
        const uint32_t from = is_row(element) ? (uint32_t)(element - TY_FIRST_DECLARED) : NONE;
        const bool rows = g && g->kind == GK_LIST && g->record != NONE && use == USE_FILTER && from != NONE &&
                          ((const globalsym *)c->globals.ptr + from)->record == g->record &&
                          ((const globalsym *)c->globals.ptr + from)->kind != GK_POOL;
        if (!g || (g->kind != GK_LIST && g->kind != GK_ARRAY) || (g->record != NONE && !rows))
            return error_at(c, PINAX_ERR_TYPE, at.line, at.column,
                            g && g->kind == GK_LIST && g->record != NONE
                                ? "filter_into fills a list of records from a list or an array of the same record"
                            : g && g->record != NONE ? "an array of records is not filled by filter_into; a list is"
                                                     : "map_into and filter_into fill a list or an array of values: "
                                                       "... end names map_into");
        if (rows) {
            if (bs.n_out != 1 || bs.n_in > 1 || (bs.n_in == 1 && bs.in[0] != element) || bs.out[0] != TY_BOOL)
                return error_at(c, PINAX_ERR_TYPE, at.line, at.column, "the block of filter_into is (x T gives bool)");
        } else if (bs.n_out != 1 || bs.n_in > 1 || (bs.n_in == 1 && bs.in[0] != element) ||
            bs.out[0] != (use == USE_MAP ? g->type : TY_BOOL) || (use == USE_FILTER && element != g->type))
            return error_at(c, PINAX_ERR_TYPE, at.line, at.column,
                            use == USE_MAP ? "the block of map_into is (x T gives U): T the element's type, U the list's"
                                           : "the block of filter_into is (x T gives bool), and the list holds T");
    } else if (use == USE_EACH) {
        if (bs.n_out || bs.n_in > 1 || (bs.n_in == 1 && bs.in[0] != element))
            return error_at(c, PINAX_ERR_TYPE, at.line, at.column, "the block of each takes one element and gives nothing");
    } else if (use == USE_FOLD) {
        init = f->depth - hidden - 1;
        if (f->depth - f->floor < (uint32_t)hidden + 1 || f->stack[init].type == TY_STEM)
            return error_at(c, PINAX_ERR_TYPE, at.line, at.column, "fold takes a first value before the stem");
        const tid acc = f->stack[init].type;
        if (bs.n_in != 2 || !bs.named || bs.n_out != 1 || bs.in[0] != acc || bs.out[0] != acc || bs.in[1] != element)
            return error_at(c, PINAX_ERR_TYPE, at.line, at.column,
                            "the block of fold is (acc U, x T gives U): U the first value's type, T the element's");
    } else if (use == USE_EACH_INDEX) {
        if (bs.n_out || bs.n_in != 2 || !bs.named || bs.in[0] != element || bs.in[1] != TY_INT)
            return error_at(c, PINAX_ERR_TYPE, at.line, at.column, "the block of each_index is (x T, i int)");
    } else if (use != USE_TIMES) { // count, any, all, first: a test of one element
        if (bs.n_out != 1 || bs.out[0] != TY_BOOL || bs.n_in > 1 || (bs.n_in == 1 && bs.in[0] != element))
            return error_at(c, PINAX_ERR_TYPE, at.line, at.column, "this block takes one element and gives a bool");
    }
    // ... into an array: the cells written so far and the array's length, below the flag
    uint8_t fill = 0, room = 0;
    if (dst != NONE && ((const globalsym *)c->globals.ptr + dst)->kind == GK_ARRAY) {
        if (!push_slot(c, TY_INT) || !push_slot(c, TY_INT))
            return false;
        f->stack[f->depth - 2].hidden = f->stack[f->depth - 1].hidden = true;
        fill = (uint8_t)slot_of(c, f->depth - 2);
        room = (uint8_t)slot_of(c, f->depth - 1);
        hidden += 2;
        if (emit(c, PNX_ABX(PNX_LOADI, fill, 0)) == NONE || emit(c, PNX_ABX(PNX_GLEN, room, dst)) == NONE)
            return false;
    }
    // what the word keeps while the block runs, above the operands of the stem
    uint8_t keep = 0, val = 0, idx = cur;
    if (use == USE_COUNT || use == USE_ANY || use == USE_ALL || use == USE_FIRST || use == USE_MAP ||
        use == USE_FILTER || (use == USE_EACH_INDEX && (kind == STEM_RANGE || stem_takes(kind)))) {
        if (!push_slot(c, use == USE_COUNT || use == USE_EACH_INDEX ? TY_INT : TY_BOOL))
            return false;
        f->stack[f->depth - 1].hidden = true;
        keep = (uint8_t)slot_of(c, f->depth - 1);
        if (emit(c, PNX_ABX(PNX_LOADI, keep, use == USE_ALL || use == USE_MAP || use == USE_FILTER)) == NONE)
            return false;
        ++hidden;
        if (use == USE_EACH_INDEX)
            idx = keep;
    }
    if (use == USE_FIRST || use == USE_FILTER) {
        if (!push_slot(c, element))
            return false;
        f->stack[f->depth - 1].hidden = true;
        val = (uint8_t)slot_of(c, f->depth - 1);
        ++hidden;
        // a value from the start, so that the word reads none it did not write when nothing is found
        if (emit(c, bank_of(element) == BANK_S ? PNX_ABC(PNX_RMARK, val, 0, 0) : PNX_ABX(PNX_LOADI, val, 0)) == NONE)
            return false;
    }
    if (!settle_all(c))
        return false;
    block *b = open_block(c, B_DO);
    if (!b)
        return false;
    b->val = val;
    b->idx = idx;
    b->element = element;
    if (use != USE_FOLD)
        b->acc = keep;
    b->use = use;
    b->stem = kind;
    b->cur = cur;
    b->lim = lim;
    b->n_hidden = hidden;
    b->user = user;
    b->dst = dst;
    b->dst_name = dst_name;
    b->full = NONE;
    b->fill = fill;
    b->room = room;
    b->floor_mark = f->floor;
    f->floor = f->depth;
    if (use == USE_FOLD) {
        const local *acc = new_local(c, sig_in_name(&bs, 0), bs.in[0]);
        if (!acc || !move_slot(c, acc->type, acc->reg, slot_of(c, init)))
            return false;
        b->acc = acc->reg;
        b->acc_type = acc->type;
    }
    b->head = f->code.n;
    b->elem_local = NONE;
    f->def_at = NONE;
    // where the element goes: a named parameter, or the stack
    uint32_t reg = 0;
    if (use != USE_TIMES) {
        const uint32_t param = use == USE_FOLD ? 1 : 0;
        if (bs.n_in > param && sig_in_name(&bs, param).length) {
            const local *l = new_local(c, sig_in_name(&bs, param), element);
            if (!l)
                return false;
            reg = l->reg;
            if (kind == STEM_RANGE)
                b->elem_local = (uint32_t)(l - f->locals);
        } else {
            if (!push_slot(c, element))
                return false;
            reg = slot_of(c, f->depth - 1);
        }
    }
    if (stem_takes(kind)) { // takes the next piece, or leaves when none is left
        if (!stem_take(c, kind, reg, src, cur, lim) || !jump_later(c, &b->exits))
            return false;
    } else {
        // go on while cur < lim (for times: while the count is at least 1), then fetch the element
        if (emit(c, use == USE_TIMES ? PNX_ABC(PNX_JGEI, cur, 1, 1) : PNX_ABC(PNX_JLT, cur, lim, 1)) == NONE ||
            !jump_later(c, &b->exits))
            return false;
        if (use != USE_TIMES && !stem_element(c, kind, reg, cur, src))
            return false;
    }
    for (uint32_t k = 0; k < groups; ++k) { // the groups of this match
        const local *l = new_local(c, sig_in_name(&full, (use == USE_FOLD ? 2u : 1u) + k), TY_STR);
        if (!l || emit(c, PNX_ABC(PNX_XCAPI, l->reg, 0, k + 1)) == NONE)
            return false;
    }
    if (use == USE_EACH_INDEX) {
        const local *l = new_local(c, sig_in_name(&bs, 1), TY_INT);
        if (!l || emit(c, PNX_ABC(PNX_MOVI, l->reg, idx, 0)) == NONE)
            return false;
    }
    if ((use == USE_FIRST || use == USE_FILTER) && !move_slot(c, element, val, reg))
        return false; // kept, in case this is the one
    for (uint32_t k = b->locals_mark; k < f->n_locals; ++k) { // they hold their values from here
        if (f->locals[k].drec != NONE)
            ((dlocal *)f->dlocals.ptr)[f->locals[k].drec].from = f->code.n;
    }
    f->def_at = NONE;
    return true;
}

// The value in register `value` written to the next cell of the array a map_into or a
// filter_into fills; when every cell is written, the jump to the block's failure is taken.
static bool array_put(comp *c, block *b, uint32_t value) {
    const globalsym *g = (const globalsym *)c->globals.ptr + b->dst;
    if (emit(c, PNX_ABC(PNX_JLT, b->fill, b->room, 1)) == NONE || !jump_later(c, &b->full))
        return false;
    if (g->strcap) { // the bytes are copied into the cell
        const uint32_t row = top_slot(c, BANK_I, 1);
        if (emit_g(c, PNX_ROW, row, b->fill, b->dst) == NONE || emit(c, PNX_ABC(PNX_FSETS, value, row, 0)) == NONE ||
            emit(c, PNX_FIELD(b->dst, 0, g->strcap)) == NONE)
            return false;
    } else if (emit_g(c, PNX_ASET, value, b->fill, b->dst) == NONE) {
        return false;
    }
    return emit(c, PNX_ABC(PNX_ADDI, b->fill, b->fill, 1)) != NONE;
}

static bool end_rex_block(comp *c, block *b);

static bool end_do(comp *c, block *b) {
    fn *f = c->f;
    if (!f->dead && !emit_defers(c, (uint32_t)(b - f->blocks) + 1)) // each run of the body leaves it
        return false;
    if (b->use == USE_WITH || b->use == USE_BY)
        return end_rex_block(c, b);
    if (!f->dead) {
        if (b->use == USE_FOLD) {
            if (f->depth != (uint32_t)b->in.depth + 1 || f->stack[f->depth - 1].type != b->acc_type)
                return type_error(c, "the block of fold leaves the new value of its first parameter");
            if (!write_reg(c, b->acc))
                return false;
        }
        entry verdict = {0};
        uint32_t made = 0;
        if (b->use == USE_MAP) { // the value the block made, out of the way of the stack check
            if (f->depth != (uint32_t)b->in.depth + 1 ||
                f->stack[f->depth - 1].type != ((const globalsym *)c->globals.ptr + b->dst)->type)
                return type_error(c, "the block of map_into leaves one value of the list's type");
            if (!settle(c, f->depth - 1))
                return false;
            made = slot_of(c, f->depth - 1);
            --f->depth;
        }
        const bool tests = b->use == USE_COUNT || b->use == USE_ANY || b->use == USE_ALL || b->use == USE_FIRST ||
                           b->use == USE_FILTER;
        if (tests) {
            if (f->depth != (uint32_t)b->in.depth + 1)
                return type_error(c, "this block leaves one bool: its verdict on the element");
            if (!pop_cond(c, &verdict))
                return false;
        }
        if (!settle_all(c))
            return false;
        if (f->depth == b->out.depth && !same_turn(c, b))
            return false;
        if (!same_stack(f, &b->in))
            return type_error(c, "this block must leave the stack as it found it");
        if (!same_owns(c, b, b->owns_in))
            return false;
        const uint32_t push = (b->use == USE_MAP || b->use == USE_FILTER) &&
                                      ((const globalsym *)c->globals.ptr + b->dst)->strcap
                                  ? PNX_LPUSHS
                                  : PNX_LPUSH;
        if ((b->use == USE_MAP || b->use == USE_FILTER) &&
            ((const globalsym *)c->globals.ptr + b->dst)->kind == GK_ARRAY) {
            uint32_t skip = NONE; // filter_into: past the store when the verdict is false
            if (b->use == USE_FILTER && (!cond_jump(c, &verdict, 1) || !jump_later(c, &skip)))
                return false;
            if (!array_put(c, b, b->use == USE_MAP ? made : b->val))
                return false;
            land(c, &skip);
        } else if (b->use == USE_MAP) { // the instruction after the push runs when the list is full
            if (emit(c, PNX_ABX(push, made, b->dst)) == NONE || !jump_later(c, &b->full))
                return false;
        } else if (b->use == USE_FILTER && ((const globalsym *)c->globals.ptr + b->dst)->record != NONE) {
            cond_negate(&verdict); // a row that passes is copied: its table rides in the second word
            if (!cond_jump(c, &verdict, 3) || emit(c, PNX_ABX(PNX_LCOPYR, b->val, b->dst)) == NONE ||
                emit(c, (uint32_t)(b->element - TY_FIRST_DECLARED)) == NONE || !jump_later(c, &b->full))
                return false;
        } else if (b->use == USE_FILTER) { // pushed when the verdict is true
            cond_negate(&verdict);
            if (!cond_jump(c, &verdict, 2) || emit(c, PNX_ABX(push, b->val, b->dst)) == NONE ||
                !jump_later(c, &b->full))
                return false;
        } else if (b->use == USE_COUNT) { // one more when the verdict is true
            cond_negate(&verdict);
            if (!cond_jump(c, &verdict, 1) || emit(c, PNX_ABC(PNX_ADDI, b->acc, b->acc, 1)) == NONE)
                return false;
        } else if (tests) { // any, first: done at the first true; all: done at the first false
            if (b->use != USE_ALL)
                cond_negate(&verdict);
            if (!cond_jump(c, &verdict, 2) || emit(c, PNX_ABX(PNX_LOADI, b->acc, b->use != USE_ALL)) == NONE ||
                !jump_later(c, &b->exits))
                return false;
        }
        if (b->use == USE_EACH_INDEX && b->idx != b->cur && emit(c, PNX_ABC(PNX_ADDI, b->idx, b->idx, 1)) == NONE)
            return false;
        if (!loop_back(c, b->head, b->cur, b->use == USE_TIMES ? -1 : stem_takes(b->stem) ? 0 : 1))
            return false;
        // A range whose element is a named local the body never stores into: after the first
        // turn the local itself counts (the copy at the top runs once). The rotated loop ends
        // ADDI cur, CHECK, JLT cur lim; those name the local instead and jump past the copy.
        const uint32_t end = f->code.n;
        if (b->elem_local != NONE && !f->locals[b->elem_local].stored && end >= 3 &&
            (*code_at(f, end - 1) & 0xffu) == PNX_JLT && (*code_at(f, end - 3) & 0xffu) == PNX_ADDI &&
            (*code_at(f, b->head + 2) & 0xffu) == PNX_MOVI) {
            const uint32_t reg = f->locals[b->elem_local].reg, back = *code_at(f, end - 1);
            *code_at(f, end - 3) = PNX_ABC(PNX_ADDI, reg, reg, 1);
            *code_at(f, end - 1) = PNX_ABC(PNX_JLT, reg, b->lim, (uint8_t)(int8_t)(OP_SC_OF(back) + 1));
        }
    }
    land(c, &b->exits);
    if (b->full != NONE) { // the list was full: the word fails
        uint32_t over = NONE;
        if (!jump_later(c, &over))
            return false;
        land(c, &b->full);
        if (emit(c, PNX_ABX(PNX_LOADI, b->acc, 0)) == NONE)
            return false;
        land(c, &over);
    }
    close_scope(f, b);
    f->floor = b->floor_mark;
    const block done = *b;
    --f->n_blocks;
    restore(f, &done.in);
    restore_owns(f, done.owns_in, done.locals_mark);
    f->dead = false;
    f->depth -= done.n_hidden;
    if (done.use == USE_FOLD) {
        --f->depth; // the first value; the result takes its place
        if (!push_slot(c, done.acc_type) || !move_slot(c, done.acc_type, slot_of(c, f->depth - 1), done.acc))
            return false;
    }
    if (done.use == USE_COUNT || done.use == USE_ANY || done.use == USE_ALL) {
        const tid type = done.use == USE_COUNT ? TY_INT : TY_BOOL;
        if (!push_slot(c, type) || !move_slot(c, type, slot_of(c, f->depth - 1), done.acc))
            return false;
    }
    if (!advance(c)) // past the end
        return false;
    if (done.use == USE_MAP || done.use == USE_FILTER) { // the list, then the word; fails with full
        if (c->tok.kind != PNX_T_NAME || !name_eq(tok_name(c), done.dst_name) || !advance(c))
            return c->err == PINAX_OK ? syntax(c, "the list to fill is expected right after the end") : false;
        if (((const globalsym *)c->globals.ptr + done.dst)->kind == GK_ARRAY) { // gives the cells written
            const uint32_t to = top_slot(c, BANK_I, 1);
            if (!settle_all(c) || !move_slot(c, TY_INT, to, done.fill) ||
                emit(c, PNX_ABX(PNX_SETFAIL, 0, PINAX_ERR_FULL)) == NONE ||
                emit(c, PNX_ABC(PNX_JNEI, done.acc, 0, 1)) == NONE)
                return false;
            f->def_at = NONE;
            return guard_op(c, TY_INT);
        }
        if (!settle_all(c) || emit(c, PNX_ABX(PNX_SETFAIL, 0, PINAX_ERR_FULL)) == NONE ||
            emit(c, PNX_ABC(PNX_JNEI, done.acc, 0, 1)) == NONE)
            return false;
        f->def_at = NONE;
        return guard_op(c, TY_NONE);
    }
    if (c->tok.kind != PNX_T_NAME || !name_eq(tok_name(c), done.user))
        return syntax(c, "the word that uses the block is expected right after its end");
    if (done.use == USE_FIRST) // the element found, or the failure not_found
        return found_or_fail(c, done.element, done.val, done.acc);
    return advance(c);
}

// The end of a word that gives a value when `flag` is set and fails with not_found otherwise.
// `value` and `flag` are registers above the stack; the current token is the word.
static bool found_or_fail(comp *c, tid type, uint32_t value, uint32_t flag) {
    const uint32_t dst = top_slot(c, bank_of(type), 1);
    if (!settle_all(c) || !move_slot(c, type, dst, value) ||
        emit(c, PNX_ABX(PNX_SETFAIL, 0, PINAX_ERR_NOT_FOUND)) == NONE || emit(c, PNX_ABC(PNX_JNEI, flag, 0, 1)) == NONE)
        return false;
    c->f->def_at = NONE;
    return guard_op(c, type);
}

// stem minimum | stem maximum : the smallest or largest int; fails on an empty stem
static bool do_extreme(comp *c, bool largest) {
    fn *f = c->f;
    if (f->depth == f->floor || bank_of(f->stack[f->depth - 1].type) != BANK_NONE)
        return type_error(c, "this word takes a stem of int values");
    uint8_t  kind, cur, lim, hidden;
    uint16_t src;
    tid     element_type;
    if (!stem_begin(c, &kind, &cur, &lim, &src, &hidden, &element_type))
        return false;
    if (element_type != TY_INT || stem_takes(kind))
        return type_error(c, "this word takes a stem of int values");
    if (!push_slot(c, TY_INT) || !push_slot(c, TY_INT))
        return false;
    const uint32_t best = slot_of(c, f->depth - 2), flag = slot_of(c, f->depth - 1);
    uint32_t       element = top_slot(c, BANK_I, 1), exits = NONE;
    if (emit(c, PNX_ABX(PNX_LOADI, flag, 0)) == NONE || emit(c, PNX_ABX(PNX_LOADI, best, 0)) == NONE || !settle_all(c))
        return false;
    const uint32_t head = f->code.n;
    if (emit(c, PNX_ABC(PNX_JLT, cur, lim, 1)) == NONE || !jump_later(c, &exits))
        return false;
    if (kind == STEM_RANGE)
        element = cur;
    else if (!stem_element(c, kind, element, cur, src))
        return false;
    // the first element is taken as it is; a later one only when it beats the best so far
    if (emit(c, PNX_ABC(PNX_JEQI, flag, 0, 1)) == NONE ||
        emit(c, largest ? PNX_ABC(PNX_JLE, element, best, 1) : PNX_ABC(PNX_JLE, best, element, 1)) == NONE ||
        emit(c, PNX_ABC(PNX_MOVI, best, element, 0)) == NONE || emit(c, PNX_ABX(PNX_LOADI, flag, 1)) == NONE ||
        !loop_back(c, head, cur, 1))
        return false;
    land(c, &exits);
    f->depth -= (uint32_t)hidden + 2;
    return found_or_fail(c, TY_INT, best, flag);
}

// sep buf stem join_into : the str values of the stem appended to the buffer, sep between two;
// fails with full when the buffer is (what was appended stays). Like the first value of fold,
// the separator and the buffer come before the stem, which its word takes at once.
static bool do_join(comp *c) {
    fn *f = c->f;
    uint8_t  kind, cur, lim, hidden;
    uint16_t src;
    tid      element;
    if (f->depth == f->floor || f->stack[f->depth - 1].type != TY_STEM)
        return type_error(c, "join_into takes a separator, a buffer and a stem of str: \",\" out s str.words str.join_into");
    if (!stem_begin(c, &kind, &cur, &lim, &src, &hidden, &element))
        return false;
    const uint32_t base = f->depth - hidden; // the operands of the stem begin here
    if (element != TY_STR || base < f->floor + 2 || f->stack[base - 1].type != TY_COLL ||
        ((const globalsym *)c->globals.ptr + f->stack[base - 1].value)->kind != GK_BUFFER ||
        f->stack[base - 2].type != TY_STR)
        return type_error(c, "join_into takes a separator, a buffer and a stem of str: \",\" out s str.words str.join_into");
    const uint32_t gi = (uint32_t)f->stack[base - 1].value;
    // the name of the buffer is no value: out of the stack; the registers of the others stay
    for (uint32_t k = base - 1; k + 1 < f->depth; ++k)
        f->stack[k] = f->stack[k + 1];
    --f->depth;
    if (!settle(c, base - 2))
        return false;
    const uint32_t sep = slot_of(c, base - 2);
    f->stack[base - 2].hidden = true;
    if (!push_slot(c, TY_INT) || !push_slot(c, TY_INT))
        return false;
    const uint32_t first = slot_of(c, f->depth - 2), ok = slot_of(c, f->depth - 1);
    f->stack[f->depth - 2].hidden = f->stack[f->depth - 1].hidden = true;
    const uint32_t piece = top_slot(c, BANK_S, 1);
    uint32_t       exits = NONE, full = NONE;
    if (emit(c, PNX_ABX(PNX_LOADI, first, 1)) == NONE || emit(c, PNX_ABX(PNX_LOADI, ok, 1)) == NONE || !settle_all(c))
        return false;
    const uint32_t head = f->code.n;
    if (stem_takes(kind)) {
        if (!stem_take(c, kind, piece, src, cur, lim) || !jump_later(c, &exits))
            return false;
    } else if (emit(c, PNX_ABC(PNX_JLT, cur, lim, 1)) == NONE || !jump_later(c, &exits) ||
               !stem_element(c, kind, piece, cur, src)) {
        return false;
    }
    if (emit(c, PNX_ABC(PNX_JEQI, first, 1, 2)) == NONE || emit(c, PNX_ABX(PNX_BCAT, sep, gi)) == NONE ||
        !jump_later(c, &full) || emit(c, PNX_ABX(PNX_LOADI, first, 0)) == NONE ||
        emit(c, PNX_ABX(PNX_BCAT, piece, gi)) == NONE || !jump_later(c, &full) ||
        !loop_back(c, head, cur, stem_takes(kind) ? 0 : 1))
        return false;
    land(c, &full);
    if (emit(c, PNX_ABX(PNX_LOADI, ok, 0)) == NONE)
        return false;
    land(c, &exits);
    f->depth -= (uint32_t)hidden + 3;
    if (!settle_all(c) || emit(c, PNX_ABX(PNX_SETFAIL, 0, PINAX_ERR_FULL)) == NONE ||
        emit(c, PNX_ABC(PNX_JNEI, ok, 0, 1)) == NONE)
        return false;
    f->def_at = NONE;
    return guard_op(c, TY_NONE);
}

// s old new buf replace_into : s with every old replaced by new, appended to the buffer; gives
// the number replaced, fails with full when the buffer is (what was appended stays). An empty
// old replaces nothing.
static bool do_replace_into(comp *c) {
    fn *f = c->f;
    const globalsym *g;
    uint32_t         gi;
    if (!need_coll(c, &g, &gi) || !need(c, 3))
        return false;
    for (uint32_t k = 1; k <= 3; ++k) {
        if (f->stack[f->depth - k].type != TY_STR || g->kind != GK_BUFFER)
            return type_error(c, "replace_into takes three str values and a buffer: s old new buf str.replace_into");
        if (!settle(c, f->depth - k))
            return false;
    }
    const uint32_t s = slot_of(c, f->depth - 3), old = slot_of(c, f->depth - 2), with = slot_of(c, f->depth - 1);
    if (!push_slot(c, TY_INT) || !push_slot(c, TY_INT) || !push_slot(c, TY_INT) || !push_slot(c, TY_INT))
        return false;
    const uint32_t count = slot_of(c, f->depth - 4), ok = slot_of(c, f->depth - 3), at = slot_of(c, f->depth - 2),
                   size = slot_of(c, f->depth - 1);
    const uint32_t piece = top_slot(c, BANK_S, 1);
    uint32_t       tail = NONE, full = NONE, over = NONE;
    if (!settle_all(c) || emit(c, PNX_ABX(PNX_LOADI, count, 0)) == NONE || emit(c, PNX_ABX(PNX_LOADI, ok, 1)) == NONE ||
        emit(c, PNX_ABC(PNX_SLEN, size, old, 0)) == NONE || emit(c, PNX_ABC(PNX_JNEI, size, 0, 1)) == NONE ||
        !jump_later(c, &tail))
        return false;
    const uint32_t head = f->code.n;
    if (emit(c, PNX_ABC(PNX_SFIND, at, s, old)) == NONE || !jump_later(c, &tail) ||
        emit(c, PNX_ABC(PNX_STAKE, piece, s, at)) == NONE || emit(c, PNX_ABX(PNX_BCAT, piece, gi)) == NONE ||
        !jump_later(c, &full) || emit(c, PNX_ABX(PNX_BCAT, with, gi)) == NONE || !jump_later(c, &full) ||
        emit(c, PNX_ABC(PNX_ADD, at, at, size)) == NONE || emit(c, PNX_ABC(PNX_SSKIP, s, s, at)) == NONE ||
        emit(c, PNX_ABC(PNX_ADDI, count, count, 1)) == NONE || !loop_back(c, head, count, 0))
        return false;
    land(c, &tail);
    if (emit(c, PNX_ABX(PNX_BCAT, s, gi)) == NONE || !jump_later(c, &full) || !jump_later(c, &over))
        return false;
    land(c, &full);
    if (emit(c, PNX_ABX(PNX_LOADI, ok, 0)) == NONE)
        return false;
    land(c, &over);
    f->depth -= 7;
    const uint32_t to = top_slot(c, BANK_I, 1);
    if (!settle_all(c) || !move_slot(c, TY_INT, to, count) || emit(c, PNX_ABX(PNX_SETFAIL, 0, PINAX_ERR_FULL)) == NONE ||
        emit(c, PNX_ABC(PNX_JNEI, ok, 0, 1)) == NONE)
        return false;
    f->def_at = NONE;
    return guard_op(c, TY_INT);
}

// How field `fd` of a record is packed: a width code of BPUT and SGET, or NONE for str(N) (a
// varint length, then the bytes).
static uint32_t pack_code(const record *r, const field *fd) {
    if (fd->is_str)
        return NONE;
    if (fd->varint)
        return PNX_W_VARINT;
    const uint32_t width = fd->extra;
    return r->big_endian && width != PNX_W_U8 && width != PNX_W_I8 ? width | PNX_W_BE : width;
}

// row buf pack (or fail) : the fields of the row in declaration order, appended to the buffer;
// when it is full the buffer is cut back to what it held. s row unpack (gives rest or fail):
// the fields read into the row, and the bytes after them; nothing is written unless all of
// them read (format otherwise: short, a bool past 1, a str longer than its field). row
// packed_size (gives int): the bytes pack would append.
static bool pack_word(comp *c, name n) {
    fn *f = c->f;
    const bool unpack = name_is(n, "unpack"), size = name_is(n, "packed_size");
    const globalsym *g = NULL;
    uint32_t         gi = 0;
    if (!unpack && !size && !need_coll(c, &g, &gi))
        return false;
    if (!need(c, unpack ? 2 : 1))
        return false;
    const tid row = f->stack[f->depth - 1].type;
    const globalsym *table = is_row(row) ? (const globalsym *)c->globals.ptr + (row - TY_FIRST_DECLARED) : NULL;
    if (!table || table->record == NONE || (g && g->kind != GK_BUFFER) || (unpack && f->stack[f->depth - 2].type != TY_STR))
        return type_error(c, unpack ? "unpack takes a str and a row of a table of records: s row bytes.unpack"
                           : size   ? "packed_size takes a row of a table of records"
                                    : "pack takes a row of a table of records and a buffer: row buf bytes.pack");
    if (f->stack[f->depth - 1].owned)
        return type_error(c, "an owned row is kept in a local before its fields are used");
    const record *r = (const record *)c->records.ptr + table->record;
    const uint32_t tab = row - TY_FIRST_DECLARED;
    uint32_t rr, rs = 0;
    if (unpack && !settle(c, f->depth - 2))
        return false;
    if (unpack)
        rs = slot_of(c, f->depth - 2);
    if (!settle(c, f->depth - 1))
        return false;
    rr = slot_of(c, f->depth - 1);
    f->depth -= unpack ? 2 : 1;
    if (!settle_all(c))
        return false;
    // temporaries above the row and the string, which stay where they are
    const uint32_t ib = top_slot(c, BANK_I, 5) + 1, t = ib, ok = ib + 1, aux = ib + 2, acc = ib + 3;
    const uint32_t sb = top_slot(c, BANK_S, 3), v = sb + 1, piece = sb + 2;
    uint32_t       bad = NONE, over = NONE;
    if (size) {
        int64_t fixed = 0;
        for (uint32_t k = 0; k < r->count; ++k) {
            const field *fd = (const field *)c->fields.ptr + r->first + k;
            const uint32_t code = pack_code(r, fd);
            if (code != NONE && code != PNX_W_VARINT)
                fixed += code & PNX_W_BE ? (code & 7u) == PNX_W_64 ? 8 : (code & 7u) == PNX_W_U32 || (code & 7u) == PNX_W_I32 ? 4 : 2
                                         : (code == PNX_W_U8 || code == PNX_W_I8) ? 1 : code == PNX_W_U16 || code == PNX_W_I16 ? 2
                                         : code == PNX_W_U32 || code == PNX_W_I32 ? 4 : 8;
        }
        if (!load_int(c, acc, fixed))
            return false;
        for (uint32_t k = 0; k < r->count; ++k) {
            const field *fd = (const field *)c->fields.ptr + r->first + k;
            const uint32_t code = pack_code(r, fd), where = PNX_FIELD(tab, fd->offset, fd->extra);
            if (code == PNX_W_VARINT) {
                if (emit(c, PNX_ABC(PNX_FGET, t, rr, 0)) == NONE || emit(c, where) == NONE ||
                    emit(c, PNX_ABC(PNX_VLEN, t, t, 0)) == NONE || emit(c, PNX_ABC(PNX_ADD, acc, acc, t)) == NONE)
                    return false;
            } else if (code == NONE) {
                if (emit(c, PNX_ABC(PNX_FGETS, v, rr, 0)) == NONE || emit(c, where) == NONE ||
                    emit(c, PNX_ABC(PNX_SLEN, t, v, 0)) == NONE || emit(c, PNX_ABC(PNX_ADD, acc, acc, t)) == NONE ||
                    emit(c, PNX_ABC(PNX_VLEN, t, t, 0)) == NONE || emit(c, PNX_ABC(PNX_ADD, acc, acc, t)) == NONE)
                    return false;
            }
        }
        if (!push_slot(c, TY_INT))
            return false;
        f->def_at = NONE;
        return emit(c, PNX_ABC(PNX_MOVI, slot_of(c, f->depth - 1), acc, 0)) != NONE && advance(c);
    }
    if (emit(c, PNX_ABX(PNX_LOADI, ok, 1)) == NONE)
        return false;
    if (!unpack) { // the length before, to cut back to
        if (emit(c, PNX_ABX(PNX_BVIEW, v, gi)) == NONE || emit(c, PNX_ABC(PNX_SLEN, aux, v, 0)) == NONE)
            return false;
        for (uint32_t k = 0; k < r->count; ++k) {
            const field *fd = (const field *)c->fields.ptr + r->first + k;
            const uint32_t code = pack_code(r, fd), where = PNX_FIELD(tab, fd->offset, fd->extra);
            if (code == NONE ? emit(c, PNX_ABC(PNX_FGETS, v, rr, 0)) == NONE || emit(c, where) == NONE ||
                                   emit_g(c, PNX_BPUTS, v, 1, gi) == NONE
                             : emit(c, PNX_ABC(PNX_FGET, t, rr, 0)) == NONE || emit(c, where) == NONE ||
                                   emit_g(c, PNX_BPUT, t, code, gi) == NONE)
                return false;
            if (!jump_later(c, &bad))
                return false;
        }
        if (!jump_later(c, &over))
            return false;
        land(c, &bad);
        if (emit(c, PNX_ABX(PNX_BTRUNC, aux, gi)) == NONE || emit(c, PNX_ABX(PNX_LOADI, ok, 0)) == NONE)
            return false;
        land(c, &over);
        f->def_at = NONE;
        return emit(c, PNX_ABX(PNX_SETFAIL, 0, PINAX_ERR_FULL)) != NONE && emit(c, PNX_ABC(PNX_JNEI, ok, 0, 1)) != NONE &&
               guard_op(c, TY_NONE);
    }
    // unpack: first every field is read and checked, then read again and stored
    for (int pass = 0; pass < 2; ++pass) {
        if (emit(c, PNX_ABC(PNX_MOVS, v, rs, 0)) == NONE)
            return false;
        for (uint32_t k = 0; k < r->count; ++k) {
            const field *fd = (const field *)c->fields.ptr + r->first + k;
            const uint32_t code = pack_code(r, fd), where = PNX_FIELD(tab, fd->offset, fd->extra);
            if (code == NONE) {
                if (emit(c, PNX_ABC(PNX_SGETS, piece, v, 1)) == NONE || !jump_later(c, &bad))
                    return false;
                if (pass == 0 && (emit(c, PNX_ABC(PNX_SLEN, t, piece, 0)) == NONE || !load_int(c, aux, fd->extra) ||
                                  emit(c, PNX_ABC(PNX_JLE, t, aux, 1)) == NONE || !jump_later(c, &bad)))
                    return false;
                if (pass == 1 && (emit(c, PNX_ABC(PNX_FSETS, piece, rr, 0)) == NONE || emit(c, where) == NONE))
                    return false;
            } else {
                if (emit(c, PNX_ABC(PNX_SGET, t, v, code)) == NONE || !jump_later(c, &bad))
                    return false;
                if (pass == 0 && fd->type == TY_BOOL &&
                    (emit(c, PNX_ABC(PNX_JLTI, t, 2, 1)) == NONE || !jump_later(c, &bad)))
                    return false;
                if (pass == 1 && (emit(c, PNX_ABC(PNX_FSET, t, rr, 0)) == NONE || emit(c, where) == NONE))
                    return false;
            }
        }
    }
    if (emit(c, PNX_ABC(PNX_MOVS, rs, v, 0)) == NONE || !jump_later(c, &over))
        return false;
    land(c, &bad);
    if (emit(c, PNX_ABX(PNX_LOADI, ok, 0)) == NONE)
        return false;
    land(c, &over);
    f->def_at = NONE;
    return emit(c, PNX_ABX(PNX_SETFAIL, 0, PINAX_ERR_FORMAT)) != NONE && emit(c, PNX_ABC(PNX_JNEI, ok, 0, 1)) != NONE &&
           guard_op(c, TY_STR);
}

// ---- json and csv (RFC-0009 section 4.5, RFC-0016 S5) -------------------------------------

static bool add_bytes(comp *c, const uint8_t *bytes, uint32_t length, uint32_t *index);

// A text constant into register `reg`.
static bool load_text(comp *c, uint32_t reg, const char *text) {
    uint32_t index;
    return add_bytes(c, (const uint8_t *)text, (uint32_t)__builtin_strlen(text), &index) &&
           emit(c, PNX_ABX(PNX_LOADKS, reg, index)) != NONE;
}
static bool load_name(comp *c, uint32_t reg, name n) {
    uint32_t index;
    return add_bytes(c, n.text, n.length, &index) && emit(c, PNX_ABX(PNX_LOADKS, reg, index)) != NONE;
}

// The registers of a record's text pack and unpack, above the row and the string.
typedef struct textregs {
    uint32_t rr, rs;           // the row; the string read (unpack)
    uint32_t t, aux, ok, size; // I
    uint32_t v, val, key;      // S
} textregs;

// The value text `val` (a JSON token of kind t, or a CSV field) checked against field fd and,
// in the second pass, stored into the row. Every failure joins `bad`.
static bool text_value(comp *c, const textregs *x, const field *fd, uint32_t where, bool json, bool store, uint32_t *bad) {
    if (fd->is_str) {
        if ((json && (emit(c, PNX_ABC(PNX_JEQI, x->t, PNX_JSON_STRING, 1)) == NONE || !jump_later(c, bad))) ||
            emit(c, PNX_ABC(PNX_SLEN, x->aux, x->val, 0)) == NONE || !load_int(c, x->size, fd->extra) ||
            emit(c, PNX_ABC(PNX_JLE, x->aux, x->size, 1)) == NONE || !jump_later(c, bad))
            return false;
        return !store || (emit(c, PNX_ABC(PNX_FSETS, x->val, x->rr, 0)) != NONE && emit(c, where) != NONE);
    }
    if (fd->type == TY_BOOL) { // true, or false; nothing else
        if (json && (emit(c, PNX_ABC(PNX_JEQI, x->t, PNX_JSON_BOOLEAN, 1)) == NONE || !jump_later(c, bad)))
            return false;
        uint32_t done = NONE;
        if (emit(c, PNX_ABX(PNX_LOADI, x->aux, 1)) == NONE || !load_text(c, x->key, "true") ||
            emit(c, PNX_ABC(PNX_JSNE, x->val, x->key, 1)) == NONE || !jump_later(c, &done) ||
            emit(c, PNX_ABX(PNX_LOADI, x->aux, 0)) == NONE || !load_text(c, x->key, "false") ||
            emit(c, PNX_ABC(PNX_JSEQ, x->val, x->key, 1)) == NONE || !jump_later(c, bad))
            return false;
        land(c, &done);
    } else { // a number: an int that fits the field
        if ((json && (emit(c, PNX_ABC(PNX_JEQI, x->t, PNX_JSON_NUMBER, 1)) == NONE || !jump_later(c, bad))) ||
            emit(c, PNX_ABC(PNX_TOINT, x->aux, x->val, 0)) == NONE || !jump_later(c, bad) ||
            emit(c, PNX_ABC(PNX_FITW, x->aux, 0, fd->extra)) == NONE || !jump_later(c, bad))
            return false;
    }
    return !store || (emit(c, PNX_ABC(PNX_FSET, x->aux, x->rr, 0)) != NONE && emit(c, where) != NONE);
}

// row buf json.pack | csv.pack (or fail): the fields as an object (names as keys) or as one
// line, appended; when the buffer is full it is cut back to what it held. s row json.unpack |
// csv.unpack (gives rest str or fail): the fields read into the row (JSON: by name, other keys
// passed over; CSV: in order, the record ending with the last), nothing written unless all are
// right (format otherwise); gives what follows.
static bool text_record(comp *c, bool json, bool unpack) {
    fn *f = c->f;
    const globalsym *g = NULL;
    uint32_t         gi = 0;
    if (!unpack && !need_coll(c, &g, &gi))
        return false;
    if (!need(c, unpack ? 2 : 1))
        return false;
    const tid row = f->stack[f->depth - 1].type;
    const globalsym *table = is_row(row) ? (const globalsym *)c->globals.ptr + (row - TY_FIRST_DECLARED) : NULL;
    if (!table || table->record == NONE || (g && g->kind != GK_BUFFER) || (unpack && f->stack[f->depth - 2].type != TY_STR))
        return type_error(c, unpack ? (json ? "unpack takes a str and a row of a table of records: s row json.unpack"
                                            : "unpack takes a str and a row of a table of records: s row csv.unpack")
                             : json ? "pack takes a row of a table of records and a buffer: row buf json.pack"
                                    : "pack takes a row of a table of records and a buffer: row buf csv.pack");
    if (f->stack[f->depth - 1].owned)
        return type_error(c, "an owned row is kept in a local before its fields are used");
    const record *r = (const record *)c->records.ptr + table->record;
    const uint32_t tab = row - TY_FIRST_DECLARED;
    textregs x = {0};
    if (unpack && !settle(c, f->depth - 2))
        return false;
    if (unpack)
        x.rs = slot_of(c, f->depth - 2);
    if (!settle(c, f->depth - 1))
        return false;
    x.rr = slot_of(c, f->depth - 1);
    f->depth -= unpack ? 2 : 1;
    if (!settle_all(c))
        return false;
    const uint32_t ib = top_slot(c, BANK_I, 6) + 1, sb = top_slot(c, BANK_S, 4);
    x.t = ib, x.aux = ib + 1, x.ok = ib + 2, x.size = ib + 3;
    x.v = sb + 1, x.val = sb + 2, x.key = sb + 3;
    uint32_t bad = NONE, over = NONE;
    if (emit(c, PNX_ABX(PNX_LOADI, x.ok, 1)) == NONE || emit(c, PNX_ABX(PNX_LOADI, x.t, 0)) == NONE)
        return false;
    if (!unpack) {
        const uint32_t start = x.size; // the length before, to cut back to
        if (emit(c, PNX_ABX(PNX_BVIEW, x.v, gi)) == NONE || emit(c, PNX_ABC(PNX_SLEN, start, x.v, 0)) == NONE ||
            (json && (emit_g(c, PNX_JPUTI, x.t, PNX_JSON_OBJECT, gi) == NONE || !jump_later(c, &bad))))
            return false;
        for (uint32_t k = 0; k < r->count; ++k) {
            const field *fd = (const field *)c->fields.ptr + r->first + k;
            const uint32_t where = PNX_FIELD(tab, fd->offset, fd->extra);
            if (json && (!load_name(c, x.key, fd->name) || emit_g(c, PNX_JPUTS, x.key, PNX_JSON_KEY, gi) == NONE ||
                         !jump_later(c, &bad)))
                return false;
            if (fd->is_str ? emit(c, PNX_ABC(PNX_FGETS, x.v, x.rr, 0)) == NONE || emit(c, where) == NONE ||
                                 emit_g(c, json ? PNX_JPUTS : PNX_CPUTS, x.v, json ? PNX_JSON_STRING : 0, gi) == NONE
                           : emit(c, PNX_ABC(PNX_FGET, x.t, x.rr, 0)) == NONE || emit(c, where) == NONE ||
                                 emit_g(c, json ? PNX_JPUTI : PNX_CPUTI, x.t,
                                        fd->type == TY_BOOL ? (json ? PNX_JSON_BOOLEAN : 1) : (json ? PNX_JSON_NUMBER : 0), gi) == NONE)
                return false;
            if (!jump_later(c, &bad))
                return false;
        }
        if (emit_g(c, json ? PNX_JPUTI : PNX_CPUTI, x.t, json ? PNX_JSON_OBJECT_END : 2, gi) == NONE ||
            !jump_later(c, &bad) || !jump_later(c, &over))
            return false;
        land(c, &bad);
        if (emit(c, PNX_ABX(PNX_BTRUNC, start, gi)) == NONE || emit(c, PNX_ABX(PNX_LOADI, x.ok, 0)) == NONE)
            return false;
        land(c, &over);
        f->def_at = NONE;
        return emit(c, PNX_ABX(PNX_SETFAIL, 0, PINAX_ERR_FULL)) != NONE && emit(c, PNX_ABC(PNX_JNEI, x.ok, 0, 1)) != NONE &&
               guard_op(c, TY_NONE);
    }
    for (int pass = 0; pass < 2; ++pass) { // first everything is checked, then read again and stored
        if (emit(c, PNX_ABC(PNX_MOVS, x.v, x.rs, 0)) == NONE)
            return false;
        if (!json) {
            for (uint32_t k = 0; k < r->count; ++k) {
                const field *fd = (const field *)c->fields.ptr + r->first + k;
                if (emit(c, PNX_ABC(PNX_CNEXT, x.v, x.val, k + 1 == r->count ? 2 : 1)) == NONE || !jump_later(c, &bad) ||
                    !text_value(c, &x, fd, PNX_FIELD(tab, fd->offset, fd->extra), false, pass == 1, &bad))
                    return false;
            }
            continue;
        }
        uint32_t done = NONE;
        if (emit(c, PNX_ABC(PNX_JNEXT, x.t, x.val, x.v)) == NONE || !jump_later(c, &bad) ||
            emit(c, PNX_ABC(PNX_JEQI, x.t, PNX_JSON_OBJECT, 1)) == NONE || !jump_later(c, &bad))
            return false;
        const uint32_t head = f->code.n;
        if (emit(c, PNX_ABC(PNX_JNEXT, x.t, x.val, x.v)) == NONE || !jump_later(c, &bad) ||
            emit(c, PNX_ABC(PNX_JNEI, x.t, PNX_JSON_OBJECT_END, 1)) == NONE || !jump_later(c, &done) ||
            emit(c, PNX_ABC(PNX_JEQI, x.t, PNX_JSON_KEY, 1)) == NONE || !jump_later(c, &bad))
            return false;
        for (uint32_t k = 0; k < r->count; ++k) { // the field of that name
            const field *fd = (const field *)c->fields.ptr + r->first + k;
            uint32_t     other = NONE;
            if (!load_name(c, x.key, fd->name) || emit(c, PNX_ABC(PNX_JSEQ, x.val, x.key, 1)) == NONE ||
                !jump_later(c, &other) || emit(c, PNX_ABC(PNX_JNEXT, x.t, x.val, x.v)) == NONE || !jump_later(c, &bad) ||
                !text_value(c, &x, fd, PNX_FIELD(tab, fd->offset, fd->extra), true, pass == 1, &bad) ||
                !loop_back(c, head, x.t, 0))
                return false;
            land(c, &other);
        }
        if (emit(c, PNX_ABC(PNX_JSKIP, x.v, 0, 0)) == NONE || !jump_later(c, &bad) || !loop_back(c, head, x.t, 0))
            return false; // a key of no field: its value passed over
        land(c, &done);
    }
    if (emit(c, PNX_ABC(PNX_MOVS, x.rs, x.v, 0)) == NONE || !jump_later(c, &over))
        return false;
    land(c, &bad);
    if (emit(c, PNX_ABX(PNX_LOADI, x.ok, 0)) == NONE)
        return false;
    land(c, &over);
    f->def_at = NONE;
    return emit(c, PNX_ABX(PNX_SETFAIL, 0, PINAX_ERR_FORMAT)) != NONE && emit(c, PNX_ABC(PNX_JNEI, x.ok, 0, 1)) != NONE &&
           guard_op(c, TY_STR);
}

// A word of json or csv that writes into a buffer: `kind` for JPUTI/JPUTS/CPUTI/CPUTS; `value`
// the type it takes (TY_NONE: none).
static bool text_put(comp *c, uint32_t op, uint32_t kind, tid value) {
    fn *f = c->f;
    const globalsym *g;
    uint32_t         gi, ra = 0;
    if (!need_coll(c, &g, &gi) || (value != TY_NONE && !need(c, 1)))
        return false;
    if (g->kind != GK_BUFFER || (value != TY_NONE && f->stack[f->depth - 1].type != value))
        return type_error(c, value == TY_NONE ? "this word takes a buffer" : "this word takes a value and a buffer: v buf json.put_int");
    if (value != TY_NONE) {
        if (!settle(c, f->depth - 1))
            return false;
        ra = slot_of(c, f->depth - 1);
        --f->depth;
    }
    if (!settle_all(c))
        return false;
    if (value == TY_NONE) { // the instruction reads A: something written
        ra = top_slot(c, BANK_I, 1);
        if (emit(c, PNX_ABX(PNX_LOADI, ra, 0)) == NONE)
            return false;
    }
    return emit_g(c, op, ra, kind, gi) != NONE && guard_op(c, TY_NONE);
}

static bool json_word(comp *c, name n) {
    fn *f = c->f;
    static const struct {
        const char *name;
        int64_t     value;
    } kinds[] = {{"object", PNX_JSON_OBJECT}, {"object_end", PNX_JSON_OBJECT_END}, {"array", PNX_JSON_ARRAY},
                 {"array_end", PNX_JSON_ARRAY_END}, {"key", PNX_JSON_KEY}, {"string", PNX_JSON_STRING},
                 {"number", PNX_JSON_NUMBER}, {"boolean", PNX_JSON_BOOLEAN}, {"null", PNX_JSON_NULL}};
    for (size_t k = 0; k < sizeof kinds / sizeof kinds[0]; ++k) { // the kinds json.next gives
        if (name_is(n, kinds[k].name))
            return push(c, (entry){.type = TY_INT, .kind = E_CONST, .value = kinds[k].value}) && advance(c);
    }
    if (name_is(n, "begin_object") || name_is(n, "end_object") || name_is(n, "begin_array") || name_is(n, "end_array") ||
        name_is(n, "put_null"))
        return text_put(c, PNX_JPUTI, name_is(n, "begin_object") ? PNX_JSON_OBJECT : name_is(n, "end_object") ? PNX_JSON_OBJECT_END
                                     : name_is(n, "begin_array") ? PNX_JSON_ARRAY : name_is(n, "end_array") ? PNX_JSON_ARRAY_END
                                                                 : PNX_JSON_NULL, TY_NONE);
    if (name_is(n, "put_int") || name_is(n, "put_bool"))
        return text_put(c, PNX_JPUTI, name_is(n, "put_int") ? PNX_JSON_NUMBER : PNX_JSON_BOOLEAN,
                        name_is(n, "put_int") ? TY_INT : TY_BOOL);
    if (name_is(n, "put_key") || name_is(n, "put_str"))
        return text_put(c, PNX_JPUTS, name_is(n, "put_key") ? PNX_JSON_KEY : PNX_JSON_STRING, TY_STR);
    if (name_is(n, "pack") || name_is(n, "unpack"))
        return text_record(c, true, name_is(n, "unpack"));
    if (!name_is(n, "next") && !name_is(n, "skip"))
        return error(c, PINAX_ERR_NOT_FOUND, "the module has no word of this name");
    if (!need(c, 1))
        return false;
    if (f->stack[f->depth - 1].type != TY_STR)
        return type_error(c, "this word takes the JSON text as a str");
    if (!settle(c, f->depth - 1))
        return false;
    const uint32_t rs = slot_of(c, f->depth - 1);
    --f->depth;
    if (!settle_all(c))
        return false;
    f->def_at = NONE;
    if (name_is(n, "skip")) // (s gives rest str or fail)
        return emit(c, PNX_ABC(PNX_JSKIP, rs, 0, 0)) != NONE && guard_op(c, TY_STR);
    // (s gives kind int, value str, rest str or fail): the value where s was, the rest above
    const uint32_t value = top_slot(c, BANK_S, 2);
    if (emit(c, PNX_ABC(PNX_MOVS, value + 1, rs, 0)) == NONE ||
        emit(c, PNX_ABC(PNX_JNEXT, top_slot(c, BANK_I, 1), value, value + 1)) == NONE || !advance(c))
        return false;
    sig three = {.n_out = 3, .fails = true};
    three.out[0] = TY_INT;
    three.out[1] = three.out[2] = TY_STR;
    const uint32_t zero[2] = {0, 0};
    return guard(c, &three, zero, zero, false);
}

static bool csv_word(comp *c, name n) {
    fn *f = c->f;
    if (name_is(n, "put"))
        return text_put(c, PNX_CPUTS, 0, TY_STR);
    if (name_is(n, "put_int") || name_is(n, "put_bool"))
        return text_put(c, PNX_CPUTI, name_is(n, "put_int") ? 0 : 1, name_is(n, "put_int") ? TY_INT : TY_BOOL);
    if (name_is(n, "end_line"))
        return text_put(c, PNX_CPUTI, 2, TY_NONE);
    if (name_is(n, "pack") || name_is(n, "unpack"))
        return text_record(c, false, name_is(n, "unpack"));
    if (!name_is(n, "fields"))
        return error(c, PINAX_ERR_NOT_FOUND, "the module has no word of this name");
    if (!need(c, 1))
        return false;
    if (f->stack[f->depth - 1].type != TY_STR)
        return type_error(c, "fields takes a line of CSV as a str");
    return settle(c, f->depth - 1) && push(c, (entry){.type = TY_STEM, .kind = E_SLOT, .op = STEM_CSV}) && advance(c);
}

// The words of the module bytes (RFC-0009 section 4.4): put_T and get_T for T in u8 u16 u32 u64
// i8 i16 i32 i64 (with _be for big-endian, two bytes and more), varint and str (a varint length
// and the bytes). put: (v, buf or fail); get: (s str gives v, rest str or fail).
static bool bytes_word(comp *c, name n) {
    fn *f = c->f;
    static const struct {
        const char *name;
        uint32_t    code;
    } kinds[] = {{"u8", PNX_W_U8},   {"u16", PNX_W_U16}, {"u32", PNX_W_U32}, {"u64", PNX_W_64},
                 {"i8", PNX_W_I8},   {"i16", PNX_W_I16}, {"i32", PNX_W_I32}, {"i64", PNX_W_64},
                 {"varint", PNX_W_VARINT}, {"str", NONE}};
    if (name_is(n, "pack") || name_is(n, "unpack") || name_is(n, "packed_size"))
        return pack_word(c, n);
    const bool put = n.length > 4 && __builtin_memcmp(n.text, "put_", 4) == 0;
    if (!put && !(n.length > 4 && __builtin_memcmp(n.text, "get_", 4) == 0))
        return error(c, PINAX_ERR_NOT_FOUND, "the module has no word of this name");
    name rest = {n.text + 4, n.length - 4};
    const bool be = rest.length > 3 && __builtin_memcmp(rest.text + rest.length - 3, "_be", 3) == 0;
    if (be)
        rest.length -= 3;
    uint32_t code = NONE - 1;
    for (uint32_t k = 0; k < sizeof kinds / sizeof kinds[0]; ++k) {
        if (name_is(rest, kinds[k].name))
            code = kinds[k].code;
    }
    if (code == NONE - 1 || (be && (code == NONE || code == PNX_W_VARINT || code == PNX_W_U8 || code == PNX_W_I8)))
        return error(c, PINAX_ERR_NOT_FOUND, "the module has no word of this name");
    if (be)
        code |= PNX_W_BE;
    uint32_t ra;
    if (put) { // v buf put_T
        const globalsym *g;
        uint32_t         gi;
        if (!need_coll(c, &g, &gi) || !need(c, 1))
            return false;
        if (g->kind != GK_BUFFER || f->stack[f->depth - 1].type != (code == NONE ? TY_STR : TY_INT))
            return type_error(c, code == NONE ? "put_str takes a str and a buffer: s buf bytes.put_str"
                                              : "this word takes an int and a buffer: n buf bytes.put_u32");
        if (!settle(c, f->depth - 1))
            return false;
        ra = slot_of(c, f->depth - 1);
        --f->depth;
        if (!settle_all(c))
            return false;
        return (code == NONE ? emit_g(c, PNX_BPUTS, ra, 1, gi) : emit_g(c, PNX_BPUT, ra, code, gi)) != NONE &&
               guard_op(c, TY_NONE);
    }
    if (!need(c, 1))
        return false;
    if (f->stack[f->depth - 1].type != TY_STR)
        return type_error(c, "this word takes a str: s bytes.get_u32");
    if (!settle(c, f->depth - 1))
        return false;
    ra = slot_of(c, f->depth - 1);
    --f->depth;
    if (!settle_all(c))
        return false;
    sig two = {.n_out = 2, .fails = true};
    two.out[0] = code == NONE ? TY_STR : TY_INT;
    two.out[1] = TY_STR;
    if (code == NONE) { // the value where s was, the rest above it
        const uint32_t value = top_slot(c, BANK_S, 2);
        if (emit(c, PNX_ABC(PNX_MOVS, value + 1, ra, 0)) == NONE ||
            emit(c, PNX_ABC(PNX_SGETS, value, value + 1, 1)) == NONE || !advance(c))
            return false;
    } else if (emit(c, PNX_ABC(PNX_SGET, top_slot(c, BANK_I, 1), ra, code)) == NONE || !advance(c)) {
        return false;
    }
    f->def_at = NONE;
    const uint32_t base[2] = {0, 0}, in[2] = {0, 0};
    return guard(c, &two, base, in, false);
}

static bool push_string(comp *c, uint32_t index);

// Bytes added as a string constant (a program of rex).
static bool add_bytes(comp *c, const uint8_t *bytes, uint32_t length, uint32_t *index) {
    const uint32_t offset = c->strbytes.n;
    for (uint32_t k = 0; k < length; ++k) {
        uint8_t *b = vec_push(c, &c->strbytes, 1);
        if (!b)
            return false;
        *b = bytes[k];
    }
    pnx_sconst *k = vec_push(c, &c->sconst, sizeof(pnx_sconst));
    if (!k)
        return false;
    *k = (pnx_sconst){offset, length};
    if (c->sconst.n - 1 > 0xffffu)
        return syntax(c, "too many string constants");
    *index = c->sconst.n - 1;
    return true;
}

// The words of rex (RFC-0011 section 4). A regex is a view of a program: rx makes it from a
// constant when the source loads, compile while the script runs.
static bool rex_word(comp *c, uint32_t id) {
    fn *f = c->f;
    uint32_t ra;
    if (id == BW_RX || id == BW_RX_COMPILE) {
        if (!need(c, 1))
            return false;
        const entry pat = f->stack[f->depth - 1];
        if (pat.type != TY_STR)
            return type_error(c, "this word takes the pattern as a str");
        if (id == BW_RX_COMPILE) {
            if (!settle(c, f->depth - 1))
                return false;
            ra = slot_of(c, f->depth - 1);
            --f->depth;
            return settle_all(c) && emit(c, PNX_ABC(PNX_XCOMP, top_slot(c, BANK_S, 1), ra, 0)) != NONE &&
                   guard_op(c, TY_REGEX);
        }
        if (!pat.lit)
            return type_error(c, "rx takes a string literal or a const name; a pattern made while running is rex.compile");
        if (!c->rx_buf && !(c->rx_buf = arena_alloc(c, PNX_REX_BUF)))
            return false;
        const pnx_sconst *k = (const pnx_sconst *)c->sconst.ptr + (pat.lit - 1);
        const char       *why = NULL;
        uint32_t          at = 0;
        const uint32_t    n = pnx_rex_compile((const uint8_t *)c->strbytes.ptr + k->offset, k->length, c->rx_buf,
                                              PNX_REX_BUF, &why, &at);
        if (!n)
            return syntax(c, why ? why : "the pattern makes a program too large");
        uint32_t index;
        if (!add_bytes(c, c->rx_buf, n, &index))
            return false;
        --f->depth; // the text gives way to the program
        if (!push_string(c, index))
            return false;
        f->stack[f->depth - 1].type = TY_REGEX;
        f->stack[f->depth - 1].lit = 0;
        return advance(c);
    }
    const uint32_t args = id == BW_RX_REPLACE ? 3 : id == BW_RX_REPLACE_INTO ? 4 : 2;
    const globalsym *g = NULL;
    uint32_t         gi = 0;
    if (id == BW_RX_REPLACE_INTO && !need_coll(c, &g, &gi))
        return false;
    if (!need(c, 2 + (args > 2)))
        return false;
    const uint32_t first = f->depth - 2 - (args > 2);
    if (f->stack[first].type != TY_STR || f->stack[first + 1].type != TY_REGEX ||
        (args > 2 && f->stack[first + 2].type != TY_STR) || (g && g->kind != GK_BUFFER))
        return type_error(c, args > 2 ? "this word takes a str, a regex, the new text (and a buffer): s re new rex.replace"
                                      : "this word takes a str and a regex: s re rex.match");
    for (uint32_t k = first; k < f->depth; ++k) {
        if (!settle(c, k))
            return false;
    }
    const uint32_t base = slot_of(c, first);
    (void)slot_of(c, f->depth - 1);
    if (id == BW_RX_MATCHES || id == BW_RX_SPLIT) // a stem over the string and the program
        return push(c, (entry){.type = TY_STEM, .kind = E_SLOT, .op = id == BW_RX_MATCHES ? STEM_REX : STEM_RSPLIT}) &&
               advance(c);
    f->depth = first;
    if (id == BW_RX_REPLACE) { // the result where s was; the count is not wanted
        if (emit(c, PNX_ABC(PNX_XREPL, top_slot(c, BANK_I, 1), base, base + 1)) == NONE || emit(c, base + 2) == NONE)
            return false;
        f->def_at = NONE;
        return push_slot(c, TY_STR) && advance(c);
    }
    if (!settle_all(c))
        return false;
    f->def_at = NONE;
    if (id == BW_RX_REPLACE_INTO) { // made in the region, then appended; gives the count
        const uint32_t count = top_slot(c, BANK_I, 1);
        return emit(c, PNX_ABC(PNX_XREPL, count, base, base + 1)) != NONE && emit(c, base + 2) != NONE &&
               emit(c, PNX_ABX(PNX_BCAT, base, gi)) != NONE && guard_op(c, TY_INT);
    }
    if (id == BW_RX_MATCH)
        return emit(c, PNX_ABC(PNX_XMATCH, top_slot(c, BANK_S, 1), base, base + 1)) != NONE && guard_op(c, TY_STR);
    if (id == BW_RX_CUT) { // before, match, after: where s and re were, and above
        (void)top_slot(c, BANK_S, 3);
        if (emit(c, PNX_ABC(PNX_XCUT, base, base + 1, base + 2)) == NONE || !advance(c))
            return false;
        sig three = {.n_out = 3, .fails = true};
        three.out[0] = three.out[1] = three.out[2] = TY_STR;
        const uint32_t zero[2] = {0, 0};
        return guard(c, &three, zero, zero, false);
    }
    if (!push_slot(c, TY_BOOL)) // is_match: 0, then 1 when the match is found
        return false;
    const uint32_t r = slot_of(c, f->depth - 1);
    return emit(c, PNX_ABX(PNX_LOADI, r, 0)) != NONE && emit(c, PNX_ABC(PNX_XMATCH, base, base, base + 1)) != NONE &&
           emit(c, PNX_AX(PNX_JMP, 1)) != NONE && emit(c, PNX_ABX(PNX_LOADI, r, 1)) != NONE && advance(c);
}

// The words of ptn that take a pattern (RFC-0009 section 4.3). The pattern is a string
// literal or a const name, checked here; the matcher checks it again when it runs.
static bool ptn_word(comp *c, uint32_t id) {
    fn *f = c->f;
    const uint32_t args = id == BW_PTN_REPLACE ? 3 : 2;
    if (!need(c, args))
        return false;
    for (uint32_t k = 1; k <= args; ++k) {
        if (f->stack[f->depth - k].type != TY_STR)
            return type_error(c, id == BW_PTN_REPLACE ? "replace takes a str, a pattern and the new text: s \"%d+\" \"#\" ptn.replace"
                                                      : "this word takes a str and a pattern: s \"%d+\" ptn.match");
    }
    const entry pat = f->stack[f->depth - args + 1];
    if (!pat.lit)
        return type_error(c, "a pattern is a string literal or a const name, so that it is checked at load");
    const pnx_sconst *k = (const pnx_sconst *)c->sconst.ptr + (pat.lit - 1);
    const char       *why;
    if (!pnx_ptn_check((const uint8_t *)c->strbytes.ptr + k->offset, k->length, &why))
        return syntax(c, why);
    for (uint32_t j = 1; j <= args; ++j) {
        if (!settle(c, f->depth - j))
            return false;
    }
    const uint32_t base = slot_of(c, f->depth - args);
    (void)slot_of(c, f->depth - 1);
    if (id == BW_PTN_MATCHES) // a stem over the string and the pattern
        return push(c, (entry){.type = TY_STEM, .kind = E_SLOT, .op = STEM_PTN}) && advance(c);
    f->depth -= args;
    if (id == BW_PTN_REPLACE)
        return emit(c, PNX_ABC(PNX_PREPL, base, base, base + 1)) != NONE && emit(c, base + 2) != NONE &&
               push_slot(c, TY_STR) && advance(c);
    if (!settle_all(c))
        return false;
    if (id == BW_PTN_MATCH)
        return emit(c, PNX_ABC(PNX_PMATCH, top_slot(c, BANK_S, 1), base, base + 1)) != NONE && guard_op(c, TY_STR);
    // is_match: 0, then 1 when the match is found (the failure word jumps past it)
    if (!push_slot(c, TY_BOOL))
        return false;
    const uint32_t r = slot_of(c, f->depth - 1);
    f->def_at = NONE;
    return emit(c, PNX_ABX(PNX_LOADI, r, 0)) != NONE && emit(c, PNX_ABC(PNX_PMATCH, base, base, base + 1)) != NONE &&
           emit(c, PNX_AX(PNX_JMP, 1)) != NONE && emit(c, PNX_ABX(PNX_LOADI, r, 1)) != NONE && advance(c);
}

// stem sum
static bool do_sum(comp *c) {
    fn *f = c->f;
    if (f->depth == f->floor || bank_of(f->stack[f->depth - 1].type) != BANK_NONE)
        return type_error(c, "sum takes a stem (for example lo hi range sum)");
    uint8_t  kind, cur, lim, hidden;
    uint16_t src;
    tid     element_type;
    if (!stem_begin(c, &kind, &cur, &lim, &src, &hidden, &element_type))
        return false;
    if (element_type != TY_INT)
        return type_error(c, "sum adds int values");
    if (!push_slot(c, TY_INT))
        return false;
    const uint32_t acc = slot_of(c, f->depth - 1), element = top_slot(c, BANK_I, 1);
    uint32_t       exits = NONE;
    if (emit(c, PNX_ABX(PNX_LOADI, acc, 0)) == NONE || !settle_all(c))
        return false;
    const uint32_t head = f->code.n;
    if (emit(c, PNX_ABC(PNX_JLT, cur, lim, 1)) == NONE || !jump_later(c, &exits))
        return false;
    if (kind == STEM_RANGE) {
        if (emit(c, PNX_ABC(PNX_ADD, acc, acc, cur)) == NONE)
            return false;
    } else if (!stem_element(c, kind, element, cur, src) || emit(c, PNX_ABC(PNX_ADD, acc, acc, element)) == NONE) {
        return false;
    }
    if (!loop_back(c, head, cur, 1))
        return false;
    land(c, &exits);
    f->depth -= (uint32_t)hidden + 1;
    return push_slot(c, TY_INT) && move_slot(c, TY_INT, slot_of(c, f->depth - 1), acc);
}

// The word after an instruction that can fail. `out` is the type of its one result, TY_NONE
// when it has none; the instruction wrote the result where the stack will hold it.
static bool guard_op(comp *c, tid out) {
    sig s = {0};
    if (out != TY_NONE) {
        s.n_out = 1;
        s.out[0] = out;
    }
    s.fails = true;
    const uint32_t base[2] = {0, 0}, in[2] = {0, 0};
    return advance(c) && guard(c, &s, base, in, false);
}

// The words the compiler expands itself. The current token is the word; like a call, this
// ends with the token after the word (and after its guard) current.
static bool builtin(comp *c, const struct builtin *w) {
    fn *f = c->f;
    const globalsym *g;
    uint32_t         gi, ra, rb = 0;
    const bool       on_coll = f->depth > f->floor && f->stack[f->depth - 1].type == TY_COLL;
    switch (w->id) {
    case BW_RANGE:
        if (!need(c, 2))
            return false;
        if (f->stack[f->depth - 1].type != TY_INT || f->stack[f->depth - 2].type != TY_INT)
            return type_error(c, "range takes two int values: lo hi range");
        return settle(c, f->depth - 2) && settle(c, f->depth - 1) &&
               push(c, (entry){.type = TY_STEM, .kind = E_SLOT, .op = STEM_RANGE}) && advance(c);
    case BW_BYTES:
        if (!need(c, 1))
            return false;
        if (f->stack[f->depth - 1].type != TY_STR)
            return type_error(c, "bytes takes a str");
        return settle(c, f->depth - 1) && push(c, (entry){.type = TY_STEM, .kind = E_SLOT, .op = STEM_BYTES}) &&
               advance(c);
    case BW_WORDS:
    case BW_CHARS:
        if (!need(c, 1))
            return false;
        if (f->stack[f->depth - 1].type != TY_STR)
            return type_error(c, "this word takes a str");
        return settle(c, f->depth - 1) &&
               push(c, (entry){.type = TY_STEM, .kind = E_SLOT, .op = w->id == BW_CHARS ? STEM_CHARS : STEM_WORDS}) &&
               advance(c);
    case BW_JOIN_INTO:
        return do_join(c);
    case BW_REPLACE_INTO:
        return do_replace_into(c);
    case BW_UPPER:
    case BW_LOWER:
    case BW_IS_UTF8:
    case BW_CHAR_COUNT: { // one str in; a new str in the region, or a bool or a count
        if (!need(c, 1))
            return false;
        if (f->stack[f->depth - 1].type != TY_STR)
            return type_error(c, "this word takes a str");
        if (!reg_of(c, f->depth - 1, &ra))
            return false;
        --f->depth;
        const bool text = w->id == BW_UPPER || w->id == BW_LOWER;
        if (!push_slot(c, text ? TY_STR : w->id == BW_IS_UTF8 ? TY_BOOL : TY_INT))
            return false;
        const uint32_t dst = slot_of(c, f->depth - 1);
        return emit_def(c, text ? PNX_ABC(PNX_RCASE, dst, ra, w->id == BW_LOWER)
                                : PNX_ABC(PNX_SUTF, dst, ra, w->id == BW_CHAR_COUNT),
                        text ? BANK_S : BANK_I) != NONE &&
               advance(c);
    }
    case BW_REPEAT: { // s n repeat : a new str in the region
        if (!need(c, 2))
            return false;
        if (f->stack[f->depth - 2].type != TY_STR || f->stack[f->depth - 1].type != TY_INT)
            return type_error(c, "repeat takes a str and a count: s n str.repeat");
        if (!reg_of(c, f->depth - 2, &ra) || !reg_of(c, f->depth - 1, &rb))
            return false;
        f->depth -= 2;
        if (!push_slot(c, TY_STR))
            return false;
        return emit_def(c, PNX_ABC(PNX_RREP, slot_of(c, f->depth - 1), ra, rb), BANK_S) != NONE && advance(c);
    }
    case BW_TO_INT_BASE: { // s base to_int_base : fails when s is not a number of that base
        if (!need(c, 2))
            return false;
        if (f->stack[f->depth - 2].type != TY_STR || f->stack[f->depth - 1].type != TY_INT)
            return type_error(c, "to_int_base takes a str and a base: s 16 str.to_int_base");
        if (!settle(c, f->depth - 2) || !settle(c, f->depth - 1))
            return false;
        ra = slot_of(c, f->depth - 2);
        rb = slot_of(c, f->depth - 1);
        f->depth -= 2;
        return settle_all(c) && emit(c, PNX_ABC(PNX_TOINTB, top_slot(c, BANK_I, 1), ra, rb)) != NONE &&
               guard_op(c, TY_INT);
    }
    case BW_SPAN: { // s class span : the longest head of bytes of the class, and the rest
        if (!need(c, 2))
            return false;
        const entry class = f->stack[f->depth - 1];
        if (f->stack[f->depth - 2].type != TY_STR || class.type != TY_INT || class.kind != E_CONST ||
            class.value < 1 || class.value > 8)
            return type_error(c, "span takes a str and a class constant: s str.digit str.span");
        --f->depth;
        if (!settle(c, f->depth - 1))
            return false;
        ra = slot_of(c, f->depth - 1);
        f->def_at = NONE;
        return push_slot(c, TY_STR) &&
               emit(c, PNX_ABC(PNX_SSPAN, ra, slot_of(c, f->depth - 1), (uint32_t)class.value)) != NONE && advance(c);
    }
    case BW_RX:
    case BW_RX_COMPILE:
    case BW_RX_IS_MATCH:
    case BW_RX_MATCH:
    case BW_RX_CUT:
    case BW_RX_MATCHES:
    case BW_RX_SPLIT:
    case BW_RX_REPLACE:
    case BW_RX_REPLACE_INTO:
        return rex_word(c, w->id);
    case BW_PTN_IS_MATCH:
    case BW_PTN_MATCH:
    case BW_PTN_MATCHES:
    case BW_PTN_REPLACE:
        return ptn_word(c, w->id);
    case BW_PTN_CAPTURE: { // n capture : capture n of the last match (0: the whole match)
        if (!need(c, 1))
            return false;
        if (f->stack[f->depth - 1].type != TY_INT)
            return type_error(c, "capture takes the number of a capture: 1 ptn.capture");
        if (!reg_of(c, f->depth - 1, &ra))
            return false;
        --f->depth;
        if (!push_slot(c, TY_STR))
            return false;
        return emit_def(c, PNX_ABC(PNX_PCAP, slot_of(c, f->depth - 1), ra, 0), BANK_S) != NONE && advance(c);
    }
    case BW_TRUNCATE: { // n c truncate : a buffer or a list cut back to n; more than it holds is a fault
        const pnx_token at = c->tok;
        if (!need_coll(c, &g, &gi) || !need(c, 1))
            return false;
        if ((g->kind != GK_BUFFER && g->kind != GK_LIST) || f->stack[f->depth - 1].type != TY_INT)
            return type_error(c, "truncate takes a length and a buffer or a list: n c truncate");
        if (g->kind == GK_LIST) { // whether something keeps one of its rows is known when the source is read
            trunc_site *t = vec_push(c, &c->truncs, sizeof *t);
            if (!t)
                return false;
            *t = (trunc_site){gi, at.line, at.column};
        }
        if (!reg_of(c, f->depth - 1, &ra))
            return false;
        --f->depth;
        return emit(c, PNX_ABX(PNX_BTRUNC, ra, gi)) != NONE && advance(c);
    }
    case BW_CRC32:
    case BW_FNV1A:
    case BW_HASH64: { // s crc32 : the hash of the bytes, as an int
        if (!need(c, 1))
            return false;
        if (f->stack[f->depth - 1].type != TY_STR)
            return type_error(c, "this word takes a str");
        if (!reg_of(c, f->depth - 1, &ra))
            return false;
        --f->depth;
        if (!push_slot(c, TY_INT))
            return false;
        const uint32_t kind = w->id == BW_CRC32 ? PNX_HASH_CRC32 : w->id == BW_FNV1A ? PNX_HASH_FNV1A : PNX_HASH_SIP;
        return emit_def(c, PNX_ABC(PNX_SHASH, slot_of(c, f->depth - 1), ra, kind), BANK_I) != NONE && advance(c);
    }
    case BW_BASE64_ENCODE:
    case BW_BASE64_DECODE:
    case BW_HEX_ENCODE:
    case BW_HEX_DECODE:
    case BW_URL_ENCODE:
    case BW_URL_DECODE: { // s buf base64_encode : appended to the buffer; fails with full or format
        if (!need_coll(c, &g, &gi) || !need(c, 1))
            return false;
        if (g->kind != GK_BUFFER || f->stack[f->depth - 1].type != TY_STR)
            return type_error(c, "this word takes a str and a buffer: s buf encode.base64_encode");
        if (!settle(c, f->depth - 1))
            return false;
        ra = slot_of(c, f->depth - 1);
        --f->depth;
        if (!settle_all(c))
            return false;
        const uint32_t kind = (uint32_t)(w->id - BW_BASE64_ENCODE); // in the order of PNX_ENC_...
        return emit_g(c, PNX_BENC, ra, kind, gi) != NONE && guard_op(c, TY_NONE);
    }
    case BW_APPEND_HEX: { // n buf append_hex : fails when the buffer is full
        if (!need_coll(c, &g, &gi) || !need(c, 1))
            return false;
        if (g->kind != GK_BUFFER || f->stack[f->depth - 1].type != TY_INT)
            return type_error(c, "append_hex takes an int and a buffer: n buf fmt.append_hex");
        if (!reg_of(c, f->depth - 1, &ra))
            return false;
        --f->depth;
        if (!settle_all(c))
            return false;
        const uint32_t scratch = top_slot(c, BANK_S, 2) + 1;
        return emit(c, PNX_ABC(PNX_RMARK, scratch, 0, 0)) != NONE && emit(c, PNX_ABC(PNX_RCATX, scratch, ra, 0)) != NONE &&
               emit(c, PNX_ABX(PNX_BCAT, scratch, gi)) != NONE && guard_op(c, TY_NONE);
    }
    case BW_ERR_TEXT: {
        if (!need(c, 1))
            return false;
        if (f->stack[f->depth - 1].type != TY_ERR)
            return type_error(c, "err_text takes an error number");
        if (!reg_of(c, f->depth - 1, &ra))
            return false;
        --f->depth;
        if (!push_slot(c, TY_STR))
            return false;
        return emit_def(c, PNX_ABC(PNX_ERRTEXT, slot_of(c, f->depth - 1), ra, 0), BANK_S) != NONE && advance(c);
    }
    case BW_SUM: return do_sum(c) && advance(c);
    case BW_MINIMUM:
    case BW_MAXIMUM: return do_extreme(c, w->id == BW_MAXIMUM);
    case BW_CALL: { // args block call : calls the word the block value names
        if (!need(c, 1))
            return false;
        const tid type = f->stack[f->depth - 1].type;
        if (!is_block(type))
            return type_error(c, "call takes a block value: args f call");
        const sig bs = *((const sig *)c->btypes.ptr + (type - TY_BLOCK));
        if (!settle(c, f->depth - 1))
            return false;
        const uint32_t which = slot_of(c, f->depth - 1);
        --f->depth;
        uint32_t base[2], in[2], out[2];
        if (!call_args(c, &bs, base) || (bs.fails && !settle_all(c)))
            return false;
        count_sig(&bs, in, out);
        (void)top_slot(c, BANK_I, (in[0] > out[0] ? in[0] : out[0]) + 1);
        (void)top_slot(c, BANK_S, in[1] > out[1] ? in[1] : out[1]);
        const uint32_t expect = in[0] | in[1] << 8 | out[0] << 16 | out[1] << 24 | (bs.fails ? 0x80000000u : 0);
        if (!check_before_call(c) || emit(c, PNX_ABC(PNX_CALLR, base[BANK_I], base[BANK_S], which)) == NONE ||
            emit(c, expect) == NONE || !advance(c))
            return false;
        return bs.fails ? guard(c, &bs, base, in, false) : push_results(c, &bs, base, in, false);
    }
    case BW_MARK: // the top of the region now; release gives back everything made after it
        if (f->marks == UINT8_MAX)
            return syntax(c, "a word makes at most 255 marks");
        ++f->marks;
        if (!push_slot(c, TY_MARK))
            return false;
        f->stack[f->depth - 1].owned = true;
        return emit(c, PNX_ABX(PNX_RTOP, slot_of(c, f->depth - 1), 0)) != NONE && advance(c);
    case BW_RELEASE:
        c->sink = true;
        if (!need(c, 1))
            return false;
        if (f->stack[f->depth - 1].type != TY_MARK)
            return type_error(c, "release takes the mark that mark gave");
        if (!reg_of(c, f->depth - 1, &ra))
            return false;
        if (!give(c, f->depth - 1))
            return type_error(c, "this mark was already released");
        --f->depth;
        return released(c, f->stack[f->depth].born) && emit(c, PNX_ABX(PNX_RCUT, ra, 0)) != NONE && advance(c);
    case BW_REGION_FREE:
        if (!push_slot(c, TY_INT))
            return false;
        return emit_def(c, PNX_ABX(PNX_RFREE, slot_of(c, f->depth - 1), 0), BANK_I) != NONE && advance(c);
    case BW_LINES:
        if (!need(c, 1))
            return false;
        if (f->stack[f->depth - 1].type != TY_STR)
            return type_error(c, "lines takes a str");
        return settle(c, f->depth - 1) && push(c, (entry){.type = TY_STEM, .kind = E_SLOT, .op = STEM_LINES}) &&
               advance(c);
    case BW_SPLIT: { // s sep split : sep is a str, or one byte written as a character
        if (!need(c, 2))
            return false;
        const entry sep = f->stack[f->depth - 1];
        if (f->stack[f->depth - 2].type != TY_STR)
            return type_error(c, "split takes a str and a separator: s sep split");
        if (sep.type == TY_INT) {
            if (sep.kind != E_CONST || sep.value < 0 || sep.value > 255)
                return type_error(c, "a separator given as a number is one byte written as a literal (',')");
            --f->depth;
            return settle(c, f->depth - 1) &&
                   push(c, (entry){.type = TY_STEM, .kind = E_SLOT, .op = STEM_SPLIT_BYTE, .value = sep.value}) &&
                   advance(c);
        }
        if (sep.type != TY_STR)
            return type_error(c, "split takes a str and a separator: s sep split");
        return settle(c, f->depth - 2) && settle(c, f->depth - 1) &&
               push(c, (entry){.type = TY_STEM, .kind = E_SLOT, .op = STEM_SPLIT}) && advance(c);
    }
    case BW_TRIM:
    case BW_TRIM_LEFT:
    case BW_TRIM_RIGHT: {
        if (!need(c, 1))
            return false;
        if (f->stack[f->depth - 1].type != TY_STR)
            return type_error(c, "this word takes a str");
        const uint32_t dst = slot_of(c, f->depth - 1);
        if (!reg_of(c, f->depth - 1, &ra) ||
            emit_def(c, PNX_ABC(PNX_STRIM, dst, ra, w->id == BW_TRIM ? 3 : w->id == BW_TRIM_LEFT ? 1 : 2), BANK_S) == NONE)
            return false;
        f->stack[f->depth - 1].kind = E_SLOT;
        f->stack[f->depth - 1].src = 0;
        return advance(c);
    }
    case BW_SLICE: { // s start n slice : a view; outside the string is a fault
        if (!need(c, 3))
            return false;
        if (f->stack[f->depth - 3].type != TY_STR || f->stack[f->depth - 2].type != TY_INT ||
            f->stack[f->depth - 1].type != TY_INT)
            return type_error(c, "slice takes a str, a start and a count: s start n slice");
        uint32_t rs, start, count;
        if (!settle(c, f->depth - 3) || !reg_of(c, f->depth - 2, &start) || !reg_of(c, f->depth - 1, &count))
            return false;
        rs = slot_of(c, f->depth - 3);
        const uint32_t end = top_slot(c, BANK_I, 2), len = end + 1;
        if (emit(c, PNX_ABC(PNX_ADD, end, start, count)) == NONE || emit(c, PNX_ABC(PNX_SLEN, len, rs, 0)) == NONE ||
            emit(c, PNX_ABC(PNX_JLE, end, len, 1)) == NONE || emit(c, PNX_ABX(PNX_FAULT, 0, PINAX_ERR_RANGE)) == NONE ||
            emit(c, PNX_ABC(PNX_SSKIP, rs, rs, start)) == NONE || emit(c, PNX_ABC(PNX_STAKE, rs, rs, count)) == NONE)
            return false;
        f->depth -= 2;
        f->def_at = NONE;
        return advance(c);
    }
    case BW_CUT:
    case BW_TRY_CUT: { // s sep cut : the parts before and after the first sep
        if (!need(c, 2))
            return false;
        if (w->id == BW_CUT && f->stack[f->depth - 2].type == TY_STR && f->stack[f->depth - 1].type == TY_INT &&
            f->stack[f->depth - 1].kind == E_CONST && f->stack[f->depth - 1].value >= 0 &&
            f->stack[f->depth - 1].value <= 255) { // the separator is one byte, written as a character
            const uint32_t byte = (uint32_t)f->stack[f->depth - 1].value;
            --f->depth;
            if (!settle(c, f->depth - 1))
                return false;
            ra = slot_of(c, f->depth - 1);
            f->def_at = NONE;
            return push_slot(c, TY_STR) && emit(c, PNX_ABC(PNX_SCUT, ra, slot_of(c, f->depth - 1), byte)) != NONE &&
                   advance(c);
        }
        if (f->stack[f->depth - 2].type != TY_STR || f->stack[f->depth - 1].type != TY_STR)
            return type_error(c, "this word takes two str values: s sep cut");
        if (!settle(c, f->depth - 2) || !settle(c, f->depth - 1))
            return false;
        ra = slot_of(c, f->depth - 2);
        rb = slot_of(c, f->depth - 1);
        f->def_at = NONE;
        if (w->id == BW_CUT)
            return emit(c, PNX_ABC(PNX_SCUTS, ra, rb, 0)) != NONE && advance(c);
        f->depth -= 2; // the two results come back to the same two registers
        if (!settle_all(c) || emit(c, PNX_ABC(PNX_SCUTSF, ra, rb, 0)) == NONE || !advance(c))
            return false;
        sig two = {.n_out = 2, .fails = true};
        two.out[0] = two.out[1] = TY_STR;
        const uint32_t base[2] = {0, 0}, in[2] = {0, 0};
        return guard(c, &two, base, in, false);
    }
    case BW_FIND_LAST: {
        if (!need(c, 2))
            return false;
        if (f->stack[f->depth - 2].type != TY_STR || f->stack[f->depth - 1].type != TY_STR)
            return type_error(c, "find_last takes two str values: s part find_last");
        if (!settle(c, f->depth - 2) || !settle(c, f->depth - 1))
            return false;
        ra = slot_of(c, f->depth - 2);
        rb = slot_of(c, f->depth - 1);
        f->depth -= 2;
        return settle_all(c) && emit(c, PNX_ABC(PNX_SFINDL, top_slot(c, BANK_I, 1), ra, rb)) != NONE &&
               guard_op(c, TY_INT);
    }
    case BW_COMPARE: { // s t compare : -1, 0 or 1 by byte order
        if (!need(c, 2))
            return false;
        if (f->stack[f->depth - 2].type != TY_STR || f->stack[f->depth - 1].type != TY_STR)
            return type_error(c, "compare takes two str values");
        if (!reg_of(c, f->depth - 2, &ra) || !reg_of(c, f->depth - 1, &rb))
            return false;
        f->depth -= 2;
        const uint32_t dst = top_slot(c, BANK_I, 1);
        return emit_def(c, PNX_ABC(PNX_SCMP, dst, ra, rb), BANK_I) != NONE && push_slot(c, TY_INT) && advance(c);
    }
    case BW_HEX:
    case BW_BIN:
    case BW_HEX_PAD:
    case BW_ZERO_PAD:
    case BW_PAD_LEFT:
    case BW_PAD_RIGHT: { // text built in the region
        const bool two = w->id != BW_HEX && w->id != BW_BIN;
        const tid  what = w->id == BW_PAD_LEFT || w->id == BW_PAD_RIGHT ? TY_STR : TY_INT;
        if (!need(c, two ? 2 : 1))
            return false;
        if (f->stack[f->depth - (two ? 2 : 1)].type != what || (two && f->stack[f->depth - 1].type != TY_INT))
            return type_error(c, two ? "this word takes a value and a width" : "this word takes an int");
        if (!reg_of(c, f->depth - (two ? 2 : 1), &ra) || (two && !reg_of(c, f->depth - 1, &rb)))
            return false;
        f->depth -= two ? 2 : 1;
        const uint32_t scratch = top_slot(c, BANK_S, 2) + 1;
        const uint32_t op = w->id == BW_HEX ? PNX_ABC(PNX_RCATX, scratch, ra, 0)
                          : w->id == BW_BIN ? PNX_ABC(PNX_RCATX, scratch, ra, 1)
                          : w->id == BW_HEX_PAD ? PNX_ABC(PNX_RCATXW, scratch, ra, rb)
                          : w->id == BW_ZERO_PAD ? PNX_ABC(PNX_RCATW, scratch, ra, rb)
                          : w->id == BW_PAD_LEFT ? PNX_ABC(PNX_RPADL, scratch, ra, rb)
                                                 : PNX_ABC(PNX_RPADR, scratch, ra, rb);
        if (emit(c, PNX_ABC(PNX_RMARK, scratch, 0, 0)) == NONE || emit(c, op) == NONE || !push_slot(c, TY_STR))
            return false;
        return move_slot(c, TY_STR, slot_of(c, f->depth - 1), scratch) && advance(c);
    }
    case BW_LEN:
    case BW_IS_EMPTY: {
        if (on_coll && w->module == 0) {
            if (!need_coll(c, &g, &gi))
                return false;
            if (g->kind == GK_INDEX || g->kind == GK_POOL)
                return type_error(c, g->kind == GK_POOL ? "a pool does not count its rows; this word is not supported for it"
                                                        : "an index has no length; ask its table");
            if (g->kind == GK_BUFFER) { // the length of its contents
                const uint32_t view = top_slot(c, BANK_S, 1), dst = top_slot(c, BANK_I, 1);
                if (emit(c, PNX_ABX(PNX_BVIEW, view, gi)) == NONE || emit_def(c, PNX_ABC(PNX_SLEN, dst, view, 0), BANK_I) == NONE)
                    return false;
                if (w->id == BW_LEN)
                    return push_slot(c, TY_INT) && advance(c);
                f->def_at = NONE;
                return push(c, (entry){.type = TY_BOOL, .kind = E_COND, .op = PNX_JEQI, .a = (uint8_t)dst, .b = 0}) &&
                       advance(c);
            }
            if (g->kind == GK_ARRAY) { // an array is always full
                return push(c, w->id == BW_LEN ? (entry){.type = TY_INT, .kind = E_CONST, .value = g->capacity}
                                               : (entry){.type = TY_BOOL, .kind = E_CONST, .value = 0}) &&
                       advance(c);
            }
            const uint32_t dst = top_slot(c, BANK_I, 1);
            if (emit_def(c, PNX_ABX(PNX_GLEN, dst, gi), BANK_I) == NONE)
                return false;
            if (w->id == BW_LEN)
                return push_slot(c, TY_INT) && advance(c);
            f->def_at = NONE;
            return push(c, (entry){.type = TY_BOOL, .kind = E_COND, .op = PNX_JEQI, .a = (uint8_t)dst, .b = 0}) &&
                   advance(c);
        }
        if (w->module == 0)
            return type_error(c, "this word takes the name of an array, list, queue or buffer; of a str it is str.len");
        if (!need(c, 1))
            return false;
        if (f->stack[f->depth - 1].type != TY_STR)
            return type_error(c, "this word takes a str");
        if (!reg_of(c, f->depth - 1, &ra))
            return false;
        --f->depth;
        const uint32_t dst = top_slot(c, BANK_I, 1);
        if (emit_def(c, PNX_ABC(PNX_SLEN, dst, ra, 0), BANK_I) == NONE)
            return false;
        if (w->id == BW_LEN)
            return push_slot(c, TY_INT) && advance(c);
        f->def_at = NONE;
        return push(c, (entry){.type = TY_BOOL, .kind = E_COND, .op = PNX_JEQI, .a = (uint8_t)dst, .b = 0}) &&
               advance(c);
    }
    case BW_BYTE_AT: {
        if (!need(c, 2))
            return false;
        if (f->stack[f->depth - 2].type != TY_STR || f->stack[f->depth - 1].type != TY_INT)
            return type_error(c, "byte_at takes a str and an index: s i byte_at");
        if (!reg_of(c, f->depth - 2, &ra) || !reg_of(c, f->depth - 1, &rb))
            return false;
        f->depth -= 2;
        const uint32_t dst = top_slot(c, BANK_I, 1);
        return emit_def(c, PNX_ABC(PNX_SBYTE, dst, ra, rb), BANK_I) != NONE && push_slot(c, TY_INT) && advance(c);
    }
    case BW_IDX: { // i names idx
        if (!need_coll(c, &g, &gi) || !need(c, 1))
            return false;
        if (g->kind != GK_ARRAY && g->kind != GK_LIST)
            return type_error(c, "idx reads a cell of an array or a list");
        if (f->stack[f->depth - 1].type != TY_INT)
            return type_error(c, "idx takes an index and the name: i names idx");
        if (!reg_of(c, f->depth - 1, &ra))
            return false;
        --f->depth;
        if (g->strcap) { // a view of the cell's text
            const uint32_t row = top_slot(c, BANK_I, 1);
            if (emit_g(c, PNX_ROW, row, ra, gi) == NONE || !push_slot(c, TY_STR))
                return false;
            return emit_def(c, PNX_ABC(PNX_FGETS, slot_of(c, f->depth - 1), row, 0), BANK_S) != NONE &&
                   emit(c, PNX_FIELD(gi, 0, g->strcap)) != NONE && advance(c);
        }
        const uint32_t dst = top_slot(c, BANK_I, 1);
        return emit_g_def(c, g->record != NONE ? PNX_ROW : PNX_AGET, dst, ra, gi, BANK_I) != NONE &&
               push_slot(c, g->type) && advance(c);
    }
    case BW_ADD_ROW: { // names add_row : a new zeroed row at the end of a list of records
        if (!need_coll(c, &g, &gi))
            return false;
        if (g->kind != GK_LIST || g->record == NONE)
            return type_error(c, "add_row adds to a list of records");
        if (!settle_all(c))
            return false;
        return emit(c, PNX_ABX(PNX_ADDROW, top_slot(c, BANK_I, 1), gi)) != NONE && guard_op(c, g->type);
    }
    case BW_NONE: // names none : the "no row" of that table
        if (!need_coll(c, &g, &gi))
            return false;
        if (g->record == NONE)
            return type_error(c, "none is the missing row of a table of records");
        return push(c, (entry){.type = g->type, .kind = E_CONST, .value = 0}) && advance(c);
    case BW_IS_NONE:
    case BW_ROW_INDEX: {
        if (!need(c, 1))
            return false;
        if (!is_row(f->stack[f->depth - 1].type))
            return type_error(c, "this word takes a row of a table");
        if (!reg_of(c, f->depth - 1, &ra))
            return false;
        --f->depth;
        if (w->id == BW_IS_NONE)
            return push(c, (entry){.type = TY_BOOL, .kind = E_COND, .op = PNX_JEQI, .a = (uint8_t)ra, .b = 0}) &&
                   advance(c);
        const uint32_t dst = top_slot(c, BANK_I, 1);
        return emit_def(c, PNX_ABC(PNX_ADDI, dst, ra, (uint8_t)(int8_t)-1), BANK_I) != NONE && push_slot(c, TY_INT) &&
               advance(c);
    }
    case BW_MAX:
    case BW_MIN: { // the larger or the smaller of two int values
        if (!need(c, 2))
            return false;
        if (f->stack[f->depth - 1].type != f->stack[f->depth - 2].type ||
            base_of(c, f->stack[f->depth - 1].type) != TY_INT)
            return type_error(c, "this word takes two int values, or two of the same declared type");
        if (!settle(c, f->depth - 2) || !reg_of(c, f->depth - 1, &rb))
            return false;
        const uint32_t dst = slot_of(c, f->depth - 2);
        if (emit(c, w->id == BW_MAX ? PNX_ABC(PNX_JLE, rb, dst, 1) : PNX_ABC(PNX_JLE, dst, rb, 1)) == NONE ||
            emit(c, PNX_ABC(PNX_MOVI, dst, rb, 0)) == NONE)
            return false;
        --f->depth;
        return advance(c);
    }
    case BW_CAPACITY:
        return need_coll(c, &g, &gi) && push(c, (entry){.type = TY_INT, .kind = E_CONST, .value = g->capacity}) &&
               advance(c);
    case BW_IS_FULL: {
        if (!need_coll(c, &g, &gi))
            return false;
        if (g->kind != GK_ARRAY && g->kind != GK_LIST && g->kind != GK_QUEUE)
            return type_error(c, "is_full asks an array, a list or a queue");
        if (g->kind == GK_ARRAY)
            return push(c, (entry){.type = TY_BOOL, .kind = E_CONST, .value = 1}) && advance(c);
        const uint32_t dst = top_slot(c, BANK_I, 2);
        if (emit(c, PNX_ABX(PNX_GLEN, dst, gi)) == NONE)
            return false;
        if (fits8(g->capacity))
            return push(c, (entry){.type = TY_BOOL, .kind = E_COND, .op = PNX_JEQI, .a = (uint8_t)dst,
                                   .b = (uint8_t)g->capacity}) && advance(c);
        if (!load_int(c, dst + 1, g->capacity))
            return false;
        f->def_at = NONE;
        return push(c, (entry){.type = TY_BOOL, .kind = E_COND, .op = PNX_JEQ, .a = (uint8_t)dst, .b = (uint8_t)(dst + 1)}) &&
               advance(c);
    }
    case BW_CLEAR:
        if (!need_coll(c, &g, &gi))
            return false;
        if (g->kind == GK_INDEX || g->kind == GK_POOL)
            return type_error(c, g->kind == GK_POOL ? "a pool is emptied by freeing its rows"
                                                    : "an index is not cleared by itself; remove its rows");
        return emit(c, PNX_ABX(g->kind == GK_BUFFER ? PNX_BCLEAR : PNX_GCLEAR, 0, gi)) != NONE && advance(c);
    case BW_APPEND:
    case BW_APPEND_INT:
    case BW_APPEND_BYTE: { // value buf append : fails when the buffer is full
        if (!need_coll(c, &g, &gi) || !need(c, 1))
            return false;
        if (g->kind != GK_BUFFER)
            return type_error(c, "this word appends to a buffer");
        if (f->stack[f->depth - 1].type != (w->id == BW_APPEND ? TY_STR : TY_INT))
            return type_error(c, w->id == BW_APPEND ? "append takes a str" : "this word takes an int");
        if (!settle(c, f->depth - 1))
            return false;
        ra = slot_of(c, f->depth - 1);
        --f->depth;
        if (!settle_all(c))
            return false;
        if (w->id != BW_APPEND && w->id != BW_APPEND_INT)
            return emit_g(c, PNX_BPUT, ra, PNX_W_U8, gi) != NONE && guard_op(c, TY_NONE);
        return emit(c, w->id == BW_APPEND ? PNX_ABX(PNX_BCAT, ra, gi) : PNX_ABX(PNX_BCATI, ra, gi)) != NONE &&
               guard_op(c, TY_NONE);
    }
    case BW_CONTENTS: // buf contents : a view of what the buffer holds now
        if (!need_coll(c, &g, &gi))
            return false;
        if (g->kind != GK_BUFFER)
            return type_error(c, "contents gives the text of a buffer");
        if (!push_slot(c, TY_STR))
            return false;
        return emit_def(c, PNX_ABX(PNX_BVIEW, slot_of(c, f->depth - 1), gi), BANK_S) != NONE && advance(c);
    case BW_ALLOC: { // nodes alloc : a free row of the pool, owned; fails when none is left
        if (!need_coll(c, &g, &gi))
            return false;
        if (g->kind != GK_POOL)
            return type_error(c, "alloc takes a row from a pool");
        if (!settle_all(c))
            return false;
        if (emit(c, PNX_ABX(PNX_PALLOC, top_slot(c, BANK_I, 1), gi)) == NONE || !advance(c))
            return false;
        sig got = {.n_out = 1, .fails = true};
        got.out[0] = g->type;
        got.out_owned[0] = true;
        const uint32_t base[2] = {0, 0}, in[2] = {0, 0};
        return guard(c, &got, base, in, false);
    }
    case BW_FREE:
    case BW_IS_ALIVE: { // row nodes free : gives the row back; using it afterwards is a fault
        if (!need_coll(c, &g, &gi))
            return false;
        c->sink = w->id == BW_FREE;
        if (!need(c, 1))
            return false;
        if (g->kind != GK_POOL || f->stack[f->depth - 1].type != g->type)
            return type_error(c, "this word takes a row of a pool and the pool: row nodes free");
        if (!reg_of(c, f->depth - 1, &ra))
            return false;
        if (w->id == BW_FREE) {
            (void)give(c, f->depth - 1); // an owned row is spent; a row read from a field is simply freed
            --f->depth;
            return emit(c, PNX_ABX(PNX_PFREE, ra, gi)) != NONE && advance(c);
        }
        --f->depth;
        const uint32_t dst = top_slot(c, BANK_I, 1);
        return emit_g_def(c, PNX_PALIVE, dst, ra, gi, BANK_I) != NONE && push_slot(c, TY_BOOL) && advance(c);
    }
    case BW_PUT:        // v jobs put : fails when the queue is full
    case BW_WAIT_PUT: { // v jobs wait_put : yields while it is full; fails when the task was cancelled
        if (!need_coll(c, &g, &gi) || !need(c, 1))
            return false;
        if (g->kind != GK_QUEUE)
            return type_error(c, w->id == BW_PUT ? "put adds to a queue" : "wait_put adds to a queue");
        if (f->stack[f->depth - 1].type != g->type)
            return type_error(c, "the value has another type than the queue holds");
        if (!settle(c, f->depth - 1))
            return false;
        ra = slot_of(c, f->depth - 1);
        --f->depth;
        return settle_all(c) && emit(c, PNX_ABX(w->id == BW_PUT ? PNX_QPUT : PNX_QWPUT, ra, gi)) != NONE &&
               guard_op(c, TY_NONE);
    }
    case BW_WAIT_TAKE: // jobs wait_take : the oldest value, yielding while the queue is empty
        if (!need_coll(c, &g, &gi))
            return false;
        if (g->kind != GK_QUEUE)
            return type_error(c, "wait_take reads the front of a queue");
        return settle_all(c) && emit(c, PNX_ABX(PNX_QWTAKE, top_slot(c, BANK_I, 1), gi)) != NONE && guard_op(c, g->type);
    case BW_PEEK:
    case BW_TAKE:
        if (w->module == 0) { // jobs take : the oldest value; fails when the queue is empty
            if (!need_coll(c, &g, &gi))
                return false;
            if (g->kind != GK_QUEUE)
                return type_error(c, "take and peek read the front of a queue");
            if (!settle_all(c))
                return false;
            return emit(c, PNX_ABX(w->id == BW_TAKE ? PNX_QTAKE : PNX_QPEEK, top_slot(c, BANK_I, 1), gi)) != NONE &&
                   guard_op(c, g->type);
        }
        [[fallthrough]]; // str.take
    case BW_SKIP:
    case BW_TAKE_LAST:
    case BW_SKIP_LAST: { // s n take : a view, as many bytes as there are
        if (!need(c, 2))
            return false;
        if (f->stack[f->depth - 2].type != TY_STR || f->stack[f->depth - 1].type != TY_INT)
            return type_error(c, "this word takes a str and a count: s n take");
        if (!reg_of(c, f->depth - 2, &ra) || !reg_of(c, f->depth - 1, &rb))
            return false;
        const uint32_t dst = slot_of(c, f->depth - 2);
        const uint32_t op = w->id == BW_TAKE ? PNX_STAKE : w->id == BW_SKIP ? PNX_SSKIP
                          : w->id == BW_TAKE_LAST ? PNX_STAKEL : PNX_SSKIPL;
        if (emit_def(c, PNX_ABC(op, dst, ra, rb), BANK_S) == NONE)
            return false;
        f->depth -= 2;
        return push_slot(c, TY_STR) && advance(c);
    }
    case BW_STARTS_WITH:
    case BW_ENDS_WITH:
    case BW_CONTAINS: { // s part starts_with
        if (!need(c, 2))
            return false;
        if (f->stack[f->depth - 2].type != TY_STR || f->stack[f->depth - 1].type != TY_STR)
            return type_error(c, "this word takes two str values: s part starts_with");
        if (!reg_of(c, f->depth - 2, &ra) || !reg_of(c, f->depth - 1, &rb))
            return false;
        f->depth -= 2;
        const uint32_t dst = top_slot(c, BANK_I, 1);
        const uint32_t op = w->id == BW_STARTS_WITH ? PNX_SPRE : w->id == BW_ENDS_WITH ? PNX_SSUF : PNX_SHAS;
        return emit_def(c, PNX_ABC(op, dst, ra, rb), BANK_I) != NONE && push_slot(c, TY_BOOL) && advance(c);
    }
    case BW_REPLACE: { // s old new replace : a new string in the region
        if (!need(c, 3))
            return false;
        for (uint32_t k = 1; k <= 3; ++k) {
            if (f->stack[f->depth - k].type != TY_STR)
                return type_error(c, "replace takes three str values: s old new replace");
        }
        if (!settle(c, f->depth - 3) || !settle(c, f->depth - 2) || !settle(c, f->depth - 1))
            return false;
        const uint32_t base = slot_of(c, f->depth - 3);
        (void)slot_of(c, f->depth - 1);
        f->depth -= 3;
        return emit(c, PNX_ABC(PNX_SREPL, base, base, base + 1)) != NONE && emit(c, base + 2) != NONE &&
               push_slot(c, TY_STR) && advance(c);
    }
    case BW_LOOKUP: { // key index lookup : the row with that key, or failure
        if (!need_coll(c, &g, &gi) || !need(c, 1))
            return false;
        if (g->kind != GK_INDEX)
            return type_error(c, "lookup takes a key and the name of an index: key by_name lookup");
        const field *key = (const field *)c->fields.ptr + g->key;
        if (f->stack[f->depth - 1].type != key->type)
            return type_error(c, "the key has another type than the key field of the index");
        if (!settle(c, f->depth - 1))
            return false;
        ra = slot_of(c, f->depth - 1);
        --f->depth;
        if (!settle_all(c))
            return false;
        const tid row = ((const globalsym *)c->globals.ptr + g->table)->type;
        return emit_g(c, key->is_str ? PNX_IXGETS : PNX_IXGET, top_slot(c, BANK_I, 1), ra, gi) != NONE &&
               guard_op(c, row);
    }
    case BW_INSERT:
    case BW_REMOVE: { // row index insert : fails when the key is there already
        if (!need_coll(c, &g, &gi) || !need(c, 1))
            return false;
        if (g->kind != GK_INDEX)
            return type_error(c, "this word takes a row and the name of an index: row by_name insert");
        if (f->stack[f->depth - 1].type != ((const globalsym *)c->globals.ptr + g->table)->type)
            return type_error(c, "the row belongs to another table than the index");
        if (!settle(c, f->depth - 1))
            return false;
        ra = slot_of(c, f->depth - 1);
        --f->depth;
        if (w->id == BW_REMOVE)
            return emit(c, PNX_ABX(PNX_IXDEL, ra, gi)) != NONE && advance(c);
        return settle_all(c) && emit(c, PNX_ABX(PNX_IXPUT, ra, gi)) != NONE && guard_op(c, TY_NONE);
    }
    case BW_PUSH: { // v names push : fails when the list is full
        if (!need_coll(c, &g, &gi) || !need(c, 1))
            return false;
        if (g->kind != GK_LIST || g->record != NONE)
            return type_error(c, g->record != NONE ? "a list of records grows with add_row"
                                                   : "push adds to a list; an array is always full");
        if (f->stack[f->depth - 1].type != g->type)
            return type_error(c, "the value pushed has another type than the cells of the list");
        if (!settle(c, f->depth - 1))
            return false;
        ra = slot_of(c, f->depth - 1);
        --f->depth;
        return settle_all(c) && emit(c, PNX_ABX(g->strcap ? PNX_LPUSHS : PNX_LPUSH, ra, gi)) != NONE &&
               guard_op(c, TY_NONE);
    }
    // ---- tasks (RFC-0005): a task is a value of the type its set names ----
    case BW_SPAWN: { // block workers spawn : a task that will run the block; fails when the set is full
        if (!need_coll(c, &g, &gi) || !need(c, 1))
            return false;
        const tid type = f->stack[f->depth - 1].type;
        const sig *bs = is_block(type) ? (const sig *)c->btypes.ptr + (type - TY_BLOCK) : NULL;
        if (g->kind != GK_TASKS || !bs || bs->n_in || bs->n_out || bs->fails)
            return type_error(c, "spawn takes a block that takes and gives nothing, and a set of tasks: do ... end workers spawn");
        if (!settle(c, f->depth - 1))
            return false;
        ra = slot_of(c, f->depth - 1);
        --f->depth;
        return settle_all(c) && emit_g(c, PNX_TSPAWN, top_slot(c, BANK_I, 1), ra, gi) != NONE &&
               guard_op(c, row_type(gi));
    }
    case BW_YIELD: // gives the run to whoever resumed this task; fails once the task was cancelled
        return settle_all(c) && emit(c, PNX_ABX(PNX_TYIELD, 0, 0)) != NONE && guard_op(c, TY_NONE);
    case BW_RESUME:
    case BW_IS_DONE:
    case BW_CANCEL: { // t resume : runs the task to its next yield; true when it has ended
        if (!need(c, 1))
            return false;
        const tid type = f->stack[f->depth - 1].type;
        gi = is_row(type) ? (uint32_t)(type - TY_FIRST_DECLARED) : NONE;
        if (gi == NONE || ((const globalsym *)c->globals.ptr + gi)->kind != GK_TASKS)
            return type_error(c, "this word takes a task");
        if (w->id == BW_RESUME && !settle_all(c))
            return false;
        if (!reg_of(c, f->depth - 1, &ra))
            return false;
        --f->depth;
        if (w->id == BW_CANCEL)
            return emit(c, PNX_ABX(PNX_TCANCEL, ra, gi)) != NONE && advance(c);
        const uint32_t dst = top_slot(c, BANK_I, 1);
        return emit_g_def(c, w->id == BW_RESUME ? PNX_TRESUME : PNX_TDONE, dst, ra, gi, BANK_I) != NONE &&
               push_slot(c, TY_BOOL) && advance(c);
    }
    case BW_RUN_ALL: { // workers run_all : resumes the tasks of the set in turn until all have ended
        if (!need_coll(c, &g, &gi))
            return false;
        if (g->kind != GK_TASKS)
            return type_error(c, "run_all takes a set of tasks");
        return settle_all(c) && emit(c, PNX_ABX(PNX_TRUNALL, 0, gi)) != NONE && advance(c);
    }
    case BW_POP: { // names pop : fails when the list is empty
        if (!need_coll(c, &g, &gi))
            return false;
        if (g->kind != GK_LIST || g->record != NONE)
            return type_error(c, "pop takes from a list of values; a list of records has no pop");
        if (!settle_all(c))
            return false;
        if (g->strcap) // the text is copied into the region: the cell is free for the next push
            return emit(c, PNX_ABX(PNX_LPOPS, top_slot(c, BANK_S, 1), gi)) != NONE && guard_op(c, g->type);
        return emit(c, PNX_ABX(PNX_LPOP, top_slot(c, BANK_I, 1), gi)) != NONE && guard_op(c, g->type);
    }
    case BW_FIND:
    case BW_TO_INT: {
        const uint32_t args = w->id == BW_FIND ? 2 : 1;
        if (!need(c, args))
            return false;
        for (uint32_t k = 0; k < args; ++k) {
            if (f->stack[f->depth - 1 - k].type != TY_STR)
                return type_error(c, w->id == BW_FIND ? "find takes two str values: s part find" : "to_int takes a str");
            if (!settle(c, f->depth - 1 - k))
                return false;
        }
        ra = slot_of(c, f->depth - args);
        rb = slot_of(c, f->depth - 1);
        f->depth -= args;
        if (!settle_all(c))
            return false;
        const uint32_t dst = top_slot(c, BANK_I, 1);
        return emit(c, w->id == BW_FIND ? PNX_ABC(PNX_SFIND, dst, ra, rb) : PNX_ABC(PNX_TOINT, dst, ra, 0)) != NONE &&
               guard_op(c, TY_INT);
    }
    case BW_TO_STR: { // the decimal text, in the region
        if (!need(c, 1))
            return false;
        if (f->stack[f->depth - 1].type != TY_INT)
            return type_error(c, "to_str takes an int");
        if (!reg_of(c, f->depth - 1, &ra))
            return false;
        --f->depth;
        const uint32_t scratch = top_slot(c, BANK_S, 2) + 1;
        if (emit(c, PNX_ABC(PNX_RMARK, scratch, 0, 0)) == NONE || emit(c, PNX_ABC(PNX_RCATI, scratch, ra, 0)) == NONE ||
            !push_slot(c, TY_STR))
            return false;
        return move_slot(c, TY_STR, slot_of(c, f->depth - 1), scratch) && advance(c);
    }
    case BW_ASSERT: { // a fault when the value is false
        entry e;
        if (!pop_cond(c, &e))
            return false;
        return cond_jump(c, &e, 1) && emit(c, PNX_ABX(PNX_FAULT, 0, PINAX_ERR_FAILED)) != NONE && advance(c);
    }
    default: return syntax(c, "this word follows the end of its block");
    }
}

// ---- literals -----------------------------------------------------------------------------

// Adds the text of a string token to the constants.
static bool add_string(comp *c, const pnx_token *tp, uint32_t *index) {
    const pnx_token t = *tp;
    const uint32_t offset = c->strbytes.n;
    uint32_t length = 0;
    if (t.body_length) {
        // room first: the decoded text is never longer than the body
        for (uint32_t k = 0; k < t.body_length; ++k) {
            if (!vec_push(c, &c->strbytes, 1))
                return false;
        }
        uint8_t *out = (uint8_t *)c->strbytes.ptr + offset;
        if (t.kind == PNX_T_STRING && t.escapes) {
            length = pnx_lex_decode(c->lx, &t, out);
        } else {
            __builtin_memcpy(out, c->lx->source + t.body, t.body_length);
            length = t.body_length;
        }
        c->strbytes.n = offset + length;
    }
    pnx_sconst *k = vec_push(c, &c->sconst, sizeof(pnx_sconst));
    if (!k)
        return false;
    *k = (pnx_sconst){offset, length};
    if (c->sconst.n - 1 > 0xffffu)
        return syntax(c, "too many string constants");
    *index = c->sconst.n - 1;
    return true;
}

static bool push_string(comp *c, uint32_t index) {
    if (!push_slot(c, TY_STR))
        return false;
    c->f->stack[c->f->depth - 1].born = 0; // the text of a literal is not in the region
    c->f->stack[c->f->depth - 1].lit = (uint16_t)(index + 1);
    return emit_def(c, PNX_ABX(PNX_LOADKS, slot_of(c, c->f->depth - 1), index), BANK_S) != NONE;
}

static bool string_literal(comp *c) {
    uint32_t index;
    return add_string(c, &c->tok, &index) && push_string(c, index);
}

static const struct {
    const char *name;
    int64_t     value;
} err_names[] = {
    {"failed", PINAX_ERR_FAILED},       {"overflow", PINAX_ERR_OVERFLOW}, {"div_zero", PINAX_ERR_DIV_ZERO},
    {"range", PINAX_ERR_RANGE},         {"full", PINAX_ERR_FULL},         {"depth", PINAX_ERR_DEPTH},
    {"stale", PINAX_ERR_STALE},         {"cancelled", PINAX_ERR_CANCELLED},
    {"interrupted", PINAX_ERR_INTERRUPTED}, {"deadlock", PINAX_ERR_DEADLOCK}, {"argument", PINAX_ERR_ARGUMENT},
    {"not_found", PINAX_ERR_NOT_FOUND}, {"format", PINAX_ERR_FORMAT},     {"state", PINAX_ERR_STATE},
};

// err.range, err.not_found ...: the numbers of pinax_err (RFC-0009 section 3.5).
static bool err_constant(name n, int64_t *value) {
    for (size_t k = 0; k < sizeof err_names / sizeof err_names[0]; ++k) {
        if (name_is(n, err_names[k].name)) {
            *value = err_names[k].value;
            return true;
        }
    }
    return false;
}

// ---- one token of a body ------------------------------------------------------------------

// The name of a global in a body: a scalar is read; an array, list, buffer or index is named.
static bool read_global(comp *c, uint32_t g) {
    fn *f = c->f;
    const globalsym *sym = (const globalsym *)c->globals.ptr + g;
    if (sym->kind != GK_SCALAR)
        return push(c, (entry){.type = TY_COLL, .kind = E_SLOT, .value = g}) && advance(c);
    if (sym->strcap) { // stored text: the one row of a table of one
        const uint32_t one = top_slot(c, BANK_I, 1);
        if (!push_slot(c, TY_STR) || emit(c, PNX_ABX(PNX_LOADI, one, 1)) == NONE)
            return false;
        return emit_def(c, PNX_ABC(PNX_FGETS, slot_of(c, f->depth - 1), one, 0), BANK_S) != NONE &&
               emit(c, PNX_FIELD(g, 0, sym->strcap)) != NONE && advance(c);
    }
    if (!push_slot(c, sym->type))
        return false;
    return emit_def(c, PNX_ABX(PNX_GETG, slot_of(c, f->depth - 1), g), BANK_I) != NONE && advance(c);
}

// A word, constant or global that module `m` exports, written m.name here.
static bool module_symbol(comp *c, uint16_t m, name n) {
    const wordsym *words = c->words.ptr;
    for (uint32_t k = 0; k < c->base.words; ++k) {
        if (words[k].module == m && words[k].exported && name_eq(words[k].name, n))
            return call_word(c, &words[k].sig, k);
    }
    const constant *consts = c->consts.ptr;
    for (uint32_t k = 0; k < c->base.consts; ++k) {
        if (consts[k].module == m && consts[k].exported && name_eq(consts[k].name, n)) {
            if (consts[k].type == TY_STR)
                return push_string(c, (uint32_t)consts[k].value) && advance(c);
            return push(c, (entry){.type = consts[k].type, .kind = E_CONST, .value = consts[k].value}) && advance(c);
        }
    }
    const globalsym *globals = c->globals.ptr;
    for (uint32_t k = 0; k < c->base.globals; ++k) {
        if (globals[k].module == m && globals[k].exported && name_eq(globals[k].name, n))
            return read_global(c, k);
    }
    const externsym *externs = c->externs.ptr;
    for (uint32_t k = 0; k < c->base.externs; ++k) {
        if (externs[k].module == m && externs[k].exported && name_eq(externs[k].name, n))
            return call_host(c, externs[k].host);
    }
    return error(c, PINAX_ERR_NOT_FOUND, "the module exports nothing of this name");
}

static bool body_name(comp *c) {
    fn *f = c->f;
    name n = tok_name(c), module = {0};
    if (c->tok.kind == PNX_T_PATH) {
        uint32_t dot = 0;
        while (n.text[dot] != '.')
            ++dot;
        module = (name){n.text, dot};
        n = (name){n.text + dot + 1, n.length - dot - 1};
        if (name_is(module, "err")) {
            int64_t value;
            if (!err_constant(n, &value))
                return error(c, PINAX_ERR_NOT_FOUND, "there is no error number of this name");
            return push(c, (entry){.type = TY_ERR, .kind = E_CONST, .value = value}) && advance(c);
        }
        if (c->data_only)
            return syntax(c, "a data source calls no words and reads no fields; only literals, const names and stores");
        name real;
        if (!module_of(c, module, &real)) // not a module in use: a field of a record
            return field_access(c, false) && advance(c);
        module = real;
        const uint32_t source = source_module(c, module);
        if (source != NONE)
            return module_symbol(c, (uint16_t)source, n);
        const uint8_t standard = standard_module(module);
        if (standard == 1) { // the byte classes of str.span
            static const char *const classes[] = {"digit",      "alpha",      "alnum",     "space",
                                                "upper_case", "lower_case", "hex_digit", "punct"};
            for (uint32_t k = 0; k < sizeof classes / sizeof classes[0]; ++k) {
                if (name_is(n, classes[k]))
                    return push(c, (entry){.type = TY_INT, .kind = E_CONST, .value = k + 1}) && advance(c);
            }
        }
        if (standard == 8 || standard == 9) { // json, csv
            if (c->data_only)
                return syntax(c, "a data source calls no words; only literals, const names and stores");
            return standard == 8 ? json_word(c, n) : csv_word(c, n);
        }
        if (standard == 5) { // bytes: the name says the width
            if (c->data_only)
                return syntax(c, "a data source calls no words; only literals, const names and stores");
            return bytes_word(c, n);
        }
        if (standard) { // a word of a standard module
            if (c->data_only)
                return syntax(c, "a data source calls no words; only literals, const names and stores");
            const struct builtin *bw = find_builtin(n, standard);
            if (!bw)
                return error(c, PINAX_ERR_NOT_FOUND, "the module has no word of this name");
            return builtin(c, bw);
        }
    } else {
        const local *l = find_local(f, n);
        if (l) {
            if (l->state == L_MOVED)
                return type_error(c, "this local gave its owned value away; it holds nothing to read");
            if (l->stale)
                return type_error(c, "this str was made after a mark that was released since; the region may hold "
                                     "other bytes there now");
            if (!push(c, (entry){.type = l->type, .kind = E_LOCAL, .reg = l->reg, .src = (uint8_t)(l - f->locals + 1)}))
                return false;
            f->stack[f->depth - 1].born = l->born;
            return advance(c);
        }
        const constant *k = find_const(c, n);
        if (k) {
            if (k->type == TY_STR)
                return push_string(c, (uint32_t)k->value) && advance(c);
            return push(c, (entry){.type = k->type, .kind = E_CONST, .value = k->value}) && advance(c);
        }
        const uint32_t g = find_global(c, n);
        if (g != NONE)
            return read_global(c, g);
        if (c->data_only)
            return syntax(c, "a data source calls no words; only literals, const names and stores");
        uint32_t index;
        if (find_word(c, n, &index))
            return call_word(c, &((const wordsym *)c->words.ptr + index)->sig, index);
        if ((index = find_extern(c, n)) != NONE)
            return call_host(c, ((const externsym *)c->externs.ptr + index)->host);
        const fwd *ahead = find_fwd(c, n, &index);
        if (ahead)
            return call_word(c, &ahead->sig, FWD_MARK | index);
        const tid made = type_of_name(c, n);
        if (base_of(c, made) != made || name_is(n, "raw")) { // 5 meters ; m raw
            if (!need(c, 1))
                return false;
            entry *e = &f->stack[f->depth - 1];
            if (name_is(n, "raw")) {
                if (base_of(c, e->type) == e->type)
                    return type_error(c, "raw takes a value of a declared type and gives its plain value");
                e->type = base_of(c, e->type);
            } else {
                if (e->type != base_of(c, made))
                    return type_error(c, "a declared type is made from a value of the type it is declared with");
                e->type = made;
            }
            return advance(c);
        }
        const core_word *w = find_core(n);
        if (w)
            return core(c, w);
        const struct builtin *bw = find_builtin(n, 0);
        if (bw)
            return builtin(c, bw);
        // not a name of the core: say so when it is a word of a module, which is written with it
        if (find_builtin(n, 1) || find_builtin(n, 2) || find_builtin(n, 3) || find_builtin(n, 4) || find_builtin(n, 6) ||
            find_builtin(n, 7))
            return error(c, PINAX_ERR_NOT_FOUND, "a word of a module is written with the module: str.len, fmt.hex");
        for (uint32_t k = 0; k < c->st->n_hosts; ++k) {
            if (name_eq(cname(c->st->hosts[k].name), n) && module_used(c, cname(c->st->hosts[k].module)))
                return error(c, PINAX_ERR_NOT_FOUND, "a word of a module is written with the module: io.println");
        }
        return error(c, PINAX_ERR_NOT_FOUND, "this name is not declared before this place");
    }
    if (c->data_only)
        return syntax(c, "a data source calls no words; only literals, const names and stores");
    const uint32_t host = find_host(c, module, n);
    if (host == NONE)
        return error(c, PINAX_ERR_NOT_FOUND, "the module has no word of this name");
    return call_host(c, host);
}

// The stack as it stands, in words, for the listing and for the values a line leaves.
static void stack_text(const comp *c, const fn *f, char *out, size_t cap) {
    size_t n = 0;
    for (uint32_t k = 0; k < f->depth; ++k) {
        if (f->stack[k].hidden)
            continue;
        const tid t = f->stack[k].type;
        const name tn = t == TY_COLL ? cname("(names)") : t == TY_STEM ? cname("(stem)")
                        : is_block(t) ? cname("word") : type_name(c, t);
        if (n + tn.length + 2 >= cap)
            break;
        if (n)
            out[n++] = ' ';
        __builtin_memcpy(out + n, tn.text, tn.length);
        n += tn.length;
    }
    if (!n && cap > 8) {
        __builtin_memcpy(out, "(empty)", 7);
        n = 7;
    }
    out[n] = '\0';
}

// see: one message for every line of a definition, saying what the stack holds after it.
static void see_line(comp *c, uint32_t next_line) {
    if (c->see && c->see_line && c->see_line != next_line && c->st->diag && c->f != c->top_fn) {
        char text[200] = "stack: ";
        stack_text(c, c->f, text + 7, sizeof text - 7);
        const pinax_str file = {(const unsigned char *)c->file, __builtin_strlen(c->file)};
        const pinax_str message = {(const unsigned char *)text, __builtin_strlen(text)};
        c->st->diag(c->st->diag_user, file, c->see_line, 0, message);
    }
    c->see_line = next_line;
}

// ---- defer --------------------------------------------------------------------------------

// Reads the body of a defer up to its end, at the place where the lexer stands. With `keep`
// the code stays (a place that leaves the block); without it everything is taken back and the
// body was only checked (where the defer is written). `taken` gets the locals whose owned
// value the body gives.
static bool defer_body(comp *c, bool keep, uint64_t *taken) {
    fn *f = c->f;
    const uint32_t depth = f->depth, floor = f->floor, n_locals = f->n_locals, n_blocks = f->n_blocks;
    const uint8_t  nloc[2] = {f->nloc[0], f->nloc[1]}, marks = f->marks;
    const uint32_t code_n = f->code.n, lines_n = f->lines.n, dlocals_n = f->dlocals.n, releases = f->releases;
    const uint32_t last_check = f->last_check;
    const bool     dead = f->dead, in_defer = c->in_defer, see = c->see;
    lstate         states[MAX_LOCALS];
    save_owns(f, states);
    f->floor = depth; // the body sees nothing that was on the stack
    f->dead = false;
    c->in_defer = true;
    c->see = false;
    while (!(is_word(c, PNX_KW_end) && f->n_blocks == n_blocks)) {
        if (c->tok.kind == PNX_T_END)
            return syntax(c, "the defer is not closed by end");
        if (!body_token(c))
            return false;
    }
    c->in_defer = in_defer;
    c->see = see;
    if (f->dead)
        return type_error(c, "a defer runs to its end; it does not leave the word or a loop");
    if (!settle_all(c))
        return false;
    if (f->depth != depth)
        return type_error(c, "a defer leaves the stack as it found it");
    *taken = 0;
    for (uint32_t k = 0; k < n_locals; ++k) {
        if (states[k].state == L_OWNS && f->locals[k].state == L_MOVED)
            *taken |= (uint64_t)1 << k;
    }
    for (uint32_t k = n_locals; k < f->n_locals; ++k) { // `or_else (e)` names end with the body
        if (f->locals[k].drec != NONE)
            ((dlocal *)f->dlocals.ptr)[f->locals[k].drec].to = f->code.n;
    }
    f->n_locals = n_locals;
    f->nloc[0] = nloc[0];
    f->nloc[1] = nloc[1];
    f->floor = floor;
    f->dead = dead;
    f->def_at = NONE;
    f->tail_at = NONE;
    if (!keep) {
        f->code.n = code_n;
        f->lines.n = lines_n;
        f->dlocals.n = dlocals_n;
        f->marks = marks;
        f->releases = releases;
        f->last_check = last_check;
        restore_owns(f, states, n_locals);
    }
    return true;
}

// Runs, as code here, the defers of the blocks from `from` inward (0: all of the word), the
// last written first. The owned values they give count as given on this path.
static bool emit_defers(comp *c, uint32_t from) {
    fn *f = c->f;
    for (uint32_t k = f->n_defers; k-- > 0;) {
        const deferred *d = &f->defers[k];
        if (d->block < from)
            continue;
        const pnx_lexer lx = *c->lx;
        const pnx_token tok = c->tok;
        c->lx->at = d->at;
        c->lx->line = d->line;
        c->lx->line_start = d->line_start;
        uint64_t taken;
        if (!advance(c) || !defer_body(c, true, &taken))
            return false;
        *c->lx = lx;
        c->tok = tok;
    }
    return true;
}

// Whether a defer of a block from `from` inward is waiting.
static bool defers_from(const fn *f, uint32_t from) {
    return f->n_defers && f->defers[f->n_defers - 1].block >= from;
}

// defer ... end : written right in a word's body, a loop's body or a do block.
static bool do_defer(comp *c) {
    fn *f = c->f;
    if (f->is_top)
        return syntax(c, "defer belongs inside a definition");
    if (f->n_blocks && f->blocks[f->n_blocks - 1].kind != B_LOOP && f->blocks[f->n_blocks - 1].kind != B_DO)
        return syntax(c, "defer is written right in a word's body, a loop's body or a do block, not in a branch");
    if (f->n_defers == MAX_DEFERS)
        return syntax(c, "a word has at most 16 defers");
    if (!advance(c))
        return false;
    deferred *d = &f->defers[f->n_defers];
    *d = (deferred){.at = c->tok.start, .line = c->tok.line, .line_start = c->tok.start - (c->tok.column - 1),
                    .block = f->n_blocks};
    uint64_t taken;
    if (!defer_body(c, false, &taken))
        return false;
    for (uint32_t k = 0; k < f->n_locals; ++k) {
        if (taken >> k & 1u)
            f->locals[k].deferred = true;
    }
    ++f->n_defers;
    return advance(c); // past the end of the defer
}

static bool body_token(comp *c) {
    fn *f = c->f;
    if (c->see)
        see_line(c, c->tok.line);
    const pnx_token t = c->tok;
    if (f->n_blocks && f->blocks[f->n_blocks - 1].kind == B_CASE)
        return case_item(c);
    if (c->data_only && t.kind == PNX_T_RESERVED && t.word != PNX_KW_true && t.word != PNX_KW_false &&
        t.word != PNX_KW_to && t.word != PNX_KW_set)
        return syntax(c, "a data source holds declarations, literals, stores and rows; this word is not allowed");
    const bool closer = t.kind == PNX_T_RESERVED && (t.word == PNX_KW_end || t.word == PNX_KW_else);
    if (f->dead && !closer)
        return syntax(c, "this code cannot be reached: the code before it always leaves");
    // a stem is taken at once by its block or by a word like sum
    if (f->depth > f->floor && f->stack[f->depth - 1].type == TY_STEM &&
        !(t.kind == PNX_T_RESERVED && t.word == PNX_KW_do) &&
        !(t.kind == PNX_T_NAME && (name_is(tok_name(c), "sum") || name_is(tok_name(c), "minimum") ||
                                   name_is(tok_name(c), "maximum"))) &&
        !(t.kind == PNX_T_PATH && ends_with_word(tok_name(c), "join_into")))
        return type_error(c, "a stem is not a value: a block or a combining word takes it at once");
    // a pending condition survives only into the word that consumes it
    if (f->depth && f->stack[f->depth - 1].kind == E_COND) {
        const bool consumer = (t.kind == PNX_T_RESERVED && (t.word == PNX_KW_if || t.word == PNX_KW_while ||
                                                            t.word == PNX_KW_until)) ||
                              (t.kind == PNX_T_NAME && name_is(tok_name(c), "not"));
        if (!consumer && !settle(c, f->depth - 1))
            return false;
    }
    if (c->eval && t.kind == PNX_T_NAME && name_is(tok_name(c), "stack") && !find_local(f, tok_name(c)) &&
        !name_is_global(c, tok_name(c))) {
        // stack : a tool word of an interactive session (RFC-0009 section 6); it tells the types
        // on the stack at this place, as tool output (line 0 of the diagnostics)
        if (c->st->diag) {
            char text[200] = "stack: ";
            stack_text(c, f, text + 7, sizeof text - 7);
            const pinax_str file = {(const unsigned char *)c->file, __builtin_strlen(c->file)};
            const pinax_str message = {(const unsigned char *)text, __builtin_strlen(text)};
            c->st->diag(c->st->diag_user, file, 0, 0, message);
        }
        return advance(c);
    }
    switch (t.kind) {
    case PNX_T_INT:
    case PNX_T_CHAR:
        return push(c, (entry){.type = TY_INT, .kind = E_CONST, .value = t.integer}) && advance(c);
    case PNX_T_FLOAT: return type_error(c, "float is not available: this build has no F bank");
    case PNX_T_STRING:
    case PNX_T_RAW:
    case PNX_T_TEXT: return string_literal(c) && advance(c);
    case PNX_T_NAME:
    case PNX_T_PATH: return body_name(c);
    case PNX_T_RESERVED: break;
    default: return syntax(c, "this token does not belong here");
    }
    if (c->in_defer && (t.word == PNX_KW_return || t.word == PNX_KW_fail || t.word == PNX_KW_break ||
                        t.word == PNX_KW_while || t.word == PNX_KW_until || t.word == PNX_KW_to || t.word == PNX_KW_set ||
                        t.word == PNX_KW_loop ||
                        t.word == PNX_KW_do || t.word == PNX_KW_defer || t.word == PNX_KW_case))
        return syntax(c, "a defer holds no return, fail, break, while, until, to, set, loop, do, case or defer");
    switch (t.word) {
    case PNX_KW_defer: return do_defer(c);
    case PNX_KW_true:
    case PNX_KW_false:
        return push(c, (entry){.type = TY_BOOL, .kind = E_CONST, .value = t.word == PNX_KW_true}) && advance(c);
    case PNX_KW_or: {
        const core_word *w = find_core(cname("or"));
        return core(c, w);
    }
    case PNX_KW_to: return store(c);
    case PNX_KW_set: return set_global(c);
    case PNX_KW_if: return do_if(c);
    case PNX_KW_else: return do_else(c);
    case PNX_KW_loop: return do_loop(c);
    case PNX_KW_while:
    case PNX_KW_until:
    case PNX_KW_break: return do_leave(c, t.word);
    case PNX_KW_return:
        if (f->is_top)
            return syntax(c, "return belongs inside a definition");
        f->dead = true;
        return emit_return(c) && advance(c);
    case PNX_KW_fail: {
        if (f->is_top || !f->sig->fails)
            return type_error(c, "fail needs `or fail` in the signature of this word");
        if (!need(c, 1))
            return false;
        entry *e = &f->stack[f->depth - 1];
        if (e->type != TY_ERR)
            return type_error(c, "fail takes an error number (err.not_found, or the e of or_else)");
        if (f->n_defers) { // the error is kept in its register while the defers run above it
            if (!settle(c, f->depth - 1) || !emit_defers(c, 0))
                return false;
            e = &f->stack[f->depth - 1];
        }
        if (!nothing_owned(c))
            return false;
        if (e->kind == E_CONST) {
            if (emit(c, PNX_ABX(PNX_FAIL, 0, (uint32_t)e->value)) == NONE)
                return false;
        } else {
            uint32_t reg;
            if (!reg_of(c, f->depth - 1, &reg) || emit(c, PNX_ABC(PNX_FAILR, reg, 0, 0)) == NONE)
                return false;
        }
        --f->depth;
        f->dead = true;
        return advance(c);
    }
    case PNX_KW_or_fail:
    case PNX_KW_or_return:
    case PNX_KW_or_else: return syntax(c, "this word follows a word that can fail, and nothing else");
    case PNX_KW_case: return do_case(c);
    case PNX_KW_cat: return do_cat(c);
    case PNX_KW_do: return do_block(c);
    case PNX_KW_end: {
        if (f->n_blocks == 0)
            return syntax(c, "end without anything open");
        block *b = &f->blocks[f->n_blocks - 1];
        switch (b->kind) {
        case B_IF: return end_if(c, b) && advance(c);
        case B_LOOP: return end_loop(c, b) && advance(c);
        case B_ORELSE: return end_orelse(c, b) && advance(c);
        case B_OF: return end_of(c, b) && advance(c);
        default: return end_do(c, b);
        }
    }
    default: return syntax(c, "this construct is not supported by the compiler yet");
    }
}

// ---- definitions --------------------------------------------------------------------------

static bool remap(comp *c, uint32_t kind, uint32_t *value, const uint8_t locals[2]) {
    if (kind != PNX_K_I && kind != PNX_K_S)
        return true;
    if (*value >= SLOT0) {
        *value = *value - SLOT0 + locals[kind == PNX_K_S];
        if (*value > 255)
            return syntax(c, "this word needs more than 256 registers of one bank; split it");
    }
    return true;
}

// Rewrites the virtual register numbers of one body now that its local count is known.
static bool finish_registers(comp *c, fn *f, uint16_t cells[2]) {
    for (uint32_t i = 0; i < f->code.n;) {
        uint32_t *w = code_at(f, i);
        const uint32_t op = *w & 0xffu;
        const pnx_opinfo info = pnx_ops_info[op];
        uint32_t a = (*w >> 8) & 0xffu, b = (*w >> 16) & 0xffu, cc = *w >> 24;
        if (op == PNX_CALL || op == PNX_HCALL || op == PNX_HCALLF || op == PNX_CALLR) {
            if (!remap(c, PNX_K_I, &a, f->maxloc) || !remap(c, PNX_K_S, &b, f->maxloc) ||
                (op == PNX_CALLR && !remap(c, PNX_K_I, &cc, f->maxloc)))
                return false;
            *w = PNX_ABC(op, a, b, cc);
        } else if (info.shape == PNX_F_ABC) {
            if (!remap(c, info.a, &a, f->maxloc) || !remap(c, info.b, &b, f->maxloc) ||
                !remap(c, info.c, &cc, f->maxloc))
                return false;
            *w = PNX_ABC(op, a, b, cc);
        } else if (info.shape != PNX_F_AX) {
            if (!remap(c, info.a, &a, f->maxloc))
                return false;
            *w = (*w & ~0xff00u) | a << 8;
        }
        if (info.second == PNX_X_SREG) { // the second word names a register too
            uint32_t x = w[1];
            if (!remap(c, PNX_K_S, &x, f->maxloc))
                return false;
            w[1] = x;
        }
        i += info.second ? 2 : 1;
    }
    for (int bank = 0; bank < 2; ++bank) {
        const uint32_t n = (uint32_t)f->maxloc[bank] + f->maxslot[bank];
        if (n > 256)
            return syntax(c, "this word needs more than 256 registers of one bank; split it");
        cells[bank] = (uint16_t)n;
    }
    return true;
}

static void fn_reset(fn *f, bool is_top) {
    const vec code = f->code, lines = f->lines;
    __builtin_memset(f, 0, sizeof *f);
    f->code = code;
    f->code.n = 0;
    f->lines = lines;
    f->lines.n = 0;
    f->def_at = NONE;
    f->tail_at = NONE;
    f->is_top = is_top;
}

// The inputs of the body in c->f are the first registers of its window: named ones become
// locals, bare ones start on the stack and are read from those registers.
static bool bind_inputs(comp *c, const sig *s) {
    fn *f = c->f;
    // a word that takes a mark cannot tell whether its str inputs were made after it: they
    // count as made after it, so that releasing the mark takes them too
    for (uint32_t k = 0; k < s->n_in; ++k)
        f->marks = s->in[k] == TY_MARK ? 1 : f->marks;
    for (uint32_t k = 0; k < s->n_in; ++k) {
        const int bank = bank_of(s->in[k]);
        local *l = &f->locals[f->n_locals++];
        *l = (local){.name = sig_in_name(s, k), .type = s->in[k], .reg = f->nloc[bank]++,
                     .state = s->in_owned[k] ? L_OWNS : L_PLAIN, .born = f->marks};
        if (!debug_local(c, l, 0))
            return false;
        if (l->name.length) {
            for (uint32_t j = 0; j + 1 < f->n_locals; ++j) {
                if (f->locals[j].name.length && name_eq(f->locals[j].name, l->name))
                    return type_error(c, "two parameters have the same name");
            }
            const name pn = l->name;
            l->name.length = 0; // not yet visible to the check below
            const bool free_name = name_is_free(c, pn);
            l->name = pn;
            if (!free_name)
                return false;
        } else if (!push(c, (entry){.type = l->type, .kind = E_LOCAL, .reg = l->reg, .src = (uint8_t)f->n_locals})) {
            return false;
        }
    }
    f->maxloc[0] = f->nloc[0];
    f->maxloc[1] = f->nloc[1];
    return true;
}

// The registers of the finished body in `f` and the size of its window, into word `index`.
static void put_text(char *out, size_t cap, name n);

static bool finish_word(comp *c, fn *f, uint32_t index) {
    for (uint32_t k = 0; k < f->dlocals.n; ++k) { // the debug records of its locals
        const dlocal *d = (const dlocal *)f->dlocals.ptr + k;
        const uint32_t to = d->to == NONE || d->to > f->code.n ? f->code.n : d->to;
        if (d->from >= to)
            continue;
        pnx_localrec *rec = vec_push(c, &c->localrecs, sizeof(pnx_localrec));
        if (!rec)
            return false;
        const tid base = base_of(c, d->type);
        *rec = (pnx_localrec){.word = index, .from = d->from, .to = to, .bank = (uint8_t)(bank_of(d->type) == BANK_S),
                              .reg = d->reg,
                              .kind = bank_of(d->type) == BANK_S ? PINAX_VALUE_STR : base == TY_BOOL ? PINAX_VALUE_BOOL
                                    : base == TY_ERR ? PINAX_VALUE_ERR : PINAX_VALUE_INT};
        put_text(rec->name, sizeof rec->name, d->name);
        put_text(rec->type, sizeof rec->type, type_name(c, d->type));
    }
    wordsym *w = (wordsym *)c->words.ptr + index;
    uint32_t in[2], out[2];
    count_sig(&w->sig, in, out);
    if (!finish_registers(c, f, w->cells))
        return false;
    for (int bank = 0; bank < 2; ++bank) { // the loader wants room for arguments and results
        const uint32_t least = in[bank] > out[bank] ? in[bank] : out[bank];
        if (w->cells[bank] < least)
            w->cells[bank] = (uint16_t)least;
    }
    return true;
}

// do ( effect ) body end , not followed by a combining word: a block that is a value. It
// becomes a word of its own, sees no local of the code around it, and its number is the value.
static bool value_block(comp *c, const sig *bs, uint32_t line, uint32_t column) {
    if (c->data_only)
        return error_at(c, PINAX_ERR_SYNTAX, line, column, "a data source holds no blocks");
    if (c->nested == MAX_NESTED)
        return error_at(c, PINAX_ERR_SYNTAX, line, column, "value blocks are nested too deeply");
    fn *outer = c->f, *f = c->block_fn[c->nested];
    if (!f) {
        f = c->block_fn[c->nested] = arena_alloc(c, sizeof(fn));
        if (!f)
            return false;
    }
    wordsym *w = vec_push(c, &c->words, sizeof(wordsym));
    if (!w)
        return false;
    const uint32_t index = c->words.n - 1;
    __builtin_memset(w, 0, sizeof *w);
    w->name = cname("(block)");
    w->sig = *bs;
    tid type;
    if (!block_type(c, bs, &type))
        return false;
    fn_reset(f, false);
    f->own_sig = *bs;
    f->sig = &f->own_sig;
    c->f = f;
    ++c->nested;
    if (!bind_inputs(c, &f->own_sig))
        return false;
    while (!(is_word(c, PNX_KW_end) && f->n_blocks == 0)) {
        if (c->tok.kind == PNX_T_END)
            return syntax(c, "the block is not closed by end");
        if (!body_token(c))
            return false;
    }
    if (!f->dead && !emit_return(c))
        return false;
    if (!finish_word(c, f, index))
        return false;
    // keep the code until the word around it is stored
    pending_code *keep = vec_push(c, &c->pending, sizeof(pending_code));
    uint32_t *copy = keep ? arena_alloc(c, (size_t)f->code.n * 8 + 8) : NULL;
    if (!copy)
        return false;
    __builtin_memcpy(copy, f->code.ptr, (size_t)f->code.n * 4);
    __builtin_memcpy(copy + f->code.n, f->lines.ptr, (size_t)f->code.n * 4);
    *keep = (pending_code){index, f->code.n, copy, copy + f->code.n};
    --c->nested;
    c->f = outer;
    return push(c, (entry){.type = type, .kind = E_CONST, .value = index}) && advance(c); // past the end
}

// word NAME ( effect ) body end        test NAME body end
// A test is a word without inputs and results, stored under the name "test.NAME" so that it
// cannot be called from the source and the tool that runs tests can find it.
static bool definition(comp *c, bool is_test) {
    if (c->top_fn->n_blocks)
        return syntax(c, "a definition does not belong inside a block");
    if (!advance(c))
        return false;
    if (c->tok.kind != PNX_T_NAME)
        return syntax(c, is_test ? "test is followed by the name of the test" : "word is followed by the name of the new word");
    name n = tok_name(c);
    if (is_test) {
        if (n.length > 26)
            return syntax(c, "a test name is at most 26 bytes long");
        uint8_t *text = arena_alloc(c, n.length + 5);
        if (!text)
            return false;
        __builtin_memcpy(text, "test.", 5);
        __builtin_memcpy(text + 5, n.text, n.length);
        n = (name){text, n.length + 5};
        if (find_word(c, n, NULL))
            return type_error(c, "a test of this name is already defined");
    } else if (c->redefining) {
        if (pnx_tasks_alive(c->st)) // the code of the words moves; a task's places in it would not
            return error(c, PINAX_ERR_STATE, "redefine waits until no task is alive (run_all, or cancel them)");
        uint32_t old;
        if (!find_word(c, n, &old) || old >= c->base.words)
            return type_error(c, "redefine replaces a word that an earlier line of the session defined");
        if (c->redefine_old != NONE)
            return syntax(c, "one redefine in a text");
        c->redefine_old = old;
        n = cname("(replaced)");
    } else if (name_is_global(c, n) && !(find_fwd(c, n, NULL) && find_fwd(c, n, NULL)->index == NONE)) {
        return type_error(c, c->eval && find_word(c, n, NULL) ? "this name is already defined; redefine word replaces it"
                                                              : "this name is already defined");
    }
    if (n.length > 31)
        return syntax(c, "a word name is at most 31 bytes long");
    if (!advance(c))
        return false;
    wordsym *w = vec_push(c, &c->words, sizeof(wordsym));
    if (!w)
        return false;
    __builtin_memset(w, 0, sizeof *w);
    w->name = n;
    c->def_index = c->words.n - 1;
    if (!is_test && !parse_sig(c, &w->sig))
        return false;
    char text[72];
    if (!sig_text(c, &w->sig, text, sizeof text))
        return syntax(c, "the signature is longer than 71 bytes");
    if (c->redefining) { // the code that calls the word was checked against this signature
        c->redefining = false;
        if (__builtin_strcmp(c->st->words[c->redefine_old].signature, text) != 0)
            return error(c, PINAX_ERR_SIGNATURE, "redefine keeps the signature of the word it replaces");
        c->redefine_new = c->def_index;
    }
    fwd *ahead = is_test ? NULL : find_fwd(c, n, NULL);
    if (ahead) { // the definition a forward declaration promised
        char promised[72];
        if (!sig_text(c, &ahead->sig, promised, sizeof promised) || __builtin_strcmp(promised, text) != 0)
            return type_error(c, "the definition has another signature than its forward declaration");
        ahead->index = c->def_index;
        if (ahead->exported)
            ((wordsym *)c->words.ptr + c->def_index)->exported = true;
    }

    fn *f = c->word_fn;
    fn_reset(f, false);
    f->own_sig = w->sig; // the word table may move while blocks inside this word are compiled
    f->sig = &f->own_sig;
    c->f = f;
    if (!bind_inputs(c, &f->own_sig))
        return false;

    while (!(is_word(c, PNX_KW_end) && f->n_blocks == 0)) {
        if (c->tok.kind == PNX_T_END)
            return syntax(c, "the definition is not closed by end");
        if (!body_token(c))
            return false;
    }
    if (c->see)
        see_line(c, 0); // the last line of the body
    if (!f->dead && !emit_return(c))
        return false;
    if (!finish_word(c, f, c->def_index))
        return false;
    c->f = c->top_fn;
    return advance(c);
}

static bool use_module(comp *c) {
    if (c->data_only)
        return syntax(c, "a data source defines no words and uses no modules");
    if (!advance(c))
        return false;
    if (c->tok.kind != PNX_T_NAME)
        return syntax(c, "use is followed by the name of a module");
    const name n = tok_name(c);
    bool known = standard_module(n) != 0; // standard modules: part of the language
    for (uint32_t k = 0; !known && k < c->st->n_hosts; ++k)
        known = name_eq(cname(c->st->hosts[k].module), n);
    if (!known && source_module(c, n) == NONE)
        return error(c, PINAX_ERR_NOT_FOUND, name_is(n, "strings") || name_is(n, "format")
                                                 ? "the standard modules are named str and fmt"
                                                 : "no module of this name is registered or loaded");
    if (!advance(c))
        return false;
    name prefix = n;
    if (is_word(c, PNX_KW_as)) { // use rex as rx : the source writes rx.match
        if (!advance(c))
            return false;
        if (c->tok.kind != PNX_T_NAME)
            return syntax(c, "as is followed by the name the module gets here: use rex as rx");
        prefix = tok_name(c);
        if (!advance(c))
            return false;
    }
    name other;
    if (module_of(c, prefix, &other))
        return name_eq(other, n) ? true : type_error(c, "this name already stands for another module");
    if (name_is(prefix, "err") || find_record_any(c, prefix) != NONE)
        return type_error(c, "this name is already the first part of other paths (err, or a record)");
    if (c->n_modules == MAX_MODULES)
        return syntax(c, "too many modules in use");
    c->modules[c->n_modules] = prefix;
    c->module_real[c->n_modules++] = n;
    return true;
}

// extern NAME ( effect ) from "lib" [as LOCAL] : a C function, called as RFC-0015 section 4.3
// says. With `as`, the script calls it LOCAL; then NAME may be any C name, a reserved or core
// word among them (`extern drop (...) from "c" as c_drop`), or a quoted name for one the lexer
// would not read as a name (`extern "note" (...) from "c" as c_note`).
static bool declare_extern(comp *c) {
    if (!advance(c))
        return false;
    const bool quoted = c->tok.kind == PNX_T_STRING;
    if (c->tok.kind != PNX_T_NAME && c->tok.kind != PNX_T_RESERVED && !quoted)
        return syntax(c, "extern names the function: extern name (effect) from \"library\" [as name]");
    if (quoted && (c->tok.body_length == 0 || c->tok.escapes))
        return syntax(c, "a quoted C function name is plain bytes");
    const name symbol = quoted ? (name){c->lx->source + c->tok.body, c->tok.body_length} : tok_name(c);
    const pnx_token at    = c->tok;
    if (symbol.length > 31)
        return syntax(c, "a C function name is at most 31 bytes long");
    sig s;
    c->cstr_mask = 0;
    c->in_extern = true;
    char text[72];
    const bool ok = advance(c) && parse_sig(c, &s) && (sig_text(c, &s, text, sizeof text) ||
                                                      syntax(c, "the signature is longer than 71 bytes"));
    c->in_extern = false;
    if (!ok)
        return false;
    if (!is_word(c, PNX_KW_from) || !advance(c) || (c->tok.kind != PNX_T_STRING && c->tok.kind != PNX_T_RAW))
        return c->err == PINAX_OK ? syntax(c, "extern ends with the library: from \"name\"") : false;
    if (c->tok.body_length == 0 || c->tok.body_length > 14 || c->tok.escapes)
        return syntax(c, "the library name is 1 to 14 plain bytes");
    char module[16] = "@", word[32] = {0};
    __builtin_memcpy(module + 1, c->lx->source + c->tok.body, c->tok.body_length);
    __builtin_memcpy(word, symbol.text, symbol.length);
    if (!advance(c))
        return false;
    name n = symbol;
    pnx_token named = at;
    if (is_word(c, PNX_KW_as)) {
        if (!advance(c) || c->tok.kind != PNX_T_NAME)
            return c->err == PINAX_OK ? syntax(c, "`as` is followed by the name the script calls the function") : false;
        n = tok_name(c);
        named = c->tok;
        if (!advance(c))
            return false;
    } else if (at.kind != PNX_T_NAME) {
        return error_at(c, PINAX_ERR_SYNTAX, at.line, at.column,
                        "this C function has the name of a reserved word: give it another with `as`");
    }
    if (name_is_global(c, n))
        return error_at(c, PINAX_ERR_TYPE, named.line, named.column,
                        n.text == symbol.text ? "this name is already defined: give the function another with `as`"
                                              : "this name is already defined");
    if (n.length > 31)
        return error_at(c, PINAX_ERR_SYNTAX, named.line, named.column, "a word name is at most 31 bytes long");
    uint32_t host;
    const pinax_err err = pnx_extern_link(c->st, module, word, text, &host);
    if (err != PINAX_OK)
        return error_at(c, err, at.line, at.column, c->st->error_text);
    externsym *slot = vec_push(c, &c->externs, sizeof(externsym));
    if (!slot)
        return false;
    *slot = (externsym){.name = n, .host = host};
    return true;
}

static bool declared_size(comp *c, uint32_t *out);

// tasks NAME count [depth [region]] : a set of tasks (RFC-0005). Each has its own registers
// for `depth` levels of calls and its own `region` bytes for the strings it builds.
static bool declare_tasks(comp *c) {
    if (!advance(c))
        return false;
    if (c->tok.kind != PNX_T_NAME)
        return syntax(c, "the declaration is: tasks name count");
    globalsym g = {.name = tok_name(c), .kind = GK_TASKS, .record = NONE, .table = 16, .key = 1024};
    if (name_is_global(c, g.name))
        return type_error(c, "this name is already defined");
    if (g.name.length > 31)
        return syntax(c, "a global name is at most 31 bytes long");
    if (!advance(c) || !declared_size(c, &g.capacity))
        return false;
    if (c->tok.kind == PNX_T_INT) { // the call depth of a task
        if (c->tok.integer < 1 || c->tok.integer > 4096)
            return syntax(c, "the call depth of a task is 1 to 4096");
        g.table = (uint32_t)c->tok.integer;
        if (!advance(c))
            return false;
        if (c->tok.kind == PNX_T_INT) { // the bytes of its region
            if (c->tok.integer < 0 || c->tok.integer > (1 << 24))
                return syntax(c, "the region of a task is at most 16 MiB");
            g.key = (uint32_t)c->tok.integer;
            if (!advance(c))
                return false;
        }
    }
    if (g.capacity > 4096)
        return syntax(c, "a set holds at most 4096 tasks");
    if (c->globals.n >= 0xfffu)
        return syntax(c, "too many globals");
    globalsym *slot = vec_push(c, &c->globals, sizeof(globalsym));
    if (!slot)
        return false;
    g.type = row_type((uint32_t)(slot - (globalsym *)c->globals.ptr));
    *slot = g;
    return true;
}

// const NAME literal
static bool declare_const(comp *c) {
    if (!advance(c))
        return false;
    if (c->tok.kind != PNX_T_NAME)
        return syntax(c, "const is followed by a name and a literal");
    constant k = {.name = tok_name(c)};
    if (name_is_global(c, k.name))
        return type_error(c, "this name is already defined");
    if (!advance(c))
        return false;
    const name value = tok_name(c);
    const constant *other;
    switch (c->tok.kind) {
    case PNX_T_INT:
    case PNX_T_CHAR:
        k.type = TY_INT;
        k.value = c->tok.integer;
        break;
    case PNX_T_STRING:
    case PNX_T_RAW:
    case PNX_T_TEXT: {
        uint32_t index;
        if (!add_string(c, &c->tok, &index))
            return false;
        k.type = TY_STR;
        k.value = index;
        break;
    }
    case PNX_T_NAME:
        other = find_const(c, value);
        if (!other)
            return type_error(c, "the value of a const is a literal or another const");
        k.type = other->type;
        k.value = other->value;
        break;
    case PNX_T_PATH:
        if (value.length > 4 && __builtin_memcmp(value.text, "err.", 4) == 0 &&
            err_constant((name){value.text + 4, value.length - 4}, &k.value)) {
            k.type = TY_ERR;
            break;
        }
        return type_error(c, "the value of a const is a literal or another const");
    case PNX_T_RESERVED:
        if (c->tok.word == PNX_KW_true || c->tok.word == PNX_KW_false) {
            k.type = TY_BOOL;
            k.value = c->tok.word == PNX_KW_true;
            break;
        }
        return syntax(c, "the value of a const is a literal");
    default: return syntax(c, "the value of a const is a literal");
    }
    constant *slot = vec_push(c, &c->consts, sizeof(constant));
    if (!slot)
        return false;
    *slot = k;
    return advance(c);
}

static bool declared_size(comp *c, uint32_t *out);

// ( N ) after `str` in a declaration.
static bool str_capacity(comp *c, uint8_t *out) {
    uint32_t n;
    if (c->tok.kind != PNX_T_LPAREN)
        return type_error(c, "stored text is str(N); a str view lives only in locals, arguments and results");
    if (!advance(c) || !declared_size(c, &n))
        return false;
    if (c->tok.kind != PNX_T_RPAREN)
        return syntax(c, "str(N) is closed by )");
    if (n > 255)
        return type_error(c, "str(N) holds at most 255 bytes for now");
    *out = (uint8_t)n;
    return advance(c);
}

// global NAME type : a scalar, zero at the start
static bool declare_global(comp *c) {
    if (!advance(c))
        return false;
    if (c->tok.kind != PNX_T_NAME)
        return syntax(c, "global is followed by a name and a type");
    globalsym g = {.name = tok_name(c), .kind = GK_SCALAR, .capacity = 1, .record = NONE};
    if (name_is_global(c, g.name))
        return type_error(c, "this name is already defined");
    if (g.name.length > 31)
        return syntax(c, "a global name is at most 31 bytes long");
    if (!advance(c))
        return false;
    if (c->tok.kind != PNX_T_NAME)
        return syntax(c, "global is followed by a name and a type");
    g.record = NONE;
    g.type = type_of_name(c, tok_name(c));
    if (!advance(c))
        return false;
    if (g.type == TY_STR) {
        if (!str_capacity(c, &g.strcap))
            return false;
    } else if (g.type == TY_NONE || bank_of(g.type) != BANK_I || is_block(g.type) || g.type == TY_MARK) {
        return type_error(c, "this type is not known, or not supported for a global yet");
    }
    if (c->globals.n >= 0xfffu)
        return syntax(c, "too many globals");
    globalsym *slot = vec_push(c, &c->globals, sizeof(globalsym));
    if (!slot)
        return false;
    *slot = g;
    return true;
}

// A size in a declaration: an integer literal or the name of an int const.
static bool declared_size(comp *c, uint32_t *out) {
    int64_t n;
    if (c->tok.kind == PNX_T_INT) {
        n = c->tok.integer;
    } else if (c->tok.kind == PNX_T_NAME && find_const(c, tok_name(c)) && find_const(c, tok_name(c))->type == TY_INT) {
        n = find_const(c, tok_name(c))->value;
    } else {
        return syntax(c, "a size is an integer literal or the name of an int const");
    }
    if (n < 1 || n > 0x7fffffff)
        return type_error(c, "a size is at least 1 and fits in 31 bits");
    *out = (uint32_t)n;
    return advance(c);
}

// array NAME type size        list NAME type size
// The type is a basic type (cells of 64 bits) or the name of a record (a table of rows).
static bool declare_collection(comp *c, uint8_t kind) {
    if (!advance(c))
        return false;
    if (c->tok.kind != PNX_T_NAME)
        return syntax(c, "the declaration is: array name type size");
    globalsym g = {.name = tok_name(c), .kind = kind, .record = NONE};
    const uint32_t announced = find_global(c, g.name);
    globalsym     *slot = NULL;
    if (announced != NONE && ((globalsym *)c->globals.ptr + announced)->forward) {
        slot = (globalsym *)c->globals.ptr + announced;
        if (slot->kind != kind)
            return type_error(c, "this name was announced by forward as another kind of table");
    } else if (name_is_global(c, g.name)) {
        return type_error(c, "this name is already defined");
    }
    if (g.name.length > 31)
        return syntax(c, "a global name is at most 31 bytes long");
    if (!advance(c))
        return false;
    if (c->tok.kind != PNX_T_NAME)
        return syntax(c, "the declaration is: array name type size");
    g.record = find_record_any(c, tok_name(c));
    g.type = basic_type(tok_name(c));
    if (g.record == NONE && g.type != TY_INT && g.type != TY_BOOL && g.type != TY_ERR && g.type != TY_STR)
        return type_error(c, "the cells are int, bool, err, str(N) or a record; other types are not supported yet");
    if (slot && g.record == NONE)
        return type_error(c, "a table announced by forward holds a record");
    if (kind == GK_QUEUE && (g.record != NONE || g.type == TY_STR))
        return type_error(c, "a queue holds int, bool or err values");
    if (kind == GK_POOL && g.record == NONE)
        return type_error(c, "a pool holds records: pool name record size");
    if (!advance(c) || (g.record == NONE && g.type == TY_STR && !str_capacity(c, &g.strcap)) ||
        !declared_size(c, &g.capacity))
        return false;
    if (slot) { // announced by forward: export on either line exports the table
        g.exported = slot->exported || c->exporting;
        g.module = slot->module;
    }
    if (!slot) {
        if (c->globals.n >= 0xfffu)
            return syntax(c, "too many globals");
        slot = vec_push(c, &c->globals, sizeof(globalsym));
        if (!slot)
            return false;
    }
    if (g.record != NONE)
        g.type = row_type((uint32_t)(slot - (globalsym *)c->globals.ptr));
    *slot = g;
    return true;
}

// forward list NAME | forward array NAME : a table a record refers to before it is declared
static bool declare_forward(comp *c) {
    if (!advance(c))
        return false;
    if (is_word(c, PNX_KW_word)) { // forward word NAME ( effect )
        if (!advance(c))
            return false;
        if (c->tok.kind != PNX_T_NAME)
            return syntax(c, "forward word names the word: forward word name (effect)");
        fwd ahead = {.name = tok_name(c), .index = NONE, .exported = c->exporting};
        if (name_is_global(c, ahead.name))
            return type_error(c, "this name is already defined");
        if (!advance(c) || !parse_sig(c, &ahead.sig))
            return false;
        if (c->fwds.n >= 0xffffu)
            return syntax(c, "too many forward declarations");
        fwd *slot = vec_push(c, &c->fwds, sizeof(fwd));
        if (!slot)
            return false;
        *slot = ahead;
        return true;
    }
    if (!is_word(c, PNX_KW_list) && !is_word(c, PNX_KW_array) && !is_word(c, PNX_KW_pool))
        return syntax(c, "forward is followed by array, list, pool or word");
    globalsym g = {.kind = is_word(c, PNX_KW_array) ? GK_ARRAY : is_word(c, PNX_KW_list) ? GK_LIST : GK_POOL,
                   .forward = true, .record = NONE};
    if (!advance(c))
        return false;
    if (c->tok.kind != PNX_T_NAME)
        return syntax(c, "forward names the table: forward list name");
    g.name = tok_name(c);
    if (name_is_global(c, g.name))
        return type_error(c, "this name is already defined");
    if (g.name.length > 31)
        return syntax(c, "a global name is at most 31 bytes long");
    if (c->globals.n >= 0xfffu)
        return syntax(c, "too many globals");
    globalsym *slot = vec_push(c, &c->globals, sizeof(globalsym));
    if (!slot)
        return false;
    g.type = row_type(c->globals.n - 1);
    *slot = g;
    return advance(c);
}

// record NAME  { field type }  end
static bool declare_record(comp *c) {
    static const struct { const char *name; uint8_t width, bytes; } widths[] = {
        {"u8", PNX_W_U8, 1},   {"u16", PNX_W_U16, 2}, {"u32", PNX_W_U32, 4}, {"u64", PNX_W_64, 8},
        {"i8", PNX_W_I8, 1},   {"i16", PNX_W_I16, 2}, {"i32", PNX_W_I32, 4}, {"i64", PNX_W_64, 8},
        {"int", PNX_W_64, 8},
    };
    if (!advance(c))
        return false;
    if (c->tok.kind != PNX_T_NAME)
        return syntax(c, "record is followed by the name of the record");
    record r = {.name = tok_name(c), .first = c->fields.n};
    if (name_is_global(c, r.name))
        return type_error(c, "this name is already defined");
    if (!advance(c))
        return false;
    while (!is_word(c, PNX_KW_end)) {
        if (is_word(c, PNX_KW_columns))
            return syntax(c, "columns is not supported by the compiler yet");
        if (is_word(c, PNX_KW_big_endian)) {
            r.big_endian = true;
            if (!advance(c))
                return false;
            continue;
        }
        if (c->tok.kind != PNX_T_NAME)
            return syntax(c, "a record is a list of fields: name type, closed by end");
        field fd = {.name = tok_name(c), .offset = (uint16_t)r.size};
        for (uint32_t k = r.first; k < c->fields.n; ++k) {
            if (name_eq(((const field *)c->fields.ptr + k)->name, fd.name))
                return type_error(c, "the record already has a field of this name");
        }
        if (!advance(c))
            return false;
        if (c->tok.kind != PNX_T_NAME)
            return syntax(c, "a field is a name and a type");
        const name tn = tok_name(c);
        uint32_t   bytes = 0;
        for (size_t k = 0; k < sizeof widths / sizeof widths[0]; ++k) {
            if (name_is(tn, widths[k].name)) {
                fd.type = TY_INT;
                fd.extra = widths[k].width;
                bytes = widths[k].bytes;
            }
        }
        if (!advance(c))
            return false;
        if (bytes) {
            // an integer of that width
        } else if (name_is(tn, "varint")) { // an int, packed in as few bytes as it needs
            fd.type = TY_INT;
            fd.extra = PNX_W_64;
            fd.varint = true;
            bytes = 8;
        } else if (name_is(tn, "bool")) {
            fd.type = TY_BOOL;
            fd.extra = PNX_W_U8;
            bytes = 1;
        } else if (name_is(tn, "str")) { // str(N): one length byte, then N bytes
            uint32_t n;
            if (c->tok.kind != PNX_T_LPAREN)
                return type_error(c, "a field holds str(N); a str view lives only in locals, arguments and results");
            if (!advance(c) || !declared_size(c, &n))
                return false;
            if (c->tok.kind != PNX_T_RPAREN)
                return syntax(c, "str(N) is closed by )");
            if (n > 255)
                return type_error(c, "str(N) fields hold at most 255 bytes for now");
            if (!advance(c))
                return false;
            fd.type = TY_STR;
            fd.is_str = true;
            fd.extra = (uint8_t)n;
            bytes = n + 1;
        } else if (is_row(type_of_name(c, tn))) { // a row of that table: its number plus one, 0 for none
            fd.type = type_of_name(c, tn);
            const bool pool = ((const globalsym *)c->globals.ptr + (fd.type - TY_FIRST_DECLARED))->kind == GK_POOL;
            fd.extra = pool ? PNX_W_64 : PNX_W_U32; // a row of a pool carries its generation
            bytes = pool ? 8 : 4;
        } else {
            return type_error(c, "this field type is not known, or not supported by the compiler yet");
        }
        r.size += bytes;
        if (r.size > 4095)
            return type_error(c, "a record is at most 4095 bytes for now");
        field *slot = vec_push(c, &c->fields, sizeof(field));
        if (!slot)
            return false;
        *slot = fd;
        ++r.count;
    }
    if (r.count == 0)
        return type_error(c, "a record has at least one field");
    record *slot = vec_push(c, &c->records, sizeof(record));
    if (!slot)
        return false;
    *slot = r;
    return advance(c);
}

// type NAME base : a type with the representation of `base` that does not mix with it
static bool declare_type(comp *c) {
    if (!advance(c))
        return false;
    if (c->tok.kind != PNX_T_NAME)
        return syntax(c, "the declaration is: type name base");
    dtype d = {.name = tok_name(c)};
    if (name_is_global(c, d.name))
        return type_error(c, "this name is already defined");
    if (!advance(c))
        return false;
    if (c->tok.kind != PNX_T_NAME)
        return syntax(c, "the declaration is: type name base");
    d.base = basic_type(tok_name(c));
    if (d.base != TY_INT && d.base != TY_STR && d.base != TY_BOOL)
        return type_error(c, "a declared type is made of int, bool or str");
    if (c->dtypes.n >= 0x3000u)
        return syntax(c, "too many declared types");
    dtype *slot = vec_push(c, &c->dtypes, sizeof(dtype));
    if (!slot)
        return false;
    *slot = d;
    return advance(c);
}

// buffer NAME size
static bool declare_buffer(comp *c) {
    if (!advance(c))
        return false;
    if (c->tok.kind != PNX_T_NAME)
        return syntax(c, "the declaration is: buffer name size");
    globalsym g = {.name = tok_name(c), .kind = GK_BUFFER, .record = NONE};
    if (name_is_global(c, g.name))
        return type_error(c, "this name is already defined");
    if (g.name.length > 31)
        return syntax(c, "a global name is at most 31 bytes long");
    if (!advance(c) || !declared_size(c, &g.capacity))
        return false;
    if (c->globals.n >= 0xfffu)
        return syntax(c, "too many globals");
    globalsym *slot = vec_push(c, &c->globals, sizeof(globalsym));
    if (!slot)
        return false;
    *slot = g;
    return true;
}

// index NAME table record.field : finds rows of the table by that field
static bool declare_index(comp *c) {
    if (!advance(c))
        return false;
    if (c->tok.kind != PNX_T_NAME)
        return syntax(c, "the declaration is: index name table record.field");
    globalsym g = {.name = tok_name(c), .kind = GK_INDEX, .record = NONE};
    if (name_is_global(c, g.name))
        return type_error(c, "this name is already defined");
    if (g.name.length > 31)
        return syntax(c, "a global name is at most 31 bytes long");
    if (!advance(c))
        return false;
    g.table = c->tok.kind == PNX_T_NAME ? find_global(c, tok_name(c)) : NONE;
    const globalsym *table = g.table == NONE ? NULL : (const globalsym *)c->globals.ptr + g.table;
    if (!table || table->record == NONE || table->forward || table->kind == GK_POOL)
        return type_error(c, "an index belongs to a declared array or list of records");
    if (!advance(c))
        return false;
    if (c->tok.kind != PNX_T_PATH)
        return syntax(c, "the key of an index is written record.field");
    const name path = tok_name(c);
    uint32_t dot = 0;
    while (path.text[dot] != '.')
        ++dot;
    const field *key = find_record(c, (name){path.text, dot}) == table->record
                           ? find_field(c, table->record, (name){path.text + dot + 1, path.length - dot - 1})
                           : NULL;
    if (!key)
        return type_error(c, "the key is a field of the record the table holds");
    if (key->type != TY_INT && key->type != TY_STR)
        return type_error(c, "the key of an index is an integer field or a str(N) field");
    g.key = (uint32_t)(key - (const field *)c->fields.ptr);
    g.capacity = 4; // slots: a power of two, at least twice the rows
    while (g.capacity < table->capacity * 2u)
        g.capacity *= 2;
    if (c->globals.n >= 0xfffu)
        return syntax(c, "too many globals");
    globalsym *slot = vec_push(c, &c->globals, sizeof(globalsym));
    if (!slot)
        return false;
    *slot = g;
    return advance(c);
}

// A literal or constant in a `row` or `assign` item. The current token is the value.
static bool item_value(comp *c, tid *type, int64_t *value) {
    const name n = tok_name(c);
    uint32_t   index;
    switch (c->tok.kind) {
    case PNX_T_INT:
    case PNX_T_CHAR:
        *type = TY_INT;
        *value = c->tok.integer;
        return true;
    case PNX_T_STRING:
    case PNX_T_RAW:
    case PNX_T_TEXT:
        if (!add_string(c, &c->tok, &index))
            return false;
        *type = TY_STR;
        *value = index;
        return true;
    case PNX_T_NAME:
        if (name_is(n, "none")) { // fits any row field
            *type = TY_NONE;
            *value = 0;
            return true;
        }
        if (find_const(c, n)) {
            *type = find_const(c, n)->type;
            *value = find_const(c, n)->value;
            return true;
        }
        return type_error(c, "a value here is a literal, the name of a const or of a row, or none");
    case PNX_T_RESERVED:
        if (c->tok.word == PNX_KW_true || c->tok.word == PNX_KW_false) {
            *type = TY_BOOL;
            *value = c->tok.word == PNX_KW_true;
            return true;
        }
        return syntax(c, "a value here is a literal or the name of a const");
    default: return syntax(c, "a value here is a literal or the name of a const");
    }
}

// The values of one row: value , value ... up to the closing parenthesis, stored field by
// field into the row in register `row` of table `gi`. The current token is the first value
// or the parenthesis; it is the parenthesis at the end.
static bool row_values(comp *c, uint32_t gi, uint32_t row) {
    const globalsym *g = (const globalsym *)c->globals.ptr + gi;
    const record *rec = (const record *)c->records.ptr + g->record;
    const uint32_t value = top_slot(c, BANK_I, 3) + 2, text = top_slot(c, BANK_S, 1);
    for (uint32_t k = 0;; ++k) {
        if (c->tok.kind == PNX_T_RPAREN && k == 0)
            return true; // an empty row: all fields zero
        if (k >= rec->count)
            return type_error(c, "the row has more values than the record has fields");
        const field *fd = (const field *)c->fields.ptr + rec->first + k;
        tid     type;
        int64_t v;
        if (!item_value(c, &type, &v))
            return false;
        if (type == TY_NONE ? !is_row(fd->type) : type != fd->type)
            return type_error(c, "a value has another type than its field");
        const uint32_t where = PNX_FIELD(gi, fd->offset, fd->extra);
        if (fd->is_str) {
            if (emit(c, PNX_ABX(PNX_LOADKS, text, (uint32_t)v)) == NONE || emit(c, PNX_ABC(PNX_FSETS, text, row, 0)) == NONE ||
                emit(c, where) == NONE)
                return false;
        } else if (!load_int(c, value, v) || emit(c, PNX_ABC(PNX_FSET, value, row, 0)) == NONE || emit(c, where) == NONE) {
            return false;
        }
        if (!advance(c))
            return false;
        if (c->tok.kind == PNX_T_RPAREN)
            return true;
        if (c->tok.kind != PNX_T_COMMA)
            return syntax(c, "values are separated by commas");
        if (!advance(c))
            return false;
    }
}

// row LIST [ NAME ] ( value , ... ) : adds a row when the source is loaded. With a name, the
// name becomes a constant for that row.
static bool row_item(comp *c) {
    fn *f = c->f;
    if (f->dead)
        return syntax(c, "this code cannot be reached: the code before it always leaves");
    if (!advance(c))
        return false;
    const uint32_t gi = c->tok.kind == PNX_T_NAME ? find_global(c, tok_name(c)) : NONE;
    globalsym *g = gi == NONE ? NULL : (globalsym *)c->globals.ptr + gi;
    if (!g || g->kind != GK_LIST || g->forward)
        return type_error(c, "row adds to a declared list: row name (values)");
    if (!advance(c))
        return false;
    name row_name = {0};
    if (c->tok.kind == PNX_T_NAME) {
        row_name = tok_name(c);
        if (g->record == NONE)
            return type_error(c, "only a row of a record has a name");
        if (name_is_global(c, row_name))
            return type_error(c, "this name is already defined");
        if (!advance(c))
            return false;
    }
    if (c->tok.kind != PNX_T_LPAREN)
        return syntax(c, "the values of a row are in parentheses");
    if (!advance(c))
        return false;
    if (g->rows == g->capacity)
        return type_error(c, "the list has no room for this row");
    const uint32_t row = top_slot(c, BANK_I, 2), value = row + 1;
    uint32_t       full = NONE, ok = NONE;
    if (g->record == NONE) { // a list of values: one value, pushed
        tid     type;
        int64_t v;
        if (!item_value(c, &type, &v))
            return false;
        if (type != g->type)
            return type_error(c, "the value has another type than the cells of the list");
        if (g->strcap) { // a new cell, then the text; the two jumps below belong to the ADDROW
            const uint32_t text = top_slot(c, BANK_S, 1);
            uint32_t       bad = NONE, fine = NONE;
            if (emit(c, PNX_ABX(PNX_ADDROW, row, gi)) == NONE || !jump_later(c, &bad) || !jump_later(c, &fine))
                return false;
            land(c, &bad);
            if (emit(c, PNX_ABX(PNX_FAULT, 0, PINAX_ERR_FULL)) == NONE)
                return false;
            land(c, &fine);
            if (emit(c, PNX_ABX(PNX_LOADKS, text, (uint32_t)v)) == NONE || emit(c, PNX_ABC(PNX_FSETS, text, row, 0)) == NONE ||
                emit(c, PNX_FIELD(gi, 0, g->strcap)) == NONE || !advance(c))
                return false;
            if (c->tok.kind != PNX_T_RPAREN)
                return syntax(c, "a row of a list of values has one value");
            ++g->rows;
            return advance(c);
        }
        if (!load_int(c, value, v) || emit(c, PNX_ABX(PNX_LPUSH, value, gi)) == NONE)
            return false;
    } else if (emit(c, PNX_ABX(PNX_ADDROW, row, gi)) == NONE) {
        return false;
    }
    // a full list is a fault while loading
    if (!jump_later(c, &full) || !jump_later(c, &ok))
        return false;
    land(c, &full);
    if (emit(c, PNX_ABX(PNX_FAULT, 0, PINAX_ERR_FULL)) == NONE)
        return false;
    land(c, &ok);
    if (g->record == NONE) {
        if (!advance(c))
            return false;
        if (c->tok.kind != PNX_T_RPAREN)
            return syntax(c, "a row of a list of values has one value");
        ++g->rows;
        return advance(c);
    }
    if (row_name.length) {
        if (!load_int(c, value, (int64_t)g->rows + 1) || emit(c, PNX_ABC(PNX_JEQ, row, value, 1)) == NONE ||
            emit(c, PNX_ABX(PNX_FAULT, 0, PINAX_ERR_STATE)) == NONE)
            return false;
        constant *k = vec_push(c, &c->consts, sizeof(constant));
        if (!k)
            return false;
        *k = (constant){.name = row_name, .type = g->type, .value = (int64_t)g->rows + 1};
        g = (globalsym *)c->globals.ptr + gi;
    }
    if (!row_values(c, gi, row))
        return false;
    // the row enters every index of its list; a key that is there already is a fault while loading
    for (uint32_t x = 0; x < c->globals.n; ++x) {
        const globalsym *ix = (const globalsym *)c->globals.ptr + x;
        if (ix->kind != GK_INDEX || ix->table != gi)
            continue;
        uint32_t bad = NONE, fine = NONE;
        if (emit(c, PNX_ABX(PNX_IXPUT, row, x)) == NONE || !jump_later(c, &bad) || !jump_later(c, &fine))
            return false;
        land(c, &bad);
        if (emit(c, PNX_ABX(PNX_FAULT, 0, PINAX_ERR_FAILED)) == NONE)
            return false;
        land(c, &fine);
    }
    ++((globalsym *)c->globals.ptr + gi)->rows;
    f->def_at = NONE;
    return advance(c);
}

// assign NAME ( value , ... ) : fills an array from cell 0, when the source is loaded
static bool assign_item(comp *c) {
    fn *f = c->f;
    if (f->dead)
        return syntax(c, "this code cannot be reached: the code before it always leaves");
    if (!advance(c))
        return false;
    const uint32_t gi = c->tok.kind == PNX_T_NAME ? find_global(c, tok_name(c)) : NONE;
    const globalsym *g = gi == NONE ? NULL : (const globalsym *)c->globals.ptr + gi;
    if (!g || g->kind != GK_ARRAY || g->forward)
        return type_error(c, "assign fills a declared array: assign name (values)");
    if (!advance(c))
        return false;
    if (c->tok.kind != PNX_T_LPAREN)
        return syntax(c, "the values of assign are in parentheses");
    if (!advance(c))
        return false;
    const uint32_t value = top_slot(c, BANK_I, 2), index = value + 1;
    for (uint32_t k = 0; g->record != NONE; ++k) { // an array of records: one parenthesised row per cell
        if (c->tok.kind != PNX_T_LPAREN)
            return syntax(c, "each value of an array of records is a row in parentheses");
        if (k >= g->capacity)
            return type_error(c, "assign has more values than the array has cells");
        if (!advance(c) || !load_int(c, value, (int64_t)k + 1) || !row_values(c, gi, value) || !advance(c))
            return false;
        g = (const globalsym *)c->globals.ptr + gi;
        if (c->tok.kind == PNX_T_RPAREN)
            return advance(c);
        if (c->tok.kind != PNX_T_COMMA)
            return syntax(c, "values are separated by commas");
        if (!advance(c))
            return false;
    }
    for (uint32_t k = 0;; ++k) {
        tid     type;
        int64_t v = c->tok.integer;
        if (c->tok.kind == PNX_T_INT || c->tok.kind == PNX_T_CHAR) {
            type = TY_INT;
        } else if (is_word(c, PNX_KW_true) || is_word(c, PNX_KW_false)) {
            type = TY_BOOL;
            v = is_word(c, PNX_KW_true);
        } else if (c->tok.kind == PNX_T_NAME && find_const(c, tok_name(c))) {
            type = find_const(c, tok_name(c))->type;
            v = find_const(c, tok_name(c))->value;
        } else if (c->tok.kind != PNX_T_STRING && c->tok.kind != PNX_T_RAW && c->tok.kind != PNX_T_TEXT) {
            return syntax(c, "a value of assign is a literal or the name of a const");
        }
        if (c->tok.kind == PNX_T_STRING || c->tok.kind == PNX_T_RAW || c->tok.kind == PNX_T_TEXT) {
            uint32_t at;
            if (!add_string(c, &c->tok, &at))
                return false;
            type = TY_STR;
            v = at;
        }
        if (type != g->type)
            return type_error(c, "a value has another type than the cells of the array");
        if (k >= g->capacity)
            return type_error(c, "assign has more values than the array has cells");
        if (g->strcap) {
            const uint32_t text = top_slot(c, BANK_S, 1);
            if (emit(c, PNX_ABX(PNX_LOADKS, text, (uint32_t)v)) == NONE || !load_int(c, index, (int64_t)k + 1) ||
                emit(c, PNX_ABC(PNX_FSETS, text, index, 0)) == NONE || emit(c, PNX_FIELD(gi, 0, g->strcap)) == NONE)
                return false;
        } else if (!load_int(c, value, v) || !load_int(c, index, k) || emit_g(c, PNX_ASET, value, index, gi) == NONE) {
            return false;
        }
        if (!advance(c))
            return false;
        if (c->tok.kind == PNX_T_RPAREN)
            break;
        if (c->tok.kind != PNX_T_COMMA)
            return syntax(c, "values are separated by commas");
        if (!advance(c))
            return false;
    }
    return advance(c);
}

// ---- the container ------------------------------------------------------------------------

// Words are stored in the order of their code. Each definition was compiled into the word
// buffer and copied out here; the code outside definitions follows as one last word.
typedef struct image {
    vec code;  // uint32_t, all words
    vec lines; // uint32_t, the source line of each code word (0 for the code of earlier loads)
} image;

static bool append_code(comp *c, image *im, const fn *f) {
    for (uint32_t k = 0; k < f->code.n; ++k) {
        uint32_t *slot = vec_push(c, &im->code, sizeof(uint32_t));
        uint32_t *line = slot ? vec_push(c, &im->lines, sizeof(uint32_t)) : NULL;
        if (!line)
            return false;
        *slot = ((const uint32_t *)f->code.ptr)[k];
        *line = ((const uint32_t *)f->lines.ptr)[k];
    }
    return true;
}

// Calls of words that were only announced carry the number of the announcement; now that
// every word has its number, they get it.
static void patch_forward_calls(const comp *c, uint32_t *code, uint32_t n) {
    for (uint32_t i = 0; i < n;) {
        const uint32_t op = code[i] & 0xffu;
        if ((op == PNX_CALL || op == PNX_TCALL) && (code[i + 1] & FWD_MARK))
            code[i + 1] = ((const fwd *)c->fwds.ptr + (code[i + 1] & ~FWD_MARK))->index;
        i += pnx_ops_info[op].second ? 2 : 1;
    }
}

// Stores the value blocks finished since the last word was stored, in the order of their numbers.
static bool store_pending(comp *c, image *im) {
    if (c->nested)
        return true; // an outer block is still open; its number comes first
    const pending_code *all = c->pending.ptr;
    for (uint32_t done = 0; done < c->pending.n; ++done) {
        const pending_code *next = NULL;
        for (uint32_t k = 0; k < c->pending.n; ++k) { // few: pick the lowest number not stored yet
            if (all[k].code && (!next || all[k].word < next->word))
                next = &all[k];
        }
        ((wordsym *)c->words.ptr + next->word)->entry = im->code.n;
        for (uint32_t k = 0; k < next->n; ++k) {
            uint32_t *slot = vec_push(c, &im->code, sizeof(uint32_t));
            uint32_t *line = slot ? vec_push(c, &im->lines, sizeof(uint32_t)) : NULL;
            if (!line)
                return false;
            *slot = next->code[k];
            *line = next->lines[k];
        }
        ((pending_code *)next)->code = NULL;
    }
    c->pending.n = 0;
    return true;
}

static void put_text(char *out, size_t cap, name n) {
    const size_t len = n.length < cap - 1 ? n.length : cap - 1;
    __builtin_memcpy(out, n.text, len);
    out[len] = '\0';
}

#define LOAD_WORD "(load)"

// What the item just compiled declared belongs to this module, and is exported when `export`
// stood before it. A data source exports all it declares.
static void mark_new(comp *c, struct counts *seen) {
    const bool out = c->exporting || c->data_only;
#define MARK(vec_, type_, field_)                                         \
    for (uint32_t k = seen->field_; k < c->vec_.n; ++k) {                 \
        type_ *e = (type_ *)c->vec_.ptr + k;                              \
        e->module = c->module;                                            \
        e->exported = e->exported || out;                                 \
    }                                                                     \
    seen->field_ = c->vec_.n
    MARK(words, wordsym, words);
    MARK(consts, constant, consts);
    MARK(globals, globalsym, globals);
    MARK(records, record, records);
    MARK(dtypes, dtype, dtypes);
    MARK(externs, externsym, externs);
#undef MARK
    c->exporting = false;
}

static bool compile(comp *c, image *im, bool *has_top) {
    if (!advance(c))
        return false;
    if (is_word(c, PNX_KW_pinax)) { // pinax 2 : the source format version (RFC-0012, RFC-0017 section 3.7)
        if (!advance(c))
            return false;
        if (c->tok.kind != PNX_T_INT)
            return syntax(c, "pinax is followed by the source format version");
        static_assert(PINAX_FORMAT == 2, "the message below names the version");
        if (c->tok.integer != PINAX_FORMAT)
            return error(c, PINAX_ERR_FORMAT, "this source is written for another format version; this build reads version 2");
        if (!advance(c))
            return false;
    }
    struct counts seen = c->base;
    while (c->tok.kind != PNX_T_END) {
        if (c->eval && c->tok.kind == PNX_T_NAME && !c->top_fn->n_blocks && c->top_fn->depth == 0 &&
            name_is(tok_name(c), "redefine") && !find_word(c, tok_name(c), NULL)) {
            // redefine word NAME (effect) ... end : a tool word of an interactive session
            if (!advance(c))
                return false;
            if (!is_word(c, PNX_KW_word))
                return syntax(c, "redefine is followed by a word definition");
            c->redefining = true;
            continue;
        }
        if (c->eval && c->tok.kind == PNX_T_NAME && !c->top_fn->n_blocks && c->top_fn->depth == 0 &&
            (name_is(tok_name(c), "see") || name_is(tok_name(c), "list_words"))) {
            // tool words of an interactive session (RFC-0009 section 6)
            if (name_is(tok_name(c), "see")) {
                c->see = true;
            } else if (c->st->diag) {
                const wordsym *all = c->words.ptr;
                for (uint32_t k = 0; k < c->words.n; ++k) {
                    char text[120], effect[72];
                    if (all[k].name.text[0] == '(')
                        continue;
                    if (k < c->base.words) // a word of an earlier load: its signature as it was written
                        __builtin_memcpy(effect, c->st->words[k].signature, sizeof effect);
                    else if (!sig_text(c, &all[k].sig, effect, sizeof effect))
                        continue;
                    size_t n = 0;
                    const name module = ((const name *)c->srcmods.ptr)[all[k].module];
                    if (all[k].module != c->module && module.length) {
                        __builtin_memcpy(text, module.text, module.length);
                        n = module.length;
                        text[n++] = '.';
                    }
                    __builtin_memcpy(text + n, all[k].name.text, all[k].name.length);
                    n += all[k].name.length;
                    text[n++] = ' ';
                    __builtin_memcpy(text + n, effect, __builtin_strlen(effect) + 1);
                    const pinax_str file = {(const unsigned char *)c->file, __builtin_strlen(c->file)};
                    const pinax_str message = {(const unsigned char *)text, __builtin_strlen(text)};
                    c->st->diag(c->st->diag_user, file, 0, 0, message);
                }
            }
            if (!advance(c))
                return false;
            continue;
        }
        if (is_word(c, PNX_KW_use)) {
            if (!use_module(c))
                return false;
        } else if (is_word(c, PNX_KW_export)) { // what other modules may name (RFC-0014)
            if (c->exporting || c->top_fn->n_blocks || !advance(c))
                return c->err == PINAX_OK ? syntax(c, "export stands once, before a definition or a declaration") : false;
            static const pnx_word_id may[] = {PNX_KW_word,  PNX_KW_const,  PNX_KW_global, PNX_KW_array, PNX_KW_list,
                                              PNX_KW_queue, PNX_KW_pool,   PNX_KW_type,   PNX_KW_buffer,
                                              PNX_KW_index, PNX_KW_record, PNX_KW_forward, PNX_KW_bound,
                                              PNX_KW_row,   PNX_KW_extern, PNX_KW_tasks};
            bool ok = false;
            for (uint32_t k = 0; k < sizeof may / sizeof may[0]; ++k)
                ok = ok || is_word(c, may[k]);
            if (!ok)
                return syntax(c, "export is followed by a definition or a declaration");
            c->exporting = true;
            continue;
        } else if (c->data_only && (is_word(c, PNX_KW_word) || is_word(c, PNX_KW_test) || is_word(c, PNX_KW_use))) {
            return syntax(c, "a data source defines no words and uses no modules");
        } else if (is_word(c, PNX_KW_word) || is_word(c, PNX_KW_test)) {
            if (!definition(c, is_word(c, PNX_KW_test)))
                return false;
            ((wordsym *)c->words.ptr + c->def_index)->entry = im->code.n;
            if (!append_code(c, im, c->word_fn) || !store_pending(c, im))
                return false;
            c->see_line = 0;
        } else if (is_word(c, PNX_KW_bound) && !c->top_fn->n_blocks) {
            // bound global | bound array : the host gives the storage (pinax_bind); until it does,
            // the declaration has storage of its own, so nothing is ever unbacked
            if (!advance(c))
                return false;
            const bool scalar = is_word(c, PNX_KW_global), bytes = is_word(c, PNX_KW_buffer);
            if (!scalar && !bytes && !is_word(c, PNX_KW_array))
                return syntax(c, "bound stands before global, array or buffer");
            pnx_lexer peek = *c->lx; // the name follows; the declaration may fill a slot announced earlier
            pnx_token named;
            if (!pnx_lex_next(&peek, &named))
                named.length = 0;
            const name bound_name = {c->lx->source + named.start, named.length};
            if (scalar ? !declare_global(c) : bytes ? !declare_buffer(c) : !declare_collection(c, GK_ARRAY))
                return false;
            const uint32_t gi = find_global(c, bound_name);
            if (gi == NONE)
                return syntax(c, "bound stands before global, array or buffer");
            ((globalsym *)c->globals.ptr + gi)->bound = true;
        } else if (is_word(c, PNX_KW_extern) && !c->top_fn->n_blocks) {
            if (c->data_only)
                return syntax(c, "a data source names no C functions");
            if (!declare_extern(c))
                return false;
        } else if (is_word(c, PNX_KW_const) && !c->top_fn->n_blocks) {
            if (!declare_const(c))
                return false;
        } else if (is_word(c, PNX_KW_global) && !c->top_fn->n_blocks) {
            if (!declare_global(c))
                return false;
        } else if ((is_word(c, PNX_KW_array) || is_word(c, PNX_KW_list) || is_word(c, PNX_KW_queue) ||
                    is_word(c, PNX_KW_pool)) &&
                   !c->top_fn->n_blocks) {
            if (!declare_collection(c, is_word(c, PNX_KW_array)   ? GK_ARRAY
                                       : is_word(c, PNX_KW_list)  ? GK_LIST
                                       : is_word(c, PNX_KW_queue) ? GK_QUEUE
                                                                  : GK_POOL))
                return false;
        } else if (is_word(c, PNX_KW_type) && !c->top_fn->n_blocks) {
            if (!declare_type(c))
                return false;
        } else if (is_word(c, PNX_KW_assign) && !c->top_fn->n_blocks) {
            if (!assign_item(c))
                return false;
        } else if (is_word(c, PNX_KW_buffer) && !c->top_fn->n_blocks) {
            if (!declare_buffer(c))
                return false;
        } else if (is_word(c, PNX_KW_tasks) && !c->top_fn->n_blocks) {
            if (c->data_only)
                return syntax(c, "a data source runs no code and declares no tasks");
            if (!declare_tasks(c))
                return false;
        } else if (is_word(c, PNX_KW_index) && !c->top_fn->n_blocks) {
            if (!declare_index(c))
                return false;
        } else if (is_word(c, PNX_KW_record) && !c->top_fn->n_blocks) {
            if (!declare_record(c))
                return false;
        } else if (is_word(c, PNX_KW_forward) && !c->top_fn->n_blocks) {
            if (!declare_forward(c))
                return false;
        } else if (is_word(c, PNX_KW_row) && !c->top_fn->n_blocks) {
            if (!row_item(c))
                return false;
        } else if (is_word(c, PNX_KW_pinax)) {
            return syntax(c, "the version line is the first thing in a source");
        } else if (!body_token(c) || !store_pending(c, im)) {
            return false;
        }
        mark_new(c, &seen);
    }
    mark_new(c, &seen);
    fn *top = c->top_fn;
    if (top->n_blocks)
        return syntax(c, "the source ends inside an open block");
    for (uint32_t k = 0; k < c->globals.n; ++k) {
        if (((const globalsym *)c->globals.ptr + k)->forward)
            return type_error(c, "a table announced by forward is never declared");
    }
    for (uint32_t k = 0; k < c->fwds.n; ++k) {
        if (((const fwd *)c->fwds.ptr + k)->index == NONE)
            return type_error(c, "a word announced by forward is never defined");
    }
    *has_top = top->code.n != 0 || top->depth != 0;
    if (*has_top) {
        if (top->depth && !top->dead && !c->eval)
            return type_error(c, "values are left on the stack at the end of the source");
        if (c->eval && !top->dead) { // what a line of a session leaves is what its code gives
            sig *s = &c->eval_sig;
            for (uint32_t k = 0; k < top->depth; ++k) {
                const tid t = top->stack[k].type;
                if (s->n_out == MAX_PARAMS || t == TY_COLL || t == TY_STEM || top->stack[k].owned)
                    return type_error(c, s->n_out == MAX_PARAMS ? "the line leaves too many values"
                                         : top->stack[k].owned  ? "the line leaves an owned value that nothing takes"
                                                                : "the line leaves a name or a stem, which is not a value");
                s->out[s->n_out++] = t;
            }
            top->sig = s;
            if (!emit_return(c))
                return false;
        } else if (!top->dead && emit(c, PNX_ABC(PNX_RET, 0, 0, 0)) == NONE) {
            return false;
        }
        wordsym *w = vec_push(c, &c->words, sizeof(wordsym));
        if (!w)
            return false;
        __builtin_memset(w, 0, sizeof *w);
        if (c->eval)
            w->sig = c->eval_sig;
        w->name = cname(LOAD_WORD);
        w->module = c->module;
        w->entry = im->code.n;
        // a line of a session gives results: their registers count even when only constants
        // were left (`1 "a" drop` leaves 1 without using a register before the return)
        if (!finish_word(c, top, c->words.n - 1) || !append_code(c, im, top))
            return false;
    }
    return true;
}

// redefine: the new code takes the place of the old word's code, and the old code goes where
// the new word was, under the hidden name "(replaced)". Calls name words by number and jumps
// are relative inside a word, so whole words of code can change places; the word that was
// replaced keeps its number, and every call to it, from earlier code too, runs the new code.
static bool move_redefined(comp *c, image *im) {
    const pinax_state *st = c->st;
    const uint32_t n = c->words.n, total = im->code.n, old_code = st->head->n_code, n_lines = st->head->n_lines;
    const uint32_t old = c->redefine_old, made = c->redefine_new;
    wordsym *all = c->words.ptr;
    uint32_t *code = im->code.ptr, *lines = im->lines.ptr;
    // the source lines of the earlier code, from its records, one for each code word
    for (uint32_t pc = 0; pc < old_code; ++pc)
        lines[pc] = 0;
    for (uint32_t k = 0; k < n_lines; ++k) {
        pnx_linerec rec, next = {old_code, 0};
        __builtin_memcpy(&rec, st->lines + (size_t)k * sizeof rec, sizeof rec);
        if (k + 1 < n_lines)
            __builtin_memcpy(&next, st->lines + (size_t)(k + 1) * sizeof next, sizeof next);
        for (uint32_t pc = rec.pc; pc < next.pc && pc < old_code; ++pc)
            lines[pc] = rec.line;
    }
    uint32_t *begin = arena_alloc(c, (size_t)(n + 1) * sizeof(uint32_t)), *entries = arena_alloc(c, (size_t)n * sizeof(uint32_t)),
             *code2 = arena_alloc(c, (size_t)total * sizeof(uint32_t)), *lines2 = arena_alloc(c, (size_t)total * sizeof(uint32_t));
    if (!begin || !entries || !code2 || !lines2)
        return false;
    for (uint32_t j = 0; j < n; ++j)
        begin[j] = j < c->base.words ? st->words[j].entry : all[j].entry;
    begin[n] = total;
    for (uint32_t j = 0; j < n; ++j) {
        if (begin[j] > begin[j + 1] || (j == c->base.words && begin[j] != old_code))
            return error(c, PINAX_ERR_STATE, "redefine: the code of the words is not laid out in their order");
    }
    for (uint32_t j = 0, at = 0; j < n; ++j) {
        const uint32_t from = j == old ? made : j == made ? old : j, length = begin[from + 1] - begin[from];
        __builtin_memcpy(code2 + at, code + begin[from], (size_t)length * sizeof(uint32_t));
        __builtin_memcpy(lines2 + at, lines + begin[from], (size_t)length * sizeof(uint32_t));
        entries[j] = at;
        at += length;
    }
    __builtin_memcpy(code, code2, (size_t)total * sizeof(uint32_t));
    __builtin_memcpy(lines, lines2, (size_t)total * sizeof(uint32_t));
    __builtin_memcpy(c->redefine_cells, all[made].cells, sizeof c->redefine_cells);
    all[made].cells[0] = st->words[old].int_cells;
    all[made].cells[1] = st->words[old].str_cells;
    for (uint32_t j = 0; j < n; ++j)
        all[j].entry = entries[j];
    c->entries = entries;
    return true;
}

// Writes the container into the arena and reports where it is.
static bool serialize(comp *c, const image *im, uint8_t **out, size_t *out_len) {
    // line records: those of the earlier loads, then one wherever the line changes in the new code
    const pinax_state *st = c->st;
    // after a redefinition every line record is made again from the lines of the code words
    const bool     moved = c->entries != NULL;
    const uint32_t old_code = st->head && !moved ? st->head->n_code : 0,
                   old_lines = st->head && !moved ? st->head->n_lines : 0;
    vec runs = {0};
    if (old_lines) {
        runs.ptr = arena_alloc(c, (size_t)old_lines * sizeof(pnx_linerec));
        if (!runs.ptr)
            return false;
        __builtin_memcpy(runs.ptr, st->lines, (size_t)old_lines * sizeof(pnx_linerec));
        runs.n = runs.cap = old_lines;
    }
    for (uint32_t k = old_code, line = 0; k < im->code.n; ++k) {
        const uint32_t here = ((const uint32_t *)im->lines.ptr)[k];
        if (k == old_code || here != line) {
            pnx_linerec *rec = vec_push(c, &runs, sizeof(pnx_linerec));
            if (!rec)
                return false;
            *rec = (pnx_linerec){k, here};
            line = here;
        }
        const uint32_t op = ((const uint32_t *)im->code.ptr)[k] & 0xffu;
        if (pnx_ops_info[op].second) // its second word is no instruction
            ++k;
    }
    const pnx_head head = {
        .magic = {'P', 'N', 'X', 'B'},
        .version = PNX_CONTAINER_VERSION,
        .order = PNX_ORDER_MARK,
        .n_words = c->words.n,
        .n_globals = c->globals.n,
        .n_imports = c->imports.n,
        .n_iconst = c->iconst.n,
        .n_sconst = c->sconst.n,
        .n_code = im->code.n,
        .n_strbytes = c->strbytes.n,
        .n_lines = runs.n,
        .n_locals = (st->head ? st->head->n_locals : 0) + c->localrecs.n,
    };
    const size_t len = sizeof head + (size_t)head.n_words * sizeof(pnx_wordrec) +
                       (size_t)head.n_globals * sizeof(pnx_globalrec) +
                       (size_t)head.n_imports * sizeof(pnx_importrec) + (size_t)head.n_iconst * 8 +
                       (size_t)head.n_sconst * sizeof(pnx_sconst) + (size_t)head.n_code * 4 + head.n_strbytes +
                       (size_t)head.n_lines * sizeof(pnx_linerec) + (size_t)head.n_locals * sizeof(pnx_localrec);
    uint8_t *p = arena_alloc(c, len);
    if (!p)
        return false;
    *out = p;
    *out_len = len;
    __builtin_memcpy(p, &head, sizeof head);
    p += sizeof head;
    for (uint32_t k = 0; k < head.n_words; ++k) {
        const wordsym *w = (const wordsym *)c->words.ptr + k;
        if (k < c->base.words) { // a word of an earlier load: its record as it is
            pnx_wordrec rec;
            __builtin_memcpy(&rec, &st->words[k], sizeof rec);
            if (moved)
                rec.entry = c->entries[k];
            if (k == c->redefine_old) {
                rec.int_cells = c->redefine_cells[0];
                rec.str_cells = c->redefine_cells[1];
            }
            __builtin_memcpy(p, &rec, sizeof rec);
            p += sizeof(pnx_wordrec);
            continue;
        }
        pnx_wordrec rec = {.entry = w->entry, .int_cells = w->cells[0], .str_cells = w->cells[1], .counted = 1,
                           .fails = w->sig.fails, .module = (uint8_t)w->module};
        uint32_t in[2], out[2];
        count_sig(&w->sig, in, out);
        rec.int_args = (uint8_t)in[0];
        rec.str_args = (uint8_t)in[1];
        rec.int_results = (uint8_t)out[0];
        rec.str_results = (uint8_t)out[1];
        put_text(rec.name, sizeof rec.name, w->name);
        if (!sig_text(c, &w->sig, rec.signature, sizeof rec.signature))
            return syntax(c, "the signature is longer than 71 bytes");
        __builtin_memcpy(p, &rec, sizeof rec);
        p += sizeof rec;
    }
    for (uint32_t k = 0; k < head.n_globals; ++k) {
        const globalsym *g = (const globalsym *)c->globals.ptr + k;
        pnx_globalrec rec = {.kind = g->kind == GK_ARRAY ? PNX_G_ARRAY : g->kind == GK_LIST ? PNX_G_LIST : PNX_G_INT,
                             .elem_bytes = 8,
                             .capacity = g->kind == GK_SCALAR ? 1 : g->capacity};
        if (g->record != NONE) {
            rec.kind = g->kind == GK_ARRAY ? PNX_G_TABLE : g->kind == GK_POOL ? PNX_G_POOL : PNX_G_ROWS;
            rec.elem_bytes = ((const record *)c->records.ptr + g->record)->size;
        } else if (g->kind == GK_TASKS) {
            rec.kind = PNX_G_TASKS;
            rec.elem_bytes = g->table; // the call depth
            rec.reserved = g->key;     // the bytes of the region
        } else if (g->kind == GK_QUEUE) {
            rec.kind = PNX_G_QUEUE;
        } else if (g->strcap) { // text cells are rows of one length byte and N bytes
            rec.kind = g->kind == GK_LIST ? PNX_G_ROWS : PNX_G_TABLE;
            rec.elem_bytes = (uint32_t)g->strcap + 1;
        } else if (g->kind == GK_BUFFER) {
            rec.kind = PNX_G_BUFFER;
        } else if (g->kind == GK_INDEX) {
            const field *key = (const field *)c->fields.ptr + g->key;
            rec.kind = PNX_G_INDEX;
            rec.elem_bytes = PNX_FIELD(g->table, key->offset, key->extra);
            rec.reserved = key->is_str;
        }
        put_text(rec.name, sizeof rec.name, g->name);
        if (g->record != NONE) {
            put_text(rec.type, sizeof rec.type, ((const record *)c->records.ptr + g->record)->name);
        } else if (g->strcap) {
            uint8_t digits[24];
            const uint32_t n = pnx_format_int(g->strcap, digits);
            __builtin_memcpy(rec.type, "str(", 4);
            __builtin_memcpy(rec.type + 4, digits, n);
            rec.type[4 + n] = ')';
        } else if (g->kind == GK_BUFFER) {
            put_text(rec.type, sizeof rec.type, cname("buffer"));
        } else if (g->kind == GK_TASKS) {
            put_text(rec.type, sizeof rec.type, cname("tasks"));
        } else if (g->kind != GK_INDEX) {
            put_text(rec.type, sizeof rec.type, type_name(c, g->type));
        }
        if (g->bound)
            rec.reserved = 2; // meant to be bound by the host
        __builtin_memcpy(p, &rec, sizeof rec);
        p += sizeof rec;
    }
    for (uint32_t k = 0; k < head.n_imports; ++k) {
        const pnx_host *h = &c->st->hosts[((const uint32_t *)c->imports.ptr)[k]];
        pnx_importrec rec = {0};
        __builtin_memcpy(rec.module, h->module, sizeof rec.module);
        __builtin_memcpy(rec.name, h->name, sizeof rec.name);
        __builtin_memcpy(rec.signature, h->signature, sizeof rec.signature);
        __builtin_memcpy(p, &rec, sizeof rec);
        p += sizeof rec;
    }
    if (head.n_iconst)
        __builtin_memcpy(p, c->iconst.ptr, (size_t)head.n_iconst * 8);
    p += (size_t)head.n_iconst * 8;
    if (head.n_sconst)
        __builtin_memcpy(p, c->sconst.ptr, (size_t)head.n_sconst * sizeof(pnx_sconst));
    p += (size_t)head.n_sconst * sizeof(pnx_sconst);
    if (head.n_code) {
        patch_forward_calls(c, im->code.ptr, head.n_code);
        __builtin_memcpy(p, im->code.ptr, (size_t)head.n_code * 4);
    }
    p += (size_t)head.n_code * 4;
    if (head.n_strbytes)
        __builtin_memcpy(p, c->strbytes.ptr, head.n_strbytes);
    p += head.n_strbytes;
    if (head.n_lines)
        __builtin_memcpy(p, runs.ptr, (size_t)head.n_lines * sizeof(pnx_linerec));
    p += (size_t)head.n_lines * sizeof(pnx_linerec);
    // local records: those of the earlier loads, then this one's; after a redefinition the two
    // words that changed code trade their records too
    for (uint32_t k = 0; k < head.n_locals; ++k, p += sizeof(pnx_localrec)) {
        const uint32_t before = head.n_locals - c->localrecs.n;
        pnx_localrec rec;
        if (k < before)
            __builtin_memcpy(&rec, st->localrecs + (size_t)k * sizeof rec, sizeof rec);
        else
            rec = ((const pnx_localrec *)c->localrecs.ptr)[k - before];
        if (moved && k < before && rec.word == c->redefine_old)
            rec.word = c->redefine_new;
        else if (moved && k >= before && rec.word == c->redefine_new)
            rec.word = c->redefine_old;
        __builtin_memcpy(p, &rec, sizeof rec);
    }
    return true;
}

// ---- the declarations, kept for the next load (RFC-0014) -----------------------------------

// What a load leaves in the block for the loads after it: the compiler's tables in one piece,
// with every name it holds copied in, so the host may let go of the source.
typedef struct symtab {
    vec    words, consts, globals, records, fields, btypes, dtypes, srcmods, externs;
    size_t bytes;
    // the session of pinax_eval(): its module number (NONE before the first line) and the
    // modules its lines opened
    uint32_t session, n_opened;
    name     opened[MAX_MODULES], opened_real[MAX_MODULES];
} symtab;

typedef void name_fn(name *n, void *user);

static void each_name(symtab *t, name_fn *f, void *user) {
    for (uint32_t k = 0; k < t->words.n; ++k) {
        wordsym *w = (wordsym *)t->words.ptr + k;
        f(&w->name, user);
    }
    for (uint32_t k = 0; k < t->consts.n; ++k)
        f(&((constant *)t->consts.ptr + k)->name, user);
    for (uint32_t k = 0; k < t->globals.n; ++k)
        f(&((globalsym *)t->globals.ptr + k)->name, user);
    for (uint32_t k = 0; k < t->records.n; ++k)
        f(&((record *)t->records.ptr + k)->name, user);
    for (uint32_t k = 0; k < t->fields.n; ++k)
        f(&((field *)t->fields.ptr + k)->name, user);
    for (uint32_t k = 0; k < t->dtypes.n; ++k)
        f(&((dtype *)t->dtypes.ptr + k)->name, user);
    for (uint32_t k = 0; k < t->srcmods.n; ++k)
        f((name *)t->srcmods.ptr + k, user);
    for (uint32_t k = 0; k < t->externs.n; ++k)
        f(&((externsym *)t->externs.ptr + k)->name, user);
    for (uint32_t k = 0; k < t->n_opened; ++k) {
        f(&t->opened[k], user);
        f(&t->opened_real[k], user);
    }
}

#define TABLES(X)                                                                                            \
    X(words, wordsym) X(consts, constant) X(globals, globalsym) X(records, record) X(fields, field)          \
    X(btypes, sig) X(dtypes, dtype) X(srcmods, name) X(externs, externsym)

// A name whose bytes will not outlive this load lies in the source or in the working memory.
typedef struct keeper {
    const uint8_t *lo[2], *hi[2];
    uint8_t       *at;
    size_t         total;
} keeper;

static bool passing(const keeper *k, const name *n) {
    const uint8_t *p = (const uint8_t *)n->text;
    return n->length && ((p >= k->lo[0] && p < k->hi[0]) || (p >= k->lo[1] && p < k->hi[1]));
}

static void name_size(name *n, void *user) {
    keeper *k = user;
    if (passing(k, n))
        k->total += n->length;
}

static void name_keep(name *n, void *user) {
    keeper *k = user;
    if (passing(k, n)) {
        __builtin_memcpy(k->at, n->text, n->length);
        n->text = (void *)k->at;
        k->at += n->length;
    }
}

typedef struct shift {
    const uint8_t *lo, *hi;
    ptrdiff_t      by;
} shift;

static void name_shift(name *n, void *user) {
    const shift *s = user;
    const uint8_t *p = (const uint8_t *)n->text;
    if (n->length && p >= s->lo && p < s->hi)
        n->text = (void *)(p + s->by);
}

// The tables as one piece in the working memory.
static symtab *keep_symbols(comp *c, pinax_str source, const uint8_t *work) {
    symtab now = {c->words, c->consts, c->globals, c->records, c->fields, c->btypes, c->dtypes, c->srcmods,
                  c->externs, 0, NONE, 0, {{0}}, {{0}}};
    const symtab *old = c->st->symbols;
    if (c->eval) { // this line's session: its number and what it has opened by now
        now.session = c->module;
        now.n_opened = c->n_modules;
        __builtin_memcpy(now.opened, c->modules, sizeof now.opened);
        __builtin_memcpy(now.opened_real, c->module_real, sizeof now.opened_real);
    } else if (old) {
        now.session = old->session;
        now.n_opened = old->n_opened;
        __builtin_memcpy(now.opened, old->opened, sizeof now.opened);
        __builtin_memcpy(now.opened_real, old->opened_real, sizeof now.opened_real);
    }
    keeper k = {{source.ptr, work}, {source.ptr + source.len, c->arena_end}, NULL, 0};
    each_name(&now, name_size, &k);
    size_t bytes = sizeof(symtab) + ((k.total + 7u) & ~(size_t)7u);
#define X(v, t) bytes += ((size_t)now.v.n * sizeof(t) + 7u) & ~(size_t)7u;
    TABLES(X)
#undef X
    uint8_t *p = arena_alloc(c, bytes);
    if (!p)
        return NULL;
    symtab *t = (symtab *)p;
    p += sizeof(symtab);
#define X(v, t_)                                                          \
    t->v = (vec){p, now.v.n, now.v.n};                                    \
    if (now.v.n)                                                          \
        __builtin_memcpy(p, now.v.ptr, (size_t)now.v.n * sizeof(t_));     \
    p += ((size_t)now.v.n * sizeof(t_) + 7u) & ~(size_t)7u;
    TABLES(X)
#undef X
    for (uint32_t k = 0; k < t->words.n; ++k) // the names of parameters stay behind
        ((wordsym *)t->words.ptr + k)->sig.names = NULL;
    for (uint32_t k = 0; k < t->btypes.n; ++k)
        ((sig *)t->btypes.ptr + k)->names = NULL;
    t->bytes = bytes;
    t->session = now.session;
    t->n_opened = now.n_opened;
    __builtin_memcpy(t->opened, now.opened, sizeof t->opened);
    __builtin_memcpy(t->opened_real, now.opened_real, sizeof t->opened_real);
    k.at = p;
    each_name(t, name_keep, &k);
    return t;
}

// The tables have moved by `by` bytes: what points into them moves along.
static void move_symbols(symtab *t, const uint8_t *from, ptrdiff_t by) {
    const size_t bytes = t->bytes;
#define X(v, t_) t->v.ptr = (uint8_t *)t->v.ptr + by;
    TABLES(X)
#undef X
    shift s = {from, from + bytes, by};
    each_name(t, name_shift, &s);
}

void pnx_symbols_move(void *symbols, const void *from, ptrdiff_t by) {
    move_symbols(symbols, from, by);
}

// FNV-1a over the sizes and a few places of what the kept tables hold: an image made by a
// build that lays them out otherwise is refused.
uint64_t pnx_symbols_layout(uint64_t hash) {
    const uint64_t parts[] = {sizeof(symtab), sizeof(wordsym), sizeof(constant), sizeof(globalsym), sizeof(record),
                              sizeof(field), sizeof(sig), sizeof(dtype), sizeof(name), sizeof(externsym),
                              offsetof(wordsym, sig), offsetof(globalsym, record), offsetof(field, offset),
                              offsetof(symtab, bytes), MAX_PARAMS};
    for (size_t k = 0; k < sizeof parts / sizeof parts[0]; ++k)
        hash = (hash ^ parts[k]) * 0x100000001b3u;
    return hash;
}

// The tables of the loads before this one, copied into the working memory to grow there.
static bool take_symbols(comp *c, image *im) {
    const pinax_state *st = c->st;
    const symtab *t = st->symbols;
    if (!t)
        return true;
#define COPY(dst, src, count, type_)                                       \
    if (count) {                                                           \
        void *p = arena_alloc(c, (size_t)(count) * sizeof(type_));         \
        if (!p)                                                            \
            return false;                                                  \
        __builtin_memcpy(p, src, (size_t)(count) * sizeof(type_));         \
        dst = (vec){p, (uint32_t)(count), (uint32_t)(count)};              \
    }
#define X(v, t_) COPY(c->v, t->v.ptr, t->v.n, t_)
    TABLES(X)
#undef X
    const pnx_head *h = st->head;
    COPY(c->iconst, st->iconst, h->n_iconst, int64_t)
    COPY(c->sconst, st->sconst, h->n_sconst, pnx_sconst)
    COPY(c->strbytes, st->segment[PNX_SEG_CONST], h->n_strbytes, uint8_t)
    COPY(im->code, st->code, h->n_code, uint32_t)
    if (h->n_code) { // the lines of that code are kept as records; these are placeholders
        void *p = arena_alloc(c, (size_t)h->n_code * sizeof(uint32_t));
        if (!p)
            return false;
        im->lines = (vec){p, h->n_code, h->n_code};
    }
    if (h->n_imports) {
        uint32_t *p = arena_alloc(c, (size_t)h->n_imports * sizeof(uint32_t));
        if (!p)
            return false;
        for (uint32_t k = 0; k < h->n_imports; ++k)
            p[k] = (uint32_t)(st->imports[k] - st->hosts);
        c->imports = (vec){p, h->n_imports, h->n_imports};
    }
#undef COPY
    c->base = (struct counts){c->words.n, c->consts.n, c->globals.n, c->records.n, c->dtypes.n, c->externs.n};
    return true;
}

pinax_str pnx_module_name(const pinax_state *st, uint32_t number) {
    const symtab *t = st->symbols;
    pinax_str out = {(const unsigned char *)"", 0};
    if (t && number < t->srcmods.n) {
        const name *n = (const name *)t->srcmods.ptr + number;
        if (n->length)
            out = (pinax_str){(const unsigned char *)n->text, n->length};
    }
    return out;
}

uint32_t pnx_module_number(const pinax_state *st, const char *text, size_t length) {
    const symtab *t = st->symbols;
    for (uint32_t k = 0; t && k < t->srcmods.n; ++k) {
        const name *n = (const name *)t->srcmods.ptr + k;
        if (n->length == length && length && __builtin_memcmp(n->text, text, length) == 0)
            return k;
    }
    return UINT32_MAX;
}

// The name a source is loaded under is the name of its module when it is written like a name.
static bool module_name(const char *file, size_t *length) {
    size_t n = 0;
    for (; file[n]; ++n) {
        const char ch = file[n];
        const bool letter = (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') || ch == '_';
        if (!letter && !(n && ch >= '0' && ch <= '9'))
            return false;
    }
    *length = n;
    return n > 0 && n <= 31;
}

// What the last pinax_eval() left (pinax.h).
uint32_t pinax_eval_count(const pinax_state *st) {
    return st ? st->eval_n : 0;
}

pinax_err pinax_eval_value(const pinax_state *st, uint32_t index, pinax_value *out) {
    if (!st || !out)
        return PINAX_ERR_ARGUMENT;
    if (index >= st->eval_n)
        return PINAX_ERR_NOT_FOUND;
    const tid type = st->eval_type[index];
    // the type's name, from the declarations the state keeps
    comp view = {.st = (pinax_state *)st};
    const symtab *t = st->symbols;
    if (t) {
        view.globals = t->globals;
        view.dtypes = t->dtypes;
    }
    const name n = type_name(&view, type);
    const tid base = base_of(&view, type);
    *out = (pinax_value){.kind = bank_of(type) == BANK_S ? PINAX_VALUE_STR : base == TY_BOOL ? PINAX_VALUE_BOOL
                                 : base == TY_ERR ? PINAX_VALUE_ERR : PINAX_VALUE_INT,
                         .i = st->eval_int[index], .s = st->eval_str[index],
                         .type = {(const unsigned char *)n.text, n.length}};
    if (out->kind == PINAX_VALUE_ERR) {
        uint32_t offset, length;
        pnx_err_name(out->i, &offset, &length);
        out->s = (pinax_str){(const unsigned char *)pnx_static_text + offset, length};
    }
    return PINAX_OK;
}

// A list that `truncate` cuts back may not have its rows kept anywhere they would outlive the
// cut: in a field, a global, a named row or an index (RFC-0017 section 3.2). A row kept in a
// local is checked when it is used (range).
static bool truncs_ok(comp *c) {
    for (uint32_t k = 0; k < c->truncs.n; ++k) {
        const trunc_site t = ((const trunc_site *)c->truncs.ptr)[k];
        const tid        row = row_type(t.global);
        bool             kept = false;
        for (uint32_t i = 0; !kept && i < c->fields.n; ++i)
            kept = ((const field *)c->fields.ptr)[i].type == row;
        for (uint32_t i = 0; !kept && i < c->globals.n; ++i) {
            const globalsym *g = (const globalsym *)c->globals.ptr + i;
            kept = i != t.global && ((g->kind == GK_INDEX && g->table == t.global) ||
                                     (g->kind != GK_INDEX && g->record == NONE && g->type == row));
        }
        for (uint32_t i = 0; !kept && i < c->consts.n; ++i)
            kept = ((const constant *)c->consts.ptr)[i].type == row;
        if (kept)
            return error_at(c, PINAX_ERR_TYPE, t.line, t.column,
                            "this list's rows are kept by a field, a global, a named row or an index; it cannot be truncated");
    }
    return true;
}

static pinax_err load_source(pinax_state *st, const char *file, pinax_str source, uint32_t mode, bool eval) {
    if (!st || (!source.ptr && source.len))
        return PINAX_ERR_ARGUMENT;
    st->eval_n = 0;
    if (st->suspended || st->host_cur) {
        st->error_text = "a run is suspended or a host word is running; finish it before loading";
        return PINAX_ERR_STATE;
    }
    if (st->head && !st->symbols) {
        st->error_text = "a state that loaded bytecode takes no source after it";
        return PINAX_ERR_STATE;
    }
    if (mode != PINAX_LOAD_CODE && mode != PINAX_LOAD_DATA)
        return PINAX_ERR_ARGUMENT;
    if (source.len > UINT32_MAX) {
        st->error_text = "the source is longer than 4 GiB";
        return PINAX_ERR_ARGUMENT;
    }
    const pnx_program before = pnx_program_now(st);

    // the unused tail of the block is the compiler's working memory
    uint8_t *const tail = st->block + ((st->used + 15u) & ~(size_t)15u);
    uint8_t *const end = st->block + st->block_len;
    comp      c = {.st = st, .file = file ? file : "", .data_only = mode == PINAX_LOAD_DATA, .path_global = NONE,
                   .eval = eval,
                   .redefine_old = NONE,
                   .redefine_new = NONE};
    c.arena = tail < end ? tail : end;
    c.arena_end = end;
    const uint8_t *const work = c.arena;
    c.lx = &c.lexer;
    pnx_lex_init(&c.lexer, source.ptr, source.len);

    image    im = {0};
    bool     has_top = false;
    uint8_t *container = NULL;
    size_t   length = 0;
    symtab  *tables = NULL;
    c.word_fn = arena_alloc(&c, sizeof(fn));
    c.top_fn = c.word_fn ? arena_alloc(&c, sizeof(fn)) : NULL;
    if (c.top_fn && take_symbols(&c, &im)) {
        // this module: the number after those loaded, and its name when the host gave one
        name   self = {0};
        size_t self_len = 0;
        if (module_name(c.file, &self_len)) {
            uint8_t *text = arena_alloc(&c, self_len);
            if (text) {
                __builtin_memcpy(text, c.file, self_len);
                self = (name){(void *)text, (uint32_t)self_len};
            }
        }
        const symtab *kept_before = st->symbols;
        const bool    resumed = eval && kept_before && kept_before->session != NONE;
        if (resumed) { // a further line of the session: its module, and the modules it has opened
            c.module = (uint16_t)kept_before->session;
            c.n_modules = kept_before->n_opened;
            __builtin_memcpy(c.modules, kept_before->opened, sizeof c.modules);
            __builtin_memcpy(c.module_real, kept_before->opened_real, sizeof c.module_real);
        } else {
            c.module = (uint16_t)c.srcmods.n;
        }
        name *slot = c.err == PINAX_OK && !resumed ? vec_push(&c, &c.srcmods, sizeof(name)) : NULL;
        if (slot) {
            *slot = self;
            bool taken = self.length && (standard_module(self) || source_module(&c, self) != NONE);
            for (uint32_t k = 0; self.length && !taken && k < st->n_hosts; ++k)
                taken = name_eq(cname(st->hosts[k].module), self);
            if (taken)
                error_at(&c, PINAX_ERR_STATE, 0, 0, "a module of this name is already loaded or registered");
            else if (c.module > 255)
                error_at(&c, PINAX_ERR_NO_ROOM, 0, 0, "a state holds at most 256 sources");
        }
    }
    if (c.err == PINAX_OK && c.top_fn) {
        fn_reset(c.word_fn, false);
        fn_reset(c.top_fn, true);
        static const sig no_results = {0};
        c.top_fn->sig = &no_results;
        c.f = c.top_fn;
        if (compile(&c, &im, &has_top) && truncs_ok(&c) && (c.redefine_new == NONE || move_redefined(&c, &im)) &&
            serialize(&c, &im, &container, &length))
            tables = keep_symbols(&c, source, work);
    }
    if (c.err != PINAX_OK) {
        st->error_text = c.message;
        st->n_hosts = before.n_hosts;
        if (eval && c.unfinished) { // the host reads on and gives the longer text
            st->error_text = "unfinished: the text ends inside a definition or a block";
            return PINAX_ERR_SYNTAX;
        }
        if (st->diag) {
            const pinax_str name = {(const unsigned char *)c.file, __builtin_strlen(c.file)};
            const pinax_str text = {(const unsigned char *)c.message, __builtin_strlen(c.message)};
            st->diag(st->diag_user, name, c.line, c.column, text);
        }
        return c.err;
    }

    // what the line of a session leaves, and whether it declared anything worth keeping
    const uint32_t n_values = eval ? c.eval_sig.n_out : 0;
    for (uint32_t k = 0; k < n_values; ++k)
        st->eval_type[k] = c.eval_sig.out[k];
    const symtab *session_before = st->symbols;
    const bool    declared = !eval || c.words.n != c.base.words + (has_top ? 1u : 0u) || c.consts.n != c.base.consts ||
                             c.globals.n != c.base.globals || c.records.n != c.base.records ||
                             c.dtypes.n != c.base.dtypes || c.externs.n != c.base.externs ||
                             !session_before || session_before->session == NONE ||
                             c.n_modules != session_before->n_opened;

    // The tables and the container move to the very end of the block, out of the way of the
    // loader, which takes its room from the front; the block is shortened for that moment.
    // The tables were made last and lie higher, so they move first.
    const size_t   table_bytes = tables->bytes;
    const uint8_t *const made_at = (const uint8_t *)tables;
    uint8_t *const parked_tables = end - table_bytes;
    __builtin_memmove(parked_tables, tables, table_bytes);
    uint8_t *const parked = parked_tables - length;
    __builtin_memmove(parked, container, length);
    const size_t full = st->block_len;
    st->block_len = (size_t)(parked - st->block);
    const pinax_str bytes = {parked, length};
    pinax_err err = pnx_load_container(st, bytes, before.head ? before.head->n_globals : 0);
    symtab *kept = NULL;
    if (err == PINAX_OK) {
        st->block_len = (size_t)(parked_tables - st->block);
        kept = pnx_take(st, table_bytes, 8);
        if (!kept) {
            pnx_program_restore(st, &before);
            st->error_text = "block too small for the code and its globals";
            err = PINAX_ERR_NO_ROOM;
        }
    }
    st->block_len = full;
    if (err == PINAX_ERR_NO_ROOM)
        st->needed += length + table_bytes;
    if (err != PINAX_OK)
        return err;
    __builtin_memcpy(kept, parked_tables, table_bytes);
    move_symbols(kept, made_at, (const uint8_t *)kept - made_at);
    st->symbols = kept;

    if (has_top) { // code outside definitions runs once, now
        const uint64_t budget = st->budget;
        if (mode == PINAX_LOAD_DATA) // its stores run on a small budget of their own, not the host's
            st->budget = 1024 + 16 * (uint64_t)source.len;
        int64_t    ints[MAX_PARAMS];
        pinax_str  strs[MAX_PARAMS];
        pinax_args values = {.ri = ints, .rs = strs};
        err = pinax_run(st, st->head->n_words - 1, n_values ? &values : NULL);
        st->budget = budget;
        if (err == PINAX_OK && n_values) { // in the order of the stack, each bank counted on its own
            for (uint32_t k = 0, ni = 0, ns = 0; k < n_values; ++k) {
                const bool text = bank_of(st->eval_type[k]) == BANK_S;
                st->eval_int[k] = text ? 0 : ints[ni++];
                st->eval_str[k] = text ? strs[ns++] : (pinax_str){(const unsigned char *)"", 0};
            }
            st->eval_n = n_values;
        }
        if (mode == PINAX_LOAD_DATA && err == PINAX_SUSPENDED) {
            st->error_text = "the data source did not finish inside its budget";
            err = PINAX_ERR_STATE;
        }
        if (err == PINAX_SUSPENDED || err == PINAX_WAITING) { // pinax_resume() finishes the load, or undoes it on a fault
            st->loading = true;
            st->before_load = before;
        }
        if (err != PINAX_OK && err != PINAX_SUSPENDED && err != PINAX_WAITING) {
            // a fault while loading: what this load added does not stay, as after any other load error
            const char *text = st->error_text;
            pnx_program_restore(st, &before);
            st->error_text = text;
        }
    }
    // a line that only ran code leaves the program as it was, unless a task it spawned still
    // lives in that code. What the line did to the globals stays: their storage is shared, and
    // their state (a list's length, a queue's head) is copied back into the older table.
    if (err == PINAX_OK && !declared && !pnx_tasks_alive(st)) {
        if (before.head && before.head->n_globals)
            __builtin_memcpy(before.globals, st->globals, (size_t)before.head->n_globals * sizeof(pnx_global));
        pnx_program_restore(st, &before);
    }
    return err;
}

pinax_err pinax_load(pinax_state *st, const char *file, pinax_str source, uint32_t mode) {
    return load_source(st, file, source, mode, false);
}

pinax_err pinax_eval(pinax_state *st, pinax_str line) {
    return load_source(st, "(eval)", line, PINAX_LOAD_CODE, true);
}
