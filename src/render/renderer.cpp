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
#include "app/respath.h"
#include "app/reslog.h"
#include <d3d9.h>
#include <wincodec.h>
#include <vector>
#include <string>
#pragma comment(lib, "windowscodecs.lib")
#pragma comment(lib, "ole32.lib")
#include <filesystem>
#include <cstdio>
#include <algorithm>
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
    // High quality: 4x MSAA when the card supports it with this back/depth
    // buffer (DISCARD swap only).
    if (r.quality >= 2 && r.d3d && !r.swapCopy && !r.fullscreen) {
        D3DDISPLAYMODE dm = {};
        if (SUCCEEDED(r.d3d->GetAdapterDisplayMode(D3DADAPTER_DEFAULT, &dm)) &&
            SUCCEEDED(r.d3d->CheckDeviceMultiSampleType(D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, dm.Format, TRUE, D3DMULTISAMPLE_4_SAMPLES, nullptr)) &&
            SUCCEEDED(r.d3d->CheckDeviceMultiSampleType(D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, D3DFMT_D24X8, TRUE, D3DMULTISAMPLE_4_SAMPLES, nullptr))) {
            pp.MultiSampleType = D3DMULTISAMPLE_4_SAMPLES;
            pp.SwapEffect = D3DSWAPEFFECT_DISCARD;
        }
    }
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

// Rebuilds the swap chain for the current width/height/quality. Textures come
// from D3DXCreateTextureFromFile (MANAGED pool) and survive; the D3DX fonts
// hold device objects and are recreated on demand.
bool CRendererD3D::ResetDevice() {
    if (!device) return false;
    for (auto& kv : fonts) if (kv.second) reinterpret_cast<IUnknown*>(kv.second)->Release();   // ID3DXFont is only forward-declared here
    fonts.clear();
    D3DPRESENT_PARAMETERS pp;
    BuildPresentParams(*this, pp);
    const HRESULT hr = device->Reset(&pp);
    DbgLog("[renderer] Reset %dx%d quality=%d msaa=%d -> hr=0x%08lx", width, height, quality, (int)pp.MultiSampleType, (unsigned long)hr);
    if (SUCCEEDED(hr)) device->SetRenderState(D3DRS_MULTISAMPLEANTIALIAS, pp.MultiSampleType != D3DMULTISAMPLE_NONE);
    return SUCCEEDED(hr);
}

void CRendererD3D::ApplyMeshSampler(unsigned stage) {
    if (!device) return;
    D3DCAPS9 caps = {};
    device->GetDeviceCaps(&caps);
    if (quality >= 2 && (caps.TextureFilterCaps & D3DPTFILTERCAPS_MINFANISOTROPIC) && caps.MaxAnisotropy > 1) {
        device->SetSamplerState(stage, D3DSAMP_MINFILTER, D3DTEXF_ANISOTROPIC);
        device->SetSamplerState(stage, D3DSAMP_MAXANISOTROPY, (std::min)(8ul, (unsigned long)caps.MaxAnisotropy));
    } else {
        device->SetSamplerState(stage, D3DSAMP_MINFILTER, D3DTEXF_LINEAR);
    }
    device->SetSamplerState(stage, D3DSAMP_MAGFILTER, D3DTEXF_LINEAR);
    device->SetSamplerState(stage, D3DSAMP_MIPFILTER, quality >= 1 ? D3DTEXF_LINEAR : D3DTEXF_POINT);
}

void CRendererD3D::BeginFrame()
{
    if (!device) return;
    bool reset = false;
    if (pendingW > 0 && pendingH > 0 && (pendingW != width || pendingH != height)) {
        width = pendingW; height = pendingH; vpW = vpH = 0; reset = true;
    }
    pendingW = pendingH = 0;
    if (pendingQuality >= 0 && pendingQuality != quality) {
        const bool msaaChange = (pendingQuality >= 2) != (quality >= 2);
        quality = pendingQuality;
        reset = reset || msaaChange;
    }
    pendingQuality = -1;
    if (reset) ResetDevice();
    device->Clear(0, nullptr, D3DCLEAR_TARGET | D3DCLEAR_ZBUFFER,
                  D3DCOLOR_XRGB(20, 24, 32), 1.0f, 0);
    device->BeginScene();
}

void CRendererD3D::Present()
{
    if (!device) return;
    device->EndScene();
    if (screenshotPending) { screenshotPending = false; SaveScreenshot(); }
    device->Present(nullptr, nullptr, nullptr, nullptr);
}

