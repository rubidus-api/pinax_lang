// pinax_state.c - the part of the host interface the P0 interpreter implements (RFC-0010):
// a state inside the host's block, host words, loading and checking a bytecode container,
// finding and running words. The rest of pinax.h is declared but answers PINAX_ERR_STATE.

#include "pinax_internal.h"

#include <stddef.h>

static const uint8_t empty_bytes[1] = {0};

const pnx_opinfo pnx_ops_info[PNX_OP_COUNT] = {
#define PNX_INFO(name, a, b, c, shape, second, fails) [PNX_##name] = {a, b, c, shape, second, fails},
    PNX_OPS(PNX_INFO)
#undef PNX_INFO
};
#define opinfo pnx_ops_info

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

pnx_program pnx_program_now(const pinax_state *st) {
    return (pnx_program){st->head, st->words, st->importrecs, st->iconst, st->sconst, st->code, st->word_counts,
                         st->imports, st->globals, st->segment[PNX_SEG_CONST], st->segment[PNX_SEG_GLOBAL],
                         st->symbols, st->used, st->n_hosts};
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
        if (op >= PNX_OP_COUNT)
            return "code: unknown instruction";
        heads[i] = 1;
        last = i;
        i += opinfo[op].second ? 2 : 1;
        if (i > range.end)
            return "code: instruction cut off";
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
            if (!reg_ok(info.a, a, word, head->n_globals) || bx >= head->n_globals)
                return "code: global outside its table";
            break;
        default: // PNX_F_AX
            target = (int64_t)i + 1 + ((int32_t)w >> 8);
            break;
        }
        if (target >= 0 || info.c == PNX_K_J || info.shape == PNX_F_AX) {
            if (target < range.begin || target >= range.end || !heads[target])
                return "code: jump outside the word or into an instruction";
        }

        // what each instruction needs of the global it names
        uint32_t want = PNX_G_COUNT, global = 0;
        switch (op) {
        case PNX_GETG: case PNX_SETG: want = PNX_G_INT; global = bx; break;
        case PNX_AGET: case PNX_ASET: want = PNX_G_ARRAY; global = c; break; // or a list, below
        case PNX_GLEN: case PNX_GCLEAR: want = PNX_G_COUNT + 1; global = bx; break; // any collection
        case PNX_ADDROW: want = PNX_G_ROWS; global = bx; break;
        case PNX_BCAT: case PNX_BCATI: want = PNX_G_BUFFER; global = bx; break;
        case PNX_QPUT: case PNX_QTAKE: case PNX_QPEEK: want = PNX_G_QUEUE; global = bx; break;
        case PNX_PALLOC: case PNX_PFREE: want = PNX_G_POOL; global = bx; break;
        case PNX_PALIVE: want = PNX_G_POOL; global = c; break;
        case PNX_IXPUT: case PNX_IXDEL: want = PNX_G_INDEX; global = bx; break;
        case PNX_IXGET: case PNX_IXGETS: want = PNX_G_INDEX; global = c; break;
        case PNX_LPUSH: case PNX_LPOP: want = PNX_G_LIST; global = bx; break;
        case PNX_ROW: want = PNX_G_TABLE; global = c; break;
        case PNX_BCLEAR: case PNX_BVIEW: want = PNX_G_BUFFER; global = bx; break;
        case PNX_BPUT: case PNX_BPUTS: want = PNX_G_BUFFER; global = c; break;
        case PNX_FGET: case PNX_FSET: case PNX_FGETS: case PNX_FSETS: want = PNX_G_TABLE; global = x & 0xfffu; break;
        default: break;
        }
        if (want != PNX_G_COUNT) {
            if (global >= head->n_globals)
                return "code: global outside its table";
            const uint32_t kind = st->globals[global].kind;
            const bool ok = kind == want || (want == PNX_G_ARRAY && kind == PNX_G_LIST) ||
                            (want == PNX_G_TABLE && (kind == PNX_G_ROWS || (kind == PNX_G_POOL && op != PNX_ROW))) ||
                            (want == PNX_G_COUNT + 1 && kind != PNX_G_INT && kind != PNX_G_BUFFER &&
                             kind != PNX_G_INDEX);
            if (!ok)
                return "code: global of the wrong kind";
            if ((op == PNX_IXGET && st->globals[global].key_is_str) ||
                (op == PNX_IXGETS && !st->globals[global].key_is_str))
                return "code: key of the wrong kind for this index";
        }
        if (info.second == PNX_X_FIELD) {
            const uint32_t offset = (x >> 12) & 0xfffu, extra = x >> 24;
            const bool is_text = op == PNX_FGETS || op == PNX_FSETS;
            if (!is_text && extra >= PNX_W_COUNT)
                return "code: unknown field width";
            if (offset + (is_text ? 1u + extra : width_bytes[extra]) > st->globals[global].elem_bytes)
                return "code: field outside its record";
        }
        if ((op == PNX_BPUT && b >= PNX_W_COUNT) || (op == PNX_SGET && c >= PNX_W_COUNT))
            return "code: unknown width";
        if (op == PNX_SREPL && b + 2 >= word->str_cells)
            return "code: register outside the window";

        if (info.second == PNX_X_WORD) {
            if (x >= head->n_words)
                return "code: call of a word outside the table";
            if (op == PNX_CALL)
                fails = st->word_counts[x].fails;
            else if (st->word_counts[x].fails != counts.fails)
                return "code: tail call between a failing and a non-failing word";
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
        if (fails && (next >= range.end || !is_failure_word(code[next] & 0xffu)))
            return "code: no failure word after an operation that can fail";
        if ((op == PNX_RETOK || op == PNX_RETF || op == PNX_FAIL || op == PNX_FAILR) && !counts.fails)
            return "code: failure return in a word without `or fail`";
        if (op == PNX_RET && counts.fails)
            return "code: plain return in a word with `or fail`";
        i = next;
    }
    return NULL;
}

