// nob.c - Pinax build driver. Self-contained: the C standard library and mkdir only.
//
//   cc -std=c23 -o nob nob.c
//   ./nob                     build build/<target>/libpinax.a (today: the vendored proven_c_lib
//                             parts Pinax uses; the interpreter is not written yet)
//   ./nob test                build, then run the tests (the tests are skipped when tests/ is absent)
//   ./nob check               the public header compiles as C99, C23 and C++17; public files
//                             are ASCII, inside the public list and free of local paths
//   ./nob freestanding        compile the portable part with -ffreestanding
//   ./nob wasm                build/wasm/pinax.wasm and pinax.mjs: the freestanding part and
//                             src/wasm/ for browsers and node ($PINAX_WASM_CC, default clang,
//                             which needs wasm-ld)
//   ./nob all                 check + freestanding + test
//   ./nob clean               remove build/
//   options: --target=win64   cross-compile with MinGW-w64 ($PINAX_WIN64_CC, default
//                             x86_64-w64-mingw32-gcc); --sanitize (ASan + UBSan); --debug;
//            --proofs (PINAX_CHECK_PROOFS: a run-time check the loader proved away stops the
//            program if it fires, RFC-0023; for the tests, the examples and the fuzzer);
//            --switch (PINAX_SWITCH_DISPATCH: the interpreter dispatches with a plain switch, as
//            for a compiler without computed goto)
//
// Two flag sets. Pinax's sources: -std=c23 -Wall -Wextra -pedantic -Werror. The vendored
// proven_c_lib: its own set (no -pedantic, which rejects its __int128), see
// vendor/proven/VENDORED.md.

#if !defined(_WIN32)
#define _POSIX_C_SOURCE 200809L
#endif

#include <stdbool.h>
#include <stdint.h>
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#if defined(_WIN32)
#include <direct.h>
#define make_dir(p) _mkdir(p)
#else
#define make_dir(p) mkdir((p), 0777)
#endif

#define COUNT(a) (sizeof(a) / sizeof((a)[0]))

// Pinax sources (no OS access; freestanding).
// The numbers of include/pinax.h, for file names (nob includes nothing of the project).
#include "include/pinax.h"
#define PINAX_ABI_OF_HEADER PINAX_ABI
#define PINAX_MINOR_OF_HEADER PINAX_VERSION_MINOR
#define PINAX_PATCH_OF_HEADER PINAX_VERSION_PATCH

static const char *const core_sources[] = {"src/pinax_compile.c", "src/pinax_lex.c", "src/pinax_state.c", "src/pinax_text.c", "src/pinax_vm.c", "src/pinax_ptn.c", "src/pinax_rex.c", "src/pinax_json.c", "src/pinax_toml.c", "src/pinax_md.c", "src/pinax_sheet.c", "src/pinax_sheetio.c", "src/pinax_export.c"};
static const size_t core_count = COUNT(core_sources);

// proven_c_lib files that build without an operating system.
static const char *const proven_portable[] = {
    "src/proven/algorithm.c", "src/proven/arena.c",         "src/proven/array.c",
    "src/proven/buffer.c",    "src/proven/encode.c",
    "src/proven/float_decimal.c", "src/proven/float_format.c", "src/proven/float_parse.c",
    "src/proven/fmt.c",       "src/proven/hash.c",          "src/proven/heap.c",
    "src/proven/map.c",       "src/proven/memory.c",        "src/proven/panic.c",
    "src/proven/pool.c",      "src/proven/random.c",        "src/proven/ring.c",
    "src/proven/scan.c",      "src/proven/time.c",          "src/proven/u8str.c",
    "src/proven/url.c",       "src/proven/utf.c",           "platform/proven_sys_math.c",
};

// Hosted builds of the library also carry proven's memory primitives and its UTF-16 strings:
// the portable files call them, and the core now calls those files. A freestanding build gets
// the first inline from the compiler (PROVEN_FREESTANDING) and leaves the second out
// (PROVEN_NO_U16STR). The library must link on its own.
// Since proven v0.120.0 hash.c folds its CRCs with the processor's carry-less multiply where it has one
// (platform/proven_sys_crc.c, hosted only; a freestanding build uses the tables).
static const char *const proven_lib_hosted[] = {"platform/proven_sys_mem.c", "src/proven/u16str.c",
                                                "platform/proven_sys_crc.c"};

// proven_c_lib files that need the operating system. Tools and tests link them; the
// library itself stays freestanding.
static const char *const proven_hosted[] = {
    "src/proven/alloc_check.c",  "src/proven/fs.c",            "src/proven/mmap.c",
    "src/proven/stream.c",       "src/proven/sysio.c",
    "platform/proven_sys_env.c", "platform/proven_sys_fs.c",   "platform/proven_sys_io.c",
    "platform/proven_sys_random.c", "platform/proven_sys_time.c",
};

// The threads of the loop (RFC-0020) go into the host library itself, so that a host that runs
// the loop links libpinax_host.a and libpinax.a and nothing else.
static const char *const proven_threads[] = {"platform/proven_sys_thread.c"};

// The standard host modules, the loop and its channels, and the command pnx (RFC-0015,
// RFC-0020): hosted, built on pinax.h.
static const char *const host_sources[] = {"src/host/pinax_host.c", "src/host/pinax_loop.c"};
// SQLite (RFC-0043): the module and the vendored amalgamation are members of libpinax_host.a that
// only a host which registers the module links. PNX_SQLITE=0 leaves both out (and pnx without it).
static const char *const sqlite_module = "src/host/pinax_sqlite.c";
static const char *const sqlite_amalgamation = "vendor/sqlite/sqlite3.c";
static bool sqlite_wanted(void) {
    const char *v = getenv("PNX_SQLITE");
    return !(v && strcmp(v, "0") == 0);
}
static const char *const pnx_sources[] = {"src/host/pnx_main.c", "src/host/pnx_fmt.c"};

static const char *const test_sources[] = {"tests/test_main.c"};
static const char *const loop_test_sources[] = {"tests/test_loop.c"};

// Public files and directories; `git ls-files` must stay inside.
static const char *const public_paths[] = {
    ".gitignore", "README.md", "README.ko.md", "LICENSE", "nob.c", "include/", "src/", "vendor/",
    "docs/spec/", "docs/manual/", "docs/tools/", // the standard and the manual (Korean and English)
    "docs/playground/", "docs/index.html", "docs/.nojekyll", // the Pages front page and the playground
    "benchmarks/", // programs that measure Pinax against Lua and LuaJIT (not part of the product)
};

typedef struct {
    const char *name;
    const char *cc;
    const char *ar;
    const char *exe;
    bool sanitize;
    bool debug;
    bool proofs; // PINAX_CHECK_PROOFS: a proven check that fires stops the program (RFC-0023)
    bool switch_dispatch; // PINAX_SWITCH_DISPATCH
} target_t;

static int run(const char *command) {
    fflush(stdout);
    int rc = system(command);
    if (rc != 0)
        fprintf(stderr, "nob: failed (%d): %s\n", rc, command);
    return rc == 0 ? 0 : 1;
}

static int mkdirs(const char *path) {
    char buf[1024];
    size_t n = strlen(path);
    if (n >= sizeof buf)
        return 1;
    memcpy(buf, path, n + 1);
    for (size_t i = 1; i <= n; ++i) {
        if (buf[i] == '/' || buf[i] == '\0') {
            char c = buf[i];
            buf[i] = '\0';
            make_dir(buf);
            buf[i] = c;
        }
    }
    return 0;
}

static bool exists(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f)
        return false;
    fclose(f);
    return true;
}

static void build_dir(char *out, size_t cap, const target_t *t) {
    snprintf(out, cap, "build/%s%s%s%s", t->name, t->sanitize ? "-asan" : (t->debug ? "-debug" : ""),
             t->proofs ? "-proofs" : "", t->switch_dispatch ? "-switch" : "");
}

static void object_path(char *out, size_t cap, const char *objdir, const char *source) {
    snprintf(out, cap, "%s/", objdir);
    size_t len = strlen(out);
    for (const char *p = source; *p && len + 1 < cap; ++p)
        out[len++] = (*p == '/') ? '_' : *p;
    out[len] = '\0';
    if (len >= 2 && out[len - 2] == '.' && out[len - 1] == 'c')
        out[len - 1] = 'o';
}

