// pinax_sheet.c - the standard module `sheet` (RFC-0042, RFC-0044): a book of sheets (tables of rows
// and columns) kept in one buffer. Cells are 8 bytes; equal large numbers and equal strings are stored
// once in a pool shared by every sheet of the book, with a use count (a flyweight). Everything is an
// offset inside the buffer: no pointers, so the buffer is copied, saved in the image and shared
// read-only as any buffer is.
//
// Layout of a book, low to high:  head | sheet table | pools (wide, text, arena) | sheet regions ->
//                                  ... free ... <- index pages (B+tree, 512 bytes each, from the top)
// A sheet region is its cells, its selection and its column table; regions are bump-allocated, a
// dropped or widened one leaves a hole that `compact` closes. Index pages are taken from the top end
// downward, so sheet data and indexes share the free middle and nothing is reserved in advance.
#include "pinax_internal.h"

#define MAGIC 0x32544853u // "SHT2"
#define HDR 256u
#define PAGE 512u
#define LEAF_MAX 41u  // entries of a leaf: 16 byte head, 41 cells of 8, 41 rows of 4
#define NODE_KEYS 30u // keys of an interior page: 16 byte head, 30 cells, 30 rows, 31 children
#define NO_SHEET UINT32_MAX
#define NAME_MAX_LEN 255u

typedef struct {
    uint32_t magic, cap, nmax, nused, cur;
    uint32_t wide_slots, wide_hi, wide_free, wide_buckets;
    uint32_t text_slots, text_hi, text_free, text_buckets;
    uint32_t arena_size, arena_top, arena_live;
    uint32_t off_sheets, off_wide, off_wbk, off_text, off_tbk, off_arena;
    uint32_t data_lo, data_top, ptop, pg_hi, pg_free, pg_free_n;
} head;

typedef struct {
    uint32_t used, name, rmax, cols, rows, sel_n, sel_all, colmajor;
    uint32_t off_cells, off_sel, off_col, size;
    uint32_t mark, pad[3];
} sht; // 64 bytes
typedef struct { uint32_t root, height, pad[2]; } colinfo; // 16 bytes; root 0 = not indexed

typedef struct { uint64_t value; uint32_t count, next; } wslot;                  // 16 bytes
typedef struct { uint32_t off, len, count, next, hash; } tslot;                  // 20 bytes

// cell tags (low 4 bits)
enum { T_EMPTY, T_INT, T_BOOL, T_DEC, T_STR, T_WINT, T_WDEC, T_DATE }; // T_DATE: Unix seconds, kept in the cell like T_INT

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
static inline uint64_t round8(uint64_t v) { return (v + 7u) & ~7ull; }

static head *H(uint8_t *b) { return (head *)(void *)b; }
static uint32_t cap_of(uint8_t *b) { return H(b)->cap; }
static bool ok(const uint8_t *b, uint32_t cap) {
    head h;
    if (cap < HDR || ((uintptr_t)b & 7u))
        return false;
    __builtin_memcpy(&h, b, sizeof h);
    return h.magic == MAGIC && h.cap == cap && h.nmax > 0 && h.nmax <= 256 && h.nused <= h.nmax && h.arena_top <= h.arena_size &&
           (h.cur == NO_SHEET || h.cur < h.nused) && h.data_lo <= h.data_top && h.data_top <= h.ptop && h.ptop <= cap;
}
static sht *SHT(uint8_t *b, uint32_t k) { return (sht *)(void *)(b + H(b)->off_sheets) + k; }
// The current sheet, or the reason there is none.
static pnx_sheet_result cur_sheet(uint8_t *b, uint32_t cap, sht **s) {
    if (!ok(b, cap))
        return PNX_SH_BAD;
    const head *h = H(b);
    if (h->cur == NO_SHEET || !SHT(b, h->cur)->used)
        return PNX_SH_STATE;
    *s = SHT(b, h->cur);
    return PNX_SH_OK;
}
static uint64_t *cell_at(uint8_t *b, const sht *s, uint32_t r, uint32_t c) {
    uint64_t *base = (uint64_t *)(void *)(b + s->off_cells);
    return s->colmajor ? base + (uint64_t)c * s->rmax + r : base + (uint64_t)r * s->cols + c;
}
static uint32_t *sel_at(uint8_t *b, const sht *s) { return (uint32_t *)(void *)(b + s->off_sel); }
static colinfo *col_at(uint8_t *b, const sht *s, uint32_t c) { return (colinfo *)(void *)(b + s->off_col) + c; }
static wslot *wide_at(uint8_t *b, uint32_t s) { return (wslot *)(void *)(b + H(b)->off_wide) + s; }
static uint32_t *wbk_at(uint8_t *b, uint32_t k) { return (uint32_t *)(void *)(b + H(b)->off_wbk) + k; }
static tslot *text_at(uint8_t *b, uint32_t s) { return (tslot *)(void *)(b + H(b)->off_text) + s; }
static uint32_t *tbk_at(uint8_t *b, uint32_t k) { return (uint32_t *)(void *)(b + H(b)->off_tbk) + k; }
static uint8_t *arena(uint8_t *b) { return b + H(b)->off_arena; }

static inline int64_t cell_int(uint64_t c) { return (int64_t)c >> 4; }
static inline bool fits_int(int64_t v) { return v >= -((int64_t)1 << 59) && v < ((int64_t)1 << 59); }
static inline bool fits_dec(int64_t v) { return v >= -((int64_t)1 << 54) && v < ((int64_t)1 << 54); }

// ---- the book -------------------------------------------------------------------------------

pnx_sheet_result pnx_sheet_open(uint8_t *b, uint32_t cap, uint32_t nmax) {
    if (((uintptr_t)b & 7u))
        return PNX_SH_BAD;
    if (nmax == 0 || nmax > 256)
        return PNX_SH_RANGE;
    const uint64_t base = HDR + (uint64_t)nmax * sizeof(sht);
    if (base + 2048 > cap)
        return PNX_SH_FULL;
    const uint64_t pools = (cap - base) * 2 / 5; // the pools take two fifths, sheets and indexes share the rest
    const uint32_t ws = (uint32_t)(pools / 160), ts = (uint32_t)(pools / 192);
    if (ws < 1 || ts < 1)
        return PNX_SH_FULL;
    head h = {.magic = MAGIC, .cap = cap, .nmax = nmax, .cur = NO_SHEET, .wide_slots = ws, .wide_buckets = ws, .text_slots = ts,
              .text_buckets = ts};
    h.off_sheets = HDR;
    h.off_wide = (uint32_t)base;
    h.off_wbk = h.off_wide + ws * 16;
    h.off_text = (uint32_t)round8((uint64_t)h.off_wbk + (uint64_t)ws * 4);
    h.off_tbk = h.off_text + ts * 20;
    h.off_arena = (uint32_t)round8((uint64_t)h.off_tbk + (uint64_t)ts * 4);
    const uint64_t pend = (base + pools) & ~7ull;
    if (h.off_arena + 64 > pend)
        return PNX_SH_FULL;
    h.arena_size = (uint32_t)(pend - h.off_arena);
    h.data_lo = h.data_top = (uint32_t)pend;
    h.ptop = cap & ~7u;
    __builtin_memset(b, 0, h.data_lo);
    __builtin_memcpy(b, &h, sizeof h);
    return PNX_SH_OK;
}

