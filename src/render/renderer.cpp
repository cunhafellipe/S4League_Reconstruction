// CRenderer_D3D - Direct3D9 renderer implementation.
//
// FUN_01c82620  CRenderer_D3D::Init          (device creation)
// FUN_01c84ac0  CRenderer_D3D::BuildPresent  (D3DPRESENT_PARAMETERS)
//
// ponytail: the 12 sub-managers the original news up (texture/shader/state/
// sprite/font/...) are deferred - they hang off this[0x11..0x1e] and each gets
// vtable[2]() init. Add them when there's geometry to draw. This gets a real
// D3D9 device that clears the backbuffer each frame.

#include "renderer.h"
#include "app/dbg.h"
#include <d3d9.h>
#include <filesystem>
#include <cstdio>
#include <cstring>
#include <cmath>

#pragma comment(lib, "d3d9.lib")
#pragma comment(lib, "gdi32.lib")

// D3DXCreateTextureFromFileA bound at runtime from the game's d3dx9_29.dll
// (loads .dds/.tga/.png). Avoids needing the legacy DirectX SDK import lib.
typedef HRESULT(WINAPI* PFN_D3DXCreateTextureFromFileA)(
    IDirect3DDevice9*, LPCSTR, IDirect3DTexture9**);
static PFN_D3DXCreateTextureFromFileA g_createTex = nullptr;
static void EnsureD3DX() {
    if (g_createTex) return;
    HMODULE m = LoadLibraryA("d3dx9_29.dll");
    if (!m) m = LoadLibraryA("d3dx9_43.dll");   // fallback
    if (m) g_createTex = (PFN_D3DXCreateTextureFromFileA)
        GetProcAddress(m, "D3DXCreateTextureFromFileA");
}

// Texture root (relative to the exe's working dir). Whole resources tree so
// intro (resources/intro), loading, and xui/image all resolve by filename.
static const char* kTexRoot = "resources/resources";

// FUN_01c84ac0 : fill D3DPRESENT_PARAMETERS from the renderer fields.
static void BuildPresentParams(const CRendererD3D& r, D3DPRESENT_PARAMETERS& pp)
{
    ZeroMemory(&pp, sizeof(pp));
    pp.BackBufferWidth        = r.width;                       // this[5]
    pp.BackBufferHeight       = r.height;                      // this[6]
    pp.BackBufferFormat       = D3DFMT_UNKNOWN;                // windowed: desktop fmt
    pp.BackBufferCount        = 1;
    pp.SwapEffect             = r.swapCopy ? D3DSWAPEFFECT_COPY
                                           : D3DSWAPEFFECT_DISCARD;   // 3 / 1
    pp.hDeviceWindow          = nullptr;                       // NULL -> focus window
    pp.Windowed               = r.fullscreen ? FALSE : TRUE;
    pp.EnableAutoDepthStencil = TRUE;
    pp.AutoDepthStencilFormat = D3DFMT_D24X8;                  // 0x4d default
    pp.Flags                  = 0;
    pp.FullScreen_RefreshRateInHz = r.fullscreen ? r.refresh : 0;
    pp.PresentationInterval   = r.vsync ? D3DPRESENT_INTERVAL_ONE
                                        : D3DPRESENT_INTERVAL_IMMEDIATE; // 1 / 0x80000000
    // ponytail: fullscreen backbuffer format + ChangeDisplaySettings mode set
    // deferred until fullscreen is exercised.
}

bool CRendererD3D::Init()
{
    DbgLog("[renderer] Init: hwnd=%p %dx%d fs=%d", (void*)hwnd, width, height, fullscreen);
    d3d = Direct3DCreate9(D3D_SDK_VERSION);   // 0x20 in the binary
    if (!d3d) { DbgLog("[renderer] Direct3DCreate9 FAILED"); return false; }

    D3DPRESENT_PARAMETERS pp;
    BuildPresentParams(*this, pp);

    // Behavior flags: base SOFTWARE VP (0x20) -> HARDWARE VP (0x40) if HW T&L,
    // | MULTITHREADED (4) when requested.
    D3DCAPS9 caps;
    DWORD flags = D3DCREATE_SOFTWARE_VERTEXPROCESSING;
    if (SUCCEEDED(d3d->GetDeviceCaps(D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, &caps)) &&
        (caps.DevCaps & D3DDEVCAPS_HWTRANSFORMANDLIGHT))
        flags = D3DCREATE_HARDWARE_VERTEXPROCESSING;
    if (multithreaded) flags |= D3DCREATE_MULTITHREADED;

    HRESULT hr = d3d->CreateDevice(
        D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, hwnd, flags, &pp, &device);
    if (FAILED(hr)) {                          // "___ERROR___ CRenderer::D3D::Init ...C"
        DbgLog("[renderer] CreateDevice FAILED hr=0x%08lx flags=0x%lx", (unsigned long)hr, (unsigned long)flags);
        d3d->Release(); d3d = nullptr;
        return false;
    }
    RECT cr = {}; GetClientRect(hwnd, &cr);
    DbgLog("[renderer] device OK; client=%dx%d backbuffer=%dx%d", cr.right, cr.bottom, width, height);
    return true;
}

