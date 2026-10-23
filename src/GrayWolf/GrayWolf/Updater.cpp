#include "Updater.h"

#include "AppLog.h"
#include "FileSystems.h"
#include "SettingsManager.h"
#include "Strings.h"

#include <miniz/miniz.h>
#include <nlohmann/json.hpp>

#include <bcrypt.h>
#include <winhttp.h>

#include <atomic>
#include <filesystem>
#include <format>
#include <fstream>
#include <mutex>
#include <regex>
#include <thread>
#include <vector>

#pragma comment(lib, "winhttp.lib")
#pragma comment(lib, "bcrypt.lib")
#pragma comment(lib, "version.lib")

namespace fs = std::filesystem;

namespace Updater
{
namespace
{
    constexpr const char* DefaultApi = "https://api.github.com";
    constexpr const char* PackagesStamp = "hash-packages.json";
    constexpr const char* StagingName = ".update-staging";
    constexpr const char* BackupName = ".update-backup";
    constexpr size_t MaxDownload = 1024ull * 1024 * 1024; // 1 GB: a sane upper bound for one asset

    fs::path P(const std::string& Utf8) { return fs::path(Strings::ToUnicodeString(Utf8)); }
    std::string U8(const fs::path& Path) { return Strings::ToNormalString(Path.wstring()); }

    std::string Env(const char* Name)
    {
        char Buf[4096];
        const DWORD N = GetEnvironmentVariableA(Name, Buf, sizeof(Buf));
        return (N > 0 && N < sizeof(Buf)) ? std::string(Buf, N) : std::string();
    }

    std::string Lower(std::string S)
    {
        for (auto& Ch : S) Ch = (char)std::tolower((unsigned char)Ch);
        return S;
    }

    // ---------------------------------------------------------------------------------------
    // HTTP (WinHTTP, redirects followed by hand so the token never leaves the API host)

    struct Url
    {
        bool Https = true;
        std::wstring Host;
        INTERNET_PORT Port = 0;
        std::wstring PathAndQuery;
    };

    bool ParseUrl(const std::string& Text, Url& Out)
    {
        const std::wstring W = Strings::ToUnicodeString(Text);
        URL_COMPONENTS Parts{};
        Parts.dwStructSize = sizeof(Parts);
        Parts.dwSchemeLength = (DWORD)-1;
        Parts.dwHostNameLength = (DWORD)-1;
        Parts.dwUrlPathLength = (DWORD)-1;
        Parts.dwExtraInfoLength = (DWORD)-1;
        if (!WinHttpCrackUrl(W.c_str(), (DWORD)W.size(), 0, &Parts)) return false;
        Out.Https = Parts.nScheme == INTERNET_SCHEME_HTTPS;
        Out.Host.assign(Parts.lpszHostName, Parts.dwHostNameLength);
        Out.Port = Parts.nPort;
        Out.PathAndQuery.assign(Parts.lpszUrlPath, Parts.dwUrlPathLength);
        if (Parts.lpszExtraInfo) Out.PathAndQuery.append(Parts.lpszExtraInfo, Parts.dwExtraInfoLength);
        if (Out.PathAndQuery.empty()) Out.PathAndQuery = L"/";
        if (Out.Host.empty()) return false;
        // Plain http only for a local test server (GRAYWOLF_UPDATE_API=http://127.0.0.1:...)
        if (!Out.Https)
        {
            const std::wstring H = Out.Host;
            if (H != L"127.0.0.1" && H != L"localhost" && H != L"::1") return false;
        }
        return true;
    }

    bool SameOrigin(const Url& A, const Url& B)
    {
        return A.Https == B.Https && A.Port == B.Port && _wcsicmp(A.Host.c_str(), B.Host.c_str()) == 0;
    }

    struct Response
    {
        bool Sent = false;      // a status line came back
        DWORD Status = 0;
        std::vector<uint8_t> Body;
        std::string Location;
        std::string Error;      // transport failure, for the log
    };

    struct Handle
    {
        HINTERNET H = nullptr;
        explicit Handle(HINTERNET In = nullptr) : H(In) {}
        ~Handle() { if (H) WinHttpCloseHandle(H); }
        Handle(const Handle&) = delete;
        Handle& operator=(const Handle&) = delete;
    };

