// pinax_toml.c - the text format of the module toml (RFC-0046): TOML 1.0 read a statement at a time, or by a path,
// without building a tree. A value is given as the bytes it has in the text; only a string with escapes or
// several lines is decoded (pnx_toml_decode), and then by the caller into the region.
#include "pinax_internal.h"

#define TML_SEGS 16
#define TML_STEPS 40
#define TML_ARRAYS 32
#define TML_DEPTH 32

static bool tml_blank(uint8_t b) { return b == ' ' || b == '\t'; }
static bool tml_bare(uint8_t b) {
    return (b >= 'a' && b <= 'z') || (b >= 'A' && b <= 'Z') || (b >= '0' && b <= '9') || b == '_' || b == '-';
}
static bool tml_digit(uint8_t b) { return b >= '0' && b <= '9'; }

typedef struct {
    uint32_t off, len;
} tml_seg;

// Blanks, comments and line ends, as many as there are.
static uint32_t tml_gap(const uint8_t *s, uint32_t n, uint32_t at, bool lines) {
    for (;;) {
        while (at < n && (tml_blank(s[at]) || (lines && (s[at] == '\n' || s[at] == '\r'))))
            ++at;
        if (at < n && s[at] == '#') {
            while (at < n && s[at] != '\n')
                ++at;
            if (!lines)
                return at;
            continue;
        }
        return at;
    }
}

// The end of a line: blanks, a comment, then a line end or the end of the text. 0 when it is, -1 when not.
static int tml_eol(const uint8_t *s, uint32_t n, uint32_t *at) {
    uint32_t k = tml_gap(s, n, *at, false);
    if (k < n) {
        if (s[k] == '\r' && k + 1 < n && s[k + 1] == '\n')
            ++k;
        if (s[k] != '\n')
            return -1;
        ++k;
    }
    *at = k;
    return 0;
}

// A string literal from its opening quote: where it ends, -1 when it does not. `*multi` tells a literal of
// three quotes.
static int64_t tml_string(const uint8_t *s, uint32_t n, uint32_t at, bool *multi) {
    const uint8_t q = s[at];
    const bool    basic = q == '"';
    *multi = at + 2 < n && s[at + 1] == q && s[at + 2] == q;
    uint32_t k = at + (*multi ? 3 : 1);
    while (k < n) {
        const uint8_t b = s[k];
        if (*multi) {
            if (basic && b == '\\') {
                k += 2;
                continue;
            }
            if (b == q && k + 2 < n && s[k + 1] == q && s[k + 2] == q) {
                uint32_t e = k + 3;
                while (e < n && s[e] == q && e < k + 5)
                    ++e;
                return e;
            }
            if ((b < 0x20 && b != '\n' && b != '\r' && b != '\t') || b == 0x7f)
                return -1;
            ++k;
            continue;
        }
        if (b == q)
            return (int64_t)k + 1;
        if (basic && b == '\\') {
            k += 2;
            continue;
        }
        if ((b < 0x20 && b != '\t') || b == 0x7f)
            return -1;
        ++k;
    }
    return -1;
}

static bool tml_delim(uint8_t b) {
    return b == ' ' || b == '\t' || b == '\r' || b == '\n' || b == ',' || b == ']' || b == '}' || b == '#';
}

// digits with single underscores between them; `radix` 10, 16, 8 or 2
static bool tml_digits(const uint8_t *s, uint32_t from, uint32_t to, int radix, bool leading_zero_ok) {
    if (from >= to)
        return false;
    if (!leading_zero_ok && s[from] == '0' && to - from > 1)
        return false;
    bool last_digit = false;
    for (uint32_t k = from; k < to; ++k) {
        const uint8_t b = s[k];
        bool ok;
        if (b == '_') {
            if (!last_digit)
                return false;
            last_digit = false;
            continue;
        }
        const int v = tml_digit(b) ? b - '0' : (b | 0x20) >= 'a' && (b | 0x20) <= 'f' ? (b | 0x20) - 'a' + 10 : 99;
        ok = v < radix;
        if (!ok)
            return false;
        last_digit = true;
    }
    return last_digit;
}

