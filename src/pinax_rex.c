// pinax_rex.c - the regular expressions of the module rex (RFC-0011): a pattern becomes a small
// program of bytes, run by a Pike VM. Every possible path steps over the input together, one
// byte at a time, so a match takes time bounded by bytes times instructions; nothing goes
// back. No back references, no look-around. The design follows the engine of the owner's
// text editor (Pike VM, leftmost and greedy like Perl and RE2); this code is written for Pinax.
//
// A program holds no address: jumps are relative, so a piece of it can be copied (for counted
// repetition) and the whole of it can lie in the constants of the code, in the region or in an
// image. Layout: the head, `n_inst` instructions of 8 bytes, `n_sets` byte sets of 32 bytes.
#include "pinax_internal.h"

enum {
    RX_BYTE,   // a: the byte
    RX_RANGE,  // a .. b
    RX_SET,    // x: the set
    RX_SPLIT,  // go on at +x first, then at +y
    RX_JMP,    // +x
    RX_SAVE,   // a: the slot that learns the place
    RX_ASSERT, // a: RX_A_...
    RX_MATCH,
    RX_OP_COUNT
};
enum { RX_A_BOL, RX_A_EOL, RX_A_MBOL, RX_A_MEOL, RX_A_WORD, RX_A_NWORD, RX_A_COUNT };

typedef struct rx_inst {
    uint8_t op, a, b, pad;
    int16_t x, y;
} rx_inst;

typedef struct rx_head {
    uint8_t  magic[2]; // "RX"
    uint8_t  version, groups;
    uint16_t n_inst, n_sets;
    uint8_t  spare[8];
} rx_head;
static_assert(sizeof(rx_inst) == 8 && sizeof(rx_head) == 16, "the program layout is part of the format");

#define RX_MAX_INST 4096u
#define RX_MAX_SETS 1024u
#define RX_INF 0xffffffffu

// ---- compiling ----------------------------------------------------------------------------

// The program is written where it will lie: instructions from the front of `out`, sets from
// its end downwards (moved behind the instructions when the pattern is done).
typedef struct rx_comp {
    const uint8_t *p;
    uint32_t       n, i;
    uint8_t       *out;  // the head, then the instructions
    uint32_t       cap;  // bytes of out
    uint32_t       n_inst, n_sets;
    bool           room; // out was too small (not the pattern's fault)
    uint32_t       groups;
    bool           icase, multiline, dotall;
    const char    *why;
    uint32_t       at;
} rx_comp;

static bool rx_fail(rx_comp *c, const char *why) {
    if (!c->why) {
        c->why = why;
        c->at = c->i;
    }
    return false;
}

static rx_inst *rx_inst_at(const rx_comp *c, uint32_t k) {
    return (rx_inst *)(void *)(c->out + sizeof(rx_head) + (size_t)k * sizeof(rx_inst));
}
static uint8_t *rx_set_at(const rx_comp *c, uint32_t k) {
    return c->out + c->cap - (size_t)(k + 1) * 32u;
}
// Room for `insts` more instructions and `sets` more sets.
static bool rx_room(rx_comp *c, uint32_t insts, uint32_t sets) {
    if (c->n_inst + insts > RX_MAX_INST)
        return rx_fail(c, "the pattern makes more than 4096 instructions");
    if (sizeof(rx_head) + (size_t)(c->n_inst + insts) * sizeof(rx_inst) + (size_t)(c->n_sets + sets) * 32u > c->cap) {
        c->room = true;
        return rx_fail(c, "no room for the program");
    }
    return true;
}

static bool rx_emit(rx_comp *c, rx_inst in) {
    if (!rx_room(c, 1, 0))
        return false;
    *rx_inst_at(c, c->n_inst++) = in;
    return true;
}

// Room for one instruction at `at`; those after it move up (jumps are relative, so they hold).
static bool rx_insert(rx_comp *c, uint32_t at, rx_inst in) {
    if (!rx_room(c, 1, 0))
        return false;
    __builtin_memmove(rx_inst_at(c, at + 1), rx_inst_at(c, at), (c->n_inst - at) * sizeof(rx_inst));
    *rx_inst_at(c, at) = in;
    ++c->n_inst;
    return true;
}

