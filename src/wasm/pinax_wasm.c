// pinax_wasm.c - Pinax as a WebAssembly module (./nob wasm). Freestanding: no C library.
//
// The core needs five functions of the C library; they are here. The module `io` prints
// through the import env.pnx_write; `src/wasm/pinax.mjs` gives it and drives the exports below.
// Memory: the state's block and the text a page passes are taken from the module's memory,
// which grows on demand; a module serves one state at a time.

#include "pinax.h"

#include <stddef.h>
#include <stdint.h>

#define WASM_IMPORT(name) __attribute__((import_module("env"), import_name(name)))
#define WASM_EXPORT(name) __attribute__((export_name(name)))

WASM_IMPORT("pnx_write") void pnx_write(int32_t stream, const void *bytes, size_t length);
WASM_IMPORT("pnx_diag") void pnx_diag(uint32_t line, uint32_t column, const void *message, size_t length);

// ---- the C library the core calls ------------------------------------------------------

void *memcpy(void *restrict to, const void *restrict from, size_t n) {
    unsigned char *t = to;
    const unsigned char *f = from;
    while (n--)
        *t++ = *f++;
    return to;
}

void *memmove(void *to, const void *from, size_t n) {
    unsigned char *t = to;
    const unsigned char *f = from;
    if (t < f) {
        while (n--)
            *t++ = *f++;
    } else {
        while (n--)
            t[n] = f[n];
    }
    return to;
}

void *memset(void *to, int byte, size_t n) {
    unsigned char *t = to;
    while (n--)
        *t++ = (unsigned char)byte;
    return to;
}

int memcmp(const void *a, const void *b, size_t n) {
    const unsigned char *x = a, *y = b;
    for (; n; --n, ++x, ++y) {
        if (*x != *y)
            return *x < *y ? -1 : 1;
    }
    return 0;
}

size_t strlen(const char *s) {
    const char *p = s;
    while (*p)
        ++p;
    return (size_t)(p - s);
}

void *memchr(const void *s, int byte, size_t n) {
    const unsigned char *p = s;
    for (; n; --n, ++p) {
        if (*p == (unsigned char)byte)
            return (void *)p;
    }
    return NULL;
}

int strncmp(const char *a, const char *b, size_t n) {
    for (; n; --n, ++a, ++b) {
        if (*a != *b || !*a)
            return (unsigned char)*a < (unsigned char)*b ? -1 : (unsigned char)*a > (unsigned char)*b;
    }
    return 0;
}

int strcmp(const char *a, const char *b) {
    return strncmp(a, b, SIZE_MAX);
}

// The compiler's helper for a 128-bit product (overflow checks of 64-bit multiplication call it
// on wasm32): the low 128 bits, the same for signed and unsigned operands.
__extension__ typedef unsigned __int128 u128;
static uint64_t mul_64x64_high(uint64_t a, uint64_t b) {
    const uint64_t al = (uint32_t)a, ah = a >> 32, bl = (uint32_t)b, bh = b >> 32;
    const uint64_t ll = al * bl, lh = al * bh, hl = ah * bl, hh = ah * bh;
    const uint64_t mid = (ll >> 32) + (uint32_t)lh + (uint32_t)hl;
    return hh + (lh >> 32) + (hl >> 32) + (mid >> 32);
}
u128 __multi3(u128 a, u128 b);
u128 __multi3(u128 a, u128 b) {
    const uint64_t al = (uint64_t)a, ah = (uint64_t)(a >> 64), bl = (uint64_t)b, bh = (uint64_t)(b >> 64);
    const uint64_t high = mul_64x64_high(al, bl) + al * bh + ah * bl;
    return (u128)high << 64 | (u128)(al * bl);
}

// ---- memory -------------------------------------------------------------------------------

extern unsigned char __heap_base;
static uintptr_t     heap_top;

// `bytes` from the end of what was handed out before, 16-byte aligned; 0 when memory cannot grow.
static void *take(size_t bytes) {
    if (!heap_top)
        heap_top = (uintptr_t)&__heap_base;
    const uintptr_t at = (heap_top + 15u) & ~(uintptr_t)15u, end = at + bytes;
    const uintptr_t have = __builtin_wasm_memory_size(0) * 65536u;
    if (end < at)
        return NULL;
    if (end > have && __builtin_wasm_memory_grow(0, (end - have + 65535u) / 65536u) == (size_t)-1)
        return NULL;
    heap_top = end;
    return (void *)at;
}

// ---- the module io ------------------------------------------------------------------------

static pinax_err io_print(pinax_call *call, void *user, const pinax_args *args) {
    (void)call;
    (void)user;
    pnx_write(1, args->s[0].ptr, args->s[0].len);
    return PINAX_OK;
}

static pinax_err io_println(pinax_call *call, void *user, const pinax_args *args) {
    (void)call;
    (void)user;
    pnx_write(1, args->s[0].ptr, args->s[0].len);
    pnx_write(1, "\n", 1);
    return PINAX_OK;
}