pinax_err pinax_load_bytecode(pinax_state *st, pinax_str bytecode) {
    if (!st || !bytecode.ptr)
        return PINAX_ERR_ARGUMENT;
    if (st->head)
        return fail(st, PINAX_ERR_STATE, "a state takes one bytecode container; sources can be added with pinax_load");
    return pnx_load_container(st, bytecode, 0);
}

pinax_err pnx_load_container(pinax_state *st, pinax_str bytecode, uint32_t keep) {
    const pnx_program before = pnx_program_now(st);
    pnx_head h;
    if (bytecode.len < sizeof h)
        return fail(st, PINAX_ERR_FORMAT, "bytecode: too short");
    __builtin_memcpy(&h, bytecode.ptr, sizeof h);
    if (__builtin_memcmp(h.magic, PNX_MAGIC, 4) != 0 || h.version != PNX_CONTAINER_VERSION ||
        h.order != PNX_ORDER_MARK)
        return fail(st, PINAX_ERR_FORMAT, "bytecode: not a version 0 container of this byte order");
    const uint64_t total = sizeof h + (uint64_t)h.n_words * sizeof(pnx_wordrec) +
                           (uint64_t)h.n_globals * sizeof(pnx_globalrec) +
                           (uint64_t)h.n_imports * sizeof(pnx_importrec) + (uint64_t)h.n_iconst * 8 +
                           (uint64_t)h.n_sconst * sizeof(pnx_sconst) + (uint64_t)h.n_code * 4 + h.n_strbytes;
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
    st->imports = st->word_counts ? take(st, sizeof(pnx_host *) * (h.n_imports + 1u), 8) : NULL;
    st->globals = st->imports ? take(st, sizeof(pnx_global) * (h.n_globals + 1u), 8) : NULL;
    if (!st->globals)
        goto rollback;

    err = PINAX_ERR_FORMAT;
    why = "bytecode: bad word record";
    for (uint32_t k = 0; k < h.n_words; ++k) {
        const pnx_wordrec *word = &st->words[k];
        if (word->counted)
            st->word_counts[k] = (pnx_counts){word->int_args, word->str_args, word->int_results, word->str_results,
                                              word->fails != 0};
        if (!terminated(word->name, sizeof word->name) || !terminated(word->signature, sizeof word->signature) ||
            (!word->counted && !pnx_signature_counts(word->signature, &st->word_counts[k])) ||
            st->word_counts[k].str_args > PNX_MAX_STR_ARGS || st->word_counts[k].str_results > PNX_MAX_STR_RESULTS ||
            word->entry >= h.n_code ||
            (k > 0 && word->entry <= st->words[k - 1].entry) || (k == 0 && word->entry != 0) ||
            word->int_cells > 256 || word->str_cells > 256)
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
        case PNX_G_INT: bytes = 8; break;
        case PNX_G_ARRAY:
        case PNX_G_LIST:
        case PNX_G_QUEUE: bytes = (uint64_t)rec->capacity * 8; break;
        case PNX_G_BUFFER: bytes = (uint64_t)rec->capacity + 8; break;
        case PNX_G_TABLE:
        case PNX_G_ROWS:
            if (rec->elem_bytes == 0)
                goto rollback;
            bytes = (uint64_t)rec->capacity * rec->elem_bytes;
            break;
        case PNX_G_POOL:
            if (rec->elem_bytes == 0)
                goto rollback;
            bytes = (uint64_t)rec->capacity * ((uint64_t)rec->elem_bytes + 8);
            break;
        case PNX_G_INDEX: { // slots: a power of two; the key field must lie inside an earlier table
            const uint32_t table = rec->elem_bytes & 0xfffu, offset = (rec->elem_bytes >> 12) & 0xfffu,
                           extra = rec->elem_bytes >> 24;
            if ((rec->capacity & (rec->capacity - 1)) != 0 || table >= k || rec->reserved > 1 ||
                (st->globals[table].kind != PNX_G_TABLE && st->globals[table].kind != PNX_G_ROWS) ||
                (rec->reserved == 0 && extra >= PNX_W_COUNT) ||
                offset + (rec->reserved ? 1u + extra : width_bytes[extra]) > st->globals[table].elem_bytes)
                goto rollback;
            bytes = (uint64_t)rec->capacity * 4;
            break;
        }
        default: goto rollback;
        }
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
        g->ptr = take(st, (size_t)bytes, 8);
        if (!g->ptr)
            goto rollback;
        g->kind = rec->kind;
        g->elem_bytes = rec->kind == PNX_G_TABLE || rec->kind == PNX_G_ROWS || rec->kind == PNX_G_POOL ? rec->elem_bytes : 8;
        g->capacity = rec->kind == PNX_G_INT ? 1 : rec->capacity;
        g->length = rec->kind == PNX_G_LIST || rec->kind == PNX_G_ROWS || rec->kind == PNX_G_QUEUE ? 0 : g->capacity;
        if (rec->kind == PNX_G_POOL) { // every slot is free: slot k links to k + 1
            uint8_t *meta = g->ptr + (size_t)g->capacity * g->elem_bytes;
            for (uint32_t slot = 0; slot < g->capacity; ++slot) {
                const uint32_t next = slot + 1 < g->capacity ? slot + 2 : 0;
                __builtin_memcpy(meta + (size_t)slot * 8 + 4, &next, 4);
            }
            g->head = 1;
        }
        if (rec->kind == PNX_G_INDEX) {
            g->key = rec->elem_bytes;
            g->key_is_str = rec->reserved != 0;
            g->elem_bytes = 4;
        }
        if (!first_global)
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
    }
    if (h.n_words == 0 && h.n_code != 0) {
        why = "bytecode: code without words";
        goto rollback;
    }
    st->region_used = 0;
    return PINAX_OK;

