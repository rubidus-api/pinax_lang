// pinax_vm.c - the P0 interpreter loop (RFC-0010).
//
// Registers are typed by the instruction, so nothing is tagged and nothing is checked at
// run time except what the language defines as a fault: overflow, division by zero, an
// index outside a declared capacity, the depth limit.
//
// Dispatch is computed goto; define PINAX_SWITCH_DISPATCH for a plain switch. The budget is
// charged by CHECK instructions before backward branches and calls; define
// PINAX_BUDGET_EACH to charge every instruction instead.

#include "pinax_internal.h"

#include <stdckdint.h>

#if !defined(PINAX_SWITCH_DISPATCH)
#pragma GCC diagnostic ignored "-Wpedantic" // labels as values
#endif

#define OP_A(w) (((w) >> 8) & 0xffu)
#define OP_B(w) (((w) >> 16) & 0xffu)
#define OP_C(w) ((w) >> 24)
#define OP_BX(w) ((w) >> 16)
#define OP_SBX(w) ((int32_t)(int16_t)((w) >> 16))
#define OP_SB(w) ((int32_t)(int8_t)(((w) >> 16) & 0xffu))
#define OP_SC(w) ((int32_t)(int8_t)((w) >> 24))
#define OP_SAX(w) ((int32_t)(w) >> 8)

static const uint8_t width_bytes[PNX_W_COUNT] = {1, 2, 4, 8, 1, 2, 4};

static inline int64_t load_width(const uint8_t *p, uint32_t width) {
    switch (width) {
    case PNX_W_U8: return *p;
    case PNX_W_U16: { uint16_t v; __builtin_memcpy(&v, p, 2); return v; }
    case PNX_W_U32: { uint32_t v; __builtin_memcpy(&v, p, 4); return v; }
    case PNX_W_I8: return (int8_t)*p;
    case PNX_W_I16: { int16_t v; __builtin_memcpy(&v, p, 2); return v; }
    case PNX_W_I32: { int32_t v; __builtin_memcpy(&v, p, 4); return v; }
    default: { int64_t v; __builtin_memcpy(&v, p, 8); return v; }
    }
}

static inline bool fits_width(int64_t v, uint32_t width) {
    switch (width) {
    case PNX_W_U8: return v >= 0 && v <= UINT8_MAX;
    case PNX_W_U16: return v >= 0 && v <= UINT16_MAX;
    case PNX_W_U32: return v >= 0 && v <= UINT32_MAX;
    case PNX_W_I8: return v >= INT8_MIN && v <= INT8_MAX;
    case PNX_W_I16: return v >= INT16_MIN && v <= INT16_MAX;
    case PNX_W_I32: return v >= INT32_MIN && v <= INT32_MAX;
    default: return true;
    }
}

static inline const uint8_t *view_bytes(const pinax_state *st, pnx_scell cell) {
    return st->segment[cell.segment] + cell.offset;
}

// Decimal integer with an optional sign; the whole view must be the number.
static bool parse_int(const uint8_t *p, uint32_t len, int64_t *out) {
    uint32_t i = 0;
    bool negative = false;
    if (len > 0 && (p[0] == '-' || p[0] == '+')) {
        negative = p[0] == '-';
        i = 1;
    }
    if (i == len)
        return false;
    int64_t value = 0;
    for (; i < len; ++i) {
        unsigned digit = (unsigned)p[i] - '0';
        if (digit > 9)
            return false;
        if (ckd_mul(&value, value, 10))
            return false;
        if (negative ? ckd_sub(&value, value, (int64_t)digit) : ckd_add(&value, value, (int64_t)digit))
            return false;
    }
    *out = value;
    return true;
}

uint8_t *pnx_region_alloc(pinax_state *st, size_t bytes) {
    if (bytes > st->region_len - st->region_used)
        return NULL;
    uint8_t *p = st->region + st->region_used;
    st->region_used += (uint32_t)bytes;
    return p;
}