    Response RawGet(HINTERNET Session, const Url& Target, const std::wstring& Headers)
    {
        Response R;
        Handle Connect(WinHttpConnect(Session, Target.Host.c_str(), Target.Port, 0));
        if (!Connect.H) { R.Error = std::format("connect failed ({})", GetLastError()); return R; }
        Handle Request(WinHttpOpenRequest(Connect.H, L"GET", Target.PathAndQuery.c_str(), nullptr, WINHTTP_NO_REFERER,
                                          WINHTTP_DEFAULT_ACCEPT_TYPES, Target.Https ? WINHTTP_FLAG_SECURE : 0));
        if (!Request.H) { R.Error = std::format("open request failed ({})", GetLastError()); return R; }
        DWORD Never = WINHTTP_OPTION_REDIRECT_POLICY_NEVER;
        WinHttpSetOption(Request.H, WINHTTP_OPTION_REDIRECT_POLICY, &Never, sizeof(Never));
        if (!WinHttpSendRequest(Request.H, Headers.empty() ? WINHTTP_NO_ADDITIONAL_HEADERS : Headers.c_str(),
                                Headers.empty() ? 0 : (DWORD)-1L, WINHTTP_NO_REQUEST_DATA, 0, 0, 0) ||
            !WinHttpReceiveResponse(Request.H, nullptr))
        {
            R.Error = std::format("request failed ({})", GetLastError());
            return R;
        }
        DWORD Status = 0, Size = sizeof(Status);
        if (!WinHttpQueryHeaders(Request.H, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX,
                                 &Status, &Size, WINHTTP_NO_HEADER_INDEX))
        {
            R.Error = "no status";
            return R;
        }
        R.Sent = true;
        R.Status = Status;
        if (Status >= 300 && Status < 400)
        {
            DWORD Len = 0;
            WinHttpQueryHeaders(Request.H, WINHTTP_QUERY_LOCATION, WINHTTP_HEADER_NAME_BY_INDEX, nullptr, &Len, WINHTTP_NO_HEADER_INDEX);
            if (Len > 0)
            {
                std::wstring Loc(Len / sizeof(wchar_t), L'\0');
                if (WinHttpQueryHeaders(Request.H, WINHTTP_QUERY_LOCATION, WINHTTP_HEADER_NAME_BY_INDEX, Loc.data(), &Len, WINHTTP_NO_HEADER_INDEX))
                {
                    Loc.resize(Len / sizeof(wchar_t));
                    R.Location = Strings::ToNormalString(Loc);
                }
            }
            return R;
        }
        for (;;)
        {
            DWORD Avail = 0;
            if (!WinHttpQueryDataAvailable(Request.H, &Avail)) { R.Sent = false; R.Error = "read failed"; return R; }
            if (Avail == 0) break;
            const size_t At = R.Body.size();
            if (At + Avail > MaxDownload) { R.Sent = false; R.Error = "download too large"; return R; }
            R.Body.resize(At + Avail);
            DWORD Read = 0;
            if (!WinHttpReadData(Request.H, R.Body.data() + At, Avail, &Read)) { R.Sent = false; R.Error = "read failed"; return R; }
            R.Body.resize(At + Read);
        }
        return R;
    }

    // One update run's connection: the API origin, and the token sent to that origin only.
    class Client
    {
    public:
        explicit Client(const Config& C) : Cfg(C)
        {
            ApiOk = ParseUrl(C.ApiBase, Api);
            Session.H = WinHttpOpen(L"GrayWolf-Updater", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME,
                                    WINHTTP_NO_PROXY_BYPASS, 0);
            if (Session.H) WinHttpSetTimeouts(Session.H, 10000, 10000, 15000, 30000);
            Token = Env("GRAYWOLF_GITHUB_TOKEN");
            if (Token.empty() && C.UseGhToken) Token = GhToken();
        }

        bool HasToken() const { return !Token.empty(); }
        bool Usable() const { return ApiOk && Session.H; }

        Response Get(std::string Address, const wchar_t* Accept)
        {
            for (int Hop = 0; Hop < 6; Hop++)
            {
                Url Target;
                if (!ParseUrl(Address, Target))
                {
                    Response R;
                    R.Error = "refused URL (https only)";
                    return R;
                }
                std::wstring Headers = std::wstring(L"Accept: ") + Accept + L"\r\nX-GitHub-Api-Version: 2022-11-28\r\n";
                if (!Token.empty() && SameOrigin(Target, Api))
                    Headers += L"Authorization: Bearer " + Strings::ToUnicodeString(Token) + L"\r\n";
                Response R = RawGet(Session.H, Target, Headers);
                if (!R.Sent || R.Status < 300 || R.Status >= 400) return R;
                if (R.Location.empty()) return R;
                // A relative Location stays on the same origin
                if (R.Location.starts_with("/"))
                    Address = std::string(Target.Https ? "https://" : "http://") + Strings::ToNormalString(Target.Host) +
                              (Target.Port == (Target.Https ? 443 : 80) ? "" : ":" + std::to_string(Target.Port)) + R.Location;
                else
                    Address = R.Location;
            }
            Response R;
            R.Error = "too many redirects";
            return R;
        }

    private:
        // `gh auth token`, only when the user turned update_use_gh_token on. Never logged.
        static std::string GhToken()
        {
            wchar_t GhPath[MAX_PATH] = {};
            if (!SearchPathW(nullptr, L"gh.exe", nullptr, MAX_PATH, GhPath, nullptr)) return {};
            SECURITY_ATTRIBUTES Sa{ sizeof(Sa), nullptr, TRUE };
            HANDLE ReadEnd = nullptr, WriteEnd = nullptr;
            if (!CreatePipe(&ReadEnd, &WriteEnd, &Sa, 0)) return {};
            SetHandleInformation(ReadEnd, HANDLE_FLAG_INHERIT, 0);
            STARTUPINFOW Si{};
            Si.cb = sizeof(Si);
            Si.dwFlags = STARTF_USESTDHANDLES;
            Si.hStdOutput = WriteEnd;
            Si.hStdError = nullptr;
            Si.hStdInput = nullptr;
            PROCESS_INFORMATION Pi{};
            std::wstring Cmd = L"\"" + std::wstring(GhPath) + L"\" auth token";
            const BOOL Started = CreateProcessW(nullptr, Cmd.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW, nullptr, nullptr, &Si, &Pi);
            CloseHandle(WriteEnd);
            std::string Out;
            if (Started)
            {
                char Buf[512];
                DWORD Read = 0;
                while (ReadFile(ReadEnd, Buf, sizeof(Buf), &Read, nullptr) && Read > 0 && Out.size() < 4096) Out.append(Buf, Read);
                WaitForSingleObject(Pi.hProcess, 5000);
                CloseHandle(Pi.hThread);
                CloseHandle(Pi.hProcess);
            }
            CloseHandle(ReadEnd);
            while (!Out.empty() && std::isspace((unsigned char)Out.back())) Out.pop_back();
            // a token is one printable word; anything else (an error text) is not one
            for (char Ch : Out) if (std::isspace((unsigned char)Ch) || Ch < 0x21 || Ch > 0x7e) return {};
            return Out;
        }

