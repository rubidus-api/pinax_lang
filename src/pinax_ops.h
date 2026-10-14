// pinax_ops.h - what each instruction does (RFC-0013 J2).
//
// One function per instruction, all with the same parameters: the state, the two register
// windows, the instruction word and its second word (0 when it has none). The dispatch loop in
// pinax_vm.c only fetches, calls and steers. Instructions that move the program counter or the
// windows themselves (calls, returns, CHECK) are in the loop, because that is the part a later
// tier replaces; conditional jumps are predicates here and the loop adds the offset.
//
// Result of a pnx_op_ function:
//     PNX_R_OK      done
//     PNX_R_FAILED  the operation failed (state->fail is set), or the sequence is used up:
//                   the word after the instruction runs instead of being skipped
//     otherwise     a pinax_err: a fault, the run ends
#ifndef PINAX_OPS_H
#define PINAX_OPS_H

#include "pinax_internal.h"

#include <stdckdint.h>

#define PNX_R_OK 0
#define PNX_R_FAILED (-1)

#define OP_A(w) (((w) >> 8) & 0xffu)
#define OP_B(w) (((w) >> 16) & 0xffu)
#define OP_C(w) ((w) >> 24)
#define OP_BX(w) ((w) >> 16)
#define OP_SBX(w) ((int32_t)(int16_t)((w) >> 16))
#define OP_SB(w) ((int32_t)(int8_t)(((w) >> 16) & 0xffu))
#define OP_SC(w) ((int32_t)(int8_t)((w) >> 24))
#define OP_SAX(w) ((int32_t)(w) >> 8)

// Forced inline: left to its own judgement the compiler stops inlining into the large dispatch
// function and the hot instructions become calls.
#if defined(__GNUC__)
#define PNX_INLINE static inline __attribute__((always_inline))
#else
#define PNX_INLINE static inline
#endif

// The instructions that name a global in C (AGET, ROW, BPUT, IXGET, ...) read its number from
// `x`: the dispatch gives C for the narrow form and the second word for the wide one (AGETW).
#define PNX_OP(name)                                                                               \
    PNX_INLINE int pnx_op_##name([[maybe_unused]] pinax_state *st, [[maybe_unused]] int64_t *R, \
                                    [[maybe_unused]] pnx_scell *S, [[maybe_unused]] uint32_t w,    \
                                    [[maybe_unused]] uint32_t x)
#define PNX_COND(name)                                                                             \
    PNX_INLINE bool pnx_cond_##name([[maybe_unused]] const pinax_state *st,                     \
                                       [[maybe_unused]] const int64_t *R,                          \
                                       [[maybe_unused]] const pnx_scell *S, uint32_t w)

static const uint8_t pnx_width_bytes[PNX_W_COUNT] = {1, 2, 4, 8, 1, 2, 4};

