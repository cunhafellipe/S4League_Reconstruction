// CRenderer_D3D - Direct3D9 renderer (IRenderer backend).
// Reconstructed from FUN_01c82620 (Init) + FUN_01c84ac0 (BuildPresentParams).
#pragma once
#include "render/irenderer.h"
#include <vector>
#include <string>
#include <unordered_map>

struct IDirect3D9;
struct IDirect3DDevice9;
struct IDirect3DTexture9;
struct ID3DXFont;

// Config fields (hwnd/width/height/fullscreen/vsync/...) are inherited from
// IRenderer. The comments map them to the original CRenderer_D3D layout.
struct CRendererD3D : IRenderer {
    IDirect3D9*       d3d    = nullptr;   // this[0x1a5]
    IDirect3DDevice9* device = nullptr;   // this[0x1a7]
    int vpW = 0, vpH = 0;                 // current 3D viewport size (0 = full backbuffer)

    bool Init()       override;   // FUN_01c82620 : Direct3DCreate9 + CreateDevice
    bool CanRender()  override;   // render gate (device present + not lost)
    void BeginFrame() override;   // Clear + BeginScene
    void Present()    override;   // EndScene + Present
    void Shutdown()   override;
    void DrawRect(int x, int y, int w, int h, uint32_t argb) override;
    int  LoadTexture(const char* name) override;
    bool GetTextureSize(int tex, int& w, int& h) override;
    void DrawImage(int x, int y, int w, int h, int tex,
                   float u0, float v0, float u1, float v1) override;
    void DrawText(int x, int y, int w, int h, const char* text, int px, uint32_t argb, int align) override;
    void DrawTriangles(const float* posUV, int vertCount,
                       const unsigned short* idx, int idxCount, int tex) override;
    void SetCamera3D(const float eye[3], const float at[3],
                     float fovYdeg, float nearZ, float farZ) override;
    void DrawMesh(const float* posUV, int vertCount,
                  const unsigned short* idx, int idxCount,
                  int tex, const float* world16, uint32_t flags = 0) override;
    void DrawMeshLightmap(const float* posUV2, int vertCount,
                          const unsigned short* idx, int idxCount,
                          int tex, int lmTex, const float* world16, uint32_t flags = 0) override;
    void SetViewport(int x, int y, int w, int h) override;   // (0,0,0,0) = full backbuffer
    void DrawParticles(const float* particles, int count, int tex, bool additive) override;

private:
    float camEye[3] = {0,0,0}, camAt[3] = {0,0,1};   // cached from the last SetCamera3D, for particle billboards
    std::vector<IDirect3DTexture9*> textures;
    std::vector<std::pair<int,int>> texSize;   // native w,h per texture
    std::unordered_map<std::string, int> texIndex;   // name -> textures[] slot
    std::unordered_map<std::string, std::string> fileIndex;  // filename -> full path
    bool fileIndexBuilt = false;
    int  ResolveAndLoad(const std::string& name);

    std::unordered_map<int, ID3DXFont*> fonts;       // pixel size -> font
    std::string fontFamily;                          // 2002.ttf family name
    bool fontInit = false;
    ID3DXFont* GetFont(int px);
};
