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
    PNX_X_COUNTS, // what a call through a register expects of the word: int args | str args << 8 |
                  // int results << 16 | str results << 24, and bit 31 when the word can fail
    PNX_X_SREG,   // a register of the S bank that the instruction reads
    PNX_X_GLOBAL, // global index: the wide form of an instruction whose C names a global
};

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
// SREPL A, B, C and a second word X: the string in S[B] with every S[C] replaced by S[X], built
// in the region.
// QPUT adds at the back of a queue (fails when full); QTAKE and QPEEK take and read the front
// (fail when empty).
// PALLOC takes a free row of a pool (fails when none is left), PFREE gives one back, PALIVE
// says whether a row is still the one that was handed out. A row of a pool carries the
// generation of its slot in the upper 32 bits; using it after PFREE is the fault STALE.
// SETFAIL sets the number a failure word after it would pass on (for words the compiler
// expands, like first, that fail without a failing instruction).
// CALLR calls the word whose number is in R[C], like CALL; the word must take and give what the
// second word says, else the fault SIGNATURE. RTOP gives the top of the region, RCUT sets it
// back to an earlier top (a later one is a fault), RFREE gives the bytes left.
// SCUTS A, B: S[A] is cut at the first S[B]; S[A] becomes the part before and S[B] the part
// after (empty when the separator is missing; SCUTSF fails then). STRIM takes white space off
// (C: 1 the front, 2 the back, 3 both). SLINE and SNEXTS take the next line or the next piece
// up to a separator string off S[A], as SWORD does for words. SFINDL finds the last
// occurrence. SCMP gives -1, 0 or 1 for the byte order of two strings.
// RCATX appends an integer in base 16 (C = 0) or 2 (C = 1) to a region string; RCATW and RCATXW
// append it in decimal or hexadecimal padded with zeros to the width in R[C]; RPADL and RPADR
// append a string padded with blanks on the left or right to that width.
// The rest of the integer words of RFC-0009 section 3.2: saturating (SAT*), unsigned (U*),
// rotating, the upper half of a product (MULHI, UMULHI), flooring division (FDIV, FMOD),
// bit counts, and the checked ones that fail instead of faulting (CHK*).
// Tasks (RFC-0005). A task is the number of its slot plus one, with the slot's generation in
// the upper 32 bits. TSPAWN A, B, G makes a task of set G that will run the word whose number
// is in R[B] (a word that takes and gives nothing; fails when no slot is free). TRESUME A, B, G
// runs task R[B] until it yields or ends and gives 1 in R[A] when it has ended. TYIELD gives
// the run back to whoever resumed the task; it fails with cancelled once the task was
// cancelled (TCANCEL); outside a task it does nothing. TDONE says whether a task has ended.
// TRUNALL G resumes the tasks of set G in turn until all have ended.
// X(name, number, A, B, C, shape, second word, fails, roles)
// The numbers are those of format 1 (RFC-0012): one range for each kind of instruction, with
// room left in every range. "roles" says what the instruction does with the registers named
// by A, B and C: r reads, w writes, m reads and leaves a value, - names no register.
#define PNX_OPS(X) \
    /* values */ \
    X(MOVI, 0x00, PNX_K_I, PNX_K_I, PNX_K_NONE, PNX_F_ABC, PNX_X_NONE, 0, "wr-") \
    X(MOVS, 0x01, PNX_K_S, PNX_K_S, PNX_K_NONE, PNX_F_ABC, PNX_X_NONE, 0, "wr-") \
    X(LOADI, 0x02, PNX_K_I, PNX_K_NONE, PNX_K_NONE, PNX_F_IMM, PNX_X_NONE, 0, "w--") \
    X(LOADK, 0x03, PNX_K_I, PNX_K_NONE, PNX_K_NONE, PNX_F_ICONST, PNX_X_NONE, 0, "w--") \
    X(LOADKS, 0x04, PNX_K_S, PNX_K_NONE, PNX_K_NONE, PNX_F_SCONST, PNX_X_NONE, 0, "w--") \
    /* integers */ \
    X(ADD, 0x08, PNX_K_I, PNX_K_I, PNX_K_I, PNX_F_ABC, PNX_X_NONE, 0, "wrr") \
    X(SUB, 0x09, PNX_K_I, PNX_K_I, PNX_K_I, PNX_F_ABC, PNX_X_NONE, 0, "wrr") \
    X(MUL, 0x0a, PNX_K_I, PNX_K_I, PNX_K_I, PNX_F_ABC, PNX_X_NONE, 0, "wrr") \
    X(DIV, 0x0b, PNX_K_I, PNX_K_I, PNX_K_I, PNX_F_ABC, PNX_X_NONE, 0, "wrr") \
    X(MOD, 0x0c, PNX_K_I, PNX_K_I, PNX_K_I, PNX_F_ABC, PNX_X_NONE, 0, "wrr") \
    X(NEG, 0x0d, PNX_K_I, PNX_K_I, PNX_K_NONE, PNX_F_ABC, PNX_X_NONE, 0, "wr-") \
    X(ADDI, 0x0e, PNX_K_I, PNX_K_I, PNX_K_IMM, PNX_F_ABC, PNX_X_NONE, 0, "wr-") \
    X(WADD, 0x0f, PNX_K_I, PNX_K_I, PNX_K_I, PNX_F_ABC, PNX_X_NONE, 0, "wrr") \
    X(WSUB, 0x10, PNX_K_I, PNX_K_I, PNX_K_I, PNX_F_ABC, PNX_X_NONE, 0, "wrr") \
    X(WMUL, 0x11, PNX_K_I, PNX_K_I, PNX_K_I, PNX_F_ABC, PNX_X_NONE, 0, "wrr") \
    X(AND, 0x12, PNX_K_I, PNX_K_I, PNX_K_I, PNX_F_ABC, PNX_X_NONE, 0, "wrr") \
    X(OR, 0x13, PNX_K_I, PNX_K_I, PNX_K_I, PNX_F_ABC, PNX_X_NONE, 0, "wrr") \
    X(XOR, 0x14, PNX_K_I, PNX_K_I, PNX_K_I, PNX_F_ABC, PNX_X_NONE, 0, "wrr") \
    X(SHL, 0x15, PNX_K_I, PNX_K_I, PNX_K_I, PNX_F_ABC, PNX_X_NONE, 0, "wrr") \
    X(SHR, 0x16, PNX_K_I, PNX_K_I, PNX_K_I, PNX_F_ABC, PNX_X_NONE, 0, "wrr") \
    X(USHR, 0x17, PNX_K_I, PNX_K_I, PNX_K_I, PNX_F_ABC, PNX_X_NONE, 0, "wrr") \
    X(SATADD, 0x18, PNX_K_I, PNX_K_I, PNX_K_I, PNX_F_ABC, PNX_X_NONE, 0, "wrr") \
    X(SATSUB, 0x19, PNX_K_I, PNX_K_I, PNX_K_I, PNX_F_ABC, PNX_X_NONE, 0, "wrr") \
    X(SATMUL, 0x1a, PNX_K_I, PNX_K_I, PNX_K_I, PNX_F_ABC, PNX_X_NONE, 0, "wrr") \
    X(UDIV, 0x1b, PNX_K_I, PNX_K_I, PNX_K_I, PNX_F_ABC, PNX_X_NONE, 0, "wrr") \
    X(UMOD, 0x1c, PNX_K_I, PNX_K_I, PNX_K_I, PNX_F_ABC, PNX_X_NONE, 0, "wrr") \
    X(ROTL, 0x1d, PNX_K_I, PNX_K_I, PNX_K_I, PNX_F_ABC, PNX_X_NONE, 0, "wrr") \
    X(ROTR, 0x1e, PNX_K_I, PNX_K_I, PNX_K_I, PNX_F_ABC, PNX_X_NONE, 0, "wrr") \
    X(MULHI, 0x1f, PNX_K_I, PNX_K_I, PNX_K_I, PNX_F_ABC, PNX_X_NONE, 0, "wrr") \
    X(UMULHI, 0x20, PNX_K_I, PNX_K_I, PNX_K_I, PNX_F_ABC, PNX_X_NONE, 0, "wrr") \
    X(FDIV, 0x21, PNX_K_I, PNX_K_I, PNX_K_I, PNX_F_ABC, PNX_X_NONE, 0, "wrr") \
    X(FMOD, 0x22, PNX_K_I, PNX_K_I, PNX_K_I, PNX_F_ABC, PNX_X_NONE, 0, "wrr") \
    X(ULT, 0x23, PNX_K_I, PNX_K_I, PNX_K_I, PNX_F_ABC, PNX_X_NONE, 0, "wrr") \
    X(ULE, 0x24, PNX_K_I, PNX_K_I, PNX_K_I, PNX_F_ABC, PNX_X_NONE, 0, "wrr") \
    X(ABS, 0x25, PNX_K_I, PNX_K_I, PNX_K_NONE, PNX_F_ABC, PNX_X_NONE, 0, "wr-") \
    X(BNOT, 0x26, PNX_K_I, PNX_K_I, PNX_K_NONE, PNX_F_ABC, PNX_X_NONE, 0, "wr-") \
    X(POPC, 0x27, PNX_K_I, PNX_K_I, PNX_K_NONE, PNX_F_ABC, PNX_X_NONE, 0, "wr-") \
    X(CLZ, 0x28, PNX_K_I, PNX_K_I, PNX_K_NONE, PNX_F_ABC, PNX_X_NONE, 0, "wr-") \
    X(CTZ, 0x29, PNX_K_I, PNX_K_I, PNX_K_NONE, PNX_F_ABC, PNX_X_NONE, 0, "wr-") \
    X(BSWAP, 0x2a, PNX_K_I, PNX_K_I, PNX_K_NONE, PNX_F_ABC, PNX_X_NONE, 0, "wr-") \
    X(CHKADD, 0x2b, PNX_K_I, PNX_K_I, PNX_K_I, PNX_F_ABC, PNX_X_NONE, 1, "wrr") \
    X(CHKSUB, 0x2c, PNX_K_I, PNX_K_I, PNX_K_I, PNX_F_ABC, PNX_X_NONE, 1, "wrr") \
    X(CHKMUL, 0x2d, PNX_K_I, PNX_K_I, PNX_K_I, PNX_F_ABC, PNX_X_NONE, 1, "wrr") \
    X(CHKDIV, 0x2e, PNX_K_I, PNX_K_I, PNX_K_I, PNX_F_ABC, PNX_X_NONE, 1, "wrr") \
    /* with a small constant: C is a signed byte (as ADDI) */ \
    X(MULI, 0x2f, PNX_K_I, PNX_K_I, PNX_K_IMM, PNX_F_ABC, PNX_X_NONE, 0, "wr-") \
    X(DIVI, 0x30, PNX_K_I, PNX_K_I, PNX_K_IMM, PNX_F_ABC, PNX_X_NONE, 0, "wr-") \
    X(MODI, 0x31, PNX_K_I, PNX_K_I, PNX_K_IMM, PNX_F_ABC, PNX_X_NONE, 0, "wr-") \
    X(ANDI, 0x32, PNX_K_I, PNX_K_I, PNX_K_IMM, PNX_F_ABC, PNX_X_NONE, 0, "wr-") \
    X(ORI, 0x33, PNX_K_I, PNX_K_I, PNX_K_IMM, PNX_F_ABC, PNX_X_NONE, 0, "wr-") \
    X(XORI, 0x34, PNX_K_I, PNX_K_I, PNX_K_IMM, PNX_F_ABC, PNX_X_NONE, 0, "wr-") \
    X(SHLI, 0x35, PNX_K_I, PNX_K_I, PNX_K_IMM, PNX_F_ABC, PNX_X_NONE, 0, "wr-") \
    X(SHRI, 0x36, PNX_K_I, PNX_K_I, PNX_K_IMM, PNX_F_ABC, PNX_X_NONE, 0, "wr-") \
    X(USHRI, 0x37, PNX_K_I, PNX_K_I, PNX_K_IMM, PNX_F_ABC, PNX_X_NONE, 0, "wr-") \
    X(WMULI, 0x38, PNX_K_I, PNX_K_I, PNX_K_IMM, PNX_F_ABC, PNX_X_NONE, 0, "wr-") \
    /* jumps */ \
    X(JEQ, 0x48, PNX_K_I, PNX_K_I, PNX_K_J, PNX_F_ABC, PNX_X_NONE, 0, "rr-") \
    X(JNE, 0x49, PNX_K_I, PNX_K_I, PNX_K_J, PNX_F_ABC, PNX_X_NONE, 0, "rr-") \
    X(JLT, 0x4a, PNX_K_I, PNX_K_I, PNX_K_J, PNX_F_ABC, PNX_X_NONE, 0, "rr-") \
    X(JLE, 0x4b, PNX_K_I, PNX_K_I, PNX_K_J, PNX_F_ABC, PNX_X_NONE, 0, "rr-") \
    X(JEQI, 0x4c, PNX_K_I, PNX_K_IMM, PNX_K_J, PNX_F_ABC, PNX_X_NONE, 0, "r--") \
    X(JNEI, 0x4d, PNX_K_I, PNX_K_IMM, PNX_K_J, PNX_F_ABC, PNX_X_NONE, 0, "r--") \
    X(JLTI, 0x4e, PNX_K_I, PNX_K_IMM, PNX_K_J, PNX_F_ABC, PNX_X_NONE, 0, "r--") \
    X(JGEI, 0x4f, PNX_K_I, PNX_K_IMM, PNX_K_J, PNX_F_ABC, PNX_X_NONE, 0, "r--") \
    X(JMP, 0x50, PNX_K_NONE, PNX_K_NONE, PNX_K_NONE, PNX_F_AX, PNX_X_NONE, 0, "---") \
    X(JSEQ, 0x51, PNX_K_S, PNX_K_S, PNX_K_J, PNX_F_ABC, PNX_X_NONE, 0, "rr-") \
    X(JSNE, 0x52, PNX_K_S, PNX_K_S, PNX_K_J, PNX_F_ABC, PNX_X_NONE, 0, "rr-") \
    /* calls, returns, failures */ \
    X(CALL, 0x58, PNX_K_IMM, PNX_K_IMM, PNX_K_NONE, PNX_F_ABC, PNX_X_WORD, 0, "---") \
    X(TCALL, 0x59, PNX_K_NONE, PNX_K_NONE, PNX_K_NONE, PNX_F_ABC, PNX_X_WORD, 0, "---") \
    X(CALLR, 0x5a, PNX_K_IMM, PNX_K_IMM, PNX_K_I, PNX_F_ABC, PNX_X_COUNTS, 0, "--r") \
    X(RET, 0x5b, PNX_K_NONE, PNX_K_NONE, PNX_K_NONE, PNX_F_ABC, PNX_X_NONE, 0, "---") \
    X(RETOK, 0x5c, PNX_K_NONE, PNX_K_NONE, PNX_K_NONE, PNX_F_ABC, PNX_X_NONE, 0, "---") \
    X(RETF, 0x5d, PNX_K_NONE, PNX_K_NONE, PNX_K_NONE, PNX_F_ABC, PNX_X_NONE, 0, "---") \
    X(FAIL, 0x5e, PNX_K_NONE, PNX_K_NONE, PNX_K_NONE, PNX_F_IMM, PNX_X_NONE, 0, "---") \
    X(FAILR, 0x5f, PNX_K_I, PNX_K_NONE, PNX_K_NONE, PNX_F_ABC, PNX_X_NONE, 0, "r--") \
    X(GETFAIL, 0x60, PNX_K_I, PNX_K_NONE, PNX_K_NONE, PNX_F_IMM, PNX_X_NONE, 0, "w--") \
    X(SETFAIL, 0x61, PNX_K_NONE, PNX_K_NONE, PNX_K_NONE, PNX_F_IMM, PNX_X_NONE, 0, "---") \
    X(HCALL, 0x62, PNX_K_IMM, PNX_K_IMM, PNX_K_NONE, PNX_F_ABC, PNX_X_IMPORT, 0, "---") \
    X(HCALLF, 0x63, PNX_K_IMM, PNX_K_IMM, PNX_K_NONE, PNX_F_ABC, PNX_X_IMPORT, 1, "---") \
    X(CHECK, 0x64, PNX_K_NONE, PNX_K_NONE, PNX_K_NONE, PNX_F_IMM, PNX_X_NONE, 0, "---") \
    X(FAULT, 0x65, PNX_K_NONE, PNX_K_NONE, PNX_K_NONE, PNX_F_IMM, PNX_X_NONE, 0, "---") \
    /* globals, collections, records */ \
    X(GETG, 0x70, PNX_K_I, PNX_K_NONE, PNX_K_NONE, PNX_F_GLOBAL, PNX_X_NONE, 0, "w--") \
    X(SETG, 0x71, PNX_K_I, PNX_K_NONE, PNX_K_NONE, PNX_F_GLOBAL, PNX_X_NONE, 0, "r--") \
    X(AGET, 0x72, PNX_K_I, PNX_K_I, PNX_K_G, PNX_F_ABC, PNX_X_NONE, 0, "wr-") \
    X(ASET, 0x73, PNX_K_I, PNX_K_I, PNX_K_G, PNX_F_ABC, PNX_X_NONE, 0, "rr-") \
    X(GLEN, 0x74, PNX_K_I, PNX_K_NONE, PNX_K_NONE, PNX_F_GLOBAL, PNX_X_NONE, 0, "w--") \
    X(GCLEAR, 0x75, PNX_K_NONE, PNX_K_NONE, PNX_K_NONE, PNX_F_GLOBAL, PNX_X_NONE, 0, "---") \
    X(LPUSH, 0x76, PNX_K_I, PNX_K_NONE, PNX_K_NONE, PNX_F_GLOBAL, PNX_X_NONE, 1, "r--") \
    X(LPOP, 0x77, PNX_K_I, PNX_K_NONE, PNX_K_NONE, PNX_F_GLOBAL, PNX_X_NONE, 1, "w--") \
    X(ROW, 0x78, PNX_K_I, PNX_K_I, PNX_K_G, PNX_F_ABC, PNX_X_NONE, 0, "wr-") \
    X(FGET, 0x79, PNX_K_I, PNX_K_I, PNX_K_NONE, PNX_F_ABC, PNX_X_FIELD, 0, "wr-") \
    X(FSET, 0x7a, PNX_K_I, PNX_K_I, PNX_K_NONE, PNX_F_ABC, PNX_X_FIELD, 0, "rr-") \
    X(FGETS, 0x7b, PNX_K_S, PNX_K_I, PNX_K_NONE, PNX_F_ABC, PNX_X_FIELD, 0, "wr-") \
    X(FSETS, 0x7c, PNX_K_S, PNX_K_I, PNX_K_NONE, PNX_F_ABC, PNX_X_FIELD, 0, "rr-") \
    X(ADDROW, 0x7d, PNX_K_I, PNX_K_NONE, PNX_K_NONE, PNX_F_GLOBAL, PNX_X_NONE, 1, "w--") \
    X(QPUT, 0x7e, PNX_K_I, PNX_K_NONE, PNX_K_NONE, PNX_F_GLOBAL, PNX_X_NONE, 1, "r--") \
    X(QTAKE, 0x7f, PNX_K_I, PNX_K_NONE, PNX_K_NONE, PNX_F_GLOBAL, PNX_X_NONE, 1, "w--") \
    X(QPEEK, 0x80, PNX_K_I, PNX_K_NONE, PNX_K_NONE, PNX_F_GLOBAL, PNX_X_NONE, 1, "w--") \
    X(PALLOC, 0x81, PNX_K_I, PNX_K_NONE, PNX_K_NONE, PNX_F_GLOBAL, PNX_X_NONE, 1, "w--") \
    X(PFREE, 0x82, PNX_K_I, PNX_K_NONE, PNX_K_NONE, PNX_F_GLOBAL, PNX_X_NONE, 0, "r--") \
    X(PALIVE, 0x83, PNX_K_I, PNX_K_I, PNX_K_G, PNX_F_ABC, PNX_X_NONE, 0, "wr-") \
    X(IXGET, 0x84, PNX_K_I, PNX_K_I, PNX_K_G, PNX_F_ABC, PNX_X_NONE, 1, "wr-") \
    X(IXGETS, 0x85, PNX_K_I, PNX_K_S, PNX_K_G, PNX_F_ABC, PNX_X_NONE, 1, "wr-") \
    X(IXPUT, 0x86, PNX_K_I, PNX_K_NONE, PNX_K_NONE, PNX_F_GLOBAL, PNX_X_NONE, 1, "r--") \
    X(IXDEL, 0x87, PNX_K_I, PNX_K_NONE, PNX_K_NONE, PNX_F_GLOBAL, PNX_X_NONE, 0, "r--") \
    /* wide forms: the global in a second word */ \
    X(AGETW, 0x88, PNX_K_I, PNX_K_I, PNX_K_NONE, PNX_F_ABC, PNX_X_GLOBAL, 0, "wr-") \
    X(ASETW, 0x89, PNX_K_I, PNX_K_I, PNX_K_NONE, PNX_F_ABC, PNX_X_GLOBAL, 0, "rr-") \
    X(ROWW, 0x8a, PNX_K_I, PNX_K_I, PNX_K_NONE, PNX_F_ABC, PNX_X_GLOBAL, 0, "wr-") \
    X(PALIVEW, 0x8b, PNX_K_I, PNX_K_I, PNX_K_NONE, PNX_F_ABC, PNX_X_GLOBAL, 0, "wr-") \
    X(IXGETW, 0x8c, PNX_K_I, PNX_K_I, PNX_K_NONE, PNX_F_ABC, PNX_X_GLOBAL, 1, "wr-") \
    X(IXGETSW, 0x8d, PNX_K_I, PNX_K_S, PNX_K_NONE, PNX_F_ABC, PNX_X_GLOBAL, 1, "wr-") \
    /* lists of str(N): a cell is a length byte and N bytes */ \
    X(LPUSHS, 0x8e, PNX_K_S, PNX_K_NONE, PNX_K_NONE, PNX_F_GLOBAL, PNX_X_NONE, 1, "r--") \
    X(LPOPS, 0x8f, PNX_K_S, PNX_K_NONE, PNX_K_NONE, PNX_F_GLOBAL, PNX_X_NONE, 1, "w--") \
    /* strings */ \
    X(SLEN, 0x90, PNX_K_I, PNX_K_S, PNX_K_NONE, PNX_F_ABC, PNX_X_NONE, 0, "wr-") \
    X(SBYTE, 0x91, PNX_K_I, PNX_K_S, PNX_K_I, PNX_F_ABC, PNX_X_NONE, 0, "wrr") \
    X(SNEXT, 0x92, PNX_K_S, PNX_K_S, PNX_K_IMM, PNX_F_ABC, PNX_X_NONE, 1, "mw-") \
    X(SCUT, 0x93, PNX_K_S, PNX_K_S, PNX_K_IMM, PNX_F_ABC, PNX_X_NONE, 0, "mw-") \
    X(TOINT, 0x94, PNX_K_I, PNX_K_S, PNX_K_NONE, PNX_F_ABC, PNX_X_NONE, 1, "wr-") \
    X(SFIND, 0x95, PNX_K_I, PNX_K_S, PNX_K_S, PNX_F_ABC, PNX_X_NONE, 1, "wrr") \
    X(SWORD, 0x96, PNX_K_S, PNX_K_S, PNX_K_NONE, PNX_F_ABC, PNX_X_NONE, 1, "mw-") \
    X(ERRTEXT, 0x97, PNX_K_S, PNX_K_I, PNX_K_NONE, PNX_F_ABC, PNX_X_NONE, 0, "wr-") \
    X(STAKE, 0x98, PNX_K_S, PNX_K_S, PNX_K_I, PNX_F_ABC, PNX_X_NONE, 0, "wrr") \
    X(SSKIP, 0x99, PNX_K_S, PNX_K_S, PNX_K_I, PNX_F_ABC, PNX_X_NONE, 0, "wrr") \
    X(STAKEL, 0x9a, PNX_K_S, PNX_K_S, PNX_K_I, PNX_F_ABC, PNX_X_NONE, 0, "wrr") \
    X(SSKIPL, 0x9b, PNX_K_S, PNX_K_S, PNX_K_I, PNX_F_ABC, PNX_X_NONE, 0, "wrr") \
    X(SPRE, 0x9c, PNX_K_I, PNX_K_S, PNX_K_S, PNX_F_ABC, PNX_X_NONE, 0, "wrr") \
    X(SSUF, 0x9d, PNX_K_I, PNX_K_S, PNX_K_S, PNX_F_ABC, PNX_X_NONE, 0, "wrr") \
    X(SHAS, 0x9e, PNX_K_I, PNX_K_S, PNX_K_S, PNX_F_ABC, PNX_X_NONE, 0, "wrr") \
    X(SREPL, 0x9f, PNX_K_S, PNX_K_S, PNX_K_S, PNX_F_ABC, PNX_X_SREG, 0, "wrr") \
    X(SCUTS, 0xa0, PNX_K_S, PNX_K_S, PNX_K_NONE, PNX_F_ABC, PNX_X_NONE, 0, "mm-") \
    X(SCUTSF, 0xa1, PNX_K_S, PNX_K_S, PNX_K_NONE, PNX_F_ABC, PNX_X_NONE, 1, "mm-") \
    X(STRIM, 0xa2, PNX_K_S, PNX_K_S, PNX_K_IMM, PNX_F_ABC, PNX_X_NONE, 0, "wr-") \
    X(SLINE, 0xa3, PNX_K_S, PNX_K_S, PNX_K_NONE, PNX_F_ABC, PNX_X_NONE, 1, "mw-") \
    X(SNEXTS, 0xa4, PNX_K_S, PNX_K_S, PNX_K_S, PNX_F_ABC, PNX_X_NONE, 1, "mwr") \
    X(SFINDL, 0xa5, PNX_K_I, PNX_K_S, PNX_K_S, PNX_F_ABC, PNX_X_NONE, 1, "wrr") \
    X(SCMP, 0xa6, PNX_K_I, PNX_K_S, PNX_K_S, PNX_F_ABC, PNX_X_NONE, 0, "wrr") \
    X(SGET, 0xa7, PNX_K_I, PNX_K_S, PNX_K_IMM, PNX_F_ABC, PNX_X_NONE, 1, "wm-") \
    X(SGETS, 0xa8, PNX_K_S, PNX_K_S, PNX_K_NONE, PNX_F_ABC, PNX_X_NONE, 1, "wm-") \
    /* strings built in the region */ \
    X(RMARK, 0xb8, PNX_K_S, PNX_K_NONE, PNX_K_NONE, PNX_F_ABC, PNX_X_NONE, 0, "w--") \
    X(RCATS, 0xb9, PNX_K_S, PNX_K_S, PNX_K_NONE, PNX_F_ABC, PNX_X_NONE, 0, "mr-") \
    X(RCATI, 0xba, PNX_K_S, PNX_K_I, PNX_K_NONE, PNX_F_ABC, PNX_X_NONE, 0, "mr-") \
    X(RCATB, 0xbb, PNX_K_S, PNX_K_I, PNX_K_NONE, PNX_F_ABC, PNX_X_NONE, 0, "mr-") \
    X(RCATX, 0xbc, PNX_K_S, PNX_K_I, PNX_K_IMM, PNX_F_ABC, PNX_X_NONE, 0, "mr-") \
    X(RCATW, 0xbd, PNX_K_S, PNX_K_I, PNX_K_I, PNX_F_ABC, PNX_X_NONE, 0, "mrr") \
    X(RCATXW, 0xbe, PNX_K_S, PNX_K_I, PNX_K_I, PNX_F_ABC, PNX_X_NONE, 0, "mrr") \
    X(RPADL, 0xbf, PNX_K_S, PNX_K_S, PNX_K_I, PNX_F_ABC, PNX_X_NONE, 0, "mrr") \
    X(RPADR, 0xc0, PNX_K_S, PNX_K_S, PNX_K_I, PNX_F_ABC, PNX_X_NONE, 0, "mrr") \
    X(RTOP, 0xc1, PNX_K_I, PNX_K_NONE, PNX_K_NONE, PNX_F_IMM, PNX_X_NONE, 0, "w--") \
    X(RCUT, 0xc2, PNX_K_I, PNX_K_NONE, PNX_K_NONE, PNX_F_IMM, PNX_X_NONE, 0, "r--") \
    X(RFREE, 0xc3, PNX_K_I, PNX_K_NONE, PNX_K_NONE, PNX_F_IMM, PNX_X_NONE, 0, "w--") \
    /* buffers */ \
    X(BCLEAR, 0xc8, PNX_K_NONE, PNX_K_NONE, PNX_K_NONE, PNX_F_GLOBAL, PNX_X_NONE, 0, "---") \
    X(BPUT, 0xc9, PNX_K_I, PNX_K_IMM, PNX_K_G, PNX_F_ABC, PNX_X_NONE, 1, "r--") \
    X(BPUTS, 0xca, PNX_K_S, PNX_K_NONE, PNX_K_G, PNX_F_ABC, PNX_X_NONE, 1, "r--") \
    X(BVIEW, 0xcb, PNX_K_S, PNX_K_NONE, PNX_K_NONE, PNX_F_GLOBAL, PNX_X_NONE, 0, "w--") \
    X(BCAT, 0xcc, PNX_K_S, PNX_K_NONE, PNX_K_NONE, PNX_F_GLOBAL, PNX_X_NONE, 1, "r--") \
    X(BCATI, 0xcd, PNX_K_I, PNX_K_NONE, PNX_K_NONE, PNX_F_GLOBAL, PNX_X_NONE, 1, "r--") \
    X(BPUTW, 0xce, PNX_K_I, PNX_K_IMM, PNX_K_NONE, PNX_F_ABC, PNX_X_GLOBAL, 1, "r--") \
    X(BPUTSW, 0xcf, PNX_K_S, PNX_K_NONE, PNX_K_NONE, PNX_F_ABC, PNX_X_GLOBAL, 1, "r--") \
    /* tasks */ \
    X(TSPAWN, 0xd0, PNX_K_I, PNX_K_I, PNX_K_G, PNX_F_ABC, PNX_X_NONE, 1, "wr-") \
    X(TRESUME, 0xd1, PNX_K_I, PNX_K_I, PNX_K_G, PNX_F_ABC, PNX_X_NONE, 0, "wr-") \
    X(TYIELD, 0xd2, PNX_K_NONE, PNX_K_NONE, PNX_K_NONE, PNX_F_IMM, PNX_X_NONE, 1, "---") \
    X(TDONE, 0xd3, PNX_K_I, PNX_K_I, PNX_K_G, PNX_F_ABC, PNX_X_NONE, 0, "wr-") \
    X(TCANCEL, 0xd4, PNX_K_I, PNX_K_NONE, PNX_K_NONE, PNX_F_GLOBAL, PNX_X_NONE, 0, "r--") \
    X(TRUNALL, 0xd5, PNX_K_NONE, PNX_K_NONE, PNX_K_NONE, PNX_F_GLOBAL, PNX_X_NONE, 0, "---") \
    X(TSPAWNW, 0xd6, PNX_K_I, PNX_K_I, PNX_K_NONE, PNX_F_ABC, PNX_X_GLOBAL, 1, "wr-") \
    X(TRESUMEW, 0xd7, PNX_K_I, PNX_K_I, PNX_K_NONE, PNX_F_ABC, PNX_X_GLOBAL, 0, "wr-") \
    X(TDONEW, 0xd8, PNX_K_I, PNX_K_I, PNX_K_NONE, PNX_F_ABC, PNX_X_GLOBAL, 0, "wr-")

