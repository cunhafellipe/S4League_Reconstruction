#pragma once
// lua_weapon_fire.h — corre los Weapon_*.lua REALES (loops/RNG/rotaciones de vector) con Lua
// de verdad (third_party/lua-5.1.5), no un parser de texto. lua_states.h/LuaStates (regex,
// portado de scn_actorstates.js) alcanza para las funciones DECLARATIVAS de los actorstates_*.lua
// (solo llamadas SetAnim/AddSound en secuencia) pero no para estas: tienen loops, math.random,
// rotacion de vectores (MAKECVECTOR3:RotateY/RotateZ) - hace falta ejecutarlas de verdad para
// sacar el patron de spread/burst real en vez de un raycast recto fijo.
//
// El shim de la API C++ que el juego inyecta (MAKECVECTOR3/CreateCAttack/AddAttack/etc) esta en
// third_party/lua_weapon_shim.lua (compartido 1:1 con el motor JS de WebS4League, es Lua puro).
#include <string>
#include <vector>

namespace luaw {

struct AttackShape {
    std::string kind;               // "ray" | "sphere" | "box" | "cylinder" | "fan" | "pole" | "thickray" | "cone"
    float centerX = 0, centerY = 0, centerZ = 0;
    float dirX = 0, dirY = 0, dirZ = 0;
    float length = 0, radius = 0, width = 0, height = 0, angle = 0;
};

struct WeaponAttack {
    bool        explosion = false;
    std::string weaponType;         // WEAPONTYPE_* (nombre, no el indice real - ver lua_weapon_fire.cpp)
    std::string attackAttrib;       // ATTACKATTRIB_* (idem, matchear por NOMBRE contra attackattribindexer.lua)
    float       fireTime = 0;       // ms desde que se aprieta el gatillo
    float       power = 0;
    float       critMult = 0, critChance = 0;
    AttackShape shape;
};

// STATE:Set* fields the attack function sets on itself before ever touching
// ATTACKS - these apply to the whole swing, not to one hitbox.
struct WeaponFireState {
    bool loaded = false; // false on missing function or Lua error; never use partial results
    // InitLua is overloaded per native state. Only the jump-attack consumer
    // interprets [normalTime, delayTime, minimumSceneUpdateTime, onlyVStateNormal].
    std::vector<float> initArgs;
    bool hasDrop = false;
    float dropTime = 0.f, dropX = 0.f, dropY = 0.f, dropZ = 0.f;
    float moveSpeedRate = 1.f;
    float moveBoxMultiplier = 1.f;
    float hitStopA = 0.f, hitStopB = 0.f;
    bool  stealthCancel = false;
};

// Corre <funcName>( OneShotTime, Power, Accuracy, AdditionalPower ) de <luaFile> (uno real de
// resources/script/Weapon_*.lua) y devuelve la lista de ataques que ESE arma calcularia para
// una rafaga/gatillazo completo. scriptDir = carpeta real de resources/script (misma que usa
// LuaStates::Load). Devuelve vector vacio si el archivo/funcion no existe o hubo error de Lua
// (se loguea via DbgLog).
// shimPath = ruta completa a lua_weapon_shim.lua (third_party/lua_weapon_shim.lua en el repo).
// outState, if given, is filled with whatever the run's STATE:Set* calls left behind.
std::vector<WeaponAttack> RunWeaponFire(const char* scriptDir, const char* shimPath, const char* luaFile,
                                         const char* funcName, float oneShotTime, float power,
                                         float accuracy, float additionalPower,
                                         WeaponFireState* outState = nullptr);

}  // namespace luaw
