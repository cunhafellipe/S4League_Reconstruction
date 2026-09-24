#include "lua_weapon_fire.h"
#include "../app/dbg.h"

extern "C" {
#include "lua.h"
#include "lauxlib.h"
#include "lualib.h"
}

#include <cstdio>
#include <cstring>
#include <fstream>
#include <sstream>
#include <set>

namespace luaw {

// Lee un archivo entero a un string (latin1/binario, igual que fs.readFileSync('latin1') del
// motor JS - los scripts traen coreano CP949 en comentarios, no hace falta decodificarlo bien
// para ejecutar el codigo).
static bool ReadFile(const std::string& path, std::string& out) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return false;
    std::ostringstream ss; ss << f.rdbuf(); out = ss.str();
    return true;
}

static bool RunString(lua_State* L, const std::string& src, const char* chunkName) {
    if (luaL_loadbuffer(L, src.c_str(), src.size(), chunkName) != 0 || lua_pcall(L, 0, LUA_MULTRET, 0) != 0) {
        DbgLog("[lua_weapon_fire] %s: %s", chunkName, lua_tostring(L, -1));
        lua_pop(L, 1);
        return false;
    }
    return true;
}

// WEAPONTYPE_*/ATTACKATTRIB_*/STATE_* de ActorStates_Constants.lua son indices de una tabla C++
// que no tenemos, y el archivo real los cuenta con un solo "local index" corrido entre
// secciones (STATE_* sigue en WEAPONTYPE_* sin resetear) - contarlos de nuevo a mano daria
// numeros distintos a los reales sin ganar nada, porque los Weapon_*.lua SOLO los pasan OPACOS
// a InitAttack1 (nunca los comparan/suman - verificado). Se definen como su propio nombre en
// string: alcanza para re-vincularlos por NOMBRE contra attackattribindexer.lua despues.
static std::string ConstantsShimSrc(const std::string& scriptDir) {
    std::string text;
    if (!ReadFile(scriptDir + "/ActorStates_Constants.lua", text)) {
        if (!ReadFile(scriptDir + "/actorstates_constants.lua", text)) return "";
    }
    std::set<std::string> names;
    // busca lineas "NOMBRE = index;" (con espacios variables) sin regex, escaneo simple.
    size_t pos = 0;
    while (pos < text.size()) {
        size_t eq = text.find('=', pos);
        if (eq == std::string::npos) break;
        // nombre = lo no-blanco antes del '='
        size_t nameEnd = eq; while (nameEnd > 0 && isspace((unsigned char)text[nameEnd - 1])) nameEnd--;
        size_t nameStart = nameEnd; while (nameStart > 0 && (isalnum((unsigned char)text[nameStart - 1]) || text[nameStart - 1] == '_')) nameStart--;
        // valor = lo que sigue al '=', debe empezar con "index"
        size_t vs = eq + 1; while (vs < text.size() && isspace((unsigned char)text[vs])) vs++;
        if (text.compare(vs, 5, "index") == 0 && nameEnd > nameStart) {
            std::string name = text.substr(nameStart, nameEnd - nameStart);
            if (!name.empty() && (isalpha((unsigned char)name[0]) || name[0] == '_')) names.insert(name);
        }
        pos = eq + 1;
    }
    std::string out;
    for (const std::string& n : names) { out += n; out += " = \""; out += n; out += "\"\n"; }
    return out;
}

static float ReadField(lua_State* L, const char* key, float fallback) {
    lua_getfield(L, -1, key);
    float v = lua_isnumber(L, -1) ? (float)lua_tonumber(L, -1) : fallback;
    lua_pop(L, 1);
    return v;
}
static std::string ReadStrField(lua_State* L, const char* key) {
    lua_getfield(L, -1, key);
    std::string v = lua_isstring(L, -1) ? lua_tostring(L, -1) : "";
    lua_pop(L, 1);
    return v;
}

// Vector3 (userdata con metatabla V3 del shim) en la posicion dada de la pila -> 3 floats.
static void ReadVec3Field(lua_State* L, const char* key, float& x, float& y, float& z) {
    lua_getfield(L, -1, key);
    if (lua_istable(L, -1)) {
        lua_getfield(L, -1, "x"); x = (float)lua_tonumber(L, -1); lua_pop(L, 1);
        lua_getfield(L, -1, "y"); y = (float)lua_tonumber(L, -1); lua_pop(L, 1);
        lua_getfield(L, -1, "z"); z = (float)lua_tonumber(L, -1); lua_pop(L, 1);
    }
    lua_pop(L, 1);
}

static AttackShape ReadShape(lua_State* L) {
    AttackShape s;
    lua_getfield(L, -1, "shape");
    if (lua_istable(L, -1)) {
        s.kind = ReadStrField(L, "kind");
        ReadVec3Field(L, "center", s.centerX, s.centerY, s.centerZ);
        ReadVec3Field(L, "dir", s.dirX, s.dirY, s.dirZ);
        s.length = ReadField(L, "length", 0);
        s.radius = ReadField(L, "radius", 0);
        s.width  = ReadField(L, "width", 0);
        s.height = ReadField(L, "height", 0);
        s.angle  = ReadField(L, "angle", 0);
    }
    lua_pop(L, 1);
    return s;
}

