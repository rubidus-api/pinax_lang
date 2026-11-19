#!/usr/bin/env python3
"""oracle.py - independent Python references for the programs of this directory (naive or library forms, not line by line):
python3 oracle.py NAME N prints the line the program must print."""
import sys, heapq
M64 = (1 << 64) - 1
def kmp(n):
    lt = [0, 1, 0, 2]
    t = [lt[((i * 1103515245 + 12345) >> 16) & 3] for i in range(n)]
    p = [0, 0, 2, 1, 2, 0]
    hits = sum(1 for i in range(n - 5) if t[i:i + 6] == p)   # the naive count; overlapping hits included, as KMP counts
    return "kmp %d %d" % (n, hits)
def dp(n):
    a = [((i * 2654435761) >> 13) & 3 for i in range(n)]
    b = [((i * 40503 + 7) >> 11) & 3 for i in range(n)]
    d = [[0] * (n + 1) for _ in range(n + 1)]
    for i in range(n + 1): d[i][0] = i
    for j in range(n + 1): d[0][j] = j
    for i in range(1, n + 1):
        for j in range(1, n + 1):
            d[i][j] = min(d[i - 1][j] + 1, d[i][j - 1] + 1, d[i - 1][j - 1] + (a[i - 1] != b[j - 1]))
    return "dp %d %d" % (n, d[n][n])
def graph(n):
    adj = [[(u * 2654435761 + k * 40503 + 1) % n for k in range(8)] for u in range(n)]
    dist = [-1] * n; dist[0] = 0; q = [0]
    for u in q:                                   # the queue grows while it is walked; every node enters once
        for v in adj[u]:
            if dist[v] == -1: dist[v] = dist[u] + 1; q.append(v)
    return "graph %d %d %d" % (n, len(q), sum(d for d in dist if d >= 0))
def sssp(n):
    adj = [[((u * 2654435761 + k * 40503 + 1) % n, 1 + ((((u + k) * 1103515245) >> 20) & 63)) for k in range(6)] for u in range(n)]
    INF = 1 << 60; dist = [INF] * n; dist[0] = 0; h = [(0, 0)]; settled = 0
    while h:
        d, u = heapq.heappop(h)
        if d > dist[u]: continue
        settled += 1
        for v, w in adj[u]:
            if d + w < dist[v]: dist[v] = d + w; heapq.heappush(h, (d + w, v))
    return "sssp %d %d %d" % (n, settled, sum(x for x in dist if x < INF))
def dsu(n):
    par = list(range(n))
    def find(x):
        while par[x] != x: par[x] = par[par[x]]; x = par[x]
        return x
    merged = 0
    for k in range(0, n - 1, 2):
        a = (((k * 6364136223846793005 + 1442695040888963407) & M64) >> 11) % n
        b = ((((k + 1) * 6364136223846793005 + 1442695040888963407) & M64) >> 11) % n
        ra, rb = find(a), find(b)
        if ra != rb: par[ra] = rb; merged += 1
    # the root ids depend on the rank rule, the number of components does not: report the checkable part
    return "dsu %d %d" % (n, merged), n - merged
def sieve(n):
    return "sieve %d %d" % (n, sum(1 for k in range(2, n) if all(k % d for d in range(2, int(k ** .5) + 1))))
def hashf(n):
    return "hash %d %d" % (n, 10 * n * (n + 1) // 2)
def matmul(n):
    a = [[i * n + j + 1 for j in range(n)] for i in range(n)]; b = [[i * n + j + 2 for j in range(n)] for i in range(n)]
    c = [[sum(a[i][k] * b[k][j] for k in range(n)) for j in range(n)] for i in range(n)]
    return "matmul %d %d %d" % (n, c[0][0], sum(x for r in c for x in r) % 1000000007)
def sorts(name, n):
    P = 1000000007; xs = sorted((k * 2654435761) % P for k in range(n))
    return "%s %d True %d" % (name, n, sum(v * (k + 1) for k, v in enumerate(xs)) % P)
def fib(n):
    a, b = 0, 1
    for _ in range(n): a, b = b, a + b
    return "fib %d %d" % (n, a)
name, n = sys.argv[1], int(sys.argv[2])
f = {"kmp": kmp, "dp": dp, "graph": graph, "sssp": sssp, "sieve": sieve, "hash": hashf, "matmul": matmul, "fib": fib}
if name in ("sort", "heap", "msort"): print(sorts(name, n))
elif name == "dsu":
    line, comps = dsu(n); print(line)
else: print(f[name](n))
