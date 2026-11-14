// pinax_sheetio.c - the files of the module `sheet` (RFC-0045): CSV and xlsx read into the current sheet of a
// book, and written from it. Nothing is allocated: the work area of an xlsx read is the spare capacity of the
// buffer the file was read into; a read takes the bytes of that buffer and may change them (CSV: quoted fields are
// unescaped in place).
//
// An xlsx is a ZIP of XML parts. The ZIP is read through its central directory; deflate is inflated in a 32 KiB
// window and handed to a small XML scanner byte by byte, so that no part is ever held whole except the shared
// strings (which a cell names by number). Writing makes a ZIP of stored (uncompressed) parts.
#include "pinax_internal.h"

// ---- numbers ---------------------------------------------------------------------------------

// The 18 digits after the point that a dec holds.
#define sio_DEC_MAX_SCALE 18u

typedef struct {
    int      kind; // 0 text, 1 int, 2 dec
    int64_t  v;
    uint32_t scale;
} sio_number;

static bool sio_is_digit(uint8_t c) { return c >= '0' && c <= '9'; }

// v * 10 + d, false on overflow (the value is held as a magnitude below 2^63; the sign comes last).
static bool sio_push_digit(uint64_t *v, uint8_t d) {
    if (*v > (UINT64_MAX - d) / 10u)
        return false;
    *v = *v * 10u + d;
    return true;
}

// Days since 1970-01-01 of a civil date (the proleptic Gregorian calendar), and back (RFC-0048).
static int64_t sio_days_from_civil(int64_t y, unsigned m, unsigned d) {
    y -= m <= 2;
    const int64_t  era = (y >= 0 ? y : y - 399) / 400;
    const unsigned yoe = (unsigned)(y - era * 400);
    const unsigned doy = (153 * (m > 2 ? m - 3 : m + 9) + 2) / 5 + d - 1;
    const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + (int64_t)doe - 719468;
}
static void sio_civil_from_days(int64_t z, int64_t *y, unsigned *m, unsigned *d) {
    z += 719468;
    const int64_t  era = (z >= 0 ? z : z - 146096) / 146097;
    const unsigned doe = (unsigned)(z - era * 146097);
    const unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    const unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    const unsigned mp = (5 * doy + 2) / 153;
    *d = doy - (153 * mp + 2) / 5 + 1;
    *m = mp < 10 ? mp + 3 : mp - 9;
    *y = (int64_t)yoe + era * 400 + (*m <= 2);
}
static bool sio_two(const uint8_t *s, uint32_t at, unsigned lo, unsigned hi, unsigned *v) {
    if (!sio_is_digit(s[at]) || !sio_is_digit(s[at + 1]))
        return false;
    *v = (unsigned)(s[at] - '0') * 10u + (unsigned)(s[at + 1] - '0');
    return *v >= lo && *v <= hi;
}
// `YYYY-MM-DD`, or with `T` or a blank and `hh:mm:ss`, and perhaps a `Z`: Unix seconds, UTC. False when the field is not one.
static bool sio_csv_date(const uint8_t *s, uint32_t n, int64_t *secs) {
    if (n != 10 && n != 19 && n != 20)
        return false;
    unsigned mo, d, h = 0, mi = 0, se = 0, a, b;
    if (!sio_two(s, 0, 0, 99, &a) || !sio_two(s, 2, 0, 99, &b) || s[4] != '-' || !sio_two(s, 5, 1, 12, &mo) || s[7] != '-' ||
        !sio_two(s, 8, 1, 31, &d))
        return false;
    const int64_t y = a * 100 + b;
    static const uint8_t days[12] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    const bool leap = (y % 4 == 0 && y % 100 != 0) || y % 400 == 0;
    if (d > days[mo - 1] + (mo == 2 && leap ? 1u : 0u))
        return false;
    if (n > 10) {
        if ((s[10] != 'T' && s[10] != ' ') || s[13] != ':' || s[16] != ':' || !sio_two(s, 11, 0, 23, &h) || !sio_two(s, 14, 0, 59, &mi) ||
            !sio_two(s, 17, 0, 59, &se) || (n == 20 && s[19] != 'Z'))
            return false;
    }
    *secs = sio_days_from_civil(y, mo, d) * 86400 + h * 3600 + mi * 60 + se;
    return true;
}
// The text of a date for save_csv: `YYYY-MM-DD`, with `Thh:mm:ss` when it is not midnight. 0 when the year is not 0 to 9999.
static uint32_t sio_out_date(uint8_t *out, int64_t secs) {
    int64_t        days = secs / 86400, rem = secs % 86400;
    if (rem < 0) {
        rem += 86400;
        --days;
    }
    int64_t  y;
    unsigned m, d;
    sio_civil_from_days(days, &y, &m, &d);
    if (y < 0 || y > 9999)
        return 0;
    uint32_t k = 0;
    out[k++] = (uint8_t)('0' + y / 1000 % 10);
    out[k++] = (uint8_t)('0' + y / 100 % 10);
    out[k++] = (uint8_t)('0' + y / 10 % 10);
    out[k++] = (uint8_t)('0' + y % 10);
    out[k++] = '-';
    out[k++] = (uint8_t)('0' + m / 10);
    out[k++] = (uint8_t)('0' + m % 10);
    out[k++] = '-';
    out[k++] = (uint8_t)('0' + d / 10);
    out[k++] = (uint8_t)('0' + d % 10);
    if (rem) {
        const unsigned h = (unsigned)(rem / 3600), mi = (unsigned)(rem / 60 % 60), se = (unsigned)(rem % 60);
        out[k++] = 'T';
        out[k++] = (uint8_t)('0' + h / 10);
        out[k++] = (uint8_t)('0' + h % 10);
        out[k++] = ':';
        out[k++] = (uint8_t)('0' + mi / 10);
        out[k++] = (uint8_t)('0' + mi % 10);
        out[k++] = ':';
        out[k++] = (uint8_t)('0' + se / 10);
        out[k++] = (uint8_t)('0' + se % 10);
    }
    return k;
}

// The CSV rule: a canonical integer, or a canonical integer with 1 to 18 digits after the point.
static sio_number sio_csv_number(const uint8_t *s, uint32_t n) {
    sio_number   r = {0, 0, 0};
    uint32_t k = 0;
    const bool neg = n > 0 && s[0] == '-';
    if (neg)
        ++k;
    const uint32_t ib = k;
    while (k < n && sio_is_digit(s[k]))
        ++k;
    const uint32_t il = k - ib;
    if (il == 0 || (il > 1 && s[ib] == '0'))
        return r;
    uint32_t fl = 0;
    if (k < n) {
        if (s[k] != '.')
            return r;
        ++k;
        const uint32_t fb = k;
        while (k < n && sio_is_digit(s[k]))
            ++k;
        fl = k - fb;
        if (fl == 0 || fl > sio_DEC_MAX_SCALE || k != n)
            return r;
    }
    uint64_t m = 0;
    for (uint32_t j = ib; j < n; ++j) {
        if (s[j] == '.')
            continue;
        if (!sio_push_digit(&m, (uint8_t)(s[j] - '0')))
            return r;
    }
    if (m > (uint64_t)INT64_MAX + (neg ? 1u : 0u) || (neg && m == 0 && fl == 0))
        return r; // "-0" stays text
    r.v = neg ? (int64_t)(0u - m) : (int64_t)m;
    r.kind = fl ? 2 : 1;
    r.scale = fl;
    return r;
}

// A number as a spreadsheet writes it (digits, a point, an exponent): trailing zeros of the fraction go, digits beyond 18
// after the point are rounded, and a value that does not fit loses its last digits; kind 0 when even the integer part does not fit.
static sio_number sio_xlsx_number(const uint8_t *s, uint32_t n) {
    sio_number   r = {0, 0, 0};
    uint32_t k = 0;
    const bool neg = n > 0 && s[0] == '-';
    if (n > 0 && (s[0] == '-' || s[0] == '+'))
        ++k;
    uint8_t  digits[64];
    uint32_t nd = 0, scale_in = 0;
    bool     point = false, any = false, cut = false;
    int64_t  scale = 0;
    for (; k < n && (sio_is_digit(s[k]) || (s[k] == '.' && !point)); ++k) {
        if (s[k] == '.') {
            point = true;
            continue;
        }
        any = true;
        if (nd == 0 && s[k] == '0' && !point)
            continue; // leading zeros of the integer part
        if (nd < sizeof digits) {
            digits[nd++] = s[k];
            if (point)
                ++scale_in;
        } else {
            cut = true; // more digits than a number of this kind holds: dropped (they are below 1e-40 or above 2^63)
            if (!point)
                return r;
        }
    }
    (void)cut;
    if (!any)
        return r;
    scale = scale_in;
    if (k < n && (s[k] == 'e' || s[k] == 'E')) {
        ++k;
        bool eneg = false;
        if (k < n && (s[k] == '+' || s[k] == '-'))
            eneg = s[k++] == '-';
        int64_t e = 0;
        if (k >= n || !sio_is_digit(s[k]))
            return r;
        for (; k < n && sio_is_digit(s[k]); ++k) {
            e = e * 10 + (s[k] - '0');
            if (e > 400)
                return r;
        }
        scale += eneg ? e : -e;
    }
    if (k != n)
        return r;
    // digits[0..nd) is an integer; the value is it times 10^-scale
    while (scale < 0) {
        if (nd + 1 > sizeof digits)
            return r;
        digits[nd++] = '0';
        ++scale;
    }
    while (scale > 0 && nd > 0 && digits[nd - 1] == '0') { // no trailing zeros
        --nd;
        --scale;
    }
    if (nd == 0) { // zero
        r.kind = 1;
        return r;
    }
    // more than 18 digits after the point: round at the 18th
    while (scale > (int64_t)sio_DEC_MAX_SCALE || nd > 19) {
        if (scale <= 0)
            return r;
        const bool up = digits[nd - 1] >= '5';
        --nd;
        --scale;
        for (uint32_t j = nd; up && j-- > 0;) {
            if (digits[j] == '9')
                digits[j] = '0';
            else {
                ++digits[j];
                break;
            }
            if (j == 0) { // carried out of the first digit
                if (nd + 1 > sizeof digits)
                    return r;
                __builtin_memmove(digits + 1, digits, nd);
                digits[0] = '1';
                ++nd;
            }
        }
        while (scale > 0 && nd > 0 && digits[nd - 1] == '0') {
            --nd;
            --scale;
        }
        if (nd == 0) {
            r.kind = 1;
            return r;
        }
    }
    for (;;) { // drop digits until the value fits
        uint64_t m = 0;
        bool     ok = true;
        for (uint32_t j = 0; j < nd && ok; ++j)
            ok = sio_push_digit(&m, (uint8_t)(digits[j] - '0'));
        if (ok && m <= (uint64_t)INT64_MAX + (neg ? 1u : 0u)) {
            r.v = neg ? (int64_t)(0u - m) : (int64_t)m;
            r.kind = scale ? 2 : 1;
            r.scale = (uint32_t)scale;
            return r;
        }
        if (scale <= 0)
            return r;
        --nd;
        --scale;
        while (scale > 0 && nd > 0 && digits[nd - 1] == '0') {
            --nd;
            --scale;
        }
    }
}