uint32_t pnx_sheet_rows(uint8_t *b, uint32_t cap) {
    sht *s;
    return cur_sheet(b, cap, &s) ? 0 : s->rows;
}
uint32_t pnx_sheet_cols(uint8_t *b, uint32_t cap) {
    sht *s;
    return cur_sheet(b, cap, &s) ? 0 : s->cols;
}
uint32_t pnx_sheet_rows_max(uint8_t *b, uint32_t cap) {
    sht *s;
    return cur_sheet(b, cap, &s) ? 0 : s->rmax;
}
// which: 0 rows, 1 columns, 2 rows_max of the current sheet.
pnx_sheet_result pnx_sheet_dim(uint8_t *b, uint32_t cap, int which, int64_t *out) {
    sht *s;
    pnx_sheet_result e = cur_sheet(b, cap, &s);
    if (!e)
        *out = which == 0 ? s->rows : which == 1 ? s->cols : s->rmax;
    return e;
}

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
static int64_t wide_value(uint8_t *b, uint32_t s) { return (int64_t)wide_at(b, s)->value; }

// Rank of a cell for ordering: numbers, then texts, then the rest (empty, bool, dec) last in either direction.
// A date counts as its seconds, an int, for everything that orders, finds or selects (RFC-0048).
static int rank_of(uint64_t c) {
    return ((c & 15u) == T_INT || (c & 15u) == T_WINT || (c & 15u) == T_DATE) ? 0 : (c & 15u) == T_STR ? 1 : 2;
}
static int64_t int_of(uint8_t *b, uint64_t c) {
    return (c & 15u) == T_INT || (c & 15u) == T_DATE ? cell_int(c) : wide_value(b, (uint32_t)(c >> 4));
}
// -1, 0, 1 for two cells of the same rank; `desc` turns the order of numbers and texts around, never the place of the rest.
static int cmp_cells(uint8_t *b, uint64_t x, uint64_t y, bool desc) {
    const int rx = rank_of(x), ry = rank_of(y);
    if (rx != ry)
        return rx < ry ? -1 : 1;
    int d = 0;
    if (rx == 0) {
        const int64_t a = int_of(b, x), c = int_of(b, y);
        d = a < c ? -1 : a > c;
    } else if (rx == 1) {
        tslot *s = text_at(b, (uint32_t)(x >> 4)), *t = text_at(b, (uint32_t)(y >> 4));
        const uint32_t m = s->len < t->len ? s->len : t->len;
        d = __builtin_memcmp(arena(b) + s->off, arena(b) + t->off, m);
        d = d ? (d < 0 ? -1 : 1) : (s->len < t->len ? -1 : s->len > t->len);
    }
    return desc ? -d : d;
}

// ---- the ordered index: a B+tree of 512 byte pages (RFC-0044 section 4) ---------------------------
// An index holds an entry (cell, row) for every cell of a column that is a number or a text, ordered by
// the value (numbers, then texts by bytes) and then by row. Leaves are linked both ways; an interior
// key is a bound: child i holds the entries below key i and from key i-1 up. Deleting merges a leaf
// that falls below half with a neighbour when both fit in one, so a sheet that is written and
// emptied again does not eat the pages. Pages are named by 1-based numbers; page k lies at ptop - k * PAGE.

typedef struct { uint8_t kind, pad; uint16_t n; uint32_t next, prev, aux; } phdr; // kind 1 leaf, 2 interior

static uint8_t *pgp(uint8_t *b, uint32_t id) { return b + H(b)->ptop - (uint64_t)id * PAGE; }
static phdr    *ph(uint8_t *p) { return (phdr *)(void *)p; }
static uint64_t *lcell(uint8_t *p) { return (uint64_t *)(void *)(p + 16); }
static uint32_t *lrow(uint8_t *p) { return (uint32_t *)(void *)(p + 16 + LEAF_MAX * 8); }
static uint64_t *icell(uint8_t *p) { return (uint64_t *)(void *)(p + 16); }
static uint32_t *irow(uint8_t *p) { return (uint32_t *)(void *)(p + 16 + NODE_KEYS * 8); }
static uint32_t *ich(uint8_t *p) { return (uint32_t *)(void *)(p + 16 + NODE_KEYS * 12); }

static uint64_t pg_lo(const head *h) { return (uint64_t)h->ptop - (uint64_t)h->pg_hi * PAGE; }
static uint64_t pg_avail(const head *h) {
    const uint64_t lo = pg_lo(h);
    return (lo > h->data_top ? (lo - h->data_top) / PAGE : 0) + h->pg_free_n;
}
static uint32_t pg_alloc(uint8_t *b) {
    head    *h = H(b);
    uint32_t id;
    if (h->pg_free) {
        id = h->pg_free;
        h->pg_free = ph(pgp(b, id))->next;
        --h->pg_free_n;
    } else {
        if (pg_lo(h) < (uint64_t)h->data_top + PAGE)
            return 0;
        id = ++h->pg_hi;
    }
    __builtin_memset(pgp(b, id), 0, PAGE);
    return id;
}
static void pg_release(uint8_t *b, uint32_t id) {
    head *h = H(b);
    uint8_t *p = pgp(b, id);
    ph(p)->kind = 0;
    ph(p)->next = h->pg_free;
    h->pg_free = id;
    ++h->pg_free_n;
}

// A probe: the key looked for, a number or a text, with a row to break ties.
typedef struct { int rank; int64_t iv; const uint8_t *s; uint32_t n; uint32_t row; } probe;

static probe probe_of(uint8_t *b, uint64_t c, uint32_t row) {
    probe p = {.rank = rank_of(c), .row = row};
    if (p.rank == 0) {
        p.iv = int_of(b, c);
    } else {
        tslot *t = text_at(b, (uint32_t)(c >> 4));
        p.s = arena(b) + t->off;
        p.n = t->len;
    }
    return p;
}
// An entry compared with a probe: -1 below it, 0 equal, 1 above.
static int cmp_probe(uint8_t *b, uint64_t cell, uint32_t row, const probe *p) {
    const int rx = rank_of(cell);
    if (rx != p->rank)
        return rx < p->rank ? -1 : 1;
    int d;
    if (rx == 0) {
        const int64_t a = int_of(b, cell);
        d = a < p->iv ? -1 : a > p->iv;
    } else {
        tslot *t = text_at(b, (uint32_t)(cell >> 4));
        const uint32_t m = t->len < p->n ? t->len : p->n;
        d = __builtin_memcmp(arena(b) + t->off, p->s, m);
        d = d ? (d < 0 ? -1 : 1) : (t->len < p->n ? -1 : t->len > p->n);
    }
    return d ? d : (row < p->row ? -1 : row > p->row);
}

typedef struct { uint32_t page[16], idx[16], depth; } tpath;

// The leaf where the first entry not below the probe lies, with the interior pages and the child taken at each.
static uint32_t descend(uint8_t *b, const colinfo *ci, const probe *pr, tpath *pa) {
    uint32_t id = ci->root;
    pa->depth = 0;
    for (uint32_t lvl = ci->height; lvl > 0; --lvl) {
        uint8_t *p = pgp(b, id);
        uint32_t lo = 0, hi = ph(p)->n;
        while (lo < hi) { // the keys at or below the probe
            const uint32_t mid = (lo + hi) / 2;
            if (cmp_probe(b, icell(p)[mid], irow(p)[mid], pr) <= 0)
                lo = mid + 1;
            else
                hi = mid;
        }
        pa->page[pa->depth] = id;
        pa->idx[pa->depth] = lo;
        ++pa->depth;
        id = ich(p)[lo];
    }
    return id;
}
static uint32_t leaf_lb(uint8_t *b, uint8_t *p, const probe *pr) {
    uint32_t lo = 0, hi = ph(p)->n;
    while (lo < hi) {
        const uint32_t mid = (lo + hi) / 2;
        if (cmp_probe(b, lcell(p)[mid], lrow(p)[mid], pr) < 0)
            lo = mid + 1;
        else
            hi = mid;
    }
    return lo;
}