bool CRendererD3D::CanRender()
{
    if (!device) return false;
    return device->TestCooperativeLevel() == D3D_OK;
}

void CRendererD3D::BeginFrame()
{
    if (!device) return;
    device->Clear(0, nullptr, D3DCLEAR_TARGET | D3DCLEAR_ZBUFFER,
                  D3DCOLOR_XRGB(20, 24, 32), 1.0f, 0);
    device->BeginScene();
}

void CRendererD3D::Present()
{
    if (!device) return;
    device->EndScene();
    device->Present(nullptr, nullptr, nullptr, nullptr);
}

// --- Text via ID3DXFont (D3DXCreateFontA from the game's d3dx9_29.dll) -------
typedef HRESULT(WINAPI* PFN_D3DXCreateFontA)(
    IDirect3DDevice9*, INT, UINT, UINT, UINT, BOOL, DWORD, DWORD, DWORD, DWORD,
    LPCSTR, ID3DXFont**);
static PFN_D3DXCreateFontA g_createFont = nullptr;

// Read the family name (name ID 1) from a TTF 'name' table.
static std::string GetTtfFamily(const char* path) {
    FILE* f = fopen(path, "rb");
    if (!f) return "";
    auto rdU16 = [&](long off) { unsigned char b[2]; fseek(f, off, SEEK_SET); fread(b, 1, 2, f); return (unsigned)((b[0] << 8) | b[1]); };
    auto rdU32 = [&](long off) { unsigned char b[4]; fseek(f, off, SEEK_SET); fread(b, 1, 4, f); return (unsigned)((b[0] << 24) | (b[1] << 16) | (b[2] << 8) | b[3]); };
    unsigned n = rdU16(4);
    long nameOff = 0;
    for (unsigned i = 0; i < n; ++i) {
        long rec = 12 + i * 16; char tag[4]; fseek(f, rec, SEEK_SET); fread(tag, 1, 4, f);
        if (!strncmp(tag, "name", 4)) { nameOff = rdU32(rec + 8); break; }
    }
    std::string res, mac;
    if (nameOff) {
        unsigned count = rdU16(nameOff + 2), strOff = rdU16(nameOff + 4);
        for (unsigned i = 0; i < count; ++i) {
            long rec = nameOff + 6 + i * 12;
            unsigned plat = rdU16(rec), nameID = rdU16(rec + 6), len = rdU16(rec + 8), off = rdU16(rec + 10);
            if (nameID != 1) continue;
            std::string s(len, 0); fseek(f, nameOff + strOff + off, SEEK_SET); fread(&s[0], 1, len, f);
            if (plat == 3 || plat == 0) { for (size_t k = 1; k < s.size(); k += 2) res.push_back(s[k]); if (!res.empty()) break; }
            else mac = s;
        }
    }
    fclose(f);
    return res.empty() ? mac : res;
}

ID3DXFont* CRendererD3D::GetFont(int px) {
    if (px <= 0) px = 12;
    auto it = fonts.find(px);
    if (it != fonts.end()) return it->second;
    if (!fontInit) {
        fontInit = true;
        const char* ttf = "resources/resources/gui/2002.ttf";
        AddFontResourceExA(ttf, FR_PRIVATE, nullptr);
        fontFamily = GetTtfFamily(ttf);
        HMODULE m = GetModuleHandleA("d3dx9_29.dll");
        if (!m) m = LoadLibraryA("d3dx9_29.dll");
        if (m) g_createFont = (PFN_D3DXCreateFontA)GetProcAddress(m, "D3DXCreateFontA");
        DbgLog("[font] family='%s' createFont=%p", fontFamily.c_str(), (void*)g_createFont);
    }
    ID3DXFont* fn = nullptr;
    if (g_createFont)
        g_createFont(device, px, 0, FW_NORMAL, 1, FALSE, DEFAULT_CHARSET,
                     OUT_DEFAULT_PRECIS, ANTIALIASED_QUALITY, DEFAULT_PITCH | FF_DONTCARE,
                     fontFamily.empty() ? "Arial" : fontFamily.c_str(), &fn);
    fonts[px] = fn;
    return fn;
}

