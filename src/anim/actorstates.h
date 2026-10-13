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
    std::string seqGirl;   // AddSequenceBySex: the female version (seq = male)
    std::string kind;      // "" AddSequence, "bysex" AddSequenceBySex, "self" AddSequenceBySelf
};
// state:AddInstantEffect(node, seq, sound, start, range): an effect on a node
// of the worn item (Iron Boots boosters).
struct ActorInstantEffect {
    std::string node, seq, sound;
    int startMs = 0;
};
// state:AddGhostTrail("GhostTrail_<weapon>_NN", startMs, endMs): the weapon's
// Line01 leaves a trail from startMs to endMs of the state (beta CActorState
// binding; the trail itself is CWeapon's MakeGhostTrailNode on "Line01").
struct ActorGhostTrail {
    std::string name;
    int startMs = 0, endMs = 0;
};
// GetAnimParam(i):SetAniNameForHuman(clip, alt, flag) - the Death Scythe's
// per-sex clip swap (GR000 -> GR000_1).
struct ActorHumanName {
    std::string label, clip, alt;
    bool flag = false;
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
    std::vector<ActorGhostTrail> ghosts;
    std::vector<ActorBlend> blend;
    std::vector<ActorHumanName> human;
    std::vector<ActorSound> transSounds;          // AddSoundTransBySex (transformed sigma)
    std::vector<ActorInstantEffect> instant;      // AddInstantEffect
    float ironBootsSpeedGhost = 1.f, ironBootsSpeedSelf = 1.f;   // SetAniPlaySpeed_IronBoots
};

using ParsedActorStates = std::unordered_map<std::string, ActorFunc>;

// Sigma shape change: while on, the resolvers below (ResolveWeaponAnim, the
// controller's LoadFromLua) read every state's transformed rows.
void SetShapeChange(bool on);
bool ShapeChange();

// Every actorstates_*.lua in scriptDir into { funcName -> ActorFunc }: the
// scripts are RUN (luaw::RunActorStates, real Lua with the client's calls
// recorded); the regex reader below is only the fallback if Lua cannot load.
ParsedActorStates ParseActorStates(const std::string& scriptDir);
ParsedActorStates ParseActorStatesRegex(const std::string& scriptDir);

// Run direction from raw input (native LH, inZ>0=front) - same mapping as anim_controller's Gate(RUN).
std::string RunDir(float inX, float inZ);

struct AttackAttrib {
    int damage = 0;
    std::string power, weaponType;
    std::string hitSeq, hitSound, atkSeq, atkSound, killImg;
    std::string gunBlastSeq, gunBlastSound, gunFireBone;
    std::string wallHitSeq, wallHitSound;   // SetDamageSequence args 2 and 4 (bullet on a wall)
    std::string gunTracerSeq;               // SetGunTracerAttachNode arg 2 (the beam muzzle -> hit)
};
using AttackAttribMap = std::unordered_map<std::string, AttackAttrib>;
AttackAttribMap ParseAttackAttribs(const std::string& scriptDir);

