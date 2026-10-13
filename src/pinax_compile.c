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
enum { TY_NONE, TY_INT, TY_BOOL, TY_ERR, TY_STEM, TY_COLL, TY_STR = TY_SBANK | 6, TY_FIRST_DECLARED = 16 };
enum { BANK_I, BANK_S, BANK_NONE };
enum { E_SLOT, E_LOCAL, E_CONST, E_COND };
enum { B_IF, B_LOOP, B_ORELSE, B_CASE, B_OF, B_DO };
enum {
    STEM_RANGE, STEM_BYTES, STEM_TIMES, STEM_COLL, STEM_WORDS, STEM_ROWS, STEM_STRS,
    STEM_LINES, STEM_SPLIT, STEM_SPLIT_BYTE // like STEM_WORDS, these take their next piece off the string
};
enum { GK_SCALAR, GK_ARRAY, GK_LIST, GK_BUFFER, GK_INDEX, GK_QUEUE, GK_POOL };
enum { L_PLAIN, L_OWNS, L_MOVED }; // what a local holds of a value that must be given exactly once
#define FWD_MARK 0x80000000u // in a call: the number of a forward declaration, patched at the end
#define TY_DISTINCT 0x4000u  // declared by `type`: this plus its number
enum { USE_EACH, USE_FOLD, USE_TIMES, USE_COUNT, USE_ANY, USE_ALL, USE_FIRST, USE_EACH_INDEX };

#define MAX_STACK 64
#define MAX_BLOCKS 32
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
    int64_t value;   // E_CONST
} entry;

typedef struct sig {
    uint8_t n_in, n_out;
    bool    fails, named;
    tid     in[MAX_PARAMS], out[MAX_PARAMS];
    bool    in_owned[MAX_PARAMS], out_owned[MAX_PARAMS];
    name    in_name[MAX_PARAMS], out_name[MAX_PARAMS];
} sig;

typedef struct wordsym {
    name     name;
    sig      sig;
    uint32_t entry;
    uint16_t cells[2];
} wordsym;

typedef struct local {
    name    name;
    tid     type;
    uint8_t reg;
    uint8_t state; // L_PLAIN, L_OWNS, L_MOVED
} local;

// The types of the stack at one moment; `hidden` marks the entries that belong to a running
// block and not to the code.
typedef struct shot {
    tid      types[MAX_STACK];
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
    uint8_t  owns_in[MAX_LOCALS], owns_out[MAX_LOCALS]; // and the state of the locals then
    // case
    uint32_t next_test;   // jump of the last test, taken when no value matched
    uint8_t  subject;
    tid      subject_type;
    bool     has_otherwise;
    uint32_t n_values;
    int64_t  values[MAX_CASE]; // integers, or string constant numbers
    // do block of a combining word
    uint8_t  use, stem, cur, lim, acc, n_hidden;
    uint8_t  val, idx;   // first: the register of the element found; each_index: the counter
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
    bool     dead; // the code here cannot be reached
    vec      code; // of uint32_t
    uint32_t start, last_check;
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
} globalsym;

// A field of a record: where it lies and how it is read. `extra` is the width code, or the
// capacity N of a str(N).
typedef struct field {
    name     name;
    tid      type;
    uint16_t offset;
    uint8_t  extra;
    bool     is_str;
} field;

typedef struct record {
    name     name;
    uint32_t first, count; // its fields in comp.fields
    uint32_t size;
} record;

// forward word NAME ( effect ) : a word called before it is defined
typedef struct fwd {
    name     name;
    sig      sig;
    uint32_t index; // its word number once defined, NONE before
} fwd;

// type NAME base : the same representation, another type
typedef struct dtype {
    name name;
    tid  base;
} dtype;

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
    vec          dtypes;   // dtype
    bool         use_strings, use_format;
    bool         data_only; // PINAX_LOAD_DATA: declarations, literals, stores and rows only
    bool         sink;      // the word being compiled may take an owned value off the stack
    name         modules[MAX_MODULES];
    uint32_t     n_modules;
    pinax_err    err;
    const char  *message;
    uint32_t     line, column;
} comp;

// ---- errors and memory ------------------------------------------------------------------

