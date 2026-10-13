// pinax_code.h - instruction set and bytecode container of the P0 interpreter.
//
// Format version 0: provisional. Opcode numbers, field widths and the container layout
// carry no compatibility promise and change whenever measurement asks for it.
//
// An instruction is one 32-bit word in one of three shapes:
//
//     [ op 8 ][ A 8 ][ B 8 ][ C 8 ]     three fields
//     [ op 8 ][ A 8 ][    Bx 16    ]     one field and a 16-bit operand
//     [ op 8 ][       Ax 24        ]     far jump
//
// Register numbers count inside the running word's window. Which bank a register is in
// follows from the instruction. Operands that do not fit follow in a second word.
// Jump offsets are relative to the word after the instruction.
#ifndef PINAX_CODE_H
#define PINAX_CODE_H

#include <stdint.h>

// Operand kinds, used by the loader to check the shape of the code.
enum {
    PNX_K_NONE, // unused
    PNX_K_I,    // register in the I bank
    PNX_K_S,    // register in the S bank
    PNX_K_IMM,  // number, not checked
    PNX_K_G,    // global index
    PNX_K_J,    // 8-bit jump offset
};

// Shapes.
enum {
    PNX_F_ABC,    // three 8-bit fields
    PNX_F_IMM,    // A and a 16-bit number
    PNX_F_ICONST, // A and an integer constant index
    PNX_F_SCONST, // A and a string constant index
    PNX_F_GLOBAL, // A and a global index
    PNX_F_AX,     // 24-bit jump offset
};

// Second word.
enum {
    PNX_X_NONE,
    PNX_X_WORD,   // word index
    PNX_X_IMPORT, // host word index
    PNX_X_FIELD,  // global 12 bits | byte offset 12 bits | width code or string capacity 8 bits
};

