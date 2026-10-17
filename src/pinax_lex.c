// pinax_lex.c - the tokenizer (RFC-0013 S1). Rules: RFC-0008 section 3 and the grammar manual,
// section 2. Number values and UTF-8 come from the vendored proven (D0017).
#include "pinax_lex.h"

#include "proven/float_parse.h"
#include "proven/scan.h"
#include "proven/utf.h"

#include <stdckdint.h>

static const char *const word_names[PNX_KW_COUNT] = {
    [PNX_KW_NONE] = "",
#define PNX_WORD_NAME(name) [PNX_KW_##name] = #name,
    PNX_RESERVED(PNX_WORD_NAME)
#undef PNX_WORD_NAME
};

const char *pnx_word_name(pnx_word_id word) {
    return word < PNX_KW_COUNT ? word_names[word] : "";
}

static bool is_letter(uint8_t c) {
    return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || c == '_';
}
static bool lex_is_digit(uint8_t c) {
    return c >= '0' && c <= '9';
}
static bool is_run(uint8_t c) {
    return is_letter(c) || lex_is_digit(c);
}
static int lex_hex_value(uint8_t c) {
    if (c >= '0' && c <= '9')
        return c - '0';
    if (c >= 'a' && c <= 'f')
        return c - 'a' + 10;
    if (c >= 'A' && c <= 'F')
        return c - 'A' + 10;
    return -1;
}

void pnx_lex_init(pnx_lexer *lx, const uint8_t *source, size_t length) {
    *lx = (pnx_lexer){.source = source, .length = length > UINT32_MAX ? UINT32_MAX : (uint32_t)length, .line = 1};
    if (lx->length >= 2 && source[0] == '#' && source[1] == '!') { // shebang line
        while (lx->at < lx->length && source[lx->at] != '\n')
            ++lx->at;
    }
}

static bool fail_at(pnx_lexer *lx, uint32_t at, const char *message) {
    lx->error = message;
    lx->error_line = lx->line;
    lx->error_column = at - lx->line_start + 1;
    return false;
}

// The byte at `at`, or 0 past the end. A NUL in the source is not valid anywhere a token can
// start, so 0 is a safe "nothing here".
static uint8_t peek(const pnx_lexer *lx, uint32_t at) {
    return at < lx->length ? lx->source[at] : 0;
}

static pnx_word_id reserved_word(const uint8_t *p, uint32_t n) {
    for (int w = 1; w < PNX_KW_COUNT; ++w) {
        const char *name = word_names[w];
        uint32_t k = 0;
        while (k < n && name[k] != '\0' && (uint8_t)name[k] == p[k])
            ++k;
        if (k == n && name[k] == '\0')
            return (pnx_word_id)w;
    }
    return PNX_KW_NONE;
}

// ---- numbers ----------------------------------------------------------------------------

// digit { [ "_" ] digit } in the given base; copies the digits without underscores.
static bool digit_run(pnx_lexer *lx, uint32_t *at, int base, char *out, uint32_t *n, uint32_t cap) {
    uint32_t i = *at;
    int v = lex_hex_value(peek(lx, i));
    if (v < 0 || v >= base)
        return fail_at(lx, i, "a digit is expected here");
    for (;;) {
        if (*n + 1 >= cap)
            return fail_at(lx, i, "the number is too long");
        out[(*n)++] = (char)lx->source[i++];
        if (peek(lx, i) == '_') {
            v = lex_hex_value(peek(lx, i + 1));
            if (v < 0 || v >= base)
                return fail_at(lx, i, "an underscore stands between two digits");
            ++i;
            continue;
        }
        v = lex_hex_value(peek(lx, i));
        if (v < 0 || v >= base)
            break;
    }
    *at = i;
    return true;
}