static const char *mode_flags(const target_t *t) {
    static char buf[160];
    snprintf(buf, sizeof buf, "%s %s%s%s", t->debug || t->sanitize ? "-O0 -g" : "-O2",
             t->sanitize ? "-fsanitize=address,undefined -fno-omit-frame-pointer -fno-sanitize-recover=all" : "",
             t->proofs ? " -DPINAX_CHECK_PROOFS" : "", t->switch_dispatch ? " -DPINAX_SWITCH_DISPATCH" : "");
    return buf;
}

static const char *own_flags(const target_t *t) {
    static char buf[512];
    snprintf(buf, sizeof buf, "-std=c23 -Wall -Wextra -pedantic -Werror %s -Iinclude -Isrc -Ivendor/proven/include",
             mode_flags(t));
    return buf;
}

// The command pnx: in the sanitize build its allocator is proven's checked one, and a run that
// does not give back every block it took ends with status 2 (the pnx cases see it).
static const char *pnx_flags(const target_t *t) {
    static char buf[600];
    snprintf(buf, sizeof buf, "%s%s", own_flags(t), t->sanitize ? " -DPROVEN_ALLOC_CHECK" : "");
    return buf;
}

static const char *proven_flags(const target_t *t) {
    static char buf[512];
    snprintf(buf, sizeof buf, "-std=c23 -Wall -Wextra -Werror %s -Ivendor/proven/include -Ivendor/proven/platform",
             mode_flags(t));
    return buf;
}

// Compile `sources` (relative to `root`, "" for the project) and append the objects to `objs`.
static int compile_set(const target_t *t, const char *flags, const char *root, const char *const *sources,
                       size_t n, const char *objdir, char *objs, size_t objs_cap) {
    for (size_t i = 0; i < n; ++i) {
        char src[512], obj[1024], cmd[4096];
        snprintf(src, sizeof src, "%s%s", root, sources[i]);
        object_path(obj, sizeof obj, objdir, src);
        snprintf(cmd, sizeof cmd, "%s %s -c %s -o %s", t->cc, flags, src, obj);
        if (run(cmd))
            return 1;
        size_t len = strlen(objs);
        if (len + strlen(obj) + 2 >= objs_cap)
            return 1;
        snprintf(objs + len, objs_cap - len, " %s", obj);
    }
    return 0;
}

// A file compiled with its own flags and kept: skipped while the object is newer than the source and
// the compiler and flags are the ones written beside it. SQLite's amalgamation is 270000 lines.
static int compile_cached(const target_t *t, const char *flags, const char *src, const char *obj) {
    char stamp[1100], want[2048], have[2048] = "", cmd[4096];
    snprintf(stamp, sizeof stamp, "%s.flags", obj);
    snprintf(want, sizeof want, "%s %s\n", t->cc, flags);
    FILE *f = fopen(stamp, "rb");
    if (f) {
        size_t n = fread(have, 1, sizeof have - 1, f);
        have[n] = '\0';
        fclose(f);
    }
    struct stat so, oo;
    if (stat(src, &so) == 0 && stat(obj, &oo) == 0 && oo.st_mtime >= so.st_mtime && strcmp(have, want) == 0) {
        printf("nob: kept %s\n", obj);
        return 0;
    }
    snprintf(cmd, sizeof cmd, "%s %s -c %s -o %s", t->cc, flags, src, obj);
    if (run(cmd))
        return 1;
    f = fopen(stamp, "wb");
    if (f) {
        fputs(want, f);
        fclose(f);
    }
    return 0;
}

// sqlite3.c: not Pinax's flags (it is third-party code, not edited, not warned about), -O2 even in a
// sanitizing build, and the options of RFC-0043 section 8. One object per target, for every variant.
static const char *sqlite_flags(void) {
    return "-O2 -w -std=gnu11 -DSQLITE_THREADSAFE=2 -DSQLITE_OMIT_LOAD_EXTENSION -DSQLITE_DQS=0 "
           "-DSQLITE_OMIT_DEPRECATED -DSQLITE_OMIT_SHARED_CACHE -DSQLITE_DEFAULT_MEMSTATUS=1 -DSQLITE_USE_ALLOCA "
           "-DSQLITE_DEFAULT_FOREIGN_KEYS=1 -DSQLITE_ENABLE_MATH_FUNCTIONS";
}

// ---- the deliverable archives (RFC-0058 C1, C2; D0116) --------------------------------------------------------
// The archives in build/<target>/ keep every name: pnx, the tests and the scripts link them. The ones a host is
// given are build/<target>/release/: each a partial link of what the public names need, in which only those
// names stay global; the core's own names (pnx_) that exported code calls are renamed pinax__ and stay global,
// outside the promise; proven's and SQLite's become local, so a host may link its own copies of either.

// The tool of the same toolchain as the target's archiver: "ar" -> "nm", "x86_64-w64-mingw32-ar" -> "...-nm".
static void tool_of(const target_t *t, const char *tool, char *out, size_t cap) {
    size_t n = strlen(t->ar);
    if (n >= 2 && strcmp(t->ar + n - 2, "ar") == 0)
        snprintf(out, cap, "%.*s%s", (int)(n - 2), t->ar, tool);
    else
        snprintf(out, cap, "%s", tool);
}

// The functions a header marks with `macro` (PINAX_API, PINAX_HOST_API), one name a line into `out`.
static int public_names(const char *header, const char *macro, FILE *out, const char *skip_prefix) {
    FILE *f = fopen(header, "r");
    if (!f)
        return 1;
    char line[1024];
    const size_t m = strlen(macro);
    while (fgets(line, sizeof line, f)) {
        const char *p = line;
        while (*p == ' ')
            ++p;
        if (strncmp(p, macro, m) != 0 || (p[m] != ' ' && p[m] != '\t'))
            continue;
        const char *open = strchr(p, '(');
        if (!open)
            continue;
        const char *end = open;
        while (end > p && (end[-1] == ' '))
            --end;
        const char *start = end;
        while (start > p && (isalnum((unsigned char)start[-1]) || start[-1] == '_'))
            --start;
        if (end > start && (!skip_prefix || strncmp(start, skip_prefix, strlen(skip_prefix)) != 0))
            fprintf(out, "%.*s\n", (int)(end - start), start);
    }
    fclose(f);
    return 0;
}

// in: archives; roots: a file of the names to pull and keep; out: the archive written. rename_core: the pnx_
// names left global become pinax__.
static int localize(const target_t *t, const char *in, const char *roots, bool rename_core, const char *out) {
    char nm[256], objcopy[256], cmd[4096], at[600], whole[600], keep[600], ren[600], local[600];
    tool_of(t, "nm", nm, sizeof nm);
    tool_of(t, "objcopy", objcopy, sizeof objcopy);
    // the files of the step beside release/, in release-work/: release/ holds only what is given
    char work[500];
    const char *slash = strrchr(out, '/');
    snprintf(work, sizeof work, "%.*s-work%s", (int)(slash - out), out, slash);
    snprintf(at, sizeof at, "%s.ld", work);
    snprintf(whole, sizeof whole, "%s.whole.o", work);
    snprintf(local, sizeof local, "%s.local.o", work);
    snprintf(keep, sizeof keep, "%s.keep", work);
    snprintf(ren, sizeof ren, "%s.rename", work);
    // the linker options file: -u for each root, so that only what they need is pulled from the archives
    snprintf(cmd, sizeof cmd, "sed 's/^/-u /' %s > %s", roots, at);
    if (run(cmd))
        return 1;
    snprintf(cmd, sizeof cmd, "%s -r -nostdlib -Wl,@%s %s -o %s", t->cc, at, in, whole);
    if (run(cmd))
        return 1;
    snprintf(cmd, sizeof cmd, "%s -g --defined-only %s | awk 'NF==3 && $3 ~ /^pnx_/ {print $3\" pinax__\"substr($3,5)}' > %s",
             nm, whole, ren);
    if (run(cmd))
        return 1;
    if (!rename_core && run(strcat(strcpy(cmd, ": > "), ren)))
        return 1;
    snprintf(cmd, sizeof cmd, "{ cat %s; awk '{print $2}' %s; } > %s", roots, ren, keep);
    if (run(cmd))
        return 1;
    snprintf(cmd, sizeof cmd, "%s --keep-global-symbols=%s --redefine-syms=%s %s %s && rm -f %s && %s rcs %s %s",
             objcopy, keep, ren, whole, local, out, t->ar, out, local);
    return run(cmd);
}

