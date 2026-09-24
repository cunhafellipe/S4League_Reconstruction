#include "gui/gui_render.h"
#include "gui/xui.h"
#include "render/irenderer.h"
#include "input/input.h"
#include "app/dbg.h"
#include <vector>
#include <algorithm>

namespace gui {

static void DrawOne(IRenderer* r, const Widget& w) {
    int x = w.left, y = w.top, cw = w.right - w.left, ch = w.bottom - w.top;
    static int dc = 0;
    if (dc < 80 && (w.skin.valid || !w.text.empty())) {
        DbgLog("[gui] %s (%d,%d %dx%d) skin=%s text='%s'", w.name.c_str(), x, y, cw, ch,
               w.skin.valid ? w.skin.texture.c_str() : "-", w.text.c_str());
        dc++;
    }
    // Buttons darken only while PRESSED (mouse held over them), not on hover.
    const Skin* sk = &w.skin;
    if (w.type == "CGUI_Button" && input::g.down &&
        input::g.mx >= w.left && input::g.mx < w.right &&
        input::g.my >= w.top && input::g.my < w.bottom)
        sk = w.skinDown.valid ? &w.skinDown : (w.skinOver.valid ? &w.skinOver : &w.skin);
    if (sk->valid) {
        int tex = r->LoadTexture(sk->texture.c_str());   // cached by name
        if (tex >= 0)
            r->DrawImage(x, y, cw, ch, tex, sk->u0, sk->v0, sk->u1, sk->v1);
    }
    if (!w.text.empty())
        r->DrawText(x, y, cw, ch, w.text.c_str(), w.fontPx, 0xFFFFFFFF, w.textAlign);
}

static long Area(const Widget* w) {
    return (long)(w->right - w->left) * (long)(w->bottom - w->top);
}

// Render 1:1 with the client / S4L XUI editor (PreviewForm.DrawCacheTree): draw
// the element (skin + text), then recurse children in REVERSE order so the first
// child in the .xui ends up ON TOP (a background = later child -> drawn first,
// behind its labels/buttons). Tree order = correct z-layering, not size-as-z.
static void DrawRec(IRenderer* r, const Widget& w) {
    if (!w.show) return;
    DrawOne(r, w);
    for (int i = (int)w.children.size() - 1; i >= 0; --i)
        DrawRec(r, w.children[i]);
}
void DrawTree(IRenderer* r, const Widget& root, int, int) {
    DrawRec(r, root);
}

// Topmost (smallest-area) visible CGUI_Button containing (mx,my).
static void HitScan(const Widget& w, int mx, int my, const Widget** best) {
    if (!w.show) return;
    if (w.type == "CGUI_Button" &&
        mx >= w.left && mx < w.right && my >= w.top && my < w.bottom &&
        (!*best || Area(&w) < Area(*best)))
        *best = &w;
    for (const auto& c : w.children) HitScan(c, mx, my, best);
}
std::string HitButton(const Widget& root, int mx, int my) {
    const Widget* best = nullptr;
    HitScan(root, mx, my, &best);
    return best ? best->name : std::string();
}

}  // namespace gui
