// pinax_state.c - the part of the host interface the P0 interpreter implements (RFC-0010):
// a state inside the host's block, host words, loading and checking a bytecode container,
// finding and running words. The rest of pinax.h is declared but answers PINAX_ERR_STATE.

#include "pinax_internal.h"

#include <stdckdint.h>
#include <stddef.h>

static const uint8_t empty_bytes[1] = {0};

const pnx_opinfo pnx_ops_info[PNX_OP_COUNT] = {
#define PNX_INFO(name, number, a, b, c, shape, second, fails, roles) [PNX_##name] = {a, b, c, shape, second, fails, 1, roles},
    PNX_OPS(PNX_INFO)
#undef PNX_INFO
};
#define opinfo pnx_ops_info

static void mark_yields(pinax_state *st);

static const uint8_t width_bytes[PNX_W_COUNT] = {1, 2, 4, 8, 1, 2, 4};

// ---- small helpers -------------------------------------------------------------------

static bool text_equal(const char *a, const char *b) {
    while (*a && *a == *b) {
        ++a;
        ++b;
    }
    return *a == *b;
}

static bool terminated(const char *text, size_t cap) {
    return __builtin_memchr(text, 0, cap) != NULL;
}

static bool copy_text(char *dst, size_t cap, const char *src) {
    size_t n = __builtin_strlen(src);
    if (n >= cap)
        return false;
    __builtin_memcpy(dst, src, n + 1);
    return true;
}

static pinax_err fail(pinax_state *st, pinax_err err, const char *text) {
    st->error_text = text;
    return err;
}

// Bytes from the block, zeroed. NULL when the block is too small; st->needed then says how
// much would have been enough so far.
static void *take(pinax_state *st, size_t bytes, size_t align) {
    size_t at = (st->used + align - 1) & ~(align - 1);
    if (bytes > SIZE_MAX - at)
        return NULL;
    if (at + bytes > st->block_len) {
        st->needed = at + bytes;
        return NULL;
    }
    st->used = at + bytes;
    __builtin_memset(st->block + at, 0, bytes);
    return st->block + at;
}

void *pnx_take(pinax_state *st, size_t bytes, size_t align) {
    return take(st, bytes, align);
}

// Scratch taken since `mark` is given back. The one way back, as take() is the one way in:
// a record of what the block holds, if take() ever keeps one, is kept here too.
static void give_back(pinax_state *st, size_t mark) {
    st->used = mark;
}

pnx_program pnx_program_now(const pinax_state *st) {
    return (pnx_program){st->head, st->words, st->importrecs, st->iconst, st->sconst, st->code, st->word_counts,
                         st->imports, st->globals, st->segment[PNX_SEG_CONST], st->segment[PNX_SEG_GLOBAL],
                         st->symbols, st->used, st->n_hosts, st->lines, st->localrecs, st->proven};
}

void pnx_program_restore(pinax_state *st, const pnx_program *p) {
    st->head = p->head;
    st->words = p->words;
    st->importrecs = p->importrecs;
    st->iconst = p->iconst;
    st->sconst = p->sconst;
    st->code = p->code;
    st->word_counts = p->word_counts;
    st->imports = p->imports;
    st->globals = p->globals;
    st->segment[PNX_SEG_CONST] = p->seg_const;
    st->segment[PNX_SEG_GLOBAL] = p->seg_global;
    st->symbols = p->symbols;
    st->used = p->used;
    st->n_hosts = p->n_hosts;
    st->lines = p->lines;
    st->localrecs = p->localrecs;
    st->proven = p->proven;
    st->suspended = false;
    st->loading = false;
}

// ---- signatures ------------------------------------------------------------------------

static bool is_name_char(char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_';
}

static bool token_is(const char *start, size_t len, const char *word) {
    return __builtin_strlen(word) == len && __builtin_memcmp(start, word, len) == 0;
}

bool pnx_signature_counts(const char *sig, pnx_counts *out) {
    pnx_counts n = {0};
    const char *p = sig;
    while (*p == ' ')
        ++p;
    if (*p++ != '(')
        return false;
    bool results = false;
    unsigned tokens = 0;        // names seen in the parameter being read
    const char *type = NULL;    // its last name is the type
    size_t type_len = 0;
    for (;;) {
        while (*p == ' ')
            ++p;
        const char *start = p;
        while (is_name_char(*p))
            ++p;
        size_t len = (size_t)(p - start);
        bool is_gives = len && token_is(start, len, "gives");
        bool is_or = len && token_is(start, len, "or");
        if (len && !is_gives && !is_or) {
            if (++tokens > 3)
                return false;
            type = start;
            type_len = len;
            continue;
        }
        // a parameter ends here
        if (tokens) {
            if (token_is(type, type_len, "float"))
                return false; // no F bank in P0
            // cstr: a str handed to a C function as a NUL-terminated copy (extern only)
            uint8_t *slot = token_is(type, type_len, "str") || token_is(type, type_len, "cstr")
                                ? (results ? &n.str_results : &n.str_args)
                                                           : (results ? &n.int_results : &n.int_args);
            if (*slot == UINT8_MAX)
                return false;
            ++*slot;
            tokens = 0;
        }
        if (is_gives) {
            if (results)
                return false;
            results = true;
        } else if (is_or) {
            while (*p == ' ')
                ++p;
            if (__builtin_strncmp(p, "fail", 4) != 0 || is_name_char(p[4]))
                return false;
            p += 4;
            while (*p == ' ')
                ++p;
            if (*p != ')')
                return false;
            n.fails = true;
        } else if (*p == ',') {
            ++p;
        } else if (*p != ')') {
            return false;
        }
        if (*p == ')') {
            ++p;
            break;
        }
    }
    while (*p == ' ')
        ++p;
    if (*p != 0 || n.str_args > PNX_MAX_STR_ARGS || n.str_results > PNX_MAX_STR_RESULTS)
        return false;
    *out = n;
    return true;
}

// ---- opening ---------------------------------------------------------------------------

pinax_err pinax_open(pinax_mem block, const pinax_config *config, pinax_state **out) {
    if (!out)
        return PINAX_ERR_ARGUMENT;
    *out = NULL;
    // the configuration may be the shorter one of a host built before hash_key was added
    const size_t without_key = offsetof(pinax_config, hash_key);
    if (!block.ptr || !config || (config->struct_size != sizeof *config && config->struct_size != without_key) ||
        config->float_slots != 0)
        return PINAX_ERR_ARGUMENT;
    uint8_t *base = block.ptr;
    size_t skip = (size_t)(-(uintptr_t)base & 15u);
    if (block.len < skip + sizeof(pinax_state))
        return PINAX_ERR_NO_ROOM;
    pinax_state *st = (pinax_state *)(void *)(base + skip);
    __builtin_memset(st, 0, sizeof *st);
    st->block = base + skip;
    st->block_len = block.len - skip;
    st->used = sizeof *st;
    st->int_slots = config->int_slots ? config->int_slots : 4096;
    st->str_slots = config->str_slots ? config->str_slots : 1024;
    st->region_len = config->region_bytes ? config->region_bytes : 65536;
    st->depth_max = st->int_slots / 4 < 64 ? 64 : st->int_slots / 4;
    st->budget = config->budget;
    st->diag = config->diag;
    st->diag_user = config->diag_user;
    // no key given: a fixed one, so that an index behaves the same from run to run
    static const uint8_t fixed_key[16] = {'p', 'i', 'n', 'a', 'x', ' ', 'i', 'n', 'd', 'e', 'x', ' ', 'k', 'e', 'y', '0'};
    bool given = false;
    for (unsigned k = 0; config->struct_size == sizeof *config && k < 16; ++k)
        given = given || config->hash_key[k] != 0;
    __builtin_memcpy(st->hash_key, given ? config->hash_key : fixed_key, 16);
    st->error_text = "";

    st->hosts = take(st, sizeof(pnx_host) * PNX_MAX_HOSTS, 16);
    st->ibank = st->hosts ? take(st, sizeof(int64_t) * st->int_slots, 16) : NULL;
    st->sbank = st->ibank ? take(st, sizeof(pnx_scell) * st->str_slots, 16) : NULL;
    st->ctrl = st->sbank ? take(st, sizeof(pnx_frame) * st->depth_max, 16) : NULL;
    st->region = st->ctrl ? take(st, st->region_len, 16) : NULL;
    st->handle_slots = config->handle_slots ? config->handle_slots : 64;
    st->handles = st->region ? take(st, sizeof(pnx_handle_slot) * st->handle_slots, 16) : NULL;
    if (!st->handles)
        return PINAX_ERR_NO_ROOM;
    for (uint32_t k = 0; k < st->handle_slots; ++k) // every slot is free: slot k links to k + 1
        st->handles[k].next_free = k + 1 < st->handle_slots ? k + 2 : 0;
    st->handle_free = 1;
    for (unsigned k = 0; k < PNX_SEG_COUNT; ++k)
        st->segment[k] = empty_bytes;
    st->segment[PNX_SEG_REGION] = st->region;
    st->segment[PNX_SEG_STATIC] = (const uint8_t *)pnx_static_text;
    *out = st;
    return PINAX_OK;
}

// Gives a slot back and, when asked, tells the host through the type's close function.
static void handle_drop(pinax_state *st, uint32_t slot, bool call_close) {
    pnx_handle_slot *h = &st->handles[slot];
    const pnx_handle_type *type = &st->handle_types[h->type - 1];
    void *object = h->object;
    ++h->generation; // even: free
    h->object = NULL;
    h->type = 0;
    h->next_free = st->handle_free;
    st->handle_free = slot + 1;
    --st->handles_open;
    if (call_close && type->close)
        type->close(object, type->user);
}

// Closes every handle that is still open, or only those opened by run `run` (0: all).
static void handles_close(pinax_state *st, uint32_t run) {
    for (uint32_t k = 0; k < st->handle_slots; ++k) {
        if (st->handles[k].type && (run == 0 || st->handles[k].run == run))
            handle_drop(st, k, true);
    }
}

void pinax_close(pinax_state *state) {
    if (state)
        handles_close(state, 0);
}

size_t pinax_needed(const pinax_state *state) {
    return state ? state->needed : 0;
}

pinax_str pinax_error_text(const pinax_state *state) {
    pinax_str text = {(const unsigned char *)state->error_text, __builtin_strlen(state->error_text)};
    return text;
}

// ---- host words --------------------------------------------------------------------------

pinax_err pinax_register(pinax_state *st, const char *module, const pinax_word_def *defs, size_t count) {
    if (!st || !module || (!defs && count))
        return PINAX_ERR_ARGUMENT;
    if (st->from_image) { // the host words of the image, given again: same name, signature and flags
        for (size_t k = 0; k < count; ++k) {
            pnx_host *h = NULL;
            for (uint32_t j = 0; !h && j < st->n_hosts; ++j) {
                if (st->hosts[j].module[0] != '@' && text_equal(st->hosts[j].module, module) && defs[k].name &&
                    text_equal(st->hosts[j].name, defs[k].name))
                    h = &st->hosts[j];
            }
            if (!h)
                return fail(st, PINAX_ERR_NOT_FOUND, "not a host word of the image");
            if (!defs[k].fn || !defs[k].signature || !text_equal(h->signature, defs[k].signature) ||
                h->flags != defs[k].flags)
                return fail(st, PINAX_ERR_SIGNATURE, "the host word of the image has another signature or flags");
            h->fn = defs[k].fn;
            h->user = defs[k].user;
        }
        return PINAX_OK;
    }
    if (st->head)
        return fail(st, PINAX_ERR_STATE, "host words must be registered before code is loaded");
    if (count > PNX_MAX_HOSTS - st->n_hosts)
        return fail(st, PINAX_ERR_NO_ROOM, "too many host words");
    for (size_t k = 0; k < count; ++k) {
        pnx_host *h = &st->hosts[st->n_hosts];
        if (!defs[k].name || !defs[k].signature || !defs[k].fn || !copy_text(h->module, sizeof h->module, module) ||
            !copy_text(h->name, sizeof h->name, defs[k].name) ||
            !copy_text(h->signature, sizeof h->signature, defs[k].signature) ||
            !pnx_signature_counts(h->signature, &h->counts))
            return fail(st, PINAX_ERR_ARGUMENT, "host word: bad name or signature");
        h->fn = defs[k].fn;
        h->user = defs[k].user;
        h->flags = defs[k].flags;
        ++st->n_hosts;
    }
    return PINAX_OK;
}

pinax_state *pinax_call_state(pinax_call *call) {
    return call->state;
}

pinax_mem pinax_call_alloc(pinax_call *call, size_t bytes) {
    pinax_mem mem = {pnx_region_alloc(call->state, bytes), bytes};
    if (!mem.ptr)
        mem.len = 0;
    return mem;
}

pinax_mem pinax_call_buffer(pinax_call *call, int64_t buffer, size_t written) {
    const pinax_state *st = call->state;
    const pinax_mem none = {NULL, 0};
    if (!st->head || buffer < 0 || buffer >= (int64_t)st->head->n_globals)
        return none;
    const pnx_global *g = &st->globals[buffer];
    if (g->kind != PNX_G_BUFFER || g->shared)
        return none;
    uint64_t used;
    __builtin_memcpy(&used, g->ptr, 8);
    if (written > g->capacity - used)
        return none;
    used += written;
    __builtin_memcpy(g->ptr, &used, 8);
    const pinax_mem room = {g->ptr + 8 + used, (size_t)(g->capacity - used)};
    return room;
}

// ---- loading -----------------------------------------------------------------------------

typedef struct span {
    uint32_t begin, end; // instruction indexes of one word
} span;

static bool is_failure_word(uint32_t op) {
    return op == PNX_JMP || op == PNX_RETF || op == PNX_FAIL || op == PNX_FAILR;
}

static bool reg_ok(uint32_t kind, uint32_t value, const pnx_wordrec *word, uint32_t n_globals) {
    switch (kind) {
    case PNX_K_I: return value < word->int_cells;
    case PNX_K_S: return value < word->str_cells;
    case PNX_K_G: return value < n_globals;
    default: return true;
    }
}

// Where a jump goes, or -1 for an instruction that does not jump.
static int64_t jump_of(const uint32_t *code, uint32_t i) {
    const uint32_t w = code[i];
    const pnx_opinfo info = opinfo[w & 0xffu];
    if (info.shape == PNX_F_AX)
        return (int64_t)i + 1 + ((int32_t)w >> 8);
    if (info.shape == PNX_F_ABC && info.c == PNX_K_J)
        return (int64_t)i + 1 + (int8_t)(w >> 24);
    return -1;
}

// An instruction that can make code run again: a jump back, a call (the callee may call again),
// a tail call. The budget is charged by a CHECK right before each, so that no run goes on for
// ever: a hand-made container without them would never be suspended.
static bool repeats(const uint32_t *code, uint32_t i) {
    const uint32_t op = code[i] & 0xffu;
    if (op == PNX_CALL || op == PNX_CALLR || op == PNX_TCALL)
        return true;
    const int64_t target = jump_of(code, i);
    return target >= 0 && target <= (int64_t)i;
}

