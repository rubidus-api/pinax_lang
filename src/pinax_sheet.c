// pinax_sheet.c - the standard module `sheet` (RFC-0042): a table of rows and columns kept in one
// buffer. Cells are 8 bytes; equal large numbers and equal strings are stored once in a pool
// with a use count (a flyweight). Everything is an offset inside the buffer: no pointers, so the
// buffer is copied, saved in the image and shared read-only as any buffer is.
#include "pinax_internal.h"



#define MAGIC 0x31544853u // "SHT1"
#define HDR 128u

typedef struct {
    uint32_t magic, cap, rmax, cols, rows;
    uint32_t sel_n, sel_all;                      // the selection: sel_all means every row, none stored
    uint32_t wide_slots, wide_hi, wide_free, wide_buckets;
    uint32_t text_slots, text_hi, text_free, text_buckets;
    uint32_t arena_size, arena_top, arena_live;
    uint32_t off_cells, off_sel, off_wide, off_wbk, off_text, off_tbk, off_arena;
} head;

typedef struct { uint64_t value; uint32_t count, next; } wslot;                  // 16 bytes
typedef struct { uint32_t off, len, count, next, hash; } tslot;                  // 20 bytes

// cell tags (low 4 bits)
enum { T_EMPTY, T_INT, T_BOOL, T_DEC, T_STR, T_WINT, T_WDEC };

static inline uint64_t mix(uint64_t x) {
    x ^= x >> 33; x *= 0xff51afd7ed558ccdull; x ^= x >> 33; x *= 0xc4ceb9fe1a85ec53ull; x ^= x >> 33;
    return x;
}
static uint32_t hash_bytes(const uint8_t *s, uint32_t n) {
    uint64_t h = 1469598103934665603ull;
    for (uint32_t k = 0; k < n; ++k)
        h = (h ^ s[k]) * 1099511628211ull;
    return (uint32_t)(mix(h) >> 16) | 1u;
}

static head *H(uint8_t *b) { return (head *)(void *)b; }
static uint32_t cap_of(uint8_t *b) { return H(b)->cap; }
static bool ok(const uint8_t *b, uint32_t cap) {
    head h;
    if (cap < HDR || ((uintptr_t)b & 7u))
        return false;
    __builtin_memcpy(&h, b, sizeof h);
    return h.magic == MAGIC && h.cap == cap && h.cols > 0 && h.rows <= h.rmax && h.arena_top <= h.arena_size;
}
static uint64_t *cell_at(uint8_t *b, uint32_t r, uint32_t c) {
    head *h = H(b);
    return (uint64_t *)(void *)(b + h->off_cells) + (uint64_t)r * h->cols + c;
}
static uint32_t *sel_at(uint8_t *b) { return (uint32_t *)(void *)(b + H(b)->off_sel); }
static wslot *wide_at(uint8_t *b, uint32_t s) { return (wslot *)(void *)(b + H(b)->off_wide) + s; }
static uint32_t *wbk_at(uint8_t *b, uint32_t k) { return (uint32_t *)(void *)(b + H(b)->off_wbk) + k; }
static tslot *text_at(uint8_t *b, uint32_t s) { return (tslot *)(void *)(b + H(b)->off_text) + s; }
static uint32_t *tbk_at(uint8_t *b, uint32_t k) { return (uint32_t *)(void *)(b + H(b)->off_tbk) + k; }
static uint8_t *arena(uint8_t *b) { return b + H(b)->off_arena; }

static inline int64_t cell_int(uint64_t c) { return (int64_t)c >> 4; }
static inline bool fits_int(int64_t v) { return v >= -((int64_t)1 << 59) && v < ((int64_t)1 << 59); }
static inline bool fits_dec(int64_t v) { return v >= -((int64_t)1 << 54) && v < ((int64_t)1 << 54); }

