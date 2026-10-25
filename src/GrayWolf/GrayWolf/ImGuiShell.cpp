#include "stdafx.h"

#include "ImGuiShell.h"
#include "ImGuiShell.Internal.h" // the shell's pieces in their own files
#include "AppLog.h"           // session log, timed steps, UI stalls
#include "ImGuiLoadGamePanel.h" // the Load Game window: a game exported from its files
#include "UiAsync.h"           // dialogs, shell opens and disk lookups off the frame
#include "SettingsManager.h"   // the list or the grid, the anims' rig

#include "imgui.h"
#include "imgui_impl_win32.h"
#include "imgui_impl_dx11.h"

#include <d3d11.h>
#include <wrl/client.h>

#include <string>

#pragma comment(lib, "imgui.lib")
#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "d3dcompiler.lib")
#pragma comment(lib, "winmm.lib")

using Microsoft::WRL::ComPtr;
using namespace ShellUi;

// From imgui_impl_win32 (forward the raw window messages to ImGui).
extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND, UINT, WPARAM, LPARAM);

namespace
{
    // Window state the WndProc mutates.
    struct WinState { int W = 1280, H = 800; bool Resized = false; bool Quit = false; bool Offscreen = false; };

    LRESULT CALLBACK WndProc(HWND Hwnd, UINT Msg, WPARAM W, LPARAM L)
    {
        if (ImGui_ImplWin32_WndProcHandler(Hwnd, Msg, W, L)) return true;
        WinState* S = reinterpret_cast<WinState*>(GetWindowLongPtr(Hwnd, GWLP_USERDATA));
        switch (Msg)
        {
        case WM_SIZE:
            if (S && W != SIZE_MINIMIZED) { S->W = (int)LOWORD(L); S->H = (int)HIWORD(L); S->Resized = true; }
            return 0;
        case WM_GETMINMAXINFO:
        {
            if (S && S->Offscreen) return 0;   // ui-grid-shot: the size asked, whatever the screen
            // Clamp the maximized window to the monitor WORK area so the bottom status bar is never
            // hidden behind the Windows taskbar.
            MONITORINFO Mi{ sizeof(Mi) };
            if (GetMonitorInfo(MonitorFromWindow(Hwnd, MONITOR_DEFAULTTONEAREST), &Mi))
            {
                MINMAXINFO* Mm = reinterpret_cast<MINMAXINFO*>(L);
                Mm->ptMaxPosition.x = Mi.rcWork.left - Mi.rcMonitor.left;
                Mm->ptMaxPosition.y = Mi.rcWork.top  - Mi.rcMonitor.top;
                Mm->ptMaxSize.x = Mm->ptMaxTrackSize.x = Mi.rcWork.right  - Mi.rcWork.left;
                Mm->ptMaxSize.y = Mm->ptMaxTrackSize.y = Mi.rcWork.bottom - Mi.rcWork.top;
            }
            return 0;
        }
        case WM_CLOSE:   if (S) S->Quit = true; DestroyWindow(Hwnd); return 0;
        case WM_DESTROY: PostQuitMessage(0); return 0;
        }
        return DefWindowProc(Hwnd, Msg, W, L);
    }

