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

## A record (2026-11-19, a shared machine with a load of 8 to 25: read a ratio of 20 percent or less as noise)

Seconds, the fastest of three runs, each the fastest of five:

| Program | n | Pinax | Exported C | Lua 5.4 | LuaJIT | Pinax / Lua 5.4 |
|---|---|---|---|---|---|---|
| fib | 32 | 0.152 | 0.052 | 0.165 | 0.024 | 0.92 |
| sieve | 1000000 | 0.085 | 0.038 | 0.105 | 0.015 | 0.81 |
| sort | 200000 | 0.211 | 0.093 | 0.218 | 0.054 | 0.97 |
| heap | 200000 | 0.335 | 0.136 | 0.433 | 0.066 | 0.77 |
| msort | 200000 | 0.260 | 0.080 | 0.311 | 0.045 | 0.84 |
| kmp | 4000000 | 0.453 | 0.117 | 0.355 | 0.074 | 1.28 |
| graph | 200000 | 0.157 | 0.067 | 0.193 | 0.042 | 0.81 |
| dsu | 200000 | 0.049 | 0.022 | 0.067 | n/a | 0.73 |
| hash | 200000 | 0.035 | 0.017 | 0.071 | 0.008 | 0.49 |
| dp | 1500 | 0.170 | 0.055 | 0.135 | 0.021 | 1.26 |
| sssp | 100000 | 0.361 | 0.146 | 0.606 | 0.092 | 0.60 |
| matmul | 200 | 0.223 | 0.073 | 0.460 | 0.026 | 0.48 |

A ratio below 1 means Pinax takes less time. The geometric mean of Pinax / Lua 5.4 is 0.79 (ten programs faster, two slower: kmp and
dp); the exported C takes 0.30 of Lua 5.4's time and LuaJIT 0.15; Pinax takes 5.4 times LuaJIT's. LuaJIT has doubles only, so dsu (a
64-bit wrapping product) is left out for it. The sizes of sort, heap and msort are larger than the usual 50000 so that a run is long
enough to measure.

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