// ---- putting cells ---------------------------------------------------------------------------

typedef struct {
    uint8_t *b;
    uint32_t cap;
    uint32_t base;  // rows the sheet had before the load
    uint32_t cols;  // columns of the sheet
    uint32_t rows;  // rows now
    uint64_t charge;
} sio_sink_t;

static pnx_sheet_result sio_put_number(sio_sink_t *k, sio_number n, int64_t r, int64_t c) {
    return n.kind == 1 ? pnx_sheet_put_int(k->b, k->cap, n.v, r, c) : pnx_sheet_put_dec(k->b, k->cap, n.v, n.scale, r, c);
}

// A row `row` (0-based in the sheet) exists after this.
static pnx_sheet_result sio_need_row(sio_sink_t *k, uint64_t row) {
    while (k->rows <= row) {
        uint32_t made;
        const pnx_sheet_result e = pnx_sheet_add_row(k->b, k->cap, &made);
        if (e)
            return e;
        ++k->rows;
        ++k->charge;
    }
    return PNX_SH_OK;
}

// ---- CSV in ----------------------------------------------------------------------------------

pnx_sheet_result pnx_sheet_load_csv(uint8_t *b, uint32_t cap, uint8_t *src, uint64_t n, int mode, int64_t *added, uint64_t *charge) {
    if (mode < 0 || mode > 2)
        return PNX_SH_RANGE;
    sio_sink_t k = {b, cap, pnx_sheet_rows(b, cap), 0, 0, 0};
    uint64_t cols_now = pnx_sheet_cols(b, cap);
    k.cols = (uint32_t)cols_now;
    k.rows = k.base;
    int64_t dim;
    pnx_sheet_result e = pnx_sheet_dim(b, cap, 0, &dim); // STATE when there is no current sheet
    if (e)
        return e;
    uint64_t at = 0;
    if (n >= 3 && src[0] == 0xef && src[1] == 0xbb && src[2] == 0xbf)
        at = 3;
    uint64_t row = k.base;
    while (at < n && e == PNX_SH_OK) {
        if (src[at] == '\n' || (src[at] == '\r' && at + 1 < n && src[at + 1] == '\n')) { // a blank line
            at += src[at] == '\n' ? 1 : 2;
            continue;
        }
        if (src[at] == '\r' && at + 1 == n)
            break;
        e = sio_need_row(&k, row);
        for (uint64_t col = 0; e == PNX_SH_OK;) {
            uint8_t *field = src + at;
            uint32_t flen = 0;
            bool     quoted = false, end = false;
            if (at < n && src[at] == '"') { // quoted: the quotes doubled
                quoted = true;
                uint64_t w = at, j = at + 1;
                bool closed = false;
                while (j < n) {
                    if (src[j] == '"') {
                        if (j + 1 < n && src[j + 1] == '"') {
                            src[w++] = '"';
                            j += 2;
                            continue;
                        }
                        closed = true;
                        ++j;
                        break;
                    }
                    src[w++] = src[j++];
                }
                if (!closed) {
                    e = PNX_SH_KIND;
                    break;
                }
                while (j < n && src[j] != ',' && src[j] != '\n') { // anything between the closing quote and the comma is kept
                    if (src[j] != '\r' || (j + 1 < n && src[j + 1] != '\n'))
                        src[w++] = src[j];
                    ++j;
                }
                flen = (uint32_t)(w - at);
                at = j;
            } else {
                uint64_t j = at;
                while (j < n && src[j] != ',' && src[j] != '\n')
                    ++j;
                uint64_t stop = j;
                if (stop > at && src[stop - 1] == '\r')
                    --stop;
                flen = (uint32_t)(stop - at);
                at = j;
            }
            if (at >= n || src[at] == '\n')
                end = true;
            if (col >= k.cols) {
                e = PNX_SH_FULL;
                break;
            }
            if (flen > 0 || quoted) {
                sio_number num = {0, 0, 0};
                int64_t    when;
                if (mode != 1)
                    num = sio_csv_number(field, flen);
                if (mode == 2 && !num.kind && sio_csv_date(field, flen, &when))
                    e = pnx_sheet_put_date(b, cap, when, (int64_t)row, (int64_t)col);
                else if (num.kind)
                    e = sio_put_number(&k, num, (int64_t)row, (int64_t)col);
                else if (flen > 0)
                    e = pnx_sheet_put_str(b, cap, field, flen, (int64_t)row, (int64_t)col);
            }
            k.charge += flen + 1;
            if (end) {
                if (at < n)
                    ++at;
                break;
            }
            ++at; // the comma
            ++col;
        }
        ++row;
    }
    *charge += k.charge + n;
    if (e) {
        (void)pnx_sheet_truncate_rows(b, cap, k.base);
        return e;
    }
    *added = (int64_t)k.rows - k.base;
    return PNX_SH_OK;
}

// ---- inflate ---------------------------------------------------------------------------------

#define sio_WIN 32768u

typedef struct {
    uint16_t count[16];
    uint16_t symbol[288];
} sio_huff;

typedef int (*sio_feed_fn)(void *ctx, const uint8_t *p, uint32_t n); // nonzero stops

typedef struct {
    const uint8_t *in;
    uint64_t       inlen, inpos;
    uint32_t       bitbuf;
    int            bitcnt;
    uint8_t       *win;
    uint64_t       wpos, fpos, limit;
    sio_feed_fn        feed;
    void          *ctx;
    sio_huff           lencode, distcode;
    int16_t        lengths[320];
} sio_inf;

enum { sio_INF_OK = 0, sio_INF_BAD = 1, sio_INF_STOP = 2 };

static int sio_inf_bits(sio_inf *s, int need) {
    uint32_t val = s->bitbuf;
    while (s->bitcnt < need) {
        if (s->inpos >= s->inlen)
            return -1;
        val |= (uint32_t)s->in[s->inpos++] << s->bitcnt;
        s->bitcnt += 8;
    }
    s->bitbuf = val >> need;
    s->bitcnt -= need;
    return (int)(val & ((1u << need) - 1u));
}

static int sio_inf_flush(sio_inf *s, bool all) {
    const uint64_t n = s->wpos - s->fpos;
    if (n == 0 || (!all && n < sio_WIN))
        return sio_INF_OK;
    const int r = s->feed(s->ctx, s->win + (s->fpos & (sio_WIN - 1)), (uint32_t)n);
    s->fpos = s->wpos;
    return r ? sio_INF_STOP : sio_INF_OK;
}

static int sio_inf_put(sio_inf *s, uint8_t c) {
    if (s->wpos >= s->limit)
        return sio_INF_BAD;
    s->win[s->wpos & (sio_WIN - 1)] = c;
    ++s->wpos;
    return (s->wpos & (sio_WIN - 1)) == 0 ? sio_inf_flush(s, false) : sio_INF_OK;
}

static int sio_inf_decode(sio_inf *s, const sio_huff *h) {
    int code = 0, first = 0, index = 0;
    for (int len = 1; len <= 15; ++len) {
        const int b = sio_inf_bits(s, 1);
        if (b < 0)
            return -1;
        code |= b;
        const int count = h->count[len];
        if (code - count < first)
            return h->symbol[index + (code - first)];
        index += count;
        first += count;
        first <<= 1;
        code <<= 1;
    }
    return -2;
}

static int sio_inf_construct(sio_huff *h, const int16_t *length, int n) {
    int16_t offs[16];
    for (int len = 0; len <= 15; ++len)
        h->count[len] = 0;
    for (int symbol = 0; symbol < n; ++symbol)
        ++h->count[length[symbol]];
    if (h->count[0] == n)
        return 0;
    int left = 1;
    for (int len = 1; len <= 15; ++len) {
        left <<= 1;
        left -= h->count[len];
        if (left < 0)
            return left;
    }
    offs[1] = 0;
    for (int len = 1; len < 15; ++len)
        offs[len + 1] = (int16_t)(offs[len] + h->count[len]);
    for (int symbol = 0; symbol < n; ++symbol) {
        if (length[symbol] != 0)
            h->symbol[offs[length[symbol]]++] = (uint16_t)symbol;
    }
    return left;
}

