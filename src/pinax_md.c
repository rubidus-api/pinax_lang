// pinax_md.c - the module md (RFC-0046): Markdown read a block at a time (pnx_md_next) or made into HTML
// (pnx_md_html, pnx_md_inline). A subset of CommonMark with GFM tables and strikethrough, written for text a script
// meets (a README, a note, a page), not for every corner of the specification. Raw HTML in the text is never
// passed on: `<` is written as `&lt;`; a link whose scheme is not http, https, mailto, tel or ftp is written as its
// text. No memory is taken: a container (a quote, an item) is cut into the tail of the output buffer and read from
// there.
#include "pinax_internal.h"

#define MDN_DEPTH 12

static bool mdn_space(uint8_t b) { return b == ' ' || b == '\t' || b == '\n' || b == '\r'; }
static bool mdn_alnum(uint8_t b) { return (b >= '0' && b <= '9') || ((b | 0x20) >= 'a' && (b | 0x20) <= 'z') || b >= 0x80; }
static bool mdn_punct(uint8_t b) { return b > 32 && b < 127 && !mdn_alnum(b); }

// ---- lines and blocks ---------------------------------------------------------------------------------

typedef struct {
    uint32_t start, end, next; // the text of the line, and where the next one starts
    uint32_t indent;           // columns of spaces before the text (a tab to a multiple of 4)
    uint32_t text;             // the first byte after them
} mdn_line;

static bool mdn_line_at(const uint8_t *s, uint32_t n, uint32_t at, mdn_line *l) {
    if (at >= n)
        return false;
    l->start = at;
    uint32_t e = at;
    while (e < n && s[e] != '\n')
        ++e;
    l->next = e < n ? e + 1 : e;
    if (e > at && s[e - 1] == '\r')
        --e;
    l->end = e;
    uint32_t col = 0, k = at;
    while (k < e && (s[k] == ' ' || s[k] == '\t')) {
        col = s[k] == '\t' ? (col + 4) & ~3u : col + 1;
        ++k;
    }
    l->indent = col;
    l->text = k;
    return true;
}
static bool mdn_blank(const uint8_t *s, const mdn_line *l) {
    (void)s;
    return l->text >= l->end;
}

// a thematic break: three or more of one of - * _ with nothing else but blanks
static bool mdn_rule(const uint8_t *s, const mdn_line *l) {
    if (l->indent > 3 || l->text >= l->end)
        return false;
    const uint8_t c = s[l->text];
    if (c != '-' && c != '*' && c != '_')
        return false;
    uint32_t count = 0;
    for (uint32_t k = l->text; k < l->end; ++k) {
        if (s[k] == c)
            ++count;
        else if (s[k] != ' ' && s[k] != '\t')
            return false;
    }
    return count >= 3;
}

static uint32_t mdn_atx(const uint8_t *s, const mdn_line *l) { // the level of a heading, 0 for none
    if (l->indent > 3)
        return 0;
    uint32_t k = l->text, h = 0;
    while (k < l->end && s[k] == '#' && h < 7) {
        ++k;
        ++h;
    }
    return h >= 1 && h <= 6 && (k == l->end || s[k] == ' ' || s[k] == '\t') ? h : 0;
}

static bool mdn_fence(const uint8_t *s, const mdn_line *l, uint8_t *ch, uint32_t *count, uint32_t *info, uint32_t *info_end) {
    if (l->indent > 3 || l->text >= l->end || (s[l->text] != '`' && s[l->text] != '~'))
        return false;
    uint32_t k = l->text;
    const uint8_t c = s[k];
    while (k < l->end && s[k] == c)
        ++k;
    if (k - l->text < 3)
        return false;
    uint32_t a = k;
    while (a < l->end && (s[a] == ' ' || s[a] == '\t'))
        ++a;
    uint32_t e = l->end;
    while (e > a && (s[e - 1] == ' ' || s[e - 1] == '\t'))
        --e;
    if (c == '`') {
        for (uint32_t m = a; m < e; ++m) {
            if (s[m] == '`')
                return false;
        }
    }
    *ch = c;
    *count = k - l->text;
    *info = a;
    *info_end = e;
    return true;
}

static bool mdn_quote(const uint8_t *s, const mdn_line *l) { return l->indent <= 3 && l->text < l->end && s[l->text] == '>'; }

// A list marker: where the content starts (columns of the item's content, counting the marker), whether the item
// is ordered, the number, the marker's own character (the bullet, or . or ) ), and whether the item has no text.
typedef struct {
    uint32_t content_col, content_at;
    bool     ordered, empty;
    uint32_t number;
    uint8_t  mark;
} mdn_marker;

