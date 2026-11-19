local n = tonumber(arg[1]) or 32
local function fib(k) if k < 2 then return k end return fib(k - 1) + fib(k - 2) end
print("fib " .. n .. " " .. fib(n))
