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

// ---- the rules of the whole document (toml.check) ------------------------------------------------------------
//
// toml.valid reads each statement alone. The rules that need the statements before it - a key is defined once, a
// table once, a table made by dotted keys is not reopened by a header, an inline table and an array are closed -
// are kept in a table of nodes in memory the caller lends (the region, given back afterwards): one node for every
// key part, found by (parent, name) through a hash, so the time is linear. A document with more nodes than the
// loan holds is `full`, never "valid". The numbers and dates of every value are checked too.

enum { TCK_IMPL, TCK_HDR, TCK_DOT, TCK_VALUE, TCK_INLINE, TCK_ARRT };

typedef struct {
    uint32_t parent, hash, off, len, next, cur;
    uint32_t sect, nel; // the end of the statements of this table's own section; the elements of an array of tables
    uint8_t  type, esc;
} tck_node;

typedef struct {
    tck_node      *nodes;
    uint32_t      *bucket;
    uint32_t       count, cap, mask;
    const uint8_t *s;
    uint32_t       n;
} tck;

#define TCK_NAME 256 // a key part with escapes longer than this decodes to nothing: the check says full

static uint32_t tck_hash(uint32_t parent, const uint8_t *p, uint32_t n) {
    uint32_t h = 2166136261u ^ (parent * 16777619u);
    for (uint32_t k = 0; k < n; ++k)
        h = (h ^ p[k]) * 16777619u;
    return h;
}

// The bytes of a key part as the key means them: quoted with escapes decoded into `buf`. -1: too long to decode.
static int tck_text(const uint8_t *s, tml_seg g, uint8_t esc, uint8_t *buf, const uint8_t **p, uint32_t *n) {
    if (!esc) {
        *p = s + g.off;
        *n = g.len;
        return 0;
    }
    if (g.len > TCK_NAME)
        return -1;
    const uint32_t m = pnx_toml_decode(s + g.off, g.len, buf);
    if (m >= PNX_TXT_BAD)
        return -2;
    *p = buf;
    *n = m;
    return 0;
}

static uint8_t tck_esc(const uint8_t *s, tml_seg g) {
    if (g.off == 0 || s[g.off - 1] != '"')
        return 0;
    for (uint32_t k = 0; k < g.len; ++k)
        if (s[g.off + k] == '\\')
            return 1;
    return 0;
}

// A node whose parent is `parent` and whose name is `name`: its index, 0 when there is none.
static uint32_t tck_find(const tck *c, uint32_t parent, uint32_t h, const uint8_t *name, uint32_t nlen) {
    for (uint32_t i = c->bucket[h & c->mask]; i; i = c->nodes[i].next) {
        const tck_node *m = &c->nodes[i];
        if (m->hash != h || m->parent != parent)
            continue;
        uint8_t        buf[TCK_NAME];
        const uint8_t *p;
        uint32_t       pn;
        if (tck_text(c->s, (tml_seg){m->off, m->len}, m->esc, buf, &p, &pn) < 0)
            continue;
        if (pn == nlen && __builtin_memcmp(p, name, nlen) == 0)
            return i;
    }
    return 0;
}

// A new node: its index, 0 when the loan is used up. `hashed` 0: it is not found by name (an element of an array of
// tables, an inline table in an array).
static uint32_t tck_make(tck *c, uint32_t parent, uint32_t h, tml_seg g, uint8_t esc, uint8_t type, bool hashed) {
    if (c->count >= c->cap)
        return 0;
    const uint32_t i = c->count++;
    c->nodes[i] = (tck_node){parent, h, g.off, g.len, 0, 0, 0, 0, type, esc};
    if (hashed) {
        c->nodes[i].next = c->bucket[h & c->mask];
        c->bucket[h & c->mask] = i;
    }
    return i;
}

static uint32_t tck_table(const tck *c, uint32_t i) { return c->nodes[i].type == TCK_ARRT ? c->nodes[i].cur : i; }

