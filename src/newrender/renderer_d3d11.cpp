// CRendererD3D11 - Direct3D11 renderer implementation.
//
// Equivalent of the DX9 CRenderer_D3D::Init/Present, on the D3D11 model:
//   Direct3DCreate9 + CreateDevice   -> D3D11CreateDeviceAndSwapChain
//   device->Clear                    -> ClearRenderTargetView/ClearDepthStencilView
//   device->Present                  -> swapChain->Present
// D3D9 present-params map onto DXGI_SWAP_CHAIN_DESC (see documentacion.txt s.12).

#include "newrender/renderer_d3d11.h"
#include <d3d11.h>

#pragma comment(lib, "d3d11.lib")

bool CRendererD3D11::Init()
{
    DXGI_SWAP_CHAIN_DESC sd = {};
    sd.BufferCount        = 1;                                  // BackBufferCount
    sd.BufferDesc.Width   = width;                             // BackBufferWidth
    sd.BufferDesc.Height  = height;                            // BackBufferHeight
    sd.BufferDesc.Format  = DXGI_FORMAT_R8G8B8A8_UNORM;        // backbuffer format
    sd.BufferDesc.RefreshRate.Numerator   = vsync ? 60 : 0;
    sd.BufferDesc.RefreshRate.Denominator = vsync ? 1  : 0;
    sd.BufferUsage        = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.OutputWindow       = hwnd;
    sd.SampleDesc.Count   = 1;                                 // MultiSampleType
    sd.SampleDesc.Quality = 0;
    sd.Windowed           = fullscreen ? FALSE : TRUE;
    sd.SwapEffect         = DXGI_SWAP_EFFECT_DISCARD;          // SwapEffect DISCARD

    UINT flags = 0;
#if defined(_DEBUG)
    flags |= D3D11_CREATE_DEVICE_DEBUG;
#endif
    D3D_FEATURE_LEVEL got;
    HRESULT hr = D3D11CreateDeviceAndSwapChain(
        nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, flags,
        nullptr, 0, D3D11_SDK_VERSION,
        &sd, &swapChain, &device, &got, &context);
    if (FAILED(hr)) return false;

    // Backbuffer -> render target view.
    ID3D11Texture2D* back = nullptr;
    if (FAILED(swapChain->GetBuffer(0, __uuidof(ID3D11Texture2D), (void**)&back)))
        return false;
    hr = device->CreateRenderTargetView(back, nullptr, &rtv);
    back->Release();
    if (FAILED(hr)) return false;

    // Depth/stencil (D3D9 AutoDepthStencil D24X8 -> D24_UNORM_S8).
    D3D11_TEXTURE2D_DESC dd = {};
    dd.Width = width; dd.Height = height; dd.MipLevels = 1; dd.ArraySize = 1;
    dd.Format = DXGI_FORMAT_D24_UNORM_S8_UINT;
    dd.SampleDesc.Count = 1;
    dd.Usage = D3D11_USAGE_DEFAULT;
    dd.BindFlags = D3D11_BIND_DEPTH_STENCIL;
    ID3D11Texture2D* depth = nullptr;
    if (SUCCEEDED(device->CreateTexture2D(&dd, nullptr, &depth))) {
        device->CreateDepthStencilView(depth, nullptr, &dsv);
        depth->Release();
    }

    D3D11_VIEWPORT vp = { 0.0f, 0.0f, (float)width, (float)height, 0.0f, 1.0f };
    context->RSSetViewports(1, &vp);
    return true;
}

bool CRendererD3D11::CanRender()
{
    return device != nullptr && swapChain != nullptr;
}

void CRendererD3D11::BeginFrame()
{
    if (!context) return;
    context->OMSetRenderTargets(1, &rtv, dsv);
    const float clear[4] = { 20.0f / 255, 24.0f / 255, 32.0f / 255, 1.0f };
    if (rtv) context->ClearRenderTargetView(rtv, clear);
    if (dsv) context->ClearDepthStencilView(dsv,
                 D3D11_CLEAR_DEPTH | D3D11_CLEAR_STENCIL, 1.0f, 0);
}

void CRendererD3D11::Present()
{
    if (swapChain) swapChain->Present(vsync ? 1 : 0, 0);
}

void CRendererD3D11::Shutdown()
{
    if (dsv)       { dsv->Release();       dsv = nullptr; }
    if (rtv)       { rtv->Release();       rtv = nullptr; }
    if (swapChain) { swapChain->Release(); swapChain = nullptr; }
    if (context)   { context->Release();   context = nullptr; }
    if (device)    { device->Release();    device = nullptr; }
}