static bool tml_two(const uint8_t *s, uint32_t k) { return tml_digit(s[k]) && tml_digit(s[k + 1]); }

// a bare token: bool, number, date or time
static int tml_token(const uint8_t *s, uint32_t n, uint32_t at, uint32_t *end) {
    uint32_t e = at;
    while (e < n && !tml_delim(s[e]))
        ++e;
    const uint32_t len = e - at;
    if (len == 0)
        return -1;
    const uint8_t *t = s + at;
    *end = e;
    if ((len == 4 && __builtin_memcmp(t, "true", 4) == 0) || (len == 5 && __builtin_memcmp(t, "false", 5) == 0))
        return PNX_TOML_BOOL;
    if (len >= 10 && t[4] == '-' && t[7] == '-' && tml_digit(t[0]) && tml_digit(t[1]) && tml_digit(t[2]) && tml_digit(t[3]) &&
        tml_two(t, 5) && tml_two(t, 8)) { // a date, perhaps with a time after a space
        if (len == 10 && e + 3 < n && s[e] == ' ' && tml_two(s, e + 1) && s[e + 3] == ':') {
            e += 1;
            while (e < n && !tml_delim(s[e]))
                ++e;
            *end = e;
        }
        return PNX_TOML_DATETIME;
    }
    if (len >= 5 && t[2] == ':' && tml_two(t, 0) && tml_two(t, 3))
        return PNX_TOML_DATETIME;
    uint32_t k = 0;
    if (t[0] == '+' || t[0] == '-')
        k = 1;
    if (len - k == 3 && (__builtin_memcmp(t + k, "inf", 3) == 0 || __builtin_memcmp(t + k, "nan", 3) == 0))
        return PNX_TOML_FLOAT;
    if (t[k] == '0' && len - k > 1 && (t[k + 1] == 'x' || t[k + 1] == 'o' || t[k + 1] == 'b')) {
        if (k)
            return -1;
        return tml_digits(t, 2, len, t[1] == 'x' ? 16 : t[1] == 'o' ? 8 : 2, true) ? PNX_TOML_INT : -1;
    }
    uint32_t dot = len, ex = len;
    for (uint32_t m = k; m < len; ++m) {
        if (t[m] == '.' && dot == len && ex == len)
            dot = m;
        else if ((t[m] | 0x20) == 'e' && ex == len)
            ex = m;
    }
    if (dot == len && ex == len)
        return tml_digits(t, k, len, 10, false) ? PNX_TOML_INT : -1;
    const uint32_t int_end = dot < ex ? dot : ex;
    if (!tml_digits(t, k, int_end, 10, false))
        return -1;
    if (dot < len && !tml_digits(t, dot + 1, ex, 10, true))
        return -1;
    if (ex < len) {
        uint32_t x = ex + 1;
        if (x < len && (t[x] == '+' || t[x] == '-'))
            ++x;
        if (!tml_digits(t, x, len, 10, true))
            return -1;
    }
    return PNX_TOML_FLOAT;
}

static int tml_keys(const uint8_t *s, uint32_t n, uint32_t *at, tml_seg *segs, uint32_t *count);