typedef struct { uint32_t leaf, pos; } iter; // leaf 0: the end
static void iter_norm(uint8_t *b, iter *it) {
    while (it->leaf && it->pos >= ph(pgp(b, it->leaf))->n) {
        it->leaf = ph(pgp(b, it->leaf))->next;
        it->pos = 0;
    }
}
static iter idx_lb(uint8_t *b, const colinfo *ci, const probe *pr) {
    tpath pa;
    const uint32_t leaf = descend(b, ci, pr, &pa);
    iter it = {leaf, leaf_lb(b, pgp(b, leaf), pr)};
    iter_norm(b, &it);
    return it;
}
static uint64_t it_cell(uint8_t *b, const iter *it) { return lcell(pgp(b, it->leaf))[it->pos]; }
static uint32_t it_row(uint8_t *b, const iter *it) { return lrow(pgp(b, it->leaf))[it->pos]; }
static void it_next(uint8_t *b, iter *it) {
    ++it->pos;
    iter_norm(b, it);
}
static iter idx_first(uint8_t *b, const colinfo *ci) {
    uint32_t id = ci->root;
    for (uint32_t lvl = ci->height; lvl > 0; --lvl)
        id = ich(pgp(b, id))[0];
    iter it = {id, 0};
    iter_norm(b, &it);
    return it;
}

static bool idx_need(uint8_t *b, const colinfo *ci) { return pg_avail(H(b)) < ci->height + 2u; }

// Inserts (cell, row). The caller made sure that idx_need is false, so no page runs out half way.
static void idx_ins(uint8_t *b, colinfo *ci, uint64_t cell, uint32_t row) {
    const probe pr = probe_of(b, cell, row);
    tpath      pa;
    const uint32_t leaf = descend(b, ci, &pr, &pa);
    uint8_t *p = pgp(b, leaf);
    uint32_t n = ph(p)->n;
    const uint32_t pos = leaf_lb(b, p, &pr);
    if (n < LEAF_MAX) {
        __builtin_memmove(lcell(p) + pos + 1, lcell(p) + pos, (size_t)(n - pos) * 8);
        __builtin_memmove(lrow(p) + pos + 1, lrow(p) + pos, (size_t)(n - pos) * 4);
        lcell(p)[pos] = cell;
        lrow(p)[pos] = row;
        ph(p)->n = (uint16_t)(n + 1);
        return;
    }
    const uint32_t nid = pg_alloc(b);
    uint8_t *q = pgp(b, nid);
    p = pgp(b, leaf);
    const uint32_t half = LEAF_MAX / 2;
    ph(q)->kind = 1;
    __builtin_memcpy(lcell(q), lcell(p) + half, (size_t)(n - half) * 8);
    __builtin_memcpy(lrow(q), lrow(p) + half, (size_t)(n - half) * 4);
    ph(q)->n = (uint16_t)(n - half);
    ph(p)->n = (uint16_t)half;
    ph(q)->next = ph(p)->next;
    ph(q)->prev = leaf;
    if (ph(p)->next)
        ph(pgp(b, ph(p)->next))->prev = nid;
    ph(p)->next = nid;
    uint8_t *t = pos <= half ? p : q;
    const uint32_t tp = pos <= half ? pos : pos - half, tn = ph(t)->n;
    __builtin_memmove(lcell(t) + tp + 1, lcell(t) + tp, (size_t)(tn - tp) * 8);
    __builtin_memmove(lrow(t) + tp + 1, lrow(t) + tp, (size_t)(tn - tp) * 4);
    lcell(t)[tp] = cell;
    lrow(t)[tp] = row;
    ph(t)->n = (uint16_t)(tn + 1);
    uint64_t sc = lcell(q)[0];
    uint32_t sr = lrow(q)[0], child = nid;
    while (pa.depth > 0) {
        --pa.depth;
        uint8_t *ip = pgp(b, pa.page[pa.depth]);
        const uint32_t at = pa.idx[pa.depth], kn = ph(ip)->n;
        uint64_t tk[NODE_KEYS + 1];
        uint32_t tr[NODE_KEYS + 1], tc[NODE_KEYS + 2];
        if (kn < NODE_KEYS) {
            __builtin_memmove(icell(ip) + at + 1, icell(ip) + at, (size_t)(kn - at) * 8);
            __builtin_memmove(irow(ip) + at + 1, irow(ip) + at, (size_t)(kn - at) * 4);
            __builtin_memmove(ich(ip) + at + 2, ich(ip) + at + 1, (size_t)(kn - at) * 4);
            icell(ip)[at] = sc;
            irow(ip)[at] = sr;
            ich(ip)[at + 1] = child;
            ph(ip)->n = (uint16_t)(kn + 1);
            return;
        }
        __builtin_memcpy(tk, icell(ip), (size_t)at * 8);
        __builtin_memcpy(tr, irow(ip), (size_t)at * 4);
        __builtin_memcpy(tc, ich(ip), (size_t)(at + 1) * 4);
        tk[at] = sc;
        tr[at] = sr;
        tc[at + 1] = child;
        __builtin_memcpy(tk + at + 1, icell(ip) + at, (size_t)(kn - at) * 8);
        __builtin_memcpy(tr + at + 1, irow(ip) + at, (size_t)(kn - at) * 4);
        __builtin_memcpy(tc + at + 2, ich(ip) + at + 1, (size_t)(kn - at) * 4);
        const uint32_t mid = (NODE_KEYS + 1) / 2, rn = NODE_KEYS - mid; // NODE_KEYS + 1 keys: mid left, one up, rn right
        const uint32_t rid = pg_alloc(b);
        ip = pgp(b, pa.page[pa.depth]);
        uint8_t *rp = pgp(b, rid);
        ph(rp)->kind = 2;
        __builtin_memcpy(icell(ip), tk, (size_t)mid * 8);
        __builtin_memcpy(irow(ip), tr, (size_t)mid * 4);
        __builtin_memcpy(ich(ip), tc, (size_t)(mid + 1) * 4);
        ph(ip)->n = (uint16_t)mid;
        __builtin_memcpy(icell(rp), tk + mid + 1, (size_t)rn * 8);
        __builtin_memcpy(irow(rp), tr + mid + 1, (size_t)rn * 4);
        __builtin_memcpy(ich(rp), tc + mid + 1, (size_t)(rn + 1) * 4);
        ph(rp)->n = (uint16_t)rn;
        sc = tk[mid];
        sr = tr[mid];
        child = rid;
    }
    const uint32_t root = pg_alloc(b);
    uint8_t *rp = pgp(b, root);
    ph(rp)->kind = 2;
    ph(rp)->n = 1;
    icell(rp)[0] = sc;
    irow(rp)[0] = sr;
    ich(rp)[0] = ci->root;
    ich(rp)[1] = child;
    ci->root = root;
    ++ci->height;
}