static bool mdn_item(const uint8_t *s, const mdn_line *l, mdn_marker *m) {
    if (l->indent > 3 || l->text >= l->end)
        return false;
    uint32_t k = l->text;
    const uint8_t c = s[k];
    uint32_t width;
    if (c == '-' || c == '+' || c == '*') {
        m->ordered = false;
        m->mark = c;
        m->number = 0;
        width = 1;
        ++k;
    } else if (c >= '0' && c <= '9') {
        uint32_t v = 0, d = 0;
        while (k < l->end && s[k] >= '0' && s[k] <= '9' && d < 10) {
            v = v * 10 + (uint32_t)(s[k] - '0');
            ++k;
            ++d;
        }
        if (d == 0 || d > 9 || k >= l->end || (s[k] != '.' && s[k] != ')'))
            return false;
        m->ordered = true;
        m->mark = s[k];
        m->number = v;
        width = d + 1;
        ++k;
    } else {
        return false;
    }
    if (k < l->end && s[k] != ' ' && s[k] != '\t')
        return false;
    uint32_t sp = 0, j = k;
    while (j < l->end && (s[j] == ' ' || s[j] == '\t')) {
        sp += s[j] == '\t' ? 4 - ((l->indent + width + sp) & 3) : 1;
        ++j;
    }
    m->empty = j >= l->end;
    if (m->empty || sp > 4) { // an empty item, or code after the marker: one blank belongs to the marker
        sp = 1;
        j = k < l->end ? k + 1 : k;
    }
    m->content_col = l->indent + width + sp;
    m->content_at = j;
    return true;
}

static bool mdn_delim_row(const uint8_t *s, uint32_t start, uint32_t end) { // | --- | :-: |
    bool dash = false, any = false;
    for (uint32_t k = start; k < end; ++k) {
        const uint8_t b = s[k];
        if (b == '-')
            dash = any = true;
        else if (b != '|' && b != ':' && b != ' ' && b != '\t')
            return false;
    }
    return dash && any;
}

static bool mdn_has_pipe(const uint8_t *s, const mdn_line *l) {
    for (uint32_t k = l->text; k < l->end; ++k) {
        if (s[k] == '|')
            return true;
    }
    return false;
}

// Whether the line begins something that ends a paragraph.
static bool mdn_interrupts(const uint8_t *s, const mdn_line *l) {
    uint8_t  ch;
    uint32_t c, i0, i1;
    mdn_marker m;
    if (mdn_blank(s, l) || mdn_atx(s, l) || mdn_fence(s, l, &ch, &c, &i0, &i1) || mdn_quote(s, l) || mdn_rule(s, l))
        return true;
    return mdn_item(s, l, &m) && !m.empty && (!m.ordered || m.number == 1);
}

typedef struct {
    int      kind, level;
    uint32_t off, len, past;
    uint32_t info_off, info_len;
} mdn_block;

static uint32_t mdn_trim_end(const uint8_t *s, uint32_t from, uint32_t end) {
    while (end > from && mdn_space(s[end - 1]))
        --end;
    return end;
}

// The next block of s[at..n): 1, or 0 when there is none.
static int mdn_scan(const uint8_t *s, uint32_t n, uint32_t at, mdn_block *b) {
    mdn_line l;
    for (;;) { // blank lines
        if (!mdn_line_at(s, n, at, &l))
            return 0;
        if (!mdn_blank(s, &l))
            break;
        at = l.next;
    }
    b->info_off = b->info_len = 0;
    b->level = 0;
    uint8_t  ch;
    uint32_t count, i0, i1;
    if (mdn_fence(s, &l, &ch, &count, &i0, &i1)) {
        b->kind = PNX_MD_CODE;
        b->info_off = i0;
        b->info_len = i1 - i0;
        b->off = l.next;
        uint32_t k = l.next;
        mdn_line m;
        b->len = 0;
        for (;;) {
            if (!mdn_line_at(s, n, k, &m)) {
                b->len = n - b->off;
                b->past = n;
                return 1;
            }
            uint8_t  c2;
            uint32_t n2, a, e;
            if (mdn_fence(s, &m, &c2, &n2, &a, &e) && c2 == ch && n2 >= count && a == e) {
                b->len = m.start - b->off;
                b->past = m.next;
                return 1;
            }
            k = m.next;
        }
    }
    if ((b->level = (int)mdn_atx(s, &l)) != 0) {
        b->kind = PNX_MD_HEADING;
        uint32_t a = l.text + (uint32_t)b->level, e = l.end;
        while (a < e && (s[a] == ' ' || s[a] == '\t'))
            ++a;
        e = mdn_trim_end(s, a, e);
        uint32_t h = e; // a closing run of #, set off by a blank
        while (h > a && s[h - 1] == '#')
            --h;
        if (h < e && (h == a || s[h - 1] == ' ' || s[h - 1] == '\t'))
            e = mdn_trim_end(s, a, h);
        b->off = a;
        b->len = e - a;
        b->past = l.next;
        return 1;
    }
    if (mdn_rule(s, &l)) {
        b->kind = PNX_MD_RULE;
        b->off = l.start;
        b->len = 0;
        b->past = l.next;
        return 1;
    }
    if (mdn_quote(s, &l)) {
        b->kind = PNX_MD_QUOTE;
        b->off = l.start;
        uint32_t k = l.next;
        mdn_line m;
        bool     text = true;
        while (mdn_line_at(s, n, k, &m) && !mdn_blank(s, &m)) {
            if (mdn_quote(s, &m)) {
                text = true;
            } else if (!text || mdn_interrupts(s, &m)) {
                break;
            }
            k = m.next;
        }
        b->len = mdn_trim_end(s, b->off, k) - b->off;
        b->past = k;
        return 1;
    }
    mdn_marker mk;
    if (mdn_item(s, &l, &mk)) {
        b->kind = PNX_MD_LIST;
        b->level = mk.ordered;
        b->off = l.start;
        uint32_t k = l.next, col = mk.content_col;
        mdn_line m;
        bool     last_blank = false;
        uint32_t end = l.next;
        while (mdn_line_at(s, n, k, &m)) {
            if (mdn_blank(s, &m)) {
                last_blank = true;
                k = m.next;
                continue;
            }
            mdn_marker nm;
            if (m.indent >= col) { // inside the item
            } else if (mdn_item(s, &m, &nm) && nm.ordered == mk.ordered && nm.mark == mk.mark) {
                col = nm.content_col;
            } else if (last_blank || mdn_interrupts(s, &m)) {
                break;
            }
            last_blank = false;
            end = m.next;
            k = m.next;
        }
        b->len = mdn_trim_end(s, b->off, end) - b->off;
        b->past = end;
        return 1;
    }
    mdn_line nx;
    if (mdn_has_pipe(s, &l) && mdn_line_at(s, n, l.next, &nx) && nx.indent <= 3 && mdn_delim_row(s, nx.text, nx.end)) {
        b->kind = PNX_MD_TABLE;
        b->off = l.start;
        uint32_t k = nx.next;
        mdn_line m;
        while (mdn_line_at(s, n, k, &m) && !mdn_blank(s, &m) && mdn_has_pipe(s, &m))
            k = m.next;
        b->len = mdn_trim_end(s, b->off, k) - b->off;
        b->past = k;
        return 1;
    }
    if (l.indent >= 4) {
        b->kind = PNX_MD_CODE;
        b->off = l.start;
        uint32_t k = l.next, end = l.next;
        mdn_line m;
        while (mdn_line_at(s, n, k, &m)) {
            if (!mdn_blank(s, &m)) {
                if (m.indent < 4)
                    break;
                end = m.next;
            }
            k = m.next;
        }
        b->len = mdn_trim_end(s, b->off, end) - b->off;
        b->level = 1; // indented: the lines keep their four blanks until they are written
        b->past = end;
        return 1;
    }
    b->kind = PNX_MD_PARAGRAPH;
    b->off = l.text;
    uint32_t k = l.next, end = l.end;
    mdn_line m;
    while (mdn_line_at(s, n, k, &m)) {
        if (mdn_blank(s, &m))
            break;
        if (m.indent <= 3 && m.text < m.end && (s[m.text] == '=' || s[m.text] == '-')) { // a line under: setext
            const uint8_t c = s[m.text];
            uint32_t      q = m.text;
            while (q < m.end && s[q] == c)
                ++q;
            while (q < m.end && (s[q] == ' ' || s[q] == '\t'))
                ++q;
            if (q == m.end) {
                b->kind = PNX_MD_HEADING;
                b->level = c == '=' ? 1 : 2;
                b->len = mdn_trim_end(s, b->off, end) - b->off;
                b->past = m.next;
                return 1;
            }
        }
        if (mdn_interrupts(s, &m))
            break;
        end = m.end;
        k = m.next;
    }
    b->len = mdn_trim_end(s, b->off, end) - b->off;
    b->past = k;
    return 1;
}