// [ sign ] digits, for exponents.
static bool exponent(pnx_lexer *lx, uint32_t *at, int32_t *value) {
    uint32_t i = *at;
    bool negative = false;
    if (peek(lx, i) == '+' || peek(lx, i) == '-') {
        negative = peek(lx, i) == '-';
        ++i;
    }
    char digits[16];
    uint32_t n = 0;
    if (!digit_run(lx, &i, 10, digits, &n, sizeof digits))
        return false;
    int32_t v = 0;
    for (uint32_t k = 0; k < n; ++k) {
        if (v > 100000)
            return fail_at(lx, *at, "the exponent is too large");
        v = v * 10 + (digits[k] - '0');
    }
    *value = negative ? -v : v;
    *at = i;
    return true;
}

static bool magnitude(pnx_lexer *lx, uint32_t at, const char *digits, uint32_t n, int base, uint64_t *out) {
    uint64_t v = 0;
    for (uint32_t k = 0; k < n; ++k) {
        if (ckd_mul(&v, v, (uint64_t)base) || ckd_add(&v, v, (uint64_t)lex_hex_value((uint8_t)digits[k])))
            return fail_at(lx, at, "the integer does not fit in 64 bits");
    }
    *out = v;
    return true;
}

static bool signed_value(pnx_lexer *lx, uint32_t at, uint64_t m, bool negative, int64_t *out) {
    if (negative ? m > (uint64_t)INT64_MAX + 1u : m > (uint64_t)INT64_MAX)
        return fail_at(lx, at, "the integer does not fit in 64 bits");
    *out = negative ? (int64_t)(0u - m) : (int64_t)m;
    return true;
}