// Checks the shape of one word's code: every register inside the window, every index inside
// its table, every jump onto an instruction of the same word, a failure word after every
// operation that can fail. Types are not checked here (P1).
static const char *check_word(const pinax_state *st, uint32_t index, span range, uint8_t *heads) {
    const pnx_head *head = st->head;
    const pnx_wordrec *word = &st->words[index];
    const pnx_counts counts = st->word_counts[index];
    const uint32_t *code = st->code;
    if (counts.int_args > word->int_cells || counts.int_results > word->int_cells ||
        counts.str_args > word->str_cells || counts.str_results > word->str_cells)
        return "word: window smaller than its arguments or results";
    if (range.begin >= range.end)
        return "word: no code";

    uint32_t last = range.begin;
    for (uint32_t i = range.begin; i < range.end;) {
        const uint32_t op = code[i] & 0xffu;
        if (!opinfo[op].known)
            return "code: unknown instruction";
        if (!heads[i])
            heads[i] = 1;
        last = i;
        i += opinfo[op].second ? 2 : 1;
        if (i > range.end)
            return "code: instruction cut off";
        if (op == PNX_GSEL) { // the next instruction uses the global it chose: not a jump target
            if (i == range.end)
                return "code: GSEL at the end of a word";
            heads[i] = 2;
        }
    }
    const uint32_t last_op = code[last] & 0xffu;
    if (last_op != PNX_JMP && last_op != PNX_RET && last_op != PNX_RETOK && last_op != PNX_RETF &&
        last_op != PNX_FAIL && last_op != PNX_FAILR && last_op != PNX_TCALL && last_op != PNX_FAULT)
        return "word: code runs off the end";

    for (uint32_t i = range.begin; i < range.end;) {
        const uint32_t w = code[i], op = w & 0xffu;
        const pnx_opinfo info = opinfo[op];
        const uint32_t a = (w >> 8) & 0xffu, b = (w >> 16) & 0xffu, c = w >> 24, bx = w >> 16;
        const uint32_t next = i + (info.second ? 2u : 1u);
        const uint32_t x = info.second ? code[i + 1] : 0;
        bool fails = info.fails;
        int64_t target = -1;

        switch (info.shape) {
        case PNX_F_ABC:
            if (!reg_ok(info.a, a, word, head->n_globals) || !reg_ok(info.b, b, word, head->n_globals) ||
                !reg_ok(info.c, c, word, head->n_globals))
                return "code: register or global outside its table";
            if (info.c == PNX_K_J)
                target = (int64_t)i + 1 + (int8_t)c;
            break;
        case PNX_F_IMM:
            if (!reg_ok(info.a, a, word, head->n_globals))
                return "code: register outside the window";
            break;
        case PNX_F_ICONST:
            if (!reg_ok(info.a, a, word, head->n_globals) || bx >= head->n_iconst)
                return "code: integer constant outside its table";
            break;
        case PNX_F_SCONST:
            if (!reg_ok(info.a, a, word, head->n_globals) || bx >= head->n_sconst)
                return "code: string constant outside its table";
            break;
        case PNX_F_GLOBAL:
            if (!reg_ok(info.a, a, word, head->n_globals) || (bx >= head->n_globals && bx != PNX_GSELECTED))
                return "code: global outside its table";
            break;
        default: // PNX_F_AX
            target = (int64_t)i + 1 + ((int32_t)w >> 8);
            break;
        }
        if (target >= 0 || info.c == PNX_K_J || info.shape == PNX_F_AX) {
            if (target < range.begin || target >= range.end || heads[target] != 1)
                return "code: jump outside the word or into an instruction";
        }
        if (repeats(code, i) && (i == range.begin || heads[i - 1] != 1 || (code[i - 1] & 0xffu) != PNX_CHECK ||
                                 code[i - 1] >> 16 == 0))
            return "code: a jump back or a call without a CHECK that charges the budget right before it";
        if (target >= 0 && repeats(code, (uint32_t)target))
            return "code: a jump onto a jump back or a call, past its CHECK";

        // what each instruction needs of the global it names
        uint32_t want = PNX_G_COUNT, global = 0;
        switch (op) {
        case PNX_GETG: case PNX_SETG: want = PNX_G_INT; global = bx; break;
        case PNX_AGET: case PNX_ASET: want = PNX_G_ARRAY; global = c; break; // or a list, below
        case PNX_GLEN: case PNX_GCLEAR: want = PNX_G_COUNT + 1; global = bx; break; // any collection
        case PNX_GCAP: want = PNX_G_COUNT + 2; global = bx; break; // any collection or a buffer
        case PNX_ADDROW: want = PNX_G_ROWS; global = bx; break;
        case PNX_BCAT: case PNX_BCATI: case PNX_BTRUNC: want = PNX_G_BUFFER; global = bx; break;
        case PNX_QPUT: case PNX_QTAKE: case PNX_QPEEK: case PNX_QWTAKE: case PNX_QWPUT: want = PNX_G_QUEUE; global = bx; break;
        case PNX_PALLOC: case PNX_PFREE: want = PNX_G_POOL; global = bx; break;
        case PNX_PALIVE: want = PNX_G_POOL; global = c; break;
        case PNX_IXPUT: case PNX_IXDEL: want = PNX_G_INDEX; global = bx; break;
        case PNX_IXGET: case PNX_IXGETS: want = PNX_G_INDEX; global = c; break;
        case PNX_LPUSH: case PNX_LPOP: want = PNX_G_LIST; global = bx; break;
        case PNX_LPUSHS: case PNX_LPOPS: want = PNX_G_ROWS; global = bx; break;
        case PNX_TSPAWN: case PNX_TRESUME: case PNX_TDONE: want = PNX_G_TASKS; global = c; break;
        case PNX_TCANCEL: case PNX_TRUNALL: want = PNX_G_TASKS; global = bx; break;
        case PNX_ROW: want = PNX_G_TABLE; global = c; break;
        case PNX_BCLEAR: case PNX_BVIEW: want = PNX_G_BUFFER; global = bx; break;
        case PNX_BPUT: case PNX_BPUTS: case PNX_BENC: case PNX_JPUTI: case PNX_JPUTS: case PNX_CPUTS: case PNX_CPUTI:
            want = PNX_G_BUFFER; global = c; break;
        case PNX_FGET: case PNX_FSET: case PNX_FGETS: case PNX_FSETS: want = PNX_G_TABLE; global = x & 0xfffu; break;
        default: break;
        }
        if (op == PNX_LCOPYR) { // a row of table x copied into list BX: rows of one size
            // one of the two may be the table a GSEL right before chose (RFC-0018)
            const bool sx = x == PNX_GSELECTED, sbx = bx == PNX_GSELECTED;
            if (sx && sbx)
                return "code: rows copied between two chosen tables";
            if ((sx || sbx) && heads[i] != 2)
                return "code: a selected global without its GSEL";
            const uint32_t gsel_kind = sx || sbx ? (code[i - 2] >> 16) & 0xffu : 0, gsel_elem = sx || sbx ? code[i - 1] : 0;
            if ((!sx && x >= head->n_globals) || (!sbx && bx >= head->n_globals))
                return "code: global outside its table";
            const uint32_t to_kind = sbx ? gsel_kind : st->globals[bx].kind, from_kind = sx ? gsel_kind : st->globals[x].kind;
            const uint32_t to_elem = sbx ? gsel_elem : st->globals[bx].elem_bytes, from_elem = sx ? gsel_elem : st->globals[x].elem_bytes;
            if (to_kind != PNX_G_ROWS || (from_kind != PNX_G_ROWS && from_kind != PNX_G_TABLE) || to_elem != from_elem)
                return "code: rows copied between tables of another kind or size";
            if (!sbx && st->globals[bx].shared)
                return "code: a shared global is written";
        } else if (info.second == PNX_X_GLOBAL) { // a wide form: what its narrow form needs, of the global in x
            switch (op) {
            case PNX_AGETW: case PNX_ASETW: want = PNX_G_ARRAY; break;
            case PNX_ROWW: want = PNX_G_TABLE; break;
            case PNX_PALIVEW: want = PNX_G_POOL; break;
            case PNX_IXGETW: case PNX_IXGETSW: want = PNX_G_INDEX; break;
            case PNX_BPUTW: case PNX_BPUTSW: case PNX_BENCW: case PNX_JPUTIW: case PNX_JPUTSW: case PNX_CPUTSW:
            case PNX_CPUTIW: want = PNX_G_BUFFER; break;
            default: want = PNX_G_TASKS; break; // TSPAWNW TRESUMEW TDONEW
            }
            global = x;
        }
        // a global a reference chose: the GSEL right before says its kind and element size
        const bool selected = global == PNX_GSELECTED ||
                              (info.second == PNX_X_FIELD && (x & 0xfffu) == PNX_FSELECTED);
        uint32_t sel_kind = PNX_G_COUNT, sel_elem = 0;
        if (selected) {
            if (heads[i] != 2 || want == PNX_G_COUNT)
                return "code: a selected global without its GSEL";
            sel_kind = (code[i - 2] >> 16) & 0xffu;
            sel_elem = code[i - 1];
        }
        if (want != PNX_G_COUNT) {
            if (!selected && global >= head->n_globals)
                return "code: global outside its table";
            const uint32_t kind = selected ? sel_kind : st->globals[global].kind;
            const bool ok = kind == want || (want == PNX_G_ARRAY && kind == PNX_G_LIST) ||
                            (op == PNX_BTRUNC && (kind == PNX_G_LIST || kind == PNX_G_ROWS)) ||
                            (want == PNX_G_TABLE && (kind == PNX_G_ROWS || (kind == PNX_G_POOL && op != PNX_ROW && op != PNX_ROWW))) ||
                            (want == PNX_G_COUNT + 1 && kind != PNX_G_INT && kind != PNX_G_BUFFER &&
                             kind != PNX_G_INDEX && kind != PNX_G_TASKS) ||
                            (want == PNX_G_COUNT + 2 && kind != PNX_G_INT && kind != PNX_G_INDEX && kind != PNX_G_TASKS &&
                             kind != PNX_G_POOL);
            if (!ok)
                return "code: global of the wrong kind";
            if (!selected && pnx_writes_global(op) && st->globals[global].shared)
                return "code: a shared global is written";
            if (selected && (want == PNX_G_INDEX || want == PNX_G_POOL || want == PNX_G_TASKS))
                return "code: this global cannot be chosen by a reference";
            if (((op == PNX_IXGET || op == PNX_IXGETW) && st->globals[global].key_is_str) ||
                ((op == PNX_IXGETS || op == PNX_IXGETSW) && !st->globals[global].key_is_str))
                return "code: key of the wrong kind for this index";
        }
        if (info.second == PNX_X_FIELD) {
            const uint32_t offset = (x >> 12) & 0xfffu, extra = x >> 24;
            const bool is_text = op == PNX_FGETS || op == PNX_FSETS;
            if (!is_text && extra >= PNX_W_COUNT)
                return "code: unknown field width";
            if (offset + (is_text ? 1u + extra : width_bytes[extra]) > (selected ? sel_elem : st->globals[global].elem_bytes))
                return "code: field outside its record";
        }
        const uint32_t wcode = op == PNX_SGET ? c : b; // a width, or varint; big-endian on a width
        if (((op == PNX_BPUT || op == PNX_BPUTW || op == PNX_SGET) &&
             (wcode & ~(PNX_W_BE | 7u) || (wcode & 7u) > PNX_W_VARINT || wcode == (PNX_W_VARINT | PNX_W_BE))) ||
            ((op == PNX_BPUTS || op == PNX_BPUTSW) && b > 1) || (op == PNX_SGETS && c > 1))
            return "code: unknown width";
        if (((op == PNX_BENC || op == PNX_BENCW) && b >= PNX_ENC_COUNT) || (op == PNX_SHASH && c >= PNX_HASH_COUNT))
            return "code: unknown encoding or hash";
        if (((op == PNX_JPUTI || op == PNX_JPUTIW) && (b == 0 || b == PNX_JSON_KEY || b == PNX_JSON_STRING ||
                                                       (b >= PNX_JSON_KINDS && (b < PNX_JSON_DEC || b > PNX_JSON_DEC + 18)))) ||
            ((op == PNX_JPUTS || op == PNX_JPUTSW) && b != PNX_JSON_KEY && b != PNX_JSON_STRING) ||
            ((op == PNX_CPUTI || op == PNX_CPUTIW) && b > 3 + 18) || ((op == PNX_CPUTS || op == PNX_CPUTSW) && b != 0) ||
            (op == PNX_CNEXT && c > 2) || (op == PNX_FITW && c >= PNX_W_COUNT))
            return "code: unknown kind of a json or csv instruction";
        if (((op == PNX_RCATD || op == PNX_STODEC) && c > 18) || (op == PNX_DRESC && (c == 0 || c > 18)))
            return "code: a decimal scale outside 0 to 18";
        if (info.second == PNX_X_SREG && x >= word->str_cells)
            return "code: register outside the window";
        { // string registers an instruction changes in place, or more than one it writes, are all
          // different from its other string registers: the instruction keeps pointers into them
            uint32_t regs[4], n = 0, changes = 0;
            bool     in_place = false;
            const uint32_t kinds[3] = {info.a, info.b, info.c}, values[3] = {a, b, c};
            for (unsigned k = 0; k < 3; ++k) {
                if (kinds[k] != PNX_K_S)
                    continue;
                regs[n++] = values[k];
                changes += info.roles[k] == 'w' || info.roles[k] == 'm';
                in_place = in_place || info.roles[k] == 'm';
            }
            if (info.second == PNX_X_SREG)
                regs[n++] = x;
            for (uint32_t p = 0; (in_place || changes > 1) && p < n; ++p) {
                for (uint32_t q = p + 1; q < n; ++q) {
                    if (regs[p] == regs[q])
                        return "code: an instruction that changes a string register names it twice";
                }
            }
        }

        if (info.second == PNX_X_WORD) {
            if (x >= head->n_words)
                return "code: call of a word outside the table";
            if (op == PNX_CALL)
                fails = st->word_counts[x].fails;
            else if (st->word_counts[x].fails != counts.fails)
                return "code: tail call between a failing and a non-failing word";
            // the callee's results are this word's: what it leaves unwritten would be read as a result
            else if (st->word_counts[x].int_results != counts.int_results ||
                     st->word_counts[x].str_results != counts.str_results)
                return "code: tail call to a word with other results";
        }
        if (info.second == PNX_X_COUNTS) { // a call through a register: its arguments and results fit the window
            fails = x >> 31 != 0;
            const uint32_t ia = x & 0xffu, sa = (x >> 8) & 0xffu, ir = (x >> 16) & 0xffu, sr = (x >> 24) & 0x7fu;
            if (a + (ia > ir ? ia : ir) > word->int_cells || b + (sa > sr ? sa : sr) > word->str_cells)
                return "code: call outside the window";
        }
        if (info.second == PNX_X_IMPORT) {
            if (x >= head->n_imports)
                return "code: host word outside the table";
            const pnx_counts hc = st->imports[x]->counts;
            if (hc.fails != (op == PNX_HCALLF))
                return "code: host call does not match the word's `or fail`";
            if (a + hc.int_args + hc.int_results > word->int_cells ||
                b + hc.str_args + hc.str_results > word->str_cells)
                return "code: host call outside the window";
        }
        // success skips the failure word (one code word): something must follow it in the word
        if (fails && (next + 1 >= range.end || !is_failure_word(code[next] & 0xffu)))
            return "code: no failure word after an operation that can fail, or nothing after it";
        if ((op == PNX_RETOK || op == PNX_RETF || op == PNX_FAIL || op == PNX_FAILR) && !counts.fails)
            return "code: failure return in a word without `or fail`";
        if (op == PNX_RET && counts.fails)
            return "code: plain return in a word with `or fail`";
        i = next;
    }
    return NULL;
}

// ---- which registers hold a value ----------------------------------------------------------
//
// The second check of a word (RFC-0013 S6): on every path, a register is written before it is
// read. Without it a word could read a string cell left by an earlier run, whose bytes may be
// gone. What a call leaves above its results is not a value either: the callee's window was
// there.

typedef struct regset {
    uint64_t i[4], s[4];
} regset;

static bool set_has(const uint64_t *set, uint32_t from, uint32_t count) {
    for (uint32_t k = from; k < from + count; ++k) {
        if (k > 255 || !(set[k >> 6] >> (k & 63u) & 1u))
            return false;
    }
    return true;
}
static void set_put(uint64_t *set, uint32_t from, uint32_t count) {
    for (uint32_t k = from; k < from + count && k < 256; ++k)
        set[k >> 6] |= (uint64_t)1 << (k & 63u);
}
static void set_cut(uint64_t *set, uint32_t from) { // nothing from `from` up holds a value
    for (uint32_t k = from; k < 256; ++k)
        set[k >> 6] &= ~((uint64_t)1 << (k & 63u));
}
// What holds a value on both ways into an instruction. True when `into` lost something.
static bool set_meet(regset *into, const regset *from) {
    bool changed = false;
    for (unsigned k = 0; k < 4; ++k) {
        const uint64_t i = into->i[k] & from->i[k], s = into->s[k] & from->s[k];
        changed = changed || i != into->i[k] || s != into->s[k];
        into->i[k] = i;
        into->s[k] = s;
    }
    return changed;
}

// The work of check_flow for one instruction: what reaches it narrows what reaches the places it
// goes on to; a place that narrowed and is not waiting to be looked at again is put on the list.
// In the judging pass (`last`), a read of a register that may hold no value is the verdict.
static const char *flow_step(const pinax_state *st, pnx_counts counts, span range, regset *in, uint32_t i, bool last,
                             uint32_t *queue, uint8_t *queued, uint32_t *n_queue) {
    const uint32_t *code = st->code;
#define MEET(at, set)                                                                              \
    do {                                                                                           \
        const uint32_t at_ = (uint32_t)(at);                                                       \
        if (set_meet(&in[at_], (set)) && !queued[at_]) {                                           \
            queued[at_] = 1;                                                                       \
            queue[(*n_queue)++] = at_;                                                             \
        }                                                                                          \
    } while (0)
    const uint32_t w = code[i], op = w & 0xffu;
    const pnx_opinfo info = opinfo[op];
    const uint32_t reg[3] = {(w >> 8) & 0xffu, (w >> 16) & 0xffu, w >> 24};
    const uint8_t kind[3] = {info.a, info.b, info.c};
    const uint32_t next = i + (info.second ? 2u : 1u);
    const uint32_t x = info.second ? code[i + 1] : 0;
    regset now = in[i - range.begin], failed;
    bool fails = info.fails, reads = true, falls = true;
    int64_t target = -1;

    const char *role = info.roles; // of each of A, B, C: r, w, m, or - for what names no register
    for (unsigned k = 0; k < 3; ++k) { // what it reads holds a value
        if (role[k] == 'r' || role[k] == 'm')
            reads = reads && set_has(kind[k] == PNX_K_I ? now.i : now.s, reg[k], 1);
    }
    if (info.second == PNX_X_SREG)
        reads = reads && x < 256 && set_has(now.s, x, 1);
    pnx_counts callee = {0};
    uint32_t ibase = reg[0], sbase = reg[1];
    const bool calls = op == PNX_CALL || op == PNX_CALLR, hosts = op == PNX_HCALL || op == PNX_HCALLF;
    if (op == PNX_CALL || op == PNX_TCALL) {
        callee = st->word_counts[x];
    } else if (op == PNX_CALLR) {
        callee = (pnx_counts){(uint8_t)x, (uint8_t)(x >> 8), (uint8_t)(x >> 16), (uint8_t)((x >> 24) & 0x7fu),
                              x >> 31 != 0, false};
    } else if (hosts) {
        callee = st->imports[x]->counts;
    }
    if (op == PNX_TCALL)
        ibase = sbase = 0;
    if (calls || hosts || op == PNX_TCALL)
        reads = reads && set_has(now.i, ibase, callee.int_args) && set_has(now.s, sbase, callee.str_args);
    if (op == PNX_RET || op == PNX_RETOK)
        reads = set_has(now.i, 0, counts.int_results) && set_has(now.s, 0, counts.str_results);
    if (!reads && last)
        return "code: a register is read before it is written";

    failed = now; // on the way where the operation fails nothing was written
    if (calls) { // the callee's window lay over everything from its arguments up
        set_cut(now.i, ibase);
        set_cut(now.s, sbase);
        failed = now;
        set_put(now.i, ibase, callee.int_results);
        set_put(now.s, sbase, callee.str_results);
        fails = callee.fails;
    } else if (hosts) { // results follow the arguments; a word run from the host word lies above
        set_cut(now.i, ibase + callee.int_args);
        set_cut(now.s, sbase + callee.str_args);
        failed = now;
        set_put(now.i, ibase + callee.int_args, callee.int_results);
        set_put(now.s, sbase + callee.str_args, callee.str_results);
    } else {
        for (unsigned k = 0; k < 3; ++k) {
            if (role[k] == 'w')
                set_put(kind[k] == PNX_K_I ? now.i : now.s, reg[k], 1);
        }
    }

    switch (op) {
    case PNX_JMP: target = (int64_t)i + 1 + ((int32_t)w >> 8); falls = false; break;
    case PNX_RET: case PNX_RETOK: case PNX_RETF: case PNX_FAIL: case PNX_FAILR: case PNX_FAULT:
    case PNX_TCALL: falls = false; break;
    default:
        if (info.shape == PNX_F_ABC && info.c == PNX_K_J)
            target = (int64_t)i + 1 + (int8_t)reg[2];
        break;
    }
    // the shape check has seen that every way on stays inside the word
    if (target >= 0)
        MEET(target - range.begin, &now);
    if (falls && fails) { // the failure word comes next; success skips it
        MEET(next - range.begin, &failed);
        if (next + 1 < range.end)
            MEET(next + 1 - range.begin, &now);
    } else if (falls && next < range.end) {
        MEET(next - range.begin, &now);
    }
    return NULL;
#undef MEET
}

// Checked after the shape: on every way to an instruction, the registers it reads hold a value.
// Each place is looked at when something that reaches it narrowed (a worklist: each narrowing
// takes a bit away, so the work is bounded by the bits, not by rounds over the word; BACKLOGS
// R17), then once more in order to judge. `work` holds a set for each code word, then a list
// of places and a flag for each.
static const char *check_flow(const pinax_state *st, uint32_t index, span range, regset *in) {
    const pnx_counts counts = st->word_counts[index];
    const uint32_t *code = st->code;
    const uint32_t n = range.end - range.begin;
    uint32_t *queue = (uint32_t *)(void *)(in + n);
    uint8_t  *queued = (uint8_t *)(void *)(queue + n);
    __builtin_memset(in, 0xff, (size_t)n * sizeof *in); // nothing known yet: every way in may narrow it
    __builtin_memset(&in[0], 0, sizeof in[0]);
    __builtin_memset(queued, 0, n);
    set_put(in[0].i, 0, counts.int_args);
    set_put(in[0].s, 0, counts.str_args);
    uint32_t n_queue = 0;
    for (uint32_t i = range.begin; i < range.end;) { // every place once, in order: the list is taken from its end
        queue[n_queue++] = i - range.begin;
        i += opinfo[code[i] & 0xffu].second ? 2u : 1u;
    }
    for (uint32_t k = 0; k < n_queue / 2; ++k) {
        const uint32_t t = queue[k];
        queue[k] = queue[n_queue - 1 - k];
        queue[n_queue - 1 - k] = t;
    }
    for (uint32_t k = 0; k < n_queue; ++k)
        queued[queue[k]] = 1;
    while (n_queue) {
        const uint32_t at = queue[--n_queue];
        queued[at] = 0;
        (void)flow_step(st, counts, range, in, at + range.begin, false, queue, queued, &n_queue);
    }
    for (uint32_t i = range.begin; i < range.end;) {
        const char *why = flow_step(st, counts, range, in, i, true, queue, queued, &n_queue);
        if (why)
            return why;
        i += opinfo[code[i] & 0xffu].second ? 2u : 1u;
    }
    return NULL;
}

// ---- which checks cannot fire (RFC-0023) ----------------------------------------------------
//
// A third pass over each word, after the two checks: it proves, where it can, that a run-time
// check cannot fire. An index a counter keeps below an array's capacity, a row checked a moment
// ago on the same table, an add of two values known to be small. The proofs change nothing
// that runs: they are a byte per instruction (`st->proven`, the reason) that `pnx see --checks`
// lists and that a C exporter would read to leave the check out. A wrong proof would be a
// check dropped, so the facts here only narrow, a call or a task switch forgets what it could
// have changed, and the PINAX_CHECK_PROOFS build stops the program when a proven check fires.
//
// Facts: a register's value lies in [lo, hi]; a register passed the index check of a global (it
// is below the length as it was then); a register passed the row check of a table (and, for a
// pool, the generation). A place keeps at most FACTS_MAX of them, and only the places where
// ways join (the first instruction of a block) keep them between visits; inside a block they
// are carried along. The worklist is over blocks; at a join a fact that is not on both ways is
// dropped and a range only widens, twice at most per end before it becomes the whole int, so
// every block is visited a bounded number of times. The judging pass then reads the proofs off
// the settled facts.

#define OP_A(w) (((w) >> 8) & 0xffu)
#define OP_B(w) (((w) >> 16) & 0xffu)
#define OP_C(w) ((w) >> 24)
#define OP_BX(w) ((w) >> 16)
#define OP_SBX(w) ((int32_t)(int16_t)((w) >> 16))
#define OP_SB(w) ((int32_t)(int8_t)(((w) >> 16) & 0xffu))
#define OP_SC(w) ((int32_t)(int8_t)((w) >> 24))
#define OP_SAX(w) ((int32_t)(w) >> 8)

enum { FACT_RANGE = 1, FACT_INDEX, FACT_ROW };
typedef struct fact {
    uint8_t  reg, kind, grew_lo, grew_hi;
    uint16_t global; // FACT_INDEX, FACT_ROW
    int64_t  lo, hi; // FACT_RANGE
} fact;
enum { FACTS_MAX = 16 };
typedef struct facts {
    uint32_t n;
    fact     f[FACTS_MAX];
} facts;