static int sio_inf_codes(sio_inf *s, const sio_huff *lc, const sio_huff *dc) {
    static const int16_t lens[29] = {3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27, 31, 35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258};
    static const int16_t lext[29] = {0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0};
    static const int16_t dists[30] = {1, 2, 3, 4, 5, 7, 9, 13, 17, 25, 33, 49, 65, 97, 129, 193, 257, 385, 513, 769, 1025, 1537, 2049, 3073, 4097, 6145, 8193, 12289, 16385, 24577};
    static const int16_t dext[30] = {0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6, 7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13};
    for (;;) {
        int symbol = sio_inf_decode(s, lc);
        if (symbol < 0)
            return sio_INF_BAD;
        if (symbol < 256) {
            const int r = sio_inf_put(s, (uint8_t)symbol);
            if (r)
                return r;
        } else if (symbol == 256) {
            return sio_INF_OK;
        } else {
            symbol -= 257;
            if (symbol >= 29)
                return sio_INF_BAD;
            int extra = sio_inf_bits(s, lext[symbol]);
            if (extra < 0)
                return sio_INF_BAD;
            const int len = lens[symbol] + extra;
            const int ds = sio_inf_decode(s, dc);
            if (ds < 0 || ds >= 30)
                return sio_INF_BAD;
            extra = sio_inf_bits(s, dext[ds]);
            if (extra < 0)
                return sio_INF_BAD;
            const uint64_t dist = (uint64_t)dists[ds] + (uint64_t)extra;
            if (dist > s->wpos || dist > sio_WIN)
                return sio_INF_BAD;
            for (int j = 0; j < len; ++j) {
                const int r = sio_inf_put(s, s->win[(s->wpos - dist) & (sio_WIN - 1)]);
                if (r)
                    return r;
            }
        }
    }
}

static int sio_inf_dynamic(sio_inf *s) {
    static const uint8_t order[19] = {16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15};
    int nlen = sio_inf_bits(s, 5), ndist = sio_inf_bits(s, 5), ncode = sio_inf_bits(s, 4);
    if (nlen < 0 || ndist < 0 || ncode < 0)
        return sio_INF_BAD;
    nlen += 257;
    ndist += 1;
    ncode += 4;
    if (nlen > 286 || ndist > 30)
        return sio_INF_BAD;
    int index = 0;
    for (; index < ncode; ++index) {
        const int v = sio_inf_bits(s, 3);
        if (v < 0)
            return sio_INF_BAD;
        s->lengths[order[index]] = (int16_t)v;
    }
    for (; index < 19; ++index)
        s->lengths[order[index]] = 0;
    if (sio_inf_construct(&s->lencode, s->lengths, 19) != 0)
        return sio_INF_BAD;
    index = 0;
    while (index < nlen + ndist) {
        int symbol = sio_inf_decode(s, &s->lencode);
        if (symbol < 0)
            return sio_INF_BAD;
        if (symbol < 16) {
            s->lengths[index++] = (int16_t)symbol;
        } else {
            int len = 0, rep;
            if (symbol == 16) {
                if (index == 0)
                    return sio_INF_BAD;
                len = s->lengths[index - 1];
                rep = sio_inf_bits(s, 2);
                if (rep < 0)
                    return sio_INF_BAD;
                rep += 3;
            } else if (symbol == 17) {
                rep = sio_inf_bits(s, 3);
                if (rep < 0)
                    return sio_INF_BAD;
                rep += 3;
            } else {
                rep = sio_inf_bits(s, 7);
                if (rep < 0)
                    return sio_INF_BAD;
                rep += 11;
            }
            if (index + rep > nlen + ndist)
                return sio_INF_BAD;
            while (rep--)
                s->lengths[index++] = (int16_t)len;
        }
    }
    if (s->lengths[256] == 0)
        return sio_INF_BAD;
    int err = sio_inf_construct(&s->lencode, s->lengths, nlen);
    if (err < 0 || (err > 0 && nlen - s->lencode.count[0] != 1))
        return sio_INF_BAD;
    err = sio_inf_construct(&s->distcode, s->lengths + nlen, ndist);
    if (err < 0 || (err > 0 && ndist - s->distcode.count[0] != 1))
        return sio_INF_BAD;
    return sio_inf_codes(s, &s->lencode, &s->distcode);
}

static int sio_inf_fixed(sio_inf *s) {
    int symbol = 0;
    for (; symbol < 144; ++symbol)
        s->lengths[symbol] = 8;
    for (; symbol < 256; ++symbol)
        s->lengths[symbol] = 9;
    for (; symbol < 280; ++symbol)
        s->lengths[symbol] = 7;
    for (; symbol < 288; ++symbol)
        s->lengths[symbol] = 8;
    (void)sio_inf_construct(&s->lencode, s->lengths, 288);
    for (symbol = 0; symbol < 30; ++symbol)
        s->lengths[symbol] = 5;
    (void)sio_inf_construct(&s->distcode, s->lengths, 30);
    return sio_inf_codes(s, &s->lencode, &s->distcode);
}

// Inflates `in` and hands the bytes to `feed`; `*produced` is their number. `limit` bounds them.
static int sio_inflate_run(sio_inf *s, const uint8_t *in, uint64_t n, uint64_t limit, sio_feed_fn feed, void *ctx, uint64_t *produced) {
    s->in = in;
    s->inlen = n;
    s->inpos = 0;
    s->bitbuf = 0;
    s->bitcnt = 0;
    s->wpos = s->fpos = 0;
    s->limit = limit;
    s->feed = feed;
    s->ctx = ctx;
    int last, r = sio_INF_OK;
    do {
        last = sio_inf_bits(s, 1);
        const int type = sio_inf_bits(s, 2);
        if (last < 0 || type < 0)
            return sio_INF_BAD;
        if (type == 0) {
            s->bitbuf = 0;
            s->bitcnt = 0;
            if (s->inpos + 4 > s->inlen)
                return sio_INF_BAD;
            const uint32_t len = s->in[s->inpos] | (uint32_t)s->in[s->inpos + 1] << 8;
            const uint32_t nlen = s->in[s->inpos + 2] | (uint32_t)s->in[s->inpos + 3] << 8;
            s->inpos += 4;
            if (len != (~nlen & 0xffffu) || s->inpos + len > s->inlen)
                return sio_INF_BAD;
            for (uint32_t j = 0; j < len && r == sio_INF_OK; ++j)
                r = sio_inf_put(s, s->in[s->inpos + j]);
            s->inpos += len;
        } else if (type == 1) {
            r = sio_inf_fixed(s);
        } else if (type == 2) {
            r = sio_inf_dynamic(s);
        } else {
            return sio_INF_BAD;
        }
        if (r != sio_INF_OK)
            return r;
    } while (!last);
    *produced = s->wpos;
    return sio_inf_flush(s, true);
}

// ---- ZIP -------------------------------------------------------------------------------------

static uint32_t sio_rd16(const uint8_t *p) { return p[0] | (uint32_t)p[1] << 8; }
static uint32_t sio_rd32(const uint8_t *p) { return sio_rd16(p) | (uint32_t)sio_rd16(p + 2) << 16; }

typedef struct {
    const uint8_t *file;
    uint64_t       size;
    uint64_t       cd;      // the central directory
    uint32_t       entries;
} sio_zip_t;

static bool sio_zip_open(sio_zip_t *z, const uint8_t *file, uint64_t size) {
    if (size < 22)
        return false;
    uint64_t from = size - 22, low = size > 22 + 65535 ? size - 22 - 65535 : 0, at = from + 1;
    while (at-- > low) {
        if (sio_rd32(file + at) == 0x06054b50u)
            break;
        if (at == low)
            return false;
    }
    if (sio_rd32(file + at) != 0x06054b50u)
        return false;
    const uint32_t entries = sio_rd16(file + at + 10), cdsize = sio_rd32(file + at + 12), cdoff = sio_rd32(file + at + 16);
    if (sio_rd16(file + at + 4) != 0 || sio_rd16(file + at + 6) != 0 || entries == 0xffffu || cdoff == 0xffffffffu ||
        (uint64_t)cdoff + cdsize > size)
        return false;
    z->file = file;
    z->size = size;
    z->cd = cdoff;
    z->entries = entries;
    return true;
}

typedef struct {
    uint32_t       method, csize, usize;
    const uint8_t *data;
} sio_zip_entry;

// 0 found, 1 not there, 2 broken or not supported
static int sio_zip_find(const sio_zip_t *z, const char *name, sio_zip_entry *out) {
    const uint32_t nlen = (uint32_t)__builtin_strlen(name);
    uint64_t       p = z->cd;
    for (uint32_t k = 0; k < z->entries; ++k) {
        if (p + 46 > z->size || sio_rd32(z->file + p) != 0x02014b50u)
            return 2;
        const uint32_t flags = sio_rd16(z->file + p + 8), method = sio_rd16(z->file + p + 10), csize = sio_rd32(z->file + p + 20),
                       usize = sio_rd32(z->file + p + 24), fl = sio_rd16(z->file + p + 28), el = sio_rd16(z->file + p + 30),
                       cl = sio_rd16(z->file + p + 32), lho = sio_rd32(z->file + p + 42);
        if (p + 46 + fl + el + cl > z->size)
            return 2;
        if (fl == nlen && __builtin_memcmp(z->file + p + 46, name, nlen) == 0) {
            if ((flags & 1u) || (method != 0 && method != 8) || csize == 0xffffffffu || usize == 0xffffffffu ||
                (uint64_t)lho + 30 > z->size || sio_rd32(z->file + lho) != 0x04034b50u)
                return 2;
            const uint64_t start = (uint64_t)lho + 30 + sio_rd16(z->file + lho + 26) + sio_rd16(z->file + lho + 28);
            if (start + csize > z->size)
                return 2;
            *out = (sio_zip_entry){method, csize, usize, z->file + start};
            return 0;
        }
        p += 46u + fl + el + cl;
    }
    return 1;
}

#define sio_ENTRY_LIMIT (256ull << 20)

// Hands the contents of an entry to `feed`.
static int sio_zip_read(sio_inf *s, const sio_zip_entry *e, sio_feed_fn feed, void *ctx, uint64_t *charge) {
    if (e->usize > sio_ENTRY_LIMIT)
        return sio_INF_BAD;
    uint64_t made = 0;
    *charge += e->usize;
    if (e->method == 0) {
        if (e->csize != e->usize)
            return sio_INF_BAD;
        for (uint32_t at = 0; at < e->csize;) {
            const uint32_t n = e->csize - at < 4096 ? e->csize - at : 4096;
            if (feed(ctx, e->data + at, n))
                return sio_INF_STOP;
            at += n;
        }
        return sio_INF_OK;
    }
    const int r = sio_inflate_run(s, e->data, e->csize, e->usize, feed, ctx, &made);
    if (r == sio_INF_OK && made != e->usize)
        return sio_INF_BAD;
    return r;
}

