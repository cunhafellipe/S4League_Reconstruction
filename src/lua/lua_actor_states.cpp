#include "lua_actor_states.h"
#include "../app/dbg.h"

extern "C" {
#include "lua.h"
#include "lauxlib.h"
#include "lualib.h"
}

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <regex>
#include <set>
#include <sstream>
#include <vector>

namespace luaw {

namespace fs = std::filesystem;

// The recorder: plays the client's side of every call an actor-state
// function makes. Labels are the script's own names - the ANIMPARAMLIST_*
// local holding the index passed to GetAnimParam (read from the caller's
// locals), the LOWER_ANIM_* constant passed to GetWalkAnimParam - so the
// result reads exactly like the file.
static const char* kRecorderSrc = R"lua(
REC, UNHANDLED, ERRORS = {}, {}, {}

local function note(name) UNHANDLED[name] = (UNHANDLED[name] or 0) + 1 end

-- Any: what an unknown call returns, so the script keeps running (chains,
-- comparisons, table indexing) while the call itself is counted.
local Any = {}
setmetatable(Any, { __index = function() return function() return Any end end,
                    __call = function() return Any end })

local revCache = {}
local function constName(prefix, v)
  local m = revCache[prefix]
  if not m then
    m = {}
    for k, val in pairs(_G) do
      if type(k) == 'string' and string.sub(k, 1, #prefix) == prefix and m[val] == nil then m[val] = k end
    end
    revCache[prefix] = m
  end
  return m[v] or tostring(v)
end

-- The ANIMPARAMLIST_* local of the calling state function whose value is idx.
local function paramLabel(idx)
  for level = 3, 6 do
    if not debug.getinfo(level, 'f') then break end
    local i = 1
    while true do
      local name, val = debug.getlocal(level, i)
      if not name then break end
      if val == idx and string.sub(name, 1, 14) == 'ANIMPARAMLIST_' then return name end
      i = i + 1
    end
  end
  return tostring(idx)
end

local function unknown(kind)
  return function(t, k)
    return function(...) note(kind .. ':' .. tostring(k)); return Any end
  end
end

local AnimParam = {}
AnimParam.__index = function(t, k)
  local m = rawget(AnimParam, k); if m then return m end
  return unknown('AnimParam')(t, k)
end
function AnimParam:SetAnim(clip, startMs, blendIn, blendOut, loop, rate, reset)
  if self.rec then
    table.insert(self.rec.anims, { label = self.label, clip = clip or '', startMs = startMs or 0, blendIn = blendIn or 0,
      blendOut = blendOut or 0, loop = loop == true, rate = rate or 1.0, reset = reset == true })
  end
  return self
end
function AnimParam:SetAniNameForHuman(a, b, flag)
  if self.rec then table.insert(self.rec.human, { label = self.label, a = a or '', b = b or '', flag = flag == true }) end
  return self
end

local function newParam(rec, label) return setmetatable({ rec = rec, label = label }, AnimParam) end

local LowerSet = {}
LowerSet.__index = function(t, k)
  -- GetWalkAnimParam(LOWER_ANIM_*) and any other Get...AnimParam(dir)
  if type(k) == 'string' and string.match(k, '^Get%a*AnimParam$') then
    return function(self, dir) return newParam(self.rec, constName('LOWER_ANIM_', dir)) end
  end
  return unknown('LowerPartStateSet')(t, k)
end

local BlendParam = {}
BlendParam.__index = function(t, k)
  local m = rawget(BlendParam, k); if m then return m end
  return unknown('BlendParam')(t, k)
end
function BlendParam:SetBlendingAnim(a, b, startMs, blendIn, blendOut, loop, rate, reset)
  if self.rec then
    table.insert(self.rec.blends, { blender = self.blender, kind = self.kind, part = self.part, dir = self.dir,
      a = a or '', b = b or '', startMs = startMs or 0, blendIn = blendIn or 0, blendOut = blendOut or 0,
      loop = loop == true, rate = tonumber(rate) or 1.0, reset = reset == true })
  end
  return self
end

local Blender = {}
Blender.__index = function(t, k)
  if type(k) == 'string' then
    local kind, part = string.match(k, '^Get(%a-)(Upper)AnimParam$')
    if not kind then kind, part = string.match(k, '^Get(%a-)(Lower)AnimParam$') end
    if kind then
      return function(self, dir)
        local d = ''
        if dir ~= nil then d = constName('LOWER_ANIM_', dir); d = string.gsub(d, '^LOWER_ANIM_', '') end
        return setmetatable({ rec = self.rec, blender = self.n, kind = kind, part = part, dir = d }, BlendParam)
      end
    end
  end
  return unknown('CustomAnimBlender')(t, k)
end

local State = {}
State.__index = function(t, k)
  local m = rawget(State, k); if m then return m end
  return unknown('state')(t, k)
end
-- The binding reads the index as a number: an undefined ANIMPARAMLIST_*
-- (Skill_Metalic's ANIMPARAMLIST_NORMAL) arrives as nil -> 0, the first slot.
function State:GetAnimParam(idx)
  if type(idx) ~= 'number' then idx = 0 end
  return newParam(self.rec, paramLabel(idx))
end
function State:GetLowerPartStateSet(i) return setmetatable({ rec = self.rec }, LowerSet) end
function State:GetCustomAnimBlender(n) return setmetatable({ rec = self.rec, n = n }, Blender) end
-- Dispatchers hand a per-weapon sub-state to other functions: those are
-- recorded when that function itself is run, so this one records nothing.
function State:GetStatePerWeapon(t) return setmetatable({ rec = nil }, State) end
function State:AddSound(file, bone, s, e)
  if self.rec then table.insert(self.rec.sounds, { bySex = false, man = file or '', girl = file or '', bone = bone or '', s = s or 0, e = e or 0 }) end
end
function State:AddSoundBySex(man, girl, bone, s, e)
  if self.rec then table.insert(self.rec.sounds, { bySex = true, man = man or '', girl = girl or man or '', bone = bone or '', s = s or 0, e = e or 0 }) end
end
function State:AddSequence(index, seq, sound, start, range, flag)
  if self.rec then table.insert(self.rec.seqs, { index = index or 0, seq = seq or '', sound = sound or '', start = start or 0 }) end
end
-- Male/female versions of the effect (Death Scythe: deathscythe_ex_att_eff).
function State:AddSequenceBySex(index, seqMan, seqGirl, sound, start, range)
  if self.rec then table.insert(self.rec.seqs, { index = index or 0, seq = seqMan or '', seqGirl = seqGirl or '', sound = sound or '', start = start or 0, kind = 'bysex' }) end
end
-- Effect only the player who acts sees (Katana A slash lines).
function State:AddSequenceBySelf(index, seq, sound, start, range)
  if self.rec then table.insert(self.rec.seqs, { index = index or 0, seq = seq or '', sound = sound or '', start = start or 0, kind = 'self' }) end
end
-- Sounds of the transformed sigma blade (its "trans" state).
function State:AddSoundTransBySex(man, girl, bone, s, e)
  if self.rec then table.insert(self.rec.transSounds, { bySex = true, man = man or '', girl = girl or man or '', bone = bone or '', s = s or 0, e = e or 0 }) end
end
-- An effect on a node of the worn item (Iron Boots boosters on R_boo_Dummy01..).
function State:AddInstantEffect(node, seq, sound, start, range)
  if self.rec then table.insert(self.rec.instant, { node = node or '', seq = seq or '', sound = sound or '', start = start or 0 }) end
end
function State:SetAniPlaySpeed_IronBoots(ghost, self_)
  if self.rec then self.rec.ironBootsSpeed = { ghost = ghost or 1.0, self_ = self_ or 1.0 } end
end
function State:AddGhostTrail(name, s, e)
  if self.rec then table.insert(self.rec.ghosts, { name = name or '', s = s or 0, e = e or 0 }) end
end

function __RunState(name)
  local f = rawget(_G, name)
  if type(f) ~= 'function' then return false end
  local rec = { anims = {}, sounds = {}, seqs = {}, ghosts = {}, blends = {}, human = {}, transSounds = {}, instant = {} }
  local ok, err = pcall(f, setmetatable({ rec = rec }, State))
  if not ok then ERRORS[name] = tostring(err) end
  REC[name] = rec
  return true
end

-- Globals the scripts read but that the client provides (ActorStateGroup,
-- GameMgr, the WEAPON objects...): an Any instead of a nil error.
setmetatable(_G, { __index = function(t, k) return Any end })
)lua";

static bool ReadAll(const fs::path& p, std::string& out) {
    std::ifstream f(p, std::ios::binary);
    if (!f) return false;
    std::ostringstream ss; ss << f.rdbuf(); out = ss.str();
    return true;
}

static bool Run(lua_State* L, const std::string& src, const std::string& chunk, std::string* err = nullptr) {
    if (luaL_loadbuffer(L, src.data(), src.size(), chunk.c_str()) != 0 || lua_pcall(L, 0, 0, 0) != 0) {
        if (err) *err = lua_tostring(L, -1) ? lua_tostring(L, -1) : "?";
        lua_pop(L, 1);
        return false;
    }
    return true;
}

static double NumField(lua_State* L, int t, const char* k, double def = 0.0) {
    lua_getfield(L, t, k); const double v = lua_isnumber(L, -1) ? lua_tonumber(L, -1) : def; lua_pop(L, 1); return v;
}
static std::string StrField(lua_State* L, int t, const char* k) {
    lua_getfield(L, t, k); std::string v = lua_isstring(L, -1) ? lua_tostring(L, -1) : ""; lua_pop(L, 1); return v;
}
static bool BoolField(lua_State* L, int t, const char* k) {
    lua_getfield(L, t, k); const bool v = lua_toboolean(L, -1) != 0; lua_pop(L, 1); return v;
}
// Calls fn(rowIndexOnStack) for each element of rec[list].
template <class F> static void EachRow(lua_State* L, int rec, const char* list, F fn) {
    lua_getfield(L, rec, list);
    const int t = lua_gettop(L);
    if (lua_istable(L, t)) {
        const int n = (int)lua_objlen(L, t);
        for (int i = 1; i <= n; ++i) { lua_rawgeti(L, t, i); fn(lua_gettop(L)); lua_pop(L, 1); }
    }
    lua_pop(L, 1);
}

bool RunActorStates(const std::string& scriptDir, anim::ParsedActorStates& out, ActorStatesRun* report) {
    std::error_code ec;
    if (!fs::exists(scriptDir, ec)) return false;
    std::vector<fs::path> files;
    fs::path constants;
    for (const auto& e : fs::directory_iterator(scriptDir, ec)) {
        std::string low = e.path().filename().string();
        std::transform(low.begin(), low.end(), low.begin(), ::tolower);
        if (low.rfind("actorstates_", 0) != 0 || low.size() < 4 || low.substr(low.size() - 4) != ".lua") continue;
        if (low == "actorstates_constants.lua") constants = e.path(); else files.push_back(e.path());
    }
    std::sort(files.begin(), files.end());
    if (files.empty()) return false;

    lua_State* L = luaL_newstate();
    if (!L) return false;
    luaL_openlibs(L);
    std::string err;
    // require "Resources/Script/X.lua" -> scriptDir/X.lua, once each.
    std::string root = scriptDir; for (char& c : root) if (c == '\\') c = '/';
    std::string esc; for (char c : root) { if (c == '\\' || c == '"') esc += '\\'; esc += c; }
    const std::string requireSrc =
        "local __root = \"" + esc + "\"\nlocal __loaded = {}\n"
        "function require(name)\n"
        "  local n = string.gsub(name, '\\\\', '/')\n"
        "  local low = string.lower(n)\n"
        "  if string.sub(low, 1, 17) == 'resources/script/' then n = string.sub(n, 18)\n"
        "  elseif string.sub(low, 1, 7) == 'script/' then n = string.sub(n, 8) end\n"
        "  local key = string.lower(n)\n"
        "  if __loaded[key] then return end\n"
        "  __loaded[key] = true\n"
        "  local f = io.open(__root .. '/' .. n, 'rb')\n"
        "  if not f then return end\n"
        "  local src = f:read('*a'); f:close()\n"
        "  local chunk = loadstring(src, n)\n"
        "  if chunk then pcall(chunk) end\n"
        "end\n";
    bool ok = Run(L, requireSrc, "require", &err);
    // Constants first (LOOP_TRUE, RESET_TRUE, LOWER_ANIM_*, STATE_*, ...), then
    // the recorder (its global fallback must not hide real constants).
    std::string text;
    if (ok && !constants.empty() && ReadAll(constants, text)) {
        if (!Run(L, text, "actorstates_constants.lua", &err)) DbgLog("[lua-actor] constants: %s", err.c_str());
        Run(L, "package.loaded = package.loaded or {}", "pl");
    }
    if (ok) ok = Run(L, kRecorderSrc, "recorder", &err);
    if (!ok) { DbgLog("[lua-actor] recorder failed: %s", err.c_str()); lua_close(L); return false; }

    // Load every file (defines its functions; top-level wiring code runs
    // against the recorder's stand-ins), remembering which functions it has.
    // The names come from what the files DEFINE once run (a compiled script
    // has no text to scan), plus the text scan for functions a runtime error
    // stopped before they were defined.
    static const std::regex fnRe(R"(function\s+([A-Za-z_][A-Za-z0-9_]*)\s*\()");
    auto luaFunctions = [&]() {
        std::set<std::string> s;
        lua_pushvalue(L, LUA_GLOBALSINDEX);
        lua_pushnil(L);
        while (lua_next(L, -2)) {
            if (lua_type(L, -2) == LUA_TSTRING && lua_isfunction(L, -1) && !lua_iscfunction(L, -1)) s.insert(lua_tostring(L, -2));
            lua_pop(L, 1);
        }
        lua_pop(L, 1);
        return s;
    };
    const std::set<std::string> before = luaFunctions();
    std::vector<std::string> names;
    int loaded = 0;
    for (const fs::path& p : files) {
        if (!ReadAll(p, text)) continue;
        if (!Run(L, text, p.filename().string(), &err)) DbgLog("[lua-actor] %s: %s", p.filename().string().c_str(), err.c_str());
        else ++loaded;
        for (auto it = std::sregex_iterator(text.begin(), text.end(), fnRe); it != std::sregex_iterator(); ++it)
            names.push_back((*it)[1].str());
    }
    for (const std::string& n : luaFunctions()) if (!before.count(n)) names.push_back(n);
    std::sort(names.begin(), names.end());
    names.erase(std::unique(names.begin(), names.end()), names.end());

    ActorStatesRun rep;
    rep.files = loaded;
    for (const std::string& name : names) {
        lua_getglobal(L, "__RunState");
        lua_pushstring(L, name.c_str());
        if (lua_pcall(L, 1, 1, 0) != 0) { lua_pop(L, 1); continue; }
        const bool ran = lua_toboolean(L, -1) != 0; lua_pop(L, 1);
        if (!ran) continue;
        ++rep.functions;
        lua_getglobal(L, "REC"); lua_getfield(L, -1, name.c_str());
        const int rec = lua_gettop(L);
        anim::ActorFunc fn;
        EachRow(L, rec, "anims", [&](int r) {
            anim::ActorAnim a;
            a.label = StrField(L, r, "label"); a.clip = StrField(L, r, "clip");
            a.startMs = (int)NumField(L, r, "startMs"); a.blendInMs = (int)NumField(L, r, "blendIn"); a.blendOutMs = (int)NumField(L, r, "blendOut");
            a.loop = BoolField(L, r, "loop"); a.rate = (float)NumField(L, r, "rate", 1.0); a.reset = BoolField(L, r, "reset");
            fn.anims.push_back(std::move(a));
        });
        EachRow(L, rec, "sounds", [&](int r) {
            anim::ActorSound s;
            s.bySex = BoolField(L, r, "bySex"); s.man = StrField(L, r, "man"); s.girl = StrField(L, r, "girl"); s.bone = StrField(L, r, "bone");
            s.start = (int)NumField(L, r, "s"); s.end = (int)NumField(L, r, "e");
            fn.sounds.push_back(std::move(s));
        });
        EachRow(L, rec, "seqs", [&](int r) {
            anim::ActorSeq q;
            q.index = (int)NumField(L, r, "index"); q.seq = StrField(L, r, "seq"); q.sound = StrField(L, r, "sound"); q.startMs = (int)NumField(L, r, "start");
            q.seqGirl = StrField(L, r, "seqGirl"); q.kind = StrField(L, r, "kind");
            fn.seqs.push_back(std::move(q));
        });
        EachRow(L, rec, "transSounds", [&](int r) {
            anim::ActorSound s;
            s.bySex = BoolField(L, r, "bySex"); s.man = StrField(L, r, "man"); s.girl = StrField(L, r, "girl"); s.bone = StrField(L, r, "bone");
            s.start = (int)NumField(L, r, "s"); s.end = (int)NumField(L, r, "e");
            fn.transSounds.push_back(std::move(s));
        });
        EachRow(L, rec, "instant", [&](int r) {
            anim::ActorInstantEffect e; e.node = StrField(L, r, "node"); e.seq = StrField(L, r, "seq"); e.sound = StrField(L, r, "sound"); e.startMs = (int)NumField(L, r, "start");
            fn.instant.push_back(std::move(e));
        });
        lua_getfield(L, rec, "ironBootsSpeed");
        if (lua_istable(L, -1)) { fn.ironBootsSpeedGhost = (float)NumField(L, lua_gettop(L), "ghost", 1.0); fn.ironBootsSpeedSelf = (float)NumField(L, lua_gettop(L), "self_", 1.0); }
        lua_pop(L, 1);
        EachRow(L, rec, "ghosts", [&](int r) {
            anim::ActorGhostTrail g; g.name = StrField(L, r, "name"); g.startMs = (int)NumField(L, r, "s"); g.endMs = (int)NumField(L, r, "e");
            fn.ghosts.push_back(std::move(g));
        });
        EachRow(L, rec, "blends", [&](int r) {
            anim::ActorBlend b;
            b.blender = (int)NumField(L, r, "blender", -1); b.kind = StrField(L, r, "kind"); b.part = StrField(L, r, "part"); b.dir = StrField(L, r, "dir");
            b.clipA = StrField(L, r, "a"); b.clipB = StrField(L, r, "b");
            b.startMs = (int)NumField(L, r, "startMs"); b.blendInMs = (int)NumField(L, r, "blendIn"); b.blendOutMs = (int)NumField(L, r, "blendOut");
            b.loop = BoolField(L, r, "loop"); b.rate = (float)NumField(L, r, "rate", 1.0); b.reset = BoolField(L, r, "reset");
            fn.blend.push_back(std::move(b));
        });
        EachRow(L, rec, "human", [&](int r) {
            anim::ActorHumanName h; h.label = StrField(L, r, "label"); h.clip = StrField(L, r, "a"); h.alt = StrField(L, r, "b"); h.flag = BoolField(L, r, "flag");
            fn.human.push_back(std::move(h));
        });
        lua_pop(L, 2);
        if (!fn.anims.empty() || !fn.sounds.empty() || !fn.seqs.empty() || !fn.blend.empty() || !fn.ghosts.empty() ||
            !fn.transSounds.empty() || !fn.instant.empty())
            out[name] = std::move(fn);
    }
    // Unhandled calls and state-function errors, for the report.
    lua_getglobal(L, "UNHANDLED");
    lua_pushnil(L);
    while (lua_next(L, -2)) { rep.unhandled[lua_tostring(L, -2)] = (int)lua_tonumber(L, -1); lua_pop(L, 1); }
    lua_pop(L, 1);
    lua_getglobal(L, "ERRORS");
    lua_pushnil(L);
    while (lua_next(L, -2)) {
        const std::string fnName = lua_tostring(L, -2);
        if (out.count(fnName)) { rep.errors[fnName] = lua_tostring(L, -1); ++rep.failed; }   // only states that record something
        lua_pop(L, 1);
    }
    lua_pop(L, 1);
    lua_close(L);
    DbgLog("[lua-actor] native run: %d files, %d functions run, %zu states recorded, %d with errors, %zu unhandled call kinds",
           rep.files, rep.functions, out.size(), rep.failed, rep.unhandled.size());
    for (const auto& kv : rep.unhandled) DbgLog("[lua-actor] unhandled %s x%d", kv.first.c_str(), kv.second);
    int shown = 0;
    for (const auto& kv : rep.errors) if (shown++ < 20) DbgLog("[lua-actor] error in %s: %s", kv.first.c_str(), kv.second.c_str());
    if (report) *report = std::move(rep);
    return !out.empty();
}

}  // namespace luaw
