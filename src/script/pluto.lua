-- pluto.lua — clean-room subset of the pluto serializer API.
--
-- Behavior contract (from the official pluto doc bundled with the engine and
-- the adv framework usage `pluto.persist({}, obj)` / `pluto.unpersist({}, str)`):
--   pluto.persist(perm, value) -> string   serialized form of value
--   pluto.unpersist(perm, str) -> value    reconstructed value
-- Supported value types: nil, boolean, number, string, and tables
-- (nested, shared references and cycles preserved). Any other type
-- (functions, userdata, threads) serializes as nil.
-- The wire format is Lua source evaluated by loadstring; saves are only
-- ever read back by this same implementation, so the encoding is free.

pluto = {}   -- global: registered by the engine at Lua init

local function is_identifier_name(n)
  return type(n) == "string" and n:match("^t%d+$") ~= nil
end

function pluto.persist(perm, value)
  local ids = {}      -- object -> variable name
  local seq = 0

  -- pass 1: assign a variable name to every reachable table
  local function assign(v)
    if type(v) ~= "table" or ids[v] then return end
    seq = seq + 1
    ids[v] = "t" .. seq
    for k, vv in pairs(v) do
      assign(k)
      assign(vv)
    end
  end
  assign(value)
  if type(value) ~= "table" then
    seq = seq + 1
    ids[value] = "t" .. seq -- non-table roots still get a slot; harmless
  end

  local stmts = {}
  local emitted = {}

  local function emit(v)
    local t = type(v)
    if t == "nil" then return "nil"
    elseif t == "boolean" then return v and "true" or "false"
    elseif t == "number" then return string.format("%.17g", v)
    elseif t == "string" then return string.format("%q", v)
    elseif t == "table" then
      local id = ids[v]
      if emitted[id] then return id end
      emitted[id] = true
      stmts[#stmts + 1] = id .. " = {}"
      for k, vv in pairs(v) do
        stmts[#stmts + 1] = id .. "[" .. emit(k) .. "] = " .. emit(vv)
      end
      return id
    end
    return "nil"
  end

  local root = emit(value)
  return table.concat(stmts, "\n") .. "\nreturn " .. root
end

function pluto.unpersist(perm, str)
  if type(str) ~= "string" or str == "" then return nil end
  local chunk, err = loadstring(str)
  if not chunk then return nil, err end
  return chunk()
end

return pluto
