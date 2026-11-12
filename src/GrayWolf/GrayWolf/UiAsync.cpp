#include "stdafx.h"

#include "UiAsync.h"

#include "AppLog.h"
#include "Strings.h"

#include "imgui.h"

#include <deque>
#include <filesystem>
#include <stdexcept>

#include <objbase.h>
#include <shellapi.h>
#include <shlobj.h>
#include <shobjidl.h>

namespace fs = std::filesystem;

namespace UiAsync
{
namespace
{
    std::atomic<HWND> OwnerWindow{ nullptr };
    std::atomic<int> Running{ 0 };

    // The dialog up, or closed and not yet collected by its caller (the UI thread's alone).
    struct DialogJob
    {
        std::string Id, Title;
        std::atomic<bool> Done{ false };
        std::string Result;
    };
    std::shared_ptr<DialogJob> Dialog;

    struct Notice
    {
        std::string Text;
        bool Error = false;
        double Until = 0;
    };
    std::mutex NoticeMutex;
    std::deque<Notice> Notices;

    // UTF-8, or the ANSI code page a path::string() gives (some of the app's paths come so)
    std::wstring Wide(const std::string& S)
    {
        try { return Strings::ToUnicodeString(S); }
        catch (const std::range_error&) { return fs::path(S).wstring(); }
    }

    // A thread of its own for COM's shell objects: single-threaded apartment, as the shell wants.
    struct ShellApartment
    {
        HRESULT Hr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
        ~ShellApartment() { if (SUCCEEDED(Hr)) CoUninitialize(); }
    };

    struct DialogSpec
    {
        bool Save = false;
        DWORD Options = 0;
        std::string Title, DefaultName, Start;
        std::vector<FileType> Types;
        std::wstring DefaultExt;
    };

    // The dialog shown (modal to Owner) on the calling thread; "" when cancelled or failed.
    std::string ShowDialog(HWND Owner, const DialogSpec& Spec)
    {
        IFileDialog* D = nullptr;
        if (FAILED(CoCreateInstance(Spec.Save ? CLSID_FileSaveDialog : CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER,
                                    IID_PPV_ARGS(&D))))
            return {};
        std::vector<COMDLG_FILTERSPEC> Filters;
        for (const FileType& T : Spec.Types) Filters.push_back({ T.Name.c_str(), T.Pattern.c_str() });
        if (!Filters.empty()) D->SetFileTypes((UINT)Filters.size(), Filters.data());
        if (!Spec.DefaultExt.empty()) D->SetDefaultExtension(Spec.DefaultExt.c_str());
        D->SetTitle(Wide(Spec.Title).c_str());
        if (!Spec.DefaultName.empty()) D->SetFileName(Wide(Spec.DefaultName).c_str());
        DWORD Flags = 0;
        D->GetOptions(&Flags);
        D->SetOptions(Flags | FOS_FORCEFILESYSTEM | Spec.Options);
        std::error_code Ec;
        IShellItem* StartItem = nullptr;
        if (!Spec.Start.empty() && fs::is_directory(Wide(Spec.Start), Ec) &&
            SUCCEEDED(SHCreateItemFromParsingName(Wide(Spec.Start).c_str(), nullptr, IID_PPV_ARGS(&StartItem))))
        {
            D->SetFolder(StartItem);
            StartItem->Release();
        }
        std::string Result;
        if (SUCCEEDED(D->Show(Owner)))
        {
            IShellItem* Item = nullptr;
            if (SUCCEEDED(D->GetResult(&Item)))
            {
                PWSTR Path = nullptr;
                if (SUCCEEDED(Item->GetDisplayName(SIGDN_FILESYSPATH, &Path)))
                {
                    Result = Strings::ToNormalString(Path);
                    CoTaskMemFree(Path);
                }
                Item->Release();
            }
        }
        D->Release();
        return Result;
    }