static int build_release_libs(const target_t *t, const char *dir) {
    char rel[300], wrk[320], roots[400], cmd[2000], in[1200], out[400];
    { // the step needs the toolchain's objcopy and nm (GNU binutils or LLVM's); without them pnx still builds
        char objcopy[256], probe[600];
        tool_of(t, "objcopy", objcopy, sizeof objcopy);
        snprintf(probe, sizeof probe, "%s --version >/dev/null 2>&1", objcopy);
        if (system(probe) != 0) {
            printf("nob: no %s: build/%s/release/ (the archives a host is given) is not built\n", objcopy, t->name);
            return 0;
        }
    }
    snprintf(rel, sizeof rel, "%s/release", dir);
    snprintf(wrk, sizeof wrk, "%s/release-work", dir);
    run(strcat(strcat(strcpy(cmd, "rm -rf "), rel), "/*.roots"));
    mkdirs(rel);
    mkdirs(wrk);
    // the core: the functions of pinax.h
    snprintf(roots, sizeof roots, "%s/libpinax.roots", wrk);
    FILE *f = fopen(roots, "w");
    if (!f || public_names("include/pinax.h", "PINAX_API", f, NULL))
        return 1;
    fclose(f);
    snprintf(in, sizeof in, "%s/libpinax.a", dir);
    // and every name of the core's own, so that the archive defines what pinax.c defines and pinax_native.h names
    char nm[256];
    tool_of(t, "nm", nm, sizeof nm);
    snprintf(cmd, sizeof cmd, "%s -g --defined-only %s | awk 'NF==3 && $3 ~ /^pnx_/ {print $3}' | sort -u >> %s", nm, in, roots);
    if (run(cmd))
        return 1;
    snprintf(out, sizeof out, "%s/libpinax.a", rel);
    if (localize(t, in, roots, true, out))
        return 1;
    // the host library without SQLite, and SQLite on its own (RFC-0058 C2)
    snprintf(roots, sizeof roots, "%s/libpinax_host.roots", wrk);
    f = fopen(roots, "w");
    if (!f || public_names("src/host/pinax_host.h", "PINAX_HOST_API", f, "pinax_host_sqlite") ||
        public_names("src/host/pinax_loop.h", "PINAX_HOST_API", f, NULL))
        return 1;
    fclose(f);
    snprintf(cmd, sizeof cmd, "grep -v '^pinax_host_register_sqlite$' %s > %s.tmp && mv %s.tmp %s", roots, roots, roots, roots);
    if (run(cmd))
        return 1;
    snprintf(in, sizeof in, "%s/libpinax_host.a", dir);
    snprintf(out, sizeof out, "%s/libpinax_host.a", rel);
    if (localize(t, in, roots, false, out))
        return 1;
    if (sqlite_wanted()) {
        snprintf(roots, sizeof roots, "%s/libpinax_sqlite.roots", wrk);
        f = fopen(roots, "w");
        if (!f)
            return 1;
        fputs("pinax_host_register_sqlite\npinax_host_sqlite_limits\n", f);
        fclose(f);
        snprintf(out, sizeof out, "%s/libpinax_sqlite.a", rel);
        if (localize(t, in, roots, false, out))
            return 1;
    }
    printf("nob: built %s (libpinax.a, libpinax_host.a%s: only the public names global)\n", rel,
           sqlite_wanted() ? ", libpinax_sqlite.a" : "");
    return 0;
}

static int build_lib(const target_t *t, char *lib, size_t lib_cap, char *hosted_objs, size_t hosted_cap) {
    char dir[256], objdir[300];
    build_dir(dir, sizeof dir, t);
    snprintf(objdir, sizeof objdir, "%s/obj", dir);
    mkdirs(objdir);

    static char objs[16384];
    objs[0] = '\0';
    if (compile_set(t, own_flags(t), "", core_sources, core_count, objdir, objs, sizeof objs) ||
        compile_set(t, proven_flags(t), "vendor/proven/", proven_portable, COUNT(proven_portable), objdir, objs,
                    sizeof objs) ||
        compile_set(t, proven_flags(t), "vendor/proven/", proven_lib_hosted, COUNT(proven_lib_hosted), objdir, objs,
                    sizeof objs))
        return 1;

    snprintf(lib, lib_cap, "%s/libpinax.a", dir);
    char cmd[20000];
    snprintf(cmd, sizeof cmd, "rm -f %s && %s rcs %s%s", lib, t->ar, lib, objs);
    if (run(cmd))
        return 1;

    hosted_objs[0] = '\0';
    if (compile_set(t, proven_flags(t), "vendor/proven/", proven_hosted, COUNT(proven_hosted), objdir, hosted_objs,
                    hosted_cap))
        return 1;
    printf("nob: built %s\n", lib);

    // libpinax_host.a and pnx; a public checkout always has them
    static char host_objs[4096], pnx_objs[4096];
    char host_lib[512], pnx[512];
    host_objs[0] = pnx_objs[0] = '\0';
    static char host_flags[640], main_flags[700];
    snprintf(host_flags, sizeof host_flags, "%s -Ivendor/sqlite", own_flags(t));
    snprintf(main_flags, sizeof main_flags, "%s%s", pnx_flags(t), sqlite_wanted() ? " -Ivendor/sqlite" : " -DPNX_NO_SQLITE");
    if (compile_set(t, own_flags(t), "", host_sources, COUNT(host_sources), objdir, host_objs, sizeof host_objs) ||
        compile_set(t, main_flags, "", pnx_sources, COUNT(pnx_sources), objdir, pnx_objs, sizeof pnx_objs))
        return 1;
    if (sqlite_wanted()) {
        char cache[300], obj[400];
        snprintf(cache, sizeof cache, "build/%s-sqlite", t->name);
        mkdirs(cache);
        snprintf(obj, sizeof obj, "%s/sqlite3.o", cache);
        if (compile_set(t, host_flags, "", &sqlite_module, 1, objdir, host_objs, sizeof host_objs) ||
            compile_cached(t, sqlite_flags(), sqlite_amalgamation, obj))
            return 1;
        size_t len = strlen(host_objs);
        snprintf(host_objs + len, sizeof host_objs - len, " %s", obj);
    }
    if (compile_set(t, proven_flags(t), "vendor/proven/", proven_threads, COUNT(proven_threads), objdir, host_objs,
                    sizeof host_objs))
        return 1;
    // The host library is whole: a host links libpinax_host.a and then libpinax.a, nothing more.
    // It carries the parts of proven that touch the system and, again, the portable ones they
    // call (random calls back into the system's random): a linker reads an archive once, but
    // goes round inside one archive until nothing more can be found there.
    static char proven_objs[8192];
    proven_objs[0] = '\0';
    for (size_t k = 0; k < COUNT(proven_portable) + COUNT(proven_lib_hosted); ++k) {
        char src[512], obj[1024];
        snprintf(src, sizeof src, "vendor/proven/%s",
                 k < COUNT(proven_portable) ? proven_portable[k] : proven_lib_hosted[k - COUNT(proven_portable)]);
        object_path(obj, sizeof obj, objdir, src);
        size_t len = strlen(proven_objs);
        snprintf(proven_objs + len, sizeof proven_objs - len, " %s", obj);
    }
    snprintf(host_lib, sizeof host_lib, "%s/libpinax_host.a", dir);
    snprintf(cmd, sizeof cmd, "rm -f %s && %s rcs %s%s%s%s", host_lib, t->ar, host_lib, host_objs, hosted_objs,
             proven_objs);
    if (run(cmd))
        return 1;
    snprintf(pnx, sizeof pnx, "%s/pnx%s", dir, t->exe);
    snprintf(cmd, sizeof cmd, "%s %s -o %s%s %s %s %s", t->cc, t->sanitize ? "-fsanitize=address,undefined" : "", pnx,
             pnx_objs, host_lib, lib, strcmp(t->exe, ".exe") == 0 ? "-static -lbcrypt" : "-lm -ldl -pthread");
    if (run(cmd))
        return 1;
    printf("nob: built %s\n", pnx);
    if (!t->sanitize && build_release_libs(t, dir))
        return 1;
    return 0;
}

