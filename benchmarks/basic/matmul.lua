local n = tonumber(arg[1]) or 200
local sz = n * n
local a, b, c = {}, {}, {}
for i = 0, sz - 1 do a[i + 1] = i + 1; b[i + 1] = i + 2 end
for i = 0, n - 1 do
  for j = 0, n - 1 do
    local acc = 0
    for k = 0, n - 1 do acc = acc + a[i * n + k + 1] * b[k * n + j + 1] end
    c[i * n + j + 1] = acc
  end
end
local total = 0
for i = 1, sz do total = (total + c[i]) % 1000000007 end
print("matmul " .. n .. " " .. c[1] .. " " .. total)
