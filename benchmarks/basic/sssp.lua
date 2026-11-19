local n = tonumber(arg[1]) or 100000
local adj, wgt, dist, hd, hv = {}, {}, {}, {}, {}
for u = 0, n - 1 do
  for k = 0, 5 do
    adj[u * 6 + k + 1] = (u * 2654435761 + k * 40503 + 1) % n
    wgt[u * 6 + k + 1] = 1 + (((u + k) * 1103515245) >> 20 & 63)
  end
end
local INF = 1 << 60
for i = 1, n do dist[i] = INF end
dist[1] = 0
local hsize = 0
local function hpush(d, v)
  local i = hsize
  hd[i + 1] = d; hv[i + 1] = v
  while i > 0 do
    local p = (i - 1) // 2
    if hd[p + 1] <= hd[i + 1] then break end
    hd[p + 1], hd[i + 1] = hd[i + 1], hd[p + 1]
    hv[p + 1], hv[i + 1] = hv[i + 1], hv[p + 1]
    i = p
  end
  hsize = hsize + 1
end
local function hpop()
  local last = hsize - 1
  hd[1] = hd[last + 1]; hv[1] = hv[last + 1]
  hsize = last
  local i = 0
  while true do
    local c = i * 2 + 1
    if c >= last then break end
    local small = c
    if c + 1 < last and hd[c + 2] < hd[c + 1] then small = c + 1 end
    if hd[i + 1] <= hd[small + 1] then break end
    hd[i + 1], hd[small + 1] = hd[small + 1], hd[i + 1]
    hv[i + 1], hv[small + 1] = hv[small + 1], hv[i + 1]
    i = small
  end
end
hpush(0, 0)
local settled = 0
while hsize > 0 do
  local d, u = hd[1], hv[1]
  hpop()
  if d <= dist[u + 1] then
    settled = settled + 1
    for k = 0, 5 do
      local v = adj[u * 6 + k + 1]
      local nd = d + wgt[u * 6 + k + 1]
      if nd < dist[v + 1] then dist[v + 1] = nd; hpush(nd, v) end
    end
  end
end
local total = 0
for i = 1, n do if dist[i] < INF then total = total + dist[i] end end
print("sssp " .. n .. " " .. settled .. " " .. total)
