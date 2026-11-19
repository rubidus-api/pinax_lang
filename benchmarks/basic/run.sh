#!/bin/sh
# run.sh - twelve simple programs (fib sieve sort heap msort kmp graph dsu hash dp sssp matmul), each written in Pinax and in Lua
# with the same algorithm, generator and size. Each is run by pnx, through its own exported code (pnx export-c, compiled with -O2),
# by Lua 5.4 and by LuaJIT; the four outputs must be the same bytes, and the fastest of R runs is printed in seconds.
#
#   sh benchmarks/basic/run.sh [R] [NAME ...]
#
# Needs ./nob build first. Lua 5.4 and LuaJIT are found as `lua` / `lua5.4` and `luajit` in the PATH, or named by LUA=... and
# JIT=... in the environment (the build tree of the project, if it has them under resources/cache, is tried last). Run it on a quiet
# machine: a shared one moves a ratio by 20 percent. `python3 benchmarks/basic/oracle.py NAME N` gives the line a program must print,
# from an independent Python version, for a small N.
set -u
root=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
cd "$root"
here=benchmarks/basic
runs=${1:-3}
[ $# -gt 0 ] && shift
only="$*"
work=$root/build/basic
rm -rf "$work"
mkdir -p "$work/obj"
pnx=$root/build/native/pnx
find_tool() { # name... : the first of them found in the PATH or built under resources/cache
    for n in "$@"; do command -v "$n" 2>/dev/null && return 0; done
    return 1
}
LUA=${LUA:-$(find_tool lua5.4 lua || ls -d resources/cache/lua/lua-5.4.*/src 2>/dev/null | tail -1 | sed 's|$|/lua|')}
JIT=${JIT:-$(find_tool luajit || ls -d resources/cache/luajit/src-*/src 2>/dev/null | tail -1 | sed 's|$|/luajit|')}
for f in "$pnx" "$LUA" "$JIT"; do [ -n "$f" ] && [ -x "$f" ] || { echo "basic: no executable '$f' (build with ./nob; set LUA= and JIT=)" >&2; exit 2; }; done
CC=${CC:-cc}
inc="-std=c23 -Iinclude -Isrc -Ivendor/proven/include"
for f in src/host/pnx_main.c src/host/pnx_fmt.c; do
    $CC $inc -O2 -DPNX_WITH_NATIVE=pnx_native -c "$f" -o "$work/obj/$(basename "$f" .c).o" || exit 2
done
export PNX_BLOCK_KB=${PNX_BLOCK_KB:-262144}

# the fastest of $runs runs, its output to $1
timed() {
    python3 -c 'import subprocess, sys, time
best = None
for _ in range(int(sys.argv[2])):
    t = time.perf_counter()
    r = subprocess.run(sys.argv[3:], stdout=open(sys.argv[1], "w"), stderr=open(sys.argv[1] + ".err", "w")).returncode
    dt = time.perf_counter() - t
    if r != 0:
        print("exit%d" % r)
        sys.exit()
    best = dt if best is None else min(best, dt)
print("%.3f" % best)' "$@"
}

fail=0
printf '%-8s %8s %9s %9s %9s %9s   %s\n' program n pnx exported lua5.4 luajit "pnx/lua"
for t in "fib 32" "sieve 1000000" "sort 200000" "heap 200000" "msort 200000" "kmp 4000000" "graph 200000" "dsu 200000" \
         "hopen 200000" "dp 1500" "sssp 100000" "matmul 200"; do
    set -- $t
    name=$1 n=$2
    if [ -n "$only" ]; then case " $only " in *" $name "*) ;; *) continue ;; esac; fi
    lf=$name; [ "$name" = hopen ] && lf=hash
    "$pnx" export-c $here/$name.pnx pnx_native >"$work/$name.c" &&
        $CC $inc -O2 -c "$work/$name.c" -o "$work/obj/$name.o" &&
        $CC -O2 -o "$work/$name" "$work/obj/pnx_main.o" "$work/obj/pnx_fmt.o" "$work/obj/$name.o" \
            build/native/libpinax_host.a build/native/libpinax.a -lm -ldl -pthread || { echo "  BUILD $name"; fail=1; continue; }
    a=$(timed "$work/$name.i" "$runs" "$pnx" $here/$name.pnx "$n")
    b=$(PNX_NATIVE_SAY=1 timed "$work/$name.e" "$runs" "$work/$name" $here/$name.pnx "$n")
    c=$(timed "$work/$name.l" "$runs" "$LUA" $here/$lf.lua "$n")
    jf=$here/$lf.lj.lua; [ -f "$jf" ] || jf=$here/$lf.lua
    if [ "$name" = dsu ]; then d=n/a; : >"$work/$name.j"; cp "$work/$name.l" "$work/$name.j" # 64-bit wrapping products: LuaJIT has doubles only
    else d=$(timed "$work/$name.j" "$runs" "$JIT" "$jf" "$n"); fi
    ratio=$(python3 -c "
try: print('%.2f' % (float('$a') / float('$c')))
except Exception: print('-')")
    printf '%-8s %8s %9s %9s %9s %9s   %s\n' "$name" "$n" "$a" "$b" "$c" "$d" "$ratio"
    grep -q '^pnx: [1-9][0-9]* runs through the exported code' "$work/$name.e.err" || { echo "  UNUSED exported code $name"; fail=1; }
    for k in i e j; do cmp -s "$work/$name.$k" "$work/$name.l" || { echo "  DIFFER $k $name"; fail=1; }; done
done
[ $fail = 0 ] && echo "basic: the same bytes in every run" || exit 1