// Removes a key from an interior page: key `k` and child `k + 1`.
static void node_drop(uint8_t *ip, uint32_t k) {
    const uint32_t n = ph(ip)->n;
    __builtin_memmove(icell(ip) + k, icell(ip) + k + 1, (size_t)(n - k - 1) * 8);
    __builtin_memmove(irow(ip) + k, irow(ip) + k + 1, (size_t)(n - k - 1) * 4);
    __builtin_memmove(ich(ip) + k + 1, ich(ip) + k + 2, (size_t)(n - k - 1) * 4);
    ph(ip)->n = (uint16_t)(n - 1);
}
static void leaf_append(uint8_t *to, uint8_t *from) {
    const uint32_t tn = ph(to)->n, fn = ph(from)->n;
    __builtin_memcpy(lcell(to) + tn, lcell(from), (size_t)fn * 8);
    __builtin_memcpy(lrow(to) + tn, lrow(from), (size_t)fn * 4);
    ph(to)->n = (uint16_t)(tn + fn);
}
// Merges leaf `from` (a right neighbour of `to` under one parent) into `to` and frees it.
static void leaf_merge(uint8_t *b, uint32_t to, uint32_t from) {
    uint8_t *tp = pgp(b, to), *fp = pgp(b, from);
    leaf_append(tp, fp);
    ph(tp)->next = ph(fp)->next;
    if (ph(fp)->next)
        ph(pgp(b, ph(fp)->next))->prev = to;
    pg_release(b, from);
}

static void idx_del(uint8_t *b, colinfo *ci, uint64_t cell, uint32_t row) {
    const probe pr = probe_of(b, cell, row);
    tpath      pa;
    const uint32_t leaf = descend(b, ci, &pr, &pa);
    uint8_t *p = pgp(b, leaf);
    const uint32_t pos = leaf_lb(b, p, &pr), n = ph(p)->n;
    if (pos >= n || lrow(p)[pos] != row || cmp_probe(b, lcell(p)[pos], row, &pr) != 0)
        return; // not there: nothing to remove
    __builtin_memmove(lcell(p) + pos, lcell(p) + pos + 1, (size_t)(n - pos - 1) * 8);
    __builtin_memmove(lrow(p) + pos, lrow(p) + pos + 1, (size_t)(n - pos - 1) * 4);
    ph(p)->n = (uint16_t)(n - 1);
    if (n - 1 >= LEAF_MAX / 2 || pa.depth == 0)
        return;
    uint8_t *ip = pgp(b, pa.page[pa.depth - 1]);
    const uint32_t at = pa.idx[pa.depth - 1], kn = ph(ip)->n;
    if (at < kn && (uint32_t)(n - 1) + ph(pgp(b, ich(ip)[at + 1]))->n <= LEAF_MAX) {
        leaf_merge(b, leaf, ich(ip)[at + 1]);
        node_drop(ip, at);
    } else if (at > 0 && (uint32_t)(n - 1) + ph(pgp(b, ich(ip)[at - 1]))->n <= LEAF_MAX) {
        leaf_merge(b, ich(ip)[at - 1], leaf);
        node_drop(ip, at - 1);
    } else {
        return;
    }
    while (ci->height > 0 && ph(pgp(b, ci->root))->n == 0) { // a root with one child is that child
        const uint32_t old = ci->root;
        ci->root = ich(pgp(b, old))[0];
        --ci->height;
        pg_release(b, old);
    }
}

static void idx_free_tree(uint8_t *b, uint32_t id, uint32_t level) {
    if (level > 0) {
        uint8_t *p = pgp(b, id);
        for (uint32_t k = 0; k <= ph(p)->n; ++k)
            idx_free_tree(b, ich(p)[k], level - 1);
    }
    pg_release(b, id);
}
static void idx_drop(uint8_t *b, colinfo *ci) {
    if (ci->root)
        idx_free_tree(b, ci->root, ci->height);
    ci->root = 0;
    ci->height = 0;
}
// The new index is an empty leaf.
static pnx_sheet_result idx_start(uint8_t *b, colinfo *ci) {
    const uint32_t id = pg_alloc(b);
    if (!id)
        return PNX_SH_FULL;
    ph(pgp(b, id))->kind = 1;
    ci->root = id;
    ci->height = 0;
    return PNX_SH_OK;
}
static inline bool indexable(uint64_t c) { return rank_of(c) < 2; }

// ---- regions --------------------------------------------------------------------------------

static uint64_t region_size(uint32_t rmax, uint32_t cols) {
    return (uint64_t)rmax * cols * 8 + round8((uint64_t)rmax * 4) + (uint64_t)cols * sizeof(colinfo);
}
// Slides the sheet regions down over the holes that dropped and widened sheets left.
static void regions_compact(uint8_t *b) {
    head    *h = H(b);
    uint32_t cursor = h->data_lo;
    for (uint32_t k = 0; k < h->nused; ++k)
        SHT(b, k)->mark = 0;
    for (;;) {
        sht *first = NULL;
        for (uint32_t k = 0; k < h->nused; ++k) {
            sht *s = SHT(b, k);
            if (s->used && !s->mark && (!first || s->off_cells < first->off_cells))
                first = s;
        }
        if (!first)
            break;
        if (first->off_cells != cursor) {
            __builtin_memmove(b + cursor, b + first->off_cells, first->size);
            const uint32_t shift = first->off_cells - cursor;
            first->off_cells -= shift;
            first->off_sel -= shift;
            first->off_col -= shift;
        }
        first->mark = 1;
        cursor += first->size;
    }
    for (uint32_t k = 0; k < h->nused; ++k)
        SHT(b, k)->mark = 0;
    h->data_top = cursor;
}
// Room for `size` bytes at the top of the sheet data, closing the holes once if that is what it takes.
static bool region_room(uint8_t *b, uint64_t size) {
    head *h = H(b);
    if ((uint64_t)h->data_top + size <= pg_lo(h))
        return true;
    regions_compact(b);
    return (uint64_t)h->data_top + size <= pg_lo(h);
}

// ---- sheets ---------------------------------------------------------------------------------

static uint32_t find_name(uint8_t *b, const uint8_t *name, uint32_t n);

static bool good_name(const uint8_t *name, uint32_t n) { return name && n > 0 && n <= NAME_MAX_LEN; }

pnx_sheet_result pnx_sheet_create(uint8_t *b, uint32_t cap, const uint8_t *name, uint32_t n, int64_t rmax, int64_t cols, bool colmajor,
                                  int64_t *num) {
    if (!ok(b, cap))
        return PNX_SH_BAD;
    head *h = H(b);
    if (rmax < 1 || rmax > (1 << 24) || cols < 1 || cols > 4096 || !good_name(name, n))
        return PNX_SH_RANGE;
    if (find_name(b, name, n) != NO_SHEET)
        return PNX_SH_KIND; // the name is taken
    uint32_t k = 0;
    while (k < h->nused && SHT(b, k)->used)
        ++k;
    if (k == h->nmax)
        return PNX_SH_FULL;
    const uint64_t size = region_size((uint32_t)rmax, (uint32_t)cols);
    if (!region_room(b, size))
        return PNX_SH_FULL;
    uint32_t slot;
    pnx_sheet_result e = text_take(b, name, n, &slot);
    if (e)
        return e;
    sht s = {.used = 1, .name = slot, .rmax = (uint32_t)rmax, .cols = (uint32_t)cols, .sel_all = 1, .colmajor = colmajor,
             .off_cells = h->data_top, .size = (uint32_t)size};
    s.off_sel = s.off_cells + (uint32_t)rmax * (uint32_t)cols * 8;
    s.off_col = s.off_sel + (uint32_t)round8((uint64_t)rmax * 4);
    __builtin_memset(b + h->data_top, 0, size);
    h->data_top += (uint32_t)size;
    *SHT(b, k) = s;
    if (k == h->nused)
        ++h->nused;
    if (h->cur == NO_SHEET)
        h->cur = k;
    *num = k;
    return PNX_SH_OK;
}

