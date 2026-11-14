#include "stdafx.h"

#include "BlenderLauncher.h"

#include "AppLog.h"
#include "SettingsManager.h"
#include "Strings.h"

#include <algorithm>
#include <cctype>
#include <filesystem>

#include <shlwapi.h>

#pragma comment(lib, "shlwapi.lib")

namespace fs = std::filesystem;

namespace
{
    // UTF-8, or the ANSI code page a path::string() gives (some of the app's folders come so)
    std::wstring W(const std::string& S)
    {
        try { return Strings::ToUnicodeString(S); }
        catch (const std::range_error&) { return fs::path(S).wstring(); }
    }
    // a path as UTF-8, the app's strings (not path::string(), which is the ANSI code page)
    std::string U(const fs::path& P) { return Strings::ToNormalString(P.wstring()); }

    bool IsFile(const fs::path& P)
    {
        std::error_code Ec;
        return !P.empty() && fs::is_regular_file(P, Ec);
    }

    std::string Lower(std::string S)
    {
        for (auto& C : S) C = (char)std::tolower((unsigned char)C);
        return S;
    }

    std::wstring Env(const wchar_t* Name)
    {
        wchar_t B[MAX_PATH * 2] = {};
        const DWORD N = GetEnvironmentVariableW(Name, B, (DWORD)std::size(B));
        return (N > 0 && N < std::size(B)) ? std::wstring(B, N) : std::wstring();
    }

    // "Blender 4.10" as {4, 10}: the numbers in a folder's name, compared as numbers
    std::vector<int> VersionOf(const std::wstring& Name)
    {
        std::vector<int> V;
        for (size_t I = 0; I < Name.size();)
        {
            if (!iswdigit(Name[I])) { I++; continue; }
            int N = 0;
            while (I < Name.size() && iswdigit(Name[I])) N = N * 10 + (Name[I++] - L'0');
            V.push_back(N);
        }
        return V;
    }

    // The newest Blender Foundation\Blender* holding blender.exe under one Program Files folder
    fs::path NewestUnder(const std::wstring& ProgramFiles)
    {
        if (ProgramFiles.empty()) return {};
        std::error_code Ec;
        fs::path Best;
        std::vector<int> BestVersion;
        for (fs::directory_iterator It(fs::path(ProgramFiles) / L"Blender Foundation", Ec), End; !Ec && It != End; It.increment(Ec))
        {
            const fs::path Exe = It->path() / L"blender.exe";
            if (!IsFile(Exe)) continue;
            const std::vector<int> V = VersionOf(It->path().filename().wstring());
            if (Best.empty() || V > BestVersion) { Best = Exe; BestVersion = V; }
        }
        return Best;
    }

    std::wstring RegString(HKEY Root, const wchar_t* Key, const wchar_t* Value)
    {
        wchar_t B[MAX_PATH * 2] = {};
        DWORD Size = sizeof(B);
        if (RegGetValueW(Root, Key, Value, RRF_RT_REG_SZ | RRF_RT_REG_EXPAND_SZ, nullptr, B, &Size) != ERROR_SUCCESS) return {};
        return B;
    }

    // blender-launcher.exe (what the installer associates .blend with) has blender.exe beside it
    fs::path BesideLauncher(const fs::path& P)
    {
        if (!IsFile(P)) return {};
        if (Lower(U(P.filename())) == "blender.exe") return P;
        const fs::path Beside = P.parent_path() / L"blender.exe";
        return IsFile(Beside) ? Beside : fs::path();
    }

    // A command-line argument for CreateProcess (CommandLineToArgvW's rules): quoted, its quotes
    // escaped, the backslashes before a quote doubled.
    std::wstring Quote(const std::wstring& Arg)
    {
        std::wstring Out = L"\"";
        size_t Slashes = 0;
        for (const wchar_t C : Arg)
        {
            if (C == L'\\') { Slashes++; continue; }
            Out.append(C == L'"' ? Slashes * 2 + 1 : Slashes, L'\\');
            Slashes = 0;
            Out += C;
        }
        Out.append(Slashes * 2, L'\\');
        return Out + L"\"";
    }

}

namespace Blender
{
    Exe Find() { return Find(SettingsManager::GetSetting("blender_path", "")); }