        Config Cfg;
        Url Api;
        bool ApiOk = false;
        Handle Session;
        std::string Token;
    };

    // ---------------------------------------------------------------------------------------
    // sha256, zip

    std::string Sha256Hex(const std::vector<uint8_t>& Data)
    {
        BCRYPT_ALG_HANDLE Alg = nullptr;
        BCRYPT_HASH_HANDLE Hash = nullptr;
        uint8_t Digest[32] = {};
        bool Ok = BCryptOpenAlgorithmProvider(&Alg, BCRYPT_SHA256_ALGORITHM, nullptr, 0) == 0 &&
                  BCryptCreateHash(Alg, &Hash, nullptr, 0, nullptr, 0, 0) == 0;
        size_t At = 0;
        while (Ok && At < Data.size())
        {
            const ULONG Chunk = (ULONG)(std::min)(Data.size() - At, (size_t)0x40000000);
            Ok = BCryptHashData(Hash, (PUCHAR)Data.data() + At, Chunk, 0) == 0;
            At += Chunk;
        }
        Ok = Ok && BCryptFinishHash(Hash, Digest, sizeof(Digest), 0) == 0;
        if (Hash) BCryptDestroyHash(Hash);
        if (Alg) BCryptCloseAlgorithmProvider(Alg, 0);
        if (!Ok) return {};
        std::string Hex;
        for (uint8_t B : Digest) Hex += std::format("{:02x}", B);
        return Hex;
    }

    // A zip entry name that stays inside the folder it is extracted to.
    bool SafeEntryName(std::string& Name)
    {
        for (auto& Ch : Name) if (Ch == '\\') Ch = '/';
        if (Name.empty() || Name[0] == '/' || Name.find(':') != std::string::npos) return false;
        size_t Start = 0;
        while (Start <= Name.size())
        {
            const size_t End = Name.find('/', Start);
            const std::string Part = Name.substr(Start, End == std::string::npos ? std::string::npos : End - Start);
            if (Part == "..") return false;
            if (End == std::string::npos) break;
            Start = End + 1;
        }
        return true;
    }

    // Extracts every file of Zip under Dir; Files gets their relative paths ('/' separated).
    bool ExtractZip(const std::vector<uint8_t>& Zip, const fs::path& Dir, std::vector<std::string>& Files, std::string& Error)
    {
        mz_zip_archive Archive{};
        if (!mz_zip_reader_init_mem(&Archive, Zip.data(), Zip.size(), 0)) { Error = "not a zip archive"; return false; }
        bool Ok = true;
        const mz_uint Count = mz_zip_reader_get_num_files(&Archive);
        for (mz_uint I = 0; I < Count && Ok; I++)
        {
            mz_zip_archive_file_stat Stat{};
            if (!mz_zip_reader_file_stat(&Archive, I, &Stat)) { Error = "bad zip entry"; Ok = false; break; }
            if (Stat.m_is_directory) continue;
            std::string Name = Stat.m_filename;
            if (!SafeEntryName(Name)) { Error = "unsafe path in archive: " + Name; Ok = false; break; }
            size_t Size = 0;
            void* Data = mz_zip_reader_extract_to_heap(&Archive, I, &Size, 0); // checks the CRC
            if (!Data) { Error = "corrupt entry: " + Name; Ok = false; break; }
            const fs::path Out = Dir / P(Name);
            std::error_code Ec;
            fs::create_directories(Out.parent_path(), Ec);
            std::ofstream File(Out, std::ios::binary | std::ios::trunc);
            File.write((const char*)Data, (std::streamsize)Size);
            mz_free(Data);
            File.close();
            if (!File) { Error = "cannot write " + U8(Out); Ok = false; break; }
            Files.push_back(Name);
        }
        mz_zip_reader_end(&Archive);
        return Ok;
    }

    bool IsLink(const fs::path& Path)
    {
        const DWORD A = GetFileAttributesW(Path.c_str());
        return A != INVALID_FILE_ATTRIBUTES && (A & FILE_ATTRIBUTE_REPARSE_POINT);
    }

    bool MoveReplace(const fs::path& From, const fs::path& To, int Tries = 1)
    {
        for (int I = 0; I < Tries; I++)
        {
            if (MoveFileExW(From.c_str(), To.c_str(), MOVEFILE_REPLACE_EXISTING)) return true;
            if (I + 1 < Tries) Sleep(250);
        }
        return false;
    }

    // Removes one of our own working folders (never a link: a dev worktree's package_index is one).
    void RemoveOwnDir(const fs::path& Dir)
    {
        std::error_code Ec;
        if (!fs::exists(Dir, Ec) || IsLink(Dir)) return;
        fs::remove_all(Dir, Ec);
    }

    std::vector<std::string> ListFiles(const fs::path& Dir)
    {
        std::vector<std::string> Out;
        std::error_code Ec;
        for (auto It = fs::recursive_directory_iterator(Dir, Ec); !Ec && It != fs::recursive_directory_iterator(); It.increment(Ec))
        {
            if (It->is_regular_file(Ec))
            {
                std::string Rel = U8(fs::relative(It->path(), Dir, Ec));
                for (auto& Ch : Rel) if (Ch == '\\') Ch = '/';
                Out.push_back(Rel);
            }
        }
        return Out;
    }