static bool number(pnx_lexer *lx, pnx_token *t) {
    const uint32_t start = lx->at;
    uint32_t i = start;
    char buf[96];
    uint32_t n = 0;
    bool negative = false, real = false, exp = false;
    uint32_t dot_at = 0;
    if (peek(lx, i) == '+' || peek(lx, i) == '-') {
        negative = peek(lx, i) == '-';
        buf[n++] = (char)peek(lx, i);
        ++i;
    }
    if (peek(lx, i) == '0' && (peek(lx, i + 1) == 'x' || peek(lx, i + 1) == 'b')) {
        const int base = peek(lx, i + 1) == 'x' ? 16 : 2;
        i += 2;
        char digits[80], fraction[80];
        uint32_t nd = 0, nf = 0;
        if (!digit_run(lx, &i, base, digits, &nd, sizeof digits))
            return false;
        bool dot = false;
        if (base == 16 && peek(lx, i) == '.' && lex_hex_value(peek(lx, i + 1)) >= 0) {
            dot = true;
            ++i;
            if (!digit_run(lx, &i, 16, fraction, &nf, sizeof fraction))
                return false;
        }
        if (base == 16 && peek(lx, i) == 'p') {
            int32_t e = 0;
            ++i;
            if (!exponent(lx, &i, &e))
                return false;
            double v = 0.0;
            for (uint32_t k = 0; k < nd; ++k)
                v = v * 16.0 + lex_hex_value((uint8_t)digits[k]);
            double scale = 1.0 / 16.0;
            for (uint32_t k = 0; k < nf; ++k, scale /= 16.0)
                v += lex_hex_value((uint8_t)fraction[k]) * scale;
            // at most 79 digits each side: past 1500 the value is already infinite or 0, and a
            // longer loop would make a few bytes of source cost a million steps
            e = e > 1500 ? 1500 : e < -1500 ? -1500 : e;
            for (; e > 0; --e)
                v *= 2.0;
            for (; e < 0; ++e)
                v /= 2.0;
            t->real = negative ? -v : v;
            real = true;
        } else {
            if (dot)
                return fail_at(lx, i, "a hexadecimal real needs a p exponent");
            uint64_t m;
            if (!magnitude(lx, start, digits, nd, base, &m) || !signed_value(lx, start, m, negative, &t->integer))
                return false;
        }
    } else {
        if (!digit_run(lx, &i, 10, buf, &n, sizeof buf - 40))
            return false;
        if (peek(lx, i) == '.' && lex_is_digit(peek(lx, i + 1))) {
            real = true;
            dot_at = n;
            buf[n++] = '.';
            ++i;
            if (!digit_run(lx, &i, 10, buf, &n, sizeof buf - 20))
                return false;
        }
        if (peek(lx, i) == 'e' && (lex_is_digit(peek(lx, i + 1)) ||
                                   ((peek(lx, i + 1) == '+' || peek(lx, i + 1) == '-') && lex_is_digit(peek(lx, i + 2))))) {
            real = true;
            exp = true;
            buf[n++] = 'e';
            ++i;
            if (peek(lx, i) == '+' || peek(lx, i) == '-')
                buf[n++] = (char)lx->source[i++];
            if (!digit_run(lx, &i, 10, buf, &n, sizeof buf))
                return false;
        }
        if (real && !exp) { // a decimal: the digits without the point, and how many follow it
            const uint32_t scale = n - dot_at - 1;
            if (scale > 18)
                return fail_at(lx, start, "a decimal has at most 18 digits after the point");
            __builtin_memmove(buf + dot_at, buf + dot_at + 1, scale);
            --n;
            const proven_u8str_view_t digits = {(const proven_byte_t *)buf, n};
            proven_scan_t scan = proven_scan_init(digits);
            const proven_result_i64_t r = proven_scan_i64(&scan);
            if (r.err != PROVEN_OK || scan.cursor != n)
                return fail_at(lx, start, "the decimal does not fit in 64 bits at its scale");
            if (is_run(peek(lx, i)) || peek(lx, i) == '.')
                return fail_at(lx, i, "a number must not run into letters, an underscore or a dot");
            t->integer = r.val;
            t->scale = (uint8_t)scale;
            t->kind = PNX_T_DEC;
            lx->at = i;
            return true;
        }
        const proven_u8str_view_t view = {(const proven_byte_t *)buf, n};
        if (real) {
            const proven_parse_double_result_t r = proven_parse_double_ascii(view);
            if (r.err != PROVEN_OK || r.consumed != n)
                return fail_at(lx, start, "the real number cannot be read");
            t->real = r.val;
        } else {
            proven_scan_t scan = proven_scan_init(view);
            const proven_result_i64_t r = proven_scan_i64(&scan);
            if (r.err != PROVEN_OK || scan.cursor != n)
                return fail_at(lx, start, "the integer does not fit in 64 bits");
            t->integer = r.val;
        }
    }
    if (is_run(peek(lx, i)) || peek(lx, i) == '.')
        return fail_at(lx, i, "a number must not run into letters, an underscore or a dot");
    t->kind = real ? PNX_T_FLOAT : PNX_T_INT;
    lx->at = i;
    return true;
}

// ---- characters and strings -------------------------------------------------------------

// Checks the escape at `at` (the backslash) and reports its length and value.
// `scalar` is set for \u and \U, whose value is a code point rather than a byte.
static bool escape(pnx_lexer *lx, uint32_t at, uint32_t *length, uint32_t *value, bool *scalar) {
    const uint8_t c = peek(lx, at + 1);
    uint32_t digits = 0;
    *scalar = false;
    *length = 2;
    switch (c) {
    case '\\': *value = '\\'; return true;
    case '"': *value = '"'; return true;
    case '\'': *value = '\''; return true;
    case 'a': *value = 7; return true;
    case 'b': *value = 8; return true;
    case 'f': *value = 12; return true;
    case 'n': *value = 10; return true;
    case 'r': *value = 13; return true;
    case 't': *value = 9; return true;
    case 'v': *value = 11; return true;
    case '0': *value = 0; return true;
    case 'x': digits = 2; break;
    case 'u': digits = 4; *scalar = true; break;
    case 'U': digits = 8; *scalar = true; break;
    default: return fail_at(lx, at, "unknown escape; a raw string r\"...\" keeps backslashes as written");
    }
    uint32_t v = 0;
    for (uint32_t k = 0; k < digits; ++k) {
        const int h = lex_hex_value(peek(lx, at + 2 + k));
        if (h < 0)
            return fail_at(lx, at, "the escape needs more hexadecimal digits");
        v = v << 4 | (uint32_t)h;
    }
    if (*scalar && (v > 0x10ffffu || (v >= 0xd800u && v <= 0xdfffu)))
        return fail_at(lx, at, "the escape is not a code point");
    *value = v;
    *length = 2 + digits;
    return true;
}