static bool rx_set(rx_comp *c, const uint8_t set[32]) {
    for (uint32_t k = 0; k < c->n_sets; ++k) { // the same set again: the same number
        if (__builtin_memcmp(rx_set_at(c, k), set, 32) == 0)
            return rx_emit(c, (rx_inst){.op = RX_SET, .x = (int16_t)k});
    }
    if (c->n_sets == RX_MAX_SETS)
        return rx_fail(c, "the pattern has more than 1024 byte sets");
    if (!rx_room(c, 1, 1))
        return false;
    __builtin_memcpy(rx_set_at(c, c->n_sets), set, 32);
    return rx_emit(c, (rx_inst){.op = RX_SET, .x = (int16_t)c->n_sets++});
}

static void rx_bit(uint8_t set[32], uint32_t b) { set[b >> 3] |= (uint8_t)(1u << (b & 7)); }
static bool rx_has(const uint8_t set[32], uint32_t b) { return (set[b >> 3] >> (b & 7)) & 1u; }

static void rx_class(uint8_t set[32], uint8_t which) { // d w s
    for (uint32_t b = 0; b < 128; ++b) {
        const bool d = b >= '0' && b <= '9', a = (b | 0x20) >= 'a' && (b | 0x20) <= 'z';
        if ((which == 'd' && d) || (which == 'w' && (d || a || b == '_')) ||
            (which == 's' && (b == ' ' || (b >= '\t' && b <= '\r'))))
            rx_bit(set, b);
    }
}

static void rx_fold(uint8_t set[32]) { // ASCII letters in both cases
    for (uint32_t b = 'a'; b <= 'z'; ++b) {
        if (rx_has(set, b) || rx_has(set, b - 32))
            rx_bit(set, b), rx_bit(set, b - 32);
    }
}

// One code point as bytes: ASCII members of `ascii`, then the valid UTF-8 forms (when `multi`),
// then any other byte alone. Leftmost-first keeps the longest reading of a valid sequence.
static bool rx_codepoint(rx_comp *c, const uint8_t ascii[32]) {
    // forms: [ascii] | C2-DF 80-BF | E0-EF (80-BF){2} | F0-F4 (80-BF){3} | 80-FF
    static const uint8_t lead[4][2] = {{0xc2, 0xdf}, {0xe0, 0xef}, {0xf0, 0xf4}, {0x80, 0xff}};
    uint32_t jumps[5], n_jumps = 0;
    for (uint32_t form = 0; form < 5; ++form) {
        const uint32_t split = c->n_inst;
        if (form < 4 && !rx_emit(c, (rx_inst){.op = RX_SPLIT, .x = 1}))
            return false;
        if (form == 0) {
            if (!rx_set(c, ascii))
                return false;
        } else {
            if (!rx_emit(c, (rx_inst){.op = RX_RANGE, .a = lead[form - 1][0], .b = lead[form - 1][1]}))
                return false;
            for (uint32_t k = 0; form < 4 && k < form; ++k) {
                if (!rx_emit(c, (rx_inst){.op = RX_RANGE, .a = 0x80, .b = 0xbf}))
                    return false;
            }
        }
        if (form < 4) {
            jumps[n_jumps++] = c->n_inst;
            if (!rx_emit(c, (rx_inst){.op = RX_JMP}))
                return false;
            rx_inst_at(c, split)->y = (int16_t)(c->n_inst - split);
        }
    }
    for (uint32_t k = 0; k < n_jumps; ++k)
        rx_inst_at(c, jumps[k])->x = (int16_t)(c->n_inst - jumps[k]);
    return true;
}

static bool rx_alt(rx_comp *c, uint32_t depth);

static int rx_hex(uint8_t b) {
    return b >= '0' && b <= '9' ? b - '0' : (b | 0x20) >= 'a' && (b | 0x20) <= 'f' ? (b | 0x20) - 'a' + 10 : -1;
}

