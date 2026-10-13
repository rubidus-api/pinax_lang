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
    X(RCATB, PNX_K_S, PNX_K_I, PNX_K_NONE, PNX_F_ABC, PNX_X_NONE, 0)

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
    PNX_G_COUNT
};

// Segments an S cell can name. Further numbers are bytes outside the block.
enum { PNX_SEG_EMPTY, PNX_SEG_CONST, PNX_SEG_GLOBAL, PNX_SEG_REGION, PNX_SEG_HOST, PNX_SEG_COUNT = 16 };

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
    uint8_t  reserved[8];
} pnx_wordrec;

typedef struct pnx_globalrec {
    char     name[32];
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