// The next block of `s`: its kind, the level (a heading's 1 to 6; a list: 1 when it is ordered), the text (a
// heading's or paragraph's inline text; code as it is written between its fences, or with its indentation; a
// quote, list or table as written) and the bytes taken. 0 at the end.
int pnx_md_next(const uint8_t *s, uint32_t n, int *kind, int *level, uint32_t *off, uint32_t *len, uint32_t *past) {
    mdn_block b;
    if (!mdn_scan(s, n, 0, &b))
        return 0;
    *kind = b.kind;
    *level = b.kind == PNX_MD_CODE ? 0 : b.level;
    *off = b.off;
    *len = b.len;
    *past = b.past;
    return 1;
}

// ---- output ---------------------------------------------------------------------------------------------

typedef struct {
    uint8_t *out;
    uint64_t lim, len; // writes stop at lim; the tail holds the container being read
    bool     full, plain;
    uint64_t work; // bytes the searches for a closing mark may still read in all: when it is spent they find nothing
} mdn_w;

// One byte of search: false when the budget is spent. A mark with no closer, repeated, would otherwise read to the end
// of the text for each one of them.
static bool mdn_spend(mdn_w *w) {
    if (w->work == 0)
        return false;
    --w->work;
    return true;
}

static void mdn_byte(mdn_w *w, uint8_t b) {
    if (w->len < w->lim)
        w->out[w->len++] = b;
    else
        w->full = true;
}
static void mdn_raw(mdn_w *w, const char *t) {
    while (*t)
        mdn_byte(w, (uint8_t)*t++);
}
static void mdn_tag(mdn_w *w, const char *t) {
    if (!w->plain)
        mdn_raw(w, t);
}
static void mdn_esc(mdn_w *w, uint8_t b) {
    switch (b) {
    case '&': mdn_raw(w, "&amp;"); break;
    case '<': mdn_raw(w, "&lt;"); break;
    case '>': mdn_raw(w, "&gt;"); break;
    case '"': mdn_raw(w, "&quot;"); break;
    default: mdn_byte(w, b); break;
    }
}
static void mdn_text(mdn_w *w, const uint8_t *s, uint32_t from, uint32_t to) {
    for (uint32_t k = from; k < to; ++k)
        mdn_esc(w, s[k]);
}

static void mdn_inline(mdn_w *w, const uint8_t *s, uint32_t n, uint32_t depth);

