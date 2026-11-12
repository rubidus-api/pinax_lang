// pnx_main.c - the command `pnx` (RFC-0015 E3): a host that registers the five standard host
// modules and runs source files.
//
//   pnx file.pnx [arguments]      loads the file; its code outside definitions runs
//   pnx test file.pnx             loads the file, then runs each of its `test name ... end`
//   pnx repl                      reads lines and runs each (pinax_eval); what a line leaves is shown
//   pnx see file.pnx              prints, for every line of every definition, the stack after it
//   pnx see --checks file.pnx     loads the file, then lists per word the run-time checks the loader
//                                 proved cannot fire (RFC-0023)
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
// --lib only to scripts you trust. --capacity NAME=N gives a capacity the script writes `host`
// (`tasks handlers host`): NAME is a declaration of the file (it is given as NAME and as FILE.NAME),
// or MODULE.NAME for a module it uses; N is 1 to 2^31 - 1 (RFC-0029 P-L2, D0066).
//
// Environment: PNX_BLOCK_KB sets the size of the state's memory block (default 16384).
#if defined(__linux__)
#define _GNU_SOURCE // dladdr
#endif

#include "pinax.h"
#include "pinax_host.h"
#include "pinax_internal.h" // pnx_checks_report
#include "pnx_fmt.h"
#include "pnx_streams.h"

#include "proven/alloc_check.h"
#include "proven/fs.h"
#include "proven/heap.h"
#include "proven/u8str.h"

#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(_WIN32)
#include <fcntl.h>
#include <io.h>
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
    case PINAX_ERR_DEADLOCK: return "deadlock: waiting for a queue that nothing can change";
    case PINAX_ERR_NO_ROOM: return "the memory block is too small (set PNX_BLOCK_KB)";
    case PINAX_ERR_NOT_FOUND: return "not found";
    case PINAX_ERR_FORMAT: return "bad format";
    default: return "error";
    }
}

// Every piece of memory pnx takes is proven's heap, through one allocator. Built with
// PROVEN_ALLOC_CHECK, the allocator is checked, and pnx says so when a run has not given back all
// it took.
#if defined(PROVEN_ALLOC_CHECK)
enum { MOST_LIVE = 4096 };
static proven_alloc_check_entry_t heap_entries[MOST_LIVE];
#define HEAP_ENTRIES heap_entries, MOST_LIVE
#else
#define HEAP_ENTRIES NULL, 0
#endif
static proven_alloc_check_t heap_check;
static proven_allocator_t   heap;
enum { HEAP_ALIGN = _Alignof(max_align_t) }; // one alignment for every block, as realloc wants it again

static void *take(size_t bytes) {
    const proven_result_mem_mut_t room = heap.alloc_fn(heap.ctx, bytes ? bytes : 1, HEAP_ALIGN);
    return room.err == PROVEN_OK ? room.value.ptr : NULL;
}

// The block at `ptr` of `old` bytes made `bytes` long; NULL, and the block as it was, when there is no room.
static void *grow(void *ptr, size_t old, size_t bytes) {
    if (!ptr)
        return take(bytes);
    const proven_result_mem_mut_t room = heap.realloc_fn(heap.ctx, ptr, old, bytes, HEAP_ALIGN);
    return room.err == PROVEN_OK ? room.value.ptr : NULL;
}

static void give(void *ptr) {
    if (ptr)
        heap.free_fn(heap.ctx, ptr);
}

static proven_u8str_view_t view_of_c(const char *text) {
    return (proven_u8str_view_t){(const proven_byte_t *)text, strlen(text)};
}

