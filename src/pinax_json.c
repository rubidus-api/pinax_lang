// pinax_json.c - the text formats of the modules json and csv (RFC-0009 section 4.5): read in one
// direction, a token or a field at a time, written straight into a buffer. No tree is built.
#include "pinax_internal.h"

static bool jsn_space(uint8_t b) { return b == ' ' || b == '\t' || b == '\n' || b == '\r'; }

static bool jsn_literal(const uint8_t *s, uint32_t n, uint32_t at, const char *w, uint32_t len) {
    if (n - at < len || __builtin_memcmp(s + at, w, len) != 0)
        return false;
    return at + len == n || !((s[at + len] | 0x20) >= 'a' && (s[at + len] | 0x20) <= 'z');
}

// The next token of `s` (n bytes): its kind (PNX_JSON_...), the bytes of its value (a string
// without its quotes, still escaped when *escaped), and how far it reaches (`past`). Commas and
// colons between tokens are passed over; a string followed by a colon is a key. 0 at the end
// of the input, -1 when what follows is no token.
int pnx_json_next(const uint8_t *s, uint32_t n, uint32_t *start, uint32_t *len, bool *escaped, uint32_t *past) {
    uint32_t at = 0;
    while (at < n && (jsn_space(s[at]) || s[at] == ',' || s[at] == ':'))
        ++at;
    *escaped = false;
    if (at == n)
        return 0;
    const uint8_t b = s[at];
    *start = at;
    *len = 1;
    *past = at + 1;
    switch (b) {
    case '{': return PNX_JSON_OBJECT;
    case '}': return PNX_JSON_OBJECT_END;
    case '[': return PNX_JSON_ARRAY;
    case ']': return PNX_JSON_ARRAY_END;
    case '"': {
        uint32_t k = at + 1;
        while (k < n && s[k] != '"') {
            if (s[k] < 0x20)
                return -1;
            if (s[k] == '\\') {
                *escaped = true;
                if (++k == n)
                    return -1;
            }
            ++k;
        }
        if (k == n)
            return -1;
        *start = at + 1;
        *len = k - at - 1;
        *past = k + 1;
        uint32_t m = k + 1;
        while (m < n && jsn_space(s[m]))
            ++m;
        if (m < n && s[m] == ':') {
            *past = m + 1;
            return PNX_JSON_KEY;
        }
        return PNX_JSON_STRING;
    }
    case 't': case 'f': case 'n': {
        const bool t = jsn_literal(s, n, at, "true", 4), f = jsn_literal(s, n, at, "false", 5), z = jsn_literal(s, n, at, "null", 4);
        if (!t && !f && !z)
            return -1;
        *len = t || z ? 4 : 5;
        *past = at + *len;
        return z ? PNX_JSON_NULL : PNX_JSON_BOOLEAN;
    }
    default: { // a number: -? digits ( . digits )? ( [eE] [+-]? digits )?
        uint32_t k = at;
        if (k < n && s[k] == '-')
            ++k;
        const uint32_t d0 = k;
        while (k < n && s[k] >= '0' && s[k] <= '9')
            ++k;
        if (k == d0)
            return -1;
        if (k < n && s[k] == '.') {
            const uint32_t d1 = ++k;
            while (k < n && s[k] >= '0' && s[k] <= '9')
                ++k;
            if (k == d1)
                return -1;
        }
        if (k < n && (s[k] | 0x20) == 'e') {
            ++k;
            if (k < n && (s[k] == '+' || s[k] == '-'))
                ++k;
            const uint32_t d2 = k;
            while (k < n && s[k] >= '0' && s[k] <= '9')
                ++k;
            if (k == d2)
                return -1;
        }
        *len = k - at;
        *past = k;
        return PNX_JSON_NUMBER;
    }
    }
}

// How far one whole value reaches: a token, or an object or array with all it holds. -1 when
// it is malformed or the input ends inside it; 0 when there is no value.
int pnx_json_skip(const uint8_t *s, uint32_t n, uint32_t *past) {
    uint32_t at = 0, depth = 0;
    for (;;) {
        uint32_t start, len, step;
        bool     escaped;
        const int kind = pnx_json_next(s + at, n - at, &start, &len, &escaped, &step);
        if (kind <= 0)
            return depth || kind < 0 || at ? -1 : 0;
        at += step;
        if (kind == PNX_JSON_OBJECT || kind == PNX_JSON_ARRAY) {
            ++depth;
        } else if (kind == PNX_JSON_OBJECT_END || kind == PNX_JSON_ARRAY_END) {
            if (depth == 0)
                return -1;
            --depth;
        } else if (kind == PNX_JSON_KEY) {
            continue; // a key, and then its value
        }
        if (depth == 0)
            break;
    }
    *past = at;
    return 1;
}

