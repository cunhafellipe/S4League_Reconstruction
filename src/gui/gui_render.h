// Draws a .xui widget tree. Step 1: layout outlines (no textures/text yet).
#pragma once
#include <string>
struct IRenderer;
namespace gui { struct Widget; }

namespace gui {
// Draw the tree at screen origin (ox,oy). Children are offset by their parent.
void DrawTree(IRenderer* r, const Widget& w, int ox = 0, int oy = 0);

// Name of the topmost visible CGUI_Button under (mx,my), or "" if none. Buttons
// are small, so smallest-area wins = the actual button. Positions come straight
// from the .xui <global> rects, so this is 1:1 with the client's button layout.
std::string HitButton(const Widget& root, int mx, int my);
}
