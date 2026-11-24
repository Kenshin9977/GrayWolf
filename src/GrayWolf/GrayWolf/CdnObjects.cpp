#include "stdafx.h"

#include "CdnObjects.h"

#include "ExportNaming.h"
#include "FileSystems.h"
#include "MappedFile.h"
#include "SettingsManager.h"
#include "WebClient.h"

#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <filesystem>
#include <format>
#include <fstream>

#include <algorithm>
#include <functional>
#include <iterator>
#include <map>
#include <memory>
#include <mutex>
#include <thread>
#include <unordered_set>

namespace
{
    std::filesystem::path KeptDir(const std::string& Base)
    {
        wchar_t* Local = nullptr;
        size_t Length = 0;
        std::filesystem::path Root;
        if (_wdupenv_s(&Local, &Length, L"LOCALAPPDATA") == 0 && Local != nullptr) Root = Local;
        std::free(Local);
        if (Root.empty()) Root = std::filesystem::temp_directory_path();
        return Root / "GrayWolf" / "cdn" / std::filesystem::path(Base).filename();
    }

    std::string HostOf(const std::string& Url)
    {
        size_t From = Url.find("://");
        From = From == std::string::npos ? 0 : From + 3;
        return Url.substr(From, Url.find_first_of(":/", From) - From);
    }

    // the session's answer and the GUI's pending question
    std::mutex ConsentLock;
    std::condition_variable Answered;
    CdnObjects::Decision Session = CdnObjects::Decision::Undecided;
    bool HasAsker = false, Pending = false;
    std::thread::id GuiThread;
    std::string PendingHost;

    std::mutex SkippedLock;
    std::unordered_set<uint64_t> SkippedKeys;

    // at most two requests per host at once; a host failing three times running is left alone
    // for the session, a URL that failed is not asked again
    constexpr int PerHost = 2, HostGivesUpAfter = 3, Attempts = 4;
    struct HostState
    {
        int Busy = 0, Failures = 0;
        bool GaveUp = false;
        std::condition_variable Free;
    };
    std::mutex HostLock;
    std::map<std::string, std::unique_ptr<HostState>> Hosts;
    std::unordered_set<std::string> FailedUrls;

    // one WinHTTP session for the run, its connections reused
    WebClient& Client()
    {
        static WebClient C;
        return C;
    }
}

CdnObjects::Mode CdnObjects::Setting()
{
    const std::string V = ExportNaming::Lower(SettingsManager::GetSetting("cdn_images", "ask"));
    if (V == "on" || V == "true") return Mode::On;
    if (V == "off" || V == "false") return Mode::Off;
    return Mode::Ask;
}

std::string CdnObjects::BaseInExe(const std::filesystem::path& Exe)
{
    static std::mutex Lock;
    static std::map<std::string, std::string> Found;
    std::lock_guard<std::mutex> Guard(Lock);
    const std::string Key = Exe.string();
    if (const auto It = Found.find(Key); It != Found.end()) return It->second;
    std::string Url;
    MappedFile Map;
    std::error_code Ec;
    if (std::filesystem::is_regular_file(Exe, Ec) && Map.Open(Exe))
    {
        // the shipped exes hold it in clear: "http://cod-assets.cdn.<host>:80/pc/<build>", NUL-ended
        static const std::string Prefix = "http://cod-assets.cdn.";
        const char* Begin = (const char*)Map.data();
        const char* End = Begin + Map.size();
        const char* At = std::search(Begin, End, std::boyer_moore_horspool_searcher(Prefix.begin(), Prefix.end()));
        if (At != End)
        {
            const char* Stop = At;
            while (Stop < End && Stop - At < 128 && *Stop >= 0x21 && *Stop < 0x7f) Stop++;
            Url.assign(At, Stop);
            // (WebClient takes no port in the host: 80 is http's anyway)
            if (const size_t Port = Url.find(":80/"); Port != std::string::npos) Url.erase(Port, 3);
            if (Url.find("/pc/") == std::string::npos) Url.clear();
        }
    }
    Found[Key] = Url;
    return Url;
}

bool CdnObjects::Offline()
{
    char Value[8] = {};
    return GetEnvironmentVariableA("GRAYWOLF_OFFLINE_IMAGES", Value, sizeof(Value)) > 0 && Value[0] == '1';
}