// Calls a host word. Arguments start at ia / sa; results follow the arguments. Returns the
// host's outcome; *fault is set when the interpreter itself cannot go on.
static pinax_err host_call(pinax_state *st, const pnx_host *h, int64_t *ia, pnx_scell *sa, bool *fault) {
    pinax_str sv[PNX_MAX_STR_ARGS];
    pinax_str rs[PNX_MAX_STR_RESULTS];
    const pnx_counts n = h->counts;
    for (uint32_t k = 0; k < n.str_args; ++k) {
        sv[k].ptr = view_bytes(st, sa[k]);
        sv[k].len = sa[k].length;
    }
    for (uint32_t k = 0; k < n.str_results; ++k) {
        rs[k].ptr = NULL;
        rs[k].len = 0;
    }
    pinax_args args = {ia, ia + n.int_args, sv, rs, NULL, NULL};
    pinax_call call = {st};
    pinax_err err = h->fn(&call, h->user, &args);
    *fault = false;
    if (err != PINAX_OK) {
        if (err < PINAX_ERR_FAILED) { // suspending or pending host words are not in P0
            *fault = true;
            return PINAX_ERR_STATE;
        }
        return err;
    }
    for (uint32_t k = 0; k < n.str_results; ++k) {
        pnx_scell *cell = &sa[n.str_args + k];
        if (rs[k].len > UINT32_MAX) {
            *fault = true;
            return PINAX_ERR_RANGE;
        }
        uint32_t len = (uint32_t)rs[k].len;
        if (len == 0) {
            *cell = (pnx_scell){PNX_SEG_EMPTY, 0, 0};
        } else if (rs[k].ptr >= st->region && rs[k].ptr + len <= st->region + st->region_used) {
            *cell = (pnx_scell){PNX_SEG_REGION, len, (uint64_t)(rs[k].ptr - st->region)};
        } else if (h->flags & PINAX_WORD_KEEP_STR) {
            if (PNX_SEG_HOST + st->host_segments >= PNX_SEG_COUNT) {
                *fault = true;
                return PINAX_ERR_FULL;
            }
            uint32_t segment = PNX_SEG_HOST + st->host_segments++;
            st->segment[segment] = rs[k].ptr;
            *cell = (pnx_scell){segment, len, 0};
        } else {
            uint8_t *copy = pnx_region_alloc(st, len);
            if (!copy) {
                *fault = true;
                return PINAX_ERR_FULL;
            }
            __builtin_memcpy(copy, rs[k].ptr, len);
            *cell = (pnx_scell){PNX_SEG_REGION, len, (uint64_t)(copy - st->region)};
        }
    }
    return PINAX_OK;
}

