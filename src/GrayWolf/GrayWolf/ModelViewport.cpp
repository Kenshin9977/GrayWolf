#include "stdafx.h"

#include "ModelViewport.h"
#include "ModelPreview.h"   // LoadPreviewGeometry / PreviewGeometry
#include "ModelPreviewGPU.h" // UploadPreviewMesh

#include <d3d11.h>
#include <wrl/client.h>
#include <windowsx.h>       // GET_X_LPARAM / GET_Y_LPARAM

#include <vector>
#include <algorithm>

#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")

using Microsoft::WRL::ComPtr;
using namespace DirectX;

namespace
{
    // Input + camera state the window proc mutates and the render loop reads.
    struct Viewport
    {
        float Yaw = 0.9f;      // around Z (up)
        float Pitch = 0.32f;   // elevation, radians
        float DistScale = 1.0f;// wheel zoom multiplier on the framing distance
        bool  Dragging = false;
        bool  Panning = false; // middle button held
        float PanDx = 0, PanDy = 0; // pixels dragged since the last frame, applied by the render loop
        XMFLOAT3 Pan{};
        int   LastX = 0, LastY = 0;
        int   Width = 900, Height = 700;
        bool  Resized = false;
        bool  Quit = false;
    };

    LRESULT CALLBACK WndProc(HWND Hwnd, UINT Msg, WPARAM W, LPARAM L)
    {
        Viewport* V = reinterpret_cast<Viewport*>(GetWindowLongPtr(Hwnd, GWLP_USERDATA));
        switch (Msg)
        {
        case WM_LBUTTONDOWN:
            if (V) { V->Dragging = true; V->LastX = GET_X_LPARAM(L); V->LastY = GET_Y_LPARAM(L); SetCapture(Hwnd); }
            return 0;
        case WM_LBUTTONUP:
            if (V) { V->Dragging = false; if (!V->Panning) ReleaseCapture(); }
            return 0;
        case WM_MBUTTONDOWN:
            if (V) { V->Panning = true; V->LastX = GET_X_LPARAM(L); V->LastY = GET_Y_LPARAM(L); SetCapture(Hwnd); }
            return 0;
        case WM_MBUTTONUP:
            if (V) { V->Panning = false; if (!V->Dragging) ReleaseCapture(); }
            return 0;
        case WM_MOUSEMOVE:
            if (V && (V->Dragging || V->Panning))
            {
                const int X = GET_X_LPARAM(L), Y = GET_Y_LPARAM(L);
                if (V->Panning)
                {
                    V->PanDx += (float)(X - V->LastX);
                    V->PanDy += (float)(Y - V->LastY);
                }
                else
                {
                    V->Yaw   -= (X - V->LastX) * 0.01f;   // the model follows the mouse
                    V->Pitch += (Y - V->LastY) * 0.01f;
                    V->Pitch = std::max(-1.50f, std::min(1.50f, V->Pitch));
                }
                V->LastX = X; V->LastY = Y;
            }
            return 0;
        case WM_MOUSEWHEEL:
            if (V)
            {
                const int Delta = GET_WHEEL_DELTA_WPARAM(W);
                V->DistScale *= (Delta > 0) ? 0.88f : 1.136f;
                V->DistScale = std::max(0.1f, std::min(8.0f, V->DistScale));
            }
            return 0;
        case WM_SIZE:
            if (V) { V->Width = std::max(1, (int)LOWORD(L)); V->Height = std::max(1, (int)HIWORD(L)); V->Resized = true; }
            return 0;
        case WM_KEYDOWN:
            if (V && W == VK_ESCAPE) { V->Quit = true; PostMessage(Hwnd, WM_CLOSE, 0, 0); }
            return 0;
        case WM_CLOSE:
            if (V) V->Quit = true;
            DestroyWindow(Hwnd);
            return 0;
        case WM_DESTROY:
            PostQuitMessage(0);
            return 0;
        }
        return DefWindowProc(Hwnd, Msg, W, L);
    }
}