static uint32_t put_utf8(uint32_t cp, uint8_t *out) {
    if (cp < 0x80u) {
        out[0] = (uint8_t)cp;
        return 1;
    }
    if (cp < 0x800u) {
        out[0] = (uint8_t)(0xc0u | cp >> 6);
        out[1] = (uint8_t)(0x80u | (cp & 0x3fu));
        return 2;
    }
    if (cp < 0x10000u) {
        out[0] = (uint8_t)(0xe0u | cp >> 12);
        out[1] = (uint8_t)(0x80u | ((cp >> 6) & 0x3fu));
        out[2] = (uint8_t)(0x80u | (cp & 0x3fu));
        return 3;
    }
    out[0] = (uint8_t)(0xf0u | cp >> 18);
    out[1] = (uint8_t)(0x80u | ((cp >> 12) & 0x3fu));
    out[2] = (uint8_t)(0x80u | ((cp >> 6) & 0x3fu));
    out[3] = (uint8_t)(0x80u | (cp & 0x3fu));
    return 4;
}

// 'a'  '\n'  U'x' ; lx->at is at the quote.
static bool character(pnx_lexer *lx, pnx_token *t, bool wide) {
    uint32_t i = lx->at + 1;
    const uint8_t c = peek(lx, i);
    if (i >= lx->length || c == '\n' || c == '\r')
        return fail_at(lx, lx->at, "the character literal is not closed");
    if (c == '\'')
        return fail_at(lx, lx->at, "the character literal is empty");
    if (c == '\\') {
        uint32_t length, value;
        bool scalar;
        if (!escape(lx, i, &length, &value, &scalar))
            return false;
        if (scalar && !wide && value > 0x7fu)
            return fail_at(lx, i, "a code point above 127 needs the U prefix");
        t->integer = value;
        i += length;
    } else if (c < 0x80u) {
        t->integer = c;
        ++i;
    } else {
        if (!wide)
            return fail_at(lx, i, "a character outside ASCII needs the U prefix");
        const proven_utf8_char_t ch =
            proven_utf8_decode_next((proven_u8str_view_t){lx->source, lx->length}, i);
        if (ch.err != PROVEN_OK)
            return fail_at(lx, i, "the source is not valid UTF-8 here");
        t->integer = ch.cp;
        i += (uint32_t)ch.len;
    }
    if (peek(lx, i) != '\'')
        return fail_at(lx, i, "a character literal holds one character");
    lx->at = i + 1;
    t->kind = PNX_T_CHAR;
    return true;
}

// "..." or r"..." ; lx->at is at the quote.
static bool string(pnx_lexer *lx, pnx_token *t, bool raw) {
    uint32_t i = lx->at + 1;
    t->body = i;
    t->escapes = false;
    for (;;) {
        const uint8_t c = peek(lx, i);
        if (i >= lx->length || c == '\n' || c == '\r')
            return fail_at(lx, lx->at, "the string is not closed on its line");
        if (c == '"')
            break;
        if (c == '\\' && !raw) {
            uint32_t length, value;
            bool scalar;
            if (!escape(lx, i, &length, &value, &scalar))
                return false;
            t->escapes = true;
            i += length;
        } else {
            ++i;
        }
    }
    t->body_length = i - t->body;
    t->kind = raw ? PNX_T_RAW : PNX_T_STRING;
    lx->at = i + 1;
    return true;
}