bool CdnObjects::Decide(Mode M, bool Asker, bool OnGuiThread, Decision S, bool& Ask)
{
    (void)OnGuiThread;
    Ask = false;
    if (M == Mode::On) return true;
    if (M == Mode::Off) return false;
    if (S != Decision::Undecided) return S == Decision::Yes;
    // only the GUI asks: a CLI run, MCP or the regress mean off unless --cdn-images
    Ask = Asker;
    return false;
}

bool CdnObjects::Allowed()
{
    if (Offline()) return false;
    const Mode M = Setting();
    std::unique_lock<std::mutex> Lock(ConsentLock);
    bool Ask = false;
    const bool OnGui = HasAsker && std::this_thread::get_id() == GuiThread;
    if (Decide(M, HasAsker, OnGui, Session, Ask)) return true;
    if (!Ask) return false;
    Pending = true;
    // the GUI's own thread cannot wait for its own dialog: the low mips this time
    if (OnGui) return false;
    Answered.wait(Lock, [] { return Session != Decision::Undecided || !Pending; });
    return Session == Decision::Yes;
}

void CdnObjects::NoteSkipped(uint64_t Key)
{
    std::lock_guard<std::mutex> Lock(SkippedLock);
    SkippedKeys.insert(Key);
}

size_t CdnObjects::Skipped()
{
    std::lock_guard<std::mutex> Lock(SkippedLock);
    return SkippedKeys.size();
}

void CdnObjects::AttachAsker()
{
    std::lock_guard<std::mutex> Lock(ConsentLock);
    HasAsker = true;
    GuiThread = std::this_thread::get_id();
}

bool CdnObjects::Question(std::string& Host)
{
    std::lock_guard<std::mutex> Lock(ConsentLock);
    if (!Pending || Session != Decision::Undecided) return false;
    Host = PendingHost.empty() ? "cod-assets.cdn.callofduty.com" : PendingHost;
    return true;
}

namespace
{
    // the answer for the session, and (Save) as the setting: asked once ever
    void Settle(bool Download, bool Save)
    {
        {
            std::lock_guard<std::mutex> Lock(ConsentLock);
            Session = Download ? CdnObjects::Decision::Yes : CdnObjects::Decision::No;
            Pending = false;
        }
        if (Save) SettingsManager::SetSetting("cdn_images", Download ? "on" : "off");
        Answered.notify_all();
    }
}

void CdnObjects::Answer(bool Download)
{
    Settle(Download, true);
}

bool CdnObjects::IsKept(const std::string& Base, uint64_t Key)
{
    std::error_code Ec;
    return std::filesystem::is_regular_file(KeptDir(Base) / (ExportNaming::Hex16(Key) + ".bin"), Ec);
}