// One value from its first byte: its kind, and where it ends (-1 when it is malformed).
static int64_t tml_value(const uint8_t *s, uint32_t n, uint32_t at, uint32_t depth, int *kind) {
    if (at >= n || depth > TML_DEPTH)
        return -1;
    const uint8_t b = s[at];
    if (b == '"' || b == '\'') {
        bool          multi;
        const int64_t e = tml_string(s, n, at, &multi);
        *kind = PNX_TOML_STRING;
        return e;
    }
    if (b == '[') {
        uint32_t k = at + 1;
        *kind = PNX_TOML_ARRAY;
        for (;;) {
            k = tml_gap(s, n, k, true);
            if (k >= n)
                return -1;
            if (s[k] == ']')
                return (int64_t)k + 1;
            int           ek;
            const int64_t e = tml_value(s, n, k, depth + 1, &ek);
            if (e < 0)
                return -1;
            k = tml_gap(s, n, (uint32_t)e, true);
            if (k >= n)
                return -1;
            if (s[k] == ',') {
                ++k;
                continue;
            }
            if (s[k] != ']')
                return -1;
            return (int64_t)k + 1;
        }
    }
    if (b == '{') {
        uint32_t k = at + 1;
        *kind = PNX_TOML_INLINE;
        k = tml_gap(s, n, k, false);
        if (k < n && s[k] == '}')
            return (int64_t)k + 1;
        for (;;) {
            tml_seg  segs[TML_SEGS];
            uint32_t count;
            k = tml_gap(s, n, k, false);
            if (tml_keys(s, n, &k, segs, &count) < 0)
                return -1;
            k = tml_gap(s, n, k, false);
            if (k >= n || s[k] != '=')
                return -1;
            k = tml_gap(s, n, k + 1, false);
            int           ek;
            const int64_t e = tml_value(s, n, k, depth + 1, &ek);
            if (e < 0)
                return -1;
            k = tml_gap(s, n, (uint32_t)e, false);
            if (k >= n)
                return -1;
            if (s[k] == ',') {
                ++k;
                continue;
            }
            if (s[k] != '}')
                return -1;
            return (int64_t)k + 1;
        }
    }
    uint32_t end;
    const int t = tml_token(s, n, at, &end);
    if (t < 0)
        return -1;
    *kind = t;
    return end;
}

// A key of dotted parts, from *at: the parts as written (quoted ones without their quotes). It stops at the first
// byte that is not part of a key; -1 when a part is empty or a quote is open.
static int tml_keys(const uint8_t *s, uint32_t n, uint32_t *at, tml_seg *segs, uint32_t *count) {
    uint32_t k = *at, c = 0;
    for (;;) {
        k = tml_gap(s, n, k, false);
        if (k >= n || c == TML_SEGS)
            return -1;
        if (s[k] == '"' || s[k] == '\'') {
            const uint8_t q = s[k];
            uint32_t      e = k + 1;
            while (e < n && s[e] != q && s[e] != '\n') {
                if (q == '"' && s[e] == '\\')
                    ++e;
                ++e;
            }
            if (e >= n || s[e] != q)
                return -1;
            segs[c++] = (tml_seg){k + 1, e - k - 1};
            k = e + 1;
        } else {
            const uint32_t b = k;
            while (k < n && tml_bare(s[k]))
                ++k;
            if (k == b)
                return -1;
            segs[c++] = (tml_seg){b, k - b};
        }
        const uint32_t after = tml_gap(s, n, k, false);
        if (after < n && s[after] == '.') {
            k = after + 1;
            continue;
        }
        *at = k;
        *count = c;
        return 0;
    }
}

typedef struct {
    int      kind;
    uint32_t nsegs;
    tml_seg  segs[TML_SEGS];
    uint32_t key_off, key_len, val_off, val_len, past;
} tml_stmt;

