local n = tonumber(arg[1]) or 200000
local P = 1000000007
local xs, buf = {}, {}
for k = 0, n - 1 do xs[k + 1] = (k * 2654435761) % P end
local function merge(lo, mid, hi)
  local i, j = lo, mid
  for k = lo, hi - 1 do
    if i >= mid then buf[k + 1] = xs[j + 1]; j = j + 1
    elseif j >= hi then buf[k + 1] = xs[i + 1]; i = i + 1
    elseif xs[i + 1] <= xs[j + 1] then buf[k + 1] = xs[i + 1]; i = i + 1
    else buf[k + 1] = xs[j + 1]; j = j + 1 end
  end
  for m = lo, hi - 1 do xs[m + 1] = buf[m + 1] end
end
local w = 1
while w < n do
  local lo = 0
  while lo < n do
    local mid = math.min(lo + w, n)
    local hi = math.min(lo + 2 * w, n)
    if mid < hi then merge(lo, mid, hi) end
    lo = lo + 2 * w
  end
  w = w * 2
end
local check, ordered = 0, true
for k = 0, n - 1 do
  local v = xs[k + 1]
  check = (check + v * (k + 1)) % P
  if k > 0 and xs[k] > v then ordered = false end
end
print("msort " .. n .. " " .. tostring(ordered) .. " " .. check)