static fact *fact_find(facts *fs, uint32_t reg, uint32_t kind, uint32_t global) {
    for (uint32_t k = 0; k < fs->n; ++k) {
        fact *f = &fs->f[k];
        if (f->reg == reg && f->kind == kind && (kind == FACT_RANGE || f->global == global))
            return f;
    }
    return NULL;
}
static void fact_remove(facts *fs, uint32_t k) {
    for (; k + 1 < fs->n; ++k)
        fs->f[k] = fs->f[k + 1];
    --fs->n;
}
static void facts_drop_reg(facts *fs, uint32_t reg) { // the register is written: nothing is known of it
    for (uint32_t k = 0; k < fs->n;) {
        if (fs->f[k].reg == reg)
            fact_remove(fs, k);
        else
            ++k;
    }
}
static void facts_drop_from(facts *fs, uint32_t reg) { // a callee's window lay over these registers
    for (uint32_t k = 0; k < fs->n;) {
        if (fs->f[k].reg >= reg)
            fact_remove(fs, k);
        else
            ++k;
    }
}
static void facts_drop_passed(facts *fs) { // a collection may have changed, or another task ran
    for (uint32_t k = 0; k < fs->n;) {
        if (fs->f[k].kind != FACT_RANGE)
            fact_remove(fs, k);
        else
            ++k;
    }
}
static void fact_put(facts *fs, fact f) {
    fact *old = fact_find(fs, f.reg, f.kind, f.global);
    if (old) {
        *old = f;
        return;
    }
    if (fs->n == FACTS_MAX) // the oldest goes: a proof lost, never one gained
        fact_remove(fs, 0);
    fs->f[fs->n++] = f;
}
// The range of a register: the whole int when nothing is known (then false).
static bool range_of(facts *fs, uint32_t reg, int64_t *lo, int64_t *hi) {
    const fact *f = fact_find(fs, reg, FACT_RANGE, 0);
    *lo = f ? f->lo : INT64_MIN;
    *hi = f ? f->hi : INT64_MAX;
    return f != NULL;
}
static void put_range(facts *fs, uint32_t reg, int64_t lo, int64_t hi) {
    if (lo == INT64_MIN && hi == INT64_MAX) {
        fact *f = fact_find(fs, reg, FACT_RANGE, 0);
        if (f)
            fact_remove(fs, (uint32_t)(f - fs->f));
        return;
    }
    fact_put(fs, (fact){(uint8_t)reg, FACT_RANGE, 0, 0, 0, lo, hi});
}
// What a way says of a register: its range meets [lo, hi]. A way that cannot be taken (the meet
// is empty) is left as it is.
static void narrow(facts *fs, uint32_t reg, int64_t lo, int64_t hi) {
    int64_t a, b;
    (void)range_of(fs, reg, &a, &b);
    a = a > lo ? a : lo;
    b = b < hi ? b : hi;
    if (a <= b)
        put_range(fs, reg, a, b);
}
// What holds on both ways into a place: a fact that one way lacks goes, a range widens. True
// when something changed.
static bool facts_meet(facts *into, facts *from) {
    bool changed = false;
    for (uint32_t k = 0; k < into->n;) {
        fact       *a = &into->f[k];
        const fact *b = fact_find(from, a->reg, a->kind, a->global);
        if (!b) {
            fact_remove(into, k);
            changed = true;
            continue;
        }
        if (a->kind == FACT_RANGE) {
            if (b->lo < a->lo) {
                a->lo = ++a->grew_lo >= 2 ? INT64_MIN : b->lo;
                changed = true;
            }
            if (b->hi > a->hi) {
                a->hi = ++a->grew_hi >= 2 ? INT64_MAX : b->hi;
                changed = true;
            }
            if (a->lo == INT64_MIN && a->hi == INT64_MAX) {
                fact_remove(into, k);
                continue;
            }
        }
        ++k;
    }
    return changed;
}

// [lo, hi] of a sum, difference or product of two ranges, clamped to the int; `over` is set
// when a clamp happened (the operation can overflow for some values in the ranges).
static int64_t clamp_add(int64_t a, int64_t b, bool *over) {
    int64_t r;
    if (!ckd_add(&r, a, b))
        return r;
    *over = true;
    return b > 0 ? INT64_MAX : INT64_MIN;
}
static int64_t clamp_sub(int64_t a, int64_t b, bool *over) {
    int64_t r;
    if (!ckd_sub(&r, a, b))
        return r;
    *over = true;
    return b < 0 ? INT64_MAX : INT64_MIN;
}
static void range_mul(int64_t alo, int64_t ahi, int64_t blo, int64_t bhi, int64_t *lo, int64_t *hi, bool *over) {
    const int64_t xs[4][2] = {{alo, blo}, {alo, bhi}, {ahi, blo}, {ahi, bhi}};
    *lo = INT64_MAX;
    *hi = INT64_MIN;
    for (unsigned k = 0; k < 4; ++k) {
        int64_t r;
        if (ckd_mul(&r, xs[k][0], xs[k][1])) {
            *over = true;
            *lo = INT64_MIN;
            *hi = INT64_MAX;
            return;
        }
        *lo = r < *lo ? r : *lo;
        *hi = r > *hi ? r : *hi;
    }
}

// The instructions whose run-time check the pass can prove away.
bool pnx_checked_op(uint32_t op) {
    switch (op) {
    case PNX_AGET: case PNX_ASET: case PNX_ROW: case PNX_AGETW: case PNX_ASETW: case PNX_ROWW:
    case PNX_FGET: case PNX_FSET: case PNX_FGETS: case PNX_FSETS:
    case PNX_ADD: case PNX_SUB: case PNX_MUL: case PNX_NEG: case PNX_ADDI: case PNX_MULI: return true;
    default: return false;
    }
}

const char *pnx_proven_text(uint8_t reason) {
    switch (reason) {
    case PNX_PROVEN_INDEX: return "the index lies below the capacity of the array";
    case PNX_PROVEN_INDEX_AGAIN: return "the index passed this collection's check before";
    case PNX_PROVEN_ROW: return "the row lies within the table";
    case PNX_PROVEN_ROW_AGAIN: return "the row passed this table's check before";
    case PNX_PROVEN_NO_OVERFLOW: return "the operands are in ranges whose result fits";
    default: return "";
    }
}

// What one instruction does to the facts. On return `now` holds on the way that falls through
// (the success way of an instruction that can fail), `alt` on the other way: where a branch
// jumps, or the failure word. `target` is where a jump goes, -1 when it does not jump. In the
// judging pass the proof found is written to st->proven.
static void prove_step(pinax_state *st, span range, uint32_t i, facts *now, facts *alt, int64_t *target, bool *falls,
                       bool *fails, bool judge) {
    const uint32_t  *code = st->code;
    const uint32_t   w = code[i], op = w & 0xffu;
    const pnx_opinfo info = opinfo[op];
    const uint32_t   a = OP_A(w), b = OP_B(w), c = OP_C(w), x = info.second ? code[i + 1] : 0;
    uint8_t          proof = PNX_PROVEN_NONE;
    *target = -1;
    *falls = true;
    *fails = info.fails;
    *alt = *now;

    // the global an instruction names (not one a GSEL chose: of that nothing is known here)
    uint32_t gi = UINT32_MAX;
    switch (op) {
    case PNX_AGET: case PNX_ASET: case PNX_ROW: gi = c; break;
    case PNX_AGETW: case PNX_ASETW: case PNX_ROWW: gi = x; break;
    case PNX_FGET: case PNX_FSET: case PNX_FGETS: case PNX_FSETS: gi = x & 0xfffu; break;
    case PNX_GLEN: case PNX_GCAP: gi = OP_BX(w); break;
    default: break;
    }
    const pnx_global *g = gi < st->head->n_globals ? &st->globals[gi] : NULL;
    // an array is always full, a table too; a shared one reads as empty until it is given
    const bool full = g && !g->shared && (g->kind == PNX_G_ARRAY || g->kind == PNX_G_TABLE);
    int64_t lo, hi;

    switch (op) {
    case PNX_MOVI: {
        if (a != b) {
            facts copy = *now;
            facts_drop_reg(now, a);
            for (uint32_t k = 0; k < copy.n; ++k) {
                if (copy.f[k].reg == b) {
                    fact f = copy.f[k];
                    f.reg = (uint8_t)a;
                    fact_put(now, f);
                }
            }
        }
        break;
    }
    case PNX_LOADI:
        facts_drop_reg(now, a);
        put_range(now, a, OP_SBX(w), OP_SBX(w));
        break;
    case PNX_LOADK:
        facts_drop_reg(now, a);
        put_range(now, a, st->iconst[OP_BX(w)], st->iconst[OP_BX(w)]);
        break;
    case PNX_ADD: case PNX_SUB: case PNX_MUL: case PNX_ADDI: case PNX_MULI: case PNX_NEG: {
        int64_t blo, bhi, clo, chi, rlo, rhi;
        bool    over = false;
        const bool bk = range_of(now, b, &blo, &bhi);
        bool       ck;
        if (op == PNX_ADDI || op == PNX_MULI) {
            clo = chi = OP_SC(w);
            ck = true;
        } else if (op == PNX_NEG) {
            clo = chi = 0;
            ck = true;
        } else {
            ck = range_of(now, c, &clo, &chi);
        }
        if (op == PNX_ADD || op == PNX_ADDI) {
            rlo = clamp_add(blo, clo, &over);
            rhi = clamp_add(bhi, chi, &over);
        } else if (op == PNX_SUB) {
            rlo = clamp_sub(blo, chi, &over);
            rhi = clamp_sub(bhi, clo, &over);
        } else if (op == PNX_NEG) { // 0 - b
            rlo = clamp_sub(0, bhi, &over);
            rhi = clamp_sub(0, blo, &over);
        } else {
            range_mul(blo, bhi, clo, chi, &rlo, &rhi, &over);
        }
        if (bk && ck && !over)
            proof = PNX_PROVEN_NO_OVERFLOW;
        facts_drop_reg(now, a); // on the success way the result is in the clamped range
        put_range(now, a, rlo, rhi);
        break;
    }
    case PNX_GLEN: case PNX_GCAP:
        facts_drop_reg(now, a);
        if (g && !g->shared)
            put_range(now, a, op == PNX_GCAP || full ? g->capacity : 0, g->capacity);
        break;
    case PNX_AGET: case PNX_ASET: case PNX_ROW: case PNX_AGETW: case PNX_ASETW: case PNX_ROWW: {
        const bool writes = op != PNX_ASET && op != PNX_ASETW;
        if (g) {
            (void)range_of(now, b, &lo, &hi);
            if (full && lo >= 0 && hi < (int64_t)g->capacity)
                proof = PNX_PROVEN_INDEX;
            else if (fact_find(now, b, FACT_INDEX, gi))
                proof = PNX_PROVEN_INDEX_AGAIN;
            narrow(now, b, 0, (int64_t)g->capacity - 1); // on the success way: below the length
            fact_put(now, (fact){(uint8_t)b, FACT_INDEX, 0, 0, (uint16_t)gi, 0, 0});
        }
        if (writes) {
            const bool known = g && range_of(now, b, &lo, &hi);
            facts_drop_reg(now, a);
            if (op == PNX_ROW || op == PNX_ROWW) { // the row is the index plus one
                if (g) {
                    if (known)
                        put_range(now, a, lo + 1, hi + 1);
                    fact_put(now, (fact){(uint8_t)a, FACT_ROW, 0, 0, (uint16_t)gi, 0, 0});
                }
            }
        }
        break;
    }
    case PNX_FGET: case PNX_FSET: case PNX_FGETS: case PNX_FSETS: {
        if (g) {
            if (fact_find(now, b, FACT_ROW, gi)) {
                proof = PNX_PROVEN_ROW_AGAIN;
            } else if (full && range_of(now, b, &lo, &hi) && lo >= 1 && hi <= (int64_t)g->capacity) {
                proof = PNX_PROVEN_ROW;
            }
            fact_put(now, (fact){(uint8_t)b, FACT_ROW, 0, 0, (uint16_t)gi, 0, 0});
            if (g->kind != PNX_G_POOL) // a pool's row carries its generation above the slot
                narrow(now, b, 1, (int64_t)g->capacity);
        }
        if (op == PNX_FGET)
            facts_drop_reg(now, a);
        break;
    }
    case PNX_JEQ: case PNX_JNE: case PNX_JLT: case PNX_JLE: case PNX_JEQI: case PNX_JNEI: case PNX_JLTI: case PNX_JGEI: {
        *target = (int64_t)i + 1 + (int8_t)c;
        const bool imm = info.b == PNX_K_IMM;
        int64_t alo, ahi, blo, bhi;
        (void)range_of(now, a, &alo, &ahi);
        if (imm)
            blo = bhi = OP_SB(w);
        else
            (void)range_of(now, b, &blo, &bhi);
        // `alt` is the way the jump takes, `now` the way that falls through
        const bool lt = op == PNX_JLT || op == PNX_JLTI, le = op == PNX_JLE;
        const bool eq = op == PNX_JEQ || op == PNX_JEQI, ne = op == PNX_JNE || op == PNX_JNEI, ge = op == PNX_JGEI;
        facts *below = lt || le ? alt : ge ? now : NULL; // where a < b (lt), a <= b (le), a < b (ge falls)
        facts *above = lt || le ? now : ge ? alt : NULL; // where a >= b (lt), a > b (le), a >= b (ge jumps)
        const bool strict_below = lt || ge, strict_above = le; // a < b (lt, ge falls); a > b (le falls)
        if (below) {
            if (bhi > INT64_MIN || !strict_below)
                narrow(below, a, INT64_MIN, strict_below ? bhi - 1 : bhi);
            if (!imm && (alo < INT64_MAX || !strict_below))
                narrow(below, b, strict_below ? alo + 1 : alo, INT64_MAX);
        }
        if (above) {
            if (blo < INT64_MAX || !strict_above)
                narrow(above, a, strict_above ? blo + 1 : blo, INT64_MAX);
            if (!imm && (ahi > INT64_MIN || !strict_above))
                narrow(above, b, INT64_MIN, strict_above ? ahi - 1 : ahi);
        }
        facts *same = eq ? alt : ne ? now : NULL;
        if (same) {
            narrow(same, a, blo, bhi);
            if (!imm)
                narrow(same, b, alo, ahi);
        }
        break;
    }
    case PNX_JMP:
        *target = (int64_t)i + 1 + OP_SAX(w);
        *falls = false;
        break;
    case PNX_RET: case PNX_RETOK: case PNX_RETF: case PNX_FAIL: case PNX_FAILR: case PNX_FAULT: case PNX_TCALL:
        *falls = false;
        break;
    case PNX_CALL: case PNX_CALLR: case PNX_HCALL: case PNX_HCALLF: // the callee may change any collection
        facts_drop_passed(now);
        facts_drop_from(now, a);
        *alt = *now;
        break;
    case PNX_CHECK: case PNX_TYIELD: // another task, or the host between two runs, may change a collection
        facts_drop_passed(now);
        *alt = *now;
        break;
    default: {
        const uint32_t reg[3] = {a, b, c};
        const uint8_t  kind[3] = {info.a, info.b, info.c};
        for (unsigned k = 0; k < 3; ++k) {
            if (kind[k] == PNX_K_I && (info.roles[k] == 'w' || info.roles[k] == 'm'))
                facts_drop_reg(now, reg[k]);
        }
        // one that names a global and may change it (or a task), or one this pass does not know
        if (info.shape == PNX_F_GLOBAL || info.a == PNX_K_G || info.b == PNX_K_G || info.c == PNX_K_G ||
            info.second == PNX_X_GLOBAL || info.second == PNX_X_FIELD || info.second == PNX_X_WORD ||
            info.second == PNX_X_IMPORT || info.second == PNX_X_COUNTS)
            facts_drop_passed(now);
        break;
    }
    }
    if (*target >= 0 && *target < range.begin) // the shape check allows no such jump; keep the index sound
        *target = -1;
    if (judge && pnx_checked_op(op)) {
        st->proven[i] = proof;
        if (info.second) // marked as the second word, so a fault's pc finds the instruction
            st->proven[i + 1] = proof == PNX_PROVEN_NONE ? PNX_PROVEN_NONE : (uint8_t)(proof | 0x80u);
    }
}

// What holds on either of two ways into a place, without the widening of facts_meet: the
// narrowing rounds of prove_word use it, where ranges may shrink again.
static void facts_join(facts *into, facts *from) {
    for (uint32_t k = 0; k < into->n;) {
        fact       *a = &into->f[k];
        const fact *b = fact_find(from, a->reg, a->kind, a->global);
        if (!b) {
            fact_remove(into, k);
            continue;
        }
        if (a->kind == FACT_RANGE) {
            a->lo = b->lo < a->lo ? b->lo : a->lo;
            a->hi = b->hi > a->hi ? b->hi : a->hi;
            if (a->lo == INT64_MIN && a->hi == INT64_MAX) {
                fact_remove(into, k);
                continue;
            }
        }
        ++k;
    }
}

// The facts of block `b` carried through its instructions. What leaves it goes out along at
// most two edges: `to` the blocks, `out` the facts on each. In the judging pass the proofs are
// written instead and no edge is reported.
typedef struct edges {
    uint32_t n, to[2];
    facts    out[2];
} edges;
static void prove_block(pinax_state *st, span range, const uint8_t *leader, const uint32_t *block_of,
                        const uint32_t *start_of, const facts *in, uint32_t b, bool judge, edges *e) {
    const uint32_t *code = st->code;
    facts           now = in[b], alt;
    uint32_t        i = start_of[b];
    e->n = 0;
    for (;;) {
        const uint32_t next = i + (opinfo[code[i] & 0xffu].second ? 2u : 1u);
        int64_t        target;
        bool           falls, fails;
        prove_step(st, range, i, &now, &alt, &target, &falls, &fails, judge);
        const bool last = next >= range.end || leader[next - range.begin] || target >= 0 || !falls || fails;
        if (!last) {
            i = next;
            continue;
        }
        if (judge)
            return;
#define EDGE(at, from)                                                                             \
        do {                                                                                       \
            e->to[e->n] = block_of[(uint32_t)(at) - range.begin];                                  \
            e->out[e->n++] = *(from);                                                              \
        } while (0)
        if (target >= 0)
            EDGE(target, &alt); // a branch: the way it takes; a JMP: `alt` is `now` (nothing was written)
        if (falls && fails) {
            EDGE(next, &alt); // the failure word, with what held before
            if (next + 1 < range.end)
                EDGE(next + 1, &now);
        } else if (falls && next < range.end) {
            EDGE(next, &now);
        }
#undef EDGE
        return;
    }
}