static uint32_t find_name(uint8_t *b, const uint8_t *name, uint32_t n) {
    const head *h = H(b);
    if (!good_name(name, n))
        return NO_SHEET;
    uint32_t hv = hash_bytes(name, n), slot = UINT32_MAX;
    for (uint32_t k = *tbk_at(b, hv % h->text_buckets); k; k = text_at(b, k - 1)->next) {
        tslot *t = text_at(b, k - 1);
        if (t->hash == hv && t->len == n && !__builtin_memcmp(arena(b) + t->off, name, n)) {
            slot = k - 1;
            break;
        }
    }
    if (slot == UINT32_MAX)
        return NO_SHEET;
    for (uint32_t k = 0; k < h->nused; ++k)
        if (SHT(b, k)->used && SHT(b, k)->name == slot)
            return k;
    return NO_SHEET;
}

pnx_sheet_result pnx_sheet_use(uint8_t *b, uint32_t cap, const uint8_t *name, uint32_t n) {
    if (!ok(b, cap))
        return PNX_SH_BAD;
    const uint32_t k = find_name(b, name, n);
    if (k == NO_SHEET)
        return PNX_SH_NONE;
    H(b)->cur = k;
    return PNX_SH_OK;
}
pnx_sheet_result pnx_sheet_use_n(uint8_t *b, uint32_t cap, int64_t k) {
    if (!ok(b, cap))
        return PNX_SH_BAD;
    if (k < 0 || (uint64_t)k >= H(b)->nused || !SHT(b, (uint32_t)k)->used)
        return PNX_SH_NONE;
    H(b)->cur = (uint32_t)k;
    return PNX_SH_OK;
}
pnx_sheet_result pnx_sheet_current(uint8_t *b, uint32_t cap, int64_t *out) {
    if (!ok(b, cap))
        return PNX_SH_BAD;
    *out = H(b)->cur == NO_SHEET ? -1 : (int64_t)H(b)->cur;
    return PNX_SH_OK;
}
pnx_sheet_result pnx_sheet_sheets(uint8_t *b, uint32_t cap, int64_t *out) {
    if (!ok(b, cap))
        return PNX_SH_BAD;
    *out = H(b)->nused;
    return PNX_SH_OK;
}
pnx_sheet_result pnx_sheet_sheet_name(uint8_t *b, uint32_t cap, int64_t k, uint32_t *off, uint32_t *len) {
    if (!ok(b, cap))
        return PNX_SH_BAD;
    if (k < 0 || (uint64_t)k >= H(b)->nused || !SHT(b, (uint32_t)k)->used)
        return PNX_SH_NONE;
    const tslot *t = text_at(b, SHT(b, (uint32_t)k)->name);
    *off = H(b)->off_arena + t->off;
    *len = t->len;
    return PNX_SH_OK;
}
pnx_sheet_result pnx_sheet_find_sheet(uint8_t *b, uint32_t cap, const uint8_t *name, uint32_t n, int64_t *num) {
    if (!ok(b, cap))
        return PNX_SH_BAD;
    const uint32_t k = find_name(b, name, n);
    if (k == NO_SHEET)
        return PNX_SH_NONE;
    *num = k;
    return PNX_SH_OK;
}

static void sheet_clear_cells(uint8_t *b, sht *s) {
    for (uint32_t c = 0; c < s->cols; ++c) {
        idx_drop(b, col_at(b, s, c));
        for (uint32_t r = 0; r < s->rows; ++r)
            release(b, *cell_at(b, s, r, c));
    }
}
pnx_sheet_result pnx_sheet_drop(uint8_t *b, uint32_t cap, const uint8_t *name, uint32_t n) {
    if (!ok(b, cap))
        return PNX_SH_BAD;
    head *h = H(b);
    const uint32_t k = find_name(b, name, n);
    if (k == NO_SHEET)
        return PNX_SH_NONE;
    sht *s = SHT(b, k);
    sheet_clear_cells(b, s);
    text_give(b, s->name);
    if (s->off_cells + s->size == h->data_top)
        h->data_top = s->off_cells;
    __builtin_memset(s, 0, sizeof *s);
    if (h->cur == k)
        h->cur = NO_SHEET;
    while (h->nused > 0 && !SHT(b, h->nused - 1)->used)
        --h->nused;
    if (h->cur != NO_SHEET && h->cur >= h->nused)
        h->cur = NO_SHEET;
    return PNX_SH_OK;
}
pnx_sheet_result pnx_sheet_rename(uint8_t *b, uint32_t cap, int64_t k, const uint8_t *name, uint32_t n) {
    if (!ok(b, cap))
        return PNX_SH_BAD;
    if (k < 0 || (uint64_t)k >= H(b)->nused || !SHT(b, (uint32_t)k)->used)
        return PNX_SH_NONE;
    if (!good_name(name, n))
        return PNX_SH_RANGE;
    const uint32_t other = find_name(b, name, n);
    if (other == (uint32_t)k)
        return PNX_SH_OK;
    if (other != NO_SHEET)
        return PNX_SH_KIND;
    uint32_t slot;
    pnx_sheet_result e = text_take(b, name, n, &slot);
    if (e)
        return e;
    sht *s = SHT(b, (uint32_t)k);
    text_give(b, s->name);
    s->name = slot;
    return PNX_SH_OK;
}

// ---- rows -----------------------------------------------------------------------------------

pnx_sheet_result pnx_sheet_add_row(uint8_t *b, uint32_t cap, uint32_t *row) {
    sht *s;
    pnx_sheet_result e = cur_sheet(b, cap, &s);
    if (e)
        return e;
    if (s->rows == s->rmax)
        return PNX_SH_FULL;
    for (uint32_t c = 0; c < s->cols; ++c)
        *cell_at(b, s, s->rows, c) = 0;
    *row = s->rows++;
    return PNX_SH_OK;
}
pnx_sheet_result pnx_sheet_truncate_rows(uint8_t *b, uint32_t cap, uint32_t n) {
    sht *s;
    pnx_sheet_result e = cur_sheet(b, cap, &s);
    if (e)
        return e;
    if (n > s->rows)
        return PNX_SH_RANGE;
    for (uint32_t c = 0; c < s->cols; ++c) {
        colinfo *ci = col_at(b, s, c);
        if (ci->root && n == 0) { // all of it goes: the tree starts again
            idx_drop(b, ci);
            idx_start(b, ci); // a page was just freed: cannot fail
        }
        for (uint32_t r = n; r < s->rows; ++r) {
            uint64_t *cell = cell_at(b, s, r, c);
            if (ci->root && n != 0 && indexable(*cell))
                idx_del(b, ci, *cell, r);
            release(b, *cell);
            *cell = 0;
        }
    }
    s->rows = n;
    s->sel_all = 1;
    s->sel_n = 0;
    return PNX_SH_OK;
}
pnx_sheet_result pnx_sheet_clear(uint8_t *b, uint32_t cap) {
    return pnx_sheet_truncate_rows(b, cap, 0);
}

