// .xui loader: a minimal XML DOM parser + mapping to the CGUI widget tree.
// ponytail: hand parser scoped to these well-formed UTF-8 .xui files (elements,
// attributes, self-closing, nesting). No CDATA/comments/entities handling -
// upgrade to MSXML if a file needs it.
#include "gui/xui.h"
#include <fstream>
#include <cstdlib>

namespace gui {

// --- tiny XML DOM -----------------------------------------------------------
struct Xml {
    std::string tag;
    std::vector<std::pair<std::string, std::string>> attr;
    std::vector<Xml> kids;
    const std::string* A(const char* k) const {
        for (auto& p : attr) if (p.first == k) return &p.second;
        return nullptr;
    }
    const Xml* Child(const char* t) const {
        for (auto& c : kids) if (c.tag == t) return &c;
        return nullptr;
    }
};

static void SkipWs(const std::string& s, size_t& i) {
    while (i < s.size() && (unsigned char)s[i] <= ' ') ++i;
}

// Parse one element starting at s[i]=='<' (not a <?...?> / <!--...-->).
static bool ParseElem(const std::string& s, size_t& i, Xml& out) {
    if (s[i] != '<') return false;
    ++i;
    while (i < s.size() && s[i] != ' ' && s[i] != '>' && s[i] != '/' && s[i] != '\t'
           && s[i] != '\r' && s[i] != '\n')
        out.tag.push_back(s[i++]);
    // attributes
    for (;;) {
        SkipWs(s, i);
        if (i >= s.size()) return false;
        if (s[i] == '/' ) { i += 2; return true; }       // "/>" self-closing
        if (s[i] == '>') { ++i; break; }
        std::string k, v;
        while (i < s.size() && s[i] != '=' && s[i] != ' ' && s[i] != '>') k.push_back(s[i++]);
        SkipWs(s, i);
        if (i < s.size() && s[i] == '=') {
            ++i; SkipWs(s, i);
            if (i < s.size() && s[i] == '"') { ++i; while (i < s.size() && s[i] != '"') v.push_back(s[i++]); ++i; }
        }
        if (!k.empty()) out.attr.emplace_back(k, v);
    }
    // children until </tag>
    for (;;) {
        SkipWs(s, i);
        if (i >= s.size()) return false;
        if (s[i] == '<') {
            if (s[i + 1] == '/') { i += 2; while (i < s.size() && s[i] != '>') ++i; ++i; return true; }
            if (s[i + 1] == '?' || s[i + 1] == '!') { while (i < s.size() && s[i] != '>') ++i; ++i; continue; }
            Xml c;
            if (!ParseElem(s, i, c)) return false;
            out.kids.push_back(std::move(c));
        } else { ++i; }   // text (ignored for .xui)
    }
}

// --- map DOM element -> Widget ----------------------------------------------
static int AtoiA(const Xml& e, const char* childTag, const char* attr) {
    const Xml* c = e.Child(childTag);
    if (!c) return 0;
    const std::string* v = c->A(attr);
    return v ? std::atoi(v->c_str()) : 0;
}

// Find the first element with `tag` in the widget's subtree, WITHOUT descending
// into <child> (those belong to sub-widgets).
static const Xml* FindElem(const Xml& e, const char* tag) {
    for (auto& k : e.kids) {
        if (k.tag == "child") continue;
        if (k.tag == tag) return &k;
        if (auto r = FindElem(k, tag)) return r;
    }
    return nullptr;
}

static Widget MapWidget(const Xml& e) {
    Widget w;
    w.type = e.tag;
    if (auto n = e.A("name")) w.name = *n;
    // Use <global> = absolute screen coords (exact final position). Falls back
    // to <local> only if a widget has no <global>.
    const char* box = e.Child("global") ? "global" : "local";
    w.left   = AtoiA(e, box, "left");
    w.top    = AtoiA(e, box, "top");
    w.right  = AtoiA(e, box, "right");
    w.bottom = AtoiA(e, box, "bottom");
    if (auto sh = e.Child("show")) { auto v = sh->A("value"); w.show = !v || *v == "true"; }
    // gui_skin_0/1/2 = normal / over(hover) / down(pressed).
    auto parseSkin = [&](const char* tag, Skin& sk) {
        const Xml* s = FindElem(e, tag);
        if (!s) return;
        if (auto t = s->A("texture")) sk.texture = *t;
        auto F = [&](const char* a, float d) { auto v = s->A(a); return v ? (float)std::atof(v->c_str()) : d; };
        sk.u0 = F("left", 0); sk.v0 = F("top", 0);
        sk.u1 = F("right", 1); sk.v1 = F("bottom", 1);
        sk.valid = !sk.texture.empty();
    };
    parseSkin("gui_skin_0", w.skin);
    parseSkin("gui_skin_1", w.skinOver);
    parseSkin("gui_skin_2", w.skinDown);
    // Static label text (<string><eng value="Login"/>) + font ("2002_15" -> 15).
    if (const Xml* str = FindElem(e, "string")) {
        if (auto eng = str->Child("eng")) if (auto v = eng->A("value")) w.text = *v;
    }
    if (const Xml* fn = FindElem(e, "font")) {
        if (auto v = fn->A("value")) {
            auto us = v->find('_');
            w.fontPx = (us != std::string::npos) ? std::atoi(v->c_str() + us + 1) : 0;
        }
    }
    if (const Xml* ta = FindElem(e, "text_align")) {
        if (auto v = ta->A("value")) w.textAlign = std::atoi(v->c_str());   // 0 L / 1 C / 2 R
    }
    if (auto ch = e.Child("child"))
        for (auto& k : ch->kids)
            if (k.tag.rfind("CGUI_", 0) == 0) w.children.push_back(MapWidget(k));
    return w;
}

static const Xml* FindFirstCGUI(const Xml& e) {
    if (e.tag.rfind("CGUI_", 0) == 0) return &e;
    for (auto& k : e.kids) if (auto r = FindFirstCGUI(k)) return r;
    return nullptr;
}

Widget LoadXui(const char* path) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return {};
    std::string s((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    size_t i = 0;
    if (s.size() >= 3 && (unsigned char)s[0] == 0xEF) i = 3;   // UTF-8 BOM

    // Find the first CGUI_* element (inside <gui_root><gui>).
    while (i < s.size()) {
        SkipWs(s, i);
        if (i < s.size() && s[i] == '<') {
            if (s[i + 1] == '?' || s[i + 1] == '!') { while (i < s.size() && s[i] != '>') ++i; ++i; continue; }
            Xml root;
            if (!ParseElem(s, i, root)) return {};
            // Recursively find the first CGUI_* element (e.g. gui_root > gui >
            // CGUI_Window; note gui_root's first child is <script_files/>).
            if (const Xml* c = FindFirstCGUI(root)) return MapWidget(*c);
            return {};
        }
        ++i;
    }
    return {};
}

}  // namespace gui

#ifdef XUI_SELFTEST
#include <cassert>
int main() {
    // Round-trip a tiny login-like tree from a temp file.
    const char* xml =
        "<?xml version=\"1.0\"?>\n<gui_root><gui>"
        "<CGUI_Window name=\"Login_Window\"><local left=\"0\" top=\"0\" right=\"776\" bottom=\"667\"/>"
        "<show value=\"true\"/><child>"
        "<CGUI_Button name=\"OK\"><local left=\"10\" top=\"20\" right=\"110\" bottom=\"50\"/></CGUI_Button>"
        "</child></CGUI_Window></gui></gui_root>";
    { std::ofstream o("_xui_test.xui", std::ios::binary); o << xml; }
    auto w = gui::LoadXui("_xui_test.xui");
    assert(w.type == "CGUI_Window" && w.name == "Login_Window" && w.right == 776);
    assert(w.children.size() == 1 && w.children[0].type == "CGUI_Button" && w.children[0].right == 110);
    return 0;
}
#endif