void CRendererD3D::DrawText(int x, int y, int w, int h, const char* text, int px, uint32_t argb, int align) {
    if (!device || !text || !*text) return;
    ID3DXFont* fn = GetFont(px);
    if (!fn) return;
    RECT rc = { x, y, x + w, y + h };
    // Horizontal per <text_align>, vertically centered (matches client/editor).
    DWORD fmt = DT_VCENTER | DT_SINGLELINE | DT_NOCLIP |
                (align == 1 ? DT_CENTER : align == 2 ? DT_RIGHT : DT_LEFT);
    // ID3DXFont::DrawTextA = vtable[14] (interface frozen since d3dx9).
    typedef INT(WINAPI* PFN_Draw)(void*, void*, LPCSTR, INT, LPRECT, DWORD, D3DCOLOR);
    void** vt = *(void***)fn;
    ((PFN_Draw)vt[14])(fn, nullptr, text, -1, &rc, fmt, (D3DCOLOR)argb);
}

// Textured triangle list in screen space (for .scn meshes).
void CRendererD3D::DrawTriangles(const float* posUV, int vc,
                                 const unsigned short* idx, int ic, int tex) {
    if (!device || tex < 0 || tex >= (int)textures.size() || vc <= 0 || ic <= 0) return;
    struct TLT { float x, y, z, rhw, u, v; };
    std::vector<TLT> v(vc);
    for (int i = 0; i < vc; ++i)
        v[i] = { posUV[i*4] - 0.5f, posUV[i*4+1] - 0.5f, 0.f, 1.f, posUV[i*4+2], posUV[i*4+3] };

    device->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);
    device->SetRenderState(D3DRS_LIGHTING, FALSE);
    device->SetRenderState(D3DRS_ZENABLE, FALSE);
    device->SetRenderState(D3DRS_ALPHABLENDENABLE, TRUE);
    device->SetRenderState(D3DRS_SRCBLEND, D3DBLEND_SRCALPHA);
    device->SetRenderState(D3DRS_DESTBLEND, D3DBLEND_INVSRCALPHA);
    device->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_SELECTARG1);
    device->SetTextureStageState(0, D3DTSS_COLORARG1, D3DTA_TEXTURE);
    device->SetTextureStageState(0, D3DTSS_ALPHAOP, D3DTOP_SELECTARG1);
    device->SetTextureStageState(0, D3DTSS_ALPHAARG1, D3DTA_TEXTURE);
    device->SetSamplerState(0, D3DSAMP_MINFILTER, D3DTEXF_LINEAR);
    device->SetSamplerState(0, D3DSAMP_MAGFILTER, D3DTEXF_LINEAR);
    device->SetTexture(0, textures[tex]);
    device->SetFVF(D3DFVF_XYZRHW | D3DFVF_TEX1);
    device->DrawIndexedPrimitiveUP(D3DPT_TRIANGLELIST, 0, vc, ic / 3,
                                   idx, D3DFMT_INDEX16, v.data(), sizeof(TLT));
}

// --- 3D character path -------------------------------------------------------
// Left-handed perspective/lookat built by hand (no legacy D3DX SDK import).
static D3DMATRIX MatPerspectiveFovLH(float fovY, float aspect, float zn, float zf) {
    float ys = 1.0f / tanf(fovY * 0.5f);
    float xs = ys / aspect;
    float q  = zf / (zf - zn);
    D3DMATRIX m = {};
    m._11 = xs; m._22 = ys; m._33 = q; m._34 = 1.0f; m._43 = -zn * q;
    return m;
}
static D3DMATRIX MatLookAtLH(const float e[3], const float a[3]) {
    auto nrm = [](float v[3]) { float l = sqrtf(v[0]*v[0]+v[1]*v[1]+v[2]*v[2]); if (l > 1e-6f) { v[0]/=l; v[1]/=l; v[2]/=l; } };
    auto crs = [](const float p[3], const float q[3], float o[3]) { o[0]=p[1]*q[2]-p[2]*q[1]; o[1]=p[2]*q[0]-p[0]*q[2]; o[2]=p[0]*q[1]-p[1]*q[0]; };
    auto dot = [](const float p[3], const float q[3]) { return p[0]*q[0]+p[1]*q[1]+p[2]*q[2]; };
    float up[3] = {0,1,0}, z[3], x[3], y[3];
    z[0]=a[0]-e[0]; z[1]=a[1]-e[1]; z[2]=a[2]-e[2]; nrm(z);   // forward (LH: +Z into scene)
    crs(up, z, x); nrm(x);                                    // right
    crs(z, x, y);                                             // up
    D3DMATRIX m = {};
    m._11=x[0]; m._12=y[0]; m._13=z[0];
    m._21=x[1]; m._22=y[1]; m._23=z[1];
    m._31=x[2]; m._32=y[2]; m._33=z[2];
    m._41=-dot(x,e); m._42=-dot(y,e); m._43=-dot(z,e); m._44=1.0f;
    return m;
}