// After a backslash: the byte it stands for (0..255), or -1 when it is a class or an assertion
// that `out` describes: 'd' 'w' 's' 'D' 'W' 'S' 'b' 'B' 'z' (then \zs or \ze in `zse`).
static int rx_escape(rx_comp *c, uint8_t *what, uint8_t *zse) {
    if (c->i >= c->n)
        return rx_fail(c, "the pattern ends with a backslash"), -2;
    const uint8_t e = c->p[c->i++];
    switch (e) {
    case 'd': case 'w': case 's': case 'D': case 'W': case 'S': case 'b': case 'B': *what = e; return -1;
    case 'z':
        if (c->i < c->n && (c->p[c->i] == 's' || c->p[c->i] == 'e')) {
            *what = 'z';
            *zse = c->p[c->i++];
            return -1;
        }
        return rx_fail(c, "\\z is followed by s or e"), -2;
    case 'n': return '\n';
    case 't': return '\t';
    case 'r': return '\r';
    case 'f': return '\f';
    case 'v': return '\v';
    case '0': return 0;
    case 'x': {
        if (c->i + 2 > c->n || rx_hex(c->p[c->i]) < 0 || rx_hex(c->p[c->i + 1]) < 0)
            return rx_fail(c, "\\x is followed by two hex digits"), -2;
        const int v = rx_hex(c->p[c->i]) * 16 + rx_hex(c->p[c->i + 1]);
        c->i += 2;
        return v;
    }
    default:
        if ((e >= '1' && e <= '9') || e == 'k')
            return rx_fail(c, "back references are not supported"), -2;
        return e;
    }
}

static bool rx_bracket(rx_comp *c) { // after '['
    uint8_t set[32] = {0};
    bool    negated = false, first = true;
    if (c->i < c->n && c->p[c->i] == '^')
        negated = true, ++c->i;
    while (c->i < c->n && (first || c->p[c->i] != ']')) {
        first = false;
        int lo;
        if (c->p[c->i] == '\\') {
            ++c->i;
            uint8_t what = 0, zse = 0;
            lo = rx_escape(c, &what, &zse);
            if (lo == -2)
                return false;
            if (lo == -1) {
                if (what != 'd' && what != 'w' && what != 's' && what != 'D' && what != 'W' && what != 'S')
                    return rx_fail(c, "only \\d \\w \\s and their capitals stand in a set");
                uint8_t cls[32] = {0};
                rx_class(cls, (uint8_t)(what | 0x20));
                for (uint32_t b = 0; b < 256; ++b) {
                    if (what >= 'a' ? rx_has(cls, b) : !rx_has(cls, b))
                        rx_bit(set, b);
                }
                continue;
            }
        } else {
            lo = c->p[c->i++];
        }
        int hi = lo;
        if (c->i + 1 < c->n && c->p[c->i] == '-' && c->p[c->i + 1] != ']') {
            ++c->i;
            if (c->p[c->i] == '\\') {
                ++c->i;
                uint8_t what = 0, zse = 0;
                hi = rx_escape(c, &what, &zse);
                if (hi < 0)
                    return hi == -1 ? rx_fail(c, "a range ends with a byte") : false;
            } else {
                hi = c->p[c->i++];
            }
            if (hi < lo)
                return rx_fail(c, "a range goes downwards");
        }
        for (int b = lo; b <= hi; ++b)
            rx_bit(set, (uint32_t)b);
    }
    if (c->i >= c->n)
        return rx_fail(c, "a set is not closed by ]");
    ++c->i;
    if (c->icase)
        rx_fold(set);
    if (!negated)
        return rx_set(c, set);
    uint8_t ascii[32] = {0}; // a negated set takes one code point that is not in it
    for (uint32_t b = 0; b < 128; ++b) {
        if (!rx_has(set, b))
            rx_bit(ascii, b);
    }
    return rx_codepoint(c, ascii);
}