static uint32_t mdn_entity(const uint8_t *s, uint32_t n, uint32_t at) { // the bytes of a character reference at s[at] = '&', or 0
    uint32_t k = at + 1;
    if (k < n && s[k] == '#') {
        ++k;
        const bool hex = k < n && (s[k] | 0x20) == 'x';
        if (hex)
            ++k;
        const uint32_t d0 = k;
        while (k < n && (hex ? (s[k] >= '0' && s[k] <= '9') || ((s[k] | 0x20) >= 'a' && (s[k] | 0x20) <= 'f') : (s[k] >= '0' && s[k] <= '9')))
            ++k;
        if (k == d0 || k - d0 > (hex ? 6u : 7u) || k >= n || s[k] != ';')
            return 0;
        return k + 1 - at;
    }
    const uint32_t a0 = k;
    while (k < n && mdn_alnum(s[k]) && s[k] < 0x80)
        ++k;
    if (k == a0 || k - a0 > 31 || k >= n || s[k] != ';')
        return 0;
    return k + 1 - at;
}

// A destination that may be a link: no scheme, or one of http, https, mailto, tel, ftp.
static bool mdn_url_ok(const uint8_t *u, uint32_t n) {
    uint32_t k = 0;
    while (k < n && u[k] != ':' && u[k] != '/' && u[k] != '?' && u[k] != '#')
        ++k;
    if (k >= n || u[k] != ':')
        return true;
    static const char *const ok[] = {"http", "https", "mailto", "tel", "ftp"};
    for (uint32_t q = 0; q < sizeof ok / sizeof ok[0]; ++q) {
        uint32_t len = 0;
        while (ok[q][len])
            ++len;
        if (len == k) {
            bool same = true;
            for (uint32_t z = 0; z < k; ++z)
                same = same && (u[z] | 0x20) == (uint8_t)ok[q][z];
            if (same)
                return true;
        }
    }
    return false;
}

static void mdn_url(mdn_w *w, const uint8_t *u, uint32_t n) {
    for (uint32_t k = 0; k < n; ++k) {
        if (u[k] == '\\' && k + 1 < n && mdn_punct(u[k + 1])) {
            ++k;
            mdn_esc(w, u[k]);
        } else if (u[k] == ' ') {
            mdn_raw(w, "%20");
        } else if (u[k] == '&' && mdn_entity(u, n, k)) {
            mdn_byte(w, '&');
        } else {
            mdn_esc(w, u[k]);
        }
    }
}

// the closing bracket of a link text that opens at s[at] = '['; nested brackets, code spans and escapes pass
static int64_t mdn_close_bracket(mdn_w *w, const uint8_t *s, uint32_t n, uint32_t at) {
    uint32_t depth = 0;
    for (uint32_t k = at; k < n; ++k) {
        if (!mdn_spend(w))
            return -1;
        if (s[k] == '\\') {
            ++k;
        } else if (s[k] == '`') {
            uint32_t r = k;
            while (r < n && s[r] == '`')
                ++r;
            const uint32_t run = r - k;
            uint32_t       q = r;
            bool           found = false;
            while (q < n) {
                if (!mdn_spend(w))
                    return -1;
                if (s[q] != '`') {
                    ++q;
                    continue;
                }
                uint32_t e = q;
                while (e < n && s[e] == '`')
                    ++e;
                if (e - q == run) {
                    found = true;
                    k = e - 1;
                    break;
                }
                q = e;
            }
            if (!found)
                k = r - 1;
        } else if (s[k] == '[') {
            ++depth;
        } else if (s[k] == ']') {
            if (--depth == 0)
                return k;
        }
    }
    return -1;
}

// ( destination "title" ) after a link text: where it ends, and the pieces; -1 when it is not one.
static int64_t mdn_dest(mdn_w *w, const uint8_t *s, uint32_t n, uint32_t at, uint32_t *d0, uint32_t *d1, uint32_t *t0, uint32_t *t1) {
    if (at >= n || s[at] != '(')
        return -1;
    uint32_t k = at + 1;
    while (k < n && mdn_space(s[k]))
        ++k;
    *t0 = *t1 = 0;
    if (k < n && s[k] == '<') {
        *d0 = ++k;
        while (k < n && s[k] != '>' && s[k] != '\n') {
            if (!mdn_spend(w))
                return -1;
            ++k;
        }
        if (k >= n || s[k] != '>')
            return -1;
        *d1 = k++;
    } else {
        *d0 = k;
        uint32_t paren = 0;
        while (k < n && !mdn_space(s[k]) && s[k] >= 0x20) {
            if (!mdn_spend(w))
                return -1;
            if (s[k] == '\\' && k + 1 < n) {
                ++k;
            } else if (s[k] == '(') {
                ++paren;
            } else if (s[k] == ')') {
                if (paren == 0)
                    break;
                --paren;
            }
            ++k;
        }
        if (paren)
            return -1;
        *d1 = k;
    }
    while (k < n && mdn_space(s[k]))
        ++k;
    if (k < n && (s[k] == '"' || s[k] == '\'')) {
        const uint8_t q = s[k++];
        *t0 = k;
        while (k < n && s[k] != q) {
            if (!mdn_spend(w))
                return -1;
            k += s[k] == '\\' ? 2 : 1;
        }
        if (k >= n)
            return -1;
        *t1 = k++;
        while (k < n && mdn_space(s[k]))
            ++k;
    }
    return k < n && s[k] == ')' ? (int64_t)k + 1 : -1;
}