// The core as a shared library (RFC-0058 E, D0116): its own -fPIC objects, only the functions of pinax.h
// exported (-fvisibility=hidden; on win64 the ones PINAX_API marks dllexport). Linux: libpinax.so.1.MINOR.PATCH,
// soname libpinax.so.<PINAX_ABI>, and the links libpinax.so.1 and libpinax.so; win64: pinax.dll, its import
// library libpinax.dll.a and pinax.def. A host compiles with -DPINAX_SHARED on win64 (dllimport). Exported code
// never links it (C3). On the native target the contract and the embedding example run against it.
static int cmd_shared(const target_t *t) {
    char dir[256], objdir[400], cmd[20000], own[1200], prov[1200];
    build_dir(dir, sizeof dir, t);
    snprintf(objdir, sizeof objdir, "%s/shared/obj", dir);
    mkdirs(objdir);
    const bool win = strcmp(t->exe, ".exe") == 0;
    snprintf(own, sizeof own, "%s -fPIC -fvisibility=hidden -DPINAX_SHARED -DPINAX_BUILDING", own_flags(t));
    snprintf(prov, sizeof prov, "%s -fPIC -fvisibility=hidden", proven_flags(t));
    static char objs[16384];
    objs[0] = '\0';
    if (compile_set(t, own, "", core_sources, core_count, objdir, objs, sizeof objs) ||
        compile_set(t, prov, "vendor/proven/", proven_portable, COUNT(proven_portable), objdir, objs, sizeof objs) ||
        compile_set(t, prov, "vendor/proven/", proven_lib_hosted, COUNT(proven_lib_hosted), objdir, objs, sizeof objs))
        return 1;
    // a shared library takes every object whole, so the system parts that proven's random and time call come too
    static const char *const shared_system[] = {"platform/proven_sys_time.c", "platform/proven_sys_random.c",
                                                "platform/proven_sys_thread.c"};
    if (compile_set(t, prov, "vendor/proven/", shared_system, COUNT(shared_system), objdir, objs, sizeof objs))
        return 1;
    char out[512];
    if (win) {
        snprintf(out, sizeof out, "%s/shared/pinax.dll", dir);
        snprintf(cmd, sizeof cmd, "%s -shared -o %s%s -Wl,--out-implib,%s/shared/libpinax.dll.a "
                 "-Wl,--output-def,%s/shared/pinax.def -static-libgcc -lbcrypt", t->cc, out, objs, dir, dir);
    } else {
        snprintf(out, sizeof out, "%s/shared/libpinax.so.%d.%d.%d", dir, PINAX_ABI_OF_HEADER, PINAX_MINOR_OF_HEADER,
                 PINAX_PATCH_OF_HEADER);
        snprintf(cmd, sizeof cmd, "%s -shared -o %s%s -Wl,-soname,libpinax.so.%d -Wl,--no-undefined -lm -pthread && ln -sf libpinax.so.%d.%d.%d "
                 "%s/shared/libpinax.so.%d && ln -sf libpinax.so.%d %s/shared/libpinax.so", t->cc, out, objs,
                 PINAX_ABI_OF_HEADER, PINAX_ABI_OF_HEADER, PINAX_MINOR_OF_HEADER, PINAX_PATCH_OF_HEADER, dir,
                 PINAX_ABI_OF_HEADER, PINAX_ABI_OF_HEADER, dir);
    }
    if (run(cmd))
        return 1;
    // what it exports: the functions of pinax.h, nothing else
    char nm[256];
    tool_of(t, "nm", nm, sizeof nm);
    if (!win) {
        snprintf(cmd, sizeof cmd, "test -z \"$(%s -D --defined-only %s | awk 'NF==3 && $2 ~ /[TDBR]/ && $3 !~ /^pinax_/')\"",
                 nm, out);
        if (run(cmd)) {
            fprintf(stderr, "nob: %s exports a name that is not pinax_\n", out);
            return 1;
        }
    }
    printf("nob: built %s\n", out);
    if (strcmp(t->name, "native") != 0 || t->sanitize)
        return 0;
    snprintf(cmd, sizeof cmd, "%s -std=c23 -Wall -Wextra -Werror -Iinclude docs/spec/examples/19-contract.c -L%s/shared -lpinax "
             "-Wl,-rpath,'$ORIGIN' -o %s/shared/contract && ./%s/shared/contract", t->cc, dir, dir, dir);
    if (run(cmd))
        return 1;
    if (exists("tests/embed/host_min.c")) {
        snprintf(cmd, sizeof cmd, "%s -std=c99 -Wall -Wextra -Werror -Iinclude tests/embed/host_min.c -L%s/shared -lpinax "
                 "-Wl,-rpath,'$ORIGIN' -o %s/shared/host_min && test \"$(./%s/shared/host_min)\" = 42", t->cc, dir, dir, dir);
        if (run(cmd))
            return 1;
    }
    printf("nob: the contract and the embedding example run against %s\n", out);
    return 0;
}

static int cmd_contract(const target_t *t);

static int cmd_test(const target_t *t) {
    char lib[512];
    static char hosted[8192];
    if (build_lib(t, lib, sizeof lib, hosted, sizeof hosted))
        return 1;
    if (!exists("tests/test_main.c")) {
        printf("nob: tests/ not present (public checkout); running the contract check of the examples instead\n");
        return cmd_contract(t);
    }
    char dir[256], objdir[300], exe[512];
    build_dir(dir, sizeof dir, t);
    snprintf(objdir, sizeof objdir, "%s/obj", dir);
    snprintf(exe, sizeof exe, "%s/pinax_tests%s", dir, t->exe);
    static char objs[8192];
    objs[0] = '\0';
    if (compile_set(t, own_flags(t), "", test_sources, COUNT(test_sources), objdir, objs, sizeof objs))
        return 1;
    // Objects first, then the archive, so that the hosted proven objects can pull from it.
    char cmd[20000];
    snprintf(cmd, sizeof cmd, "%s %s -o %s%s%s %s %s", t->cc, t->sanitize ? "-fsanitize=address,undefined" : "", exe,
             objs, hosted, lib, strcmp(t->exe, ".exe") == 0 ? "-static -lbcrypt" : "-lm -pthread");
    if (run(cmd))
        return 1;
    // the loop and its channels, with real threads: the host library as a host links it
    char loop_exe[512];
    snprintf(loop_exe, sizeof loop_exe, "%s/pinax_loop_tests%s", dir, t->exe);
    objs[0] = '\0';
    if (compile_set(t, own_flags(t), "", loop_test_sources, COUNT(loop_test_sources), objdir, objs, sizeof objs))
        return 1;
    snprintf(cmd, sizeof cmd, "%s %s -o %s%s %s/libpinax_host.a %s %s", t->cc,
             t->sanitize ? "-fsanitize=address,undefined" : "", loop_exe, objs, dir, lib,
             strcmp(t->exe, ".exe") == 0 ? "-static -lbcrypt" : "-lm -pthread");
    if (run(cmd))
        return 1;
    if (strcmp(t->name, "native") != 0) {
        printf("nob: built %s and %s (cross target; run them on the target)\n", exe, loop_exe);
        return 0;
    }
    snprintf(cmd, sizeof cmd, "./%s && ./%s", exe, loop_exe);
    if (run(cmd))
        return 1;
    // RFC-0043: a host that does not register sqlite does not link it (the archive members stay out)
    snprintf(cmd, sizeof cmd, "! nm %s | grep -q ' sqlite3_'", loop_exe);
    if (run(cmd))
        return 1;
    if (exists("tests/embed/host_min.c")) { // the embedding example: the public header and the library, nothing else
        snprintf(cmd, sizeof cmd,
                 "%s -std=c99 -Wall -Wextra -Werror %s -Iinclude tests/embed/host_min.c %s -lm -o %s/host_min && "
                 "test \"$(./%s/host_min)\" = 42",
                 t->cc, t->sanitize ? "-fsanitize=address,undefined" : "", lib, dir, dir);
        if (run(cmd))
            return 1;
    }
    if (exists("tests/pnx/run.sh")) { // the command and the host modules, through real files
        snprintf(cmd, sizeof cmd, "sh tests/pnx/run.sh %s/pnx", dir);
        if (run(cmd))
            return 1;
    }
    printf("nob: tests passed\n");
    return 0;
}

