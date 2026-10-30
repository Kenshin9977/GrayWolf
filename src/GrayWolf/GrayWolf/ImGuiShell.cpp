#include "stdafx.h"

#include "ImGuiShell.h"
#include "ImGuiShell.Internal.h" // the shell's pieces in their own files
#include "AppLog.h"           // session log, timed steps, UI stalls
#include "ImGuiLoadGamePanel.h" // the Extract tab: a game exported from its files
#include "UiAsync.h"           // dialogs, shell opens and disk lookups off the frame
#include "SettingsManager.h"   // the list or the grid, the anims' rig
#include "ImGuiTheme.h"        // the fonts and colours

#include "imgui.h"
#include "imgui_internal.h"   // FindWindowByName
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
            if (S && S->Offscreen)   // ui-grid-shot, ui-record: the size asked, whatever the screen
            {
                MINMAXINFO* Mm = reinterpret_cast<MINMAXINFO*>(L);
                Mm->ptMaxTrackSize.x = Mm->ptMaxSize.x = 16384;
                Mm->ptMaxTrackSize.y = Mm->ptMaxSize.y = 16384;
                return 0;
            }
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

        // the fonts (Segoe UI, its semibold, the icons merged) and the colours: ImGuiTheme.h
        Theme::Load(Dpi);
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
        std::snprintf(Assets.Filter, sizeof(Assets.Filter), "%s", ShotMode->Search.c_str());
        if (!ShotMode->AnimRig.empty()) Assets.GridRig = ShotMode->AnimRig;
        Shot = std::make_unique<GridShot>(*ShotMode, Device, Context);
    }
    if (!ModelsDir.empty()) Catalog.ShownFolders.push_back(ModelsDir);
    RefreshPresets(Assets);
}

bool RunImGuiShell(const std::string& ModelsDir, const GridShotOptions* ShotMode, const UiRecordOptions* Record)
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
    const bool Hidden = ShotMode || Record;   // ui-grid-shot, ui-record: offscreen at the size asked
    if (ShotMode) { State.W = ShotMode->Width; State.H = ShotMode->Height; }
    if (Record) { State.W = Record->Width; State.H = Record->Height; }
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

    if (!Hidden) ShowWindow(Hwnd, SW_SHOWMAXIMIZED);   // ui-grid-shot and ui-record stay hidden
    else
    {
        // the client area the size asked (the screen's work area does not cap a hidden window)
        State.Offscreen = true;
        RECT Want{ 0, 0, State.W, State.H };
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
    if (Record) S.Recorder = std::make_unique<UiRecorder>(*Record, Dev.Get(), Ctx.Get());

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
        if (S.Recorder) S.Recorder->FrameBegin(S);
        ImGui::NewFrame();

        bool OpenHelp = false;
        if (DrawMainMenuBar(OpenHelp)) State.Quit = true;
        DrawStatusBar(S);
        BuildDockLayout(ImGui::DockSpaceOverViewport());
        DrawAssetsPanel(S, OpenHelp);
        DrawViewportPanel(S);
        DrawFilterHelp(OpenHelp);

        // (kept until the window exists: the first frame has drawn none yet)
        if (const std::string Id = "###" + S.FocusWindow; !S.FocusWindow.empty() && ImGui::FindWindowByName(Id.c_str()) != nullptr)
        {
            ImGui::SetWindowFocus(Id.c_str());
            S.FocusWindow.clear();
        }
        if (!S.LoadGameScript.empty()) { LoadGame.Script(S.LoadGameScript); S.LoadGameScript.clear(); }
        LoadGame.Draw(Dev.Get());
        S.LoadGameWorking = S.Recorder && LoadGame.Working();
        S.LoadGameIndicator = LoadGame.ShowedProgress();

        if (LoadGame.ConsumeExportJustFinished())
        {
            // Merge the freshly-exported game into the Assets tab's list; MergeListed dedups by path, so a
            // re-run is safe.
            OpenFolder(S, LoadGame.ExportOutputDir(), false);
        }
        // Load Game's preview: its folder listed, the asset shown once it is (PollListing)
        if (std::string Dir, Name; LoadGame.ConsumePreview(Dir, Name))
        {
            OpenFolder(S, Dir, false);
            S.SelectWhenListed = Name;
            S.SelectWhenListedDir = Dir;
            S.SelectWhenListedFrames = 600;
        }
        if (LoadGame.ConsumeShowAssets()) ImGui::SetWindowFocus(Theme::LibraryWindow);

        UiAsync::DrawOverlay();   // a dialog's dimmed layer, the notices
        ImGui::Render();
        const float Clear[4] = { 0.10f, 0.10f, 0.11f, 1.0f };
        ID3D11RenderTargetView* Bb[] = { BackRTV.Get() };
        Ctx->OMSetRenderTargets(1, Bb, nullptr);
        Ctx->ClearRenderTargetView(BackRTV.Get(), Clear);
        ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());

        PollUiShotTriggers(S, Swap.Get());

        const bool ShotSelectDone = !S.ShotMode || (S.ShotSelected >= S.ShotMode->Select.size() && S.ShotSelectWait <= 0);
        if (S.Shot && S.Shot->FrameEnd(Swap.Get(), !S.Catalog.Walking() && !S.SelectFirstModel && !S.Viewport.Loader.Busy() &&
                                       !S.Viewport.AnimRigLoading && ShotSelectDone,
                                       !S.Assets.GridView || (S.Assets.Grid && S.Assets.Grid->Settled()), S.Assets.Grid ? S.Assets.Grid->Stats() + "\n" + S.Rigs->Stats() : std::string()))
            State.Quit = true;
        if (S.Recorder && S.Recorder->FrameEnd(S, Swap.Get())) State.Quit = true;
        Swap->Present(Hidden ? 0 : 1, 0);
        AppLog::FrameEnd();
    }

    S.Catalog.Stop();
    UiAsync::Shutdown();

    S.Assets.Grid.reset();   // its workers stop before the device goes
    if (S.Recorder) S.Recorder->Finish();   // its frames on disk, audio.wav written
    const bool ShotSaved = (!S.Shot || S.Shot->Ok()) && (!S.Recorder || S.Recorder->Ok());
    if (S.Shot) spdlog::info("ui-grid-shot\n{}", S.Shot->Summary());
    if (S.Recorder) spdlog::info("ui-record\n{}", S.Recorder->Summary());
    ImGui_ImplDX11_Shutdown();
    ImGui_ImplWin32_Shutdown();
    ImGui::DestroyContext();
    DestroyWindow(Hwnd);
    return ShotSaved;
}