    // The fonts at the window's DPI and the shell's theme.
    void SetUpFontsAndStyle(HWND Hwnd)
    {
        ImGuiIO& io = ImGui::GetIO();
        float Dpi = 1.0f;
        if (HMODULE U = GetModuleHandleW(L"user32.dll"))
        {
            using GetDpiFn = UINT(WINAPI*)(HWND);
            if (auto Fn = reinterpret_cast<GetDpiFn>(GetProcAddress(U, "GetDpiForWindow")))
            {
                const UINT D = Fn(Hwnd);
                if (D > 0) Dpi = (float)D / 96.0f;
            }
        }

        // Segoe UI at physical pixel size; falls back to the default if unavailable.
        // The atlas only rasterizes glyphs in the ranges given (default: up to U+00FF), so the symbols this
        // shell uses beyond that (U+25B6/U+23F8/U+25A0, U+2212, U+2013, U+2026, U+2248) are added explicitly.
        // The games' level names (DiskGame::LevelNames) add Latin Extended (Polish, Czech...), Greek and
        // Cyrillic, from Segoe UI too, and CODOL's Chinese, merged from Microsoft YaHei (its common
        // simplified set, without oversampling: the atlas stays small) when the system has it.
        static ImVector<ImWchar> ExtraGlyphRanges;
        {
            static const ImWchar LatinExtended[] = { 0x0100, 0x024F, 0x0370, 0x03FF, 0x2000, 0x206F, 0 };
            ImFontGlyphRangesBuilder Builder;
            Builder.AddRanges(io.Fonts->GetGlyphRangesDefault());
            Builder.AddRanges(LatinExtended);
            Builder.AddRanges(io.Fonts->GetGlyphRangesCyrillic());
            Builder.AddText("\xE2\x96\xB6\xE2\x8F\xB8\xE2\x96\xA0\xE2\x88\x92\xE2\x80\x93\xE2\x80\xA6\xE2\x89\x88"); // ▶⏸■−–…≈
            Builder.BuildRanges(&ExtraGlyphRanges);
        }
        ImFontConfig Fc; Fc.OversampleH = 2; Fc.OversampleV = 2;
        if (!io.Fonts->AddFontFromFileTTF("C:\\Windows\\Fonts\\segoeui.ttf", 17.0f * Dpi, &Fc, ExtraGlyphRanges.Data))
            io.Fonts->AddFontDefault();
        else if (GetFileAttributesW(L"C:\\Windows\\Fonts\\msyh.ttc") != INVALID_FILE_ATTRIBUTES)
        {
            ImFontConfig Cjk; Cjk.MergeMode = true; Cjk.OversampleH = 1; Cjk.OversampleV = 1;
            io.Fonts->AddFontFromFileTTF("C:\\Windows\\Fonts\\msyh.ttc", 17.0f * Dpi, &Cjk, io.Fonts->GetGlyphRangesChineseSimplifiedCommon());
        }
        io.FontGlobalScale = 1.0f;

        // Softer, more finished dark theme (rounded frames, roomier padding, a blue accent).
        ImGui::StyleColorsDark();
        ImGuiStyle& St = ImGui::GetStyle();
        St.WindowRounding = 6.0f; St.FrameRounding = 5.0f; St.GrabRounding = 4.0f; St.TabRounding = 5.0f; St.ScrollbarRounding = 6.0f;
        St.WindowPadding = ImVec2(10, 10); St.FramePadding = ImVec2(9, 5); St.ItemSpacing = ImVec2(8, 7); St.CellPadding = ImVec2(7, 4);
        St.Colors[ImGuiCol_WindowBg]        = ImVec4(0.11f, 0.115f, 0.13f, 1.0f);
        St.Colors[ImGuiCol_FrameBg]         = ImVec4(0.17f, 0.18f, 0.20f, 1.0f);
        St.Colors[ImGuiCol_Header]          = ImVec4(0.20f, 0.33f, 0.48f, 1.0f);
        St.Colors[ImGuiCol_HeaderHovered]   = ImVec4(0.26f, 0.42f, 0.60f, 1.0f);
        St.Colors[ImGuiCol_HeaderActive]    = ImVec4(0.30f, 0.48f, 0.68f, 1.0f);
        St.Colors[ImGuiCol_TitleBgActive]   = ImVec4(0.15f, 0.22f, 0.30f, 1.0f);
        St.Colors[ImGuiCol_TableHeaderBg]   = ImVec4(0.17f, 0.19f, 0.23f, 1.0f);
        // Blue-accent buttons + zebra rows + subtle borders for a more finished feel.
        St.Colors[ImGuiCol_Button]          = ImVec4(0.20f, 0.30f, 0.44f, 1.0f);
        St.Colors[ImGuiCol_ButtonHovered]   = ImVec4(0.26f, 0.40f, 0.58f, 1.0f);
        St.Colors[ImGuiCol_ButtonActive]    = ImVec4(0.30f, 0.48f, 0.68f, 1.0f);
        St.Colors[ImGuiCol_CheckMark]       = ImVec4(0.40f, 0.62f, 0.92f, 1.0f);
        St.Colors[ImGuiCol_SliderGrab]      = ImVec4(0.36f, 0.56f, 0.82f, 1.0f);
        St.Colors[ImGuiCol_SliderGrabActive]= ImVec4(0.44f, 0.66f, 0.95f, 1.0f);
        St.Colors[ImGuiCol_Tab]             = ImVec4(0.16f, 0.20f, 0.27f, 1.0f);
        St.Colors[ImGuiCol_TabHovered]      = ImVec4(0.28f, 0.44f, 0.62f, 1.0f);
        St.Colors[ImGuiCol_Border]          = ImVec4(1.0f, 1.0f, 1.0f, 0.07f);
        St.Colors[ImGuiCol_TableBorderLight]= ImVec4(1.0f, 1.0f, 1.0f, 0.06f);
        St.Colors[ImGuiCol_TableRowBg]      = ImVec4(1.0f, 1.0f, 1.0f, 0.0f);
        St.Colors[ImGuiCol_TableRowBgAlt]   = ImVec4(1.0f, 1.0f, 1.0f, 0.025f);
        St.ScaleAllSizes(Dpi);
    }
}