// ---- the one-file distribution (RFC-0015 E7) ----------------------------------------------
//
// ./nob dist writes build/dist/pinax.h (the header as it is) and build/dist/pinax.c: the core
// and the parts of the vendored proven_c_lib it calls, as one translation unit. A host adds
// those two files to its build and nothing else.

// What the core links from proven_c_lib (hosted build).
static const char *const dist_proven[] = {
    "vendor/proven/platform/proven_sys_mem.c", "vendor/proven/src/proven/buffer.c",
    "vendor/proven/src/proven/encode.c",        "vendor/proven/src/proven/url.c",
    "vendor/proven/src/proven/float_decimal.c", "vendor/proven/src/proven/float_format.c",
    "vendor/proven/src/proven/float_parse.c",   "vendor/proven/src/proven/fmt.c",
    "vendor/proven/src/proven/hash.c",          "vendor/proven/src/proven/scan.c",
    "vendor/proven/src/proven/u16str.c",        "vendor/proven/src/proven/u8str.c",
    "vendor/proven/src/proven/utf.c",           "vendor/proven/platform/proven_sys_crc.c",
};
// Static names several of those files define: in one translation unit each file listed here
// sees its own under another name.
static const struct {
    const char *file, *name;
} dist_renames[] = {{"vendor/proven/src/proven/encode.c", "hex_value"}, {"vendor/proven/src/proven/url.c", "hex_value"}};

static const char *const dist_dirs[] = {"include/", "src/", "vendor/proven/include/", "vendor/proven/platform/",
                                        "vendor/proven/src/proven/", "vendor/proven/src/"};

static char   dist_seen[256][200];
static size_t dist_seen_n;

static bool dist_visit(const char *path) {
    for (size_t k = 0; k < dist_seen_n; ++k) {
        if (strcmp(dist_seen[k], path) == 0)
            return false;
    }
    if (dist_seen_n == COUNT(dist_seen))
        return false;
    snprintf(dist_seen[dist_seen_n++], sizeof dist_seen[0], "%s", path);
    return true;
}

// Copies `path` to `out`, putting each file it includes with quotes in its place, once.
// A file included more than once on purpose (the interpreter's body, once for pnx_execute and once
// for pnx_step): put in every time, not only the first.
static const char *const dist_repeat[] = {"src/pinax_exec.h"};
static bool dist_repeats(const char *path) {
    for (size_t k = 0; k < COUNT(dist_repeat); ++k)
        if (strcmp(path, dist_repeat[k]) == 0)
            return true;
    return false;
}

static int dist_emit(FILE *out, const char *path) {
    FILE *in = fopen(path, "r");
    if (!in) {
        fprintf(stderr, "nob: cannot read %s\n", path);
        return 1;
    }
    char dir[200] = "";
    const char *slash = strrchr(path, '/');
    if (slash)
        snprintf(dir, sizeof dir, "%.*s", (int)(slash - path + 1), path);
    fprintf(out, "\n/* ---- %s ---- */\n", path);
    char line[4096];
    int  status = 0;
    while (!status && fgets(line, sizeof line, in)) {
        const char *p = line;
        while (*p == ' ' || *p == '\t')
            ++p;
        char name[160];
        if (*p == '#' && sscanf(p + 1, " include \"%159[^\"]\"", name) == 1) {
            if (strcmp(name, "pinax.h") == 0) { // the public header stays a file of its own
                if (dist_visit("include/pinax.h"))
                    fputs("#include \"pinax.h\"\n", out);
                continue;
            }
            char found[400] = "";
            snprintf(found, sizeof found, "%s%s", dir, name);
            for (size_t k = 0; !exists(found) && k < COUNT(dist_dirs); ++k)
                snprintf(found, sizeof found, "%s%s", dist_dirs[k], name);
            if (exists(found)) {
                if (dist_repeats(found) || dist_visit(found))
                    status = dist_emit(out, found);
                continue;
            }
        }
        fputs(line, out);
    }
    fclose(in);
    return status;
}

static int cmd_dist(const target_t *t) {
    if (mkdirs("build/dist"))
        return 1;
    if (run("cp include/pinax.h build/dist/pinax.h"))
        return 1;
    FILE *out = fopen("build/dist/pinax.c", "w");
    if (!out)
        return 1;
    fputs("/*\n * pinax.c - the Pinax scripting language as one file: the core and the parts of\n"
          " * proven_c_lib it uses. Generated by `./nob dist`; do not edit. Build it as C23 beside\n"
          " * pinax.h:   cc -std=c23 -c pinax.c\n"
          " * Both Pinax and proven_c_lib are under the MIT license; their notices follow.\n */\n",
          out);
    static const char *const notices[] = {"LICENSE", "vendor/proven/LICENSE"};
    for (size_t k = 0; k < COUNT(notices); ++k) {
        FILE *in = fopen(notices[k], "r");
        char  line[1024];
        if (!in) {
            fprintf(stderr, "nob: cannot read %s\n", notices[k]);
            fclose(out);
            return 1;
        }
        fprintf(out, "/*\n * %s\n *\n", k ? "proven_c_lib" : "Pinax");
        while (fgets(line, sizeof line, in))
            fprintf(out, " * %s", line);
        fputs(" */\n", out);
        fclose(in);
    }
    // a table of a proven header is used by one of its files only: here every file sees it
    fputs("#if defined(__GNUC__)\n#pragma GCC diagnostic ignored \"-Wunused-const-variable\"\n#endif\n", out);
    dist_seen_n = 0;
    int status = 0;
    for (size_t k = 0; !status && k < COUNT(dist_proven); ++k) {
        for (size_t j = 0; j < COUNT(dist_renames); ++j) {
            if (strcmp(dist_renames[j].file, dist_proven[k]) == 0)
                fprintf(out, "#define %s %s_%zu\n", dist_renames[j].name, dist_renames[j].name, k);
        }
        status = dist_emit(out, dist_proven[k]);
        for (size_t j = 0; j < COUNT(dist_renames); ++j) {
            if (strcmp(dist_renames[j].file, dist_proven[k]) == 0)
                fprintf(out, "#undef %s\n", dist_renames[j].name);
        }
    }
    for (size_t k = 0; !status && k < core_count; ++k)
        status = dist_emit(out, core_sources[k]);
    if (fclose(out) || status)
        return 1;
    // it must build on its own, and the embedding example must run on it
    char cmd[2000];
    snprintf(cmd, sizeof cmd, "%s -std=c23 -O2 -Wall -Wextra -Werror -Ibuild/dist -c build/dist/pinax.c -o build/dist/pinax.o",
             t->cc);
    if (run(cmd))
        return 1;
    // Only the public names stay global (RFC-0058 C1): every other global of the file gets the reserved prefix,
    // pnx_x as pinax__x and proven_x as pinax__proven_x, by a block of defines put first, then it is built again.
    // pinax_native.h gets the same block for the names it calls.
    snprintf(cmd, sizeof cmd,
             "nm -g --defined-only build/dist/pinax.o | awk 'NF==3 && $3 ~ /^pnx_/ {print \"#define \"$3\" pinax__\"substr($3,5)} "
             "NF==3 && $3 ~ /^proven_/ {print \"#define \"$3\" pinax__\"$3}' | sort -u > build/dist/prefix.h && "
             "{ echo '/* Every name of this file that is not public gets the prefix pinax__ (RFC-0058 C1). */'; "
             "cat build/dist/prefix.h build/dist/pinax.c; } > build/dist/pinax.c.new && mv build/dist/pinax.c.new build/dist/pinax.c");
    if (run(cmd))
        return 1;
    snprintf(cmd, sizeof cmd, "%s -std=c23 -O2 -Wall -Wextra -Werror -Ibuild/dist -c build/dist/pinax.c -o build/dist/pinax.o && "
             "test -z \"$(nm -g --defined-only build/dist/pinax.o | awk 'NF==3 && $3 !~ /^pinax_/')\"",
             t->cc);
    if (run(cmd))
        return 1;
    if (exists("tests/embed/host_min.c")) {
        snprintf(cmd, sizeof cmd,
                 "%s -std=c99 -Wall -Wextra -Werror -Ibuild/dist tests/embed/host_min.c build/dist/pinax.o -lm "
                 "-o build/dist/host_min && test \"$(./build/dist/host_min)\" = 42",
                 t->cc);
        if (run(cmd))
            return 1;
    }
    // what exported code includes (RFC-0028): the instruction functions, as one header of this build
    FILE *native = fopen("build/dist/pinax_native.h", "w");
    if (!native)
        return 1;
    fputs("/*\n * pinax_native.h - what the C that `pnx export-c` writes includes: the instruction functions\n"
          " * of this build of pinax.c. Generated by `./nob dist`; do not edit. Code exported by another build\n"
          " * carries another hash and is not attached. Under the MIT license, as pinax.c.\n */\n"
          "#ifndef PINAX_NATIVE_H\n#define PINAX_NATIVE_H\n"
          "#if defined(__GNUC__)\n#pragma GCC diagnostic ignored \"-Wunused-const-variable\"\n"
          "#pragma GCC diagnostic ignored \"-Wunused-function\"\n#endif\n",
          native);
    { // the names of pinax.c it calls, under the prefix pinax.c gives them (RFC-0058 C1)
        FILE *prefix = fopen("build/dist/prefix.h", "r");
        char  line[512];
        if (!prefix) {
            fclose(native);
            return 1;
        }
        while (fgets(line, sizeof line, prefix))
            if (strncmp(line, "#define pnx_", 12) == 0)
                fputs(line, native);
        fclose(prefix);
    }
    dist_seen_n = 0;
    status = dist_emit(native, "src/pinax_ops.h");
    fputs("#endif\n", native);
    if (fclose(native) || status)
        return 1;
    snprintf(cmd, sizeof cmd,
             "printf '#include \"pinax_native.h\"\\nint pinax_native_check(void) { return PINAX_OK; }\\n' | "
             "%s -std=c23 -Wall -Wextra -Werror -Ibuild/dist -x c -c - -o build/dist/native_check.o",
             t->cc);
    if (run(cmd))
        return 1;
    if (run("! grep -qi sqlite build/dist/pinax.c")) // RFC-0043: the one-file core knows nothing of sqlite
        return 1;
    printf("nob: wrote build/dist/pinax.h, build/dist/pinax.c and build/dist/pinax_native.h\n");
    return 0;
}

