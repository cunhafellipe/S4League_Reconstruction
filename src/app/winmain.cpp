// S4 Client reconstruction - entry point + application loop + Engine::Frame.
//
// Binary-driven rebuild. Original call chain (Ghidra):
//   entry (03100000)  -> LoadLibraryA("sneoz.dll"), JMP __tmainCRTStartup
//   FUN_01b25f2a      -> __tmainCRTStartup (CRT init) -> calls WinMain
//   FUN_00f163d0      -> WinMain(hInst,hPrev,lpCmdLine,nShow)   [reconstructed]
//
// Object model:
//   AppHost  (local_330, 0x18 bytes, ctor FUN_01b28b80): +0x04 HWND,
//            +0x10 deferred-msg queue, +0x14 Engine*. Owns the loop FUN_01b29210.
//   Engine   (0x70 bytes, ctor FUN_00f19100). vtable @ 01f17274:
//            [0] FUN_00f2c950 dtor   [2] FUN_00f1dbd0 Frame   [3] FUN_00f1d590 Shutdown
//            fields: +0x04 HWND  +0x3c sys  +0x4c Renderer  +0x54 World
//                    +0x60 sys   +0x6c Audio

#include <windows.h>
#include <mmsystem.h>
#include "render/renderer.h"
#include "newrender/renderer_d3d11.h"
#include "world/world.h"
#include "scenes/scenes.h"
#include "input/input.h"
#include "net/config.h"
#include "app/dbg.h"
#include "app/focus.h"
#include "audio/audio.h"
#include <string>

#pragma comment(lib, "winmm.lib")

// Render backend switch: 0 = Direct3D9 (original), 1 = Direct3D11 (newrender).
#define S4_RENDER_DX11 0

// Frame limiter: cap to a target fps with a hybrid sleep+spin (Kethen-style, like
// fps.cpp) so movement/camera pace smoothly instead of running uncapped/jittery.
// F2 cycles 60 -> 144 -> uncapped. timeBeginPeriod(1) makes Sleep(1) ~1ms accurate.
static int g_targetFps = 144;
static void LimitFrameRate() {
    if (g_targetFps <= 0) return;
    static LARGE_INTEGER freq = { 0 }, last = { 0 };
    if (freq.QuadPart == 0) { QueryPerformanceFrequency(&freq); QueryPerformanceCounter(&last); return; }
    const double target = 1.0 / (double)g_targetFps;
    LARGE_INTEGER now;
    for (;;) {
        QueryPerformanceCounter(&now);
        double elapsed = (double)(now.QuadPart - last.QuadPart) / freq.QuadPart;
        double remaining = target - elapsed;
        if (remaining <= 0.0) break;
        if (remaining > 0.0015) Sleep(1);   // coarse sleep, leave ~1.5ms to spin for precision
    }
    last = now;
}

// --- Unresolved dependencies (reverse next) ---------------------------------
// FID_conflict:_Immortalize<>  application singleton (CApp).
// FUN_01b79480 / FUN_01ba7320  version.ini + region ladder.
// FUN_0158ed70 / FUN_0164a280  .x7 language pack / .s4 user data.
// FUN_00f1dbd0 subsystem pumps: FUN_00f5f880(audio) FUN_00f2fbe0(+0x3c)
//   FUN_01a63a20 FUN_01930c40 FUN_01a01bf0 FUN_01913940 FUN_00f43140(+0x60)
//   FUN_012928b0(+0x54) FUN_017dc8f0.
// FUN_01297400  World::Update (returns frame cost). FUN_01292bb0 World::Present.
// Renderer vtable[0x3c] = CanRender. DAT_025bd0b4 = input manager.

static int NowMs() { return (int)timeGetTime(); }   // FUN_01b36d90

