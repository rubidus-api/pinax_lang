-- handler.lua - the same handler for Lua 5.4 and LuaJIT (the host gives the table web: uri, header, respond).
local function sum_mod(n)
  local s = 0
  for i = 1, n do s = s + (i * i) % 7 end
  return s
end
function handle()
  if web.header("x-token") ~= "pilot" then return web.respond(403, '{"error":"forbidden"}') end
  local uri = web.uri()
  local q = uri:match("%?(.*)$") or ""
  local d = q:match("n=(%d*)")
  local n = d and tonumber(d) or -1
  if n < 0 or n > 100000 then return web.respond(400, '{"error":"n"}') end
  return web.respond(200, '{"n":' .. n .. ',"sum":' .. sum_mod(n) .. '}')
end