std::optional<std::vector<uint8_t>> CdnObjects::Fetch(const std::string& Base, const std::string& Path, uint64_t Key,
                                                      const std::function<bool(const std::vector<uint8_t>&)>& Whole)
{
    if (Offline()) return std::nullopt;
    const std::filesystem::path Kept = KeptDir(Base) / (ExportNaming::Hex16(Key) + ".bin");
    // fetched once: read from the disk, whatever the setting says now
    if (std::ifstream In(Kept, std::ios::binary); In)
    {
        std::vector<uint8_t> Stored((std::istreambuf_iterator<char>(In)), std::istreambuf_iterator<char>());
        if (Whole(Stored)) return Stored;
    }
    const std::string Host = HostOf(Base), Url = Base + Path;
    {
        std::lock_guard<std::mutex> Lock(ConsentLock);
        if (PendingHost.empty()) PendingHost = Host;
    }
    if (!Allowed())
    {
        NoteSkipped(Key);
        return std::nullopt;
    }
    HostState* H = nullptr;
    {
        std::unique_lock<std::mutex> Lock(HostLock);
        if (FailedUrls.count(Url)) return std::nullopt;
        auto& Slot = Hosts[Host];
        if (!Slot) Slot = std::make_unique<HostState>();
        H = Slot.get();
        H->Free.wait(Lock, [H] { return H->Busy < PerHost || H->GaveUp; });
        if (H->GaveUp)
        {
            NoteSkipped(Key);
            return std::nullopt;
        }
        H->Busy++;
    }
    std::optional<std::vector<uint8_t>> Body;
    bool HostTrouble = false;
    for (int Attempt = 0; Attempt < Attempts; Attempt++)
    {
        uint32_t Status = 0;
        try
        {
            Body = Client().DownloadData(Url, &Status);
        }
        catch (...)
        {
            Body.reset();
        }
        if (Status == 200 && Body) break;
        Body.reset();
        // throttled, a server error or no answer: again after 1, 2, 4 s; else (404) the key's answer
        HostTrouble = Status == 0 || Status == 429 || Status >= 500;
        if (!HostTrouble) break;
        if (Attempt + 1 < Attempts) std::this_thread::sleep_for(std::chrono::seconds(1 << Attempt));
    }
    const bool Good = Body && Whole(*Body);
    {
        std::lock_guard<std::mutex> Lock(HostLock);
        H->Busy--;
        if (Body) H->Failures = 0;
        else if (HostTrouble && ++H->Failures >= HostGivesUpAfter) H->GaveUp = true;
        if (!Good) FailedUrls.insert(Url);
    }
    H->Free.notify_all();
    if (!Good) return std::nullopt;
    std::error_code Ec;
    std::filesystem::create_directories(Kept.parent_path(), Ec);
    FileSystems::WriteReplacing(Kept.string(), [&](std::ostream& Out) { Out.write((const char*)Body->data(), (std::streamsize)Body->size()); });
    return Body;
}

std::string CdnObjects::SelfTest()
{
    std::string Failures;
    const auto Expect = [&](const char* Name, Mode M, bool Asker, bool OnGui, Decision S, bool WantAllowed, bool WantAsk) {
        bool Ask = false;
        const bool Got = Decide(M, Asker, OnGui, S, Ask);
        if (Got != WantAllowed || Ask != WantAsk)
            Failures += std::format("FAIL cdn consent {}: allowed {} ask {}\n", Name, Got, Ask);
    };
    Expect("on", Mode::On, false, false, Decision::Undecided, true, false);
    Expect("on, said no", Mode::On, true, false, Decision::No, true, false);
    Expect("off", Mode::Off, true, false, Decision::Undecided, false, false);
    Expect("off, said yes", Mode::Off, true, false, Decision::Yes, false, false);
    Expect("ask, CLI", Mode::Ask, false, false, Decision::Undecided, false, false);
    Expect("ask, GUI worker", Mode::Ask, true, false, Decision::Undecided, false, true);
    Expect("ask, GUI thread", Mode::Ask, true, true, Decision::Undecided, false, true);
    Expect("ask, said yes", Mode::Ask, true, false, Decision::Yes, true, false);
    Expect("ask, said not now", Mode::Ask, true, false, Decision::No, false, false);
    if (HostOf("http://cod-assets.cdn.callofduty.com/pc/iw9_3") != "cod-assets.cdn.callofduty.com")
        Failures += "FAIL cdn host of a URL\n";
    // the live round: a worker waits for the GUI's answer, which then holds for the session
    const auto Saved = SettingsManager::GetSetting("cdn_images", "ask");
    SettingsManager::SetRunSetting("cdn_images", "ask");
    AttachAsker();
    bool WorkerGot = true;
    std::thread Worker([&] { WorkerGot = Allowed(); });
    std::string H;
    for (int I = 0; I < 300 && !Question(H); I++) std::this_thread::sleep_for(std::chrono::milliseconds(10));
    if (!Question(H)) Failures += "FAIL cdn consent: no question put to the GUI\n";
    // (not saved: a self-test leaves the user's settings alone)
    Settle(false, false);
    Worker.join();
    if (WorkerGot) Failures += "FAIL cdn consent: a worker downloads after \"Don't download\"\n";
    if (Question(H)) Failures += "FAIL cdn consent: asked again after \"Don't download\"\n";
    if (Allowed()) Failures += "FAIL cdn consent: allowed after \"Don't download\"\n";
    {
        std::lock_guard<std::mutex> Lock(ConsentLock);
        Session = Decision::Undecided, HasAsker = false, Pending = false;
    }
    SettingsManager::SetRunSetting("cdn_images", Saved);
    return Failures;
}