// X(name, A, B, C, shape, second word, fails)
// "fails": the next word is not skipped when the operation fails (or, for SNEXT, when the
// sequence is used up); it must be JMP, RETF or FAIL.
//
// Added for compiled code (still format 0): JSNE; GETFAIL (the number of the failure just
// received) and FAILR (fail with the number in a register); RMARK starts a string at the top of
// the region and RCATS, RCATI, RCATB append a string, a decimal integer, true or false to it.
// GLEN and GCLEAR give and reset the length of an array or list; LPUSH and LPOP add and take
// at the end of a list and fail when it is full or empty; SFIND finds a string in a string.
// SWORD takes the next word between white space off a string (like SNEXT, the next word runs
// when none is left); ERRTEXT gives the name of an error number.
//
// Rows. A row of a table is its number plus one, and 0 is "no row"; that is what a register,
// a global and a field of a record hold alike. ROW makes a row from an index, FGET and the
// like take a row, and ADDROW appends a zeroed row to a list of records (fails when full).
//
// BCAT and BCATI append a string and a decimal integer to a buffer (fail when it is full).
// STAKE, SSKIP, STAKEL, SSKIPL give the first n bytes, all but the first n, the last n, all but
// the last n (as many as there are; a negative n is a fault). SPRE, SSUF, SHAS give 1 when the
// second string starts, ends or occurs in the first.
// IXGET and IXGETS find the row with an int or str key in an index (fail when there is none);
// IXPUT enters a row (fails when its key is there already); IXDEL takes a row out.
// SREPL A, B: the string in S[B] with every S[B+1] replaced by S[B+2], built in the region.
// QPUT adds at the back of a queue (fails when full); QTAKE and QPEEK take and read the front
// (fail when empty).
// PALLOC takes a free row of a pool (fails when none is left), PFREE gives one back, PALIVE
// says whether a row is still the one that was handed out. A row of a pool carries the
// generation of its slot in the upper 32 bits; using it after PFREE is the fault STALE.
#define PNX_OPS(X)                                                                                 \
    X(MOVI, PNX_K_I, PNX_K_I, PNX_K_NONE, PNX_F_ABC, PNX_X_NONE, 0)                                \
    X(MOVS, PNX_K_S, PNX_K_S, PNX_K_NONE, PNX_F_ABC, PNX_X_NONE, 0)                                \
    X(LOADI, PNX_K_I, PNX_K_NONE, PNX_K_NONE, PNX_F_IMM, PNX_X_NONE, 0)                            \
    X(LOADK, PNX_K_I, PNX_K_NONE, PNX_K_NONE, PNX_F_ICONST, PNX_X_NONE, 0)                         \
    X(LOADKS, PNX_K_S, PNX_K_NONE, PNX_K_NONE, PNX_F_SCONST, PNX_X_NONE, 0)                        \
    X(ADD, PNX_K_I, PNX_K_I, PNX_K_I, PNX_F_ABC, PNX_X_NONE, 0)                                    \
    X(SUB, PNX_K_I, PNX_K_I, PNX_K_I, PNX_F_ABC, PNX_X_NONE, 0)                                    \
    X(MUL, PNX_K_I, PNX_K_I, PNX_K_I, PNX_F_ABC, PNX_X_NONE, 0)                                    \
    X(DIV, PNX_K_I, PNX_K_I, PNX_K_I, PNX_F_ABC, PNX_X_NONE, 0)                                    \
    X(MOD, PNX_K_I, PNX_K_I, PNX_K_I, PNX_F_ABC, PNX_X_NONE, 0)                                    \
    X(NEG, PNX_K_I, PNX_K_I, PNX_K_NONE, PNX_F_ABC, PNX_X_NONE, 0)                                 \
    X(ADDI, PNX_K_I, PNX_K_I, PNX_K_IMM, PNX_F_ABC, PNX_X_NONE, 0)                                 \
    X(WADD, PNX_K_I, PNX_K_I, PNX_K_I, PNX_F_ABC, PNX_X_NONE, 0)                                   \
    X(WSUB, PNX_K_I, PNX_K_I, PNX_K_I, PNX_F_ABC, PNX_X_NONE, 0)                                   \
    X(WMUL, PNX_K_I, PNX_K_I, PNX_K_I, PNX_F_ABC, PNX_X_NONE, 0)                                   \
    X(AND, PNX_K_I, PNX_K_I, PNX_K_I, PNX_F_ABC, PNX_X_NONE, 0)                                    \
    X(OR, PNX_K_I, PNX_K_I, PNX_K_I, PNX_F_ABC, PNX_X_NONE, 0)                                     \
    X(XOR, PNX_K_I, PNX_K_I, PNX_K_I, PNX_F_ABC, PNX_X_NONE, 0)                                    \
    X(SHL, PNX_K_I, PNX_K_I, PNX_K_I, PNX_F_ABC, PNX_X_NONE, 0)                                    \
    X(SHR, PNX_K_I, PNX_K_I, PNX_K_I, PNX_F_ABC, PNX_X_NONE, 0)                                    \
    X(USHR, PNX_K_I, PNX_K_I, PNX_K_I, PNX_F_ABC, PNX_X_NONE, 0)                                   \
    X(JEQ, PNX_K_I, PNX_K_I, PNX_K_J, PNX_F_ABC, PNX_X_NONE, 0)                                    \
    X(JNE, PNX_K_I, PNX_K_I, PNX_K_J, PNX_F_ABC, PNX_X_NONE, 0)                                    \
    X(JLT, PNX_K_I, PNX_K_I, PNX_K_J, PNX_F_ABC, PNX_X_NONE, 0)                                    \
    X(JLE, PNX_K_I, PNX_K_I, PNX_K_J, PNX_F_ABC, PNX_X_NONE, 0)                                    \
    X(JEQI, PNX_K_I, PNX_K_IMM, PNX_K_J, PNX_F_ABC, PNX_X_NONE, 0)                                 \
    X(JNEI, PNX_K_I, PNX_K_IMM, PNX_K_J, PNX_F_ABC, PNX_X_NONE, 0)                                 \
    X(JLTI, PNX_K_I, PNX_K_IMM, PNX_K_J, PNX_F_ABC, PNX_X_NONE, 0)                                 \
    X(JGEI, PNX_K_I, PNX_K_IMM, PNX_K_J, PNX_F_ABC, PNX_X_NONE, 0)                                 \
    X(JMP, PNX_K_NONE, PNX_K_NONE, PNX_K_NONE, PNX_F_AX, PNX_X_NONE, 0)                            \
    X(GETG, PNX_K_I, PNX_K_NONE, PNX_K_NONE, PNX_F_GLOBAL, PNX_X_NONE, 0)                          \
    X(SETG, PNX_K_I, PNX_K_NONE, PNX_K_NONE, PNX_F_GLOBAL, PNX_X_NONE, 0)                          \
    X(AGET, PNX_K_I, PNX_K_I, PNX_K_G, PNX_F_ABC, PNX_X_NONE, 0)                                   \
    X(ASET, PNX_K_I, PNX_K_I, PNX_K_G, PNX_F_ABC, PNX_X_NONE, 0)                                   \
    X(ROW, PNX_K_I, PNX_K_I, PNX_K_G, PNX_F_ABC, PNX_X_NONE, 0)                                    \
    X(FGET, PNX_K_I, PNX_K_I, PNX_K_NONE, PNX_F_ABC, PNX_X_FIELD, 0)                               \
    X(FSET, PNX_K_I, PNX_K_I, PNX_K_NONE, PNX_F_ABC, PNX_X_FIELD, 0)                               \
    X(FGETS, PNX_K_S, PNX_K_I, PNX_K_NONE, PNX_F_ABC, PNX_X_FIELD, 0)                              \
    X(FSETS, PNX_K_S, PNX_K_I, PNX_K_NONE, PNX_F_ABC, PNX_X_FIELD, 0)                              \
    X(SLEN, PNX_K_I, PNX_K_S, PNX_K_NONE, PNX_F_ABC, PNX_X_NONE, 0)                                \
    X(SBYTE, PNX_K_I, PNX_K_S, PNX_K_I, PNX_F_ABC, PNX_X_NONE, 0)                                  \
    X(JSEQ, PNX_K_S, PNX_K_S, PNX_K_J, PNX_F_ABC, PNX_X_NONE, 0)                                   \
    X(SNEXT, PNX_K_S, PNX_K_S, PNX_K_IMM, PNX_F_ABC, PNX_X_NONE, 1)                                \
    X(SCUT, PNX_K_S, PNX_K_S, PNX_K_IMM, PNX_F_ABC, PNX_X_NONE, 0)                                 \
    X(TOINT, PNX_K_I, PNX_K_S, PNX_K_NONE, PNX_F_ABC, PNX_X_NONE, 1)                               \
    X(BCLEAR, PNX_K_NONE, PNX_K_NONE, PNX_K_NONE, PNX_F_GLOBAL, PNX_X_NONE, 0)                     \
    X(BPUT, PNX_K_I, PNX_K_IMM, PNX_K_G, PNX_F_ABC, PNX_X_NONE, 1)                                 \
    X(BPUTS, PNX_K_S, PNX_K_NONE, PNX_K_G, PNX_F_ABC, PNX_X_NONE, 1)                               \
    X(BVIEW, PNX_K_S, PNX_K_NONE, PNX_K_NONE, PNX_F_GLOBAL, PNX_X_NONE, 0)                         \
    X(SGET, PNX_K_I, PNX_K_S, PNX_K_IMM, PNX_F_ABC, PNX_X_NONE, 1)                                 \
    X(SGETS, PNX_K_S, PNX_K_S, PNX_K_NONE, PNX_F_ABC, PNX_X_NONE, 1)                               \
    X(CALL, PNX_K_IMM, PNX_K_IMM, PNX_K_NONE, PNX_F_ABC, PNX_X_WORD, 0)                            \
    X(TCALL, PNX_K_NONE, PNX_K_NONE, PNX_K_NONE, PNX_F_ABC, PNX_X_WORD, 0)                         \
    X(RET, PNX_K_NONE, PNX_K_NONE, PNX_K_NONE, PNX_F_ABC, PNX_X_NONE, 0)                           \
    X(RETOK, PNX_K_NONE, PNX_K_NONE, PNX_K_NONE, PNX_F_ABC, PNX_X_NONE, 0)                         \
    X(RETF, PNX_K_NONE, PNX_K_NONE, PNX_K_NONE, PNX_F_ABC, PNX_X_NONE, 0)                          \
    X(FAIL, PNX_K_NONE, PNX_K_NONE, PNX_K_NONE, PNX_F_IMM, PNX_X_NONE, 0)                          \
    X(HCALL, PNX_K_IMM, PNX_K_IMM, PNX_K_NONE, PNX_F_ABC, PNX_X_IMPORT, 0)                         \
    X(HCALLF, PNX_K_IMM, PNX_K_IMM, PNX_K_NONE, PNX_F_ABC, PNX_X_IMPORT, 1)                        \
    X(CHECK, PNX_K_NONE, PNX_K_NONE, PNX_K_NONE, PNX_F_IMM, PNX_X_NONE, 0)                         \
    X(FAULT, PNX_K_NONE, PNX_K_NONE, PNX_K_NONE, PNX_F_IMM, PNX_X_NONE, 0)                         \
    X(JSNE, PNX_K_S, PNX_K_S, PNX_K_J, PNX_F_ABC, PNX_X_NONE, 0)                                   \
    X(GETFAIL, PNX_K_I, PNX_K_NONE, PNX_K_NONE, PNX_F_IMM, PNX_X_NONE, 0)                          \
    X(FAILR, PNX_K_I, PNX_K_NONE, PNX_K_NONE, PNX_F_ABC, PNX_X_NONE, 0)                            \
    X(RMARK, PNX_K_S, PNX_K_NONE, PNX_K_NONE, PNX_F_ABC, PNX_X_NONE, 0)                            \
    X(RCATS, PNX_K_S, PNX_K_S, PNX_K_NONE, PNX_F_ABC, PNX_X_NONE, 0)                               \
    X(RCATI, PNX_K_S, PNX_K_I, PNX_K_NONE, PNX_F_ABC, PNX_X_NONE, 0)                               \
    X(RCATB, PNX_K_S, PNX_K_I, PNX_K_NONE, PNX_F_ABC, PNX_X_NONE, 0)                               \
    X(GLEN, PNX_K_I, PNX_K_NONE, PNX_K_NONE, PNX_F_GLOBAL, PNX_X_NONE, 0)                          \
    X(GCLEAR, PNX_K_NONE, PNX_K_NONE, PNX_K_NONE, PNX_F_GLOBAL, PNX_X_NONE, 0)                     \
    X(LPUSH, PNX_K_I, PNX_K_NONE, PNX_K_NONE, PNX_F_GLOBAL, PNX_X_NONE, 1)                         \
    X(LPOP, PNX_K_I, PNX_K_NONE, PNX_K_NONE, PNX_F_GLOBAL, PNX_X_NONE, 1)                          \
    X(SFIND, PNX_K_I, PNX_K_S, PNX_K_S, PNX_F_ABC, PNX_X_NONE, 1)                                  \
    X(SWORD, PNX_K_S, PNX_K_S, PNX_K_NONE, PNX_F_ABC, PNX_X_NONE, 1)                               \
    X(ERRTEXT, PNX_K_S, PNX_K_I, PNX_K_NONE, PNX_F_ABC, PNX_X_NONE, 0)                             \
    X(ADDROW, PNX_K_I, PNX_K_NONE, PNX_K_NONE, PNX_F_GLOBAL, PNX_X_NONE, 1)                        \
    X(BCAT, PNX_K_S, PNX_K_NONE, PNX_K_NONE, PNX_F_GLOBAL, PNX_X_NONE, 1)                          \
    X(BCATI, PNX_K_I, PNX_K_NONE, PNX_K_NONE, PNX_F_GLOBAL, PNX_X_NONE, 1)                         \
    X(STAKE, PNX_K_S, PNX_K_S, PNX_K_I, PNX_F_ABC, PNX_X_NONE, 0)                                  \
    X(SSKIP, PNX_K_S, PNX_K_S, PNX_K_I, PNX_F_ABC, PNX_X_NONE, 0)                                  \
    X(STAKEL, PNX_K_S, PNX_K_S, PNX_K_I, PNX_F_ABC, PNX_X_NONE, 0)                                 \
    X(SSKIPL, PNX_K_S, PNX_K_S, PNX_K_I, PNX_F_ABC, PNX_X_NONE, 0)                                 \
    X(SPRE, PNX_K_I, PNX_K_S, PNX_K_S, PNX_F_ABC, PNX_X_NONE, 0)                                   \
    X(SSUF, PNX_K_I, PNX_K_S, PNX_K_S, PNX_F_ABC, PNX_X_NONE, 0)                                   \
    X(SHAS, PNX_K_I, PNX_K_S, PNX_K_S, PNX_F_ABC, PNX_X_NONE, 0)                                   \
    X(IXGET, PNX_K_I, PNX_K_I, PNX_K_G, PNX_F_ABC, PNX_X_NONE, 1)                                  \
    X(IXGETS, PNX_K_I, PNX_K_S, PNX_K_G, PNX_F_ABC, PNX_X_NONE, 1)                                 \
    X(IXPUT, PNX_K_I, PNX_K_NONE, PNX_K_NONE, PNX_F_GLOBAL, PNX_X_NONE, 1)                         \
    X(IXDEL, PNX_K_I, PNX_K_NONE, PNX_K_NONE, PNX_F_GLOBAL, PNX_X_NONE, 0)                         \
    X(SREPL, PNX_K_S, PNX_K_S, PNX_K_NONE, PNX_F_ABC, PNX_X_NONE, 0)                               \
    X(QPUT, PNX_K_I, PNX_K_NONE, PNX_K_NONE, PNX_F_GLOBAL, PNX_X_NONE, 1)                          \
    X(QTAKE, PNX_K_I, PNX_K_NONE, PNX_K_NONE, PNX_F_GLOBAL, PNX_X_NONE, 1)                         \
    X(QPEEK, PNX_K_I, PNX_K_NONE, PNX_K_NONE, PNX_F_GLOBAL, PNX_X_NONE, 1)                         \
    X(PALLOC, PNX_K_I, PNX_K_NONE, PNX_K_NONE, PNX_F_GLOBAL, PNX_X_NONE, 1)                        \
    X(PFREE, PNX_K_I, PNX_K_NONE, PNX_K_NONE, PNX_F_GLOBAL, PNX_X_NONE, 0)                         \
    X(PALIVE, PNX_K_I, PNX_K_I, PNX_K_G, PNX_F_ABC, PNX_X_NONE, 0)