// The proofs of one word. Blocks: the first instruction of the word, every place a jump goes
// to, the instruction after a jump, after one that does not go on and after one that can fail
// (its failure word), and the one after that. Rounds: first the worklist with widening until
// nothing changes (a range at a loop head grows to the whole int after two growths), then two
// rounds that take each block's facts again as what its ways in bring, without widening, so
// that a counter the loop's own test bounds gets its bound back; both keep every value above
// the true one. Then the judging pass. No room for the scratch means no proofs, never a refusal.
static void prove_word(pinax_state *st, span range) {
    const uint32_t *code = st->code;
    const uint32_t  n = range.end - range.begin;
    const size_t    mark = st->used;
    uint8_t        *leader = take(st, n, 1);
    uint32_t       *block_of = leader ? take(st, (size_t)n * 4, 4) : NULL;
    if (!block_of) {
        give_back(st, mark);
        return;
    }
    leader[0] = 1;
    for (uint32_t i = range.begin; i < range.end;) {
        const uint32_t   w = code[i], op = w & 0xffu;
        const pnx_opinfo info = opinfo[op];
        const uint32_t   next = i + (info.second ? 2u : 1u);
        int64_t          target = -1;
        bool             falls = true;
        switch (op) {
        case PNX_JMP: target = (int64_t)i + 1 + OP_SAX(w); falls = false; break;
        case PNX_RET: case PNX_RETOK: case PNX_RETF: case PNX_FAIL: case PNX_FAILR: case PNX_FAULT: case PNX_TCALL:
            falls = false; break;
        default:
            if (info.shape == PNX_F_ABC && info.c == PNX_K_J)
                target = (int64_t)i + 1 + (int8_t)(w >> 24);
            break;
        }
        if (target >= range.begin && target < range.end)
            leader[target - range.begin] = 1;
        if ((target >= 0 || !falls || info.fails) && next < range.end)
            leader[next - range.begin] = 1;
        if (info.fails && next + 1 < range.end)
            leader[next + 1 - range.begin] = 1;
        i = next;
    }
    uint32_t n_blocks = 0;
    for (uint32_t i = range.begin; i < range.end;) {
        if (leader[i - range.begin])
            ++n_blocks;
        block_of[i - range.begin] = n_blocks - 1;
        i += opinfo[code[i] & 0xffu].second ? 2u : 1u;
    }
    uint32_t *start_of = take(st, (size_t)n_blocks * 4, 4);
    facts    *fs = start_of ? take(st, (size_t)n_blocks * sizeof(facts), 8) : NULL;
    uint32_t *queue = fs ? take(st, (size_t)n_blocks * 4, 4) : NULL;
    uint8_t  *queued = queue ? take(st, n_blocks, 1) : NULL;
    uint8_t  *seen = queued ? take(st, n_blocks, 1) : NULL;
    uint32_t *pred_at = seen ? take(st, ((size_t)n_blocks + 1) * 4, 4) : NULL; // the ways into each block
    uint32_t *preds = pred_at ? take(st, (size_t)n_blocks * 2 * 4, 4) : NULL;
    if (!preds) {
        give_back(st, mark);
        return;
    }
    for (uint32_t i = range.begin, b = 0; i < range.end;) {
        if (leader[i - range.begin])
            start_of[b++] = i;
        i += opinfo[code[i] & 0xffu].second ? 2u : 1u;
    }
    edges    e;
    uint32_t n_queue = 0;
    seen[0] = queued[0] = 1;
    queue[n_queue++] = 0;
    while (n_queue) {
        const uint32_t b = queue[--n_queue];
        queued[b] = 0;
        prove_block(st, range, leader, block_of, start_of, fs, b, false, &e);
        for (uint32_t k = 0; k < e.n; ++k) {
            const uint32_t to = e.to[k];
            if (!seen[to]) {
                fs[to] = e.out[k];
                seen[to] = 1;
            } else if (!facts_meet(&fs[to], &e.out[k])) {
                continue;
            }
            if (!queued[to]) {
                queued[to] = 1;
                queue[n_queue++] = to;
            }
        }
    }
    // the narrowing rounds: each block's facts become what its ways in bring now, in order, so
    // that a bound found at a loop's test reaches the head in the same round
    __builtin_memset(pred_at, 0, ((size_t)n_blocks + 1) * 4);
    for (uint32_t b = 0; b < n_blocks; ++b) {
        if (!seen[b])
            continue;
        prove_block(st, range, leader, block_of, start_of, fs, b, false, &e);
        for (uint32_t k = 0; k < e.n; ++k)
            ++pred_at[e.to[k] + 1];
    }
    for (uint32_t b = 0; b < n_blocks; ++b)
        pred_at[b + 1] += pred_at[b];
    for (uint32_t b = 0; b < n_blocks; ++b) {
        if (!seen[b])
            continue;
        prove_block(st, range, leader, block_of, start_of, fs, b, false, &e);
        for (uint32_t k = 0; k < e.n; ++k)
            preds[pred_at[e.to[k]]++] = b;
    }
    for (uint32_t b = n_blocks; b > 0; --b)
        pred_at[b] = pred_at[b - 1];
    pred_at[0] = 0;
    for (unsigned round = 0; round < 4; ++round) {
        bool changed = false;
        for (uint32_t b = 1; b < n_blocks; ++b) { // the first block keeps its empty facts
            if (!seen[b])
                continue;
            facts now;
            bool  first = true;
            for (uint32_t k = pred_at[b]; k < pred_at[b + 1]; ++k) {
                prove_block(st, range, leader, block_of, start_of, fs, preds[k], false, &e);
                for (uint32_t j = 0; j < e.n; ++j) {
                    if (e.to[j] != b)
                        continue;
                    if (first)
                        now = e.out[j];
                    else
                        facts_join(&now, &e.out[j]);
                    first = false;
                }
            }
            if (!first && __builtin_memcmp(&now, &fs[b], sizeof now) != 0) {
                fs[b] = now;
                changed = true;
            }
        }
        if (!changed)
            break;
    }
    for (uint32_t b = 0; b < n_blocks; ++b) {
        if (seen[b])
            prove_block(st, range, leader, block_of, start_of, fs, b, true, &e);
    }
    give_back(st, mark);
}

// For `pnx see --checks`: a line per word (its checks and how many are proven), then a line per
// proven instruction with its source line, the instruction and the reason.
void pnx_checks_report(const pinax_state *st, void (*put)(void *user, const char *text, size_t len), void *user) {
    static const char *const names[256] = {
#define PNX_NAME(name, number, a, b, c, shape, second, fails, roles) [number] = #name,
        PNX_OPS(PNX_NAME)
#undef PNX_NAME
    };
    if (!st->head)
        return;
    uint32_t line_k = 0, line = 0;
    for (uint32_t k = 0; k < st->head->n_words; ++k) {
        const span range = {st->words[k].entry, k + 1 < st->head->n_words ? st->words[k + 1].entry : st->head->n_code};
        uint32_t   checks = 0, proven = 0;
        for (uint32_t i = range.begin; i < range.end; i += opinfo[st->code[i] & 0xffu].second ? 2u : 1u) {
            checks += pnx_checked_op(st->code[i] & 0xffu);
            proven += st->proven[i] != PNX_PROVEN_NONE;
        }
        char    text[256];
        uint8_t digits[24];
        size_t  n = 0;
#define PUT_TEXT(s, len)                                                                           \
        do {                                                                                       \
            for (size_t put_k = 0; put_k < (size_t)(len) && n < sizeof text; ++put_k)              \
                text[n++] = (s)[put_k];                                                            \
        } while (0)
#define PUT_INT(v)                                                                                 \
        do {                                                                                       \
            const uint32_t count = pnx_format_int((int64_t)(v), digits);                           \
            PUT_TEXT(digits, count);                                                               \
        } while (0)
        PUT_TEXT("word ", 5);
        PUT_TEXT(st->words[k].name, __builtin_strlen(st->words[k].name));
        PUT_TEXT(": ", 2);
        PUT_INT(checks);
        PUT_TEXT(checks == 1 ? " check, " : " checks, ", checks == 1 ? 8 : 9);
        PUT_INT(proven);
        PUT_TEXT(" proven\n", 8);
        put(user, text, n);
        for (uint32_t i = range.begin; i < range.end; i += opinfo[st->code[i] & 0xffu].second ? 2u : 1u) {
            for (; line_k < st->head->n_lines; ++line_k) { // the line records rise with the pc
                pnx_linerec rec;
                __builtin_memcpy(&rec, st->lines + (size_t)line_k * sizeof rec, sizeof rec);
                if (rec.pc > i)
                    break;
                line = rec.line;
            }
            if (st->proven[i] == PNX_PROVEN_NONE)
                continue;
            n = 0;
            PUT_TEXT("  line ", 7);
            PUT_INT(line);
            PUT_TEXT(": ", 2);
            PUT_TEXT(names[st->code[i] & 0xffu], __builtin_strlen(names[st->code[i] & 0xffu]));
            PUT_TEXT(": ", 2);
            PUT_TEXT(pnx_proven_text(st->proven[i]), __builtin_strlen(pnx_proven_text(st->proven[i])));
            PUT_TEXT("\n", 1);
            put(user, text, n);
        }
#undef PUT_INT
#undef PUT_TEXT
    }
}

pinax_err pinax_load_bytecode(pinax_state *st, pinax_str bytecode) {
    if (!st || !bytecode.ptr)
        return PINAX_ERR_ARGUMENT;
    if (st->head)
        return fail(st, PINAX_ERR_STATE, "a state takes one bytecode container; sources can be added with pinax_load");
    return pnx_load_container(st, bytecode, 0);
}

// the records after the head are read in place: the head keeps them 8-byte aligned
static_assert(sizeof(pnx_head) % 8 == 0 && sizeof(pnx_localrec) % 8 == 0, "container records lose their alignment");

// A shared global the host has not given (or no longer gives, after an image is opened): it
// reads as empty and has no storage. The few bytes a scalar or a buffer reads are zeros here.
static const uint64_t no_storage[2];
static void unshared(pnx_global *g) {
    g->ptr = g->own = (uint8_t *)(uintptr_t)no_storage; // read only: nothing writes a shared global
    g->length = 0;
    if (g->kind == PNX_G_INDEX)
        g->capacity = 1; // one empty slot: every key is not found
}

// The bytes of a container whose head this is: the head, then every record and segment in order.
static uint64_t container_bytes(const pnx_head *h) {
    return sizeof *h + (uint64_t)h->n_words * sizeof(pnx_wordrec) + (uint64_t)h->n_globals * sizeof(pnx_globalrec) +
           (uint64_t)h->n_imports * sizeof(pnx_importrec) + (uint64_t)h->n_iconst * 8 +
           (uint64_t)h->n_sconst * sizeof(pnx_sconst) + (uint64_t)h->n_code * 4 + h->n_strbytes +
           (uint64_t)h->n_lines * sizeof(pnx_linerec) + (uint64_t)h->n_locals * sizeof(pnx_localrec);
}

// The bytes of storage a global record asks for (the loader checks the record first).
static uint64_t storage_bytes(const pnx_globalrec *rec) {
    switch (rec->kind) {
    case PNX_G_INT: return 8;
    case PNX_G_ARRAY:
    case PNX_G_LIST:
    case PNX_G_QUEUE: return (uint64_t)rec->capacity * 8;
    case PNX_G_BUFFER: return (uint64_t)rec->capacity + 8;
    case PNX_G_TABLE:
    case PNX_G_ROWS: return (uint64_t)rec->capacity * rec->elem_bytes;
    case PNX_G_POOL: return (uint64_t)rec->capacity * ((uint64_t)rec->elem_bytes + 8);
    case PNX_G_INDEX: return (uint64_t)rec->capacity * 4;
    case PNX_G_TASKS: // elem_bytes: the call depth of a task; reserved: the bytes of its region
        return (uint64_t)rec->capacity *
               (sizeof(pnx_task) + (uint64_t)rec->elem_bytes * (PNX_TASK_INTS * 8 + PNX_TASK_STRS * sizeof(pnx_scell) +
                                                                 sizeof(pnx_frame)) +
                ((rec->reserved + 15u) & ~15u));
    default: return 0;
    }
}

pinax_err pnx_load_container(pinax_state *st, pinax_str bytecode, uint32_t keep) {
    const pnx_program before = pnx_program_now(st);
    pnx_head h;
    if (bytecode.len < sizeof h)
        return fail(st, PINAX_ERR_FORMAT, "bytecode: too short");
    __builtin_memcpy(&h, bytecode.ptr, sizeof h);
    if (__builtin_memcmp(h.magic, PNX_MAGIC, 4) != 0 || h.version != PNX_CONTAINER_VERSION ||
        h.order != PNX_ORDER_MARK || h.reserved != 0)
        return fail(st, PINAX_ERR_FORMAT, "bytecode: not a container of this version and byte order");
    const uint64_t total = container_bytes(&h);
    if (total != bytecode.len || h.n_globals > 0xfffu || h.n_code > st->region_len)
        return fail(st, PINAX_ERR_FORMAT, "bytecode: sizes do not add up or exceed the P0 limits");

    const size_t mark = st->used;
    pinax_err err = PINAX_ERR_NO_ROOM;
    const char *why = "block too small for the code and its globals";
    uint8_t *copy = take(st, bytecode.len, 8);
    if (!copy)
        goto rollback;
    __builtin_memcpy(copy, bytecode.ptr, bytecode.len);
    st->head = (const pnx_head *)(const void *)copy;
    st->words = (const pnx_wordrec *)(const void *)(copy + sizeof h);
    const pnx_globalrec *globalrecs = (const pnx_globalrec *)(const void *)(st->words + h.n_words);
    st->importrecs = (const pnx_importrec *)(const void *)(globalrecs + h.n_globals);
    st->iconst = (const int64_t *)(const void *)(st->importrecs + h.n_imports);
    st->sconst = (const pnx_sconst *)(const void *)(st->iconst + h.n_iconst);
    st->code = (const uint32_t *)(const void *)(st->sconst + h.n_sconst);
    st->segment[PNX_SEG_CONST] = (const uint8_t *)(st->code + h.n_code);

    st->word_counts = take(st, sizeof(pnx_counts) * (h.n_words + 1u), 8);
    st->proven = st->word_counts ? take(st, h.n_code + 1u, 1) : NULL; // zero: nothing proven yet
    st->imports = st->proven ? take(st, sizeof(pnx_host *) * (h.n_imports + 1u), 8) : NULL;
    st->globals = st->imports ? take(st, sizeof(pnx_global) * (h.n_globals + 1u), 8) : NULL;
    if (!st->globals)
        goto rollback;

    err = PINAX_ERR_FORMAT;
    why = "bytecode: bad word record";
    for (uint32_t k = 0; k < h.n_words; ++k) {
        const pnx_wordrec *word = &st->words[k];
        if (word->counted)
            st->word_counts[k] = (pnx_counts){word->int_args, word->str_args, word->int_results, word->str_results,
                                              word->fails != 0, false};
        if (!terminated(word->name, sizeof word->name) || !terminated(word->signature, sizeof word->signature) ||
            (!word->counted && !pnx_signature_counts(word->signature, &st->word_counts[k])) ||
            st->word_counts[k].str_args > PNX_MAX_STR_ARGS || st->word_counts[k].str_results > PNX_MAX_STR_RESULTS ||
            word->entry >= h.n_code ||
            (k > 0 && word->entry <= st->words[k - 1].entry) || (k == 0 && word->entry != 0) ||
            word->int_cells > 256 || word->str_cells > 256)
            goto rollback;
    }
    st->lines = st->segment[PNX_SEG_CONST] + h.n_strbytes;
    why = "bytecode: bad line record";
    for (uint32_t k = 0, before = 0; k < h.n_lines; ++k) {
        pnx_linerec rec;
        __builtin_memcpy(&rec, st->lines + (size_t)k * sizeof rec, sizeof rec);
        if (rec.pc >= h.n_code || (k && rec.pc <= before))
            goto rollback;
        before = rec.pc;
    }
    st->localrecs = st->lines + (size_t)h.n_lines * sizeof(pnx_linerec);
    why = "bytecode: bad local record";
    for (uint32_t k = 0; k < h.n_locals; ++k) {
        pnx_localrec rec;
        __builtin_memcpy(&rec, st->localrecs + (size_t)k * sizeof rec, sizeof rec);
        if (rec.word >= h.n_words)
            goto rollback;
        const pnx_wordrec *word = &st->words[rec.word];
        const uint32_t length = (rec.word + 1 < h.n_words ? st->words[rec.word + 1].entry : h.n_code) - word->entry;
        if (rec.from >= rec.to || rec.to > length || rec.bank > 1 || rec.reg >= (rec.bank ? word->str_cells : word->int_cells) ||
            rec.kind > PINAX_VALUE_ERR || (rec.bank == 1) != (rec.kind == PINAX_VALUE_STR) || !rec.name[0] ||
            !terminated(rec.name, sizeof rec.name) || !terminated(rec.type, sizeof rec.type))
            goto rollback;
    }
    why = "bytecode: bad string constant";
    for (uint32_t k = 0; k < h.n_sconst; ++k)
        if ((uint64_t)st->sconst[k].offset + st->sconst[k].length > h.n_strbytes)
            goto rollback;

    for (uint32_t k = 0; k < h.n_imports; ++k) {
        const pnx_importrec *rec = &st->importrecs[k];
        err = PINAX_ERR_FORMAT;
        why = "bytecode: bad import record";
        if (!terminated(rec->module, sizeof rec->module) || !terminated(rec->name, sizeof rec->name) ||
            !terminated(rec->signature, sizeof rec->signature))
            goto rollback;
        const pnx_host *found = NULL;
        bool named = false;
        for (uint32_t j = 0; j < st->n_hosts && !found; ++j) {
            if (text_equal(st->hosts[j].module, rec->module) && text_equal(st->hosts[j].name, rec->name)) {
                named = true;
                if (text_equal(st->hosts[j].signature, rec->signature))
                    found = &st->hosts[j];
            }
        }
        if (!found && !named && rec->module[0] == '@') { // a C function named by extern
            uint32_t index;
            err = pnx_extern_link(st, rec->module, rec->name, rec->signature, &index);
            why = st->error_text;
            if (err != PINAX_OK)
                goto rollback;
            found = &st->hosts[index];
        }
        if (!found) {
            err = named ? PINAX_ERR_SIGNATURE : PINAX_ERR_NOT_FOUND;
            why = named ? "a host word has another signature than the code expects"
                        : "the code uses a host word that is not registered";
            goto rollback;
        }
        st->imports[k] = found;
    }

    uint8_t *first_global = NULL;
    for (uint32_t k = 0; k < h.n_globals; ++k) {
        const pnx_globalrec *rec = &globalrecs[k];
        pnx_global *g = &st->globals[k];
        uint64_t bytes;
        err = PINAX_ERR_FORMAT;
        why = "bytecode: bad global record";
        if (!terminated(rec->name, sizeof rec->name) || !terminated(rec->type, sizeof rec->type) || rec->capacity == 0)
            goto rollback;
        switch (rec->kind) {
        case PNX_G_INT:
        case PNX_G_ARRAY:
        case PNX_G_LIST:
        case PNX_G_QUEUE:
        case PNX_G_BUFFER: break;
        case PNX_G_TABLE:
        case PNX_G_ROWS:
        case PNX_G_POOL:
            if (rec->elem_bytes == 0)
                goto rollback;
            break;
        case PNX_G_INDEX: { // slots: a power of two; the key field must lie inside an earlier table
            const uint32_t table = rec->elem_bytes & 0xfffu, offset = (rec->elem_bytes >> 12) & 0xfffu,
                           extra = rec->elem_bytes >> 24;
            const uint32_t is_str = rec->reserved & 1u;
            if ((rec->capacity & (rec->capacity - 1)) != 0 || table >= k || (rec->reserved & ~5u) ||
                (st->globals[table].kind != PNX_G_TABLE && st->globals[table].kind != PNX_G_ROWS) ||
                (!is_str && extra >= PNX_W_COUNT) ||
                offset + (is_str ? 1u + extra : width_bytes[extra]) > st->globals[table].elem_bytes)
                goto rollback;
            break;
        }
        case PNX_G_TASKS: // elem_bytes: the call depth of a task; reserved: the bytes of its region
            if (rec->capacity > 4096 || rec->elem_bytes == 0 || rec->elem_bytes > 4096 || rec->reserved > (1u << 24))
                goto rollback;
            break;
        default: goto rollback;
        }
        bytes = storage_bytes(rec);
        const bool shared = rec->kind == PNX_G_INDEX ? (rec->reserved & 4u) != 0
                          : rec->kind != PNX_G_TASKS && rec->reserved == 4;
        if (shared && (rec->kind == PNX_G_QUEUE || rec->kind == PNX_G_POOL))
            goto rollback;
        if (k < keep) { // a global of the program being extended: its storage and state stay
            if (before.globals[k].kind != rec->kind)
                goto rollback;
            *g = before.globals[k];
            if (!first_global)
                first_global = (uint8_t *)before.seg_global;
            continue;
        }
        err = PINAX_ERR_NO_ROOM;
        why = "block too small for the code and its globals";
        if (bytes > SIZE_MAX / 2)
            goto rollback;
        g->ptr = g->own = shared ? NULL : take(st, (size_t)bytes, 8); // shared: no storage of its own
        if (!g->ptr && !shared)
            goto rollback;
        g->kind = rec->kind;
        g->elem_bytes = rec->kind == PNX_G_TABLE || rec->kind == PNX_G_ROWS || rec->kind == PNX_G_POOL ? rec->elem_bytes : 8;
        g->capacity = rec->kind == PNX_G_INT ? 1 : rec->capacity;
        g->length = rec->kind == PNX_G_LIST || rec->kind == PNX_G_ROWS || rec->kind == PNX_G_QUEUE ? 0 : g->capacity;
        st->qturn += rec->kind == PNX_G_QUEUE; // a new queue: a task waiting for a queue looks again
        if (rec->kind == PNX_G_POOL) { // every slot is free: slot k links to k + 1
            uint8_t *meta = g->ptr + (size_t)g->capacity * g->elem_bytes;
            for (uint32_t slot = 0; slot < g->capacity; ++slot) {
                const uint32_t next = slot + 1 < g->capacity ? slot + 2 : 0;
                __builtin_memcpy(meta + (size_t)slot * 8 + 4, &next, 4);
            }
            g->head = 1;
        }
        if (rec->kind == PNX_G_TASKS) { // every task gets its registers, its control stack and its region
            pnx_task *tasks = (pnx_task *)(void *)g->ptr;
            uint8_t  *room = (uint8_t *)(tasks + g->capacity);
            const uint32_t depth = rec->elem_bytes, region = (rec->reserved + 15u) & ~15u;
            if ((uint64_t)(room - st->region) + bytes > UINT32_MAX) {
                why = "block too large for tasks: their regions must lie within 4 GiB of the main one";
                goto rollback;
            }
            for (uint32_t slot = 0; slot < g->capacity; ++slot) {
                pnx_task *t = &tasks[slot];
                t->int_slots = depth * PNX_TASK_INTS;
                t->str_slots = depth * PNX_TASK_STRS;
                t->depth_max = depth;
                t->ibank = (int64_t *)(void *)room;
                room += (size_t)t->int_slots * 8;
                t->sbank = (pnx_scell *)(void *)room;
                room += (size_t)t->str_slots * sizeof(pnx_scell);
                t->ctrl = (pnx_frame *)(void *)room;
                room += (size_t)depth * sizeof(pnx_frame);
                t->region_base = (uint32_t)(room - st->region);
                t->region_bytes = rec->reserved;
                room += region;
            }
            g->head = g->capacity - 1; // run_all takes the slot after the one it took last: slot 0 first
        }
        if (rec->kind == PNX_G_INDEX) {
            g->key = rec->elem_bytes;
            g->key_is_str = (rec->reserved & 1u) != 0;
            g->elem_bytes = 4;
        }
        g->shared = shared;
        if (shared)
            unshared(g);
        if (!first_global && !shared)
            first_global = g->ptr;
    }
    st->segment[PNX_SEG_GLOBAL] = first_global ? first_global : empty_bytes;

    err = PINAX_ERR_FORMAT;
    __builtin_memset(st->region, 0, h.n_code); // scratch: which code words start an instruction
    for (uint32_t k = 0; k < h.n_words; ++k) {
        span range = {st->words[k].entry, k + 1 < h.n_words ? st->words[k + 1].entry : h.n_code};
        why = check_word(st, k, range, st->region);
        if (why)
            goto rollback;
        // room for the second check: one set of registers for every code word of this word
        const size_t flow_mark = st->used;
        regset *sets = take(st, (size_t)(range.end - range.begin) * (sizeof(regset) + 4 + 1), 8);
        if (!sets) {
            err = PINAX_ERR_NO_ROOM;
            why = "block too small to check the code";
            goto rollback;
        }
        why = check_flow(st, k, range, sets);
        give_back(st, flow_mark);
        if (why)
            goto rollback;
        const size_t needed = st->needed; // RFC-0023: which checks cannot fire; never refuses, so a scratch
        prove_word(st, range);            // that did not fit is not a size the host is told
        st->needed = needed;
    }
    if (h.n_words == 0 && h.n_code != 0) {
        why = "bytecode: code without words";
        goto rollback;
    }
    mark_yields(st);
    st->region_used = 0;
    return PINAX_OK;

rollback:
    (void)mark;
    pnx_program_restore(st, &before);
    return fail(st, err, why);
}