static bool rx_atom(rx_comp *c, uint32_t depth) {
    const uint8_t b = c->p[c->i++];
    switch (b) {
    case '(': {
        uint32_t group = 0;
        if (c->i + 1 < c->n && c->p[c->i] == '?' && c->p[c->i + 1] == ':') {
            c->i += 2;
        } else if (c->i < c->n && c->p[c->i] == '?') {
            return rx_fail(c, "(? is (?: here; flags stand only at the start of the pattern");
        } else {
            if (++c->groups > PNX_PTN_CAPS)
                return rx_fail(c, "a pattern has at most nine groups");
            group = c->groups;
        }
        if (group && !rx_emit(c, (rx_inst){.op = RX_SAVE, .a = (uint8_t)(2 * group)}))
            return false;
        if (!rx_alt(c, depth + 1))
            return false;
        if (c->i >= c->n || c->p[c->i] != ')')
            return rx_fail(c, "a group is not closed by )");
        ++c->i;
        return !group || rx_emit(c, (rx_inst){.op = RX_SAVE, .a = (uint8_t)(2 * group + 1)});
    }
    case '[': return rx_bracket(c);
    case '.': {
        uint8_t ascii[32] = {0};
        for (uint32_t k = 0; k < 128; ++k) {
            if (k != '\n' || c->dotall)
                rx_bit(ascii, k);
        }
        return rx_codepoint(c, ascii);
    }
    case '^': return rx_emit(c, (rx_inst){.op = RX_ASSERT, .a = c->multiline ? RX_A_MBOL : RX_A_BOL});
    case '$': return rx_emit(c, (rx_inst){.op = RX_ASSERT, .a = c->multiline ? RX_A_MEOL : RX_A_EOL});
    case '*': case '+': case '?': case '{': return rx_fail(c, "a repetition follows nothing");
    case ')': return rx_fail(c, "a ) closes no group");
    case '\\': {
        uint8_t what = 0, zse = 0;
        const int v = rx_escape(c, &what, &zse);
        if (v == -2)
            return false;
        if (v >= 0) {
            if (c->icase && ((v | 0x20) >= 'a' && (v | 0x20) <= 'z')) {
                uint8_t set[32] = {0};
                rx_bit(set, (uint32_t)v);
                rx_fold(set);
                return rx_set(c, set);
            }
            return rx_emit(c, (rx_inst){.op = RX_BYTE, .a = (uint8_t)v});
        }
        if (what == 'b' || what == 'B')
            return rx_emit(c, (rx_inst){.op = RX_ASSERT, .a = what == 'b' ? RX_A_WORD : RX_A_NWORD});
        if (what == 'z')
            return rx_emit(c, (rx_inst){.op = RX_SAVE, .a = zse == 's' ? 0 : 1});
        uint8_t set[32] = {0};
        rx_class(set, (uint8_t)(what | 0x20));
        if (what >= 'a')
            return rx_set(c, set);
        uint8_t ascii[32] = {0}; // \D \W \S: one code point outside the class
        for (uint32_t k = 0; k < 128; ++k) {
            if (!rx_has(set, k))
                rx_bit(ascii, k);
        }
        return rx_codepoint(c, ascii);
    }
    default:
        if (c->icase && ((b | 0x20) >= 'a' && (b | 0x20) <= 'z')) {
            uint8_t set[32] = {0};
            rx_bit(set, b);
            rx_fold(set);
            return rx_set(c, set);
        }
        return rx_emit(c, (rx_inst){.op = RX_BYTE, .a = b});
    }
}

// The instructions from `from` on, copied to the end.
static bool rx_copy(rx_comp *c, uint32_t from, uint32_t to) {
    if (!rx_room(c, to - from, 0))
        return false;
    __builtin_memcpy(rx_inst_at(c, c->n_inst), rx_inst_at(c, from), (to - from) * sizeof(rx_inst));
    c->n_inst += to - from;
    return true;
}

