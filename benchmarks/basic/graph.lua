local n = tonumber(arg[1]) or 200000
local adj, dist, que = {}, {}, {}
for u = 0, n - 1 do
  for k = 0, 7 do adj[u * 8 + k + 1] = (u * 2654435761 + k * 40503 + 1) % n end
end
for i = 1, n do dist[i] = -1 end
dist[1] = 0
que[1] = 0
local qh, qt, seen = 0, 1, 1
while qh < qt do
  local u = que[qh + 1]
  qh = qh + 1
  local du = dist[u + 1]
  for k = 0, 7 do
    local v = adj[u * 8 + k + 1]
    if dist[v + 1] == -1 then
      dist[v + 1] = du + 1
      if qt < n then que[qt + 1] = v; qt = qt + 1; seen = seen + 1 end
    end
  end
end
local total = 0
for i = 1, n do local d = dist[i]; if d >= 0 then total = total + d end end
print("graph " .. n .. " " .. seen .. " " .. total)