// ---- XML -------------------------------------------------------------------------------------

#define sio_TAG_MAX 1024u
#define sio_TEXT_MAX 65536u

typedef struct sio_xml sio_xml;
struct sio_xml {
    int      state;       // 0 text, 1 tag, 2 comment/declaration
    uint32_t tn;          // bytes of the tag so far
    uint8_t  quote;
    bool     capture;     // text between tags is kept
    uint32_t tlen;        // bytes kept
    uint32_t seg;         // where the segment being captured began
    int      fail;        // 0, or a pnx_sheet_result + 1 that stops the scan
    uint8_t *tag;
    uint8_t *text;
    void    *ctx;
    void   (*open)(sio_xml *x, const uint8_t *name, uint32_t nlen, const uint8_t *attrs, uint32_t alen, bool selfclose);
    void   (*close)(sio_xml *x, const uint8_t *name, uint32_t nlen);
};

static bool sio_xml_attr(const uint8_t *a, uint32_t n, const char *name, const uint8_t **v, uint32_t *vn) {
    const uint32_t nl = (uint32_t)__builtin_strlen(name);
    uint32_t       k = 0;
    while (k < n) {
        while (k < n && (a[k] == ' ' || a[k] == '\t' || a[k] == '\n' || a[k] == '\r' || a[k] == '/'))
            ++k;
        const uint32_t nb = k;
        while (k < n && a[k] != '=' && a[k] != ' ' && a[k] != '\t' && a[k] != '\n' && a[k] != '\r')
            ++k;
        const uint32_t ne = k;
        while (k < n && a[k] != '"' && a[k] != '\'')
            ++k;
        if (k >= n)
            return false;
        const uint8_t q = a[k++];
        const uint32_t vb = k;
        while (k < n && a[k] != q)
            ++k;
        if (k >= n)
            return false;
        if (ne - nb == nl && __builtin_memcmp(a + nb, name, nl) == 0) {
            *v = a + vb;
            *vn = k - vb;
            return true;
        }
        ++k;
    }
    return false;
}

static uint32_t sio_utf8(uint32_t cp, uint8_t *out) {
    if (cp < 0x80) {
        out[0] = (uint8_t)cp;
        return 1;
    }
    if (cp < 0x800) {
        out[0] = (uint8_t)(0xc0 | cp >> 6);
        out[1] = (uint8_t)(0x80 | (cp & 0x3f));
        return 2;
    }
    if (cp < 0x10000) {
        out[0] = (uint8_t)(0xe0 | cp >> 12);
        out[1] = (uint8_t)(0x80 | ((cp >> 6) & 0x3f));
        out[2] = (uint8_t)(0x80 | (cp & 0x3f));
        return 3;
    }
    out[0] = (uint8_t)(0xf0 | cp >> 18);
    out[1] = (uint8_t)(0x80 | ((cp >> 12) & 0x3f));
    out[2] = (uint8_t)(0x80 | ((cp >> 6) & 0x3f));
    out[3] = (uint8_t)(0x80 | (cp & 0x3f));
    return 4;
}
static int sio_hexval(uint8_t c) { return sio_is_digit(c) ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1; }

// The entities and the _xHHHH_ escapes of a text, in place; the new length. Every replacement is not longer than what it replaces.
static uint32_t sio_xml_unescape(uint8_t *p, uint32_t n) {
    uint32_t w = 0;
    for (uint32_t k = 0; k < n;) {
        if (p[k] == '&') {
            uint32_t j = k + 1;
            while (j < n && j - k < 12 && p[j] != ';')
                ++j;
            if (j < n && p[j] == ';') {
                const uint32_t el = j - k - 1;
                const uint8_t *e = p + k + 1;
                uint32_t       cp = UINT32_MAX;
                if (el == 2 && e[0] == 'l' && e[1] == 't')
                    cp = '<';
                else if (el == 2 && e[0] == 'g' && e[1] == 't')
                    cp = '>';
                else if (el == 3 && __builtin_memcmp(e, "amp", 3) == 0)
                    cp = '&';
                else if (el == 4 && __builtin_memcmp(e, "quot", 4) == 0)
                    cp = '"';
                else if (el == 4 && __builtin_memcmp(e, "apos", 4) == 0)
                    cp = '\'';
                else if (el >= 2 && e[0] == '#') {
                    uint32_t v = 0;
                    bool     ok = true;
                    if (e[1] == 'x' || e[1] == 'X') {
                        ok = el >= 3 && el <= 8;
                        for (uint32_t q = 2; q < el && ok; ++q)
                            ok = sio_hexval(e[q]) >= 0, v = v << 4 | (uint32_t)(ok ? sio_hexval(e[q]) : 0);
                    } else {
                        ok = el <= 8;
                        for (uint32_t q = 1; q < el && ok; ++q)
                            ok = sio_is_digit(e[q]), v = v * 10 + (uint32_t)(e[q] - '0');
                    }
                    if (ok && v > 0 && v <= 0x10ffff && !(v >= 0xd800 && v < 0xe000))
                        cp = v;
                }
                if (cp != UINT32_MAX) {
                    w += sio_utf8(cp, p + w);
                    k = j + 1;
                    continue;
                }
            }
        } else if (p[k] == '_' && k + 6 < n && p[k + 1] == 'x' && p[k + 6] == '_') {
            uint32_t v = 0;
            bool     ok = true;
            for (uint32_t q = 2; q < 6 && ok; ++q)
                ok = sio_hexval(p[k + q]) >= 0, v = v << 4 | (uint32_t)(ok ? sio_hexval(p[k + q]) : 0);
            if (ok && v > 0 && !(v >= 0xd800 && v < 0xe000)) {
                w += sio_utf8(v, p + w);
                k += 7;
                continue;
            }
        }
        p[w++] = p[k++];
    }
    return w;
}

static void sio_xml_tag_done(sio_xml *x) {
    const uint8_t *t = x->tag;
    uint32_t       n = x->tn;
    if (n == 0 || t[0] == '?' || t[0] == '!')
        return;
    if (t[0] == '/') {
        uint32_t e = 1;
        while (e < n && t[e] != ' ' && t[e] != '\t' && t[e] != '\n' && t[e] != '\r')
            ++e;
        x->close(x, t + 1, e - 1);
        return;
    }
    const bool selfclose = t[n - 1] == '/';
    uint32_t   e = 0;
    while (e < n && t[e] != ' ' && t[e] != '\t' && t[e] != '\n' && t[e] != '\r' && t[e] != '/')
        ++e;
    x->open(x, t, e, t + e, n - e, selfclose);
    if (selfclose)
        x->close(x, t, e);
}

static int sio_xml_feed(void *ctx, const uint8_t *p, uint32_t n) {
    sio_xml *x = ctx;
    for (uint32_t k = 0; k < n; ++k) {
        const uint8_t c = p[k];
        if (x->state == 0) {
            if (c == '<') {
                x->state = 1;
                x->tn = 0;
                x->quote = 0;
            } else if (x->capture) {
                if (x->tlen >= sio_TEXT_MAX) {
                    x->fail = PNX_SH_FULL + 1;
                    return 1;
                }
                x->text[x->tlen++] = c;
            }
        } else {
            if (x->tn < sio_TAG_MAX) {
                x->tag[x->tn++] = c;
            } else {
                x->fail = PNX_SH_KIND + 1;
                return 1;
            }
            if (x->tn == 3 && __builtin_memcmp(x->tag, "!--", 3) == 0) {
                x->state = 2;
            } else if (x->tn == 8 && __builtin_memcmp(x->tag, "![CDATA[", 8) == 0) {
                x->fail = PNX_SH_KIND + 1;
                return 1;
            } else if (x->state == 2) {
                if (c == '>' && x->tn >= 5 && x->tag[x->tn - 2] == '-' && x->tag[x->tn - 3] == '-') {
                    x->state = 0;
                }
            } else if (x->quote) {
                if (c == x->quote)
                    x->quote = 0;
            } else if (c == '"' || c == '\'') {
                x->quote = c;
            } else if (c == '>') {
                --x->tn;
                x->state = 0;
                sio_xml_tag_done(x);
                if (x->fail)
                    return 1;
            }
        }
    }
    return 0;
}

// Starts and ends a piece of captured text; the piece is unescaped when it ends.
static void sio_xml_begin(sio_xml *x) {
    x->capture = true;
    x->seg = x->tlen;
}
static void sio_xml_end(sio_xml *x) {
    x->capture = false;
    if (x->tlen < x->seg) // the text was cut by a tag that reset it (malformed): nothing of this piece is left
        x->seg = x->tlen;
    x->tlen = x->seg + sio_xml_unescape(x->text + x->seg, x->tlen - x->seg);
}

// ---- xlsx in ---------------------------------------------------------------------------------

enum { sio_PH_WORKBOOK, sio_PH_RELS, sio_PH_STYLES, sio_PH_SST, sio_PH_SHEET };

#define sio_MAX_FMT 96u
#define sio_MAX_XF 8192u

typedef struct {
    sio_sink_t   sink;
    int      phase;
    int      mode;
    int64_t  want;          // the sheet asked for
    int64_t  seen;          // sheets passed in workbook.xml
    char     rid[64];       // its relationship
    bool     have_rid;
    char     target[160];   // where the relationships say it is
    bool     date1904;
    // styles
    uint32_t fmt_id[sio_MAX_FMT];
    bool     fmt_date[sio_MAX_FMT];
    uint32_t nfmt;
    bool     in_xfs;
    uint32_t nxf;
    uint8_t  xf_date[sio_MAX_XF / 8];
    // shared strings
    uint8_t *strs;          // decoded strings, growing up
    uint32_t strs_used, strs_cap;
    uint32_t *table;        // (offset) of string k, growing down from the end: table[-1 - k]
    uint32_t nstr;
    uint32_t item_start;
    bool     in_si, in_rph, in_t;
    // sheet
    bool     in_is;
    int64_t  row, col;
    uint32_t ctype;         // 0 number, 1 shared, 2 string, 3 bool, 4 error, 5 inline, 6 iso date
    uint32_t cstyle;
    bool     have_v;
    int      err;
} sio_xlsx;