// ---- finding and running -----------------------------------------------------------------

// A task holds places in the code (where it goes on, where its calls return): while one lives,
// the code must not move under it.
// ---- compaction ------------------------------------------------------------------------

// A load that declares takes a fresh copy of the whole program (its container, the loader's
// tables, the symbol tables), and the copy it replaces is left in the block, between the storage
// of the globals declared earlier, which stays where it is (RFC-0024). After such a load the live
// pieces are moved down over the dead ones, in address order, so that the block holds one copy.
// Only when nothing points into the block from a run: no run suspended, no load half done, no
// host word running, no task alive (a task's saved context points into its slot).

typedef struct piece {
    uint8_t  *at;
    size_t    bytes;
    ptrdiff_t by;
} piece;

// Where an address lies after the move: in the piece that holds it, moved with it; or the same
// (an address outside every piece: the host's memory, the fixed front of the block, NULL).
static void *moved_to(const piece *pieces, uint32_t n, const void *p) {
    const uintptr_t a = (uintptr_t)p;
    uint32_t lo = 0, hi = n;
    while (lo < hi) {
        const uint32_t mid = lo + (hi - lo) / 2;
        const uintptr_t from = (uintptr_t)pieces[mid].at;
        if (a < from)
            hi = mid;
        else if (a >= from + pieces[mid].bytes)
            lo = mid + 1;
        else
            return (uint8_t *)p + pieces[mid].by;
    }
    return (void *)p;
}
#define RELOC(field) ((field) = (typeof(field))moved_to(pieces, n, (const void *)(field)))

void pnx_compact(pinax_state *st) {
    if (!st->head || st->suspended || st->loading || st->host_cur || st->nest || st->frozen || pnx_tasks_alive(st))
        return;
    const pnx_head *h = st->head;
    const uint32_t  most = h->n_globals + 6;
    const size_t    mark = st->used;
    piece *pieces = take(st, (size_t)most * sizeof(piece), 8); // the list of live pieces, as scratch above them all
    if (!pieces) {
        give_back(st, mark);
        return;
    }
    uint32_t n = 0;
    pieces[n++] = (piece){(uint8_t *)(uintptr_t)h, (size_t)container_bytes(h), 0};
    pieces[n++] = (piece){(uint8_t *)st->word_counts, sizeof(pnx_counts) * ((size_t)h->n_words + 1u), 0};
    pieces[n++] = (piece){st->proven, (size_t)h->n_code + 1u, 0};
    pieces[n++] = (piece){(uint8_t *)st->imports, sizeof(pnx_host *) * ((size_t)h->n_imports + 1u), 0};
    pieces[n++] = (piece){(uint8_t *)st->globals, sizeof(pnx_global) * ((size_t)h->n_globals + 1u), 0};
    const pnx_globalrec *recs = (const pnx_globalrec *)(const void *)(st->words + h->n_words);
    const uint8_t *const floor = (const uint8_t *)st->handles + sizeof(pnx_handle_slot) * st->handle_slots;
    for (uint32_t k = 0; k < h->n_globals; ++k) {
        const pnx_global *g = &st->globals[k];
        if (!g->shared && g->own >= floor && g->own < st->block + mark)
            pieces[n++] = (piece){g->own, (size_t)storage_bytes(&recs[k]), 0};
    }
    if (st->symbols)
        pieces[n++] = (piece){st->symbols, pnx_symbols_bytes(st->symbols), 0};
    for (uint32_t i = 1; i < n; ++i) { // by address; nearly in order already
        const piece p = pieces[i];
        uint32_t    j = i;
        while (j > 0 && pieces[j - 1].at > p.at)
            pieces[j] = pieces[j - 1], --j;
        pieces[j] = p;
    }
    uint8_t *cursor = (uint8_t *)(uintptr_t)(((uintptr_t)floor + 15u) & ~(uintptr_t)15u);
    for (uint32_t i = 0; i < n; ++i) {
        uint8_t *const dst = (uint8_t *)(uintptr_t)(((uintptr_t)cursor + 15u) & ~(uintptr_t)15u);
        if (dst > pieces[i].at || pieces[i].at + pieces[i].bytes > st->block + mark) { // not the layout this expects: leave the block as it is
            give_back(st, mark);
            return;
        }
        if (dst != pieces[i].at)
            __builtin_memmove(dst, pieces[i].at, pieces[i].bytes);
        pieces[i].by = dst - pieces[i].at;
        cursor = dst + pieces[i].bytes;
    }
    RELOC(st->head), RELOC(st->words), RELOC(st->importrecs), RELOC(st->iconst), RELOC(st->sconst), RELOC(st->code);
    h = st->head; // the head moved with its container; where it was may hold another piece now
    RELOC(st->lines), RELOC(st->localrecs), RELOC(st->segment[PNX_SEG_CONST]), RELOC(st->segment[PNX_SEG_GLOBAL]);
    RELOC(st->word_counts), RELOC(st->proven), RELOC(st->imports), RELOC(st->globals);
    if (st->symbols) {
        const void *from = st->symbols;
        RELOC(st->symbols);
        if (st->symbols != from)
            pnx_symbols_move(st->symbols, from, (const uint8_t *)st->symbols - (const uint8_t *)from);
    }
    for (uint32_t k = 0; k < h->n_globals; ++k) {
        pnx_global *g = &st->globals[k];
        if (g->shared)
            continue;
        const uint8_t *const was = g->own;
        RELOC(g->own);
        RELOC(g->ptr); // the host's memory, when bound, lies in no piece and stays
        const ptrdiff_t by = g->own - was;
        for (uint32_t slot = 0; g->kind == PNX_G_TASKS && slot < g->capacity; ++slot) {
            pnx_task *t = (pnx_task *)(void *)g->own + slot;
            RELOC(t->ibank), RELOC(t->sbank), RELOC(t->ctrl), RELOC(t->parent);
            RELOC(t->ctx.ibank), RELOC(t->ctx.sbank), RELOC(t->ctx.ctrl);
            t->region_base = (uint32_t)((ptrdiff_t)t->region_base + by); // offsets from the region, which stays
            t->ctx.region_used = (uint32_t)((ptrdiff_t)t->ctx.region_used + by);
            t->ctx.region_len = (uint32_t)((ptrdiff_t)t->ctx.region_len + by);
            t->ctx.region_floor = (uint32_t)((ptrdiff_t)t->ctx.region_floor + by);
            t->ptn = (pnx_ptn_found){0};
        }
    }
    for (uint32_t k = 0; k < st->eval_n; ++k)
        RELOC(st->eval_str[k].ptr);
    st->fault_ibank = NULL, st->fault_sbank = NULL; // read right after the fault, as an image does not carry them
    st->ptn = (pnx_ptn_found){0};                   // a match's offsets into a global's storage do not survive the move
    give_back(st, mark);
    st->used = (size_t)(cursor - st->block);
}
#undef RELOC

bool pnx_tasks_alive(const pinax_state *st) {
    for (uint32_t k = 0; st->head && k < st->head->n_globals; ++k) {
        const pnx_global *g = &st->globals[k];
        for (uint32_t slot = 0; g->kind == PNX_G_TASKS && slot < g->capacity; ++slot) {
            if (((const pnx_task *)(const void *)g->ptr)[slot].state != PNX_TASK_FREE)
                return true;
        }
    }
    return false;
}

// Which words may yield (RFC-0005 section 3.5): a word that yields, runs the tasks of a set,
// calls a host word that may finish later, or calls a word that may yield. Such a word cannot
// run inside a host word: the C stack is in the way. A call through a register (a value
// block) is not followed; a yield there inside a host word stays a fault at run time.
//
// Whether word k yields on its own, and the words it calls, from one scan of its code.
static bool word_yields_itself(const pinax_state *st, uint32_t k, void (*call)(void *, uint32_t, uint32_t), void *user) {
    const pnx_head *h = st->head;
    const uint32_t end = k + 1 < h->n_words ? st->words[k + 1].entry : h->n_code;
    bool yields = false;
    for (uint32_t i = st->words[k].entry; i < end;) {
        const uint32_t op = st->code[i] & 0xffu;
        const pnx_opinfo info = opinfo[op];
        const uint32_t x = info.second ? st->code[i + 1] : 0;
        yields = yields || op == PNX_TYIELD || op == PNX_TRUNALL || op == PNX_QWTAKE || op == PNX_QWPUT ||
                 ((op == PNX_HCALL || op == PNX_HCALLF) && (st->imports[x]->flags & PINAX_WORD_AWAITABLE));
        if (info.second == PNX_X_WORD && call)
            call(user, k, x);
        i += info.second ? 2u : 1u;
    }
    return yields;
}

// The callers of every word, as one array of word numbers cut by `start` (BACKLOGS R16).
typedef struct callers {
    uint32_t *start, *at, *list;
} callers;
static void callers_count(void *user, uint32_t from, uint32_t to) {
    (void)from;
    ++((callers *)user)->start[to + 1];
}
static void callers_fill(void *user, uint32_t from, uint32_t to) {
    callers *c = user;
    c->list[c->at[to]++] = from;
}

// Round after round, each over all the code, until no word gains the flag: correct, and quadratic
// in the number of words along a chain of calls. Used when the block has no room for the pass below.
static void mark_yields_slowly(pinax_state *st) {
    const pnx_head *h = st->head;
    for (bool changed = true; changed;) {
        changed = false;
        for (uint32_t k = 0; k < h->n_words; ++k) {
            if (st->word_counts[k].yields)
                continue;
            const uint32_t end = k + 1 < h->n_words ? st->words[k + 1].entry : h->n_code;
            bool yields = word_yields_itself(st, k, NULL, NULL);
            for (uint32_t i = st->words[k].entry; i < end && !yields;) {
                const uint32_t op = st->code[i] & 0xffu;
                const pnx_opinfo info = opinfo[op];
                yields = info.second == PNX_X_WORD && st->word_counts[st->code[i + 1]].yields;
                i += info.second ? 2u : 1u;
            }
            if (yields) {
                st->word_counts[k].yields = true;
                changed = true;
            }
        }
    }
}

// One scan of the code gives the words that yield on their own and who calls whom; the flag then
// spreads from those words to their callers, each word entered once: linear in code and calls.
static void mark_yields(pinax_state *st) {
    const pnx_head *h = st->head;
    const uint32_t n = h->n_words;
    for (uint32_t k = 0; k < n; ++k)
        st->word_counts[k].yields = false;
    const size_t mark = st->used; // the block as scratch, given back below (as check_flow does)
    callers c = {take(st, ((size_t)n + 1) * 4, 4), take(st, ((size_t)n + 1) * 4, 4), NULL};
    uint32_t *work = take(st, (size_t)n * 4, 4);
    if (!c.start || !c.at || !work) {
        give_back(st, mark);
        mark_yields_slowly(st);
        return;
    }
    uint32_t n_work = 0;
    for (uint32_t k = 0; k < n; ++k) {
        if (word_yields_itself(st, k, callers_count, &c)) {
            st->word_counts[k].yields = true;
            work[n_work++] = k;
        }
    }
    for (uint32_t k = 0; k < n; ++k) // start[k]: where the callers of word k begin
        c.start[k + 1] += c.start[k];
    c.list = take(st, (size_t)c.start[n] * 4 + 4, 4);
    if (!c.list) {
        give_back(st, mark);
        mark_yields_slowly(st);
        return;
    }
    __builtin_memcpy(c.at, c.start, ((size_t)n + 1) * 4);
    for (uint32_t k = 0; k < n; ++k)
        (void)word_yields_itself(st, k, callers_fill, &c);
    while (n_work) {
        const uint32_t w = work[--n_work];
        for (uint32_t j = c.start[w]; j < c.start[w + 1]; ++j) {
            const uint32_t caller = c.list[j];
            if (!st->word_counts[caller].yields) {
                st->word_counts[caller].yields = true;
                work[n_work++] = caller;
            }
        }
    }
    give_back(st, mark);
}

pinax_err pinax_find(pinax_state *st, const char *name, const char *signature, uint32_t flags, pinax_word *out) {
    if (!st || !name || !signature || !out)
        return PINAX_ERR_ARGUMENT;
    if (!st->head)
        return fail(st, PINAX_ERR_STATE, "no code loaded");
    // a plain name is a word of the first source; module.word names a word of that module
    uint32_t module = 0;
    const char *word = name;
    for (int pass = 0; pass < 2; ++pass) {
        for (uint32_t k = 0; k < st->head->n_words; ++k) {
            if (st->words[k].module == module && text_equal(st->words[k].name, word)) {
                if (!text_equal(st->words[k].signature, signature)) // the text as written
                    return fail(st, PINAX_ERR_SIGNATURE, "the word has another signature");
                if ((flags & PINAX_FIND_NESTED) && st->word_counts[k].yields)
                    return fail(st, PINAX_ERR_SIGNATURE, "the word may yield, so it cannot run inside a host word");
                *out = k;
                return PINAX_OK;
            }
        }
        const char *dot = name;
        while (*dot && *dot != '.')
            ++dot;
        if (pass == 1 || !*dot)
            break;
        module = pnx_module_number(st, name, (size_t)(dot - name));
        if (module == UINT32_MAX)
            break;
        word = dot + 1;
    }
    return fail(st, PINAX_ERR_NOT_FOUND, "no such word");
}

pinax_err pinax_word_at(const pinax_state *st, uint32_t index, pinax_word_info *out) {
    if (!st || !out)
        return PINAX_ERR_ARGUMENT;
    for (uint32_t k = 0; st->head && k < st->head->n_words; ++k) {
        const pnx_wordrec *w = &st->words[k];
        if (w->name[0] == '(') // the code outside definitions, value blocks
            continue;
        if (index-- == 0) {
            out->word = k;
            out->module = pnx_module_name(st, w->module);
            out->name = (pinax_str){(const unsigned char *)w->name, __builtin_strlen(w->name)};
            out->signature = (pinax_str){(const unsigned char *)w->signature, __builtin_strlen(w->signature)};
            return PINAX_OK;
        }
    }
    return PINAX_ERR_NOT_FOUND;
}

// Writes where the last fault happened into st->where: "module:line: error", or "line N: error"
// for a source without a module name, or only the error when the code carries no lines.
static void fault_place(pinax_state *st, pinax_err err) {
    char  *out = st->where;
    size_t n = 0;
    const size_t cap = sizeof st->where - 1;
#define PUT(bytes, len)                                                                            \
    for (size_t put_k = 0; put_k < (size_t)(len) && n < cap; ++put_k)                              \
        out[n++] = (char)(bytes)[put_k]
    const uint32_t pc = st->fault_pc ? st->fault_pc - 1 : 0;
    uint32_t line = 0;
    for (uint32_t k = 0; k < st->head->n_lines; ++k) {
        pnx_linerec rec;
        __builtin_memcpy(&rec, st->lines + (size_t)k * sizeof rec, sizeof rec);
        if (rec.pc > pc)
            break;
        line = rec.line;
    }
    if (line) {
        uint32_t word = 0;
        while (word + 1 < st->head->n_words && st->words[word + 1].entry <= pc)
            ++word;
        const pinax_str module = pnx_module_name(st, st->words[word].module);
        uint8_t digits[24];
        const uint32_t count = pnx_format_int(line, digits);
        if (module.len) {
            PUT(module.ptr, module.len);
            PUT(":", 1);
        } else {
            PUT("line ", 5);
        }
        PUT(digits, count);
        PUT(": ", 2);
    }
    uint32_t offset, length;
    pnx_err_name(err, &offset, &length);
    PUT(pnx_static_text + offset, length);
#undef PUT
    out[n] = '\0';
}

// Whether the first `n` bytes of a string cell lie in memory the state knows: its block, the
// interpreter's own texts, or a host string of this run.
static bool cell_readable(const pinax_state *st, pnx_scell cell, uint32_t n) {
    if (cell.segment >= PNX_SEG_COUNT || cell.segment == PNX_SEG_EMPTY || cell.segment == PNX_SEG_DONE ||
        cell.offset > UINT32_MAX)
        return false;
    if (cell.segment == PNX_SEG_KEPT) // a host word vouched that these bytes outlive the state
        return true;
    if (cell.segment >= PNX_SEG_HOST)
        return cell.segment < PNX_SEG_HOST + st->host_segments &&
               cell.offset + n <= st->host_len[cell.segment - PNX_SEG_HOST];
    if (cell.segment == PNX_SEG_STATIC)
        return cell.offset + n <= pnx_static_len;
    const uintptr_t at = (uintptr_t)st->segment[cell.segment] + (uintptr_t)cell.offset, lo = (uintptr_t)st->block;
    return at >= lo && at <= lo + st->used && n <= lo + st->used - at;
}