enum {
#define PNX_ENUM(name, number, a, b, c, shape, second, fails, roles) PNX_##name = number,
    PNX_OPS(PNX_ENUM)
#undef PNX_ENUM
    PNX_OP_COUNT = 256 // the numbers in between are not instructions
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
    PNX_G_TASKS,  // capacity tasks; elem_bytes is the call depth of each, reserved the bytes of its region
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
// records, integer constants, string constant records, code, string bytes, line records; in
// this order, native byte order.
#define PNX_MAGIC "PNXB"
#define PNX_CONTAINER_VERSION 1
#define PNX_ORDER_MARK 0x0102

typedef struct pnx_head {
    uint8_t  magic[4];
    uint16_t version;
    uint16_t order;
    uint32_t n_words, n_globals, n_imports, n_iconst, n_sconst, n_code, n_strbytes;
    uint32_t n_lines; // line records after the string bytes; a loader that wants none may skip them
} pnx_head;

// Debug information: from code word `pc` on, the code came from source line `line` (of the
// source the word belongs to). Records are in rising order of pc.
typedef struct pnx_linerec {
    uint32_t pc, line;
} pnx_linerec;

typedef struct pnx_wordrec {
    char     name[32];
    char     signature[72];
    uint32_t entry; // index of the first instruction; words are stored in rising order
    uint16_t int_cells, str_cells;
    // How many arguments and results live in each bank. With `counted` 0 the loader reads them
    // off the signature, where every type but str is an integer; a compiler that knows
    // declared types (a type made of str) writes them here.
    uint8_t  counted, int_args, str_args, int_results, str_results, fails;
    uint8_t  module;   // the source module the word belongs to: 0 is the first source loaded
    uint8_t  reserved;
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