void CRendererD3D::SetCamera3D(const float eye[3], const float at[3],
                               float fovYdeg, float nearZ, float farZ) {
    if (!device) return;
    float aspect = (vpW > 0 && vpH > 0) ? (float)vpW / (float)vpH
                 : height ? (float)width / (float)height : 4.0f/3.0f;
    D3DMATRIX view = MatLookAtLH(eye, at);
    D3DMATRIX proj = MatPerspectiveFovLH(fovYdeg * 3.14159265f / 180.0f, aspect, nearZ, farZ);
    device->SetTransform(D3DTS_VIEW, &view);
    device->SetTransform(D3DTS_PROJECTION, &proj);
    camEye[0]=eye[0]; camEye[1]=eye[1]; camEye[2]=eye[2];
    camAt[0]=at[0]; camAt[1]=at[1]; camAt[2]=at[2];
}

// Camera-facing quads (2 tris each), colored per particle, no depth write so
// overlapping effects don't fight each other for draw order.
void CRendererD3D::DrawParticles(const float* particles, int count, int tex, bool additive) {
    if (!device || count <= 0) return;

    float fwd[3] = { camAt[0]-camEye[0], camAt[1]-camEye[1], camAt[2]-camEye[2] };
    float fl = std::sqrt(fwd[0]*fwd[0]+fwd[1]*fwd[1]+fwd[2]*fwd[2]);
    if (fl < 1e-5f) return;
    fwd[0]/=fl; fwd[1]/=fl; fwd[2]/=fl;
    const float worldUp[3] = {0,1,0};
    float right[3] = { fwd[1]*worldUp[2]-fwd[2]*worldUp[1], fwd[2]*worldUp[0]-fwd[0]*worldUp[2], fwd[0]*worldUp[1]-fwd[1]*worldUp[0] };
    float rl = std::sqrt(right[0]*right[0]+right[1]*right[1]+right[2]*right[2]);
    if (rl < 1e-5f) { right[0]=1; right[1]=0; right[2]=0; } else { right[0]/=rl; right[1]/=rl; right[2]/=rl; }
    float up[3] = { right[1]*fwd[2]-right[2]*fwd[1], right[2]*fwd[0]-right[0]*fwd[2], right[0]*fwd[1]-right[1]*fwd[0] };

    struct PV { float x,y,z; DWORD color; float u,v; };
    std::vector<PV> v((size_t)count * 4);
    std::vector<unsigned short> idx((size_t)count * 6);
    for (int i = 0; i < count; ++i) {
        const float* p = particles + i*8;
        float hs = p[7] * 0.5f;
        DWORD color = D3DCOLOR_COLORVALUE(p[3], p[4], p[5], p[6]);
        PV* q = &v[i*4];
        for (int c = 0; c < 4; ++c) {
            float sx = (c==0||c==3) ? -hs : hs;
            float sy = (c==0||c==1) ? -hs : hs;
            q[c].x = p[0] + right[0]*sx + up[0]*sy;
            q[c].y = p[1] + right[1]*sx + up[1]*sy;
            q[c].z = p[2] + right[2]*sx + up[2]*sy;
            q[c].color = color;
            q[c].u = sx > 0 ? 1.f : 0.f;
            q[c].v = sy > 0 ? 0.f : 1.f;
        }
        unsigned short base = (unsigned short)(i*4);
        unsigned short* qi = &idx[i*6];
        qi[0]=base; qi[1]=base+1; qi[2]=base+2; qi[3]=base; qi[4]=base+2; qi[5]=base+3;
    }

    D3DMATRIX ident = {}; ident._11=ident._22=ident._33=ident._44=1.0f;
    device->SetTransform(D3DTS_WORLD, &ident);
    device->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);
    device->SetRenderState(D3DRS_LIGHTING, FALSE);
    device->SetRenderState(D3DRS_ZENABLE, TRUE);
    device->SetRenderState(D3DRS_ZWRITEENABLE, FALSE);
    device->SetRenderState(D3DRS_ALPHABLENDENABLE, TRUE);
    device->SetRenderState(D3DRS_SRCBLEND, D3DBLEND_SRCALPHA);
    device->SetRenderState(D3DRS_DESTBLEND, additive ? D3DBLEND_ONE : D3DBLEND_INVSRCALPHA);
    bool textured = (tex >= 0 && tex < (int)textures.size());
    if (textured) {
        device->SetTexture(0, textures[tex]);
        device->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_MODULATE);
        device->SetTextureStageState(0, D3DTSS_COLORARG1, D3DTA_TEXTURE);
        device->SetTextureStageState(0, D3DTSS_COLORARG2, D3DTA_DIFFUSE);
        // Final alpha = texture's own alpha (the soft round falloff baked into
        // the sprite) times the particle's own fade - the earlier "invisible"
        // symptom was the missing UVs below, not this.
        device->SetTextureStageState(0, D3DTSS_ALPHAOP, D3DTOP_MODULATE);
        device->SetTextureStageState(0, D3DTSS_ALPHAARG1, D3DTA_TEXTURE);
        device->SetTextureStageState(0, D3DTSS_ALPHAARG2, D3DTA_DIFFUSE);
        device->SetSamplerState(0, D3DSAMP_MINFILTER, D3DTEXF_LINEAR);
        device->SetSamplerState(0, D3DSAMP_MAGFILTER, D3DTEXF_LINEAR);
    } else {
        device->SetTexture(0, nullptr);
        device->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_SELECTARG1);
        device->SetTextureStageState(0, D3DTSS_COLORARG1, D3DTA_DIFFUSE);
        device->SetTextureStageState(0, D3DTSS_ALPHAOP, D3DTOP_SELECTARG1);
        device->SetTextureStageState(0, D3DTSS_ALPHAARG1, D3DTA_DIFFUSE);
    }
    device->SetFVF(D3DFVF_XYZ | D3DFVF_DIFFUSE | D3DFVF_TEX1);
    device->DrawIndexedPrimitiveUP(D3DPT_TRIANGLELIST, 0, count*4, count*2,
                                   idx.data(), D3DFMT_INDEX16, v.data(), sizeof(PV));

    device->SetRenderState(D3DRS_ZENABLE, FALSE);
    device->SetRenderState(D3DRS_ALPHABLENDENABLE, FALSE);
    device->SetTextureStageState(0, D3DTSS_COLORARG2, D3DTA_CURRENT);
    device->SetTextureStageState(0, D3DTSS_ALPHAARG2, D3DTA_CURRENT);
}