// The back buffer (resolved first: it may be multisampled) copied to system
// memory and written as a PNG through WIC into screenshots/.
void CRendererD3D::SaveScreenshot()
{
    IDirect3DSurface9* back = nullptr;
    if (FAILED(device->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &back))) return;
    D3DSURFACE_DESC d; back->GetDesc(&d);
    IDirect3DSurface9 *resolved = nullptr, *sys = nullptr;
    bool ok = SUCCEEDED(device->CreateRenderTarget(d.Width, d.Height, d.Format, D3DMULTISAMPLE_NONE, 0, FALSE, &resolved, nullptr)) &&
              SUCCEEDED(device->StretchRect(back, nullptr, resolved, nullptr, D3DTEXF_NONE)) &&
              SUCCEEDED(device->CreateOffscreenPlainSurface(d.Width, d.Height, d.Format, D3DPOOL_SYSTEMMEM, &sys, nullptr)) &&
              SUCCEEDED(device->GetRenderTargetData(resolved, sys));
    std::string path;
    D3DLOCKED_RECT lr;
    if (ok && (d.Format == D3DFMT_X8R8G8B8 || d.Format == D3DFMT_A8R8G8B8) && SUCCEEDED(sys->LockRect(&lr, nullptr, D3DLOCK_READONLY))) {
        std::vector<BYTE> px((size_t)d.Width * d.Height * 4);
        for (UINT y = 0; y < d.Height; ++y) {
            const BYTE* src = (const BYTE*)lr.pBits + (size_t)y * lr.Pitch;
            BYTE* dst = px.data() + (size_t)y * d.Width * 4;
            memcpy(dst, src, d.Width * 4);
            for (UINT x = 0; x < d.Width; ++x) dst[x * 4 + 3] = 255;
        }
        sys->UnlockRect();
        CreateDirectoryA("screenshots", nullptr);
        SYSTEMTIME t; GetLocalTime(&t);
        char name[96];
        snprintf(name, sizeof(name), "screenshots/S4_%04d-%02d-%02d_%02d-%02d-%02d_%03d.png",
                 t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond, t.wMilliseconds);
        path = name;
        const HRESULT co = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
        IWICImagingFactory* wic = nullptr; IWICStream* st = nullptr; IWICBitmapEncoder* enc = nullptr;
        IWICBitmapFrameEncode* fr = nullptr; IPropertyBag2* props = nullptr;
        wchar_t wname[96]; MultiByteToWideChar(CP_ACP, 0, name, -1, wname, 96);
        WICPixelFormatGUID fmt = GUID_WICPixelFormat32bppBGRA;
        ok = SUCCEEDED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&wic))) &&
             SUCCEEDED(wic->CreateStream(&st)) && SUCCEEDED(st->InitializeFromFilename(wname, GENERIC_WRITE)) &&
             SUCCEEDED(wic->CreateEncoder(GUID_ContainerFormatPng, nullptr, &enc)) &&
             SUCCEEDED(enc->Initialize(st, WICBitmapEncoderNoCache)) &&
             SUCCEEDED(enc->CreateNewFrame(&fr, &props)) && SUCCEEDED(fr->Initialize(props)) &&
             SUCCEEDED(fr->SetSize(d.Width, d.Height)) && SUCCEEDED(fr->SetPixelFormat(&fmt)) &&
             SUCCEEDED(fr->WritePixels(d.Height, d.Width * 4, (UINT)px.size(), px.data())) &&
             SUCCEEDED(fr->Commit()) && SUCCEEDED(enc->Commit());
        if (props) props->Release(); if (fr) fr->Release(); if (enc) enc->Release(); if (st) st->Release(); if (wic) wic->Release();
        if (SUCCEEDED(co)) CoUninitialize();
    } else ok = false;
    if (sys) sys->Release(); if (resolved) resolved->Release(); back->Release();
    DbgLog("[screenshot] %s %s (%ux%u fmt=%d)", ok ? "saved" : "FAILED", path.c_str(), d.Width, d.Height, (int)d.Format);
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
        const std::string ttfPath = RES("resources/resources/gui/2002.ttf");
        const char* ttf = ttfPath.c_str();
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
    device->SetTexture(0, Tex(tex));
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
void CRendererD3D::DrawParticles(const float* particles, int count, int tex, bool additive, int renderType, const float* uv) {
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
        const float* p = particles + i*12;
        const float hw = p[7] * 0.5f, hh = p[8] * 0.5f;
        // Axes of this quad: camera right/up, or for a streak the velocity
        // projected on the view plane (long side) and its perpendicular.
        float ax[3] = { right[0], right[1], right[2] }, ay[3] = { up[0], up[1], up[2] };
        float hhLocal = hh;
        float hwLocal = hw;
        if (renderType >= 10) {
            // S10 sub_1C8D470 builds corners directly at +/- sizeX,sizeY.
            // S10 sub_1C8D470 puts the corners at +-key size, but on screen the
            // client's effects measure half that (side-by-side with the
            // original: smash rail right at half, revolver flash and wall hit
            // double at full) - a scale somewhere in the effect node chain not
            // found yet. Half extents = key size / 2 until that is located.
            hwLocal = p[7] * 0.5f; hhLocal = p[8] * 0.5f;
            const int t = renderType - 10;
            float bx[3], by[3];   // the quad's plane before its own rotation
            if (t == 1) {
                const float vl = std::sqrt(p[9]*p[9] + p[10]*p[10] + p[11]*p[11]);
                if (vl > 1e-3f) {
                    ay[0] = p[9]/vl; ay[1] = p[10]/vl; ay[2] = p[11]/vl;
                    // Across = view direction x velocity (client: cross with the eye ray).
                    const float e[3] = { p[0]-camEye[0], p[1]-camEye[1], p[2]-camEye[2] };
                    float cx[3] = { e[1]*ay[2]-e[2]*ay[1], e[2]*ay[0]-e[0]*ay[2], e[0]*ay[1]-e[1]*ay[0] };
                    const float cl = std::sqrt(cx[0]*cx[0] + cx[1]*cx[1] + cx[2]*cx[2]);
                    if (cl > 1e-5f) { ax[0] = cx[0]/cl; ax[1] = cx[1]/cl; ax[2] = cx[2]/cl; }
                }
            } else {
                if (t == 2) {          // upright, facing the camera horizontally
                    float hx = right[0], hz = right[2]; const float hl = std::sqrt(hx*hx + hz*hz);
                    if (hl > 1e-5f) { hx /= hl; hz /= hl; } else { hx = 1.f; hz = 0.f; }
                    bx[0] = hx; bx[1] = 0.f; bx[2] = hz; by[0] = 0.f; by[1] = 1.f; by[2] = 0.f;
                } else if (t == 3) {   // flat on the ground
                    bx[0] = 1.f; bx[1] = 0.f; bx[2] = 0.f; by[0] = 0.f; by[1] = 0.f; by[2] = 1.f;
                } else {               // camera-facing
                    std::memcpy(bx, right, sizeof(bx)); std::memcpy(by, up, sizeof(by));
                }
                const float rr = p[9] * 3.14159265f / 180.f, cr = std::cos(rr), sr = std::sin(rr);
                for (int k = 0; k < 3; ++k) { ax[k] = bx[k]*cr + by[k]*sr; ay[k] = -bx[k]*sr + by[k]*cr; }
            }
        }
        if (renderType == 2) {
            // True 3D segment: long side exactly along p[9..11] (full length),
            // short side facing the camera - perspective foreshortens it.
            const float sv[3] = { p[9], p[10], p[11] };
            const float sl = std::sqrt(sv[0]*sv[0] + sv[1]*sv[1] + sv[2]*sv[2]);
            if (sl > 1e-3f) {
                ay[0] = sv[0]/sl; ay[1] = sv[1]/sl; ay[2] = sv[2]/sl;
                float cx[3] = { fwd[1]*ay[2]-fwd[2]*ay[1], fwd[2]*ay[0]-fwd[0]*ay[2], fwd[0]*ay[1]-fwd[1]*ay[0] };
                const float cl = std::sqrt(cx[0]*cx[0] + cx[1]*cx[1] + cx[2]*cx[2]);
                if (cl > 1e-5f) { ax[0] = cx[0]/cl; ax[1] = cx[1]/cl; ax[2] = cx[2]/cl; }
                hhLocal = sl * 0.5f;
            }
        }
        if (renderType == 1) {
            float v[3] = { p[9], p[10], p[11] };
            const float vf = v[0]*fwd[0] + v[1]*fwd[1] + v[2]*fwd[2];
            v[0] -= fwd[0]*vf; v[1] -= fwd[1]*vf; v[2] -= fwd[2]*vf;
            const float vl = std::sqrt(v[0]*v[0] + v[1]*v[1] + v[2]*v[2]);
            if (vl > 1e-3f) {
                ay[0] = v[0]/vl; ay[1] = v[1]/vl; ay[2] = v[2]/vl;
                ax[0] = fwd[1]*ay[2] - fwd[2]*ay[1]; ax[1] = fwd[2]*ay[0] - fwd[0]*ay[2]; ax[2] = fwd[0]*ay[1] - fwd[1]*ay[0];
            }
        }
        auto c01 = [](float x) { return x < 0.f ? 0.f : (x > 1.f ? 1.f : x); };
        DWORD color = D3DCOLOR_COLORVALUE(c01(p[3]), c01(p[4]), c01(p[5]), c01(p[6]));
        PV* q = &v[i*4];
        for (int c = 0; c < 4; ++c) {
            float sx = (c==0||c==3) ? -hwLocal : hwLocal;
            float sy = (c==0||c==1) ? -hhLocal : hhLocal;
            q[c].x = p[0] + ax[0]*sx + ay[0]*sy;
            q[c].y = p[1] + ax[1]*sx + ay[1]*sy;
            q[c].z = p[2] + ax[2]*sx + ay[2]*sy;
            q[c].color = color;
            static const int uvCorner[4] = { 1, 3, 2, 0 }; // BL, BR, TR, TL
            q[c].u = uv ? uv[uvCorner[c]*2] : ((c==1||c==2) ? 1.f : 0.f);
            q[c].v = uv ? uv[uvCorner[c]*2+1] : ((c==0||c==1) ? 1.f : 0.f);
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
        device->SetTexture(0, Tex(tex));
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

void CRendererD3D::DrawBeam(const float* pts, int count, float width, uint32_t argb, int tex, bool additive, float u0, float u1) {
    if (!device || count < 2) return;
    struct PV { float x,y,z; DWORD color; float u,v; };
    std::vector<PV> v((size_t)count * 2);
    float total = 0.f;
    for (int i = 1; i < count; ++i) {
        const float* a = pts + (i-1)*3; const float* b = pts + i*3;
        total += std::sqrt((b[0]-a[0])*(b[0]-a[0]) + (b[1]-a[1])*(b[1]-a[1]) + (b[2]-a[2])*(b[2]-a[2]));
    }
    if (total < 1e-3f) return;
    float run = 0.f;
    for (int i = 0; i < count; ++i) {
        const float* p = pts + i*3;
        const float* a = pts + (i > 0 ? i-1 : i)*3; const float* b = pts + (i < count-1 ? i+1 : i)*3;
        float t[3] = { b[0]-a[0], b[1]-a[1], b[2]-a[2] };
        // Across = tangent x (point -> eye): the ribbon always faces the camera.
        const float e[3] = { camEye[0]-p[0], camEye[1]-p[1], camEye[2]-p[2] };
        float s[3] = { t[1]*e[2]-t[2]*e[1], t[2]*e[0]-t[0]*e[2], t[0]*e[1]-t[1]*e[0] };
        const float sl = std::sqrt(s[0]*s[0] + s[1]*s[1] + s[2]*s[2]);
        if (sl > 1e-5f) { s[0] /= sl; s[1] /= sl; s[2] /= sl; } else { s[0] = 0; s[1] = 1; s[2] = 0; }
        if (i > 0) { const float* q = pts + (i-1)*3; run += std::sqrt((p[0]-q[0])*(p[0]-q[0]) + (p[1]-q[1])*(p[1]-q[1]) + (p[2]-q[2])*(p[2]-q[2])); }
        const float u = u0 + (u1 - u0) * (run / total), hw = width * 0.5f;
        v[i*2]   = { p[0] - s[0]*hw, p[1] - s[1]*hw, p[2] - s[2]*hw, (DWORD)argb, u, 1.f };
        v[i*2+1] = { p[0] + s[0]*hw, p[1] + s[1]*hw, p[2] + s[2]*hw, (DWORD)argb, u, 0.f };
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
    const bool textured = (tex >= 0 && tex < (int)textures.size());
    device->SetTexture(0, textured ? Tex(tex) : nullptr);
    device->SetTextureStageState(0, D3DTSS_COLOROP, textured ? D3DTOP_MODULATE : D3DTOP_SELECTARG2);
    device->SetTextureStageState(0, D3DTSS_COLORARG1, D3DTA_TEXTURE);
    device->SetTextureStageState(0, D3DTSS_COLORARG2, D3DTA_DIFFUSE);
    device->SetTextureStageState(0, D3DTSS_ALPHAOP, textured ? D3DTOP_MODULATE : D3DTOP_SELECTARG2);
    device->SetTextureStageState(0, D3DTSS_ALPHAARG1, D3DTA_TEXTURE);
    device->SetTextureStageState(0, D3DTSS_ALPHAARG2, D3DTA_DIFFUSE);
    device->SetSamplerState(0, D3DSAMP_ADDRESSU, D3DTADDRESS_WRAP);
    device->SetSamplerState(0, D3DSAMP_ADDRESSV, D3DTADDRESS_CLAMP);
    device->SetSamplerState(0, D3DSAMP_MINFILTER, D3DTEXF_LINEAR);
    device->SetSamplerState(0, D3DSAMP_MAGFILTER, D3DTEXF_LINEAR);
    device->SetFVF(D3DFVF_XYZ | D3DFVF_DIFFUSE | D3DFVF_TEX1);
    device->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, count*2 - 2, v.data(), sizeof(PV));
    device->SetSamplerState(0, D3DSAMP_ADDRESSV, D3DTADDRESS_WRAP);
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
    if (meshCapture && vc > 0 && ic > 0) {
        CapturedMesh c;
        c.verts.assign(posUV, posUV + (size_t)vc * 5); c.vertCount = vc;
        c.idx.assign(idx, idx + ic); c.tex = tex; c.flags = flags;
        if (world16) std::memcpy(c.world, world16, sizeof(c.world));
        else { c.world[0] = c.world[5] = c.world[10] = c.world[15] = 1.f; }
        meshCapture->push_back(std::move(c));
        return;
    }
    if (!device || vc <= 0 || ic <= 0) return;
    // posUV is already xyz uv per vertex: sent as is (no repack).
    BeginMeshStates(tex, world16, flags);
    device->SetFVF(D3DFVF_XYZ | D3DFVF_TEX1);
    device->DrawIndexedPrimitiveUP(D3DPT_TRIANGLELIST, 0, vc, ic / 3,
                                   idx, D3DFMT_INDEX16, posUV, sizeof(float) * 5);
    EndMeshStates();
}

void CRendererD3D::BeginMeshStates(int tex, const float* world16, uint32_t flags) {
    D3DMATRIX world;
    if (world16) std::memcpy(&world, world16, sizeof(world));
    else { world = {}; world._11 = world._22 = world._33 = world._44 = 1.0f; }
    device->SetTransform(D3DTS_WORLD, &world);

    // Per-mesh state from the .scn RenderFlag (io_s4league_scn): 2=TRANSPARENT
    // (alpha blend), 4=CUTOUT (alpha test, opaque), 8=NO_CULLING, 64=NO_DEPTH_WRITE.
    // 32 = FLARE and 0x40000000 (set by the caller from an "alphablend2" name
    // tag) are ADDITIVE: glow/energy meshes add light instead of covering.
    const bool additiveMesh = (flags & 32) != 0 || (flags & 0x40000000u) != 0;
    const bool transparent = (flags & 2) != 0 || additiveMesh;
    const bool cutout      = (flags & 4) != 0 && !additiveMesh;
    const bool noZWrite    = (flags & 64) != 0;
    device->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);      // winding unconfirmed -> no cull
    device->SetRenderState(D3DRS_LIGHTING, FALSE);             // unlit textured (S4 characters)
    device->SetRenderState(D3DRS_ZENABLE, (flags & 0x20000000u) ? FALSE : TRUE);   // runtime: the sky pass (2048 in files = nomipmap)
    device->SetRenderState(D3DRS_ZWRITEENABLE, (transparent || noZWrite) ? FALSE : TRUE);
    bool textured = (tex >= 0 && tex < (int)textures.size());
    if (textured) {
        // Cutout (hair/edges): alpha TEST only, keep Z-write -> solid strands, no
        // sorting gaps. Transparent: alpha BLEND, no Z-write. Else opaque.
        device->SetRenderState(D3DRS_ALPHABLENDENABLE, transparent ? TRUE : FALSE);
        device->SetRenderState(D3DRS_SRCBLEND, D3DBLEND_SRCALPHA);
        device->SetRenderState(D3DRS_DESTBLEND, additiveMesh ? D3DBLEND_ONE : D3DBLEND_INVSRCALPHA);
        device->SetRenderState(D3DRS_ALPHATESTENABLE, (cutout || transparent) ? TRUE : FALSE);
        device->SetRenderState(D3DRS_ALPHAREF, cutout ? 128 : 8);   // hair needs a firm cutoff
        device->SetRenderState(D3DRS_ALPHAFUNC, D3DCMP_GREATEREQUAL);
        device->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_SELECTARG1);
        device->SetTextureStageState(0, D3DTSS_COLORARG1, D3DTA_TEXTURE);
        device->SetTextureStageState(0, D3DTSS_ALPHAOP, D3DTOP_SELECTARG1);
        device->SetTextureStageState(0, D3DTSS_ALPHAARG1, D3DTA_TEXTURE);
        if (meshFade < 0.999f) {
            // Fading effect mesh: texture x fade (colour too, for additive glow).
            const DWORD f = (DWORD)((meshFade < 0.f ? 0.f : meshFade) * 255.f);
            device->SetRenderState(D3DRS_TEXTUREFACTOR, (f << 24) | (f << 16) | (f << 8) | f);
            device->SetRenderState(D3DRS_ALPHABLENDENABLE, TRUE);
            device->SetRenderState(D3DRS_ZWRITEENABLE, FALSE);
            // Once, not twice: additive glow fades its colour (alpha stays the
            // texture's), alpha-blended meshes fade their alpha. Modulating
            // both turned a 0.7 opacity key into 0.49.
            device->SetTextureStageState(0, D3DTSS_COLOROP, additiveMesh ? D3DTOP_MODULATE : D3DTOP_SELECTARG1);
            device->SetTextureStageState(0, D3DTSS_COLORARG2, D3DTA_TFACTOR);
            device->SetTextureStageState(0, D3DTSS_ALPHAOP, additiveMesh ? D3DTOP_SELECTARG1 : D3DTOP_MODULATE);
            device->SetTextureStageState(0, D3DTSS_ALPHAARG2, D3DTA_TFACTOR);
        }
        ApplyMeshSampler(0);   // quality: trilinear / anisotropic
        device->SetTexture(0, Tex(tex));
    } else {
        // No texture -> flat color so the geometry (skinning) is visible.
        device->SetRenderState(D3DRS_ALPHABLENDENABLE, FALSE);
        device->SetRenderState(D3DRS_ALPHATESTENABLE, FALSE);
        device->SetTexture(0, nullptr);
        device->SetRenderState(D3DRS_TEXTUREFACTOR, 0xFFFFFFFF);   // missing texture -> plain white
        device->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_SELECTARG1);
        device->SetTextureStageState(0, D3DTSS_COLORARG1, D3DTA_TFACTOR);
        device->SetTextureStageState(0, D3DTSS_ALPHAOP, D3DTOP_SELECTARG1);
        device->SetTextureStageState(0, D3DTSS_ALPHAARG1, D3DTA_TFACTOR);
    }
}

