// pinax_ptn.c - the small pattern matcher of the module ptn (RFC-0009 section 4.3): Lua-sized
// patterns of byte classes, sets, four repetitions, anchors and nine captures. No
// alternation, no grouped repetition, no back references, no %b or %f.
//
// The matcher goes back on failure, as Lua's does. Its recursion is bounded by the length of
// the pattern (at most PNX_PTN_MAX bytes); its work by a count of steps per call.
#include "pinax_internal.h"

typedef struct ptn_matcher {
    const uint8_t *s, *send, *p, *pend;
    uint32_t       level;
    const uint8_t *cstart[PNX_PTN_CAPS];
    int64_t        clen[PNX_PTN_CAPS]; // -1 while the capture is open
    uint64_t       steps;
    bool           abort; // a malformed pattern, or too many steps
} ptn_matcher;

#define PNX_PTN_STEPS (1u << 24)

static bool ptn_alpha(uint8_t c) { return (c | 0x20) >= 'a' && (c | 0x20) <= 'z'; }
static bool ptn_digit(uint8_t c) { return c >= '0' && c <= '9'; }

static bool ptn_class(uint8_t c, uint8_t cl) {
    bool r;
    switch (cl | 0x20) {
    case 'a': r = ptn_alpha(c); break;
    case 'c': r = c < 32 || c == 127; break;
    case 'd': r = ptn_digit(c); break;
    case 'l': r = c >= 'a' && c <= 'z'; break;
    case 'p': r = (c >= '!' && c <= '/') || (c >= ':' && c <= '@') || (c >= '[' && c <= '`') || (c >= '{' && c <= '~'); break;
    case 's': r = c == ' ' || (c >= '\t' && c <= '\r'); break;
    case 'u': r = c >= 'A' && c <= 'Z'; break;
    case 'w': r = ptn_alpha(c) || ptn_digit(c); break;
    case 'x': r = ptn_digit(c) || ((c | 0x20) >= 'a' && (c | 0x20) <= 'f'); break;
    default: return cl == c; // an escaped byte stands for itself
    }
    return (cl >= 'A' && cl <= 'Z') ? !r : r;
}

// The end of the single item at p: past `.`, a byte, `%x` or a set; NULL when malformed.
static const uint8_t *ptn_item_end(const ptn_matcher *m, const uint8_t *p) {
    if (*p == '%')
        return p + 1 < m->pend ? p + 2 : NULL;
    if (*p != '[')
        return p + 1;
    ++p;
    if (p < m->pend && *p == '^')
        ++p;
    do { // the first ] after [ or [^ is a member
        if (p >= m->pend)
            return NULL;
        if (*p == '%') {
            if (++p >= m->pend)
                return NULL;
        }
        ++p;
    } while (p < m->pend && *p != ']');
    return p < m->pend ? p + 1 : NULL;
}

// Whether c is in the set [p .. ep) (p at '[', ep past ']').
static bool ptn_in_set(uint8_t c, const uint8_t *p, const uint8_t *ep) {
    bool yes = true;
    ++p;
    if (*p == '^') {
        yes = false;
        ++p;
    }
    const uint8_t *end = ep - 1; // at the closing ']'; a ']' right after [ or [^ is a member before it
    while (p < end) {
        if (*p == '%') {
            if (ptn_class(c, p[1]))
                return yes;
            p += 2;
        } else if (p + 2 < end && p[1] == '-') {
            if (p[0] <= c && c <= p[2])
                return yes;
            p += 3;
        } else {
            if (*p == c)
                return yes;
            ++p;
        }
    }
    return !yes;
}

static bool ptn_single(const uint8_t *s, const ptn_matcher *m, const uint8_t *p, const uint8_t *ep) {
    if (s >= m->send)
        return false;
    switch (*p) {
    case '.': return true;
    case '%': return ptn_class(*s, p[1]);
    case '[': return ptn_in_set(*s, p, ep);
    default: return *p == *s;
    }
}

static const uint8_t *ptn_match_at(ptn_matcher *m, const uint8_t *s, const uint8_t *p);

static const uint8_t *ptn_more(ptn_matcher *m, const uint8_t *s, const uint8_t *p, const uint8_t *ep) { // * and +
    uint64_t i = 0;
    while (ptn_single(s + i, m, p, ep))
        ++i;
    m->steps += i;
    for (;; --i) {
        const uint8_t *r = ptn_match_at(m, s + i, ep + 1);
        if (r || m->abort || i == 0)
            return r;
    }
}

static const uint8_t *ptn_fewer(ptn_matcher *m, const uint8_t *s, const uint8_t *p, const uint8_t *ep) { // -
    for (;;) {
        const uint8_t *r = ptn_match_at(m, s, ep + 1);
        if (r || m->abort)
            return r;
        if (!ptn_single(s, m, p, ep))
            return NULL;
        ++s;
        if (++m->steps > PNX_PTN_STEPS) {
            m->abort = true;
            return NULL;
        }
    }
}