static int cmd_freestanding(void) {
    const char *objdir = "build/freestanding/obj";
    mkdirs(objdir);
    target_t t = {.name = "freestanding", .cc = getenv("CC") ? getenv("CC") : "cc"};
    const char *fs = "-ffreestanding -DPROVEN_FREESTANDING -DPROVEN_FMT_NO_FLOAT -DPROVEN_NO_U16STR";
    char own[1024], prov[1024];
    snprintf(own, sizeof own, "-std=c23 -Wall -Wextra -pedantic -Werror -O2 %s -Iinclude -Isrc -Ivendor/proven/include", fs);
    snprintf(prov, sizeof prov, "-std=c23 -Wall -Wextra -Werror -O2 %s -Ivendor/proven/include -Ivendor/proven/platform",
             fs);
    static char objs[16384];
    objs[0] = '\0';
    if (compile_set(&t, own, "", core_sources, core_count, objdir, objs, sizeof objs) ||
        compile_set(&t, prov, "vendor/proven/", proven_portable, COUNT(proven_portable), objdir, objs, sizeof objs))
        return 1;
    printf("nob: freestanding compile ok (%zu core + %zu proven files)\n", core_count, COUNT(proven_portable));
    return 0;
}

// The whole file at `path`, 0-terminated, in memory the caller frees; NULL when it cannot be read.
static char *slurp(const char *path, size_t *length) {
    FILE *f = fopen(path, "rb");
    if (!f)
        return NULL;
    char  *text = NULL;
    size_t n = 0, cap = 0;
    for (size_t got;;) {
        if (n + 65536 + 1 > cap) {
            char *bigger = realloc(text, cap = (n + 65536 + 1) * 2);
            if (!bigger) {
                free(text);
                fclose(f);
                return NULL;
            }
            text = bigger;
        }
        if ((got = fread(text + n, 1, 65536, f)) == 0)
            break;
        n += got;
    }
    fclose(f);
    text[n] = '\0';
    *length = n;
    return text;
}

// A JavaScript module's text with each line's leading `export ` taken off, so that it can stand
// inside a page's script (the page and the module become one script).
static void put_module(FILE *out, const char *text) {
    for (const char *line = text; *line;) {
        const char *end = strchr(line, '\n');
        end = end ? end + 1 : line + strlen(line);
        if (strncmp(line, "export ", 7) == 0)
            line += 7;
        fwrite(line, 1, (size_t)(end - line), out);
        line = end;
    }
}

// The version of include/pinax.h ("0.34.5") and the date of the newest release heading of CHANGELOG.md ("2026-11-19"), for the
// start message of the playground. Each is "?" when it cannot be read.
static void release_facts(char *version, size_t vn, char *date, size_t dn) {
    snprintf(version, vn, "?");
    snprintf(date, dn, "?");
    size_t n;
    char  *h = slurp("include/pinax.h", &n);
    if (h) {
        const char *k[3] = {"PINAX_VERSION_MAJOR", "PINAX_VERSION_MINOR", "PINAX_VERSION_PATCH"};
        int         v[3] = {-1, -1, -1};
        for (int i = 0; i < 3; ++i) {
            const char *at = strstr(h, k[i]);
            if (at)
                v[i] = atoi(at + strlen(k[i]));
        }
        if (v[0] >= 0 && v[1] >= 0 && v[2] >= 0)
            snprintf(version, vn, "%d.%d.%d", v[0], v[1], v[2]);
        free(h);
    }
    char *c = slurp("CHANGELOG.md", &n);
    if (c) {
        // the first heading with a date ("## [0.36.0] - 2026-11-25"), with or without an `## [Unreleased]` above it
        for (const char *r = strstr(c, "\n## ["); r; r = strstr(r + 1, "\n## [")) {
            const char *eol = strchr(r + 1, '\n'), *close = strstr(r + 1, "] - ");
            if (close && (!eol || close < eol) && strlen(close) >= 14) {
                snprintf(date, dn, "%.10s", close + 4);
                break;
            }
        }
        free(c);
    }
}

// `page` with every `from` replaced by `to`, in memory the caller frees.
static char *replace_all(const char *page, size_t *n, const char *from, const char *to) {
    size_t count = 0, fl = strlen(from), tl = strlen(to);
    for (const char *p = page; (p = strstr(p, from)); p += fl)
        ++count;
    char *out = malloc(*n + count * (tl > fl ? tl - fl : 0) + 1), *w = out;
    if (!out)
        return NULL;
    const char *p = page, *q;
    while ((q = strstr(p, from))) {
        memcpy(w, p, (size_t)(q - p));
        w += q - p;
        memcpy(w, to, tl);
        w += tl;
        p = q + fl;
    }
    size_t rest = strlen(p);
    memcpy(w, p, rest + 1);
    *n = (size_t)(w - out) + rest;
    return out;
}