    // A swap interrupted after it set files aside: put the previous files back.
    void RecoverInterruptedSwap(const fs::path& PackageDir)
    {
        const fs::path Backup = PackageDir / BackupName;
        std::error_code Ec;
        if (!fs::exists(Backup, Ec)) return;
        for (const auto& Rel : ListFiles(Backup))
            MoveReplace(Backup / P(Rel), PackageDir / P(Rel));
        AppLog::Log().warn("updates: restored package_index files set aside by an interrupted update");
        RemoveOwnDir(Backup);
    }

    // Moves every staged file into Dir, the files it replaces set aside in Backup; on any failure
    // the new files are removed and the previous ones put back.
    bool SwapIn(const fs::path& Staging, const fs::path& Dir, const fs::path& Backup, const std::vector<std::string>& Files,
                std::string& Error)
    {
        RemoveOwnDir(Backup);
        std::error_code Ec;
        std::vector<std::string> Moved, Installed;
        bool Ok = true;
        for (const auto& Rel : Files)
        {
            const fs::path Target = Dir / P(Rel), Aside = Backup / P(Rel);
            fs::create_directories(Target.parent_path(), Ec);
            if (fs::exists(Target, Ec))
            {
                fs::create_directories(Aside.parent_path(), Ec);
                if (!MoveReplace(Target, Aside, 4)) { Error = std::format("cannot set aside {} ({})", Rel, GetLastError()); Ok = false; break; }
                Moved.push_back(Rel);
            }
            if (!MoveReplace(Staging / P(Rel), Target, 4)) { Error = std::format("cannot install {} ({})", Rel, GetLastError()); Ok = false; break; }
            Installed.push_back(Rel);
        }
        if (!Ok)
        {
            for (const auto& Rel : Installed) DeleteFileW((Dir / P(Rel)).c_str());
            for (const auto& Rel : Moved) MoveReplace(Backup / P(Rel), Dir / P(Rel), 4);
        }
        RemoveOwnDir(Backup);
        return Ok;
    }

    bool WriteFileAtomic(const fs::path& Path, const std::string& Text)
    {
        const fs::path Tmp = fs::path(Path).concat(L".tmp");
        {
            std::ofstream Out(Tmp, std::ios::binary | std::ios::trunc);
            Out << Text;
            if (!Out) return false;
        }
        return MoveReplace(Tmp, Path, 4);
    }

    // A per-user install (tools/installer/GrayWolf.iss, AppId GrayWolf) shows its version in Windows'
    // installed apps: once the updater has replaced that install's files, that line follows.
    bool UpdateUninstallEntry(const fs::path& Target, const std::string& Version)
    {
        HKEY Key = nullptr;
        if (RegOpenKeyExW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\GrayWolf_is1", 0,
                          KEY_QUERY_VALUE | KEY_SET_VALUE, &Key) != ERROR_SUCCESS)
            return false;
        wchar_t Location[MAX_PATH] = {};
        DWORD Size = sizeof(Location) - sizeof(wchar_t), Type = 0;
        std::error_code Ec;
        bool Ok = RegQueryValueExW(Key, L"InstallLocation", nullptr, &Type, (BYTE*)Location, &Size) == ERROR_SUCCESS &&
                  Type == REG_SZ && fs::equivalent(fs::path(Location), Target, Ec);
        if (Ok)
        {
            const std::wstring W = Strings::ToUnicodeString(Version);
            Ok = RegSetValueExW(Key, L"DisplayVersion", 0, REG_SZ, (const BYTE*)W.c_str(), DWORD((W.size() + 1) * sizeof(wchar_t))) ==
                 ERROR_SUCCESS;
        }
        RegCloseKey(Key);
        return Ok;
    }

    std::string SafeVersion(const std::string& V)
    {
        std::string Out;
        for (char Ch : V) if (std::isalnum((unsigned char)Ch) || Ch == '.' || Ch == '-') Out += Ch;
        return Out.empty() ? "unknown" : Out;
    }

    Status Download(Client& Http, const Asset& A, std::vector<uint8_t>& Data, std::string& Message)
    {
        const Response R = Http.Get(A.Url, L"application/octet-stream");
        if (!R.Sent) { Message = "download failed: " + R.Error; return Status::NetworkError; }
        if (R.Status != 200) { Message = std::format("download of {} failed: HTTP {}", A.File, R.Status); return Status::NetworkError; }
        const std::string Got = Sha256Hex(R.Body);
        if (Got.empty() || Got != Lower(A.Sha256))
        {
            Message = std::format("{}: sha256 mismatch (manifest {}, downloaded {}): not installed", A.File, A.Sha256, Got);
            return Status::VerifyFailed;
        }
        Data = std::move(R.Body);
        return Status::Updated;
    }
}

const char* StatusName(Status S)
{
    switch (S)
    {
    case Status::UpToDate: return "up-to-date";
    case Status::Available: return "available";
    case Status::Updated: return "updated";
    case Status::Unavailable: return "unavailable";
    case Status::NetworkError: return "network-error";
    case Status::VerifyFailed: return "verify-failed";
    case Status::Refused: return "refused";
    case Status::Failed: return "failed";
    }
    return "?";
}