// The piece from `start` to the end repeated min to max times (max RX_INF: no bound). Copies are
// laid one after another; jumps are relative, so a copy needs no change.
static bool rx_repeat(rx_comp *c, uint32_t start, uint32_t min, uint32_t max, bool lazy) {
    const uint32_t len = c->n_inst - start;
    if (min > 1000 || (max != RX_INF && max > 1000))
        return rx_fail(c, "a counted repetition is at most 1000");
    if (max == 0) {
        c->n_inst = start;
        return true;
    }
    for (uint32_t k = 1; k < min; ++k) {
        if (!rx_copy(c, start, start + len))
            return false;
    }
    if (max == min)
        return true;
    if (max == RX_INF && min > 0) { // the last required copy loops: SPLIT back to it, out
        const int16_t back = (int16_t)-(int32_t)len;
        return rx_emit(c, lazy ? (rx_inst){.op = RX_SPLIT, .x = 1, .y = back} : (rx_inst){.op = RX_SPLIT, .x = back, .y = 1});
    }
    if (max == RX_INF) { // SPLIT body, out ; body ; JMP back to the SPLIT
        const int16_t out = (int16_t)(len + 2);
        if (!rx_insert(c, start, lazy ? (rx_inst){.op = RX_SPLIT, .x = out, .y = 1} : (rx_inst){.op = RX_SPLIT, .x = 1, .y = out}))
            return false;
        return rx_emit(c, (rx_inst){.op = RX_JMP, .x = (int16_t)-(int32_t)(len + 1)});
    }
    // max - min optional copies, each SPLIT body, past it
    const uint32_t optional = max - min, base = min ? c->n_inst : start;
    for (uint32_t k = min ? 0 : 1; k < optional; ++k) {
        if (!rx_copy(c, start, start + len))
            return false;
    }
    const int16_t past = (int16_t)(len + 1);
    for (uint32_t k = optional; k-- > 0;) { // from the last: an insertion moves only what follows it
        if (!rx_insert(c, base + k * len, lazy ? (rx_inst){.op = RX_SPLIT, .x = past, .y = 1}
                                               : (rx_inst){.op = RX_SPLIT, .x = 1, .y = past}))
            return false;
    }
    return true;
}

static bool rx_number(rx_comp *c, uint32_t *out) {
    uint32_t v = 0, digits = 0;
    while (c->i < c->n && c->p[c->i] >= '0' && c->p[c->i] <= '9' && digits < 5)
        v = v * 10 + (c->p[c->i++] - '0'), ++digits;
    *out = v;
    return digits > 0;
}

static bool rx_concat(rx_comp *c, uint32_t depth) {
    while (c->i < c->n && c->p[c->i] != '|' && c->p[c->i] != ')') {
        const uint32_t start = c->n_inst;
        if (!rx_atom(c, depth))
            return false;
        while (c->i < c->n) {
            const uint8_t q = c->p[c->i];
            uint32_t      min, max;
            if (q == '*' || q == '+' || q == '?') {
                ++c->i;
                min = q == '+' ? 1 : 0;
                max = q == '?' ? 1 : RX_INF;
            } else if (q == '{') {
                const uint32_t back = c->i++;
                if (!rx_number(c, &min)) {
                    c->i = back;
                    return rx_fail(c, "{ is followed by a count: {n}, {n,} or {n,m}");
                }
                max = min;
                if (c->i < c->n && c->p[c->i] == ',') {
                    ++c->i;
                    if (!rx_number(c, &max))
                        max = RX_INF;
                }
                if (c->i >= c->n || c->p[c->i] != '}')
                    return rx_fail(c, "a count is closed by }");
                ++c->i;
                if (max < min)
                    return rx_fail(c, "a counted repetition goes downwards");
            } else {
                break;
            }
            bool lazy = false;
            if (c->i < c->n && c->p[c->i] == '?')
                lazy = true, ++c->i;
            if (!rx_repeat(c, start, min, max, lazy))
                return false;
        }
    }
    return true;
}

static bool rx_alt(rx_comp *c, uint32_t depth) {
    if (depth > 64)
        return rx_fail(c, "groups are nested more than 64 deep");
    const uint32_t start = c->n_inst;
    if (!rx_concat(c, depth))
        return false;
    if (c->i >= c->n || c->p[c->i] != '|')
        return true;
    ++c->i;
    // SPLIT first, rest ; first ; JMP end ; rest
    if (!rx_insert(c, start, (rx_inst){.op = RX_SPLIT, .x = 1}))
        return false;
    const uint32_t jmp = c->n_inst;
    if (!rx_emit(c, (rx_inst){.op = RX_JMP}))
        return false;
    rx_inst_at(c, start)->y = (int16_t)(c->n_inst - start);
    if (!rx_alt(c, depth))
        return false;
    rx_inst_at(c, jmp)->x = (int16_t)(c->n_inst - jmp);
    return true;
}

