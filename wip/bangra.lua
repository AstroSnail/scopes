--[[
Bangra Interpreter
Copyright (c) 2017 Leonard Ritter

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.
--]]

local global_opts = {
    trace_execution = false,
    print_lua_traceback = false
}

--------------------------------------------------------------------------------
-- verify luajit was built with the right flags

do

    local t = setmetatable({count=10}, {
        __len = function(x)
            return x.count
        end})
    -- the flag can be uncommented in luajit/src/Makefile
    assert(#t == 10, "luajit must be built with -DLUAJIT_ENABLE_LUA52COMPAT")
end

--------------------------------------------------------------------------------
-- strict.lua
--------------------------------------------------------------------------------

do
    local mt = getmetatable(_G)
    if mt == nil then
      mt = {}
      setmetatable(_G, mt)
    end

    __STRICT = true
    mt.__declared = {
        global = true
    }

    mt.__newindex = function (t, n, v)
      if __STRICT and not mt.__declared[n] then
        --local w = debug.getinfo(2, "S").what
        --if w ~= "main" and w ~= "C" then
        error("assign to undeclared variable '"..n.."'", 2)
        --end
        mt.__declared[n] = true
      end
      rawset(t, n, v)
    end

    mt.__index = function (t, n)
      if not mt.__declared[n] and debug.getinfo(2, "S").what ~= "C" then
        error("variable '"..n.."' is not declared", 2)
      end
      return rawget(t, n)
    end

    function global(...)
       for _, v in ipairs{...} do mt.__declared[v] = true end
    end
end

--------------------------------------------------------------------------------
-- 30log.lua
--------------------------------------------------------------------------------

local function class()
    local assert, pairs, type, tostring, setmetatable = assert, pairs, type, tostring, setmetatable
    local baseMt, _instances, _classes, _class = {}, setmetatable({},{__mode='k'}), setmetatable({},{__mode='k'})
    local function assert_class(class, method) assert(_classes[class], ('Wrong method call. Expected class:%s.'):format(method)) end
    local function deep_copy(t, dest, aType) t = t or {}; local r = dest or {}
      for k,v in pairs(t) do
        if aType and type(v)==aType then r[k] = v elseif not aType then
          if type(v) == 'table' and k ~= "__index" then r[k] = deep_copy(v) else r[k] = v end
        end
      end; return r
    end
    local function instantiate(self,...)
      assert_class(self, 'new(...) or class(...)'); local instance = {class = self}; _instances[instance] = tostring(instance); setmetatable(instance,self)
      if self.init then if type(self.init) == 'table' then deep_copy(self.init, instance) else self.init(instance, ...) end; end; return instance
    end
    local function extend(self, name, extra_params)
      assert_class(self, 'extend(...)'); local heir = {}; _classes[heir] = tostring(heir); deep_copy(extra_params, deep_copy(self, heir));
      heir.name, heir.__index, heir.super = extra_params and extra_params.name or name, heir, self; return setmetatable(heir,self)
    end
    baseMt = { __call = function (self,...) return self:new(...) end, __tostring = function(self,...)
      if _instances[self] then return ("instance of '%s' (%s)"):format(rawget(self.class,'name') or '?', _instances[self]) end
      return _classes[self] and ("class '%s' (%s)"):format(rawget(self,'name') or '?',_classes[self]) or self
    end}; _classes[baseMt] = tostring(baseMt); setmetatable(baseMt, {__tostring = baseMt.__tostring})
    local class = {isClass = function(class, ofsuper) local isclass = not not _classes[class]; if ofsuper then return isclass and (class.super == ofsuper) end; return isclass end, isInstance = function(instance, ofclass)
        local isinstance = not not _instances[instance]; if ofclass then return isinstance and (instance.class == ofclass) end; return isinstance end}; _class = function(name, attr)
      local c = deep_copy(attr); c.mixins=setmetatable({},{__mode='k'}); _classes[c] = tostring(c); c.name, c.__tostring, c.__call = name or c.name, baseMt.__tostring, baseMt.__call
      c.include = function(self,mixin) assert_class(self, 'include(mixin)'); self.mixins[mixin] = true; return deep_copy(mixin, self, 'function') end
      c.new, c.extend, c.__index, c.includes = instantiate, extend, c, function(self,mixin) assert_class(self,'includes(mixin)') return not not (self.mixins[mixin] or (self.super and self.super:includes(mixin))) end
      c.extends = function(self, class) assert_class(self, 'extends(class)') local super = self; repeat super = super.super until (super == class or super == nil); return class and (super == class) end
        return setmetatable(c, baseMt) end; class._DESCRIPTION = '30 lines library for object orientation in Lua'; class._VERSION = '30log v1.0.0'; class._URL = 'http://github.com/Yonaba/30log'; class._LICENSE = 'MIT LICENSE <http://www.opensource.org/licenses/mit-license.php>'
    return setmetatable(class,{__call = function(_,...) return _class(...) end })
end
class = class()

--------------------------------------------------------------------------------
-- reflect.lua
--------------------------------------------------------------------------------

local function reflect()
    --[[ LuaJIT FFI reflection Library ]]--
    --[[ Copyright (C) 2014 Peter Cawley <lua@corsix.org>. All rights reserved.

    Permission is hereby granted, free of charge, to any person obtaining a copy
    of this software and associated documentation files (the "Software"), to deal
    in the Software without restriction, including without limitation the rights
    to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
    copies of the Software, and to permit persons to whom the Software is
    furnished to do so, subject to the following conditions:

    The above copyright notice and this permission notice shall be included in
    all copies or substantial portions of the Software.

    THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
    IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
    FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL THE
    AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
    LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
    OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
    THE SOFTWARE.
    --]]
    local ffi = require "ffi"
    local bit = require "bit"
    local reflect = {}

    local CTState, init_CTState
    local miscmap, init_miscmap

    local function gc_str(gcref) -- Convert a GCref (to a GCstr) into a string
      if gcref ~= 0 then
        local ts = ffi.cast("uint32_t*", gcref)
        return ffi.string(ts + 4, ts[3])
      end
    end

    local typeinfo = ffi.typeinfo or function(id)
      -- ffi.typeof is present in LuaJIT v2.1 since 8th Oct 2014 (d6ff3afc)
      -- this is an emulation layer for older versions of LuaJIT
      local ctype = (CTState or init_CTState()).tab[id]
      return {
        info = ctype.info,
        size = bit.bnot(ctype.size) ~= 0 and ctype.size,
        sib = ctype.sib ~= 0 and ctype.sib,
        name = gc_str(ctype.name),
      }
    end

    local function memptr(gcobj)
      return tonumber(tostring(gcobj):match"%x*$", 16)
    end

    init_CTState = function()
      -- Relevant minimal definitions from lj_ctype.h
      ffi.cdef [[
        typedef struct CType {
          uint32_t info;
          uint32_t size;
          uint16_t sib;
          uint16_t next;
          uint32_t name;
        } CType;

        typedef struct CTState {
          CType *tab;
          uint32_t top;
          uint32_t sizetab;
          void *L;
          void *g;
          void *finalizer;
          void *miscmap;
        } CTState;
      ]]

      -- Acquire a pointer to this Lua universe's CTState
      local co = coroutine.create(function()end) -- Any live coroutine will do.
      local uint32_ptr = ffi.typeof("uint32_t*")
      local G = ffi.cast(uint32_ptr, ffi.cast(uint32_ptr, memptr(co))[2])
      -- In global_State, `MRef ctype_state` is immediately before `GCRef gcroot[GCROOT_MAX]`.
      -- We first find (an entry in) gcroot by looking for a metamethod name string.
      local anchor = ffi.cast("uint32_t", ffi.cast("const char*", "__index"))
      local i = 0
      while math.abs(tonumber(G[i] - anchor)) > 64 do
        i = i + 1
      end
      -- We then work backwards looking for something resembling ctype_state.
      repeat
        i = i - 1
        CTState = ffi.cast("CTState*", G[i])
      until ffi.cast(uint32_ptr, CTState.g) == G

      return CTState
    end

    init_miscmap = function()
      -- Acquire the CTState's miscmap table as a Lua variable
      local t = {}; t[0] = t
      local tvalue = ffi.cast("uint32_t*", memptr(t))[2]
      ffi.cast("uint32_t*", tvalue)[ffi.abi"le" and 0 or 1] = ffi.cast("uint32_t", ffi.cast("uintptr_t", (CTState or init_CTState()).miscmap))
      miscmap = t[0]
      return miscmap
    end

    -- Information for unpacking a `struct CType`.
    -- One table per CT_* constant, containing:
    -- * A name for that CT_
    -- * Roles of the cid and size fields.
    -- * Whether the sib field is meaningful.
    -- * Zero or more applicable boolean flags.
    local CTs = {[0] =
      {"int",
        "", "size", false,
        {0x08000000, "bool"},
        {0x04000000, "float", "subwhat"},
        {0x02000000, "const"},
        {0x01000000, "volatile"},
        {0x00800000, "unsigned"},
        {0x00400000, "long"},
      },
      {"struct",
        "", "size", true,
        {0x02000000, "const"},
        {0x01000000, "volatile"},
        {0x00800000, "union", "subwhat"},
        {0x00100000, "vla"},
      },
      {"ptr",
        "element_type", "size", false,
        {0x02000000, "const"},
        {0x01000000, "volatile"},
        {0x00800000, "ref", "subwhat"},
      },
      {"array",
        "element_type", "size", false,
        {0x08000000, "vector"},
        {0x04000000, "complex"},
        {0x02000000, "const"},
        {0x01000000, "volatile"},
        {0x00100000, "vla"},
      },
      {"void",
        "", "size", false,
        {0x02000000, "const"},
        {0x01000000, "volatile"},
      },
      {"enum",
        "type", "size", true,
      },
      {"func",
        "return_type", "nargs", true,
        {0x00800000, "vararg"},
        {0x00400000, "sse_reg_params"},
      },
      {"typedef", -- Not seen
        "element_type", "", false,
      },
      {"attrib", -- Only seen internally
        "type", "value", true,
      },
      {"field",
        "type", "offset", true,
      },
      {"bitfield",
        "", "offset", true,
        {0x08000000, "bool"},
        {0x02000000, "const"},
        {0x01000000, "volatile"},
        {0x00800000, "unsigned"},
      },
      {"constant",
        "type", "value", true,
        {0x02000000, "const"},
      },
      {"extern", -- Not seen
        "CID", "", true,
      },
      {"kw", -- Not seen
        "TOK", "size",
      },
    }

    -- Set of CType::cid roles which are a CTypeID.
    local type_keys = {
      element_type = true,
      return_type = true,
      value_type = true,
      type = true,
    }

    -- Create a metatable for each CT.
    local metatables = {
    }
    for _, CT in ipairs(CTs) do
      local what = CT[1]
      local mt = {__index = {}}
      metatables[what] = mt
    end

    -- Logic for merging an attribute CType onto the annotated CType.
    local CTAs = {[0] =
      function(a, refct) error("TODO: CTA_NONE") end,
      function(a, refct) error("TODO: CTA_QUAL") end,
      function(a, refct)
        a = 2^a.value
        refct.alignment = a
        refct.attributes.align = a
      end,
      function(a, refct)
        refct.transparent = true
        refct.attributes.subtype = refct.typeid
      end,
      function(a, refct) refct.sym_name = a.name end,
      function(a, refct) error("TODO: CTA_BAD") end,
    }

    -- C function calling conventions (CTCC_* constants in lj_refct.h)
    local CTCCs = {[0] =
      "cdecl",
      "thiscall",
      "fastcall",
      "stdcall",
    }

    local function refct_from_id(id) -- refct = refct_from_id(CTypeID)
      local ctype = typeinfo(id)
      local CT_code = bit.rshift(ctype.info, 28)
      local CT = CTs[CT_code]
      local what = CT[1]
      local refct = setmetatable({
        what = what,
        typeid = id,
        name = ctype.name,
      }, metatables[what])

      -- Interpret (most of) the CType::info field
      for i = 5, #CT do
        if bit.band(ctype.info, CT[i][1]) ~= 0 then
          if CT[i][3] == "subwhat" then
            refct.what = CT[i][2]
          else
            refct[CT[i][2]] = true
          end
        end
      end
      if CT_code <= 5 then
        refct.alignment = bit.lshift(1, bit.band(bit.rshift(ctype.info, 16), 15))
      elseif what == "func" then
        refct.convention = CTCCs[bit.band(bit.rshift(ctype.info, 16), 3)]
      end

      if CT[2] ~= "" then -- Interpret the CType::cid field
        local k = CT[2]
        local cid = bit.band(ctype.info, 0xffff)
        if type_keys[k] then
          if cid == 0 then
            cid = nil
          else
            cid = refct_from_id(cid)
          end
        end
        refct[k] = cid
      end

      if CT[3] ~= "" then -- Interpret the CType::size field
        local k = CT[3]
        refct[k] = ctype.size or (k == "size" and "none")
      end

      if what == "attrib" then
        -- Merge leading attributes onto the type being decorated.
        local CTA = CTAs[bit.band(bit.rshift(ctype.info, 16), 0xff)]
        if refct.type then
          local ct = refct.type
          ct.attributes = {}
          CTA(refct, ct)
          ct.typeid = refct.typeid
          refct = ct
        else
          refct.CTA = CTA
        end
      elseif what == "bitfield" then
        -- Decode extra bitfield fields, and make it look like a normal field.
        refct.offset = refct.offset + bit.band(ctype.info, 127) / 8
        refct.size = bit.band(bit.rshift(ctype.info, 8), 127) / 8
        refct.type = {
          what = "int",
          bool = refct.bool,
          const = refct.const,
          volatile = refct.volatile,
          unsigned = refct.unsigned,
          size = bit.band(bit.rshift(ctype.info, 16), 127),
        }
        refct.bool, refct.const, refct.volatile, refct.unsigned = nil
      end

      if CT[4] then -- Merge sibling attributes onto this type.
        while ctype.sib do
          local entry = typeinfo(ctype.sib)
          if CTs[bit.rshift(entry.info, 28)][1] ~= "attrib" then break end
          if bit.band(entry.info, 0xffff) ~= 0 then break end
          local sib = refct_from_id(ctype.sib)
          sib:CTA(refct)
          ctype = entry
        end
      end

      return refct
    end

    local function sib_iter(s, refct)
      repeat
        local ctype = typeinfo(refct.typeid)
        if not ctype.sib then return end
        refct = refct_from_id(ctype.sib)
      until refct.what ~= "attrib" -- Pure attribs are skipped.
      return refct
    end

    local function siblings(refct)
      -- Follow to the end of the attrib chain, if any.
      while refct.attributes do
        refct = refct_from_id(refct.attributes.subtype or typeinfo(refct.typeid).sib)
      end

      return sib_iter, nil, refct
    end

    metatables.struct.__index.members = siblings
    metatables.func.__index.arguments = siblings
    metatables.enum.__index.values = siblings

    local function find_sibling(refct, name)
      local num = tonumber(name)
      if num then
        for sib in siblings(refct) do
          if num == 1 then
            return sib
          end
          num = num - 1
        end
      else
        for sib in siblings(refct) do
          if sib.name == name then
            return sib
          end
        end
      end
    end

    metatables.struct.__index.member = find_sibling
    metatables.func.__index.argument = find_sibling
    metatables.enum.__index.value = find_sibling

    function reflect.typeof(x) -- refct = reflect.typeof(ct)
      return refct_from_id(tonumber(ffi.typeof(x)))
    end

    function reflect.getmetatable(x) -- mt = reflect.getmetatable(ct)
      return (miscmap or init_miscmap())[-tonumber(ffi.typeof(x))]
    end

    return reflect
end
reflect = reflect()

--------------------------------------------------------------------------------
-- IMPORTS
--------------------------------------------------------------------------------

local WIN32 = (jit.os == "Windows")

local ord = string.byte
local tochar = string.char
local format = string.format
local substr = string.sub
local null = nil

local traceback = debug.traceback

local lshift = bit.lshift
local rshift = bit.rshift
local band = bit.band

local ffi = require 'ffi'
local typeof = ffi.typeof
local istype = ffi.istype
local new = ffi.new
local cdef = ffi.cdef
local C = ffi.C
local cstr = ffi.string
local cast = ffi.cast
local copy = ffi.copy

local void = typeof('void')
local voidp = typeof('$ *', void)
local voidpp = typeof('$ *', voidp)

local int8_t = typeof('int8_t')
local int16_t = typeof('int16_t')
local int32_t = typeof('int32_t')
local int64_t = typeof('int64_t')

local uint8_t = typeof('uint8_t')
local uint16_t = typeof('uint16_t')
local uint32_t = typeof('uint32_t')
local uint64_t = typeof('uint64_t')

local int = typeof('int')
local bool = typeof('bool')

local float = typeof('float')
local double = typeof('double')

local size_t = typeof('size_t')

local p_int8_t = typeof('$ *', int8_t)
local vla_int8_t = typeof('$[?]', int8_t)

local rawstring = typeof('const char *')

-- import embedded header
cdef[[
typedef union {
    uintptr_t uintptr;
    void *ptr;
} cast_t;

unsigned char wip_bangra_lua_h[];
unsigned int wip_bangra_lua_h_len;
]]

local cast_t = typeof('cast_t')

local MAP_FAILED = new(cast_t, -1).ptr
local NULL = new(cast_t, 0).ptr

local typeid_char = reflect.typeof(int8_t).typeid
local function is_char_array_ctype(refct)
    return refct.what == 'array' and refct.element_type.typeid == typeid_char
end

local function zstr_from_buffer(ptr, size)
    local s = new(typeof('$[$]', int8_t, size + 1))
    copy(s, ptr, size)
    s[size] = 0
    return s
end

do
    cdef(cstr(zstr_from_buffer(C.wip_bangra_lua_h, C.wip_bangra_lua_h_len)))
end

local off_t = typeof('__off_t')

local function stderr_writer(x)
    C.fputs(x, C.stderr)
end

local function stdout_writer(x)
    C.fputs(x, C.stdout)
end

local function string_writer(s)
    s = s or ""
    return function (x)
        if x == null then
            return s
        else
            s = s .. x
        end
    end
end

local function min(a,b)
    if (a < b) then
        return a
    else
        return b
    end
end

local function max(a,b)
    if (a > b) then
        return a
    else
        return b
    end
end

local function make_get_enum_name(T)
    local revtable ={}
    for k,v in pairs(T) do
        revtable[v] = k
    end
    return function (k)
        return revtable[k]
    end
end

local function set(keys)
    local r = {}
    for i,k in ipairs(keys) do
        r[k] = i
    end
    return r
end

local function update(a, b)
    for k,v in pairs(b) do
        a[k] = v
    end
end

local function split(str)
    local result = {}
    for s in string.gmatch(str, "%S+") do
      table.insert(result, s)
    end
    return result
end

local function cformat(fmt, ...)
    local size = C.stb_snprintf(null, 0, fmt, ...)
    local s = vla_int8_t(size + 1)
    C.stb_snprintf(s, size + 1, fmt, ...)
    return cstr(s)
end

local function escape_string(s, quote_chars)
    local len = #s
    local size = C.escape_string(null, s, len, quote_chars)
    local es = vla_int8_t(size + 1)
    C.escape_string(es, s, len, quote_chars)
    return cstr(es)
end

local function endswith(str,tail)
   return tail=='' or substr(str,-(#tail))==tail
end

local repr
local function assert_luatype(ltype, x)
    if type(x) == ltype then
        return x
    else
        error(ltype .. " expected, got " .. repr(x))
    end
end

local function assert_number(x) assert_luatype("number", x) end
local function assert_string(x) assert_luatype("string", x) end
local function assert_table(x) assert_luatype("table", x) end
local function assert_boolean(x) assert_luatype("boolean", x) end
local function assert_cdata(x) assert_luatype("cdata", x) end
local function assert_function(x) assert_luatype("function", x) end

local function safecall(f1, f2)
    local function fwd_xpcall_dest(result, ...)
        if result then
            return ...
        else
            return f2(...)
        end
    end
    return fwd_xpcall_dest(xpcall(f1, function(err) return err end))
end

local function protect(obj)
    local mt = getmetatable(obj)
    assert(mt)
    assert(mt.__index == null)
    function mt.__index(cls, name)
        error("no such attribute in "
            .. tostring(cls)
            .. ": " .. tostring(name))
    end
end

--------------------------------------------------------------------------------
-- ANSI COLOR FORMATTING
--------------------------------------------------------------------------------

local ANSI = {
RESET           = "\027[0m",
COLOR_BLACK     = "\027[30m",
COLOR_RED       = "\027[31m",
COLOR_GREEN     = "\027[32m",
COLOR_YELLOW    = "\027[33m",
COLOR_BLUE      = "\027[34m",
COLOR_MAGENTA   = "\027[35m",
COLOR_CYAN      = "\027[36m",
COLOR_GRAY60    = "\027[37m",

COLOR_GRAY30    = "\027[30;1m",
COLOR_XRED      = "\027[31;1m",
COLOR_XGREEN    = "\027[32;1m",
COLOR_XYELLOW   = "\027[33;1m",
COLOR_XBLUE     = "\027[34;1m",
COLOR_XMAGENTA  = "\027[35;1m",
COLOR_XCYAN     = "\027[36;1m",
COLOR_WHITE     = "\027[37;1m",
COLOR_RGB       = function(hexcode, isbg)
    local r = band(rshift(hexcode, 16), 0xff)
    local g = band(rshift(hexcode, 8), 0xff)
    local b = band(hexcode, 0xff)
    local ctrlcode
    if isbg then
        ctrlcode = "\027[48;2;"
    else
        ctrlcode = "\027[38;2;"
    end
    return ctrlcode
        .. tostring(r) .. ";"
        .. tostring(g) .. ";"
        .. tostring(b) .. "m"
end
}

local SUPPORT_ISO_8613_3 = not WIN32
local STYLE
if SUPPORT_ISO_8613_3 then
local BG = ANSI.COLOR_RGB(0x2D2D2D, true)
STYLE = {
FOREGROUND = ANSI.COLOR_RGB(0xCCCCCC),
BACKGROUND = ANSI.COLOR_RGB(0x2D2D2D, true),
SYMBOL = ANSI.COLOR_RGB(0xCCCCCC),
STRING = ANSI.COLOR_RGB(0xCC99CC),
NUMBER = ANSI.COLOR_RGB(0x99CC99),
KEYWORD = ANSI.COLOR_RGB(0x6699CC),
FUNCTION = ANSI.COLOR_RGB(0xFFCC66),
SFXFUNCTION = ANSI.COLOR_RGB(0xCC6666),
OPERATOR = ANSI.COLOR_RGB(0x66CCCC),
INSTRUCTION = ANSI.COLOR_YELLOW,
TYPE = ANSI.COLOR_RGB(0xF99157),
COMMENT = ANSI.COLOR_RGB(0x999999),
ERROR = ANSI.COLOR_XRED,
LOCATION = ANSI.COLOR_RGB(0x999999),
}
else
STYLE = {
FOREGROUND = ANSI.COLOR_WHITE,
BACKGROUND = ANSI.RESET,
STRING = ANSI.COLOR_XMAGENTA,
NUMBER = ANSI.COLOR_XGREEN,
KEYWORD = ANSI.COLOR_XBLUE,
FUNCTION = ANSI.COLOR_GREEN,
SFXFUNCTION = ANSI.COLOR_RED,
OPERATOR = ANSI.COLOR_XCYAN,
INSTRUCTION = ANSI.COLOR_YELLOW,
TYPE = ANSI.COLOR_XYELLOW,
COMMENT = ANSI.COLOR_GRAY30,
ERROR = ANSI.COLOR_XRED,
LOCATION = ANSI.COLOR_GRAY30,
}
end

local is_tty = (C.isatty(C.fileno(C.stdout)) == 1)
local support_ansi = is_tty
local ansi
if is_tty then
    local reset = ANSI.RESET
    ansi = function(style, x) return style .. x .. reset end
else
    ansi = function(style, x) return x end
end

repr = function(x)
    local visited = {}
    local function _repr(x, maxd)
        if type(x) == "table" then
            if visited[x] then
                maxd = 0
            end
            local mt = getmetatable(x)
            if mt and mt.__tostring then
                return mt.__tostring(x)
            end
            visited[x] = x
            local s = ansi(STYLE.OPERATOR,"{")
            if maxd <= 0 then
                s = s .. ansi(STYLE.COMMENT, "...")
            else
                local n = ''
                for k,v in pairs(x) do
                    if n ~= '' then
                        n = n .. ansi(STYLE.OPERATOR,",")
                    end
                    k = _repr(k, maxd - 1)
                    n = n .. k .. ansi(STYLE.OPERATOR, "=") .. _repr(v, maxd - 1)
                end
                if mt then
                    if n ~= '' then
                        n = n .. ansi(STYLE.OPERATOR,",")
                    end
                    if mt.__class then
                        n = n .. ansi(STYLE.KEYWORD, "class")
                            .. ansi(STYLE.OPERATOR, "=")
                            .. tostring(mt.__class)
                    else
                        n = n .. ansi(STYLE.KEYWORD, "meta")
                            .. ansi(STYLE.OPERATOR, "=")
                            .. _repr(mt, maxd - 1)
                    end
                end
                s = s .. n
            end
            s = s .. ansi(STYLE.OPERATOR,"}")
            return s
        elseif type(x) == "number" then
            return ansi(STYLE.NUMBER, tostring(x))
        elseif type(x) == "boolean" then
            return ansi(STYLE.KEYWORD, tostring(x))
        elseif type(x) == "string" then
            return ansi(STYLE.STRING, format("%q", x))
        elseif type(x) == "nil" then
            return ansi(STYLE.KEYWORD, "null")
        end
        return tostring(x)
    end
    return _repr(x, 10)
end

--------------------------------------------------------------------------------
--
--------------------------------------------------------------------------------

local builtins = {}
local builtin_ops = {}

--------------------------------------------------------------------------------
-- SYMBOL
--------------------------------------------------------------------------------

local SYMBOL_ESCAPE_CHARS = "[]{}()\""

local Symbol = {__class="Symbol"}
local function assert_symbol(x)
    if getmetatable(x) == Symbol then
        return x
    else
        error("symbol expected, got " .. repr(x))
    end
end
do
    Symbol.__index = Symbol

    local next_symbol_id = 0
    local name_symbol_map = {}
    local cls = Symbol
    setmetatable(Symbol, {
        __call = function(cls, name)
            assert_string(name)
            local sym = name_symbol_map[name]
            if (sym == null) then
                sym = setmetatable({name=name, index=next_symbol_id},Symbol)
                next_symbol_id = next_symbol_id + 1
                name_symbol_map[name] = sym
            end
            return sym
        end
    })
    function cls:__tostring()
        return ansi(STYLE.SYMBOL, self.name)
    end
end

local function define_symbols(def)
    def({Unnamed=''})
    def({ContinuationForm='form:fn/cc'})

    def({ListWildcard='#list'})
    def({SymbolWildcard='#symbol'})

    def({Compare='compare'})
    def({CountOf='countof'})
    def({Slice='slice'})
    def({Cast='cast'})
    def({Size='size'})
    def({Alignment='alignment'})
    def({Unsigned='unsigned'})
    def({Bitwidth='bitwidth'})
    def({Super='super'})
    def({At='@'})
    def({ApplyType='apply-type'})
    def({ElementType="element-type"})
    def({Join='..'})
    def({Add='+'})
    def({Sub='-'})
    def({Mul='*'})
    def({Div='/'})

    -- ad-hoc builtin names
    def({ExecuteReturn='execute-return'})
    def({RCompare='rcompare'})
    def({SliceForwarder='slice-forwarder'})
    def({JoinForwarder='join-forwarder'})
    def({CompareListNext="compare-list-next"})

end

do
    define_symbols(function(kv)
        local key, value = next(kv)
        Symbol[key] = Symbol(value)
    end)
end

--------------------------------------------------------------------------------
-- ANCHOR
--------------------------------------------------------------------------------

local Anchor = class("Anchor")
local function assert_anchor(x)
    if getmetatable(x) == Anchor then
        return x
    else
        error("expected anchor, got " .. repr(x))
    end
end
do
    local cls = Anchor
    function cls:init(path, lineno, column, offset)
        assert_string(path)
        assert_number(lineno)
        assert_number(column)
        offset = offset or 0
        assert_number(offset)
        self.path = path
        self.lineno = lineno
        self.column = column
        self.offset = offset
    end
    -- defined elsewhere:
    -- function cls.stream_source_line
    function cls:format_plain()
        return self.path
            .. ':' .. format("%i", self.lineno)
            .. ':' .. format("%i", self.column)
    end
    function cls:stream_message_with_source(writer, msg)
        writer(ansi(STYLE.LOCATION, self:format_plain() .. ":"))
        writer(" ")
        writer(msg)
        writer("\n")
        self:stream_source_line(writer)
    end
    function cls:__tostring()
        return ansi(STYLE.LOCATION, self:format_plain())
    end
end

--------------------------------------------------------------------------------
-- SCOPES
--------------------------------------------------------------------------------

local Any = {}

local function assert_any(x)
    if getmetatable(x) == Any then
        return x
    else
        error("any expected, got " .. tostring(x))
    end
end

local function quote_error(msg)
    if type(msg) == "string" then
        msg = {msg = msg, quoted = true}
    end
    error(msg)
end

local set_active_anchor
local get_active_anchor
do
    local _active_anchor

    set_active_anchor = function(anchor)
        if anchor ~= null then
            assert_anchor(anchor)
        end
        _active_anchor = anchor
    end
    get_active_anchor = function()
        return _active_anchor
    end
end

local function with_anchor(anchor, f)
    local _anchor = get_active_anchor()
    set_active_anchor(anchor)
    f()
    set_active_anchor(_anchor)
end

local function location_error(msg)
    if type(msg) == "string" then
        msg = {msg = msg, anchor = get_active_anchor()}
    end
    error(msg)
end

local function unwrap(_type, value)
    assert_any(value)
    if (value.type == _type) then
        return value.value
    else
        location_error("type "
            .. _type.displayname
            .. " expected, got "
            .. value.type.displayname
            )
    end
end

local unsyntax
local Type = {}

local Scope = class("Scope")
local function assert_scope(x)
    if getmetatable(x) == Scope then
        return x
    else
        error("scope expected, not " .. repr(x))
    end
end
do
    local cls = Scope
    function cls:init(scope)
        -- symbol -> any
        self.symbols = {}
        if scope ~= null then
            assert_scope(scope)
        end
        self.parent = scope
    end
    function cls:__tostring()
        local count = 0
        for k,v in pairs(self.symbols) do
            count = count + 1
        end
        return
            ansi(STYLE.KEYWORD, "scope")
            .. ansi(STYLE.COMMENT, "<")
            .. format("%i symbols", count)
            .. ansi(STYLE.COMMENT, ">")
    end
    function cls:bind(sxname, value)
        local name = unwrap(Type.Symbol, unsyntax(sxname))
        assert_any(value)
        assert(self.symbols[name] == null)
        self.symbols[name] = { sxname, value }
    end
    function cls:lookup(name)
        assert_symbol(name)
        local entry = self.symbols[name]
        if entry then
            return entry[2]
        end
        if self.parent then
            return self.parent:lookup(name)
        end
    end

end

--------------------------------------------------------------------------------
-- TYPE
--------------------------------------------------------------------------------

local is_none
local format_any_value

local function assert_type(x)
    if getmetatable(x) == Type then
        return x
    else
        error("type expected, got " .. repr(x))
    end
end
local function define_types(def)
    def('Void')
    def('Any')
    def('Type')

    def('Bool')

    def('Integer')
    def('Real')

    def('I8')
    def('I16')
    def('I32')
    def('I64')

    def('U8')
    def('U16')
    def('U32')
    def('U64')

    def('R32')
    def('R64')

    def('Builtin')

    def('Scope')

    def('Symbol')
    def('List')
    def('String')

    def('Form')
    def('Parameter')
    def('Flow')
    def('Table')

    def('Closure')
    def('Frame')
    def('Anchor')
end

do
    Type.__index = Type
    local idx = 0

    local cls = Type
    setmetatable(Type, {
        __call = function(cls, name, displayname)
            local k = idx
            idx = idx + 1
            return setmetatable({
                name = name,
                displayname = displayname or ansi(STYLE.TYPE, name),
                index = idx,
                scope = Scope()
            }, Type)
        end
    })
    protect(cls)
    function cls:bind(name, value)
        return self.scope:bind(name, value)
    end
    function cls:lookup(name)
        local value = self.scope:lookup(name)
        if value ~= null then
            return value
        end
        local super = self.scope:lookup(Symbol.Super)
        if super ~= null then
            return super.value:lookup(name)
        end
    end
    function cls:super()
        local super = self:lookup(Symbol.Super)
        return super and unwrap(Type.Type, super)
    end
    function cls:set_super(_type)
        assert_type(_type)
        self:bind(Any(Symbol.Super), Any(_type))
    end
    function cls:element_type()
        local et = self:lookup(Symbol.ElementType)
        return et and unwrap(Type.Type, et)
    end
    function cls:set_element_type(_type)
        assert_type(_type)
        self:bind(Any(Symbol.ElementType), Any(_type))
    end
    function cls:__call(...)
        return self:__call(...)
    end

    function cls:__tostring()
        return self.displayname
    end

    define_types(function(name)
        cls[name] = Type(string.lower(name))
    end)

    local function make_qualifier_type(name, on_create)
        local cls = Type(name)
        local cache = {}
        function cls:__call(_type)
            assert_type(_type)
            local val = cache[_type]
            if not val then
                val = Type(
                    self.name .. "[" .. _type.name .. "]",
                    self.displayname
                    .. ansi(STYLE.OPERATOR, "[")
                    .. _type.displayname
                    .. ansi(STYLE.OPERATOR, "]"))
                val:set_element_type(_type)
                val:set_super(self)
                function val:format_value(value)
                    return format_any_value(self:element_type(), value)
                end
                cache[_type] = val
                if on_create then
                    on_create(val)
                end
            end
            return val
        end
        return cls
    end
    Type.Macro = make_qualifier_type("macro")
    Type.Quote = make_qualifier_type("quote")
    Type.Syntax = make_qualifier_type("syntax", function(val)
        function val:format_value(value)
            return
                ansi(STYLE.LOCATION,
                    tostring(value.anchor:format_plain()) .. ":")
                .. format_any_value(
                    self:element_type(),
                    value.datum)
        end
    end)
    Type.SizeT = Type.U64
end

local function is_quote_type(_type)
    assert_type(_type)
    return _type:super() == Type.Quote
end

local function is_macro_type(_type)
    assert_type(_type)
    return _type:super() == Type.Macro
end

local function each_numerical_type(f, opts)
    if opts == null then
        opts = {
            floats = true,
            ints = true,
        }
    end
    if opts.ints then
        opts.signed = true
        opts.unsigned = true
    end
    if opts.floats then
        f(Type.R32)
        f(Type.R64)
    end
    if opts.signed then
        f(Type.I8)
        f(Type.I16)
        f(Type.I32)
        f(Type.I64)
    end
    if opts.unsigned then
        f(Type.U8)
        f(Type.U16)
        f(Type.U32)
        f(Type.U64)
    end
end

--------------------------------------------------------------------------------
-- ANY
--------------------------------------------------------------------------------

local MT_TYPE_MAP = {
    [Symbol] = Type.Symbol,
    [Type] = Type.Type,
    [Scope] = Type.Scope,
    [Anchor] = Type.Anchor
}

format_any_value = function(_type, x)
    if rawget(_type, 'format_value') then
        return _type:format_value(x)
    else
        return repr(x)
    end
end

do
    local function wrap(value)
        local t = type(value)
        if t == 'table' then
            local mt = getmetatable(value)
            if mt == null then
                return Type.Table, value
            else
                local ty = MT_TYPE_MAP[mt]
                if ty ~= null then
                    return ty, value
                end
            end
        elseif t == 'string' then
            return Type.String, value
        elseif t == 'cdata' then
            local ct = typeof(value)
            if bool == ct then
                return Type.Bool, value
            elseif int8_t == ct then
                return Type.I8, value
            elseif int16_t == ct then
                return Type.I16, value
            elseif int32_t == ct then
                return Type.I32, value
            elseif int64_t == ct then
                return Type.I64, value
            elseif uint8_t == ct then
                return Type.U8, value
            elseif uint16_t == ct then
                return Type.U16, value
            elseif uint32_t == ct then
                return Type.U32, value
            elseif uint64_t == ct then
                return Type.U64, value
            elseif float == ct then
                return Type.R32, value
            elseif double == ct then
                return Type.R64, value
            end
            local refct = reflect.typeof(value)
            if is_char_array_ctype(refct) then
                return Type.String, cstr(value)
            end
        end
        error("unable to wrap " .. repr(value))
    end

    setmetatable(Any, {
        __call = function(cls, arg1, arg2)
            local _type = arg1
            local value = arg2
            if type(arg2) == "nil" then
                -- wrap syntax
                _type, value = wrap(arg1)
            else
                local _type = arg1
                local value = arg2
            end
            assert_type(_type)
            return setmetatable({
                type = _type,
                value = value
            }, cls)
        end
    })
end
function Any.__tostring(self)
    if getmetatable(self.type) ~= Type then
        return ansi(STYLE.ERROR, "corrupted value")
    else
        return format_any_value(self.type, self.value)
            .. ansi(STYLE.OPERATOR, ":")
            .. self.type.displayname
    end
end

local function assert_any_type(_type, value)
    assert_any(value)
    if (value.type == _type) then
        return value.value
    else
        error("type "
            .. _type.displayname
            .. " expected, got "
            .. value.type.displayname
            )
    end
end

local none = Any(Type.Void, NULL)
is_none = function(value)
    return value.type == Type.Void
end
local is_null_or_none = function(value)
    return value == null or value.type == Type.Void
end

--------------------------------------------------------------------------------
-- SYNTAX OBJECTS
--------------------------------------------------------------------------------

local function qualify(qualifier_type, x)
    assert_any(x)
    return Any(qualifier_type(x.type), x.value)
end

local function unqualify(qualifier_type, x)
    assert_any(x)
    if (x.type:super() ~= qualifier_type) then
        location_error("attempting to unqualify type "
            .. repr(qualifier_type)
            .. " from unrelated type "
            .. repr(x.type))
    end
    return Any(x.type:element_type(), x.value)
end

local function quote(x) return qualify(Type.Quote, x) end
local function unquote(x) return unqualify(Type.Quote, x) end

local function macro(x) return qualify(Type.Macro, x) end
local function unmacro(x) return unqualify(Type.Macro, x) end

local function is_syntax_type(_type)
    assert_type(_type)
    return _type:super() == Type.Syntax
end

local function assert_syntax(x)
    assert_any(x)
    if is_syntax_type(x.type) then
        return x
    else
        location_error("expected syntax, got " .. repr(x))
    end
end

local syntax
do
    local Syntax = {}
    do
        Syntax.__index = Syntax
        local cls = Syntax
        function cls:__len()
            return #self.datum
        end
        function cls:__tostring()
            return
                ansi(STYLE.COMMENT,
                    self.anchor:format_plain()
                    .. ":")
                .. tostring(self.datum)
        end
    end

    syntax = function(x, anchor)
        assert_any(x)
        assert(not is_syntax_type(x.type))
        assert_anchor(anchor)
        x = qualify(Type.Syntax, x)
        x.value = setmetatable({
            datum = x.value,
            anchor = anchor
        }, Syntax)
        return x
    end
    unsyntax = function(x)
        assert_any(x)
        if is_syntax_type(x.type) then
            local anchor = x.value.anchor
            x = unqualify(Type.Syntax, x)
            x.value = x.value.datum
            return x, anchor
        else
            return x
        end
    end
end

Type.SyntaxList = Type.Syntax(Type.List)
Type.SyntaxSymbol = Type.Syntax(Type.Symbol)

--------------------------------------------------------------------------------
-- S-EXPR LEXER / TOKENIZER
--------------------------------------------------------------------------------

local Token = {
    none = -1,
    eof = 0,
    open = ord('('),
    close = ord(')'),
    square_open = ord('['),
    square_close = ord(']'),
    curly_open = ord('{'),
    curly_close = ord('}'),
    string = ord('"'),
    symbol = ord('S'),
    escape = ord('\\'),
    statement = ord(';'),
    number = ord('N'),
}

local get_token_name = make_get_enum_name(Token)

local token_terminators = new(rawstring, "()[]{}\"';#")

local Lexer = {}
do
    Lexer.__index = Lexer

    local TAB = ord('\t')
    local CR = ord('\n')
    local BS = ord('\\')

    local function verify_good_taste(c)
        if (c == TAB) then
            location_error("please use spaces instead of tabs.")
        end
    end

    local cls = Lexer
    function cls.init(input_stream, eof, path, offset)
        offset = offset or 0
        eof = eof or (input_stream + C.strlen(input_stream))

        local self = setmetatable({}, Lexer)
        self.base_offset = offset
        self.path = path
        self.input_stream = input_stream
        self.eof = eof
        self.next_cursor = input_stream
        self.next_lineno = 1
        self.next_line = input_stream
        return self
    end
    function cls:offset()
        return self.base_offset + (self.cursor - self.input_stream)
    end
    function cls:column()
        return self.cursor - self.line + 1
    end
    function cls:anchor()
        return Anchor(self.path, self.lineno, self:column(), self:offset())
    end
    function cls:next()
        local c = self.next_cursor[0]
        self.next_cursor = self.next_cursor + 1
        return c
    end
    function cls:is_eof()
        return self.next_cursor == self.eof
    end
    function cls:newline()
        self.next_lineno = self.next_lineno + 1
        self.next_line = self.next_cursor
    end
    function cls:read_symbol()
        local escape = false
        while (true) do
            if (self:is_eof()) then
                break
            end
            local c = self:next()
            if (escape) then
                if (c == CR) then
                    self:newline()
                end
                escape = false
            elseif (c == BS) then
                escape = true
            elseif (0 ~= C.isspace(c)) or (NULL ~= C.strchr(token_terminators, c)) then
                self.next_cursor = self.next_cursor - 1
                break
            end
        end
        self.string = self.cursor
        self.string_len = self.next_cursor - self.cursor
    end
    function cls:read_string(terminator)
        local escape = false
        while (true) do
            if (self:is_eof()) then
                location_error("unterminated sequence")
                break
            end
            local c = self:next()
            if (c == CR) then
                self:newline()
            end
            if (escape) then
                escape = false
            elseif (c == BS) then
                escape = true
            elseif (c == terminator) then
                break
            end
        end
        self.string = self.cursor
        self.string_len = self.next_cursor - self.cursor
    end
    local pp_int8_t = typeof('$*[1]', int8_t)
    local function make_read_number(srctype, f)
        local atype = typeof('$[$]', srctype, 1)
        local rtype = typeof('$&', srctype)
        return function (self)
            local cendp = new(pp_int8_t)
            local errno = 0
            local srcval = new(atype)
            f(srcval, self.cursor, cendp, 0)
            self.number = Any(cast(srctype, cast(rtype, srcval)))
            local cend = cendp[0]
            if ((cend == self.cursor)
                or (errno == C._ERANGE)
                or (cend >= self.eof)
                or ((0 == C.isspace(cend[0]))
                    and (NULL == C.strchr(token_terminators, cend[0])))) then
                return false
            end
            self.next_cursor = cend
            return true
        end
    end
    cls.read_int64 = make_read_number(int64_t, C.bangra_strtoll)
    cls.read_uint64 = make_read_number(uint64_t, C.bangra_strtoull)
    cls.read_real32 = make_read_number(float,
        function (dest, cursor, cendp, base)
            return C.bangra_strtof(dest, cursor, cendp)
        end)
    function cls:next_token()
        self.lineno = self.next_lineno
        self.line = self.next_line
        self.cursor = self.next_cursor
        set_active_anchor(self:anchor())
    end
    function cls:read_token ()
        local c
        local cc
    ::skip::
        self:next_token()
        if (self:is_eof()) then
            self.token = Token.eof
            goto done
        end
        c = self:next()
        verify_good_taste(c)
        if (c == CR) then
            self:newline()
        end
        if (0 ~= C.isspace(c)) then
            goto skip
        end
        cc = tochar(c)
        if (cc == '#') then
            self:read_string(CR)
            goto skip
        elseif (cc == '(') then self.token = Token.open
        elseif (cc == ')') then self.token = Token.close
        elseif (cc == '[') then self.token = Token.square_open
        elseif (cc == ']') then self.token = Token.square_close
        elseif (cc == '{') then self.token = Token.curly_open
        elseif (cc == '}') then self.token = Token.curly_close
        elseif (cc == '\\') then self.token = Token.escape
        elseif (cc == '"') then self.token = Token.string; self:read_string(c)
        elseif (cc == ';') then self.token = Token.statement
        else
            if (self:read_int64()
                or self:read_uint64()
                or self:read_real32()) then self.token = Token.number
            else self.token = Token.symbol; self:read_symbol() end
        end
    ::done::
        return self.token
    end
    function cls:get_symbol()
        local dest = zstr_from_buffer(self.string, self.string_len)
        local size = C.unescape_string(dest)
        return Any(Symbol(cstr(zstr_from_buffer(dest, size))))
    end
    function cls:get_string()
        local dest = zstr_from_buffer(self.string + 1, self.string_len - 2)
        local size = C.unescape_string(dest)
        return Any(zstr_from_buffer(dest, size))
    end
    function cls:get_number()
        if ((self.number.type == Type.I64)
            and (self.number.value <= 0x7fffffffll)
            and (self.number.value >= -0x80000000ll)) then
            return Any(int32_t(self.number.value))
        elseif ((self.number.type == Type.U64)
            and (self.number.value <= 0xffffffffull)) then
            return Any(uint32_t(self.number.value))
        end
        -- return copy instead of reference
        return self.number
    end
    function cls:get()
        if (self.token == Token.number) then
            return self:get_number()
        elseif (self.token == Token.symbol) then
            return self:get_symbol()
        elseif (self.token == Token.string) then
            return self:get_string()
        end
    end
end

--------------------------------------------------------------------------------
-- SOURCE FILE
--------------------------------------------------------------------------------

local SourceFile = class("SourceFile")
do
    local file_cache = setmetatable({}, {__mode = "v"})
    local gc_token = typeof('struct {}')

    local cls = SourceFile
    function cls:init(path)
        self.path = path
        self.fd = -1
        self.length = 0
        self.ptr = MAP_FAILED
    end

    function cls.open(path, str)
        assert_string(path)
        local file = file_cache[path]
        if file == null then
            if str ~= null then
                assert_string(str)
                local file = SourceFile(path)
                file.ptr = new(rawstring, str)
                file.length = #str
                -- keep reference
                file._str = str
                file_cache[path] = file
                return file
            else
                local file = SourceFile(path)
                file.fd = C.open(path, C._O_RDONLY)
                if (file.fd >= 0) then
                    file.length = C.lseek(file.fd, 0, C._SEEK_END)
                    file.ptr = C.mmap(null,
                        file.length, C._PROT_READ, C._MAP_PRIVATE, file.fd, 0)
                    if (file.ptr ~= MAP_FAILED) then
                        file_cache[path] = file
                        file._close_token = ffi.gc(new(gc_token), cls.close)
                        return file
                    end
                end
                file:close()
            end
        else
            return file
        end
    end
    function cls:close()
        assert(not self._str)
        if (self.ptr ~= MAP_FAILED) then
            C.munmap(self.ptr, self.length)
            self.ptr = MAP_FAILED
            self.length = 0
        end
        if (self.fd >= 0) then
            C.close(self.fd)
            self.fd = -1
        end
    end
    function cls:is_open()
        return (self.fd ~= -1)
    end
    function cls:strptr()
        assert(self:is_open() or self._str)
        return cast(rawstring, self.ptr)
    end
    local CR = ord('\n')
    function cls:stream_line(writer, offset, indent)
        local str = self:strptr()
        if (offset >= self.length) then
            writer("<cannot display location in source file>\n")
            return
        end
        indent = indent or '    '
        local start = offset
        local send = offset
        while (start > 0) do
            local c = str[start-1]
            if (c == CR) then
                break
            end
            start = start - 1
        end
        while (start < offset) do
            local c = str[start]
            if C.isspace(c) == 0 then
                break
            end
            start = start + 1
        end
        while (send < self.length) do
            if (str[send] == CR) then
                break
            end
            send = send + 1
        end
        local line = zstr_from_buffer(str + start, send - start)
        writer(indent)
        writer(cstr(line))
        writer("\n")
        local column = offset - start
        if column > 0 then
            writer(indent)
            for i=1,column do
                writer(' ')
            end
            writer(ansi(STYLE.OPERATOR, '^'))
            writer("\n")
        end
    end
end

function Anchor:stream_source_line(writer, indent)
    local sf = SourceFile.open(self.path)
    if sf then
        sf:stream_line(writer, self.offset, indent)
    end
end

--------------------------------------------------------------------------------

local debugger
local function location_error_handler(err)
    local is_complex_msg = type(err) == "table" and err.msg
    local w = string_writer()
    if global_opts.print_lua_traceback or not is_complex_msg then
        w(traceback("",3))
        w('\n\n')
    end
    debugger.stream_traceback(w)
    if is_complex_msg then
        if err.quoted then
            -- return as-is
            return err.msg
        end
        if err.anchor then
            err.anchor:stream_message_with_source(w, err.msg)
        else
            w(err.msg)
            w('\n')
        end
        return w()
    else
        w(tostring(err))
        print(w())
        os.exit(1)
    end
end

--------------------------------------------------------------------------------
-- S-EXPR PARSER
--------------------------------------------------------------------------------

local EOL = {count=0}
local List = {}
MT_TYPE_MAP[List] = Type.List
setmetatable(EOL, List)
local function assert_list(x)
    if getmetatable(x) == List then
        return x
    else
        error("expected list, got " .. repr(x))
    end
end
do
    List.__class = "List"

    function List:__len()
        return self.count
    end

    function List:__index(key)
        local value = rawget(self, key)
        if value == null and self == EOL then
            location_error("cannot index into empty list")
        end
    end

    function List.from_args(...)
        local l = EOL
        for i=select('#',...),1,-1 do
            l = List(select(i, ...), l)
        end
        return l
    end

    setmetatable(List, {
        __call = function (cls, at, next)
            assert_any(at)
            assert_list(next)
            local count
            if (next ~= EOL) then
                count = next.count + 1
            else
                count = 1
            end
            return setmetatable({
                at = at,
                next = next,
                count = count
            }, List)
        end
    })
end

-- (a . (b . (c . (d . NIL)))) -> (d . (c . (b . (a . NIL))))
-- this is the mutating version; input lists are modified, direction is inverted
local function reverse_list_inplace(l, eol, cat_to)
    assert_list(l)
    eol = eol or EOL
    cat_to = cat_to or EOL
    assert_list(eol)
    assert_list(cat_to)
    local next = cat_to
    local count = 0
    if (cat_to ~= EOL) then
        count = cat_to.count
    end
    while (l ~= eol) do
        count = count + 1
        local iternext = l.next
        l.next = next
        l.count = count
        next = l
        l = iternext
    end
    return next
end

local function ListBuilder(lexer)
    local prev = EOL
    local eol = EOL
    local cls = {}
    function cls.append(value)
        assert_any(value)
        assert_syntax(value)
        prev = List(value, prev)
        assert(prev)
    end
    function cls.reset_start()
        eol = prev
    end
    function cls.is_expression_empty()
        return (prev == EOL)
    end
    function cls.split(anchor)
        -- if we haven't appended anything, that's an error
        if (cls.is_expression_empty()) then
            error("can't split empty expression")
        end
        -- reverse what we have, up to last split point and wrap result
        -- in cell
        prev = List(syntax(Any(reverse_list_inplace(prev, eol)),anchor), eol)
        assert(prev)
        cls.reset_start()
    end
    function cls.is_single_result()
        return (prev ~= EOL) and (prev.next == EOL)
    end
    function cls.get_single_result()
        if (prev ~= EOL) then
            return prev.at
        else
            return none
        end
    end
    function cls.get_result()
        return reverse_list_inplace(prev)
    end
    return cls
end

local function parse(lexer)
    local parse_naked
    local parse_any

    -- parses a list to its terminator and returns a handle to the first cell
    local function parse_list(end_token)
        local builder = ListBuilder(lexer)
        lexer:read_token()
        while (true) do
            if (lexer.token == end_token) then
                break
            elseif (lexer.token == Token.escape) then
                local column = lexer:column()
                lexer:read_token()
                builder.append(parse_naked(column, end_token))
            elseif (lexer.token == Token.eof) then
                location_error("missing closing bracket")
                -- point to beginning of list
                --error_origin = builder.getAnchor();
            elseif (lexer.token == Token.statement) then
                builder.split(lexer:anchor())
                lexer:read_token()
            else
                builder.append(parse_any())
                lexer:read_token()
            end
        end
        return builder.get_result()
    end

    -- parses the next sequence and returns it wrapped in a cell that points
    -- to prev
    parse_any = function()
        assert(lexer.token ~= Token.eof)
        local anchor = lexer:anchor()
        if (lexer.token == Token.open) then
            return syntax(Any(parse_list(Token.close)), anchor)
        elseif (lexer.token == Token.square_open) then
            local list = parse_list(Token.square_close)
            local sym = get_symbol("[")
            return syntax(Any(List(wrap(sym), list)), anchor)
        elseif (lexer.token == Token.curly_open) then
            local list = parse_list(Token.curly_close)
            local sym = get_symbol("{")
            return syntax(Any(List(wrap(sym), list)), anchor)
        elseif ((lexer.token == Token.close)
            or (lexer.token == Token.square_close)
            or (lexer.token == Token.curly_close)) then
            location_error("stray closing bracket")
        elseif (lexer.token == Token.string) then
            return syntax(lexer:get_string(), anchor)
        elseif (lexer.token == Token.symbol) then
            return syntax(lexer:get_symbol(), anchor)
        elseif (lexer.token == Token.number) then
            return syntax(lexer:get_number(), anchor)
        else
            error("unexpected token: %c (%i)",
                tochar(lexer.cursor[0]), lexer.cursor[0])
        end
    end

    parse_naked = function(column, end_token)
        local lineno = lexer.lineno

        local escape = false
        local subcolumn = 0

        local anchor = lexer:anchor()
        local builder = ListBuilder(lexer)

        while (lexer.token ~= Token.eof) do
            if (lexer.token == end_token) then
                break
            elseif (lexer.token == Token.escape) then
                escape = true
                lexer:read_token()
                if (lexer.lineno <= lineno) then
                    location_error("escape character is not at end of line")
                end
                lineno = lexer.lineno
            elseif (lexer.lineno > lineno) then
                if (subcolumn == 0) then
                    subcolumn = lexer:column()
                elseif (lexer:column() ~= subcolumn) then
                    location_error("indentation mismatch")
                end
                if (column ~= subcolumn) then
                    if ((column + 4) ~= subcolumn) then
                        location_error("indentations must nest by 4 spaces.")
                    end
                end

                escape = false
                builder.reset_start()
                lineno = lexer.lineno
                -- keep adding elements while we're in the same line
                while ((lexer.token ~= Token.eof)
                        and (lexer.token ~= end_token)
                        and (lexer.lineno == lineno)) do
                    builder.append(parse_naked(subcolumn, end_token))
                end
            elseif (lexer.token == Token.statement) then
                if builder.is_expression_empty() then
                    lexer:read_token()
                else
                    builder.split(lexer:anchor())
                    lexer:read_token()
                    -- if we are in the same line, continue in parent
                    if (lexer.lineno == lineno) then
                        break
                    end
                end
            else
                builder.append(parse_any())
                lineno = lexer.next_lineno
                lexer:read_token()
            end

            if (((not escape) or (lexer.lineno > lineno))
                and (lexer:column() <= column)) then
                break
            end
        end

        if (builder.is_single_result()) then
            return builder.get_single_result()
        else
            return syntax(Any(builder.get_result()), anchor)
        end
    end

    local function parse_root()
        lexer:read_token()
        local anchor = lexer:anchor()
        local builder = ListBuilder(lexer)
        while (lexer.token ~= Token.eof) do
            if (lexer.token == Token.none) then
                break
            end
            builder.append(parse_naked(1, Token.none))
        end
        return syntax(Any(builder.get_result()), anchor)
    end

    return xpcall(
        function()
            return parse_root()
        end,
        location_error_handler
    )
end

--------------------------------------------------------------------------------
-- VALUE PRINTER
--------------------------------------------------------------------------------

local ANCHOR_SEP = ":"
local CONT_SEP = " ⮕ "

-- keywords and macros
local KEYWORDS = set(split(
    "let true false fn quote with ::* ::@ call escape do dump-syntax"
        .. " syntax-extend if else elseif loop repeat none assert qquote"
        .. " unquote unquote-splice globals return splice continuation"
        .. " try except define in for empty-list empty-tuple raise"
        .. " yield xlet cc/call fn/cc null"
    ))

    -- builtin and global functions
local FUNCTIONS = set(split(
    "external branch print repr tupleof import-c eval structof typeof"
        .. " macro block-macro block-scope-macro cons expand empty?"
        .. " dump list-head? countof tableof slice none? list-atom?"
        .. " list-load list-parse load require cstr exit hash min max"
        .. " va-arg va-countof range zip enumerate bitcast element-type"
        .. " qualify disqualify iter iterator? list? symbol? parse-c"
        .. " get-exception-handler xpcall error sizeof prompt null?"
        .. " extern-library arrayof get-scope-symbol syntax-cons"
        .. " datum->syntax syntax->datum syntax->anchor syntax-do"
    ))

-- builtin and global functions with side effects
local SFXFUNCTIONS = set(split(
    "set-scope-symbol! set-globals! set-exception-handler! bind! set!"
    ))

-- builtin operator functions that can also be used as infix
local OPERATORS = set(split(
    "+ - ++ -- * / % == != > >= < <= not and or = @ ** ^ & | ~ , . .. : += -="
        .. " *= /= %= ^= &= |= ~= <- ? := // << >>"
    ))

local TYPES = set(split(
        "int i8 i16 i32 i64 u8 u16 u32 u64 void string"
        .. " rawstring opaque r16 r32 r64 half float double symbol list parameter"
        .. " frame closure flow integer real cfunction array tuple vector"
        .. " pointer struct enum bool uint tag qualifier syntax-list"
        .. " syntax-symbol syntax anchor"
        .. " iterator type table size_t usize_t ssize_t void*"
    ))

local function StreamValueFormat(naked, depth, opts)
    opts = opts or {}
    opts.depth = depth or 0
    opts.naked = naked or false
    opts.maxdepth = opts.maxdepth or lshift(1,30)
    opts.maxlength = opts.maxlength or lshift(1,30)
    opts.keywords = opts.keywords or KEYWORDS
    opts.functions = opts.functions or FUNCTIONS
    opts.sfxfunctions = opts.sfxfunctions or SFXFUNCTIONS
    opts.operators = opts.operators or OPERATORS
    opts.types = opts.types or TYPES
    opts.anchors = opts.anchors or "line"
    return opts
end

local stream_expr
do

local simple_types = set({
    Type.Symbol, Type.String, Type.I32, Type.R32
})

local function is_nested(e)
    if is_syntax_type(e.type) then
        e = unsyntax(e)
    end
    if (e.type == Type.List) then
        local it = e.value
        while (it ~= EOL) do
            local q = it.at
            if is_syntax_type(q.type) then
                q = unsyntax(q)
            end
            if simple_types[q.type] == null then
                return true
            end
            it = it.next
        end
    end
    return false
end

local function stream_indent(writer, depth)
    depth = depth or 0
    for i=1,depth do
        writer("    ")
    end
end

stream_expr = function(writer, e, format)
    format = format or StreamValueFormat()

    local depth = format.depth
    local maxdepth = format.maxdepth
    local maxlength = format.maxlength
    local naked = format.naked
    local line_anchors = (format.anchors == "line")
    local atom_anchors = (format.anchors == "all")

    local last_anchor

    local function stream_anchor(anchor)
        if anchor then
            local str
            if not last_anchor or last_anchor.path ~= anchor.path then
                str = anchor.path
                    .. ":" .. tostring(anchor.lineno)
                    .. ":" .. tostring(anchor.column) .. ANCHOR_SEP
            elseif not last_anchor or last_anchor.lineno ~= anchor.lineno then
                str = ":" .. tostring(anchor.lineno)
                    .. ":" .. tostring(anchor.column) .. ANCHOR_SEP
            elseif not last_anchor or last_anchor.column ~= anchor.column then
                str = "::" .. tostring(anchor.column) .. ANCHOR_SEP
            else
                str = "::" .. ANCHOR_SEP
            end

            writer(ansi(STYLE.COMMENT, str))
            last_anchor = anchor
        else
            --writer(ansi(STYLE.ERROR, "?"))
        end
    end

    local function walk(e, depth, maxdepth, naked)
        assert_any(e)

        local quoted = false

        local anchor
        if is_quote_type(e.type) then
            e = unquote(e)
            quoted = true
        end

        if is_syntax_type(e.type) then
            anchor = e.value.anchor
            e = unsyntax(e)
        end

        local otype = e.type
        if quoted then
            otype = Type.Quote(otype)
        end

        if (naked) then
            stream_indent(writer, depth)
        end
        if atom_anchors then
            stream_anchor(anchor)
        end

        if (e.type == Type.List) then
            if naked and line_anchors and not atom_anchors then
                stream_anchor(anchor)
            end

            maxdepth = maxdepth - 1

            local it = e.value
            if (it == EOL) then
                writer(ansi(STYLE.OPERATOR,"()"))
                if (naked) then
                    writer('\n')
                end
                return
            end
            if maxdepth == 0 then
                writer(ansi(STYLE.OPERATOR,"("))
                writer(ansi(STYLE.COMMENT,"<...>"))
                writer(ansi(STYLE.OPERATOR,")"))
                if (naked) then
                    writer('\n')
                end
                return
            end
            local offset = 0
            if (naked) then
                local single = (it.next == EOL)
                if is_nested(it.at) then
                    writer(";")
                    writer('\n')
                    goto print_sparse
                end
            ::print_terse::
                depth = depth
                naked = false
                walk(it.at, depth, maxdepth, naked)
                it = it.next
                offset = offset + 1
                while (it ~= EOL) do
                    if (is_nested(it.at)) then
                        break
                    end
                    writer(' ')
                    walk(it.at, depth, maxdepth, naked)
                    offset = offset + 1
                    it = it.next
                end
                if single then
                    writer(";")
                end
                writer('\n')
            ::print_sparse::
                while (it ~= EOL) do
                    local depth = depth + 1
                    naked = true
                    local value = it.at
                    if ((value.type ~= Type.List) -- not a list
                        and (offset >= 1) -- not first element in list
                        and (it.next ~= EOL) -- not last element in list
                        and not(is_nested(it.next.at))) then -- next element can be terse packed too
                        single = false
                        stream_indent(writer, depth)
                        writer("\\ ")
                        goto print_terse
                    end
                    if (offset >= maxlength) then
                        stream_indent(writer, depth)
                        writer("<...>\n")
                        return
                    end
                    walk(value, depth, maxdepth, naked)
                    offset = offset + 1
                    it = it.next
                end

            else
                depth = depth + 1
                naked = false
                writer(ansi(STYLE.OPERATOR,'('))
                while (it ~= EOL) do
                    if (offset > 0) then
                        writer(' ')
                    end
                    if (offset >= maxlength) then
                        writer(ansi(STYLE.COMMENT,"..."))
                        break
                    end
                    walk(it.at, depth, maxdepth, naked)
                    offset = offset + 1
                    it = it.next
                end
                writer(ansi(STYLE.OPERATOR,')'))
                if (naked) then
                    writer('\n')
                end
            end
        else
            if (e.type == Type.Symbol) then
                local name = e.value.name
                local style =
                    (format.keywords[name] and STYLE.KEYWORD)
                    or (format.functions[name] and STYLE.FUNCTION)
                    or (format.sfxfunctions[name] and STYLE.SFXFUNCTION)
                    or (format.operators[name] and STYLE.OPERATOR)
                    or (format.types[name] and STYLE.TYPE)
                    or STYLE.SYMBOL
                if (style and support_ansi) then writer(style) end
                writer(escape_string(name, SYMBOL_ESCAPE_CHARS))
                if (style and support_ansi) then writer(ANSI.RESET) end
            else
                writer(format_any_value(e.type, e.value))
            end
            if
                quoted
                or (e.type ~= Type.I32
                and e.type ~= Type.R32
                and e.type ~= Type.String
                and e.type ~= Type.Symbol
                and e.type ~= Type.Parameter) then
                writer(ansi(STYLE.OPERATOR, ":"))
                writer(tostring(otype))
            end
            if (naked) then
                writer('\n')
            end
        end
    end
    walk(e, depth, maxdepth, naked)
end
end -- do

function List.__tostring(self)
    local s = ""
    local fmt = StreamValueFormat(false)
    fmt.maxdepth = 5
    fmt.maxlength = 10
    stream_expr(
        function (x)
            s = s .. x
        end,
        Any(self), fmt)
    return s
end

--------------------------------------------------------------------------------
-- IL OBJECTS
--------------------------------------------------------------------------------

-- CFF form implemented after
-- Leissa et al., Graph-Based Higher-Order Intermediate Representation
-- http://compilers.cs.uni-saarland.de/papers/lkh15_cgo.pdf
--
-- some parts of the paper use hindley-milner notation
-- https://en.wikipedia.org/wiki/Hindley%E2%80%93Milner_type_system
--
-- more reading material:
-- Simple and Effective Type Check Removal through Lazy Basic Block Versioning
-- https://arxiv.org/pdf/1411.0352v2.pdf
-- Julia: A Fast Dynamic Language for Technical Computing
-- http://arxiv.org/pdf/1209.5145v1.pdf

local Builtin = class("Builtin")
MT_TYPE_MAP[Builtin] = Type.Builtin
do
    local cls = Builtin
    function cls:init(func, name)
        assert_function(func)
        self.func = func
        name = name or Symbol.Unnamed
        assert_symbol(name)
        self.name = name
    end
    function cls:__call(...)
        return self.func(...)
    end
    function cls:__tostring()
        if self.name ~= Symbol.Unnamed then
            return ansi(STYLE.FUNCTION, self.name.name)
        else
            return ansi(STYLE.ERROR, tostring(self.func))
        end
    end
end

local Form = class("Form")
MT_TYPE_MAP[Form] = Type.Form
do
    local cls = Form
    function cls:init(func, name)
        assert_function(func)
        assert_symbol(name)
        self.func = func
        self.name = name
    end
    function cls:__call(...)
        return self.func(...)
    end
    function cls:__tostring()
        return ansi(STYLE.KEYWORD, self.name.name)
    end
end

local Parameter = class("Parameter")
MT_TYPE_MAP[Parameter] = Type.Parameter
local function assert_parameter(x)
    if getmetatable(x) == Parameter then
        return x
    else
        error("expected parameter, got " .. repr(x))
    end
end
do
    local cls = Parameter
    function cls:init(name, _type)
        assert_syntax(name)
        local name,anchor = unsyntax(name)
        name = unwrap(Type.Symbol, name)
        assert_symbol(name)
        assert_anchor(anchor)
        _type = _type or Type.Any
        assert_type(_type)
        self.flow = null
        self.index = -1
        self.name = name
        self.type = _type
        self.anchor = anchor
        self.vararg = endswith(name.name, "...")
    end
    function cls:__tostring()
        return
            (function()
                if self.flow ~= null then
                    return tostring(self.flow)
                        .. ansi(STYLE.OPERATOR, "@")
                        .. ansi(STYLE.NUMBER, self.index - 1)
                else
                    return ""
                end
            end)()
            .. ansi(STYLE.COMMENT, "%")
            .. ansi(STYLE.SYMBOL, self.name.name)
            ..
                (function()
                    if self.vararg then
                        return ansi(STYLE.KEYWORD, "…")
                    else
                        return ""
                    end
                end)()
            ..
                (function()
                    if self.type ~= Type.Any then
                        return ansi(STYLE.OPERATOR, ":")
                            .. self.type.displayname
                    else
                        return ""
                    end
                end)()
    end
end

local ARG_Cont = 1
local ARG_Func = 2
local ARG_Arg0 = 3

local PARAM_Cont = 1
local PARAM_Arg0 = 2

local Flow = class("Flow")
MT_TYPE_MAP[Flow] = Type.Flow
local function assert_flow(x)
    if getmetatable(x) == Flow then
        return x
    else
        error("expected flow, got " .. repr(x))
    end
end
do
    local cls = Flow
    local unique_id_counter = 1

    function cls:init(name)
        assert_syntax(name)
        local name,anchor = unsyntax(name)
        name = unwrap(Type.Symbol, name)
        assert_symbol(name)
        self.uid = unique_id_counter
        unique_id_counter = unique_id_counter + 1
        self.parameters = {}
        self.arguments = {}
        self.name = name
        self.anchor = anchor
    end

    function cls:set_body_anchor(anchor)
        assert_anchor(anchor)
        self.body_anchor = anchor
    end

    function cls:__tostring()
        return
            ansi(STYLE.KEYWORD, "λ")
            .. ansi(STYLE.SYMBOL, self.name.name)
            .. ansi(STYLE.OPERATOR, "#")
            .. ansi(STYLE.NUMBER, self.uid)
    end

    function cls:append_parameter(param)
        assert_table(param)
        assert_parameter(param)
        assert(param.flow == null)
        param.flow = self
        table.insert(self.parameters, param)
        param.index = #self.parameters
        return param
    end

    -- an empty function
    -- you have to add the continuation argument manually
    function cls.create_empty_function(name)
        return Flow(name)
    end

    -- a function that eventually returns
    function cls.create_function(name)
        local value = Flow(name)
        local sym, anchor = unsyntax(name)
        sym = unwrap(Type.Symbol, sym)
        -- continuation is always first argument
        -- this argument will be called when the function is done
        value:append_parameter(
            Parameter(syntax(Any(Symbol("return-" .. sym.name)),anchor),
                Type.Any))
        return value
    end

    -- a continuation that never returns
    function cls.create_continuation(name)
        local value = Flow(name)
        -- first argument is present, but unused
        value:append_parameter(
            Parameter(name, Type.Any))
        return value
    end
end

local Frame = class("Frame")
MT_TYPE_MAP[Frame] = Type.Frame
local function assert_frame(x)
    if getmetatable(x) == Frame then
        return x
    else
        error("expected frame, got " .. repr(x))
    end
end
do -- compact hashtables as much as one can where possible
    local cls = Frame
    function cls:init(frame)
        if (frame == null) then
            self.parent = null
            self.owner = self
            -- flow -> {frame-idx, {values}}
            self.map = {}
            self.index = 0
        else
            assert_frame(frame)
            self.parent = frame
            self.owner = frame.owner
            self.index = frame.index + 1
            self.map = frame.map
        end
    end
    function cls:__tostring()
        return
            ansi(STYLE.KEYWORD, "frame")
            .. ansi(STYLE.COMMENT, "<")
            .. ansi(STYLE.OPERATOR, "#")
            .. ansi(STYLE.NUMBER, tostring(self.index))
            .. ansi(STYLE.COMMENT, ">")
    end
    function cls:bind(flow, values)
        assert_flow(flow)
        assert_table(values)
        self = Frame(self)
        if self.map[flow] then
            self.map = {}
            self.owner = self
        end
        self.map[flow] = { self.index, values }
        return self
    end
    function cls:rebind(cont, index, value)
        assert_flow(cont)
        assert_number(index)
        assert_any(value)
        local ptr = self
        while ptr do
            ptr = ptr.owner
            local entry = ptr.map[cont]
            if (entry ~= null) then
                local values = entry[2]
                assert (index <= #values)
                values[index] = value
                return
            end
            ptr = ptr.parent
        end
    end

    function cls:get(cont, index, defvalue)
        assert_number(index)
        if cont then
            assert_flow(cont)
            -- parameter is bound - attempt resolve
            local ptr = self
            while ptr do
                ptr = ptr.owner
                local entry = ptr.map[cont]
                if (entry ~= null) then
                    local entry_index,values = unpack(entry)
                    assert (index <= #values)
                    if (self.index >= entry_index) then
                        return values[index]
                    end
                end
                ptr = ptr.parent
            end
        end
        return defvalue
    end
end
--[[
do -- clone frame for every mapping
    local cls = Frame
    function cls:init(frame)
        if frame ~= null then
            assert_frame(frame)
            self.index = frame.index + 1
        else
            self.index = 0
        end
        self.parent = frame
        self.map = {}
    end
    function cls:__tostring()
        return
            ansi(STYLE.KEYWORD, "frame")
            .. ansi(STYLE.COMMENT, "<")
            .. ansi(STYLE.OPERATOR, "#")
            .. ansi(STYLE.NUMBER, tostring(self.index))
            .. ansi(STYLE.COMMENT, ">")
    end
    function cls:bind(flow, values)
        assert_flow(flow)
        assert_table(values)
        self = Frame(self)
        self.map[flow] = values
        return self
    end
    function cls:rebind(cont, index, value)
        assert_flow(cont)
        assert_number(index)
        assert_any(value)
        local ptr = self
        while ptr do
            local entry = ptr.map[cont]
            if (entry ~= null) then
                local values = entry
                assert (index <= #values)
                values[index] = value
                return
            end
            ptr = ptr.parent
        end
    end

    function cls:get(cont, index, defvalue)
        assert_number(index)
        if cont then
            assert_flow(cont)
            -- parameter is bound - attempt resolve
            local ptr = self
            while ptr do
                local entry = ptr.map[cont]
                if (entry ~= null) then
                    local values = entry
                    assert (index <= #values)
                    return values[index]
                end
                ptr = ptr.parent
            end
        end
        return defvalue
    end
end
]]

local Closure = class("Closure")
MT_TYPE_MAP[Closure] = Type.Closure
do
    local cls = Closure
    function cls:init(flow, frame)
        assert_flow(flow)
        assert_frame(frame)
        self.flow = flow
        self.frame = frame
    end
    function cls:__tostring()
        return tostring(self.frame)
            .. ansi(STYLE.OPERATOR, "[")
            .. tostring(self.flow)
            .. ansi(STYLE.OPERATOR, "]")
    end
end

--------------------------------------------------------------------------------
-- IL PRINTER
--------------------------------------------------------------------------------

local stream_il
do
    stream_il = function(writer, afunc)
        local last_anchor
        local function stream_anchor(anchor)
            if anchor then
                local str
                if not last_anchor or last_anchor.path ~= anchor.path then
                    str = anchor.path
                        .. ":" .. tostring(anchor.lineno)
                        .. ":" .. tostring(anchor.column) .. ANCHOR_SEP
                elseif not last_anchor or last_anchor.lineno ~= anchor.lineno then
                    str = ":" .. tostring(anchor.lineno)
                        .. ":" .. tostring(anchor.column) .. ANCHOR_SEP
                elseif not last_anchor or last_anchor.column ~= anchor.column then
                    str = "::" .. tostring(anchor.column) .. ANCHOR_SEP
                else
                    str = "::" .. ANCHOR_SEP
                end

                writer(ansi(STYLE.COMMENT, str))
                last_anchor = anchor
            else
                --writer(ansi(STYLE.ERROR, "?"))
            end
        end

        local visited = {}
        local stream_any
        local function stream_flow_label(aflow)
            writer(ansi(STYLE.KEYWORD, "λ"))
            writer(ansi(STYLE.SYMBOL, aflow.name.name))
            writer(ansi(STYLE.OPERATOR, "#"))
            writer(ansi(STYLE.NUMBER, tostring(aflow.uid)))
        end

        local function stream_param_label(param, aflow)
            if param.flow ~= aflow then
                stream_flow_label(param.flow)
            end
            if param.name == Symbol.Unnamed then
                writer(ansi(STYLE.OPERATOR, "@"))
                writer(ansi(STYLE.NUMBER, tostring(param.index)))
            else
                writer(ansi(STYLE.COMMENT, "%"))
                writer(ansi(STYLE.SYMBOL, param.name.name))
            end
            if param.vararg then
                writer(ansi(STYLE.KEYWORD, "…"))
            end
        end

        local function stream_argument(arg, aflow)
            if is_syntax_type(arg.type) then
                local anchor
                arg,anchor = unsyntax(arg)
                stream_anchor(anchor)
            end

            if arg.type == Type.Parameter then
                stream_param_label(arg.value, aflow)
            elseif arg.type == Type.Flow then
                stream_flow_label(arg.value)
            else
                writer(tostring(arg))
            end
        end

        local function stream_flow (aflow, aframe)
            if visited[aflow] then
                return
            end
            visited[aflow] = true
            stream_anchor(aflow.anchor)
            writer(ansi(STYLE.KEYWORD, "fn/cc"))
            writer(" ")
            writer(ansi(STYLE.SYMBOL, aflow.name.name))
            writer(ansi(STYLE.OPERATOR, "#"))
            writer(ansi(STYLE.NUMBER, tostring(aflow.uid)))
            writer(" ")
            writer(ansi(STYLE.OPERATOR, "("))
            for i,param in ipairs(aflow.parameters) do
                if i > 1 then
                    writer(" ")
                end
                stream_param_label(param, aflow)
            end
            writer(ansi(STYLE.OPERATOR, ")"))
            writer("\n    ")
            if #aflow.arguments == 0 then
                writer(ansi(STYLE.ERROR, "empty"))
            else
                if aflow.body_anchor then
                    stream_anchor(aflow.body_anchor)
                    writer(' ')
                end
                for i=2,#aflow.arguments do
                    if i > 2 then
                        writer(" ")
                    end
                    local arg = aflow.arguments[i]
                    stream_argument(arg, aflow)
                end
                local cont = aflow.arguments[1]
                if not is_none(cont) then
                    writer(ansi(STYLE.COMMENT,CONT_SEP))
                    stream_argument(cont, aflow)
                end
            end
            writer("\n")

            --print(flow_decl_label(aflow, aframe))

            for i,arg in ipairs(aflow.arguments) do
                arg = unsyntax(arg)
                stream_any(arg)
            end
        end
        local function stream_closure(aclosure)
            stream_flow(aclosure.entry, aclosure.frame)
        end
        stream_any = function(afunc, aframe)
            if afunc.type == Type.Flow then
                stream_flow(afunc.value, aframe)
            elseif afunc.type == Type.Closure then
                stream_closure(afunc.value)
            end
        end
        stream_any(afunc)
    end
end

--------------------------------------------------------------------------------
-- DEBUG SERVICES
--------------------------------------------------------------------------------

debugger = {}
do
    local stack = {}
    local cls = debugger
    local last_anchor
    function cls.dump_traceback()
        cls.stream_traceback(stderr_writer)
    end
    function cls.stream_traceback(writer)
        writer("Traceback (most recent call last):\n")
        for i=1,#stack do
            local entry = stack[i]
            local anchor = entry[1]
            local frame = entry[2]
            local cont = entry[3]
            local dest = entry[4]
            if dest.type == Type.Builtin then
                local builtin = dest.value
                writer('  in builtin ')
                writer(tostring(builtin))
                writer('\n')
            elseif dest.type == Type.Flow then
                local flow = dest.value
                if anchor == null then
                    anchor = flow.body_anchor or flow.anchor
                end
                if anchor then
                    writer('  File ')
                    writer(repr(anchor.path))
                    writer(', line ')
                    writer(ansi(STYLE.NUMBER, tostring(anchor.lineno)))
                    if flow.name ~= Symbol.Unnamed then
                        writer(', in ')
                        writer(tostring(flow.name))
                    end
                    writer('\n')
                    anchor:stream_source_line(writer)
                end
            end
        end
    end
    local function is_eq(a,b)
        if a.type ~= b.type then
            return false
        end
        if is_none(a) then
            return true
        end
        if a.value ~= b.value then
            return false
        end
        return true
    end
    local function pop_stack(i)
        for k=#stack,i,-1 do
            stack[k] = null
        end
    end
    function cls.enter_call(frame, cont, dest, ...)
        for i=1,#stack do
            local entry = stack[i]
            local _cont = entry[3]
            if is_none(_cont) or is_eq(_cont, dest) then
                pop_stack(i)
                break
            end
        end

        if #stack > 0 then
            stack[#stack][1] = last_anchor
        end
        if dest.type == Type.Closure then
            local closure = dest.value
            frame = closure.frame
            dest = Any(closure.flow)
        end
        local anchor
        if dest.type == Type.Flow then
            local flow = dest.value
            local flow_anchor = flow.body_anchor or flow.anchor
            if flow_anchor then
                last_anchor = flow_anchor
                anchor = flow.anchor
                set_active_anchor(last_anchor)
            end
        end
        table.insert(stack, { anchor, frame, cont, dest, ... })
        if false then
            for i=1,#stack do
                local entry = stack[i]
                print(i,unpack(entry))
            end
            print("----")
        end
    end
end

--------------------------------------------------------------------------------
-- INTERPRETER
--------------------------------------------------------------------------------

local function evaluate(argindex, frame, value)
    assert_number(argindex)
    assert_frame(frame)
    assert_any(value)

    if (value.type == Type.Parameter) then
        local param = value.value
        local result = frame:get(param.flow, param.index)
        if result == nil then
            location_error(tostring(param) .. " unbound in frame")
        end
        return result
    elseif (value.type == Type.Flow) then
        if (argindex == ARG_Func) then
            -- no closure creation required
            return value
        else
            -- create closure
            return Any(Closure(value.value, frame))
        end
    end
    return value
end

local function dump_trace(writer, frame, cont, dest, ...)
    writer(repr(dest))
    for i=1,select('#', ...) do
        writer(' ')
        writer(repr(select(i, ...)))
    end
    if not is_none(cont) then
        writer(ansi(STYLE.COMMENT, CONT_SEP))
        writer(repr(cont))
    end
end

local call
local function call_flow(frame, cont, flow, ...)
    assert_frame(frame)
    assert_any(cont)
    assert_flow(flow)

    local rbuf = { cont, Any(flow), ... }
    local rcount = #rbuf

    local pcount = #flow.parameters
    assert(pcount >= 1)

    -- tmpargs map directly to param indices; that means
    -- the callee is not included.
    local tmpargs = {}

    -- copy over continuation argument
    tmpargs[PARAM_Cont] = cont
    local tcount = pcount - PARAM_Arg0 + 1
    local srci = ARG_Arg0
    for i=0,(tcount - 1) do
        local dsti = PARAM_Arg0 + i
        local param = flow.parameters[dsti]
        if param.vararg then
            -- how many parameters after this one
            local remparams = tcount - i - 1
            -- how many varargs to capture
            local vargsize = max(0, rcount - srci - remparams + 1)
            local argvalues = {}
            for k=srci,(srci + vargsize - 1) do
                assert(rbuf[k])
                argvalues[k - srci + 1] = rbuf[k]
            end
            tmpargs[dsti] = Any(argvalues)
            srci = srci + vargsize
        elseif srci <= rcount then
            tmpargs[dsti] = rbuf[srci]
            srci = srci + 1
        else
            tmpargs[dsti] = none
        end
    end

    frame = frame:bind(flow, tmpargs)
    --set_anchor(frame, get_anchor(flow))

    if global_opts.trace_execution then
        local w = string_writer()
        w(ansi(STYLE.KEYWORD, "flow "))
        dump_trace(w, frame, unpack(flow.arguments))
        if flow.body_anchor then
            flow.body_anchor:stream_message_with_source(stderr_writer, w())
        else
            stderr_writer("<unknown source location>: ")
            stderr_writer(w())
        end
        stderr_writer('\n')
    end

    if (#flow.arguments == 0) then
        location_error("function never returns")
    end

    local idx = 1
    local wbuf = {}
    local numflowargs = #flow.arguments
    for i=1,numflowargs do
        local arg = flow.arguments[i]
        if is_quote_type(arg.type) then
            arg = unquote(arg)
        else
            arg = unsyntax(arg)
        end
        if arg.type == Type.Parameter and arg.value.vararg then
            arg = evaluate(i, frame, arg)
            local args = unwrap(Type.Table, arg)
            if i == numflowargs then
                -- splice as-is
                for k=1,#args do
                    wbuf[idx] = args[k]
                    idx = idx + 1
                end
            else
                -- splice first argument or none
                if #args >= 1 then
                    wbuf[idx] = args[1]
                else
                    wbuf[idx] = none
                end
                idx = idx + 1
            end
        else
            wbuf[idx] = evaluate(i, frame, arg)
            idx = idx + 1
        end
    end

    return call(frame, unpack(wbuf))
end

call = function(frame, cont, dest, ...)
    if global_opts.trace_execution then
        stderr_writer(ansi(STYLE.KEYWORD, "trace "))
        dump_trace(stderr_writer, frame, cont, dest, ...)
        stderr_writer('\n')
    end
    assert_frame(frame)
    assert_any(cont)
    assert_any(dest)
    for i=1,select('#', ...) do
        assert_any(select(i, ...))
    end

    if dest.type == Type.Closure then
        debugger.enter_call(frame, cont, dest, ...)
        local closure = dest.value
        return call_flow(closure.frame, cont, closure.flow, ...)
    elseif dest.type == Type.Flow then
        debugger.enter_call(frame, cont, dest, ...)
        return call_flow(frame, cont, dest.value, ...)
    elseif dest.type == Type.Builtin then
        debugger.enter_call(frame, cont, dest, ...)
        local func = dest.value.func
        return func(frame, cont, dest, ...)
    elseif dest.type == Type.Type then
        local ty = dest.value
        local func = ty:lookup(Symbol.ApplyType)
        if func ~= null then
            return call(frame, cont, func, ...)
        else
            error("can not apply type "
                .. ty.displayname)
        end
    else
        location_error("don't know how to apply value of type "
            .. dest.type.displayname)
    end
end

local function execute(cont, dest, ...)
    assert_function(cont)
    assert_any(dest)
    local wrapped_cont = Any(Builtin(function(frame, _cont, dest, ...)
        assert_frame(frame)
        return cont(...)
    end, Symbol.ExecuteReturn))
    return call(Frame(), wrapped_cont, dest, ...)
end

--------------------------------------------------------------------------------
-- MACRO EXPANDER
--------------------------------------------------------------------------------

local function verify_list_parameter_count(expr, mincount, maxcount)
    assert_list(expr)
    if ((mincount <= 0) and (maxcount == -1)) then
        return true
    end
    local argcount = #expr - 1

    if ((maxcount >= 0) and (argcount > maxcount)) then
        location_error(
            format("excess argument. At most %i arguments expected", maxcount))
        return false
    end
    if ((mincount >= 0) and (argcount < mincount)) then
        location_error(
            format("at least %i arguments expected", mincount))
        return false
    end
    return true;
end

local function verify_at_parameter_count(topit, mincount, maxcount)
    assert_list(topit)
    assert(topit ~= EOL)
    local val = topit.at
    assert_syntax(val)
    verify_list_parameter_count(
        unwrap(Type.List, unsyntax(val)), mincount, maxcount)
end

--------------------------------------------------------------------------------

-- new descending approach for compiler to optimize tail calls:
-- 1. each function is called with a flow node as target argument; it represents
--    a continuation that should be called with the resulting value.

local globals

local expand
local compile

local expand_continuation
local expand_syntax_extend

local expand_root

local function wrap_expand_builtin(f)
    return function(frame, cont, dest, topit, env)
        local cur_list, cur_env = f(
            unwrap(Type.Scope, env),
            unwrap(Type.List, topit))
        assert(cur_env)
        return call(frame, none, cont, Any(cur_list), Any(cur_env))
    end
end

do

local function toparameter(env, value)
    assert_scope(env)
    assert_syntax(value)
    local anchor = value.value.anchor
    local _value = unsyntax(value)
    if _value.type == Type.Parameter then
        return value
    else
        local param = Any(Parameter(value, Type.Any))
        env:bind(value, param)
        return syntax(param, anchor)
    end
end

local function expand_expr_list(env, it)
    assert_scope(env)
    assert_list(it)

    local l = EOL
    while (it ~= EOL) do
        local nextlist,nextscope = expand(env, it)
        assert_list(nextlist)
        if (nextlist == EOL) then
            break
        end
        l = List(nextlist.at, l)
        it = nextlist.next
        env = nextscope
    end
    return reverse_list_inplace(l)
end

expand_continuation = function(env, topit)
    assert_scope(env)
    assert_list(topit)
    verify_at_parameter_count(topit, 1, -1)

    local it = topit.at

    assert_syntax(it)
    local anchor = it.value.anchor
    it = unwrap(Type.List, unsyntax(it))

    assert_syntax(it.at)
    local anchor_kw = it.at.value.anchor

    it = it.next

    local sym
    assert(it ~= EOL)

    assert_syntax(it.at)
    local trysym = unsyntax(it.at)
    if (trysym.type == Type.Symbol) then
        sym = it.at
        it = it.next
        assert(it ~= EOL)
    else
        sym = syntax(Any(Symbol.Unnamed), anchor_kw)
    end

    local params = it.at
    assert_syntax(params)
    local anchor_params = params.value.anchor
    params = unwrap(Type.List, unsyntax(params))

    it = it.next

    local subenv = Scope(env)

    local outargs = EOL
    local param = params
    while (param ~= EOL) do
        outargs = List(toparameter(subenv, param.at), outargs)
        param = param.next
    end

    return List(
            quote(syntax(Any(
                List(
                    syntax(globals:lookup(Symbol.ContinuationForm) or none, anchor_kw),
                    List(
                        sym,
                        List(
                            syntax(Any(reverse_list_inplace(outargs)),
                                anchor_params),
                            expand_expr_list(subenv, it))))), anchor)),
            topit.next), env
end

expand_syntax_extend = function(env, topit)
    local cur_list, cur_env = expand_continuation(env, topit)

    local expr = cur_list.at
    local anchor = expr.value.anchor

    local fun = unsyntax(compile(unquote(expr), none))
    return cur_list.next, execute(function(expr_env)
        return unwrap(Type.Scope, expr_env)
    end, fun, Any(env))
end

local function expand_wildcard(env, handler, topit)
    assert_scope(env)
    assert_any(handler)
    assert_list(topit)
    return execute(function(result)
        if (is_none(result)) then
            return EOL
        end
        return unwrap(Type.List, result)
    end, handler, Any(topit), Any(env))
end

local function expand_macro(env, handler, topit)
    assert_scope(env)
    assert_any(handler)
    assert_list(topit)
    local result,_0,_1 = xpcall(function()
        return execute(function(result_list, result_scope)
            --print(handler, result_list, result_scope)
            if (is_none(result_list)) then
                return EOL
            end
            result_list = unwrap(Type.List, result_list)
            result_scope = result_scope and unwrap(Type.Scope, result_scope)
            if result_list ~= EOL and result_scope == null then
                error(tostring(handler) .. " did not return a scope")
            end
            if not is_quote_type(result_list.at.type) then
                assert_syntax(result_list.at)
            end
            return result_list, result_scope
        end, handler,  Any(topit), Any(env))
    end, location_error_handler)
    if result then
        return _0,_1
    else
        local w = string_writer()
        assert_syntax(topit.at)
        local anchor = topit.at.value.anchor
        anchor:stream_message_with_source(w, 'while expanding expression')
        local fmt = StreamValueFormat()
        fmt.naked = true
        fmt.maxdepth = 3
        fmt.maxlength = 5
        stream_expr(w, topit.at, fmt)
        w(_0)
        quote_error(w())
    end
end

expand = function(env, topit)
    assert_scope(env)
    assert_list(topit)
    local result = none
::process::
    assert(topit ~= EOL)
    local expr = topit.at
    if (is_quote_type(expr.type)) then
        -- remove qualifier and return as-is
        return List(unquote(expr), topit.next), env
    end
    assert_syntax(expr)
    local anchor = expr.value.anchor
    set_active_anchor(anchor)
    expr = unsyntax(expr)
    if (is_quote_type(expr.type)) then
        location_error("syntax must not wrap quote")
    end
    if (expr.type == Type.List) then
        local list = expr.value
        if (list == EOL) then
            location_error("expression is empty")
        end

        local head = list.at
        assert_syntax(head)
        local head_anchor = expr.value.anchor
        head = unsyntax(head)

        -- resolve symbol
        if (head.type == Type.Symbol) then
            head = env:lookup(head.value) or none
        end

        if (is_macro_type(head.type)) then
            local result_list,result_env = expand_macro(env, unmacro(head), topit)
            if (result_list ~= EOL) then
                topit = result_list
                env = result_env
                assert_scope(env)
                goto process
            elseif result_scope then
                return EOL, env
            end
        end

        local default_handler = env:lookup(Symbol.ListWildcard)
        if default_handler then
            local result = expand_wildcard(env, default_handler, topit)
            if result then
                topit = result
                goto process
            end
        end

        local it = unwrap(Type.List, expr)
        result = syntax(Any(expand_expr_list(env, it)), anchor)
        topit = topit.next
    elseif expr.type == Type.Symbol then
        local value = expr.value
        result = env:lookup(value)
        if result == null then
            local default_handler = env:lookup(Symbol.SymbolWildcard)
            if default_handler then
                local result = expand_wildcard(env, default_handler, topit)
                if result then
                    topit = result
                    goto process
                end
            end
            location_error(
                format("no value bound to name '%s' in scope", value.name))
        end
        result = syntax(result, anchor)
        topit = topit.next
    else
        result = topit.at
        topit = topit.next
    end
    return List(result, topit), env
end

expand_root = function(expr, scope)
    local anchor
    if is_syntax_type(expr.type) then
        anchor = expr.value.anchor
        expr = unsyntax(expr)
    end
    expr = unwrap(Type.List, expr)
    return xpcall(function()
        local result = Any(expand_expr_list(scope or globals, expr))
        if anchor then
            return syntax(result, anchor)
        else
            return result
        end
    end, location_error_handler)
end

end -- do

--------------------------------------------------------------------------------
-- COMPILER
--------------------------------------------------------------------------------

local compile_root

do
local builder = {}
do
    local state

    function builder.continue_at(flow)
        state = flow
        if state ~= null then
            assert(#state.arguments == 0)
        end
    end

    function builder.with(f)
        local oldstate = state
        local result = f()
        builder.continue_at(oldstate)
        return result
    end

    -- arguments must include continuation
    function builder.br(arguments, anchor)
        assert_table(arguments)
        assert_anchor(anchor)
        for i=1,#arguments do
            local arg = arguments[i]
            assert_any(arg)
        end
        assert(#arguments >= 2)
        if (state == null) then
            error("can not define body: continuation already exited.")
        end
        assert(#state.arguments == 0)
        state.arguments = arguments
        state:set_body_anchor(anchor)
        state = null
    end
end

--------------------------------------------------------------------------------

-- write a result to a given destination; value types for dest:
-- none: value will be discarded
-- symbol: value should be returned as constant or parameter
-- *: value should be passed to dest which is assumed to be a continuation
local function write_dest(dest, value)
    assert_any(dest)
    if not is_none(dest) then
        if dest.type == Type.Symbol then
            -- a known value is returned - no need to generate code
            return value
        else
            local v = value
            local anchor
            if (is_quote_type(v.type)) then
                v = unquote(v)
            end
            if (is_syntax_type(v.type)) then
                v,anchor = unsyntax(v)
            end
            builder.br({none, dest, value}, anchor)
        end
    end
    return value
end

-- for return values that don't need to be stored
local function compile_to_none(sxvalue)
    assert_syntax(sxvalue)
    local anchor = sxvalue.value.anchor
    local value = unsyntax(sxvalue)
    if (value.type == Type.List) then
        -- complex expression
        local next = Flow.create_continuation(
            syntax(Any(Symbol.Unnamed), anchor))
        compile(sxvalue, Any(next))
        builder.continue_at(next)
    else
        -- constant value - technically we could just ignore it
        compile(sxvalue, none)
    end
end

local function compile_expr_list(it, dest, anchor)
    assert_list(it)
    assert_any(dest)
    assert_anchor(anchor)
    if (it == EOL) then
        if is_none(dest) then
            error("expression list has no instructions")
        end
        return write_dest(dest, syntax(none, anchor))
    else
        while (it ~= EOL) do
            local next = it.next
            if next == EOL then -- last element goes to dest
                return compile(it.at, dest)
            else
                -- write to unused parameter
                compile_to_none(it.at)
            end
            it = next
        end
    end
    if not is_none(dest) then
        error("unreachable branch")
    end
    return none
end

local function compile_do(it, dest)
    assert_syntax(it)
    assert_any(dest)

    local anchor = it.value.anchor
    it = unwrap(Type.List, unsyntax(it))

    it = it.next
    return compile_expr_list(it, dest, anchor)
end

local function compile_continuation(it, dest, anchor)
    assert_syntax(it)
    assert_any(dest)

    local anchor = it.value.anchor
    it = unwrap(Type.List, unsyntax(it))

    it = it.next

    local func_name = it.at

    it = it.next

    local expr_parameters = it.at
    assert_syntax(expr_parameters)
    local params_anchor = expr_parameters.value.anchor
    expr_parameters = unsyntax(it.at)

    it = it.next

    return builder.with(function()
        local func = Flow.create_empty_function(func_name, anchor)

        builder.continue_at(func)

        local params = unwrap(Type.List, expr_parameters)
        while (params ~= EOL) do
            local param = params.at
            assert_syntax(param)
            func:append_parameter(unwrap(Type.Parameter, unsyntax(param)))
            params = params.next
        end
        if (#func.parameters == 0) then
            set_active_anchor(params_anchor)
            location_error("explicit continuation parameter missing")
        end
        compile_expr_list(it, none, anchor)
        if #func.arguments == 0 then
            location_error("function does not return")
        end
        return write_dest(dest, syntax(Any(func), anchor))
    end)
end

local function compile_to_parameter(sxvalue)
    local value = unsyntax(sxvalue)
    if (value.type == Type.List) then
        -- complex expression
        return compile(sxvalue, Any(Symbol.Unnamed))
    else
        -- constant value - can be inserted directly
        return compile(sxvalue, none)
    end
end

local function compile_implicit_call(it, dest, anchor)
    assert_list(it)
    assert_any(dest)

    local callable = compile_to_parameter(it.at)
    it = it.next

    local args = { dest, callable }
    while (it ~= EOL) do
        table.insert(args, compile_to_parameter(it.at))
        it = it.next
    end

    if (dest.type == Type.Symbol) then
        local sxdest = syntax(dest, anchor)
        local next = Flow.create_continuation(sxdest)
        local param = Parameter(sxdest, Type.Any)
        param.vararg = true
        next:append_parameter(param)
        -- patch dest to an actual function
        args[1] = Any(next)
        builder.br(args, anchor)
        builder.continue_at(next)
        return Any(next.parameters[PARAM_Arg0])
    else
        builder.br(args, anchor)
        return none
    end
end

local function compile_call(it, dest, anchor)
    assert_syntax(it)
    assert_any(dest)

    local anchor = it.value.anchor
    it = unwrap(Type.List, unsyntax(it))

    it = it.next
    return compile_implicit_call(it, dest, anchor)
end

local function compile_contcall(it, dest, anchor)
    assert_syntax(it)
    assert_any(dest)
    assert(not is_none(dest))

    local anchor = it.value.anchor
    it = unwrap(Type.List, unsyntax(it))

    it = it.next
    if (it == EOL) then
        location_error("continuation expected")
    end
    local args = {}
    table.insert(args, compile_to_parameter(it.at))
    it = it.next
    if (it == EOL) then
        location_error("callable expected")
    end
    table.insert(args, compile_to_parameter(it.at))
    it = it.next
    while (it ~= EOL) do
        table.insert(args, compile_to_parameter(it.at))
        it = it.next
    end
    builder.br(args, anchor)
    return none
end

--------------------------------------------------------------------------------

compile = function(sxexpr, dest, anchor)
    assert_any(dest)
    assert_any(sxexpr)
    if (is_quote_type(sxexpr.type)) then
        -- write as-is
        return write_dest(dest, sxexpr)
    end
    assert_syntax(sxexpr)

    local anchor = sxexpr.value.anchor
    local expr = unsyntax(sxexpr)

    if (expr.type == Type.List) then
        local slist = expr.value
        if (slist == EOL) then
            location_error("empty expression")
        end
        local head = slist.at
        assert_syntax(head)
        head = unsyntax(head)
        if (head.type == Type.Form) then
            return head.value(sxexpr, dest)
        else
            return compile_implicit_call(slist, dest, anchor)
        end
    else
        return write_dest(dest, sxexpr)
    end
end

--------------------------------------------------------------------------------

-- path must be resident
compile_root = function(expr, name)
    assert_syntax(expr)
    assert_string(name)

    local anchor = expr.value.anchor
    expr = unwrap(Type.List, unsyntax(expr))

    local mainfunc = Flow.create_function(syntax(Any(Symbol(name)), anchor))
    local ret = mainfunc.parameters[PARAM_Cont]
    builder.continue_at(mainfunc)

    compile_expr_list(expr, Any(ret), anchor)

    return Any(mainfunc)
end

-- special forms
--------------------------------------------------------------------------------

builtins.call = Form(compile_call, Symbol("call"))
builtins["cc/call"] = Form(compile_contcall, Symbol("cc/call"))
builtins[Symbol.ContinuationForm] = Form(compile_continuation, Symbol("fn/cc"))
builtins["do"] = Form(compile_do, Symbol("do"))

end -- do

function Anchor.extract(value)
    if is_syntax_type(value.type) then
        return value.value.anchor
    elseif value.type == Type.Parameter then
        local anchor = value.value.anchor
        if anchor then
            return anchor
        end
    elseif value.type == Type.List then
        if value.value ~= EOL then
            local head = value.value.at
            -- try to extract head
            return Anchor.extract(head)
        end
    end
end

--------------------------------------------------------------------------------
-- BUILTINS
--------------------------------------------------------------------------------

do -- reduce number of locals

local function checkargs(mincount, maxcount, ...)
    if ((mincount <= 0) and (maxcount == -1)) then
        return true
    end

    local count = 0
    for i=1,select('#', ...) do
        local arg = select(i, ...)
        if arg ~= null then
            assert_any(arg)
            count = count + 1
        else
            break
        end
    end

    if ((maxcount >= 0) and (count > maxcount)) then
        location_error(
            format("excess argument. At most %i arguments expected", maxcount))
    end
    if ((mincount >= 0) and (count < mincount)) then
        location_error(
            format("at least %i arguments expected", mincount))
    end
    return count
end

local function wrap_simple_builtin(f)
    return function(frame, cont, self, ...)
        if is_none(cont) then
            location_error("missing return")
        end
        return call(frame, none, cont, f(...))
    end
end

local function builtin_macro(value)
    return macro(Any(Builtin(wrap_expand_builtin(value))))
end

local function builtin_forward(name, errmsg)
    assert_symbol(name)
    assert_string(errmsg)
    return function(frame, cont, self, value, ...)
        checkargs(1,1, value)
        local func = value.type:lookup(name)
        if func == null then
            location_error("type "
                .. value.type.displayname
                .. " " .. errmsg)
        end
        return call(frame, cont, func, value, ...)
    end
end

local function builtin_op(_type, name, func)
    table.insert(builtin_ops, {_type, name, func})
end

local function unwrap_integer(value)
    local super = value.type:super()
    if super == Type.Integer then
        return int64_t(value.value)
    else
        location_error("integer expected, not " .. repr(value))
    end
end

local cast

-- constants
--------------------------------------------------------------------------------

builtins["true"] = bool(true)
builtins["false"] = bool(false)
builtins["none"] = none

-- types
--------------------------------------------------------------------------------

builtins.void = Type.Void
builtins.any = Type.Any
builtins.bool = Type.Bool

builtins.i8 = Type.I8
builtins.i16 = Type.I16
builtins.i32 = Type.I32
builtins.i64 = Type.I64

builtins.u8 = Type.U8
builtins.u16 = Type.U16
builtins.u32 = Type.U32
builtins.u64 = Type.U64

builtins.r32 = Type.R32
builtins.r64 = Type.R64

builtins.scope = Type.Scope
builtins.symbol = Type.Symbol
builtins.list = Type.List
builtins.parameter = Type.Parameter
builtins.string = Type.String

builtins["syntax-list"] = Type.SyntaxList
builtins["syntax-symbol"] = Type.SyntaxSymbol

-- builtin macros
--------------------------------------------------------------------------------

builtins["fn/cc"] = builtin_macro(expand_continuation)
builtins["syntax-extend"] = builtin_macro(expand_syntax_extend)

-- flow control
--------------------------------------------------------------------------------

local b_true = bool(true)
function builtins.branch(frame, cont, self, cond, then_cont, else_cont)
    checkargs(3,3,cond,then_cont,else_cont)
    if unwrap(Type.Bool, cond) == b_true then
        return call(frame, cont, then_cont)
    else
        return call(frame, cont, else_cont)
    end
end

local function ordered_branch(frame, cont, self, a, b,
    equal_cont, unordered_cont, less_cont, greater_cont)
    checkargs(6,6,a,b,equal_cont,unordered_cont,less_cont,greater_cont)
    local function unordered()
        local rcmp = b.type:lookup(Symbol.Compare)
        if rcmp then
            return call(frame, cont, rcmp, b, a,
                equal_cont, unordered_cont, greater_cont, less_cont)
        else
            error("types "
                .. a.type.displayname
                .. " and "
                .. b.type.displayname
                .. " are incomparable")
        end
    end
    local cmp = a.type:lookup(Symbol.Compare)
    if cmp then
        return call(frame, cont, cmp, a, b,
            equal_cont, Any(Builtin(unordered, Symbol.RCompare)),
            less_cont, greater_cont)
    else
        return unordered()
    end
end
-- ordered-branch(a, b, equal, unordered [, less [, greater]])
builtins['ordered-branch'] = ordered_branch

builtins.error = function(frame, cont, self, msg)
    checkargs(1,1, msg)
    location_error(unwrap(Type.String, msg))
end

-- constructors
--------------------------------------------------------------------------------

builtins["list-load"] = wrap_simple_builtin(function(path)
    checkargs(1,1, path)
    path = unwrap(Type.String, path)
    local src = SourceFile.open(path)
    local ptr = src:strptr()
    local lexer = Lexer.init(ptr, ptr + src.length, path)
    local result,expr = parse(lexer)
    if result then
        return expr
    else
        error(expr)
    end
end)

builtins.expand = wrap_simple_builtin(function(expr, scope)
    checkargs(2,2, expr, scope)
    local _scope = unwrap(Type.Scope, scope)
    local result,expexpr = expand_root(expr, _scope)
    if result then
        return expexpr, scope
    else
        error(expexpr)
    end
end)

builtins.eval = wrap_simple_builtin(function(expr, scope, path)
    local argcount = checkargs(1,3, expr, scope, path)
    local scope
    if argcount > 1 then
        scope = unwrap(Type.Scope, scope)
    else
        scope = globals
    end
    local path
    if argcount > 2 then
        path = unwrap(Type.String, path)
    else
        path = "<eval>"
    end
    local result,expexpr = expand_root(unwrap(Type.List, expr), scope)
    if result then
        return compile_root(expexpr, path)
    else
        error(expexpr)
    end
end)

builtins.escape = wrap_simple_builtin(function(value)
    checkargs(1,1, value)
    return quote(value)
end)

builtins["block-scope-macro"] = wrap_simple_builtin(function(func)
    checkargs(1,1, func)
    unwrap(Type.Closure, func)
    return macro(func)
end)

builtins.cons = wrap_simple_builtin(function(at, next)
    checkargs(2,2, at, next)
    next = unwrap(Type.List, next)
    return Any(List(at, next))
end)

builtins["syntax-cons"] = wrap_simple_builtin(function(at, next)
    checkargs(2,2, at, next)
    if not is_quote_type(at.type) then
        assert_syntax(at)
    end
    assert_syntax(next)
    local next, next_anchor = unsyntax(next)
    next = unwrap(Type.List, next)
    return syntax(Any(List(at, next)), next_anchor)
end)

builtins["syntax->datum"] = wrap_simple_builtin(function(value)
    checkargs(1,1,value)
    assert_syntax(value)
    return (unsyntax(value))
end)

builtins["syntax->anchor"] = wrap_simple_builtin(function(value)
    checkargs(1,1,value)
    assert_syntax(value)
    local _,anchor = unsyntax(value)
    return Any(anchor)
end)

builtins["active-anchor"] = wrap_simple_builtin(function()
    return Any(get_active_anchor())
end)

builtins["datum->syntax"] = wrap_simple_builtin(function(value, anchor)
    checkargs(1,2,value,anchor)

    if is_syntax_type(value.type) then
        location_error("argument must not be syntax")
    end
    if is_null_or_none(anchor) then
        anchor = Anchor.extract(value)
        if anchor == null then
            location_error("argument of type "
                .. repr(value.type)
                .. " does not embed anchor")
        end
    else
        anchor = unwrap(Type.Anchor, anchor)
    end
    return syntax(value,anchor)
end)

builtin_op(Type.Symbol, Symbol.ApplyType,
    wrap_simple_builtin(function(name)
        checkargs(1,1,name)
        return Any(Symbol(unwrap(Type.String, name)))
    end))

builtin_op(Type.List, Symbol.ApplyType,
    wrap_simple_builtin(function(...)
        checkargs(0,-1,...)
        return Any(List.from_args(...))
    end))

builtin_op(Type.SyntaxList, Symbol.ApplyType,
    wrap_simple_builtin(function(...)
        checkargs(0,-1,...)
        local vacount = select('#', ...)
        for i=1,vacount do
            assert_syntax(select(i, ...))
        end
        local anchor
        if vacount > 0 then
            local _
            _,anchor = unsyntax(select(1, ...))
        else
            anchor = get_active_anchor()
        end
        return syntax(Any(List.from_args(...)), anchor)
    end))

builtin_op(Type.Parameter, Symbol.ApplyType,
    wrap_simple_builtin(function(name)
        checkargs(1,1,name)
        return Any(Parameter(name))
    end))

each_numerical_type(function(T)
    builtin_op(T, Symbol.ApplyType,
        wrap_simple_builtin(function(x)
            checkargs(1,1,x)
            local xs = x.type:super()
            if xs ~= Type.Integer and xs ~= Type.Float then
                error("Unable to apply type "
                    .. T.displayname .. " to value of type "
                    .. x.type.displayname)
            end
            return Any(T.ctype(x.value))
        end))
end)

builtin_op(Type.Syntax, Symbol.ApplyType,
    wrap_simple_builtin(function(_type)
        checkargs(1,1,_type)
        return Any(Type.Syntax(unwrap(Type.Type, _type)))
    end))

-- comparisons
--------------------------------------------------------------------------------

local any_true = Any(bool(true))
local any_false = Any(bool(false))
local return_true = Any(Builtin(function(frame, cont, self)
    return call(frame, none, cont, any_true)
end, Symbol("return-true")))
local return_false = Any(Builtin(function(frame, cont, self)
    return call(frame, none, cont, any_false)
end, Symbol("return-false")))
builtins['=='] = function(frame, cont, self, a, b)
    return ordered_branch(frame, cont, self, a, b,
        return_true, return_false, return_false, return_false)
end
builtins['!='] = function(frame, cont, self, a, b)
    return ordered_branch(frame, cont, self, a, b,
        return_false, return_true, return_true, return_true)
end
builtins['<'] = function(frame, cont, self, a, b)
    return ordered_branch(frame, cont, self, a, b,
        return_false, return_false, return_true, return_false)
end
builtins['<='] = function(frame, cont, self, a, b)
    return ordered_branch(frame, cont, self, a, b,
        return_true, return_false, return_true, return_false)
end
builtins['>'] = function(frame, cont, self, a, b)
    return ordered_branch(frame, cont, self, a, b,
        return_false, return_false, return_false, return_true)
end
builtins['>='] = function(frame, cont, self, a, b)
    return ordered_branch(frame, cont, self, a, b,
        return_true, return_false, return_false, return_true)
end

local function compare_func(T)
    builtin_op(T, Symbol.Compare,
        function(frame, cont, self, a, b,
            equal_cont, unordered_cont, less_cont, greater_cont)
            a = unwrap(T, a)
            b = unwrap(T, b)
            if (a == b) then
                return call(frame, cont, equal_cont)
            else
                return call(frame, cont, unordered_cont)
            end
        end)
end

compare_func(Type.Bool)
compare_func(Type.Symbol)
compare_func(Type.Parameter)
compare_func(Type.Flow)

each_numerical_type(function(T)
    builtin_op(T, Symbol.Compare,
        function(frame, cont, self, a, b,
            equal_cont, unordered_cont, less_cont, greater_cont)
            a = unwrap(T, a)
            b = unwrap(T, b)
            if (a == b) then
                return call(frame, cont, equal_cont)
            elseif (a < b) then
                return call(frame, cont, less_cont)
            else
                return call(frame, cont, greater_cont)
            end
        end)
end, {ints=true})

local function compare_real_func(T, base)
    local eq = C[base .. '_eq']
    local lt = C[base .. '_lt']
    local gt = C[base .. '_gt']
    builtin_op(T, Symbol.Compare,
        function(frame, cont, self, a, b,
            equal_cont, unordered_cont, less_cont, greater_cont)
            a = unwrap(T, a)
            b = unwrap(T, b)
            if eq(a,b) then
                return call(frame, cont, equal_cont)
            elseif lt(a,b) then
                return call(frame, cont, less_cont)
            elseif gt(a,b) then
                return call(frame, cont, greater_cont)
            else
                return call(frame, cont, unordered_cont)
            end
        end)
end

compare_real_func(Type.R32, 'bangra_r32')
compare_real_func(Type.R64, 'bangra_r64')

builtin_op(Type.List, Symbol.Compare,
    function(frame, cont, self, a, b,
        equal_cont, unordered_cont, less_cont, greater_cont)
        local x = unwrap(Type.List, a)
        local y = unwrap(Type.List, b)
        local function loop()
            if (x == y) then
                return call(frame, cont, equal_cont)
            elseif (x == EOL) then
                return call(frame, cont, less_cont)
            elseif (y == EOL) then
                return call(frame, cont, greater_cont)
            end
            return ordered_branch(frame, cont, none, x.at, y.at,
                Any(Builtin(function()
                    x = x.next
                    y = y.next
                    return loop()
                end, Symbol.CompareListNext)),
                unordered_cont, less_cont, greater_cont)
        end
        return loop()
    end)

builtin_op(Type.Type, Symbol.Compare,
    function(frame, cont, self, a, b,
        equal_cont, unordered_cont, less_cont, greater_cont)
        local x = unwrap(Type.Type, a)
        local y = unwrap(Type.Type, b)
        if x == y then
            return call(frame, cont, equal_cont)
        else
            local xs = x:super()
            local ys = y:super()
            if xs == y then
                return call(frame, cont, less_cont)
            elseif ys == x then
                return call(frame, cont, greater_cont)
            else
                return call(frame, cont, unordered_cont)
            end
        end
    end)

builtin_op(Type.Syntax, Symbol.Compare,
    function(frame, cont, self, a, b,
        equal_cont, unordered_cont, less_cont, greater_cont)
        local x = unsyntax(a)
        local y = unsyntax(b)
        return ordered_branch(frame, cont, none,
            x, y, equal_cont, unordered_cont, less_cont, greater_cont)
    end)

-- cast
--------------------------------------------------------------------------------

cast = function(frame, cont, self, value, totype)
    checkargs(2,2, value, totype)
    local fromtype = Any(value.type)
    local func = value.type:lookup(Symbol.Cast)
    local function fallback_call(err)
        local desttype = unwrap(Type.Type, totype)
        local function errmsg()
            location_error("can not cast from type "
                .. value.type.displayname
                .. " to "
                .. desttype.displayname)
        end
        func = desttype:lookup(Symbol.Cast)
        if func ~= null then
            return safecall(function()
                return call(frame, cont, func, fromtype, totype, value)
            end, errmsg)
        else
            errmsg()
        end
    end
    if func ~= null then
        return safecall(
            function()
                return call(frame, cont, func, fromtype, totype, value)
            end, fallback_call)
    end
    return fallback_call()
end
builtins.cast = cast

local default_casts = wrap_simple_builtin(function(fromtype, totype, value)
    fromtype = unwrap(Type.Type, fromtype)
    totype = unwrap(Type.Type, totype)
    local fromsuper = fromtype:lookup(Symbol.Super)
    local tosuper = totype:lookup(Symbol.Super)
    fromsuper = fromsuper and unwrap(Type.Type, fromsuper)
    tosuper = tosuper and unwrap(Type.Type, tosuper)
    -- extend integer types of same signed type, but no truncation
    if fromsuper == Type.Integer and tosuper == Type.Integer then
        local from_unsigned = unwrap(Type.Bool, fromtype:lookup(Symbol.Unsigned))
        local to_unsigned = unwrap(Type.Bool, totype:lookup(Symbol.Unsigned))
        if (from_unsigned == to_unsigned) then
            local from_size = unwrap(Type.SizeT, fromtype:lookup(Symbol.Size))
            local to_size = unwrap(Type.SizeT, totype:lookup(Symbol.Size))
            if from_size <= to_size then
                return Any(totype.ctype(value.value))
            end
        end
    end
    error("incompatible types")
end)

each_numerical_type(function(T)
    builtin_op(T, Symbol.Cast, default_casts)
end)

-- join
--------------------------------------------------------------------------------

local function builtin_forward_op2(name, errmsg)
    assert_symbol(name)
    assert_string(errmsg)
    return function(frame, cont, self, a, b, ...)
        checkargs(2,2, a, b, ...)
        local func = a.type:lookup(name)
        local function fallback_call(err)
            func = b.type:lookup(name)
            if func ~= null then
                return call(frame, cont, func, a, b, any_true)
            else
                error("can not " .. errmsg .. " values of type "
                    .. a.type.displayname
                    .. " and "
                    .. b.type.displayname)
            end
        end
        if func ~= null then
            return safecall(
                function()
                    return call(frame, cont, func, a, b, any_false)
                end, fallback_call)
        end
        return fallback_call()
    end
end

builtins[Symbol.Join] = builtin_forward_op2(Symbol.Join, "join")

builtin_op(Type.String, Symbol.Join,
    wrap_simple_builtin(function(a, b, flipped)
        checkargs(3,3,a,b,flipped)
        a = unwrap(Type.String, a)
        b = unwrap(Type.String, b)
        return Any(a .. b)
    end))

builtin_op(Type.List, Symbol.Join,
    wrap_simple_builtin(function(a, b)
        checkargs(2,2,a,b)
        local la = unwrap(Type.List, a)
        local lb = unwrap(Type.List, b)
        local l = lb
        while (la ~= EOL) do
            l = List(la.at, l)
            la = la.next
        end
        return Any(reverse_list_inplace(l, lb, lb))
    end))

builtin_op(Type.Syntax, Symbol.Join,
    function(frame, cont, self, a, b)
        checkargs(2,2,a,b)
        assert_syntax(a)
        assert_syntax(b)
        local aa, ba
        a,aa = unsyntax(a)
        b,ba = unsyntax(b)
        local join = builtins[Symbol.Join]
        return join(frame,
            Any(Builtin(function(_frame, _cont, _dest, l)
                return call(_frame, none, cont, syntax(l, aa))
            end, Symbol.JoinForwarder)),
            none, a, b)
    end)

-- arithmetic
--------------------------------------------------------------------------------

builtins[Symbol.Add] = builtin_forward_op2(Symbol.Add, "add")
builtins[Symbol.Sub] = builtin_forward_op2(Symbol.Sub, "subtract")
builtins[Symbol.Mul] = builtin_forward_op2(Symbol.Mul, "multiply")
builtins[Symbol.Div] = builtin_forward_op2(Symbol.Div, "divide")

each_numerical_type(function(T)
    local function arithmetic_op(sym, op)
        builtin_op(T, sym,
            wrap_simple_builtin(function(a,b)
                checkargs(2,2,a,b)
                return Any(op(unwrap(T, a),unwrap(T, b)))
            end))
    end
    arithmetic_op(Symbol.Add, function(a,b) return a + b end)
    arithmetic_op(Symbol.Sub, function(a,b) return a - b end)
    arithmetic_op(Symbol.Mul, function(a,b) return a * b end)
    arithmetic_op(Symbol.Div, function(a,b) return a / b end)
end)

-- interrogation
--------------------------------------------------------------------------------

builtins.typeof = wrap_simple_builtin(function(value)
    checkargs(1,1, value)
    return Any(value.type)
end)

local countof = builtin_forward(Symbol.CountOf, "is not countable")
builtins.countof = countof

local function countof_func(T)
    builtin_op(T, Symbol.CountOf,
        function(frame, cont, self, value)
            value = value.value
            return call(frame, none, cont, Any(size_t(#value)))
        end)
end

countof_func(Type.String)
countof_func(Type.List)
countof_func(Type.Syntax)

local at = builtin_forward(Symbol.At, "is not indexable")
builtins[Symbol.At] = at

builtin_op(Type.List, Symbol.At,
    wrap_simple_builtin(function(x, i)
        checkargs(2,2,x,i)
        x = unwrap(Type.List, x)
        i = unwrap_integer(i)
        for k=1,tonumber(i) do
            x = x.next
        end
        return x.at
    end))
builtin_op(Type.Syntax, Symbol.At,
    function(frame, cont, self, value, ...)
        value = unsyntax(value)
        return at(frame, cont, none, value, ...)
    end)

local fwd_slice = builtin_forward(Symbol.Slice, "is not sliceable")
builtins.slice = function(frame, cont, self, obj, start_index, end_index)
    checkargs(2,3, obj, start_index, end_index)
    return countof(frame,
        Any(Builtin(function(_frame, _cont, self, l)
            l = unwrap_integer(l)
            local i0 = unwrap_integer(start_index)
            if (i0 < size_t(0)) then
                i0 = i0 + l
            end
            i0 = min(max(i0, size_t(0)), l)
            local i1
            if end_index then
                i1 = unwrap_integer(end_index)
                if (i1 < size_t(0)) then
                    i1 = i1 + l
                end
                i1 = min(max(i1, i0), l)
            else
                i1 = l
            end
            return fwd_slice(frame, cont, none, obj, Any(i0), Any(i1))
        end, Symbol.SliceForwarder)), countof, obj)
end

builtin_op(Type.Syntax, Symbol.Slice,
    function(frame, cont, self, value, ...)
        local value, anchor = unsyntax(value)
        return fwd_slice(frame,
            Any(Builtin(function(_frame, _cont, _self, l)
                return call(frame, none, cont, syntax(l, anchor))
            end, Symbol.SliceForwarder)), none, value, ...)
    end)

builtin_op(Type.List, Symbol.Slice,
    wrap_simple_builtin(function(value, i0, i1)
        checkargs(3,3,value,i0,i1)
        local list = unwrap(Type.List, value)
        i0 = unwrap_integer(i0)
        i1 = unwrap_integer(i1)
        local i = int64_t(0)
        while (i < i0) do
            assert(list ~= EOL)
            list = list.next
            i = i + 1
        end
        local count = int64_t(0)
        if list ~= EOL then
            count = list.count
        end
        if (count ~= (i1 - i0)) then
            -- need to chop off tail, which requires creating a new list
            assert(list ~= EOL)
            local outlist = EOL
            while (i < i1) do
                assert(list ~= EOL)
                outlist = List(list.at, outlist)
                list = list.next
                i = i + 1
            end
            list = reverse_list_inplace(outlist)
        end
        return Any(list)
    end))

builtins["get-scope-symbol"] = wrap_simple_builtin(function(scope, key, defvalue)
    checkargs(2, 3, scope, key, defvalue)

    scope = unwrap(Type.Scope, scope)
    key = unwrap(Type.Symbol, unsyntax(key))

    return scope:lookup(key) or defvalue or none
end)

-- data manipulation
--------------------------------------------------------------------------------

builtins["set-scope-symbol!"] = wrap_simple_builtin(function(dest, key, value)
    checkargs(3,3, dest, key, value)
    local atable = unwrap(Type.Scope, dest)
    atable:bind(key, value)
end)

builtins["bind!"] = function(frame, cont, self, param, value)
    checkargs(2,2, param, value)
    if is_quote_type(param.type) then
        param = unquote(param)
    end
    param = unwrap(Type.Parameter, param)
    if not param.flow then
        error("can't rebind unbound parameter")
    end
    frame:rebind(param.flow, param.index, value)
    return call(frame, none, cont)
end

-- auxiliary utilities
--------------------------------------------------------------------------------

builtins.dump = wrap_simple_builtin(function(value)
    checkargs(1,1,value)
    local fmt = StreamValueFormat()
    fmt.naked = true
    fmt.anchors = "all"
    stream_expr(
        stdout_writer,
        value, fmt)
    return value
end)

builtins.print = wrap_simple_builtin(function(...)
    local writer = stdout_writer
    for i=1,select('#', ...) do
        if i > 1 then
            writer(' ')
        end
        local arg = select(i, ...)
        if arg.type == Type.String then
            writer(arg.value)
        else
            writer(format_any_value(arg.type, arg.value))
        end
    end
    writer('\n')
end)

--------------------------------------------------------------------------------
-- GLOBALS
--------------------------------------------------------------------------------

local function prepare_builtin_value(name, value, _type)
    local ty = type(value)
    if ty == "function" then
        value = Builtin(value)
    end
    if getmetatable(value) ~= Any then
        value = Any(value)
    end
    if type(name) == "string" then
        name = Symbol(name)
    end
    local displayname = name
    if _type then
        displayname = Symbol(_type.name .. "." .. name.name)
    end
    if ((value.type == Type.Builtin)
        or (value.type == Type.Form))
        and value.value.name == Symbol.Unnamed then
        value.value.name = displayname
    elseif is_macro_type(value.type)
        and value.value.name == Symbol.Unnamed then
        value.value.name = displayname
    end
    return Any(name), value
end

local function decl_builtin(name, value)
    globals:bind(prepare_builtin_value(name, value))
end

function Type.Void:format_value()
    return ansi(STYLE.KEYWORD, "none")
end
function Type.Symbol:format_symbol(x)
    return ansi(STYLE.SYMBOL,
        escape_string(x.name, SYMBOL_ESCAPE_CHARS))
end
function Type.String:format_symbol(x)
    return ansi(STYLE.STRING,
        '"' .. escape_string(x, "\"") .. '"')
end
function Type.Builtin:format_symbol(x)
    return tostring(x)
end

local function init_globals()
    local function configure_int_type(_type, ctype, fmt)
        local refct = reflect.typeof(ctype)
        _type:bind(Any(Symbol.Size), Any(size_t(refct.size)))
        _type:bind(Any(Symbol.Alignment), Any(size_t(refct.alignment)))
        _type:bind(Any(Symbol.Bitwidth), Any(int(refct.size * 8)))
        _type:bind(Any(Symbol.Unsigned), Any(bool(refct.unsigned or false)))
        _type:bind(Any(Symbol.Super), Any(Type.Integer))
        _type.ctype = ctype
        if _type == Type.Bool then
            function _type:format_value(x)
                if x == bool(true) then
                    return ansi(STYLE.KEYWORD, "true")
                else
                    return ansi(STYLE.KEYWORD, "false")
                end
            end
        else
            function _type:format_value(x)
                return ansi(STYLE.NUMBER, cformat(fmt, x))
            end
        end
    end
    local function configure_real_type(_type, ctype)
        local refct = reflect.typeof(ctype)
        _type:bind(Any(Symbol.Size), Any(size_t(refct.size)))
        _type:bind(Any(Symbol.Alignment), Any(size_t(refct.alignment)))
        _type:bind(Any(Symbol.Bitwidth), Any(int(refct.size * 8)))
        _type:bind(Any(Symbol.Super), Any(Type.Real))
        _type.ctype = ctype
        function _type:format_value(x)
            return ansi(STYLE.NUMBER, cformat("%g", x))
        end
    end
    configure_int_type(Type.Bool, bool)
    configure_int_type(Type.U8, uint8_t, "%d")
    configure_int_type(Type.U16, uint16_t, "%d")
    configure_int_type(Type.U32, uint32_t, "%d")
    configure_int_type(Type.U64, uint64_t, "%lld")
    configure_int_type(Type.I8, int8_t, "%u")
    configure_int_type(Type.I16, int16_t, "%u")
    configure_int_type(Type.I32, int32_t, "%u")
    configure_int_type(Type.I64, int64_t, "%llu")

    configure_real_type(Type.R32, float)
    configure_real_type(Type.R64, double)

    globals = Scope()
    for name,value in pairs(builtins) do
        decl_builtin(name, value)
    end
    globals:bind(Any(Symbol("globals")), Any(globals))

    for _,entry in ipairs(builtin_ops) do
        local _type,name,value = unpack(entry)
        name,value = prepare_builtin_value(name,value,_type)
        _type:bind(name, value)
    end
end

init_globals()
end -- do

--------------------------------------------------------------------------------
-- TESTING
--------------------------------------------------------------------------------

local macro_test2 = [[
print "result:"
    call
        fn/cc (return tuple)
            call
                fn/cc (_ f1 f2)
                    f1; f2;
                    return true
                tuple 1 2 3
                tuple 4 5 6

        fn/cc (return vars...)
            return
                fn/cc (_)
                    print "captured:" vars...
                    _;


true
]]

local macro_test = [[
do
    print 1 2 3
    print "hello world"
print
    == 3 4
    == 3 3
    != 3 3
    != 3 4
print
    == 1.0 1.0
    == 1.0 1.1
    < 10.0 5.0
    < 5.0 10.0
    != 1.0 1.0

print "yes"
print "this"
print "is"
print "dog"

((fn/cc (_ x y z w) (print x y z w) (_ x y)) "yes" "this" "is" "dog!")

;
    fn/cc testf (_ x y z w)
        print "heyy"
        print "hooo"
        call
            fn/cc subf (_ x y)
                print "hoo"
                _ x y
            \ 2 3
        print x y z w
        _ x y
    \ "yes" "this" "is" "dog!"

do
    print
        i64 5

    print
        == (r32 5) 5.0
    #error "oh my god"

print "varargs:"
    call
        fn/cc (_)
            _ 1 2 3
    "test"
    call
        fn/cc (_)
            _ 1 2 3 4

print
    countof "test"
;
    fn/cc more-testing (_ x y)
        ordered-branch 3 4
            fn/cc (_)
                _ "=="
            fn/cc (_)
                _ "!="
            fn/cc (_)
                print "hey"
                print "crash here"
                _ "<"
            fn/cc (_)
                _ ">"
        _;
    \ 3 4

]]

local lexer_test = [[
test
    test test
numbers -1 0x7fffffff 0xffffffff 0xffffffffff 0x7fffffffffffffff
    \ 0xffffffffffffffff 0.00012345 1 2 3.5 10.0 1001.0 1001.1 1001.001
    \ 1. .1 0.1 .01 0.01 1e-22 3.1415914159141591415914159 inf nan 1.33
    \ 1.0 0.0 "te\"st\n\ttest!" test
test (1 2; 3 4; 5 6)

cond
    i == 0;
        print "yes"
    i == 1;
        print "no"

function test (a b)
    assert (a != b)
    + a b

; more more
    one
    two
    three
; test test test
]]

local function test_lexer()
    local src = SourceFile.open("<test>", macro_test2)
    local ptr = src:strptr()
    local lexer = Lexer.init(ptr, ptr + src.length, src.path)
    local result,expr = parse(lexer)
    if result then
        print("parsed result:")
        stream_expr(stdout_writer, expr, StreamValueFormat(true))
        local result,expexpr = expand_root(expr)
        if result then
            print("expanded result:")
            stream_expr(stdout_writer, expexpr, StreamValueFormat(true))
            local func = compile_root(expexpr, "main")
            print("generated IL:")
            stream_il(stdout_writer, func)
            print("executing",func)
            local result,err = xpcall(
                function()
                    execute(
                        function(...)
                            print(...)
                        end,
                        func)
                end, location_error_handler)
            if not result then
                print(err)
            end
        else
            print(expexpr)
        end
    else
        print(expr)
    end
end

local function test_bangra()
    local src = SourceFile.open("bangra.b")
    local ptr = src:strptr()
    local lexer = Lexer.init(ptr, ptr + src.length, src.path)
    local result,expr = parse(lexer)
    if result then
        --stream_expr(stdout_writer, expr, StreamValueFormat(true))
        local result,expexpr = expand_root(expr)
        if result then
            --stream_expr(stdout_writer, Any(expexpr), StreamValueFormat(true))
            local func = compile_root(expexpr, "main")
            execute(
                function(...)
                    print(...)
                end,
                func)
            print("executing",func)
            local result,err = xpcall(
                function()
                    execute(
                        function(...)
                            print(...)
                        end,
                        func)
                end, location_error_handler)
            if not result then
                print(err)
            end
        else
            print(expexpr)
        end
    else
        print(expr)
    end
end

local function test_ansicolors()
    for k,v in pairs(ANSI) do
        if type(v) == "string" then
            print(ansi(v, k))
        end
    end
    print(ansi(ANSI.COLOR_RGB(0x4080ff), "yes"))

end

local function test_list()
    local l = List(Any(int(5)), EOL)
    print(l.next)
    --print(EOL.next)
end

do
    local result,err = xpcall(function()
        --test_list()
        --test_lexer()
        test_bangra()
        --test_ansicolors()
    end,
    location_error_handler)
    if not result then
        print(err)
    end
end