uint32_t pnx_lex_decode(const pnx_lexer *lx, const pnx_token *t, uint8_t *out) {
    uint32_t n = 0;
    pnx_lexer scratch = *lx; // escape() reports through the lexer; the token was checked already
    for (uint32_t i = t->body; i < t->body + t->body_length;) {
        if (lx->source[i] != '\\' || t->kind != PNX_T_STRING) {
            out[n++] = lx->source[i++];
            continue;
        }
        uint32_t length = 1, value = 0;
        bool scalar = false;
        if (!escape(&scratch, i, &length, &value, &scalar))
            break;
        if (scalar)
            n += put_utf8(value, out + n);
        else
            out[n++] = (uint8_t)value;
        i += length;
    }
    return n;
}

// ---- remarks and text -------------------------------------------------------------------

static void skip_line(pnx_lexer *lx) {
    while (lx->at < lx->length && lx->source[lx->at] != '\n')
        ++lx->at;
}

// After `note` or `text`: the tag, then lines up to the line that starts with the tag in the
// first column. For text the body is reported and nothing else may follow the tag on the
// opening line.
static bool tagged_block(pnx_lexer *lx, pnx_token *t, bool is_text) {
    const uint32_t opened = lx->at;
    while (peek(lx, lx->at) == ' ' || peek(lx, lx->at) == '\t')
        ++lx->at;
    const uint32_t tag = lx->at;
    if (!is_letter(peek(lx, tag)))
        return fail_at(lx, tag, is_text ? "text needs a tag: text TAG" : "note needs a tag: note TAG");
    while (is_run(peek(lx, lx->at)))
        ++lx->at;
    const uint32_t tag_length = lx->at - tag;
    if (reserved_word(lx->source + tag, tag_length) != PNX_KW_NONE)
        return fail_at(lx, tag, "the tag is a reserved word");
    if (is_text) {
        for (; lx->at < lx->length && lx->source[lx->at] != '\n'; ++lx->at) {
            const uint8_t c = lx->source[lx->at];
            if (c != ' ' && c != '\t' && c != '\r')
                return fail_at(lx, lx->at, "the text starts on the next line; nothing follows the tag");
        }
    } else {
        skip_line(lx);
    }
    if (lx->at >= lx->length)
        return fail_at(lx, opened, is_text ? "the text is not closed by its tag" : "the note is not closed by its tag");
    const uint32_t opening_line = lx->line, opening_start = lx->line_start;
    ++lx->at; // the newline
    ++lx->line;
    lx->line_start = lx->at;
    const uint32_t body = lx->at;
    for (;;) {
        const uint32_t line = lx->at;
        bool closes = lx->length - line >= tag_length;
        for (uint32_t k = 0; closes && k < tag_length; ++k)
            closes = lx->source[line + k] == lx->source[tag + k];
        if (closes && !is_run(peek(lx, line + tag_length))) {
            lx->at = line + tag_length;
            for (uint32_t k = lx->at; k < lx->length && lx->source[k] != '\n'; ++k) {
                const uint8_t c = lx->source[k];
                if (c != ' ' && c != '\t' && c != '\r')
                    return fail_at(lx, k, "only white space follows the closing tag on its line");
            }
            if (is_text) {
                uint32_t end = line; // drop the one line break before the closing line
                if (end > body && lx->source[end - 1] == '\n')
                    --end;
                if (end > body && lx->source[end - 1] == '\r')
                    --end;
                t->kind = PNX_T_TEXT;
                t->body = body;
                t->body_length = end - body;
            }
            return true;
        }
        skip_line(lx);
        if (lx->at >= lx->length) {
            lx->line = opening_line;
            lx->line_start = opening_start;
            return fail_at(lx, opened, is_text ? "the text is not closed by its tag" : "the note is not closed by its tag");
        }
        ++lx->at;
        ++lx->line;
        lx->line_start = lx->at;
    }
}

// ---- the tokenizer ----------------------------------------------------------------------

