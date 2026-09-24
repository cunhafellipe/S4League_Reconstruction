// IRenderer - backend-agnostic renderer interface.
//
// The Engine talks to renderer->{Init,CanRender,BeginFrame,Present,Shutdown}
// and never to a graphics API directly, so the backend (D3D9 / D3D11) is
// swappable. Config lives here so WinMain wiring is identical for any backend.
#pragma once
#include <windows.h>
#include <cstdint>

struct IRenderer {
    // Config (filled by WinMain before Init). Mirrors CRenderer_D3D fields.
    HWND hwnd          = nullptr;
    bool fullscreen    = false;
    int  bpp           = 32;
    int  width         = 1024;
    int  height        = 768;
    int  refresh       = 0;
    bool vsync         = false;
    bool swapCopy      = false;
    bool multithreaded = false;

    virtual ~IRenderer() = default;
    virtual bool Init()       = 0;
    virtual bool CanRender()  = 0;
    virtual void BeginFrame() = 0;   // clear + begin scene
    virtual void Present()    = 0;   // end scene + present
    virtual void Shutdown()   = 0;

    // 2D: filled rect in screen space, color = 0xAARRGGBB. Default no-op so a
    // backend can opt in (DX9 implements; DX11 pending).
    virtual void DrawRect(int x, int y, int w, int h, uint32_t argb) { (void)x;(void)y;(void)w;(void)h;(void)argb; }

    // Textures for the .xui UI. LoadTexture returns an opaque handle (cached by
    // name), -1 on failure. DrawImage blits a UV sub-rect of it into (x,y,w,h).
    virtual int  LoadTexture(const char* name) { (void)name; return -1; }
    virtual bool GetTextureSize(int tex, int& w, int& h) { (void)tex; w = h = 0; return false; }
    virtual void DrawImage(int x, int y, int w, int h, int tex,
                           float u0, float v0, float u1, float v1) {
        (void)x;(void)y;(void)w;(void)h;(void)tex;(void)u0;(void)v0;(void)u1;(void)v1;
    }
    // Text via the game TTF (2002.ttf) at pixel size `px`, color 0xAARRGGBB.
    // align: 0=left 1=center 2=right (from the widget's <text_align>). Vertically
    // centered in (x,y,w,h) to match the client / XUI editor.
    virtual void DrawText(int x, int y, int w, int h, const char* text, int px, uint32_t argb, int align = 0) {
        (void)x;(void)y;(void)w;(void)h;(void)text;(void)px;(void)argb;(void)align;
    }
    // Textured triangle list for .scn meshes. posUV = [screenX,screenY,u,v] per
    // vertex; idx = triangle indices into it. tex = LoadTexture handle.
    virtual void DrawTriangles(const float* posUV, int vertCount,
                               const unsigned short* idx, int idxCount, int tex) {
        (void)posUV;(void)vertCount;(void)idx;(void)idxCount;(void)tex;
    }

    // --- 3D character path (.scn meshes) ------------------------------------
    // Set the perspective camera once per frame, then DrawMesh each model.
    // eye/at are xyz world points, fovYdeg vertical FOV, nearZ/farZ clip planes.
    virtual void SetCamera3D(const float eye[3], const float at[3],
                             float fovYdeg, float nearZ, float farZ) {
        (void)eye;(void)at;(void)fovYdeg;(void)nearZ;(void)farZ;
    }
    // posUV = [x,y,z,u,v] per vertex (5 floats). world16 = model node matrix
    // (row-major) or nullptr for identity. tex = LoadTexture handle. flags =
    // scn::RenderFlag bits (transparent/cutout/nocull/no-depth-write).
    virtual void DrawMesh(const float* posUV, int vertCount,
                          const unsigned short* idx, int idxCount,
                          int tex, const float* world16, uint32_t flags = 0) {
        (void)posUV;(void)vertCount;(void)idx;(void)idxCount;(void)tex;(void)world16;(void)flags;
    }
    // Same as DrawMesh but with a baked LIGHTMAP: posUV2 = [x,y,z,u1,v1,u2,v2] per
    // vertex (7 floats); stage0=main tex(uv1) MODULATE2X stage1=lmTex(uv2). Gives
    // maps their warm baked lighting instead of the flat/"cold" look.
    virtual void DrawMeshLightmap(const float* posUV2, int vertCount,
                                  const unsigned short* idx, int idxCount,
                                  int tex, int lmTex, const float* world16, uint32_t flags = 0) {
        (void)posUV2;(void)vertCount;(void)idx;(void)idxCount;(void)tex;(void)lmTex;(void)world16;(void)flags;
    }
    // Restrict 3D rendering to a screen rect (character preview panel). (0,0,0,0)
    // = full backbuffer. Also sets the perspective aspect for SetCamera3D.
    virtual void SetViewport(int x, int y, int w, int h) { (void)x;(void)y;(void)w;(void)h; }

    // Particle effects: a batch of camera-facing colored squares. `particles`
    // is 8 floats per particle - x,y,z (world position), r,g,b,a (tint 0..1),
    // size (world-space edge length). tex < 0 draws an untextured tinted
    // square. additive picks glow-style blending (fire, energy, sparks) over
    // regular alpha blending (smoke, dust).
    virtual void DrawParticles(const float* particles, int count, int tex, bool additive) {
        (void)particles;(void)count;(void)tex;(void)additive;
    }
};
