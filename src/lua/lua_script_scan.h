// Reads the client's data scripts by RUNNING them, so a plain-text script and a
// compiled one (the vanilla resources ship every .lua as Lua 5.1 bytecode,
// "\x1bLuaQ") give the same answer. The regex readers in anim/actorstates.cpp,
// the weapon function finder and the constants readers use these when the
// file is bytecode.
//
// Every script runs in a sandbox state: an undefined global is an inert,
// callable object (or its own name for the *_ constants), `require` records
// the file it names and loads nothing.
#pragma once
#include "anim/actorstates.h"
#include <string>
#include <utility>
#include <vector>

namespace luaw {

// True when the file starts with the Lua bytecode signature.
bool IsBytecodeFile(const std::string& path);

struct ScriptScan {
    bool ok = false;                       // the chunk loaded (its top level may still have stopped early)
    std::vector<std::string> functions;    // global Lua functions the file defines
    std::vector<std::string> requires_;    // files it require()s, as written
};
// Cached per path.
const ScriptScan& ScanScript(const std::string& path);

// The numeric globals of a constants file (ActorStates_Constants.lua) with
// their values, in value order: STATE_*, WEAPONTYPE_*, ATTACKATTRIB_*...
std::vector<std::pair<std::string, double>> ConstantsOf(const std::string& path);

// actorstates_gameplay.lua's dispatch functions (ActorState_Normal,
// UseWeapon1_Weak, ...) -> which state function each weapon type calls.
bool ReadWeaponAttackMap(const std::string& gameplayFile, anim::WeaponAttackMap& out);
// attackattribindexer.lua: every GetAttackAttrib(...) block.
bool ReadAttackAttribs(const std::string& indexerFile, anim::AttackAttribMap& out);

}  // namespace luaw