// a code span of `run` backticks that opens at s[at]: where it closes (the first byte after), 0 when it does not
static uint32_t mdn_code_end(mdn_w *w, const uint8_t *s, uint32_t n, uint32_t at, uint32_t run) {
    uint32_t q = at + run;
    while (q < n) {
        if (!mdn_spend(w))
            return 0;
        if (s[q] != '`') {
            ++q;
            continue;
        }
        uint32_t e = q;
        while (e < n && s[e] == '`')
            ++e;
        if (e - q == run)
            return q;
        q = e;
    }
    return 0;
}

// The closer of an emphasis that opened with `k` bytes `c` at s[at]: the position of a run of c that can close,
// 0 when there is none. Code spans and links on the way are passed over.
static uint32_t mdn_closer(mdn_w *w, const uint8_t *s, uint32_t n, uint32_t from, uint8_t c, uint32_t want, uint32_t *run) {
    for (uint32_t k = from; k < n; ++k) {
        if (!mdn_spend(w))
            return 0;
        if (s[k] == '\\') {
            ++k;
        } else if (s[k] == '`') {
            uint32_t r = k;
            while (r < n && s[r] == '`')
                ++r;
            const uint32_t close = mdn_code_end(w, s, n, k, r - k);
            k = close ? close + (r - k) - 1 : r - 1;
        } else if (s[k] == c) {
            uint32_t r = k;
            while (r < n && s[r] == c)
                ++r;
            const bool after_space = k == from || mdn_space(s[k - 1]);
            const bool before_alnum = r < n && mdn_alnum(s[r]);
            if (!after_space && !(c == '_' && before_alnum) && r - k >= (want > 2 ? 2 : want)) {
                *run = r - k;
                return k;
            }
            k = r - 1;
        }
    }
    return 0;
}