void CRendererD3D::EndMeshStates() {
    device->SetRenderState(D3DRS_ZENABLE, FALSE);             // restore 2D-friendly state
    device->SetRenderState(D3DRS_ALPHATESTENABLE, FALSE);
    device->SetTextureStageState(0, D3DTSS_COLORARG2, D3DTA_CURRENT);
    device->SetTextureStageState(0, D3DTSS_ALPHAARG2, D3DTA_CURRENT);
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
    BeginLightmapStates(tex, lmTex, world16, flags);
    device->SetFVF(D3DFVF_XYZ | D3DFVF_TEX2);
    device->DrawIndexedPrimitiveUP(D3DPT_TRIANGLELIST, 0, vc, ic / 3,
                                   idx, D3DFMT_INDEX16, posUV2, sizeof(float) * 7);
    EndLightmapStates();
}

void CRendererD3D::BeginLightmapStates(int tex, int lmTex, const float* world16, uint32_t flags) {
    D3DMATRIX world;
    if (world16) std::memcpy(&world, world16, sizeof(world));
    else { world = {}; world._11=world._22=world._33=world._44=1.0f; }
    device->SetTransform(D3DTS_WORLD, &world);

    const bool cutout = (flags & 4) != 0;
    device->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);
    device->SetRenderState(D3DRS_LIGHTING, FALSE);
    device->SetRenderState(D3DRS_ZENABLE, (flags & 0x20000000u) ? FALSE : TRUE);   // runtime: the sky pass
    device->SetRenderState(D3DRS_ZWRITEENABLE, (flags & 64) ? FALSE : TRUE);
    device->SetRenderState(D3DRS_ALPHABLENDENABLE, FALSE);
    device->SetRenderState(D3DRS_ALPHATESTENABLE, cutout ? TRUE : FALSE);
    device->SetRenderState(D3DRS_ALPHAREF, 128);
    device->SetRenderState(D3DRS_ALPHAFUNC, D3DCMP_GREATEREQUAL);
    // Stage 0: main texture on UV set 0.
    device->SetTexture(0, Tex(tex));
    device->SetTextureStageState(0, D3DTSS_TEXCOORDINDEX, 0);
    device->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_SELECTARG1);
    device->SetTextureStageState(0, D3DTSS_COLORARG1, D3DTA_TEXTURE);
    device->SetTextureStageState(0, D3DTSS_ALPHAOP, D3DTOP_SELECTARG1);
    device->SetTextureStageState(0, D3DTSS_ALPHAARG1, D3DTA_TEXTURE);
    // Stage 1: lightmap on UV set 1, MODULATE2X (baked lighting keeps brightness).
    device->SetTexture(1, Tex(lmTex));
    device->SetTextureStageState(1, D3DTSS_TEXCOORDINDEX, 1);
    device->SetTextureStageState(1, D3DTSS_COLOROP, D3DTOP_MODULATE2X);
    device->SetTextureStageState(1, D3DTSS_COLORARG1, D3DTA_TEXTURE);
    device->SetTextureStageState(1, D3DTSS_COLORARG2, D3DTA_CURRENT);
    device->SetTextureStageState(1, D3DTSS_ALPHAOP, D3DTOP_SELECTARG2);
    device->SetTextureStageState(1, D3DTSS_ALPHAARG2, D3DTA_CURRENT);
    ApplyMeshSampler(0);
    ApplyMeshSampler(1);
}