pnx_sheet_result pnx_sheet_init(uint8_t *b, uint32_t cap, uint32_t rmax, uint32_t cols) {
    if (((uintptr_t)b & 7u))
        return PNX_SH_BAD;
    if (rmax == 0 || cols == 0 || cols > 4096 || rmax > (1u << 24))
        return PNX_SH_RANGE;
    const uint64_t cells = (uint64_t)rmax * cols * 8, sel = ((uint64_t)rmax * 4 + 7) & ~7ull;
    if (HDR + cells + sel + 512 > cap)
        return PNX_SH_FULL;
    const uint64_t rest = cap - HDR - cells - sel;
    uint32_t ws = (uint32_t)(rest / 8 / 20), ts = (uint32_t)(rest / 8 / 24);
    if (ws < 1 || ts < 1)
        return PNX_SH_FULL;
    const uint64_t wbytes = (uint64_t)ws * 20, tbytes = (uint64_t)ts * 24;
    head h = {.magic = MAGIC, .cap = cap, .rmax = rmax, .cols = cols, .sel_all = 1, .wide_slots = ws, .wide_buckets = ws,
              .text_slots = ts, .text_buckets = ts};
    h.off_cells = HDR;
    h.off_sel = (uint32_t)(HDR + cells);
    h.off_wide = (uint32_t)(h.off_sel + sel);
    h.off_wbk = h.off_wide + ws * 16;
    h.off_text = (uint32_t)((h.off_wbk + (uint64_t)ws * 4 + 7) & ~7ull);
    h.off_tbk = h.off_text + ts * 20;
    h.off_arena = (uint32_t)((h.off_tbk + (uint64_t)ts * 4 + 7) & ~7ull);
    if (h.off_arena + 64 > cap || wbytes + tbytes > rest)
        return PNX_SH_FULL;
    h.arena_size = cap - h.off_arena;
    __builtin_memset(b, 0, h.off_arena);
    __builtin_memcpy(b, &h, sizeof h);
    return PNX_SH_OK;
}

uint32_t pnx_sheet_rows(uint8_t *b, uint32_t cap) { return ok(b, cap) ? H(b)->rows : 0; }
uint32_t pnx_sheet_cols(uint8_t *b, uint32_t cap) { return ok(b, cap) ? H(b)->cols : 0; }
uint32_t pnx_sheet_rows_max(uint8_t *b, uint32_t cap) { return ok(b, cap) ? H(b)->rmax : 0; }

// ---- pools ----------------------------------------------------------------------------------
// A slot's `next` chains it in its bucket while it is used, and links the free list when not.

static pnx_sheet_result wide_take(uint8_t *b, uint64_t value, uint32_t *slot) {
    head *h = H(b);
    uint32_t *bk = wbk_at(b, (uint32_t)(mix((uint64_t)value) % h->wide_buckets));
    for (uint32_t s = *bk; s; s = wide_at(b, s - 1)->next) {
        if (wide_at(b, s - 1)->value == value) {
            ++wide_at(b, s - 1)->count;
            *slot = s - 1;
            return PNX_SH_OK;
        }
    }
    uint32_t s;
    if (h->wide_free) {
        s = h->wide_free - 1;
        h->wide_free = wide_at(b, s)->next;
    } else if (h->wide_hi < h->wide_slots) {
        s = h->wide_hi++;
    } else {
        return PNX_SH_FULL;
    }
    *wide_at(b, s) = (wslot){value, 1, *bk};
    *bk = s + 1;
    *slot = s;
    return PNX_SH_OK;
}
static void wide_give(uint8_t *b, uint32_t s) {
    head  *h = H(b);
    wslot *w = wide_at(b, s);
    if (--w->count)
        return;
    uint32_t *bk = wbk_at(b, (uint32_t)(mix(w->value) % h->wide_buckets));
    while (*bk != s + 1)
        bk = &wide_at(b, *bk - 1)->next;
    *bk = w->next;
    w->next = h->wide_free;
    h->wide_free = s + 1;
}

// The arena holds blocks {slot, length, bytes}; a block is live when its slot still points at it.
// Dead blocks stay until the arena is full and a new text needs the room: then the live ones slide down.
static void arena_compact(uint8_t *b) {
    head    *h = H(b);
    uint8_t *a = arena(b);
    uint32_t from = 0, to = 0;
    while (from < h->arena_top) {
        uint32_t slot, len;
        __builtin_memcpy(&slot, a + from, 4);
        __builtin_memcpy(&len, a + from + 4, 4);
        const uint32_t block = 8 + len;
        tslot         *t = slot < h->text_hi ? text_at(b, slot) : NULL;
        if (t && t->count && t->off == from + 8) {
            if (to != from)
                __builtin_memmove(a + to, a + from, block);
            t->off = to + 8;
            to += block;
        }
        from += block;
    }
    h->arena_top = to;
}

