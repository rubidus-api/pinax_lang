local n = tonumber(arg[1]) or 1000000
local s = {}
for k = 0, n - 1 do s[k + 1] = true end
s[1] = false; s[2] = false
local i = 2
while i * i < n do
  if s[i + 1] then
    local j = i * i
    while j < n do s[j + 1] = false; j = j + i end
  end
  i = i + 1
end
local cnt = 0
for k = 0, n - 1 do if s[k + 1] then cnt = cnt + 1 end end
print("sieve " .. n .. " " .. cnt)
