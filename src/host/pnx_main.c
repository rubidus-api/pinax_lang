// pnx_main.c - the command `pnx` (RFC-0015 E3): a host that registers the five standard host
// modules and runs source files.
//
//   pnx file.pnx [arguments]      loads the file; its code outside definitions runs
//   pnx test file.pnx             loads the file, then runs each of its `test name ... end`
//   pnx repl                      reads lines and runs each (pinax_eval); what a line leaves is shown
//   pnx see file.pnx              prints, for every line of every definition, the stack after it
//   pnx fmt file.pnx              prints the file with its lines laid out the one way
//
// A module the file uses that is no host module is looked for beside it: `use shapes` loads
// shapes.pnx from the directory of the file first.
//
// Options, before the command: --lib NAME=PATH opens the shared library at PATH, and a script's
// `extern f (...) from "NAME"` may then name a function of it (RFC-0015 section 4.3). Only the
// libraries given this way are opened, and only functions they define themselves are found, not
// those of the libraries they depend on (the C library's `system`); with no --lib a
// script that names a C function does not load. Calling C is outside the type checks: give
// --lib only to scripts you trust.
//
// Environment: PNX_BLOCK_KB sets the size of the state's memory block (default 16384).
#if defined(__linux__)
#define _GNU_SOURCE // dladdr
#endif

#include "pinax.h"
#include "pinax_host.h"
#include "pnx_fmt.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(_WIN32)
#include <windows.h>
#else
#include <dlfcn.h>
#endif

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

// ---- --lib: the libraries a script may name ----------------------------------------------

enum { MAX_LIBS = 16 };
static struct {
    const char *name; // as the script writes it after `from`
    void       *handle;
} libs[MAX_LIBS];
static int n_libs;

// NAME=PATH: opens PATH now, so that a wrong path is told before the script runs.
static bool open_lib(const char *option) {
    const char *eq = strchr(option, '=');
    if (!eq || eq == option || eq - option > 14 || !eq[1]) {
        fprintf(stderr, "pnx: --lib takes NAME=PATH, NAME being 1 to 14 bytes\n");
        return false;
    }
    if (n_libs == MAX_LIBS) {
        fprintf(stderr, "pnx: at most %d libraries\n", MAX_LIBS);
        return false;
    }
#if defined(_WIN32)
    void *handle = (void *)LoadLibraryA(eq + 1);
#else
    void *handle = dlopen(eq + 1, RTLD_NOW | RTLD_LOCAL);
#endif
    if (!handle) {
        fprintf(stderr, "pnx: cannot open the library %s\n", eq + 1);
        return false;
    }
    char *name = malloc((size_t)(eq - option) + 1);
    if (!name)
        return false;
    memcpy(name, option, (size_t)(eq - option));
    name[eq - option] = 0;
    libs[n_libs].name = name;
    libs[n_libs++].handle = handle;
    return true;
}

static pinax_extern_fn resolve(void *user, pinax_str library, pinax_str symbol) {
    (void)user;
    char wanted[32];
    if (symbol.len >= sizeof wanted)
        return NULL;
    memcpy(wanted, symbol.ptr, symbol.len);
    wanted[symbol.len] = 0;
    for (int k = 0; k < n_libs; ++k) {
        if (strlen(libs[k].name) != library.len || memcmp(libs[k].name, library.ptr, library.len) != 0)
            continue;
#if defined(_WIN32)
        return (pinax_extern_fn)GetProcAddress((HMODULE)libs[k].handle, wanted);
#else
        // dlsym also finds what the library's own dependencies define (the C library's
        // `system`, say): keep only a function of the library itself
        void   *found = dlsym(libs[k].handle, wanted);
        Dl_info info;
        if (!found || !dladdr(found, &info) || !info.dli_fname)
            return NULL;
        void *owner = dlopen(info.dli_fname, RTLD_LAZY | RTLD_NOLOAD);
        if (owner)
            dlclose(owner);
        if (owner != libs[k].handle)
            return NULL;
        pinax_extern_fn fn = NULL;
        memcpy(&fn, &found, sizeof fn); // an object pointer to a function pointer, as POSIX allows
        return fn;
#endif
    }
    return NULL;
}