static pnx_sheet_result text_take(uint8_t *b, const uint8_t *s, uint32_t n, uint32_t *slot) {
    head *h = H(b);
    const uint32_t hv = hash_bytes(s, n);
    uint32_t *bk = tbk_at(b, hv % h->text_buckets);
    for (uint32_t k = *bk; k; k = text_at(b, k - 1)->next) {
        tslot *t = text_at(b, k - 1);
        if (t->hash == hv && t->len == n && !__builtin_memcmp(arena(b) + t->off, s, n)) {
            ++t->count;
            *slot = k - 1;
            return PNX_SH_OK;
        }
    }
    uint32_t id;
    if (h->text_free)
        id = h->text_free - 1;
    else if (h->text_hi < h->text_slots)
        id = h->text_hi;
    else
        return PNX_SH_FULL;
    const uint32_t block = 8 + n;
    if (h->arena_size - h->arena_top < block) {
        if (h->arena_size - h->arena_live < block)
            return PNX_SH_FULL;
        uint8_t keep[256]; // the text may lie in the arena, which is about to move
        if (s >= b && s < b + cap_of(b)) {
            if (n > sizeof keep)
                return PNX_SH_FULL;
            __builtin_memcpy(keep, s, n);
            s = keep;
        }
        arena_compact(b);
    }
    if (h->text_free)
        h->text_free = text_at(b, id)->next;
    else
        ++h->text_hi;
    uint8_t *at = arena(b) + h->arena_top;
    __builtin_memcpy(at, &id, 4);
    __builtin_memcpy(at + 4, &n, 4);
    __builtin_memcpy(at + 8, s, n);
    *text_at(b, id) = (tslot){h->arena_top + 8, n, 1, *bk, hv};
    h->arena_top += block;
    h->arena_live += block;
    *bk = id + 1;
    *slot = id;
    return PNX_SH_OK;
}
static void text_give(uint8_t *b, uint32_t s) {
    head  *h = H(b);
    tslot *t = text_at(b, s);
    if (--t->count)
        return;
    uint32_t *bk = tbk_at(b, t->hash % h->text_buckets);
    while (*bk != s + 1)
        bk = &text_at(b, *bk - 1)->next;
    *bk = t->next;
    h->arena_live -= 8 + t->len;
    t->next = h->text_free;
    h->text_free = s + 1;
}

// A cell going away gives back what it holds.
static void release(uint8_t *b, uint64_t c) {
    switch (c & 15u) {
    case T_STR: text_give(b, (uint32_t)(c >> 4)); break;
    case T_WINT: wide_give(b, (uint32_t)(c >> 4)); break;
    case T_WDEC: wide_give(b, (uint32_t)(c >> 9)); break;
    default: break;
    }
}
// A cell copied takes another use.
static void retain(uint8_t *b, uint64_t c) {
    switch (c & 15u) {
    case T_STR: ++text_at(b, (uint32_t)(c >> 4))->count; break;
    case T_WINT: ++wide_at(b, (uint32_t)(c >> 4))->count; break;
    case T_WDEC: ++wide_at(b, (uint32_t)(c >> 9))->count; break;
    default: break;
    }
}

// ---- rows -----------------------------------------------------------------------------------

pnx_sheet_result pnx_sheet_add_row(uint8_t *b, uint32_t cap, uint32_t *row) {
    if (!ok(b, cap))
        return PNX_SH_BAD;
    head *h = H(b);
    if (h->rows == h->rmax)
        return PNX_SH_FULL;
    __builtin_memset(cell_at(b, h->rows, 0), 0, (size_t)h->cols * 8);
    *row = h->rows++;
    return PNX_SH_OK;
}
pnx_sheet_result pnx_sheet_truncate_rows(uint8_t *b, uint32_t cap, uint32_t n) {
    if (!ok(b, cap))
        return PNX_SH_BAD;
    head *h = H(b);
    if (n > h->rows)
        return PNX_SH_RANGE;
    for (uint32_t r = n; r < h->rows; ++r) {
        for (uint32_t c = 0; c < h->cols; ++c) {
            release(b, *cell_at(b, r, c));
            *cell_at(b, r, c) = 0;
        }
    }
    h->rows = n;
    h->sel_all = 1;
    h->sel_n = 0;
    return PNX_SH_OK;
}
pnx_sheet_result pnx_sheet_clear(uint8_t *b, uint32_t cap) {
    return pnx_sheet_truncate_rows(b, cap, 0);
}