bool PreviewModelInteractive(const std::vector<std::string>& SemodelPaths)
{
    PreviewGeometry Geo;
    if (!LoadPreviewGeometry(SemodelPaths, Geo)) return false;
    const UINT VertexCount = (UINT)(Geo.Pos.size() / 3);
    if (VertexCount == 0 || Geo.Idx.empty()) return false;

    Viewport State;

    // Render at physical pixels so the 3D view is crisp under Windows display scaling (e.g. 125%),
    // instead of being drawn at logical size and bitmap-stretched (blurry). Per-monitor-v2 is loaded
    // dynamically to dodge SDK header gaps; fall back to the always-available system-DPI call.
    if (HMODULE User32 = GetModuleHandleW(L"user32.dll"))
    {
        using SetCtxFn = BOOL(WINAPI*)(HANDLE);
        if (auto Fn = reinterpret_cast<SetCtxFn>(GetProcAddress(User32, "SetProcessDpiAwarenessContext")))
            Fn(reinterpret_cast<HANDLE>(-4)); // DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2
        else
            SetProcessDPIAware();
    }

    HINSTANCE Inst = GetModuleHandle(nullptr);
    WNDCLASSEX Wc{}; Wc.cbSize = sizeof(Wc); Wc.style = CS_HREDRAW | CS_VREDRAW;
    Wc.lpfnWndProc = WndProc; Wc.hInstance = Inst; Wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    Wc.hbrBackground = (HBRUSH)(COLOR_WINDOW); Wc.lpszClassName = L"GrayWolfModelViewport";
    RegisterClassEx(&Wc);

    RECT R{ 0, 0, State.Width, State.Height };
    AdjustWindowRect(&R, WS_OVERLAPPEDWINDOW, FALSE);
    HWND Hwnd = CreateWindowEx(0, Wc.lpszClassName, L"GrayWolf - Model Preview  (drag: orbit, middle drag: pan, wheel: zoom, Esc: close)",
                               WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT,
                               R.right - R.left, R.bottom - R.top, nullptr, nullptr, Inst, nullptr);
    if (!Hwnd) return false;
    SetWindowLongPtr(Hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(&State));
    ShowWindow(Hwnd, SW_SHOW);

    DXGI_SWAP_CHAIN_DESC Scd{};
    Scd.BufferCount = 2;
    Scd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    Scd.BufferDesc.Width = State.Width; Scd.BufferDesc.Height = State.Height;
    Scd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    Scd.OutputWindow = Hwnd;
    Scd.SampleDesc.Count = 1;
    Scd.Windowed = TRUE;
    Scd.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;

    ComPtr<ID3D11Device> Dev;
    ComPtr<ID3D11DeviceContext> Ctx;
    ComPtr<IDXGISwapChain> Swap;
    const D3D_FEATURE_LEVEL Want[] = { D3D_FEATURE_LEVEL_11_0 };
    D3D_FEATURE_LEVEL Got{};
    HRESULT Hr = D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, Want, 1,
                     D3D11_SDK_VERSION, &Scd, &Swap, &Dev, &Got, &Ctx);
    if (FAILED(Hr))
        Hr = D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, Want, 1,
                     D3D11_SDK_VERSION, &Scd, &Swap, &Dev, &Got, &Ctx);
    if (FAILED(Hr)) { DestroyWindow(Hwnd); return false; }

    PreviewRenderer Renderer(Dev.Get());
    const PreviewGpuMesh Mesh = UploadPreviewMesh(Dev.Get(), Geo);
    PreviewTarget Target;
    if (!Renderer.Ok() || !Mesh.Valid) { DestroyWindow(Hwnd); return false; }

    const float BgColor[4] = { 0.086f, 0.086f, 0.098f, 1.0f }; // dark studio grey
    while (!State.Quit)
    {
        MSG Msg;
        while (PeekMessage(&Msg, nullptr, 0, 0, PM_REMOVE))
        {
            if (Msg.message == WM_QUIT) { State.Quit = true; break; }
            TranslateMessage(&Msg);
            DispatchMessage(&Msg);
        }
        if (State.Quit) break;

        if (State.Resized && State.Width > 0 && State.Height > 0)
        {
            Swap->ResizeBuffers(0, State.Width, State.Height, DXGI_FORMAT_UNKNOWN, 0);
            State.Resized = false;
        }
        if (!Target.Resize(Dev.Get(), State.Width, State.Height)) continue;

        if (State.PanDx != 0 || State.PanDy != 0)
        {
            PanPreviewCamera(OrbitPreviewCamera(Mesh, State.Yaw, State.Pitch, State.DistScale, State.Pan),
                             State.PanDx, State.PanDy, (float)State.Height, State.Pan);
            State.PanDx = State.PanDy = 0;
        }
        Renderer.Render(Ctx.Get(), &Mesh, OrbitPreviewCamera(Mesh, State.Yaw, State.Pitch, State.DistScale, State.Pan),
                        Target, BgColor);
        ComPtr<ID3D11Texture2D> Back;
        if (SUCCEEDED(Swap->GetBuffer(0, __uuidof(ID3D11Texture2D), &Back))) Ctx->CopyResource(Back.Get(), Target.Output.Get());
        Swap->Present(1, 0);
    }

    return true;
}
