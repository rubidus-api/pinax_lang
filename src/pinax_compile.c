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

enum { TY_NONE, TY_INT, TY_BOOL, TY_STR, TY_ERR, TY_STEM };
enum { BANK_I, BANK_S, BANK_NONE };
enum { E_SLOT, E_LOCAL, E_CONST, E_COND };
enum { B_IF, B_LOOP, B_ORELSE, B_CASE, B_OF, B_DO };
enum { STEM_RANGE, STEM_BYTES, STEM_TIMES };
enum { USE_EACH, USE_FOLD, USE_TIMES };
#define HIDDEN 0x80u // in a stack snapshot: the entry belongs to a running block, not to the code

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
    uint8_t type, kind;
    uint8_t reg;     // E_LOCAL: the local's register
    uint8_t op, a, b; // E_COND: branch instruction and its two fields (b: register or immediate)
                      // a stem: op is its kind
    bool    hidden;  // an operand of a running block: code cannot reach it
    int64_t value;   // E_CONST
} entry;

typedef struct sig {
    uint8_t n_in, n_out;
    bool    fails, named;
    uint8_t in[MAX_PARAMS], out[MAX_PARAMS];
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
    uint8_t type, reg;
} local;

typedef struct block {
    uint8_t  kind;
    bool     has_else, has_out, targeted;
    name     name;       // loop name
    uint32_t head;       // loop: first instruction
    uint32_t exits;      // chain of jumps to the end
    uint32_t else_jump;  // if: the jump taken when the condition is false
    uint32_t locals_mark;
    uint8_t  nloc_mark[2];
    uint8_t  depth, out_depth;
    uint8_t  types[MAX_STACK], out_types[MAX_STACK];
    // case
    uint32_t next_test;   // jump of the last test, taken when no value matched
    uint8_t  subject, subject_type;
    bool     has_otherwise;
    uint32_t n_values;
    int64_t  values[MAX_CASE]; // integers, or string constant numbers
    // do block of a combining word
    uint8_t  use, stem, cur, lim, acc, acc_type, n_hidden;
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
    uint32_t def_at; // instruction that just wrote def_reg, for retargeting a store
    uint8_t  def_reg, def_bank;
    bool     is_top;
    const sig *sig;
} fn;

typedef struct constant {
    name     name;
    uint8_t  type;
    int64_t  value; // integer, or the string constant number
} constant;

typedef struct globalsym {
    name    name;
    uint8_t type;
} globalsym;

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
    bool         use_strings;
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

static int bank_of(uint8_t type) {
    return type == TY_STR ? BANK_S : type == TY_STEM ? BANK_NONE : BANK_I;
}

static const char *type_name(uint8_t type) {
    return type == TY_INT ? "int" : type == TY_BOOL ? "bool" : type == TY_ERR ? "err" : "str";
}