// The columns made C2 (more, or fewer when the cut columns are empty), rows kept: a new region is made at
// the top of the sheet data and the old one is left as a hole for `compact`. Indexes keep their pages.
pnx_sheet_result pnx_sheet_widen(uint8_t *b, uint32_t cap, uint32_t c2) {
    sht *s;
    pnx_sheet_result e = cur_sheet(b, cap, &s);
    if (e)
        return e;
    if (c2 == 0 || c2 > 4096)
        return PNX_SH_RANGE;
    if (c2 == s->cols)
        return PNX_SH_OK;
    for (uint32_t c = c2; c < s->cols; ++c)
        for (uint32_t r = 0; r < s->rows; ++r)
            if (*cell_at(b, s, r, c))
                return PNX_SH_KIND;
    const uint64_t size = region_size(s->rmax, c2);
    const uint32_t me = H(b)->cur;
    if (!region_room(b, size))
        return PNX_SH_FULL;
    s = SHT(b, me); // compaction moved the regions, not the table
    head *h = H(b);
    const sht old = *s;
    uint8_t *region = b + h->data_top;
    __builtin_memset(region, 0, size);
    s->off_cells = h->data_top;
    s->off_sel = s->off_cells + s->rmax * c2 * 8;
    s->off_col = s->off_sel + (uint32_t)round8((uint64_t)s->rmax * 4);
    s->size = (uint32_t)size;
    s->cols = c2;
    sht from = old;
    const uint32_t keep = c2 < old.cols ? c2 : old.cols;
    for (uint32_t c = 0; c < keep; ++c)
        for (uint32_t r = 0; r < old.rows; ++r)
            *cell_at(b, s, r, c) = *cell_at(b, &from, r, c);
    __builtin_memcpy(sel_at(b, s), sel_at(b, &from), (size_t)old.rmax * 4);
    for (uint32_t c = 0; c < keep; ++c)
        *col_at(b, s, c) = *col_at(b, &from, c);
    for (uint32_t c = keep; c < old.cols; ++c) // cut columns were empty: their indexes hold nothing
        idx_drop(b, col_at(b, &from, c));
    h->data_top += (uint32_t)size;
    return PNX_SH_OK;
}

// ---- cells ----------------------------------------------------------------------------------

static pnx_sheet_result at_sh(const sht *s, uint8_t *b, int64_t r, int64_t c, uint64_t **cell) {
    if (r < 0 || c < 0 || (uint64_t)r >= s->rows || (uint64_t)c >= s->cols)
        return PNX_SH_RANGE;
    *cell = cell_at(b, s, (uint32_t)r, (uint32_t)c);
    return PNX_SH_OK;
}
static pnx_sheet_result at(uint8_t *b, uint32_t cap, int64_t r, int64_t c, uint64_t **cell) {
    sht *s;
    pnx_sheet_result e = cur_sheet(b, cap, &s);
    return e ? e : at_sh(s, b, r, c, cell);
}