static void mdn_inline(mdn_w *w, const uint8_t *s, uint32_t n, uint32_t depth) {
    if (depth > MDN_DEPTH) {
        mdn_text(w, s, 0, n);
        return;
    }
    for (uint32_t k = 0; k < n && !w->full; ++k) {
        const uint8_t b = s[k];
        switch (b) {
        case '\\':
            if (k + 1 < n && mdn_punct(s[k + 1])) {
                mdn_esc(w, s[++k]);
            } else if (k + 1 < n && s[k + 1] == '\n') {
                mdn_tag(w, "<br />");
                mdn_byte(w, '\n');
                ++k;
                while (k + 1 < n && (s[k + 1] == ' ' || s[k + 1] == '\t'))
                    ++k;
            } else {
                mdn_byte(w, b);
            }
            break;
        case '`': {
            uint32_t r = k;
            while (r < n && s[r] == '`')
                ++r;
            const uint32_t close = mdn_code_end(w, s, n, k, r - k);
            if (!close) {
                mdn_text(w, s, k, r);
                k = r - 1;
                break;
            }
            uint32_t a = r, e = close;
            bool     all_space = true;
            for (uint32_t q = a; q < e; ++q)
                all_space = all_space && (s[q] == ' ' || s[q] == '\n');
            if (!all_space && e - a >= 2 && (s[a] == ' ' || s[a] == '\n') && (s[e - 1] == ' ' || s[e - 1] == '\n')) {
                ++a;
                --e;
            }
            mdn_tag(w, "<code>");
            for (uint32_t q = a; q < e; ++q)
                mdn_esc(w, s[q] == '\n' ? ' ' : s[q]);
            mdn_tag(w, "</code>");
            k = close + (r - k) - 1;
            break;
        }
        case '&': {
            const uint32_t e = mdn_entity(s, n, k);
            if (e) {
                for (uint32_t q = 0; q < e; ++q)
                    mdn_byte(w, s[k + q]);
                k += e - 1;
            } else {
                mdn_raw(w, "&amp;");
            }
            break;
        }
        case '<': { // an autolink <http://...> or <a@b.c>; anything else is text
            uint32_t e = k + 1;
            bool     colon = false, at_sign = false, bad = false;
            while (e < n && s[e] != '>' && !bad) {
                if (!mdn_spend(w) || s[e] == ' ' || s[e] == '<' || s[e] == '\n')
                    bad = true;
                colon = colon || s[e] == ':';
                at_sign = at_sign || s[e] == '@';
                ++e;
            }
            if (e < n && !bad && e > k + 1 && (colon || at_sign) && (!colon || mdn_url_ok(s + k + 1, e - k - 1))) {
                if (!colon) {
                    mdn_tag(w, "<a href=\"mailto:");
                } else {
                    mdn_tag(w, "<a href=\"");
                }
                if (!w->plain)
                    mdn_url(w, s + k + 1, e - k - 1);
                mdn_tag(w, "\">");
                mdn_text(w, s, k + 1, e);
                mdn_tag(w, "</a>");
                k = e;
            } else {
                mdn_raw(w, "&lt;");
            }
            break;
        }
        case '!': case '[': {
            const bool image = b == '!';
            if (image && !(k + 1 < n && s[k + 1] == '[')) {
                mdn_byte(w, b);
                break;
            }
            const uint32_t open = image ? k + 1 : k;
            const int64_t  close = mdn_close_bracket(w, s, n, open);
            uint32_t       d0, d1, t0, t1;
            int64_t        end = close < 0 ? -1 : mdn_dest(w, s, n, (uint32_t)close + 1, &d0, &d1, &t0, &t1);
            if (end < 0) {
                mdn_byte(w, b);
                break;
            }
            const bool ok = mdn_url_ok(s + d0, d1 - d0);
            if (image) {
                if (ok) {
                    mdn_tag(w, "<img src=\"");
                    if (!w->plain)
                        mdn_url(w, s + d0, d1 - d0);
                    mdn_tag(w, "\" alt=\"");
                    const bool was = w->plain;
                    w->plain = true;
                    mdn_inline(w, s + open + 1, (uint32_t)close - open - 1, depth + 1);
                    w->plain = was;
                    mdn_tag(w, "\"");
                    if (t1 > t0) {
                        mdn_tag(w, " title=\"");
                        if (!w->plain)
                            mdn_text(w, s, t0, t1);
                        mdn_tag(w, "\"");
                    }
                    mdn_tag(w, " />");
                } else {
                    mdn_inline(w, s + open + 1, (uint32_t)close - open - 1, depth + 1);
                }
            } else {
                if (ok) {
                    mdn_tag(w, "<a href=\"");
                    if (!w->plain)
                        mdn_url(w, s + d0, d1 - d0);
                    mdn_tag(w, "\"");
                    if (t1 > t0) {
                        mdn_tag(w, " title=\"");
                        if (!w->plain)
                            mdn_text(w, s, t0, t1);
                        mdn_tag(w, "\"");
                    }
                    mdn_tag(w, ">");
                }
                mdn_inline(w, s + open + 1, (uint32_t)close - open - 1, depth + 1);
                if (ok)
                    mdn_tag(w, "</a>");
            }
            k = (uint32_t)end - 1;
            break;
        }
        case '*': case '_': case '~': {
            uint32_t r = k;
            while (r < n && s[r] == b)
                ++r;
            const uint32_t run = r - k;
            const bool     before_alnum = k > 0 && mdn_alnum(s[k - 1]);
            const bool     opens = r < n && !mdn_space(s[r]) && !(b == '_' && before_alnum) && (b != '~' || run == 2);
            uint32_t       closer_run = 0;
            const uint32_t at = opens ? mdn_closer(w, s, n, r, b, run, &closer_run) : 0;
            if (!opens || !at || (b == '~' && closer_run != 2)) {
                for (uint32_t q = k; q < r; ++q)
                    mdn_byte(w, b);
                k = r - 1;
                break;
            }
            uint32_t use = b == '~' ? 2 : run >= 3 && closer_run >= 3 ? 3 : run >= 2 && closer_run >= 2 ? 2 : 1;
            for (uint32_t q = 0; q < run - use; ++q) // what the opener has over is text
                mdn_byte(w, b);
            const char *open_tag = b == '~' ? "<del>" : use == 3 ? "<em><strong>" : use == 2 ? "<strong>" : "<em>";
            const char *close_tag = b == '~' ? "</del>" : use == 3 ? "</strong></em>" : use == 2 ? "</strong>" : "</em>";
            mdn_tag(w, open_tag);
            mdn_inline(w, s + r, at - r, depth + 1);
            mdn_tag(w, close_tag);
            k = at + use - 1;
            break;
        }
        case '\n': {
            uint32_t back = w->len;
            uint32_t spaces = 0;
            while (back > 0 && w->out[back - 1] == ' ' && !w->plain) {
                --back;
                ++spaces;
            }
            if (spaces >= 2 && !w->plain) {
                w->len = back;
                mdn_raw(w, "<br />");
            } else if (spaces && !w->plain) {
                w->len = back;
            }
            mdn_byte(w, '\n');
            while (k + 1 < n && (s[k + 1] == ' ' || s[k + 1] == '\t'))
                ++k;
            break;
        }
        default: mdn_esc(w, b); break;
        }
    }
}

// ---- blocks to HTML ---------------------------------------------------------------------------------------

static void mdn_blocks(mdn_w *w, const uint8_t *s, uint32_t n, uint32_t depth, bool tight);

// Cut the lines of s[from..to) into the tail of the output with `strip` blanks off each: a text to read as blocks.
// Gives its start in the buffer, or NULL when there is no room; *m its length. The caller puts `lim` back.
static uint8_t *mdn_cut(mdn_w *w, uint32_t m) {
    if (w->lim < w->len + m + 1) {
        w->full = true;
        return NULL;
    }
    w->lim -= m;
    return w->out + w->lim;
}

static void mdn_quote_html(mdn_w *w, const uint8_t *s, uint32_t n, uint32_t depth) {
    const uint64_t saved = w->lim;
    uint8_t *buf = mdn_cut(w, n);
    if (!buf)
        return;
    uint32_t m = 0;
    mdn_line l;
    for (uint32_t at = 0; mdn_line_at(s, n, at, &l); at = l.next) {
        uint32_t k = l.text;
        if (mdn_quote(s, &l)) {
            ++k;
            if (k < l.end && s[k] == ' ')
                ++k;
        } else {
            k = l.text;
        }
        for (; k < l.end; ++k)
            buf[m++] = s[k];
        buf[m++] = '\n';
    }
    mdn_tag(w, "<blockquote>\n");
    mdn_blocks(w, buf, m, depth + 1, false);
    mdn_tag(w, "</blockquote>\n");
    w->lim = saved;
}