static bool sio_builtin_date(uint32_t id) {
    return (id >= 14 && id <= 22) || (id >= 27 && id <= 36) || (id >= 45 && id <= 47) || (id >= 50 && id <= 58) || (id >= 71 && id <= 81);
}

// A custom number format is a date or a time when it has y, d, h, s or m outside quotes, brackets and escapes.
static bool sio_format_is_date(const uint8_t *f, uint32_t n) {
    for (uint32_t k = 0; k < n; ++k) {
        const uint8_t c = f[k];
        if (c == '"') {
            for (++k; k < n && f[k] != '"'; ++k) {
            }
        } else if (c == '[') {
            uint32_t j = k + 1;
            while (j < n && f[j] != ']')
                ++j;
            if (j - k - 1 >= 1 && j - k - 1 <= 2 && (f[k + 1] == 'h' || f[k + 1] == 'H' || f[k + 1] == 'm' || f[k + 1] == 'M' || f[k + 1] == 's' || f[k + 1] == 'S'))
                return true; // elapsed time
            k = j;
        } else if (c == '\\' || c == '_' || c == '*') {
            ++k;
        } else if (c == 'y' || c == 'Y' || c == 'd' || c == 'D' || c == 'h' || c == 'H' || c == 's' || c == 'S' || c == 'm' || c == 'M') {
            return true;
        }
    }
    return false;
}

static bool sio_style_is_date(const sio_xlsx *a, uint32_t xf) { return xf < a->nxf && (a->xf_date[xf >> 3] >> (xf & 7)) & 1u; }

static bool sio_name_is(const uint8_t *n, uint32_t nl, const char *s) { return nl == __builtin_strlen(s) && __builtin_memcmp(n, s, nl) == 0; }

static bool sio_parse_uint(const uint8_t *v, uint32_t n, uint32_t *out) {
    if (n == 0 || n > 9)
        return false;
    uint32_t r = 0;
    for (uint32_t k = 0; k < n; ++k) {
        if (!sio_is_digit(v[k]))
            return false;
        r = r * 10 + (uint32_t)(v[k] - '0');
    }
    *out = r;
    return true;
}

// "B12": column 1, row 11 (0-based).
static bool sio_cell_ref(const uint8_t *v, uint32_t n, int64_t *col, int64_t *row) {
    uint32_t k = 0;
    int64_t  c = 0;
    while (k < n && ((v[k] >= 'A' && v[k] <= 'Z') || (v[k] >= 'a' && v[k] <= 'z'))) {
        c = c * 26 + ((v[k] & 0xdf) - 'A' + 1);
        if (c > 1000000)
            return false;
        ++k;
    }
    uint32_t r;
    if (k == 0 || !sio_parse_uint(v + k, n - k, &r) || r == 0)
        return false;
    *col = c - 1;
    *row = (int64_t)r - 1;
    return true;
}

static void sio_xlsx_open(sio_xml *x, const uint8_t *name, uint32_t nl, const uint8_t *at, uint32_t al, bool selfclose) {
    sio_xlsx *a = x->ctx;
    const uint8_t *v;
    uint32_t       vn;
    switch (a->phase) {
    case sio_PH_WORKBOOK:
        if (sio_name_is(name, nl, "workbookPr") && sio_xml_attr(at, al, "date1904", &v, &vn))
            a->date1904 = (vn == 1 && (v[0] == '1')) || (vn == 4 && __builtin_memcmp(v, "true", 4) == 0);
        else if (sio_name_is(name, nl, "sheet")) {
            if (a->seen == a->want && sio_xml_attr(at, al, "r:id", &v, &vn) && vn < sizeof a->rid) {
                __builtin_memcpy(a->rid, v, vn);
                a->rid[vn] = 0;
                a->have_rid = true;
            }
            ++a->seen;
        }
        break;
    case sio_PH_RELS:
        if (sio_name_is(name, nl, "Relationship") && a->have_rid && sio_xml_attr(at, al, "Id", &v, &vn) && vn == __builtin_strlen(a->rid) &&
            __builtin_memcmp(v, a->rid, vn) == 0 && sio_xml_attr(at, al, "Target", &v, &vn)) {
            const uint32_t pre = vn > 0 && v[0] == '/' ? 0u : 3u; // "xl/" before a relative one
            if (vn + pre >= sizeof a->target) {
                x->fail = PNX_SH_KIND + 1;
                return;
            }
            if (pre)
                __builtin_memcpy(a->target, "xl/", 3);
            __builtin_memcpy(a->target + pre, v + (pre ? 0 : 1), vn - (pre ? 0 : 1));
            a->target[pre + vn - (pre ? 0 : 1)] = 0;
        }
        break;
    case sio_PH_STYLES:
        if (sio_name_is(name, nl, "numFmt") && sio_xml_attr(at, al, "numFmtId", &v, &vn)) {
            uint32_t id;
            const uint8_t *f;
            uint32_t       fn;
            if (sio_parse_uint(v, vn, &id) && sio_xml_attr(at, al, "formatCode", &f, &fn) && a->nfmt < sio_MAX_FMT) {
                uint8_t code[160];
                if (fn > sizeof code)
                    fn = sizeof code;
                __builtin_memcpy(code, f, fn);
                fn = sio_xml_unescape(code, fn);
                a->fmt_id[a->nfmt] = id;
                a->fmt_date[a->nfmt++] = sio_format_is_date(code, fn);
            }
        } else if (sio_name_is(name, nl, "cellXfs")) {
            a->in_xfs = true;
        } else if (a->in_xfs && sio_name_is(name, nl, "xf")) {
            uint32_t id = 0;
            bool     date = false;
            if (sio_xml_attr(at, al, "numFmtId", &v, &vn) && sio_parse_uint(v, vn, &id)) {
                date = sio_builtin_date(id);
                for (uint32_t k = 0; k < a->nfmt; ++k) {
                    if (a->fmt_id[k] == id)
                        date = a->fmt_date[k];
                }
            }
            if (a->nxf < sio_MAX_XF) {
                if (date)
                    a->xf_date[a->nxf >> 3] |= (uint8_t)(1u << (a->nxf & 7));
                ++a->nxf;
            }
        }
        break;
    case sio_PH_SST:
        if (sio_name_is(name, nl, "si")) {
            a->in_si = true;
            a->item_start = a->strs_used;
            x->capture = false;
            x->tlen = 0;
        } else if (sio_name_is(name, nl, "rPh")) {
            a->in_rph = true;
        } else if (sio_name_is(name, nl, "t") && a->in_si && !a->in_rph && !selfclose) {
            sio_xml_begin(x);
        }
        break;
    default: // PH_SHEET
        if (sio_name_is(name, nl, "row")) {
            uint32_t r;
            if (sio_xml_attr(at, al, "r", &v, &vn) && sio_parse_uint(v, vn, &r) && r > 0)
                a->row = (int64_t)r - 1;
            else
                ++a->row;
            a->col = -1;
        } else if (sio_name_is(name, nl, "c")) {
            int64_t col, row;
            if (sio_xml_attr(at, al, "r", &v, &vn) && sio_cell_ref(v, vn, &col, &row)) {
                a->col = col;
                a->row = row;
            } else {
                ++a->col;
            }
            a->ctype = 0;
            a->cstyle = 0;
            a->have_v = false;
            a->in_is = false;
            x->capture = false;
            x->tlen = 0;
            if (sio_xml_attr(at, al, "t", &v, &vn)) {
                a->ctype = sio_name_is(v, vn, "s") ? 1 : sio_name_is(v, vn, "str") ? 2 : sio_name_is(v, vn, "b") ? 3 : sio_name_is(v, vn, "e") ? 4
                         : sio_name_is(v, vn, "inlineStr") ? 5 : sio_name_is(v, vn, "d") ? 6 : 0;
            }
            uint32_t s;
            if (sio_xml_attr(at, al, "s", &v, &vn) && sio_parse_uint(v, vn, &s))
                a->cstyle = s;
        } else if (sio_name_is(name, nl, "v") && !selfclose) {
            sio_xml_begin(x);
        } else if (sio_name_is(name, nl, "is")) {
            a->in_is = true;
        } else if (sio_name_is(name, nl, "rPh")) {
            a->in_rph = true;
        } else if (sio_name_is(name, nl, "t") && a->in_is && !a->in_rph && !selfclose) {
            sio_xml_begin(x);
        }
        break;
    }
}

// Unix seconds of a spreadsheet serial number (v / 10^scale days), in 64 bits: the fraction of a day is cut to 9 digits (86 microseconds).
static bool sio_serial_to_unix(const sio_xlsx *a, sio_number n, int64_t *out) {
    int64_t v = n.v;
    uint32_t scale = n.scale;
    while (scale > 9) { // one digit less, rounded half up: floor((v + 5) / 10)
        if (v > INT64_MAX - 5)
            return false;
        const int64_t t = v + 5;
        v = t / 10 - (t % 10 < 0 ? 1 : 0);
        --scale;
    }
    int64_t pow = 1;
    for (uint32_t k = 0; k < scale; ++k)
        pow *= 10;
    int64_t whole = v / pow, frac = v % pow;
    if (frac < 0) {
        frac += pow;
        --whole;
    }
    const int64_t fsecs = (frac * 86400 + pow / 2) / pow;
    if (whole < 1 && whole >= 0 && !a->date1904) { // a time of day only
        *out = fsecs;
        return true;
    }
    const int64_t off = a->date1904 ? 24107 : (whole < 61 ? 25568 : 25569);
    const int64_t days = whole - off;
    if (days > 100000000000ll || days < -100000000000ll)
        return false;
    *out = days * 86400 + fsecs;
    return true;
}