// The columns made C2 (more, or fewer when the cut columns are empty), rows kept, in the same buffer:
// the pools move up or down with the cells.
pnx_sheet_result pnx_sheet_widen(uint8_t *b, uint32_t cap, uint32_t c2) {
    if (!ok(b, cap))
        return PNX_SH_BAD;
    head h = *H(b);
    if (c2 == 0 || c2 > 4096)
        return PNX_SH_RANGE;
    if (c2 == h.cols)
        return PNX_SH_OK;
    if (c2 < h.cols) {
        for (uint32_t r = 0; r < h.rows; ++r)
            for (uint32_t c = c2; c < h.cols; ++c)
                if (*cell_at(b, r, c))
                    return PNX_SH_KIND;
        // pack the rows toward the front
        uint64_t *base = (uint64_t *)(void *)(b + h.off_cells);
        for (uint32_t r = 0; r < h.rows; ++r)
            __builtin_memmove(base + (uint64_t)r * c2, base + (uint64_t)r * h.cols, (size_t)c2 * 8);
        const uint64_t cells2 = (uint64_t)h.rmax * c2 * 8, old = (uint64_t)h.rmax * h.cols * 8;
        const uint32_t shift = (uint32_t)(old - cells2);
        __builtin_memmove(b + h.off_sel - shift, b + h.off_sel, h.arena_top + h.off_arena - h.off_sel);
        head *p = H(b);
        p->cols = c2;
        p->off_sel -= shift; p->off_wide -= shift; p->off_wbk -= shift; p->off_text -= shift; p->off_tbk -= shift;
        p->off_arena -= shift;
        p->arena_size += shift;
        return PNX_SH_OK;
    }
    const uint64_t cells2 = (uint64_t)h.rmax * c2 * 8, old = (uint64_t)h.rmax * h.cols * 8;
    const uint64_t shift = cells2 - old;
    const uint64_t used = (uint64_t)h.off_arena + h.arena_top; // bytes in use
    if (shift > h.arena_size - h.arena_top || used + shift > cap)
        return PNX_SH_FULL;
    __builtin_memmove(b + h.off_sel + shift, b + h.off_sel, used - h.off_sel);
    head *p = H(b);
    p->off_sel += (uint32_t)shift; p->off_wide += (uint32_t)shift; p->off_wbk += (uint32_t)shift;
    p->off_text += (uint32_t)shift; p->off_tbk += (uint32_t)shift; p->off_arena += (uint32_t)shift;
    p->arena_size -= (uint32_t)shift;
    p->cols = c2;
    uint64_t *base = (uint64_t *)(void *)(b + p->off_cells);
    for (uint32_t r = h.rows; r-- > 0;) { // from the last row, so a row is not written over before it is moved
        __builtin_memmove(base + (uint64_t)r * c2, base + (uint64_t)r * h.cols, (size_t)h.cols * 8);
        __builtin_memset(base + (uint64_t)r * c2 + h.cols, 0, (size_t)(c2 - h.cols) * 8);
    }
    return PNX_SH_OK;
}

// ---- cells ----------------------------------------------------------------------------------

static pnx_sheet_result at(uint8_t *b, uint32_t cap, int64_t r, int64_t c, uint64_t **cell) {
    if (!ok(b, cap))
        return PNX_SH_BAD;
    if (r < 0 || c < 0 || (uint64_t)r >= H(b)->rows || (uint64_t)c >= H(b)->cols)
        return PNX_SH_RANGE;
    *cell = cell_at(b, (uint32_t)r, (uint32_t)c);
    return PNX_SH_OK;
}

static pnx_sheet_result put_raw(uint8_t *b, uint32_t cap, int64_t r, int64_t c, uint64_t nc) {
    uint64_t *cell;
    pnx_sheet_result e = at(b, cap, r, c, &cell);
    if (e)
        return e;
    release(b, *cell); // after the new value was taken: a cell set to what it held keeps its slot
    *cell = nc;
    return PNX_SH_OK;
}