// Copies the locals of the word the last fault happened in, by the local records, while the
// registers still hold them: a load that faults is undone right after, records and all.
static void fault_locals(pinax_state *st) {
    st->fault_n = st->fault_used = 0;
    int64_t *const   ibank = st->fault_ibank;
    pnx_scell *const sbank = st->fault_sbank;
    st->fault_ibank = NULL; // read once, right after the fault: an image does not carry them
    st->fault_sbank = NULL;
    if (!st->head || !st->head->n_locals || !st->head->n_words || !ibank || !sbank)
        return;
    const uint32_t pc = st->fault_pc ? st->fault_pc - 1 : 0;
    uint32_t word = 0;
    while (word + 1 < st->head->n_words && st->words[word + 1].entry <= pc)
        ++word;
    const uint32_t at = pc - st->words[word].entry;
    for (uint32_t k = 0; k < st->head->n_locals && st->fault_n < 16; ++k) {
        pnx_localrec rec;
        __builtin_memcpy(&rec, st->localrecs + (size_t)k * sizeof rec, sizeof rec);
        if (rec.word != word || at < rec.from || at >= rec.to)
            continue;
        const uint32_t reg = (rec.bank ? st->fault_sbase : st->fault_ibase) + rec.reg;
        if (reg >= (rec.bank ? st->fault_str_slots : st->fault_int_slots))
            continue;
        const uint32_t name_len = (uint32_t)__builtin_strlen(rec.name), type_len = (uint32_t)__builtin_strlen(rec.type);
        uint32_t text_len = 0;
        pnx_scell cell = {0};
        if (rec.bank) { // the first bytes of a str, when they are bytes this state can vouch for: the
            // local records are not checked against the code, so the register may hold a cell left
            // by an earlier run (whose host string may be gone)
            cell = sbank[reg];
            text_len = cell.length < 32 ? cell.length : 32;
            if (!cell_readable(st, cell, text_len))
                text_len = 0;
        }
        if (name_len + type_len + text_len > sizeof st->fault_pool - st->fault_used)
            break;
        struct pnx_faultlocal *out = &st->fault_locals[st->fault_n++];
        uint8_t *pool = st->fault_pool;
        *out = (struct pnx_faultlocal){.name = (uint16_t)st->fault_used, .name_len = (uint8_t)name_len,
                                       .type = (uint16_t)(st->fault_used + name_len), .type_len = (uint8_t)type_len,
                                       .text = (uint16_t)(st->fault_used + name_len + type_len),
                                       .text_len = (uint16_t)text_len, .kind = rec.kind,
                                       .i = rec.bank ? 0 : ibank[reg]};
        __builtin_memcpy(pool + out->name, rec.name, name_len);
        __builtin_memcpy(pool + out->type, rec.type, type_len);
        if (text_len)
            __builtin_memcpy(pool + out->text, st->segment[cell.segment] + cell.offset, text_len);
        st->fault_used += name_len + type_len + text_len;
    }
}

static pinax_err deliver(pinax_state *st, pinax_err err, const pinax_args *results) {
    if (err == PINAX_OK)
        st->fault_n = 0;
    if (err != PINAX_OK) {
        if (err != PINAX_SUSPENDED && err != PINAX_WAITING) {
            fault_locals(st);
            fault_place(st, err);
            st->error_text = st->where;
            handles_close(st, st->run_serial); // what this run opened and still holds
        }
        return err;
    }
    const pnx_counts n = st->word_counts[st->top_word];
    if (st->loading && !results)
        return PINAX_OK; // a line of a session that was suspended: what it left is not shown
    if ((n.int_results && !(results && results->ri)) || (n.str_results && !(results && results->rs)))
        return fail(st, PINAX_ERR_ARGUMENT, "no place given for the results");
    for (uint32_t k = 0; k < n.int_results; ++k)
        results->ri[k] = st->ibank[k];
    for (uint32_t k = 0; k < n.str_results; ++k) {
        results->rs[k].ptr = st->segment[st->sbank[k].segment] + st->sbank[k].offset;
        results->rs[k].len = st->sbank[k].length;
    }
    return PINAX_OK;
}

// A word run from inside a host word (the host word was itself called by a running word). The
// word gets its window above the slots of that call and runs to its end here: it cannot be
// suspended, since the C stack of the host word is in the way.
static pinax_err run_nested(pinax_state *st, pinax_word word, const pinax_args *args) {
    const pnx_wordrec *rec = &st->words[word];
    const pnx_counts n = st->word_counts[word];
    if (n.yields)
        return fail(st, PINAX_ERR_SIGNATURE, "the word may yield, so it cannot run inside a host word");
    const pnx_host *const host = st->host_cur;
    int64_t *const   host_ia = st->host_ia;
    pnx_scell *const host_sa = st->host_sa;
    const uint32_t ibase = (uint32_t)(host_ia - st->ibank) + host->counts.int_args + host->counts.int_results;
    const uint32_t sbase = (uint32_t)(host_sa - st->sbank) + host->counts.str_args + host->counts.str_results;
    if (st->depth >= st->depth_max || ibase + rec->int_cells > st->int_slots ||
        sbase + rec->str_cells > st->str_slots)
        return fail(st, PINAX_ERR_DEPTH, "the word's window does not fit the banks");
    pnx_task *const task = st->task;
    pnx_context outer; // where the run goes back to, whatever task that word resumes and leaves
    pnx_ctx_save(st, &outer);
    for (uint32_t k = 0; k < n.int_args; ++k)
        st->ibank[ibase + k] = args->i[k];
    for (uint32_t k = 0; k < n.str_args; ++k) { // the bytes move into the region: they may not outlive the call
        if (args->s[k].len > UINT32_MAX)
            return fail(st, PINAX_ERR_ARGUMENT, "a string argument is longer than 4 GiB");
        uint8_t *copy = pnx_region_alloc(st, args->s[k].len);
        if (!copy)
            return fail(st, PINAX_ERR_FULL, "the region has no room for the string arguments");
        if (args->s[k].len)
            __builtin_memcpy(copy, args->s[k].ptr, args->s[k].len);
        st->sbank[sbase + k] = (pnx_scell){PNX_SEG_REGION, (uint32_t)args->s[k].len, (uint64_t)(copy - st->region)};
    }
    if (ibase + rec->int_cells > st->int_peak)
        st->int_peak = ibase + rec->int_cells;
    if (sbase + rec->str_cells > st->str_peak)
        st->str_peak = sbase + rec->str_cells;
    const uint32_t pc = st->pc, int_base = st->int_base, str_base = st->str_base, floor = st->depth_floor;
    const uint32_t depth = st->depth;
    const pinax_err outer_fail = st->fail;
    const bool ends_task = st->ends_task;
    const uint32_t region_floor = st->region_floor;
    st->ends_task = false; // this word returns to the host word, whatever runs around it
    st->region_floor = st->region_used; // nor does it cut the region below its arguments
    st->pc = rec->entry;
    st->int_base = ibase;
    st->str_base = sbase;
    st->depth_floor = depth;
    st->fail = PINAX_ERR_FAILED;
    ++st->nest;
    pinax_err err = pnx_execute(st);
    --st->nest;
    st->host_cur = host; // the host words that word called have come and gone
    st->host_ia = host_ia;
    st->host_sa = host_sa;
    if (err == PINAX_SUSPENDED) { // the budget ran out: this run ends, and the host word says so
        st->suspended = false;
        st->error_text = "the instruction budget ran out in a word run from a host word";
        err = PINAX_ERR_INTERRUPTED;
    }
    if (st->task != task) // it ended inside a task it resumed: those tasks end, and the run is put back
        pnx_tasks_unwind(st, task, &outer);
    st->region_floor = region_floor;
    st->pc = pc;
    st->int_base = int_base;
    st->str_base = str_base;
    st->depth_floor = floor;
    st->depth = depth;
    st->fail = outer_fail;
    st->ends_task = ends_task;
    if (err != PINAX_OK)
        return err;
    if ((n.int_results && !(args && args->ri)) || (n.str_results && !(args && args->rs)))
        return fail(st, PINAX_ERR_ARGUMENT, "no place given for the results");
    for (uint32_t k = 0; k < n.int_results; ++k)
        args->ri[k] = st->ibank[ibase + k];
    for (uint32_t k = 0; k < n.str_results; ++k) {
        const pnx_scell cell = st->sbank[sbase + k];
        args->rs[k].ptr = st->segment[cell.segment] + cell.offset;
        args->rs[k].len = cell.length;
    }
    return PINAX_OK;
}

static pinax_err hosts_given(pinax_state *st);

pinax_err pinax_run(pinax_state *st, pinax_word word, const pinax_args *args) {
    if (!st)
        return PINAX_ERR_ARGUMENT;
    if (!st->head || st->suspended)
        return fail(st, PINAX_ERR_STATE, "no code loaded, or a suspended run must be resumed first");
    if (st->frozen && !st->host_cur)
        return fail(st, PINAX_ERR_STATE, "the state lent a global with pinax_share: it does not run any more");
    if (hosts_given(st) != PINAX_OK)
        return PINAX_ERR_NOT_FOUND;
    if (!st->load_runs && !st->host_cur) { // the code of a load runs before the host can share anything
        for (uint32_t k = 0; k < st->head->n_globals; ++k) {
            if (st->globals[k].shared && st->globals[k].ptr == (const uint8_t *)no_storage)
                return fail(st, PINAX_ERR_NOT_FOUND, "a shared global is not given yet: pinax_share");
        }
    }
    if (word >= st->head->n_words)
        return fail(st, PINAX_ERR_ARGUMENT, "not a word of this state");
    const pnx_wordrec *rec = &st->words[word];
    const pnx_counts n = st->word_counts[word];
    if ((n.int_args && !(args && args->i)) || (n.str_args && !(args && args->s)))
        return fail(st, PINAX_ERR_ARGUMENT, "arguments missing");
    if (st->host_cur)
        return run_nested(st, word, args);
    if (rec->int_cells > st->int_slots || rec->str_cells > st->str_slots)
        return fail(st, PINAX_ERR_DEPTH, "the word's window does not fit the banks");

    if (st->region_used > st->region_peak)
        st->region_peak = st->region_used;
    if (rec->int_cells > st->int_peak)
        st->int_peak = rec->int_cells;
    if (rec->str_cells > st->str_peak)
        st->str_peak = rec->str_cells;
    st->region_used = 0; // results of the previous run end here
    st->ptn = (pnx_ptn_found){0}; // and its last match: a capture is a view of what that run matched
    st->host_segments = n.str_args;
    for (uint32_t k = 0; k < n.int_args; ++k)
        st->ibank[k] = args->i[k];
    for (uint32_t k = 0; k < n.str_args; ++k) {
        if (args->s[k].len > UINT32_MAX)
            return fail(st, PINAX_ERR_ARGUMENT, "a string argument is longer than 4 GiB");
        st->segment[PNX_SEG_HOST + k] = args->s[k].len ? args->s[k].ptr : empty_bytes;
        st->host_len[k] = (uint32_t)args->s[k].len;
        st->sbank[k] = (pnx_scell){PNX_SEG_HOST + k, (uint32_t)args->s[k].len, 0};
    }
    st->top_word = word;
    if (++st->run_serial == 0)
        st->run_serial = 1;
    st->pc = rec->entry;
    st->int_base = st->str_base = st->depth = st->depth_floor = 0;
    st->region_floor = 0;
    st->ends_task = false;
    st->resume_dst = UINT32_MAX;
    st->left = st->budget;
    st->charge = 0;
    st->fail = PINAX_ERR_FAILED;
    return deliver(st, pnx_execute(st), args);
}

pinax_err pinax_resume(pinax_state *st, const pinax_args *results) {
    if (!st)
        return PINAX_ERR_ARGUMENT;
    if (!st->suspended || st->host_cur)
        return fail(st, PINAX_ERR_STATE, "nothing is suspended");
    if (hosts_given(st) != PINAX_OK)
        return PINAX_ERR_NOT_FOUND;
    if (st->main_wait.token)
        return PINAX_WAITING; // the operation the main run waits for has not been completed
    st->left = st->budget > st->owed ? st->budget : st->owed;
    st->waiting = false;
    st->owed = 0;
    pinax_err run = st->main_fault;
    st->main_fault = PINAX_OK;
    if (run != PINAX_OK) { // the operation failed and the word that called it has no `or fail`
        st->suspended = false;
        st->fault_pc = st->pc;
        st->fault_ibase = st->int_base;
        st->fault_sbase = st->str_base;
        pnx_fault_banks(st);
        if (st->task)
            pnx_tasks_abort(st);
    } else {
        run = pnx_execute(st);
    }
    const pinax_err err = deliver(st, run, results);
    if (st->loading && err != PINAX_SUSPENDED && err != PINAX_WAITING) { // the load ends here: done, or failed and undone
        st->loading = false;
        if (err != PINAX_OK) {
            const char *text = st->error_text;
            const pnx_program before = st->before_load;
            pnx_program_restore(st, &before);
            st->error_text = text;
        } else {
            pnx_compact(st);
        }
    }
    return err;
}

pinax_err pinax_abort(pinax_state *st) {
    if (!st)
        return PINAX_ERR_ARGUMENT;
    if (!st->suspended || st->host_cur)
        return fail(st, PINAX_ERR_STATE, "nothing is suspended");
    st->suspended = false;
    st->waiting = false;
    st->owed = 0;
    st->main_fault = PINAX_OK;
    pnx_tasks_abort(st); // back to the main run; every task is freed, every token forgotten
    handles_close(st, st->run_serial);
    st->fault_n = 0;
    if (st->loading) { // a load whose code ran out of budget: undone, as a failed load is
        st->loading = false;
        const pnx_program before = st->before_load;
        pnx_program_restore(st, &before);
    }
    st->error_text = "the run was abandoned";
    return PINAX_OK;
}

void pinax_set_budget(pinax_state *st, uint64_t instructions) {
    st->budget = instructions;
}

void pinax_stats_get(const pinax_state *st, pinax_stats *out) {
    const size_t size = out->struct_size;
    if (size != sizeof *out)
        return;
    __builtin_memset(out, 0, sizeof *out);
    out->struct_size = size;
    out->depth_peak = st->depth_peak;
    out->int_slots_peak = st->int_peak;
    out->str_slots_peak = st->str_peak;
    out->region_bytes_peak = st->region_used > st->region_peak ? st->region_used : st->region_peak;
    out->handles_peak = st->handles_peak;
    out->instructions = st->instructions; // an upper bound, see CHECK
    out->block_bytes_used = st->used;
}

// ---- host memory as globals ---------------------------------------------------------------

// The storage of a global declared `bound`: a scalar, an array (of integers, of str(N), of
// records) or a buffer. A buffer's memory begins with its length, a uint64_t, and its declared
// bytes follow.
// A run is suspended, waits, or a host word of it is running: the storage of its globals stays.
static bool run_unfinished(const pinax_state *st) {
    return st->host_cur || st->nest || st->suspended || st->loading;
}

pinax_err pinax_bind(pinax_state *st, const char *global, void *memory, size_t bytes) {
    if (!st || !global || !memory)
        return PINAX_ERR_ARGUMENT;
    if (!st->head)
        return fail(st, PINAX_ERR_STATE, "no code loaded");
    if (run_unfinished(st)) // a view the run holds may point into the storage the host takes back
        return fail(st, PINAX_ERR_STATE, "a run of the state is not finished");
    const pnx_globalrec *recs = (const pnx_globalrec *)(const void *)(st->words + st->head->n_words);
    for (uint32_t k = 0; k < st->head->n_globals; ++k) {
        if (!text_equal(recs[k].name, global))
            continue;
        pnx_global *g = &st->globals[k];
        if (recs[k].reserved != 2)
            return fail(st, PINAX_ERR_STATE, "the script did not declare this global bound");
        if (st->frozen)
            return fail(st, PINAX_ERR_STATE, "the state lent a global with pinax_share: its storage stays");
        const size_t want = g->kind == PNX_G_INT || g->kind == PNX_G_ARRAY ? (size_t)g->capacity * 8
                            : g->kind == PNX_G_TABLE                       ? (size_t)g->capacity * g->elem_bytes
                            : g->kind == PNX_G_BUFFER                      ? (size_t)g->capacity + 8
                                                                           : 0;
        if (!want)
            return fail(st, PINAX_ERR_STATE, "a global of this kind cannot be bound");
        if (bytes != want)
            return fail(st, PINAX_ERR_ARGUMENT, "the memory does not have the declared size");
        if (g->kind == PNX_G_BUFFER) { // a length the host left there must fit
            uint64_t used;
            __builtin_memcpy(&used, memory, 8);
            if (used > g->capacity)
                return fail(st, PINAX_ERR_ARGUMENT, "the buffer's length is more than its declared size");
        }
        g->ptr = memory;
        return PINAX_OK;
    }
    return fail(st, PINAX_ERR_NOT_FOUND, "no such global");
}

// The record of the global named `name`, or NULL.
static const pnx_globalrec *global_rec(const pinax_state *st, const char *name, uint32_t *index) {
    const pnx_globalrec *recs = (const pnx_globalrec *)(const void *)(st->words + st->head->n_words);
    for (uint32_t k = 0; k < st->head->n_globals; ++k) {
        if (text_equal(recs[k].name, name)) {
            *index = k;
            return &recs[k];
        }
    }
    return NULL;
}

// Points shared global `ti` at storage that stays as it is. An index of a table given anew no
// longer fits that table: it waits to be given again.
static void give_shared(pinax_state *st, uint32_t ti, uint8_t *ptr, uint32_t length, uint32_t head, uint32_t capacity) {
    pnx_global *t = &st->globals[ti];
    const bool  index = t->kind == PNX_G_INDEX;
    t->ptr = ptr;
    t->length = length;
    t->head = head;
    t->capacity = index ? capacity : t->capacity;
    for (uint32_t k = 0; !index && k < st->head->n_globals; ++k) {
        pnx_global *x = &st->globals[k];
        if (x->kind == PNX_G_INDEX && x->shared && (x->key & 0xfffu) == ti)
            unshared(x);
    }
}

// RFC-0020 section 4: the target's declaration, written `shared`, reads the source's storage.
pinax_err pinax_share(pinax_state *st, const char *global, pinax_state *source, const char *from) {
    if (!st || !global || !source || !from || st == source)
        return PINAX_ERR_ARGUMENT;
    if (!st->head || !source->head)
        return fail(st, PINAX_ERR_STATE, "no code loaded");
    if (run_unfinished(st) || run_unfinished(source))
        return fail(st, PINAX_ERR_STATE, "a run of one of the states is not finished");
    uint32_t             ti, si;
    const pnx_globalrec *trec = global_rec(st, global, &ti), *srec = global_rec(source, from, &si);
    if (!trec || !srec)
        return fail(st, PINAX_ERR_NOT_FOUND, "no such global");
    pnx_global       *t = &st->globals[ti];
    const pnx_global *g = &source->globals[si];
    if (!t->shared)
        return fail(st, PINAX_ERR_STATE, "the script did not declare this global shared");
    const bool index = trec->kind == PNX_G_INDEX;
    if (trec->kind != srec->kind || trec->capacity != srec->capacity ||
        (!index && trec->elem_bytes != srec->elem_bytes) || !text_equal(trec->type, (const char *)srec->type) ||
        (index && ((trec->elem_bytes >> 12) != (srec->elem_bytes >> 12) || t->key_is_str != g->key_is_str ||
                   __builtin_memcmp(st->hash_key, source->hash_key, sizeof st->hash_key) != 0)))
        return fail(st, PINAX_ERR_SIGNATURE,
                    index ? "the index differs: its key, its size or the hash key of the states"
                          : "the two declarations differ in kind, type or size");
    if (g->shared && g->ptr == (const uint8_t *)no_storage)
        return fail(st, PINAX_ERR_STATE, "the source's global is shared and not given itself");
    if (index) { // its slots name rows of the source's table: the target's table must be that one
        const pnx_global *table = &st->globals[t->key & 0xfffu], *from_table = &source->globals[g->key & 0xfffu];
        if (!table->shared || table->ptr != from_table->ptr || table->length != from_table->length)
            return fail(st, PINAX_ERR_STATE, "share the index's table first, from the same state");
    }
    give_shared(st, ti, g->ptr, g->length, g->head, trec->capacity);
    source->frozen = true; // from now on its bytes stay as they are
    return PINAX_OK;
}

// ---- the shareable globals as one file without addresses (RFC-0020 section 4.2) ------------

// The file: this head, one entry per global, then each global's bytes, 16-byte aligned. Nothing in
// it is an address, so every process maps it where it likes.
typedef struct pnx_share_head {
    char          magic[4]; // "PNXS"
    uint32_t      format, order, count;
    uint64_t      length;   // of the whole file
    unsigned char hash_key[16];
    uint8_t       reserved[24];
} pnx_share_head;
typedef struct pnx_share_entry {
    pnx_globalrec rec;           // as the source declared it; reserved: an index's key kind only
    uint32_t      length, table; // cells in use; an index: the entry of its table
    uint64_t      offset, bytes; // from the start of the file
    uint64_t      reserved;      // 0
} pnx_share_entry;
static_assert(sizeof(pnx_share_head) == 64 && sizeof(pnx_share_entry) % 16 == 0, "share file records lose their alignment");

// What a global keeps in its storage, by kind; 0 for a kind that is not shared.
static uint64_t share_bytes(uint32_t kind, uint64_t capacity, uint64_t elem_bytes) {
    switch (kind) {
    case PNX_G_INT: return 8;
    case PNX_G_ARRAY: case PNX_G_LIST: return capacity * 8;
    case PNX_G_BUFFER: return capacity + 8;
    case PNX_G_TABLE: case PNX_G_ROWS: return capacity * elem_bytes;
    case PNX_G_INDEX: return capacity * 4;
    default: return 0;
    }
}