// One step down from `parent` by the key part `g`: the node, 0 when there is none, -1 too long, -2 malformed.
static int64_t tck_step(const tck *c, uint32_t parent, tml_seg g, uint8_t esc, uint32_t *hash) {
    uint8_t        buf[TCK_NAME];
    const uint8_t *p;
    uint32_t       n;
    const int      r = tck_text(c->s, g, esc, buf, &p, &n);
    if (r < 0)
        return r == -1 ? -1 : -2;
    *hash = tck_hash(parent, p, n);
    return tck_find(c, parent, *hash, p, n);
}

// ---- numbers and dates ----

static bool tck_int(const uint8_t *t, uint32_t len) {
    uint32_t k = 0;
    bool     neg = false;
    if (t[0] == '+' || t[0] == '-') {
        neg = t[0] == '-';
        k = 1;
    }
    uint32_t radix = 10;
    if (len - k > 1 && t[k] == '0' && (t[k + 1] == 'x' || t[k + 1] == 'o' || t[k + 1] == 'b')) {
        radix = t[k + 1] == 'x' ? 16 : t[k + 1] == 'o' ? 8 : 2;
        k += 2;
        neg = false;
    }
    const uint64_t limit = neg ? (uint64_t)1 << 63 : ((uint64_t)1 << 63) - 1;
    uint64_t       v = 0;
    for (; k < len; ++k) {
        if (t[k] == '_')
            continue;
        const uint8_t  b = t[k] | 0x20;
        const uint32_t d = tml_digit(t[k]) ? t[k] - '0' : b - 'a' + 10;
        if (v > (limit - d) / radix)
            return false;
        v = v * radix + d;
    }
    return true;
}

// A float must be a finite double: its size, from the digits and the exponent, against 1.7976931348623157e308.
static bool tck_float(const uint8_t *t, uint32_t len) {
    uint32_t k = 0;
    if (t[0] == '+' || t[0] == '-')
        k = 1;
    if (len - k == 3 && (t[k] == 'i' || t[k] == 'n'))
        return true; // inf and nan
    char     sig[17];
    uint32_t ns = 0, ints = 0, lead = 0;
    bool     seen = false, point = false;
    int64_t  ex = 0;
    for (; k < len; ++k) {
        const uint8_t b = t[k];
        if (b == '_')
            continue;
        if (b == '.') {
            point = true;
            continue;
        }
        if ((b | 0x20) == 'e') {
            bool eneg = false;
            ++k;
            if (k < len && (t[k] == '+' || t[k] == '-'))
                eneg = t[k++] == '-';
            for (; k < len; ++k)
                if (tml_digit(t[k]) && ex < 100000)
                    ex = ex * 10 + (t[k] - '0');
            if (eneg)
                ex = -ex;
            break;
        }
        if (!seen && b == '0') {
            if (point)
                ++lead;
            continue;
        }
        seen = true;
        if (!point)
            ++ints;
        if (ns < 17)
            sig[ns++] = (char)b;
    }
    if (!seen)
        return true; // zero
    const int64_t mag = ints ? (int64_t)ints - 1 + ex : ex - 1 - (int64_t)lead;
    if (mag > 308)
        return false;
    if (mag < 308)
        return true;
    static const char max[] = "17976931348623157";
    for (uint32_t i = 0; i < 17; ++i) {
        const char d = i < ns ? sig[i] : '0';
        if (d != max[i])
            return d < max[i];
    }
    return true;
}

// `digits` decimal digits at `at` in t, within lo..hi.
static bool tck_num(const uint8_t *t, uint32_t len, uint32_t at, uint32_t digits, uint32_t lo, uint32_t hi, uint32_t *value) {
    if (at + digits > len)
        return false;
    uint32_t v = 0;
    for (uint32_t k = 0; k < digits; ++k) {
        if (!tml_digit(t[at + k]))
            return false;
        v = v * 10 + (t[at + k] - '0');
    }
    *value = v;
    return v >= lo && v <= hi;
}

