// nob.c - Pinax build driver. Self-contained: the C standard library and mkdir only.
//
//   cc -std=c23 -o nob nob.c
//   ./nob                     build build/<target>/libpinax.a (today: the vendored proven_c_lib
//                             parts Pinax uses; the interpreter is not written yet)
//   ./nob test                build, then run the tests (the tests are skipped when tests/ is absent)
//   ./nob check               the public header compiles as C99, C23 and C++17; public files
//                             are ASCII, inside the public list and free of local paths
//   ./nob freestanding        compile the portable part with -ffreestanding
//   ./nob all                 check + freestanding + test
//   ./nob clean               remove build/
//   options: --target=win64   cross-compile with MinGW-w64 ($PINAX_WIN64_CC, default
//                             x86_64-w64-mingw32-gcc); --sanitize (ASan + UBSan); --debug
//
// Two flag sets. Pinax's sources: -std=c23 -Wall -Wextra -pedantic -Werror. The vendored
// proven_c_lib: its own set (no -pedantic, which rejects its __int128), see
// vendor/proven/VENDORED.md.

#if !defined(_WIN32)
#define _POSIX_C_SOURCE 200809L
#endif

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(_WIN32)
#include <direct.h>
#define make_dir(p) _mkdir(p)
#else
#include <sys/stat.h>
#define make_dir(p) mkdir((p), 0777)
#endif

#define COUNT(a) (sizeof(a) / sizeof((a)[0]))

// Pinax sources (no OS access; freestanding).
static const char *const core_sources[] = {"src/pinax_compile.c", "src/pinax_lex.c", "src/pinax_state.c", "src/pinax_text.c", "src/pinax_vm.c", "src/pinax_ptn.c", "src/pinax_rex.c"};
static const size_t core_count = COUNT(core_sources);