    Exe Find(const std::string& Set)
    {
        if (!Set.empty())
        {
            // a folder is taken as Blender's install folder
            fs::path P = fs::path(W(Set));
            std::error_code Ec;
            if (fs::is_directory(P, Ec)) P /= L"blender.exe";
            if (const fs::path Exe = BesideLauncher(P); !Exe.empty()) return { U(Exe), "setting" };
            AppLog::Log().warn("blender: the blender_path setting names no blender.exe: {}", Set);
        }
        if (const fs::path P = Env(L"BLENDER"); IsFile(P)) return { U(P), "BLENDER" };
        // 64-bit Program Files first (ProgramW6432 is it even from a 32-bit process), then x86
        for (const wchar_t* Var : { L"ProgramW6432", L"ProgramFiles", L"ProgramFiles(x86)" })
            if (const fs::path P = NewestUnder(Env(Var)); !P.empty()) return { U(P), "Program Files" };
        {
            std::vector<fs::path> Steam;
            if (const std::wstring S = RegString(HKEY_CURRENT_USER, L"Software\\Valve\\Steam", L"SteamPath"); !S.empty()) Steam.push_back(S);
            if (const std::wstring S = Env(L"ProgramFiles(x86)"); !S.empty()) Steam.push_back(fs::path(S) / L"Steam");
            for (const fs::path& S : Steam)
                if (const fs::path P = S / L"steamapps" / L"common" / L"Blender" / L"blender.exe"; IsFile(P))
                    return { U(P.lexically_normal()), "Steam" };
        }
        for (HKEY Root : { HKEY_CURRENT_USER, HKEY_LOCAL_MACHINE })
            if (const fs::path P = BesideLauncher(RegString(Root, L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\App Paths\\blender.exe", nullptr)); !P.empty())
                return { U(P), "App Paths" };
        {
            wchar_t B[MAX_PATH * 2] = {};
            DWORD N = (DWORD)std::size(B);
            if (SUCCEEDED(AssocQueryStringW(ASSOCF_NOTRUNCATE, ASSOCSTR_EXECUTABLE, L".blend", L"open", B, &N)))
                if (const fs::path P = BesideLauncher(B); !P.empty()) return { U(P), ".blend association" };
        }
        {
            wchar_t B[MAX_PATH * 2] = {};
            if (SearchPathW(nullptr, L"blender.exe", nullptr, (DWORD)std::size(B), B, nullptr) > 0 && IsFile(B))
                return { U(fs::path(B)), "PATH" };
        }
        return {};
    }

    bool Map::HasBlend() const { return IsFile(fs::path(W(Blend))); }
    bool Map::HasScript() const { return IsFile(fs::path(W(Script))); }

    namespace
    {
        Map Named(const fs::path& Dir, const std::wstring& Name)
        {
            Map M;
            M.Name = Strings::ToNormalString(Name);
            M.Glb = U((Dir / (Name + L".glb")).lexically_normal());
            M.Script = U((Dir / (Name + L"_import.py")).lexically_normal());
            M.Blend = U((Dir / (Name + L".blend")).lexically_normal());
            return M;
        }

        // <map>_import.py's map name; "" for any other file
        std::wstring ScriptMap(const fs::path& P)
        {
            const std::wstring Stem = P.stem().wstring();
            const std::wstring Suffix = L"_import";
            if (Lower(U(P.extension())) != ".py" || Stem.size() <= Suffix.size()) return {};
            if (_wcsicmp(Stem.c_str() + Stem.size() - Suffix.size(), Suffix.c_str()) != 0) return {};
            return Stem.substr(0, Stem.size() - Suffix.size());
        }
    }

    bool MapOf(const std::string& File, Map& Out, std::string& Error)
    {
        const fs::path P = fs::absolute(fs::path(W(File)));
        const std::string Ext = Lower(U(P.extension()));
        if (Ext == ".glb" || Ext == ".blend") Out = Named(P.parent_path(), P.stem().wstring());
        else if (const std::wstring Name = ScriptMap(P); !Name.empty()) Out = Named(P.parent_path(), Name);
        // a model's .cast or .gltf: opened the same way through the <model>_import.py beside it
        else if (Ext == ".cast" || Ext == ".gltf") Out = Named(P.parent_path(), P.stem().wstring());
        else
        {
            Error = "not a map or a model export: " + File + " (its .glb, .cast, its _import.py or its .blend)";
            return false;
        }
        return true;
    }

    std::vector<Map> MapsIn(const std::string& Folder)
    {
        std::vector<Map> Maps;
        const fs::path Root = fs::path(W(Folder));
        for (const fs::path& Dir : { Root, Root / L"maps" })
        {
            std::error_code Ec;
            for (fs::directory_iterator It(Dir, Ec), End; !Ec && It != End; It.increment(Ec))
                if (const std::wstring Name = ScriptMap(It->path()); !Name.empty() && It->is_regular_file(Ec))
                    Maps.push_back(Named(Dir, Name));
        }
        std::sort(Maps.begin(), Maps.end(), [](const Map& A, const Map& B) { return _stricmp(A.Name.c_str(), B.Name.c_str()) < 0; });
        return Maps;
    }

    bool Open(const Map& M, const LaunchOptions& O, std::string& Did, std::string& Error)
    {
        const Exe B = O.Use.Path.empty() ? Find() : O.Use;
        if (B.Path.empty())
        {
            Error = "Blender not found: install it, or set its path in Settings > Blender (or the BLENDER variable)";
            return false;
        }
        const bool Import = O.Reimport || !M.HasBlend();
        if (!Import && O.Background)
        {
            Did = "already saved: " + M.Blend;
            return true;
        }
        if (Import && !M.HasScript())
        {
            Error = "no import script beside it: " + M.Script + " (export it again: GrayWolf writes <name>_import.py next to a map's .glb, "
                    "and next to a Cast or glTF model with the Blender script format on)";
            return false;
        }

        std::wstring Cmd = Quote(W(B.Path));
        if (!Import)
            Cmd += L" " + Quote(W(M.Blend));
        else
        {
            // the script run as --python runs it, then the .blend saved, only when it raised nothing
            // (a failed import is not saved as the map's .blend); the paths ride after "--"
            const wchar_t* Expr =
                L"import bpy, sys; a = sys.argv[sys.argv.index('--') + 1:]; "
                L"exec(compile(open(a[0], encoding='utf-8').read(), a[0], 'exec'), {'__file__': a[0], '__name__': '__main__'}); "
                L"bpy.ops.wm.save_as_mainfile(filepath=a[1])";
            if (O.Background) Cmd += L" --background --python-exit-code 1";
            Cmd += L" --python-expr " + Quote(Expr) + L" -- " + Quote(W(M.Script)) + L" " + Quote(W(M.Blend));
            if (!O.Props.empty()) Cmd += L" --props " + Quote(W(O.Props));
        }

        STARTUPINFOW Si{};
        Si.cb = sizeof(Si);
        PROCESS_INFORMATION Pi{};
        // Blender's own console (it hides it: Window > Toggle System Console shows the import's
        // summary line and errors); in the background, its output goes to a log in %TEMP%
        const DWORD Flags = (O.Background ? CREATE_NO_WINDOW : CREATE_NEW_CONSOLE) | CREATE_UNICODE_ENVIRONMENT;
        const std::wstring Dir = fs::path(W(M.Glb)).parent_path().wstring();
        const fs::path LogPath = fs::temp_directory_path() / (L"graywolf_blender_" + W(M.Name) + L".log");
        HANDLE Log = INVALID_HANDLE_VALUE;
        if (O.Background)
        {
            SECURITY_ATTRIBUTES Sa{ sizeof(Sa), nullptr, TRUE };
            Log = CreateFileW(LogPath.c_str(), GENERIC_WRITE, FILE_SHARE_READ, &Sa, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
            if (Log != INVALID_HANDLE_VALUE)
            {
                Si.dwFlags = STARTF_USESTDHANDLES;
                Si.hStdOutput = Si.hStdError = Log;
                Si.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
            }
        }
        const bool Started = CreateProcessW(nullptr, Cmd.data(), nullptr, nullptr, Log != INVALID_HANDLE_VALUE, Flags, nullptr,
                                            Dir.c_str(), &Si, &Pi) != FALSE;
        const DWORD StartError = GetLastError();
        if (Log != INVALID_HANDLE_VALUE) CloseHandle(Log);
        if (!Started)
        {
            Error = "Blender did not start (" + B.Path + "): error " + std::to_string(StartError);
            return false;
        }
        AppLog::Log().info("blender: {} {} with {} (from {}), pid {}", Import ? "importing" : "opening",
                           Import ? M.Script : M.Blend, B.Path, B.From, Pi.dwProcessId);
        CloseHandle(Pi.hThread);
        if (O.Background)
        {
            WaitForSingleObject(Pi.hProcess, INFINITE);
            DWORD Code = 1;
            GetExitCodeProcess(Pi.hProcess, &Code);
            CloseHandle(Pi.hProcess);
            if (Code != 0 || !M.HasBlend())
            {
                Error = "Blender's import of " + M.Name + " failed (exit code " + std::to_string(Code) + "): see " + U(LogPath);
                return false;
            }
            Did = "imported and saved " + M.Blend;
            return true;
        }
        CloseHandle(Pi.hProcess);
        Did = Import ? "importing " + M.Name + " in Blender (saved as " + U(fs::path(W(M.Blend)).filename()) + ")"
                     : "opened " + U(fs::path(W(M.Blend)).filename()) + " in Blender";
        return true;
    }
}