static void mdn_list_html(mdn_w *w, const uint8_t *s, uint32_t n, uint32_t depth) {
    mdn_line   l;
    mdn_marker first;
    if (!mdn_line_at(s, n, 0, &l) || !mdn_item(s, &l, &first))
        return;
    // the items: each begins at a marker of the first one's kind that is not inside the item before it
    bool     loose = false;
    uint32_t items = 0;
    {
        uint32_t col = first.content_col;
        bool     blank = false;
        for (uint32_t at = 0; mdn_line_at(s, n, at, &l); at = l.next) {
            mdn_marker m;
            if (mdn_blank(s, &l)) {
                blank = true;
                continue;
            }
            if (l.indent < col && mdn_item(s, &l, &m) && m.ordered == first.ordered && m.mark == first.mark) {
                if (items && blank)
                    loose = true;
                ++items;
                col = m.content_col;
            } else if (blank && items) {
                loose = true; // a blank line inside an item
            }
            blank = false;
        }
    }
    mdn_raw(w, first.ordered ? "<ol" : "<ul");
    if (first.ordered && first.number != 1) {
        mdn_raw(w, " start=\"");
        char     d[12];
        uint32_t c = 0, v = first.number;
        do {
            d[c++] = (char)('0' + v % 10);
            v /= 10;
        } while (v);
        while (c)
            mdn_byte(w, (uint8_t)d[--c]);
        mdn_raw(w, "\"");
    }
    mdn_raw(w, ">\n");
    uint32_t col = 0, at = 0;
    while (at < n && !w->full) {
        mdn_line   head;
        mdn_marker m;
        if (!mdn_line_at(s, n, at, &head) || !mdn_item(s, &head, &m))
            break;
        col = m.content_col;
        const uint64_t saved = w->lim;
        uint8_t *buf = mdn_cut(w, n - at);
        if (!buf)
            return;
        uint32_t used = 0;
        for (uint32_t k = m.content_at; k < head.end; ++k)
            buf[used++] = s[k];
        buf[used++] = '\n';
        uint32_t next = head.next;
        for (mdn_line c; mdn_line_at(s, n, next, &c); next = c.next) {
            mdn_marker nm;
            if (!mdn_blank(s, &c) && c.indent < col && mdn_item(s, &c, &nm) && nm.ordered == first.ordered && nm.mark == first.mark)
                break;
            if (mdn_blank(s, &c)) {
                buf[used++] = '\n';
                continue;
            }
            uint32_t from = c.start, cols = 0;
            const uint32_t drop = c.indent >= col ? col : c.indent;
            while (from < c.end && cols < drop && (s[from] == ' ' || s[from] == '\t')) {
                cols += s[from] == '\t' ? 4 - (cols & 3) : 1;
                ++from;
            }
            for (; from < c.end; ++from)
                buf[used++] = s[from];
            buf[used++] = '\n';
        }
        while (used > 0 && buf[used - 1] == '\n' && used > 1 && buf[used - 2] == '\n')
            --used;
        mdn_raw(w, "<li>");
        if (loose)
            mdn_byte(w, '\n');
        mdn_blocks(w, buf, used, depth + 1, !loose);
        mdn_raw(w, "</li>\n");
        w->lim = saved;
        at = next;
    }
    mdn_raw(w, first.ordered ? "</ol>\n" : "</ul>\n");
}

static void mdn_table_html(mdn_w *w, const uint8_t *s, uint32_t n) {
    uint8_t align[64];
    uint32_t cols = 0;
    mdn_line head, delim;
    if (!mdn_line_at(s, n, 0, &head) || !mdn_line_at(s, n, head.next, &delim))
        return;
    { // the alignment of each column, from the row of dashes
        uint32_t k = delim.text;
        if (k < delim.end && s[k] == '|')
            ++k;
        while (k < delim.end && cols < sizeof align) {
            uint32_t e = k;
            while (e < delim.end && s[e] != '|')
                ++e;
            uint32_t a = k, z = e;
            while (a < z && s[a] == ' ')
                ++a;
            while (z > a && s[z - 1] == ' ')
                --z;
            const bool left = a < z && s[a] == ':', right = a < z && s[z - 1] == ':';
            if (a < z || e < delim.end)
                align[cols++] = left && right ? 'c' : right ? 'r' : left ? 'l' : 0;
            k = e + 1;
        }
    }
    mdn_raw(w, "<table>\n");
    uint32_t at = 0;
    for (uint32_t row = 0; mdn_line_at(s, n, at, &head); ++row, at = head.next) {
        if (row == 1)
            continue; // the dashes
        const bool th = row == 0;
        if (row == 0)
            mdn_raw(w, "<thead>\n");
        if (row == 2)
            mdn_raw(w, "<tbody>\n");
        mdn_raw(w, "<tr>\n");
        uint32_t k = head.text;
        if (k < head.end && s[k] == '|')
            ++k;
        for (uint32_t c = 0; c < cols; ++c) {
            uint32_t e = k;
            while (e < head.end && (s[e] != '|' || (e > k && s[e - 1] == '\\')))
                ++e;
            uint32_t a = k, z = e;
            while (a < z && mdn_space(s[a]))
                ++a;
            while (z > a && mdn_space(s[z - 1]))
                --z;
            mdn_raw(w, th ? "<th" : "<td");
            if (align[c]) {
                mdn_raw(w, " align=\"");
                mdn_raw(w, align[c] == 'c' ? "center" : align[c] == 'r' ? "right" : "left");
                mdn_raw(w, "\"");
            }
            mdn_byte(w, '>');
            mdn_inline(w, s + a, z - a, 1);
            mdn_raw(w, th ? "</th>\n" : "</td>\n");
            k = e < head.end ? e + 1 : e;
        }
        mdn_raw(w, "</tr>\n");
        if (row == 0)
            mdn_raw(w, "</thead>\n");
    }
    if (at > 0 && mdn_line_at(s, n, 0, &head)) {
        uint32_t rows = 0;
        for (uint32_t p = 0; mdn_line_at(s, n, p, &head); p = head.next)
            ++rows;
        if (rows > 2)
            mdn_raw(w, "</tbody>\n");
    }
    mdn_raw(w, "</table>\n");
}