// The next statement from `at`: 1, 0 at the end of the text, -1 when it is malformed.
static int tml_next(const uint8_t *s, uint32_t n, uint32_t at, tml_stmt *st) {
    at = tml_gap(s, n, at, true);
    if (at >= n)
        return 0;
    st->val_off = st->val_len = 0;
    if (s[at] == '[') {
        const bool dbl = at + 1 < n && s[at + 1] == '[';
        uint32_t   k = at + (dbl ? 2 : 1);
        st->key_off = tml_gap(s, n, k, false);
        if (tml_keys(s, n, &k, st->segs, &st->nsegs) < 0)
            return -1;
        uint32_t e = k;
        while (e > st->key_off && tml_blank(s[e - 1]))
            --e;
        st->key_len = e - st->key_off;
        k = tml_gap(s, n, k, false);
        if (k >= n || s[k] != ']' || (dbl && (k + 1 >= n || s[k + 1] != ']')))
            return -1;
        k += dbl ? 2 : 1;
        if (tml_eol(s, n, &k) < 0)
            return -1;
        st->kind = dbl ? PNX_TOML_TABLES : PNX_TOML_TABLE;
        st->past = k;
        return 1;
    }
    uint32_t k = at;
    st->key_off = at;
    if (tml_keys(s, n, &k, st->segs, &st->nsegs) < 0)
        return -1;
    uint32_t e = k;
    while (e > st->key_off && tml_blank(s[e - 1]))
        --e;
    st->key_len = e - st->key_off;
    k = tml_gap(s, n, k, false);
    if (k >= n || s[k] != '=')
        return -1;
    k = tml_gap(s, n, k + 1, false);
    int           kind;
    const int64_t end = tml_value(s, n, k, 0, &kind);
    if (end < 0)
        return -1;
    st->kind = kind;
    st->val_off = k;
    st->val_len = (uint32_t)end - k;
    k = (uint32_t)end;
    if (tml_eol(s, n, &k) < 0)
        return -1;
    st->past = k;
    return 1;
}

// The next statement of `s`, as toml.next gives it: the kind, the key as written (the table of a header), the
// value as the text has it (a string with its quotes), and the bytes it took.
int pnx_toml_next(const uint8_t *s, uint32_t n, int *kind, uint32_t *key_off, uint32_t *key_len, uint32_t *val_off,
                  uint32_t *val_len, uint32_t *past) {
    tml_stmt st;
    const int r = tml_next(s, n, 0, &st);
    if (r <= 0)
        return r;
    *kind = st.kind;
    *key_off = st.key_off;
    *key_len = st.key_len;
    *val_off = st.val_off;
    *val_len = st.val_len;
    *past = st.past;
    return 1;
}

bool pnx_toml_valid(const uint8_t *s, uint32_t n) {
    uint32_t at = 0;
    tml_stmt st;
    for (;;) {
        const int r = tml_next(s, n, at, &st);
        if (r < 0)
            return false;
        if (r == 0)
            return true;
        at = st.past;
    }
}

// ---- strings ----------------------------------------------------------------------------------------------

// A string literal (with its quotes) and where its bytes are in the text. 0: they are the bytes themselves
// (*off, *len); 1: they must be decoded (pnx_toml_decode gives at most `len` bytes); -1: it is not a string.
int pnx_toml_string(const uint8_t *s, uint32_t n, uint32_t *off, uint32_t *len) {
    if (n < 2 || (s[0] != '"' && s[0] != '\''))
        return -1;
    const bool multi = n >= 6 && s[1] == s[0] && s[2] == s[0];
    uint32_t   from = multi ? 3 : 1, to = n - from;
    if (n < 2 * from)
        return -1;
    if (multi) { // a line end right after the opening quotes is dropped
        if (from < to && s[from] == '\n')
            ++from;
        else if (from + 1 < to && s[from] == '\r' && s[from + 1] == '\n')
            from += 2;
    }
    *off = from;
    *len = to > from ? to - from : 0;
    if (s[0] == '"') {
        for (uint32_t k = from; k < to; ++k) {
            if (s[k] == '\\')
                return 1;
        }
    }
    return 0;
}

static int tml_hex(const uint8_t *p, uint32_t count, uint32_t *value) {
    uint32_t v = 0;
    for (uint32_t k = 0; k < count; ++k) {
        const uint8_t c = p[k];
        const int     d = tml_digit(c) ? c - '0' : (c | 0x20) >= 'a' && (c | 0x20) <= 'f' ? (c | 0x20) - 'a' + 10 : -1;
        if (d < 0)
            return -1;
        v = v * 16 + (uint32_t)d;
    }
    *value = v;
    return 0;
}