static pinax_err io_eprint(pinax_call *call, void *user, const pinax_args *args) {
    (void)call;
    (void)user;
    pnx_write(2, args->s[0].ptr, args->s[0].len);
    pnx_write(2, "\n", 1);
    return PINAX_OK;
}

static pinax_err io_no_input(pinax_call *call, void *user, const pinax_args *args) {
    (void)call;
    (void)user;
    (void)args;
    return PINAX_ERR_NOT_FOUND; // there is no standard input here: always at its end
}

static void diag(void *user, pinax_str file, uint32_t line, uint32_t column, pinax_str message) {
    (void)user;
    (void)file;
    pnx_diag(line, column, message.ptr, message.len);
}

// ---- what the page calls ------------------------------------------------------------------

static pinax_state *state;
static size_t       block_bytes;

// A place for `bytes` of text the page writes before a load (taken once for each size).
WASM_EXPORT("pnx_wasm_text") void *pnx_wasm_text(size_t bytes) {
    static unsigned char *text;
    static size_t         text_bytes;
    if (bytes > text_bytes) {
        text = take(bytes);
        text_bytes = text ? bytes : 0;
    }
    return text;
}

// Opens the state in a block of `bytes` (taken once; a later open reuses it when large enough)
// with the module io. Returns a pinax_err.
WASM_EXPORT("pnx_wasm_open") int32_t pnx_wasm_open(size_t bytes) {
    static void *block;
    if (state) {
        pinax_close(state);
        state = NULL;
    }
    if (bytes > block_bytes) {
        block = take(bytes);
        block_bytes = block ? bytes : 0;
    }
    if (!block)
        return PINAX_ERR_NO_ROOM;
    // the capacities pnx gives, so that a script reaches the same limits in both
    const pinax_config config = {.struct_size = sizeof config, .diag = diag, .region_bytes = 4u << 20,
                                 .int_slots = 65536, .str_slots = 16384};
    pinax_err err = pinax_open((pinax_mem){block, block_bytes}, &config, &state);
    static const pinax_word_def words[] = {
        {"print", "(s str)", io_print, NULL, 0},
        {"println", "(s str)", io_println, NULL, 0},
        {"eprint", "(s str)", io_eprint, NULL, 0},
        {"read_line", "(gives line str or fail)", io_no_input, NULL, 0},
        {"read_line_into", "(buf buffer or fail)", io_no_input, NULL, 0},
    };
    if (err == PINAX_OK)
        err = pinax_register(state, "io", words, sizeof words / sizeof words[0]);
    return err;
}

// Loads the source the page wrote at pnx_wasm_text(): `name` (NUL-terminated, in the same
// place after the source) is its module name, or none when `name` is 0. Returns a pinax_err;
// the code outside definitions has run when it is PINAX_OK.
WASM_EXPORT("pnx_wasm_load") int32_t pnx_wasm_load(const unsigned char *source, size_t length, const char *name) {
    if (!state)
        return PINAX_ERR_STATE;
    return pinax_load(state, name, (pinax_str){source, length}, PINAX_LOAD_CODE);
}

// The text of the last failure: its bytes at the returned place, its length from pnx_wasm_error_length().
WASM_EXPORT("pnx_wasm_error") const unsigned char *pnx_wasm_error(void) {
    return state ? pinax_error_text(state).ptr : NULL;
}
WASM_EXPORT("pnx_wasm_error_length") size_t pnx_wasm_error_length(void) {
    return state ? pinax_error_text(state).len : 0;
}

// After a load that ended with a fault: local `index` of the word it happened in, as 32-bit words
// at the returned place: kind, the integer (low, high), then place and length of the name, the
// type and the str bytes or the error's name. 0 past the last (pinax_fault_local).
WASM_EXPORT("pnx_wasm_fault_local") const uint32_t *pnx_wasm_fault_local(uint32_t index) {
    static uint32_t out[9];
    pinax_str   name;
    pinax_value v;
    if (!state || pinax_fault_local(state, index, &name, &v) != PINAX_OK)
        return NULL;
    const uint64_t i = (uint64_t)v.i;
    out[0] = v.kind;
    out[1] = (uint32_t)i;
    out[2] = (uint32_t)(i >> 32);
    out[3] = (uint32_t)(uintptr_t)name.ptr;
    out[4] = (uint32_t)name.len;
    out[5] = (uint32_t)(uintptr_t)v.type.ptr;
    out[6] = (uint32_t)v.type.len;
    out[7] = (uint32_t)(uintptr_t)v.s.ptr;
    out[8] = (uint32_t)v.s.len;
    return out;
}

WASM_EXPORT("pnx_wasm_close") void pnx_wasm_close(void) {
    if (state)
        pinax_close(state);
    state = NULL;
}