PNX_INLINE int64_t pnx_load_width(const uint8_t *p, uint32_t width) {
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

PNX_INLINE bool pnx_fits_width(int64_t v, uint32_t width) {
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

PNX_INLINE const uint8_t *pnx_view_bytes(const pinax_state *st, pnx_scell cell) {
    return st->segment[cell.segment] + cell.offset;
}

PNX_INLINE uint64_t pnx_buffer_len(const pnx_global *g) {
    uint64_t len;
    __builtin_memcpy(&len, g->ptr, 8);
    return len;
}

// The row number (from 0) a row value names in table `g`, checked. A row is its number plus
// one; "no row" (0) wraps and is out of range. A row of a pool also carries the generation of
// its slot, which must still be the slot's.
PNX_INLINE int pnx_row_of(const pnx_global *g, int64_t value, uint64_t *row) {
    const uint64_t v = (uint64_t)value;
    if (g->kind == PNX_G_POOL) {
        const uint32_t slot = (uint32_t)v - 1;
        uint32_t generation;
        if (slot >= g->capacity)
            return PINAX_ERR_RANGE;
        __builtin_memcpy(&generation, g->ptr + (size_t)g->capacity * g->elem_bytes + (size_t)slot * 8, 4);
        if (generation != (uint32_t)(v >> 32))
            return PINAX_ERR_STALE;
        *row = slot;
        return PNX_R_OK;
    }
    *row = v - 1;
    return *row >= g->length ? PINAX_ERR_RANGE : PNX_R_OK;
}

// ---- moving -----------------------------------------------------------------------------

PNX_OP(MOVI) { R[OP_A(w)] = R[OP_B(w)]; return PNX_R_OK; }
PNX_OP(MOVS) { S[OP_A(w)] = S[OP_B(w)]; return PNX_R_OK; }
PNX_OP(LOADI) { R[OP_A(w)] = OP_SBX(w); return PNX_R_OK; }
PNX_OP(LOADK) { R[OP_A(w)] = st->iconst[OP_BX(w)]; return PNX_R_OK; }
PNX_OP(LOADKS) {
    const pnx_sconst k = st->sconst[OP_BX(w)];
    S[OP_A(w)] = (pnx_scell){PNX_SEG_CONST, k.length, k.offset};
    return PNX_R_OK;
}

// ---- integers ---------------------------------------------------------------------------

PNX_OP(ADD) { return ckd_add(&R[OP_A(w)], R[OP_B(w)], R[OP_C(w)]) ? PINAX_ERR_OVERFLOW : PNX_R_OK; }
PNX_OP(SUB) { return ckd_sub(&R[OP_A(w)], R[OP_B(w)], R[OP_C(w)]) ? PINAX_ERR_OVERFLOW : PNX_R_OK; }
PNX_OP(MUL) { return ckd_mul(&R[OP_A(w)], R[OP_B(w)], R[OP_C(w)]) ? PINAX_ERR_OVERFLOW : PNX_R_OK; }
PNX_OP(DIV) {
    const int64_t a = R[OP_B(w)], b = R[OP_C(w)];
    if (b == 0)
        return PINAX_ERR_DIV_ZERO;
    if (a == INT64_MIN && b == -1)
        return PINAX_ERR_OVERFLOW;
    R[OP_A(w)] = a / b;
    return PNX_R_OK;
}
PNX_OP(MOD) {
    const int64_t a = R[OP_B(w)], b = R[OP_C(w)];
    if (b == 0)
        return PINAX_ERR_DIV_ZERO;
    R[OP_A(w)] = b == -1 ? 0 : a % b;
    return PNX_R_OK;
}
PNX_OP(NEG) { return ckd_sub(&R[OP_A(w)], (int64_t)0, R[OP_B(w)]) ? PINAX_ERR_OVERFLOW : PNX_R_OK; }
PNX_OP(ADDI) {
    return ckd_add(&R[OP_A(w)], R[OP_B(w)], (int64_t)OP_SC(w)) ? PINAX_ERR_OVERFLOW : PNX_R_OK;
}
PNX_OP(MULI) { return ckd_mul(&R[OP_A(w)], R[OP_B(w)], (int64_t)OP_SC(w)) ? PINAX_ERR_OVERFLOW : PNX_R_OK; }
PNX_OP(DIVI) { // the compiler gives no 0; -1 would overflow at INT64_MIN
    const int64_t a = R[OP_B(w)], b = OP_SC(w);
    if (b == 0)
        return PINAX_ERR_DIV_ZERO;
    if (a == INT64_MIN && b == -1)
        return PINAX_ERR_OVERFLOW;
    R[OP_A(w)] = a / b;
    return PNX_R_OK;
}
PNX_OP(MODI) {
    const int64_t a = R[OP_B(w)], b = OP_SC(w);
    if (b == 0)
        return PINAX_ERR_DIV_ZERO;
    R[OP_A(w)] = b == -1 ? 0 : a % b;
    return PNX_R_OK;
}
PNX_OP(ANDI) { R[OP_A(w)] = R[OP_B(w)] & (int64_t)OP_SC(w); return PNX_R_OK; }
PNX_OP(ORI) { R[OP_A(w)] = R[OP_B(w)] | (int64_t)OP_SC(w); return PNX_R_OK; }
PNX_OP(XORI) { R[OP_A(w)] = R[OP_B(w)] ^ (int64_t)OP_SC(w); return PNX_R_OK; }
PNX_OP(SHLI) { R[OP_A(w)] = (int64_t)((uint64_t)R[OP_B(w)] << (OP_SC(w) & 63)); return PNX_R_OK; }
PNX_OP(SHRI) { R[OP_A(w)] = R[OP_B(w)] >> (OP_SC(w) & 63); return PNX_R_OK; }
PNX_OP(USHRI) { R[OP_A(w)] = (int64_t)((uint64_t)R[OP_B(w)] >> (OP_SC(w) & 63)); return PNX_R_OK; }
PNX_OP(WMULI) { R[OP_A(w)] = (int64_t)((uint64_t)R[OP_B(w)] * (uint64_t)(int64_t)OP_SC(w)); return PNX_R_OK; }
PNX_OP(WADD) { R[OP_A(w)] = (int64_t)((uint64_t)R[OP_B(w)] + (uint64_t)R[OP_C(w)]); return PNX_R_OK; }
PNX_OP(WSUB) { R[OP_A(w)] = (int64_t)((uint64_t)R[OP_B(w)] - (uint64_t)R[OP_C(w)]); return PNX_R_OK; }
PNX_OP(WMUL) { R[OP_A(w)] = (int64_t)((uint64_t)R[OP_B(w)] * (uint64_t)R[OP_C(w)]); return PNX_R_OK; }
PNX_OP(AND) { R[OP_A(w)] = R[OP_B(w)] & R[OP_C(w)]; return PNX_R_OK; }
PNX_OP(OR) { R[OP_A(w)] = R[OP_B(w)] | R[OP_C(w)]; return PNX_R_OK; }
PNX_OP(XOR) { R[OP_A(w)] = R[OP_B(w)] ^ R[OP_C(w)]; return PNX_R_OK; }
PNX_OP(SHL) { R[OP_A(w)] = (int64_t)((uint64_t)R[OP_B(w)] << (R[OP_C(w)] & 63)); return PNX_R_OK; }
PNX_OP(SHR) { R[OP_A(w)] = R[OP_B(w)] >> (R[OP_C(w)] & 63); return PNX_R_OK; }
PNX_OP(USHR) { R[OP_A(w)] = (int64_t)((uint64_t)R[OP_B(w)] >> (R[OP_C(w)] & 63)); return PNX_R_OK; }

// ---- conditions of the compare-and-branch instructions -------------------------------------

PNX_COND(JEQ) { return R[OP_A(w)] == R[OP_B(w)]; }
PNX_COND(JNE) { return R[OP_A(w)] != R[OP_B(w)]; }
PNX_COND(JLT) { return R[OP_A(w)] < R[OP_B(w)]; }
PNX_COND(JLE) { return R[OP_A(w)] <= R[OP_B(w)]; }
PNX_COND(JEQI) { return R[OP_A(w)] == OP_SB(w); }
PNX_COND(JNEI) { return R[OP_A(w)] != OP_SB(w); }
PNX_COND(JLTI) { return R[OP_A(w)] < OP_SB(w); }
PNX_COND(JGEI) { return R[OP_A(w)] >= OP_SB(w); }
PNX_COND(JSNE) {
    const pnx_scell a = S[OP_A(w)], b = S[OP_B(w)];
    return !(a.length == b.length &&
             (a.length == 0 || __builtin_memcmp(pnx_view_bytes(st, a), pnx_view_bytes(st, b), a.length) == 0));
}
PNX_COND(JSEQ) {
    const pnx_scell a = S[OP_A(w)], b = S[OP_B(w)];
    return a.length == b.length &&
           (a.length == 0 || __builtin_memcmp(pnx_view_bytes(st, a), pnx_view_bytes(st, b), a.length) == 0);
}

// ---- globals, arrays, records -----------------------------------------------------------

PNX_OP(GETG) { __builtin_memcpy(&R[OP_A(w)], st->globals[OP_BX(w)].ptr, 8); return PNX_R_OK; }
PNX_OP(SETG) { __builtin_memcpy(st->globals[OP_BX(w)].ptr, &R[OP_A(w)], 8); return PNX_R_OK; }
PNX_OP(AGET) {
    const pnx_global *g = &st->globals[x];
    const uint64_t index = (uint64_t)R[OP_B(w)];
    if (index >= g->length)
        return PINAX_ERR_RANGE;
    __builtin_memcpy(&R[OP_A(w)], g->ptr + index * 8, 8);
    return PNX_R_OK;
}
PNX_OP(ASET) {
    const pnx_global *g = &st->globals[x];
    const uint64_t index = (uint64_t)R[OP_B(w)];
    if (index >= g->length)
        return PINAX_ERR_RANGE;
    __builtin_memcpy(g->ptr + index * 8, &R[OP_A(w)], 8);
    return PNX_R_OK;
}
PNX_OP(ROW) {
    const uint64_t index = (uint64_t)R[OP_B(w)];
    if (index >= st->globals[x].length)
        return PINAX_ERR_RANGE;
    R[OP_A(w)] = (int64_t)index + 1;
    return PNX_R_OK;
}
PNX_OP(FGET) {
    const pnx_global *g = &st->globals[x & 0xfffu];
    uint64_t row;
    const int bad = pnx_row_of(g, R[OP_B(w)], &row);
    if (bad)
        return bad;
    R[OP_A(w)] = pnx_load_width(g->ptr + row * g->elem_bytes + ((x >> 12) & 0xfffu), x >> 24);
    return PNX_R_OK;
}
PNX_OP(FSET) {
    const pnx_global *g = &st->globals[x & 0xfffu];
    const int64_t value = R[OP_A(w)];
    uint64_t row;
    const int bad = pnx_row_of(g, R[OP_B(w)], &row);
    if (bad)
        return bad;
    if (!pnx_fits_width(value, x >> 24))
        return PINAX_ERR_OVERFLOW;
    __builtin_memcpy(g->ptr + row * g->elem_bytes + ((x >> 12) & 0xfffu), &value, pnx_width_bytes[x >> 24]);
    return PNX_R_OK;
}
PNX_OP(FGETS) {
    const pnx_global *g = &st->globals[x & 0xfffu];
    uint64_t row;
    const int bad = pnx_row_of(g, R[OP_B(w)], &row);
    if (bad)
        return bad;
    const uint8_t *field = g->ptr + row * g->elem_bytes + ((x >> 12) & 0xfffu);
    S[OP_A(w)] = (pnx_scell){PNX_SEG_GLOBAL, field[0], (uint64_t)(field + 1 - st->segment[PNX_SEG_GLOBAL])};
    return PNX_R_OK;
}
PNX_OP(FSETS) {
    const pnx_global *g = &st->globals[x & 0xfffu];
    const pnx_scell value = S[OP_A(w)];
    uint64_t row;
    const int bad = pnx_row_of(g, R[OP_B(w)], &row);
    if (bad)
        return bad;
    if (value.length > (x >> 24))
        return PINAX_ERR_RANGE;
    uint8_t *field = g->ptr + row * g->elem_bytes + ((x >> 12) & 0xfffu);
    field[0] = (uint8_t)value.length;
    __builtin_memmove(field + 1, pnx_view_bytes(st, value), value.length);
    return PNX_R_OK;
}

// ---- strings ----------------------------------------------------------------------------

PNX_OP(SLEN) { R[OP_A(w)] = S[OP_B(w)].length; return PNX_R_OK; }
PNX_OP(SBYTE) {
    const pnx_scell s = S[OP_B(w)];
    const uint64_t index = (uint64_t)R[OP_C(w)];
    if (index >= s.length)
        return PINAX_ERR_RANGE;
    R[OP_A(w)] = pnx_view_bytes(st, s)[index];
    return PNX_R_OK;
}
PNX_OP(SNEXT) { // PNX_R_FAILED: the sequence is used up
    pnx_scell *rest = &S[OP_A(w)];
    if (rest->offset == PNX_SNEXT_DONE)
        return PNX_R_FAILED;
    const uint32_t n = pnx_find_byte(pnx_view_bytes(st, *rest), rest->length, (uint8_t)OP_C(w));
    st->charge += n != UINT32_MAX ? n : rest->length;
    if (n != UINT32_MAX) {
        S[OP_B(w)] = (pnx_scell){rest->segment, n, rest->offset};
        rest->offset += n + 1u;
        rest->length -= n + 1u;
    } else {
        S[OP_B(w)] = *rest;
        rest->length = 0;
        rest->offset = PNX_SNEXT_DONE;
    }
    return PNX_R_OK;
}
PNX_OP(SCUT) {
    pnx_scell *head = &S[OP_A(w)];
    const uint32_t n = pnx_find_byte(pnx_view_bytes(st, *head), head->length, (uint8_t)OP_C(w));
    if (n != UINT32_MAX) {
        S[OP_B(w)] = (pnx_scell){head->segment, head->length - n - 1u, head->offset + n + 1u};
        head->length = n;
    } else {
        S[OP_B(w)] = (pnx_scell){PNX_SEG_EMPTY, 0, 0};
    }
    return PNX_R_OK;
}
PNX_OP(TOINT) {
    const pnx_scell s = S[OP_B(w)];
    if (pnx_parse_int(pnx_view_bytes(st, s), s.length, &R[OP_A(w)]))
        return PNX_R_OK;
    st->fail = PINAX_ERR_FORMAT;
    return PNX_R_FAILED;
}

// ---- buffers ----------------------------------------------------------------------------

PNX_OP(BCLEAR) {
    const uint64_t zero = 0;
    __builtin_memcpy(st->globals[OP_BX(w)].ptr, &zero, 8);
    return PNX_R_OK;
}
PNX_OP(BPUT) {
    const pnx_global *g = &st->globals[x];
    const uint32_t n = pnx_width_bytes[OP_B(w)];
    const int64_t value = R[OP_A(w)];
    uint64_t len = pnx_buffer_len(g);
    if (len + n > g->capacity) {
        st->fail = PINAX_ERR_FULL;
        return PNX_R_FAILED;
    }
    if (!pnx_fits_width(value, OP_B(w)))
        return PINAX_ERR_OVERFLOW;
    __builtin_memcpy(g->ptr + 8 + len, &value, n);
    len += n;
    __builtin_memcpy(g->ptr, &len, 8);
    return PNX_R_OK;
}
PNX_OP(BPUTS) { // one length byte, then the bytes
    const pnx_global *g = &st->globals[x];
    const pnx_scell s = S[OP_A(w)];
    uint64_t len = pnx_buffer_len(g);
    if (s.length > 255)
        return PINAX_ERR_RANGE;
    if (len + 1 + s.length > g->capacity) {
        st->fail = PINAX_ERR_FULL;
        return PNX_R_FAILED;
    }
    g->ptr[8 + len] = (uint8_t)s.length;
    __builtin_memmove(g->ptr + 8 + len + 1, pnx_view_bytes(st, s), s.length);
    len += 1 + s.length;
    __builtin_memcpy(g->ptr, &len, 8);
    return PNX_R_OK;
}
PNX_OP(BVIEW) {
    const pnx_global *g = &st->globals[OP_BX(w)];
    S[OP_A(w)] = (pnx_scell){PNX_SEG_GLOBAL, (uint32_t)pnx_buffer_len(g),
                             (uint64_t)(g->ptr + 8 - st->segment[PNX_SEG_GLOBAL])};
    return PNX_R_OK;
}
PNX_OP(SGET) {
    pnx_scell *s = &S[OP_B(w)];
    const uint32_t n = pnx_width_bytes[OP_C(w)];
    if (s->length < n) {
        st->fail = PINAX_ERR_FORMAT;
        return PNX_R_FAILED;
    }
    R[OP_A(w)] = pnx_load_width(pnx_view_bytes(st, *s), OP_C(w));
    s->offset += n;
    s->length -= n;
    return PNX_R_OK;
}
PNX_OP(SGETS) {
    pnx_scell *s = &S[OP_B(w)];
    if (s->length < 1 || s->length - 1u < pnx_view_bytes(st, *s)[0]) {
        st->fail = PINAX_ERR_FORMAT;
        return PNX_R_FAILED;
    }
    const uint32_t n = pnx_view_bytes(st, *s)[0];
    S[OP_A(w)] = (pnx_scell){s->segment, n, s->offset + 1};
    s->offset += n + 1u;
    s->length -= n + 1u;
    return PNX_R_OK;
}

// ---- failures and strings built in the region ---------------------------------------------

PNX_OP(GETFAIL) { R[OP_A(w)] = st->fail; return PNX_R_OK; }

PNX_OP(RMARK) {
    S[OP_A(w)] = (pnx_scell){PNX_SEG_REGION, 0, st->region_used};
    return PNX_R_OK;
}

// Appends `n` bytes to the string in `cell`, which must end at the top of the region.
PNX_INLINE int pnx_region_append(pinax_state *st, pnx_scell *cell, const uint8_t *bytes, uint32_t n) {
    if (cell->segment != PNX_SEG_REGION || cell->offset + cell->length != st->region_used)
        return PINAX_ERR_STATE;
    if (n > st->region_len - st->region_used || n > UINT32_MAX - cell->length)
        return PINAX_ERR_FULL;
    __builtin_memmove(st->region + st->region_used, bytes, n);
    st->region_used += n;
    cell->length += n;
    return PNX_R_OK;
}
PNX_OP(RCATS) {
    const pnx_scell s = S[OP_B(w)];
    st->charge += s.length;
    return pnx_region_append(st, &S[OP_A(w)], pnx_view_bytes(st, s), s.length);
}
PNX_OP(RCATI) {
    uint8_t digits[24];
    const uint32_t n = pnx_format_int(R[OP_B(w)], digits);
    return pnx_region_append(st, &S[OP_A(w)], digits, n);
}
PNX_OP(RCATB) {
    return R[OP_B(w)] ? pnx_region_append(st, &S[OP_A(w)], (const uint8_t *)"true", 4)
                      : pnx_region_append(st, &S[OP_A(w)], (const uint8_t *)"false", 5);
}

// ---- arrays and lists of integers ---------------------------------------------------------

PNX_OP(GLEN) { R[OP_A(w)] = st->globals[OP_BX(w)].length; return PNX_R_OK; }
PNX_OP(GCLEAR) { // an array is zeroed; a list becomes empty
    pnx_global *g = &st->globals[OP_BX(w)];
    if (g->kind == PNX_G_LIST || g->kind == PNX_G_ROWS || g->kind == PNX_G_QUEUE)
        g->length = g->head = 0;
    else
        __builtin_memset(g->ptr, 0, (size_t)g->capacity * g->elem_bytes);
    return PNX_R_OK;
}
PNX_OP(ADDROW) {
    pnx_global *g = &st->globals[OP_BX(w)];
    if (g->length == g->capacity) {
        st->fail = PINAX_ERR_FULL;
        return PNX_R_FAILED;
    }
    __builtin_memset(g->ptr + (size_t)g->length * g->elem_bytes, 0, g->elem_bytes);
    R[OP_A(w)] = (int64_t)++g->length;
    return PNX_R_OK;
}
PNX_OP(LPUSH) {
    pnx_global *g = &st->globals[OP_BX(w)];
    if (g->length == g->capacity) {
        st->fail = PINAX_ERR_FULL;
        return PNX_R_FAILED;
    }
    __builtin_memcpy(g->ptr + (size_t)g->length * 8, &R[OP_A(w)], 8);
    ++g->length;
    return PNX_R_OK;
}
PNX_OP(LPOP) {
    pnx_global *g = &st->globals[OP_BX(w)];
    if (g->length == 0) {
        st->fail = PINAX_ERR_RANGE;
        return PNX_R_FAILED;
    }
    --g->length;
    __builtin_memcpy(&R[OP_A(w)], g->ptr + (size_t)g->length * 8, 8);
    return PNX_R_OK;
}
// Appends a copy of row R[A] of table x (a list or an array of the same record) to the
// list of records BX. Fails with full.
PNX_OP(LCOPYR) {
    pnx_global *g = &st->globals[OP_BX(w)];
    const pnx_global *from = &st->globals[x];
    uint64_t row;
    const int bad = pnx_row_of(from, R[OP_A(w)], &row);
    if (bad)
        return bad;
    if (g->length == g->capacity) {
        st->fail = PINAX_ERR_FULL;
        return PNX_R_FAILED;
    }
    __builtin_memmove(g->ptr + (size_t)g->length * g->elem_bytes, from->ptr + row * from->elem_bytes, g->elem_bytes);
    ++g->length;
    return PNX_R_OK;
}

// A list of str(N) is a table of text cells: push copies the text into a new last cell (too
// long is a fault, as a store is), pop copies the last cell's text into the region and drops it.
PNX_OP(LPUSHS) {
    pnx_global *g = &st->globals[OP_BX(w)];
    const pnx_scell value = S[OP_A(w)];
    if (g->length == g->capacity) {
        st->fail = PINAX_ERR_FULL;
        return PNX_R_FAILED;
    }
    if (value.length > g->elem_bytes - 1)
        return PINAX_ERR_RANGE;
    uint8_t *cell = g->ptr + (size_t)g->length * g->elem_bytes;
    cell[0] = (uint8_t)value.length;
    __builtin_memmove(cell + 1, pnx_view_bytes(st, value), value.length);
    ++g->length;
    return PNX_R_OK;
}
PNX_OP(LPOPS) {
    pnx_global *g = &st->globals[OP_BX(w)];
    if (g->length == 0) {
        st->fail = PINAX_ERR_RANGE;
        return PNX_R_FAILED;
    }
    const uint8_t *cell = g->ptr + (size_t)(g->length - 1) * g->elem_bytes;
    uint8_t *copy = cell[0] ? pnx_region_alloc(st, cell[0]) : NULL;
    if (cell[0] && !copy)
        return PINAX_ERR_FULL;
    --g->length;
    if (!cell[0]) {
        S[OP_A(w)] = (pnx_scell){PNX_SEG_EMPTY, 0, 0};
        return PNX_R_OK;
    }
    __builtin_memcpy(copy, cell + 1, cell[0]);
    S[OP_A(w)] = (pnx_scell){PNX_SEG_REGION, cell[0], (uint64_t)(copy - st->region)};
    return PNX_R_OK;
}
PNX_OP(SFIND) {
    const pnx_scell hay = S[OP_B(w)], part = S[OP_C(w)];
    const uint32_t at = pnx_find(pnx_view_bytes(st, hay), hay.length, pnx_view_bytes(st, part), part.length);
    st->charge += at != UINT32_MAX ? at : hay.length;
    if (at == UINT32_MAX) {
        st->fail = PINAX_ERR_NOT_FOUND;
        return PNX_R_FAILED;
    }
    R[OP_A(w)] = at;
    return PNX_R_OK;
}

PNX_INLINE bool pnx_is_space(uint8_t c) {
    return c == ' ' || c == '\t' || c == '\n' || c == '\r';
}
PNX_OP(SWORD) { // PNX_R_FAILED: no word is left
    pnx_scell *rest = &S[OP_A(w)];
    const uint8_t *bytes = pnx_view_bytes(st, *rest);
    uint32_t i = 0;
    while (i < rest->length && pnx_is_space(bytes[i]))
        ++i;
    uint32_t end = i;
    while (end < rest->length && !pnx_is_space(bytes[end]))
        ++end;
    if (end == i) {
        rest->offset += rest->length;
        rest->length = 0;
        return PNX_R_FAILED;
    }
    S[OP_B(w)] = (pnx_scell){rest->segment, end - i, rest->offset + i};
    rest->offset += end;
    rest->length -= end;
    st->charge += end;
    return PNX_R_OK;
}
PNX_OP(ERRTEXT) {
    uint32_t offset, length;
    pnx_err_name(R[OP_B(w)], &offset, &length);
    S[OP_A(w)] = (pnx_scell){PNX_SEG_STATIC, length, offset};
    return PNX_R_OK;
}

// ---- buffers, views, indexes --------------------------------------------------------------

PNX_INLINE int pnx_buffer_append(pinax_state *st, const pnx_global *g, const uint8_t *bytes, uint32_t n) {
    uint64_t len = pnx_buffer_len(g);
    if (n > g->capacity - len) {
        st->fail = PINAX_ERR_FULL;
        return PNX_R_FAILED;
    }
    __builtin_memmove(g->ptr + 8 + len, bytes, n);
    len += n;
    __builtin_memcpy(g->ptr, &len, 8);
    return PNX_R_OK;
}
PNX_OP(BCAT) {
    const pnx_scell s = S[OP_A(w)];
    st->charge += s.length;
    return pnx_buffer_append(st, &st->globals[OP_BX(w)], pnx_view_bytes(st, s), s.length);
}
PNX_OP(BCATI) {
    uint8_t digits[24];
    const uint32_t n = pnx_format_int(R[OP_A(w)], digits);
    return pnx_buffer_append(st, &st->globals[OP_BX(w)], digits, n);
}

#define PNX_VIEW_OP(name, expr_offset, expr_length)                                                \
    PNX_OP(name) {                                                                                 \
        const pnx_scell s = S[OP_B(w)];                                                            \
        if (R[OP_C(w)] < 0)                                                                        \
            return PINAX_ERR_RANGE;                                                                \
        const uint32_t n = (uint64_t)R[OP_C(w)] > s.length ? s.length : (uint32_t)R[OP_C(w)];      \
        S[OP_A(w)] = (pnx_scell){s.segment, (expr_length), s.offset + (expr_offset)};              \
        return PNX_R_OK;                                                                           \
    }
PNX_VIEW_OP(STAKE, 0u, n)
PNX_VIEW_OP(SSKIP, n, s.length - n)
PNX_VIEW_OP(STAKEL, s.length - n, n)
PNX_VIEW_OP(SSKIPL, 0u, s.length - n)

PNX_OP(SPRE) {
    const pnx_scell s = S[OP_B(w)], p = S[OP_C(w)];
    R[OP_A(w)] = p.length <= s.length &&
                 (p.length == 0 || __builtin_memcmp(pnx_view_bytes(st, s), pnx_view_bytes(st, p), p.length) == 0);
    return PNX_R_OK;
}
PNX_OP(SSUF) {
    const pnx_scell s = S[OP_B(w)], p = S[OP_C(w)];
    R[OP_A(w)] = p.length <= s.length &&
                 (p.length == 0 || __builtin_memcmp(pnx_view_bytes(st, s) + (s.length - p.length),
                                                    pnx_view_bytes(st, p), p.length) == 0);
    return PNX_R_OK;
}
PNX_OP(SHAS) {
    const pnx_scell s = S[OP_B(w)], p = S[OP_C(w)];
    R[OP_A(w)] = pnx_find(pnx_view_bytes(st, s), s.length, pnx_view_bytes(st, p), p.length) != UINT32_MAX;
    st->charge += s.length;
    return PNX_R_OK;
}

PNX_OP(SREPL) {
    const pnx_scell s = S[OP_B(w)], old = S[OP_C(w)], with = S[x];
    const uint8_t *bytes = pnx_view_bytes(st, s), *from = pnx_view_bytes(st, old), *to = pnx_view_bytes(st, with);
    pnx_scell out = {PNX_SEG_REGION, 0, st->region_used};
    uint32_t at = 0;
    while (old.length != 0 && at < s.length) {
        const uint32_t hit = pnx_find(bytes + at, s.length - at, from, old.length);
        if (hit == UINT32_MAX)
            break;
        int r = pnx_region_append(st, &out, bytes + at, hit);
        if (r == PNX_R_OK)
            r = pnx_region_append(st, &out, to, with.length);
        if (r != PNX_R_OK)
            return r;
        at += hit + old.length;
    }
    const int r = pnx_region_append(st, &out, bytes + at, s.length - at);
    if (r != PNX_R_OK)
        return r;
    st->charge += (uint64_t)s.length + out.length;
    S[OP_A(w)] = out;
    return PNX_R_OK;
}

PNX_OP(QPUT) {
    pnx_global *g = &st->globals[OP_BX(w)];
    if (g->length == g->capacity) {
        st->fail = PINAX_ERR_FULL;
        return PNX_R_FAILED;
    }
    const uint32_t at = (uint32_t)(((uint64_t)g->head + g->length) % g->capacity);
    __builtin_memcpy(g->ptr + (size_t)at * 8, &R[OP_A(w)], 8);
    ++g->length;
    return PNX_R_OK;
}
PNX_OP(QTAKE) {
    pnx_global *g = &st->globals[OP_BX(w)];
    if (g->length == 0) {
        st->fail = PINAX_ERR_RANGE;
        return PNX_R_FAILED;
    }
    __builtin_memcpy(&R[OP_A(w)], g->ptr + (size_t)g->head * 8, 8);
    g->head = g->head + 1 == g->capacity ? 0 : g->head + 1;
    --g->length;
    return PNX_R_OK;
}
PNX_OP(QPEEK) {
    const pnx_global *g = &st->globals[OP_BX(w)];
    if (g->length == 0) {
        st->fail = PINAX_ERR_RANGE;
        return PNX_R_FAILED;
    }
    __builtin_memcpy(&R[OP_A(w)], g->ptr + (size_t)g->head * 8, 8);
    return PNX_R_OK;
}

PNX_OP(PALLOC) {
    pnx_global *g = &st->globals[OP_BX(w)];
    if (g->head == 0) {
        st->fail = PINAX_ERR_FULL;
        return PNX_R_FAILED;
    }
    const uint32_t slot = g->head - 1;
    uint8_t *meta = g->ptr + (size_t)g->capacity * g->elem_bytes + (size_t)slot * 8;
    uint32_t generation;
    __builtin_memcpy(&generation, meta, 4);
    __builtin_memcpy(&g->head, meta + 4, 4);
    ++generation; // odd: handed out
    __builtin_memcpy(meta, &generation, 4);
    __builtin_memset(g->ptr + (size_t)slot * g->elem_bytes, 0, g->elem_bytes);
    R[OP_A(w)] = (int64_t)((uint64_t)generation << 32 | (slot + 1u));
    return PNX_R_OK;
}
PNX_OP(PFREE) {
    pnx_global *g = &st->globals[OP_BX(w)];
    uint64_t row;
    const int bad = pnx_row_of(g, R[OP_A(w)], &row);
    if (bad)
        return bad;
    uint8_t *meta = g->ptr + (size_t)g->capacity * g->elem_bytes + (size_t)row * 8;
    uint32_t generation;
    __builtin_memcpy(&generation, meta, 4);
    ++generation; // even: free
    __builtin_memcpy(meta, &generation, 4);
    __builtin_memcpy(meta + 4, &g->head, 4);
    g->head = (uint32_t)row + 1;
    return PNX_R_OK;
}
PNX_OP(PALIVE) {
    uint64_t row;
    R[OP_A(w)] = pnx_row_of(&st->globals[x], R[OP_B(w)], &row) == PNX_R_OK;
    return PNX_R_OK;
}

// ---- more string words --------------------------------------------------------------------

PNX_INLINE int pnx_cut(pinax_state *st, pnx_scell *S, uint32_t w, bool must) {
    pnx_scell *head = &S[OP_A(w)], *sep = &S[OP_B(w)];
    const uint32_t at = pnx_find(pnx_view_bytes(st, *head), head->length, pnx_view_bytes(st, *sep), sep->length);
    if (at == UINT32_MAX) {
        if (must) {
            st->fail = PINAX_ERR_NOT_FOUND;
            return PNX_R_FAILED;
        }
        *sep = (pnx_scell){PNX_SEG_EMPTY, 0, 0};
        return PNX_R_OK;
    }
    const pnx_scell tail = {head->segment, head->length - at - sep->length, head->offset + at + sep->length};
    head->length = at;
    *sep = tail;
    return PNX_R_OK;
}
PNX_OP(SCUTS) { return pnx_cut(st, S, w, false); }
PNX_OP(SCUTSF) { return pnx_cut(st, S, w, true); }
PNX_OP(STRIM) {
    pnx_scell s = S[OP_B(w)];
    const uint8_t *bytes = pnx_view_bytes(st, s);
    uint32_t from = 0, to = s.length;
    if (OP_C(w) & 1u)
        while (from < to && pnx_is_space(bytes[from]))
            ++from;
    if (OP_C(w) & 2u)
        while (to > from && pnx_is_space(bytes[to - 1]))
            --to;
    S[OP_A(w)] = (pnx_scell){s.segment, to - from, s.offset + from};
    return PNX_R_OK;
}
PNX_OP(SLINE) { // PNX_R_FAILED: no line is left. A final line break does not start an empty line.
    pnx_scell *rest = &S[OP_A(w)];
    if (rest->length == 0)
        return PNX_R_FAILED;
    const uint8_t *bytes = pnx_view_bytes(st, *rest);
    const uint32_t at = pnx_find_byte(bytes, rest->length, '\n');
    uint32_t n = at == UINT32_MAX ? rest->length : at;
    st->charge += n;
    const uint32_t used = at == UINT32_MAX ? rest->length : at + 1;
    if (at != UINT32_MAX && n > 0 && bytes[n - 1] == '\r')
        --n;
    S[OP_B(w)] = (pnx_scell){rest->segment, n, rest->offset};
    rest->offset += used;
    rest->length -= used;
    return PNX_R_OK;
}
PNX_OP(SNEXTS) { // like SNEXT, with a separator string; an empty separator never matches
    pnx_scell *rest = &S[OP_A(w)];
    if (rest->offset == PNX_SNEXT_DONE)
        return PNX_R_FAILED;
    const pnx_scell sep = S[OP_C(w)];
    const uint32_t at = sep.length ? pnx_find(pnx_view_bytes(st, *rest), rest->length, pnx_view_bytes(st, sep), sep.length)
                                   : UINT32_MAX;
    st->charge += at != UINT32_MAX ? at : rest->length;
    if (at != UINT32_MAX) {
        S[OP_B(w)] = (pnx_scell){rest->segment, at, rest->offset};
        rest->offset += at + sep.length;
        rest->length -= at + sep.length;
    } else {
        S[OP_B(w)] = *rest;
        rest->length = 0;
        rest->offset = PNX_SNEXT_DONE;
    }
    return PNX_R_OK;
}
PNX_OP(SFINDL) {
    const pnx_scell hay = S[OP_B(w)], part = S[OP_C(w)];
    const uint32_t at = pnx_find_last(pnx_view_bytes(st, hay), hay.length, pnx_view_bytes(st, part), part.length);
    if (at == UINT32_MAX) {
        st->fail = PINAX_ERR_NOT_FOUND;
        return PNX_R_FAILED;
    }
    R[OP_A(w)] = at;
    return PNX_R_OK;
}
PNX_OP(SCMP) {
    const pnx_scell a = S[OP_B(w)], b = S[OP_C(w)];
    const uint32_t n = a.length < b.length ? a.length : b.length;
    const int c = n ? __builtin_memcmp(pnx_view_bytes(st, a), pnx_view_bytes(st, b), n) : 0;
    R[OP_A(w)] = c < 0 ? -1 : c > 0 ? 1 : a.length < b.length ? -1 : a.length > b.length;
    return PNX_R_OK;
}

// Digits of `value` in base 16 or 2 (as an unsigned number), without leading zeros.
PNX_INLINE uint32_t pnx_digits(uint64_t value, uint32_t shift, uint8_t out[64]) {
    uint8_t reversed[64];
    uint32_t n = 0;
    do {
        reversed[n++] = (uint8_t)"0123456789abcdef"[value & ((1u << shift) - 1u)];
        value >>= shift;
    } while (value);
    for (uint32_t k = 0; k < n; ++k)
        out[k] = reversed[n - 1 - k];
    return n;
}
// Appends `n` bytes after `pad` copies of `fill`.
PNX_INLINE int pnx_append_padded(pinax_state *st, pnx_scell *cell, const uint8_t *bytes, uint32_t n, int64_t width,
                                 uint8_t fill, bool right) {
    if (width < 0 || width > UINT32_MAX)
        return PINAX_ERR_RANGE;
    uint32_t pad = (uint64_t)width > n ? (uint32_t)width - n : 0;
    int r = PNX_R_OK;
    if (right)
        r = pnx_region_append(st, cell, bytes, n);
    for (; r == PNX_R_OK && pad; --pad)
        r = pnx_region_append(st, cell, &fill, 1);
    if (!right && r == PNX_R_OK)
        r = pnx_region_append(st, cell, bytes, n);
    return r;
}
PNX_OP(RCATX) {
    uint8_t digits[64];
    const uint32_t n = pnx_digits((uint64_t)R[OP_B(w)], OP_C(w) ? 1 : 4, digits);
    return pnx_region_append(st, &S[OP_A(w)], digits, n);
}
PNX_OP(RCATXW) {
    uint8_t digits[64];
    const uint32_t n = pnx_digits((uint64_t)R[OP_B(w)], 4, digits);
    return pnx_append_padded(st, &S[OP_A(w)], digits, n, R[OP_C(w)], '0', false);
}
PNX_OP(RCATW) { // a minus sign stays in front of the zeros
    uint8_t digits[24];
    const uint32_t n = pnx_format_int(R[OP_B(w)], digits);
    if (n && digits[0] == '-') {
        const int r = pnx_region_append(st, &S[OP_A(w)], digits, 1);
        return r != PNX_R_OK ? r : pnx_append_padded(st, &S[OP_A(w)], digits + 1, n - 1, R[OP_C(w)] - 1, '0', false);
    }
    return pnx_append_padded(st, &S[OP_A(w)], digits, n, R[OP_C(w)], '0', false);
}
PNX_OP(RPADL) {
    const pnx_scell s = S[OP_B(w)];
    return pnx_append_padded(st, &S[OP_A(w)], pnx_view_bytes(st, s), s.length, R[OP_C(w)], ' ', false);
}
PNX_OP(RPADR) {
    const pnx_scell s = S[OP_B(w)];
    return pnx_append_padded(st, &S[OP_A(w)], pnx_view_bytes(st, s), s.length, R[OP_C(w)], ' ', true);
}

PNX_OP(RTOP) { R[OP_A(w)] = st->region_used; return PNX_R_OK; }
PNX_OP(RCUT) {
    const uint64_t top = (uint64_t)R[OP_A(w)];
    if (top > st->region_used)
        return PINAX_ERR_RANGE;
    if (st->region_used > st->region_peak)
        st->region_peak = st->region_used;
    st->region_used = (uint32_t)top;
    return PNX_R_OK;
}
PNX_OP(RFREE) { R[OP_A(w)] = st->region_len - st->region_used; return PNX_R_OK; }

PNX_OP(SETFAIL) { st->fail = (pinax_err)OP_BX(w); return PNX_R_OK; }

// ---- the other integer words --------------------------------------------------------------

#define PNX_BIN(name, expr)                                                                        \
    PNX_OP(name) {                                                                                 \
        const int64_t a = R[OP_B(w)], b = R[OP_C(w)];                                              \
        (void)a, (void)b;                                                                          \
        R[OP_A(w)] = (expr);                                                                       \
        return PNX_R_OK;                                                                           \
    }
#define PNX_UN(name, expr)                                                                         \
    PNX_OP(name) {                                                                                 \
        const int64_t a = R[OP_B(w)];                                                              \
        R[OP_A(w)] = (expr);                                                                       \
        return PNX_R_OK;                                                                           \
    }

PNX_INLINE int64_t pnx_sat(int64_t a, int64_t b, int op) {
    int64_t r;
    const bool over = op == 0 ? ckd_add(&r, a, b) : op == 1 ? ckd_sub(&r, a, b) : ckd_mul(&r, a, b);
    if (!over)
        return r;
    const bool negative = op == 0 ? a < 0 : op == 1 ? a < 0 : (a < 0) != (b < 0);
    return negative ? INT64_MIN : INT64_MAX;
}
// The upper 64 bits of the 128-bit product, from 32-bit halves (no wider type is assumed).
PNX_INLINE uint64_t pnx_umulhi(uint64_t a, uint64_t b) {
    const uint64_t al = (uint32_t)a, ah = a >> 32, bl = (uint32_t)b, bh = b >> 32;
    const uint64_t ll = al * bl, lh = al * bh, hl = ah * bl, hh = ah * bh;
    const uint64_t mid = (ll >> 32) + (uint32_t)lh + (uint32_t)hl;
    return hh + (lh >> 32) + (hl >> 32) + (mid >> 32);
}
PNX_INLINE int64_t pnx_mulhi(int64_t a, int64_t b) {
    uint64_t hi = pnx_umulhi((uint64_t)a, (uint64_t)b);
    if (a < 0)
        hi -= (uint64_t)b;
    if (b < 0)
        hi -= (uint64_t)a;
    return (int64_t)hi;
}

PNX_BIN(SATADD, pnx_sat(a, b, 0))
PNX_BIN(SATSUB, pnx_sat(a, b, 1))
PNX_BIN(SATMUL, pnx_sat(a, b, 2))
PNX_BIN(ROTL, (int64_t)((uint64_t)a << (b & 63) | (uint64_t)a >> ((64 - (b & 63)) & 63)))
PNX_BIN(ROTR, (int64_t)((uint64_t)a >> (b & 63) | (uint64_t)a << ((64 - (b & 63)) & 63)))
PNX_BIN(MULHI, pnx_mulhi(a, b))
PNX_BIN(UMULHI, (int64_t)pnx_umulhi((uint64_t)a, (uint64_t)b))
PNX_BIN(ULT, (uint64_t)a < (uint64_t)b)
PNX_BIN(ULE, (uint64_t)a <= (uint64_t)b)
PNX_OP(UDIV) {
    if (R[OP_C(w)] == 0)
        return PINAX_ERR_DIV_ZERO;
    R[OP_A(w)] = (int64_t)((uint64_t)R[OP_B(w)] / (uint64_t)R[OP_C(w)]);
    return PNX_R_OK;
}
PNX_OP(UMOD) {
    if (R[OP_C(w)] == 0)
        return PINAX_ERR_DIV_ZERO;
    R[OP_A(w)] = (int64_t)((uint64_t)R[OP_B(w)] % (uint64_t)R[OP_C(w)]);
    return PNX_R_OK;
}
PNX_OP(FDIV) { // rounds toward minus infinity
    const int64_t a = R[OP_B(w)], b = R[OP_C(w)];
    if (b == 0)
        return PINAX_ERR_DIV_ZERO;
    if (a == INT64_MIN && b == -1)
        return PINAX_ERR_OVERFLOW;
    const int64_t q = a / b;
    R[OP_A(w)] = (a % b != 0 && (a < 0) != (b < 0)) ? q - 1 : q;
    return PNX_R_OK;
}
PNX_OP(FMOD) { // the remainder has the sign of the divisor
    const int64_t a = R[OP_B(w)], b = R[OP_C(w)];
    if (b == 0)
        return PINAX_ERR_DIV_ZERO;
    const int64_t r = b == -1 ? 0 : a % b;
    R[OP_A(w)] = (r != 0 && (r < 0) != (b < 0)) ? r + b : r;
    return PNX_R_OK;
}
PNX_OP(ABS) {
    const int64_t a = R[OP_B(w)];
    if (a == INT64_MIN)
        return PINAX_ERR_OVERFLOW;
    R[OP_A(w)] = a < 0 ? -a : a;
    return PNX_R_OK;
}
PNX_UN(BNOT, ~a)
PNX_UN(POPC, __builtin_popcountll((unsigned long long)a))
PNX_UN(CLZ, a == 0 ? 64 : __builtin_clzll((unsigned long long)a))
PNX_UN(CTZ, a == 0 ? 64 : __builtin_ctzll((unsigned long long)a))
PNX_UN(BSWAP, (int64_t)__builtin_bswap64((uint64_t)a))

#define PNX_CHK(name, call)                                                                        \
    PNX_OP(name) {                                                                                 \
        const int64_t a = R[OP_B(w)], b = R[OP_C(w)];                                              \
        int64_t r;                                                                                 \
        if (call) {                                                                                \
            st->fail = PINAX_ERR_OVERFLOW;                                                         \
            return PNX_R_FAILED;                                                                   \
        }                                                                                          \
        R[OP_A(w)] = r;                                                                            \
        return PNX_R_OK;                                                                           \
    }
PNX_CHK(CHKADD, ckd_add(&r, a, b))
PNX_CHK(CHKSUB, ckd_sub(&r, a, b))
PNX_CHK(CHKMUL, ckd_mul(&r, a, b))
PNX_OP(CHKDIV) { // fails on a zero divisor and on the one quotient that does not fit
    const int64_t a = R[OP_B(w)], b = R[OP_C(w)];
    if (b == 0 || (a == INT64_MIN && b == -1)) {
        st->fail = b == 0 ? PINAX_ERR_DIV_ZERO : PINAX_ERR_OVERFLOW;
        return PNX_R_FAILED;
    }
    R[OP_A(w)] = a / b;
    return PNX_R_OK;
}

// An index is an open-addressed table of rows (row number plus one; 0 is empty, all ones a
// removed entry). The keys are read from the rows themselves.
#define PNX_IX_GONE UINT32_MAX

// The key bytes of `row` (number plus one) for index `x`.
PNX_INLINE const uint8_t *pnx_index_key(const pinax_state *st, const pnx_global *x, uint32_t row, uint32_t *length,
                                        uint8_t scratch[8]) {
    const pnx_global *table = &st->globals[x->key & 0xfffu];
    const uint8_t *field = table->ptr + (size_t)(row - 1) * table->elem_bytes + ((x->key >> 12) & 0xfffu);
    if (x->key_is_str) {
        *length = field[0];
        return field + 1;
    }
    const int64_t v = pnx_load_width(field, x->key >> 24);
    __builtin_memcpy(scratch, &v, 8);
    *length = 8;
    return scratch;
}

// Finds the slot of a key. Returns the slot that holds it, or the slot where it would go
// (the first removed slot met, else the empty one) with *found false.
PNX_INLINE uint32_t pnx_index_find(const pinax_state *st, const pnx_global *x, const uint8_t *key, uint32_t length,
                                   bool *found) {
    const uint32_t mask = x->capacity - 1;
    uint32_t slot = (uint32_t)pnx_hash(st->hash_key, key, length) & mask, spare = UINT32_MAX;
    *found = false;
    for (uint32_t step = 0; step < x->capacity; ++step, slot = (slot + 1) & mask) {
        uint32_t row;
        __builtin_memcpy(&row, x->ptr + (size_t)slot * 4, 4);
        if (row == 0)
            return spare != UINT32_MAX ? spare : slot;
        if (row == PNX_IX_GONE) {
            if (spare == UINT32_MAX)
                spare = slot;
            continue;
        }
        uint32_t n;
        uint8_t  scratch[8];
        const uint8_t *have = pnx_index_key(st, x, row, &n, scratch);
        if (n == length && __builtin_memcmp(have, key, n) == 0) {
            *found = true;
            return slot;
        }
    }
    return spare; // every slot was used once; UINT32_MAX when none is free
}

PNX_INLINE int pnx_index_get(pinax_state *st, const pnx_global *x, const uint8_t *key, uint32_t length,
                             int64_t *out) {
    bool found;
    const uint32_t slot = pnx_index_find(st, x, key, length, &found);
    if (!found) {
        st->fail = PINAX_ERR_NOT_FOUND;
        return PNX_R_FAILED;
    }
    uint32_t row;
    __builtin_memcpy(&row, x->ptr + (size_t)slot * 4, 4);
    *out = row;
    return PNX_R_OK;
}
PNX_OP(IXGET) {
    uint8_t key[8];
    __builtin_memcpy(key, &R[OP_B(w)], 8);
    return pnx_index_get(st, &st->globals[x], key, 8, &R[OP_A(w)]);
}
PNX_OP(IXGETS) {
    const pnx_scell s = S[OP_B(w)];
    return pnx_index_get(st, &st->globals[x], pnx_view_bytes(st, s), s.length, &R[OP_A(w)]);
}
PNX_OP(IXPUT) {
    const pnx_global *ix = &st->globals[OP_BX(w)];
    const uint64_t row = (uint64_t)R[OP_A(w)];
    if (row - 1 >= st->globals[ix->key & 0xfffu].length)
        return PINAX_ERR_RANGE;
    uint32_t n;
    uint8_t  scratch[8];
    const uint8_t *key = pnx_index_key(st, ix, (uint32_t)row, &n, scratch);
    bool found;
    const uint32_t slot = pnx_index_find(st, ix, key, n, &found);
    if (found || slot == UINT32_MAX) {
        st->fail = found ? PINAX_ERR_FAILED : PINAX_ERR_FULL;
        return PNX_R_FAILED;
    }
    const uint32_t value = (uint32_t)row;
    __builtin_memcpy(ix->ptr + (size_t)slot * 4, &value, 4);
    return PNX_R_OK;
}
PNX_OP(IXDEL) { // nothing happens when the row is not in the index
    const pnx_global *ix = &st->globals[OP_BX(w)];
    const uint64_t row = (uint64_t)R[OP_A(w)];
    if (row - 1 >= st->globals[ix->key & 0xfffu].length)
        return PINAX_ERR_RANGE;
    uint32_t n;
    uint8_t  scratch[8];
    const uint8_t *key = pnx_index_key(st, ix, (uint32_t)row, &n, scratch);
    bool found;
    const uint32_t slot = pnx_index_find(st, ix, key, n, &found);
    if (found) {
        uint32_t have;
        __builtin_memcpy(&have, ix->ptr + (size_t)slot * 4, 4);
        if (have == (uint32_t)row) {
            const uint32_t gone = PNX_IX_GONE;
            __builtin_memcpy(ix->ptr + (size_t)slot * 4, &gone, 4);
        }
    }
    return PNX_R_OK;
}

#endif // PINAX_OPS_H
