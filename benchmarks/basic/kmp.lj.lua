local n = tonumber(arg[1]) or 4000000
local letter = { [0] = 0, 1, 0, 2 }
local floor = math.floor
local txt = {}
for i = 0, n - 1 do txt[i + 1] = letter[floor((i * 1103515245 + 12345) / 65536) % 4] end
local pat = { 0, 0, 2, 1, 2, 0 }
local m = 6
local pf = { 0 }
local k = 0
for q = 1, m - 1 do
  while k > 0 and pat[k + 1] ~= pat[q + 1] do k = pf[k] end
  if pat[k + 1] == pat[q + 1] then k = k + 1 end
  pf[q + 1] = k
end
local state, hits = 0, 0
for i = 1, n do
  local c = txt[i]
  while state > 0 and pat[state + 1] ~= c do state = pf[state] end
  if pat[state + 1] == c then state = state + 1 end
  if state == m then hits = hits + 1; state = pf[state] end
end
print("kmp " .. n .. " " .. hits)