static bool tck_time(const uint8_t *t, uint32_t len, uint32_t at, uint32_t *end) {
    uint32_t h, m, sec;
    if (at + 8 > len || t[at + 2] != ':' || t[at + 5] != ':')
        return false;
    if (!tck_num(t, len, at, 2, 0, 23, &h) || !tck_num(t, len, at + 3, 2, 0, 59, &m) || !tck_num(t, len, at + 6, 2, 0, 60, &sec))
        return false;
    at += 8;
    if (at < len && t[at] == '.') {
        const uint32_t from = ++at;
        while (at < len && tml_digit(t[at]))
            ++at;
        if (at == from)
            return false;
    }
    *end = at;
    return true;
}

static bool tck_datetime(const uint8_t *t, uint32_t len) {
    uint32_t at = 0, end;
    if (len >= 3 && t[2] == ':') // a local time
        return tck_time(t, len, 0, &end) && end == len;
    uint32_t y, mo, d;
    if (len < 10 || !tck_num(t, len, 0, 4, 0, 9999, &y) || t[4] != '-' || !tck_num(t, len, 5, 2, 1, 12, &mo) ||
        t[7] != '-' || !tck_num(t, len, 8, 2, 1, 31, &d))
        return false;
    static const uint8_t days[12] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    const bool leap = (y % 4 == 0 && y % 100 != 0) || y % 400 == 0;
    if (d < 1 || d > days[mo - 1] + (mo == 2 && leap ? 1u : 0u))
        return false;
    at = 10;
    if (at == len)
        return true;
    if (t[at] != 'T' && t[at] != 't' && t[at] != ' ')
        return false;
    if (!tck_time(t, len, at + 1, &end))
        return false;
    at = end;
    if (at == len)
        return true;
    if ((t[at] == 'Z' || t[at] == 'z') && at + 1 == len)
        return true;
    uint32_t oh, om;
    return (t[at] == '+' || t[at] == '-') && at + 6 == len && t[at + 3] == ':' && tck_num(t, len, at + 1, 2, 0, 23, &oh) &&
           tck_num(t, len, at + 4, 2, 0, 59, &om);
}

static bool tck_scalar(int kind, const uint8_t *t, uint32_t len) {
    if (kind == PNX_TOML_INT)
        return tck_int(t, len);
    if (kind == PNX_TOML_FLOAT)
        return tck_float(t, len);
    if (kind == PNX_TOML_DATETIME)
        return tck_datetime(t, len);
    return true;
}

// ---- keys and values ----

#define TCK_BAD (-1)
#define TCK_FULL (-2)

static int64_t tck_assign(tck *c, uint32_t table, const tml_seg *segs, uint32_t nsegs, uint32_t vat, uint32_t depth);