enum {
#define PNX_ENUM(name, a, b, c, shape, second, fails) PNX_##name,
    PNX_OPS(PNX_ENUM)
#undef PNX_ENUM
    PNX_OP_COUNT
};

#define PNX_ABC(op, a, b, c)                                                                       \
    ((uint32_t)(op) | ((uint32_t)(a) & 0xffu) << 8 | ((uint32_t)(b) & 0xffu) << 16 |             \
     ((uint32_t)(c) & 0xffu) << 24)
#define PNX_ABX(op, a, bx) ((uint32_t)(op) | ((uint32_t)(a) & 0xffu) << 8 | ((uint32_t)(bx) & 0xffffu) << 16)
#define PNX_AX(op, ax) ((uint32_t)(op) | ((uint32_t)(ax) & 0xffffffu) << 8)
#define PNX_FIELD(global, offset, extra)                                                           \
    (((uint32_t)(global) & 0xfffu) | ((uint32_t)(offset) & 0xfffu) << 12 | ((uint32_t)(extra) & 0xffu) << 24)

// Width codes of FGET, FSET, BPUT and SGET. Bytes are little-endian.
enum { PNX_W_U8, PNX_W_U16, PNX_W_U32, PNX_W_64, PNX_W_I8, PNX_W_I16, PNX_W_I32, PNX_W_COUNT };