// Compiles `pat` into `out` (`cap` bytes, the program's place): the length of the program, or
// 0 with `why` and `at` (the place in the pattern) set; `out` too small: 0 with `why` NULL.
uint32_t pnx_rex_compile(const uint8_t *pat, uint32_t plen, uint8_t *out, uint32_t cap, const char **why, uint32_t *at) {
    rx_comp  comp = {.p = pat, .n = plen, .out = out, .cap = cap};
    rx_comp *cc = &comp;
    if (cap < sizeof(rx_head)) {
        *why = NULL;
        return 0;
    }
    // flags at the very start: (?i) (?m) (?s), several letters in one
    if (plen >= 3 && pat[0] == '(' && pat[1] == '?' && pat[2] != ':') {
        uint32_t k = 2;
        for (; k < plen && pat[k] != ')'; ++k) {
            if (pat[k] == 'i')
                cc->icase = true;
            else if (pat[k] == 'm')
                cc->multiline = true;
            else if (pat[k] == 's')
                cc->dotall = true;
            else
                break;
        }
        if (k >= plen || pat[k] != ')') {
            *why = "the flags at the start are (?i) (?m) (?s)";
            *at = k;
            return 0;
        }
        cc->i = k + 1;
    }
    bool ok = rx_emit(cc, (rx_inst){.op = RX_SAVE, .a = 0}) && rx_alt(cc, 0);
    if (ok && cc->i < cc->n)
        ok = rx_fail(cc, "a ) closes no group");
    ok = ok && rx_emit(cc, (rx_inst){.op = RX_MATCH});
    if (!ok) {
        *why = cc->room ? NULL : cc->why ? cc->why : "the pattern is too large";
        *at = cc->at;
        return 0;
    }
    const uint32_t size = (uint32_t)(sizeof(rx_head) + cc->n_inst * sizeof(rx_inst) + cc->n_sets * 32u);
    const rx_head  head = {.magic = {'R', 'X'}, .version = 1, .groups = (uint8_t)cc->groups,
                           .n_inst = (uint16_t)cc->n_inst, .n_sets = (uint16_t)cc->n_sets};
    __builtin_memcpy(out, &head, sizeof head);
    // the sets, last first at the end, go behind the instructions in their order
    uint8_t *sets = out + sizeof head + cc->n_inst * sizeof(rx_inst);
    __builtin_memmove(sets, out + cap - cc->n_sets * 32u, cc->n_sets * 32u);
    for (uint32_t k = 0; k < cc->n_sets / 2; ++k) {
        uint8_t t[32];
        __builtin_memcpy(t, sets + k * 32u, 32);
        __builtin_memcpy(sets + k * 32u, sets + (cc->n_sets - 1 - k) * 32u, 32);
        __builtin_memcpy(sets + (cc->n_sets - 1 - k) * 32u, t, 32);
    }
    return size;
}

// ---- running ------------------------------------------------------------------------------

// The program's head, when the bytes are a program whose every jump, set and slot lies inside
// it; NULL otherwise. Checked at every use: a program may come from bytecode or a view.
static const rx_inst *rx_check(const uint8_t *prog, uint32_t len, rx_head *head) {
    if (len < sizeof *head)
        return NULL;
    __builtin_memcpy(head, prog, sizeof *head);
    if (head->magic[0] != 'R' || head->magic[1] != 'X' || head->version != 1 || head->groups > PNX_PTN_CAPS ||
        head->n_inst == 0 || head->n_inst > RX_MAX_INST || head->n_sets > RX_MAX_SETS ||
        len != sizeof *head + head->n_inst * sizeof(rx_inst) + head->n_sets * 32u)
        return NULL;
    const uint8_t *at = prog + sizeof *head;
    const uint32_t slots = 2u * (head->groups + 1u);
    for (uint32_t k = 0; k < head->n_inst; ++k) {
        rx_inst in;
        __builtin_memcpy(&in, at + k * sizeof in, sizeof in);
        const int64_t x = (int64_t)k + in.x, y = (int64_t)k + in.y;
        if (in.op >= RX_OP_COUNT || ((in.op == RX_SPLIT || in.op == RX_JMP) && (x < 0 || x >= head->n_inst)) ||
            (in.op == RX_SPLIT && (y < 0 || y >= head->n_inst)) || (in.op == RX_SET && (in.x < 0 || in.x >= head->n_sets)) ||
            (in.op == RX_SAVE && in.a >= slots) || (in.op == RX_ASSERT && in.a >= RX_A_COUNT) ||
            (in.op == RX_RANGE && in.a > in.b))
            return NULL;
        if (k + 1 == head->n_inst && in.op != RX_MATCH && in.op != RX_JMP && in.op != RX_SPLIT)
            return NULL; // the last one would go on past the program
    }
    return (const rx_inst *)(const void *)at; // 8-byte records after a 16-byte head; read with memcpy below
}

