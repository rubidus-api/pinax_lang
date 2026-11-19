local n = tonumber(arg[1]) or 200000
local cap = n * 2 + 1
local keys, vals = {}, {}
for i = 1, cap do keys[i] = 0; vals[i] = 0 end
for k = 1, n do
  local slot = (k * 2654435761) % cap
  while true do
    if keys[slot + 1] == 0 then keys[slot + 1] = k; vals[slot + 1] = k * 10; break end
    slot = (slot + 1) % cap
  end
end
local sum = 0
for k = 1, n do
  local slot = (k * 2654435761) % cap
  while true do
    if keys[slot + 1] == k then sum = sum + vals[slot + 1]; break end
    slot = (slot + 1) % cap
  end
end
print("hash " .. n .. " " .. sum)
