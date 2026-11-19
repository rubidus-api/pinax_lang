local n = tonumber(arg[1]) or 1500
local a, b, prev, cur = {}, {}, {}, {}
for i = 0, n - 1 do
  a[i + 1] = math.floor(i * 2654435761 / 8192) % 4
  b[i + 1] = math.floor((i * 40503 + 7) / 2048) % 4
end
for j = 0, n do prev[j + 1] = j end
for i = 1, n do
  cur[1] = i
  local ai = a[i]
  for k = 1, n do
    local best = prev[k + 1] + 1
    local ins = cur[k] + 1
    if ins < best then best = ins end
    local sb = prev[k] + (ai ~= b[k] and 1 or 0)
    if sb < best then best = sb end
    cur[k + 1] = best
  end
  for t = 1, n + 1 do prev[t] = cur[t] end
end
print("dp " .. n .. " " .. prev[n + 1])