// A value from its first byte: where it ends. `node` is the node of the key that holds it (an inline table fills
// it), 0 for an element of an array (an inline table there is a node nobody finds). TCK_BAD, TCK_FULL.
static int64_t tck_value(tck *c, uint32_t node, uint32_t at, uint32_t depth) {
    const uint8_t *s = c->s;
    const uint32_t n = c->n;
    if (at >= n || depth > TML_DEPTH)
        return TCK_BAD;
    if (s[at] == '[') {
        uint32_t k = at + 1;
        for (;;) {
            k = tml_gap(s, n, k, true);
            if (k >= n)
                return TCK_BAD;
            if (s[k] == ']')
                return (int64_t)k + 1;
            const int64_t e = tck_value(c, 0, k, depth + 1);
            if (e < 0)
                return e;
            k = tml_gap(s, n, (uint32_t)e, true);
            if (k < n && s[k] == ',') {
                ++k;
                continue;
            }
            if (k >= n || s[k] != ']')
                return TCK_BAD;
            return (int64_t)k + 1;
        }
    }
    if (s[at] == '{') {
        if (!node) {
            node = tck_make(c, 0, 0, (tml_seg){0, 0}, 0, TCK_INLINE, false);
            if (!node)
                return TCK_FULL;
        }
        uint32_t k = tml_gap(s, n, at + 1, false);
        if (k < n && s[k] == '}')
            return (int64_t)k + 1;
        for (;;) {
            tml_seg  segs[TML_SEGS];
            uint32_t count;
            k = tml_gap(s, n, k, false);
            if (tml_keys(s, n, &k, segs, &count) < 0)
                return TCK_BAD;
            k = tml_gap(s, n, k, false);
            if (k >= n || s[k] != '=')
                return TCK_BAD;
            k = tml_gap(s, n, k + 1, false);
            const int64_t e = tck_assign(c, node, segs, count, k, depth + 1);
            if (e < 0)
                return e;
            k = tml_gap(s, n, (uint32_t)e, false);
            if (k < n && s[k] == ',') {
                ++k;
                continue;
            }
            if (k >= n || s[k] != '}')
                return TCK_BAD;
            return (int64_t)k + 1;
        }
    }
    int64_t e;
    int     kind = 0;
    if (s[at] == '"' || s[at] == '\'') {
        bool multi;
        e = tml_string(s, n, at, &multi);
    } else {
        uint32_t end = 0;
        kind = tml_token(s, n, at, &end);
        e = kind < 0 ? -1 : (int64_t)end;
    }
    if (e < 0 || !tck_scalar(kind, s + at, (uint32_t)e - at))
        return TCK_BAD;
    return e;
}

// key parts `segs` set down from `table`, then the value at `vat`: the end of the value. The parts before the last
// are tables made by the dotted key (new, or made so before); the last must be new.
static int64_t tck_assign(tck *c, uint32_t table, const tml_seg *segs, uint32_t nsegs, uint32_t vat, uint32_t depth) {
    uint32_t parent = table;
    for (uint32_t i = 0; i < nsegs; ++i) {
        const uint8_t esc = tck_esc(c->s, segs[i]);
        uint32_t      h;
        const int64_t f = tck_step(c, parent, segs[i], esc, &h);
        if (f == -1)
            return TCK_FULL;
        if (f == -2)
            return TCK_BAD;
        const bool last = i + 1 == nsegs;
        if (!last) {
            uint32_t m = (uint32_t)f;
            if (!m) {
                m = tck_make(c, parent, h, segs[i], esc, TCK_DOT, true);
                if (!m)
                    return TCK_FULL;
            } else if (c->nodes[m].type != TCK_DOT) {
                return TCK_BAD;
            }
            parent = m;
            continue;
        }
        if (f)
            return TCK_BAD; // a second definition
        const bool inline_table = vat < c->n && c->s[vat] == '{';
        const uint32_t m = tck_make(c, parent, h, segs[i], esc, inline_table ? TCK_INLINE : TCK_VALUE, true);
        if (!m)
            return TCK_FULL;
        return tck_value(c, m, vat, depth);
    }
    return TCK_BAD;
}

