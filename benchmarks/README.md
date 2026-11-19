# Benchmarks

Programs that measure Pinax against the engines a Pinax user would otherwise take. Nothing here is part of the product; the numbers
are a record, and the programs are there so that you can measure on your own machine.

- [`basic/`](basic/): twelve simple programs — `fib` `sieve` `sort` `heap` `msort` `kmp` `graph` (breadth-first search) `dsu`
  (union-find) `hash` (open addressing, the file `hopen.pnx`) `dp` (edit distance) `sssp` (Dijkstra with a binary heap) `matmul` —
  each in Pinax (`NAME.pnx`) and in Lua (`NAME.lua`, and `NAME.lj.lua` where LuaJIT needs another form), with the same algorithm,
  the same generator and the same size. `sh benchmarks/basic/run.sh` runs each with `pnx`, through its own exported C, with Lua 5.4
  and with LuaJIT, checks that the four outputs are the same bytes, and prints the fastest of R runs. `oracle.py` gives the line each
  must print from an independent Python version.
- [`server/`](server/): what a script costs per request when it is embedded in a server (the way ngx_lua embeds Lua): a handler (a header
  check, a query, an integer loop, a JSON answer) called for synthetic requests in C, in Pinax (one state; a clone per request), in Lua
  5.4 and in LuaJIT (the handler called; a coroutine per request). No network and no nginx: the cost is the script's own.

Both need `./nob` first, and Lua 5.4 and LuaJIT (named by `LUA=`, `JIT=`; `LUA_DIR=`, `JIT_DIR=` for the server programs). Run them
on a quiet machine.

## A record (2026-11-19, a machine with a load of about 2.5; the fastest of three runs, each the fastest of five)

Seconds:

| Program | n | Pinax | Exported C | Lua 5.4 | LuaJIT | Pinax / Lua 5.4 |
|---|---|---|---|---|---|---|
| fib | 32 | 0.140 | 0.050 | 0.161 | 0.022 | 0.87 |
| sieve | 1000000 | 0.074 | 0.033 | 0.077 | 0.012 | 0.96 |
| sort | 200000 | 0.162 | 0.058 | 0.193 | 0.051 | 0.84 |
| heap | 200000 | 0.243 | 0.095 | 0.339 | 0.062 | 0.72 |
| msort | 200000 | 0.244 | 0.077 | 0.288 | 0.043 | 0.85 |
| kmp | 4000000 | 0.300 | 0.091 | 0.345 | 0.073 | 0.87 |
| graph | 200000 | 0.149 | 0.065 | 0.181 | 0.040 | 0.82 |
| dsu | 200000 | 0.047 | 0.021 | 0.061 | n/a | 0.77 |
| hash | 200000 | 0.029 | 0.017 | 0.063 | 0.009 | 0.46 |
| dp | 1500 | 0.157 | 0.048 | 0.134 | 0.021 | 1.17 |
| sssp | 100000 | 0.334 | 0.135 | 0.531 | 0.083 | 0.63 |
| matmul | 200 | 0.209 | 0.065 | 0.411 | 0.026 | 0.51 |

A ratio below 1 means Pinax takes less time. The geometric mean of Pinax / Lua 5.4 is 0.77 (eleven programs faster, one slower: dp);
the exported C takes 0.29 of Lua 5.4's time and LuaJIT 0.16; Pinax takes 4.8 times LuaJIT's. LuaJIT has doubles only, so dsu (a
64-bit wrapping product) is left out for it. The sizes of sort, heap and msort are larger than the usual 50000 so that a run is long
enough to measure.

**A correction.** The first record of this table (taken on a shared machine with a load of 8 to 25) showed kmp 1.28 and dp 1.26 times
slower than Lua. The Pinax programs were not written like the Lua ones: kmp called a procedure for the letter where Lua read a table of
four, dp read the letter of its row in the inner loop where Lua read it once, sort and heap called a procedure to swap where Lua swapped
in place. They are written alike now, and the programs of each language do the same work.

Microseconds a request costs on the script side, one thread (`server/`):

| Model | n = 100 | n = 10000 | held by a live request |
|---|---|---|---|
| C | 0.13 | 6.7 | none |
| Pinax, one state, the handler run for each request | 1.45 | 118 | the region of the run |
| Pinax, a clone for each request | 8.4 | 130 | an image of 177 KB |
| Lua 5.4, the handler called | 1.99 | 127 | none |
| Lua 5.4, a coroutine for each request | 2.2 | 126 | 0.9 KB |
| LuaJIT, the handler called | 0.50 | 8.3 | none |
| LuaJIT, a coroutine for each request | 0.68 | 9.0 | 0.5 KB |

What the numbers do not say: nginx was not measured; C is not a column of the basic programs; and the machine was shared.