static bool share_saved(const pinax_state *st, uint32_t k) {
    const pnx_global *g = &st->globals[k];
    if (g->shared || !share_bytes(g->kind, g->capacity, g->elem_bytes))
        return false;
    return g->kind != PNX_G_INDEX || !st->globals[g->key & 0xfffu].shared; // an index goes with its table
}

size_t pinax_share_size(const pinax_state *st) {
    if (!st || !st->head)
        return 0;
    uint64_t count = 0, data = 0;
    for (uint32_t k = 0; k < st->head->n_globals; ++k) {
        if (share_saved(st, k)) {
            const pnx_global *g = &st->globals[k];
            ++count;
            data += (share_bytes(g->kind, g->capacity, g->elem_bytes) + 15u) & ~(uint64_t)15u;
        }
    }
    return (size_t)(sizeof(pnx_share_head) + count * sizeof(pnx_share_entry) + data);
}

pinax_err pinax_share_save(const pinax_state *st, pinax_mem out, size_t *written) {
    if (!st || !out.ptr || !written)
        return PINAX_ERR_ARGUMENT;
    *written = 0;
    if (!st->head)
        return PINAX_ERR_STATE;
    if (st->host_cur || st->nest || st->loading || st->suspended) // its bytes may be half changed
        return PINAX_ERR_STATE;
    const size_t size = pinax_share_size(st);
    if (out.len < size)
        return PINAX_ERR_NO_ROOM;
    const pnx_globalrec *recs = (const pnx_globalrec *)(const void *)(st->words + st->head->n_words);
    uint8_t             *file = out.ptr;
    pnx_share_head       head = {.magic = {'P', 'N', 'X', 'S'}, .format = PINAX_FORMAT, .order = PNX_ORDER_MARK,
                                 .length = size};
    __builtin_memcpy(head.hash_key, st->hash_key, sizeof head.hash_key);
    for (uint32_t k = 0; k < st->head->n_globals; ++k)
        head.count += share_saved(st, k);
    __builtin_memset(file, 0, size);
    __builtin_memcpy(file, &head, sizeof head);
    uint64_t at = sizeof head + (uint64_t)head.count * sizeof(pnx_share_entry);
    uint32_t n = 0;
    for (uint32_t k = 0; k < st->head->n_globals; ++k) {
        if (!share_saved(st, k))
            continue;
        const pnx_global *g = &st->globals[k];
        pnx_share_entry   e = {.rec = recs[k], .length = g->length, .table = UINT32_MAX, .offset = at,
                               .bytes = share_bytes(g->kind, g->capacity, g->elem_bytes)};
        e.rec.reserved = g->kind == PNX_G_INDEX ? recs[k].reserved & 1u : 0;
        for (uint32_t j = 0, m = 0; g->kind == PNX_G_INDEX && j < k; ++j) { // the entry its table got
            if (j == (g->key & 0xfffu))
                e.table = m;
            m += share_saved(st, j);
        }
        __builtin_memcpy(file + sizeof head + (size_t)n * sizeof e, &e, sizeof e);
        __builtin_memcpy(file + at, g->ptr, (size_t)e.bytes);
        at += (e.bytes + 15u) & ~(uint64_t)15u;
        ++n;
    }
    *written = size;
    return PINAX_OK;
}

// The entry `k` of a share file whose head was checked, itself checked: inside the file, aligned,
// of the size its declaration gives, with no more cells in use than it holds.
static const pnx_share_entry *share_entry(pinax_str file, uint32_t k) {
    const pnx_share_head *head = (const pnx_share_head *)(const void *)file.ptr;
    const pnx_share_entry *e = (const pnx_share_entry *)(const void *)(file.ptr + sizeof *head) + k;
    const uint64_t start = sizeof *head + (uint64_t)head->count * sizeof *e;
    const uint64_t want = share_bytes(e->rec.kind, e->rec.capacity, e->rec.elem_bytes);
    if (!terminated(e->rec.name, sizeof e->rec.name) || !terminated(e->rec.type, sizeof e->rec.type) || !want ||
        e->rec.capacity == 0 || e->bytes != want || e->offset % 16 || e->offset < start || e->offset > file.len ||
        e->bytes > file.len - e->offset || e->length > e->rec.capacity || e->reserved)
        return NULL;
    const uint8_t *bytes = file.ptr + e->offset;
    uint64_t       used;
    switch (e->rec.kind) {
    case PNX_G_INT: case PNX_G_ARRAY: case PNX_G_TABLE: // always full
        return e->length == (e->rec.kind == PNX_G_INT ? 1u : e->rec.capacity) ? e : NULL;
    case PNX_G_BUFFER:
        __builtin_memcpy(&used, bytes, 8);
        return used <= e->rec.capacity ? e : NULL;
    case PNX_G_INDEX: { // a power of two of slots, each empty, removed or a row of its table
        if (e->rec.capacity & (e->rec.capacity - 1) || e->table >= k || (e->rec.reserved & ~1u))
            return NULL;
        // the kind first: an index of an index would go on down the file, a call for each entry
        const uint32_t table_kind = ((const pnx_share_entry *)(const void *)(file.ptr + sizeof *head) + e->table)->rec.kind;
        if (table_kind != PNX_G_TABLE && table_kind != PNX_G_ROWS)
            return NULL;
        const pnx_share_entry *table = share_entry(file, e->table);
        if (!table)
            return NULL;
        const uint32_t offset = (e->rec.elem_bytes >> 12) & 0xfffu, extra = e->rec.elem_bytes >> 24;
        if (offset + ((e->rec.reserved & 1u) ? 1u + extra : extra < PNX_W_COUNT ? width_bytes[extra] : UINT32_MAX) >
            table->rec.elem_bytes)
            return NULL;
        for (uint32_t slot = 0; slot < e->rec.capacity; ++slot) {
            uint32_t row;
            __builtin_memcpy(&row, bytes + (size_t)slot * 4, 4);
            if (row != UINT32_MAX && row > table->length) // all ones: a removed entry
                return NULL;
        }
        return e;
    }
    default: return e;
    }
}

pinax_err pinax_share_file(pinax_state *st, const char *global, pinax_str file, const char *from) {
    if (!st || !global || !from || !file.ptr)
        return PINAX_ERR_ARGUMENT;
    if (!st->head)
        return fail(st, PINAX_ERR_STATE, "no code loaded");
    if (run_unfinished(st))
        return fail(st, PINAX_ERR_STATE, "a run of the state is not finished");
    if ((uintptr_t)file.ptr % 16)
        return fail(st, PINAX_ERR_ARGUMENT, "the file's bytes must start 16-byte aligned (a mapping always does)");
    pnx_share_head head;
    if (file.len < sizeof head)
        return fail(st, PINAX_ERR_FORMAT, "not a share file");
    __builtin_memcpy(&head, file.ptr, sizeof head);
    if (__builtin_memcmp(head.magic, "PNXS", 4) != 0 || head.format != PINAX_FORMAT || head.order != PNX_ORDER_MARK ||
        head.length != file.len || head.count > (file.len - sizeof head) / sizeof(pnx_share_entry))
        return fail(st, PINAX_ERR_FORMAT, "not a share file of this format and byte order, or cut short");
    uint32_t             ti, fi = UINT32_MAX;
    const pnx_globalrec *trec = global_rec(st, global, &ti);
    const pnx_share_entry *entries = (const pnx_share_entry *)(const void *)(file.ptr + sizeof head);
    for (uint32_t k = 0; fi == UINT32_MAX && k < head.count; ++k) {
        if (terminated(entries[k].rec.name, sizeof entries[k].rec.name) && text_equal(entries[k].rec.name, from))
            fi = k;
    }
    if (!trec || fi == UINT32_MAX)
        return fail(st, PINAX_ERR_NOT_FOUND, "no such global");
    const pnx_share_entry *e = share_entry(file, fi);
    if (!e)
        return fail(st, PINAX_ERR_FORMAT, "the file's entry for this global is damaged");
    pnx_global *t = &st->globals[ti];
    if (!t->shared)
        return fail(st, PINAX_ERR_STATE, "the script did not declare this global shared");
    const bool index = trec->kind == PNX_G_INDEX;
    if (trec->kind != e->rec.kind || trec->capacity != e->rec.capacity ||
        (!index && trec->elem_bytes != e->rec.elem_bytes) || !text_equal(trec->type, e->rec.type) ||
        (index && ((trec->elem_bytes >> 12) != (e->rec.elem_bytes >> 12) || t->key_is_str != (e->rec.reserved & 1u) ||
                   __builtin_memcmp(st->hash_key, head.hash_key, sizeof st->hash_key) != 0)))
        return fail(st, PINAX_ERR_SIGNATURE,
                    index ? "the index differs: its key, its size or the hash key of the state and the file"
                          : "the two declarations differ in kind, type or size");
    if (index) { // its slots name rows of the file's table: the state's table must be that one
        const pnx_share_entry *table = &entries[e->table];
        const pnx_global      *mine = &st->globals[t->key & 0xfffu];
        if (!mine->shared || mine->ptr != file.ptr + table->offset || mine->length != table->length)
            return fail(st, PINAX_ERR_STATE, "share the index's table first, from the same file");
    }
    give_shared(st, ti, (uint8_t *)(uintptr_t)(file.ptr + e->offset), e->length, 0, trec->capacity);
    return PINAX_OK;
}

// ---- declared in pinax.h, not in P0 ---------------------------------------------------------

pinax_token pinax_call_pending(pinax_call *call) {
    pinax_state *st = call->state;
    if (!call->token)
        call->token = ++st->token_serial;
    return call->token;
}

// Puts the results of a host operation where the call that started it expected them. `region`
// is the top of the region of the run that waits; strings are copied there.
static pinax_err wake(pinax_state *st, const pnx_wait *w, int64_t *ibank, pnx_scell *sbank, uint32_t *region_used,
                      uint32_t region_len, const pinax_args *results) {
    const pnx_counts n = w->host->counts;
    if ((n.int_results && !(results && results->ri)) || (n.str_results && !(results && results->rs)))
        return PINAX_ERR_ARGUMENT;
    for (uint32_t k = 0; k < n.int_results; ++k)
        ibank[w->ia + n.int_args + k] = results->ri[k];
    for (uint32_t k = 0; k < n.str_results; ++k) {
        const pinax_str s = results->rs[k];
        if (s.len > region_len - *region_used)
            return PINAX_ERR_FULL;
        if (s.len)
            __builtin_memcpy(st->region + *region_used, s.ptr, s.len);
        sbank[w->sa + n.str_args + k] = (pnx_scell){s.len ? PNX_SEG_REGION : PNX_SEG_EMPTY, (uint32_t)s.len,
                                                    s.len ? *region_used : 0};
        *region_used += (uint32_t)s.len;
    }
    return PINAX_OK;
}

pinax_err pinax_complete(pinax_state *st, pinax_token token, pinax_err outcome, const pinax_args *results) {
    if (!st || !token)
        return PINAX_ERR_ARGUMENT;
    if (st->host_cur)
        return fail(st, PINAX_ERR_STATE, "a host word is running");
    if (st->main_wait.token == token) { // the main run waits
        const pnx_wait w = st->main_wait;
        if (outcome == PINAX_OK)
            outcome = wake(st, &w, st->ibank, st->sbank, &st->region_used, st->region_len, results);
        st->main_wait.token = 0;
        if (outcome == PINAX_OK)
            st->pc += w.fails ? 1u : 0u; // past the failure word
        else if (w.fails && outcome >= PINAX_ERR_FAILED)
            st->fail = outcome;
        else
            st->main_fault = outcome >= PINAX_ERR_FAILED ? outcome : PINAX_ERR_STATE;
        return PINAX_OK;
    }
    for (uint32_t k = 0; st->head && k < st->head->n_globals; ++k) {
        const pnx_global *g = &st->globals[k];
        for (uint32_t slot = 0; g->kind == PNX_G_TASKS && slot < g->capacity; ++slot) {
            pnx_task *t = (pnx_task *)(void *)g->ptr + slot;
            if (t->state != PNX_TASK_WAITING || t->wait.token != token)
                continue;
            if (outcome == PINAX_OK)
                outcome = wake(st, &t->wait, t->ibank, t->sbank, &t->ctx.region_used, t->ctx.region_len, results);
            if (t->cancelled && t->wait.fails) // cancelled while it waited: that is what it learns
                outcome = PINAX_ERR_CANCELLED;
            t->wait.token = 0;
            t->state = PNX_TASK_READY;
            if (outcome == PINAX_OK)
                t->ctx.pc += t->wait.fails ? 1u : 0u;
            else if (t->wait.fails && outcome >= PINAX_ERR_FAILED)
                t->wake_fail = outcome;
            else
                t->wake_fault = outcome >= PINAX_ERR_FAILED ? outcome : PINAX_ERR_STATE;
            return PINAX_OK;
        }
    }
    return fail(st, PINAX_ERR_NOT_FOUND, "no run waits for this operation");
}

void pinax_set_resolver(pinax_state *st, pinax_resolve_fn resolve, void *user) {
    if (st) {
        st->resolve = resolve;
        st->resolve_user = user;
        for (uint32_t k = 0; st->from_image && resolve && k < st->n_hosts; ++k) { // the externs of an image
            pnx_host *h = &st->hosts[k];
            if (h->module[0] == '@' && !h->ext)
                h->ext = resolve(user, (pinax_str){(const unsigned char *)h->module + 1, __builtin_strlen(h->module) - 1},
                                 (pinax_str){(const unsigned char *)h->name, __builtin_strlen(h->name)});
        }
    }
}

// The kind of one parameter of an extern signature, from its words: [name] [owned] type.
static bool extern_kind(const pinax_state *st, const char *type, size_t len, bool owned, bool result, uint8_t *kind) {
    for (uint32_t k = 0; k < st->n_handle_types; ++k) {
        if (token_is(type, len, st->handle_types[k].name)) {
            *kind = (uint8_t)(PNX_XA_HANDLE + k + (owned && !result ? PNX_XA_OWNED : 0));
            return true;
        }
    }
    if (owned || token_is(type, len, "float"))
        return false;
    const bool text = token_is(type, len, "str"), ctext = token_is(type, len, "cstr");
    if ((text || ctext) && result)
        return false;
    *kind = text ? PNX_XA_STR : ctext ? PNX_XA_CSTR : PNX_XA_INT;
    return true;
}

// Reads "(a int, s str gives r file or fail)" into the layout of the C call.
static bool extern_layout(const pinax_state *st, const char *sig, pnx_host *h) {
    const char *p = sig;
    if (*p++ != '(')
        return false;
    bool results = false, owned = false, fails = false;
    unsigned tokens = 0, c_args = 0;
    const char *type = NULL;
    size_t type_len = 0;
    h->ext_n = 0;
    h->ext_result = PNX_XA_NONE;
    for (;;) {
        while (*p == ' ')
            ++p;
        const char *start = p;
        while (is_name_char(*p))
            ++p;
        const size_t len = (size_t)(p - start);
        const bool is_gives = len && token_is(start, len, "gives"), is_or = len && token_is(start, len, "or");
        if (len && !is_gives && !is_or) {
            if (token_is(start, len, "owned"))
                owned = true;
            type = start;
            type_len = len;
            ++tokens;
            continue;
        }
        if (tokens) {
            uint8_t kind;
            if (!extern_kind(st, type, type_len, owned, results, &kind))
                return false;
            if (results) {
                if (h->ext_result != PNX_XA_NONE)
                    return false; // one result
                h->ext_result = kind;
            } else {
                c_args += kind == PNX_XA_STR ? 2 : 1;
                if (h->ext_n == 8 || c_args > 8)
                    return false;
                h->ext_kind[h->ext_n++] = kind;
            }
            tokens = 0;
            owned = false;
        }
        if (is_gives) {
            results = true;
        } else if (is_or) {
            fails = true; // "or fail)": the shape was checked by pnx_signature_counts
            while (*p && *p != ')')
                ++p;
        } else if (*p == ',') {
            ++p;
        }
        if (*p == ')')
            break;
        if (!*p)
            return false;
    }
    // only a pointer can say that the call failed: by being null
    return !fails || h->ext_result >= PNX_XA_HANDLE;
}

typedef intptr_t x0(void);
typedef intptr_t x1(intptr_t);
typedef intptr_t x2(intptr_t, intptr_t);
typedef intptr_t x3(intptr_t, intptr_t, intptr_t);
typedef intptr_t x4(intptr_t, intptr_t, intptr_t, intptr_t);
typedef intptr_t x5(intptr_t, intptr_t, intptr_t, intptr_t, intptr_t);
typedef intptr_t x6(intptr_t, intptr_t, intptr_t, intptr_t, intptr_t, intptr_t);
typedef intptr_t x7(intptr_t, intptr_t, intptr_t, intptr_t, intptr_t, intptr_t, intptr_t);
typedef intptr_t x8(intptr_t, intptr_t, intptr_t, intptr_t, intptr_t, intptr_t, intptr_t, intptr_t);

// The host word behind every extern: lays the arguments out as RFC-0015 section 4.3 says and
// calls the C function.
static pinax_err extern_call(pinax_call *call, void *user, const pinax_args *args) {
    const pnx_host *h = user;
    pinax_state *st = call->state;
    const uint32_t region_mark = st->region_used;
    intptr_t a[8] = {0};
    pinax_handle taken[8];
    uint8_t taken_type[8];
    unsigned n = 0, ni = 0, ns = 0, n_taken = 0;
    for (unsigned k = 0; k < h->ext_n; ++k) {
        const uint8_t kind = h->ext_kind[k];
        if (kind == PNX_XA_INT) {
            a[n++] = (intptr_t)args->i[ni++];
        } else if (kind == PNX_XA_STR) {
            a[n++] = (intptr_t)args->s[ns].ptr;
            a[n++] = (intptr_t)args->s[ns++].len;
        } else if (kind == PNX_XA_CSTR) { // a copy with a NUL after it, for the time of the call
            const pinax_str s = args->s[ns++];
            uint8_t *copy = pnx_region_alloc(st, s.len + 1);
            if (!copy) {
                pnx_region_give_back(st, region_mark);
                return PINAX_ERR_FULL;
            }
            if (s.len)
                __builtin_memcpy(copy, s.ptr, s.len);
            copy[s.len] = 0;
            a[n++] = (intptr_t)copy;
        } else {
            const uint32_t type = (uint32_t)(kind & 0x0fu) + 1;
            const pinax_handle handle = (pinax_handle)args->i[ni++];
            void *object = pinax_handle_get(st, type, handle);
            if (!object) {
                pnx_region_give_back(st, region_mark);
                return PINAX_ERR_STALE;
            }
            if (kind & PNX_XA_OWNED) {
                taken[n_taken] = handle;
                taken_type[n_taken++] = (uint8_t)type;
            }
            a[n++] = (intptr_t)object;
        }
    }
    intptr_t r = 0;
    switch (n) {
    case 0: r = ((x0 *)h->ext)(); break;
    case 1: r = ((x1 *)h->ext)(a[0]); break;
    case 2: r = ((x2 *)h->ext)(a[0], a[1]); break;
    case 3: r = ((x3 *)h->ext)(a[0], a[1], a[2]); break;
    case 4: r = ((x4 *)h->ext)(a[0], a[1], a[2], a[3]); break;
    case 5: r = ((x5 *)h->ext)(a[0], a[1], a[2], a[3], a[4]); break;
    case 6: r = ((x6 *)h->ext)(a[0], a[1], a[2], a[3], a[4], a[5]); break;
    case 7: r = ((x7 *)h->ext)(a[0], a[1], a[2], a[3], a[4], a[5], a[6]); break;
    default: r = ((x8 *)h->ext)(a[0], a[1], a[2], a[3], a[4], a[5], a[6], a[7]); break;
    }
    pnx_region_give_back(st, region_mark);
    for (unsigned k = 0; k < n_taken; ++k) // the function took the object: the script's handle ends
        (void)pinax_handle_release(st, taken_type[k], taken[k]);
    if (h->ext_result == PNX_XA_INT) {
        args->ri[0] = (int64_t)r;
    } else if (h->ext_result >= PNX_XA_HANDLE) {
        pinax_handle out;
        if (!r)
            return PINAX_ERR_FAILED;
        const pinax_err err = pinax_handle_new(st, (uint32_t)(h->ext_result & 0x0fu) + 1, (void *)r, &out);
        if (err != PINAX_OK)
            return err;
        args->ri[0] = (int64_t)out;
    }
    return PINAX_OK;
}