int CompareVersions(const std::string& A, const std::string& B)
{
    auto Parts = [](const std::string& S) {
        std::vector<long long> Out;
        size_t I = (!S.empty() && (S[0] == 'v' || S[0] == 'V')) ? 1 : 0;
        while (I < S.size())
        {
            long long N = 0;
            bool Any = false;
            while (I < S.size() && std::isdigit((unsigned char)S[I])) { N = N * 10 + (S[I] - '0'); I++; Any = true; }
            if (Any) Out.push_back(N);
            while (I < S.size() && !std::isdigit((unsigned char)S[I])) I++;
        }
        return Out;
    };
    auto X = Parts(A), Y = Parts(B);
    const size_t N = (std::max)(X.size(), Y.size());
    X.resize(N, 0);
    Y.resize(N, 0);
    for (size_t I = 0; I < N; I++)
        if (X[I] != Y[I]) return X[I] < Y[I] ? -1 : 1;
    return 0;
}

std::string AppVersion()
{
    wchar_t Exe[MAX_PATH] = {};
    GetModuleFileNameW(nullptr, Exe, MAX_PATH);
    DWORD Ignored = 0;
    const DWORD Size = GetFileVersionInfoSizeW(Exe, &Ignored);
    if (!Size) return "0";
    std::vector<uint8_t> Buf(Size);
    VS_FIXEDFILEINFO* Info = nullptr;
    UINT Len = 0;
    if (!GetFileVersionInfoW(Exe, 0, Size, Buf.data()) || !VerQueryValueW(Buf.data(), L"\\", (void**)&Info, &Len) || !Info)
        return "0";
    return std::format("{}.{}.{}.{}", HIWORD(Info->dwProductVersionMS), LOWORD(Info->dwProductVersionMS),
                       HIWORD(Info->dwProductVersionLS), LOWORD(Info->dwProductVersionLS));
}

std::string LocalPackagesVersion(const std::string& PackageDir)
{
    std::ifstream In(P(PackageDir) / PackagesStamp, std::ios::binary);
    if (!In) return {};
    const auto J = nlohmann::json::parse(In, nullptr, false);
    if (J.is_discarded() || !J.is_object() || !J.contains("version") || !J["version"].is_string()) return {};
    return J["version"].get<std::string>();
}

Config FromSettings()
{
    Config C;
    C.Repo = SettingsManager::GetSetting("update_repo", "Kenshin9977/GrayWolf");
    C.ApiBase = Env("GRAYWOLF_UPDATE_API");
    if (C.ApiBase.empty()) C.ApiBase = DefaultApi;
    while (!C.ApiBase.empty() && C.ApiBase.back() == '/') C.ApiBase.pop_back();
    C.UseGhToken = SettingsManager::GetSetting("update_use_gh_token", "false") == "true";
    C.AppDir = FileSystems::GetApplicationPath();
    C.PackageDir = U8(P(C.AppDir) / "package_index");
    return C;
}

CheckResult Check(const Config& C)
{
    CheckResult Out;
    Out.LocalApp = AppVersion();
    Out.LocalPackages = LocalPackagesVersion(C.PackageDir);

    static const std::regex RepoShape("^[A-Za-z0-9_.-]+/[A-Za-z0-9_.-]+$");
    if (!std::regex_match(C.Repo, RepoShape))
    {
        Out.Result = Status::Unavailable;
        Out.Message = "update_repo is not owner/name: " + C.Repo;
        return Out;
    }
    Client Http(C);
    if (!Http.Usable())
    {
        Out.Result = Status::NetworkError;
        Out.Message = "bad API address or no HTTP session: " + C.ApiBase;
        return Out;
    }

    const Response R = Http.Get(C.ApiBase + "/repos/" + C.Repo + "/releases/latest", L"application/vnd.github+json");
    if (!R.Sent)
    {
        Out.Result = Status::NetworkError;
        Out.Message = "network unavailable: " + R.Error;
        return Out;
    }
    if (R.Status == 404)
    {
        Out.Result = Status::Unavailable;
        Out.Message = Http.HasToken()
            ? "updates unavailable: no release published in " + C.Repo
            : "updates unavailable (private repository): set GRAYWOLF_GITHUB_TOKEN, or turn update_use_gh_token on";
        return Out;
    }
    if (R.Status != 200)
    {
        Out.Result = Status::NetworkError;
        Out.Message = std::format("GitHub API answered HTTP {}{}", R.Status,
                                  R.Status == 401 ? " (bad token)" : R.Status == 403 ? " (rate limit or no access)" : "");
        return Out;
    }

    const auto Rel = nlohmann::json::parse(R.Body.begin(), R.Body.end(), nullptr, false);
    if (Rel.is_discarded() || !Rel.is_object())
    {
        Out.Result = Status::NetworkError;
        Out.Message = "unreadable release JSON";
        return Out;
    }
    Out.Latest.Tag = Rel.value("tag_name", "");
    std::map<std::string, std::string> AssetUrls;
    if (Rel.contains("assets") && Rel["assets"].is_array())
        for (const auto& A : Rel["assets"])
            if (A.is_object() && A.contains("name") && A["name"].is_string() && A.contains("url") && A["url"].is_string())
                AssetUrls[A["name"].get<std::string>()] = A["url"].get<std::string>();

    const auto ManifestUrl = AssetUrls.find("manifest.json");
    if (ManifestUrl == AssetUrls.end())
    {
        Out.Result = Status::Unavailable;
        Out.Message = "release " + Out.Latest.Tag + " has no manifest.json";
        return Out;
    }
    const Response M = Http.Get(ManifestUrl->second, L"application/octet-stream");
    if (!M.Sent || M.Status != 200)
    {
        Out.Result = Status::NetworkError;
        Out.Message = M.Sent ? std::format("manifest.json: HTTP {}", M.Status) : "manifest.json: " + M.Error;
        return Out;
    }
    const auto Man = nlohmann::json::parse(M.Body.begin(), M.Body.end(), nullptr, false);
    if (Man.is_discarded() || !Man.is_object())
    {
        Out.Result = Status::Unavailable;
        Out.Message = "unreadable manifest.json";
        return Out;
    }
    auto Part = [&](const char* Key) -> std::optional<Asset> {
        if (!Man.contains(Key) || !Man[Key].is_object()) return std::nullopt;
        const auto& J = Man[Key];
        Asset A;
        A.Version = J.value("version", "");
        A.File = J.value("file", "");
        A.Sha256 = J.value("sha256", "");
        const auto U = AssetUrls.find(A.File);
        if (A.Version.empty() || A.File.empty() || A.Sha256.size() != 64 || U == AssetUrls.end())
        {
            AppLog::Log().warn("updates: manifest entry '{}' incomplete or its file is not in the release", Key);
            return std::nullopt;
        }
        A.Url = U->second;
        return A;
    };
    Out.Latest.App = Part("app");
    Out.Latest.Packages = Part("packages");
    Out.AppNewer = Out.Latest.App && CompareVersions(Out.Latest.App->Version, Out.LocalApp) > 0;
    Out.PackagesNewer = Out.Latest.Packages &&
        (C.Force || Out.LocalPackages.empty() || CompareVersions(Out.Latest.Packages->Version, Out.LocalPackages) > 0);
    Out.Result = (Out.AppNewer || Out.PackagesNewer) ? Status::Available : Status::UpToDate;
    Out.Message = std::format("latest {}: app {} (have {}), packages {} (have {})", Out.Latest.Tag,
                              Out.Latest.App ? Out.Latest.App->Version : "-", Out.LocalApp,
                              Out.Latest.Packages ? Out.Latest.Packages->Version : "-",
                              Out.LocalPackages.empty() ? "none" : Out.LocalPackages);
    return Out;
}