void CRendererD3D::EndLightmapStates() {
    // Disable stage 1 so following single-texture draws aren't modulated.
    device->SetTextureStageState(1, D3DTSS_COLOROP, D3DTOP_DISABLE);
    device->SetTextureStageState(1, D3DTSS_ALPHAOP, D3DTOP_DISABLE);
    device->SetTexture(1, nullptr);
    device->SetRenderState(D3DRS_ZENABLE, FALSE);
    device->SetRenderState(D3DRS_ALPHATESTENABLE, FALSE);
}

int CRendererD3D::CreateStaticMesh(const float* verts, int floatsPerVert, int vc,
                                   const unsigned short* idx, int ic) {
    if (!device || vc <= 0 || ic <= 0 || (floatsPerVert != 5 && floatsPerVert != 7)) return -1;
    const DWORD fvf = floatsPerVert == 7 ? (D3DFVF_XYZ | D3DFVF_TEX2) : (D3DFVF_XYZ | D3DFVF_TEX1);
    const UINT vbBytes = (UINT)(vc * floatsPerVert * sizeof(float)), ibBytes = (UINT)(ic * sizeof(unsigned short));
    StaticMesh sm; sm.vertCount = vc; sm.floats = floatsPerVert;
    // MANAGED: survives device resets (ResetDevice) without re-uploading.
    if (FAILED(device->CreateVertexBuffer(vbBytes, D3DUSAGE_WRITEONLY, fvf, D3DPOOL_MANAGED, &sm.vb, nullptr))) return -1;
    if (FAILED(device->CreateIndexBuffer(ibBytes, D3DUSAGE_WRITEONLY, D3DFMT_INDEX16, D3DPOOL_MANAGED, &sm.ib, nullptr))) {
        sm.vb->Release(); return -1;
    }
    void* p = nullptr;
    if (SUCCEEDED(sm.vb->Lock(0, vbBytes, &p, 0))) { std::memcpy(p, verts, vbBytes); sm.vb->Unlock(); }
    if (SUCCEEDED(sm.ib->Lock(0, ibBytes, &p, 0))) { std::memcpy(p, idx, ibBytes); sm.ib->Unlock(); }
    staticMeshes.push_back(sm);
    return (int)staticMeshes.size() - 1;
}