// The whole file, in a block of the heap; NULL when it cannot be read.
static char *read_file(proven_u8str_view_t path, size_t *length) {
    const proven_result_file_t opened = proven_fs_open(heap, path, PROVEN_FS_READ);
    if (opened.err != PROVEN_OK)
        return NULL;
    const proven_result_size_t size = proven_fs_size(opened.value);
    size_t cap = size.err == PROVEN_OK && size.value ? size.value + 1 : 65536, n = 0;
    char  *text = take(cap);
    while (text) {
        if (n == cap) { // a file that grew, or one whose size the system does not know
            char *grown = grow(text, cap, cap * 2);
            if (!grown) {
                give(text);
                text = NULL;
                break;
            }
            text = grown;
            cap *= 2;
        }
        const proven_result_size_t got = proven_fs_read(opened.value, (proven_mem_mut_t){(proven_byte_t *)text + n, cap - n});
        if (got.err == PROVEN_ERR_EOF || (got.err == PROVEN_OK && got.value == 0)) { // the end is a code of its own
            break;
        } else if (got.err != PROVEN_OK) {
            give(text);
            text = NULL;
        } else {
            n += got.value;
        }
    }
    (void)proven_fs_close(opened.value);
    *length = n;
    return text;
}

// ---- --capacity: the capacities a script leaves to the host ---------------------------------

enum { MAX_CAPACITIES = 16 };
static struct {
    char     name[48];
    uint32_t value;
} capacities[MAX_CAPACITIES];
static int n_capacities;

static bool take_capacity(const char *option) {
    const char *eq = strchr(option, '=');
    char       *end = NULL;
    const long long n = eq ? strtoll(eq + 1, &end, 10) : 0;
    if (!eq || eq == option || eq - option >= (ptrdiff_t)sizeof capacities[0].name || !eq[1] || *end || n < 1 ||
        n > 0x7fffffff) {
        fprintf(stderr, "pnx: --capacity takes NAME=N, N being 1 to 2147483647\n");
        return false;
    }
    if (n_capacities == MAX_CAPACITIES) {
        fprintf(stderr, "pnx: at most %d capacities\n", MAX_CAPACITIES);
        return false;
    }
    memcpy(capacities[n_capacities].name, option, (size_t)(eq - option));
    capacities[n_capacities].name[eq - option] = 0;
    capacities[n_capacities++].value = (uint32_t)n;
    return true;
}

// Each as given; a plain name also as the file's module's, since the modules it uses load first.
static pinax_err give_capacities(pinax_state *state, const char *module) {
    for (int k = 0; k < n_capacities; ++k) {
        pinax_err err = pinax_capacity(state, capacities[k].name, capacities[k].value);
        if (err == PINAX_OK && module[0] && !strchr(capacities[k].name, '.')) {
            char full[96];
            snprintf(full, sizeof full, "%.31s.%.47s", module, capacities[k].name); // a module name is at most 31 bytes
            err = pinax_capacity(state, full, capacities[k].value);
        }
        if (err != PINAX_OK) {
            fprintf(stderr, "pnx: --capacity %s: %s\n", capacities[k].name, err_name(err));
            return err;
        }
    }
    return PINAX_OK;
}

// ---- --lib: the libraries a script may name ----------------------------------------------

enum { MAX_LIBS = 16 };
static struct {
    proven_u8str_view_t name; // as the script writes it after `from`: the part of the option before =
    void               *handle;
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
    libs[n_libs].name = proven_u8str_view_slice(view_of_c(option), 0, (size_t)(eq - option)); // argv outlives it
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
        if (!proven_u8str_view_eq(libs[k].name, (proven_u8str_view_t){library.ptr, library.len}))
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
    }
    n_libs = 0;
}

// The last separator of a path: / anywhere, and \ too on Windows.
static const char *last_sep(const char *path) {
    const char *slash = strrchr(path, '/');
#if defined(_WIN32)
    const char *back = strrchr(path, '\\');
    if (back && (!slash || back > slash))
        slash = back;
#endif
    return slash;
}

static bool is_name_byte(char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_';
}

// The sources loaded: the name each was loaded under and the file it came from, so that a
// diagnostic can name the file.
enum { MOST_SOURCES = 64 };
static struct {
    char  name[32];
    char *path;  // ends with a 0 byte: a command line argument, or `owned`
    char *owned; // the path pnx made for a module beside the file, given back at the end
    char *text;
} sources[MOST_SOURCES];
static int n_sources;

