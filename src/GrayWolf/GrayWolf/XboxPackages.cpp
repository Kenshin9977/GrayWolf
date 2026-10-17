#include "stdafx.h"

#include "XboxPackages.h"
#include "Strings.h"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <regex>

namespace fs = std::filesystem;

namespace
{
    std::string Lower(std::string S)
    {
        for (auto& C : S) C = (char)tolower((unsigned char)C);
        return S;
    }

    // "\\?\C:\Program Files\WindowsApps\Pkg\" -> "C:\Program Files\WindowsApps\Pkg"
    std::string Plain(std::string S)
    {
        if (S.rfind("\\\\?\\", 0) == 0) S = S.substr(4);
        std::replace(S.begin(), S.end(), '/', '\\');
        while (S.size() > 3 && S.back() == '\\') S.pop_back();
        return S;
    }

    // The folder as the system resolves it: a package installed to a folder the user picked is a
    // junction in WindowsApps to that folder, and both spellings reach this code
    std::string Final(const std::string& Dir)
    {
        HANDLE H = CreateFileW(fs::path(Dir).wstring().c_str(), 0, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                               nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr);
        if (H == INVALID_HANDLE_VALUE) return Dir;
        wchar_t Buf[2048];
        const DWORD N = GetFinalPathNameByHandleW(H, Buf, 2048, FILE_NAME_NORMALIZED);
        CloseHandle(H);
        return N > 0 && N < 2048 ? Plain(Strings::ToNormalString(std::wstring(Buf, N))) : Dir;
    }

