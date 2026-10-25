#include "stdafx.h"

#include "GameInstalls.h"
#include "Strings.h"
#include "XboxPackages.h"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <map>
#include <regex>

namespace fs = std::filesystem;

namespace
{
    struct Title
    {
        const char* Display;
        int Year;
        std::vector<int> SteamApps;          // every app id that installs into this game's folder
        std::vector<const char*> Markers;    // any of these directly in the folder identifies it
        const char* DiskId;                  // DiskGame
    };

    // A game inside a shared install: its zone folder there, and its reader (which lists that folder)
    struct Part
    {
        const char* Folder;
        const char* Display;
        int Year;
        const char* DiskId;
    };

    // Call of Duty HQ holds every title since MWII (2022), each in its own zone folder
    const std::vector<Part>& HqParts()
    {
        static const std::vector<Part> All = {
            { "cod23", "Modern Warfare III",            2023, "mwiii" },
            { "sp23",  "Modern Warfare III (campaign)", 2023, "mwiii_sp" },
            { "cod24", "Black Ops 6",                   2024, "bo6" },
            { "sp24",  "Black Ops 6 (campaign)",        2024, "bo6sp" },
            { "cod25", "Black Ops 7",                   2025, "bo7" },
        };
        return All;
    }

    // The one table. Titles since MWII (2022) share the "Call of Duty HQ" folder behind cod.exe (a
    // single game's copy has its own codNN-cod.exe): it is listed once per game its zone folders
    // hold (HqParts). Mod tools (BO1 42740, BO3 455130) are not games.
    const std::vector<Title>& Titles()
    {
        static const std::vector<Title> All = {
            { "Call of Duty",                   2003, { 2620 },                   { "CoDSP.exe", "CoDMP.exe" },          "cod1" },
            { "Call of Duty 2",                 2005, { 2630 },                   { "CoD2SP_s.exe", "CoD2MP_s.exe" },    "cod2" },
            { "Call of Duty 4: Modern Warfare", 2007, { 7940 },                   { "iw3sp.exe", "iw3mp.exe" },          "cod4" },
            { "World at War",                   2008, { 10090 },                  { "CoDWaW.exe", "CoDWaWmp.exe" },      "waw" },
            { "Modern Warfare 2",               2009, { 10180, 10190 },           { "iw4sp.exe", "iw4mp.exe" },          "mw2" },
            { "Black Ops",                      2010, { 42700, 42710 },           { "BlackOps.exe", "BlackOpsMP.exe" },  "bo1" },
            { "Modern Warfare 3",               2011, { 42680, 42690 },           { "iw5sp.exe", "iw5mp.exe" },          "mw3" },
            { "Black Ops II",                   2012, { 202970, 202990, 212910 }, { "t6zm.exe", "t6sp.exe", "t6mp.exe" }, "bo2" },
            { "Ghosts",                         2013, { 209160, 209170 },         { "iw6sp64_ship.exe" },                "ghosts" },
            { "Advanced Warfare",               2014, { 209650, 209660 },         { "s1_sp64_ship.exe" },                "aw" },
            { "Black Ops III",                  2015, { 311210 },                 { "BlackOps3.exe" },                   "bo3" },
            { "Call of Duty Online",            2015, {},                         { "codoMP_client_shipRetail.exe" },    "codol" },
            { "Infinite Warfare",               2016, { 292730 },                 { "iw7_ship.exe" },                    "iw" },
            { "Modern Warfare Remastered",      2016, { 393080, 393100 },         { "h1_sp64_ship.exe" },                "mwr" },
            { "WWII",                           2017, { 476600, 476620 },         { "s2_sp64_ship.exe" },                "ww2" },
            { "Black Ops 4",                    2018, {},                         { "BlackOps4.exe" },                   "bo4" },
            { "Modern Warfare (2019)",          2019, {},                         { "ModernWarfare.exe" },               "mw2019" },
            { "Black Ops Cold War",             2020, {},                         { "BlackOpsColdWar.exe" },             "coldwar" },
            { "Call of Duty HQ",                2022, { 1938090 },                { "cod.exe", "cod23-cod.exe", "cod24-cod.exe", "cod25-cod.exe" }, "" },
        };
        return All;
    }

    const Title* ByAppId(int App)
    {
        for (const auto& T : Titles())
            if (std::find(T.SteamApps.begin(), T.SteamApps.end(), App) != T.SteamApps.end()) return &T;
        return nullptr;
    }

    const Title* ByMarker(const fs::path& Dir)
    {
        std::error_code Ec;
        for (const auto& T : Titles())
            for (const char* M : T.Markers)
                if (fs::exists(Dir / M, Ec)) return &T;
        return nullptr;
    }

    std::string ReadRegString(HKEY Root, const wchar_t* SubKey, const wchar_t* Value, DWORD Extra)
    {
        HKEY Key = nullptr;
        if (RegOpenKeyExW(Root, SubKey, 0, KEY_READ | Extra, &Key) != ERROR_SUCCESS) return "";
        wchar_t Buf[1024]{};
        DWORD Size = sizeof(Buf) - sizeof(wchar_t), Type = 0;
        const bool Ok = RegQueryValueExW(Key, Value, nullptr, &Type, (LPBYTE)Buf, &Size) == ERROR_SUCCESS &&
                        (Type == REG_SZ || Type == REG_EXPAND_SZ);
        RegCloseKey(Key);
        return Ok ? Strings::ToNormalString(Buf) : "";
    }