// Per-frame globals mirrored from the binary's DAT_ cells.
static int  g_inputState      = 0;      // DAT_025bcf08: 0=normal, 2=window-drag
static bool g_limitInit       = false;  // DAT_025bd8dc & 1
static int  g_frameCount      = 0;      // DAT_025bd8cc
static int  g_frameBudget     = 0;      // DAT_025bd8d0
static int  g_frameAccum      = 0;      // DAT_025bd8d4
static bool g_limitOff        = false;  // DAT_025bd8d8
static bool g_renderThisFrame = true;   // DAT_025690e1
static bool g_screenshotLatch = false;  // DAT_025bd8e0

// ---------------------------------------------------------------------------
// Engine : the +0x14 object. One concrete struct (no interface: exactly one).
struct Engine {
    bool  alive    = true;
    HWND  hwnd     = nullptr;   // +0x04
    void* audio    = nullptr;   // +0x6c
    void* sys3c    = nullptr;   // +0x3c
    IRenderer* renderer = nullptr;   // +0x4c  (D3D9 or D3D11 backend)
    World* world   = nullptr;   // +0x54
    void* sys60    = nullptr;   // +0x60

    void Setup1() {}            // FUN_01b28940
    void Setup2() {}            // FUN_01b289a0
    void Setup3() {}            // FUN_01b28a20
    void Frame();               // FUN_00f1dbd0
    void Shutdown() { alive = false; }   // FUN_00f1d590

    // Opaque sub-calls kept as named stubs (real targets in the comments above).
    void PumpAudio()      {}    // FUN_00f5f880(audio)
    void PumpSubsystems() { proud::g_client.Poll(); if (world) world->Update(); }  // net + FUN_012928b0
    int  WorldUpdate()    { return 0; }        // FUN_01297400(world,this): frame-cost timer
    void WorldPresent()   { if (world) world->Present(); } // FUN_01292bb0(world)
    bool RendererReady()  { return renderer && renderer->CanRender(); }  // vtable[0x3c]
    bool WorldWantsShot() { return false; }    // *(world+0x110) vtable[0x14]
    bool CursorLocked()   { return false; }    // *(world+0x160)
    void StoreFrameTime(int) {}                // *(world+0x1c0) = dt
    void PauseInput()     {}    // FUN_01c7f3d0/3a0(DAT_025bd0b4)
    void TakeScreenshot() {}    // ./ScreenShot/ _s S4 %04d-%02d-... .ext
};

// AppHost : local_330. Holds window + Engine, owns the loop.
struct AppHost {
    HWND    hwnd   = nullptr;   // +0x04
    Engine* engine = nullptr;   // +0x14

    void FlushDeferred() {}     // FUN_01b290b0

    // FUN_01b29210 : the real game loop.
    void Run()
    {
        FlushDeferred();
        timeBeginPeriod(1);                       // 1ms scheduler for the frame limiter
        MSG msg;
        while (engine && engine->alive) {
            if (!PeekMessageA(&msg, nullptr, 0, 0, PM_REMOVE)) {
                engine->Frame();                 // idle -> tick (vtable[2])
                LimitFrameRate();                // pace to g_targetFps (smooth movement)
            } else {
                if (msg.message == WM_QUIT) break;
                if (!IsDialogMessageA(nullptr, &msg)) {
                    TranslateMessage(&msg);
                    DispatchMessageA(&msg);
                }
            }
        }
        timeEndPeriod(1);
        if (engine) engine->Shutdown();          // vtable[3]
    }
};

static AppHost* g_host = nullptr;