// A header: the table it opens, 0 for the root, TCK_BAD, TCK_FULL.
static int64_t tck_header(tck *c, const tml_seg *segs, uint32_t nsegs, bool dbl) {
    uint32_t parent = 0;
    for (uint32_t i = 0; i < nsegs; ++i) {
        const uint8_t esc = tck_esc(c->s, segs[i]);
        uint32_t      h;
        const int64_t f = tck_step(c, parent, segs[i], esc, &h);
        if (f == -1)
            return TCK_FULL;
        if (f == -2)
            return TCK_BAD;
        uint32_t m = (uint32_t)f;
        if (i + 1 < nsegs) {
            if (!m) {
                m = tck_make(c, parent, h, segs[i], esc, TCK_IMPL, true);
                if (!m)
                    return TCK_FULL;
            } else if (c->nodes[m].type == TCK_VALUE || c->nodes[m].type == TCK_INLINE) {
                return TCK_BAD;
            }
            parent = tck_table(c, m);
            continue;
        }
        if (dbl) {
            if (!m) {
                m = tck_make(c, parent, h, segs[i], esc, TCK_ARRT, true);
                if (!m)
                    return TCK_FULL;
            } else if (c->nodes[m].type != TCK_ARRT) {
                return TCK_BAD;
            }
            const uint32_t el = tck_make(c, m, ++c->nodes[m].nel, (tml_seg){0, 0}, 0, TCK_HDR, false);
            if (!el)
                return TCK_FULL;
            c->nodes[m].cur = el;
            return el;
        }
        if (!m) {
            m = tck_make(c, parent, h, segs[i], esc, TCK_HDR, true);
            if (!m)
                return TCK_FULL;
        } else if (c->nodes[m].type == TCK_IMPL) {
            c->nodes[m].type = TCK_HDR;
        } else {
            return TCK_BAD;
        }
        return m;
    }
    return TCK_BAD;
}

// Memory the check wants for a text of n bytes: at most one node for every byte, and its bucket.
size_t pnx_toml_check_room(uint32_t n) { return ((size_t)n + 16) * (sizeof(tck_node) + 8) + 16; }

static bool tck_init(tck *c, const uint8_t *s, uint32_t n, uint8_t *mem, size_t memlen) {
    const uintptr_t pad = (8 - ((uintptr_t)mem & 7)) & 7;
    if (memlen < pad + 4 * sizeof(tck_node))
        return false;
    mem += pad;
    memlen -= pad;
    const uint32_t m = (uint32_t)(memlen / (sizeof(tck_node) + 8));
    uint32_t buckets = 1;
    while (buckets * 2 <= m && buckets < (1u << 24))
        buckets *= 2;
    const size_t tail = (memlen - (size_t)buckets * 4) & ~(size_t)7; // where the buckets begin
    *c = (tck){(tck_node *)mem, (uint32_t *)(mem + tail), 1, (uint32_t)(tail / sizeof(tck_node)), buckets - 1, s, n};
    __builtin_memset(c->bucket, 0, (size_t)buckets * 4);
    c->nodes[0] = (tck_node){0, 0, 0, 0, 0, 0, 0, 0, TCK_HDR, 0}; // the root
    return true;
}

// Every statement of the text against the rules: 0, 1 it breaks one, 2 the loan is used up.
static int tck_run(tck *c) {
    uint32_t cur = 0, at = 0;
    tml_stmt st;
    for (;;) {
        const int r = tml_next(c->s, c->n, at, &st);
        if (r < 0)
            return 1;
        if (r == 0)
            return 0;
        at = st.past;
        int64_t e;
        if (st.kind == PNX_TOML_TABLE || st.kind == PNX_TOML_TABLES) {
            e = tck_header(c, st.segs, st.nsegs, st.kind == PNX_TOML_TABLES);
            if (e >= 0)
                cur = (uint32_t)e;
        } else {
            e = tck_assign(c, cur, st.segs, st.nsegs, st.val_off, 0);
        }
        if (e == TCK_BAD)
            return 1;
        if (e == TCK_FULL)
            return 2;
        c->nodes[cur].sect = st.past;
    }
}

// 0: the document keeps every rule; 1: it does not; 2: `mem` is too small to say.
int pnx_toml_check(const uint8_t *s, uint32_t n, uint8_t *mem, size_t memlen) {
    tck c;
    if (!tck_init(&c, s, n, mem, memlen))
        return 2;
    return tck_run(&c);
}

// ---- writing: toml.set and toml.remove ------------------------------------------------------------------------
//
// The document is copied with one change; every other byte, comments and layout included, stays. A value is written
// on one line: a string quoted and escaped, an int, float, bool or date as the text the program gives (it must be
// one), an array of one line. The result must keep the rules of toml.check, and is not given otherwise.