uint32_t pnx_rex_groups(const uint8_t *prog, uint32_t len) {
    rx_head head;
    return rx_check(prog, len, &head) ? head.groups : 0;
}

// The work a search needs: two lists of threads (a place and the slots each), the marks of the
// places a list has, and the stack that follows the jumps.
size_t pnx_rex_work(const uint8_t *prog, uint32_t len) {
    rx_head head;
    if (!rx_check(prog, len, &head))
        return 0;
    const size_t n = head.n_inst, slots = 2u * (head.groups + 1u);
    return 2 * (n * 4 + n * slots * 4) + n * 4 + (3 * n + 4) * 8 + slots * 4 + 64;
}

typedef struct rx_list {
    uint32_t  n;
    uint32_t *pc, *slots;
} rx_list;

typedef struct rx_run {
    const uint8_t *code; // instructions, unaligned
    const uint8_t *sets;
    const uint8_t *s;
    uint32_t       len, slots;
    uint32_t      *mark, gen;
    uint64_t      *stack;
    uint64_t       steps;
} rx_run;

static rx_inst rx_at(const rx_run *r, uint32_t pc) {
    rx_inst in;
    __builtin_memcpy(&in, r->code + (size_t)pc * sizeof in, sizeof in);
    return in;
}

static bool rx_word(const rx_run *r, uint32_t at) {
    if (at >= r->len)
        return false;
    const uint8_t b = r->s[at];
    return (b >= '0' && b <= '9') || ((b | 0x20) >= 'a' && (b | 0x20) <= 'z') || b == '_';
}

static bool rx_holds(const rx_run *r, uint32_t kind, uint32_t sp) {
    switch (kind) {
    case RX_A_BOL: return sp == 0;
    case RX_A_EOL: return sp == r->len;
    case RX_A_MBOL: return sp == 0 || r->s[sp - 1] == '\n';
    case RX_A_MEOL: return sp == r->len || r->s[sp] == '\n';
    default: {
        const bool b = (sp > 0 && rx_word(r, sp - 1)) != rx_word(r, sp);
        return kind == RX_A_WORD ? b : !b;
    }
    }
}

// Adds the thread at `pc` with `slots` to list l, following jumps, splits, saves and
// assertions on the stack, in priority order; each place once per list.
static void rx_add(rx_run *r, rx_list *l, uint32_t pc, uint32_t *slots, uint32_t sp) {
    uint32_t top = 0;
    r->stack[top++] = pc; // a place to visit; with bit 63 set: a slot to restore (slot << 32 | value)
    while (top) {
        const uint64_t e = r->stack[--top];
        if (e >> 63) {
            slots[(e >> 32) & 0x7fffffffu] = (uint32_t)e;
            continue;
        }
        pc = (uint32_t)e;
        if (r->mark[pc] == r->gen)
            continue;
        r->mark[pc] = r->gen;
        ++r->steps;
        const rx_inst in = rx_at(r, pc);
        switch (in.op) {
        case RX_JMP: r->stack[top++] = (uint32_t)((int32_t)pc + in.x); break;
        case RX_SPLIT:
            r->stack[top++] = (uint32_t)((int32_t)pc + in.y);
            r->stack[top++] = (uint32_t)((int32_t)pc + in.x);
            break;
        case RX_SAVE:
            r->stack[top++] = (uint64_t)1 << 63 | (uint64_t)in.a << 32 | slots[in.a];
            slots[in.a] = sp;
            r->stack[top++] = pc + 1;
            break;
        case RX_ASSERT:
            if (rx_holds(r, in.a, sp))
                r->stack[top++] = pc + 1;
            break;
        default: // a thread that waits for a byte, or one that matched
            l->pc[l->n] = pc;
            __builtin_memcpy(&l->slots[(size_t)l->n * r->slots], slots, r->slots * 4);
            ++l->n;
            break;
        }
    }
}

