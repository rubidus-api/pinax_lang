// pnx_fmt.c - the formatter behind `pnx fmt` (RFC-0001 section 12).
//
// What it fixes is the layout of lines: the indentation follows the blocks (two spaces a
// level), white space at the end of a line goes, several empty lines become one, the text
// ends with one line break. What stands on a line is left as written, so the tokens, their
// lines and the code the compiler makes of them are the same before and after.
//
// It reads the tokens with the compiler's own lexer and runs nothing.
#include "pnx_fmt.h"

#include "pinax_lex.h"

#include <stdlib.h>
#include <string.h>

static bool opens(const pnx_token *t) {
    if (t->kind != PNX_T_RESERVED)
        return false;
    switch (t->word) {
    case PNX_KW_word: case PNX_KW_test: case PNX_KW_record: case PNX_KW_do: case PNX_KW_if: case PNX_KW_loop:
    case PNX_KW_case: case PNX_KW_of: case PNX_KW_otherwise: case PNX_KW_or_else: case PNX_KW_defer: return true;
    default: return false;
    }
}

static bool is_word(const pnx_token *t, pnx_word_id word) {
    return t->kind == PNX_T_RESERVED && t->word == word;
}

int pnx_format(const char *text, size_t length, FILE *out, const char *name) {
    // every token, with the line it ends on
    pnx_lexer lx;
    pnx_lex_init(&lx, (const uint8_t *)text, length);
    size_t     n = 0, cap = 1024;
    pnx_token *tokens = malloc(cap * sizeof *tokens);
    if (!tokens)
        return 2;
    for (;;) {
        if (n == cap) {
            pnx_token *grown = realloc(tokens, (cap *= 2) * sizeof *tokens);
            if (!grown) {
                free(tokens);
                return 2;
            }
            tokens = grown;
        }
        if (!pnx_lex_next(&lx, &tokens[n])) {
            fprintf(stderr, "%s:%u:%u: %s\n", name, (unsigned)lx.error_line, (unsigned)lx.error_column, lx.error);
            free(tokens);
            return 1;
        }
        if (tokens[n].kind == PNX_T_END)
            break;
        ++n;
    }

    size_t   at = 0, token = 0; // the byte and the token the next line starts with
    int      depth = 0, parens = 0;
    bool     blank_before = false, any = false;
    size_t   inside_until = 0; // a token that began on an earlier line runs up to this byte
    while (at < length) {
        size_t end = at;
        while (end < length && text[end] != '\n')
            ++end;
        size_t last = end; // without white space at the end
        while (last > at && (text[last - 1] == ' ' || text[last - 1] == '\t' || text[last - 1] == '\r'))
            --last;
        size_t first = at;
        while (first < last && (text[first] == ' ' || text[first] == '\t'))
            ++first;

        const bool continued = at < inside_until || parens > 0; // inside a text literal or a list in parentheses
        int        indent = depth;
        bool       leading = true;
        for (; token < n && tokens[token].start < end; ++token) {
            const pnx_token *t = &tokens[token];
            if (t->kind == PNX_T_LPAREN) {
                ++parens;
            } else if (t->kind == PNX_T_RPAREN) {
                if (parens)
                    --parens;
            } else if (parens == 0 && is_word(t, PNX_KW_end)) {
                if (depth)
                    --depth;
                if (leading)
                    indent = depth;
                continue; // a run of `end` at the start of a line closes that many levels
            } else if (parens == 0 && is_word(t, PNX_KW_else)) {
                if (leading && depth)
                    indent = depth - 1;
            } else if (parens == 0 && opens(t)) {
                ++depth;
            }
            leading = false;
            if ((size_t)t->start + t->length > end)
                inside_until = (size_t)t->start + t->length;
        }

        if (first == last && !continued) { // an empty line: at most one, and none at the start
            blank_before = any;
        } else {
            if (blank_before)
                fputc('\n', out);
            blank_before = false;
            any = true;
            if (continued) {
                fwrite(text + at, 1, last - at, out); // as written, but for the white space at its end
            } else {
                for (int k = 0; k < indent * 2; ++k)
                    fputc(' ', out);
                fwrite(text + first, 1, last - first, out);
            }
            fputc('\n', out);
        }
        at = end + 1;
    }
    free(tokens);
    return 0;
}
