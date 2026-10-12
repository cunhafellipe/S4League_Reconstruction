// resources/xml/weapon_attach.x7 parser, ported 1:1 from
// C:\S4Web2\engine\weapon_attach.js. The OFFICIAL weapon-piece -> bone table
// the client itself reads (CWeaponAttachInfoManager, FUN_016738d0). Replaces
// guessing the bone from a file suffix (_l/_r), which sent pieces that
// actually belong on the forearm to L_Hand_Dummy.
#pragma once
#include <optional>
#include <string>
#include <unordered_map>

namespace data {

struct WeaponAttachEntry { std::string attack; int link = 0; std::string idle; bool hasIdle = false; };

// (weaponType, sceneIndex) -> attach entry, loaded from resourcesPath/xml/weapon_attach.x7.
const std::unordered_map<std::string, WeaponAttachEntry>& LoadWeaponAttach(const std::string& resourcesPath);

// WEAPONTYPE_* -> numeric id, parsed from resources/script/actorstates_constants.lua
// (the block starts at index=-1: UNUSED=-1, 3_SWORD=0, COUNTER_SWORD=1, ...).
const std::unordered_map<std::string, int>& WeaponTypeIds(const std::string& resourcesPath);

// Weapon type number from a Lua state suffix ('CounterSword', 'PlasmaSword', ...),
// via typeBySuffix (anim::WeaponAttackMap::typeBySuffix).
std::optional<int> TypeFromSuffix(const std::string& resourcesPath, const std::string& suffix,
                                   const std::unordered_map<std::string, std::string>& typeBySuffix);

// Bone the weapon's `sceneIndex` piece attaches to for weapon `type`.
// idle = holstered (back/waist) instead of in-hand. Returns empty if not found.
std::string AttachBone(const std::string& resourcesPath, int type, int sceneIndex, bool idle);
// link of that piece: 0 = CWeapon::LoadWeaponModel (hangs by its Wea_Dummy,
// hand <-> holster), 1 = CWeapon::LoadAttachWeaponModel (the whole SCN root
// hierarchy hangs from the bone as authored - fists, forearm parts, cards).
// -1 if the table has no entry.
int AttachLink(const std::string& resourcesPath, int type, int sceneIndex);

}  // namespace data