static uint8_t type_of_name(name n) {
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
        if (c->tok.kind != PNX_T_NAME)
            return syntax(c, "a parameter is a type, or a name and a type");
        name first = tok_name(c), param = {0}, type = first;
        if (!advance(c))
            return false;
        if (c->tok.kind == PNX_T_NAME) {
            param = first;
            type = tok_name(c);
            if (!advance(c))
                return false;
        }
        const uint8_t t = type_of_name(type);
        if (t == TY_NONE)
            return type_error(c, name_is(type, "float") ? "float is not available: this build has no F bank"
                                                        : "this type is not known here");
        uint8_t *count = outputs ? &s->n_out : &s->n_in;
        if (*count == MAX_PARAMS)
            return syntax(c, "too many parameters");
        if (outputs) {
            s->out[*count] = t;
            s->out_name[*count] = param;
        } else {
            s->in[*count] = t;
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
            if (c->tok.kind != PNX_T_NAME)
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
static bool sig_text(const sig *s, char *out, size_t cap) {
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
            const char *tn = type_name(side ? s->out[k] : s->in[k]);
            if (k)
                PUT(", ", 2);
            if (pn.length) {
                PUT(pn.text, pn.length);
                PUT(" ", 1);
            }
            PUT(tn, __builtin_strlen(tn));
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

static bool push_slot(comp *c, uint8_t type) {
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
    if (e->kind == E_SLOT || e->type == TY_STEM)
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
    if (c->f->depth - c->f->floor < count)
        return type_error(c, "the stack does not hold enough values for this word");
    for (uint32_t k = 0; k < count; ++k) {
        if (c->f->stack[c->f->depth - 1 - k].type == TY_STEM)
            return type_error(c, "a stem is not a value: a combining word takes it at once");
    }
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

enum { C_STACK, C_ARITH, C_COMPARE, C_BOOL, C_NOT, C_NEG };
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

// Words the compiler expands itself: stems, combining words, the basic string words.
enum { BW_RANGE, BW_SUM, BW_EACH, BW_FOLD, BW_TIMES, BW_LEN, BW_IS_EMPTY, BW_BYTE_AT, BW_BYTES };
static const struct builtin {
    const char *name;
    uint8_t     id;
    bool        strings; // needs `use strings`
} builtins[] = {
    {"range", BW_RANGE, false},     {"sum", BW_SUM, false},        {"each", BW_EACH, false},
    {"fold", BW_FOLD, false},       {"times", BW_TIMES, false},    {"len", BW_LEN, true},
    {"is_empty", BW_IS_EMPTY, true}, {"byte_at", BW_BYTE_AT, true}, {"bytes", BW_BYTES, true},
};

static const struct builtin *find_builtin(const comp *c, name n) {
    for (size_t k = 0; k < sizeof builtins / sizeof builtins[0]; ++k) {
        if (name_is(n, builtins[k].name) && (!builtins[k].strings || c->use_strings))
            return &builtins[k];
    }
    return NULL;
}

// A name every part of the source sees: a word, a constant, a global, a type, a core word.
static bool name_is_global(comp *c, name n) {
    bool ambiguous;
    return find_word(c, n, NULL) || find_core(n) || type_of_name(n) != TY_NONE || name_is(n, "float") ||
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

static bool move_slot(comp *c, uint8_t type, uint32_t to, uint32_t from) {
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
            const uint8_t  t = top->type, u = under->type;
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
    if (a->type != TY_INT || b->type != TY_INT)
        return type_error(c, "this word takes two int values");
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
    return push_slot(c, TY_INT);
}

static bool compare_word(comp *c, int which) {
    fn *f = c->f;
    if (!need(c, 2))
        return false;
    entry *a = &f->stack[f->depth - 2], *b = &f->stack[f->depth - 1];
    if (a->type != b->type)
        return type_error(c, "the two values compared must have the same type");
    if (a->type != TY_INT && which != CMP_EQ && which != CMP_NE)
        return type_error(c, "values of this type are compared with eq and ne only");
    entry e = {.type = TY_BOOL, .kind = E_COND};
    uint32_t ra, rb;
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

static bool core(comp *c, const core_word *w) {
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
    if (!need(c, s->n_in))
        return false;
    const uint32_t first = f->depth - s->n_in;
    for (uint32_t k = 0; k < s->n_in; ++k) {
        if (f->stack[first + k].type != s->in[k])
            return type_error(c, "an argument has another type than the word's signature asks for");
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
        if (host && in[bank] && !move_slot(c, s->out[k], base[bank] + seen[bank], base[bank] + in[bank] + seen[bank]))
            return false;
        ++seen[bank];
    }
    return true;
}

static bool emit_return(comp *c);
static block *open_block(comp *c, uint8_t kind);
static void snapshot(const fn *f, uint8_t *types, uint8_t *depth);
static bool name_is_free(comp *c, name n);

// The word after a call that can fail (RFC-0008 section 7.2). The call has been emitted; the
// next instruction is the one that runs on failure and is skipped on success. The current
// token is the guard.
static bool guard(comp *c, const sig *s, const uint32_t base[2], const uint32_t in[2], bool host) {
    fn *f = c->f;
    if (is_word(c, PNX_KW_or_fail)) {
        if (f->is_top || !f->sig->fails)
            return type_error(c, "or_fail passes the failure on, so this word needs `or fail` in its signature");
        return emit(c, PNX_ABC(PNX_RETF, 0, 0, 0)) != NONE && push_results(c, s, base, in, host) && advance(c);
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
        if (!emit_return(c))
            return false;
        f->depth = after;
        land(c, &resume);
        return advance(c);
    }
    // or_else [ ( e ) ] body end : the body runs on failure and leaves what the word would have
    block *b = open_block(c, B_ORELSE);
    if (!b)
        return false;
    snapshot(f, b->out_types, &b->out_depth);
    b->has_out = true;
    b->depth = (uint8_t)before;
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
static bool call_word(comp *c, uint32_t index) {
    const wordsym *w = (const wordsym *)c->words.ptr + index;
    const sig s = w->sig;
    uint32_t base[2], in[2], out[2];
    if (!call_args(c, &s, base) || (s.fails && !settle_all(c)))
        return false;
    count_sig(&s, in, out);
    (void)top_slot(c, BANK_I, in[0] > out[0] ? in[0] : out[0]);
    (void)top_slot(c, BANK_S, in[1] > out[1] ? in[1] : out[1]);
    if (!check_before_call(c) || emit(c, PNX_ABC(PNX_CALL, base[BANK_I], base[BANK_S], 0)) == NONE ||
        emit(c, index) == NONE || !advance(c))
        return false;
    return s.fails ? guard(c, &s, base, in, false) : push_results(c, &s, base, in, false);
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
    if (!need(c, 1))
        return false;
    const uint32_t top = f->depth - 1;
    entry    e = f->stack[top];
    const int bank = bank_of(e.type);
    const uint32_t g = find_local(f, n) ? NONE : find_global(c, n);
    if (g != NONE) { // a scalar global
        const globalsym *sym = (const globalsym *)c->globals.ptr + g;
        uint32_t reg;
        if (sym->type != e.type)
            return type_error(c, "this global has another type than the value stored into it");
        if (!reg_of(c, top, &reg) || emit(c, PNX_ABX(PNX_SETG, reg, g)) == NONE)
            return false;
        --f->depth;
        return true;
    }
    local *l = find_local(f, n);
    if (!l) {
        if (e.type == TY_STEM)
            return type_error(c, "a stem is not a value: a combining word takes it at once");
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
    } else {
        if (l->type != e.type)
            return type_error(c, "this local variable has another type than the value stored into it");
        // values on the stack that were read from this local must keep the old value
        for (uint32_t k = 0; k < top; ++k) {
            if (f->stack[k].kind == E_LOCAL && bank_of(f->stack[k].type) == bank && f->stack[k].reg == l->reg &&
                !settle(c, k))
                return false;
        }
    }
    return write_reg(c, l->reg);
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
        return syntax(c, "storing into a field is not supported yet");
    return syntax(c, "to is followed by a name or a list of names");
}

// ---- control ------------------------------------------------------------------------------

static void snapshot(const fn *f, uint8_t *types, uint8_t *depth) {
    for (uint32_t k = 0; k < f->depth; ++k)
        types[k] = (uint8_t)(f->stack[k].type | (f->stack[k].hidden ? HIDDEN : 0));
    *depth = (uint8_t)f->depth;
}

static void restore(fn *f, const uint8_t *types, uint8_t depth) {
    for (uint32_t k = 0; k < depth; ++k)
        f->stack[k] = (entry){.type = (uint8_t)(types[k] & ~HIDDEN), .kind = E_SLOT, .hidden = (types[k] & HIDDEN) != 0};
    f->depth = depth;
}

static bool same_stack(const fn *f, const uint8_t *types, uint8_t depth) {
    if (f->depth != depth)
        return false;
    for (uint32_t k = 0; k < depth; ++k) {
        if (f->stack[k].type != (types[k] & ~HIDDEN))
            return false;
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
    snapshot(f, b->types, &b->depth);
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
        snapshot(f, b->out_types, &b->out_depth);
        b->has_out = true;
        if (!jump_later(c, &b->exits))
            return false;
    }
    land(c, &b->else_jump);
    close_scope(f, b);
    restore(f, b->types, b->depth);
    f->dead = false;
    b->has_else = true;
    return advance(c);
}

static bool end_if(comp *c, block *b) {
    fn *f = c->f;
    if (!f->dead) {
        if (!settle_all(c))
            return false;
        if (b->has_out ? !same_stack(f, b->out_types, b->out_depth)
                       : !b->has_else && !same_stack(f, b->types, b->depth))
            return type_error(c, b->has_else ? "the two branches of this if leave different stacks"
                                             : "an if without else must leave the stack as it found it");
    } else if (b->has_out) {
        restore(f, b->out_types, b->out_depth);
        f->dead = false;
    } else if (!b->has_else) {
        restore(f, b->types, b->depth);
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
    if (!same_stack(f, target->types, target->depth))
        return error_at(c, PINAX_ERR_TYPE, at.line, at.column,
                        "where a loop is left the stack must be as it was when the loop began");
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
        if (!same_stack(f, b->types, b->depth))
            return type_error(c, "one turn of a loop must leave the stack as it found it");
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
    restore(f, b->types, b->depth);
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
        ++count[bank_of(s->out[n])];
        at[n++] = k;
    }
    if (n != s->n_out)
        return type_error(c, "the stack does not hold exactly the results of the signature here");
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
        if (!same_stack(f, b->out_types, b->out_depth))
            return type_error(c, "the body of or_else must leave what the word would have given, or leave");
    }
    restore(f, b->out_types, b->out_depth);
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
    const uint8_t type = f->stack[f->depth - 1].type;
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
    uint8_t type = TY_NONE;
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
            restore(f, b->out_types, b->out_depth);
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
            if (!same_stack(f, parent->out_types, parent->out_depth))
                return type_error(c, "the branches of this case leave different stacks");
        } else {
            snapshot(f, parent->out_types, &parent->out_depth);
            parent->has_out = true;
        }
        if (!jump_later(c, &parent->exits))
            return false;
    }
    close_scope(f, b);
    --f->n_blocks;
    restore(f, parent->types, parent->depth);
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
        const uint8_t type = f->stack[before].type;
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
        const uint8_t type = f->stack[k].type;
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
static bool stem_begin(comp *c, uint8_t *kind, uint8_t *cur, uint8_t *lim, uint8_t *src, uint8_t *count) {
    fn *f = c->f;
    const entry stem = f->stack[--f->depth];
    *kind = stem.op;
    *src = 0;
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

// Puts the current element of the stem into `reg`.
static bool stem_element(comp *c, uint8_t kind, uint32_t reg, uint32_t cur, uint32_t src) {
    return emit(c, kind == STEM_RANGE ? PNX_ABC(PNX_MOVI, reg, cur, 0) : PNX_ABC(PNX_SBYTE, reg, src, cur)) != NONE;
}

static bool loop_back(comp *c, uint32_t head, uint32_t cur, int step) {
    fn *f = c->f;
    if (emit(c, PNX_ABC(PNX_ADDI, cur, cur, (uint8_t)(int8_t)step)) == NONE)
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

static local *new_local(comp *c, name n, uint8_t type) {
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
    const bool has_stem = f->depth > f->floor && f->stack[f->depth - 1].type == TY_STEM;
    uint8_t use, kind = STEM_TIMES, cur = 0, lim = 0, src = 0, hidden = 0;
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
    } else if (name_is(user, "each") || name_is(user, "fold")) {
        use = name_is(user, "each") ? USE_EACH : USE_FOLD;
        if (!has_stem)
            return error_at(c, PINAX_ERR_TYPE, at.line, at.column,
                            "this combining word needs a stem before its block (for example lo hi range)");
        if (!stem_begin(c, &kind, &cur, &lim, &src, &hidden))
            return false;
    } else {
        return error_at(c, PINAX_ERR_SYNTAX, at.line, at.column,
                        "this block is used by a word the compiler does not expand yet");
    }
    const uint8_t element = TY_INT; // ranges and bytes give int
    uint32_t      init = NONE;
    if (use == USE_EACH) {
        if (bs.n_out || bs.n_in > 1 || (bs.n_in == 1 && bs.in[0] != element))
            return error_at(c, PINAX_ERR_TYPE, at.line, at.column, "the block of each takes one element and gives nothing");
    } else if (use == USE_FOLD) {
        init = f->depth - hidden - 1;
        if (f->depth - f->floor < (uint32_t)hidden + 1 || f->stack[init].type == TY_STEM)
            return error_at(c, PINAX_ERR_TYPE, at.line, at.column, "fold takes a first value before the stem");
        const uint8_t acc = f->stack[init].type;
        if (bs.n_in != 2 || !bs.named || bs.n_out != 1 || bs.in[0] != acc || bs.out[0] != acc || bs.in[1] != element)
            return error_at(c, PINAX_ERR_TYPE, at.line, at.column,
                            "the block of fold is (acc U, x T gives U): U the first value's type, T the element's");
    }
    if (!settle_all(c))
        return false;
    block *b = open_block(c, B_DO);
    if (!b)
        return false;
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
    // the test: go on while cur < lim (or, for times, while the count is at least 1)
    if (emit(c, use == USE_TIMES ? PNX_ABC(PNX_JGEI, cur, 1, 1) : PNX_ABC(PNX_JLT, cur, lim, 1)) == NONE ||
        !jump_later(c, &b->exits))
        return false;
    if (use != USE_TIMES) {
        const uint32_t param = use == USE_FOLD ? 1 : 0;
        if (bs.n_in > param && bs.in_name[param].length) {
            const local *l = new_local(c, bs.in_name[param], element);
            if (!l || !stem_element(c, kind, l->reg, cur, src))
                return false;
        } else { // no name: the element starts on the stack
            if (!push_slot(c, element) || !stem_element(c, kind, slot_of(c, f->depth - 1), cur, src))
                return false;
        }
    }
    return true;
}

static bool end_do(comp *c, block *b) {
    fn *f = c->f;
    if (!f->dead) {
        if (b->use == USE_FOLD) {
            if (f->depth != (uint32_t)b->depth + 1 || f->stack[f->depth - 1].type != b->acc_type)
                return type_error(c, "the block of fold leaves the new value of its first parameter");
            if (!write_reg(c, b->acc))
                return false;
        }
        if (!settle_all(c))
            return false;
        if (!same_stack(f, b->types, b->depth))
            return type_error(c, "this block must leave the stack as it found it");
        if (!loop_back(c, b->head, b->cur, b->use == USE_TIMES ? -1 : 1))
            return false;
    }
    land(c, &b->exits);
    close_scope(f, b);
    f->floor = b->floor_mark;
    const block done = *b;
    --f->n_blocks;
    restore(f, done.types, done.depth);
    f->dead = false;
    f->depth -= done.n_hidden;
    if (done.use == USE_FOLD) {
        --f->depth; // the first value; the result takes its place
        if (!push_slot(c, done.acc_type) || !move_slot(c, done.acc_type, slot_of(c, f->depth - 1), done.acc))
            return false;
    }
    if (!advance(c)) // past the end
        return false;
    if (c->tok.kind != PNX_T_NAME || !name_eq(tok_name(c), done.user))
        return syntax(c, "the word that uses the block is expected right after its end");
    return true; // the caller moves past the word
}

// stem sum
static bool do_sum(comp *c) {
    fn *f = c->f;
    if (f->depth == f->floor || f->stack[f->depth - 1].type != TY_STEM)
        return type_error(c, "sum takes a stem (for example lo hi range sum)");
    uint8_t kind, cur, lim, src, hidden;
    if (!stem_begin(c, &kind, &cur, &lim, &src, &hidden))
        return false;
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

// The words the compiler expands itself. The current token is the word.
static bool builtin(comp *c, const struct builtin *w) {
    fn *f = c->f;
    switch (w->id) {
    case BW_RANGE:
        if (!need(c, 2))
            return false;
        if (f->stack[f->depth - 1].type != TY_INT || f->stack[f->depth - 2].type != TY_INT)
            return type_error(c, "range takes two int values: lo hi range");
        return settle(c, f->depth - 2) && settle(c, f->depth - 1) &&
               push(c, (entry){.type = TY_STEM, .kind = E_SLOT, .op = STEM_RANGE});
    case BW_BYTES:
        if (!need(c, 1))
            return false;
        if (f->stack[f->depth - 1].type != TY_STR)
            return type_error(c, "bytes takes a str");
        return settle(c, f->depth - 1) && push(c, (entry){.type = TY_STEM, .kind = E_SLOT, .op = STEM_BYTES});
    case BW_SUM: return do_sum(c);
    case BW_LEN:
    case BW_IS_EMPTY: {
        if (!need(c, 1))
            return false;
        if (f->stack[f->depth - 1].type != TY_STR)
            return type_error(c, "this word takes a str");
        uint32_t reg;
        if (!reg_of(c, f->depth - 1, &reg))
            return false;
        --f->depth;
        const uint32_t dst = top_slot(c, BANK_I, 1);
        if (emit_def(c, PNX_ABC(PNX_SLEN, dst, reg, 0), BANK_I) == NONE)
            return false;
        if (w->id == BW_LEN)
            return push_slot(c, TY_INT);
        c->f->def_at = NONE;
        return push(c, (entry){.type = TY_BOOL, .kind = E_COND, .op = PNX_JEQI, .a = (uint8_t)dst, .b = 0});
    }
    case BW_BYTE_AT: {
        if (!need(c, 2))
            return false;
        if (f->stack[f->depth - 2].type != TY_STR || f->stack[f->depth - 1].type != TY_INT)
            return type_error(c, "byte_at takes a str and an index: s i byte_at");
        uint32_t rs, ri;
        if (!reg_of(c, f->depth - 2, &rs) || !reg_of(c, f->depth - 1, &ri))
            return false;
        f->depth -= 2;
        const uint32_t dst = top_slot(c, BANK_I, 1);
        return emit_def(c, PNX_ABC(PNX_SBYTE, dst, rs, ri), BANK_I) != NONE && push_slot(c, TY_INT);
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
            return error(c, PINAX_ERR_NOT_FOUND, "this path does not name a word of a module in use");
    } else {
        const local *l = find_local(f, n);
        if (l)
            return push(c, (entry){.type = l->type, .kind = E_LOCAL, .reg = l->reg}) && advance(c);
        const constant *k = find_const(c, n);
        if (k) {
            if (k->type == TY_STR)
                return push_string(c, (uint32_t)k->value) && advance(c);
            return push(c, (entry){.type = k->type, .kind = E_CONST, .value = k->value}) && advance(c);
        }
        const uint32_t g = find_global(c, n);
        if (g != NONE) {
            const globalsym *sym = (const globalsym *)c->globals.ptr + g;
            if (!push_slot(c, sym->type))
                return false;
            return emit_def(c, PNX_ABX(PNX_GETG, slot_of(c, f->depth - 1), g), BANK_I) != NONE && advance(c);
        }
        uint32_t index;
        if (find_word(c, n, &index))
            return call_word(c, index);
        const core_word *w = find_core(n);
        if (w)
            return core(c, w) && advance(c);
        const struct builtin *bw = find_builtin(c, n);
        if (bw)
            return builtin(c, bw) && advance(c);
    }
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
    const bool closer = t.kind == PNX_T_RESERVED && (t.word == PNX_KW_end || t.word == PNX_KW_else);
    if (f->dead && !closer)
        return syntax(c, "this code cannot be reached: the code before it always leaves");
    // a stem is taken at once by its block or by a word like sum
    if (f->depth > f->floor && f->stack[f->depth - 1].type == TY_STEM &&
        !(t.kind == PNX_T_RESERVED && t.word == PNX_KW_do) && !(t.kind == PNX_T_NAME && name_is(tok_name(c), "sum")))
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
        return core(c, w) && advance(c);
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
        default: return end_do(c, b) && advance(c);
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
    f->is_top = is_top;
}

// word NAME ( effect ) body end
static bool definition(comp *c) {
    if (c->top_fn->n_blocks)
        return syntax(c, "a definition does not belong inside a block");
    if (!advance(c))
        return false;
    if (c->tok.kind != PNX_T_NAME)
        return syntax(c, "word is followed by the name of the new word");
    const name n = tok_name(c);
    if (name_is_global(c, n))
        return type_error(c, "this name is already defined");
    if (n.length > 31)
        return syntax(c, "a word name is at most 31 bytes long");
    if (!advance(c))
        return false;
    wordsym *w = vec_push(c, &c->words, sizeof(wordsym));
    if (!w)
        return false;
    __builtin_memset(w, 0, sizeof *w);
    w->name = n;
    if (!parse_sig(c, &w->sig))
        return false;
    char text[72];
    if (!sig_text(&w->sig, text, sizeof text))
        return syntax(c, "the signature is longer than 71 bytes");

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
    if (!advance(c))
        return false;
    if (c->tok.kind != PNX_T_NAME)
        return syntax(c, "use is followed by the name of a module");
    const name n = tok_name(c);
    if (name_is(n, "strings")) { // a standard module: part of the language, not of the host
        c->use_strings = true;
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

// global NAME type : a scalar, zero at the start
static bool declare_global(comp *c) {
    if (!advance(c))
        return false;
    if (c->tok.kind != PNX_T_NAME)
        return syntax(c, "global is followed by a name and a type");
    globalsym g = {.name = tok_name(c)};
    if (name_is_global(c, g.name))
        return type_error(c, "this name is already defined");
    if (g.name.length > 31)
        return syntax(c, "a global name is at most 31 bytes long");
    if (!advance(c))
        return false;
    if (c->tok.kind != PNX_T_NAME)
        return syntax(c, "global is followed by a name and a type");
    g.type = type_of_name(tok_name(c));
    if (g.type != TY_INT && g.type != TY_BOOL && g.type != TY_ERR)
        return type_error(c, g.type == TY_STR ? "a global holds str(N), not a str view; that is not supported yet"
                                              : "this type is not supported for a global yet");
    if (c->globals.n >= 0xfffu)
        return syntax(c, "too many globals");
    globalsym *slot = vec_push(c, &c->globals, sizeof(globalsym));
    if (!slot)
        return false;
    *slot = g;
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
        } else if (is_word(c, PNX_KW_word)) {
            wordsym *last;
            if (!definition(c))
                return false;
            last = (wordsym *)c->words.ptr + (c->words.n - 1);
            last->entry = im->code.n;
            if (!append_code(c, im, c->word_fn))
                return false;
        } else if (is_word(c, PNX_KW_const) && !c->top_fn->n_blocks) {
            if (!declare_const(c))
                return false;
        } else if (is_word(c, PNX_KW_global) && !c->top_fn->n_blocks) {
            if (!declare_global(c))
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
        pnx_wordrec rec = {.entry = w->entry, .int_cells = w->cells[0], .str_cells = w->cells[1]};
        put_text(rec.name, sizeof rec.name, w->name);
        if (!sig_text(&w->sig, rec.signature, sizeof rec.signature))
            return syntax(c, "the signature is longer than 71 bytes");
        __builtin_memcpy(p, &rec, sizeof rec);
        p += sizeof rec;
    }
    for (uint32_t k = 0; k < head.n_globals; ++k) {
        const globalsym *g = (const globalsym *)c->globals.ptr + k;
        pnx_globalrec rec = {.kind = PNX_G_INT, .elem_bytes = 8, .capacity = 1};
        put_text(rec.name, sizeof rec.name, g->name);
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
    if (head.n_code)
        __builtin_memcpy(p, im->code.ptr, (size_t)head.n_code * 4);
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
    if (mode != PINAX_LOAD_CODE) {
        st->error_text = "data-only load is not implemented yet";
        return PINAX_ERR_STATE;
    }
    if (source.len > UINT32_MAX) {
        st->error_text = "the source is longer than 4 GiB";
        return PINAX_ERR_ARGUMENT;
    }

    // the unused tail of the block is the compiler's working memory
    uint8_t *const tail = st->block + ((st->used + 15u) & ~(size_t)15u);
    uint8_t *const end = st->block + st->block_len;
    comp      c = {.st = st, .file = file ? file : ""};
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
        err = pinax_run(st, st->head->n_words - 1, NULL);
        if (err != PINAX_OK && err != PINAX_SUSPENDED) {
            // a fault while loading: nothing stays loaded, as after any other load error
            const char *text = st->error_text;
            pnx_unload(st, mark);
            st->error_text = text;
        }
    }
    return err;
}