// The escapes of a basic string decoded into `out`: the bytes it made, PNX_TXT_BAD when an escape is wrong.
uint32_t pnx_toml_decode(const uint8_t *in, uint32_t n, uint8_t *out) {
    uint32_t o = 0;
    for (uint32_t k = 0; k < n; ++k) {
        if (in[k] != '\\') {
            out[o++] = in[k];
            continue;
        }
        if (++k == n)
            return PNX_TXT_BAD;
        switch (in[k]) {
        case 'b': out[o++] = '\b'; break;
        case 't': out[o++] = '\t'; break;
        case 'n': out[o++] = '\n'; break;
        case 'f': out[o++] = '\f'; break;
        case 'r': out[o++] = '\r'; break;
        case 'e': out[o++] = 0x1b; break;
        case '"': out[o++] = '"'; break;
        case '\\': out[o++] = '\\'; break;
        case 'u': case 'U': {
            const uint32_t digits = in[k] == 'u' ? 4 : 8;
            uint32_t       cp;
            if (n - k <= digits || tml_hex(in + k + 1, digits, &cp) < 0 || cp > 0x10ffff || (cp >= 0xd800 && cp <= 0xdfff))
                return PNX_TXT_BAD;
            k += digits;
            if (cp < 0x80) {
                out[o++] = (uint8_t)cp;
            } else if (cp < 0x800) {
                out[o++] = (uint8_t)(0xc0 | cp >> 6);
                out[o++] = (uint8_t)(0x80 | (cp & 0x3f));
            } else if (cp < 0x10000) {
                out[o++] = (uint8_t)(0xe0 | cp >> 12);
                out[o++] = (uint8_t)(0x80 | ((cp >> 6) & 0x3f));
                out[o++] = (uint8_t)(0x80 | (cp & 0x3f));
            } else {
                out[o++] = (uint8_t)(0xf0 | cp >> 18);
                out[o++] = (uint8_t)(0x80 | ((cp >> 12) & 0x3f));
                out[o++] = (uint8_t)(0x80 | ((cp >> 6) & 0x3f));
                out[o++] = (uint8_t)(0x80 | (cp & 0x3f));
            }
            break;
        }
        case ' ': case '\t': case '\r': case '\n': { // a backslash at the end of a line of a multi-line string
            uint32_t m = k;
            while (m < n && (in[m] == ' ' || in[m] == '\t'))
                ++m;
            if (m < n && in[m] == '\r')
                ++m;
            if (m >= n || in[m] != '\n')
                return PNX_TXT_BAD;
            while (m < n && (in[m] == ' ' || in[m] == '\t' || in[m] == '\n' || in[m] == '\r'))
                ++m;
            k = m - 1;
            break;
        }
        default: return PNX_TXT_BAD;
        }
    }
    return o;
}

// ---- a value by path --------------------------------------------------------------------------------------

typedef struct {
    const uint8_t *key; // NULL: an index
    uint32_t       key_len;
    uint32_t       index;
} tml_step;

// `a.b[2]."c d"`: names, quoted names and indexes. The number of steps, or -1.
static int tml_path(const uint8_t *p, uint32_t n, tml_step *steps) {
    uint32_t at = 0, c = 0;
    bool     first = true;
    while (at < n) {
        if (c == TML_STEPS)
            return -1;
        if (p[at] == '[') {
            uint64_t v = 0;
            const uint32_t d0 = ++at;
            while (at < n && tml_digit(p[at])) {
                v = v * 10 + (uint64_t)(p[at] - '0');
                if (v > INT32_MAX)
                    return -1;
                ++at;
            }
            if (at == d0 || at >= n || p[at] != ']')
                return -1;
            ++at;
            steps[c++] = (tml_step){NULL, 0, (uint32_t)v};
            first = false;
            continue;
        }
        if (p[at] == '.') {
            if (first)
                return -1;
            ++at;
        } else if (!first) {
            return -1;
        }
        first = false;
        if (at < n && (p[at] == '"' || p[at] == '\'')) {
            const uint8_t q = p[at++];
            const uint32_t b = at;
            while (at < n && p[at] != q)
                ++at;
            if (at >= n)
                return -1;
            steps[c++] = (tml_step){p + b, at - b, 0};
            ++at;
        } else {
            const uint32_t b = at;
            while (at < n && tml_bare(p[at]))
                ++at;
            if (at == b)
                return -1;
            steps[c++] = (tml_step){p + b, at - b, 0};
        }
    }
    return (int)c;
}