typedef struct {
    uint8_t *out;
    uint64_t cap, len;
} tml_w;

static void tw_put(tml_w *w, const uint8_t *p, uint64_t n) {
    if (w->out && w->len + n <= w->cap && n)
        __builtin_memcpy(w->out + w->len, p, n);
    w->len += n;
}
static void tw_str(tml_w *w, const char *z) { tw_put(w, (const uint8_t *)z, __builtin_strlen(z)); }

// UTF-8 of one code point at p (n bytes left): its length, 0 when it is not well formed.
static uint32_t tml_utf8(const uint8_t *p, uint32_t n) {
    if (p[0] < 0x80)
        return 1;
    uint32_t len, cp;
    if (p[0] >= 0xC2 && p[0] <= 0xDF) {
        len = 2;
        cp = p[0] & 0x1F;
    } else if ((p[0] & 0xF0) == 0xE0) {
        len = 3;
        cp = p[0] & 0x0F;
    } else if (p[0] >= 0xF0 && p[0] <= 0xF4) {
        len = 4;
        cp = p[0] & 0x07;
    } else {
        return 0;
    }
    if (len > n)
        return 0;
    for (uint32_t k = 1; k < len; ++k) {
        if ((p[k] & 0xC0) != 0x80)
            return 0;
        cp = cp << 6 | (p[k] & 0x3F);
    }
    if ((len == 3 && cp < 0x800) || (len == 4 && (cp < 0x10000 || cp > 0x10FFFF)) || (cp >= 0xD800 && cp <= 0xDFFF))
        return 0;
    return len;
}

// The content of a basic string for the bytes p: escapes where TOML wants them. false: not UTF-8.
static bool tw_escaped(tml_w *w, const uint8_t *p, uint32_t n) {
    static const char hex[] = "0123456789ABCDEF";
    for (uint32_t k = 0; k < n;) {
        const uint8_t b = p[k];
        const uint32_t u = tml_utf8(p + k, n - k);
        if (!u)
            return false;
        if (u > 1) {
            tw_put(w, p + k, u);
            k += u;
            continue;
        }
        ++k;
        switch (b) {
        case '"': tw_str(w, "\\\""); break;
        case '\\': tw_str(w, "\\\\"); break;
        case '\b': tw_str(w, "\\b"); break;
        case '\t': tw_str(w, "\\t"); break;
        case '\n': tw_str(w, "\\n"); break;
        case '\f': tw_str(w, "\\f"); break;
        case '\r': tw_str(w, "\\r"); break;
        default:
            if (b < 0x20 || b == 0x7F) {
                const uint8_t e[6] = {'\\', 'u', '0', '0', (uint8_t)hex[b >> 4], (uint8_t)hex[b & 15]};
                tw_put(w, e, 6);
            } else {
                tw_put(w, &b, 1);
            }
        }
    }
    return true;
}

// A value of `kind` as text on one line: false when it is not one.
static bool tw_value(tml_w *w, int kind, const uint8_t *v, uint32_t vn) {
    if (kind == PNX_TOML_STRING) {
        tw_str(w, "\"");
        if (!tw_escaped(w, v, vn))
            return false;
        tw_str(w, "\"");
        return true;
    }
    if (vn == 0)
        return false;
    for (uint32_t k = 0; k < vn; ++k)
        if (v[k] == '\n' || v[k] == '\r')
            return false;
    int k2 = 0;
    if (kind == PNX_TOML_ARRAY) {
        if (v[0] != '[')
            return false;
        const int64_t e = tml_value(v, vn, 0, 0, &k2);
        if (e != (int64_t)vn || k2 != PNX_TOML_ARRAY)
            return false;
    } else if (kind == PNX_TOML_INT || kind == PNX_TOML_FLOAT || kind == PNX_TOML_BOOL || kind == PNX_TOML_DATETIME) {
        uint32_t end = 0;
        const int t = tml_token(v, vn, 0, &end);
        if (t != kind || end != vn || !tck_scalar(kind, v, vn))
            return false;
    } else {
        return false;
    }
    tw_put(w, v, vn);
    return true;
}