static void sio_xlsx_close(sio_xml *x, const uint8_t *name, uint32_t nl) {
    sio_xlsx *a = x->ctx;
    switch (a->phase) {
    case sio_PH_STYLES:
        if (sio_name_is(name, nl, "cellXfs"))
            a->in_xfs = false;
        break;
    case sio_PH_SST:
        if (sio_name_is(name, nl, "t") && x->capture) {
            sio_xml_end(x);
        } else if (sio_name_is(name, nl, "rPh")) {
            a->in_rph = false;
        } else if (sio_name_is(name, nl, "si") && a->in_si) {
            a->in_si = false;
            // the item: what the <t> pieces gave, joined; the table grows down from the end of the area
            const uint32_t need = x->tlen + 4;
            if ((uint64_t)a->strs_used + need > a->strs_cap - (uint64_t)a->nstr * 4) {
                x->fail = PNX_SH_FULL + 1;
                return;
            }
            __builtin_memcpy(a->strs + a->strs_used, x->text, x->tlen);
            a->table[-1 - (int64_t)a->nstr] = a->strs_used | 0; // offset; the length is the next offset minus this
            a->strs_used += x->tlen;
            ++a->nstr;
            a->item_start = a->strs_used;
            x->tlen = 0;
        }
        break;
    default:
        if (a->phase != sio_PH_SHEET)
            break;
        if (sio_name_is(name, nl, "v") && x->capture) {
            sio_xml_end(x);
            a->have_v = true;
        } else if (sio_name_is(name, nl, "t") && x->capture) {
            sio_xml_end(x);
            a->have_v = true;
        } else if (sio_name_is(name, nl, "rPh")) {
            a->in_rph = false;
        } else if (sio_name_is(name, nl, "is")) {
            a->in_is = false;
        } else if (sio_name_is(name, nl, "c")) {
            if (!a->have_v || x->tlen == 0 || a->col < 0)
                break;
            const int64_t row = (int64_t)a->sink.base + a->row;
            if (a->col >= (int64_t)a->sink.cols || row >= (int64_t)UINT32_MAX) {
                x->fail = PNX_SH_FULL + 1;
                break;
            }
            pnx_sheet_result e = sio_need_row(&a->sink, (uint64_t)row);
            const uint8_t   *t = x->text;
            const uint32_t   n = x->tlen;
            if (!e) {
                switch (a->ctype) {
                case 1: { // a shared string
                    uint32_t k;
                    if (!sio_parse_uint(t, n, &k) || k >= a->nstr) {
                        e = PNX_SH_KIND;
                        break;
                    }
                    const uint32_t off = a->table[-1 - (int64_t)k];
                    const uint32_t end = k + 1 < a->nstr ? a->table[-2 - (int64_t)k] : a->strs_used;
                    if (end > off)
                        e = pnx_sheet_put_str(a->sink.b, a->sink.cap, a->strs + off, end - off, row, a->col);
                    break;
                }
                case 2: case 4: case 5: case 6:
                    e = pnx_sheet_put_str(a->sink.b, a->sink.cap, t, n, row, a->col);
                    break;
                case 3:
                    e = pnx_sheet_put_bool(a->sink.b, a->sink.cap, n == 1 && t[0] == '1', row, a->col);
                    break;
                default: {
                    const sio_number num = sio_xlsx_number(t, n);
                    if (a->mode != 1 && num.kind && sio_style_is_date(a, a->cstyle)) {
                        int64_t secs;
                        if (sio_serial_to_unix(a, num, &secs)) {
                            e = a->mode == 2 ? pnx_sheet_put_date(a->sink.b, a->sink.cap, secs, row, a->col)
                                             : pnx_sheet_put_int(a->sink.b, a->sink.cap, secs, row, a->col);
                            break;
                        }
                    }
                    if (num.kind)
                        e = sio_put_number(&a->sink, num, row, a->col);
                    else
                        e = pnx_sheet_put_str(a->sink.b, a->sink.cap, t, n, row, a->col);
                    break;
                }
                }
            }
            a->sink.charge += n + 8;
            if (e)
                x->fail = (int)e + 1;
            x->tlen = 0;
        }
        break;
    }
}

typedef struct {
    sio_inf  *z;
    sio_xml   x;
    sio_xlsx *a;
} sio_xrun;

// One part of the file through the scanner. 0 ok, 1 missing, else a result + 2.
static int sio_xlsx_part(sio_xrun *r, const sio_zip_t *zip, const char *name, int phase, uint64_t *charge) {
    sio_zip_entry e;
    const int f = sio_zip_find(zip, name, &e);
    if (f)
        return f == 1 ? 1 : (int)PNX_SH_KIND + 2;
    r->a->phase = phase;
    r->x.state = 0;
    r->x.tn = 0;
    r->x.capture = false;
    r->x.tlen = 0;
    r->x.fail = 0;
    const int z = sio_zip_read(r->z, &e, sio_xml_feed, &r->x, charge);
    if (r->x.fail)
        return r->x.fail - 1 + 2;
    return z == sio_INF_OK ? 0 : (int)PNX_SH_KIND + 2;
}

pnx_sheet_result pnx_sheet_load_xlsx(uint8_t *b, uint32_t cap, uint8_t *src, uint64_t n, uint64_t srccap, int mode, int64_t sheet,
                                     int64_t *added, uint64_t *charge) {
    if (mode < 0 || mode > 2)
        return PNX_SH_RANGE;
    if (sheet < 0)
        return PNX_SH_NONE;
    int64_t dim;
    pnx_sheet_result e = pnx_sheet_dim(b, cap, 0, &dim);
    if (e)
        return e;
    sio_zip_t zip;
    if (!sio_zip_open(&zip, src, n))
        return PNX_SH_KIND;
    // the work area: after the file, aligned
    uint64_t at = (n + 15u) & ~15ull;
    const uint64_t need = sizeof(sio_inf) + sio_WIN + sizeof(sio_xlsx) + sio_TAG_MAX + sio_TEXT_MAX + 4096;
    if (at > srccap || srccap - at < need)
        return PNX_SH_FULL;
    uint8_t *w = src + at;
    sio_xrun     r;
    r.z = (sio_inf *)(void *)w;
    w += (sizeof(sio_inf) + 15u) & ~15ull;
    r.z->win = w;
    w += sio_WIN;
    r.a = (sio_xlsx *)(void *)w;
    w += (sizeof(sio_xlsx) + 15u) & ~15ull;
    __builtin_memset(r.a, 0, sizeof *r.a);
    r.x = (sio_xml){.tag = w, .text = w + sio_TAG_MAX, .ctx = r.a, .open = sio_xlsx_open, .close = sio_xlsx_close};
    w += sio_TAG_MAX + sio_TEXT_MAX;
    // what is left holds the shared strings: the text from the low end, the table of their offsets from the high end
    const uint64_t left = srccap - (uint64_t)(w - src);
    r.a->strs = w;
    r.a->strs_cap = (uint32_t)(left > 0x7fffffffu ? 0x7fffffffu : left) & ~3u;
    r.a->table = (uint32_t *)(void *)(w + r.a->strs_cap);
    r.a->sink = (sio_sink_t){b, cap, pnx_sheet_rows(b, cap), pnx_sheet_cols(b, cap), 0, 0};
    r.a->sink.rows = r.a->sink.base;
    r.a->mode = mode;
    r.a->want = sheet;
    r.a->row = -1;

    int p = sio_xlsx_part(&r, &zip, "xl/workbook.xml", sio_PH_WORKBOOK, charge);
    if (p == 0 && !r.a->have_rid)
        p = (int)PNX_SH_NONE + 2;
    if (p == 0)
        p = sio_xlsx_part(&r, &zip, "xl/_rels/workbook.xml.rels", sio_PH_RELS, charge);
    if (p == 0 && r.a->target[0] == 0)
        p = (int)PNX_SH_KIND + 2;
    if (p == 0) {
        p = sio_xlsx_part(&r, &zip, "xl/styles.xml", sio_PH_STYLES, charge);
        if (p == 1)
            p = 0; // no styles: no dates
    }
    if (p == 0) {
        p = sio_xlsx_part(&r, &zip, "xl/sharedStrings.xml", sio_PH_SST, charge);
        if (p == 1)
            p = 0;
    }
    if (p == 0) {
        p = sio_xlsx_part(&r, &zip, r.a->target, sio_PH_SHEET, charge);
        if (p == 1)
            p = (int)PNX_SH_KIND + 2;
    }
    *charge += r.a->sink.charge;
    if (p) {
        (void)pnx_sheet_truncate_rows(b, cap, r.a->sink.base);
        return (p >= 2 ? (pnx_sheet_result)(p - 2) : PNX_SH_KIND);
    }
    *added = (int64_t)r.a->sink.rows - r.a->sink.base;
    return PNX_SH_OK;
}

// ---- writing ---------------------------------------------------------------------------------

typedef struct {
    uint8_t *p;
    uint64_t cap, len;
    bool     full;
} sio_out_t;

static void sio_out_bytes(sio_out_t *o, const void *s, uint64_t n) {
    if (o->full || o->cap - o->len < n) {
        o->full = true;
        return;
    }
    __builtin_memcpy(o->p + o->len, s, n);
    o->len += n;
}
static void sio_out_str(sio_out_t *o, const char *s) { sio_out_bytes(o, s, __builtin_strlen(s)); }
static void sio_out_u32(sio_out_t *o, uint32_t v) {
    const uint8_t t[4] = {(uint8_t)v, (uint8_t)(v >> 8), (uint8_t)(v >> 16), (uint8_t)(v >> 24)};
    sio_out_bytes(o, t, 4);
}
static void sio_out_u16(sio_out_t *o, uint32_t v) {
    const uint8_t t[2] = {(uint8_t)v, (uint8_t)(v >> 8)};
    sio_out_bytes(o, t, 2);
}
static uint32_t sio_out_int(uint8_t *buf, int64_t v) {
    uint8_t  t[24];
    uint32_t n = 0, w = 0;
    uint64_t m = v < 0 ? 0u - (uint64_t)v : (uint64_t)v;
    do {
        t[n++] = (uint8_t)('0' + m % 10);
        m /= 10;
    } while (m);
    if (v < 0)
        buf[w++] = '-';
    while (n)
        buf[w++] = t[--n];
    return w;
}
// v / 10^scale as text with exactly `scale` digits after the point
static uint32_t sio_out_dec(uint8_t *buf, int64_t v, uint32_t scale) {
    uint8_t  t[40];
    uint32_t n = 0, w = 0;
    uint64_t m = v < 0 ? 0u - (uint64_t)v : (uint64_t)v;
    for (uint32_t k = 0; k < scale; ++k) {
        t[n++] = (uint8_t)('0' + m % 10);
        m /= 10;
    }
    if (scale)
        t[n++] = '.';
    do {
        t[n++] = (uint8_t)('0' + m % 10);
        m /= 10;
    } while (m);
    if (v < 0)
        buf[w++] = '-';
    while (n)
        buf[w++] = t[--n];
    return w;
}