// Textured 3D mesh (a .scn model). Uses the depth buffer; leaves Z disabled on
// exit so the 2D bg/text draws that follow are unaffected.
void CRendererD3D::DrawMesh(const float* posUV, int vc,
                            const unsigned short* idx, int ic, int tex,
                            const float* world16, uint32_t flags) {
    if (!device || vc <= 0 || ic <= 0) return;
    struct V3T { float x, y, z, u, v; };
    std::vector<V3T> v(vc);
    for (int i = 0; i < vc; ++i)
        v[i] = { posUV[i*5], posUV[i*5+1], posUV[i*5+2], posUV[i*5+3], posUV[i*5+4] };

    D3DMATRIX world;
    if (world16) std::memcpy(&world, world16, sizeof(world));
    else { world = {}; world._11 = world._22 = world._33 = world._44 = 1.0f; }
    device->SetTransform(D3DTS_WORLD, &world);

    // Per-mesh state from the .scn RenderFlag (io_s4league_scn): 2=TRANSPARENT
    // (alpha blend), 4=CUTOUT (alpha test, opaque), 8=NO_CULLING, 64=NO_DEPTH_WRITE.
    const bool transparent = (flags & 2) != 0;
    const bool cutout      = (flags & 4) != 0;
    const bool noZWrite    = (flags & 64) != 0;
    device->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);      // winding unconfirmed -> no cull
    device->SetRenderState(D3DRS_LIGHTING, FALSE);             // unlit textured (S4 characters)
    device->SetRenderState(D3DRS_ZENABLE, TRUE);
    device->SetRenderState(D3DRS_ZWRITEENABLE, (transparent || noZWrite) ? FALSE : TRUE);
    bool textured = (tex >= 0 && tex < (int)textures.size());
    if (textured) {
        // Cutout (hair/edges): alpha TEST only, keep Z-write -> solid strands, no
        // sorting gaps. Transparent: alpha BLEND, no Z-write. Else opaque.
        device->SetRenderState(D3DRS_ALPHABLENDENABLE, transparent ? TRUE : FALSE);
        device->SetRenderState(D3DRS_SRCBLEND, D3DBLEND_SRCALPHA);
        device->SetRenderState(D3DRS_DESTBLEND, D3DBLEND_INVSRCALPHA);
        device->SetRenderState(D3DRS_ALPHATESTENABLE, (cutout || transparent) ? TRUE : FALSE);
        device->SetRenderState(D3DRS_ALPHAREF, cutout ? 128 : 8);   // hair needs a firm cutoff
        device->SetRenderState(D3DRS_ALPHAFUNC, D3DCMP_GREATEREQUAL);
        device->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_SELECTARG1);
        device->SetTextureStageState(0, D3DTSS_COLORARG1, D3DTA_TEXTURE);
        device->SetTextureStageState(0, D3DTSS_ALPHAOP, D3DTOP_SELECTARG1);
        device->SetTextureStageState(0, D3DTSS_ALPHAARG1, D3DTA_TEXTURE);
        device->SetSamplerState(0, D3DSAMP_MINFILTER, D3DTEXF_LINEAR);
        device->SetSamplerState(0, D3DSAMP_MAGFILTER, D3DTEXF_LINEAR);
        device->SetTexture(0, textures[tex]);
    } else {
        // No texture -> flat color so the geometry (skinning) is visible.
        device->SetRenderState(D3DRS_ALPHABLENDENABLE, FALSE);
        device->SetRenderState(D3DRS_ALPHATESTENABLE, FALSE);
        device->SetTexture(0, nullptr);
        device->SetRenderState(D3DRS_TEXTUREFACTOR, 0xFFB0A0E0);   // light violet
        device->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_SELECTARG1);
        device->SetTextureStageState(0, D3DTSS_COLORARG1, D3DTA_TFACTOR);
        device->SetTextureStageState(0, D3DTSS_ALPHAOP, D3DTOP_SELECTARG1);
        device->SetTextureStageState(0, D3DTSS_ALPHAARG1, D3DTA_TFACTOR);
    }
    device->SetFVF(D3DFVF_XYZ | D3DFVF_TEX1);
    device->DrawIndexedPrimitiveUP(D3DPT_TRIANGLELIST, 0, vc, ic / 3,
                                   idx, D3DFMT_INDEX16, v.data(), sizeof(V3T));
    device->SetRenderState(D3DRS_ZENABLE, FALSE);             // restore 2D-friendly state
    device->SetRenderState(D3DRS_ALPHATESTENABLE, FALSE);
}