void CRendererD3D::DrawStaticMesh(int mesh, int firstIndex, int triCount, int tex, int lmTex,
                                  const float* world16, uint32_t flags) {
    if (!device || mesh < 0 || mesh >= (int)staticMeshes.size() || triCount <= 0) return;
    const StaticMesh& sm = staticMeshes[mesh];
    if (!sm.vb || !sm.ib) return;
    const bool lm = sm.floats == 7 && lmTex >= 0 && lmTex < (int)textures.size() && tex >= 0 && tex < (int)textures.size();
    if (lm) BeginLightmapStates(tex, lmTex, world16, flags);
    else    BeginMeshStates(tex, world16, flags);
    // A 7-float mesh drawn without its lightmap: same buffer, the first UV set only.
    device->SetFVF(sm.floats == 7 ? (D3DFVF_XYZ | D3DFVF_TEX2) : (D3DFVF_XYZ | D3DFVF_TEX1));
    device->SetStreamSource(0, sm.vb, 0, (UINT)(sm.floats * sizeof(float)));
    device->SetIndices(sm.ib);
    device->DrawIndexedPrimitive(D3DPT_TRIANGLELIST, 0, 0, (UINT)sm.vertCount, (UINT)firstIndex, (UINT)triCount);
    device->SetStreamSource(0, nullptr, 0, 0);
    device->SetIndices(nullptr);
    if (lm) EndLightmapStates();
    else    EndMeshStates();
}