// FUN_00f1dbd0 : Engine::Frame - per-frame update + render.
void Engine::Frame()
{
    // 1. Audio pump.
    audio::Pump();

    // 2. Lose input focus if we're not the focused/active window.
    if (hwnd != GetFocus() || hwnd != GetActiveWindow())
        g_inputState = 0;

    // 3. Window-drag mode: LMB held drags the (borderless) window.
    if (g_inputState == 2) {
        if (GetAsyncKeyState(VK_LBUTTON) == 0) {
            g_inputState = 0;
        } else {
            POINT p; GetCursorPos(&p);
            static POINT last = p;
            if (last.x != p.x && last.y != p.y) {
                RECT wr; GetWindowRect(hwnd, &wr);
                MoveWindow(hwnd, wr.left + (p.x - last.x), wr.top + (p.y - last.y),
                           wr.right - wr.left, wr.bottom - wr.top, TRUE);
            }
            last = p;
        }
    }

    // 4. Not the active window -> throttle, pause input, release cursor clip.
    if (hwnd != GetActiveWindow()) {
        Sleep(5);
        PauseInput();
        ClipCursor(nullptr);
    }

    // 5. Frame timing start.
    int t0 = NowMs();

    // 6. Pump the subsystems (input, sound, particles, net, ...).
    PumpSubsystems();

    // 7. One-time frame-limiter init.
    if (!g_limitInit) {
        g_limitInit = true;
        g_frameCount = g_frameBudget = g_frameAccum = 0;
        g_limitOff = false;
    }

    // 8. Update the world; accumulate cost against the frame budget.
    int cost = WorldUpdate();               // FUN_01297400
    if (!g_limitOff) {
        ++g_frameCount;
        g_frameAccum += cost;
        g_renderThisFrame = !(g_frameAccum <= g_frameBudget);
    } else {
        g_renderThisFrame = true;
    }

    // 9. Render gate: present when the renderer is ready.
    static int dbgOnce = 0;
    if (dbgOnce < 3) { DbgLog("[frame] tick %d rendererReady=%d world=%p", dbgOnce, (int)RendererReady(), (void*)world); dbgOnce++; }
    if (RendererReady()) {                   // renderer vtable[0x3c]
        renderer->BeginFrame();              // Clear + BeginScene
        WorldPresent();                      // FUN_01292bb0 (world draws)
        renderer->Present();                 // EndScene + Present
        if (WorldWantsShot() && hwnd == GetActiveWindow()) {
            bool pressed = (GetAsyncKeyState(VK_SNAPSHOT) & 0x8000) != 0;
            if (!pressed) {
                g_screenshotLatch = false;
            } else if (!g_screenshotLatch && g_renderThisFrame) {
                g_screenshotLatch = true;
                g_frameBudget = 3000;         // pause limiter during capture
                TakeScreenshot();             // -> ./ScreenShot/
            }
        }
    }

    // 10. Store this frame's cost on the world (world+0x1c0).
    StoreFrameTime(NowMs() - t0);

    // 11. Mouselook: recenter the cursor while active and locked.
    if (hwnd == GetActiveWindow() && CursorLocked()) {
        RECT cr; GetClientRect(hwnd, &cr);
        POINT c = { (cr.right - cr.left) / 2, (cr.bottom - cr.top) / 2 };
        ClientToScreen(hwnd, &c);
        SetCursorPos(c.x, c.y);
    }

    input::ClearEdges();   // per-frame: consume this frame's key/click edges
}