bool pnx_lex_next(pnx_lexer *lx, pnx_token *t) {
    for (;;) {
        // white space
        while (lx->at < lx->length) {
            const uint8_t c = lx->source[lx->at];
            if (c == '\n') {
                ++lx->at;
                ++lx->line;
                lx->line_start = lx->at;
            } else if (c == ' ' || c == '\t' || c == '\r') {
                ++lx->at;
            } else {
                break;
            }
        }
        *t = (pnx_token){.kind = PNX_T_END, .start = lx->at, .line = lx->line, .column = lx->at - lx->line_start + 1};
        if (lx->at >= lx->length)
            return true;
        const uint8_t c = lx->source[lx->at];

        if (c == '(' || c == ')' || c == ',') {
            t->kind = c == '(' ? PNX_T_LPAREN : c == ')' ? PNX_T_RPAREN : PNX_T_COMMA;
            t->length = 1;
            ++lx->at;
            return true;
        }
        if (lex_is_digit(c) || ((c == '+' || c == '-') && lex_is_digit(peek(lx, lx->at + 1)))) {
            if (!number(lx, t))
                return false;
            t->length = lx->at - t->start;
            return true;
        }
        if (c == '"') {
            if (!string(lx, t, false))
                return false;
            t->length = lx->at - t->start;
            return true;
        }
        if (c == '\'') {
            if (!character(lx, t, false))
                return false;
            t->length = lx->at - t->start;
            return true;
        }
        if (!is_letter(c)) {
            if (c == '.')
                return fail_at(lx, lx->at, "a dot joins two names or sits inside a number; it does not stand alone");
            if (c == '+' || c == '-')
                return fail_at(lx, lx->at, "a sign is attached to its number; arithmetic uses words (add, sub)");
            return fail_at(lx, lx->at, "this character is not allowed outside literals and remarks");
        }

        // a run: reserved word, name, path, or the prefix of a literal
        uint32_t i = lx->at;
        while (is_run(peek(lx, i)))
            ++i;
        const uint32_t run = i - lx->at;
        if (peek(lx, i) == '\'') {
            if (run != 1 || c != 'U')
                return fail_at(lx, lx->at, "the only prefix of a character literal is U");
            lx->at = i;
            if (!character(lx, t, true))
                return false;
            t->length = lx->at - t->start;
            return true;
        }
        if (peek(lx, i) == '"') {
            if (run != 1 || c != 'r')
                return fail_at(lx, lx->at, "the only prefix of a string is r (raw string)");
            lx->at = i;
            if (!string(lx, t, true))
                return false;
            t->length = lx->at - t->start;
            return true;
        }
        const pnx_word_id word = reserved_word(lx->source + lx->at, run);
        if (word != PNX_KW_NONE) {
            if (peek(lx, i) == '.')
                return fail_at(lx, i, "a reserved word is not part of a path");
            lx->at = i;
            if (word == PNX_KW_rem) {
                skip_line(lx);
                continue;
            }
            if (word == PNX_KW_note) {
                if (!tagged_block(lx, t, false))
                    return false;
                continue;
            }
            if (word == PNX_KW_text) {
                if (!tagged_block(lx, t, true))
                    return false;
                t->length = lx->at - t->start;
                return true;
            }
            t->kind = PNX_T_RESERVED;
            t->word = word;
            t->length = run;
            return true;
        }
        t->kind = PNX_T_NAME;
        while (peek(lx, i) == '.') {
            const uint32_t part = i + 1;
            if (!is_letter(peek(lx, part)))
                return fail_at(lx, i, "a dot is followed by a name, with no space");
            uint32_t k = part;
            while (is_run(peek(lx, k)))
                ++k;
            // After a dot a run is a member name, reserved or not (json.array): only what a module
            // defines itself can be named so, since a script declares no reserved name.
            if (peek(lx, k) == '"' || peek(lx, k) == '\'')
                return fail_at(lx, k, "a literal must be separated from the name before it");
            t->kind = PNX_T_PATH;
            i = k;
        }
        t->length = i - lx->at;
        lx->at = i;
        return true;
    }
}