void CRendererD3D::ReleaseStaticMesh(int mesh) {
    if (mesh < 0 || mesh >= (int)staticMeshes.size()) return;
    StaticMesh& sm = staticMeshes[mesh];
    if (sm.vb) { sm.vb->Release(); sm.vb = nullptr; }
    if (sm.ib) { sm.ib->Release(); sm.ib = nullptr; }
}

void CRendererD3D::ReleaseStaticMeshes() {
    for (StaticMesh& sm : staticMeshes) { if (sm.vb) sm.vb->Release(); if (sm.ib) sm.ib->Release(); }
    staticMeshes.clear();
}

void CRendererD3D::SetViewport(int x, int y, int w, int h) {
    if (!device) return;
    if (w <= 0 || h <= 0) { x = 0; y = 0; w = width; h = height; vpW = vpH = 0; }
    else { vpW = w; vpH = h; }
    D3DVIEWPORT9 vp = { (DWORD)x, (DWORD)y, (DWORD)w, (DWORD)h, 0.0f, 1.0f };
    device->SetViewport(&vp);
}

void CRendererD3D::ClearViewport(uint32_t argb) {
    if (!device) return;
    device->Clear(0, nullptr, D3DCLEAR_TARGET | D3DCLEAR_ZBUFFER, (D3DCOLOR)argb, 1.0f, 0);   // viewport-clipped
}