static void mdn_code_html(mdn_w *w, const uint8_t *s, const mdn_block *b) {
    mdn_raw(w, "<pre><code");
    if (b->info_len) { // the first word of the info string, when it is only letters, digits and - _ + # .
        uint32_t e = b->info_off;
        while (e < b->info_off + b->info_len && !mdn_space(s[e]))
            ++e;
        bool ok = e > b->info_off;
        for (uint32_t k = b->info_off; k < e; ++k)
            ok = ok && (mdn_alnum(s[k]) || s[k] == '-' || s[k] == '_' || s[k] == '+' || s[k] == '#' || s[k] == '.') && s[k] < 0x80;
        if (ok) {
            mdn_raw(w, " class=\"language-");
            mdn_text(w, s, b->info_off, e);
            mdn_raw(w, "\"");
        }
    }
    mdn_byte(w, '>');
    if (b->level == 1) { // indented: four blanks off each line
        mdn_line l;
        for (uint32_t at = b->off; at < b->off + b->len && mdn_line_at(s, b->off + b->len, at, &l); at = l.next) {
            uint32_t k = l.start, cols = 0;
            while (k < l.end && cols < 4 && (s[k] == ' ' || s[k] == '\t')) {
                cols += s[k] == '\t' ? 4 - (cols & 3) : 1;
                ++k;
            }
            mdn_text(w, s, k, l.end);
            mdn_byte(w, '\n');
        }
    } else {
        mdn_text(w, s, b->off, b->off + b->len);
    }
    mdn_raw(w, "</code></pre>\n");
}

static void mdn_blocks(mdn_w *w, const uint8_t *s, uint32_t n, uint32_t depth, bool tight) {
    uint32_t at = 0;
    mdn_block b;
    while (!w->full && mdn_scan(s, n, at, &b)) {
        at = b.past;
        if (depth > MDN_DEPTH) {
            mdn_text(w, s, b.off, b.off + b.len);
            continue;
        }
        const uint8_t *t = s + b.off;
        switch (b.kind) {
        case PNX_MD_HEADING: {
            char tag[4] = {'<', 'h', (char)('0' + b.level), 0};
            mdn_raw(w, tag);
            mdn_byte(w, '>');
            mdn_inline(w, t, b.len, 0);
            mdn_raw(w, "</h");
            mdn_byte(w, (uint8_t)('0' + b.level));
            mdn_raw(w, ">\n");
            break;
        }
        case PNX_MD_PARAGRAPH:
            if (tight) {
                mdn_inline(w, t, b.len, 0);
                mdn_line more;
                bool     last = !mdn_line_at(s, n, at, &more);
                mdn_block rest;
                if (!last && !mdn_scan(s, n, at, &rest))
                    last = true;
                if (!last)
                    mdn_byte(w, '\n');
            } else {
                mdn_raw(w, "<p>");
                mdn_inline(w, t, b.len, 0);
                mdn_raw(w, "</p>\n");
            }
            break;
        case PNX_MD_CODE: mdn_code_html(w, s, &b); break;
        case PNX_MD_RULE: mdn_raw(w, "<hr />\n"); break;
        case PNX_MD_QUOTE: mdn_quote_html(w, t, b.len, depth); break;
        case PNX_MD_LIST: mdn_list_html(w, t, b.len, depth); break;
        case PNX_MD_TABLE: mdn_table_html(w, t, b.len); break;
        }
    }
}

// Markdown as HTML into out[0..cap): the length, PNX_TXT_FULL when it does not fit.
uint32_t pnx_md_html(const uint8_t *s, uint32_t n, uint8_t *out, uint64_t cap) {
    mdn_w w = {out, cap, 0, false, false, (uint64_t)n * 16 + 4096};
    mdn_blocks(&w, s, n, 0, false);
    return w.full || w.len >= PNX_TXT_BAD ? PNX_TXT_FULL : (uint32_t)w.len;
}

// The same for the text of one line or paragraph, with no block around it.
uint32_t pnx_md_inline(const uint8_t *s, uint32_t n, uint8_t *out, uint64_t cap) {
    mdn_w w = {out, cap, 0, false, false, (uint64_t)n * 16 + 4096};
    mdn_inline(&w, s, n, 0);
    return w.full || w.len >= PNX_TXT_BAD ? PNX_TXT_FULL : (uint32_t)w.len;
}