pnx_sheet_result pnx_sheet_put_int(uint8_t *b, uint32_t cap, int64_t v, int64_t r, int64_t c) {
    uint64_t *cell;
    pnx_sheet_result e = at(b, cap, r, c, &cell);
    if (e)
        return e;
    if (fits_int(v))
        return put_raw(b, cap, r, c, (uint64_t)v << 4 | T_INT);
    uint32_t s;
    if ((e = wide_take(b, (uint64_t)v, &s)))
        return e;
    return put_raw(b, cap, r, c, (uint64_t)s << 4 | T_WINT);
}
pnx_sheet_result pnx_sheet_put_bool(uint8_t *b, uint32_t cap, int64_t v, int64_t r, int64_t c) {
    return put_raw(b, cap, r, c, (v ? 16u : 0u) | T_BOOL);
}
pnx_sheet_result pnx_sheet_put_dec(uint8_t *b, uint32_t cap, int64_t v, uint32_t scale, int64_t r, int64_t c) {
    uint64_t *cell;
    pnx_sheet_result e = at(b, cap, r, c, &cell);
    if (e)
        return e;
    if (fits_dec(v))
        return put_raw(b, cap, r, c, (uint64_t)v << 9 | (uint64_t)scale << 4 | T_DEC);
    uint32_t s;
    if ((e = wide_take(b, (uint64_t)v, &s)))
        return e;
    return put_raw(b, cap, r, c, (uint64_t)s << 9 | (uint64_t)scale << 4 | T_WDEC);
}
pnx_sheet_result pnx_sheet_put_str(uint8_t *b, uint32_t cap, const uint8_t *s, uint32_t n, int64_t r, int64_t c) {
    uint64_t *cell;
    pnx_sheet_result e = at(b, cap, r, c, &cell);
    if (e)
        return e;
    uint32_t slot;
    if ((e = text_take(b, s, n, &slot)))
        return e;
    return put_raw(b, cap, r, c, (uint64_t)slot << 4 | T_STR);
}
pnx_sheet_result pnx_sheet_clear_cell(uint8_t *b, uint32_t cap, int64_t r, int64_t c) {
    uint64_t *cell;
    pnx_sheet_result e = at(b, cap, r, c, &cell);
    if (e)
        return e;
    release(b, *cell);
    *cell = 0;
    return PNX_SH_OK;
}
pnx_sheet_result pnx_sheet_copy_cell(uint8_t *b, uint32_t cap, int64_t r1, int64_t c1, int64_t r2, int64_t c2) {
    uint64_t *from, *to;
    pnx_sheet_result e = at(b, cap, r1, c1, &from);
    if (!e)
        e = at(b, cap, r2, c2, &to);
    if (e)
        return e;
    const uint64_t v = *from;
    retain(b, v);
    release(b, *to);
    *to = v;
    return PNX_SH_OK;
}

static uint32_t kind_of(uint64_t c) {
    switch (c & 15u) {
    case T_EMPTY: return 0;
    case T_INT: case T_WINT: return 1;
    case T_BOOL: return 2;
    case T_DEC: case T_WDEC: return 3;
    default: return 4;
    }
}
pnx_sheet_result pnx_sheet_kind(uint8_t *b, uint32_t cap, int64_t r, int64_t c, int64_t *kind) {
    uint64_t *cell;
    pnx_sheet_result e = at(b, cap, r, c, &cell);
    if (!e)
        *kind = kind_of(*cell);
    return e;
}
static int64_t wide_value(uint8_t *b, uint32_t s) { return (int64_t)wide_at(b, s)->value; }
pnx_sheet_result pnx_sheet_get_int(uint8_t *b, uint32_t cap, int64_t r, int64_t c, int64_t *v) {
    uint64_t *cell;
    pnx_sheet_result e = at(b, cap, r, c, &cell);
    if (e)
        return e;
    if ((*cell & 15u) == T_INT)
        *v = cell_int(*cell);
    else if ((*cell & 15u) == T_WINT)
        *v = wide_value(b, (uint32_t)(*cell >> 4));
    else
        return PNX_SH_KIND;
    return PNX_SH_OK;
}
pnx_sheet_result pnx_sheet_get_bool(uint8_t *b, uint32_t cap, int64_t r, int64_t c, int64_t *v) {
    uint64_t *cell;
    pnx_sheet_result e = at(b, cap, r, c, &cell);
    if (e)
        return e;
    if ((*cell & 15u) != T_BOOL)
        return PNX_SH_KIND;
    *v = (*cell >> 4) & 1u;
    return PNX_SH_OK;
}
pnx_sheet_result pnx_sheet_get_dec(uint8_t *b, uint32_t cap, int64_t r, int64_t c, uint32_t scale, int64_t *v) {
    uint64_t *cell;
    pnx_sheet_result e = at(b, cap, r, c, &cell);
    if (e)
        return e;
    if (((*cell & 15u) != T_DEC && (*cell & 15u) != T_WDEC) || ((*cell >> 4) & 31u) != scale)
        return PNX_SH_KIND;
    *v = (*cell & 15u) == T_DEC ? (int64_t)*cell >> 9 : wide_value(b, (uint32_t)(*cell >> 9));
    return PNX_SH_OK;
}
// The text of a cell, as an offset and length inside the buffer's data.
pnx_sheet_result pnx_sheet_get_str(uint8_t *b, uint32_t cap, int64_t r, int64_t c, uint32_t *off, uint32_t *len) {
    uint64_t *cell;
    pnx_sheet_result e = at(b, cap, r, c, &cell);
    if (e)
        return e;
    if ((*cell & 15u) != T_STR)
        return PNX_SH_KIND;
    tslot *t = text_at(b, (uint32_t)(*cell >> 4));
    *off = H(b)->off_arena + t->off;
    *len = t->len;
    return PNX_SH_OK;
}

