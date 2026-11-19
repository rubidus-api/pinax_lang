// ngxsim.c - what a script costs per request when it is embedded in a server as ngx_lua embeds Lua.
// There is no network and no nginx: the host calls the handler for synthetic requests and counts what it answers, so the cost
// measured is the script's own: the host calls, the parsing, the building of the answer, and the context a request runs in.
//   build with -DSIM_PINAX, -DSIM_LUA (Lua 5.4) or -DSIM_LUAJIT, or -DSIM_C for the bound.
//   ngxsim [N] [REQUESTS]   N is the query's n (default 100), REQUESTS the number of requests (default 200000)
// Prints one line per model: the microseconds a request costs, and a checksum of every answer (all models agree).
#define _POSIX_C_SOURCE 200809L
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static double now(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return (double)t.tv_sec + (double)t.tv_nsec * 1e-9; }

// the synthetic requests: two kinds in a fixed order, so every model answers the same sequence
static const char *g_uri; static size_t g_uri_len; static const char *g_token;
static uint64_t g_sum, g_answers, g_bytes;
static int g_n = 100;
static char uris[4][32];
static void set_request(long i) {
    int k = (int)(i & 3);
    g_uri = uris[k]; g_uri_len = strlen(uris[k]);
    g_token = (k == 3) ? "nope" : "pilot";
}
static void answered(int status, const char *body, size_t len) {
    g_answers++; g_bytes += len; g_sum = g_sum * 31 + (uint64_t)status;
    for (size_t i = 0; i < len; ++i) g_sum = g_sum * 131 + (unsigned char)body[i];
}

#if defined(SIM_C)
static uint64_t sum_mod(long n) { uint64_t s = 0; for (long i = 1; i <= n; ++i) s += (uint64_t)((i * i) % 7); return s; }
static void handle_c(void) {
    if (strcmp(g_token, "pilot") != 0) { answered(403, "{\"error\":\"forbidden\"}", 21); return; }
    const char *q = strchr(g_uri, '?'); long n = -1;
    if (q && strncmp(q + 1, "n=", 2) == 0) { n = 0; const char *d = q + 3; if (*d < '0' || *d > '9') n = -1; else while (*d >= '0' && *d <= '9') n = n * 10 + (*d++ - '0'); }
    if (n < 0 || n > 100000) { answered(400, "{\"error\":\"n\"}", 13); return; }
    char buf[64]; int len = snprintf(buf, sizeof buf, "{\"n\":%ld,\"sum\":%llu}", n, (unsigned long long)sum_mod(n));
    answered(200, buf, (size_t)len);
}
int main(int argc, char **argv) {
    g_n = argc > 1 ? atoi(argv[1]) : 100; long reqs = argc > 2 ? atol(argv[2]) : 200000;
    snprintf(uris[0], 32, "/cpu?n=%d", g_n); snprintf(uris[1], 32, "/cpu?n=%d", g_n); snprintf(uris[2], 32, "/cpu?n=x"); snprintf(uris[3], 32, "/cpu?n=%d", g_n);
    double t = now(); for (long i = 0; i < reqs; ++i) { set_request(i); handle_c(); } t = now() - t;
    printf("%-22s %9.3f us/request  (%llu answers, %llu bytes, checksum %016llx)\n", "c", t / (double)reqs * 1e6, (unsigned long long)g_answers, (unsigned long long)g_bytes, (unsigned long long)g_sum);
    return 0;
}