void CRendererD3D::DrawMeshLightmap(const float* posUV2, int vc,
                                    const unsigned short* idx, int ic, int tex, int lmTex,
                                    const float* world16, uint32_t flags) {
    // No lightmap available -> fall back to the single-texture path (repack to 5-float).
    if (!device || vc <= 0 || ic <= 0) return;
    if (lmTex < 0 || lmTex >= (int)textures.size() || tex < 0 || tex >= (int)textures.size()) {
        std::vector<float> p5((size_t)vc * 5);
        for (int i = 0; i < vc; ++i) { const float* s = posUV2 + i*7;
            p5[i*5+0]=s[0]; p5[i*5+1]=s[1]; p5[i*5+2]=s[2]; p5[i*5+3]=s[3]; p5[i*5+4]=s[4]; }
        DrawMesh(p5.data(), vc, idx, ic, tex, world16, flags);
        return;
    }
    struct V { float x,y,z,u1,v1,u2,v2; };
    std::vector<V> v(vc);
    for (int i = 0; i < vc; ++i) { const float* s = posUV2 + i*7;
        v[i] = { s[0],s[1],s[2], s[3],s[4], s[5],s[6] }; }

    D3DMATRIX world;
    if (world16) std::memcpy(&world, world16, sizeof(world));
    else { world = {}; world._11=world._22=world._33=world._44=1.0f; }
    device->SetTransform(D3DTS_WORLD, &world);

    const bool cutout = (flags & 4) != 0;
    device->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);
    device->SetRenderState(D3DRS_LIGHTING, FALSE);
    device->SetRenderState(D3DRS_ZENABLE, TRUE);
    device->SetRenderState(D3DRS_ZWRITEENABLE, TRUE);
    device->SetRenderState(D3DRS_ALPHABLENDENABLE, FALSE);
    device->SetRenderState(D3DRS_ALPHATESTENABLE, cutout ? TRUE : FALSE);
    device->SetRenderState(D3DRS_ALPHAREF, 128);
    device->SetRenderState(D3DRS_ALPHAFUNC, D3DCMP_GREATEREQUAL);
    // Stage 0: main texture on UV set 0.
    device->SetTexture(0, textures[tex]);
    device->SetTextureStageState(0, D3DTSS_TEXCOORDINDEX, 0);
    device->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_SELECTARG1);
    device->SetTextureStageState(0, D3DTSS_COLORARG1, D3DTA_TEXTURE);
    device->SetTextureStageState(0, D3DTSS_ALPHAOP, D3DTOP_SELECTARG1);
    device->SetTextureStageState(0, D3DTSS_ALPHAARG1, D3DTA_TEXTURE);
    // Stage 1: lightmap on UV set 1, MODULATE2X (baked lighting keeps brightness).
    device->SetTexture(1, textures[lmTex]);
    device->SetTextureStageState(1, D3DTSS_TEXCOORDINDEX, 1);
    device->SetTextureStageState(1, D3DTSS_COLOROP, D3DTOP_MODULATE2X);
    device->SetTextureStageState(1, D3DTSS_COLORARG1, D3DTA_TEXTURE);
    device->SetTextureStageState(1, D3DTSS_COLORARG2, D3DTA_CURRENT);
    device->SetTextureStageState(1, D3DTSS_ALPHAOP, D3DTOP_SELECTARG2);
    device->SetTextureStageState(1, D3DTSS_ALPHAARG2, D3DTA_CURRENT);
    device->SetSamplerState(1, D3DSAMP_MINFILTER, D3DTEXF_LINEAR);
    device->SetSamplerState(1, D3DSAMP_MAGFILTER, D3DTEXF_LINEAR);

    device->SetFVF(D3DFVF_XYZ | D3DFVF_TEX2);
    device->DrawIndexedPrimitiveUP(D3DPT_TRIANGLELIST, 0, vc, ic / 3,
                                   idx, D3DFMT_INDEX16, v.data(), sizeof(V));
    // Disable stage 1 so following single-texture draws aren't modulated.
    device->SetTextureStageState(1, D3DTSS_COLOROP, D3DTOP_DISABLE);
    device->SetTextureStageState(1, D3DTSS_ALPHAOP, D3DTOP_DISABLE);
    device->SetTexture(1, nullptr);
    device->SetRenderState(D3DRS_ZENABLE, FALSE);
    device->SetRenderState(D3DRS_ALPHATESTENABLE, FALSE);
}