// ---- searching ------------------------------------------------------------------------------

// Whether a cell equals an int key / a text slot.
static bool eq_int(uint8_t *b, uint64_t c, int64_t key) {
    if ((c & 15u) == T_INT)
        return cell_int(c) == key;
    return (c & 15u) == T_WINT && wide_value(b, (uint32_t)(c >> 4)) == key;
}
// The slot of a text in the pool, or UINT32_MAX when it is not there (then it is in no cell).
static uint32_t text_find(uint8_t *b, const uint8_t *s, uint32_t n) {
    head *h = H(b);
    const uint32_t hv = hash_bytes(s, n);
    for (uint32_t k = *tbk_at(b, hv % h->text_buckets); k; k = text_at(b, k - 1)->next) {
        tslot *t = text_at(b, k - 1);
        if (t->hash == hv && t->len == n && !__builtin_memcmp(arena(b) + t->off, s, n))
            return k - 1;
    }
    return UINT32_MAX;
}

// along = 0: down column `line` from row `from`; 1: across row `line` from column `from`.
static pnx_sheet_result find(uint8_t *b, uint32_t cap, bool across, bool text, int64_t ikey, const uint8_t *s, uint32_t n,
                             int64_t line, int64_t from, int64_t *found) {
    if (!ok(b, cap))
        return PNX_SH_BAD;
    head *h = H(b);
    const uint32_t lines = across ? h->rows : h->cols, len = across ? h->cols : h->rows;
    if (line < 0 || (uint64_t)line >= lines || from < 0)
        return PNX_SH_RANGE;
    uint32_t slot = 0;
    if (text) {
        slot = text_find(b, s, n);
        if (slot == UINT32_MAX)
            return PNX_SH_NONE;
    }
    for (uint64_t k = (uint64_t)from; k < len; ++k) {
        const uint64_t c = across ? *cell_at(b, (uint32_t)line, (uint32_t)k) : *cell_at(b, (uint32_t)k, (uint32_t)line);
        if (text ? ((c & 15u) == T_STR && (uint32_t)(c >> 4) == slot) : eq_int(b, c, ikey)) {
            *found = (int64_t)k;
            return PNX_SH_OK;
        }
    }
    return PNX_SH_NONE;
}
pnx_sheet_result pnx_sheet_find_int(uint8_t *b, uint32_t cap, bool across, int64_t key, int64_t line, int64_t from, int64_t *f) {
    return find(b, cap, across, false, key, NULL, 0, line, from, f);
}
pnx_sheet_result pnx_sheet_find_str(uint8_t *b, uint32_t cap, bool across, const uint8_t *s, uint32_t n, int64_t line,
                                    int64_t from, int64_t *f) {
    return find(b, cap, across, true, 0, s, n, line, from, f);
}

// ---- the selection (rows chosen by where and narrow) -----------------------------------------

uint32_t pnx_sheet_sel_len(uint8_t *b, uint32_t cap) {
    if (!ok(b, cap))
        return 0;
    return H(b)->sel_all ? H(b)->rows : H(b)->sel_n;
}
pnx_sheet_result pnx_sheet_sel_row(uint8_t *b, uint32_t cap, int64_t k, int64_t *row) {
    if (!ok(b, cap))
        return PNX_SH_BAD;
    if (k < 0 || (uint64_t)k >= pnx_sheet_sel_len(b, cap))
        return PNX_SH_RANGE;
    *row = H(b)->sel_all ? k : sel_at(b)[k];
    return PNX_SH_OK;
}
pnx_sheet_result pnx_sheet_select_all(uint8_t *b, uint32_t cap) {
    if (!ok(b, cap))
        return PNX_SH_BAD;
    H(b)->sel_all = 1;
    H(b)->sel_n = 0;
    return PNX_SH_OK;
}

