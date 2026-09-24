// Minimal input state. WndProc sets edges; scenes read them in OnUpdate;
// Engine clears edges once per frame. ponytail: global is fine for one window.
#pragma once

namespace input {
struct State {
    bool anyKey = false;   // a key was pressed this frame
    bool click  = false;   // left mouse pressed this frame (edge)
    bool down   = false;   // left mouse currently held (level; not cleared per frame)
    bool cycleAnim = false;// '+' pressed this frame (cycle character animation)
    int  mx = 0, my = 0;   // cursor position (client space)
    long mouseDX = 0;      // raw relative mouse delta since last consume (WM_INPUT)
    long mouseDY = 0;      // consumer (WorldState mouse-look) resets these to 0
};
inline State g;
inline void ClearEdges() { g.anyKey = false; g.click = false; g.cycleAnim = false; }
}  // namespace input