static uint32_t sio_crc32_of(const uint8_t *p, uint64_t n) {
    static const uint32_t T[16] = {0x00000000, 0x1db71064, 0x3b6e20c8, 0x26d930ac, 0x76dc4190, 0x6b6b51f4, 0x4db26158, 0x5005713c,
                                   0xedb88320, 0xf00f9344, 0xd6d6a3e8, 0xcb61b38c, 0x9b64c2b0, 0x86d3d2d4, 0xa00ae278, 0xbdbdf21c};
    uint32_t       c = 0xffffffffu;
    for (uint64_t k = 0; k < n; ++k) {
        c ^= p[k];
        c = (c >> 4) ^ T[c & 15u];
        c = (c >> 4) ^ T[c & 15u];
    }
    return ~c;
}

static bool sio_dec_scale_of(uint8_t *b, uint32_t cap, int64_t r, int64_t c, int64_t *v, uint32_t *scale) {
    for (uint32_t s = 0; s <= sio_DEC_MAX_SCALE; ++s) {
        if (pnx_sheet_get_dec(b, cap, r, c, s, v) == PNX_SH_OK) {
            *scale = s;
            return true;
        }
    }
    return false;
}

// the last column that holds a cell, plus one; and the rows
static pnx_sheet_result sio_used_area(uint8_t *b, uint32_t cap, uint32_t *rows, uint32_t *cols, uint64_t *charge) {
    int64_t dim;
    pnx_sheet_result e = pnx_sheet_dim(b, cap, 0, &dim);
    if (e)
        return e;
    *rows = pnx_sheet_rows(b, cap);
    const uint32_t all = pnx_sheet_cols(b, cap);
    uint32_t       used = 0;
    for (uint32_t r = 0; r < *rows; ++r) {
        for (uint32_t c = all; c > used; --c) {
            int64_t kind;
            if (pnx_sheet_kind(b, cap, r, c - 1, &kind) == PNX_SH_OK && kind != 0) {
                used = c;
                break;
            }
        }
    }
    *charge += (uint64_t)*rows * (all ? all : 1);
    *cols = used;
    return PNX_SH_OK;
}

pnx_sheet_result pnx_sheet_save_csv(uint8_t *b, uint32_t cap, uint8_t *dst, uint64_t dstcap, int64_t *len, uint64_t *charge) {
    uint32_t rows, cols;
    pnx_sheet_result e = sio_used_area(b, cap, &rows, &cols, charge);
    if (e)
        return e;
    sio_out_t o = {dst, dstcap, 0, false};
    for (uint32_t r = 0; r < rows && !o.full; ++r) {
        for (uint32_t c = 0; c < cols && !o.full; ++c) {
            if (c)
                sio_out_bytes(&o, ",", 1);
            int64_t kind = 0, v;
            (void)pnx_sheet_kind(b, cap, r, c, &kind);
            uint8_t num[48];
            if (kind == 1 && pnx_sheet_get_int(b, cap, r, c, &v) == PNX_SH_OK) {
                sio_out_bytes(&o, num, sio_out_int(num, v));
            } else if (kind == 2 && pnx_sheet_get_bool(b, cap, r, c, &v) == PNX_SH_OK) {
                sio_out_str(&o, v ? "true" : "false");
            } else if (kind == 3) {
                uint32_t scale;
                if (sio_dec_scale_of(b, cap, r, c, &v, &scale))
                    sio_out_bytes(&o, num, sio_out_dec(num, v, scale));
            } else if (kind == 5) {
                if (pnx_sheet_get_date(b, cap, r, c, &v) == PNX_SH_OK) {
                    const uint32_t dn = sio_out_date(num, v);
                    sio_out_bytes(&o, num, dn ? dn : sio_out_int(num, v)); // outside the years 0 to 9999: the seconds
                }
            } else if (kind == 4) {
                uint32_t off, n;
                if (pnx_sheet_get_str(b, cap, r, c, &off, &n) == PNX_SH_OK) {
                    const uint8_t *s = b + off;
                    bool           quote = false;
                    for (uint32_t k = 0; k < n; ++k)
                        quote = quote || s[k] == ',' || s[k] == '"' || s[k] == '\n' || s[k] == '\r';
                    if (!quote) {
                        sio_out_bytes(&o, s, n);
                    } else {
                        sio_out_bytes(&o, "\"", 1);
                        for (uint32_t k = 0; k < n; ++k) {
                            if (s[k] == '"')
                                sio_out_bytes(&o, "\"", 1);
                            sio_out_bytes(&o, s + k, 1);
                        }
                        sio_out_bytes(&o, "\"", 1);
                    }
                    *charge += n;
                }
            }
        }
        sio_out_bytes(&o, "\n", 1);
    }
    *charge += o.len;
    if (o.full)
        return PNX_SH_FULL;
    *len = (int64_t)o.len;
    return PNX_SH_OK;
}

static void sio_col_letters(uint32_t c, char *out) { // 0 -> A
    char t[8];
    uint32_t n = 0;
    c += 1;
    while (c) {
        t[n++] = (char)('A' + (c - 1) % 26);
        c = (c - 1) / 26;
    }
    for (uint32_t k = 0; k < n; ++k)
        out[k] = t[n - 1 - k];
    out[n] = 0;
}

// Text for XML: the five characters escaped, control characters other than tab and line breaks dropped.
static void sio_out_xml(sio_out_t *o, const uint8_t *s, uint32_t n) {
    for (uint32_t k = 0; k < n; ++k) {
        const uint8_t c = s[k];
        if (c == '<')
            sio_out_str(o, "&lt;");
        else if (c == '>')
            sio_out_str(o, "&gt;");
        else if (c == '&')
            sio_out_str(o, "&amp;");
        else if (c == '"')
            sio_out_str(o, "&quot;");
        else if (c >= 0x20 || c == '\t' || c == '\n')
            sio_out_bytes(o, &c, 1);
    }
}

typedef struct {
    uint32_t crc, size, lho;
    const char *name;
} sio_zpart;

static void sio_zip_begin(sio_out_t *o, sio_zpart *z, const char *name) {
    z->name = name;
    z->lho = (uint32_t)o->len;
    sio_out_u32(o, 0x04034b50u);
    sio_out_u16(o, 20);
    sio_out_u16(o, 0x0800); // utf-8 names
    sio_out_u16(o, 0);      // stored
    sio_out_u16(o, 0);
    sio_out_u16(o, 0x21);   // 1980-01-01
    sio_out_u32(o, 0);      // crc, sizes: patched when the part ends
    sio_out_u32(o, 0);
    sio_out_u32(o, 0);
    sio_out_u16(o, (uint32_t)__builtin_strlen(name));
    sio_out_u16(o, 0);
    sio_out_str(o, name);
    z->size = (uint32_t)o->len; // where the data begins, for now
}
static void sio_zip_end(sio_out_t *o, sio_zpart *z) {
    const uint32_t start = z->size, size = (uint32_t)o->len - start;
    z->size = size;
    if (o->full)
        return;
    z->crc = sio_crc32_of(o->p + start, size);
    uint8_t *h = o->p + z->lho;
    const uint32_t v[3] = {z->crc, size, size};
    for (int k = 0; k < 3; ++k) {
        h[14 + 4 * k] = (uint8_t)v[k];
        h[15 + 4 * k] = (uint8_t)(v[k] >> 8);
        h[16 + 4 * k] = (uint8_t)(v[k] >> 16);
        h[17 + 4 * k] = (uint8_t)(v[k] >> 24);
    }
}