std::vector<WeaponAttack> RunWeaponFire(const char* scriptDirC, const char* shimPath, const char* luaFile,
                                         const char* funcName, float oneShotTime, float power,
                                         float accuracy, float additionalPower,
                                         WeaponFireState* outState) {
    std::vector<WeaponAttack> out;
    if (outState) *outState = WeaponFireState{};
    const std::string scriptDir = scriptDirC;

    lua_State* L = luaL_newstate();
    if (!L) return out;
    luaL_openlibs(L);

    bool ok = RunString(L, ConstantsShimSrc(scriptDir), "constants");

    std::string shimSrc;
    if (ok && ReadFile(shimPath, shimSrc)) {
        ok = RunString(L, shimSrc, "shim");
    } else if (ok) {
        DbgLog("[lua_weapon_fire] no se encontro el shim en %s", shimPath);
        ok = false;
    }

    std::string weaponSrc;
    if (ok && ReadFile(scriptDir + "/" + luaFile, weaponSrc)) {
        ok = RunString(L, weaponSrc, luaFile);
    } else if (ok) {
        DbgLog("[lua_weapon_fire] no se pudo leer %s/%s", scriptDirC, luaFile);
        ok = false;
    }

    if (ok) {
        RunString(L, "ATTACKS = {}", "reset-attacks");   // por si el .lua ya la toco al cargar

        lua_getglobal(L, funcName);
        if (lua_isfunction(L, -1)) {
            lua_pushnumber(L, oneShotTime);
            lua_pushnumber(L, power);
            lua_pushnumber(L, accuracy);
            lua_pushnumber(L, additionalPower);
            if (lua_pcall(L, 4, 0, 0) != 0) {
                DbgLog("[lua_weapon_fire] %s: %s", funcName, lua_tostring(L, -1));
                lua_pop(L, 1);
            } else {
                if (outState) {
                    lua_getglobal(L, "STATE");
                    if (lua_istable(L, -1)) {
                        outState->loaded = true;
                        lua_getfield(L, -1, "lua");
                        if (lua_istable(L, -1)) {
                            for (int i = 1, n = (int)lua_objlen(L, -1); i <= n; ++i) {
                                lua_rawgeti(L, -1, i);
                                outState->initArgs.push_back(lua_isboolean(L, -1)
                                    ? float(lua_toboolean(L, -1)) : float(lua_tonumber(L, -1)));
                                lua_pop(L, 1);
                            }
                        }
                        lua_pop(L, 1);
                        lua_getfield(L, -1, "dropTime");
                        outState->hasDrop = lua_isnumber(L, -1) != 0;
                        lua_pop(L, 1);
                        outState->dropTime = ReadField(L, "dropTime", 0.f);
                        ReadVec3Field(L, "dropVector", outState->dropX, outState->dropY, outState->dropZ);
                        outState->moveSpeedRate      = ReadField(L, "moveSpeedRate", 1.f);
                        outState->moveBoxMultiplier   = ReadField(L, "moveBoxMultiplier", 1.f);
                        outState->hitStopA            = ReadField(L, "hitStopA", 0.f);
                        outState->hitStopB            = ReadField(L, "hitStopB", 0.f);
                        lua_getfield(L, -1, "stealthCancel");
                        outState->stealthCancel = lua_toboolean(L, -1) != 0;
                        lua_pop(L, 1);
                    }
                    lua_pop(L, 1);   // STATE
                }
                lua_getglobal(L, "ATTACKS");
                if (lua_istable(L, -1)) {
                    int n = (int)lua_objlen(L, -1);
                    for (int i = 1; i <= n; ++i) {
                        lua_rawgeti(L, -1, i);
                        if (lua_istable(L, -1)) {
                            WeaponAttack a;
                            a.explosion    = lua_toboolean(L, -1) && false;   // placeholder, ver abajo
                            lua_getfield(L, -1, "explosion"); a.explosion = lua_toboolean(L, -1) != 0; lua_pop(L, 1);
                            a.weaponType   = ReadStrField(L, "weaponType");
                            a.attackAttrib = ReadStrField(L, "attackAttrib");
                            a.fireTime     = ReadField(L, "fireTime", 0);
                            a.power        = ReadField(L, "power", 0);
                            a.critMult     = ReadField(L, "critMult", 0);
                            a.critChance   = ReadField(L, "critChance", 0);
                            a.shape        = ReadShape(L);
                            out.push_back(std::move(a));
                        }
                        lua_pop(L, 1);
                    }
                }
                lua_pop(L, 1);   // ATTACKS
            }
        } else {
            DbgLog("[lua_weapon_fire] %s no existe en %s", funcName, luaFile);
            lua_pop(L, 1);
        }
    }

    lua_close(L);
    return out;
}

}  // namespace luaw
