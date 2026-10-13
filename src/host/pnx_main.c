// pnx_main.c - the command `pnx` (RFC-0015 E3): a host that registers the five standard host
// modules and runs source files.
//
//   pnx file.pnx [arguments]      loads the file; its code outside definitions runs
//   pnx test file.pnx             loads the file, then runs each of its `test name ... end`
//
// A module the file uses that is no host module is looked for beside it: `use shapes` loads
// shapes.pnx from the directory of the file first.
//
// Environment: PNX_BLOCK_KB sets the size of the state's memory block (default 16384).
#include "pinax.h"
#include "pinax_host.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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

// The sources loaded: the name each was loaded under and the file it came from, so that a
// diagnostic can name the file.
enum { MOST_SOURCES = 64 };
static struct {
    char  name[32];
    char *path, *text;
} sources[MOST_SOURCES];
static int n_sources;

static void diag(void *user, pinax_str file, uint32_t line, uint32_t column, pinax_str message) {
    (void)user;
    for (int k = 0; k < n_sources; ++k) {
        if (strlen(sources[k].name) == file.len && memcmp(sources[k].name, file.ptr, file.len) == 0) {
            file = (pinax_str){(const unsigned char *)sources[k].path, strlen(sources[k].path)};
            break;
        }
    }
    fprintf(stderr, "%.*s:%u:%u: %.*s\n", (int)file.len, (const char *)file.ptr, (unsigned)line, (unsigned)column,
            (int)message.len, (const char *)message.ptr);
}

static pinax_err finish(pinax_state *state, pinax_err err) {
    while (err == PINAX_SUSPENDED)
        err = pinax_resume(state, NULL);
    return err;
}

// Loads the file under `name`, after the modules it uses: for every line `use NAME`, the file
// NAME.pnx beside this one, when there is such a file and it is not loaded yet. (A module the
// host registered has no such file; a `use` line inside a text literal names one by mistake
// at worst.) Returns the outcome of the load; *said tells that the error was printed.
static pinax_err load_file(pinax_state *state, const char *path, const char *name, bool *said) {
    if (n_sources == MOST_SOURCES) {
        fprintf(stderr, "pnx: more than %d source files\n", MOST_SOURCES);
        *said = true;
        return PINAX_ERR_NO_ROOM;
    }
    size_t length = 0;
    char  *text = read_file(path, &length);
    if (!text) {
        fprintf(stderr, "pnx: cannot read %s\n", path);
        *said = true;
        return PINAX_ERR_NOT_FOUND;
    }
    const int self = n_sources++;
    snprintf(sources[self].name, sizeof sources[self].name, "%s", name);
    sources[self].path = malloc(strlen(path) + 1);
    sources[self].text = text;
    if (!sources[self].path)
        return PINAX_ERR_NO_ROOM;
    strcpy(sources[self].path, path);

    const char *slash = strrchr(path, '/');
    const size_t dir = slash ? (size_t)(slash - path) + 1 : 0;
    for (size_t i = 0; i + 4 < length; ++i) {
        if ((i != 0 && text[i - 1] != '\n') || memcmp(text + i, "use", 3) != 0 || (text[i + 3] != ' ' && text[i + 3] != '\t'))
            continue;
        size_t at = i + 3;
        while (at < length && (text[at] == ' ' || text[at] == '\t'))
            ++at;
        size_t end = at;
        while (end < length && is_name_byte(text[end]))
            ++end;
        if (end == at || end - at > 31)
            continue;
        char used[32];
        memcpy(used, text + at, end - at);
        used[end - at] = '\0';
        bool loaded = false;
        for (int k = 0; k < n_sources; ++k)
            loaded = loaded || strcmp(sources[k].name, used) == 0;
        if (loaded)
            continue;
        char *beside = malloc(dir + strlen(used) + 5);
        if (!beside)
            return PINAX_ERR_NO_ROOM;
        memcpy(beside, path, dir);
        sprintf(beside + dir, "%s.pnx", used);
        FILE *there = fopen(beside, "rb");
        pinax_err err = PINAX_OK;
        if (there) {
            fclose(there);
            const int mark = n_sources;
            err = load_file(state, beside, used, said);
            if (err == PINAX_ERR_STATE && !*said) { // the name is a module of the host: that one is meant
                for (int k = mark; k < n_sources; ++k) {
                    if (strcmp(sources[k].name, used) == 0)
                        sources[k].name[0] = '\0';
                }
                err = PINAX_OK;
            }
            if (err != PINAX_OK && !*said && err != PINAX_ERR_SYNTAX && err != PINAX_ERR_TYPE) {
                fprintf(stderr, "%s: %s\n", beside, err_name(err));
                *said = true;
            }
        }
        free(beside);
        if (err != PINAX_OK)
            return err;
    }
    return finish(state, pinax_load(state, name, (pinax_str){(const unsigned char *)text, length}, PINAX_LOAD_CODE));
}

// Runs every test of the file given on the command line: the words test.NAME of its module.
static int run_tests(pinax_state *state, const char *module) {
    int ran = 0, failed = 0;
    pinax_word_info info;
    for (uint32_t k = 0; pinax_word_at(state, k, &info) == PINAX_OK; ++k) {
        if (info.name.len < 6 || memcmp(info.name.ptr, "test.", 5) != 0 || info.module.len != strlen(module) ||
            memcmp(info.module.ptr, module, info.module.len) != 0)
            continue;
        pinax_err err = finish(state, pinax_run(state, info.word, NULL));
        ++ran;
        const int   n = (int)info.name.len - 5;
        const char *name = (const char *)info.name.ptr + 5;
        if (err != PINAX_OK) {
            ++failed;
            printf("FAIL %.*s (%s)\n", n, name, err_name(err));
        } else {
            printf("ok   %.*s\n", n, name);
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
    const char *file_name = argv[at];
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

    // The file is loaded under its own name without ".pnx" when that is written like a word,
    // so that its tests and its diagnostics can be told from those of the modules it uses.
    char        module[32] = "";
    const char *base = strrchr(file_name, '/') ? strrchr(file_name, '/') + 1 : file_name;
    size_t      stem = strlen(base);
    if (stem > 4 && strcmp(base + stem - 4, ".pnx") == 0)
        stem -= 4;
    bool plain = stem > 0 && stem < sizeof module && !(base[0] >= '0' && base[0] <= '9');
    for (size_t k = 0; plain && k < stem; ++k)
        plain = is_name_byte(base[k]);
    if (plain)
        memcpy(module, base, stem);
    bool said = false;
    err = load_file(state, file_name, plain ? module : file_name, &said);
    if (err == PINAX_ERR_STATE && plain && !said) { // the file is named like a module of the host
        plain = false;
        err = load_file(state, file_name, file_name, &said);
    }
    int status = 0;
    if (err != PINAX_OK) {
        if (!said && err != PINAX_ERR_SYNTAX && err != PINAX_ERR_TYPE) // those two were reported with their place
            fprintf(stderr, "%s: %s\n", file_name, err_name(err));
        status = 1;
    } else if (testing) {
        status = run_tests(state, plain ? module : "");
    }
    fflush(stdout);
    pinax_close(state);
    free(block);
    for (int k = 0; k < n_sources; ++k) {
        free(sources[k].path);
        free(sources[k].text);
    }
    return status;
}
