// pnx_main.c - the command `pnx` (RFC-0015 E3): a host that registers the five standard host
// modules and runs source files.
//
//   pnx file.pnx [arguments]      loads the file; its code outside definitions runs
//   pnx test file.pnx             loads the file, then runs each of its `test name ... end`
//
// Environment: PNX_BLOCK_KB sets the size of the state's memory block (default 16384).
#include "pinax.h"
#include "pinax_host.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *file_name = "";

static void diag(void *user, pinax_str file, uint32_t line, uint32_t column, pinax_str message) {
    (void)user;
    fprintf(stderr, "%.*s:%u:%u: %.*s\n", (int)file.len, (const char *)file.ptr, (unsigned)line, (unsigned)column,
            (int)message.len, (const char *)message.ptr);
}

static const char *err_name(pinax_err err) {
    switch (err) {
    case PINAX_ERR_FAILED: return "failed";
    case PINAX_ERR_OVERFLOW: return "integer overflow";
    case PINAX_ERR_DIV_ZERO: return "division by zero";
    case PINAX_ERR_RANGE: return "index out of range";
    case PINAX_ERR_FULL: return "a declared capacity is used up";
    case PINAX_ERR_DEPTH: return "call depth limit";
    case PINAX_ERR_STALE: return "a row or handle was used after it was given back";
    case PINAX_ERR_NO_ROOM: return "the memory block is too small (set PNX_BLOCK_KB)";
    case PINAX_ERR_NOT_FOUND: return "not found";
    case PINAX_ERR_FORMAT: return "bad format";
    default: return "error";
    }
}

static char *read_file(const char *path, size_t *length) {
    FILE *f = fopen(path, "rb");
    if (!f)
        return NULL;
    char  *text = NULL;
    size_t n = 0, cap = 0;
    for (;;) {
        if (n == cap) {
            cap = cap ? cap * 2 : 65536;
            char *grown = realloc(text, cap);
            if (!grown) {
                free(text);
                fclose(f);
                return NULL;
            }
            text = grown;
        }
        const size_t got = fread(text + n, 1, cap - n, f);
        if (got == 0)
            break;
        n += got;
    }
    fclose(f);
    *length = n;
    return text;
}

static bool is_name_byte(char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_';
}

// Runs every `test NAME` of the source: the word test.NAME. pinax.h has no way to list words,
// so the names are read off the source text: the word `test` at the start of a line.
static int run_tests(pinax_state *state, const char *text, size_t length) {
    int ran = 0, failed = 0;
    for (size_t i = 0; i + 5 < length; ++i) {
        if ((i != 0 && text[i - 1] != '\n') || memcmp(text + i, "test", 4) != 0 || (text[i + 4] != ' ' && text[i + 4] != '\t'))
            continue;
        size_t at = i + 4;
        while (at < length && (text[at] == ' ' || text[at] == '\t'))
            ++at;
        size_t end = at;
        while (end < length && is_name_byte(text[end]))
            ++end;
        char name[40];
        if (end == at || end - at > 26)
            continue;
        memcpy(name, "test.", 5);
        memcpy(name + 5, text + at, end - at);
        name[5 + end - at] = '\0';
        pinax_word word;
        if (pinax_find(state, name, "()", 0, &word) != PINAX_OK)
            continue;
        pinax_err err = pinax_run(state, word, NULL);
        while (err == PINAX_SUSPENDED)
            err = pinax_resume(state, NULL);
        ++ran;
        if (err != PINAX_OK) {
            ++failed;
            printf("FAIL %s (%s)\n", name + 5, err_name(err));
        } else {
            printf("ok   %s\n", name + 5);
        }
    }
    printf("%d tests, %d failed\n", ran, failed);
    return failed ? 1 : 0;
}

int main(int argc, char **argv) {
    const bool testing = argc > 1 && strcmp(argv[1], "test") == 0;
    const int  at = testing ? 2 : 1;
    if (argc <= at) {
        fprintf(stderr, "usage: pnx file.pnx [arguments]\n       pnx test file.pnx\n");
        return 2;
    }
    file_name = argv[at];
    size_t length = 0;
    char  *text = read_file(file_name, &length);
    if (!text) {
        fprintf(stderr, "pnx: cannot read %s\n", file_name);
        return 2;
    }
    const char *kb = getenv("PNX_BLOCK_KB");
    const size_t block_bytes = (size_t)(kb && atol(kb) > 0 ? atol(kb) : 16384) * 1024;
    void *block = malloc(block_bytes);
    if (!block) {
        fprintf(stderr, "pnx: no memory for a block of %zu bytes\n", block_bytes);
        return 2;
    }
    // a script's own regions: generous, since a command line tool is not short of memory
    pinax_config config = {.struct_size = sizeof config, .diag = diag, .region_bytes = 4u << 20, .int_slots = 65536,
                           .str_slots = 16384};
    pinax_state *state = NULL;
    pinax_err err = pinax_open((pinax_mem){block, block_bytes}, &config, &state);
    if (err == PINAX_OK)
        err = pinax_host_register_all(state);
    if (err != PINAX_OK) {
        fprintf(stderr, "pnx: cannot start (%s)\n", err_name(err));
        return 2;
    }
    pinax_host_set_args(argc - at, (const char *const *)(argv + at));

    err = pinax_load(state, file_name, (pinax_str){(const unsigned char *)text, length}, PINAX_LOAD_CODE);
    while (err == PINAX_SUSPENDED)
        err = pinax_resume(state, NULL);
    int status = 0;
    if (err != PINAX_OK) {
        if (err != PINAX_ERR_SYNTAX && err != PINAX_ERR_TYPE) // those two were reported with their place
            fprintf(stderr, "%s: %s\n", file_name, err_name(err));
        status = 1;
    } else if (testing) {
        status = run_tests(state, text, length);
    }
    fflush(stdout);
    pinax_close(state);
    free(block);
    free(text);
    return status;
}