static bool error_at(comp *c, pinax_err err, uint32_t line, uint32_t column, const char *message) {
    if (c->err == PINAX_OK) {
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
    return type >= TY_FIRST_DECLARED && type < TY_DISTINCT;
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
    if (base_of(c, type) != type)
        return ((const dtype *)c->dtypes.ptr + ((type & ~TY_SBANK) - TY_DISTINCT))->name;
    return cname(type == TY_INT ? "int" : type == TY_BOOL ? "bool" : type == TY_ERR ? "err" : "str");
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
    return TY_NONE;
}

// A type as written in a signature or a declaration: a basic type, or the name of a table of
// records, which is the type of its rows.
static tid type_of_name(const comp *c, name n) {
    const tid basic = basic_type(n);
    if (basic != TY_NONE)
        return basic;
    for (uint32_t k = 0; k < c->dtypes.n; ++k) {
        const dtype *d = (const dtype *)c->dtypes.ptr + k;
        if (name_eq(d->name, n))
            return (tid)((TY_DISTINCT + k) | (d->base & TY_SBANK));
    }
    const uint32_t g = find_global(c, n);
    if (g != NONE) {
        const globalsym *sym = (const globalsym *)c->globals.ptr + g;
        if (sym->kind != GK_SCALAR && (sym->forward || sym->record != NONE))
            return row_type(g);
    }
    return TY_NONE;
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
        bool owned = false;
        if (is_word(c, PNX_KW_owned)) { // owned type
            owned = true;
            if (!advance(c))
                return false;
        }
        if (c->tok.kind != PNX_T_NAME)
            return syntax(c, "a parameter is a type, or a name and a type");
        name first = tok_name(c), param = {0}, type = first;
        if (!advance(c))
            return false;
        if (!owned && is_word(c, PNX_KW_owned)) { // name owned type
            owned = true;
            param = first;
            if (!advance(c))
                return false;
            if (c->tok.kind != PNX_T_NAME)
                return syntax(c, "owned is followed by a type");
            type = tok_name(c);
            if (!advance(c))
                return false;
        } else if (c->tok.kind == PNX_T_NAME) {
            if (owned)
                return syntax(c, "owned stands right before the type: name owned type");
            param = first;
            type = tok_name(c);
            if (!advance(c))
                return false;
        }
        const tid t = type_of_name(c, type);
        if (t == TY_NONE)
            return type_error(c, name_is(type, "float") ? "float is not available: this build has no F bank"
                                                        : "this type is not known here");
        uint8_t *count = outputs ? &s->n_out : &s->n_in;
        if (*count == MAX_PARAMS)
            return syntax(c, "too many parameters");
        if (outputs) {
            s->out[*count] = t;
            s->out_owned[*count] = owned;
            s->out_name[*count] = param;
        } else {
            s->in[*count] = t;
            s->in_owned[*count] = owned;
            s->in_name[*count] = param;
            if (param.length)
                any_named = true;
            else
                any_bare = true;
        }
        ++*count;
        if (c->tok.kind == PNX_T_COMMA) {
            if (!advance(c))
                return false;
            if (c->tok.kind != PNX_T_NAME && !is_word(c, PNX_KW_owned))
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
            const name pn = side ? s->out_name[k] : s->in_name[k];
            const name tn = type_name(c, side ? s->out[k] : s->in[k]);
            if (k)
                PUT(", ", 2);
            if (pn.length) {
                PUT(pn.text, pn.length);
                PUT(" ", 1);
            }
            if (side ? s->out_owned[k] : s->in_owned[k])
                PUT("owned ", 6);
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
    const bool ok = advance(c) && parse_sig(c, s);
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
    if (!slot)
        return NONE;
    *slot = word;
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

static bool push(comp *c, entry e) {
    fn *f = c->f;
    if (f->depth == MAX_STACK)
        return syntax(c, "the stack is deeper than 64 values here");
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
        ok = emit_def(c, PNX_ABC(e->type == TY_STR ? PNX_MOVS : PNX_MOVI, reg, e->reg, 0), bank_of(e->type)) != NONE;
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
    if (*index > 255)
        return syntax(c, "only the first 256 globals can be arrays or lists for now");
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
    for (uint32_t k = 0; k < c->words.n; ++k) {
        if (name_eq(all[k].name, n)) {
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

static bool module_used(const comp *c, name module) {
    for (uint32_t k = 0; k < c->n_modules; ++k) {
        if (name_eq(c->modules[k], module))
            return true;
    }
    return false;
}

// A host word by bare name among the modules in use, or by module and name. Returns the index
// into state->hosts, NONE when there is none; *ambiguous when two modules have the name.
static uint32_t find_host(const comp *c, name module, name word, bool *ambiguous) {
    uint32_t found = NONE;
    *ambiguous = false;
    for (uint32_t k = 0; k < c->st->n_hosts; ++k) {
        const pnx_host *h = &c->st->hosts[k];
        if (!name_eq(cname(h->name), word))
            continue;
        if (module.length ? !name_eq(cname(h->module), module) : !module_used(c, cname(h->module)))
            continue;
        if (found != NONE)
            *ambiguous = true;
        found = k;
    }
    return found;
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
    for (uint32_t k = 0; k < c->consts.n; ++k) {
        if (name_eq(all[k].name, n))
            return &all[k];
    }
    return NULL;
}

static uint32_t find_global(const comp *c, name n) {
    const globalsym *all = c->globals.ptr;
    for (uint32_t k = 0; k < c->globals.n; ++k) {
        if (name_eq(all[k].name, n))
            return k;
    }
    return NONE;
}

static uint32_t find_record(const comp *c, name n) {
    const record *all = c->records.ptr;
    for (uint32_t k = 0; k < c->records.n; ++k) {
        if (name_eq(all[k].name, n))
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
    BW_ALLOC, BW_FREE, BW_IS_ALIVE, BW_COUNT, BW_ANY, BW_ALL, BW_FIRST, BW_EACH_INDEX, BW_MINIMUM, BW_MAXIMUM,
    BW_SLICE, BW_CUT, BW_TRY_CUT, BW_TRIM, BW_TRIM_LEFT, BW_TRIM_RIGHT, BW_LINES, BW_SPLIT, BW_FIND_LAST, BW_COMPARE,
    BW_HEX, BW_BIN, BW_HEX_PAD, BW_ZERO_PAD, BW_PAD_LEFT, BW_PAD_RIGHT
};
static const struct builtin {
    const char *name;
    uint8_t     id;
    uint8_t     module; // 0: core, 1: needs `use strings`, 2: needs `use format`
} builtins[] = {
    {"range", BW_RANGE, false},     {"sum", BW_SUM, false},        {"each", BW_EACH, false},
    {"fold", BW_FOLD, false},       {"times", BW_TIMES, false},    {"len", BW_LEN, false},
    {"is_empty", BW_IS_EMPTY, false}, {"byte_at", BW_BYTE_AT, true}, {"bytes", BW_BYTES, true},
    {"idx", BW_IDX, false},         {"capacity", BW_CAPACITY, false}, {"is_full", BW_IS_FULL, false},
    {"clear", BW_CLEAR, false},     {"push", BW_PUSH, false},      {"pop", BW_POP, false},
    {"find", BW_FIND, true},        {"to_int", BW_TO_INT, true},   {"to_str", BW_TO_STR, true},
    {"assert", BW_ASSERT, false},   {"words", BW_WORDS, true},     {"err_text", BW_ERR_TEXT, false},
    {"add_row", BW_ADD_ROW, false}, {"is_none", BW_IS_NONE, false}, {"none", BW_NONE, false},
    {"row_index", BW_ROW_INDEX, false}, {"max", BW_MAX, false},    {"min", BW_MIN, false},
    {"append", BW_APPEND, false},   {"append_byte", BW_APPEND_BYTE, false}, {"append_int", BW_APPEND_INT, false},
    {"contents", BW_CONTENTS, false}, {"take", BW_TAKE, false},    {"skip", BW_SKIP, true},
    {"take_last", BW_TAKE_LAST, true}, {"skip_last", BW_SKIP_LAST, true},
    {"starts_with", BW_STARTS_WITH, true}, {"ends_with", BW_ENDS_WITH, true}, {"contains", BW_CONTAINS, true},
    {"lookup", BW_LOOKUP, false},   {"insert", BW_INSERT, false},  {"remove", BW_REMOVE, false},
    {"replace", BW_REPLACE, true},  {"put", BW_PUT, false},        {"peek", BW_PEEK, false},
    {"alloc", BW_ALLOC, false},     {"free", BW_FREE, false},      {"is_alive", BW_IS_ALIVE, false},
    {"count", BW_COUNT, false},     {"any", BW_ANY, false},        {"all", BW_ALL, false},
    {"first", BW_FIRST, false},     {"each_index", BW_EACH_INDEX, false},
    {"minimum", BW_MINIMUM, false}, {"maximum", BW_MAXIMUM, false},
    {"slice", BW_SLICE, 1},         {"cut", BW_CUT, 1},            {"try_cut", BW_TRY_CUT, 1},
    {"trim", BW_TRIM, 1},           {"trim_left", BW_TRIM_LEFT, 1}, {"trim_right", BW_TRIM_RIGHT, 1},
    {"lines", BW_LINES, 1},         {"split", BW_SPLIT, 1},        {"find_last", BW_FIND_LAST, 1},
    {"compare", BW_COMPARE, 1},     {"pad_left", BW_PAD_LEFT, 1},  {"pad_right", BW_PAD_RIGHT, 1},
    {"hex", BW_HEX, 2},             {"bin", BW_BIN, 2},            {"hex_pad", BW_HEX_PAD, 2},
    {"zero_pad", BW_ZERO_PAD, 2},
};

// `len` and `is_empty` belong to the core for arrays and lists and to `strings` for a str; the
// others marked so are words of `strings` only.
static const struct builtin *find_builtin(const comp *c, name n) {
    for (size_t k = 0; k < sizeof builtins / sizeof builtins[0]; ++k) {
        if (name_is(n, builtins[k].name) &&
            (builtins[k].module == 0 || (builtins[k].module == 1 && c->use_strings) ||
             (builtins[k].module == 2 && c->use_format)))
            return &builtins[k];
    }
    return NULL;
}

// A name every part of the source sees: a word, a constant, a global, a type, a core word.
static bool name_is_global(comp *c, name n) {
    bool ambiguous;
    return find_word(c, n, NULL) || find_fwd(c, n, NULL) || find_core(n) || type_of_name(c, n) != TY_NONE ||
           name_is(n, "float") || name_is(n, "raw") ||
           find_record(c, n) != NONE ||
           name_is(n, "err") || find_const(c, n) || find_global(c, n) != NONE || find_builtin(c, n) ||
           find_host(c, (name){0}, n, &ambiguous) != NONE;
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
    return emit_def(c, PNX_ABC(type == TY_STR ? PNX_MOVS : PNX_MOVI, to, from, 0), bank_of(type)) != NONE;
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
            return move_slot(c, copy.type, slot_of(c, f->depth - 1), source);
        }
        return push(c, copy);
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

static bool fits8(int64_t v) {
    return v >= INT8_MIN && v <= INT8_MAX;
}

static bool arith_word(comp *c, uint32_t op) {
    fn *f = c->f;
    if (!need(c, 2))
        return false;
    entry *a = &f->stack[f->depth - 2], *b = &f->stack[f->depth - 1];
    const tid result = a->type;
    if (a->type != b->type || base_of(c, a->type) != TY_INT ||
        (a->type != TY_INT && op != PNX_ADD && op != PNX_SUB))
        return type_error(c, a->type == b->type && base_of(c, a->type) == TY_INT
                                 ? "values of a declared type are added and subtracted; for other arithmetic take raw"
                                 : "this word takes two int values, or two of the same declared type");
    const uint32_t dst = slot_of(c, f->depth - 2);
    uint32_t ra, rb;
    if ((op == PNX_ADD || op == PNX_SUB) && b->kind == E_CONST && fits8(op == PNX_ADD ? b->value : -b->value)) {
        const int64_t imm = op == PNX_ADD ? b->value : -b->value;
        if (!reg_of(c, f->depth - 2, &ra) || emit_def(c, PNX_ABC(PNX_ADDI, dst, ra, (uint8_t)(int8_t)imm), BANK_I) == NONE)
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
        if (f->stack[f->depth - 1].type != TY_INT)
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
        if (f->stack[f->depth - 1].type != TY_INT)
            return type_error(c, "neg takes an int");
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
static bool call_args(comp *c, const sig *s, uint32_t base[2]) {
    fn *f = c->f;
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
static block *open_block(comp *c, uint8_t kind);
static void snapshot(const fn *f, shot *out);
static bool name_is_free(comp *c, name n);

// The word after a call that can fail (RFC-0008 section 7.2). The call has been emitted; the
// next instruction is the one that runs on failure and is skipped on success. The current
// token is the guard.
static bool guard(comp *c, const sig *s, const uint32_t base[2], const uint32_t in[2], bool host) {
    fn *f = c->f;
    if (is_word(c, PNX_KW_or_fail)) {
        if (f->is_top || !f->sig->fails)
            return type_error(c, "or_fail passes the failure on, so this word needs `or fail` in its signature");
        return nothing_owned(c) && emit(c, PNX_ABC(PNX_RETF, 0, 0, 0)) != NONE &&
               push_results(c, s, base, in, host) && advance(c);
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
        if (emit(c, PNX_ABX(PNX_GETFAIL, l->reg, 0)) == NONE || !advance(c))
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
    const uint32_t g = find_local(f, n) ? NONE : find_global(c, n);
    if (g != NONE && ((const globalsym *)c->globals.ptr + g)->kind != GK_SCALAR) { // value index to names
        const globalsym *sym = (const globalsym *)c->globals.ptr + g;
        uint32_t rv, ri;
        if (!need(c, 2))
            return false;
        if (sym->record != NONE)
            return type_error(c, "a table of records is written field by field: value row to record.field");
        if (f->stack[top].type != TY_INT || f->stack[top - 1].type != sym->type)
            return type_error(c, "a cell is stored as: value index to names, with the value of the cell's type");
        if (g > 255)
            return syntax(c, "only the first 256 globals can be arrays or lists for now");
        if (!reg_of(c, top - 1, &rv) || !reg_of(c, top, &ri))
            return false;
        if (sym->strcap) { // the bytes are copied into the cell
            const uint32_t row = top_slot(c, BANK_I, 1);
            if (emit(c, PNX_ABC(PNX_ROW, row, ri, g)) == NONE || emit(c, PNX_ABC(PNX_FSETS, rv, row, 0)) == NONE ||
                emit(c, PNX_FIELD(g, 0, sym->strcap)) == NONE)
                return false;
        } else if (emit(c, PNX_ABC(PNX_ASET, rv, ri, g)) == NONE) {
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
            return type_error(c, "outside a definition `to` stores into a declared global, not a new local");
        if (!name_is_free(c, n))
            return false;
        if (f->n_locals == MAX_LOCALS || f->nloc[bank] >= SLOT0 - 1)
            return syntax(c, "too many local variables in this word");
        l = &f->locals[f->n_locals++];
        *l = (local){.name = n, .type = e.type, .reg = f->nloc[bank]++};
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
        // values on the stack that were read from this local must keep the old value
        for (uint32_t k = 0; k < top; ++k) {
            if (f->stack[k].kind == E_LOCAL && bank_of(f->stack[k].type) == bank && f->stack[k].reg == l->reg &&
                !settle(c, k))
                return false;
        }
    }
    return write_reg(c, l->reg);
}

// record.field : reads the field of the row on top of the stack, or (after `to`) stores the
// value below the row into it. The current token is the path.
static bool field_access(comp *c, bool write) {
    fn *f = c->f;
    const name path = tok_name(c);
    uint32_t dot = 0;
    while (path.text[dot] != '.')
        ++dot;
    const uint32_t r = find_record(c, (name){path.text, dot});
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
    if (c->tok.kind == PNX_T_PATH)
        return field_access(c, true) && advance(c);
    return syntax(c, "to is followed by a name or a list of names");
}

// ---- control ------------------------------------------------------------------------------

static void snapshot(const fn *f, shot *out) {
    out->hidden = out->owned = 0;
    for (uint32_t k = 0; k < f->depth; ++k) {
        out->types[k] = f->stack[k].type;
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
                              .owned = (from->owned >> k & 1u) != 0};
    f->depth = from->depth;
}

static bool same_stack(const fn *f, const shot *as) {
    if (f->depth != as->depth)
        return false;
    for (uint32_t k = 0; k < as->depth; ++k) {
        if (f->stack[k].type != as->types[k] || f->stack[k].owned != ((as->owned >> k & 1u) != 0))
            return false;
    }
    return true;
}

// The state of the locals (what they own) is part of what must agree where paths meet.
static void save_owns(const fn *f, uint8_t *out) {
    for (uint32_t k = 0; k < f->n_locals; ++k)
        out[k] = f->locals[k].state;
}

static void restore_owns(fn *f, const uint8_t *from, uint32_t count) {
    for (uint32_t k = 0; k < count && k < f->n_locals; ++k)
        f->locals[k].state = from[k];
}

// The locals that existed when block `b` began hold what `as` says, and those made inside it
// hold nothing owned any more.
static bool same_owns(comp *c, const block *b, const uint8_t *as) {
    const fn *f = c->f;
    for (uint32_t k = as ? 0 : b->locals_mark; k < f->n_locals; ++k) {
        if (k < b->locals_mark ? f->locals[k].state != as[k] : f->locals[k].state == L_OWNS)
            return type_error(c, k < b->locals_mark
                                     ? "an owned value is given on one path and kept on another; every path must do the same"
                                     : "an owned value made in this block is not given before the block ends");
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
    return b;
}

// Locals made inside a block end with it.
static void close_scope(fn *f, const block *b) {
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
    if (!same_owns(c, target, target->owns_in))
        return false;
    target->targeted = true;
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
    if (!f->dead) {
        if (!settle_all(c))
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
static bool scan_user(comp *c, name *out) {
    pnx_lexer lx = *c->lx;
    pnx_token t;
    for (uint32_t depth = 1; depth;) {
        if (!pnx_lex_next(&lx, &t))
            return error_at(c, PINAX_ERR_SYNTAX, lx.error_line, lx.error_column, lx.error);
        if (t.kind == PNX_T_END)
            return syntax(c, "this block is not closed by end");
        if (t.kind != PNX_T_RESERVED)
            continue;
        switch (t.word) {
        case PNX_KW_do: case PNX_KW_if: case PNX_KW_loop: case PNX_KW_case:
        case PNX_KW_of: case PNX_KW_otherwise: case PNX_KW_or_else: ++depth; break;
        case PNX_KW_end: --depth; break;
        default: break;
        }
    }
    if (!pnx_lex_next(&lx, &t))
        return error_at(c, PINAX_ERR_SYNTAX, lx.error_line, lx.error_column, lx.error);
    if (t.kind != PNX_T_NAME)
        return syntax(c, "a block is followed by the word that uses it (each, fold, times)");
    *out = (name){lx.source + t.start, t.length};
    return true;
}

// Turns the stem on top of the stack into the registers of a loop: `cur` runs up to `lim`.
// The operands stay on the stack, marked as belonging to the block. Returns their count.
static bool guard_op(comp *c, tid out);
static bool found_or_fail(comp *c, tid type, uint32_t value, uint32_t flag);

static bool stem_begin(comp *c, uint8_t *kind, uint8_t *cur, uint8_t *lim, uint8_t *src, uint8_t *count,
                       tid *element) {
    fn *f = c->f;
    const entry stem = f->stack[--f->depth];
    *kind = stem.op;
    *src = 0;
    *element = TY_INT;
    if (stem.type == TY_COLL) { // an array or list: an index and the length
        const globalsym *g = (const globalsym *)c->globals.ptr + stem.value;
        if (g->kind != GK_ARRAY && g->kind != GK_LIST)
            return type_error(c, "a buffer or an index is not a stem; each goes over an array or a list");
        if (stem.value > 255)
            return syntax(c, "only the first 256 globals can be arrays or lists for now");
        if (!push_slot(c, TY_INT) || !push_slot(c, TY_INT))
            return false;
        f->stack[f->depth - 2].hidden = f->stack[f->depth - 1].hidden = true;
        *kind = g->record != NONE ? STEM_ROWS : g->strcap ? STEM_STRS : STEM_COLL;
        *src = (uint8_t)stem.value;
        *cur = (uint8_t)slot_of(c, f->depth - 2);
        *lim = (uint8_t)slot_of(c, f->depth - 1);
        *count = 2;
        *element = g->type;
        return emit(c, PNX_ABX(PNX_LOADI, *cur, 0)) != NONE && emit(c, PNX_ABX(PNX_GLEN, *lim, (uint32_t)stem.value)) != NONE;
    }
    if (stem.op == STEM_WORDS || stem.op == STEM_LINES || stem.op == STEM_SPLIT_BYTE) {
        // the string itself is the rest still to read; for a byte separator, `cur` is the byte
        f->stack[f->depth - 1].hidden = true;
        *src = (uint8_t)slot_of(c, f->depth - 1);
        *cur = (uint8_t)stem.value;
        *lim = 0;
        *count = 1;
        *element = TY_STR;
        return true;
    }
    if (stem.op == STEM_SPLIT) { // the string and, above it, the separator
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
    *src = (uint8_t)slot_of(c, f->depth - 1);
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
    return kind == STEM_WORDS || kind == STEM_LINES || kind == STEM_SPLIT || kind == STEM_SPLIT_BYTE;
}

// Takes the next piece into `reg`; the instruction after it runs when there is none.
static bool stem_take(comp *c, uint8_t kind, uint32_t reg, uint32_t src, uint32_t cur, uint32_t lim) {
    return emit(c, kind == STEM_WORDS   ? PNX_ABC(PNX_SWORD, src, reg, 0)
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
    return emit(c, kind == STEM_RANGE  ? PNX_ABC(PNX_MOVI, reg, cur, 0)
                   : kind == STEM_COLL ? PNX_ABC(PNX_AGET, reg, cur, src)
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
    *l = (local){.name = n, .type = type, .reg = f->nloc[bank]++};
    if (f->nloc[bank] > f->maxloc[bank])
        f->maxloc[bank] = f->nloc[bank];
    return l;
}

// stem do [ ( params ) ] body end each | fold      count do body end times
static bool do_block(comp *c) {
    fn *f = c->f;
    name user;
    if (!scan_user(c, &user))
        return false;
    const pnx_token at = c->tok;
    sig bs = {0};
    if (!advance(c))
        return false;
    if (c->tok.kind == PNX_T_LPAREN && !parse_sig(c, &bs))
        return false;
    if (bs.fails)
        return error_at(c, PINAX_ERR_SYNTAX, at.line, at.column, "a block that can fail is not supported yet");
    const bool has_stem = f->depth > f->floor && bank_of(f->stack[f->depth - 1].type) == BANK_NONE;
    uint8_t use, kind = STEM_TIMES, cur = 0, lim = 0, src = 0, hidden = 0;
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
               name_is(user, "all") || name_is(user, "first") || name_is(user, "each_index")) {
        use = name_is(user, "each") ? USE_EACH : name_is(user, "fold") ? USE_FOLD : name_is(user, "count") ? USE_COUNT
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
    uint32_t init = NONE;
    if (use == USE_EACH) {
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
    // what the word keeps while the block runs, above the operands of the stem
    uint8_t keep = 0, val = 0, idx = cur;
    if (use == USE_COUNT || use == USE_ANY || use == USE_ALL || use == USE_FIRST ||
        (use == USE_EACH_INDEX && (kind == STEM_RANGE || stem_takes(kind)))) {
        if (!push_slot(c, use == USE_COUNT || use == USE_EACH_INDEX ? TY_INT : TY_BOOL))
            return false;
        f->stack[f->depth - 1].hidden = true;
        keep = (uint8_t)slot_of(c, f->depth - 1);
        if (emit(c, PNX_ABX(PNX_LOADI, keep, use == USE_ALL)) == NONE)
            return false;
        ++hidden;
        if (use == USE_EACH_INDEX)
            idx = keep;
    }
    if (use == USE_FIRST) {
        if (!push_slot(c, element))
            return false;
        f->stack[f->depth - 1].hidden = true;
        val = (uint8_t)slot_of(c, f->depth - 1);
        ++hidden;
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
    b->floor_mark = f->floor;
    f->floor = f->depth;
    if (use == USE_FOLD) {
        const local *acc = new_local(c, bs.in_name[0], bs.in[0]);
        if (!acc || !move_slot(c, acc->type, acc->reg, slot_of(c, init)))
            return false;
        b->acc = acc->reg;
        b->acc_type = acc->type;
    }
    b->head = f->code.n;
    f->def_at = NONE;
    // where the element goes: a named parameter, or the stack
    uint32_t reg = 0;
    if (use != USE_TIMES) {
        const uint32_t param = use == USE_FOLD ? 1 : 0;
        if (bs.n_in > param && bs.in_name[param].length) {
            const local *l = new_local(c, bs.in_name[param], element);
            if (!l)
                return false;
            reg = l->reg;
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
    if (use == USE_EACH_INDEX) {
        const local *l = new_local(c, bs.in_name[1], TY_INT);
        if (!l || emit(c, PNX_ABC(PNX_MOVI, l->reg, idx, 0)) == NONE)
            return false;
    }
    if (use == USE_FIRST && !move_slot(c, element, val, reg))
        return false; // kept, in case this is the one
    f->def_at = NONE;
    return true;
}

static bool end_do(comp *c, block *b) {
    fn *f = c->f;
    if (!f->dead) {
        if (b->use == USE_FOLD) {
            if (f->depth != (uint32_t)b->in.depth + 1 || f->stack[f->depth - 1].type != b->acc_type)
                return type_error(c, "the block of fold leaves the new value of its first parameter");
            if (!write_reg(c, b->acc))
                return false;
        }
        entry verdict = {0};
        const bool tests = b->use == USE_COUNT || b->use == USE_ANY || b->use == USE_ALL || b->use == USE_FIRST;
        if (tests) {
            if (f->depth != (uint32_t)b->in.depth + 1)
                return type_error(c, "this block leaves one bool: its verdict on the element");
            if (!pop_cond(c, &verdict))
                return false;
        }
        if (!settle_all(c))
            return false;
        if (!same_stack(f, &b->in))
            return type_error(c, "this block must leave the stack as it found it");
        if (!same_owns(c, b, b->owns_in))
            return false;
        if (b->use == USE_COUNT) { // one more when the verdict is true
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
    }
    land(c, &b->exits);
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
    uint8_t kind, cur, lim, src, hidden;
    tid     element_type;
    if (!stem_begin(c, &kind, &cur, &lim, &src, &hidden, &element_type))
        return false;
    if (element_type != TY_INT || stem_takes(kind))
        return type_error(c, "this word takes a stem of int values");
    if (!push_slot(c, TY_INT) || !push_slot(c, TY_INT))
        return false;
    const uint32_t best = slot_of(c, f->depth - 2), flag = slot_of(c, f->depth - 1);
    uint32_t       element = top_slot(c, BANK_I, 1), exits = NONE;
    if (emit(c, PNX_ABX(PNX_LOADI, flag, 0)) == NONE || !settle_all(c))
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

// stem sum
static bool do_sum(comp *c) {
    fn *f = c->f;
    if (f->depth == f->floor || bank_of(f->stack[f->depth - 1].type) != BANK_NONE)
        return type_error(c, "sum takes a stem (for example lo hi range sum)");
    uint8_t kind, cur, lim, src, hidden;
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
        if (!need(c, 1))
            return false;
        if (f->stack[f->depth - 1].type != TY_STR)
            return type_error(c, "words takes a str");
        return settle(c, f->depth - 1) && push(c, (entry){.type = TY_STEM, .kind = E_SLOT, .op = STEM_WORDS}) &&
               advance(c);
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
        if (on_coll) {
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
        if (!need(c, 1))
            return false;
        if (f->stack[f->depth - 1].type != TY_STR)
            return type_error(c, "this word takes a str, or the name of an array or list");
        if (!c->use_strings)
            return error(c, PINAX_ERR_NOT_FOUND, "len and is_empty of a str are words of the module strings: use strings");
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
            if (emit(c, PNX_ABC(PNX_ROW, row, ra, gi)) == NONE || !push_slot(c, TY_STR))
                return false;
            return emit_def(c, PNX_ABC(PNX_FGETS, slot_of(c, f->depth - 1), row, 0), BANK_S) != NONE &&
                   emit(c, PNX_FIELD(gi, 0, g->strcap)) != NONE && advance(c);
        }
        const uint32_t dst = top_slot(c, BANK_I, 1);
        return emit_def(c, PNX_ABC(g->record != NONE ? PNX_ROW : PNX_AGET, dst, ra, gi), BANK_I) != NONE &&
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
        if (f->stack[f->depth - 1].type != TY_INT || f->stack[f->depth - 2].type != TY_INT)
            return type_error(c, "this word takes two int values");
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
        const uint32_t op = w->id == BW_APPEND ? PNX_ABX(PNX_BCAT, ra, gi)
                          : w->id == BW_APPEND_INT ? PNX_ABX(PNX_BCATI, ra, gi) : PNX_ABC(PNX_BPUT, ra, PNX_W_U8, gi);
        return emit(c, op) != NONE && guard_op(c, TY_NONE);
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
        return emit_def(c, PNX_ABC(PNX_PALIVE, dst, ra, gi), BANK_I) != NONE && push_slot(c, TY_BOOL) && advance(c);
    }
    case BW_PUT: { // v jobs put : fails when the queue is full
        if (!need_coll(c, &g, &gi) || !need(c, 1))
            return false;
        if (g->kind != GK_QUEUE)
            return type_error(c, "put adds to a queue");
        if (f->stack[f->depth - 1].type != g->type)
            return type_error(c, "the value has another type than the queue holds");
        if (!settle(c, f->depth - 1))
            return false;
        ra = slot_of(c, f->depth - 1);
        --f->depth;
        return settle_all(c) && emit(c, PNX_ABX(PNX_QPUT, ra, gi)) != NONE && guard_op(c, TY_NONE);
    }
    case BW_PEEK:
    case BW_TAKE:
        if (on_coll) { // jobs take : the oldest value; fails when the queue is empty
            if (!need_coll(c, &g, &gi))
                return false;
            if (g->kind != GK_QUEUE)
                return type_error(c, "take and peek read the front of a queue");
            if (!settle_all(c))
                return false;
            return emit(c, PNX_ABX(w->id == BW_TAKE ? PNX_QTAKE : PNX_QPEEK, top_slot(c, BANK_I, 1), gi)) != NONE &&
                   guard_op(c, g->type);
        }
        if (w->id == BW_PEEK || !c->use_strings)
            return type_error(c, "this word takes the name of a queue (take of a str needs use strings)");
        [[fallthrough]];
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
        return emit_def(c, PNX_ABC(PNX_SREPL, base, base, 0), BANK_S) != NONE && push_slot(c, TY_STR) && advance(c);
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
        return emit(c, PNX_ABC(key->is_str ? PNX_IXGETS : PNX_IXGET, top_slot(c, BANK_I, 1), ra, gi)) != NONE &&
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
        if (g->kind != GK_LIST || g->record != NONE || g->strcap)
            return type_error(c, g->record != NONE ? "a list of records grows with add_row"
                                 : g->strcap       ? "push and pop of a list of str(N) are not supported yet"
                                                   : "push adds to a list; an array is always full");
        if (f->stack[f->depth - 1].type != g->type)
            return type_error(c, "the value pushed has another type than the cells of the list");
        if (!settle(c, f->depth - 1))
            return false;
        ra = slot_of(c, f->depth - 1);
        --f->depth;
        return settle_all(c) && emit(c, PNX_ABX(PNX_LPUSH, ra, gi)) != NONE && guard_op(c, TY_NONE);
    }
    case BW_POP: { // names pop : fails when the list is empty
        if (!need_coll(c, &g, &gi))
            return false;
        if (g->kind != GK_LIST || g->record != NONE || g->strcap)
            return type_error(c, "pop takes from a list of int, bool or err values");
        if (!settle_all(c))
            return false;
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
    {"interrupted", PINAX_ERR_INTERRUPTED}, {"argument", PINAX_ERR_ARGUMENT},
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
        if (!module_used(c, module))
            return field_access(c, false) && advance(c);
    } else {
        const local *l = find_local(f, n);
        if (l) {
            if (l->state == L_MOVED)
                return type_error(c, "this local gave its owned value away; it holds nothing to read");
            return push(c, (entry){.type = l->type, .kind = E_LOCAL, .reg = l->reg, .src = (uint8_t)(l - f->locals + 1)}) &&
                   advance(c);
        }
        const constant *k = find_const(c, n);
        if (k) {
            if (k->type == TY_STR)
                return push_string(c, (uint32_t)k->value) && advance(c);
            return push(c, (entry){.type = k->type, .kind = E_CONST, .value = k->value}) && advance(c);
        }
        const uint32_t g = find_global(c, n);
        if (g != NONE) {
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
        if (c->data_only)
            return syntax(c, "a data source calls no words; only literals, const names and stores");
        uint32_t index;
        if (find_word(c, n, &index))
            return call_word(c, &((const wordsym *)c->words.ptr + index)->sig, index);
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
        const struct builtin *bw = find_builtin(c, n);
        if (bw)
            return builtin(c, bw);
    }
    if (c->data_only)
        return syntax(c, "a data source calls no words; only literals, const names and stores");
    bool ambiguous;
    const uint32_t host = find_host(c, module, n, &ambiguous);
    if (ambiguous)
        return type_error(c, "two modules in use have a word of this name; write module.word");
    if (host == NONE)
        return error(c, PINAX_ERR_NOT_FOUND, "this name is not declared before this place");
    return call_host(c, host);
}

static bool body_token(comp *c) {
    fn *f = c->f;
    const pnx_token t = c->tok;
    if (f->n_blocks && f->blocks[f->n_blocks - 1].kind == B_CASE)
        return case_item(c);
    if (c->data_only && t.kind == PNX_T_RESERVED && t.word != PNX_KW_true && t.word != PNX_KW_false &&
        t.word != PNX_KW_to)
        return syntax(c, "a data source holds declarations, literals, stores and rows; this word is not allowed");
    const bool closer = t.kind == PNX_T_RESERVED && (t.word == PNX_KW_end || t.word == PNX_KW_else);
    if (f->dead && !closer)
        return syntax(c, "this code cannot be reached: the code before it always leaves");
    // a stem is taken at once by its block or by a word like sum
    if (f->depth > f->floor && f->stack[f->depth - 1].type == TY_STEM &&
        !(t.kind == PNX_T_RESERVED && t.word == PNX_KW_do) &&
        !(t.kind == PNX_T_NAME && (name_is(tok_name(c), "sum") || name_is(tok_name(c), "minimum") ||
                                   name_is(tok_name(c), "maximum"))))
        return type_error(c, "a stem is not a value: a block or a combining word takes it at once");
    // a pending condition survives only into the word that consumes it
    if (f->depth && f->stack[f->depth - 1].kind == E_COND) {
        const bool consumer = (t.kind == PNX_T_RESERVED && (t.word == PNX_KW_if || t.word == PNX_KW_while ||
                                                            t.word == PNX_KW_until)) ||
                              (t.kind == PNX_T_NAME && name_is(tok_name(c), "not"));
        if (!consumer && !settle(c, f->depth - 1))
            return false;
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
    switch (t.word) {
    case PNX_KW_true:
    case PNX_KW_false:
        return push(c, (entry){.type = TY_BOOL, .kind = E_CONST, .value = t.word == PNX_KW_true}) && advance(c);
    case PNX_KW_or: {
        const core_word *w = find_core(cname("or"));
        return core(c, w);
    }
    case PNX_KW_to: return store(c);
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
        if (op == PNX_CALL || op == PNX_HCALL || op == PNX_HCALLF) {
            if (!remap(c, PNX_K_I, &a, f->maxloc) || !remap(c, PNX_K_S, &b, f->maxloc))
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
    const vec code = f->code;
    __builtin_memset(f, 0, sizeof *f);
    f->code = code;
    f->code.n = 0;
    f->def_at = NONE;
    f->tail_at = NONE;
    f->is_top = is_top;
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
    } else if (name_is_global(c, n) && !(find_fwd(c, n, NULL) && find_fwd(c, n, NULL)->index == NONE)) {
        return type_error(c, "this name is already defined");
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
    if (!is_test && !parse_sig(c, &w->sig))
        return false;
    char text[72];
    if (!sig_text(c, &w->sig, text, sizeof text))
        return syntax(c, "the signature is longer than 71 bytes");
    fwd *ahead = is_test ? NULL : find_fwd(c, n, NULL);
    if (ahead) { // the definition a forward declaration promised
        char promised[72];
        if (!sig_text(c, &ahead->sig, promised, sizeof promised) || __builtin_strcmp(promised, text) != 0)
            return type_error(c, "the definition has another signature than its forward declaration");
        ahead->index = c->words.n - 1;
    }

    fn *f = c->word_fn;
    fn_reset(f, false);
    f->sig = &w->sig;
    c->f = f;
    // inputs are the first registers of the window: named ones are locals, bare ones start on
    // the stack and are read from those registers
    for (uint32_t k = 0; k < w->sig.n_in; ++k) {
        const int bank = bank_of(w->sig.in[k]);
        local *l = &f->locals[f->n_locals++];
        *l = (local){.name = w->sig.in_name[k], .type = w->sig.in[k], .reg = f->nloc[bank]++};
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
        } else if (!push(c, (entry){.type = l->type, .kind = E_LOCAL, .reg = l->reg})) {
            return false;
        }
    }
    f->maxloc[0] = f->nloc[0];
    f->maxloc[1] = f->nloc[1];

    while (!(is_word(c, PNX_KW_end) && f->n_blocks == 0)) {
        if (c->tok.kind == PNX_T_END)
            return syntax(c, "the definition is not closed by end");
        if (!body_token(c))
            return false;
    }
    if (!f->dead && !emit_return(c))
        return false;
    w = (wordsym *)c->words.ptr + (c->words.n - 1);
    uint32_t in[2], out[2];
    count_sig(&w->sig, in, out);
    if (!finish_registers(c, f, w->cells))
        return false;
    for (int bank = 0; bank < 2; ++bank) { // the loader wants room for arguments and results
        const uint32_t least = in[bank] > out[bank] ? in[bank] : out[bank];
        if (w->cells[bank] < least)
            w->cells[bank] = (uint16_t)least;
    }
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
    if (name_is(n, "strings") || name_is(n, "format")) { // standard modules: part of the language
        if (name_is(n, "strings"))
            c->use_strings = true;
        else
            c->use_format = true;
        return advance(c);
    }
    bool registered = false;
    for (uint32_t k = 0; k < c->st->n_hosts; ++k)
        registered = registered || name_eq(cname(c->st->hosts[k].module), n);
    if (!registered)
        return error(c, PINAX_ERR_NOT_FOUND, "the host has not registered this module");
    if (!module_used(c, n)) {
        if (c->n_modules == MAX_MODULES)
            return syntax(c, "too many modules in use");
        c->modules[c->n_modules++] = n;
    }
    return advance(c);
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
    } else if (g.type == TY_NONE || bank_of(g.type) != BANK_I) {
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
    g.record = find_record(c, tok_name(c));
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
        fwd ahead = {.name = tok_name(c), .index = NONE};
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
        if (is_word(c, PNX_KW_big_endian) || is_word(c, PNX_KW_columns))
            return syntax(c, "big_endian and columns are not supported by the compiler yet");
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
    if (gi > 255)
        return syntax(c, "only the first 256 globals can be arrays or lists for now");
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
    if (gi > 255)
        return syntax(c, "only the first 256 globals can be arrays or lists for now");
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
        } else if (!load_int(c, value, v) || !load_int(c, index, k) || emit(c, PNX_ABC(PNX_ASET, value, index, gi)) == NONE) {
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
    vec code; // uint32_t, all words
} image;

static bool append_code(comp *c, image *im, const fn *f) {
    for (uint32_t k = 0; k < f->code.n; ++k) {
        uint32_t *slot = vec_push(c, &im->code, sizeof(uint32_t));
        if (!slot)
            return false;
        *slot = ((const uint32_t *)f->code.ptr)[k];
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

static void put_text(char *out, size_t cap, name n) {
    const size_t len = n.length < cap - 1 ? n.length : cap - 1;
    __builtin_memcpy(out, n.text, len);
    out[len] = '\0';
}

#define LOAD_WORD "(load)"

static bool compile(comp *c, image *im, bool *has_top) {
    if (!advance(c))
        return false;
    if (is_word(c, PNX_KW_pinax)) { // pinax 1 : the source format version (RFC-0012)
        if (!advance(c))
            return false;
        if (c->tok.kind != PNX_T_INT)
            return syntax(c, "pinax is followed by the source format version");
        if (c->tok.integer != 1)
            return error(c, PINAX_ERR_FORMAT, "this source is written for another format version; this build reads version 1");
        if (!advance(c))
            return false;
    }
    while (c->tok.kind != PNX_T_END) {
        if (is_word(c, PNX_KW_use)) {
            if (!use_module(c))
                return false;
        } else if (is_word(c, PNX_KW_export)) {
            if (!advance(c)) // every word is visible to the host for now; modules come later
                return false;
            if (!is_word(c, PNX_KW_word))
                return syntax(c, "export is followed by a definition or a declaration");
        } else if (c->data_only && (is_word(c, PNX_KW_word) || is_word(c, PNX_KW_test) || is_word(c, PNX_KW_use))) {
            return syntax(c, "a data source defines no words and uses no modules");
        } else if (is_word(c, PNX_KW_word) || is_word(c, PNX_KW_test)) {
            wordsym *last;
            if (!definition(c, is_word(c, PNX_KW_test)))
                return false;
            last = (wordsym *)c->words.ptr + (c->words.n - 1);
            last->entry = im->code.n;
            if (!append_code(c, im, c->word_fn))
                return false;
        } else if (is_word(c, PNX_KW_bound) && !c->top_fn->n_blocks) {
            // bound global | bound array : the host gives the storage (pinax_bind); until it does,
            // the declaration has storage of its own, so nothing is ever unbacked
            if (!advance(c))
                return false;
            const bool scalar = is_word(c, PNX_KW_global);
            if (!scalar && !is_word(c, PNX_KW_array))
                return syntax(c, is_word(c, PNX_KW_buffer) ? "a bound buffer is not supported yet"
                                                           : "bound stands before global, array or buffer");
            if (scalar ? !declare_global(c) : !declare_collection(c, GK_ARRAY))
                return false;
            globalsym *g = (globalsym *)c->globals.ptr + (c->globals.n - 1);
            if (g->record != NONE || g->strcap || bank_of(g->type) != BANK_I)
                return type_error(c, "bound storage holds int, bool or err values for now");
            g->bound = true;
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
        } else if (!body_token(c)) {
            return false;
        }
    }
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
        if (top->depth && !top->dead)
            return type_error(c, "values are left on the stack at the end of the source");
        if (!top->dead && emit(c, PNX_ABC(PNX_RET, 0, 0, 0)) == NONE)
            return false;
        wordsym *w = vec_push(c, &c->words, sizeof(wordsym));
        if (!w)
            return false;
        __builtin_memset(w, 0, sizeof *w);
        w->name = cname(LOAD_WORD);
        w->entry = im->code.n;
        if (!finish_registers(c, top, w->cells) || !append_code(c, im, top))
            return false;
    }
    return true;
}

// Writes the container into the arena and reports where it is.
static bool serialize(comp *c, const image *im, uint8_t **out, size_t *out_len) {
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
    };
    const size_t len = sizeof head + (size_t)head.n_words * sizeof(pnx_wordrec) +
                       (size_t)head.n_globals * sizeof(pnx_globalrec) +
                       (size_t)head.n_imports * sizeof(pnx_importrec) + (size_t)head.n_iconst * 8 +
                       (size_t)head.n_sconst * sizeof(pnx_sconst) + (size_t)head.n_code * 4 + head.n_strbytes;
    uint8_t *p = arena_alloc(c, len);
    if (!p)
        return false;
    *out = p;
    *out_len = len;
    __builtin_memcpy(p, &head, sizeof head);
    p += sizeof head;
    for (uint32_t k = 0; k < head.n_words; ++k) {
        const wordsym *w = (const wordsym *)c->words.ptr + k;
        pnx_wordrec rec = {.entry = w->entry, .int_cells = w->cells[0], .str_cells = w->cells[1], .counted = 1,
                           .fails = w->sig.fails};
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
    return true;
}

pinax_err pinax_load(pinax_state *st, const char *file, pinax_str source, uint32_t mode) {
    if (!st || (!source.ptr && source.len))
        return PINAX_ERR_ARGUMENT;
    if (st->head || st->suspended) {
        st->error_text = "one source or container per state for now; load before running";
        return PINAX_ERR_STATE;
    }
    if (mode != PINAX_LOAD_CODE && mode != PINAX_LOAD_DATA)
        return PINAX_ERR_ARGUMENT;
    if (source.len > UINT32_MAX) {
        st->error_text = "the source is longer than 4 GiB";
        return PINAX_ERR_ARGUMENT;
    }

    // the unused tail of the block is the compiler's working memory
    uint8_t *const tail = st->block + ((st->used + 15u) & ~(size_t)15u);
    uint8_t *const end = st->block + st->block_len;
    comp      c = {.st = st, .file = file ? file : "", .data_only = mode == PINAX_LOAD_DATA};
    c.arena = tail < end ? tail : end;
    c.arena_end = end;
    c.lx = &c.lexer;
    pnx_lex_init(&c.lexer, source.ptr, source.len);

    image    im = {0};
    bool     has_top = false;
    uint8_t *container = NULL;
    size_t   length = 0;
    c.word_fn = arena_alloc(&c, sizeof(fn));
    c.top_fn = c.word_fn ? arena_alloc(&c, sizeof(fn)) : NULL;
    if (c.top_fn) {
        fn_reset(c.word_fn, false);
        fn_reset(c.top_fn, true);
        static const sig no_results = {0};
        c.top_fn->sig = &no_results;
        c.f = c.top_fn;
        if (compile(&c, &im, &has_top))
            (void)serialize(&c, &im, &container, &length);
    }
    if (c.err != PINAX_OK) {
        st->error_text = c.message;
        if (st->diag) {
            const pinax_str name = {(const unsigned char *)c.file, __builtin_strlen(c.file)};
            const pinax_str text = {(const unsigned char *)c.message, __builtin_strlen(c.message)};
            st->diag(st->diag_user, name, c.line, c.column, text);
        }
        return c.err;
    }

    // The container moves to the very end of the block, out of the way of the loader, which
    // takes its room from the front; the block is shortened for that moment.
    uint8_t *const parked = end - length;
    __builtin_memmove(parked, container, length);
    const size_t full = st->block_len, mark = st->used;
    st->block_len = (size_t)(parked - st->block);
    const pinax_str bytes = {parked, length};
    pinax_err err = pinax_load_bytecode(st, bytes);
    st->block_len = full;
    if (err == PINAX_ERR_NO_ROOM)
        st->needed += length;
    if (err != PINAX_OK)
        return err;
    if (has_top) { // code outside definitions runs once, now
        const uint64_t budget = st->budget;
        if (mode == PINAX_LOAD_DATA) // its stores run on a small budget of their own, not the host's
            st->budget = 1024 + 16 * (uint64_t)source.len;
        err = pinax_run(st, st->head->n_words - 1, NULL);
        st->budget = budget;
        if (mode == PINAX_LOAD_DATA && err == PINAX_SUSPENDED) {
            st->error_text = "the data source did not finish inside its budget";
            err = PINAX_ERR_STATE;
        }
        if (err != PINAX_OK && err != PINAX_SUSPENDED) {
            // a fault while loading: nothing stays loaded, as after any other load error
            const char *text = st->error_text;
            pnx_unload(st, mark);
            st->error_text = text;
        }
    }
    return err;
}