ShellUi::ShellState::ShellState(ID3D11Device* Device, ID3D11DeviceContext* Context, const std::string& Dir, const GridShotOptions* Mode)
    : Dev(Device), Ctx(Context), ModelsDir(Dir), ShotMode(Mode), Viewport(Device, Context)
{
    if (!ModelsDir.empty()) Catalog.ListFolder(ModelsDir);
    Assets.GridView = SettingsManager::GetSetting("assets_view", "grid") == "grid";
    if (ShotMode)
    {
        Assets.GridView = !ShotMode->List;
        Assets.TypeFilter = ShotMode->Type;
        if (!ShotMode->AnimRig.empty()) Assets.GridRig = ShotMode->AnimRig;
        Shot = std::make_unique<GridShot>(*ShotMode, Device, Context);
    }
    if (!ModelsDir.empty()) Catalog.ShownFolders.push_back(ModelsDir);
    RefreshPresets(Assets);
}

bool RunImGuiShell(const std::string& ModelsDir, const GridShotOptions* ShotMode)
{
    // Crisp under display scaling.
    if (HMODULE User32 = GetModuleHandleW(L"user32.dll"))
    {
        using SetCtxFn = BOOL(WINAPI*)(HANDLE);
        if (auto Fn = reinterpret_cast<SetCtxFn>(GetProcAddress(User32, "SetProcessDpiAwarenessContext")))
            Fn(reinterpret_cast<HANDLE>(-4));
        else SetProcessDPIAware();
    }

    WinState State;

    HINSTANCE Inst = GetModuleHandle(nullptr);
    WNDCLASSEX Wc{}; Wc.cbSize=sizeof(Wc); Wc.style=CS_HREDRAW|CS_VREDRAW; Wc.lpfnWndProc=WndProc;
    Wc.hInstance=Inst; Wc.hCursor=LoadCursor(nullptr,IDC_ARROW); Wc.lpszClassName=L"GrayWolfImGuiShell";
    RegisterClassEx(&Wc);
    if (ShotMode) { State.W = ShotMode->Width; State.H = ShotMode->Height; }
    HWND Hwnd = CreateWindowEx(0, Wc.lpszClassName, L"GrayWolf - Asset Browser (ImGui + D3D11)",
                               WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT, State.W, State.H,
                               nullptr, nullptr, Inst, nullptr);
    if (!Hwnd) return false;
    SetWindowLongPtr(Hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(&State));
    UiAsync::SetOwner(Hwnd);   // the file dialogs' owner: on top of it, and it disabled while they are up

    DXGI_SWAP_CHAIN_DESC Scd{};
    Scd.BufferCount=2; Scd.BufferDesc.Format=DXGI_FORMAT_R8G8B8A8_UNORM; Scd.BufferDesc.Width=State.W; Scd.BufferDesc.Height=State.H;
    Scd.BufferUsage=DXGI_USAGE_RENDER_TARGET_OUTPUT; Scd.OutputWindow=Hwnd; Scd.SampleDesc.Count=1; Scd.Windowed=TRUE; Scd.SwapEffect=DXGI_SWAP_EFFECT_DISCARD;
    ComPtr<ID3D11Device> Dev; ComPtr<ID3D11DeviceContext> Ctx; ComPtr<IDXGISwapChain> Swap;
    const D3D_FEATURE_LEVEL Want[]={D3D_FEATURE_LEVEL_11_0}; D3D_FEATURE_LEVEL Got{};
    HRESULT Hr = D3D11CreateDeviceAndSwapChain(nullptr,D3D_DRIVER_TYPE_HARDWARE,nullptr,0,Want,1,D3D11_SDK_VERSION,&Scd,&Swap,&Dev,&Got,&Ctx);
    if (FAILED(Hr)) Hr = D3D11CreateDeviceAndSwapChain(nullptr,D3D_DRIVER_TYPE_WARP,nullptr,0,Want,1,D3D11_SDK_VERSION,&Scd,&Swap,&Dev,&Got,&Ctx);
    if (FAILED(Hr)) { DestroyWindow(Hwnd); return false; }

    ComPtr<ID3D11RenderTargetView> BackRTV;
    auto CreateBackRTV = [&]{ BackRTV.Reset(); ComPtr<ID3D11Texture2D> B; if (SUCCEEDED(Swap->GetBuffer(0,__uuidof(ID3D11Texture2D),&B))) Dev->CreateRenderTargetView(B.Get(),nullptr,&BackRTV); };
    CreateBackRTV();

    if (!ShotMode) ShowWindow(Hwnd, SW_SHOWMAXIMIZED);   // ui-grid-shot stays hidden
    else
    {
        // the client area the size asked (the screen's work area does not cap a hidden window)
        State.Offscreen = true;
        RECT Want{ 0, 0, ShotMode->Width, ShotMode->Height };
        AdjustWindowRect(&Want, WS_OVERLAPPEDWINDOW, FALSE);
        SetWindowPos(Hwnd, nullptr, 0, 0, Want.right - Want.left, Want.bottom - Want.top, SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
        RECT Rc{}; GetClientRect(Hwnd, &Rc); State.W = Rc.right; State.H = Rc.bottom; State.Resized = true;
    }

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_DockingEnable;
    io.IniFilename = nullptr;
    SetUpFontsAndStyle(Hwnd);

    ImGui_ImplWin32_Init(Hwnd);
    ImGui_ImplDX11_Init(Dev.Get(), Ctx.Get());

    LoadGamePanel LoadGame;
    ShellState S(Dev.Get(), Ctx.Get(), ModelsDir, ShotMode);

    while (!State.Quit)
    {
        MSG Msg;
        while (PeekMessage(&Msg, nullptr, 0, 0, PM_REMOVE))
        {
            TranslateMessage(&Msg); DispatchMessage(&Msg);
            if (Msg.message == WM_QUIT) State.Quit = true;
        }
        if (State.Quit) break;
        AppLog::FrameBegin();
        if (S.Shot) S.Shot->FrameBegin();
        S.Catalog.DrainScanResults();
        PollListing(S);
        { GpuMesh Ready; double LoadMs = 0; if (S.Viewport.Loader.Poll(Ready, LoadMs)) { S.Viewport.Mesh = std::move(Ready); ResetCamera(S.Viewport); } }

        if (State.Resized) { BackRTV.Reset(); Swap->ResizeBuffers(0, State.W, State.H, DXGI_FORMAT_UNKNOWN, 0); CreateBackRTV(); State.Resized = false; }

        ImGui_ImplDX11_NewFrame();
        ImGui_ImplWin32_NewFrame();
        ImGui::NewFrame();

        bool OpenHelp = false;
        if (DrawMainMenuBar(OpenHelp)) State.Quit = true;
        DrawStatusBar(S);
        BuildDockLayout(ImGui::DockSpaceOverViewport());
        DrawAssetsPanel(S, OpenHelp);
        DrawViewportPanel(S);
        DrawFilterHelp(OpenHelp);

        LoadGame.Draw(Dev.Get());

        if (LoadGame.ConsumeExportJustFinished())
        {
            // Merge the freshly-exported game into the Assets tab's list; MergeListed dedups by path, so a
            // re-run is safe.
            OpenFolder(S, LoadGame.ExportOutputDir(), false);
        }

        UiAsync::DrawOverlay();   // a dialog's dimmed layer, the notices
        ImGui::Render();
        const float Clear[4] = { 0.10f, 0.10f, 0.11f, 1.0f };
        ID3D11RenderTargetView* Bb[] = { BackRTV.Get() };
        Ctx->OMSetRenderTargets(1, Bb, nullptr);
        Ctx->ClearRenderTargetView(BackRTV.Get(), Clear);
        ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());

        PollUiShotTriggers(S, Swap.Get());

        if (S.Shot && S.Shot->FrameEnd(Swap.Get(), !S.Catalog.Walking() && !S.SelectFirstModel && !S.Viewport.Loader.Busy(),
                                       !S.Assets.GridView || (S.Assets.Grid && S.Assets.Grid->Settled()), S.Assets.Grid ? S.Assets.Grid->Stats() + "\n" + S.Rigs->Stats() : std::string()))
            State.Quit = true;
        Swap->Present(S.Shot ? 0 : 1, 0);
        AppLog::FrameEnd();
    }

    S.Catalog.Stop();
    UiAsync::Shutdown();

    S.Assets.Grid.reset();   // its workers stop before the device goes
    const bool ShotSaved = !S.Shot || S.Shot->Ok();
    if (S.Shot) spdlog::info("ui-grid-shot\n{}", S.Shot->Summary());
    ImGui_ImplDX11_Shutdown();
    ImGui_ImplWin32_Shutdown();
    ImGui::DestroyContext();
    DestroyWindow(Hwnd);
    return ShotSaved;
}