static bool rx_takes(const rx_run *r, rx_inst in, uint8_t b) {
    switch (in.op) {
    case RX_BYTE: return b == in.a;
    case RX_RANGE: return b >= in.a && b <= in.b;
    case RX_SET: return (r->sets[(size_t)in.x * 32 + (b >> 3)] >> (b & 7)) & 1u;
    default: return false;
    }
}

// The leftmost match in s at or after `from`: 1 (found: the places of the whole match and the
// groups in `found`, 2 per group, PNX_REX_NONE for a group that did not take part), 0 none,
// -1 not a program. `work` has pnx_rex_work() bytes, 8-byte aligned.
int pnx_rex_search(const uint8_t *prog, uint32_t plen, const uint8_t *s, uint32_t len, uint32_t from, uint32_t *found,
                   void *work, uint64_t *steps) {
    rx_head head;
    if (!rx_check(prog, plen, &head))
        return -1;
    const uint32_t n = head.n_inst, slots = 2u * (head.groups + 1u);
    uint8_t       *w = work;
    rx_run         r = {.code = prog + sizeof head, .sets = prog + sizeof head + (size_t)n * sizeof(rx_inst), .s = s,
                        .len = len, .slots = slots};
    r.stack = (uint64_t *)(void *)w;
    w += (3 * (size_t)n + 4) * 8;
    r.mark = (uint32_t *)(void *)w;
    w += (size_t)n * 4;
    rx_list a = {0, (uint32_t *)(void *)w, NULL}, b;
    w += (size_t)n * 4;
    a.slots = (uint32_t *)(void *)w;
    w += (size_t)n * slots * 4;
    b = (rx_list){0, (uint32_t *)(void *)w, NULL};
    w += (size_t)n * 4;
    b.slots = (uint32_t *)(void *)w;
    w += (size_t)n * slots * 4;
    uint32_t *fresh = (uint32_t *)(void *)w;
    for (uint32_t k = 0; k < n; ++k)
        r.mark[k] = 0;
    r.gen = 1;
    rx_list *now = &a, *next = &b;
    bool     matched = false;
    for (uint32_t sp = from;; ++sp) {
        if (!matched) { // a new thread starts here, after all the others
            for (uint32_t k = 0; k < slots; ++k)
                fresh[k] = PNX_REX_NONE;
            rx_add(&r, now, 0, fresh, sp);
        }
        if (now->n == 0 && (matched || sp >= len))
            break;
        ++r.gen;
        next->n = 0;
        for (uint32_t t = 0; t < now->n; ++t) {
            const rx_inst in = rx_at(&r, now->pc[t]);
            uint32_t     *ts = &now->slots[(size_t)t * slots];
            if (in.op == RX_MATCH) { // the threads after it have lower priority: dropped
                __builtin_memcpy(found, ts, slots * 4);
                if (found[1] == PNX_REX_NONE)
                    found[1] = sp;
                matched = true;
                break;
            }
            if (sp < len && rx_takes(&r, in, s[sp]))
                rx_add(&r, next, now->pc[t] + 1, ts, sp + 1);
        }
        r.steps += now->n;
        rx_list *swap = now;
        now = next;
        next = swap;
        if (sp >= len)
            break;
    }
    *steps += r.steps;
    if (matched && found[0] == PNX_REX_NONE) // \zs never reached cannot happen: slot 0 is set first
        found[0] = from;
    if (matched && found[1] < found[0]) // \ze before \zs: an empty match at \zs
        found[1] = found[0];
    return matched;
}