rollback:
    (void)mark;
    pnx_program_restore(st, &before);
    return fail(st, err, why);
}

// ---- finding and running -----------------------------------------------------------------

pinax_err pinax_find(pinax_state *st, const char *name, const char *signature, uint32_t flags, pinax_word *out) {
    (void)flags; // nothing yields in P0, so every word may be run from a host word
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

static pinax_err deliver(pinax_state *st, pinax_err err, const pinax_args *results) {
    if (err != PINAX_OK) {
        if (err != PINAX_SUSPENDED) {
            st->error_text = "the script ended with a fault or a failure";
            handles_close(st, st->run_serial); // what this run opened and still holds
        }
        return err;
    }
    const pnx_counts n = st->word_counts[st->top_word];
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

pinax_err pinax_run(pinax_state *st, pinax_word word, const pinax_args *args) {
    if (!st)
        return PINAX_ERR_ARGUMENT;
    if (!st->head || st->suspended)
        return fail(st, PINAX_ERR_STATE, "no code loaded, or a suspended run must be resumed first");
    if (word >= st->head->n_words)
        return fail(st, PINAX_ERR_ARGUMENT, "not a word of this state");
    const pnx_wordrec *rec = &st->words[word];
    const pnx_counts n = st->word_counts[word];
    if ((n.int_args && !(args && args->i)) || (n.str_args && !(args && args->s)))
        return fail(st, PINAX_ERR_ARGUMENT, "arguments missing");
    if (rec->int_cells > st->int_slots || rec->str_cells > st->str_slots)
        return fail(st, PINAX_ERR_DEPTH, "the word's window does not fit the banks");

    st->region_used = 0; // results of the previous run end here
    st->host_segments = n.str_args;
    for (uint32_t k = 0; k < n.int_args; ++k)
        st->ibank[k] = args->i[k];
    for (uint32_t k = 0; k < n.str_args; ++k) {
        if (args->s[k].len > UINT32_MAX)
            return fail(st, PINAX_ERR_ARGUMENT, "a string argument is longer than 4 GiB");
        st->segment[PNX_SEG_HOST + k] = args->s[k].len ? args->s[k].ptr : empty_bytes;
        st->sbank[k] = (pnx_scell){PNX_SEG_HOST + k, (uint32_t)args->s[k].len, 0};
    }
    st->top_word = word;
    if (++st->run_serial == 0)
        st->run_serial = 1;
    st->pc = rec->entry;
    st->int_base = st->str_base = st->depth = 0;
    st->left = st->budget;
    st->charge = 0;
    st->fail = PINAX_ERR_FAILED;
    return deliver(st, pnx_execute(st), args);
}

pinax_err pinax_resume(pinax_state *st, const pinax_args *results) {
    if (!st)
        return PINAX_ERR_ARGUMENT;
    if (!st->suspended)
        return fail(st, PINAX_ERR_STATE, "nothing is suspended");
    st->left = st->budget > st->owed ? st->budget : st->owed;
    st->owed = 0;
    const pinax_err err = deliver(st, pnx_execute(st), results);
    if (st->loading && err != PINAX_SUSPENDED) { // the load ends here: done, or failed and undone
        st->loading = false;
        if (err != PINAX_OK) {
            const char *text = st->error_text;
            const pnx_program before = st->before_load;
            pnx_program_restore(st, &before);
            st->error_text = text;
        }
    }
    return err;
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
    out->handles_peak = st->handles_peak;
    out->instructions = st->instructions; // an upper bound, see CHECK
    out->block_bytes_used = st->used;
}

// ---- host memory as globals ---------------------------------------------------------------

// P0: scalars and integer arrays only. A bound buffer would need a segment of its own.
pinax_err pinax_bind(pinax_state *st, const char *global, void *memory, size_t bytes) {
    if (!st || !global || !memory)
        return PINAX_ERR_ARGUMENT;
    if (!st->head)
        return fail(st, PINAX_ERR_STATE, "no code loaded");
    const pnx_globalrec *recs = (const pnx_globalrec *)(const void *)(st->words + st->head->n_words);
    for (uint32_t k = 0; k < st->head->n_globals; ++k) {
        if (!text_equal(recs[k].name, global))
            continue;
        pnx_global *g = &st->globals[k];
        if (g->kind != PNX_G_INT && g->kind != PNX_G_ARRAY)
            return fail(st, PINAX_ERR_STATE, "P0 binds integers and integer arrays only");
        if (bytes != (size_t)g->capacity * 8)
            return fail(st, PINAX_ERR_ARGUMENT, "the memory does not have the declared size");
        g->ptr = memory;
        return PINAX_OK;
    }
    return fail(st, PINAX_ERR_NOT_FOUND, "no such global");
}

// ---- declared in pinax.h, not in P0 ---------------------------------------------------------

pinax_token pinax_call_pending(pinax_call *call) {
    (void)call;
    return 0;
}

pinax_err pinax_complete(pinax_state *st, pinax_token token, pinax_err outcome, const pinax_args *results) {
    (void)st, (void)token, (void)outcome, (void)results;
    return PINAX_ERR_STATE;
}

void pinax_set_resolver(pinax_state *st, pinax_resolve_fn resolve, void *user) {
    if (st) {
        st->resolve = resolve;
        st->resolve_user = user;
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
                st->region_used = region_mark;
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
                st->region_used = region_mark;
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
    st->region_used = region_mark;
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
        return PINAX_OK;
    }
    return fail(st, PINAX_ERR_NOT_FOUND, "no such global");
}

pinax_err pinax_eval(pinax_state *st, pinax_str line) {
    (void)st, (void)line;
    return PINAX_ERR_STATE;
}

size_t pinax_image_size(const pinax_state *st) {
    (void)st;
    return 0;
}

pinax_err pinax_image_save(const pinax_state *st, pinax_mem out, size_t *written) {
    (void)st, (void)out, (void)written;
    return PINAX_ERR_STATE;
}

pinax_err pinax_image_open(pinax_mem block, pinax_str image, const pinax_config *config, pinax_state **out) {
    (void)block, (void)image, (void)config, (void)out;
    return PINAX_ERR_STATE;
}

pinax_err pinax_clone(const pinax_state *source, pinax_mem block, pinax_state **out) {
    (void)source, (void)block, (void)out;
    return PINAX_ERR_STATE;
}