#elif defined(SIM_PINAX)
#include "pinax.h"
#include <stdarg.h>
static pinax_err w_uri(pinax_call *c, void *u, const pinax_args *a) { (void)c; (void)u; a->rs[0] = (pinax_str){(const unsigned char *)g_uri, g_uri_len}; return PINAX_OK; }
static pinax_err w_header(pinax_call *c, void *u, const pinax_args *a) {
    (void)c; (void)u;
    if (a->s[0].len == 7 && memcmp(a->s[0].ptr, "x-token", 7) == 0) a->rs[0] = (pinax_str){(const unsigned char *)g_token, strlen(g_token)}; else a->rs[0] = (pinax_str){NULL, 0};
    return PINAX_OK;
}
static pinax_err w_respond(pinax_call *c, void *u, const pinax_args *a) { (void)c; (void)u; answered((int)a->i[0], (const char *)a->s[0].ptr, a->s[0].len); return PINAX_OK; }
static const pinax_word_def defs[] = {
    {"uri", "(gives p str)", w_uri, NULL, 0}, {"header", "(name str gives v str)", w_header, NULL, 0}, {"respond", "(status int, body str)", w_respond, NULL, 0}};
static unsigned char block[1 << 22], cblock[1 << 22];
int main(int argc, char **argv) {
    g_n = argc > 1 ? atoi(argv[1]) : 100; long reqs = argc > 2 ? atol(argv[2]) : 200000;
    snprintf(uris[0], 32, "/cpu?n=%d", g_n); snprintf(uris[1], 32, "/cpu?n=%d", g_n); snprintf(uris[2], 32, "/cpu?n=x"); snprintf(uris[3], 32, "/cpu?n=%d", g_n);
    FILE *f = fopen("benchmarks/server/handler.pnx", "rb"); if (!f) { fprintf(stderr, "run from the repository root\n"); return 2; }
    static char src[8192]; size_t n = fread(src, 1, sizeof src, f); fclose(f);
    pinax_config cfg = {.struct_size = sizeof cfg, .budget = 100000000};
    pinax_state *st;
    if (pinax_open((pinax_mem){block, sizeof block}, &cfg, &st) != PINAX_OK || pinax_register(st, "web", defs, 3) != PINAX_OK) return 2;
    if (pinax_load(st, "main", (pinax_str){(const unsigned char *)src, n}, PINAX_LOAD_CODE) != PINAX_OK) { pinax_str w = pinax_error_text(st); fprintf(stderr, "load: %.*s\n", (int)w.len, (const char *)w.ptr); return 2; }
    pinax_word h; if (pinax_find(st, "handle", "()", 0, &h) != PINAX_OK) { fprintf(stderr, "no handle\n"); return 2; }
    const size_t image = pinax_image_size(st);
    // model 1: one state, the handler run for each request (the region of a run is given back at its end)
    g_sum = g_answers = g_bytes = 0;
    double t = now();
    for (long i = 0; i < reqs; ++i) { set_request(i); pinax_args a = {0}; if (pinax_run(st, h, &a) != PINAX_OK) { pinax_str w = pinax_error_text(st); fprintf(stderr, "run: %.*s\n", (int)w.len, (const char *)w.ptr); return 1; } }
    t = now() - t;
    printf("%-22s %9.3f us/request  (%llu answers, %llu bytes, checksum %016llx)\n", "pinax run", t / (double)reqs * 1e6, (unsigned long long)g_answers, (unsigned long long)g_bytes, (unsigned long long)g_sum);
    // model 2: a clone of the loaded state for each request, closed at its end (isolation as strong as a process of its own, no sharing)
    long creqs = reqs / 10; g_sum = g_answers = g_bytes = 0;
    t = now();
    for (long i = 0; i < creqs; ++i) {
        set_request(i); pinax_state *copy;
        if (pinax_clone_with(st, (pinax_mem){cblock, image * 2 + 4096}, PINAX_CLONE_KEEP_HOSTS, &copy) != PINAX_OK) { fprintf(stderr, "clone\n"); return 1; }
        pinax_args a = {0}; if (pinax_run(copy, h, &a) != PINAX_OK) { fprintf(stderr, "clone run\n"); return 1; }
        pinax_close(copy);
    }
    t = now() - t;
    printf("%-22s %9.3f us/request  (%ld requests; image %zu bytes per live request; checksum %016llx)\n", "pinax clone", t / (double)creqs * 1e6, creqs, image, (unsigned long long)g_sum);
    return 0;
}

