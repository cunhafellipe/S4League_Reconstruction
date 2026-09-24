// Ported 1:1 from C:\S4Web2\engine\item_db.js.
#include "item_db.h"
#include <fstream>
#include <regex>
#include <sstream>

namespace data {

const std::unordered_map<std::string, IdRange> kIdRange = {
    {"hair", {1000000, 1009999}}, {"face", {1010000, 1019999}}, {"top", {1020000, 1029999}},
    {"pants", {1030000, 1039999}}, {"gloves", {1040000, 1049999}}, {"shoes", {1050000, 1059999}},
    {"accessories", {1060000, 1069999}}, {"pets", {1070000, 1079999}},
    {"melee", {2000000, 2009999}}, {"guns", {2010000, 2019999}}, {"heavies", {2020001, 2029999}},
    {"snipers", {2030001, 2039999}}, {"sentries", {2040001, 2049999}}, {"thrown", {2050001, 2059999}},
    {"special", {2060001, 2069999}},
};
const std::unordered_map<std::string, std::string> kCatSlot = {
    {"hair", "hair"}, {"face", "face"}, {"top", "body"}, {"pants", "leg"}, {"gloves", "hand"}, {"shoes", "foot"},
};
const std::vector<std::string> kWeaponCats = { "melee", "guns", "heavies", "snipers", "sentries", "thrown", "special" };

std::string CategoryOf(int key) {
    for (auto& kv : kIdRange) if (key >= kv.second.lo && key <= kv.second.hi) return kv.first;
    return "";
}

static std::string Attr(const std::string& tag, const std::string& name) {
    std::regex re(name + "=\"([^\"]*)\"");
    std::smatch m;
    return std::regex_search(tag, m, re) ? m[1].str() : "";
}
static std::string MatchOr(const std::string& s, const std::regex& re) {
    std::smatch m;
    return std::regex_search(s, m, re) ? m[0].str() : "";
}

std::vector<Item> ParseItemDb(const std::string& file) {
    std::vector<Item> items;
    std::ifstream f(file, std::ios::binary);
    if (!f) return items;
    std::ostringstream ss; ss << f.rdbuf();
    std::string xml = ss.str();   // item.x7 declares UTF-8 (correct Korean names)

    static const std::regex itemRe(R"re(<item item_key="(\d+)">([\s\S]*?)</item>)re");
    static const std::regex baseRe(R"re(<base [^>]*/?>)re");
    static const std::regex graphicRe(R"re(<graphic [^>]*/?>)re");

    for (auto it = std::sregex_iterator(xml.begin(), xml.end(), itemRe); it != std::sregex_iterator(); ++it) {
        auto& m = *it;
        int key = std::stoi(m[1].str());
        std::string body = m[2].str();
        std::string cat = CategoryOf(key);
        if (cat.empty()) continue;

        std::string baseTag = MatchOr(body, baseRe);
        std::string gTag = MatchOr(body, graphicRe);

        Item item;
        item.key = key; item.cat = cat;
        auto slotIt = kCatSlot.find(cat); item.slot = slotIt != kCatSlot.end() ? slotIt->second : "";
        item.sex = Attr(baseTag, "sex"); if (item.sex.empty()) item.sex = "unisex";
        item.name = Attr(baseTag, "name"); if (item.name.empty()) item.name = "#" + std::to_string(key);
        item.part = Attr(gTag, "to_part_scene_file");
        item.icon = Attr(gTag, "icon_image");
        item.hiding = Attr(gTag, "hiding_option");
        for (int i = 1; i <= 6; ++i) {
            std::string f2 = Attr(gTag, "to_node_scene_file" + std::to_string(i));
            if (f2.empty() || f2 == "-") continue;
            ItemNode n; n.file = f2;
            n.parent = Attr(gTag, "to_node_parent_node" + std::to_string(i));
            if (n.parent.empty()) n.parent = "Bip01";
            n.anim = Attr(gTag, "to_node_animation_part" + std::to_string(i)) == "1";
            item.nodes.push_back(std::move(n));
        }
        items.push_back(std::move(item));
    }
    return items;
}

}  // namespace data