static int jsn_hex4(const uint8_t *p) {
    int v = 0;
    for (int k = 0; k < 4; ++k) {
        const uint8_t c = p[k];
        const int     d = c >= '0' && c <= '9' ? c - '0' : (c | 0x20) >= 'a' && (c | 0x20) <= 'f' ? (c | 0x20) - 'a' + 10 : -1;
        if (d < 0)
            return -1;
        v = v * 16 + d;
    }
    return v;
}

// The bytes of an escaped string into `out` (never longer than the input): their count, or
// UINT32_MAX when an escape is malformed. \uXXXX becomes UTF-8; a lone surrogate is malformed.
uint32_t pnx_json_unescape(const uint8_t *in, uint32_t n, uint8_t *out) {
    uint32_t o = 0;
    for (uint32_t k = 0; k < n; ++k) {
        if (in[k] != '\\') {
            out[o++] = in[k];
            continue;
        }
        if (++k == n)
            return UINT32_MAX;
        switch (in[k]) {
        case '"': out[o++] = '"'; break;
        case '\\': out[o++] = '\\'; break;
        case '/': out[o++] = '/'; break;
        case 'b': out[o++] = '\b'; break;
        case 'f': out[o++] = '\f'; break;
        case 'n': out[o++] = '\n'; break;
        case 'r': out[o++] = '\r'; break;
        case 't': out[o++] = '\t'; break;
        case 'u': {
            if (n - k < 5)
                return UINT32_MAX;
            int32_t cp = jsn_hex4(in + k + 1);
            k += 4;
            if (cp < 0 || (cp >= 0xdc00 && cp <= 0xdfff))
                return UINT32_MAX;
            if (cp >= 0xd800 && cp <= 0xdbff) { // a pair
                if (n - k < 7 || in[k + 1] != '\\' || in[k + 2] != 'u')
                    return UINT32_MAX;
                const int lo = jsn_hex4(in + k + 3);
                if (lo < 0xdc00 || lo > 0xdfff)
                    return UINT32_MAX;
                cp = 0x10000 + ((cp - 0xd800) << 10) + (lo - 0xdc00);
                k += 6;
            }
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
        default: return UINT32_MAX;
        }
    }
    return o;
}

// `in` as a JSON string with its quotes, into `out` (`cap` bytes): its length, or UINT32_MAX
// when it does not fit. Control bytes become \uXXXX; other bytes go as they are.
uint32_t pnx_json_quote(const uint8_t *in, uint32_t n, uint8_t *out, uint64_t cap) {
    static const char hex[] = "0123456789abcdef";
    uint64_t o = 0;
#define PUT(b)                                                                                     \
    do {                                                                                           \
        if (o == cap)                                                                              \
            return UINT32_MAX;                                                                     \
        out[o++] = (uint8_t)(b);                                                                   \
    } while (0)
    PUT('"');
    for (uint32_t k = 0; k < n; ++k) {
        const uint8_t b = in[k];
        if (b == '"' || b == '\\') {
            PUT('\\');
            PUT(b);
        } else if (b == '\n') {
            PUT('\\');
            PUT('n');
        } else if (b == '\t') {
            PUT('\\');
            PUT('t');
        } else if (b == '\r') {
            PUT('\\');
            PUT('r');
        } else if (b < 0x20) {
            PUT('\\');
            PUT('u');
            PUT('0');
            PUT('0');
            PUT(hex[b >> 4]);
            PUT(hex[b & 15]);
        } else {
            PUT(b);
        }
    }
    PUT('"');
#undef PUT
    return o > UINT32_MAX - 1 ? UINT32_MAX : (uint32_t)o;
}

// ---- csv ----------------------------------------------------------------------------------

// One field at the front of `s`: where its text lies (`start`, `len`; with doubled quotes when
// *doubled), how far it reaches with its separator (`past`), and whether the record ends with
// it (a line break or the end of the input). -1 when a quoted field is not closed or is
// followed by something other than a separator.
int pnx_csv_field(const uint8_t *s, uint32_t n, uint32_t *start, uint32_t *len, bool *doubled, uint32_t *past, bool *last) {
    *doubled = false;
    uint32_t k = 0;
    if (n && s[0] == '"') {
        k = 1;
        for (;;) {
            if (k >= n)
                return -1;
            if (s[k] == '"') {
                if (k + 1 < n && s[k + 1] == '"') {
                    *doubled = true;
                    k += 2;
                    continue;
                }
                break;
            }
            ++k;
        }
        *start = 1;
        *len = k - 1;
        ++k; // past the closing quote
    } else {
        while (k < n && s[k] != ',' && s[k] != '\n' && !(s[k] == '\r' && (k + 1 == n || s[k + 1] == '\n')))
            ++k;
        *start = 0;
        *len = k;
    }
    if (k == n) {
        *past = k;
        *last = true;
        return 1;
    }
    if (s[k] == ',') {
        *past = k + 1;
        *last = false;
        return 1;
    }
    if (s[k] == '\r' && k + 1 < n && s[k + 1] == '\n')
        ++k;
    else if (s[k] == '\r' && k + 1 == n)
        ;
    else if (s[k] != '\n')
        return -1;
    *past = k + 1;
    *last = true;
    return 1;
}

// `in` as a CSV field into `out`: quoted (quotes doubled) when it holds a comma, a quote or a
// line break. Its length, or UINT32_MAX when it does not fit.
uint32_t pnx_csv_quote(const uint8_t *in, uint32_t n, uint8_t *out, uint64_t cap) {
    bool quote = false;
    for (uint32_t k = 0; k < n && !quote; ++k)
        quote = in[k] == ',' || in[k] == '"' || in[k] == '\n' || in[k] == '\r';
    uint64_t o = 0;
    if (!quote) {
        if (n > cap)
            return UINT32_MAX;
        __builtin_memmove(out, in, n);
        return n;
    }
    if (o == cap)
        return UINT32_MAX;
    out[o++] = '"';
    for (uint32_t k = 0; k < n; ++k) {
        if (o + (in[k] == '"' ? 2 : 1) > cap)
            return UINT32_MAX;
        if (in[k] == '"')
            out[o++] = '"';
        out[o++] = in[k];
    }
    if (o == cap)
        return UINT32_MAX;
    out[o++] = '"';
    return (uint32_t)o;
}

// ---- RFC-0046: a value by path, validity, minify and pretty ---------------------------------------------

// Strict JSON (RFC 8259) for `valid`, `minify` and `pretty`; json.next above is lenient on purpose (it reads a
// stream). Depth is bounded: a text nested deeper than 64 is not valid here.
static bool jsx_string(const uint8_t *s, uint32_t n, uint32_t *at) {
    uint32_t k = *at + 1;
    while (k < n && s[k] != '"') {
        if (s[k] < 0x20)
            return false;
        if (s[k] == '\\') {
            if (++k == n)
                return false;
            if (s[k] == 'u') {
                if (n - k < 5)
                    return false;
                for (uint32_t d = 1; d <= 4; ++d) {
                    const uint8_t c = s[k + d];
                    if (!((c >= '0' && c <= '9') || ((c | 0x20) >= 'a' && (c | 0x20) <= 'f')))
                        return false;
                }
                k += 4;
            } else if (!__builtin_memchr("\"\\/bfnrt", s[k], 8)) {
                return false;
            }
        }
        ++k;
    }
    if (k == n)
        return false;
    *at = k + 1;
    return true;
}

static void jsx_ws(const uint8_t *s, uint32_t n, uint32_t *at) {
    while (*at < n && jsn_space(s[*at]))
        ++*at;
}

static bool jsx_value(const uint8_t *s, uint32_t n, uint32_t *at, uint32_t depth) {
    if (depth > 64)
        return false;
    jsx_ws(s, n, at);
    if (*at == n)
        return false;
    const uint8_t b = s[*at];
    if (b == '"')
        return jsx_string(s, n, at);
    if (b == '{' || b == '[') {
        const uint8_t close = b == '{' ? '}' : ']';
        ++*at;
        jsx_ws(s, n, at);
        if (*at < n && s[*at] == close) {
            ++*at;
            return true;
        }
        for (;;) {
            if (b == '{') {
                jsx_ws(s, n, at);
                if (*at == n || s[*at] != '"' || !jsx_string(s, n, at))
                    return false;
                jsx_ws(s, n, at);
                if (*at == n || s[*at] != ':')
                    return false;
                ++*at;
            }
            if (!jsx_value(s, n, at, depth + 1))
                return false;
            jsx_ws(s, n, at);
            if (*at == n)
                return false;
            if (s[*at] == ',') {
                ++*at;
                continue;
            }
            if (s[*at] != close)
                return false;
            ++*at;
            return true;
        }
    }
    if (b == 't' || b == 'f' || b == 'n') {
        const char *w = b == 't' ? "true" : b == 'f' ? "false" : "null";
        const uint32_t len = b == 'f' ? 5 : 4;
        if (n - *at < len || __builtin_memcmp(s + *at, w, len) != 0)
            return false;
        *at += len;
        return true;
    }
    uint32_t k = *at;
    if (k < n && s[k] == '-')
        ++k;
    if (k == n || s[k] < '0' || s[k] > '9')
        return false;
    if (s[k] == '0') {
        ++k;
    } else {
        while (k < n && s[k] >= '0' && s[k] <= '9')
            ++k;
    }
    if (k < n && s[k] == '.') {
        const uint32_t d = ++k;
        while (k < n && s[k] >= '0' && s[k] <= '9')
            ++k;
        if (k == d)
            return false;
    }
    if (k < n && (s[k] | 0x20) == 'e') {
        ++k;
        if (k < n && (s[k] == '+' || s[k] == '-'))
            ++k;
        const uint32_t d = k;
        while (k < n && s[k] >= '0' && s[k] <= '9')
            ++k;
        if (k == d)
            return false;
    }
    *at = k;
    return true;
}

// One JSON value and nothing else but white space.
bool pnx_json_valid(const uint8_t *s, uint32_t n) {
    uint32_t at = 0;
    if (!jsx_value(s, n, &at, 0))
        return false;
    jsx_ws(s, n, &at);
    return at == n;
}

typedef struct {
    uint8_t *out;
    uint64_t cap, len;
    bool     full;
} jsx_out;
static void jsx_put(jsx_out *o, uint8_t b) {
    if (o->len < o->cap)
        o->out[o->len] = b;
    else
        o->full = true;
    ++o->len;
}

// The same value without the white space between its tokens. PNX_TXT_BAD when it is not valid JSON, PNX_TXT_FULL
// when the output does not fit (the bytes it needs are not counted).
uint32_t pnx_json_minify(const uint8_t *s, uint32_t n, uint8_t *out, uint64_t cap) {
    if (!pnx_json_valid(s, n))
        return PNX_TXT_BAD;
    jsx_out o = {out, cap, 0, false};
    bool    in_string = false;
    for (uint32_t k = 0; k < n; ++k) {
        const uint8_t b = s[k];
        if (in_string) {
            jsx_put(&o, b);
            if (b == '\\')
                jsx_put(&o, s[++k]);
            else if (b == '"')
                in_string = false;
        } else if (!jsn_space(b)) {
            jsx_put(&o, b);
            in_string = b == '"';
        }
    }
    return o.full || o.len >= PNX_TXT_BAD ? PNX_TXT_FULL : (uint32_t)o.len;
}

static void jsx_newline(jsx_out *o, uint32_t indent, uint32_t depth) {
    jsx_put(o, '\n');
    for (uint64_t k = 0; k < (uint64_t)indent * depth && !o->full; ++k)
        jsx_put(o, ' ');
}

// The value laid out one member or element to a line, `indent` spaces for each level (0 to 16); an empty object or
// array stays as it is.
uint32_t pnx_json_pretty(const uint8_t *s, uint32_t n, uint32_t indent, uint8_t *out, uint64_t cap) {
    if (indent > 16 || !pnx_json_valid(s, n))
        return PNX_TXT_BAD;
    jsx_out  o = {out, cap, 0, false};
    uint32_t depth = 0;
    for (uint32_t k = 0; k < n; ++k) {
        const uint8_t b = s[k];
        if (jsn_space(b))
            continue;
        if (b == '"') {
            uint32_t e = k;
            (void)jsx_string(s, n, &e);
            for (; k < e; ++k)
                jsx_put(&o, s[k]);
            --k;
        } else if (b == '{' || b == '[') {
            uint32_t m = k + 1;
            while (m < n && jsn_space(s[m]))
                ++m;
            jsx_put(&o, b);
            if (m < n && s[m] == (b == '{' ? '}' : ']')) {
                jsx_put(&o, s[m]);
                k = m;
            } else {
                ++depth;
                jsx_newline(&o, indent, depth);
            }
        } else if (b == '}' || b == ']') {
            --depth;
            jsx_newline(&o, indent, depth);
            jsx_put(&o, b);
        } else if (b == ',') {
            jsx_put(&o, b);
            jsx_newline(&o, indent, depth);
        } else if (b == ':') {
            jsx_put(&o, b);
            jsx_put(&o, ' ');
        } else {
            jsx_put(&o, b);
        }
        if (o.full)
            return PNX_TXT_FULL;
    }
    return o.full || o.len >= PNX_TXT_BAD ? PNX_TXT_FULL : (uint32_t)o.len;
}

// A path: names joined by dots and indexes in brackets (`a.b[2].c`), a name that holds a dot or a bracket as
// ["a.b"]. Gives 1 and the next piece (a name or an index), 0 at the end, -1 when the path is malformed.
typedef struct {
    const uint8_t *key;
    uint32_t       key_len;
    int64_t        index; // -1: a name
} jsx_step;
static int jsx_path_step(const uint8_t *p, uint32_t n, uint32_t *at, jsx_step *step, bool first) {
    if (*at == n)
        return 0;
    if (p[*at] == '.') {
        ++*at;
    } else if (!first && p[*at] != '[') {
        return -1;
    }
    if (*at < n && p[*at] == '[') {
        ++*at;
        if (*at < n && p[*at] == '"') {
            const uint32_t s0 = ++*at;
            while (*at < n && p[*at] != '"')
                ++*at;
            if (*at + 1 >= n || p[*at + 1] != ']')
                return -1;
            step->key = p + s0;
            step->key_len = *at - s0;
            step->index = -1;
            *at += 2;
            return 1;
        }
        int64_t v = 0;
        const uint32_t d0 = *at;
        while (*at < n && p[*at] >= '0' && p[*at] <= '9') {
            v = v * 10 + (p[*at] - '0');
            if (v > INT32_MAX)
                return -1;
            ++*at;
        }
        if (*at == d0 || *at == n || p[*at] != ']')
            return -1;
        ++*at;
        step->index = v;
        step->key = NULL;
        step->key_len = 0;
        return 1;
    }
    const uint32_t s0 = *at;
    while (*at < n && p[*at] != '.' && p[*at] != '[')
        ++*at;
    if (*at == s0)
        return -1;
    step->key = p + s0;
    step->key_len = *at - s0;
    step->index = -1;
    return 1;
}

// The value `path` names in `s`: 1 found (kind, and the bytes of the value in *start and *len, a string without its
// quotes and still escaped when *escaped; an object or array whole), 0 when there is none, -1 when the text or the
// path is malformed where it was read.
int pnx_json_at(const uint8_t *s, uint32_t n, const uint8_t *path, uint32_t pn, int *kind, uint32_t *start, uint32_t *len,
                bool *escaped) {
    uint32_t base = 0, at = 0;
    jsx_step step;
    bool     first = true;
    for (;;) {
        const int r = jsx_path_step(path, pn, &at, &step, first);
        first = false;
        if (r < 0)
            return -1;
        if (r == 0)
            break;
        uint32_t st, ln, past;
        bool     esc;
        const int open = pnx_json_next(s + base, n - base, &st, &ln, &esc, &past);
        if (open < 0)
            return -1;
        if (step.index < 0 ? open != PNX_JSON_OBJECT : open != PNX_JSON_ARRAY)
            return 0;
        base += past;
        int64_t seen = 0;
        for (;;) {
            const int t = pnx_json_next(s + base, n - base, &st, &ln, &esc, &past);
            if (t < 0)
                return -1;
            if (t == 0 || t == PNX_JSON_OBJECT_END || t == PNX_JSON_ARRAY_END)
                return t == 0 ? -1 : 0;
            bool match;
            if (step.index < 0) {
                if (t != PNX_JSON_KEY)
                    return -1;
                uint8_t  buf[256];
                match = false;
                if (!esc) {
                    match = ln == step.key_len && __builtin_memcmp(s + base + st, step.key, ln) == 0;
                } else if (ln <= sizeof buf) {
                    const uint32_t m = pnx_json_unescape(s + base + st, ln, buf);
                    match = m == step.key_len && __builtin_memcmp(buf, step.key, m) == 0;
                }
                base += past;
            } else {
                match = seen++ == step.index;
            }
            if (match)
                break;
            uint32_t skip;
            if (step.index >= 0 && t == PNX_JSON_KEY)
                return -1;
            if (pnx_json_skip(s + base, n - base, &skip) <= 0)
                return -1;
            base += skip;
        }
    }
    uint32_t st, ln, past;
    bool     esc;
    int      k = pnx_json_next(s + base, n - base, &st, &ln, &esc, &past);
    if (k <= 0)
        return -1;
    if (k == PNX_JSON_KEY)
        k = PNX_JSON_STRING;
    if (k == PNX_JSON_OBJECT || k == PNX_JSON_ARRAY) {
        uint32_t whole;
        if (pnx_json_skip(s + base, n - base, &whole) <= 0)
            return -1;
        *start = base + st;
        *len = whole - st;
        *escaped = false;
    } else if (k == PNX_JSON_OBJECT_END || k == PNX_JSON_ARRAY_END) {
        return -1;
    } else {
        *start = base + st;
        *len = ln;
        *escaped = esc;
    }
    *kind = k;
    return 1;
}
