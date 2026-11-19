local n = tonumber(arg[1]) or 200000
local P = 1000000007
local xs = {}
for k = 0, n - 1 do xs[k + 1] = (k * 2654435761) % P end
local function sift(root, nn)
  while true do
    local child = root * 2 + 1
    if child >= nn then break end
    local big = child
    if child + 1 < nn and xs[child + 1] < xs[child + 2] then big = child + 1 end
    if not (xs[root + 1] < xs[big + 1]) then break end
    xs[root + 1], xs[big + 1] = xs[big + 1], xs[root + 1]
    root = big
  end
end
local i = n // 2
while i > 0 do sift(i - 1, n); i = i - 1 end
local e = n - 1
while e > 0 do xs[1], xs[e + 1] = xs[e + 1], xs[1]; sift(0, e); e = e - 1 end
local check, ordered = 0, true
for k = 0, n - 1 do
  local v = xs[k + 1]
  check = (check + v * (k + 1)) % P
  if k > 0 and xs[k] > v then ordered = false end
end
print("heap " .. n .. " " .. tostring(ordered) .. " " .. check)