    bool OpenDialog(const std::string& Id, DialogSpec Spec)
    {
        if (Dialog && !Dialog->Done) return false;
        auto Job = std::make_shared<DialogJob>();
        Job->Id = Id;
        Job->Title = Spec.Title;
        Dialog = Job;
        const HWND Owner = OwnerWindow.load();
        std::thread([Job, Owner, Spec = std::move(Spec)] {
            {
                ShellApartment Com;
                AppLog::Timer Shown("dialog \"" + Spec.Title + "\"");
                Shown.UserWait();
                Job->Result = ShowDialog(Owner, Spec);
            }
            Job->Done = true;
        }).detach();
        return true;
    }
}

namespace Detail
{
    void Started() { Running++; }
    void Finished() { Running--; }
    double Now() { return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count(); }
}

void SetOwner(HWND Owner) { OwnerWindow = Owner; }

bool PickFolder(const std::string& Id, const std::string& Title, const std::string& Start)
{
    DialogSpec S;
    S.Options = FOS_PICKFOLDERS;
    S.Title = Title;
    S.Start = Start;
    return OpenDialog(Id, std::move(S));
}

bool PickOpenFile(const std::string& Id, const std::string& Title, const std::vector<FileType>& Types, const std::string& Start)
{
    DialogSpec S;
    S.Options = FOS_FILEMUSTEXIST;
    S.Title = Title;
    S.Types = Types;
    S.Start = Start;
    return OpenDialog(Id, std::move(S));
}

bool PickSaveFile(const std::string& Id, const std::string& Title, const std::vector<FileType>& Types, const std::wstring& DefaultExt,
                  const std::string& DefaultName, const std::string& Start)
{
    DialogSpec S;
    S.Save = true;
    S.Options = FOS_OVERWRITEPROMPT;
    S.Title = Title;
    S.Types = Types;
    S.DefaultExt = DefaultExt;
    S.DefaultName = DefaultName;
    S.Start = Start;
    return OpenDialog(Id, std::move(S));
}

bool Picked(const std::string& Id, std::string& Path)
{
    if (!Dialog || Dialog->Id != Id || !Dialog->Done) return false;
    Path = Dialog->Result;
    Dialog.reset();
    return !Path.empty();
}

std::string DialogTitle()
{
    return Dialog && !Dialog->Done ? Dialog->Title : std::string();
}

// Target (a path or a link) with its program, on a worker thread with COM up
static void Launch(HWND Owner, const std::string& Target)
{
    const std::wstring W = Wide(Target);
    SHELLEXECUTEINFOW Sei{};
    Sei.cbSize = sizeof(Sei);
    // this thread ends with the call: the launch completes before it returns
    Sei.fMask = SEE_MASK_NOASYNC | SEE_MASK_FLAG_LOG_USAGE;
    Sei.hwnd = Owner;
    Sei.lpFile = W.c_str();
    Sei.nShow = SW_SHOWNORMAL;
    if (!ShellExecuteExW(&Sei))
    {
        const DWORD Error = GetLastError();
        if (Error != ERROR_CANCELLED) Notify("could not open " + Target + " (error " + std::to_string(Error) + ")", true);
    }
}

void OpenInShell(const std::string& Path, bool Reveal)
{
    const HWND Owner = OwnerWindow.load();
    Run(std::string(Reveal ? "show in folder " : "open ") + Path, [Path, Reveal, Owner] {
        ShellApartment Com;
        const std::wstring W = Wide(Path);
        std::error_code Ec;
        if (!fs::exists(W, Ec))
        {
            Notify("not found: " + Path, true);
            return;
        }
        if (Reveal)
        {
            PIDLIST_ABSOLUTE Item = nullptr;
            HRESULT Hr = SHParseDisplayName(W.c_str(), nullptr, &Item, 0, nullptr);
            if (SUCCEEDED(Hr)) Hr = SHOpenFolderAndSelectItems(Item, 0, nullptr, 0);
            if (Item != nullptr) CoTaskMemFree(Item);
            if (FAILED(Hr)) Notify("could not show " + Path + " in its folder (hr " + std::to_string((long)Hr) + ")", true);
            return;
        }
        Launch(Owner, Path);
    });
}

void OpenUrl(const std::string& Url)
{
    if (Url.rfind("https://", 0) != 0)
    {
        Notify("not a web link: " + Url, true);
        return;
    }
    const HWND Owner = OwnerWindow.load();
    Run("open " + Url, [Url, Owner] {
        ShellApartment Com;
        Launch(Owner, Url);
    });
}

void Run(const std::string& What, std::function<void()> Work)
{
    Detail::Started();
    std::thread([What, Work = std::move(Work)] {
        {
            AppLog::Timer Step(What, spdlog::level::debug, 5000);
            try { Work(); }
            catch (const std::exception& E) { AppLog::Log().warn("{} failed: {}", What, E.what()); }
            catch (...) { AppLog::Log().warn("{} failed", What); }
        }
        Detail::Finished();
    }).detach();
}

void Notify(const std::string& Text, bool Error)
{
    if (Error) AppLog::Log().warn("{}", Text);
    std::lock_guard<std::mutex> Lock(NoticeMutex);
    Notices.push_back({ Text, Error, Detail::Now() + (Error ? 10.0 : 6.0) });
    while (Notices.size() > 4) Notices.pop_front();
}

void DrawOverlay()
{
    const ImGuiViewport* Vp = ImGui::GetMainViewport();
    if (const std::string Title = DialogTitle(); !Title.empty())
    {
        // the main window is disabled while its dialog is up: shown so, and still drawing
        ImDrawList* Dl = ImGui::GetForegroundDrawList();
        const ImVec2 Min = Vp->Pos, Max(Vp->Pos.x + Vp->Size.x, Vp->Pos.y + Vp->Size.y);
        Dl->AddRectFilled(Min, Max, IM_COL32(8, 8, 10, 140));
        const std::string Text = "Waiting for the dialog: " + Title;
        const ImVec2 C((Min.x + Max.x) * 0.5f, (Min.y + Max.y) * 0.5f);
        const float R = ImGui::GetFontSize() * 0.9f, T = (float)ImGui::GetTime() * 6.0f;
        Dl->PathArcTo(ImVec2(C.x, C.y - R * 1.5f), R, T, T + 4.2f, 32);
        Dl->PathStroke(IM_COL32(120, 170, 230, 230), 0, 3.0f);
        const ImVec2 Ts = ImGui::CalcTextSize(Text.c_str());
        Dl->AddText(ImVec2(C.x - Ts.x * 0.5f, C.y), IM_COL32(225, 225, 230, 255), Text.c_str());
    }

    std::vector<Notice> Shown;
    {
        std::lock_guard<std::mutex> Lock(NoticeMutex);
        const double Now = Detail::Now();
        while (!Notices.empty() && Notices.front().Until < Now) Notices.pop_front();
        Shown.assign(Notices.begin(), Notices.end());
    }
    if (Shown.empty()) return;
    const float Pad = ImGui::GetFontSize();
    ImGui::SetNextWindowPos(ImVec2(Vp->WorkPos.x + Vp->WorkSize.x - Pad, Vp->WorkPos.y + Vp->WorkSize.y - Pad), ImGuiCond_Always,
                            ImVec2(1.0f, 1.0f));
    ImGui::SetNextWindowBgAlpha(0.85f);
    const ImGuiWindowFlags Flags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings |
                                   ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoInputs |
                                   ImGuiWindowFlags_NoDocking;
    if (ImGui::Begin("##uiasync_notices", nullptr, Flags))
        for (const Notice& N : Shown)
        {
            if (N.Error) ImGui::TextColored(ImVec4(1.0f, 0.62f, 0.4f, 1.0f), "%s", N.Text.c_str());
            else ImGui::TextUnformatted(N.Text.c_str());
        }
    ImGui::End();
}

void Shutdown()
{
    for (int Waited = 0; Running.load() > 0 && Waited < 2000; Waited += 20) Sleep(20);
}
}