void CRendererD3D::SetViewport(int x, int y, int w, int h) {
    if (!device) return;
    if (w <= 0 || h <= 0) { x = 0; y = 0; w = width; h = height; vpW = vpH = 0; }
    else { vpW = w; vpH = h; }
    D3DVIEWPORT9 vp = { (DWORD)x, (DWORD)y, (DWORD)w, (DWORD)h, 0.0f, 1.0f };
    device->SetViewport(&vp);
}

void CRendererD3D::Shutdown()
{
    for (auto* t : textures) if (t) t->Release();
    textures.clear(); texIndex.clear();
    if (device) { device->Release(); device = nullptr; }
    if (d3d)    { d3d->Release();    d3d    = nullptr; }
}

// 2D filled rect via pre-transformed vertices (no world/view matrix needed).
void CRendererD3D::DrawRect(int x, int y, int w, int h, uint32_t argb)
{
    if (!device) return;
    struct TL { float x, y, z, rhw; D3DCOLOR c; };
    TL v[4] = {
        { (float)x,       (float)y,       0.f, 1.f, argb },
        { (float)(x + w), (float)y,       0.f, 1.f, argb },
        { (float)x,       (float)(y + h), 0.f, 1.f, argb },
        { (float)(x + w), (float)(y + h), 0.f, 1.f, argb },
    };
    device->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);
    device->SetRenderState(D3DRS_LIGHTING, FALSE);
    device->SetRenderState(D3DRS_ZENABLE, FALSE);
    device->SetTexture(0, nullptr);
    device->SetFVF(D3DFVF_XYZRHW | D3DFVF_DIFFUSE);
    device->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 2, v, sizeof(TL));
}

// Build filename -> full path index over the UI texture tree (once).
int CRendererD3D::ResolveAndLoad(const std::string& name) {
    namespace fs = std::filesystem;
    if (!fileIndexBuilt) {
        fileIndexBuilt = true;
        // Scan xui/image FIRST so UI textures win name collisions (emplace keeps
        // the first inserted); then the whole tree for intro/loading/etc.
        auto scan = [&](const char* root) {
            std::error_code ec;
            for (auto it = fs::recursive_directory_iterator(root, ec);
                 !ec && it != fs::recursive_directory_iterator(); it.increment(ec)) {
                if (it->is_regular_file(ec)) {
                    std::string fn = it->path().filename().string();
                    for (auto& c : fn) c = (char)tolower(c);
                    std::string full = it->path().string();
                    auto ex = fileIndex.find(fn);
                    // On duplicate filenames prefer the SHORTER path (e.g.
                    // lobby/_eu_login_bg.dds over lobby/clubmanager_image/...).
                    if (ex == fileIndex.end() || full.size() < ex->second.size())
                        fileIndex[fn] = full;
                }
            }
        };
        scan("resources/resources/xui/image");
        scan(kTexRoot);
    }
    EnsureD3DX();
    if (!g_createTex) return -1;

    // A name with a path separator is a .scn-relative texture: load it directly
    // (the client resolves textures relative to the model's own folder, so the
    // right variant wins over same-named files elsewhere). .tga -> .dds fallback.
    if (name.find('/') != std::string::npos || name.find('\\') != std::string::npos) {
        std::string paths[2] = { name, name };
        if (name.size() > 4 && (name.compare(name.size()-4,4,".tga")==0 || name.compare(name.size()-4,4,".TGA")==0))
            paths[1] = name.substr(0, name.size()-4) + ".dds";
        for (const std::string& p : paths) {
            IDirect3DTexture9* dt = nullptr;
            if (SUCCEEDED(g_createTex(device, p.c_str(), &dt)) && dt) {
                D3DSURFACE_DESC d = {}; dt->GetLevelDesc(0, &d);
                textures.push_back(dt); texSize.push_back({ (int)d.Width, (int)d.Height });
                return (int)textures.size() - 1;
            }
        }
        return -1;
    }

    std::string key = name;
    for (auto& c : key) c = (char)tolower(c);
    auto f = fileIndex.find(key);
    // Source data names textures .tga or .bmp; both ship as .dds in the actual tree.
    if (f == fileIndex.end() && key.size() > 4 &&
        (key.compare(key.size() - 4, 4, ".tga") == 0 || key.compare(key.size() - 4, 4, ".bmp") == 0))
        f = fileIndex.find(key.substr(0, key.size() - 4) + ".dds");
    if (f == fileIndex.end()) return -1;
    if (name.find("login_bg") != std::string::npos || name.find("logo_back") != std::string::npos)
        DbgLog("[tex] resolve '%s' -> %s", name.c_str(), f->second.c_str());
    IDirect3DTexture9* tex = nullptr;
    if (FAILED(g_createTex(device, f->second.c_str(), &tex)) || !tex) return -1;
    D3DSURFACE_DESC sd = {};
    tex->GetLevelDesc(0, &sd);
    textures.push_back(tex);
    texSize.push_back({ (int)sd.Width, (int)sd.Height });
    return (int)textures.size() - 1;
}

