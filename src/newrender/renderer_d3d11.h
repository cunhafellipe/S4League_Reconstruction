// CRendererD3D11 - Direct3D11 renderer (IRenderer backend).
//
// Drop-in replacement for CRendererD3D (DX9). The Engine is unchanged; flip
// S4_RENDER_DX11 in winmain.cpp to select this backend.
//
// Scope note: this implements the same surface the DX9 renderer has today
// (Init / CanRender / clear+present). When real geometry gets reconstructed,
// DX11 needs HLSL SM4+ shaders to replace D3D9's fixed-function pipeline - that
// shader work is the only "hard" part of the DX9->DX11 move.
#pragma once
#include "render/irenderer.h"

struct IDXGISwapChain;
struct ID3D11Device;
struct ID3D11DeviceContext;
struct ID3D11RenderTargetView;
struct ID3D11DepthStencilView;

struct CRendererD3D11 : IRenderer {
    IDXGISwapChain*         swapChain = nullptr;
    ID3D11Device*           device    = nullptr;
    ID3D11DeviceContext*    context   = nullptr;
    ID3D11RenderTargetView* rtv       = nullptr;
    ID3D11DepthStencilView* dsv       = nullptr;

    bool Init()       override;   // D3D11CreateDeviceAndSwapChain
    bool CanRender()  override;
    void BeginFrame() override;   // OMSetRenderTargets + Clear
    void Present()    override;   // swapChain->Present
    void Shutdown()   override;
};