Status UpdatePackages(const Config& C, const Asset& Packages, std::string& Message)
{
    const fs::path Dir = P(C.PackageDir);
    std::error_code Ec;
    if (!C.Force && fs::exists(Dir, Ec))
    {
        if (IsLink(Dir))
        {
            Message = "package_index is a link (a dev worktree): left alone (update --force replaces its files)";
            return Status::Refused;
        }
        if (LocalPackagesVersion(C.PackageDir).empty())
        {
            bool HasFiles = false;
            for (auto It = fs::directory_iterator(Dir, Ec); !Ec && It != fs::directory_iterator(); It.increment(Ec))
                if (It->is_regular_file(Ec)) { HasFiles = true; break; }
            if (HasFiles)
            {
                Message = "package_index holds files not installed by updates (no hash-packages.json): left alone "
                          "(update --force installs the release's files over them)";
                return Status::Refused;
            }
        }
    }
    fs::create_directories(Dir, Ec);
    RecoverInterruptedSwap(Dir);

    Client Http(C);
    std::vector<uint8_t> Zip;
    const Status Got = Download(Http, Packages, Zip, Message);
    if (Got != Status::Updated) return Got;

    const fs::path Staging = Dir / StagingName;
    RemoveOwnDir(Staging);
    std::vector<std::string> Files;
    std::string Error;
    if (!ExtractZip(Zip, Staging, Files, Error) || Files.empty())
    {
        RemoveOwnDir(Staging);
        Message = Packages.File + ": " + (Error.empty() ? "empty archive" : Error) + ": not installed";
        return Status::VerifyFailed;
    }
    if (!SwapIn(Staging, Dir, Dir / BackupName, Files, Error))
    {
        RemoveOwnDir(Staging);
        Message = "package swap failed, previous files kept: " + Error;
        return Status::Failed;
    }
    RemoveOwnDir(Staging);

    nlohmann::json Stamp = { { "version", Packages.Version }, { "file", Packages.File }, { "sha256", Lower(Packages.Sha256) },
                             { "files", Files } };
    if (!WriteFileAtomic(Dir / PackagesStamp, Stamp.dump(2)))
    {
        Message = "packages installed but hash-packages.json could not be written";
        return Status::Failed;
    }
    Message = std::format("hash packages {} installed ({} files)", Packages.Version, Files.size());
    return Status::Updated;
}

Status StageApp(const Config& C, const Asset& App, std::string& StageDir, std::string& Message)
{
    const fs::path Stage = P(C.AppDir) / ".update" / ("stage-" + SafeVersion(App.Version));
    StageDir = U8(Stage);
    {
        std::ifstream Marker(Stage / ".verified", std::ios::binary);
        std::string Sha;
        if (Marker && std::getline(Marker, Sha) && Sha == Lower(App.Sha256))
        {
            Message = "GrayWolf " + App.Version + " already downloaded";
            return Status::Updated;
        }
    }
    Client Http(C);
    std::vector<uint8_t> Zip;
    const Status Got = Download(Http, App, Zip, Message);
    if (Got != Status::Updated) return Got;
    RemoveOwnDir(Stage);
    std::vector<std::string> Files;
    std::string Error;
    std::error_code Ec;
    if (!ExtractZip(Zip, Stage, Files, Error) || !fs::exists(Stage / "GrayWolf.exe", Ec))
    {
        RemoveOwnDir(Stage);
        Message = App.File + ": " + (Error.empty() ? "no GrayWolf.exe at the archive's root" : Error);
        return Status::VerifyFailed;
    }
    std::ofstream(Stage / ".verified", std::ios::binary) << Lower(App.Sha256) << "\n";
    Message = "GrayWolf " + App.Version + " downloaded and verified";
    return Status::Updated;
}