// ---------------------------------------------------------------------------
static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_CLOSE:   DestroyWindow(hwnd); return 0;
    case WM_SIZE:
        if (wp != SIZE_MINIMIZED && LOWORD(lp) > 0 && HIWORD(lp) > 0 && g_host && g_host->engine && g_host->engine->renderer)
            g_host->engine->renderer->RequestResize(LOWORD(lp), HIWORD(lp));
        break;
    case WM_DESTROY:
        if (g_host && g_host->engine) g_host->engine->alive = false;
        PostQuitMessage(0);
        return 0;
    case WM_INPUT: {
        UINT sz = 0;
        GetRawInputData((HRAWINPUT)lp, RID_INPUT, nullptr, &sz, sizeof(RAWINPUTHEADER));
        if (sz && sz <= 64) {
            BYTE buf[64];
            if (GetRawInputData((HRAWINPUT)lp, RID_INPUT, buf, &sz, sizeof(RAWINPUTHEADER)) == sz) {
                RAWINPUT* ri = (RAWINPUT*)buf;
                // Mouse-look only while the game window is the focused one: raw
                // input can still arrive with the game in the background.
                if (ri->header.dwType == RIM_TYPEMOUSE && GameHasFocus()) {
                    const RAWMOUSE& m = ri->data.mouse;
                    if (m.usFlags & MOUSE_MOVE_ABSOLUTE) {
                        // Absolute mode (RDP/VM): deltas come from the last position,
                        // scaled from the 0..65535 normalized range down to ~pixels.
                        static LONG lastAX = 0, lastAY = 0; static bool haveAbs = false;
                        if (haveAbs) {
                            input::g.mouseDX += (m.lLastX - lastAX) / 28;
                            input::g.mouseDY += (m.lLastY - lastAY) / 28;
                        }
                        lastAX = m.lLastX; lastAY = m.lLastY; haveAbs = true;
                    } else {
                        input::g.mouseDX += m.lLastX;
                        input::g.mouseDY += m.lLastY;
                    }
                    if ((m.lLastX || m.lLastY)) {
                        static bool wmOnce = false;
                        if (!wmOnce) { wmOnce = true; DbgLog("[input] WM_INPUT mode=%s x=%ld y=%ld",
                            (m.usFlags & MOUSE_MOVE_ABSOLUTE) ? "ABS" : "REL", m.lLastX, m.lLastY); }
                    }
                }
            }
        }
        break;   // let DefWindowProc do WM_INPUT cleanup
    }
    case WM_KEYDOWN:      input::g.anyKey = true;
                          if (wp == VK_ADD || wp == VK_OEM_PLUS) input::g.cycleAnim = true;
                          if (wp == VK_F2) {   // cycle frame cap 60 -> 144 -> uncapped
                              g_targetFps = (g_targetFps == 60) ? 144 : (g_targetFps == 144) ? 0 : 60;
                              DbgLog("[fps] cap -> %d", g_targetFps);
                          }
                          return 0;
    case WM_MOUSEWHEEL:   if (GameHasFocus()) input::g.wheel += GET_WHEEL_DELTA_WPARAM(wp) / WHEEL_DELTA; return 0;
    case WM_LBUTTONDOWN:  input::g.click = true; input::g.down = true;
                          input::g.mx = LOWORD(lp); input::g.my = HIWORD(lp); return 0;
    case WM_LBUTTONUP:    input::g.down = false; return 0;
    case WM_MOUSEMOVE:    input::g.mx = LOWORD(lp); input::g.my = HIWORD(lp); return 0;
    }
    return DefWindowProc(hwnd, msg, wp, lp);
}

// Window styles verbatim from FUN_00f163d0 (local_490).
// 0x06CA0000 (the client's fixed window) + WS_THICKFRAME | WS_MAXIMIZEBOX:
// resizable; the renderer resets its back buffer to the new client size.
static constexpr DWORD kStyleWindowed   = 0x06CA0000 | WS_THICKFRAME | WS_MAXIMIZEBOX;
static constexpr DWORD kStyleFullscreen = 0x86000000;
static bool g_fullscreen = false;    // singleton+0x2c
static int  g_width  = 1024;         // singleton+0x24
static int  g_height = 768;          // singleton+0x28