pnx_sheet_result pnx_sheet_save_xlsx(uint8_t *b, uint32_t cap, uint8_t *dst, uint64_t dstcap, int64_t *len, uint64_t *charge) {
    uint32_t rows, cols;
    pnx_sheet_result e = sio_used_area(b, cap, &rows, &cols, charge);
    if (e)
        return e;
    // the name of the sheet: 31 characters, none of []:*?/\ (Excel refuses them)
    int64_t  cur;
    uint32_t noff = 0, nlen = 0;
    char     name[32];
    uint32_t nn = 0;
    if (pnx_sheet_current(b, cap, &cur) == PNX_SH_OK && pnx_sheet_sheet_name(b, cap, cur, &noff, &nlen) == PNX_SH_OK) {
        for (uint32_t k = 0; k < nlen && nn < 31; ++k) {
            const uint8_t c = b[noff + k];
            if (c < 0x20 || c == '[' || c == ']' || c == ':' || c == '*' || c == '?' || c == '/' || c == '\\' || c == '&' || c == '<' ||
                c == '>' || c == '"' || c == '\'' || c >= 0x80)
                name[nn++] = '_';
            else
                name[nn++] = (char)c;
        }
    }
    if (nn == 0) {
        __builtin_memcpy(name, "Sheet1", 6);
        nn = 6;
    }
    name[nn] = 0;

    bool has_date = false; // a styles part is written only for a sheet that has a date: the others stay as they were
    for (uint32_t r = 0; r < rows && !has_date; ++r)
        for (uint32_t c = 0; c < cols && !has_date; ++c) {
            int64_t kind = 0;
            has_date = pnx_sheet_kind(b, cap, r, c, &kind) == PNX_SH_OK && kind == 5;
        }
    *charge += (uint64_t)rows * cols;

    sio_out_t o = {dst, dstcap, 0, false};
    sio_zpart z[6];
    sio_zip_begin(&o, &z[0], "[Content_Types].xml");
    sio_out_str(&o, "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>\n<Types xmlns=\"http://schemas.openxmlformats.org/package/2006/content-types\">"
                "<Default Extension=\"rels\" ContentType=\"application/vnd.openxmlformats-package.relationships+xml\"/>"
                "<Default Extension=\"xml\" ContentType=\"application/xml\"/>"
                "<Override PartName=\"/xl/workbook.xml\" ContentType=\"application/vnd.openxmlformats-officedocument.spreadsheetml.sheet.main+xml\"/>"
                "<Override PartName=\"/xl/worksheets/sheet1.xml\" ContentType=\"application/vnd.openxmlformats-officedocument.spreadsheetml.worksheet+xml\"/>");
    if (has_date)
        sio_out_str(&o, "<Override PartName=\"/xl/styles.xml\" ContentType=\"application/vnd.openxmlformats-officedocument.spreadsheetml.styles+xml\"/>");
    sio_out_str(&o, "</Types>");
    sio_zip_end(&o, &z[0]);
    sio_zip_begin(&o, &z[1], "_rels/.rels");
    sio_out_str(&o, "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>\n<Relationships xmlns=\"http://schemas.openxmlformats.org/package/2006/relationships\">"
                "<Relationship Id=\"rId1\" Type=\"http://schemas.openxmlformats.org/officeDocument/2006/relationships/officeDocument\" Target=\"xl/workbook.xml\"/></Relationships>");
    sio_zip_end(&o, &z[1]);
    sio_zip_begin(&o, &z[2], "xl/workbook.xml");
    sio_out_str(&o, "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>\n<workbook xmlns=\"http://schemas.openxmlformats.org/spreadsheetml/2006/main\" "
                "xmlns:r=\"http://schemas.openxmlformats.org/officeDocument/2006/relationships\"><sheets><sheet name=\"");
    sio_out_xml(&o, (const uint8_t *)name, nn);
    sio_out_str(&o, "\" sheetId=\"1\" r:id=\"rId1\"/></sheets></workbook>");
    sio_zip_end(&o, &z[2]);
    sio_zip_begin(&o, &z[3], "xl/_rels/workbook.xml.rels");
    sio_out_str(&o, "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>\n<Relationships xmlns=\"http://schemas.openxmlformats.org/package/2006/relationships\">"
                "<Relationship Id=\"rId1\" Type=\"http://schemas.openxmlformats.org/officeDocument/2006/relationships/worksheet\" Target=\"worksheets/sheet1.xml\"/>");
    if (has_date)
        sio_out_str(&o, "<Relationship Id=\"rId2\" Type=\"http://schemas.openxmlformats.org/officeDocument/2006/relationships/styles\" Target=\"styles.xml\"/>");
    sio_out_str(&o, "</Relationships>");
    sio_zip_end(&o, &z[3]);
    int nparts = 5;
    if (has_date) { // one number format, `yyyy-mm-dd hh:mm:ss`, as the second cell format: s="1"
        sio_zip_begin(&o, &z[4], "xl/styles.xml");
        sio_out_str(&o, "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>\n<styleSheet xmlns=\"http://schemas.openxmlformats.org/spreadsheetml/2006/main\">"
                    "<numFmts count=\"1\"><numFmt numFmtId=\"164\" formatCode=\"yyyy-mm-dd hh:mm:ss\"/></numFmts>"
                    "<fonts count=\"1\"><font><sz val=\"11\"/><name val=\"Calibri\"/></font></fonts>"
                    "<fills count=\"2\"><fill><patternFill patternType=\"none\"/></fill><fill><patternFill patternType=\"gray125\"/></fill></fills>"
                    "<borders count=\"1\"><border><left/><right/><top/><bottom/><diagonal/></border></borders>"
                    "<cellStyleXfs count=\"1\"><xf numFmtId=\"0\" fontId=\"0\" fillId=\"0\" borderId=\"0\"/></cellStyleXfs>"
                    "<cellXfs count=\"2\"><xf numFmtId=\"0\" fontId=\"0\" fillId=\"0\" borderId=\"0\" xfId=\"0\"/>"
                    "<xf numFmtId=\"164\" fontId=\"0\" fillId=\"0\" borderId=\"0\" xfId=\"0\" applyNumberFormat=\"1\"/></cellXfs>"
                    "<cellStyles count=\"1\"><cellStyle name=\"Normal\" xfId=\"0\" builtinId=\"0\"/></cellStyles></styleSheet>");
        sio_zip_end(&o, &z[4]);
        nparts = 6;
    }
    sio_zpart *const sheet_part = &z[nparts - 1];
    sio_zip_begin(&o, sheet_part, "xl/worksheets/sheet1.xml");
    sio_out_str(&o, "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>\n<worksheet xmlns=\"http://schemas.openxmlformats.org/spreadsheetml/2006/main\"><sheetData>");
    for (uint32_t r = 0; r < rows && !o.full; ++r) {
        bool open = false;
        for (uint32_t c = 0; c < cols && !o.full; ++c) {
            int64_t kind = 0, v;
            if (pnx_sheet_kind(b, cap, r, c, &kind) != PNX_SH_OK || kind == 0)
                continue;
            uint8_t num[48];
            char    ref[16], row[16];
            sio_col_letters(c, ref);
            if (!open) {
                row[sio_out_int((uint8_t *)row, (int64_t)r + 1)] = 0;
                sio_out_str(&o, "<row r=\"");
                sio_out_str(&o, row);
                sio_out_str(&o, "\">");
                open = true;
            }
            row[sio_out_int((uint8_t *)row, (int64_t)r + 1)] = 0;
            sio_out_str(&o, "<c r=\"");
            sio_out_str(&o, ref);
            sio_out_str(&o, row);
            if (kind == 1 && pnx_sheet_get_int(b, cap, r, c, &v) == PNX_SH_OK) {
                sio_out_str(&o, "\"><v>");
                sio_out_bytes(&o, num, sio_out_int(num, v));
                sio_out_str(&o, "</v></c>");
            } else if (kind == 2 && pnx_sheet_get_bool(b, cap, r, c, &v) == PNX_SH_OK) {
                sio_out_str(&o, v ? "\" t=\"b\"><v>1</v></c>" : "\" t=\"b\"><v>0</v></c>");
            } else if (kind == 5 && pnx_sheet_get_date(b, cap, r, c, &v) == PNX_SH_OK) {
                int64_t days = v / 86400, rem = v % 86400;
                if (rem < 0) {
                    rem += 86400;
                    --days;
                }
                if (days + 25569 >= 61 && days + 25569 < 2958466) { // the 1900 date system (day 61 is 1900-03-01): a date cell
                    const int64_t frac = (rem * 1000000000 + 43200) / 86400;
                    sio_out_str(&o, "\" s=\"1\"><v>");
                    sio_out_bytes(&o, num, sio_out_int(num, days + 25569));
                    if (rem) {
                        sio_out_str(&o, ".");
                        for (int64_t d = 100000000; d >= 1; d /= 10)
                            sio_out_bytes(&o, (uint8_t[]){(uint8_t)('0' + frac / d % 10)}, 1);
                    }
                    sio_out_str(&o, "</v></c>");
                } else { // too early or too late for the format: the seconds as a number
                    sio_out_str(&o, "\"><v>");
                    sio_out_bytes(&o, num, sio_out_int(num, v));
                    sio_out_str(&o, "</v></c>");
                }
            } else if (kind == 3) {
                uint32_t scale = 0;
                v = 0;
                (void)sio_dec_scale_of(b, cap, r, c, &v, &scale);
                sio_out_str(&o, "\"><v>");
                sio_out_bytes(&o, num, sio_out_dec(num, v, scale));
                sio_out_str(&o, "</v></c>");
            } else {
                uint32_t off = 0, n = 0;
                (void)pnx_sheet_get_str(b, cap, r, c, &off, &n);
                sio_out_str(&o, "\" t=\"inlineStr\"><is><t xml:space=\"preserve\">");
                sio_out_xml(&o, b + off, n);
                sio_out_str(&o, "</t></is></c>");
                *charge += n;
            }
        }
        if (open)
            sio_out_str(&o, "</row>");
    }
    sio_out_str(&o, "</sheetData></worksheet>");
    sio_zip_end(&o, sheet_part);
    // the central directory
    const uint32_t cd = (uint32_t)o.len;
    for (int k = 0; k < nparts; ++k) {
        sio_out_u32(&o, 0x02014b50u);
        sio_out_u16(&o, 20);
        sio_out_u16(&o, 20);
        sio_out_u16(&o, 0x0800);
        sio_out_u16(&o, 0);
        sio_out_u16(&o, 0);
        sio_out_u16(&o, 0x21);
        sio_out_u32(&o, z[k].crc);
        sio_out_u32(&o, z[k].size);
        sio_out_u32(&o, z[k].size);
        sio_out_u16(&o, (uint32_t)__builtin_strlen(z[k].name));
        sio_out_u16(&o, 0);
        sio_out_u16(&o, 0);
        sio_out_u16(&o, 0);
        sio_out_u16(&o, 0);
        sio_out_u32(&o, 0);
        sio_out_u32(&o, z[k].lho);
        sio_out_str(&o, z[k].name);
    }
    const uint32_t cdsize = (uint32_t)o.len - cd;
    sio_out_u32(&o, 0x06054b50u);
    sio_out_u16(&o, 0);
    sio_out_u16(&o, 0);
    sio_out_u16(&o, (uint32_t)nparts);
    sio_out_u16(&o, (uint32_t)nparts);
    sio_out_u32(&o, cdsize);
    sio_out_u32(&o, cd);
    sio_out_u16(&o, 0);
    *charge += o.len;
    if (o.full || o.len > 0xffffffffu)
        return PNX_SH_FULL;
    *len = (int64_t)o.len;
    return PNX_SH_OK;
}