bool LaunchApply(const Config& C, const std::string& StageDir, bool Relaunch, std::string& Message)
{
    // The helper is a copy of this exe: it runs from .update\apply while GrayWolf.exe is replaced.
    wchar_t Self[MAX_PATH] = {};
    GetModuleFileNameW(nullptr, Self, MAX_PATH);
    const fs::path ApplyDir = P(C.AppDir) / ".update" / "apply";
    const fs::path Helper = ApplyDir / "GrayWolf-apply.exe";
    std::error_code Ec;
    fs::create_directories(ApplyDir, Ec);
    if (!CopyFileW(Self, Helper.c_str(), FALSE))
    {
        Message = std::format("cannot copy the update helper ({})", GetLastError());
        return false;
    }
    std::wstring Cmd = L"\"" + Helper.wstring() + L"\" apply-update \"" + P(StageDir).wstring() + L"\" --target \"" +
                       P(C.AppDir).wstring() + L"\" --wait-pid " + std::to_wstring(GetCurrentProcessId()) +
                       (Relaunch ? L" --relaunch" : L"");
    STARTUPINFOW Si{};
    Si.cb = sizeof(Si);
    PROCESS_INFORMATION Pi{};
    if (!CreateProcessW(nullptr, Cmd.data(), nullptr, nullptr, FALSE, DETACHED_PROCESS | CREATE_NO_WINDOW, nullptr,
                        ApplyDir.c_str(), &Si, &Pi))
    {
        Message = std::format("cannot start the update helper ({})", GetLastError());
        return false;
    }
    CloseHandle(Pi.hThread);
    CloseHandle(Pi.hProcess);
    Message = "update helper started: GrayWolf is replaced once this process exits";
    return true;
}

int ApplyStaged(const std::string& StageDir, const std::string& TargetDir, uint32_t WaitPid, bool Relaunch, std::string& Report)
{
    const fs::path Stage = P(StageDir), Target = P(TargetDir);
    std::error_code Ec;
    if (!fs::exists(Stage / "GrayWolf.exe", Ec) || !fs::exists(Stage / ".verified", Ec))
    {
        Report += "result=error\nreason=not a verified update folder\n";
        return 2;
    }
    if (WaitPid)
    {
        if (HANDLE Proc = OpenProcess(SYNCHRONIZE, FALSE, WaitPid))
        {
            const DWORD Wait = WaitForSingleObject(Proc, 120000);
            CloseHandle(Proc);
            if (Wait != WAIT_OBJECT_0)
            {
                Report += std::format("result=error\nreason=process {} did not exit\n", WaitPid);
                return 3;
            }
        }
    }

    // Top-level folders (zonekit\) are replaced whole, top-level files one by one; the user's data
    // (package_index\, settings, logs) is never in the archive's set and is never touched.
    const fs::path Backup = Target / ".update" / "backup";
    RemoveOwnDir(Backup);
    fs::create_directories(Backup, Ec);
    std::vector<fs::path> Entries;
    for (auto It = fs::directory_iterator(Stage, Ec); !Ec && It != fs::directory_iterator(); It.increment(Ec))
    {
        const std::wstring Name = It->path().filename().wstring();
        if (Name == L".verified" || _wcsicmp(Name.c_str(), L"package_index") == 0 || _wcsicmp(Name.c_str(), L"graywolf.json") == 0 ||
            _wcsicmp(Name.c_str(), L".update") == 0)
            continue;
        Entries.push_back(It->path().filename());
    }
    std::vector<fs::path> Aside, Placed;
    bool Ok = true;
    std::string Error;
    for (const auto& Name : Entries)
    {
        const fs::path Dst = Target / Name;
        if (fs::exists(Dst, Ec))
        {
            if (!MoveReplace(Dst, Backup / Name, 20)) { Error = std::format("cannot set aside {} ({})", U8(Name), GetLastError()); Ok = false; break; }
            Aside.push_back(Name);
        }
        Placed.push_back(Name);
        if (fs::is_directory(Stage / Name, Ec))
            fs::copy(Stage / Name, Dst, fs::copy_options::recursive | fs::copy_options::overwrite_existing, Ec);
        else
            fs::copy_file(Stage / Name, Dst, fs::copy_options::overwrite_existing, Ec);
        if (Ec) { Error = std::format("cannot copy {}: {}", U8(Name), Ec.message()); Ok = false; break; }
    }
    if (!Ok)
    {
        for (const auto& Name : Placed)
        {
            std::error_code E2;
            if (IsLink(Target / Name)) continue;
            fs::remove_all(Target / Name, E2);
        }
        for (const auto& Name : Aside) MoveReplace(Backup / Name, Target / Name, 20);
        RemoveOwnDir(Backup);
        Report += std::format("result=error\nreason={}\nrolled_back=yes\n", Error);
        return 4;
    }
    RemoveOwnDir(Backup);
    RemoveOwnDir(Stage);
    Report += std::format("result=ok\nreplaced={}\n", Entries.size());
    // the stage folder is stage-<version>
    const std::string StageName = U8((Stage.has_filename() ? Stage : Stage.parent_path()).filename());
    if (StageName.starts_with("stage-") && UpdateUninstallEntry(Target, StageName.substr(6))) Report += "uninstall_entry=updated\n";
    if (Relaunch)
    {
        const std::wstring Exe = (Target / "GrayWolf.exe").wstring();
        std::wstring Cmd = L"\"" + Exe + L"\"";
        STARTUPINFOW Si{};
        Si.cb = sizeof(Si);
        PROCESS_INFORMATION Pi{};
        if (CreateProcessW(nullptr, Cmd.data(), nullptr, nullptr, FALSE, 0, nullptr, Target.c_str(), &Si, &Pi))
        {
            CloseHandle(Pi.hThread);
            CloseHandle(Pi.hProcess);
            Report += "relaunched=yes\n";
        }
        else
            Report += "relaunched=no\n";
    }
    return 0;
}

