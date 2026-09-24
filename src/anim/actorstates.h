// actorstates_*.lua parser, ported 1:1 from C:\S4Web2\engine\scn_actorstates.js.
// Does NOT interpret Lua - states are declarative data: calls to
//   Get[Walk]AnimParam(LABEL):SetAnim("clip", startMs, blendInMs, blendOutMs, LOOP_x, rate, RESET_x)
// plus AddSound/AddSoundBySex/AddSequence/GetCustomAnimBlender/SetBlendingAnim,
// extracted per-function with regex.
//
// Verified against the real Lua->C++ binding FUN_01653BC0.
#pragma once
#include "anim_states.h"
#include "scn/skin.h"
#include <string>
#include <unordered_map>
#include <vector>

namespace anim {

struct ActorAnim {
    std::string label, clip;
    int startMs = 0, blendInMs = 0, blendOutMs = 0;
    bool loop = false;
    float rate = 1.f;
    bool reset = false;
};
struct ActorSound {
    bool bySex = false;
    std::string man, girl, bone;
    int start = 0, end = 0;
};
struct ActorSeq {
    int index = 0;
    std::string seq, sound;
    int startMs = 0;
};
struct ActorBlend {
    int blender = -1;
    std::string kind;   // Walk/Jump/Fly
    std::string part;   // Upper/Lower
    std::string dir;    // FRONT.. (empty for Jump/Fly)
    std::string clipA, clipB;
    int startMs = 0, blendInMs = 0, blendOutMs = 0;
    bool loop = false;
    float rate = 1.f;
    bool reset = false;
};
struct ActorFunc {
    std::vector<ActorAnim> anims;
    std::vector<ActorSound> sounds;
    std::vector<ActorSeq> seqs;
    std::vector<ActorBlend> blend;
};

using ParsedActorStates = std::unordered_map<std::string, ActorFunc>;

// Parses every actorstates_*.lua in scriptDir into { funcName -> ActorFunc }.
ParsedActorStates ParseActorStates(const std::string& scriptDir);

// Run direction from raw input (native LH, inZ>0=front) - same mapping as anim_controller's Gate(RUN).
std::string RunDir(float inX, float inZ);

struct AttackAttrib {
    int damage = 0;
    std::string power, weaponType;
    std::string hitSeq, hitSound, atkSeq, atkSound, killImg;
    std::string gunBlastSeq, gunBlastSound, gunFireBone;
};
using AttackAttribMap = std::unordered_map<std::string, AttackAttrib>;
AttackAttribMap ParseAttackAttribs(const std::string& scriptDir);

struct WeaponAttribs {
    const AttackAttrib* weak = nullptr;
    const AttackAttrib* strong = nullptr;
    std::string gunBlastSeq, gunBlastSound, gunFireBone;
};
WeaponAttribs ResolveWeaponAttribs(const AttackAttribMap& attribMap, const std::string& weaponType);

struct WeaponAttackMap {
    std::unordered_map<std::string, std::string> typeBySuffix;
    std::unordered_map<std::string, std::string> weakByType, strongByType, strong1ByType, counterByType;
    std::unordered_map<std::string, std::string> attack2ByType, attack3ByType, attack4ByType, attack5ByType;
    std::unordered_map<std::string, std::string> use1ByType, use2ByType;
};
WeaponAttackMap ParseWeaponAttackMap(const std::string& scriptDir);

struct WeaponAnim {
    const ActorAnim* idle = nullptr;
    std::unordered_map<std::string, ActorAnim> run;              // dir -> param
    std::vector<ActorAnim> atkWeak;                               // full combo hit1..5
    const ActorAnim* atkStrong = nullptr;
    const ActorAnim* atkStrong1 = nullptr;
    // Legs keep playing their OWN walk/stand clip (keyed by direction, "NORMAL"
    // for standing still) while the upper body plays weak/strong - real S4
    // architecture (state:GetLowerPartStateSet(0):GetWalkAnimParam(dir):SetAnim),
    // not an approximation. Empty map = this weapon's swing has no lower-body
    // data of its own (fall back to whatever locomotion is already playing).
    std::unordered_map<std::string, ActorAnim> lowerWeak, lowerStrong, lowerStrong1;
    const ActorAnim* atkCounter = nullptr;
    std::vector<ActorAnim> atkWeapon2;                             // full phase sequence (wind-up -> held pose, ...)
    const ActorSound* sndWeapon2 = nullptr;
    const ActorSound* sndJumpAtk = nullptr;
    const ActorSound* sndJumpAtkWeak = nullptr;
    const ActorSeq* seqWeapon2 = nullptr;
    std::vector<ActorAnim> jumpAtk;  // NORMAL -> DELAY; do not discard the landing/recovery clip
    const ActorAnim* jumpAtkWeak = nullptr;
    const ActorAnim* reload = nullptr;
    std::unordered_map<std::string, ActorAnim> dmg;               // FRONT/BACK/LEFT/RIGHT
    bool isGun = false;
    int dmgWeak = 0, dmgStrong = 0;
    std::string hitSoundWeak, hitSoundStrong, hitSeqWeak, hitSeqStrong;
    const ActorSound* sndWeak = nullptr; const ActorSound* sndStrong = nullptr;
    const ActorSeq* seqWeak = nullptr; const ActorSeq* seqStrong = nullptr;
    const ActorSeq* seqJumpAtk = nullptr; const ActorSeq* seqJumpAtkWeak = nullptr;
    std::string gunBlastSeq, gunBlastSound, gunFireBone;
};
// Owns the ActorFunc storage that WeaponAnim's pointers reference - keep this
// alive as long as the returned WeaponAnim is used (same lifetime contract as
// `parsed` in the JS: weaponAnim(parsed, ...) borrows from it).
WeaponAnim ResolveWeaponAnim(const ParsedActorStates& parsed, const std::string& weapon,
                              const WeaponAttackMap* attackMap, const AttackAttribMap* attribMap);

// dir -> RUN gate slot (same mapping as anim_controller's Gate(RUN)).
extern const std::unordered_map<std::string, int> kRunSlot;

// Base (unarmed) state tables, in AnimController::LoadDefs format.
std::unordered_map<std::string, std::vector<SAnimParam>> BaseParams(const ParsedActorStates& parsed);
// Per-weapon NORMAL/RUN overrides, same format, merge on top of BaseParams via LoadDefs.
std::unordered_map<std::string, std::vector<SAnimParam>> WeaponDefs(const ParsedActorStates& parsed, const std::string& weapon);

// GetCustomAnimBlender blocks of one function, grouped by blender index and direction.
struct BlendSlot { std::unordered_map<std::string, ActorBlend> upper, lower; };
std::unordered_map<int, BlendSlot> WeaponBlend(const ParsedActorStates& parsed, const std::string& funcName);

// Fire-hold pose per direction for guns with GetCustomAnimBlender. Returns
// nullptr if the weapon has no such blend.
const std::unordered_map<std::string, ActorBlend>* WeaponFireBlend(const ParsedActorStates& parsed, const std::string& weapon);

// Composes the fire-hold pose at time `t` (ms since fire/aim started).
scn::Pose FireBlendPose(const ActorBlend* entry, float t);

// ActorAnim rows -> AnimController::LoadDefs SAnimParam rows.
std::vector<SAnimParam> ToSAnimParams(const std::vector<ActorAnim>& anims);

// Legacy facade (pre-1:1-port scenes.cpp/anim_controller.cpp): parses and holds
// every actorstates_*.lua of a script directory plus the weapon-attack/attrib
// tables, in one call.
class LuaStates {
public:
    bool Load(const char* scriptDir);
    bool loaded() const { return loaded_; }
    size_t size() const { return parsed_.size(); }
    // Suffixes of functions "<prefix><suffix>" (e.g. Weapons("NormalState_") ->
    // "PlasmaSword", "WeaponUnused", ...).
    std::vector<std::string> Weapons(const char* prefix) const;

    const ParsedActorStates& parsed() const { return parsed_; }
    const WeaponAttackMap& attackMap() const { return attackMap_; }
    const AttackAttribMap& attribMap() const { return attribMap_; }

private:
    ParsedActorStates parsed_;
    WeaponAttackMap attackMap_;
    AttackAttribMap attribMap_;
    bool loaded_ = false;
};

}  // namespace anim