// Kinds of global.
enum {
    PNX_G_INT,    // one 64-bit integer
    PNX_G_ARRAY,  // capacity 64-bit integers
    PNX_G_BUFFER, // byte buffer of capacity bytes
    PNX_G_TABLE,  // capacity records of elem_bytes bytes; a str(N) field is one length byte and N bytes
    PNX_G_LIST,   // up to capacity 64-bit integers; the length starts at 0
    PNX_G_ROWS,   // up to capacity records of elem_bytes bytes; the length starts at 0
    PNX_G_INDEX,  // lookup table of a table of records: capacity slots of 32 bits, each a row or 0;
                  // elem_bytes is the key field as PNX_FIELD(table, offset, extra), reserved is 1
                  // for a str(N) key (extra = N) and 0 for an integer key (extra = width code)
    PNX_G_QUEUE,  // a ring of capacity 64-bit integers, first in first out
    PNX_G_POOL,   // capacity records of elem_bytes bytes, then per slot a 32-bit generation (odd while
                  // the slot is handed out) and a 32-bit link of the free list
    PNX_G_COUNT
};

// Segments an S cell can name. Further numbers are bytes outside the block.
enum {
    PNX_SEG_EMPTY, PNX_SEG_CONST, PNX_SEG_GLOBAL, PNX_SEG_REGION,
    PNX_SEG_STATIC, // texts built into the interpreter (error names)
    PNX_SEG_HOST,
    PNX_SEG_COUNT = 16
};

