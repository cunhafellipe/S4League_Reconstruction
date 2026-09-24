// Ported 1:1 from C:\S4Web2\engine\scn_actorstates.js.
#include "actorstates.h"
#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <regex>
#include <sstream>

namespace fs = std::filesystem;

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
static const std::regex kBlenderRe(R"(customAnimBlender\s*=\s*state\s*:\s*GetCustomAnimBlender\s*\(\s*(\d+)\s*\))");
static const std::regex kBlendRe(
    R"re(customAnimBlender\s*:\s*Get(Walk|Jump|Fly)(Upper|Lower)AnimParam\s*\(\s*(LOWER_ANIM_\w+)?\s*\)\s*:\s*SetBlendingAnim\s*\(\s*"([^"]*)"\s*,\s*"([^"]*)"\s*,\s*(\d+)\s*,\s*(\d+)\s*,\s*(\d+)\s*,\s*(LOOP_\w+)\s*,\s*([\d.]+)\s*,\s*(RESET_\w+)\s*\))re");
static const std::regex kFnRe(R"(function\s+(\w+)\s*\()");

static std::string StripLowerAnimPrefix(const std::string& s) {
    static const std::string pfx = "LOWER_ANIM_";
    return s.rfind(pfx, 0) == 0 ? s.substr(pfx.size()) : s;
}

ParsedActorStates ParseActorStates(const std::string& scriptDir) {
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
                b.loop = m[9].str() == "LOOP_TRUE"; b.rate = std::stof(m[10].str()); b.reset = m[11].str() == "RESET_TRUE";
                fn.blend.push_back(std::move(b));
            }
            if (!fn.anims.empty() || !fn.sounds.empty() || !fn.seqs.empty() || !fn.blend.empty())
                out[marks[i].name] = std::move(fn);
        }
    }
    return out;
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
static std::vector<ActorAnim> Upper(const ActorFunc* fn) {
    std::vector<ActorAnim> out;
    if (!fn) return out;
    for (const ActorAnim& a : fn->anims)
        if (a.label.rfind("ANIMPARAMLIST_", 0) == 0 && a.clip != "base") out.push_back(a);
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
    static const std::regex gtRe(R"re(SetGunTracerAttachNode\s*\(\s*"([^"]*)")re");
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
        if (std::regex_search(body, m, dsRe)) { a.hitSeq = m[1].str(); a.hitSound = m[3].str(); }
        if (std::regex_search(body, m, asRe)) { a.atkSeq = m[1].str(); a.atkSound = m[2].str(); }
        Match1(body, kiRe, a.killImg);
        bool haveGb = std::regex_search(body, m, gbRe);
        if (haveGb) { a.gunBlastSeq = m[1].str(); a.gunBlastSound = m[3].str(); a.gunFireBone = m[4].str(); }
        else Match1(body, gtRe, a.gunFireBone);
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
    if (gunEntry) { out.gunBlastSeq = gunEntry->gunBlastSeq; out.gunBlastSound = gunEntry->gunBlastSound; out.gunFireBone = gunEntry->gunFireBone; }
    return out;
}

WeaponAttackMap ParseWeaponAttackMap(const std::string& scriptDir) {
    WeaponAttackMap out;
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
                              const WeaponAttackMap* attackMap, const AttackAttribMap* attribMap) {
    WeaponAnim out;
    const ActorFunc* norm = Get(parsed, "NormalState_" + weapon);
    const ActorFunc* run  = Get(parsed, "RunState_" + weapon);
    const ActorFunc* weak = Get(parsed, "SwordAtkWeakState_" + weapon);
    const ActorFunc* strong = Get(parsed, "SwordAtkStrongState_" + weapon);
    const ActorFunc* strong1 = nullptr; const ActorFunc* counter = nullptr; const ActorFunc* weapon2 = nullptr;
    std::vector<ActorAnim> comboRest;

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
                }
            }
        }
    }
    const ActorFunc* jatk = Get(parsed, "SwordJumpAtkState_" + weapon);
    const ActorFunc* jweak = Get(parsed, "SwordJumpAtkWeakState_" + weapon);

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
    reloadStore = Upper(Get(parsed, "ReloadState_" + weapon)); out.reload = reloadStore.empty() ? nullptr : &reloadStore[0];

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
    out.gunBlastSeq = at.gunBlastSeq; out.gunBlastSound = at.gunBlastSound; out.gunFireBone = at.gunFireBone;
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
static std::vector<SAnimParam> RunTable(const ActorFunc* runFn) {
    std::vector<SAnimParam> run(16);
    for (const ActorAnim& a : Upper(runFn)) {
        if (a.label.find("UPPER_") == std::string::npos) continue;
        std::string d = a.label.rfind("ANIMPARAMLIST_UPPER_", 0) == 0 ? a.label.substr(20) : a.label;
        auto it = kRunSlot.find(d);
        if (it == kRunSlot.end()) continue;
        SAnimParam p; p.clip = a.clip; p.startMs = a.startMs; p.blendInMs = a.blendInMs; p.blendOutMs = a.blendOutMs;
        p.rate = a.rate; p.reset = a.reset; p.loop = a.loop;
        run[it->second] = p; run[it->second - 8] = p;
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
        // jumpUpper/jumpLower/flyUpper/flyLower not stored separately here (unused
        // by WeaponFireBlend, which only reads .upper); kept out of BlendSlot to
        // match the JS shape actually consumed downstream.
    }
    return out;
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
