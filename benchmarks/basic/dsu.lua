local n = tonumber(arg[1]) or 200000
local p, rk = {}, {}
for i = 0, n - 1 do p[i + 1] = i; rk[i + 1] = 0 end
local function find(x0)
  local r = x0
  while p[r + 1] ~= r do r = p[r + 1] end
  local x = x0
  while x ~= r do local nx = p[x + 1]; p[x + 1] = r; x = nx end
  return r
end
local function unite(a, b)
  local ra, rb = find(a), find(b)
  if ra == rb then return 0 end
  if rk[ra + 1] < rk[rb + 1] then p[ra + 1] = rb
  else
    p[rb + 1] = ra
    if rk[ra + 1] == rk[rb + 1] then rk[ra + 1] = rk[ra + 1] + 1 end
  end
  return 1
end
local function pair(i) return (i * 6364136223846793005 + 1442695040888963407) >> 11 end
local merged, k = 0, 0
while k + 1 < n do
  merged = merged + unite(pair(k) % n, pair(k + 1) % n)
  k = k + 2
end
local roots = 0
for i = 0, n - 1 do roots = (roots + find(i)) % 1000000007 end
print("dsu " .. n .. " " .. merged .. " " .. roots)
