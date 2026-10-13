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
    const pnx_global *g = &st->globals[OP_C(w)];
    const uint64_t index = (uint64_t)R[OP_B(w)];
    if (index >= g->length)
        return PINAX_ERR_RANGE;
    __builtin_memcpy(&R[OP_A(w)], g->ptr + index * 8, 8);
    return PNX_R_OK;
}
PNX_OP(ASET) {
    const pnx_global *g = &st->globals[OP_C(w)];
    const uint64_t index = (uint64_t)R[OP_B(w)];
    if (index >= g->length)
        return PINAX_ERR_RANGE;
    __builtin_memcpy(g->ptr + index * 8, &R[OP_A(w)], 8);
    return PNX_R_OK;
}
PNX_OP(ROW) {
    const uint64_t index = (uint64_t)R[OP_B(w)];
    if (index >= st->globals[OP_C(w)].length)
        return PINAX_ERR_RANGE;
    R[OP_A(w)] = (int64_t)index + 1;
    return PNX_R_OK;
}
PNX_OP(FGET) {
    const pnx_global *g = &st->globals[x & 0xfffu];
    const uint64_t row = (uint64_t)R[OP_B(w)] - 1; // "no row" wraps and is out of range
    if (row >= g->length)
        return PINAX_ERR_RANGE;
    R[OP_A(w)] = pnx_load_width(g->ptr + row * g->elem_bytes + ((x >> 12) & 0xfffu), x >> 24);
    return PNX_R_OK;
}
PNX_OP(FSET) {
    const pnx_global *g = &st->globals[x & 0xfffu];
    const uint64_t row = (uint64_t)R[OP_B(w)] - 1; // "no row" wraps and is out of range
    const int64_t value = R[OP_A(w)];
    if (row >= g->length)
        return PINAX_ERR_RANGE;
    if (!pnx_fits_width(value, x >> 24))
        return PINAX_ERR_OVERFLOW;
    __builtin_memcpy(g->ptr + row * g->elem_bytes + ((x >> 12) & 0xfffu), &value, pnx_width_bytes[x >> 24]);
    return PNX_R_OK;
}
PNX_OP(FGETS) {
    const pnx_global *g = &st->globals[x & 0xfffu];
    const uint64_t row = (uint64_t)R[OP_B(w)] - 1; // "no row" wraps and is out of range
    if (row >= g->length)
        return PINAX_ERR_RANGE;
    const uint8_t *field = g->ptr + row * g->elem_bytes + ((x >> 12) & 0xfffu);
    S[OP_A(w)] = (pnx_scell){PNX_SEG_GLOBAL, field[0], (uint64_t)(field + 1 - st->segment[PNX_SEG_GLOBAL])};
    return PNX_R_OK;
}
PNX_OP(FSETS) {
    const pnx_global *g = &st->globals[x & 0xfffu];
    const uint64_t row = (uint64_t)R[OP_B(w)] - 1; // "no row" wraps and is out of range
    const pnx_scell value = S[OP_A(w)];
    if (row >= g->length)
        return PINAX_ERR_RANGE;
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
    const pnx_global *g = &st->globals[OP_C(w)];
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
    const pnx_global *g = &st->globals[OP_C(w)];
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
PNX_OP(SFIND) {
    const pnx_scell hay = S[OP_B(w)], part = S[OP_C(w)];
    const uint32_t at = pnx_find(pnx_view_bytes(st, hay), hay.length, pnx_view_bytes(st, part), part.length);
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
    return PNX_R_OK;
}

PNX_OP(SREPL) {
    const pnx_scell s = S[OP_B(w)], old = S[OP_B(w) + 1], with = S[OP_B(w) + 2];
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
    uint32_t slot = (uint32_t)pnx_hash(key, length) & mask, spare = UINT32_MAX;
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
    return pnx_index_get(st, &st->globals[OP_C(w)], key, 8, &R[OP_A(w)]);
}
PNX_OP(IXGETS) {
    const pnx_scell s = S[OP_B(w)];
    return pnx_index_get(st, &st->globals[OP_C(w)], pnx_view_bytes(st, s), s.length, &R[OP_A(w)]);
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