// build/dist/pinax-playground.html: src/wasm/playground.html with, at its marker lines, pinax.wasm
// in base64 (@PINAX_WASM_BASE64@), pinax.mjs (// @PINAX_MJS@) and console.mjs (// @PINAX_CONSOLE@),
// in that order. One file that runs Pinax opened from disk, with no server (the page cannot fetch
// beside itself there).
static int write_playground(void) {
    size_t page_n, mjs_n, con_n, words_n, comp_n, wasm_n;
    char  *page = slurp("src/wasm/playground.html", &page_n);
    if (page) { // the start message names the version and the day of its release
        char version[32], date[32];
        release_facts(version, sizeof version, date, sizeof date);
        char *a1 = replace_all(page, &page_n, "@PINAX_VERSION@", version);
        char *a2 = a1 ? replace_all(a1, &page_n, "@PINAX_DATE@", date) : NULL;
        free(page), free(a1);
        page = a2;
    }
    char  *mjs = slurp("src/wasm/pinax.mjs", &mjs_n);
    char  *con = slurp("src/wasm/console.mjs", &con_n);
    char  *words = slurp("src/wasm/words.mjs", &words_n);
    char  *comp = slurp("src/wasm/complete.mjs", &comp_n);
    char  *wasm = slurp("build/wasm/pinax.wasm", &wasm_n);
    FILE  *out = NULL;
    int    bad = 1;
    static const char *const marks[] = {"@PINAX_WASM_BASE64@\n", "// @PINAX_MJS@\n", "// @PINAX_CONSOLE@\n"};
    char *at[3] = {NULL, NULL, NULL};
    for (int k = 0; k < 3 && page; ++k)
        at[k] = strstr(k ? at[k - 1] : page, marks[k]);
    if (!at[2] || !mjs || !con || !words || !comp || !wasm || strstr(mjs, "</script") || strstr(con, "</script") || strstr(words, "</script") || strstr(comp, "</script") ||
        mkdirs("build/dist") || !(out = fopen("build/dist/pinax-playground.html", "wb"))) {
        fprintf(stderr, "nob: cannot write build/dist/pinax-playground.html\n");
        goto done;
    }
    const char *from = page;
    for (int k = 0; k < 3; ++k) {
        fwrite(from, 1, (size_t)(at[k] - from), out);
        from = at[k] + strlen(marks[k]);
        if (k == 1)
            put_module(out, mjs);
        else if (k == 2) { // the words and the completion come before the terminal that the page builds on
            put_module(out, words);
            put_module(out, comp);
            put_module(out, con);
        }
        else {
            static const char digits[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
            const unsigned char *b = (const unsigned char *)wasm;
            for (size_t i = 0; i < wasm_n; i += 3) {
                const uint32_t v = (uint32_t)b[i] << 16 | (i + 1 < wasm_n ? (uint32_t)b[i + 1] << 8 : 0) |
                                   (i + 2 < wasm_n ? b[i + 2] : 0);
                fputc(digits[v >> 18 & 63], out);
                fputc(digits[v >> 12 & 63], out);
                fputc(i + 1 < wasm_n ? digits[v >> 6 & 63] : '=', out);
                fputc(i + 2 < wasm_n ? digits[v & 63] : '=', out);
                if ((i / 3 + 1) % 19 == 0) // lines of 76
                    fputc('\n', out);
            }
            fputc('\n', out);
        }
    }
    fwrite(from, 1, page_n - (size_t)(from - page), out);
    bad = ferror(out);
done:
    if (out && fclose(out))
        bad = 1;
    free(page), free(mjs), free(con), free(wasm);
    return bad;
}

// The three promises a host relies on (a block too small, a budget and its resume, a registered C function), as
// docs/spec/examples/19-contract.c states them: built against the library of this target and run, so that an
// adopter can check them with their own compiler and platform with one command: ./nob contract
static int cmd_contract(const target_t *t) {
    char lib[512];
    static char hosted[8192];
    if (build_lib(t, lib, sizeof lib, hosted, sizeof hosted))
        return 1;
    if (!exists("docs/spec/examples/19-contract.c")) {
        fprintf(stderr, "nob: docs/spec/examples/19-contract.c is missing\n");
        return 1;
    }
    char dir[256], exe[512], cmd[4096];
    build_dir(dir, sizeof dir, t);
    snprintf(exe, sizeof exe, "%s/contract%s", dir, t->exe);
    if (!t->sanitize) { // the promises hold for what a release gives a host: the core's archive alone (RFC-0058 C1)
        snprintf(lib, sizeof lib, "%s/release/libpinax.a", dir);
        hosted[0] = '\0';
    }
    snprintf(cmd, sizeof cmd, "%s -std=c23 -Wall -Wextra -Werror -Iinclude %s docs/spec/examples/19-contract.c%s %s %s -o %s", t->cc,
             t->sanitize ? "-fsanitize=address,undefined" : "", hosted, lib,
             strcmp(t->exe, ".exe") == 0 ? "-static -lbcrypt" : "-lm -pthread", exe);
    if (run(cmd))
        return 1;
    if (strcmp(t->name, "native") != 0) {
        printf("nob: built %s (cross target; run it on the target)\n", exe);
        return 0;
    }
    snprintf(cmd, sizeof cmd, "./%s", exe);
    return run(cmd);
}

// The freestanding part as a WebAssembly module, with the glue of src/wasm/: no C library, the
// module io printing through an import, exports that src/wasm/pinax.mjs drives (P3).
static int cmd_wasm(void) {
    const char *objdir = "build/wasm/obj";
    mkdirs(objdir);
    static char cc[512];
    snprintf(cc, sizeof cc, "%s --target=wasm32 -nostdlib", getenv("PINAX_WASM_CC") ? getenv("PINAX_WASM_CC") : "clang");
    target_t t = {.name = "wasm", .cc = cc};
    const char *fs = "-ffreestanding -DPROVEN_FREESTANDING -DPROVEN_FMT_NO_FLOAT -DPROVEN_NO_U16STR";
    char own[1024], prov[1024];
    snprintf(own, sizeof own, "-std=c23 -Wall -Wextra -pedantic -Werror -O2 %s -Iinclude -Isrc -Ivendor/proven/include", fs);
    snprintf(prov, sizeof prov, "-std=c23 -Wall -Wextra -Werror -O2 %s -Ivendor/proven/include -Ivendor/proven/platform",
             fs);
    static const char *const glue[] = {"src/wasm/pinax_wasm.c"};
    static char objs[16384];
    objs[0] = '\0';
    if (compile_set(&t, own, "", core_sources, core_count, objdir, objs, sizeof objs) ||
        compile_set(&t, prov, "vendor/proven/", proven_portable, COUNT(proven_portable), objdir, objs, sizeof objs) ||
        compile_set(&t, own, "", glue, COUNT(glue), objdir, objs, sizeof objs))
        return 1;
    // a stack of 1 MB: the compiler recurses into nested blocks
    static char cmd[20000];
    snprintf(cmd, sizeof cmd,
             "%s -Wl,--no-entry -Wl,-z,stack-size=1048576 -Wl,--strip-all -o build/wasm/pinax.wasm%s && "
             "cp src/wasm/pinax.mjs build/wasm/pinax.mjs",
             cc, objs);
    if (run(cmd))
        return 1;
    if (write_playground())
        return 1;
    // the Pages copy (docs/playground/index.html) is the same file, so it cannot drift
    {
        size_t n;
        char  *page = slurp("build/dist/pinax-playground.html", &n);
        FILE  *f = page && !mkdirs("docs/playground") ? fopen("docs/playground/index.html", "wb") : NULL;
        if (!f || fwrite(page, 1, n, f) != n) {
            fprintf(stderr, "nob: cannot write docs/playground/index.html\n");
            return 1;
        }
        fclose(f);
    }
    printf("nob: wrote build/wasm/pinax.wasm, build/wasm/pinax.mjs and build/dist/pinax-playground.html\n");
    return 0;
}

// ---------------------------------------------------------------------------------------
// Checks
// ---------------------------------------------------------------------------------------

// pinax.h promises to need nothing but <stddef.h> and <stdint.h>, in C99 and later and in C++.
static int check_headers(int *problems) {
    mkdirs("build/check");
    // Each public header alone, in every C and C++ standard the promise names (RFC-0058 B6, D0116),
    // with the compiler of the build and, when it is there, clang.
    static const char *const headers[] = {"pinax.h", "pinax_loop.h", "pinax_host.h"};
    for (size_t h = 0; h < COUNT(headers); ++h) {
        char path[256];
        for (int cpp = 0; cpp < 2; ++cpp) {
            snprintf(path, sizeof path, "build/check/header%zu.%s", h, cpp ? "cpp" : "c");
            FILE *f = fopen(path, "w");
            if (!f)
                return 1;
            fprintf(f, "#include \"%s\"\nint pinax_header_check(void) { return PINAX_OK; }\n", headers[h]);
            fclose(f);
        }
    }
    const char *cc = getenv("CC") ? getenv("CC") : "cc";
    const char *cxx = getenv("CXX") ? getenv("CXX") : "c++";
    const char *compilers[2][2] = {{cc, cxx}, {"clang", "clang++"}};
    char cmd[1024];
    static const char *const standards[] = {"c99", "c11", "c17", "c23"};
    static const char *const cxx_standards[] = {"c++11", "c++20"};
    for (int k = 0; k < 2; ++k) {
        snprintf(cmd, sizeof cmd, "%s --version >/dev/null 2>&1", compilers[k][0]);
        if (k == 1 && system(cmd) != 0)
            continue;
        for (size_t h = 0; h < COUNT(headers); ++h)
            for (size_t i = 0; i < COUNT(standards); ++i) {
                snprintf(cmd, sizeof cmd,
                         "%s -std=%s -Wall -Wextra -Wpedantic -Werror -Iinclude -Isrc/host -fsyntax-only build/check/header%zu.c",
                         compilers[k][0], standards[i], h);
                *problems += run(cmd);
            }
        snprintf(cmd, sizeof cmd, "%s --version >/dev/null 2>&1", compilers[k][1]);
        if (system(cmd) != 0) {
            printf("nob check: no %s; skipping its C++ header check\n", compilers[k][1]);
            continue;
        }
        for (size_t h = 0; h < COUNT(headers); ++h)
            for (size_t i = 0; i < COUNT(cxx_standards); ++i) {
                snprintf(cmd, sizeof cmd,
                         "%s -std=%s -Wall -Wextra -Wpedantic -Werror -Iinclude -Isrc/host -fsyntax-only build/check/header%zu.cpp",
                         compilers[k][1], cxx_standards[i], h);
                *problems += run(cmd);
            }
    }
    FILE *f;
    // The proven adapter, with proven on the include path.
    f = fopen("build/check/adapter.c", "w");
    if (!f)
        return 1;
    fputs("#include \"pinax_proven.h\"\nint pinax_adapter_check(void) { return PINAX_OK; }\n", f);
    fclose(f);
    snprintf(cmd, sizeof cmd,
             "%s -std=c23 -Wall -Wextra -pedantic -Werror -Iinclude -Ivendor/proven/include -fsyntax-only "
             "build/check/adapter.c", cc);
    *problems += run(cmd);
    return 0;
}

static int check_public(int *problems) {
    FILE *p = popen("git ls-files 2>/dev/null", "r");
    if (!p)
        return 1;
    char line[1024];
    int files = 0;
    while (fgets(line, sizeof line, p)) {
        line[strcspn(line, "\n")] = '\0';
        files++;
        bool ok = false;
        for (size_t k = 0; k < COUNT(public_paths); ++k) {
            size_t len = strlen(public_paths[k]);
            ok |= public_paths[k][len - 1] == '/' ? strncmp(line, public_paths[k], len) == 0
                                                 : strcmp(line, public_paths[k]) == 0;
        }
        if (!ok) {
            fprintf(stderr, "nob check: tracked file outside the public list: %s\n", line);
            (*problems)++;
        }
    }
    pclose(p);
    // Local paths must not appear in public files.
    // The pattern is split so that this file does not match its own scan.
    p = popen("git ls-files -z -- . ':(exclude)vendor/sqlite' | xargs -0 grep -lE '/(o)pt/data|/(h)ome/[a-z]|/(m)nt/[a-z]|/(U)sers/' 2>/dev/null", "r");
    if (p) {
        while (fgets(line, sizeof line, p)) {
            line[strcspn(line, "\n")] = '\0';
            fprintf(stderr, "nob check: local absolute path in public file %s\n", line);
            (*problems)++;
        }
        pclose(p);
    }
    // Pinax's own public sources are ASCII (the Korean README and the vendored tree are not checked).
    p = popen("git ls-files -z -- nob.c include src README.md | LC_ALL=C xargs -0 grep -lP '[^\\x00-\\x7F]' 2>/dev/null", "r");
    if (p) {
        while (fgets(line, sizeof line, p)) {
            line[strcspn(line, "\n")] = '\0';
            if (strcmp(line, "README.md") == 0)
                continue; // its first line links the Korean README by name
            fprintf(stderr, "nob check: non-ASCII character in %s\n", line);
            (*problems)++;
        }
        pclose(p);
    }
    printf("nob check: %d tracked files checked against the public list\n", files);
    return 0;
}

static int cmd_check(void) {
    int problems = 0;
    if (check_headers(&problems) || check_public(&problems))
        return 1;
    if (problems) {
        fprintf(stderr, "nob check: %d problem(s)\n", problems);
        return 1;
    }
    printf("nob check: ok\n");
    return 0;
}

int main(int argc, char **argv) {
    target_t t = {.name = "native", .cc = getenv("CC") ? getenv("CC") : "cc", .ar = "ar", .exe = ""};
    const char *command = "build";
    for (int i = 1; i < argc; ++i) {
        const char *a = argv[i];
        if (strcmp(a, "--target=win64") == 0) {
            t.name = "win64";
            t.cc = getenv("PINAX_WIN64_CC") ? getenv("PINAX_WIN64_CC") : "x86_64-w64-mingw32-gcc";
            t.ar = getenv("PINAX_WIN64_AR") ? getenv("PINAX_WIN64_AR") : "x86_64-w64-mingw32-ar";
            t.exe = ".exe";
        } else if (strcmp(a, "--target=native") == 0) {
        } else if (strcmp(a, "--sanitize") == 0) {
            t.sanitize = true;
        } else if (strcmp(a, "--debug") == 0) {
            t.debug = true;
        } else if (strcmp(a, "--proofs") == 0) {
            t.proofs = true;
        } else if (strcmp(a, "--switch") == 0) {
            t.switch_dispatch = true;
        } else if (a[0] != '-' && strcmp(command, "build") == 0) {
            command = a;
        } else {
            fprintf(stderr, "nob: unknown option %s\n"
                            "usage: ./nob [build|test|contract|check|freestanding|wasm|dist|abi|shared|all|clean] "
                            "[--target=native|win64] [--sanitize] [--debug] [--proofs] [--switch]\n", a);
            return 2;
        }
    }

    if (strcmp(command, "build") == 0) {
        char lib[512];
        static char hosted[8192];
        return build_lib(&t, lib, sizeof lib, hosted, sizeof hosted);
    }
    if (strcmp(command, "test") == 0)
        return cmd_test(&t);
    if (strcmp(command, "check") == 0)
        return cmd_check();
    if (strcmp(command, "freestanding") == 0)
        return cmd_freestanding();
    if (strcmp(command, "dist") == 0)
        return cmd_dist(&t);
    if (strcmp(command, "contract") == 0)
        return cmd_contract(&t);
    if (strcmp(command, "wasm") == 0)
        return cmd_wasm();
    if (strcmp(command, "shared") == 0)
        return cmd_shared(&t);
    if (strcmp(command, "abi") == 0) {
        // The binary interface of the public headers against the baselines (RFC-0058 D, D0116); it needs
        // clang (every target compiled only) and the built deliverables for the symbol lines.
        if (!exists("tests/abi/abi.py")) {
            printf("nob: tests/ not present (public checkout): no ABI baselines here\n");
            return 0;
        }
        return run("python3 tests/abi/abi.py");
    }
    if (strcmp(command, "all") == 0)
        return cmd_check() || cmd_freestanding() || cmd_test(&t) || cmd_contract(&t) || cmd_dist(&t);
    if (strcmp(command, "clean") == 0)
        return run("rm -rf build");
    fprintf(stderr, "nob: unknown command %s\n", command);
    return 2;
}