// Puts the value `nc` (already taken from the pools) into a cell, keeping the column's index in step.
// If the index cannot take it, the cell is left as it was and the value is given back.
static pnx_sheet_result put_raw_sh(uint8_t *b, const sht *s, int64_t r, int64_t c, uint64_t nc) {
    uint64_t *cell;
    pnx_sheet_result e = at_sh(s, b, r, c, &cell);
    if (e) {
        release(b, nc);
        return e;
    }
    colinfo *ci = col_at(b, s, (uint32_t)c);
    if (ci->root) {
        if (indexable(nc) && idx_need(b, ci)) {
            release(b, nc);
            return PNX_SH_FULL;
        }
        if (indexable(*cell))
            idx_del(b, ci, *cell, (uint32_t)r);
    }
    release(b, *cell); // after the new value was taken: a cell set to what it held keeps its slot
    *cell = nc;
    if (ci->root && indexable(nc))
        idx_ins(b, ci, nc, (uint32_t)r);
    return PNX_SH_OK;
}
static pnx_sheet_result put_raw(uint8_t *b, uint32_t cap, int64_t r, int64_t c, uint64_t nc) {
    sht *s;
    pnx_sheet_result e = cur_sheet(b, cap, &s);
    if (e)
        return e;
    return put_raw_sh(b, s, r, c, nc);
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
// A date is Unix seconds in the cell itself, so it must fit 60 bits (any date of the years 1 to 9999 does, by far).
pnx_sheet_result pnx_sheet_put_date(uint8_t *b, uint32_t cap, int64_t v, int64_t r, int64_t c) {
    if (!fits_int(v))
        return PNX_SH_RANGE;
    return put_raw(b, cap, r, c, (uint64_t)v << 4 | T_DATE);
}
pnx_sheet_result pnx_sheet_get_date(uint8_t *b, uint32_t cap, int64_t r, int64_t c, int64_t *v) {
    uint64_t *cell;
    pnx_sheet_result e = at(b, cap, r, c, &cell);
    if (e)
        return e;
    if ((*cell & 15u) != T_DATE)
        return PNX_SH_KIND;
    *v = cell_int(*cell);
    return PNX_SH_OK;
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
    sht *s;
    pnx_sheet_result e = cur_sheet(b, cap, &s);
    if (e)
        return e;
    uint64_t *cell;
    if ((e = at_sh(s, b, r, c, &cell)))
        return e;
    colinfo *ci = col_at(b, s, (uint32_t)c);
    if (ci->root && indexable(*cell))
        idx_del(b, ci, *cell, (uint32_t)r);
    release(b, *cell);
    *cell = 0;
    return PNX_SH_OK;
}
// A cell of the current sheet copied into a cell of sheet `k` (the same sheet for copy_cell): the value is shared.
pnx_sheet_result pnx_sheet_copy_to(uint8_t *b, uint32_t cap, int64_t r1, int64_t c1, int64_t k, int64_t r2, int64_t c2) {
    sht *s;
    pnx_sheet_result e = cur_sheet(b, cap, &s);
    if (e)
        return e;
    if (k < 0 || (uint64_t)k >= H(b)->nused || !SHT(b, (uint32_t)k)->used)
        return PNX_SH_NONE;
    const sht *t = SHT(b, (uint32_t)k);
    uint64_t *from, *to;
    if ((e = at_sh(s, b, r1, c1, &from)) || (e = at_sh(t, b, r2, c2, &to)))
        return e;
    const uint64_t v = *from;
    retain(b, v);
    return put_raw_sh(b, t, r2, c2, v);
}
pnx_sheet_result pnx_sheet_copy_cell(uint8_t *b, uint32_t cap, int64_t r1, int64_t c1, int64_t r2, int64_t c2) {
    if (!ok(b, cap))
        return PNX_SH_BAD;
    return pnx_sheet_copy_to(b, cap, r1, c1, H(b)->cur == NO_SHEET ? -1 : (int64_t)H(b)->cur, r2, c2);
}

static uint32_t kind_of(uint64_t c) {
    switch (c & 15u) {
    case T_EMPTY: return 0;
    case T_INT: case T_WINT: return 1;
    case T_BOOL: return 2;
    case T_DEC: case T_WDEC: return 3;
    case T_DATE: return 5;
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
pnx_sheet_result pnx_sheet_get_int(uint8_t *b, uint32_t cap, int64_t r, int64_t c, int64_t *v) {
    uint64_t *cell;
    pnx_sheet_result e = at(b, cap, r, c, &cell);
    if (e)
        return e;
    if (rank_of(*cell) != 0 || (*cell & 15u) == T_DATE)
        return PNX_SH_KIND; // a date is read with `date`
    *v = int_of(b, *cell);
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

// ---- the index words ------------------------------------------------------------------------

pnx_sheet_result pnx_sheet_index(uint8_t *b, uint32_t cap, int64_t col) {
    sht *s;
    pnx_sheet_result e = cur_sheet(b, cap, &s);
    if (e)
        return e;
    if (col < 0 || (uint64_t)col >= s->cols)
        return PNX_SH_RANGE;
    colinfo *ci = col_at(b, s, (uint32_t)col);
    if (ci->root)
        return PNX_SH_OK;
    if ((e = idx_start(b, ci)))
        return e;
    for (uint32_t r = 0; r < s->rows; ++r) {
        const uint64_t cell = *cell_at(b, s, r, (uint32_t)col);
        if (!indexable(cell))
            continue;
        if (idx_need(b, ci)) {
            idx_drop(b, ci);
            return PNX_SH_FULL;
        }
        idx_ins(b, ci, cell, r);
    }
    return PNX_SH_OK;
}
pnx_sheet_result pnx_sheet_drop_index(uint8_t *b, uint32_t cap, int64_t col) {
    sht *s;
    pnx_sheet_result e = cur_sheet(b, cap, &s);
    if (e)
        return e;
    if (col < 0 || (uint64_t)col >= s->cols)
        return PNX_SH_RANGE;
    idx_drop(b, col_at(b, s, (uint32_t)col));
    return PNX_SH_OK;
}
pnx_sheet_result pnx_sheet_indexed(uint8_t *b, uint32_t cap, int64_t col, int64_t *out) {
    sht *s;
    pnx_sheet_result e = cur_sheet(b, cap, &s);
    if (e)
        return e;
    if (col < 0 || (uint64_t)col >= s->cols)
        return PNX_SH_RANGE;
    *out = col_at(b, s, (uint32_t)col)->root != 0;
    return PNX_SH_OK;
}

// ---- searching ------------------------------------------------------------------------------

// Whether a cell equals an int key.
static bool eq_int(uint8_t *b, uint64_t c, int64_t key) {
    if ((c & 15u) == T_INT || (c & 15u) == T_DATE)
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

static const uint8_t none_bytes[1] = {0};

// along = 0: down column `line` from row `from`; 1: across row `line` from column `from`.
static pnx_sheet_result find(uint8_t *b, uint32_t cap, bool across, bool text, int64_t ikey, const uint8_t *s, uint32_t n,
                             int64_t line, int64_t from, int64_t *found, uint64_t *scanned) {
    sht *sh;
    pnx_sheet_result e = cur_sheet(b, cap, &sh);
    if (e)
        return e;
    const uint32_t lines = across ? sh->rows : sh->cols, len = across ? sh->cols : sh->rows;
    if (line < 0 || (uint64_t)line >= lines || from < 0)
        return PNX_SH_RANGE;
    uint32_t slot = 0;
    if (text) {
        slot = text_find(b, s, n);
        if (slot == UINT32_MAX)
            return PNX_SH_NONE;
    }
    *scanned = 8;
    const colinfo *ci = across ? NULL : col_at(b, sh, (uint32_t)line);
    if (ci && ci->root) { // the first entry not below (key, from), if it has the key
        probe pr = {.rank = text ? 1 : 0, .iv = ikey, .s = text ? s : none_bytes, .n = text ? n : 0,
                    .row = from > UINT32_MAX ? UINT32_MAX : (uint32_t)from};
        const iter it = idx_lb(b, ci, &pr);
        if (!it.leaf)
            return PNX_SH_NONE;
        pr.row = it_row(b, &it);
        if (cmp_probe(b, it_cell(b, &it), pr.row, &pr) != 0)
            return PNX_SH_NONE;
        *found = pr.row;
        return PNX_SH_OK;
    }
    *scanned = len;
    for (uint64_t k = (uint64_t)from; k < len; ++k) {
        const uint64_t c = across ? *cell_at(b, sh, (uint32_t)line, (uint32_t)k) : *cell_at(b, sh, (uint32_t)k, (uint32_t)line);
        if (text ? ((c & 15u) == T_STR && (uint32_t)(c >> 4) == slot) : eq_int(b, c, ikey)) {
            *found = (int64_t)k;
            return PNX_SH_OK;
        }
    }
    return PNX_SH_NONE;
}
pnx_sheet_result pnx_sheet_find_int(uint8_t *b, uint32_t cap, bool across, int64_t key, int64_t line, int64_t from, int64_t *f,
                                    uint64_t *scanned) {
    return find(b, cap, across, false, key, NULL, 0, line, from, f, scanned);
}
pnx_sheet_result pnx_sheet_find_str(uint8_t *b, uint32_t cap, bool across, const uint8_t *s, uint32_t n, int64_t line,
                                    int64_t from, int64_t *f, uint64_t *scanned) {
    return find(b, cap, across, true, 0, s, n, line, from, f, scanned);
}

// ---- the selection (rows chosen by where and narrow) -----------------------------------------

uint32_t pnx_sheet_sel_len(uint8_t *b, uint32_t cap) {
    sht *s;
    if (cur_sheet(b, cap, &s))
        return 0;
    return s->sel_all ? s->rows : s->sel_n;
}
pnx_sheet_result pnx_sheet_sel_row(uint8_t *b, uint32_t cap, int64_t k, int64_t *row) {
    sht *s;
    pnx_sheet_result e = cur_sheet(b, cap, &s);
    if (e)
        return e;
    if (k < 0 || (uint64_t)k >= pnx_sheet_sel_len(b, cap))
        return PNX_SH_RANGE;
    *row = s->sel_all ? k : sel_at(b, s)[k];
    return PNX_SH_OK;
}
pnx_sheet_result pnx_sheet_select_all(uint8_t *b, uint32_t cap) {
    sht *s;
    pnx_sheet_result e = cur_sheet(b, cap, &s);
    if (e)
        return e;
    s->sel_all = 1;
    s->sel_n = 0;
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
                                  uint32_t n, uint64_t *scanned) {
    sht *sh;
    pnx_sheet_result e = cur_sheet(b, cap, &sh);
    if (e)
        return e;
    if (col < 0 || (uint64_t)col >= sh->cols || op < 0 || op > 5)
        return PNX_SH_RANGE;
    const uint32_t count = pnx_sheet_sel_len(b, cap);
    uint32_t keep = 0;
    uint32_t *sel = sel_at(b, sh);
    for (uint32_t k = 0; k < count; ++k) {
        const uint32_t r = sh->sel_all ? k : sel[k];
        const uint64_t c = *cell_at(b, sh, r, (uint32_t)col);
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
        } else if (rank_of(c) == 0) {
            hit = test_int((int)op, int_of(b, c), ikey);
        }
        if (hit)
            sel[keep++] = r;
    }
    sh->sel_all = 0;
    sh->sel_n = keep;
    *scanned = count;
    return PNX_SH_OK;
}

// A free stretch in the middle of the buffer (between the sheet data and the index pages) for `bytes`, or NULL.
static uint32_t *middle(uint8_t *b, uint64_t bytes) {
    const head *h = H(b);
    const uint64_t lo = pg_lo(h);
    if (lo < h->data_top + bytes + 8)
        return NULL;
    return (uint32_t *)(void *)(b + ((lo - bytes) & ~7ull));
}
static void sort_u32(uint32_t *a, uint32_t n, uint32_t *tmp) {
    for (uint32_t width = 1; width < n; width *= 2) {
        for (uint32_t lo = 0; lo < n; lo += 2 * width) {
            const uint32_t mid = lo + width < n ? lo + width : n, hi = lo + 2 * width < n ? lo + 2 * width : n;
            uint32_t i = lo, j = mid, o = lo;
            while (i < mid && j < hi)
                tmp[o++] = a[j] < a[i] ? a[j++] : a[i++];
            while (i < mid) tmp[o++] = a[i++];
            while (j < hi) tmp[o++] = a[j++];
        }
        __builtin_memcpy(a, tmp, (size_t)n * 4);
    }
}

// where on an indexed column: the rows come out of the tree in key order and are put in row order. False
// when there is no room to sort them (the scan does it instead).
static bool where_indexed(uint8_t *b, sht *sh, const colinfo *ci, bool text, int op, int64_t ikey, const uint8_t *s, uint32_t n,
                          uint64_t *scanned) {
    const int rank = text ? 1 : 0;
    const uint8_t *key = text ? s : none_bytes;
    const uint32_t kn = text ? n : 0;
    probe lo = {.rank = rank, .iv = text ? 0 : ikey, .s = key, .n = kn, .row = 0};
    probe hi = lo;
    hi.row = UINT32_MAX;
    iter it;
    if (op == 0 || op == 5) {
        it = idx_lb(b, ci, &lo);
    } else if (op == 4) {
        it = idx_lb(b, ci, &hi);
    } else {
        probe first = {.rank = rank, .iv = INT64_MIN, .s = none_bytes, .n = 0, .row = 0};
        it = idx_lb(b, ci, &first);
    }
    uint32_t *sel = sel_at(b, sh), k = 0;
    for (; it.leaf; it_next(b, &it)) {
        const uint64_t cell = it_cell(b, &it);
        const uint32_t row = it_row(b, &it);
        if (rank_of(cell) != rank)
            break;
        if (op == 0 && cmp_probe(b, cell, row, &hi) > 0)
            break;
        if (op == 2 && cmp_probe(b, cell, row, &lo) >= 0)
            break;
        if (op == 3 && cmp_probe(b, cell, row, &hi) > 0)
            break;
        sel[k++] = row;
    }
    uint32_t *tmp = middle(b, (uint64_t)k * 4);
    if (!tmp && k > 1)
        return false;
    sort_u32(sel, k, tmp);
    sh->sel_all = 0;
    sh->sel_n = k;
    *scanned = 16 + k;
    return true;
}

pnx_sheet_result pnx_sheet_where(uint8_t *b, uint32_t cap, bool text, int64_t col, int64_t op, int64_t ikey, const uint8_t *s,
                                 uint32_t n, uint64_t *scanned) {
    sht *sh;
    pnx_sheet_result e = cur_sheet(b, cap, &sh);
    if (e)
        return e;
    if (col < 0 || (uint64_t)col >= sh->cols || op < 0 || op > 5)
        return PNX_SH_RANGE;
    const colinfo *ci = col_at(b, sh, (uint32_t)col);
    if (ci->root && op != 1 && where_indexed(b, sh, ci, text, (int)op, ikey, s, n, scanned))
        return PNX_SH_OK;
    sh->sel_all = 1;
    sh->sel_n = 0;
    return pnx_sheet_narrow(b, cap, text, col, op, ikey, s, n, scanned);
}

// ---- totals ---------------------------------------------------------------------------------
// what: 0 sum, 1 min, 2 max, 3 count. over: 0 a column (all rows), 1 a row (all columns), 2 a column of the selection.
pnx_sheet_result pnx_sheet_total(uint8_t *b, uint32_t cap, int what, int over, int64_t line, int64_t *out, uint64_t *scanned) {
    sht *h;
    pnx_sheet_result e = cur_sheet(b, cap, &h);
    if (e)
        return e;
    const uint32_t lines = over == 1 ? h->rows : h->cols;
    if (line < 0 || (uint64_t)line >= lines)
        return PNX_SH_RANGE;
    const uint32_t len = over == 0 ? h->rows : over == 1 ? h->cols : pnx_sheet_sel_len(b, cap);
    int64_t acc = 0, seen = 0;
    for (uint32_t k = 0; k < len; ++k) {
        uint32_t r = over == 1 ? (uint32_t)line : over == 0 ? k : (h->sel_all ? k : sel_at(b, h)[k]);
        uint32_t c = over == 1 ? k : (uint32_t)line;
        const uint64_t cell = *cell_at(b, h, r, c);
        if (rank_of(cell) != 0)
            continue;
        const int64_t v = int_of(b, cell);
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

// All rows ordered by an indexed column: the tree gives the numbers and texts in order, the other cells follow in row order.
static void order_indexed(uint8_t *b, sht *h, const colinfo *ci, uint32_t col, bool desc) {
    uint32_t *sel = sel_at(b, h), k = 0;
    for (iter it = idx_first(b, ci); it.leaf; it_next(b, &it))
        sel[k++] = it_row(b, &it);
    if (desc) { // the whole run backwards, then each run of equal keys forwards again: ties keep their row order
        for (uint32_t i = 0, j = k; i + 1 < j; ++i, --j) {
            const uint32_t t = sel[i];
            sel[i] = sel[j - 1];
            sel[j - 1] = t;
        }
        for (uint32_t i = 0; i < k;) {
            uint32_t j = i + 1;
            while (j < k && cmp_cells(b, *cell_at(b, h, sel[i], col), *cell_at(b, h, sel[j], col), false) == 0)
                ++j;
            for (uint32_t x = i, y = j; x + 1 < y; ++x, --y) {
                const uint32_t t = sel[x];
                sel[x] = sel[y - 1];
                sel[y - 1] = t;
            }
            i = j;
        }
    }
    for (uint32_t r = 0; r < h->rows; ++r)
        if (!indexable(*cell_at(b, h, r, col)))
            sel[k++] = r;
    h->sel_n = k;
    h->sel_all = 0;
}

// A stable merge sort of the selected rows by column `col`; the work area is the free middle of the buffer.
pnx_sheet_result pnx_sheet_order_by(uint8_t *b, uint32_t cap, int64_t col, bool desc, uint64_t *scanned) {
    sht *h;
    pnx_sheet_result e = cur_sheet(b, cap, &h);
    if (e)
        return e;
    if (col < 0 || (uint64_t)col >= h->cols)
        return PNX_SH_RANGE;
    const colinfo *ci = col_at(b, h, (uint32_t)col);
    if (h->sel_all && ci->root) {
        order_indexed(b, h, ci, (uint32_t)col, desc);
        *scanned = (uint64_t)h->rows * 2;
        return PNX_SH_OK;
    }
    if (h->sel_all) { // the rows become an explicit selection first
        uint32_t *sel = sel_at(b, h);
        for (uint32_t k = 0; k < h->rows; ++k)
            sel[k] = k;
        h->sel_n = h->rows;
        h->sel_all = 0;
    }
    const uint32_t n = h->sel_n;
    uint32_t *sel = sel_at(b, h);
    uint32_t *tmp = middle(b, (uint64_t)n * 4);
    if (!tmp && n > 1)
        return PNX_SH_FULL;
    for (uint32_t width = 1; width < n; width *= 2) {
        for (uint32_t lo = 0; lo < n; lo += 2 * width) {
            const uint32_t mid = lo + width < n ? lo + width : n, hi = lo + 2 * width < n ? lo + 2 * width : n;
            uint32_t i = lo, j = mid, o = lo;
            while (i < mid && j < hi) {
                const int d = cmp_cells(b, *cell_at(b, h, sel[j], (uint32_t)col), *cell_at(b, h, sel[i], (uint32_t)col), desc);
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
    regions_compact(b);
    return PNX_SH_OK;
}
