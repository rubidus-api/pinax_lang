local n = tonumber(arg[1]) or 200000
local P = 1000000007
local xs = {}
for k = 0, n - 1 do xs[k + 1] = (k * 2654435761) % P end
local function qsort(lo, hi)
  while hi - lo > 1 do
    local pivot = xs[hi]
    local i, j = lo, lo
    while j < hi - 1 do
      if xs[j + 1] <= pivot then xs[i + 1], xs[j + 1] = xs[j + 1], xs[i + 1]; i = i + 1 end
      j = j + 1
    end
    xs[i + 1], xs[hi] = xs[hi], xs[i + 1]
    if i - lo <= hi - i - 1 then qsort(lo, i); lo = i + 1 else qsort(i + 1, hi); hi = i end
  end
end
qsort(0, n)
local check, ordered = 0, true
for k = 0, n - 1 do
  local v = xs[k + 1]
  check = (check + v * (k + 1)) % P
  if k > 0 and xs[k] > v then ordered = false end
end
print("sort " .. n .. " " .. tostring(ordered) .. " " .. check)
