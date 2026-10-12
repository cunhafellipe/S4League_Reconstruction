// Ported 1:1 from C:\S4Web2\engine\weapon_attach.js.
#include "weapon_attach.h"
#include <filesystem>
#include <fstream>
#include <regex>
#include <sstream>

namespace fs = std::filesystem;

namespace data {

const std::unordered_map<std::string, WeaponAttachEntry>& LoadWeaponAttach(const std::string& resourcesPath) {
    static std::unordered_map<std::string, std::unordered_map<std::string, WeaponAttachEntry>> cache;
    auto it = cache.find(resourcesPath);
    if (it != cache.end()) return it->second;
    auto& out = cache[resourcesPath];

    std::ifstream f((fs::path(resourcesPath) / "xml" / "weapon_attach.x7").string(), std::ios::binary);
    if (!f) {
        // No weapon_attach.x7 in this resource set: use the 2008 beta's own
        // table, which it hardcodes in Game::Weapon::CWeapon setup
        // (sub_71F420 -> CWeapon::LoadWeaponModel sub_71EB40 for link=0
        // pieces with a hand + holster node, sub_71E8C0 for link=1 pieces
        // fixed to one node). Identical to the .x7 for every type it covers.
        struct Row { int type, piece; const char* attack; int link; const char* idle; };
        static const Row kBeta[] = {
            { 0, 0, "R_Hand_Dummy",    0, "Spine_Dummy" },
            { 1, 0, "R_Hand_Dummy",    0, "Gun_Dummy"   },
            { 1, 1, "Bip01 L Forearm", 1, nullptr       },
            { 2, 0, "R_Hand_Dummy",    0, "Gun_Dummy"   },
            { 2, 1, "L_Hand_Dummy",    0, "Gun_Dummy"   },
            { 3, 0, "R_Hand_Dummy",    0, "Gun_Dummy"   },
            { 4, 0, "R_Hand_Dummy",    0, "Gun_Dummy"   },
            { 5, 0, "L_Hand_Dummy",    0, "Spine_Dummy" },
            { 6, 0, "R_Hand_Dummy",    0, "Spine_Dummy" },
            { 7, 0, "R_Hand_Dummy",    0, "Spine_Dummy" },
            { 8, 0, "Dummy_L",         1, nullptr       },
            { 8, 1, "Dummy_R",         1, nullptr       },
            { 9, 0, "Dummy_L",         1, nullptr       },
            { 9, 1, "Dummy_R",         1, nullptr       },
            {10, 0, "R_Hand_Dummy",    0, "Spine_Dummy" },
            {11, 0, "Bip01 R Forearm", 1, nullptr       },
            {12, 0, "Bip01 R Forearm", 1, nullptr       },
            {13, 0, "R_Hand_Dummy",    0, "Spine_Dummy" },
            {14, 0, "R_Hand_Dummy",    0, "Spine_Dummy" },
            {14, 1, "L_Hand_Dummy",    0, "Spine_Dummy" },
            {14, 2, "L_Hand_Dummy",    0, "Spine_Dummy" },
            {15, 0, "Card_Dummy",      1, nullptr       },
            {15, 1, "R_Hand_Dummy",    1, nullptr       },
            {15, 2, "L_Hand_Dummy",    1, nullptr       },
            {16, 0, "R_Hand_Dummy",    0, "Spine_Dummy" },
        };
        for (const Row& r : kBeta) {
            WeaponAttachEntry e;
            e.attack = r.attack; e.link = r.link;
            if (r.idle) { e.idle = r.idle; e.hasIdle = true; }
            out[std::to_string(r.type) + ":" + std::to_string(r.piece)] = std::move(e);
        }
        return out;
    }
    std::ostringstream ss; ss << f.rdbuf();
    std::string xml = ss.str();

    static const std::regex re(
        R"re(<weapon\s+type="(\d+)"\s+sceneIndex="(\d+)"\s+attackAttach="([^"]*)"\s+link="(\d+)"(?:\s+idleAttach="([^"]*)")?)re");
    for (auto sit = std::sregex_iterator(xml.begin(), xml.end(), re); sit != std::sregex_iterator(); ++sit) {
        auto& m = *sit;
        std::string key = m[1].str() + ":" + m[2].str();
        // A (type, sceneIndex) pair can repeat (e.g. type 43, left/right calf): first
        // one wins, same as the client's std::map which doesn't overwrite an
        // already-inserted key.
        if (out.count(key)) continue;
        WeaponAttachEntry e;
        e.attack = m[3].str(); e.link = std::stoi(m[4].str());
        if (m[5].matched) { e.idle = m[5].str(); e.hasIdle = true; }
        out[key] = std::move(e);
    }
    return out;
}

const std::unordered_map<std::string, int>& WeaponTypeIds(const std::string& resourcesPath) {
    static std::unordered_map<std::string, std::unordered_map<std::string, int>> cache;
    auto it = cache.find(resourcesPath);
    if (it != cache.end()) return it->second;
    auto& out = cache[resourcesPath];

    std::ifstream f((fs::path(resourcesPath) / "resources" / "script" / "actorstates_constants.lua").string(), std::ios::binary);
    if (!f) return out;
    std::ostringstream ss; ss << f.rdbuf();
    std::string text = ss.str();

    // The Lua ships a "WEAPONYTPE_" typo. std::regex has no multiline '^' anchor
    // by default, so match per-line instead of over the whole text like the JS
    // 'gm' regex does.
    static const std::regex re(R"(^\s*(WEAPON[TY]{2}PE_\w+)\s*=\s*index\s*;)");
    int i = -1;
    std::istringstream lines(text);
    std::string line;
    while (std::getline(lines, line)) {
        std::smatch m;
        if (std::regex_search(line, m, re)) out[m[1].str()] = i++;
    }
    return out;
}

std::optional<int> TypeFromSuffix(const std::string& resourcesPath, const std::string& suffix,
                                   const std::unordered_map<std::string, std::string>& typeBySuffix) {
    if (suffix.empty()) return std::nullopt;
    auto sit = typeBySuffix.find(suffix);
    if (sit == typeBySuffix.end()) return std::nullopt;
    auto& ids = WeaponTypeIds(resourcesPath);
    std::string typeName = sit->second;
    // The early actorstates script calls the plasma family
    // WEAPONTYPE_PLASMA_SWORD, while the constants table used by the client
    // calls the same slot WEAPONTYPE_3_SWORD. Both are the same wire ID (0).
    if (typeName == "WEAPONTYPE_PLASMA_SWORD" && !ids.count(typeName))
        typeName = "WEAPONTYPE_3_SWORD";
    auto iit = ids.find(typeName);
    return iit != ids.end() ? std::optional<int>(iit->second) : std::nullopt;
}

int AttachLink(const std::string& resourcesPath, int type, int sceneIndex) {
    auto& table = LoadWeaponAttach(resourcesPath);
    auto it = table.find(std::to_string(type) + ":" + std::to_string(sceneIndex));
    return it == table.end() ? -1 : it->second.link;
}

std::string AttachBone(const std::string& resourcesPath, int type, int sceneIndex, bool idle) {
    auto& table = LoadWeaponAttach(resourcesPath);
    auto it = table.find(std::to_string(type) + ":" + std::to_string(sceneIndex));
    if (it == table.end()) return "";
    return (idle && it->second.hasIdle) ? it->second.idle : it->second.attack;
}

}  // namespace data