static const char *eval_path = "(eval)"; // the file whose text is given to pinax_eval, when there is one
static bool        refused;              // the last text was refused with a message
static bool        tool_to_stderr;       // pnx export-c: standard output is the C, so tool output goes beside it
static void put_text(void *user, const char *text, size_t len) { fwrite(text, 1, len, (FILE *)user); }

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
    pnx_streams_flush(); // what the script wrote comes first
    fputs(line, stdout);
    fflush(stdout); // and this before what it writes next
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
    if (line == 0) { // tool output: the list of words, the stack at a place, the order
        char out[600];
        snprintf(out, sizeof out, "%.*s\n", (int)message.len, (const char *)message.ptr);
        if (tool_to_stderr)
            fputs(out, stderr);
        else
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
        if (proven_u8str_view_eq(view_of_c(sources[k].name), (proven_u8str_view_t){file.ptr, file.len})) {
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
    pnx_streams_flush(); // what io kept back, before pnx writes anything of its own
    return err;
}

// Loads the file under `name`, after the modules it uses: for every line `use NAME`, the file
// NAME.pnx beside this one, when there is such a file and it is not loaded yet. (A module the
// host registered has no such file; a `use` line inside a text literal names one by mistake
// at worst.) Returns the outcome of the load; *said tells that the error was printed.
static pinax_err load_file(pinax_state *state, char *path, char *owned, const char *name, bool *said, bool see) {
    if (n_sources == MOST_SOURCES) {
        fprintf(stderr, "pnx: more than %d source files\n", MOST_SOURCES);
        give(owned);
        *said = true;
        return PINAX_ERR_NO_ROOM;
    }
    size_t length = 0;
    char  *text = read_file(view_of_c(path), &length);
    if (!text) {
        fprintf(stderr, "pnx: cannot read %s\n", path);
        give(owned);
        *said = true;
        return PINAX_ERR_NOT_FOUND;
    }
    const int self = n_sources++;
    snprintf(sources[self].name, sizeof sources[self].name, "%s", name);
    sources[self].path = path;
    sources[self].owned = owned;
    sources[self].text = text;

    // the modules it uses: each line `use NAME`, its name a slice of the text
    const proven_u8str_view_t source = {(const proven_byte_t *)text, length};
    const char *slash = last_sep(path);
    const proven_u8str_view_t dir = proven_u8str_view_slice(view_of_c(path), 0, slash ? (size_t)(slash - path) + 1 : 0);
    for (size_t i = 0, at = 0, in_list = 0; i + 4 < length; ++i) {
        // a line `use NAME`, or each NAME of a line `set_order (NAME, NAME)` (RFC-0040 part C, RFC-0041)
        if (in_list) {
            at = i;
            while (at < length && (text[at] == ' ' || text[at] == '\t' || text[at] == ','))
                ++at;
            if (at >= length || !is_name_byte(text[at])) {
                in_list = 0;
                continue;
            }
        } else if ((i == 0 || text[i - 1] == '\n') && length - i > 10 && memcmp(text + i, "set_order", 9) == 0 &&
                   (text[i + 9] == ' ' || text[i + 9] == '\t' || text[i + 9] == '(')) {
            at = i + 9;
            while (at < length && (text[at] == ' ' || text[at] == '\t'))
                ++at;
            if (at >= length || text[at] != '(')
                continue;
            i = at;
            in_list = 1;
            continue;
        } else {
            if ((i != 0 && text[i - 1] != '\n') || memcmp(text + i, "use", 3) != 0 || (text[i + 3] != ' ' && text[i + 3] != '\t'))
                continue;
            at = i + 3;
            while (at < length && (text[at] == ' ' || text[at] == '\t'))
                ++at;
        }
        size_t end = at;
        while (end < length && is_name_byte(text[end]))
            ++end;
        if (in_list)
            i = end - 1; // the next name of the list, from the end of this one
        if (end == at || end - at > 31)
            continue;
        if (in_list && end - at == 9 && memcmp(text + at, "get_order", 9) == 0)
            continue; // the order as it is, not a module
        const proven_u8str_view_t used = proven_u8str_view_slice(source, at, end - at);
        bool loaded = false;
        for (int k = 0; k < n_sources; ++k)
            loaded = loaded || proven_u8str_view_eq(view_of_c(sources[k].name), used);
        if (loaded)
            continue;
        char *beside = take(dir.size + used.size + 5); // DIR/NAME.pnx
        if (!beside)
            return PINAX_ERR_NO_ROOM;
        if (dir.size)
            memcpy(beside, dir.ptr, dir.size);
        memcpy(beside + dir.size, used.ptr, used.size);
        memcpy(beside + dir.size + used.size, ".pnx", 5);
        char used_name[32]; // pinax_load takes the name as C text
        memcpy(used_name, used.ptr, used.size);
        used_name[used.size] = '\0';
        const proven_result_file_t there = proven_fs_open(heap, view_of_c(beside), PROVEN_FS_READ);
        pinax_err err = PINAX_OK;
        if (there.err == PROVEN_OK) {
            (void)proven_fs_close(there.value);
            const int mark = n_sources;
            err = load_file(state, beside, beside, used_name, said, false); // the source keeps the path now
            if (err == PINAX_ERR_STATE && !*said) { // the name is a module of the host: that one is meant
                for (int k = mark; k < n_sources; ++k) {
                    if (strcmp(sources[k].name, used_name) == 0)
                        sources[k].name[0] = '\0';
                }
                err = PINAX_OK;
            }
            if (err != PINAX_OK && !*said && err != PINAX_ERR_SYNTAX && err != PINAX_ERR_TYPE) {
                fprintf(stderr, "%s: %s\n", beside, err_name(err));
                *said = true;
            }
        } else {
            give(beside);
        }
        if (err != PINAX_OK)
            return err;
    }
    if (see) { // the same text as a session's, with the tool word `see` before it on its first line
        char *listed = take(length + 4);
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
        give(listed);
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
// What a session declares fills the block. Said once, when three quarters of it are used.
static void warn_block(pinax_state *state, size_t block_bytes, bool *warned) {
    pinax_stats stats = {.struct_size = sizeof stats};
    pinax_stats_get(state, &stats);
    if (*warned || stats.block_bytes_used < block_bytes / 4 * 3)
        return;
    *warned = true;
    fprintf(stderr, "pnx: the block is %zu percent used; a session that declares more needs a larger PNX_BLOCK_KB\n",
            stats.block_bytes_used / (block_bytes / 100));
}

static int repl(pinax_state *state, size_t block_bytes) {
    static char text[1 << 16];
    size_t      n = 0;
    bool        warned = false;
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
        int asked = 0;
        if (err == PINAX_ERR_INTERRUPTED && pinax_host_exit_asked(&asked)) { // sh.exit ends the session
            fwrite(held, 1, held_n, stdout);
            fflush(stdout);
            fputc('\n', stderr);
            return asked;
        }
        warn_block(state, block_bytes, &warned);
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
// `unnamed` is the file loaded under no module name: the state says "line 12: ..." for it.
static bool report_fault(pinax_state *state, pinax_err err, const char *unnamed) {
    const pinax_str text = pinax_error_text(state);
    if (unnamed && text.len > 5 && memcmp(text.ptr, "line ", 5) == 0 && text.ptr[5] >= '0' && text.ptr[5] <= '9') {
        const unsigned char *rest = text.ptr + 5;
        const unsigned char *end = memchr(rest, ':', (size_t)(text.ptr + text.len - rest));
        if (!end)
            return false;
        fprintf(stderr, "%s:%.*s: %s\n", unnamed, (int)(end - rest), (const char *)rest, err_name(err));
        return true;
    }
    const unsigned char *colon = memchr(text.ptr, ':', text.len);
    if (!colon || colon + 1 >= text.ptr + text.len || colon[1] < '0' || colon[1] > '9')
        return false;
    const proven_u8str_view_t name = {text.ptr, (size_t)(colon - text.ptr)}; // "NAME:12: div_zero"
    const char               *path = NULL;
    for (int k = 0; k < n_sources && !path; ++k) {
        if (proven_u8str_view_eq(view_of_c(sources[k].name), name))
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
#if defined(SIGPIPE)
    signal(SIGPIPE, SIG_IGN); // a command that stops reading a pipe makes the write fail, not pnx end
#endif
#if defined(_WIN32)
    // What pnx prints is the same bytes on every system, as the module io writes them: no \r
    // before each \n.
    _setmode(_fileno(stdout), _O_BINARY);
    _setmode(_fileno(stderr), _O_BINARY);
#endif
    heap = proven_alloc_checked(&heap_check, proven_heap_allocator(), HEAP_ENTRIES);
    // options before the command; what follows is read as if they were not there
    int options = 0;
    while (options + 2 < argc && (strcmp(argv[1 + options], "--lib") == 0 || strcmp(argv[1 + options], "--capacity") == 0)) {
        if (strcmp(argv[1 + options], "--capacity") == 0 ? !take_capacity(argv[2 + options]) : !open_lib(argv[2 + options])) {
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
    int status = run(argc, argv);
    close_libs();
#if defined(PROVEN_ALLOC_CHECK)
    if (proven_alloc_check_live(&heap_check) != 0 || heap_check.faults != 0) {
        fprintf(stderr, "pnx: %zu blocks of memory not given back, %zu refused\n", (size_t)proven_alloc_check_live(&heap_check),
                (size_t)heap_check.faults);
        status = 2;
    }
#endif
    return status;
}

// What `use` loaded, given back.
static void give_sources(void) {
    for (int k = 0; k < n_sources; ++k) {
        give(sources[k].owned);
        give(sources[k].text);
    }
    n_sources = 0;
}

static int run(int argc, char **argv) {
    const bool testing = argc > 1 && strcmp(argv[1], "test") == 0;
    const bool checking = argc > 2 && strcmp(argv[1], "see") == 0 && strcmp(argv[2], "--checks") == 0;
    const bool effects = argc > 2 && strcmp(argv[1], "see") == 0 && strcmp(argv[2], "--effects") == 0;
    const bool seeing = argc > 1 && strcmp(argv[1], "see") == 0 && !checking && !effects;
    const bool formatting = argc > 1 && strcmp(argv[1], "fmt") == 0;
    const bool talking = argc == 2 && strcmp(argv[1], "repl") == 0;
    const bool exporting = argc > 1 && strcmp(argv[1], "export-c") == 0;
    const int  at = checking || effects ? 3 : testing || seeing || formatting || exporting ? 2 : 1;
    if (argc <= at) {
        fprintf(stderr, "usage: pnx [--lib NAME=PATH]... [--capacity NAME=N]... file.pnx [arguments]\n       pnx test file.pnx\n"
                        "       pnx repl\n       pnx see file.pnx\n       pnx see --checks file.pnx\n"
                        "       pnx see --effects file.pnx\n"
                        "       pnx fmt file.pnx\n       pnx export-c file.pnx [NAME]   (the code as C, RFC-0028)\n");
        return 2;
    }
    if (formatting) {
        size_t length = 0;
        char  *text = read_file(view_of_c(argv[at]), &length);
        if (!text) {
            fprintf(stderr, "pnx: cannot read %s\n", argv[at]);
            return 2;
        }
        const int status = pnx_format(heap, text, length, stdout, argv[at]);
        give(text);
        return status;
    }
    char *const file_name = argv[at];
    const char *kb = getenv("PNX_BLOCK_KB");
    const size_t block_bytes = (size_t)(kb && atol(kb) > 0 ? atol(kb) : 16384) * 1024;
    void *block = take(block_bytes);
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
    if (err == PINAX_OK) // a command line tool runs commands (RFC-0040 part B)
        err = pinax_host_register(state, "sh");
#if !defined(PNX_NO_SQLITE) // a command line tool opens databases (RFC-0043)
    if (err == PINAX_OK) {
        const char *mb = getenv("PNX_SQLITE_HEAP_MB"); // the hard limit; the soft one is a quarter of it
        if (mb && atol(mb) > 0)
            pinax_host_sqlite_limits((int64_t)atol(mb) * (1 << 20) / 4, (int64_t)atol(mb) * (1 << 20));
        err = pinax_host_register_sqlite(state);
    }
#endif
    if (err == PINAX_OK && n_libs)
        pinax_set_resolver(state, resolve, NULL);
    if (err != PINAX_OK) {
        fprintf(stderr, "pnx: cannot start (%s)\n", err_name(err));
        if (state)
            pinax_close(state);
        give(block);
        return 2;
    }
    pinax_host_set_args(argc - at, (const char *const *)(argv + at));
    pnx_streams_own(!talking); // the repl reads standard input itself: no read-ahead there
    if (exporting || effects)
        state->export_only = true; // compile and check, run nothing
    tool_to_stderr = exporting;
#if defined(PNX_WITH_NATIVE) // a pnx with a program's exported code linked in (tests/tools/export_check.sh)
    extern const pinax_native PNX_WITH_NATIVE;
    pinax_native_attach(state, &PNX_WITH_NATIVE);
#endif
    if (talking) {
        if (give_capacities(state, "") != PINAX_OK) {
            pinax_close(state);
            give(block);
            return 2;
        }
        const int status = repl(state, block_bytes);
        pinax_close(state);
        give(block);
        give_sources();
        return status;
    }

    // The file is loaded under its own name without ".pnx" when that is written like a word,
    // so that its tests and its diagnostics can be told from those of the modules it uses.
    char                module[32] = "";
    proven_u8str_view_t stem = view_of_c(last_sep(file_name) ? last_sep(file_name) + 1 : file_name); // the file's own name
    if (stem.size > 4 && proven_u8str_view_ends_with(stem, PROVEN_LIT(".pnx")))
        stem.size -= 4;
    bool plain = stem.size > 0 && stem.size < sizeof module && !(stem.ptr[0] >= '0' && stem.ptr[0] <= '9');
    for (size_t k = 0; plain && k < stem.size; ++k)
        plain = is_name_byte((char)stem.ptr[k]);
    if (plain)
        memcpy(module, stem.ptr, stem.size);
    if (give_capacities(state, plain ? module : "") != PINAX_OK) {
        pinax_close(state);
        give(block);
        return 2;
    }
    bool said = false;
    err = load_file(state, file_name, NULL, plain ? module : file_name, &said, seeing);
    if (err == PINAX_ERR_STATE && plain && !said && !seeing) { // the file is named like a module of the host
        plain = false;
        err = load_file(state, file_name, NULL, file_name, &said, false);
    }
    int status = 0;
    int asked = 0;
    if (err == PINAX_ERR_INTERRUPTED && pinax_host_exit_asked(&asked)) { // sh.exit: the program ends so
        status = asked;
    } else if (err != PINAX_OK) {
        if (!said && err >= PINAX_ERR_FAILED && err < PINAX_ERR_ARGUMENT)
            said = report_fault(state, err, plain ? NULL : file_name); // a fault of the script: with its line when the state knows it
        if (!said && err != PINAX_ERR_SYNTAX && err != PINAX_ERR_TYPE) // those two were reported with their place
            fprintf(stderr, "%s: %s\n", file_name, err_name(err));
        show_fault_locals(state, stderr); // and the locals of the word it happened in
        status = 1;
    } else if (testing) {
        status = run_tests(state, plain ? module : "");
    } else if (checking) {
        pnx_checks_report(state, put_text, stdout);
    } else if (effects) { // the words of the file itself: the last module loaded
        uint32_t module = 0;
        for (uint32_t k = 0; state->head && k < state->head->n_words; ++k)
            module = state->words[k].module > module ? state->words[k].module : module;
        pnx_effects_report(state, module, put_text, stdout);
    } else if (exporting) {
        const char *name = argc > at + 1 ? argv[at + 1] : "pnx_native";
        size_t      cap = 1u << 20, len = 0;
        char       *text = NULL;
        for (size_t had = 0;;) { // the text's length is known once written: grow until it fits
            char *bigger = grow(text, had, cap);
            if (!bigger)
                break;
            text = bigger;
            had = cap;
            if ((len = pnx_export_c(state, name, text, cap)) != 0)
                break;
            cap *= 4;
        }
        if (len)
            fwrite(text, 1, len, stdout);
        else
            status = 2;
        give(text);
    }
#if defined(PNX_WITH_NATIVE)
    if (getenv("PNX_NATIVE_SAY"))
        fprintf(stderr, "pnx: %llu runs through the exported code\n", (unsigned long long)state->native_runs);
#endif
    pnx_streams_flush();
    fflush(stdout);
    pinax_close(state);
    give(block);
    give_sources();
    return status;
}