    // Steam's install folder, then every library its libraryfolders.vdf lists ("path" lines).
    std::vector<fs::path> SteamLibraries()
    {
        std::vector<fs::path> Out;
        std::string Steam = ReadRegString(HKEY_CURRENT_USER, L"Software\\Valve\\Steam", L"SteamPath", 0);
        if (Steam.empty()) Steam = ReadRegString(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Valve\\Steam", L"InstallPath", KEY_WOW64_32KEY);
        if (Steam.empty()) return Out;
        Out.push_back(fs::path(Steam));
        std::ifstream In(fs::path(Steam) / "steamapps" / "libraryfolders.vdf");
        const std::regex PathRe(R"RX("path"\s+"([^"]+)")RX");
        std::string Line;
        std::error_code Ec;
        while (std::getline(In, Line))
        {
            std::smatch M;
            if (!std::regex_search(Line, M, PathRe)) continue;
            std::string P;
            const std::string Raw = M[1].str();
            for (size_t K = 0; K < Raw.size(); K++)        // the vdf doubles its backslashes
            {
                if (Raw[K] == '\\' && K + 1 < Raw.size() && Raw[K + 1] == '\\') K++;
                P += Raw[K];
            }
            if (fs::is_directory(P, Ec)) Out.push_back(fs::path(P));
        }
        return Out;
    }

    // An appmanifest's "key" "value" pairs (flat: the keys read here are top-level)
    std::map<std::string, std::string> ReadManifest(const fs::path& File)
    {
        std::map<std::string, std::string> Out;
        std::ifstream In(File);
        const std::regex Pair(R"RX(^\s*"([^"]+)"\s+"([^"]*)")RX");
        std::string Line;
        while (std::getline(In, Line))
        {
            std::smatch M;
            if (std::regex_search(Line, M, Pair)) Out.emplace(M[1].str(), M[2].str());
        }
        return Out;
    }

    // (install folder, launcher): "Battle.net" when Blizzard published the key, else "installer"
    std::vector<std::pair<std::string, std::string>> UninstallLocations()
    {
        std::vector<std::pair<std::string, std::string>> Out;
        for (const wchar_t* Root : { L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Uninstall",
                                     L"SOFTWARE\\WOW6432Node\\Microsoft\\Windows\\CurrentVersion\\Uninstall" })
        {
            HKEY Key = nullptr;
            if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, Root, 0, KEY_READ, &Key) != ERROR_SUCCESS) continue;
            wchar_t Name[512];
            DWORD Index = 0, NameSize = 512;
            while (RegEnumKeyExW(Key, Index++, Name, &NameSize, nullptr, nullptr, nullptr, nullptr) == ERROR_SUCCESS)
            {
                NameSize = 512;
                const std::wstring Sub = std::wstring(Root) + L"\\" + Name;
                if (ReadRegString(HKEY_LOCAL_MACHINE, Sub.c_str(), L"DisplayName", 0).find("Call of Duty") == std::string::npos) continue;
                const auto Location = ReadRegString(HKEY_LOCAL_MACHINE, Sub.c_str(), L"InstallLocation", 0);
                const auto Publisher = ReadRegString(HKEY_LOCAL_MACHINE, Sub.c_str(), L"Publisher", 0);
                if (!Location.empty())
                    Out.emplace_back(Location, Publisher.find("Blizzard") != std::string::npos ? "Battle.net" : "installer");
            }
            RegCloseKey(Key);
        }
        return Out;
    }

    // The usual folders on every fixed drive, for copies no launcher knows (moved, copied by hand)
    std::vector<fs::path> CommonRoots()
    {
        std::vector<fs::path> Out;
        wchar_t Drives[512]{};
        const DWORD Len = GetLogicalDriveStringsW(511, Drives);
        for (const wchar_t* D = Drives; D < Drives + Len && *D; D += wcslen(D) + 1)
        {
            if (GetDriveTypeW(D) != DRIVE_FIXED) continue;
            const fs::path Root(D);
            for (const char* C : { "SteamLibrary\\steamapps\\common", "Steam\\steamapps\\common",
                                   "Program Files (x86)\\Steam\\steamapps\\common", "Games",
                                   "Program Files (x86)\\Call of Duty HQ", "Call of Duty HQ",
                                   "Battle.net", "Program Files (x86)\\Battle.net" })
                Out.push_back(Root / C);
        }
        return Out;
    }

    // The folder as the system resolves it (junctions, short names, case), else its normalised
    // spelling: the same install reached two ways is one entry
    std::string FinalPath(const fs::path& Dir)
    {
        std::wstring Out;
        HANDLE H = CreateFileW(Dir.wstring().c_str(), 0, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                               nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr);
        if (H != INVALID_HANDLE_VALUE)
        {
            wchar_t Buf[2048];
            const DWORD N = GetFinalPathNameByHandleW(H, Buf, 2048, FILE_NAME_NORMALIZED);
            if (N > 0 && N < 2048) Out.assign(Buf, N);
            CloseHandle(H);
        }
        if (Out.rfind(L"\\\\?\\", 0) == 0) Out = Out.substr(4);
        if (Out.empty()) Out = Dir.lexically_normal().wstring();
        std::string S = Strings::ToNormalString(Out);
        std::replace(S.begin(), S.end(), '/', '\\');
        while (S.size() > 3 && S.back() == '\\') S.pop_back();
        if (S.size() >= 2 && S[1] == ':') S[0] = (char)toupper((unsigned char)S[0]);
        return S;
    }

    std::string Lower(std::string S)
    {
        for (auto& C : S) C = (char)tolower((unsigned char)C);
        return S;
    }
}

namespace GameInstalls
{
    std::vector<Install> Detect()
    {
        std::vector<Install> Found;
        std::vector<std::string> Seen;
        std::error_code Ec;
        const auto Add = [&](const fs::path& Dir, const Title& T, const char* Source)
        {
            if (!fs::is_directory(Dir, Ec)) return;
            const std::string Path = FinalPath(Dir);
            const std::string Key = Lower(Path);
            if (std::find(Seen.begin(), Seen.end(), Key) != Seen.end()) return;
            Seen.push_back(Key);
            Install I;
            I.Path = Path;
            I.Display = T.Display;
            I.Label = T.Display;
            I.Year = T.Year;
            I.DiskId = T.DiskId;
            I.Source = Source;
            // the shared HQ folder (or one game's copy of it): one entry per game it holds, all on
            // the same folder
            bool Split = false;
            if (std::string(T.Display) == "Call of Duty HQ")
                for (const Part& P : HqParts())
                {
                    if (!fs::is_directory(Dir / P.Folder, Ec)) continue;
                    Install G = I;
                    G.Display = G.Label = P.Display;
                    G.Year = P.Year;
                    G.DiskId = P.DiskId;
                    Found.push_back(std::move(G));
                    Split = true;
                }
            if (!Split) Found.push_back(std::move(I));
        };

        // Steam: each library's app manifests name the game and say it is fully installed (StateFlags 4)
        const auto Libraries = SteamLibraries();
        for (const auto& Lib : Libraries)
            for (const auto& E : fs::directory_iterator(Lib / "steamapps", Ec))
            {
                const std::string Name = E.path().filename().string();
                if (Name.rfind("appmanifest_", 0) != 0 || E.path().extension() != ".acf") continue;
                const auto M = ReadManifest(E.path());
                const auto App = M.find("appid"), Dir = M.find("installdir"), Flags = M.find("StateFlags");
                if (App == M.end() || Dir == M.end()) continue;
                if (Flags != M.end() && (std::atoi(Flags->second.c_str()) & 4) == 0) continue;
                if (const Title* T = ByAppId(std::atoi(App->second.c_str())))
                    Add(Lib / "steamapps" / "common" / Dir->second, *T, "Steam");
            }

        // Battle.net and other installers: their uninstall keys, the game told by its exe
        for (const auto& [Dir, Launcher] : UninstallLocations())
            if (const Title* T = ByMarker(Dir)) Add(Dir, *T, Launcher.c_str());

        // the Xbox app (PC Game Pass, Microsoft Store): its packages, the game told by its exe (a
        // DLC package has none: its zones are listed with the game's, IDiskGame::ListZones)
        for (const auto& P : XboxPackages::Installed())
            if (const Title* T = ByMarker(P.Root)) Add(P.Root, *T, "Xbox");

        // copies no launcher manages: the usual folders, and Steam library folders without a manifest
        std::vector<fs::path> Roots = CommonRoots();
        for (const auto& Lib : Libraries) Roots.push_back(Lib / "steamapps" / "common");
        for (const auto& Root : Roots)
        {
            if (!fs::is_directory(Root, Ec)) continue;
            if (const Title* T = ByMarker(Root)) Add(Root, *T, "folder");
            for (const auto& E : fs::directory_iterator(Root, Ec))
                if (E.is_directory(Ec))
                    if (const Title* T = ByMarker(E.path())) Add(E.path(), *T, "folder");
        }

        std::stable_sort(Found.begin(), Found.end(), [](const Install& A, const Install& B) {
            return A.Year != B.Year ? A.Year < B.Year : A.Display < B.Display;
        });
        // two installs of one game: tell them apart by drive and folder
        for (auto& I : Found)
        {
            const auto Same = std::count_if(Found.begin(), Found.end(), [&](const Install& O) { return O.Display == I.Display; });
            // an Xbox app game's folder is <its folder>\Content: the folder above names it
            fs::path Folder(I.Path);
            if (Lower(Folder.filename().string()) == "content" && Folder.has_parent_path()) Folder = Folder.parent_path();
            if (Same > 1) I.Label = I.Display + "  (" + I.Path.substr(0, 2) + " " + Folder.filename().string() + ")";
        }
        return Found;
    }
}
