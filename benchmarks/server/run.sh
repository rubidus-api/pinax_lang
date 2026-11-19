#!/bin/sh
# run.sh - what a script costs per request when it is embedded in a server (ngxsim.c): builds the four models (a handler in C, in
# Pinax, in Lua 5.4 and in LuaJIT) and prints, for a light request (n = 100) and a heavy one (n = 10000), the microseconds a request
# costs and the memory a live request holds. Every model writes the same answers (the checksums are equal).
#
#   sh benchmarks/server/run.sh [REQUESTS]
#
# Needs ./nob build first. The Lua 5.4 and LuaJIT build trees (the directory with the headers and the static library: lua.h and
# liblua.a, luajit.h and libluajit.a) are named by LUA_DIR=... and JIT_DIR=...; the trees under resources/cache are tried otherwise.
# Not nginx: there is no network and no web server, only the call of a script for a request and what it answers.
set -u
root=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
cd "$root"
reqs=${1:-300000}
LUA_DIR=${LUA_DIR:-$(ls -d resources/cache/lua/lua-5.4.*/src 2>/dev/null | tail -1)}
JIT_DIR=${JIT_DIR:-$(ls -d resources/cache/luajit/src-*/src 2>/dev/null | tail -1)}
[ -f build/native/libpinax.a ] || { echo "server: no build/native/libpinax.a; run ./nob build" >&2; exit 2; }
out=build/server
mkdir -p "$out"
CC=${CC:-cc}
f="-std=gnu11 -O2"
$CC $f -DSIM_C benchmarks/server/ngxsim.c -o $out/sim_c || exit 2
$CC $f -DSIM_PINAX -Iinclude -Isrc -Ivendor/proven/include benchmarks/server/ngxsim.c build/native/libpinax.a -lm -pthread -o $out/sim_pinax || exit 2
if [ -f "$LUA_DIR/liblua.a" ]; then $CC $f -DSIM_LUA -I"$LUA_DIR" benchmarks/server/ngxsim.c "$LUA_DIR/liblua.a" -lm -ldl -o $out/sim_lua || exit 2; else echo "server: no Lua 5.4 (LUA_DIR); that model is left out" >&2; fi
if [ -f "$JIT_DIR/libluajit.a" ]; then $CC $f -DSIM_LUAJIT -I"$JIT_DIR" benchmarks/server/ngxsim.c "$JIT_DIR/libluajit.a" -lm -ldl -o $out/sim_luajit || exit 2; else echo "server: no LuaJIT (JIT_DIR); that model is left out" >&2; fi
for n in 100 10000; do
    r=$reqs; [ "$n" = 10000 ] && r=$((reqs / 15))
    echo "== n = $n, $r requests"
    for b in sim_c sim_pinax sim_lua sim_luajit; do [ -x $out/$b ] && $out/$b "$n" "$r"; done
done