// FUN_01b28ea0 -> FUN_01b29be0 -> FUN_01b29d90 : create the "S4 Client" window.
static HWND CreateGameWindow()
{
    WNDCLASSEXA wc = { sizeof(wc) };
    wc.style         = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc   = WndProc;
    wc.hInstance     = GetModuleHandleA(nullptr);
    wc.hCursor       = LoadCursor(nullptr, IDC_ARROW);
    wc.lpszClassName = "S4 Client";
    RegisterClassExA(&wc);

    DWORD style = g_fullscreen ? kStyleFullscreen : kStyleWindowed;
    RECT rc = { 0, 0, g_width, g_height };
    AdjustWindowRect(&rc, style, FALSE);        // FUN_01b29d90

    HWND hwnd = CreateWindowExA(
        0, "S4 Client", "S4 Client", style,
        0, 0, rc.right - rc.left, rc.bottom - rc.top,
        nullptr, nullptr, GetModuleHandleA(nullptr), nullptr);

    // FUN_01b28ef0(DAT_025bdbbc, this): register in global window manager. TODO.

    // Register raw mouse input (relative deltas, like the client's DirectInput8)
    // for smooth mouse-look with no cursor-recenter hacks.
    RAWINPUTDEVICE rid = {};
    rid.usUsagePage = 0x01;   // generic desktop
    rid.usUsage     = 0x02;   // mouse
    rid.dwFlags     = 0;
    rid.hwndTarget  = hwnd;
    BOOL rawOk = RegisterRawInputDevices(&rid, 1, sizeof(rid));
    SetGameHwnd(hwnd);   // game input only while this window is focused (app/focus.h)
    DbgLog("[input] RegisterRawInputDevices -> %d (err=%lu) hwnd=%p", (int)rawOk, GetLastError(), (void*)hwnd);
    return hwnd;
}

// WinMain spine stubs (reverse in place, keep compiling).
static bool InitInstance() { return true; }   // FUN_01a8b960
static void LoadConfig() {                    // FUN_01b79480 + region ladder
    std::string ip = "127.0.0.1"; int port = 28002;
    if (cfg::ServerEndpoint(ip, port))
        DbgLog("[config] auth server %s:%d (version.ini [SERVER])", ip.c_str(), port);
    else
        DbgLog("[config] no version.ini -> default %s:%d", ip.c_str(), port);
}
static void LoadResources(){}                 // FUN_0158ed70 .x7 + FUN_0164a280 .s4
static void SaveUserData() {}                 // FUN_0164a000 + FUN_01648ce0

// FUN_00f163d0 : WinMain(hInst, hPrevInstance, lpCmdLine, nCmdShow)
int WINAPI WinMain(HINSTANCE, HINSTANCE, LPSTR, int nCmdShow)
{
    if (!InitInstance())
        return 0;

    TIMECAPS tc = {};
    UINT period = (timeGetDevCaps(&tc, sizeof(tc)) == TIMERR_NOERROR) ? tc.wPeriodMin : 1;
    timeBeginPeriod(period);

    LoadConfig();
    LoadResources();
    audio::Init();

    HWND hwnd = CreateGameWindow();
    if (!hwnd) {
        timeEndPeriod(period);
        return 0;
    }

    Engine  engine;
    AppHost host;
    engine.hwnd = hwnd;
    host.hwnd   = hwnd;
    host.engine = &engine;
    g_host = &host;

    // Renderer backend (FUN_01c82620 for DX9). Swap via S4_RENDER_DX11.
#if S4_RENDER_DX11
    CRendererD3D11 renderer;
#else
    CRendererD3D renderer;
#endif
    renderer.hwnd       = hwnd;
    renderer.fullscreen = g_fullscreen;
    renderer.width      = g_width;
    renderer.height     = g_height;
    renderer.Init();
    engine.renderer = &renderer;

    // World: scene/state manager (FUN_012928b0). No scene yet -> clears only.
    World world;
    world.renderer = &renderer;
    engine.world = &world;

    // Flow (real assets): Intro(_eu_intro.x7) -> Title(title_back00/ani)
    //   --key/auth--> ServerSelect(4server.xui) -> WorldState (3D map + movement).
    // Skipping straight to WorldState for now while the client is in active testing.
    IntroState        intro;
    TitleState        title;
    ServerSelectState serverSelect;
    WorldState        worldState;
    intro.next = &title;
    title.next = &serverSelect;
    serverSelect.world3d = &worldState;
    world.ChangeScene(&worldState);

    ShowWindow(hwnd, nCmdShow);
    UpdateWindow(hwnd);

    engine.Setup1();
    engine.Setup2();
    engine.Setup3();
    host.Run();

    renderer.Shutdown();
    audio::Shutdown();
    SaveUserData();
    timeEndPeriod(period);
    return 0;
}
