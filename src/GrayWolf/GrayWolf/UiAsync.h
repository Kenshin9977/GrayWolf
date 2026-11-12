#pragma once

// UiAsync: what the GUI hands to Windows or to the disk without stopping its frame. File dialogs
// run on a thread of their own (owned by the main window), shell opens and one-off steps on
// workers, and a value read from the disk is a Cached<T> the frame reads and never waits on.

#include <atomic>
#include <chrono>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include <windows.h>

namespace UiAsync
{
    // The window the dialogs belong to (the shell's main window), set once it exists.
    void SetOwner(HWND Owner);

    // One filter of a file dialog: L"Blender", L"blender.exe;blender-launcher.exe".
    struct FileType
    {
        std::wstring Name;
        std::wstring Pattern;
    };

    // Opens a dialog unless one is already up (false then). Start: the folder it opens on (when it
    // exists). Id names the caller: Picked(Id, ...) gets the answer.
    bool PickFolder(const std::string& Id, const std::string& Title, const std::string& Start = {});
    bool PickOpenFile(const std::string& Id, const std::string& Title, const std::vector<FileType>& Types, const std::string& Start = {});
    bool PickSaveFile(const std::string& Id, const std::string& Title, const std::vector<FileType>& Types, const std::wstring& DefaultExt,
                      const std::string& DefaultName, const std::string& Start = {});

    // True once, on the frame the dialog asked under Id was closed with a path picked (Path set);
    // a cancelled one is dropped without a word. Poll it every frame, outside any popup or menu
    // the dialog may close.
    bool Picked(const std::string& Id, std::string& Path);

    // A dialog is up: its title ("" when none). The shell dims itself meanwhile.
    std::string DialogTitle();

    // Opens Path with its program (a folder in Explorer), or, Reveal, shows it selected in its
    // folder. Returns at once.
    void OpenInShell(const std::string& Path, bool Reveal = false);
    // Opens an https:// link in the default browser (anything else is refused). Returns at once.
    void OpenUrl(const std::string& Url);

    // Runs Work on a worker thread, timed in the session log under What.
    void Run(const std::string& What, std::function<void()> Work);

    // A line for the user (a failed open, a saved file), shown a few seconds at the bottom right.
    void Notify(const std::string& Text, bool Error = false);

    // Once per frame, after every window: the dialog's dimmed layer and the notices.
    void DrawOverlay();

    // At exit: waits (at most a moment) for the workers still running.
    void Shutdown();

    namespace Detail
    {
        void Started();
        void Finished();
        double Now();   // seconds, steady clock
    }

    // A value the frame shows but must not compute itself (a directory listing, a file's
    // existence, a program found on the disk).
    template <class T>
    class Cached
    {
    public:
        // Key's value as last computed (T{} until the first one is in: Pending then), computed
        // again on a worker when Key changes or every Every seconds. A new key starts at once;
        // a result for an older key is dropped.
        T Get(const std::string& Key, double Every, std::function<T()> Compute, bool* Pending = nullptr)
        {
            std::shared_ptr<State> S = Shared;
            std::lock_guard<std::mutex> Lock(S->M);
            const double Now = Detail::Now();
            if (Key != S->Key)
            {
                S->Key = Key;
                S->Has = false;
                S->Value = T{};
                S->Running = false;   // the older key's run, if any, is ignored when it ends
                S->Generation++;
            }
            if (!S->Running && (!S->Has || S->Stale || Now - S->At >= Every))
            {
                S->Running = true;
                S->Stale = false;
                const uint64_t Gen = S->Generation;
                Detail::Started();
                std::thread([S, Gen, Compute = std::move(Compute)] {
                    T Value{};
                    try { Value = Compute(); } catch (...) {}
                    {
                        std::lock_guard<std::mutex> L(S->M);
                        if (S->Generation == Gen)
                        {
                            S->Value = std::move(Value);
                            S->Has = true;
                            S->Running = false;
                            S->At = Detail::Now();
                        }
                    }
                    Detail::Finished();
                }).detach();
            }
            if (Pending != nullptr) *Pending = !S->Has;
            return S->Value;
        }

        // The next Get computes again; Forget also drops the value until then (a trigger consumed).
        void Refresh() { std::lock_guard<std::mutex> L(Shared->M); Shared->Stale = true; }
        void Forget()
        {
            std::lock_guard<std::mutex> L(Shared->M);
            Shared->Has = false;
            Shared->Value = T{};
            Shared->Running = false;
            Shared->Generation++;
        }

    private:
        struct State
        {
            std::mutex M;
            std::string Key = "\x01";   // no key yet: the first Get is a change
            T Value{};
            bool Has = false, Running = false, Stale = false;
            uint64_t Generation = 0;
            double At = 0;
        };
        std::shared_ptr<State> Shared = std::make_shared<State>();
    };
}