bool CRendererD3D::GetTextureSize(int tex, int& w, int& h) {
    if (tex < 0 || tex >= (int)texSize.size()) { w = h = 0; return false; }
    w = texSize[tex].first; h = texSize[tex].second;
    return true;
}

int CRendererD3D::LoadTexture(const char* name) {
    if (!name || !*name) return -1;
    std::string key = name;
    // "_rc_" in the .xui is the region-code placeholder; the client substitutes
    // the real region prefix at runtime. EU here -> "_eu_".
    if (key.rfind("_rc_", 0) == 0) key = "_eu_" + key.substr(4);
    auto it = texIndex.find(key);
    if (it != texIndex.end()) return it->second;   // cached (may be -1)
    int idx = ResolveAndLoad(key);
    DbgLog("[tex] load '%s' -> %d", key.c_str(), idx);
    texIndex.emplace(key, idx);
    return idx;
}

void CRendererD3D::DrawImage(int x, int y, int w, int h, int tex,
                             float u0, float v0, float u1, float v1) {
    if (!device || tex < 0 || tex >= (int)textures.size()) return;
    static int dc = 0;
    if (dc < 60) { DbgLog("[img] #%d rect=(%d,%d %dx%d) tex=%d", dc, x, y, w, h, tex); dc++; }
    struct TLT { float x, y, z, rhw, u, v; };
    // -0.5 texel offset for correct D3D9 texel-to-pixel mapping.
    TLT v[4] = {
        { x - 0.5f,       y - 0.5f,       0.f, 1.f, u0, v0 },
        { x + w - 0.5f,   y - 0.5f,       0.f, 1.f, u1, v0 },
        { x - 0.5f,       y + h - 0.5f,   0.f, 1.f, u0, v1 },
        { x + w - 0.5f,   y + h - 0.5f,   0.f, 1.f, u1, v1 },
    };
    device->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);
    device->SetRenderState(D3DRS_LIGHTING, FALSE);
    device->SetRenderState(D3DRS_ZENABLE, FALSE);
    device->SetRenderState(D3DRS_ALPHABLENDENABLE, TRUE);
    device->SetRenderState(D3DRS_SRCBLEND, D3DBLEND_SRCALPHA);
    device->SetRenderState(D3DRS_DESTBLEND, D3DBLEND_INVSRCALPHA);
    device->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_SELECTARG1);
    device->SetTextureStageState(0, D3DTSS_COLORARG1, D3DTA_TEXTURE);
    device->SetTextureStageState(0, D3DTSS_ALPHAOP, D3DTOP_SELECTARG1);
    device->SetTextureStageState(0, D3DTSS_ALPHAARG1, D3DTA_TEXTURE);
    device->SetSamplerState(0, D3DSAMP_MINFILTER, D3DTEXF_LINEAR);
    device->SetSamplerState(0, D3DSAMP_MAGFILTER, D3DTEXF_LINEAR);
    device->SetTexture(0, textures[tex]);
    device->SetFVF(D3DFVF_XYZRHW | D3DFVF_TEX1);
    device->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 2, v, sizeof(TLT));
}