static bool tml_same(const tml_step *a, const tml_step *b) {
    if ((a->key == NULL) != (b->key == NULL))
        return false;
    if (!a->key)
        return a->index == b->index;
    return a->key_len == b->key_len && __builtin_memcmp(a->key, b->key, a->key_len) == 0;
}

typedef struct {
    int      kind;
    uint32_t off, len;
} tml_found;

// A value that is an array or an inline table, searched for the steps from `from` on: the same steps as in the text.
static int tml_descend(const uint8_t *s, uint32_t vo, uint32_t vl, const tml_step *t, uint32_t from, uint32_t m,
                       tml_found *out, uint32_t depth) {
    if (from == m) {
        int kind;
        const int64_t e = tml_value(s, vo + vl, vo, 0, &kind);
        if (e < 0)
            return -1;
        *out = (tml_found){kind, vo, (uint32_t)e - vo};
        return 1;
    }
    const uint32_t end = vo + vl;
    if (depth > TML_DEPTH)
        return -1;
    if (s[vo] == '[') {
        if (t[from].key)
            return 0;
        uint32_t k = vo + 1, seen = 0;
        for (;;) {
            k = tml_gap(s, end, k, true);
            if (k >= end || s[k] == ']')
                return 0;
            int           ek;
            const int64_t e = tml_value(s, end, k, 0, &ek);
            if (e < 0)
                return -1;
            if (seen++ == t[from].index)
                return tml_descend(s, k, (uint32_t)e - k, t, from + 1, m, out, depth + 1);
            k = tml_gap(s, end, (uint32_t)e, true);
            if (k < end && s[k] == ',')
                ++k;
        }
    }
    if (s[vo] != '{' || !t[from].key)
        return 0;
    uint32_t k = vo + 1;
    for (;;) {
        k = tml_gap(s, end, k, false);
        if (k >= end || s[k] == '}')
            return 0;
        tml_seg  segs[TML_SEGS];
        uint32_t count;
        if (tml_keys(s, end, &k, segs, &count) < 0)
            return -1;
        k = tml_gap(s, end, k, false);
        if (k >= end || s[k] != '=')
            return -1;
        k = tml_gap(s, end, k + 1, false);
        int           ek;
        const int64_t e = tml_value(s, end, k, 0, &ek);
        if (e < 0)
            return -1;
        bool match = from + count <= m;
        for (uint32_t q = 0; match && q < count; ++q) {
            tml_step st = {s + segs[q].off, segs[q].len, 0};
            match = tml_same(&st, &t[from + q]);
        }
        if (match)
            return tml_descend(s, k, (uint32_t)e - k, t, from + count, m, out, depth + 1);
        k = tml_gap(s, end, (uint32_t)e, false);
        if (k < end && s[k] == ',')
            ++k;
    }
}

// The arrays of tables seen so far: [[a.b]] is the number of its elements, and the elements of what is under an
// element are counted again from it.
typedef struct {
    uint32_t nsegs;
    tml_seg  segs[TML_SEGS];
    uint32_t count;
} tml_array;

static bool tml_seg_same(const uint8_t *s, tml_seg a, tml_seg b) {
    return a.len == b.len && __builtin_memcmp(s + a.off, s + b.off, a.len) == 0;
}

