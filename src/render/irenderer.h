// IRenderer - backend-agnostic renderer interface.
//
// The Engine talks to renderer->{Init,CanRender,BeginFrame,Present,Shutdown}
// and never to a graphics API directly, so the backend (D3D9 / D3D11) is
// swappable. Config lives here so WinMain wiring is identical for any backend.
#pragma once
#include <windows.h>
#include <cstdint>
#include <vector>

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
    // Graphics quality: 0 low, 1 medium, 2 high (texture filtering, MSAA,
    // particle density - see CRendererD3D::ApplyMeshSampler / DrawEffects).
    int  quality       = 2;

    virtual ~IRenderer() = default;
    // Window resized / quality changed: queued, applied at the next BeginFrame
    // (a D3D9 device can't be Reset between BeginScene and EndScene).
    virtual void RequestResize(int w, int h) { (void)w; (void)h; }
    virtual void RequestQuality(int q) { quality = q; }
    // PrtScr: the next presented frame is saved as screenshots/<date_time>.png.
    virtual void RequestScreenshot() {}
    // Releases textures not drawn for unusedMs; they load again by name when
    // next drawn (their handles stay valid).
    virtual void TrimTextures(unsigned unusedMs) { (void)unusedMs; }
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
    // Static geometry (map meshes): uploaded ONCE to GPU buffers, then drawn by
    // handle - the client keeps map geometry in vertex/index buffers instead of
    // re-sending every vertex each frame. floatsPerVert: 5 = xyz uv, 7 = xyz uv uv2.
    virtual int  CreateStaticMesh(const float* verts, int floatsPerVert, int vertCount,
                                  const unsigned short* idx, int idxCount) {
        (void)verts;(void)floatsPerVert;(void)vertCount;(void)idx;(void)idxCount; return -1;
    }
    // Draws triCount triangles from firstIndex. lmTex >= 0 (and a 7-float mesh):
    // main texture x lightmap, as DrawMeshLightmap; else as DrawMesh.
    virtual void DrawStaticMesh(int mesh, int firstIndex, int triCount, int tex, int lmTex,
                                const float* world16, uint32_t flags = 0) {
        (void)mesh;(void)firstIndex;(void)triCount;(void)tex;(void)lmTex;(void)world16;(void)flags;
    }
    virtual void ReleaseStaticMeshes() {}
    // Frees one static mesh (its slot stays; drawing it then does nothing).
    virtual void ReleaseStaticMesh(int mesh) { (void)mesh; }
    // While a sink is set, DrawMesh stores the call (vertices, indices, texture,
    // world matrix, flags) instead of drawing it: a pose drawn once can be
    // turned into static meshes and replayed from the GPU.
    struct CapturedMesh {
        std::vector<float> verts; int vertCount = 0;
        std::vector<unsigned short> idx;
        int tex = -1; float world[16] = {}; uint32_t flags = 0;
    };
    virtual void SetMeshCapture(std::vector<CapturedMesh>* sink) { (void)sink; }
    // Restrict 3D rendering to a screen rect (character preview panel). (0,0,0,0)
    // = full backbuffer. Also sets the perspective aspect for SetCamera3D.
    virtual void SetViewport(int x, int y, int w, int h) { (void)x;(void)y;(void)w;(void)h; }
    // Clears color + depth inside the current viewport only (preview panels
    // drawn over the world start from an empty background).
    virtual void ClearViewport(uint32_t argb) { (void)argb; }

    // Particle effects: a batch of camera-facing colored rectangles.
    // `particles` is 9 floats per particle - x,y,z (world position), r,g,b,a
    // (tint 0..1), width, height (world units; .seq keys carry them apart,
    // e.g. 5 x 300 lightning streaks). tex < 0 draws an untextured tinted
    // quad. additive picks glow-style blending (fire, energy, sparks) over
    // regular alpha blending (smoke, dust).
    // 12 floats per particle: the 9 above + velocity (world). renderType 1
    // (.seq CActParticle) = streak: the height runs along the on-screen
    // velocity (fastrun / lightning trails); 0 = plain camera-facing quad;
    // 2 = a 3D segment: centred at x,y,z, spanning the full vector p[9..11].
    // 10..13 = .seq CActParticle RenderType 0..3 as the client builds them
    // (S10 sub_1C8D470): width/height are HALF extents;
    // 10 faces the camera, 12 stands upright turning only about Y, 13 lies
    // flat (XZ) - all three turned by p[9] degrees; 11 stretches along the
    // velocity p[9..11] (h along it, w across).
    // Optional .seq UV pairs: top-left, bottom-left, top-right, bottom-right.
    virtual void DrawParticles(const float* particles, int count, int tex, bool additive, int renderType, const float* uv = nullptr) {
        (void)particles;(void)count;(void)tex;(void)additive;(void)renderType;(void)uv;
    }
    // A camera-facing textured ribbon through `count` world points (3 floats
    // each) - .seq CActLightning beams. u runs along the ribbon from u0 to u1
    // (the texture repeats and scrolls), v across it; argb tints/fades.
    // Opacity for the next DrawMesh calls (.seq effect meshes fading in/out);
    // 1 = as authored. Callers set it back to 1 after.
    float meshFade = 1.f;
    virtual void DrawBeam(const float* pts, int count, float width, uint32_t argb, int tex, bool additive, float u0, float u1) {
        (void)pts;(void)count;(void)width;(void)argb;(void)tex;(void)additive;(void)u0;(void)u1;
    }
};