#else // SIM_LUA / SIM_LUAJIT
#include <lua.h>
#include <lauxlib.h>
#include <lualib.h>
static int l_uri(lua_State *L) { lua_pushlstring(L, g_uri, g_uri_len); return 1; }
static int l_header(lua_State *L) { size_t n; const char *s = luaL_checklstring(L, 1, &n); if (n == 7 && memcmp(s, "x-token", 7) == 0) lua_pushstring(L, g_token); else lua_pushstring(L, ""); return 1; }
static int l_respond(lua_State *L) { size_t n; int st = (int)luaL_checkinteger(L, 1); const char *b = luaL_checklstring(L, 2, &n); answered(st, b, n); return 0; }
static void host(lua_State *L) {
    luaL_openlibs(L); lua_newtable(L);
    lua_pushcfunction(L, l_uri); lua_setfield(L, -2, "uri"); lua_pushcfunction(L, l_header); lua_setfield(L, -2, "header"); lua_pushcfunction(L, l_respond); lua_setfield(L, -2, "respond");
    lua_setglobal(L, "web");
}
int main(int argc, char **argv) {
    g_n = argc > 1 ? atoi(argv[1]) : 100; long reqs = argc > 2 ? atol(argv[2]) : 200000;
    snprintf(uris[0], 32, "/cpu?n=%d", g_n); snprintf(uris[1], 32, "/cpu?n=%d", g_n); snprintf(uris[2], 32, "/cpu?n=x"); snprintf(uris[3], 32, "/cpu?n=%d", g_n);
    lua_State *L = luaL_newstate(); host(L);
    if (luaL_dofile(L, "benchmarks/server/handler.lua")) { fprintf(stderr, "%s\n", lua_tostring(L, -1)); return 2; }
#ifdef SIM_LUAJIT
    const char *name = "luajit";
#else
    const char *name = "lua5.4";
#endif
    // model 1: the handler called in the one state for each request
    g_sum = g_answers = g_bytes = 0; double t = now();
    for (long i = 0; i < reqs; ++i) { set_request(i); lua_getglobal(L, "handle"); if (lua_pcall(L, 0, 0, 0)) { fprintf(stderr, "%s\n", lua_tostring(L, -1)); return 1; } }
    t = now() - t;
    printf("%-22s %9.3f us/request  (%llu answers, %llu bytes, checksum %016llx)\n", name, t / (double)reqs * 1e6, (unsigned long long)g_answers, (unsigned long long)g_bytes, (unsigned long long)g_sum);
    // model 2: a coroutine for each request, as ngx_lua runs a request in its own light thread of the shared state
    g_sum = g_answers = g_bytes = 0; t = now();
    for (long i = 0; i < reqs; ++i) {
        set_request(i); lua_State *co = lua_newthread(L); lua_getglobal(co, "handle");
#if defined(SIM_LUAJIT) || LUA_VERSION_NUM < 504
        int rc = lua_resume(co, 0);
#else
        int nres; int rc = lua_resume(co, L, 0, &nres);
#endif
        if (rc != 0) { fprintf(stderr, "resume: %s\n", lua_tostring(co, -1)); return 1; }
        lua_pop(L, 1);
    }
    t = now() - t;
    printf("%-22s %9.3f us/request  (checksum %016llx)\n", "  + a coroutine each", t / (double)reqs * 1e6, (unsigned long long)g_sum);
    // memory: what a live request holds, a coroutine of the shared state (kilobytes by the collector's count)
    lua_checkstack(L, 1100); lua_gc(L, LUA_GCCOLLECT, 0); int before = lua_gc(L, LUA_GCCOUNT, 0); lua_State *cos[1000];
    for (int i = 0; i < 1000; ++i) { cos[i] = lua_newthread(L); lua_getglobal(cos[i], "handle"); }
    int after = lua_gc(L, LUA_GCCOUNT, 0);
    printf("%-22s %9.3f KB per live coroutine (1000 made and kept)\n", "  memory", (double)(after - before) / 1000.0);
    (void)cos; lua_close(L); return 0;
}
#endif
