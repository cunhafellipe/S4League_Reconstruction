#include "lua_script_scan.h"
#include "../app/dbg.h"

extern "C" {
#include "lua.h"
#include "lauxlib.h"
#include "lualib.h"
}

#include <algorithm>
#include <cstring>
#include <fstream>
#include <map>
#include <sstream>

namespace luaw {

static bool ReadAll(const std::string& path, std::string& out) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return false;
    std::ostringstream ss; ss << f.rdbuf(); out = ss.str();
    return true;
}

bool IsBytecodeFile(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    char h[4] = {};
    f.read(h, 4);
    return f.gcount() == 4 && h[0] == 0x1B && h[1] == 'L' && h[2] == 'u' && h[3] == 'a';
}

// Sandbox: undefined globals never error. Constants (UPPER_CASE) are their own
// name; anything else is an inert callable. `require` is recorded.
static const char* kSandboxSrc = R"lua(
REQUIRES = {}
local Any
Any = setmetatable({}, {
  __index = function() return Any end,
  __call = function() return Any end,
  __newindex = function() end,
  __concat = function(a, b) return "" end,
  __add = function() return 0 end, __sub = function() return 0 end,
  __mul = function() return 0 end, __div = function() return 0 end,
  __lt = function() return false end, __le = function() return false end,
})
SANDBOX_ANY = Any
function require(name) REQUIRES[#REQUIRES + 1] = tostring(name) end
setmetatable(_G, { __index = function(_, k)
  if type(k) == 'string' and string.find(k, '^[A-Z][A-Z0-9_]*$') then return k end
  return Any
end })
)lua";

struct Sandbox {
    lua_State* L = nullptr;
    Sandbox() {
        L = luaL_newstate();
        if (!L) return;
        luaL_openlibs(L);
        if (luaL_loadbuffer(L, kSandboxSrc, strlen(kSandboxSrc), "sandbox") != 0 || lua_pcall(L, 0, 0, 0) != 0) {
            lua_close(L); L = nullptr;
        }
    }
    ~Sandbox() { if (L) lua_close(L); }
    // Runs a chunk; a runtime error stops it where it is (what it defined so far stays).
    bool Run(const std::string& src, const char* name) {
        if (!L) return false;
        if (luaL_loadbuffer(L, src.data(), src.size(), name) != 0) { lua_pop(L, 1); return false; }
        if (lua_pcall(L, 0, 0, 0) != 0) { lua_pop(L, 1); return true; }
        return true;
    }
    std::vector<std::string> LuaFunctions() {
        std::vector<std::string> out;
        lua_pushvalue(L, LUA_GLOBALSINDEX);
        lua_pushnil(L);
        while (lua_next(L, -2)) {
            if (lua_type(L, -2) == LUA_TSTRING && lua_isfunction(L, -1) && !lua_iscfunction(L, -1)) {
                const std::string k = lua_tostring(L, -2);
                if (k != "require") out.push_back(k);
            }
            lua_pop(L, 1);
        }
        lua_pop(L, 1);
        return out;
    }
};

const ScriptScan& ScanScript(const std::string& path) {
    static std::map<std::string, ScriptScan> cache;
    auto it = cache.find(path);
    if (it != cache.end()) return it->second;
    ScriptScan& out = cache[path];
    std::string src;
    if (!ReadAll(path, src)) return out;
    Sandbox sb;
    if (!sb.L) return out;
    out.ok = sb.Run(src, path.c_str());
    out.functions = sb.LuaFunctions();
    lua_getglobal(sb.L, "REQUIRES");
    for (int i = 1, n = (int)lua_objlen(sb.L, -1); i <= n; ++i) {
        lua_rawgeti(sb.L, -1, i);
        if (lua_isstring(sb.L, -1)) out.requires_.push_back(lua_tostring(sb.L, -1));
        lua_pop(sb.L, 1);
    }
    lua_pop(sb.L, 1);
    return out;
}