// A key part: bare when it can be, else quoted.
static bool tw_key(tml_w *w, const uint8_t *p, uint32_t n) {
    bool bare = n > 0;
    for (uint32_t k = 0; k < n; ++k)
        bare = bare && tml_bare(p[k]);
    if (bare) {
        tw_put(w, p, n);
        return true;
    }
    tw_str(w, "\"");
    if (!tw_escaped(w, p, n))
        return false;
    tw_str(w, "\"");
    return true;
}

static bool tw_keys(tml_w *w, const tml_step *t, uint32_t from, uint32_t to) {
    for (uint32_t i = from; i < to; ++i) {
        if (i > from)
            tw_str(w, ".");
        if (!tw_key(w, t[i].key, t[i].key_len))
            return false;
    }
    return true;
}

// The result of a writer: 0, 1 format, 2 full, 3 not found.
enum { TW_OK = 0, TW_FORMAT, TW_FULL, TW_NONE };

static int tw_finish(tml_w *w, uint32_t *outlen, uint8_t *mem, size_t memlen) {
    if (w->len > w->cap)
        return TW_FULL;
    const int r = pnx_toml_check(w->out, (uint32_t)w->len, mem, memlen);
    if (r)
        return r == 1 ? TW_FORMAT : TW_FULL;
    *outlen = (uint32_t)w->len;
    return TW_OK;
}

// Memory toml.set and toml.remove want for a text of n bytes that grows by at most `extra`.
size_t pnx_toml_write_room(uint32_t n, uint32_t extra) { return pnx_toml_check_room(n + extra); }