// The container pinax_load_bytecode() accepts: head, word records, global records, import
// records, integer constants, string constant records, code, string bytes; in this order,
// native byte order.
#define PNX_MAGIC "PNXB"
#define PNX_CONTAINER_VERSION 0
#define PNX_ORDER_MARK 0x0102

typedef struct pnx_head {
    uint8_t  magic[4];
    uint16_t version;
    uint16_t order;
    uint32_t n_words, n_globals, n_imports, n_iconst, n_sconst, n_code, n_strbytes;
    uint32_t reserved;
} pnx_head;

typedef struct pnx_wordrec {
    char     name[32];
    char     signature[72];
    uint32_t entry; // index of the first instruction; words are stored in rising order
    uint16_t int_cells, str_cells;
    // How many arguments and results live in each bank. With `counted` 0 the loader reads them
    // off the signature, where every type but str is an integer; a compiler that knows
    // declared types (a type made of str) writes them here.
    uint8_t  counted, int_args, str_args, int_results, str_results, fails;
    uint8_t  reserved[2];
} pnx_wordrec;

typedef struct pnx_globalrec {
    char     name[32];
    char     type[32]; // the type as declared, for pinax_global_find(); may be empty
    uint32_t kind, elem_bytes, capacity, reserved;
} pnx_globalrec;

typedef struct pnx_importrec {
    char module[16];
    char name[32];
    char signature[72];
} pnx_importrec;

typedef struct pnx_sconst {
    uint32_t offset, length;
} pnx_sconst;

#endif // PINAX_CODE_H