// op: 0 eq, 1 ne, 2 lt, 3 le, 4 gt, 5 ge. Ints compare as numbers; texts as bytes; a cell of
// another kind, or empty, does not match.
static bool test_int(int op, int64_t a, int64_t k) {
    switch (op) {
    case 0: return a == k;
    case 1: return a != k;
    case 2: return a < k;
    case 3: return a <= k;
    case 4: return a > k;
    default: return a >= k;
    }
}
static bool test_cmp(int op, int d) { return test_int(op, d, 0); }

pnx_sheet_result pnx_sheet_narrow(uint8_t *b, uint32_t cap, bool text, int64_t col, int64_t op, int64_t ikey, const uint8_t *s,
                                  uint32_t n) {
    if (!ok(b, cap))
        return PNX_SH_BAD;
    head *h = H(b);
    if (col < 0 || (uint64_t)col >= h->cols || op < 0 || op > 5)
        return PNX_SH_RANGE;
    const uint32_t count = pnx_sheet_sel_len(b, cap);
    uint32_t keep = 0;
    uint32_t *sel = sel_at(b);
    for (uint32_t k = 0; k < count; ++k) {
        const uint32_t r = h->sel_all ? k : sel[k];
        const uint64_t c = *cell_at(b, r, (uint32_t)col);
        bool hit = false;
        if (text) {
            if ((c & 15u) == T_STR) {
                tslot *t = text_at(b, (uint32_t)(c >> 4));
                const uint32_t m = t->len < n ? t->len : n;
                int d = __builtin_memcmp(arena(b) + t->off, s, m);
                if (!d)
                    d = t->len < n ? -1 : t->len > n ? 1 : 0;
                hit = test_cmp((int)op, d);
            }
        } else if ((c & 15u) == T_INT || (c & 15u) == T_WINT) {
            const int64_t v = (c & 15u) == T_INT ? cell_int(c) : wide_value(b, (uint32_t)(c >> 4));
            hit = test_int((int)op, v, ikey);
        }
        if (hit)
            sel[keep++] = r;
    }
    h->sel_all = 0;
    h->sel_n = keep;
    return PNX_SH_OK;
}
pnx_sheet_result pnx_sheet_where(uint8_t *b, uint32_t cap, bool text, int64_t col, int64_t op, int64_t ikey, const uint8_t *s,
                                 uint32_t n) {
    pnx_sheet_result e = pnx_sheet_select_all(b, cap);
    return e ? e : pnx_sheet_narrow(b, cap, text, col, op, ikey, s, n);
}

// ---- totals ---------------------------------------------------------------------------------
// what: 0 sum, 1 min, 2 max, 3 count. over: 0 a column (all rows), 1 a row (all columns), 2 a column of the selection.
pnx_sheet_result pnx_sheet_total(uint8_t *b, uint32_t cap, int what, int over, int64_t line, int64_t *out, uint64_t *scanned) {
    if (!ok(b, cap))
        return PNX_SH_BAD;
    head *h = H(b);
    const uint32_t lines = over == 1 ? h->rows : h->cols;
    if (line < 0 || (uint64_t)line >= lines)
        return PNX_SH_RANGE;
    const uint32_t len = over == 0 ? h->rows : over == 1 ? h->cols : pnx_sheet_sel_len(b, cap);
    int64_t acc = 0, seen = 0;
    for (uint32_t k = 0; k < len; ++k) {
        uint32_t r = over == 1 ? (uint32_t)line : over == 0 ? k : (h->sel_all ? k : sel_at(b)[k]);
        uint32_t c = over == 1 ? k : (uint32_t)line;
        const uint64_t cell = *cell_at(b, r, c);
        if ((cell & 15u) != T_INT && (cell & 15u) != T_WINT)
            continue;
        const int64_t v = (cell & 15u) == T_INT ? cell_int(cell) : wide_value(b, (uint32_t)(cell >> 4));
        if (what == 0) {
            if (__builtin_add_overflow(acc, v, &acc))
                return PNX_SH_OVERFLOW;
        } else if (what == 1) {
            acc = seen ? (v < acc ? v : acc) : v;
        } else if (what == 2) {
            acc = seen ? (v > acc ? v : acc) : v;
        }
        ++seen;
    }
    *scanned = len;
    if (what == 3)
        acc = seen;
    else if (what != 0 && !seen)
        return PNX_SH_NONE; // min and max of nothing
    *out = acc;
    return PNX_SH_OK;
}