    // MicrosoftGame.config's identity and, for a DLC, the game it belongs to. False without one.
    bool ReadConfig(const fs::path& Root, XboxPackages::Package& Out)
    {
        std::ifstream In(Root / "MicrosoftGame.config", std::ios::binary);
        if (!In) return false;
        std::string Text((std::istreambuf_iterator<char>(In)), std::istreambuf_iterator<char>());
        // the GDK writes it in UTF-8; a UTF-16 one (BOM FF FE) is narrowed, its markup being ASCII
        if (Text.size() >= 2 && (unsigned char)Text[0] == 0xff && (unsigned char)Text[1] == 0xfe)
        {
            std::string Narrow;
            for (size_t I = 2; I + 1 < Text.size(); I += 2) Narrow += Text[I + 1] ? '?' : Text[I];
            Text = std::move(Narrow);
        }
        static const std::regex Identity(R"re(<Identity\b[^>]*?\bName\s*=\s*"([^"]+)")re");
        static const std::regex Main(R"re(<MainPackageDependency\b[^>]*?\bName\s*=\s*"([^"]+)")re");
        std::smatch M;
        if (!std::regex_search(Text, M, Identity)) return false;
        Out.Root = Final(Root.string());
        Out.Name = M[1].str();
        Out.MainPackage = std::regex_search(Text, M, Main) ? M[1].str() : "";
        return true;
    }

    // The folders Gaming Services records: Root\<package key>\<install key>, value "Root"
    std::vector<std::string> RepositoryRoots()
    {
        std::vector<std::string> Out;
        const wchar_t* Base = L"SOFTWARE\\Microsoft\\GamingServices\\PackageRepository\\Root";
        HKEY Key = nullptr;
        if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, Base, 0, KEY_READ, &Key) != ERROR_SUCCESS) return Out;
        wchar_t Name[512];
        DWORD NameSize = 512;
        for (DWORD I = 0; RegEnumKeyExW(Key, I, Name, &NameSize, nullptr, nullptr, nullptr, nullptr) == ERROR_SUCCESS; ++I, NameSize = 512)
        {
            HKEY Sub = nullptr;
            if (RegOpenKeyExW(Key, Name, 0, KEY_READ, &Sub) != ERROR_SUCCESS) continue;
            wchar_t Inner[512];
            DWORD InnerSize = 512;
            for (DWORD J = 0; RegEnumKeyExW(Sub, J, Inner, &InnerSize, nullptr, nullptr, nullptr, nullptr) == ERROR_SUCCESS; ++J, InnerSize = 512)
            {
                wchar_t Buf[1024]{};
                DWORD Size = sizeof(Buf) - sizeof(wchar_t), Type = 0;
                if (RegGetValueW(Sub, Inner, L"Root", RRF_RT_REG_SZ, &Type, Buf, &Size) == ERROR_SUCCESS && Buf[0])
                    Out.push_back(Plain(Strings::ToNormalString(Buf)));
            }
            RegCloseKey(Sub);
        }
        RegCloseKey(Key);
        return Out;
    }

    // Each fixed drive's .GamingRoot: "RGBX", a u32 count, then that many UTF-16 folder paths
    // (zero-terminated, relative to the drive) where the Xbox app installs; a game there is
    // <folder>\<game>\Content
    std::vector<std::string> GamingRootGames()
    {
        std::vector<std::string> Out;
        wchar_t Drives[512]{};
        const DWORD Len = GetLogicalDriveStringsW(511, Drives);
        for (const wchar_t* D = Drives; D < Drives + Len && *D; D += wcslen(D) + 1)
        {
            if (GetDriveTypeW(D) != DRIVE_FIXED) continue;
            std::ifstream In(fs::path(D) / ".GamingRoot", std::ios::binary);
            if (!In) continue;
            std::string Bytes((std::istreambuf_iterator<char>(In)), std::istreambuf_iterator<char>());
            if (Bytes.size() < 8 || Bytes.compare(0, 4, "RGBX") != 0) continue;
            uint32_t Count = 0;
            memcpy(&Count, Bytes.data() + 4, 4);
            size_t At = 8;
            for (uint32_t I = 0; I < Count && I < 64 && At + 1 < Bytes.size(); ++I)
            {
                std::wstring Folder;
                for (; At + 1 < Bytes.size(); At += 2)
                {
                    const wchar_t C = (wchar_t)((unsigned char)Bytes[At] | ((unsigned char)Bytes[At + 1] << 8));
                    if (C == 0) { At += 2; break; }
                    Folder += C;
                }
                if (Folder.empty()) continue;
                std::error_code Ec;
                for (const auto& E : fs::directory_iterator(fs::path(D) / Folder, Ec))
                    if (E.is_directory(Ec) && fs::exists(E.path() / "Content" / "MicrosoftGame.config", Ec))
                        Out.push_back(Plain((E.path() / "Content").string()));
            }
        }
        return Out;
    }

    bool SameName(const std::string& A, const std::string& B) { return !A.empty() && Lower(A) == Lower(B); }
}

namespace XboxPackages
{
    std::vector<Package> Installed()
    {
        std::vector<Package> Out;
        std::vector<std::string> Seen;
        auto Roots = RepositoryRoots();
        for (auto& R : GamingRootGames()) Roots.push_back(std::move(R));
        for (const auto& Given : Roots)
        {
            const std::string R = Final(Given);
            const std::string Key = Lower(R);
            if (std::find(Seen.begin(), Seen.end(), Key) != Seen.end()) continue;
            Seen.push_back(Key);
            Package P;
            if (ReadConfig(R, P)) Out.push_back(std::move(P));
        }
        return Out;
    }

    bool Containing(const std::string& Folder, Package& Out)
    {
        std::error_code Ec;
        for (fs::path P = fs::path(Final(fs::absolute(fs::path(Folder), Ec).string())); !P.empty(); P = P.parent_path())
        {
            if (fs::exists(P / "MicrosoftGame.config", Ec)) return ReadConfig(P, Out);
            if (P == P.parent_path()) break;
        }
        return false;
    }

    std::vector<Package> Dlc(const std::string& Root)
    {
        std::vector<Package> Out;
        Package Game;
        if (!ReadConfig(Root, Game) || !Game.MainPackage.empty()) return Out;
        for (auto& P : Installed())
            if (SameName(P.MainPackage, Game.Name)) Out.push_back(std::move(P));
        return Out;
    }

    std::vector<std::string> SameFolderInGame(const std::string& Folder)
    {
        std::vector<std::string> Out;
        Package Own;
        if (!Containing(Folder, Own)) return Out;
        std::error_code Ec;
        const fs::path Rel = fs::relative(fs::path(Final(fs::absolute(fs::path(Folder), Ec).string())), Own.Root, Ec);
        if (Ec) return Out;
        const std::string Game = Own.MainPackage.empty() ? Own.Name : Own.MainPackage;
        for (const auto& P : Installed())
        {
            if (Lower(P.Root) == Lower(Own.Root)) continue;
            if (!SameName(P.Name, Game) && !SameName(P.MainPackage, Game)) continue;
            const fs::path There = fs::path(P.Root) / Rel;
            if (fs::is_directory(There, Ec)) Out.push_back(There.lexically_normal().string());
        }
        return Out;
    }
}