struct WeaponAttribs {
    const AttackAttrib* weak = nullptr;
    const AttackAttrib* strong = nullptr;
    std::string gunBlastSeq, gunBlastSound, gunFireBone, gunWallSeq, gunWallSound, gunTracerSeq, gunHitSeq, gunHitSound;
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
    // Per combo hit: its DELAY row (counter sword AfterDash1..4: R0000 hit,
    // R0025 recovery). Same size as atkWeak; empty clip = none (plasma's
    // FIRST/SECOND/THIRD are hits only).
    std::vector<ActorAnim> atkWeakDelay;
    // The state function behind each attack (all its AddSequence / AddInstantEffect /
    // AddGhostTrail): per combo hit, and per held / jump / right-click action.
    std::vector<const ActorFunc*> fnWeakHits;
    bool weaponChange = false;   // right click is a shape change (sigma blade: CHANGE_ENABLE / CHANGE_DISABLE)
    const ActorFunc* fnStrong = nullptr; const ActorFunc* fnStrong1 = nullptr; const ActorFunc* fnCounter = nullptr;
    const ActorFunc* fnWeapon2 = nullptr; const ActorFunc* fnJump = nullptr; const ActorFunc* fnJumpWeak = nullptr;
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
    std::unordered_map<std::string, ActorAnim> lowerReload;
    std::unordered_map<std::string, ActorAnim> dmg;               // FRONT/BACK/LEFT/RIGHT
    bool isGun = false;
    int dmgWeak = 0, dmgStrong = 0;
    std::string hitSoundWeak, hitSoundStrong, hitSeqWeak, hitSeqStrong;
    const ActorSound* sndWeak = nullptr; const ActorSound* sndStrong = nullptr;
    const ActorSeq* seqWeak = nullptr; const ActorSeq* seqStrong = nullptr;
    const ActorSeq* seqJumpAtk = nullptr; const ActorSeq* seqJumpAtkWeak = nullptr;
    // state:AddGhostTrail per attack state (the blade trail and its window).
    const ActorGhostTrail* ghostWeak = nullptr; const ActorGhostTrail* ghostStrong = nullptr;
    const ActorGhostTrail* ghostStrong1 = nullptr; const ActorGhostTrail* ghostCounter = nullptr;
    const ActorGhostTrail* ghostWeapon2 = nullptr;
    const ActorGhostTrail* ghostJumpAtk = nullptr; const ActorGhostTrail* ghostJumpAtkWeak = nullptr;
    std::string gunBlastSeq, gunBlastSound, gunFireBone, gunWallSeq, gunWallSound, gunTracerSeq;
    std::string gunHitSeq, gunHitSound;   // SetDamageSequence args 1 and 3: a bullet on a player
};
// Owns the ActorFunc storage that WeaponAnim's pointers reference - keep this
// alive as long as the returned WeaponAnim is used (same lifetime contract as
// `parsed` in the JS: weaponAnim(parsed, ...) borrows from it).
// The item's own weapon_lua.x7 rows: state ("left_weak", "left_strong1",
// "left_jump", "right_attack", "left_attack2"...) -> the weapon_*.lua attack
// function (weapon=) and the actorstates_*.lua function (ani=). When given,
// they name every state's function; the type tables only fill what is missing.
struct ItemLuaRow { std::string weapon, ani; };
using ItemLuaRows = std::unordered_map<std::string, ItemLuaRow>;
WeaponAnim ResolveWeaponAnim(const ParsedActorStates& parsed, const std::string& weapon,
                              const WeaponAttackMap* attackMap, const AttackAttribMap* attribMap,
                              const ItemLuaRows* rows = nullptr);

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
// Both halves of that blender: Get*UpperAnimParam = aiming UP (A straight up,
// B level), Get*LowerAnimParam = aiming DOWN (A straight down, B level) -
// CCustomAnimBlender::Update (beta sub_723DC0). nullptr if none.
const BlendSlot* WeaponFireBlendSlot(const ParsedActorStates& parsed, const std::string& weapon);
// The same fire state's lowerPartStateSet rows (LOWER_ANIM_<dir> -> clip):
// the legs while firing - E0008.. walking, AS002 (NORMAL) standing.
std::unordered_map<std::string, ActorAnim> WeaponFireLower(const ParsedActorStates& parsed, const std::string& weapon);

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
    const ActorFunc* Func(const std::string& name) const { auto it = parsed_.find(name); return it == parsed_.end() ? nullptr : &it->second; }
    const WeaponAttackMap& attackMap() const { return attackMap_; }
    // A weapon suffix the dispatch tables do not list (ASSULT_RIFLE, HOMING_RIFLE,
    // SPARK_RIFLE, TURRET, ...): register the WEAPONTYPE_* the item's own
    // <ability type> gives, so its attacks, attribute and gun state resolve.
    void AddTypeSuffix(const std::string& suffix, const std::string& typeName) {
        if (!suffix.empty() && !typeName.empty() && !attackMap_.typeBySuffix.count(suffix)) attackMap_.typeBySuffix[suffix] = typeName;
    }
    const AttackAttribMap& attribMap() const { return attribMap_; }

private:
    ParsedActorStates parsed_;
    WeaponAttackMap attackMap_;
    AttackAttribMap attribMap_;
    bool loaded_ = false;
};

}  // namespace anim