// ---- ordering the selection -----------------------------------------------------------------

// Rank of a cell for ordering: numbers, then texts, then the rest (empty, bool, dec) last in either direction.
static int rank_of(uint64_t c) {
    return ((c & 15u) == T_INT || (c & 15u) == T_WINT) ? 0 : (c & 15u) == T_STR ? 1 : 2;
}
// -1, 0, 1 for two cells of the same rank; `desc` turns the order of numbers and texts around, never the place of the rest.
static int cmp_cells(uint8_t *b, uint64_t x, uint64_t y, bool desc) {
    const int rx = rank_of(x), ry = rank_of(y);
    if (rx != ry)
        return rx < ry ? -1 : 1;
    int d = 0;
    if (rx == 0) {
        const int64_t a = (x & 15u) == T_INT ? cell_int(x) : wide_value(b, (uint32_t)(x >> 4));
        const int64_t c = (y & 15u) == T_INT ? cell_int(y) : wide_value(b, (uint32_t)(y >> 4));
        d = a < c ? -1 : a > c;
    } else if (rx == 1) {
        tslot *s = text_at(b, (uint32_t)(x >> 4)), *t = text_at(b, (uint32_t)(y >> 4));
        const uint32_t m = s->len < t->len ? s->len : t->len;
        d = __builtin_memcmp(arena(b) + s->off, arena(b) + t->off, m);
        d = d ? (d < 0 ? -1 : 1) : (s->len < t->len ? -1 : s->len > t->len);
    }
    return desc ? -d : d;
}

// A stable merge sort of the selected rows by column `col` (the work area is the free end of the
// buffer: the rows go to the selection area of the same size at the end of the arena... here a
// plain insertion into a second pass over `sel`, O(n log n) by merging through the arena's free tail).
pnx_sheet_result pnx_sheet_order_by(uint8_t *b, uint32_t cap, int64_t col, bool desc, uint64_t *scanned) {
    if (!ok(b, cap))
        return PNX_SH_BAD;
    head *h = H(b);
    if (col < 0 || (uint64_t)col >= h->cols)
        return PNX_SH_RANGE;
    if (h->sel_all) { // the rows become an explicit selection first
        uint32_t *sel = sel_at(b);
        for (uint32_t k = 0; k < h->rows; ++k)
            sel[k] = k;
        h->sel_n = h->rows;
        h->sel_all = 0;
    }
    const uint32_t n = h->sel_n;
    uint32_t *sel = sel_at(b);
    // the work area: n 32-bit numbers at the end of the arena (free space), or the sheet is too full
    if ((uint64_t)n * 4 > h->arena_size - h->arena_top)
        return PNX_SH_FULL;
    uint32_t *tmp = (uint32_t *)(void *)(b + (cap - n * 4u) / 4 * 4);
    if ((uint8_t *)tmp < arena(b) + h->arena_top)
        return PNX_SH_FULL;
    for (uint32_t width = 1; width < n; width *= 2) {
        for (uint32_t lo = 0; lo < n; lo += 2 * width) {
            const uint32_t mid = lo + width < n ? lo + width : n, hi = lo + 2 * width < n ? lo + 2 * width : n;
            uint32_t i = lo, j = mid, o = lo;
            while (i < mid && j < hi) {
                const int d = cmp_cells(b, *cell_at(b, sel[j], (uint32_t)col), *cell_at(b, sel[i], (uint32_t)col), desc);
                tmp[o++] = d < 0 ? sel[j++] : sel[i++]; // equal keys keep their order: take the left one
            }
            while (i < mid) tmp[o++] = sel[i++];
            while (j < hi) tmp[o++] = sel[j++];
        }
        __builtin_memcpy(sel, tmp, (size_t)n * 4);
    }
    *scanned = (uint64_t)n * (n > 1 ? 32 - (uint32_t)__builtin_clz(n - 1) : 1);
    return PNX_SH_OK;
}

pnx_sheet_result pnx_sheet_compact(uint8_t *b, uint32_t cap) {
    if (!ok(b, cap))
        return PNX_SH_BAD;
    arena_compact(b);
    return PNX_SH_OK;
}