void CleanupLeftovers(const std::string& AppDir)
{
    const fs::path Root = P(AppDir) / ".update";
    std::error_code Ec;
    if (!fs::exists(Root, Ec) || IsLink(Root)) return;
    RemoveOwnDir(Root / "apply");
    RemoveOwnDir(Root / "backup");
    // stages of a version this exe already is (or is past)
    const std::string Mine = AppVersion();
    for (auto It = fs::directory_iterator(Root, Ec); !Ec && It != fs::directory_iterator(); It.increment(Ec))
    {
        const std::string Name = U8(It->path().filename());
        if (Name.starts_with("stage-") && CompareVersions(Name.substr(6), Mine) <= 0) RemoveOwnDir(It->path());
    }
}

// ---------------------------------------------------------------------------------------------
// GUI background

namespace Background
{
namespace
{
    struct Shared
    {
        std::mutex Lock;
        View State;
        std::optional<Asset> PendingApp;
        std::string StagedDir;
    };
    // Never destroyed: a detached worker may still be running while the process exits.
    Shared& Data = *new Shared;
    std::mutex& Lock = Data.Lock;
    View& State = Data.State;
    std::optional<Asset>& PendingApp = Data.PendingApp;
    std::string& StagedDir = Data.StagedDir;
    std::atomic<bool> Running{ false };
    std::atomic<bool> PrivateLogged{ false };

    void Set(const std::function<void(View&)>& F)
    {
        std::lock_guard<std::mutex> G(Lock);
        F(State);
    }

    // One job at a time on a detached worker: the UI only reads the View.
    bool Launch(std::function<void()> Job)
    {
        bool Expected = false;
        if (!Running.compare_exchange_strong(Expected, true)) return false;
        Set([](View& V) { V.Busy = true; });
        std::thread([Job = std::move(Job)] {
            try { Job(); }
            catch (const std::exception& E) { AppLog::Log().warn("updates: {}", E.what()); }
            Set([](View& V) { V.Busy = false; });
            Running = false;
        }).detach();
        return true;
    }
}

void Start(bool Startup)
{
    if (Startup && (SettingsManager::GetSetting("auto_update", "true") != "true" || !Env("GRAYWOLF_NO_UPDATE").empty()))
    {
        AppLog::Log().info("updates: startup check off");
        return;
    }
    Launch([] {
        const Config C = FromSettings();
        CleanupLeftovers(C.AppDir);
        const CheckResult R = Check(C);
        std::string Line = R.Message;
        // a network failure is only logged; the private-repository case once per run
        const bool Private = R.Result == Status::Unavailable && R.Message.starts_with("updates unavailable (private");
        if (!Private || !PrivateLogged.exchange(true)) AppLog::Log().info("updates: {}", R.Message);

        if (R.PackagesNewer && R.Latest.Packages)
        {
            std::string Msg;
            const Status S = UpdatePackages(C, *R.Latest.Packages, Msg);
            AppLog::Log().info("updates: packages {}: {}", StatusName(S), Msg);
            Line = Msg;
        }
        Set([&](View& V) {
            V.Line = Line;
            if (R.AppNewer && R.Latest.App) V.AppVersion = R.Latest.App->Version;
        });
        std::lock_guard<std::mutex> G(Lock);
        if (R.AppNewer && R.Latest.App) PendingApp = R.Latest.App;
    });
}

void DownloadApp()
{
    Launch([] {
        std::optional<Asset> App;
        {
            std::lock_guard<std::mutex> G(Lock);
            App = PendingApp;
        }
        if (!App) return;
        const Config C = FromSettings();
        std::string Dir, Msg;
        const Status S = StageApp(C, *App, Dir, Msg);
        AppLog::Log().info("updates: app {}: {}", StatusName(S), Msg);
        std::lock_guard<std::mutex> G(Lock);
        if (S == Status::Updated) { StagedDir = Dir; State.AppStaged = true; State.Error.clear(); }
        else State.Error = Msg;
        State.Line = Msg;
    });
}

bool ApplyAndRestart(std::string& Message)
{
    std::string Dir;
    {
        std::lock_guard<std::mutex> G(Lock);
        Dir = StagedDir;
    }
    if (Dir.empty()) { Message = "no update downloaded"; return false; }
    const bool Ok = LaunchApply(FromSettings(), Dir, true, Message);
    AppLog::Log().info("updates: {}", Message);
    return Ok;
}

View Get()
{
    std::lock_guard<std::mutex> G(Lock);
    return State;
}
}
}