pinax_err pnx_extern_link(pinax_state *st, const char *module, const char *name, const char *signature,
                          uint32_t *index) {
    pnx_host made = {0};
    if (!copy_text(made.module, sizeof made.module, module) || !copy_text(made.name, sizeof made.name, name) ||
        !copy_text(made.signature, sizeof made.signature, signature))
        return fail(st, PINAX_ERR_ARGUMENT, "extern: the library or function name is too long");
    if (!pnx_signature_counts(made.signature, &made.counts) || !extern_layout(st, made.signature, &made))
        return fail(st, PINAX_ERR_TYPE,
                    "extern: int, handle, str and cstr arguments (eight for C at most), one int or handle result; "
                    "or fail only with a handle result");
    for (uint32_t k = 0; k < st->n_hosts; ++k) {
        if (text_equal(st->hosts[k].module, module) && text_equal(st->hosts[k].name, name)) {
            if (!text_equal(st->hosts[k].signature, signature))
                return fail(st, PINAX_ERR_SIGNATURE, "extern: the function is already declared with another signature");
            *index = k;
            return PINAX_OK;
        }
    }
    if (st->n_hosts == PNX_MAX_HOSTS)
        return fail(st, PINAX_ERR_NO_ROOM, "too many host words and extern functions");
    if (!st->resolve)
        return fail(st, PINAX_ERR_NOT_FOUND, "extern: the host gave no resolver, so no C function can be named");
    const pinax_str library = {(const unsigned char *)module + 1, __builtin_strlen(module) - 1};
    const pinax_str symbol = {(const unsigned char *)name, __builtin_strlen(name)};
    made.ext = st->resolve(st->resolve_user, library, symbol);
    if (!made.ext)
        return fail(st, PINAX_ERR_NOT_FOUND, "extern: the host does not give this function");
    pnx_host *h = &st->hosts[st->n_hosts];
    *h = made;
    h->fn = extern_call;
    h->user = h;
    *index = st->n_hosts++;
    return PINAX_OK;
}

pinax_err pinax_handle_type(pinax_state *st, const char *name, void (*close)(void *object, void *user),
                            void *user, uint32_t *out_type) {
    if (!st || !name || !out_type)
        return PINAX_ERR_ARGUMENT;
    for (uint32_t k = 0; st->from_image && k < st->n_handle_types; ++k) { // given again: its number back
        if (text_equal(st->handle_types[k].name, name)) {
            st->handle_types[k].close = close;
            st->handle_types[k].user = user;
            *out_type = k + 1;
            return PINAX_OK;
        }
    }
    if (st->head)
        return fail(st, PINAX_ERR_STATE, "handle types must be given before code is loaded");
    if (st->n_handle_types == PNX_MAX_HANDLE_TYPES)
        return fail(st, PINAX_ERR_NO_ROOM, "too many handle types");
    for (uint32_t k = 0; k < st->n_handle_types; ++k) {
        if (text_equal(st->handle_types[k].name, name))
            return fail(st, PINAX_ERR_ARGUMENT, "a handle type of this name exists");
    }
    pnx_handle_type *type = &st->handle_types[st->n_handle_types];
    if (!copy_text(type->name, sizeof type->name, name))
        return fail(st, PINAX_ERR_ARGUMENT, "handle type: the name is too long");
    type->close = close;
    type->user = user;
    *out_type = ++st->n_handle_types;
    return PINAX_OK;
}

// The slot a handle of this type names, or NULL when it is stale or of another type.
static pnx_handle_slot *handle_slot(pinax_state *st, uint32_t type, pinax_handle handle) {
    const uint32_t slot = (uint32_t)handle - 1;
    if (!st || type == 0 || slot >= st->handle_slots)
        return NULL;
    pnx_handle_slot *h = &st->handles[slot];
    return h->type == type && h->generation == (uint32_t)(handle >> 32) ? h : NULL;
}

pinax_err pinax_handle_new(pinax_state *st, uint32_t type, void *object, pinax_handle *out) {
    if (!st || !out || type == 0 || type > st->n_handle_types)
        return PINAX_ERR_ARGUMENT;
    if (st->handle_free == 0)
        return fail(st, PINAX_ERR_FULL, "the handle table is full (config.handle_slots)");
    const uint32_t slot = st->handle_free - 1;
    pnx_handle_slot *h = &st->handles[slot];
    st->handle_free = h->next_free;
    ++h->generation; // odd: in use
    h->object = object;
    h->type = type;
    h->run = st->run_serial;
    if (++st->handles_open > st->handles_peak)
        st->handles_peak = st->handles_open;
    *out = (uint64_t)h->generation << 32 | (slot + 1u);
    return PINAX_OK;
}

void *pinax_handle_get(pinax_state *st, uint32_t type, pinax_handle handle) {
    const pnx_handle_slot *h = handle_slot(st, type, handle);
    return h ? h->object : NULL;
}

// The host took the object back (it closes it itself); the handle is dead from now on.
pinax_err pinax_handle_release(pinax_state *st, uint32_t type, pinax_handle handle) {
    pnx_handle_slot *h = handle_slot(st, type, handle);
    if (!h)
        return st ? fail(st, PINAX_ERR_STALE, "the handle is not open") : PINAX_ERR_ARGUMENT;
    handle_drop(st, (uint32_t)(h - st->handles), false);
    return PINAX_OK;
}

pinax_err pinax_global_find(pinax_state *st, const char *name, pinax_global *out) {
    if (!st || !name || !out)
        return PINAX_ERR_ARGUMENT;
    if (!st->head)
        return fail(st, PINAX_ERR_STATE, "no code loaded");
    const pnx_globalrec *recs = (const pnx_globalrec *)(const void *)(st->words + st->head->n_words);
    for (uint32_t k = 0; k < st->head->n_globals; ++k) {
        if (!text_equal(recs[k].name, name))
            continue;
        const pnx_global *g = &st->globals[k];
        if (g->kind == PNX_G_INDEX)
            break; // an index is not data of its own
        const bool buffer = g->kind == PNX_G_BUFFER;
        out->ptr = buffer ? g->ptr + 8 : g->ptr; // a buffer's bytes follow its 64-bit length
        out->element_bytes = buffer ? 1 : g->elem_bytes;
        out->capacity = g->capacity;
        out->type.ptr = (const unsigned char *)recs[k].type;
        out->type.len = __builtin_strlen(recs[k].type);
        out->number = k;
        return PINAX_OK;
    }
    return fail(st, PINAX_ERR_NOT_FOUND, "no such global");
}


// ---- images and clones (D0036) ------------------------------------------------------------
//
// An image is a head and the used part of the block, byte for byte. Everything in the block
// that names a place in it is an address; opening moves those addresses to where the block is
// now. What belongs to the process that saved it (host functions and their data, handle
// objects, bound memory, the static texts) is let go, and the host gives it again by name.

typedef struct pnx_image_head {
    uint8_t  magic[4]; // "PNXI"
    uint16_t format;   // PINAX_FORMAT
    uint16_t order;    // PNX_ORDER_MARK as the saving machine wrote it
    uint32_t head_bytes, reserved;
    uint64_t layout;    // image_layout() of the build that saved it
    uint64_t used;      // the bytes of the block that follow
    uint64_t base;      // where the block began when it was saved
    uint64_t block_len; // how long that block was
    uint8_t  spare[16];
} pnx_image_head;
static_assert(sizeof(pnx_image_head) == 64, "the head of an image keeps the state 16-byte aligned");

// FNV-1a over the sizes and a few places of what the block holds, and the numbers its code
// and texts depend on: a build that differs in any of them refuses the image.
static uint64_t image_layout(void) {
    const uint64_t parts[] = {sizeof(pinax_state), sizeof(pnx_task), sizeof(pnx_global), sizeof(pnx_host),
                              sizeof(pnx_handle_slot), sizeof(pnx_handle_type), sizeof(pnx_context),
                              sizeof(pnx_scell), sizeof(pnx_frame), sizeof(pnx_counts), sizeof(void *),
                              offsetof(pinax_state, symbols), offsetof(pinax_state, handles),
                              offsetof(pinax_state, globals), offsetof(pinax_state, main), offsetof(pnx_task, ctx),
                              offsetof(pnx_global, own), offsetof(pnx_global, shared), offsetof(pnx_host, fn), PNX_OP_COUNT, PNX_CONTAINER_VERSION,
                              PINAX_FORMAT, pnx_static_len, PNX_SEG_COUNT, PNX_MAX_HOSTS, PNX_MAX_HANDLE_TYPES};
    uint64_t hash = 0xcbf29ce484222325u;
    for (size_t k = 0; k < sizeof parts / sizeof parts[0]; ++k)
        hash = (hash ^ parts[k]) * 0x100000001b3u;
    return pnx_symbols_layout(hash);
}

size_t pinax_image_size(const pinax_state *st) {
    return st ? sizeof(pnx_image_head) + st->used : 0;
}

pinax_err pinax_image_save(const pinax_state *st, pinax_mem out, size_t *written) {
    if (!st || !out.ptr || !written)
        return PINAX_ERR_ARGUMENT;
    *written = 0;
    if (st->host_cur || st->nest || st->loading) // a host word runs, or a load is not finished
        return PINAX_ERR_STATE;
    const size_t size = pinax_image_size(st);
    if (out.len < size)
        return PINAX_ERR_NO_ROOM;
    const pnx_image_head head = {.magic = {'P', 'N', 'X', 'I'}, .format = PINAX_FORMAT, .order = PNX_ORDER_MARK,
                                 .head_bytes = sizeof head, .layout = image_layout(), .used = st->used,
                                 .base = (uint64_t)(uintptr_t)st->block, .block_len = st->block_len};
    __builtin_memcpy(out.ptr, &head, sizeof head);
    __builtin_memmove((uint8_t *)out.ptr + sizeof head, st->block, st->used);
    *written = size;
    return PINAX_OK;
}

// Addresses of the block as it was saved, from `lo` to `hi` (its end included), move by `by`.
// An address outside it is no address an image may hold: `bad` is set.
typedef struct moving {
    uintptr_t lo, hi;
    ptrdiff_t by;
    bool      bad;
} moving;

static void *moved(moving *m, const void *p) {
    const uintptr_t a = (uintptr_t)p;
    if (!p)
        return NULL;
    if (a < m->lo || a > m->hi) {
        m->bad = true;
        return NULL;
    }
    return (void *)(a + (uintptr_t)m->by);
}
#define MOVE(field) ((field) = (typeof(field))moved(&m, (const void *)(field)))

// A string value whose bytes are not in the block as it is now (a host's text, bound memory,
// anything an image cannot carry) becomes empty.
static void scrub_cells(pinax_state *st, pnx_scell *cells, uint32_t count) {
    const uintptr_t lo = (uintptr_t)st->block, hi = lo + st->used;
    for (uint32_t k = 0; cells && k < count; ++k) {
        pnx_scell *c = &cells[k];
        bool keep = c->segment == PNX_SEG_EMPTY || c->segment == PNX_SEG_DONE ? c->length == 0
                  : c->segment == PNX_SEG_STATIC ? c->offset <= pnx_static_len && c->length <= pnx_static_len - c->offset
                                                 : false;
        if (c->segment == PNX_SEG_CONST || c->segment == PNX_SEG_GLOBAL || c->segment == PNX_SEG_REGION) {
            const uintptr_t at = (uintptr_t)st->segment[c->segment] + (uintptr_t)c->offset;
            keep = c->offset < UINTPTR_MAX / 2 && at >= lo && at <= hi && c->length <= hi - at;
        }
        if (!keep)
            *c = (pnx_scell){PNX_SEG_EMPTY, 0, 0};
    }
}

pinax_err pinax_image_open(pinax_mem block, pinax_str image, const pinax_config *config, pinax_state **out) {
    if (!out)
        return PINAX_ERR_ARGUMENT;
    *out = NULL;
    const size_t without_key = offsetof(pinax_config, hash_key);
    if (!block.ptr || !image.ptr || !config ||
        (config->struct_size != sizeof *config && config->struct_size != without_key))
        return PINAX_ERR_ARGUMENT;
    pnx_image_head head;
    if (image.len < sizeof head)
        return PINAX_ERR_FORMAT;
    __builtin_memcpy(&head, image.ptr, sizeof head);
    if (__builtin_memcmp(head.magic, "PNXI", 4) != 0 || head.format != PINAX_FORMAT || head.order != PNX_ORDER_MARK ||
        head.head_bytes != sizeof head || head.layout != image_layout() || head.used < sizeof(pinax_state) ||
        head.used > image.len - sizeof head || image.len - sizeof head != head.used)
        return PINAX_ERR_FORMAT;
    const size_t used = (size_t)head.used;
    uint8_t     *body = (uint8_t *)block.ptr + sizeof head;
    const size_t skip = (size_t)(-(uintptr_t)body & 15u);
    if (block.len < sizeof head + skip + used)
        return PINAX_ERR_NO_ROOM;
    if ((const void *)image.ptr == block.ptr) { // in place: only when the block is not 16-byte aligned are bytes moved
        if (skip)
            __builtin_memmove(body + skip, body, used);
    } else {
        __builtin_memcpy(block.ptr, &head, sizeof head);
        __builtin_memmove(body + skip, image.ptr + sizeof head, used);
    }
    body += skip;
    pinax_state *st = (pinax_state *)(void *)body;
    moving       m = {(uintptr_t)head.base, (uintptr_t)head.base + used, (ptrdiff_t)((uintptr_t)body - (uintptr_t)head.base),
                      false};
    if (st->host_cur || st->nest || st->loading)
        return PINAX_ERR_FORMAT;
    st->block = body;
    st->block_len = block.len - sizeof head - skip;
    st->needed = 0;
    MOVE(st->ibank), MOVE(st->sbank), MOVE(st->ctrl), MOVE(st->region), MOVE(st->hosts), MOVE(st->handles);
    MOVE(st->head), MOVE(st->words), MOVE(st->importrecs), MOVE(st->iconst), MOVE(st->sconst), MOVE(st->code);
    MOVE(st->word_counts), MOVE(st->imports), MOVE(st->globals), MOVE(st->lines), MOVE(st->localrecs);
    MOVE(st->proven);
    MOVE(st->task), MOVE(st->main.ibank), MOVE(st->main.sbank), MOVE(st->main.ctrl), MOVE(st->main_wait.host);
    if (st->symbols) {
        const void *from = st->symbols;
        MOVE(st->symbols);
        if (!m.bad)
            pnx_symbols_move(st->symbols, from, m.by);
    }
    if (m.bad)
        return PINAX_ERR_FORMAT;
    for (uint32_t k = 0; k < PNX_SEG_COUNT; ++k) { // the segments in the block move; the rest are let go
        const uintptr_t at = (uintptr_t)st->segment[k];
        st->segment[k] = (k == PNX_SEG_CONST || k == PNX_SEG_GLOBAL || k == PNX_SEG_REGION) && at >= m.lo && at <= m.hi
                             ? (const uint8_t *)(at + (uintptr_t)m.by)
                             : empty_bytes;
    }
    st->segment[PNX_SEG_STATIC] = (const uint8_t *)pnx_static_text;
    st->segment[PNX_SEG_REGION] = st->region;
    st->host_segments = 0;
    for (uint32_t k = 0; st->head && k < st->head->n_imports; ++k)
        MOVE(st->imports[k]);
    for (uint32_t k = 0; st->head && k < st->head->n_globals; ++k) {
        pnx_global *g = &st->globals[k];
        if (g->shared) { // what another state lent is not in the image: the host gives it again
            unshared(g);
            continue;
        }
        MOVE(g->own);
        const uintptr_t at = (uintptr_t)g->ptr;
        g->ptr = at >= m.lo && at <= m.hi ? (uint8_t *)(at + (uintptr_t)m.by) : g->own; // bound: back on its own
        for (uint32_t slot = 0; g->kind == PNX_G_TASKS && slot < g->capacity; ++slot) {
            pnx_task *t = (pnx_task *)(void *)g->ptr + slot;
            MOVE(t->ctx.ibank), MOVE(t->ctx.sbank), MOVE(t->ctx.ctrl), MOVE(t->parent);
            MOVE(t->ibank), MOVE(t->sbank), MOVE(t->ctrl), MOVE(t->wait.host);
        }
    }
    if (m.bad)
        return PINAX_ERR_FORMAT;
    // what was the saving process's is let go: host words wait to be given again
    for (uint32_t k = 0; k < st->n_hosts; ++k) {
        pnx_host *h = &st->hosts[k];
        h->ext = NULL;
        h->fn = h->module[0] == '@' ? extern_call : NULL;
        h->user = h->module[0] == '@' ? h : NULL;
    }
    for (uint32_t k = 0; k < st->n_handle_types; ++k)
        st->handle_types[k].close = NULL, st->handle_types[k].user = NULL;
    for (uint32_t k = 0; k < st->handle_slots; ++k) { // closed without telling anyone: the objects were not ours
        if (st->handles[k].type)
            handle_drop(st, k, false);
    }
    st->host_cur = NULL, st->host_ia = NULL, st->host_sa = NULL;
    st->fault_ibank = NULL, st->fault_sbank = NULL;
    __builtin_memset(&st->before_load, 0, sizeof st->before_load);
    st->resolve = NULL, st->resolve_user = NULL;
    st->diag = config->diag, st->diag_user = config->diag_user;
    st->budget = config->budget;
    st->error_text = "";
    st->eval_n = 0;
    st->fault_n = 0;
    st->ptn = (pnx_ptn_found){0}; // the last match (ptn.capture, the groups of rex) does not survive an image
    scrub_cells(st, st->sbank, st->str_slots);
    scrub_cells(st, st->main.sbank, st->main.str_slots);
    for (uint32_t k = 0; st->head && k < st->head->n_globals; ++k) {
        const pnx_global *g = &st->globals[k];
        for (uint32_t slot = 0; g->kind == PNX_G_TASKS && slot < g->capacity; ++slot) {
            pnx_task *t = (pnx_task *)(void *)g->ptr + slot;
            scrub_cells(st, t->sbank, t->str_slots);
            t->ptn = (pnx_ptn_found){0};
        }
    }
    st->from_image = true;
    st->frozen = st->load_runs = false; // what the saved state lent stays with it
    // operations a run waited for will not be completed by this process: they come back interrupted
    if (st->main_wait.token)
        (void)pinax_complete(st, st->main_wait.token, PINAX_ERR_INTERRUPTED, NULL);
    for (uint32_t k = 0; st->head && k < st->head->n_globals; ++k) {
        const pnx_global *g = &st->globals[k];
        for (uint32_t slot = 0; g->kind == PNX_G_TASKS && slot < g->capacity; ++slot) {
            const pnx_task *t = (pnx_task *)(void *)g->ptr + slot;
            if (t->state == PNX_TASK_WAITING && t->wait.token)
                (void)pinax_complete(st, t->wait.token, PINAX_ERR_INTERRUPTED, NULL);
        }
    }
    st->error_text = "";
    *out = st;
    return PINAX_OK;
}
#undef MOVE

pinax_err pinax_clone(const pinax_state *source, pinax_mem block, pinax_state **out) {
    if (!source || !out)
        return PINAX_ERR_ARGUMENT;
    *out = NULL;
    size_t          written = 0;
    const pinax_err err = pinax_image_save(source, block, &written);
    if (err != PINAX_OK)
        return err;
    const pinax_config config = {.struct_size = sizeof config, .budget = source->budget, .diag = source->diag,
                                 .diag_user = source->diag_user};
    return pinax_image_open(block, (pinax_str){(const unsigned char *)block.ptr, written}, &config, out);
}

// A state opened from an image runs only when every host word and extern function of the
// image is given again.
static pinax_err hosts_given(pinax_state *st) {
    for (uint32_t k = 0; st->from_image && k < st->n_hosts; ++k) {
        const pnx_host *h = &st->hosts[k];
        if (h->module[0] == '@' ? !h->ext : !h->fn)
            return fail(st, PINAX_ERR_NOT_FOUND, h->module[0] == '@'
                                                     ? "an extern function of the image is not given again: set a resolver"
                                                     : "a host word of the image is not given again: register it");
    }
    return PINAX_OK;
}

pinax_err pinax_fault_local(const pinax_state *st, uint32_t index, pinax_str *name, pinax_value *out) {
    if (!st || !name || !out)
        return PINAX_ERR_ARGUMENT;
    if (index >= st->fault_n)
        return PINAX_ERR_NOT_FOUND;
    const struct pnx_faultlocal *l = &st->fault_locals[index];
    *name = (pinax_str){st->fault_pool + l->name, l->name_len};
    *out = (pinax_value){.kind = l->kind, .i = l->i, .s = {st->fault_pool + l->text, l->text_len},
                         .type = {st->fault_pool + l->type, l->type_len}};
    if (l->kind == PINAX_VALUE_ERR) {
        uint32_t offset, length;
        pnx_err_name(l->i, &offset, &length);
        out->s = (pinax_str){(const unsigned char *)pnx_static_text + offset, length};
    }
    return PINAX_OK;
}