// proven_c_lib files that build without an operating system.
static const char *const proven_portable[] = {
    "src/proven/algorithm.c", "src/proven/arena.c",         "src/proven/array.c",
    "src/proven/buffer.c",    "src/proven/deflate.c",       "src/proven/encode.c",
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
static const char *const proven_lib_hosted[] = {"platform/proven_sys_mem.c", "src/proven/u16str.c"};

// proven_c_lib files that need the operating system. Tools and tests link them; the
// library itself stays freestanding.
static const char *const proven_hosted[] = {
    "src/proven/alloc_check.c",  "src/proven/fs.c",            "src/proven/mmap.c",
    "src/proven/stream.c",       "src/proven/sysio.c",
    "platform/proven_sys_env.c", "platform/proven_sys_fs.c",   "platform/proven_sys_io.c",
    "platform/proven_sys_random.c", "platform/proven_sys_time.c",
};

// The standard host modules and the command pnx (RFC-0015): hosted, built on pinax.h.
static const char *const host_sources[] = {"src/host/pinax_host.c"};
static const char *const pnx_sources[] = {"src/host/pnx_main.c", "src/host/pnx_fmt.c"};

static const char *const test_sources[] = {"tests/test_main.c"};

// Public files and directories; `git ls-files` must stay inside.
static const char *const public_paths[] = {
    ".gitignore", "README.md", "README.ko.md", "LICENSE", "nob.c", "include/", "src/", "vendor/",
};

typedef struct {
    const char *name;
    const char *cc;
    const char *ar;
    const char *exe;
    bool sanitize;
    bool debug;
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
    snprintf(out, cap, "build/%s%s", t->name, t->sanitize ? "-asan" : (t->debug ? "-debug" : ""));
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
    snprintf(buf, sizeof buf, "%s %s", t->debug || t->sanitize ? "-O0 -g" : "-O2",
             t->sanitize ? "-fsanitize=address,undefined -fno-omit-frame-pointer -fno-sanitize-recover=all" : "");
    return buf;
}

static const char *own_flags(const target_t *t) {
    static char buf[512];
    snprintf(buf, sizeof buf, "-std=c23 -Wall -Wextra -pedantic -Werror %s -Iinclude -Isrc -Ivendor/proven/include",
             mode_flags(t));
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
    if (compile_set(t, own_flags(t), "", host_sources, COUNT(host_sources), objdir, host_objs, sizeof host_objs) ||
        compile_set(t, own_flags(t), "", pnx_sources, COUNT(pnx_sources), objdir, pnx_objs, sizeof pnx_objs))
        return 1;
    snprintf(host_lib, sizeof host_lib, "%s/libpinax_host.a", dir);
    snprintf(cmd, sizeof cmd, "rm -f %s && %s rcs %s%s", host_lib, t->ar, host_lib, host_objs);
    if (run(cmd))
        return 1;
    snprintf(pnx, sizeof pnx, "%s/pnx%s", dir, t->exe);
    snprintf(cmd, sizeof cmd, "%s %s -o %s%s %s%s %s %s", t->cc, t->sanitize ? "-fsanitize=address,undefined" : "", pnx,
             pnx_objs, host_lib, hosted_objs, lib, strcmp(t->exe, ".exe") == 0 ? "-static -lbcrypt" : "-lm -ldl");
    if (run(cmd))
        return 1;
    printf("nob: built %s\n", pnx);
    return 0;
}

static int cmd_test(const target_t *t) {
    char lib[512];
    static char hosted[8192];
    if (build_lib(t, lib, sizeof lib, hosted, sizeof hosted))
        return 1;
    if (!exists("tests/test_main.c")) {
        printf("nob: tests/ not present (public checkout); skipping tests\n");
        return 0;
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
             objs, hosted, lib, strcmp(t->exe, ".exe") == 0 ? "-static -lbcrypt" : "-lm");
    if (run(cmd))
        return 1;
    if (strcmp(t->name, "native") != 0) {
        printf("nob: built %s (cross target; run it on the target)\n", exe);
        return 0;
    }
    snprintf(cmd, sizeof cmd, "./%s", exe);
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
    "vendor/proven/src/proven/utf.c",
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
                if (dist_visit(found))
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
    if (exists("tests/embed/host_min.c")) {
        snprintf(cmd, sizeof cmd,
                 "%s -std=c99 -Wall -Wextra -Werror -Ibuild/dist tests/embed/host_min.c build/dist/pinax.o -lm "
                 "-o build/dist/host_min && test \"$(./build/dist/host_min)\" = 42",
                 t->cc);
        if (run(cmd))
            return 1;
    }
    printf("nob: wrote build/dist/pinax.h and build/dist/pinax.c\n");
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

// ---------------------------------------------------------------------------------------
// Checks
// ---------------------------------------------------------------------------------------

// pinax.h promises to need nothing but <stddef.h> and <stdint.h>, in C99 and later and in C++.
static int check_headers(int *problems) {
    mkdirs("build/check");
    FILE *f = fopen("build/check/header.c", "w");
    if (!f)
        return 1;
    fputs("#include \"pinax.h\"\nint pinax_header_check(void) { return PINAX_OK; }\n", f);
    fclose(f);
    if (run("cp build/check/header.c build/check/header.cpp"))
        return 1;
    const char *cc = getenv("CC") ? getenv("CC") : "cc";
    const char *cxx = getenv("CXX") ? getenv("CXX") : "c++";
    char cmd[1024];
    static const char *const standards[] = {"c99", "c23"};
    for (size_t i = 0; i < COUNT(standards); ++i) {
        snprintf(cmd, sizeof cmd, "%s -std=%s -Wall -Wextra -Wpedantic -Werror -Iinclude -fsyntax-only build/check/header.c",
                 cc, standards[i]);
        *problems += run(cmd);
    }
    snprintf(cmd, sizeof cmd, "%s --version >/dev/null 2>&1", cxx);
    if (system(cmd) == 0) {
        snprintf(cmd, sizeof cmd,
                 "%s -std=c++17 -Wall -Wextra -Wpedantic -Werror -Iinclude -fsyntax-only build/check/header.cpp", cxx);
        *problems += run(cmd);
    } else {
        printf("nob check: no C++ compiler; skipping the C++ header check\n");
    }
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
    p = popen("git ls-files -z | xargs -0 grep -lE '/(o)pt/data|/(h)ome/[a-z]|/(m)nt/[a-z]|/(U)sers/' 2>/dev/null", "r");
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
        } else if (a[0] != '-' && strcmp(command, "build") == 0) {
            command = a;
        } else {
            fprintf(stderr, "nob: unknown option %s\n"
                            "usage: ./nob [build|test|check|freestanding|dist|all|clean] "
                            "[--target=native|win64] [--sanitize] [--debug]\n", a);
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
    if (strcmp(command, "all") == 0)
        return cmd_check() || cmd_freestanding() || cmd_test(&t) || cmd_dist(&t);
    if (strcmp(command, "clean") == 0)
        return run("rm -rf build");
    fprintf(stderr, "nob: unknown command %s\n", command);
    return 2;
}
