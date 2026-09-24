// item.x7 (item DB) parser, ported 1:1 from C:\S4Web2\engine\item_db.js.
#pragma once
#include <string>
#include <unordered_map>
#include <vector>

namespace data {

struct IdRange { int lo, hi; };
extern const std::unordered_map<std::string, IdRange> kIdRange;
extern const std::unordered_map<std::string, std::string> kCatSlot;   // clothing category -> loadParts slot
extern const std::vector<std::string> kWeaponCats;

// Category from item_key range, or empty string if none matches.
std::string CategoryOf(int key);

struct ItemNode { std::string file, parent; bool anim = false; };
struct Item {
    int key = 0;
    std::string cat, slot, sex, name;
    std::string part;   // to_part_scene_file
    std::string icon, hiding;
    std::vector<ItemNode> nodes;   // to_node_scene_fileN attachments
};

// Parses item.x7 -> equippable items classified by item_key range.
std::vector<Item> ParseItemDb(const std::string& file);

}  // namespace data