pinax_err pnx_execute(pinax_state *st) {
    const uint32_t *const code = st->code;
    const uint32_t *pc = code + st->pc;
    uint32_t ibase = st->int_base, sbase = st->str_base, depth = st->depth;
    int64_t *R = st->ibank + ibase;
    pnx_scell *S = st->sbank + sbase;
    const uint64_t budget = st->budget;
    uint64_t left = st->left, count = 0;
    uint32_t w = 0;
    pinax_err result = PINAX_OK;

#define FAULT(error)                                                                               \
    do {                                                                                           \
        result = (error);                                                                          \
        goto finished;                                                                             \
    } while (0)

#if defined(PINAX_BUDGET_EACH)
#define BUDGET_STEP()                                                                              \
    do {                                                                                           \
        ++count;                                                                                   \
        if (budget) {                                                                              \
            if (left == 0)                                                                         \
                goto suspended;                                                                    \
            --left;                                                                                \
        }                                                                                          \
    } while (0)
#else
#define BUDGET_STEP() ((void)0)
#endif

#if defined(PINAX_SWITCH_DISPATCH)
#define CASE(name) case PNX_##name:
#define NEXT() continue
    for (;;) {
        BUDGET_STEP();
        w = *pc++;
        switch (w & 0xffu) {
#else
    static const void *const labels[PNX_OP_COUNT] = {
#define PNX_LABEL(name, a, b, c, shape, second, fails) [PNX_##name] = &&L_##name,
        PNX_OPS(PNX_LABEL)
#undef PNX_LABEL
    };
#define CASE(name) L_##name:
#define NEXT()                                                                                     \
    do {                                                                                           \
        BUDGET_STEP();                                                                             \
        w = *pc++;                                                                                 \
        goto *labels[w & 0xffu];                                                                   \
    } while (0)
    NEXT();
    {
        {
#endif

        CASE(MOVI) { R[OP_A(w)] = R[OP_B(w)]; NEXT(); }
        CASE(MOVS) { S[OP_A(w)] = S[OP_B(w)]; NEXT(); }
        CASE(LOADI) { R[OP_A(w)] = OP_SBX(w); NEXT(); }
        CASE(LOADK) { R[OP_A(w)] = st->iconst[OP_BX(w)]; NEXT(); }
        CASE(LOADKS) {
            const pnx_sconst k = st->sconst[OP_BX(w)];
            S[OP_A(w)] = (pnx_scell){PNX_SEG_CONST, k.length, k.offset};
            NEXT();
        }

        CASE(ADD) { if (ckd_add(&R[OP_A(w)], R[OP_B(w)], R[OP_C(w)])) FAULT(PINAX_ERR_OVERFLOW); NEXT(); }
        CASE(SUB) { if (ckd_sub(&R[OP_A(w)], R[OP_B(w)], R[OP_C(w)])) FAULT(PINAX_ERR_OVERFLOW); NEXT(); }
        CASE(MUL) { if (ckd_mul(&R[OP_A(w)], R[OP_B(w)], R[OP_C(w)])) FAULT(PINAX_ERR_OVERFLOW); NEXT(); }
        CASE(DIV) {
            const int64_t a = R[OP_B(w)], b = R[OP_C(w)];
            if (b == 0 || (a == INT64_MIN && b == -1))
                FAULT(PINAX_ERR_OVERFLOW);
            R[OP_A(w)] = a / b;
            NEXT();
        }
        CASE(MOD) {
            const int64_t a = R[OP_B(w)], b = R[OP_C(w)];
            if (b == 0)
                FAULT(PINAX_ERR_OVERFLOW);
            R[OP_A(w)] = b == -1 ? 0 : a % b;
            NEXT();
        }
        CASE(NEG) { if (ckd_sub(&R[OP_A(w)], (int64_t)0, R[OP_B(w)])) FAULT(PINAX_ERR_OVERFLOW); NEXT(); }
        CASE(ADDI) { if (ckd_add(&R[OP_A(w)], R[OP_B(w)], (int64_t)OP_SC(w))) FAULT(PINAX_ERR_OVERFLOW); NEXT(); }
        CASE(WADD) { R[OP_A(w)] = (int64_t)((uint64_t)R[OP_B(w)] + (uint64_t)R[OP_C(w)]); NEXT(); }
        CASE(WSUB) { R[OP_A(w)] = (int64_t)((uint64_t)R[OP_B(w)] - (uint64_t)R[OP_C(w)]); NEXT(); }
        CASE(WMUL) { R[OP_A(w)] = (int64_t)((uint64_t)R[OP_B(w)] * (uint64_t)R[OP_C(w)]); NEXT(); }
        CASE(AND) { R[OP_A(w)] = R[OP_B(w)] & R[OP_C(w)]; NEXT(); }
        CASE(OR) { R[OP_A(w)] = R[OP_B(w)] | R[OP_C(w)]; NEXT(); }
        CASE(XOR) { R[OP_A(w)] = R[OP_B(w)] ^ R[OP_C(w)]; NEXT(); }
        CASE(SHL) { R[OP_A(w)] = (int64_t)((uint64_t)R[OP_B(w)] << (R[OP_C(w)] & 63)); NEXT(); }
        CASE(SHR) { R[OP_A(w)] = R[OP_B(w)] >> (R[OP_C(w)] & 63); NEXT(); }
        CASE(USHR) { R[OP_A(w)] = (int64_t)((uint64_t)R[OP_B(w)] >> (R[OP_C(w)] & 63)); NEXT(); }

        CASE(JEQ) { if (R[OP_A(w)] == R[OP_B(w)]) pc += OP_SC(w); NEXT(); }
        CASE(JNE) { if (R[OP_A(w)] != R[OP_B(w)]) pc += OP_SC(w); NEXT(); }
        CASE(JLT) { if (R[OP_A(w)] < R[OP_B(w)]) pc += OP_SC(w); NEXT(); }
        CASE(JLE) { if (R[OP_A(w)] <= R[OP_B(w)]) pc += OP_SC(w); NEXT(); }
        CASE(JEQI) { if (R[OP_A(w)] == OP_SB(w)) pc += OP_SC(w); NEXT(); }
        CASE(JNEI) { if (R[OP_A(w)] != OP_SB(w)) pc += OP_SC(w); NEXT(); }
        CASE(JLTI) { if (R[OP_A(w)] < OP_SB(w)) pc += OP_SC(w); NEXT(); }
        CASE(JGEI) { if (R[OP_A(w)] >= OP_SB(w)) pc += OP_SC(w); NEXT(); }
        CASE(JMP) { pc += OP_SAX(w); NEXT(); }

        CASE(GETG) { __builtin_memcpy(&R[OP_A(w)], st->globals[OP_BX(w)].ptr, 8); NEXT(); }
        CASE(SETG) { __builtin_memcpy(st->globals[OP_BX(w)].ptr, &R[OP_A(w)], 8); NEXT(); }
        CASE(AGET) {
            const pnx_global *g = &st->globals[OP_C(w)];
            const uint64_t index = (uint64_t)R[OP_B(w)];
            if (index >= g->capacity)
                FAULT(PINAX_ERR_RANGE);
            __builtin_memcpy(&R[OP_A(w)], g->ptr + index * 8, 8);
            NEXT();
        }
        CASE(ASET) {
            const pnx_global *g = &st->globals[OP_C(w)];
            const uint64_t index = (uint64_t)R[OP_B(w)];
            if (index >= g->capacity)
                FAULT(PINAX_ERR_RANGE);
            __builtin_memcpy(g->ptr + index * 8, &R[OP_A(w)], 8);
            NEXT();
        }
        CASE(ROW) {
            const uint64_t index = (uint64_t)R[OP_B(w)];
            if (index >= st->globals[OP_C(w)].capacity)
                FAULT(PINAX_ERR_RANGE);
            R[OP_A(w)] = (int64_t)index;
            NEXT();
        }
        CASE(FGET) {
            const uint32_t x = *pc++;
            const pnx_global *g = &st->globals[x & 0xfffu];
            const uint64_t row = (uint64_t)R[OP_B(w)];
            if (row >= g->capacity)
                FAULT(PINAX_ERR_RANGE);
            R[OP_A(w)] = load_width(g->ptr + row * g->elem_bytes + ((x >> 12) & 0xfffu), x >> 24);
            NEXT();
        }
        CASE(FSET) {
            const uint32_t x = *pc++;
            const pnx_global *g = &st->globals[x & 0xfffu];
            const uint64_t row = (uint64_t)R[OP_B(w)];
            const int64_t value = R[OP_A(w)];
            if (row >= g->capacity)
                FAULT(PINAX_ERR_RANGE);
            if (!fits_width(value, x >> 24))
                FAULT(PINAX_ERR_OVERFLOW);
            __builtin_memcpy(g->ptr + row * g->elem_bytes + ((x >> 12) & 0xfffu), &value, width_bytes[x >> 24]);
            NEXT();
        }
        CASE(FGETS) {
            const uint32_t x = *pc++;
            const pnx_global *g = &st->globals[x & 0xfffu];
            const uint64_t row = (uint64_t)R[OP_B(w)];
            if (row >= g->capacity)
                FAULT(PINAX_ERR_RANGE);
            const uint8_t *field = g->ptr + row * g->elem_bytes + ((x >> 12) & 0xfffu);
            S[OP_A(w)] = (pnx_scell){PNX_SEG_GLOBAL, field[0],
                                     (uint64_t)(field + 1 - st->segment[PNX_SEG_GLOBAL])};
            NEXT();
        }
        CASE(FSETS) {
            const uint32_t x = *pc++;
            const pnx_global *g = &st->globals[x & 0xfffu];
            const uint64_t row = (uint64_t)R[OP_B(w)];
            const pnx_scell value = S[OP_A(w)];
            if (row >= g->capacity)
                FAULT(PINAX_ERR_RANGE);
            if (value.length > (x >> 24))
                FAULT(PINAX_ERR_RANGE);
            uint8_t *field = g->ptr + row * g->elem_bytes + ((x >> 12) & 0xfffu);
            field[0] = (uint8_t)value.length;
            __builtin_memmove(field + 1, view_bytes(st, value), value.length);
            NEXT();
        }

        CASE(SLEN) { R[OP_A(w)] = S[OP_B(w)].length; NEXT(); }
        CASE(SBYTE) {
            const pnx_scell s = S[OP_B(w)];
            const uint64_t index = (uint64_t)R[OP_C(w)];
            if (index >= s.length)
                FAULT(PINAX_ERR_RANGE);
            R[OP_A(w)] = view_bytes(st, s)[index];
            NEXT();
        }
        CASE(JSEQ) {
            const pnx_scell a = S[OP_A(w)], b = S[OP_B(w)];
            if (a.length == b.length &&
                (a.length == 0 || __builtin_memcmp(view_bytes(st, a), view_bytes(st, b), a.length) == 0))
                pc += OP_SC(w);
            NEXT();
        }
        CASE(SNEXT) { // on a used-up sequence the next word runs; otherwise it is skipped
            pnx_scell *rest = &S[OP_A(w)];
            if (rest->offset == PNX_SNEXT_DONE)
                NEXT();
            const uint8_t *bytes = view_bytes(st, *rest);
            const uint8_t *hit = rest->length ? __builtin_memchr(bytes, (int)OP_C(w), rest->length) : NULL;
            if (hit) {
                const uint32_t n = (uint32_t)(hit - bytes);
                S[OP_B(w)] = (pnx_scell){rest->segment, n, rest->offset};
                rest->offset += n + 1u;
                rest->length -= n + 1u;
            } else {
                S[OP_B(w)] = *rest;
                rest->length = 0;
                rest->offset = PNX_SNEXT_DONE;
            }
            ++pc;
            NEXT();
        }
        CASE(SCUT) {
            pnx_scell *head = &S[OP_A(w)];
            const uint8_t *bytes = view_bytes(st, *head);
            const uint8_t *hit = head->length ? __builtin_memchr(bytes, (int)OP_C(w), head->length) : NULL;
            if (hit) {
                const uint32_t n = (uint32_t)(hit - bytes);
                S[OP_B(w)] = (pnx_scell){head->segment, head->length - n - 1u, head->offset + n + 1u};
                head->length = n;
            } else {
                S[OP_B(w)] = (pnx_scell){PNX_SEG_EMPTY, 0, 0};
            }
            NEXT();
        }
        CASE(TOINT) {
            const pnx_scell s = S[OP_B(w)];
            if (parse_int(view_bytes(st, s), s.length, &R[OP_A(w)]))
                ++pc;
            else
                st->fail = PINAX_ERR_FORMAT;
            NEXT();
        }

        CASE(BCLEAR) {
            const uint64_t zero = 0;
            __builtin_memcpy(st->globals[OP_BX(w)].ptr, &zero, 8);
            NEXT();
        }
        CASE(BPUT) {
            const pnx_global *g = &st->globals[OP_C(w)];
            const uint32_t n = width_bytes[OP_B(w)];
            const int64_t value = R[OP_A(w)];
            uint64_t len;
            __builtin_memcpy(&len, g->ptr, 8);
            if (len + n > g->capacity) {
                st->fail = PINAX_ERR_FULL;
                NEXT();
            }
            if (!fits_width(value, OP_B(w)))
                FAULT(PINAX_ERR_OVERFLOW);
            __builtin_memcpy(g->ptr + 8 + len, &value, n);
            len += n;
            __builtin_memcpy(g->ptr, &len, 8);
            ++pc;
            NEXT();
        }
        CASE(BPUTS) { // one length byte, then the bytes
            const pnx_global *g = &st->globals[OP_C(w)];
            const pnx_scell s = S[OP_A(w)];
            uint64_t len;
            __builtin_memcpy(&len, g->ptr, 8);
            if (s.length > 255)
                FAULT(PINAX_ERR_RANGE);
            if (len + 1 + s.length > g->capacity) {
                st->fail = PINAX_ERR_FULL;
                NEXT();
            }
            g->ptr[8 + len] = (uint8_t)s.length;
            __builtin_memmove(g->ptr + 8 + len + 1, view_bytes(st, s), s.length);
            len += 1 + s.length;
            __builtin_memcpy(g->ptr, &len, 8);
            ++pc;
            NEXT();
        }
        CASE(BVIEW) {
            const pnx_global *g = &st->globals[OP_BX(w)];
            uint64_t len;
            __builtin_memcpy(&len, g->ptr, 8);
            S[OP_A(w)] = (pnx_scell){PNX_SEG_GLOBAL, (uint32_t)len,
                                     (uint64_t)(g->ptr + 8 - st->segment[PNX_SEG_GLOBAL])};
            NEXT();
        }
        CASE(SGET) {
            pnx_scell *s = &S[OP_B(w)];
            const uint32_t n = width_bytes[OP_C(w)];
            if (s->length < n) {
                st->fail = PINAX_ERR_FORMAT;
                NEXT();
            }
            R[OP_A(w)] = load_width(view_bytes(st, *s), OP_C(w));
            s->offset += n;
            s->length -= n;
            ++pc;
            NEXT();
        }
        CASE(SGETS) {
            pnx_scell *s = &S[OP_B(w)];
            if (s->length < 1 || s->length - 1u < view_bytes(st, *s)[0]) {
                st->fail = PINAX_ERR_FORMAT;
                NEXT();
            }
            const uint32_t n = view_bytes(st, *s)[0];
            S[OP_A(w)] = (pnx_scell){s->segment, n, s->offset + 1};
            s->offset += n + 1u;
            s->length -= n + 1u;
            ++pc;
            NEXT();
        }

        CASE(CALL) {
            const pnx_wordrec *callee = &st->words[*pc++];
            const uint32_t new_ibase = ibase + OP_A(w), new_sbase = sbase + OP_B(w);
            if (depth == st->depth_max || new_ibase + callee->int_cells > st->int_slots ||
                new_sbase + callee->str_cells > st->str_slots)
                FAULT(PINAX_ERR_DEPTH);
            st->ctrl[depth++] = (pnx_frame){(uint32_t)(pc - code), ibase, sbase, 0};
            if (depth > st->depth_peak)
                st->depth_peak = depth;
            ibase = new_ibase;
            sbase = new_sbase;
            R = st->ibank + ibase;
            S = st->sbank + sbase;
            pc = code + callee->entry;
            NEXT();
        }
        CASE(TCALL) {
            const pnx_wordrec *callee = &st->words[*pc++];
            if (ibase + callee->int_cells > st->int_slots || sbase + callee->str_cells > st->str_slots)
                FAULT(PINAX_ERR_DEPTH);
            pc = code + callee->entry;
            NEXT();
        }
        CASE(RET) {
            if (depth == 0)
                goto finished;
            const pnx_frame f = st->ctrl[--depth];
            pc = code + f.pc;
            ibase = f.int_base;
            sbase = f.str_base;
            R = st->ibank + ibase;
            S = st->sbank + sbase;
            NEXT();
        }
        CASE(RETOK) { // success of a word that may fail: skip the caller's failure word
            if (depth == 0)
                goto finished;
            const pnx_frame f = st->ctrl[--depth];
            pc = code + f.pc + 1;
            ibase = f.int_base;
            sbase = f.str_base;
            R = st->ibank + ibase;
            S = st->sbank + sbase;
            NEXT();
        }
        CASE(FAIL) {
            st->fail = (pinax_err)OP_BX(w);
            goto fail_return;
        }
        CASE(RETF) {
        fail_return:
            if (depth == 0)
                FAULT(st->fail);
            const pnx_frame f = st->ctrl[--depth];
            pc = code + f.pc; // the caller's failure word
            ibase = f.int_base;
            sbase = f.str_base;
            R = st->ibank + ibase;
            S = st->sbank + sbase;
            NEXT();
        }
        CASE(HCALL) {
            bool fault;
            const pinax_err err = host_call(st, st->imports[*pc], R + OP_A(w), S + OP_B(w), &fault);
            ++pc;
            if (err != PINAX_OK)
                FAULT(err);
            NEXT();
        }
        CASE(HCALLF) {
            bool fault;
            const pinax_err err = host_call(st, st->imports[*pc], R + OP_A(w), S + OP_B(w), &fault);
            ++pc;
            if (err == PINAX_OK)
                ++pc;
            else if (fault)
                FAULT(err);
            else
                st->fail = err;
            NEXT();
        }

        CASE(CHECK) {
#if !defined(PINAX_BUDGET_EACH)
            const uint32_t n = OP_BX(w);
            count += n;
            if (budget) {
                if (left < n) {
                    --pc;
                    st->owed = n; // the next resume gets at least this much, so it moves on
                    goto suspended;
                }
                left -= n;
            }
#endif
            NEXT();
        }
        CASE(FAULT) { FAULT((pinax_err)OP_BX(w)); }

#if defined(PINAX_SWITCH_DISPATCH)
        default:
            FAULT(PINAX_ERR_FORMAT);
        }
    }
#else
        }
    }
#endif

suspended:
    st->pc = (uint32_t)(pc - code);
    st->int_base = ibase;
    st->str_base = sbase;
    st->depth = depth;
    st->left = 0;
    st->instructions += count;
    st->suspended = true;
    return PINAX_SUSPENDED;

finished:
    st->left = left;
    st->instructions += count;
    st->suspended = false;
    st->depth = 0;
    return result;
}