std::vector<std::pair<std::string, double>> ConstantsOf(const std::string& path) {
    std::vector<std::pair<std::string, double>> out;
    std::string src;
    if (!ReadAll(path, src)) return out;
    lua_State* L = luaL_newstate();
    if (!L) return out;
    luaL_openlibs(L);
    // A plain table environment: only what the file assigns shows up in it.
    if (luaL_loadbuffer(L, src.data(), src.size(), "constants") == 0) {
        lua_newtable(L);
        lua_pushvalue(L, -1);
        lua_setfield(L, LUA_REGISTRYINDEX, "constants_env");
        lua_setfenv(L, -2);
        if (lua_pcall(L, 0, 0, 0) != 0) lua_pop(L, 1);
        lua_getfield(L, LUA_REGISTRYINDEX, "constants_env");
        lua_pushnil(L);
        while (lua_next(L, -2)) {
            if (lua_type(L, -2) == LUA_TSTRING && lua_type(L, -1) == LUA_TNUMBER)
                out.push_back({ lua_tostring(L, -2), lua_tonumber(L, -1) });
            lua_pop(L, 1);
        }
        lua_pop(L, 1);
    } else lua_pop(L, 1);
    lua_close(L);
    std::stable_sort(out.begin(), out.end(), [](const auto& a, const auto& b) { return a.second < b.second; });
    return out;
}

// The dispatch functions: `Fn( state:GetStatePerWeapon( WEAPONTYPE_X ) )`.
// Every unknown global is a recorder that notes (name, weapon type).
static const char* kDispatchSrc = R"lua(
local STATEOBJ = setmetatable({}, { __index = function(_, k)
  if k == 'GetStatePerWeapon' then return function(self, wt) return { wt = wt } end end
  return function() return SANDBOX_ANY end
end })
CALLS = nil
local function recorder(name)
  return setmetatable({}, {
    __call = function(self, arg)
      if CALLS then CALLS[#CALLS + 1] = { name, (type(arg) == 'table') and arg.wt or false } end
      return SANDBOX_ANY
    end,
    __index = function() return SANDBOX_ANY end,
  })
end
setmetatable(_G, { __index = function(_, k)
  if type(k) == 'string' and (string.find(k, '^WEAPONTYPE_') or string.find(k, '^WEAPONYTPE_')) then return k end
  if type(k) == 'string' and string.find(k, '^[A-Z][A-Z0-9_]*$') then return k end
  return recorder(k)
end })
function __Dispatch(name)
  local f = rawget(_G, name)
  if type(f) ~= 'function' then return nil end
  CALLS = {}
  pcall(f, STATEOBJ)
  local c = CALLS; CALLS = nil
  return c
end
)lua";

bool ReadWeaponAttackMap(const std::string& gameplayFile, anim::WeaponAttackMap& out) {
    std::string src;
    if (!ReadAll(gameplayFile, src)) return false;
    Sandbox sb;
    if (!sb.L) return false;
    if (!sb.Run(kDispatchSrc, "dispatch")) return false;
    if (!sb.Run(src, "gameplay")) return false;

    auto calls = [&](const char* fn) {
        std::vector<std::pair<std::string, std::string>> v;   // callee, weapon type
        lua_getglobal(sb.L, "__Dispatch");
        lua_pushstring(sb.L, fn);
        if (lua_pcall(sb.L, 1, 1, 0) != 0) { lua_pop(sb.L, 1); return v; }
        if (lua_istable(sb.L, -1))
            for (int i = 1, n = (int)lua_objlen(sb.L, -1); i <= n; ++i) {
                lua_rawgeti(sb.L, -1, i);
                lua_rawgeti(sb.L, -1, 1);
                const std::string callee = lua_isstring(sb.L, -1) ? lua_tostring(sb.L, -1) : "";
                lua_pop(sb.L, 1);
                lua_rawgeti(sb.L, -1, 2);
                const std::string wt = lua_isstring(sb.L, -1) ? lua_tostring(sb.L, -1) : "";
                lua_pop(sb.L, 2);
                if (!callee.empty() && wt.rfind("WEAPONTYPE_", 0) == 0) v.push_back({ callee, wt });
            }
        lua_pop(sb.L, 1);
        return v;
    };
    auto dispatch = [&](const char* fn, std::unordered_map<std::string, std::string>& map) {
        for (const auto& c : calls(fn)) if (!map.count(c.second)) map[c.second] = c.first;
    };
    for (const auto& c : calls("ActorState_Normal")) {
        static const char pre[] = "NormalState_";
        if (c.first.rfind(pre, 0) == 0 && !out.typeBySuffix.count(c.first.substr(sizeof(pre) - 1)))
            out.typeBySuffix[c.first.substr(sizeof(pre) - 1)] = c.second;
    }
    dispatch("UseWeapon1_Weak", out.weakByType);
    dispatch("UseWeapon1_Strong", out.strongByType);
    dispatch("UseWeapon1_Strong1", out.strong1ByType);
    dispatch("UseWeapon1_CounterAttack", out.counterByType);
    dispatch("UseWeapon1_Attack2", out.attack2ByType);
    dispatch("UseWeapon1_Attack3", out.attack3ByType);
    dispatch("UseWeapon1_Attack4", out.attack4ByType);
    dispatch("UseWeapon1_Attack5", out.attack5ByType);
    dispatch("UseWeapon1", out.use1ByType);
    dispatch("UseWeapon2", out.use2ByType);
    DbgLog("[lua-scan] gameplay dispatch (compiled script): %zu weapon suffixes, weak %zu strong %zu use1 %zu use2 %zu",
           out.typeBySuffix.size(), out.weakByType.size(), out.strongByType.size(), out.use1ByType.size(), out.use2ByType.size());
    return !out.typeBySuffix.empty();
}