void CRendererD3D::Shutdown()
{
    ReleaseStaticMeshes();
    for (auto* t : textures) if (t) t->Release();
    textures.clear(); texIndex.clear(); texKey.clear(); texLastUse.clear();
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
    // Own color/alpha source: the vertex color. A mesh drawn before (an
    // untextured piece selects the texture factor) left stage 0 reading
    // TFACTOR, so every UI rect came out in that color (white panels).
    device->SetRenderState(D3DRS_ALPHABLENDENABLE, TRUE);
    device->SetRenderState(D3DRS_SRCBLEND, D3DBLEND_SRCALPHA);
    device->SetRenderState(D3DRS_DESTBLEND, D3DBLEND_INVSRCALPHA);
    device->SetRenderState(D3DRS_ALPHATESTENABLE, FALSE);
    device->SetTexture(0, nullptr);
    device->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_SELECTARG1);
    device->SetTextureStageState(0, D3DTSS_COLORARG1, D3DTA_DIFFUSE);
    device->SetTextureStageState(0, D3DTSS_ALPHAOP, D3DTOP_SELECTARG1);
    device->SetTextureStageState(0, D3DTSS_ALPHAARG1, D3DTA_DIFFUSE);
    device->SetTextureStageState(1, D3DTSS_COLOROP, D3DTOP_DISABLE);
    device->SetTextureStageState(1, D3DTSS_ALPHAOP, D3DTOP_DISABLE);
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
        scan(RESC("resources/resources/xui/image"));
        scan(RESC("resources/resources"));
    }
    EnsureD3DX();
    if (!g_createTex) return -1;

    // A name with a path separator is a .scn-relative texture: load it directly
    // (the client resolves textures relative to the model's own folder, so the
    // right variant wins over same-named files elsewhere). Any image format:
    // the name as written first, then the same name in every other format
    // (.dds .tga .png .jpg .jpeg .bmp) - the .scn names the source image and
    // the tree may ship another (127_male_body asks for _atex.png, only the
    // .dds exists).
    static const char* kImageExts[] = { ".dds", ".tga", ".png", ".jpg", ".jpeg", ".bmp" };
    auto variants = [](const std::string& n) {
        std::vector<std::string> out{ n };
        const size_t dot = n.find_last_of('.');
        const size_t sep = n.find_last_of("/\\");
        if (dot == std::string::npos || (sep != std::string::npos && dot < sep)) return out;
        std::string ext = n.substr(dot);
        for (auto& c : ext) c = (char)tolower(c);
        bool isImage = false;
        for (const char* e : kImageExts) if (ext == e) isImage = true;
        if (!isImage) return out;
        for (const char* e : kImageExts) if (ext != e) out.push_back(n.substr(0, dot) + e);
        return out;
    };
    if (name.find('/') != std::string::npos || name.find('\\') != std::string::npos) {
        for (const std::string& p : variants(name)) {
            if (GetFileAttributesA(p.c_str()) == INVALID_FILE_ATTRIBUTES) continue;
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
    // Same name in any image format (source data names .tga/.bmp/.png, the
    // tree may ship .dds or another one).
    auto f = fileIndex.end();
    for (const std::string& v : variants(key)) { f = fileIndex.find(v); if (f != fileIndex.end()) break; }
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
    if (idx >= 0) {
        if (texKey.size() < textures.size()) { texKey.resize(textures.size()); texLastUse.resize(textures.size(), 0); texBytes.resize(textures.size(), 0); }
        texKey[idx] = key; texLastUse[idx] = GetTickCount();
        texBytes[idx] = TexMemory(textures[idx], texInfo);
        reslog::Loaded(reslog::TEX, key, texBytes[idx], texInfo);
    }
    return idx;
}

// Bytes of every mip level in its own format (what D3D keeps for it; the
// managed pool holds this in system memory too).
size_t CRendererD3D::TexMemory(IDirect3DTexture9* t, char* info) {
    size_t total = 0;
    D3DSURFACE_DESC d = {};
    const DWORD levels = t ? t->GetLevelCount() : 0;
    for (DWORD l = 0; l < levels; ++l) {
        if (FAILED(t->GetLevelDesc(l, &d))) break;
        const size_t px = (size_t)d.Width * d.Height;
        switch (d.Format) {
        case D3DFMT_DXT1: total += (std::max)(size_t(8), px / 2); break;
        case D3DFMT_DXT2: case D3DFMT_DXT3: case D3DFMT_DXT4: case D3DFMT_DXT5: total += (std::max)(size_t(16), px); break;
        case D3DFMT_A8: case D3DFMT_L8: case D3DFMT_P8: total += px; break;
        case D3DFMT_R5G6B5: case D3DFMT_A1R5G5B5: case D3DFMT_A4R4G4B4: case D3DFMT_X1R5G5B5: case D3DFMT_A8L8: total += px * 2; break;
        default: total += px * 4; break;
        }
    }
    if (info && t && SUCCEEDED(t->GetLevelDesc(0, &d))) {
        const char* f = d.Format == D3DFMT_DXT1 ? "DXT1" : d.Format == D3DFMT_DXT3 ? "DXT3" : d.Format == D3DFMT_DXT5 ? "DXT5" :
                        d.Format == D3DFMT_A8R8G8B8 ? "A8R8G8B8" : d.Format == D3DFMT_X8R8G8B8 ? "X8R8G8B8" : "other";
        snprintf(info, 64, "(%ux%u %s, %lu mips)", d.Width, d.Height, f, (unsigned long)levels);
    }
    return total;
}

// A slot's texture, loaded again by its name if TrimTextures released it -
// handles held by the UI, the maps and the models stay valid.
IDirect3DTexture9* CRendererD3D::Tex(int i) {
    if (i < 0 || i >= (int)textures.size()) return nullptr;
    if (i < (int)texLastUse.size()) texLastUse[i] = GetTickCount();
    if (!textures[i] && i < (int)texKey.size() && !texKey[i].empty()) {
        const int n = ResolveAndLoad(texKey[i]);
        if (n >= 0) {
            textures[i] = textures[n]; texSize[i] = texSize[n];
            textures.pop_back(); texSize.pop_back();
            DbgLog("[tex] reload '%s' -> %d", texKey[i].c_str(), i);
            if (i < (int)texBytes.size()) { texBytes[i] = TexMemory(textures[i], texInfo); reslog::Loaded(reslog::TEX, texKey[i], texBytes[i], "(reloaded)"); }
        }
    }
    return textures[i];
}

void CRendererD3D::TrimTextures(unsigned unusedMs) {
    const DWORD now = GetTickCount();
    int freed = 0; double mb = 0.0;
    for (size_t i = 0; i < textures.size() && i < texLastUse.size(); ++i) {
        if (!textures[i] || texKey[i].empty() || now - texLastUse[i] < unusedMs) continue;
        mb += texSize[i].first * (double)texSize[i].second * 4.0 * 1.33 / (1024.0 * 1024.0);
        textures[i]->Release(); textures[i] = nullptr; ++freed;
        if (i < texBytes.size()) reslog::Released(reslog::TEX, texKey[i], texBytes[i], "(not drawn for a while)");
    }
    if (freed) DbgLog("[tex] trimmed %d textures unused for %us (~%.0f MB)", freed, unusedMs / 1000, mb);
    int live = 0; double liveMb = 0.0;
    for (size_t i = 0; i < textures.size(); ++i) if (textures[i]) {
        ++live; liveMb += texSize[i].first * (double)texSize[i].second * 4.0 * 1.33 / (1024.0 * 1024.0);
    }
    DbgLog("[tex] live %d textures, ~%.0f MB at 32 bpp (DXT files are 4-8x smaller); %zu slots", live, liveMb, textures.size());
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
    device->SetTexture(0, Tex(tex));
    device->SetFVF(D3DFVF_XYZRHW | D3DFVF_TEX1);
    device->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 2, v, sizeof(TLT));
}