static void close_libs(void) {
    for (int k = 0; k < n_libs; ++k) {
#if defined(_WIN32)
        FreeLibrary((HMODULE)libs[k].handle);
#else
        dlclose(libs[k].handle);
#endif
        free((void *)libs[k].name);
    }
    n_libs = 0;
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

static const char *eval_path = "(eval)"; // the file whose text is given to pinax_eval, when there is one
static bool        refused;              // the last text was refused with a message
static const char *see_text;             // pnx see: the text being listed, to print each line beside its stack
static size_t      see_length;
// pnx repl: a text that turns out to be unfinished is run again when it has grown, so what
// its listing says is held back until the text has run
static char   held[1 << 15];
static size_t held_n;
static bool   holding;

static void say(const char *line) {
    if (holding) {
        const size_t n = strlen(line);
        if (held_n + n < sizeof held) {
            memcpy(held + held_n, line, n);
            held_n += n;
        }
        return;
    }
    fputs(line, stdout);
    fflush(stdout); // the host modules write without a buffer; keep the order
}

// What a line of the session left: 3 "abc" true err.full
static void show_values(const pinax_state *state) {
    const uint32_t n = pinax_eval_count(state);
    char           out[320];
    size_t         at = 0;
    for (uint32_t k = 0; k < n && at + 80 < sizeof out; ++k) {
        pinax_value v;
        if (pinax_eval_value(state, k, &v) != PINAX_OK)
            break;
        const char *gap = k ? " " : "";
        const int   shown = v.s.len > 60 ? 60 : (int)v.s.len;
        int         written;
        if (v.kind == PINAX_VALUE_STR)
            written = snprintf(out + at, sizeof out - at, "%s\"%.*s%s\"", gap, shown, (const char *)v.s.ptr,
                               shown < (int)v.s.len ? "..." : "");
        else if (v.kind == PINAX_VALUE_BOOL)
            written = snprintf(out + at, sizeof out - at, "%s%s", gap, v.i ? "true" : "false");
        else if (v.kind == PINAX_VALUE_ERR)
            written = snprintf(out + at, sizeof out - at, "%serr.%.*s", gap, (int)v.s.len, (const char *)v.s.ptr);
        else
            written = snprintf(out + at, sizeof out - at, "%s%lld", gap, (long long)v.i);
        at += (size_t)written;
    }
    if (n && at + 1 < sizeof out) {
        out[at++] = '\n';
        out[at] = 0;
        say(out);
    }
}

static void diag(void *user, pinax_str file, uint32_t line, uint32_t column, pinax_str message) {
    (void)user;
    if (line == 0) { // tool output: the list of words, the stack at a place
        char out[320];
        snprintf(out, sizeof out, "%.*s\n", (int)message.len, (const char *)message.ptr);
        say(out);
        return;
    }
    if (column == 0) { // the listing of `see`: the line as written, then the stack after it
        char        out[400];
        const char *stack = (const char *)message.ptr;
        int         stack_n = (int)message.len;
        if (stack_n > 7 && memcmp(stack, "stack: ", 7) == 0) {
            stack += 7;
            stack_n -= 7;
        }
        const char *from = see_text, *end = see_text + see_length;
        for (uint32_t k = 1; from && k < line && from < end; ++k) { // the start of that line
            const char *next = memchr(from, '\n', (size_t)(end - from));
            from = next ? next + 1 : end;
        }
        int written = 0;
        if (from && from < end) {
            const char *stop = memchr(from, '\n', (size_t)(end - from));
            int         n = (int)((stop ? stop : end) - from);
            while (n && (from[n - 1] == ' ' || from[n - 1] == '\r' || from[n - 1] == '\t'))
                --n;
            written = snprintf(out, sizeof out, "%5u  %-48.*s", (unsigned)line, n > 200 ? 200 : n, from);
        } else {
            written = snprintf(out, sizeof out, "%5u ", (unsigned)line);
        }
        snprintf(out + written, sizeof out - (size_t)written, " | %.*s\n", stack_n, stack);
        say(out);
        return;
    }
    refused = true;
    if (file.len == 6 && memcmp(file.ptr, "(eval)", 6) == 0)
        file = (pinax_str){(const unsigned char *)eval_path, strlen(eval_path)};
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
static pinax_err load_file(pinax_state *state, const char *path, const char *name, bool *said, bool see) {
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
            err = load_file(state, beside, used, said, false);
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
    if (see) { // the same text as a session's, with the tool word `see` before it on its first line
        char *listed = malloc(length + 4);
        if (!listed)
            return PINAX_ERR_NO_ROOM;
        memcpy(listed, "see ", 4);
        memcpy(listed + 4, text, length);
        eval_path = sources[self].path;
        see_text = text;
        see_length = length;
        const pinax_err err = finish(state, pinax_eval(state, (pinax_str){(const unsigned char *)listed, length + 4}));
        if (err == PINAX_OK)
            show_values(state);
        free(listed);
        return err;
    }
    return finish(state, pinax_load(state, name, (pinax_str){(const unsigned char *)text, length}, PINAX_LOAD_CODE));
}

// Prints the locals of the word a fault happened in, one a line: "  twice int = 10".
static void show_fault_locals(const pinax_state *state, FILE *out) {
    pinax_str   name;
    pinax_value v;
    for (uint32_t k = 0; pinax_fault_local(state, k, &name, &v) == PINAX_OK; ++k) {
        fprintf(out, "  %.*s %.*s = ", (int)name.len, (const char *)name.ptr, (int)v.type.len, (const char *)v.type.ptr);
        if (v.kind == PINAX_VALUE_STR)
            fprintf(out, "\"%.*s\"\n", (int)v.s.len, (const char *)v.s.ptr);
        else if (v.kind == PINAX_VALUE_ERR)
            fprintf(out, "err.%.*s\n", (int)v.s.len, (const char *)v.s.ptr);
        else if (v.kind == PINAX_VALUE_BOOL)
            fprintf(out, "%s\n", v.i ? "true" : "false");
        else
            fprintf(out, "%lld\n", (long long)v.i);
    }
}

// Reads lines from standard input and runs each as a line of one session. A definition or a
// block may go over several lines: the text is run when it is complete.
static int repl(pinax_state *state) {
    static char text[1 << 16];
    size_t      n = 0;
    for (;;) {
        fputs(n ? ".. " : "> ", stderr);
        if (!fgets(text + n, (int)(sizeof text - n), stdin))
            break;
        n += strlen(text + n);
        refused = false;
        holding = true;
        held_n = 0;
        see_text = text;
        see_length = n;
        pinax_err err = finish(state, pinax_eval(state, (pinax_str){(const unsigned char *)text, n}));
        if (err == PINAX_OK)
            show_values(state);
        holding = false;
        const pinax_str why = pinax_error_text(state);
        if (err == PINAX_ERR_SYNTAX && why.len >= 11 && memcmp(why.ptr, "unfinished:", 11) == 0 &&
            n + 1 < sizeof text)
            continue; // more lines are to come
        fwrite(held, 1, held_n, stdout);
        if (err != PINAX_OK && !refused) { // a fault of the line, or an error without a place
            printf("error: %.*s\n", (int)why.len, (const char *)why.ptr);
            show_fault_locals(state, stdout);
        }
        fflush(stdout);
        n = 0;
    }
    if (n)
        printf("error: the input ended inside a definition or a block\n");
    fputc('\n', stderr);
    return 0;
}

// Prints where a script's fault happened. The state says "module:line: error"; the module
// becomes the file it was loaded from. False when the state has no line for it.
static bool report_fault(pinax_state *state, pinax_err err) {
    const pinax_str text = pinax_error_text(state);
    const unsigned char *colon = memchr(text.ptr, ':', text.len);
    if (!colon || colon + 1 >= text.ptr + text.len || colon[1] < '0' || colon[1] > '9')
        return false;
    const size_t name = (size_t)(colon - text.ptr);
    const char  *path = NULL;
    for (int k = 0; k < n_sources && !path; ++k) {
        if (strlen(sources[k].name) == name && memcmp(sources[k].name, text.ptr, name) == 0)
            path = sources[k].path;
    }
    const unsigned char *rest = colon + 1; // "12: div_zero"
    const unsigned char *end = memchr(rest, ':', (size_t)(text.ptr + text.len - rest));
    if (!path || !end)
        return false;
    fprintf(stderr, "%s:%.*s: %s\n", path, (int)(end - rest), (const char *)rest, err_name(err));
    return true;
}

// Runs every test of the file given on the command line: the words test.NAME of its module.
static int run(int argc, char **argv);

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
    // options before the command; what follows is read as if they were not there
    int options = 0;
    while (options + 2 < argc && strcmp(argv[1 + options], "--lib") == 0) {
        if (!open_lib(argv[2 + options])) {
            close_libs();
            return 2;
        }
        options += 2;
    }
    if (options) {
        argv[options] = argv[0];
        argv += options;
        argc -= options;
    }
    const int status = run(argc, argv);
    close_libs();
    return status;
}

static int run(int argc, char **argv) {
    const bool testing = argc > 1 && strcmp(argv[1], "test") == 0;
    const bool seeing = argc > 1 && strcmp(argv[1], "see") == 0;
    const bool formatting = argc > 1 && strcmp(argv[1], "fmt") == 0;
    const bool talking = argc == 2 && strcmp(argv[1], "repl") == 0;
    const int  at = testing || seeing || formatting ? 2 : 1;
    if (argc <= at) {
        fprintf(stderr, "usage: pnx [--lib NAME=PATH]... file.pnx [arguments]\n       pnx test file.pnx\n"
                        "       pnx repl\n       pnx see file.pnx\n       pnx fmt file.pnx\n");
        return 2;
    }
    if (formatting) {
        size_t length = 0;
        char  *text = read_file(argv[at], &length);
        if (!text) {
            fprintf(stderr, "pnx: cannot read %s\n", argv[at]);
            return 2;
        }
        const int status = pnx_format(text, length, stdout, argv[at]);
        free(text);
        return status;
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
    if (err == PINAX_OK && n_libs)
        pinax_set_resolver(state, resolve, NULL);
    if (err != PINAX_OK) {
        fprintf(stderr, "pnx: cannot start (%s)\n", err_name(err));
        return 2;
    }
    pinax_host_set_args(argc - at, (const char *const *)(argv + at));
    if (talking) {
        const int status = repl(state);
        pinax_close(state);
        free(block);
        return status;
    }

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
    err = load_file(state, file_name, plain ? module : file_name, &said, seeing);
    if (err == PINAX_ERR_STATE && plain && !said && !seeing) { // the file is named like a module of the host
        plain = false;
        err = load_file(state, file_name, file_name, &said, false);
    }
    int status = 0;
    if (err != PINAX_OK) {
        if (!said && err >= PINAX_ERR_FAILED && err < PINAX_ERR_ARGUMENT)
            said = report_fault(state, err); // a fault of the script: with its line when the state knows it
        if (!said && err != PINAX_ERR_SYNTAX && err != PINAX_ERR_TYPE) // those two were reported with their place
            fprintf(stderr, "%s: %s\n", file_name, err_name(err));
        show_fault_locals(state, stderr); // and the locals of the word it happened in
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
