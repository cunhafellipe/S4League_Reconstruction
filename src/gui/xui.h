// .xui UI loader. .xui files are plain XML describing a CGUI widget tree
// (CGUI_Window / CGUI_GroupBox / CGUI_Button / CGUI_EditBox / ...).
#pragma once
#include <string>
#include <vector>

namespace gui {

// A textured skin (gui_skin_0/1/2 = normal/over/down). UVs are normalized 0..1
// into `texture` (a .dds/.tga atlas under resources/xui/image/).
struct Skin {
    std::string texture;              // atlas file name
    float u0 = 0, v0 = 0, u1 = 1, v1 = 1;   // left/top/right/bottom
    bool  valid = false;
};

struct Widget {
    std::string type;                 // CGUI_* class name (the XML tag)
    std::string name;                 // name attribute
    int  left = 0, top = 0, right = 0, bottom = 0;   // <local> rect
    bool show = true;                 // <show value>
    Skin skin;                        // gui_skin_0 (normal state)
    Skin skinOver, skinDown;          // gui_skin_1 (hover), gui_skin_2 (pressed)
    std::string text;                 // <string><eng value=...> (static label text)
    int  fontPx = 0;                  // <font value="2002_15"> -> 15 (0 = none)
    int  textAlign = 0;               // <text_align value> : 0=left 1=center 2=right
    std::vector<Widget> children;     // from <child>
};

// Parse a .xui file into a widget tree. Root type is "" on failure.
Widget LoadXui(const char* path);

}  // namespace gui
