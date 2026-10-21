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
// A collection reached through a reference (RFC-0018) is named by PNX_GSELECTED (in a field,
// PNX_FSELECTED): the global that the GSEL right before chose.
#define PNX_GX(st, i) ((i) == PNX_GSELECTED ? (st)->gsel : (i))
#define PNX_GF(st, x) (((x) & 0xfffu) == PNX_FSELECTED ? (st)->gsel : (x) & 0xfffu)
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

PNX_OP(GETG) { __builtin_memcpy(&R[OP_A(w)], st->globals[PNX_GX(st, OP_BX(w))].ptr, 8); return PNX_R_OK; }
PNX_OP(SETG) { __builtin_memcpy(st->globals[PNX_GX(st, OP_BX(w))].ptr, &R[OP_A(w)], 8); return PNX_R_OK; }
PNX_OP(AGET) {
    const pnx_global *g = &st->globals[PNX_GX(st, x)];
    const uint64_t index = (uint64_t)R[OP_B(w)];
    if (index >= g->length)
        return PINAX_ERR_RANGE;
    __builtin_memcpy(&R[OP_A(w)], g->ptr + index * 8, 8);
    return PNX_R_OK;
}
PNX_OP(ASET) {
    const pnx_global *g = &st->globals[PNX_GX(st, x)];
    const uint64_t index = (uint64_t)R[OP_B(w)];
    if (index >= g->length)
        return PINAX_ERR_RANGE;
    __builtin_memcpy(g->ptr + index * 8, &R[OP_A(w)], 8);
    return PNX_R_OK;
}
PNX_OP(ROW) {
    const uint64_t index = (uint64_t)R[OP_B(w)];
    if (index >= st->globals[PNX_GX(st, x)].length)
        return PINAX_ERR_RANGE;
    R[OP_A(w)] = (int64_t)index + 1;
    return PNX_R_OK;
}
PNX_OP(FGET) {
    const pnx_global *g = &st->globals[PNX_GF(st, x)];
    uint64_t row;
    const int bad = pnx_row_of(g, R[OP_B(w)], &row);
    if (bad)
        return bad;
    R[OP_A(w)] = pnx_load_width(g->ptr + row * g->elem_bytes + ((x >> 12) & 0xfffu), x >> 24);
    return PNX_R_OK;
}
PNX_OP(FSET) {
    const pnx_global *g = &st->globals[PNX_GF(st, x)];
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
    const pnx_global *g = &st->globals[PNX_GF(st, x)];
    uint64_t row;
    const int bad = pnx_row_of(g, R[OP_B(w)], &row);
    if (bad)
        return bad;
    const uint8_t *field = g->ptr + row * g->elem_bytes + ((x >> 12) & 0xfffu);
    // never more than N: a shared table may come from a file another process wrote
    const uint32_t length = field[0] <= (x >> 24) ? field[0] : x >> 24;
    S[OP_A(w)] = (pnx_scell){PNX_SEG_GLOBAL, length, (uint64_t)(field + 1 - st->segment[PNX_SEG_GLOBAL])};
    return PNX_R_OK;
}
PNX_OP(FSETS) {
    const pnx_global *g = &st->globals[PNX_GF(st, x)];
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
    if (rest->segment == PNX_SEG_DONE)
        return PNX_R_FAILED;
    const uint32_t n = pnx_find_byte(pnx_view_bytes(st, *rest), rest->length, (uint8_t)OP_C(w));
    st->charge += n != UINT32_MAX ? n : rest->length;
    if (n != UINT32_MAX) {
        S[OP_B(w)] = (pnx_scell){rest->segment, n, rest->offset};
        rest->offset += n + 1u;
        rest->length -= n + 1u;
    } else {
        S[OP_B(w)] = *rest;
        *rest = PNX_SNEXT_DONE;
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
    __builtin_memcpy(st->globals[PNX_GX(st, OP_BX(w))].ptr, &zero, 8);
    return PNX_R_OK;
}
// The bytes of `value` in width code `code` (PNX_W_BE, PNX_W_VARINT): their count.
PNX_INLINE uint32_t pnx_store_code(uint8_t out[10], int64_t value, uint32_t code) {
    if (code == PNX_W_VARINT) {
        uint64_t v = (uint64_t)value;
        uint32_t n = 0;
        do {
            out[n++] = (uint8_t)((v & 0x7fu) | (v > 0x7fu ? 0x80u : 0u));
            v >>= 7;
        } while (v);
        return n;
    }
    const uint32_t n = pnx_width_bytes[code & 7u];
    for (uint32_t k = 0; k < n; ++k) // little-endian whatever the machine
        out[code & PNX_W_BE ? n - 1 - k : k] = (uint8_t)((uint64_t)value >> (8 * k));
    return n;
}
// The value at the front of `p` (`length` bytes) in width code `code`, and the bytes it took;
// 0 when there are not enough, or a varint is longer than ten bytes or past 64 bits.
PNX_INLINE uint32_t pnx_load_code(const uint8_t *p, uint32_t length, uint32_t code, int64_t *value) {
    if (code == PNX_W_VARINT) {
        uint64_t v = 0;
        for (uint32_t k = 0; k < length && k < 10; ++k) {
            if (k == 9 && p[k] > 1)
                return 0;
            v |= (uint64_t)(p[k] & 0x7fu) << (7 * k);
            if (!(p[k] & 0x80u)) {
                *value = (int64_t)v;
                return k + 1;
            }
        }
        return 0;
    }
    const uint32_t width = code & 7u, n = pnx_width_bytes[width];
    if (length < n)
        return 0;
    uint8_t le[8];
    for (uint32_t k = 0; k < n; ++k)
        le[k] = p[code & PNX_W_BE ? n - 1 - k : k];
    *value = pnx_load_width(le, width);
    return n;
}

PNX_OP(BPUT) {
    const pnx_global *g = &st->globals[PNX_GX(st, x)];
    const int64_t value = R[OP_A(w)];
    const uint32_t code = OP_B(w);
    uint64_t len = pnx_buffer_len(g);
    if (code != PNX_W_VARINT && !pnx_fits_width(value, code & 7u))
        return PINAX_ERR_OVERFLOW;
    uint8_t bytes[10];
    const uint32_t n = pnx_store_code(bytes, value, code);
    if (len + n > g->capacity) {
        st->fail = PINAX_ERR_FULL;
        return PNX_R_FAILED;
    }
    __builtin_memcpy(g->ptr + 8 + len, bytes, n);
    len += n;
    __builtin_memcpy(g->ptr, &len, 8);
    return PNX_R_OK;
}
// B 0: one length byte, then the bytes (longer than 255 is a fault); B 1: the length as a varint
PNX_OP(BPUTS) {
    const pnx_global *g = &st->globals[PNX_GX(st, x)];
    const pnx_scell s = S[OP_A(w)];
    uint64_t len = pnx_buffer_len(g);
    if (!OP_B(w) && s.length > 255)
        return PINAX_ERR_RANGE;
    uint8_t head[10];
    const uint32_t h = OP_B(w) ? pnx_store_code(head, s.length, PNX_W_VARINT) : (head[0] = (uint8_t)s.length, 1u);
    if (len + h + s.length > g->capacity) {
        st->fail = PINAX_ERR_FULL;
        return PNX_R_FAILED;
    }
    __builtin_memcpy(g->ptr + 8 + len, head, h);
    __builtin_memmove(g->ptr + 8 + len + h, pnx_view_bytes(st, s), s.length);
    len += h + s.length;
    __builtin_memcpy(g->ptr, &len, 8);
    return PNX_R_OK;
}
// The buffer cut back to R[A] bytes, or the list to R[A] elements; more than it holds, or
// fewer than none, is a fault.
PNX_OP(BTRUNC) {
    pnx_global    *g = &st->globals[PNX_GX(st, OP_BX(w))];
    const uint64_t n = (uint64_t)R[OP_A(w)];
    if (g->kind != PNX_G_BUFFER) {
        if (n > g->length)
            return PINAX_ERR_RANGE;
        g->length = (uint32_t)n;
        return PNX_R_OK;
    }
    if (n > pnx_buffer_len(g))
        return PINAX_ERR_RANGE;
    __builtin_memcpy(g->ptr, &n, 8);
    return PNX_R_OK;
}
// The bytes R[B] takes as a varint.
PNX_OP(VLEN) {
    uint8_t bytes[10];
    R[OP_A(w)] = pnx_store_code(bytes, R[OP_B(w)], PNX_W_VARINT);
    return PNX_R_OK;
}
PNX_OP(BVIEW) {
    const pnx_global *g = &st->globals[PNX_GX(st, OP_BX(w))];
    S[OP_A(w)] = (pnx_scell){PNX_SEG_GLOBAL, (uint32_t)pnx_buffer_len(g),
                             (uint64_t)(g->ptr + 8 - st->segment[PNX_SEG_GLOBAL])};
    return PNX_R_OK;
}
PNX_OP(SGET) {
    pnx_scell *s = &S[OP_B(w)];
    const uint32_t n = pnx_load_code(pnx_view_bytes(st, *s), s->length, OP_C(w), &R[OP_A(w)]);
    if (!n) {
        st->fail = PINAX_ERR_FORMAT;
        return PNX_R_FAILED;
    }
    s->offset += n;
    s->length -= n;
    return PNX_R_OK;
}
// C 0: a length byte, then the bytes; C 1: the length as a varint
PNX_OP(SGETS) {
    pnx_scell *s = &S[OP_B(w)];
    const uint8_t *p = pnx_view_bytes(st, *s);
    int64_t  n = 0;
    uint32_t h = 0;
    if (OP_C(w))
        h = pnx_load_code(p, s->length, PNX_W_VARINT, &n);
    else if (s->length)
        n = p[0], h = 1;
    if (!h || n < 0 || (uint64_t)n > s->length - h) {
        st->fail = PINAX_ERR_FORMAT;
        return PNX_R_FAILED;
    }
    S[OP_A(w)] = (pnx_scell){s->segment, (uint32_t)n, s->offset + h};
    s->offset += h + (uint32_t)n;
    s->length -= h + (uint32_t)n;
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
PNX_OP(RCATD) {
    uint8_t digits[24];
    const uint32_t n = pnx_format_dec(R[OP_B(w)], OP_C(w), digits);
    return pnx_region_append(st, &S[OP_A(w)], digits, n);
}
PNX_OP(STODEC) {
    const pnx_scell s = S[OP_B(w)];
    if (pnx_parse_dec(pnx_view_bytes(st, s), s.length, OP_C(w), &R[OP_A(w)]))
        return PNX_R_OK;
    st->fail = PINAX_ERR_FORMAT;
    return PNX_R_FAILED;
}
PNX_OP(DRESC) { // rounded half away from zero
    const int64_t v = R[OP_B(w)], p = pnx_pow10[OP_C(w)];
    int64_t q = v / p;
    const int64_t r = v % p;
    if ((r < 0 ? -r : r) >= p - (r < 0 ? -r : r)) // |r| * 2 >= p, without overflow
        q += v < 0 ? -1 : 1;
    R[OP_A(w)] = q;
    return PNX_R_OK;
}
PNX_OP(RCATB) {
    return R[OP_B(w)] ? pnx_region_append(st, &S[OP_A(w)], (const uint8_t *)"true", 4)
                      : pnx_region_append(st, &S[OP_A(w)], (const uint8_t *)"false", 5);
}

// ---- arrays and lists of integers ---------------------------------------------------------

PNX_OP(GCAP) { R[OP_A(w)] = st->globals[PNX_GX(st, OP_BX(w))].capacity; return PNX_R_OK; }
PNX_OP(GLEN) { R[OP_A(w)] = st->globals[PNX_GX(st, OP_BX(w))].length; return PNX_R_OK; }
PNX_OP(GCLEAR) { // an array is zeroed; a list becomes empty
    pnx_global *g = &st->globals[PNX_GX(st, OP_BX(w))];
    if (g->kind == PNX_G_LIST || g->kind == PNX_G_ROWS || g->kind == PNX_G_QUEUE) {
        g->length = g->head = 0;
        st->qturn += g->kind == PNX_G_QUEUE;
    }
    else
        __builtin_memset(g->ptr, 0, (size_t)g->capacity * g->elem_bytes);
    return PNX_R_OK;
}
PNX_OP(ADDROW) {
    pnx_global *g = &st->globals[PNX_GX(st, OP_BX(w))];
    if (g->length == g->capacity) {
        st->fail = PINAX_ERR_FULL;
        return PNX_R_FAILED;
    }
    __builtin_memset(g->ptr + (size_t)g->length * g->elem_bytes, 0, g->elem_bytes);
    R[OP_A(w)] = (int64_t)++g->length;
    return PNX_R_OK;
}
PNX_OP(LPUSH) {
    pnx_global *g = &st->globals[PNX_GX(st, OP_BX(w))];
    if (g->length == g->capacity) {
        st->fail = PINAX_ERR_FULL;
        return PNX_R_FAILED;
    }
    __builtin_memcpy(g->ptr + (size_t)g->length * 8, &R[OP_A(w)], 8);
    ++g->length;
    return PNX_R_OK;
}
PNX_OP(LPOP) {
    pnx_global *g = &st->globals[PNX_GX(st, OP_BX(w))];
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
    pnx_global *g = &st->globals[PNX_GX(st, OP_BX(w))];
    const pnx_global *from = &st->globals[PNX_GX(st, x)];
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
    pnx_global *g = &st->globals[PNX_GX(st, OP_BX(w))];
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
    pnx_global *g = &st->globals[PNX_GX(st, OP_BX(w))];
    if (g->length == 0) {
        st->fail = PINAX_ERR_RANGE;
        return PNX_R_FAILED;
    }
    const uint8_t *cell = g->ptr + (size_t)(g->length - 1) * g->elem_bytes;
    // never more than N: the cell may have come from a file or bound memory (as FGETS)
    const uint32_t n = cell[0] < g->elem_bytes ? cell[0] : g->elem_bytes - 1;
    uint8_t *copy = n ? pnx_region_alloc(st, n) : NULL;
    if (n && !copy)
        return PINAX_ERR_FULL;
    --g->length;
    if (!n) {
        S[OP_A(w)] = (pnx_scell){PNX_SEG_EMPTY, 0, 0};
        return PNX_R_OK;
    }
    __builtin_memcpy(copy, cell + 1, n);
    S[OP_A(w)] = (pnx_scell){PNX_SEG_REGION, n, (uint64_t)(copy - st->region)};
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
    return pnx_buffer_append(st, &st->globals[PNX_GX(st, OP_BX(w))], pnx_view_bytes(st, s), s.length);
}
PNX_OP(BCATI) {
    uint8_t digits[24];
    const uint32_t n = pnx_format_int(R[OP_A(w)], digits);
    return pnx_buffer_append(st, &st->globals[PNX_GX(st, OP_BX(w))], digits, n);
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
    pnx_global *g = &st->globals[PNX_GX(st, OP_BX(w))];
    if (g->length == g->capacity) {
        st->fail = PINAX_ERR_FULL;
        return PNX_R_FAILED;
    }
    const uint32_t at = (uint32_t)(((uint64_t)g->head + g->length) % g->capacity);
    __builtin_memcpy(g->ptr + (size_t)at * 8, &R[OP_A(w)], 8);
    ++g->length;
    ++st->qturn;
    return PNX_R_OK;
}
PNX_OP(QTAKE) {
    pnx_global *g = &st->globals[PNX_GX(st, OP_BX(w))];
    if (g->length == 0) {
        st->fail = PINAX_ERR_RANGE;
        return PNX_R_FAILED;
    }
    __builtin_memcpy(&R[OP_A(w)], g->ptr + (size_t)g->head * 8, 8);
    g->head = g->head + 1 == g->capacity ? 0 : g->head + 1;
    --g->length;
    ++st->qturn;
    return PNX_R_OK;
}
PNX_OP(QPEEK) {
    const pnx_global *g = &st->globals[PNX_GX(st, OP_BX(w))];
    if (g->length == 0) {
        st->fail = PINAX_ERR_RANGE;
        return PNX_R_FAILED;
    }
    __builtin_memcpy(&R[OP_A(w)], g->ptr + (size_t)g->head * 8, 8);
    return PNX_R_OK;
}

PNX_OP(PALLOC) {
    pnx_global *g = &st->globals[PNX_GX(st, OP_BX(w))];
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
    pnx_global *g = &st->globals[PNX_GX(st, OP_BX(w))];
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
    R[OP_A(w)] = pnx_row_of(&st->globals[PNX_GX(st, x)], R[OP_B(w)], &row) == PNX_R_OK;
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
    if (rest->segment == PNX_SEG_DONE)
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
        *rest = PNX_SNEXT_DONE;
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
        const int64_t width = R[OP_C(w)];
        return r != PNX_R_OK ? r : pnx_append_padded(st, &S[OP_A(w)], digits + 1, n - 1, width > 0 ? width - 1 : width, '0', false);
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

// ---- the module ptn (RFC-0016 S3) ------------------------------------------------------------

PNX_INLINE pnx_ptn_found *pnx_ptn_now(pinax_state *st) {
    return st->task ? &st->task->ptn : &st->ptn;
}
// The first match in `s` at or after `from`, its captures kept for ptn.capture as views of s.
// PNX_R_OK found, PNX_R_FAILED none, or a fault: a malformed pattern, or one that went back
// too often.
PNX_INLINE int pnx_ptn_run(pinax_state *st, pnx_scell s, uint32_t from, pnx_scell pat, pnx_ptn_caps *caps) {
    uint64_t  steps = 0;
    const int r = pnx_ptn_find(pnx_view_bytes(st, s), s.length, from, pnx_view_bytes(st, pat), pat.length, caps, &steps);
    st->charge += steps;
    if (r < 0)
        return r == -1 ? PINAX_ERR_FORMAT : PINAX_ERR_DEPTH;
    if (!r)
        return PNX_R_FAILED;
    pnx_ptn_found *f = pnx_ptn_now(st);
    f->cap[0] = (pnx_scell){s.segment, caps->len, s.offset + caps->at};
    for (uint32_t k = 0; k < caps->n; ++k)
        f->cap[k + 1] = (pnx_scell){s.segment, caps->clen[k], s.offset + caps->start[k]};
    f->n = 1 + caps->n;
    return PNX_R_OK;
}
PNX_OP(PMATCH) {
    pnx_ptn_caps caps;
    const int    r = pnx_ptn_run(st, S[OP_B(w)], 0, S[OP_C(w)], &caps);
    if (r == PNX_R_OK)
        S[OP_A(w)] = pnx_ptn_now(st)->cap[0];
    else if (r == PNX_R_FAILED)
        st->fail = PINAX_ERR_NOT_FOUND;
    return r;
}
// Capture R[B] of the last match (0: the whole match); none such is a fault.
PNX_OP(PCAP) {
    const pnx_ptn_found *f = pnx_ptn_now(st);
    const int64_t        k = R[OP_B(w)];
    if (k < 0 || (uint64_t)k >= f->n)
        return PINAX_ERR_RANGE;
    S[OP_A(w)] = f->cap[k];
    return PNX_R_OK;
}
// A: what is left to search; B: the next match. An empty match moves on by a byte.
PNX_OP(PNEXT) {
    pnx_scell *rest = &S[OP_A(w)];
    if (rest->segment == PNX_SEG_DONE)
        return PNX_R_FAILED;
    pnx_ptn_caps caps;
    const int    r = pnx_ptn_run(st, *rest, 0, S[OP_C(w)], &caps);
    if (r != PNX_R_OK) {
        if (r == PNX_R_FAILED)
            *rest = PNX_SNEXT_DONE;
        return r;
    }
    S[OP_B(w)] = pnx_ptn_now(st)->cap[0];
    uint32_t past = caps.at + caps.len;
    const pnx_scell pat = S[OP_C(w)];
    if ((caps.len == 0 && past == rest->length) || (pat.length && pnx_view_bytes(st, pat)[0] == '^')) {
        // nothing after an empty match at the end; an anchored pattern matches once, at the start
        *rest = PNX_SNEXT_DONE;
        return PNX_R_OK;
    }
    if (caps.len == 0)
        ++past;
    rest->offset += past;
    rest->length -= past;
    return PNX_R_OK;
}
// A new str in the region: B with every match of pattern C replaced by the str in register x,
// where %0 is the match, %1 to %9 its captures and %% a %.
PNX_OP(PREPL) {
    const pnx_scell s = S[OP_B(w)], pat = S[OP_C(w)], with = S[x];
    const uint8_t  *bytes = pnx_view_bytes(st, s), *to = pnx_view_bytes(st, with);
    pnx_scell       out = {PNX_SEG_REGION, 0, st->region_used};
    uint32_t        at = 0;
    while (at <= s.length) {
        pnx_ptn_caps caps;
        const int    r = pnx_ptn_run(st, s, at, pat, &caps);
        if (r == PNX_R_FAILED)
            break;
        if (r != PNX_R_OK)
            return r;
        int e = pnx_region_append(st, &out, bytes + at, caps.at - at);
        for (uint32_t k = 0; e == PNX_R_OK && k < with.length; ++k) {
            if (to[k] != '%' || k + 1 == with.length) {
                e = pnx_region_append(st, &out, to + k, 1);
                continue;
            }
            const uint8_t d = to[++k];
            if (d >= '0' && d <= '9' && (uint32_t)(d - '0') <= caps.n) {
                const uint32_t from = d == '0' ? caps.at : caps.start[d - '1'], n = d == '0' ? caps.len : caps.clen[d - '1'];
                e = pnx_region_append(st, &out, bytes + from, n);
            } else if (d == '%') {
                e = pnx_region_append(st, &out, to + k, 1);
            } else {
                e = pnx_region_append(st, &out, to + k - 1, 2);
            }
        }
        if (e != PNX_R_OK)
            return e;
        at = caps.at + caps.len;
        if (pat.length && pnx_view_bytes(st, pat)[0] == '^') // anchored: only at the start, as in Lua's gsub
            break;
        if (caps.len == 0) { // an empty match: the byte after it is kept and the search moves on
            if (at < s.length && (e = pnx_region_append(st, &out, bytes + at, 1)) != PNX_R_OK)
                return e;
            ++at;
        }
    }
    if (at < s.length) {
        const int e = pnx_region_append(st, &out, bytes + at, s.length - at);
        if (e != PNX_R_OK)
            return e;
    }
    S[OP_A(w)] = out;
    return PNX_R_OK;
}

// ---- the module rex (RFC-0011, RFC-0016 S4) ----------------------------------------------

// A search with the program in view `re`: its work lies at the end of the region, out of reach
// of what is built there meanwhile, and is given back after. PNX_R_OK (the groups in `found`
// and kept for the block parameters), PNX_R_FAILED none, or a fault: not a program (format),
// no room for the work (full).
PNX_INLINE int pnx_rex_run(pinax_state *st, pnx_scell s, uint32_t from, pnx_scell re, uint32_t found[2 + 2 * PNX_PTN_CAPS]) {
    const uint8_t *prog = pnx_view_bytes(st, re);
    const size_t   need = pnx_rex_work(prog, re.length);
    if (!need)
        return PINAX_ERR_FORMAT;
    const uint32_t end = st->region_len & ~7u, saved = st->region_len;
    if (need > end - st->region_used)
        return PINAX_ERR_FULL;
    st->region_len = (uint32_t)((end - need) & ~(size_t)7u);
    uint64_t  steps = 0;
    const int r = pnx_rex_search(prog, re.length, pnx_view_bytes(st, s), s.length, from, found, st->region + st->region_len,
                                 &steps);
    st->region_len = saved;
    st->charge += steps;
    if (r < 0)
        return PINAX_ERR_FORMAT;
    if (!r)
        return PNX_R_FAILED;
    pnx_ptn_found *f = pnx_ptn_now(st);
    const uint32_t groups = pnx_rex_groups(prog, re.length);
    for (uint32_t k = 0; k <= groups; ++k) {
        const uint32_t a = found[2 * k], b = found[2 * k + 1];
        f->cap[k] = a == PNX_REX_NONE || b == PNX_REX_NONE || b < a ? (pnx_scell){PNX_SEG_EMPTY, 0, 0}
                                                                     : (pnx_scell){s.segment, b - a, s.offset + a};
    }
    f->n = groups + 1;
    return PNX_R_OK;
}
PNX_INLINE uint32_t pnx_utf8_len(const uint8_t *b, uint32_t n);

// The bytes of one code point at the front of `p` (one when it is not a valid sequence).
PNX_INLINE uint32_t pnx_cp_len(const uint8_t *p, uint32_t n) {
    const uint32_t k = n ? pnx_utf8_len(p, n) : 0;
    return k ? k : 1;
}
PNX_OP(XMATCH) {
    uint32_t  found[2 + 2 * PNX_PTN_CAPS];
    const int r = pnx_rex_run(st, S[OP_B(w)], 0, S[OP_C(w)], found);
    if (r == PNX_R_OK)
        S[OP_A(w)] = pnx_ptn_now(st)->cap[0];
    else if (r == PNX_R_FAILED)
        st->fail = PINAX_ERR_NOT_FOUND;
    return r;
}
// A: the str in, what lies before the match out; B: the program in, the match out; C: after.
PNX_OP(XCUT) {
    uint32_t        found[2 + 2 * PNX_PTN_CAPS];
    const pnx_scell s = S[OP_A(w)];
    const int       r = pnx_rex_run(st, s, 0, S[OP_B(w)], found);
    if (r == PNX_R_FAILED)
        st->fail = PINAX_ERR_NOT_FOUND;
    if (r != PNX_R_OK)
        return r;
    S[OP_B(w)] = pnx_ptn_now(st)->cap[0];
    S[OP_A(w)] = (pnx_scell){s.segment, found[0], s.offset};
    S[OP_C(w)] = (pnx_scell){s.segment, s.length - found[1], s.offset + found[1]};
    return PNX_R_OK;
}
// The stems of rex go over the whole string C, from the place R[A] (past the end: done), so
// that ^, $ and \b see the bytes around; the program is in register x.
// After a match at m0..m1 the next search starts at m1, or a code point on after an empty one.
PNX_INLINE uint32_t pnx_rex_past(const pinax_state *st, pnx_scell s, uint32_t m0, uint32_t m1) {
    if (m1 > m0)
        return m1;
    return m1 >= s.length ? s.length + 1 : m1 + pnx_cp_len(pnx_view_bytes(st, s) + m1, s.length - m1);
}
// B: the next match (its groups kept for XCAPI). PNX_R_FAILED: none is left.
PNX_OP(XNEXT) {
    const pnx_scell s = S[OP_C(w)];
    const int64_t   at = R[OP_A(w)];
    if (at < 0 || at > (int64_t)s.length)
        return PNX_R_FAILED;
    uint32_t  found[2 + 2 * PNX_PTN_CAPS];
    const int r = pnx_rex_run(st, s, (uint32_t)at, S[x], found);
    if (r != PNX_R_OK) {
        if (r == PNX_R_FAILED)
            R[OP_A(w)] = (int64_t)s.length + 1;
        return r;
    }
    S[OP_B(w)] = pnx_ptn_now(st)->cap[0];
    R[OP_A(w)] = pnx_rex_past(st, s, found[0], found[1]);
    return PNX_R_OK;
}
// B: the next piece, from R[A] up to the next match (an empty match right at R[A] does not
// count); the last piece runs to the end.
PNX_OP(XSPLIT) {
    const pnx_scell s = S[OP_C(w)];
    const int64_t   at = R[OP_A(w)];
    if (at < 0 || at > (int64_t)s.length)
        return PNX_R_FAILED;
    uint32_t found[2 + 2 * PNX_PTN_CAPS];
    int      r = pnx_rex_run(st, s, (uint32_t)at, S[x], found);
    if (r == PNX_R_OK && found[1] <= found[0] && found[0] == at) { // empty right here: a code point on
        r = at < (int64_t)s.length
                ? pnx_rex_run(st, s, (uint32_t)at + pnx_cp_len(pnx_view_bytes(st, s) + at, s.length - (uint32_t)at), S[x], found)
                : PNX_R_FAILED;
    }
    if (r == PNX_R_FAILED) {
        S[OP_B(w)] = (pnx_scell){s.segment, s.length - (uint32_t)at, s.offset + (uint32_t)at};
        R[OP_A(w)] = (int64_t)s.length + 1;
        return PNX_R_OK;
    }
    if (r != PNX_R_OK)
        return r;
    S[OP_B(w)] = (pnx_scell){s.segment, found[0] - (uint32_t)at, s.offset + (uint32_t)at};
    R[OP_A(w)] = found[1] > found[0] ? found[1] : found[0];
    return PNX_R_OK;
}
// Group C of the last match (empty when it has no such group or the group took no part); C 10:
// the code point XSTEP kept after an empty match.
PNX_OP(XCAPI) {
    const pnx_ptn_found *f = pnx_ptn_now(st);
    S[OP_A(w)] = OP_C(w) == 10 ? f->skipped : OP_C(w) < f->n ? f->cap[OP_C(w)] : (pnx_scell){PNX_SEG_EMPTY, 0, 0};
    return PNX_R_OK;
}
// rex.replace_by, one match at a time. B: the text from R[A] to the next match of the program
// in register x in the whole string C (its groups kept). After an empty match the next code
// point is kept aside (XCAPI 10) and the search moves past it. PNX_R_FAILED: no match is left;
// what remains is C from R[A] on.
PNX_OP(XSTEP) {
    const pnx_scell s = S[OP_C(w)];
    const int64_t   at = R[OP_A(w)];
    pnx_ptn_found  *f = pnx_ptn_now(st);
    if (at < 0 || at > (int64_t)s.length)
        return PNX_R_FAILED;
    uint32_t  found[2 + 2 * PNX_PTN_CAPS];
    const int r = pnx_rex_run(st, s, (uint32_t)at, S[x], found);
    if (r != PNX_R_OK)
        return r;
    S[OP_B(w)] = (pnx_scell){s.segment, found[0] - (uint32_t)at, s.offset + (uint32_t)at};
    f->skipped = (pnx_scell){PNX_SEG_EMPTY, 0, 0};
    const uint32_t past = pnx_rex_past(st, s, found[0], found[1]);
    if (found[1] <= found[0] && found[1] < s.length)
        f->skipped = (pnx_scell){s.segment, past - found[1], s.offset + found[1]};
    R[OP_A(w)] = past;
    return PNX_R_OK;
}
// B, with every match of C replaced by the str in register x (\0 the match, \1 to \9 the groups,
// \\ a backslash), made in the region; B gets it and A the number of matches replaced.
PNX_OP(XREPL) {
    const pnx_scell s = S[OP_B(w)], re = S[OP_C(w)], with = S[x];
    const uint8_t  *bytes = pnx_view_bytes(st, s), *to = pnx_view_bytes(st, with);
    pnx_scell       out = {PNX_SEG_REGION, 0, st->region_used};
    uint32_t        at = 0;
    int64_t         count = 0;
    while (at <= s.length) {
        uint32_t  found[2 + 2 * PNX_PTN_CAPS];
        const int r = pnx_rex_run(st, s, at, re, found);
        if (r == PNX_R_FAILED)
            break;
        if (r != PNX_R_OK)
            return r;
        ++count;
        int e = pnx_region_append(st, &out, bytes + at, found[0] - at);
        for (uint32_t k = 0; e == PNX_R_OK && k < with.length; ++k) {
            if (to[k] != '\\' || k + 1 == with.length) {
                e = pnx_region_append(st, &out, to + k, 1);
                continue;
            }
            const uint8_t d = to[++k];
            const pnx_ptn_found *f = pnx_ptn_now(st);
            if (d >= '0' && d <= '9') {
                const pnx_scell g = (uint32_t)(d - '0') < f->n ? f->cap[d - '0'] : (pnx_scell){PNX_SEG_EMPTY, 0, 0};
                e = pnx_region_append(st, &out, pnx_view_bytes(st, g), g.length);
            } else if (d == '\\') {
                e = pnx_region_append(st, &out, to + k, 1);
            } else {
                e = pnx_region_append(st, &out, to + k - 1, 2);
            }
        }
        if (e != PNX_R_OK)
            return e;
        at = found[1] > found[0] ? found[1] : found[0];
        if (found[1] <= found[0]) { // an empty match: the code point after it is kept, the search moves on
            if (at >= s.length)
                break;
            const uint32_t n = pnx_cp_len(bytes + at, s.length - at);
            if ((e = pnx_region_append(st, &out, bytes + at, n)) != PNX_R_OK)
                return e;
            at += n;
        }
    }
    if (at < s.length) {
        const int e = pnx_region_append(st, &out, bytes + at, s.length - at);
        if (e != PNX_R_OK)
            return e;
    }
    S[OP_B(w)] = out;
    R[OP_A(w)] = count;
    return PNX_R_OK;
}
// A program made from the str B, in the region (8-byte aligned). PNX_R_FAILED: B is no pattern
// (format), or the region has no room (full).
PNX_OP(XCOMP) {
    const pnx_scell pat = S[OP_B(w)];
    const uint32_t  start = (st->region_used + 7u) & ~7u;
    if (start > st->region_len) {
        st->fail = PINAX_ERR_FULL;
        return PNX_R_FAILED;
    }
    const char    *why = NULL;
    uint32_t       at = 0;
    const uint32_t n = pnx_rex_compile(pnx_view_bytes(st, pat), pat.length, st->region + start, st->region_len - start, &why, &at);
    st->charge += pat.length;
    if (!n) {
        st->fail = why ? PINAX_ERR_FORMAT : PINAX_ERR_FULL;
        return PNX_R_FAILED;
    }
    st->region_used = start + n;
    S[OP_A(w)] = (pnx_scell){PNX_SEG_REGION, n, start};
    return PNX_R_OK;
}

// ---- the modules json and csv (RFC-0016 S5) ------------------------------------------------

// The value R[A] fits width C. PNX_R_FAILED (format) otherwise: a number read from text.
PNX_OP(FITW) {
    if (pnx_fits_width(R[OP_A(w)], OP_C(w)))
        return PNX_R_OK;
    st->fail = PINAX_ERR_FORMAT;
    return PNX_R_FAILED;
}
// A: the kind of the next token of C; B: its value, a view (a string with escapes is decoded
// into the region); C: what follows it. PNX_R_FAILED: no token is left (not_found), or what
// follows is none (format).
PNX_OP(JNEXT) {
    pnx_scell     *rest = &S[OP_C(w)];
    const uint8_t *bytes = pnx_view_bytes(st, *rest);
    uint32_t       start = 0, len = 0, past = 0;
    bool           escaped = false;
    const int      kind = pnx_json_next(bytes, rest->length, &start, &len, &escaped, &past);
    st->charge += past;
    if (kind <= 0) {
        st->fail = kind == 0 ? PINAX_ERR_NOT_FOUND : PINAX_ERR_FORMAT;
        return PNX_R_FAILED;
    }
    pnx_scell value = {rest->segment, len, rest->offset + start};
    if (escaped) {
        const uint32_t before = st->region_used;
        uint8_t       *copy = len ? pnx_region_alloc(st, len) : NULL;
        if (len && !copy)
            return PINAX_ERR_FULL;
        const uint32_t n = pnx_json_unescape(bytes + start, len, copy);
        if (n == UINT32_MAX) {
            pnx_region_give_back(st, before);
            st->fail = PINAX_ERR_FORMAT;
            return PNX_R_FAILED;
        }
        st->region_used = (uint32_t)(copy - st->region) + n;
        value = (pnx_scell){PNX_SEG_REGION, n, (uint64_t)(copy - st->region)};
    }
    R[OP_A(w)] = kind;
    S[OP_B(w)] = value;
    rest->offset += past;
    rest->length -= past;
    return PNX_R_OK;
}
// A: past one whole value. PNX_R_FAILED: none is left (not_found), or it is malformed (format).
PNX_OP(JSKIP) {
    pnx_scell *rest = &S[OP_A(w)];
    uint32_t   past = 0;
    const int  r = pnx_json_skip(pnx_view_bytes(st, *rest), rest->length, &past);
    st->charge += past;
    if (r <= 0) {
        st->fail = r == 0 ? PINAX_ERR_NOT_FOUND : PINAX_ERR_FORMAT;
        return PNX_R_FAILED;
    }
    rest->offset += past;
    rest->length -= past;
    return PNX_R_OK;
}
// Whether a value written into a JSON buffer now needs a comma before it.
PNX_INLINE uint32_t pnx_json_comma(const pnx_global *g) {
    const uint64_t len = pnx_buffer_len(g);
    if (!len)
        return 0;
    const uint8_t last = g->ptr[8 + len - 1];
    return last != '{' && last != '[' && last != ':' && last != ',';
}
// B: what to write into buffer x: 1 { 2 } 3 [ 4 ] 7 the number R[A] 8 the bool R[A] 9 null.
PNX_OP(JPUTI) {
    const pnx_global *g = &st->globals[PNX_GX(st, x)];
    const uint32_t    kind = OP_B(w);
    uint8_t           out[32];
    uint32_t          n = 0;
    if (kind != PNX_JSON_OBJECT_END && kind != PNX_JSON_ARRAY_END && pnx_json_comma(g))
        out[n++] = ',';
    switch (kind) {
    case PNX_JSON_OBJECT: out[n++] = '{'; break;
    case PNX_JSON_OBJECT_END: out[n++] = '}'; break;
    case PNX_JSON_ARRAY: out[n++] = '['; break;
    case PNX_JSON_ARRAY_END: out[n++] = ']'; break;
    case PNX_JSON_NUMBER: n += pnx_format_int(R[OP_A(w)], out + n); break;
    case PNX_JSON_DEC + 0: case PNX_JSON_DEC + 1: case PNX_JSON_DEC + 2: case PNX_JSON_DEC + 3: case PNX_JSON_DEC + 4:
    case PNX_JSON_DEC + 5: case PNX_JSON_DEC + 6: case PNX_JSON_DEC + 7: case PNX_JSON_DEC + 8: case PNX_JSON_DEC + 9:
    case PNX_JSON_DEC + 10: case PNX_JSON_DEC + 11: case PNX_JSON_DEC + 12: case PNX_JSON_DEC + 13:
    case PNX_JSON_DEC + 14: case PNX_JSON_DEC + 15: case PNX_JSON_DEC + 16: case PNX_JSON_DEC + 17:
    case PNX_JSON_DEC + 18: n += pnx_format_dec(R[OP_A(w)], kind - PNX_JSON_DEC, out + n); break;
    case PNX_JSON_BOOLEAN:
        __builtin_memcpy(out + n, R[OP_A(w)] ? "true" : "false", R[OP_A(w)] ? 4 : 5);
        n += R[OP_A(w)] ? 4 : 5;
        break;
    default: __builtin_memcpy(out + n, "null", 4); n += 4; break;
    }
    return pnx_buffer_append(st, g, out, n);
}
// B: 5 the key A (and a colon), 6 the string A, quoted, into buffer x.
PNX_OP(JPUTS) {
    const pnx_global *g = &st->globals[PNX_GX(st, x)];
    const pnx_scell   s = S[OP_A(w)];
    uint64_t          len = pnx_buffer_len(g);
    const uint32_t    comma = pnx_json_comma(g), colon = OP_B(w) == PNX_JSON_KEY;
    uint8_t          *at = g->ptr + 8 + len;
    const uint64_t    room = g->capacity - len;
    st->charge += s.length;
    if (room < comma + colon) {
        st->fail = PINAX_ERR_FULL;
        return PNX_R_FAILED;
    }
    if (comma)
        at[0] = ',';
    const uint32_t n = pnx_json_quote(pnx_view_bytes(st, s), s.length, at + comma, room - comma - colon);
    if (n == UINT32_MAX) {
        st->fail = PINAX_ERR_FULL;
        return PNX_R_FAILED;
    }
    if (colon)
        at[comma + n] = ':';
    len += comma + n + colon;
    __builtin_memcpy(g->ptr, &len, 8);
    return PNX_R_OK;
}
// A: the rest of a CSV record or text; B: its next field (a quoted one with doubled quotes is
// decoded into the region). C 0: a stem over one record, done after its last field (a
// malformed quoted field is given as it stands, and ends it); C 1: a field that is not the last
// of its record; C 2: the last field of its record. PNX_R_FAILED: none is left, or (C 1, 2) the
// field is malformed or the record ends otherwise (format).
PNX_OP(CNEXT) {
    pnx_scell     *rest = &S[OP_A(w)];
    const uint32_t mode = OP_C(w);
    if (rest->segment == PNX_SEG_DONE)
        return PNX_R_FAILED;
    const uint8_t *bytes = pnx_view_bytes(st, *rest);
    uint32_t       start = 0, len = 0, past = 0;
    bool           doubled = false, last = false;
    const int      r = pnx_csv_field(bytes, rest->length, &start, &len, &doubled, &past, &last);
    st->charge += past;
    if (r < 0 || (mode == 1 && last) || (mode == 2 && !last)) {
        if (mode) {
            st->fail = PINAX_ERR_FORMAT;
            return PNX_R_FAILED;
        }
        S[OP_B(w)] = *rest; // as it stands
        *rest = PNX_SNEXT_DONE;
        return PNX_R_OK;
    }
    pnx_scell field = {rest->segment, len, rest->offset + start};
    if (doubled) { // "" becomes "
        uint8_t *copy = pnx_region_alloc(st, len);
        if (!copy)
            return PINAX_ERR_FULL;
        uint32_t n = 0;
        for (uint32_t k = 0; k < len; ++k) {
            copy[n++] = bytes[start + k];
            if (bytes[start + k] == '"')
                ++k;
        }
        st->region_used = (uint32_t)(copy - st->region) + n;
        field = (pnx_scell){PNX_SEG_REGION, n, (uint64_t)(copy - st->region)};
    }
    S[OP_B(w)] = field;
    if (mode == 0 && last) {
        *rest = PNX_SNEXT_DONE;
    } else {
        rest->offset += past;
        rest->length -= past;
    }
    return PNX_R_OK;
}
// A CSV field A into buffer x: a comma first unless it begins a line; quoted when it must be.
PNX_OP(CPUTS) {
    const pnx_global *g = &st->globals[PNX_GX(st, x)];
    const pnx_scell   s = S[OP_A(w)];
    uint64_t          len = pnx_buffer_len(g);
    const uint32_t    comma = len && g->ptr[8 + len - 1] != '\n';
    const uint64_t    room = g->capacity - len;
    st->charge += s.length;
    if (room < comma) {
        st->fail = PINAX_ERR_FULL;
        return PNX_R_FAILED;
    }
    if (comma)
        g->ptr[8 + len] = ',';
    const uint32_t n = pnx_csv_quote(pnx_view_bytes(st, s), s.length, g->ptr + 8 + len + comma, room - comma);
    if (n == UINT32_MAX) {
        st->fail = PINAX_ERR_FULL;
        return PNX_R_FAILED;
    }
    len += comma + n;
    __builtin_memcpy(g->ptr, &len, 8);
    return PNX_R_OK;
}
// B 0: the number R[A] as a CSV field; 1: the bool R[A]; 2: the end of the line.
PNX_OP(CPUTI) {
    const pnx_global *g = &st->globals[PNX_GX(st, x)];
    const uint64_t    len = pnx_buffer_len(g);
    uint8_t           out[32];
    uint32_t          n = 0;
    if (OP_B(w) == 2) {
        out[n++] = '\n';
    } else {
        if (len && g->ptr[8 + len - 1] != '\n')
            out[n++] = ',';
        if (OP_B(w) == 0) {
            n += pnx_format_int(R[OP_A(w)], out + n);
        } else if (OP_B(w) >= 3) { // a dec(B - 3)
            n += pnx_format_dec(R[OP_A(w)], OP_B(w) - 3, out + n);
        } else {
            __builtin_memcpy(out + n, R[OP_A(w)] ? "true" : "false", R[OP_A(w)] ? 4 : 5);
            n += R[OP_A(w)] ? 4 : 5;
        }
    }
    return pnx_buffer_append(st, g, out, n);
}

// ---- the modules hash and encode (RFC-0016 S1) ---------------------------------------------

PNX_OP(SHASH) {
    const pnx_scell s = S[OP_B(w)];
    R[OP_A(w)] = (int64_t)pnx_hash_kind(OP_C(w), pnx_view_bytes(st, s), s.length);
    st->charge += s.length;
    return PNX_R_OK;
}
// A, encoded or decoded as B says, appended to buffer x. PNX_R_FAILED: no room (full), or the
// text is not of the form (format); the buffer keeps what it held.
PNX_OP(BENC) {
    const pnx_global *g = &st->globals[PNX_GX(st, x)];
    const pnx_scell   s = S[OP_A(w)];
    uint64_t          len = pnx_buffer_len(g), n = 0;
    const int r = pnx_encode(OP_B(w), pnx_view_bytes(st, s), s.length, g->ptr + 8 + len, g->capacity - len, &n);
    st->charge += s.length + n;
    if (r) {
        st->fail = r == 1 ? PINAX_ERR_FULL : PINAX_ERR_FORMAT;
        return PNX_R_FAILED;
    }
    len += n;
    __builtin_memcpy(g->ptr, &len, 8);
    return PNX_R_OK;
}

// ---- the rest of the str module (RFC-0016 S0) ------------------------------------------------

// The length of the UTF-8 sequence that starts at bytes[0] (of n), or 0 when it is not a valid
// one: overlong forms, surrogates and values past U+10FFFF are not valid.
PNX_INLINE uint32_t pnx_utf8_len(const uint8_t *b, uint32_t n) {
    const uint8_t c = b[0];
    if (c < 0x80)
        return 1;
    const uint32_t len = c >= 0xc2 && c <= 0xdf ? 2 : c >= 0xe0 && c <= 0xef ? 3 : c >= 0xf0 && c <= 0xf4 ? 4 : 0;
    if (!len || len > n)
        return 0;
    for (uint32_t k = 1; k < len; ++k) {
        if ((b[k] & 0xc0) != 0x80)
            return 0;
    }
    if ((c == 0xe0 && b[1] < 0xa0) || (c == 0xed && b[1] > 0x9f) || (c == 0xf0 && b[1] < 0x90) || (c == 0xf4 && b[1] > 0x8f))
        return 0;
    return len;
}
// C 0: whether the str is valid UTF-8; C 1: its characters, a byte that starts no valid
// sequence counting as one
PNX_OP(SUTF) {
    const pnx_scell s = S[OP_B(w)];
    const uint8_t *b = pnx_view_bytes(st, s);
    int64_t count = 0, valid = 1;
    for (uint32_t at = 0; at < s.length; ++count) {
        const uint32_t n = pnx_utf8_len(b + at, s.length - at);
        valid &= n != 0;
        at += n ? n : 1;
    }
    st->charge += s.length;
    R[OP_A(w)] = OP_C(w) ? count : valid;
    return PNX_R_OK;
}
// The integer written in base R[C] (2 to 36; another base is a fault), with an optional '-'.
// PNX_R_FAILED: no digits, a byte that is no digit of the base, or too large.
PNX_OP(TOINTB) {
    const pnx_scell s = S[OP_B(w)];
    const int64_t base = R[OP_C(w)];
    if (base < 2 || base > 36)
        return PINAX_ERR_RANGE;
    const uint8_t *b = pnx_view_bytes(st, s);
    const bool minus = s.length && b[0] == '-';
    uint64_t value = 0;
    const uint64_t limit = minus ? (uint64_t)INT64_MAX + 1u : (uint64_t)INT64_MAX;
    st->charge += s.length;
    st->fail = PINAX_ERR_FORMAT;
    if (s.length == (uint32_t)minus)
        return PNX_R_FAILED;
    for (uint32_t k = minus; k < s.length; ++k) {
        const uint8_t c = b[k];
        const int64_t d = c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'z' ? c - 'a' + 10
                        : c >= 'A' && c <= 'Z' ? c - 'A' + 10 : 99;
        if (d >= base || value > (limit - (uint64_t)d) / (uint64_t)base)
            return PNX_R_FAILED;
        value = value * (uint64_t)base + (uint64_t)d;
    }
    R[OP_A(w)] = minus ? (int64_t)(0u - value) : (int64_t)value;
    return PNX_R_OK;
}
// Byte classes of str.span, numbered as the constants str.digit ... str.punct are.
PNX_INLINE bool pnx_in_class(uint8_t c, uint32_t class) {
    switch (class) {
    case 1: return c >= '0' && c <= '9';
    case 2: return (c | 0x20) >= 'a' && (c | 0x20) <= 'z';
    case 3: return (c >= '0' && c <= '9') || ((c | 0x20) >= 'a' && (c | 0x20) <= 'z');
    case 4: return c == ' ' || (c >= '\t' && c <= '\r');
    case 5: return c >= 'A' && c <= 'Z';
    case 6: return c >= 'a' && c <= 'z';
    case 7: return (c >= '0' && c <= '9') || ((c | 0x20) >= 'a' && (c | 0x20) <= 'f');
    case 8: return (c >= '!' && c <= '/') || (c >= ':' && c <= '@') || (c >= '[' && c <= '`') || (c >= '{' && c <= '~');
    default: return false;
    }
}
// A: the str in, its longest head of bytes of class C out; B: the rest
PNX_OP(SSPAN) {
    pnx_scell *head = &S[OP_A(w)];
    const uint8_t *b = pnx_view_bytes(st, *head);
    uint32_t n = 0;
    while (n < head->length && pnx_in_class(b[n], OP_C(w)))
        ++n;
    st->charge += n;
    S[OP_B(w)] = (pnx_scell){head->segment, head->length - n, head->offset + n};
    head->length = n;
    return PNX_R_OK;
}
// A: the rest of the str; B: its next character (a byte that starts no valid sequence is one).
// PNX_R_FAILED: none is left.
PNX_OP(SCHAR) {
    pnx_scell *rest = &S[OP_A(w)];
    if (rest->length == 0)
        return PNX_R_FAILED;
    const uint32_t valid = pnx_utf8_len(pnx_view_bytes(st, *rest), rest->length), n = valid ? valid : 1;
    S[OP_B(w)] = (pnx_scell){rest->segment, n, rest->offset};
    rest->offset += n;
    rest->length -= n;
    st->charge += n;
    return PNX_R_OK;
}
// A new str in the region: B in upper case (C 0) or lower case (C 1), ASCII letters only.
PNX_OP(RCASE) {
    const pnx_scell s = S[OP_B(w)];
    pnx_scell out = {PNX_SEG_REGION, 0, st->region_used};
    const int r = pnx_region_append(st, &out, pnx_view_bytes(st, s), s.length);
    if (r != PNX_R_OK)
        return r;
    uint8_t *b = st->region + out.offset;
    for (uint32_t k = 0; k < out.length; ++k) {
        if (OP_C(w) ? (b[k] >= 'A' && b[k] <= 'Z') : (b[k] >= 'a' && b[k] <= 'z'))
            b[k] ^= 0x20;
    }
    st->charge += s.length;
    S[OP_A(w)] = out;
    return PNX_R_OK;
}
// A new str in the region: B written R[C] times (a negative count is a fault).
PNX_OP(RREP) {
    const pnx_scell s = S[OP_B(w)];
    const int64_t times = R[OP_C(w)];
    if (times < 0)
        return PINAX_ERR_RANGE;
    pnx_scell out = {PNX_SEG_REGION, 0, st->region_used};
    for (int64_t k = 0; k < times && s.length; ++k) {
        const int r = pnx_region_append(st, &out, pnx_view_bytes(st, s), s.length);
        if (r != PNX_R_OK)
            return r;
    }
    st->charge += out.length;
    S[OP_A(w)] = out;
    return PNX_R_OK;
}

PNX_OP(RTOP) { R[OP_A(w)] = st->region_used; return PNX_R_OK; }
PNX_OP(RCUT) {
    const uint64_t top = (uint64_t)R[OP_A(w)];
    if (top > st->region_used || top < st->region_floor) // not below this run's part (a task's, a nested word's)
        return PINAX_ERR_RANGE;
    if (st->region_used > st->region_peak)
        st->region_peak = st->region_used;
    pnx_region_give_back(st, (uint32_t)top);
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
        *length = field[0] <= (x->key >> 24) ? field[0] : x->key >> 24; // N at most, as FGETS
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
    return pnx_index_get(st, &st->globals[PNX_GX(st, x)], key, 8, &R[OP_A(w)]);
}
PNX_OP(IXGETS) {
    const pnx_scell s = S[OP_B(w)];
    return pnx_index_get(st, &st->globals[PNX_GX(st, x)], pnx_view_bytes(st, s), s.length, &R[OP_A(w)]);
}
PNX_OP(IXPUT) {
    const pnx_global *ix = &st->globals[PNX_GX(st, OP_BX(w))];
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
    const pnx_global *ix = &st->globals[PNX_GX(st, OP_BX(w))];
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