int pnx_toml_set(const uint8_t *s, uint32_t n, const uint8_t *path, uint32_t pn, int kind, const uint8_t *val, uint32_t vn,
                 uint8_t *out, uint64_t cap, uint32_t *outlen, uint8_t *mem, size_t memlen) {
    tml_w probe = {NULL, 0, 0};
    if (!tw_value(&probe, kind, val, vn))
        return TW_FORMAT;
    tml_step steps[TML_STEPS];
    const int ns = tml_path(path, pn, steps);
    if (ns <= 0)
        return TW_FORMAT;
    int      fk = 0;
    uint32_t fo = 0, fl = 0;
    const int found = pnx_toml_at(s, n, path, pn, &fk, &fo, &fl);
    if (found < 0 || (found == 1 && fk == PNX_TOML_TABLE))
        return TW_FORMAT;
    tml_w w = {out, cap, 0};
    if (found == 1) { // a value that is there: replaced
        tw_put(&w, s, fo);
        tw_value(&w, kind, val, vn);
        tw_put(&w, s + fo + fl, n - fo - fl);
        return tw_finish(&w, outlen, mem, memlen);
    }
    tck c;
    if (!tck_init(&c, s, n, mem, memlen))
        return TW_FULL;
    const int rr = tck_run(&c);
    if (rr)
        return rr == 1 ? TW_FORMAT : TW_FULL;
    uint32_t parent = 0, used = 0, own = 0, ownnode = 0;
    int      i = 0;
    for (; i < ns; ++i) {
        if (!steps[i].key)
            return TW_NONE; // an index of something that is not there
        const uint32_t h = tck_hash(parent, steps[i].key, steps[i].key_len);
        uint32_t       m = tck_find(&c, parent, h, steps[i].key, steps[i].key_len);
        if (!m)
            break;
        const uint8_t ty = c.nodes[m].type;
        if (ty == TCK_VALUE || ty == TCK_INLINE)
            return TW_FORMAT; // nothing can be added to a value
        used = (uint32_t)i + 1;
        if (ty == TCK_ARRT) {
            if (i + 1 >= ns || steps[i + 1].key)
                return TW_NONE;
            uint32_t el = 0;
            for (uint32_t k = 1; k < c.count && !el; ++k)
                if (c.nodes[k].parent == m && c.nodes[k].hash == steps[i + 1].index + 1u && c.nodes[k].type == TCK_HDR)
                    el = k;
            if (!el)
                return TW_NONE;
            ++i;
            used = (uint32_t)i + 1;
            m = el;
            own = used;
            ownnode = m;
        } else if (ty == TCK_HDR) {
            own = used;
            ownnode = m;
        }
        parent = m;
    }
    for (int k = i; k < ns; ++k)
        if (!steps[k].key)
            return TW_NONE;
    if (i >= ns)
        return TW_FORMAT; // it is there after all, as something that is not a value
    const uint8_t pty = c.nodes[parent].type;
    if (parent == 0 || pty == TCK_HDR) {
        own = used;
        ownnode = parent;
    }
    if (pty == TCK_HDR || parent == 0 || pty == TCK_DOT) {
        // the key goes at the end of the statements of the table that owns the place, dotted from there
        const uint32_t at = c.nodes[ownnode].sect;
        tw_put(&w, s, at);
        if (at > 0 && s[at - 1] != '\n')
            tw_str(&w, "\n");
        if (!tw_keys(&w, steps, own, (uint32_t)ns))
            return TW_FORMAT;
        tw_str(&w, " = ");
        tw_value(&w, kind, val, vn);
        tw_str(&w, "\n");
        tw_put(&w, s + at, n - at);
        return tw_finish(&w, outlen, mem, memlen);
    }
    // an implicit table (made only by the headers of tables below it) has no section: a header of its own at the end
    for (uint32_t k = 0; k < used; ++k)
        if (!steps[k].key)
            return TW_FORMAT;
    tw_put(&w, s, n);
    if (n > 0) {
        if (s[n - 1] != '\n')
            tw_str(&w, "\n");
        tw_str(&w, "\n");
    }
    tw_str(&w, "[");
    if (!tw_keys(&w, steps, 0, used))
        return TW_FORMAT;
    tw_str(&w, "]\n");
    if (!tw_keys(&w, steps, used, (uint32_t)ns))
        return TW_FORMAT;
    tw_str(&w, " = ");
    tw_value(&w, kind, val, vn);
    tw_str(&w, "\n");
    return tw_finish(&w, outlen, mem, memlen);
}

int pnx_toml_remove(const uint8_t *s, uint32_t n, const uint8_t *path, uint32_t pn, uint8_t *out, uint64_t cap, uint32_t *outlen,
                    uint8_t *mem, size_t memlen) {
    int      fk = 0;
    uint32_t fo = 0, fl = 0;
    const int found = pnx_toml_at(s, n, path, pn, &fk, &fo, &fl);
    if (found < 0)
        return TW_FORMAT;
    if (found == 0)
        return TW_NONE;
    if (fk == PNX_TOML_TABLE)
        return TW_FORMAT;
    uint32_t at = 0;
    tml_stmt st;
    for (;;) {
        const int r = tml_next(s, n, at, &st);
        if (r < 0)
            return TW_FORMAT;
        if (r == 0)
            return TW_FORMAT; // the value is inside another one (an element of an array or an inline table)
        if (st.kind != PNX_TOML_TABLE && st.kind != PNX_TOML_TABLES && st.val_off == fo && st.val_len == fl)
            break;
        at = st.past;
    }
    uint32_t from = st.key_off;
    while (from > 0 && s[from - 1] != '\n')
        --from;
    tml_w w = {out, cap, 0};
    tw_put(&w, s, from);
    tw_put(&w, s + st.past, n - st.past);
    return tw_finish(&w, outlen, mem, memlen);
}
