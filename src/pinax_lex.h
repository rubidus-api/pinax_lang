// pinax_lex.h - tokens of Pinax source (RFC-0008 section 3; docs: the grammar manual, section 2).
// Not a public header.
#ifndef PINAX_LEX_H
#define PINAX_LEX_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// Reserved words. The order is the order of the table in pinax_lex.c.
#define PNX_RESERVED(X)                                                                            \
    X(rem) X(note) X(text) X(true) X(false) X(pinax)                                               \
    X(use) X(as) X(export)                                                                         \
    X(word) X(global) X(const) X(array) X(list) X(pool) X(queue) X(buffer) X(tasks) X(record)     \
    X(type) X(extern) X(from) X(forward) X(index) X(bound) X(make) X(test)                         \
    X(row) X(assign) X(big_endian) X(columns)                                                      \
    X(gives) X(or) X(fail) X(owned)                                                                \
    X(to) X(do) X(end) X(if) X(else) X(loop) X(while) X(until) X(break) X(case) X(of)              \
    X(otherwise) X(return) X(or_fail) X(or_return) X(or_else) X(cat) X(defer)

typedef enum pnx_word_id {
    PNX_KW_NONE,
#define PNX_WORD_ENUM(name) PNX_KW_##name,
    PNX_RESERVED(PNX_WORD_ENUM)
#undef PNX_WORD_ENUM
    PNX_KW_COUNT
} pnx_word_id;

typedef enum pnx_token_kind {
    PNX_T_END,      // end of the source
    PNX_T_RESERVED, // a reserved word; `word` says which. rem, note and text never reach the caller
    PNX_T_NAME,
    PNX_T_PATH,     // name.name[.name ...]
    PNX_T_INT,      // `integer`
    PNX_T_FLOAT,    // `real`
    PNX_T_CHAR,     // `integer` is the byte or the code point
    PNX_T_STRING,   // body between the quotes; `escapes` says whether it must be decoded
    PNX_T_RAW,      // r"..."; body as written
    PNX_T_TEXT,     // text TAG ... TAG; body as written
    PNX_T_LPAREN,
    PNX_T_RPAREN,
    PNX_T_COMMA
} pnx_token_kind;

typedef struct pnx_token {
    pnx_token_kind kind;
    pnx_word_id    word;
    uint32_t       start, length; // the token in the source
    uint32_t       body, body_length; // strings: the bytes between the delimiters
    uint32_t       line, column;  // 1-based; column counts bytes
    bool           escapes;
    int64_t        integer;
    double         real;
} pnx_token;

typedef struct pnx_lexer {
    const uint8_t *source;
    uint32_t       length, at;
    uint32_t       line, line_start;
    // Set when pnx_lex_next() returns false.
    const char *error;
    uint32_t    error_line, error_column;
} pnx_lexer;

void pnx_lex_init(pnx_lexer *lexer, const uint8_t *source, size_t length);

// Reads the next token. Remarks are skipped. At the end of the source the token is PNX_T_END,
// again on every later call. Returns false on text that is not Pinax; `error` says why.
bool pnx_lex_next(pnx_lexer *lexer, pnx_token *token);

// Decodes the body of a PNX_T_STRING token into `out` (at most body_length bytes).
// Returns the number of bytes written. The lexer has already checked the escapes.
uint32_t pnx_lex_decode(const pnx_lexer *lexer, const pnx_token *token, uint8_t *out);

const char *pnx_word_name(pnx_word_id word);

#endif // PINAX_LEX_H