static const char* kAttribSrc = R"lua(
ATTRIBS = {}
local AttrMT = { __index = function(t, k)
  return function(self, ...)
    local c = rawget(self, 'calls')
    if not c[k] then c[k] = { ... } end
    return SANDBOX_ANY
  end
end }
AttackAttribIndexer = {
  GetAttackAttrib = function(self, name)
    local a = setmetatable({ calls = {} }, AttrMT)
    ATTRIBS[tostring(name)] = a
    return a
  end,
}
)lua";

bool ReadAttackAttribs(const std::string& indexerFile, anim::AttackAttribMap& out) {
    std::string src;
    if (!ReadAll(indexerFile, src)) return false;
    Sandbox sb;
    if (!sb.L) return false;
    if (!sb.Run(kAttribSrc, "attribs")) return false;
    if (!sb.Run(src, "indexer")) return false;
    // The indexer's own entry point(s): AttackAttribIndexer_Run and any other global that starts like it.
    for (const std::string& fn : sb.LuaFunctions()) {
        if (fn.rfind("AttackAttribIndexer", 0) != 0) continue;
        lua_getglobal(sb.L, fn.c_str());
        if (lua_pcall(sb.L, 0, 0, 0) != 0) lua_pop(sb.L, 1);
    }
    lua_getglobal(sb.L, "ATTRIBS");
    const int attribs = lua_gettop(sb.L);
    lua_pushnil(sb.L);
    while (lua_next(sb.L, attribs)) {
        const std::string name = lua_tostring(sb.L, -2);
        const int a = lua_gettop(sb.L);
        lua_getfield(sb.L, a, "calls");
        const int calls = lua_gettop(sb.L);
        auto arg = [&](const char* call, int i, std::string& s) {
            lua_getfield(sb.L, calls, call);
            if (lua_istable(sb.L, -1)) {
                lua_rawgeti(sb.L, -1, i);
                if (lua_isstring(sb.L, -1)) s = lua_tostring(sb.L, -1);
                lua_pop(sb.L, 1);
            }
            lua_pop(sb.L, 1);
        };
        auto has = [&](const char* call) {
            lua_getfield(sb.L, calls, call);
            const bool t = lua_istable(sb.L, -1);
            lua_pop(sb.L, 1);
            return t;
        };
        anim::AttackAttrib at;
        std::string dmg;
        arg("SetDamageData", 1, dmg);
        if (!dmg.empty()) at.damage = (int)std::strtol(dmg.c_str(), nullptr, 10);
        arg("SetPower", 1, at.power);
        arg("SetWeaponType", 1, at.weaponType);
        arg("SetDamageSequence", 1, at.hitSeq); arg("SetDamageSequence", 2, at.wallHitSeq);
        arg("SetDamageSequence", 3, at.hitSound); arg("SetDamageSequence", 4, at.wallHitSound);
        arg("SetAttackSequence", 1, at.atkSeq); arg("SetAttackSequence", 2, at.atkSound);
        arg("SetKillMessageImage", 1, at.killImg);
        if (has("SetGunBlastSequence")) {
            arg("SetGunBlastSequence", 1, at.gunBlastSeq); arg("SetGunBlastSequence", 3, at.gunBlastSound);
            arg("SetGunBlastSequence", 4, at.gunFireBone);
        }
        if (has("SetGunTracerAttachNode")) {
            if (!has("SetGunBlastSequence")) arg("SetGunTracerAttachNode", 1, at.gunFireBone);
            arg("SetGunTracerAttachNode", 2, at.gunTracerSeq);
        }
        out[name] = std::move(at);
        lua_pop(sb.L, 2);   // calls, value
    }
    lua_pop(sb.L, 1);
    DbgLog("[lua-scan] attack attribs (compiled script): %zu entries", out.size());
    return !out.empty();
}

}  // namespace luaw