// The value of `path` in `s`: 1 found (kind and the bytes of the value; a table names no value: kind table, no
// bytes), 0 when there is none, -1 when the text or the path is malformed where it was read.
int pnx_toml_at(const uint8_t *s, uint32_t n, const uint8_t *path, uint32_t pn, int *kind, uint32_t *off, uint32_t *len) {
    tml_step t[TML_STEPS];
    const int m = tml_path(path, pn, t);
    if (m < 0)
        return -1;
    if (m == 0) {
        *kind = PNX_TOML_TABLE;
        *off = *len = 0;
        return 1;
    }
    tml_step  head[TML_STEPS]; // the steps of the table the statements belong to now
    uint32_t  nhead = 0;
    tml_array arrays[TML_ARRAYS];
    uint32_t  narrays = 0;
    bool      table_seen = false;
    uint32_t  at = 0;
    tml_stmt  st;
    for (;;) {
        const int r = tml_next(s, n, at, &st);
        if (r < 0)
            return -1;
        if (r == 0)
            break;
        at = st.past;
        tml_step steps[TML_STEPS];
        uint32_t ns;
        if (st.kind == PNX_TOML_TABLE || st.kind == PNX_TOML_TABLES) {
            uint32_t w = 0; // the arrays of tables under a table that begins again are counted again
            for (uint32_t a = 0; a < narrays; ++a) {
                bool under = arrays[a].nsegs > st.nsegs;
                for (uint32_t q = 0; under && q < st.nsegs; ++q)
                    under = tml_seg_same(s, arrays[a].segs[q], st.segs[q]);
                if (!under)
                    arrays[w++] = arrays[a];
            }
            narrays = w;
            ns = 0;
            for (uint32_t q = 0; q < st.nsegs; ++q) {
                if (ns + 2 > TML_STEPS)
                    return -1;
                steps[ns++] = (tml_step){s + st.segs[q].off, st.segs[q].len, 0};
                int found = -1;
                for (uint32_t a = 0; a < narrays; ++a) {
                    bool same = arrays[a].nsegs == q + 1;
                    for (uint32_t z = 0; same && z <= q; ++z)
                        same = tml_seg_same(s, arrays[a].segs[z], st.segs[z]);
                    if (same)
                        found = (int)a;
                }
                if (st.kind == PNX_TOML_TABLES && q + 1 == st.nsegs) {
                    if (found < 0) {
                        if (narrays == TML_ARRAYS)
                            return -1;
                        found = (int)narrays++;
                        arrays[found].nsegs = st.nsegs;
                        __builtin_memcpy(arrays[found].segs, st.segs, sizeof(tml_seg) * st.nsegs);
                        arrays[found].count = 0;
                    }
                    ++arrays[found].count;
                }
                if (found >= 0)
                    steps[ns++] = (tml_step){NULL, 0, arrays[found].count - 1};
            }
            __builtin_memcpy(head, steps, sizeof(tml_step) * ns);
            nhead = ns;
        } else {
            __builtin_memcpy(steps, head, sizeof(tml_step) * nhead);
            ns = nhead;
            if (ns + st.nsegs > TML_STEPS)
                return -1;
            for (uint32_t q = 0; q < st.nsegs; ++q)
                steps[ns++] = (tml_step){s + st.segs[q].off, st.segs[q].len, 0};
        }
        uint32_t l = ns < (uint32_t)m ? ns : (uint32_t)m, same = 0;
        while (same < l && tml_same(&steps[same], &t[same]))
            ++same;
        const bool header = st.kind == PNX_TOML_TABLE || st.kind == PNX_TOML_TABLES;
        if (same == (uint32_t)m) { // the path names this statement, or a table it lies in
            if (header || ns > (uint32_t)m) {
                table_seen = true;
            } else {
                *kind = st.kind;
                *off = st.val_off;
                *len = st.val_len;
                return 1;
            }
        } else if (same == ns && !header) { // the value holds more of the path
            tml_found f;
            const int d = tml_descend(s, st.val_off, st.val_len, t, ns, (uint32_t)m, &f, 0);
            if (d < 0)
                return -1;
            if (d > 0) {
                *kind = f.kind;
                *off = f.off;
                *len = f.len;
                return 1;
            }
        }
    }
    if (table_seen) {
        *kind = PNX_TOML_TABLE;
        *off = *len = 0;
        return 1;
    }
    return 0;
}
