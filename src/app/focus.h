// Whether the game's own window is the focused one - gameplay input (WASD,
// mouse clicks, space) must never react to input meant for some other
// window just because the game process happens to still be running.
#pragma once
#include <windows.h>

inline HWND& GameHwndSlot() { static HWND h = nullptr; return h; }
inline void SetGameHwnd(HWND h) { GameHwndSlot() = h; }
inline bool GameHasFocus() {
    HWND h = GameHwndSlot();
    return h && GetForegroundWindow() == h;
}

// Drop-in for GetAsyncKeyState that reads as "nothing pressed" whenever the
// game window isn't focused - a click or keypress meant for some other
// window must never register as game input just because this process is
// still running in the background.
// The real call still runs while unfocused so its "pressed since last call"
// bit (& 1) is consumed - otherwise a key typed in another window would fire
// the moment the game gets the focus back.
inline SHORT FocusedKeyState(int vk) {
    const SHORT s = GetAsyncKeyState(vk);
    return GameHasFocus() ? s : 0;
}
