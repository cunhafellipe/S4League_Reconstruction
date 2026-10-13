// Ported 1:1 from C:\S4Web2\engine\scn_actorstates.js.
#include "actorstates.h"
#include "lua/lua_actor_states.h"
#include "app/dbg.h"
#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <cctype>
#include <cstdlib>
#include <regex>
#include <sstream>

namespace fs = std::filesystem;

#include "lua/lua_script_scan.h"

namespace anim {

// Reads a .lua file and strips line comments (--...) before any regex - without
// this, a hand-commented dispatch line in the real game (e.g. a strong attack a
// weapon does NOT have) reads the same as live code, registering a function the
// game never calls.
static std::string ReadLua(const std::string& file) {
    std::ifstream f(file, std::ios::binary);
    if (!f) return "";
    std::ostringstream ss; ss << f.rdbuf();
    std::string text = ss.str();
    return std::regex_replace(text, std::regex("--[^\\n]*"), "");
}

// Custom "re" raw-string delimiter throughout this file: the patterns below
// contain literal )" sequences (from "([^"]*)" capture groups), which would
// otherwise terminate an empty-delimiter R"(...)" raw string early.
static const std::regex kSetAnimRe(
    R"re(Get(?:Walk)?AnimParam\s*\(\s*([A-Z0-9_]+)\s*\)\s*:\s*SetAnim\s*\(\s*"([^"]*)"\s*,\s*(\d+)\s*,\s*(\d+)\s*,\s*(\d+)\s*,\s*(LOOP_\w+)\s*,\s*([\d.]+)\s*,\s*(RESET_\w+))re");
static const std::regex kSoundRe(
    R"re(Add(SoundBySex|Sound)\s*\(\s*"([^"]*)"\s*(?:,\s*"([^"]*)"\s*)?,\s*"([^"]*)"\s*,\s*(\d+)\s*,\s*(\d+))re");
static const std::regex kSeqRe(R"re(AddSequence\s*\(\s*(\d+)\s*,\s*"([^"]*)"\s*,\s*"([^"]*)"\s*,\s*(\d+))re");
static const std::regex kGhostRe(R"re(AddGhostTrail\s*\(\s*"([^"]*)"\s*,\s*(\d+)\s*,\s*(\d+))re");
static const std::regex kBlenderRe(R"(customAnimBlender\s*=\s*state\s*:\s*GetCustomAnimBlender\s*\(\s*(\d+)\s*\))");
static const std::regex kBlendRe(
    R"re(customAnimBlender\s*:\s*Get(Walk|Jump|Fly)(Upper|Lower)AnimParam\s*\(\s*(LOWER_ANIM_\w+)?\s*\)\s*:\s*SetBlendingAnim\s*\(\s*"([^"]*)"\s*,\s*"([^"]*)"\s*,\s*(\d+)\s*,\s*(\d+)\s*,\s*(\d+)\s*,\s*(LOOP_\w+)\s*,\s*([A-Za-z_]\w*|[\d.]+)\s*,\s*(RESET_\w+)\s*\))re");
static const std::regex kFnRe(R"(function\s+(\w+)\s*\()");

static std::string StripLowerAnimPrefix(const std::string& s) {
    static const std::string pfx = "LOWER_ANIM_";
    return s.rfind(pfx, 0) == 0 ? s.substr(pfx.size()) : s;
}

static float Scalar(const std::string& body, const std::string& token) {
    char* end = nullptr;
    const float literal = std::strtof(token.c_str(), &end);
    if (end && *end == '\0') return literal;
    std::smatch m;
    const std::regex assignment("(?:local\\s+)?" + token + "\\s*=\\s*([0-9]+(?:\\.[0-9]+)?)");
    return std::regex_search(body, m, assignment) ? std::stof(m[1].str()) : 1.f;
}

ParsedActorStates ParseActorStatesRegex(const std::string& scriptDir) {
    ParsedActorStates out;
    std::error_code ec;
    if (!fs::exists(scriptDir, ec)) return out;
    for (const auto& entry : fs::directory_iterator(scriptDir, ec)) {
        std::string fname = entry.path().filename().string();
        std::string lower = fname; std::transform(lower.begin(), lower.end(), lower.begin(), ::tolower);
        if (lower.rfind("actorstates_", 0) != 0 || lower.size() < 4 || lower.substr(lower.size()-4) != ".lua") continue;
        std::string text = ReadLua(entry.path().string());
        if (text.empty()) continue;

        struct Mark { std::string name; size_t at; };
        std::vector<Mark> marks;
        for (auto it = std::sregex_iterator(text.begin(), text.end(), kFnRe); it != std::sregex_iterator(); ++it)
            marks.push_back({ (*it)[1].str(), (size_t)it->position() });

        for (size_t i = 0; i < marks.size(); ++i) {
            size_t start = marks[i].at;
            size_t end = (i + 1 < marks.size()) ? marks[i+1].at : text.size();
            std::string body = text.substr(start, end - start);
            ActorFunc fn;

            for (auto it = std::sregex_iterator(body.begin(), body.end(), kSetAnimRe); it != std::sregex_iterator(); ++it) {
                auto& m = *it;
                ActorAnim a;
                a.label = m[1].str(); a.clip = m[2].str();
                a.startMs = std::stoi(m[3].str()); a.blendInMs = std::stoi(m[4].str()); a.blendOutMs = std::stoi(m[5].str());
                a.loop = m[6].str() == "LOOP_TRUE"; a.rate = std::stof(m[7].str()); a.reset = m[8].str() == "RESET_TRUE";
                fn.anims.push_back(std::move(a));
            }
            for (auto it = std::sregex_iterator(body.begin(), body.end(), kSoundRe); it != std::sregex_iterator(); ++it) {
                auto& m = *it;
                ActorSound s;
                s.bySex = m[1].str() == "SoundBySex";
                s.man = m[2].str(); s.girl = m[3].matched ? m[3].str() : m[2].str(); s.bone = m[4].str();
                s.start = std::stoi(m[5].str()); s.end = std::stoi(m[6].str());
                fn.sounds.push_back(std::move(s));
            }
            for (auto it = std::sregex_iterator(body.begin(), body.end(), kSeqRe); it != std::sregex_iterator(); ++it) {
                auto& m = *it;
                ActorSeq q; q.index = std::stoi(m[1].str()); q.seq = m[2].str(); q.sound = m[3].str(); q.startMs = std::stoi(m[4].str());
                fn.seqs.push_back(std::move(q));
            }
            for (auto it = std::sregex_iterator(body.begin(), body.end(), kGhostRe); it != std::sregex_iterator(); ++it) {
                auto& m = *it;
                ActorGhostTrail g; g.name = m[1].str(); g.startMs = std::stoi(m[2].str()); g.endMs = std::stoi(m[3].str());
                fn.ghosts.push_back(std::move(g));
            }
            struct BlockStart { size_t at; int blender; };
            std::vector<BlockStart> blockStarts;
            for (auto it = std::sregex_iterator(body.begin(), body.end(), kBlenderRe); it != std::sregex_iterator(); ++it)
                blockStarts.push_back({ (size_t)it->position(), std::stoi((*it)[1].str()) });
            for (auto it = std::sregex_iterator(body.begin(), body.end(), kBlendRe); it != std::sregex_iterator(); ++it) {
                auto& m = *it;
                int blender = -1;
                size_t pos = (size_t)it->position();
                for (const BlockStart& b : blockStarts) { if (b.at <= pos) blender = b.blender; else break; }
                if (blender < 0) continue;
                ActorBlend b;
                b.blender = blender; b.kind = m[1].str(); b.part = m[2].str();
                b.dir = m[3].matched ? StripLowerAnimPrefix(m[3].str()) : "";
                b.clipA = m[4].str(); b.clipB = m[5].str();
                b.startMs = std::stoi(m[6].str()); b.blendInMs = std::stoi(m[7].str()); b.blendOutMs = std::stoi(m[8].str());
                b.loop = m[9].str() == "LOOP_TRUE"; b.rate = Scalar(body, m[10].str()); b.reset = m[11].str() == "RESET_TRUE";
                fn.blend.push_back(std::move(b));
            }
            if (!fn.anims.empty() || !fn.sounds.empty() || !fn.seqs.empty() || !fn.blend.empty())
                out[marks[i].name] = std::move(fn);
        }
    }
    return out;
}

ParsedActorStates ParseActorStates(const std::string& scriptDir) {
    ParsedActorStates native;
    if (luaw::RunActorStates(scriptDir, native)) return native;
    DbgLog("[lua-actor] native run failed: falling back to the regex reader");
    return ParseActorStatesRegex(scriptDir);
}

std::string RunDir(float inX, float inZ) {
    bool f = inZ > 0.5f, b = inZ < -0.5f, l = inX < -0.5f, r = inX > 0.5f;
    if (f && l) return "LEFT_FRONT"; if (f && r) return "RIGHT_FRONT";
    if (b && l) return "LEFT_BACK";  if (b && r) return "RIGHT_BACK";
    if (b) return "BACK"; if (l) return "LEFT"; if (r) return "RIGHT";
    return "FRONT";
}

// Only the upper-body AnimParam entries (ANIMPARAMLIST_*); drops the leg walk
// (LOWER_ANIM_*). Clip "base" (lowercase, not a real BASE clip on the skeleton)
// is the Lua's own sentinel (e.g. SubMachineGun Firing/Misfiring) meaning "no
// clip of its own, keep the current pose" - not a real clip name, so it's dropped.
// Sigma blade shape change (S10 InitSigmaBlade_ChangeWeapon): while the
// weapon is transformed, every state plays its ANIMPARAMLIST_SHAPECHNAGE_*
// row (the scripts' spelling) instead of the plain one, the idle its
// ANIMPARAMLIST_CHANGE_NORMAL, and the weapon-change state its CHANGE_DISABLE
// pair (revert) instead of CHANGE_ENABLE (transform).
static bool g_shapeChange = false;
void SetShapeChange(bool on) { g_shapeChange = on; }
bool ShapeChange() { return g_shapeChange; }

static bool IsShapeVariant(const std::string& l) {
    return l.find("SHAPECHNAGE_") != std::string::npos || l.find("SHAPECHANGE_") != std::string::npos ||
           l == "ANIMPARAMLIST_CHANGE_NORMAL";
}

static std::vector<ActorAnim> Upper(const ActorFunc* fn) {
    std::vector<ActorAnim> rows, out;
    if (!fn) return out;
    for (const ActorAnim& a : fn->anims)
        if (a.label.rfind("ANIMPARAMLIST_", 0) == 0 && a.clip != "base") rows.push_back(a);
    // The weapon-change state: its ENABLE pair, or the DISABLE pair while
    // transformed, as NORMAL / DELAY.
    bool change = false;
    for (const ActorAnim& a : rows) if (a.label.find("CHANGE_ENABLE_") != std::string::npos) { change = true; break; }
    if (change) {
        const std::string pre = g_shapeChange ? "ANIMPARAMLIST_CHANGE_DISABLE_" : "ANIMPARAMLIST_CHANGE_ENABLE_";
        for (const ActorAnim& a : rows)
            if (a.label.rfind(pre, 0) == 0) { ActorAnim b = a; b.label = "ANIMPARAMLIST_" + a.label.substr(pre.size()); out.push_back(b); }
        return out;
    }
    for (const ActorAnim& a : rows) {
        if (IsShapeVariant(a.label)) continue;
        const ActorAnim* use = &a;
        if (g_shapeChange) {
            const std::string rest = a.label.substr(14);   // after ANIMPARAMLIST_
            for (const ActorAnim& v : rows)
                if (v.label == "ANIMPARAMLIST_SHAPECHNAGE_" + rest || v.label == "ANIMPARAMLIST_SHAPECHANGE_" + rest ||
                    (rest == "NORMAL" && v.label == "ANIMPARAMLIST_CHANGE_NORMAL")) { use = &v; break; }
        }
        ActorAnim b = *use; b.label = a.label;   // consumers keep reading NORMAL / DELAY / FIRST ...
        out.push_back(b);
    }
    return out;
}
// The leg walk entries (LOWER_ANIM_*) this same function defines for its own
// lowerPartStateSet - keyed by direction ("FRONT", "LEFT_FRONT", ..., "NORMAL"
// for standing still) so the legs keep walking/standing on their own real clip
// while the upper body plays this function's attack pose.
static std::unordered_map<std::string, ActorAnim> Lower(const ActorFunc* fn) {
    std::unordered_map<std::string, ActorAnim> out;
    if (!fn) return out;
    static const std::string pfx = "LOWER_ANIM_";
    for (const ActorAnim& a : fn->anims)
        if (a.label.rfind(pfx, 0) == 0) out[a.label.substr(pfx.size())] = a;
    return out;
}
static const ActorSound* FirstSound(const ActorFunc* fn) {
    return (fn && !fn->sounds.empty()) ? &fn->sounds[0] : nullptr;
}
static const ActorFunc* Get(const ParsedActorStates& parsed, const std::string& name) {
    auto it = parsed.find(name);
    return it != parsed.end() ? &it->second : nullptr;
}

static std::string FuncBody(const std::string& text, const std::string& name) {
    std::regex re("function\\s+" + name + "\\s*\\([^)]*\\)([\\s\\S]*?)\\nend");
    std::smatch m;
    return std::regex_search(text, m, re) ? m[1].str() : "";
}
static std::unordered_map<std::string, std::string> ParseDispatch(const std::string& body) {
    std::unordered_map<std::string, std::string> map;
    static const std::regex re(R"((\w+)\s*\(\s*state:GetStatePerWeapon\(\s*(WEAPONTYPE_\w+))");
    for (auto it = std::sregex_iterator(body.begin(), body.end(), re); it != std::sregex_iterator(); ++it) {
        auto& m = *it;
        std::string key = m[2].str();
        if (!map.count(key)) map[key] = m[1].str();
    }
    return map;
}

static bool Match1(const std::string& s, const std::regex& re, std::string& out) {
    std::smatch m;
    if (std::regex_search(s, m, re)) { out = m[1].str(); return true; }
    return false;
}

AttackAttribMap ParseAttackAttribs(const std::string& scriptDir) {
    AttackAttribMap out;
    {   // compiled (vanilla) script: run it instead of reading text
        const std::string file = (fs::path(scriptDir) / "attackattribindexer.lua").string();
        if (luaw::IsBytecodeFile(file) && luaw::ReadAttackAttribs(file, out)) return out;
    }
    std::string text = ReadLua((fs::path(scriptDir) / "attackattribindexer.lua").string());
    if (text.empty()) return out;

    static const std::regex splitRe(R"(GetAttackAttrib\(\s*(ATTACKATTRIB_\w+)\s*\))");
    std::vector<std::pair<std::string, size_t>> marks;   // name, position right after the marker
    for (auto it = std::sregex_iterator(text.begin(), text.end(), splitRe); it != std::sregex_iterator(); ++it)
        marks.push_back({ (*it)[1].str(), (size_t)(it->position() + it->length()) });

    static const std::regex dmgRe(R"(SetDamageData\s*\(\s*(\d+))");
    static const std::regex dsRe(R"re(SetDamageSequence\s*\(\s*"([^"]*)"\s*,\s*"([^"]*)"\s*,\s*"([^"]*)"\s*,\s*"([^"]*)")re");
    static const std::regex asRe(R"re(SetAttackSequence\s*\(\s*"([^"]*)"\s*,\s*"([^"]*)")re");
    static const std::regex gbRe(R"re(SetGunBlastSequence\s*\(\s*"([^"]*)"\s*,\s*"([^"]*)"\s*,\s*"([^"]*)"\s*,\s*"([^"]*)")re");
    static const std::regex gtRe(R"re(SetGunTracerAttachNode\s*\(\s*"([^"]*)"\s*(?:,\s*"([^"]*)")?)re");
    static const std::regex powRe(R"(SetPower\s*\(\s*(POWER_\w+))");
    static const std::regex wtRe(R"(SetWeaponType\s*\(\s*(WEAPONTYPE_\w+))");
    static const std::regex kiRe(R"re(SetKillMessageImage\s*\(\s*"([^"]*)")re");

    for (size_t i = 0; i < marks.size(); ++i) {
        size_t bodyEnd = (i + 1 < marks.size()) ? marks[i+1].second : text.size();
        // body starts right after this marker's own name+")" and ends at the next marker start
        std::string body = text.substr(marks[i].second, bodyEnd - marks[i].second);
        // trim to just before the next marker text if it overlapped (split semantics)
        AttackAttrib a;
        std::smatch m;
        if (std::regex_search(body, m, dmgRe)) a.damage = std::stoi(m[1].str());
        Match1(body, powRe, a.power);
        Match1(body, wtRe, a.weaponType);
        if (std::regex_search(body, m, dsRe)) { a.hitSeq = m[1].str(); a.hitSound = m[3].str(); a.wallHitSeq = m[2].str(); a.wallHitSound = m[4].str(); }
        if (std::regex_search(body, m, asRe)) { a.atkSeq = m[1].str(); a.atkSound = m[2].str(); }
        Match1(body, kiRe, a.killImg);
        bool haveGb = std::regex_search(body, m, gbRe);
        if (haveGb) { a.gunBlastSeq = m[1].str(); a.gunBlastSound = m[3].str(); a.gunFireBone = m[4].str(); }
        if (std::regex_search(body, m, gtRe)) {
            if (!haveGb) a.gunFireBone = m[1].str();
            a.gunTracerSeq = m[2].str();
        }
        out[marks[i].first] = std::move(a);
    }
    return out;
}

WeaponAttribs ResolveWeaponAttribs(const AttackAttribMap& attribMap, const std::string& weaponType) {
    WeaponAttribs out;
    if (attribMap.empty() || weaponType.empty()) return out;
    std::vector<const std::pair<const std::string, AttackAttrib>*> mine;
    for (auto& kv : attribMap) if (kv.second.weaponType == weaponType) mine.push_back(&kv);

    auto pick = [&](const std::regex& re) -> const AttackAttrib* {
        for (auto* e : mine) if (std::regex_search(e->first, re)) return &e->second;
        return nullptr;
    };
    static const std::regex standWeakRe(R"(_STAND_WEAK$)"), weakRe(R"(WEAK$)");
    static const std::regex standStrongRe(R"(_STAND_STRONG$)"), strongRe(R"(STRONG$)");

    const AttackAttrib* weak = pick(standWeakRe); if (!weak) weak = pick(weakRe);
    if (!weak) for (auto* e : mine) if (e->second.power == "POWER_WEAK") { weak = &e->second; break; }
    if (!weak && !mine.empty()) weak = &mine.front()->second;

    const AttackAttrib* strong = pick(standStrongRe); if (!strong) strong = pick(strongRe);
    if (!strong) for (auto* e : mine) if (e->second.power == "POWER_STRONG") { strong = &e->second; break; }
    if (!strong) strong = weak;

    const AttackAttrib* gunEntry = nullptr;
    for (auto* e : mine) if (!e->second.gunFireBone.empty()) { gunEntry = &e->second; break; }

    out.weak = weak; out.strong = strong;
    if (gunEntry) { out.gunBlastSeq = gunEntry->gunBlastSeq; out.gunBlastSound = gunEntry->gunBlastSound; out.gunFireBone = gunEntry->gunFireBone;
                    out.gunWallSeq = gunEntry->wallHitSeq; out.gunWallSound = gunEntry->wallHitSound; out.gunTracerSeq = gunEntry->gunTracerSeq;
                    out.gunHitSeq = gunEntry->hitSeq; out.gunHitSound = gunEntry->hitSound; }
    return out;
}

WeaponAttackMap ParseWeaponAttackMap(const std::string& scriptDir) {
    WeaponAttackMap out;
    {
        const std::string file = (fs::path(scriptDir) / "actorstates_gameplay.lua").string();
        if (luaw::IsBytecodeFile(file) && luaw::ReadWeaponAttackMap(file, out)) return out;
    }
    std::string text = ReadLua((fs::path(scriptDir) / "actorstates_gameplay.lua").string());
    if (text.empty()) return out;

    std::string normBody = FuncBody(text, "ActorState_Normal");
    static const std::regex nre(R"(NormalState_(\w+)\s*\(\s*state:GetStatePerWeapon\(\s*(WEAPONTYPE_\w+))");
    for (auto it = std::sregex_iterator(normBody.begin(), normBody.end(), nre); it != std::sregex_iterator(); ++it) {
        auto& m = *it;
        if (!out.typeBySuffix.count(m[1].str())) out.typeBySuffix[m[1].str()] = m[2].str();
    }
    out.weakByType    = ParseDispatch(FuncBody(text, "UseWeapon1_Weak"));
    out.strongByType  = ParseDispatch(FuncBody(text, "UseWeapon1_Strong"));
    out.strong1ByType = ParseDispatch(FuncBody(text, "UseWeapon1_Strong1"));
    out.counterByType = ParseDispatch(FuncBody(text, "UseWeapon1_CounterAttack"));
    out.attack2ByType = ParseDispatch(FuncBody(text, "UseWeapon1_Attack2"));
    out.attack3ByType = ParseDispatch(FuncBody(text, "UseWeapon1_Attack3"));
    out.attack4ByType = ParseDispatch(FuncBody(text, "UseWeapon1_Attack4"));
    out.attack5ByType = ParseDispatch(FuncBody(text, "UseWeapon1_Attack5"));
    out.use1ByType    = ParseDispatch(FuncBody(text, "UseWeapon1"));
    out.use2ByType    = ParseDispatch(FuncBody(text, "UseWeapon2"));
    return out;
}

static bool HasUpper(const ActorFunc* fn) { return !Upper(fn).empty(); }

WeaponAnim ResolveWeaponAnim(const ParsedActorStates& parsed, const std::string& weapon,
                              const WeaponAttackMap* attackMap, const AttackAttribMap* attribMap,
                              const ItemLuaRows* rows) {
    WeaponAnim out;
    // The item row's ani= function for a state, when the scripts have it.
    auto rowFn = [&](const char* state) -> const ActorFunc* {
        if (!rows) return nullptr;
        auto it = rows->find(state);
        return it == rows->end() || it->second.ani.empty() ? nullptr : Get(parsed, it->second.ani);
    };
    const ActorFunc* norm = Get(parsed, "NormalState_" + weapon);
    const ActorFunc* run  = Get(parsed, "RunState_" + weapon);
    const ActorFunc* weak = Get(parsed, "SwordAtkWeakState_" + weapon);
    const ActorFunc* strong = Get(parsed, "SwordAtkStrongState_" + weapon);
    const ActorFunc* strong1 = nullptr; const ActorFunc* counter = nullptr; const ActorFunc* weapon2 = nullptr;
    std::vector<ActorAnim> comboRest;
    std::vector<const ActorFunc*> comboRestFns;   // the function each comboRest row came from
    if (const ActorFunc* f = rowFn("normal")) norm = f;
    if (const ActorFunc* f = rowFn("run")) run = f;

    std::string type;
    if (attackMap) {
        auto it = attackMap->typeBySuffix.find(weapon);
        if (it != attackMap->typeBySuffix.end()) {
            type = it->second;
            auto findFn = [&](const std::unordered_map<std::string, std::string>& m) -> const ActorFunc* {
                auto fit = m.find(type); return fit != m.end() ? Get(parsed, fit->second) : nullptr;
            };
            const ActorFunc* use1 = findFn(attackMap->use1ByType);
            if (!weak || !HasUpper(weak)) { const ActorFunc* f = findFn(attackMap->weakByType); if (!f) f = use1; if (f) weak = f; }
            if (!strong || !HasUpper(strong)) { const ActorFunc* f = findFn(attackMap->strongByType); if (!f) f = use1; if (f) strong = f; }
            if (const ActorFunc* f = findFn(attackMap->strong1ByType)) strong1 = f;
            if (const ActorFunc* f = findFn(attackMap->counterByType)) counter = f;
            if (const ActorFunc* f = findFn(attackMap->use2ByType)) weapon2 = f;
            for (const auto* tbl : { &attackMap->attack2ByType, &attackMap->attack3ByType, &attackMap->attack4ByType, &attackMap->attack5ByType }) {
                if (const ActorFunc* f = findFn(*tbl)) {
                    std::vector<ActorAnim> u = Upper(f);
                    comboRest.insert(comboRest.end(), u.begin(), u.end());
                    comboRestFns.insert(comboRestFns.end(), u.size(), f);
                }
            }
        }
    }
    const ActorFunc* jatk = Get(parsed, "SwordJumpAtkState_" + weapon);
    const ActorFunc* jweak = Get(parsed, "SwordJumpAtkWeakState_" + weapon);

    // The item's rows name the exact functions (skins: UseWeapon1State_
    // SpyDagger_Mach3, SwordJumpAtkState_magmaBlade; the counter sword's
    // AfterDash1..4 chain and its CounterAttack hold). left_strong1 is the
    // held swing (SwordAtkStrongState), left_strong2 the second one
    // (SwordAtkStrong1State), left_attack the click for guns and daggers.
    if (rows) {
        const ActorFunc* rWeak = rowFn("left_weak");
        if (!rWeak) rWeak = rowFn("left_attack");
        if (rWeak && HasUpper(rWeak)) weak = rWeak;
        if (const ActorFunc* f = rowFn("left_strong1")) if (HasUpper(f)) strong = f;
        if (const ActorFunc* f = rowFn("left_strong2")) if (HasUpper(f)) strong1 = f;
        if (const ActorFunc* f = rowFn("counter_attack")) counter = f;
        if (const ActorFunc* f = rowFn("right_attack")) weapon2 = f;
        if (const ActorFunc* f = rowFn("left_jump")) jatk = f;
        std::vector<ActorAnim> rowCombo;
        std::vector<const ActorFunc*> rowComboFns;
        for (const char* st : { "left_attack2", "left_attack3", "left_attack4", "left_attack5" })
            if (const ActorFunc* f = rowFn(st)) {
                std::vector<ActorAnim> u = Upper(f);
                rowCombo.insert(rowCombo.end(), u.begin(), u.end());
                rowComboFns.insert(rowComboFns.end(), u.size(), f);
            }
        if (!rowCombo.empty()) { comboRest = rowCombo; comboRestFns = rowComboFns; }
    }

    for (const ActorAnim& a : Upper(run)) {
        if (a.label.find("UPPER_") == std::string::npos) continue;
        std::string d = a.label.rfind("ANIMPARAMLIST_UPPER_", 0) == 0 ? a.label.substr(20) : a.label;
        if (!out.run.count(d)) out.run[d] = a;
    }

    static const std::regex meleeRe(R"(SWORD|BLADE|KNUCKLE|CLAW|SCYTHE|BAT|KATANA|DAGGER|SHIELD|CARD|BOOTS|BREAKER|VITAL_SHOCK)",
                                     std::regex::icase);
    bool isMelee = type.empty() ? true : std::regex_search(type, meleeRe);

    const ActorFunc* dmgFn = Get(parsed, "DamageState_" + weapon);
    for (const ActorAnim& a : Upper(dmgFn)) {
        std::string d = a.label.rfind("ANIMPARAMLIST_", 0) == 0 ? a.label.substr(14) : a.label;
        static const std::string suf = "_WEAK";
        if (d.size() >= suf.size() && d.compare(d.size()-suf.size(), suf.size(), suf) == 0) d = d.substr(0, d.size()-suf.size());
        if (!out.dmg.count(d)) out.dmg[d] = a;
    }

    WeaponAttribs at = attribMap ? ResolveWeaponAttribs(*attribMap, type) : WeaponAttribs{};

    // ponytail: function-static scratch storage for the single-entry pointers
    // below - each ResolveWeaponAnim call overwrites it, so treat the returned
    // WeaponAnim as valid only until the next call (same "use it immediately"
    // pattern as scn::MakeNodeResolver). Upgrade to per-call owned storage if a
    // caller ever needs to keep two WeaponAnim results alive at once.
    static std::vector<ActorAnim> idleStore, strongStore, strong1Store, counterStore, jweakStore, reloadStore;
    idleStore = Upper(norm); out.idle = idleStore.empty() ? nullptr : &idleStore[0];
    out.atkWeak = Upper(weak); out.atkWeak.insert(out.atkWeak.end(), comboRest.begin(), comboRest.end());
    {
        std::vector<const ActorFunc*> fns(out.atkWeak.size() - comboRest.size(), weak);
        fns.insert(fns.end(), comboRestFns.begin(), comboRestFns.end());
        fns.resize(out.atkWeak.size(), weak);
        // NORMAL/DELAY pairs are one hit each (its swing, then its recovery):
        // the combo steps hit to hit, the DELAY row plays after the hit.
        std::vector<ActorAnim> hits, delays;
        std::vector<const ActorFunc*> hitFns;
        for (size_t i = 0; i < out.atkWeak.size(); ++i) {
            const ActorAnim& a = out.atkWeak[i];
            if (a.label.find("DELAY") != std::string::npos) { if (!hits.empty() && delays.back().clip.empty()) delays.back() = a; continue; }
            hits.push_back(a); delays.push_back(ActorAnim{}); hitFns.push_back(fns[i]);
        }
        if (!hits.empty()) { out.atkWeak = std::move(hits); out.atkWeakDelay = std::move(delays); out.fnWeakHits = std::move(hitFns); }
        else { out.atkWeakDelay.assign(out.atkWeak.size(), ActorAnim{}); out.fnWeakHits = std::move(fns); }
    }
    out.fnStrong = strong; out.fnStrong1 = strong1; out.fnCounter = counter; out.fnWeapon2 = weapon2;
    if (weapon2) for (const ActorAnim& a : weapon2->anims) if (a.label.find("CHANGE_ENABLE_") != std::string::npos) { out.weaponChange = true; break; }
    strongStore = Upper(strong); out.atkStrong = strongStore.empty() ? nullptr : &strongStore[0];
    strong1Store = Upper(strong1); out.atkStrong1 = strong1Store.empty() ? nullptr : &strong1Store[0];
    out.lowerWeak = Lower(weak); out.lowerStrong = Lower(strong); out.lowerStrong1 = Lower(strong1);
    counterStore = Upper(counter); out.atkCounter = counterStore.empty() ? nullptr : &counterStore[0];
    out.atkWeapon2 = Upper(weapon2);
    // SpyDagger exposes NORMAL_FIRST/NORMAL_SECOND as alternate normal slots,
    // followed by DELAY. They are not a three-stage animation sequence.
    if (out.atkWeapon2.size() >= 3 &&
        out.atkWeapon2[0].label.find("NORMAL_FIRST") != std::string::npos &&
        out.atkWeapon2[1].label.find("NORMAL_SECOND") != std::string::npos &&
        out.atkWeapon2[2].label.find("DELAY") != std::string::npos)
        out.atkWeapon2.erase(out.atkWeapon2.begin() + 1);
    out.sndWeapon2 = FirstSound(weapon2);
    out.seqWeapon2 = (weapon2 && !weapon2->seqs.empty()) ? &weapon2->seqs[0] : nullptr;
    out.jumpAtk = Upper(jatk);
    out.sndJumpAtk = FirstSound(jatk);
    out.sndJumpAtkWeak = FirstSound(jweak);
    jweakStore = Upper(jweak); out.jumpAtkWeak = jweakStore.empty() ? nullptr : &jweakStore[0];
    const ActorFunc* reload = Get(parsed, "ReloadState_" + weapon);
    reloadStore = Upper(reload); out.reload = reloadStore.empty() ? nullptr : &reloadStore[0];
    out.lowerReload = Lower(reload);

    out.isGun = !type.empty() && !isMelee;
    out.dmgWeak = at.weak ? at.weak->damage : 0;
    out.dmgStrong = at.strong ? at.strong->damage : 0;
    out.hitSoundWeak = at.weak ? at.weak->hitSound : "";
    out.hitSoundStrong = at.strong ? at.strong->hitSound : "";
    out.hitSeqWeak = at.weak ? at.weak->hitSeq : "";
    out.hitSeqStrong = at.strong ? at.strong->hitSeq : "";
    out.sndWeak = FirstSound(weak); out.sndStrong = FirstSound(strong);
    out.seqWeak = (weak && !weak->seqs.empty()) ? &weak->seqs[0] : nullptr;
    out.seqStrong = (strong && !strong->seqs.empty()) ? &strong->seqs[0] : nullptr;
    out.seqJumpAtk = (jatk && !jatk->seqs.empty()) ? &jatk->seqs[0] : nullptr;
    out.seqJumpAtkWeak = (jweak && !jweak->seqs.empty()) ? &jweak->seqs[0] : nullptr;
    out.fnJump = jatk; out.fnJumpWeak = jweak;
    auto ghostOf = [](const ActorFunc* f) -> const ActorGhostTrail* {
        // An empty name ("", 50, 600) = no trail.
        if (f) for (const ActorGhostTrail& g : f->ghosts) if (!g.name.empty()) return &g;
        return nullptr;
    };
    out.ghostWeak = ghostOf(weak); out.ghostStrong = ghostOf(strong); out.ghostStrong1 = ghostOf(strong1);
    out.ghostCounter = ghostOf(counter); out.ghostWeapon2 = ghostOf(weapon2);
    out.ghostJumpAtk = ghostOf(jatk); out.ghostJumpAtkWeak = ghostOf(jweak);
    out.gunBlastSeq = at.gunBlastSeq; out.gunBlastSound = at.gunBlastSound; out.gunFireBone = at.gunFireBone;
    out.gunWallSeq = at.gunWallSeq; out.gunWallSound = at.gunWallSound; out.gunTracerSeq = at.gunTracerSeq;
    out.gunHitSeq = at.gunHitSeq; out.gunHitSound = at.gunHitSound;
    return out;
}

const std::unordered_map<std::string, int> kRunSlot = {
    {"FRONT",8}, {"LEFT_FRONT",9}, {"LEFT",10}, {"LEFT_BACK",11},
    {"RIGHT_FRONT",12}, {"RIGHT",13}, {"RIGHT_BACK",14}, {"BACK",15},
};

std::vector<SAnimParam> ToSAnimParams(const std::vector<ActorAnim>& anims) {
    std::vector<SAnimParam> out;
    for (const ActorAnim& a : anims) {
        SAnimParam p; p.clip = a.clip; p.startMs = a.startMs; p.blendInMs = a.blendInMs; p.blendOutMs = a.blendOutMs;
        p.rate = a.rate; p.reset = a.reset; p.loop = a.loop;
        out.push_back(p);
    }
    return out;
}
static std::vector<SAnimParam> ToParams(const std::vector<ActorAnim>& anims) { return ToSAnimParams(anims); }
// RunState_<Weapon> carries 16 rows: ANIMPARAMLIST_UPPER_<dir> (torso, slots
// 0..7) and ANIMPARAMLIST_LOWER_<dir> (legs, slots 8..15), by direction NAME -
// the SetAnim calls are not written in slot order. They differ for the guns:
// MachineGun's torso stays on F0018 (aiming) in every direction while its legs
// go F0018..F0025; reading only the UPPER rows into both halves made the legs
// run forward while strafing. A missing half falls back to the other one.
static std::vector<SAnimParam> RunTable(const ActorFunc* runFn) {
    std::vector<SAnimParam> run(16);
    std::vector<bool> have(16, false);
    for (const ActorAnim& a : Upper(runFn)) {
        int base;
        std::string d;
        if (a.label.rfind("ANIMPARAMLIST_UPPER_", 0) == 0)      { base = 0; d = a.label.substr(20); }
        else if (a.label.rfind("ANIMPARAMLIST_LOWER_", 0) == 0) { base = 8; d = a.label.substr(20); }
        else continue;
        auto it = kRunSlot.find(d);
        if (it == kRunSlot.end()) continue;
        SAnimParam p; p.clip = a.clip; p.startMs = a.startMs; p.blendInMs = a.blendInMs; p.blendOutMs = a.blendOutMs;
        p.rate = a.rate; p.reset = a.reset; p.loop = a.loop;
        const int slot = (it->second - 8) + base;
        run[slot] = p; have[slot] = true;
    }
    for (int i = 0; i < 8; ++i) {
        if (!have[i] && have[i + 8]) run[i] = run[i + 8];
        if (!have[i + 8] && have[i]) run[i + 8] = run[i];
    }
    return run;
}

std::unordered_map<std::string, std::vector<SAnimParam>> BaseParams(const ParsedActorStates& parsed) {
    return {
        { "NORMAL",    ToParams(Upper(Get(parsed, "NormalState_WeaponUnused"))) },
        { "RUN",       RunTable(Get(parsed, "RunState_WeaponUnused")) },
        { "SIT",       ToParams(Upper(Get(parsed, "ActorState_Sit"))) },
        { "JUMP",      ToParams(Upper(Get(parsed, "ActorState_Jump"))) },
        { "BOUNDJUMP", ToParams(Upper(Get(parsed, "ActorState_BoundJump"))) },
        { "FALL",      ToParams(Upper(Get(parsed, "ActorState_Fall"))) },
        { "DODGE",     ToParams(Upper(Get(parsed, "ActorState_Dodge"))) },
        { "FASTRUN",   ToParams(Upper(Get(parsed, "ActorState_FastRun"))) },
    };
}

std::unordered_map<std::string, std::vector<SAnimParam>> WeaponDefs(const ParsedActorStates& parsed, const std::string& weapon) {
    std::unordered_map<std::string, std::vector<SAnimParam>> out;
    std::vector<ActorAnim> norm = Upper(Get(parsed, "NormalState_" + weapon));
    if (!norm.empty()) out["NORMAL"] = ToParams(norm);
    const ActorFunc* runFn = Get(parsed, "RunState_" + weapon);
    if (runFn) out["RUN"] = RunTable(runFn);
    return out;
}

std::unordered_map<int, BlendSlot> WeaponBlend(const ParsedActorStates& parsed, const std::string& funcName) {
    std::unordered_map<int, BlendSlot> out;
    const ActorFunc* fn = Get(parsed, funcName);
    if (!fn || fn->blend.empty()) return out;
    for (const ActorBlend& b : fn->blend) {
        if (b.blender < 0) continue;
        BlendSlot& slot = out[b.blender];
        if (b.kind == "Walk") (b.part == "Upper" ? slot.upper : slot.lower)[b.dir] = b;
        // GetJump*/GetFly*AnimParam: the aim blend while rising / falling,
        // keyed "JUMP" / "FLY" next to the walking directions.
        else if (b.kind == "Jump" || b.kind == "Fly")
            (b.part == "Upper" ? slot.upper : slot.lower)[b.kind == "Jump" ? "JUMP" : "FLY"] = b;
    }
    return out;
}

const BlendSlot* WeaponFireBlendSlot(const ParsedActorStates& parsed, const std::string& weapon) {
    static std::unordered_map<int, BlendSlot> keepAlive;
    // UseWeapon1State_<X>Attack / <X>Gun (smash rifle: SMG3Gun) / <X>, then any
    // UseWeapon1State_<X><non-digit>... that has a blender (SMG must not take SMG3).
    const std::string pre = "UseWeapon1State_" + weapon;
    std::vector<std::string> names = { pre + "Attack", pre + "Gun", pre };
    for (const auto& kv : parsed)
        if (kv.first.size() > pre.size() && kv.first.compare(0, pre.size(), pre) == 0 &&
            !std::isdigit((unsigned char)kv.first[pre.size()]) && !kv.second.blend.empty()) names.push_back(kv.first);
    for (const std::string& fnName : names) {
        keepAlive = WeaponBlend(parsed, fnName);
        if (keepAlive.empty()) continue;
        int best = -1;
        for (auto& kv : keepAlive) if (kv.first > best) best = kv.first;
        if (best >= 0 && !keepAlive[best].upper.empty()) return &keepAlive[best];
    }
    return nullptr;
}

std::unordered_map<std::string, ActorAnim> WeaponFireLower(const ParsedActorStates& parsed, const std::string& weapon) {
    const std::string pre = "UseWeapon1State_" + weapon;
    std::vector<std::string> names = { pre + "Attack", pre + "Gun", pre };
    for (const auto& kv : parsed)
        if (kv.first.size() > pre.size() && kv.first.compare(0, pre.size(), pre) == 0 &&
            !std::isdigit((unsigned char)kv.first[pre.size()]) && !kv.second.blend.empty()) names.push_back(kv.first);
    for (const std::string& n : names) {
        const ActorFunc* fn = Get(parsed, n);
        if (fn) {
            auto lower = Lower(fn);
            if (!lower.empty()) return lower;
        }
    }
    return {};
}

const std::unordered_map<std::string, ActorBlend>* WeaponFireBlend(const ParsedActorStates& parsed, const std::string& weapon) {
    static std::unordered_map<int, BlendSlot> keepAlive;   // caller uses the pointer immediately, same as JS
    for (const std::string& fnName : { "UseWeapon1State_" + weapon + "Attack", "UseWeapon1State_" + weapon }) {
        keepAlive = WeaponBlend(parsed, fnName);
        if (keepAlive.empty()) continue;
        int best = -1;
        for (auto& kv : keepAlive) if (kv.first > best) best = kv.first;
        if (best >= 0 && !keepAlive[best].upper.empty()) return &keepAlive[best].upper;
    }
    return nullptr;
}

scn::Pose FireBlendPose(const ActorBlend* entry, float t) {
    if (!entry) return scn::Pose{};
    float since = float(entry->startMs);
    scn::Pose pose;
    if (t < since) { pose.clipA = entry->clipA; pose.tickA = (uint32_t)t; pose.blend = 0.f; return pose; }
    float bt = entry->blendInMs > 0 ? (std::min)(1.f, (t - since) / float(entry->blendInMs)) : 1.f;
    pose.clipA = entry->clipA; pose.tickA = (uint32_t)t;
    pose.clipB = entry->clipB; pose.tickB = (uint32_t)(t - since);
    pose.blend = bt;
    return pose;
}

bool LuaStates::Load(const char* scriptDir) {
    parsed_ = ParseActorStates(scriptDir);
    attackMap_ = ParseWeaponAttackMap(scriptDir);
    attribMap_ = ParseAttackAttribs(scriptDir);
    loaded_ = !parsed_.empty();
    return loaded_;
}

std::vector<std::string> LuaStates::Weapons(const char* prefix) const {
    std::vector<std::string> out;
    std::string p = prefix;
    for (auto& kv : parsed_)
        if (kv.first.rfind(p, 0) == 0) out.push_back(kv.first.substr(p.size()));
    return out;
}

}  // namespace anim