static const uint8_t *ptn_match_at(ptn_matcher *m, const uint8_t *s, const uint8_t *p) {
    for (;;) {
        if (++m->steps > PNX_PTN_STEPS) {
            m->abort = true;
            return NULL;
        }
        if (p == m->pend)
            return s;
        if (*p == '(') {
            if (m->level == PNX_PTN_CAPS) {
                m->abort = true;
                return NULL;
            }
            m->cstart[m->level] = s;
            m->clen[m->level++] = -1;
            const uint8_t *r = ptn_match_at(m, s, p + 1);
            if (!r)
                --m->level;
            return r;
        }
        if (*p == ')') {
            int64_t open = -1;
            for (int64_t k = (int64_t)m->level - 1; k >= 0 && open < 0; --k)
                open = m->clen[k] == -1 ? k : -1;
            if (open < 0) {
                m->abort = true;
                return NULL;
            }
            m->clen[open] = s - m->cstart[open];
            const uint8_t *r = ptn_match_at(m, s, p + 1);
            if (!r)
                m->clen[open] = -1;
            return r;
        }
        if (*p == '$' && p + 1 == m->pend)
            return s == m->send ? s : NULL;
        const uint8_t *ep = ptn_item_end(m, p);
        if (!ep) {
            m->abort = true;
            return NULL;
        }
        const bool    one = ptn_single(s, m, p, ep);
        const uint8_t q = ep < m->pend ? *ep : 0;
        if (q == '?') {
            const uint8_t *r = one ? ptn_match_at(m, s + 1, ep + 1) : NULL;
            if (r || m->abort)
                return r;
            p = ep + 1;
        } else if (q == '+') {
            return one ? ptn_more(m, s + 1, p, ep) : NULL;
        } else if (q == '*') {
            return ptn_more(m, s, p, ep);
        } else if (q == '-') {
            return ptn_fewer(m, s, p, ep);
        } else {
            if (!one)
                return NULL;
            ++s;
            p = ep;
        }
    }
}

int pnx_ptn_find(const uint8_t *s, uint32_t length, uint32_t from, const uint8_t *pat, uint32_t plen, pnx_ptn_caps *caps,
                 uint64_t *steps) {
    const char *why;
    if (!pnx_ptn_check(pat, plen, &why)) // a pattern from bytecode or a view is checked here too
        return -1;
    const bool anchored = plen && pat[0] == '^';
    ptn_matcher    m = {.s = s, .send = s + length, .p = pat + anchored, .pend = pat + plen};
    int        found = 0;
    for (uint32_t at = from; at <= length && !m.abort; ++at) {
        m.level = 0;
        const uint8_t *end = ptn_match_at(&m, s + at, m.p);
        if (end) {
            caps->at = at;
            caps->len = (uint32_t)(end - (s + at));
            caps->n = m.level;
            for (uint32_t k = 0; k < m.level; ++k) {
                caps->start[k] = (uint32_t)(m.cstart[k] - s);
                caps->clen[k] = m.clen[k] < 0 ? 0 : (uint32_t)m.clen[k];
            }
            found = 1;
            break;
        }
        if (anchored)
            break;
    }
    *steps += m.steps;
    return m.abort ? -2 : found;
}

// The checks the compiler makes of a constant pattern; the matcher does not rely on them.
bool pnx_ptn_check(const uint8_t *pat, uint32_t plen, const char **why) {
    ptn_matcher m = {.p = pat, .pend = pat + plen};
    int     open = 0, made = 0;
    if (plen > PNX_PTN_MAX) {
        *why = "a pattern is at most 255 bytes";
        return false;
    }
    for (const uint8_t *p = pat + (plen && pat[0] == '^'); p < m.pend;) {
        if (*p == '(') {
            if (p + 1 < m.pend && p[1] == ')') {
                *why = "a position capture () is not supported";
                return false;
            }
            if (++made > PNX_PTN_CAPS) {
                *why = "a pattern has at most nine captures";
                return false;
            }
            ++open, ++p;
            continue;
        }
        if (*p == ')') {
            if (--open < 0) {
                *why = "a ) closes no capture";
                return false;
            }
            ++p;
            continue;
        }
        if (*p == '$' && p + 1 == m.pend)
            break;
        if (*p == '%' && p + 1 < m.pend && (p[1] == 'b' || p[1] == 'f' || ptn_digit(p[1]))) {
            *why = "%b, %f and back references are not supported";
            return false;
        }
        const uint8_t *ep = ptn_item_end(&m, p);
        if (!ep) {
            *why = *p == '[' ? "a set is not closed by ]" : "the pattern ends with %";
            return false;
        }
        p = ep < m.pend && (*ep == '*' || *ep == '+' || *ep == '-' || *ep == '?') ? ep + 1 : ep;
    }
    if (open) {
        *why = "a capture is not closed by )";
        return false;
    }
    return true;
}
