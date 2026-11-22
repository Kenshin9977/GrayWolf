#include "stdafx.h"

#include "ModToolsTree.h"

#include "DiskGame.h"
#include "ExportNaming.h"
#include "GameInstalls.h"
#include "SettingsManager.h"

#include <fstream>
#include <iterator>
#include <mutex>

namespace fs = std::filesystem;

namespace ModToolsTree
{
namespace
{
    std::mutex Lock;
    Record Run;   // under Lock

    std::string Slashed(const fs::path& P)
    {
        std::string S = P.string();
        for (char& C : S)
            if (C == '/') C = '\\';
        return S;
    }

    bool SameBytes(const fs::path& File, const std::string& Bytes)
    {
        std::ifstream In(File, std::ios::binary);
        if (!In) return false;
        const std::string Was((std::istreambuf_iterator<char>(In)), std::istreambuf_iterator<char>());
        return Was == Bytes;
    }

    void Opened(const fs::path& File)
    {
        std::lock_guard<std::mutex> L(Lock);
        const std::string S = Slashed(File);
        for (const auto& F : Run.Files)
            if (F == S) return;
        Run.Files.push_back(S);
    }
}

bool IsRoot(const fs::path& Folder, std::string* Missing)
{
    std::error_code Ec;
    for (const char* F : { "bin/asseteditor_modtools.exe", "bin/linker_modtools.exe" })
        if (!fs::is_regular_file(Folder / F, Ec))
        {
            if (Missing) *Missing = Slashed(fs::path(F));
            return false;
        }
    for (const char* D : { "source_data", "model_export", "map_source" })
        if (!fs::is_directory(Folder / D, Ec))
        {
            if (Missing) *Missing = std::string(D) + "\\";
            return false;
        }
    return true;
}

std::string Detect()
{
    for (const auto& I : GameInstalls::Detect())
        if (I.DiskId == "bo3" && IsRoot(I.Path)) return I.Path;
    return std::string();
}

std::string Resolve(const std::string& Given, std::string& Why)
{
    std::string Root = Given;
    const char* From = "--mod-tools-root";
    if (Root.empty())
    {
        Root = SettingsManager::GetSetting("bo3_mod_tools_root", "");
        From = "bo3_mod_tools_root (graywolf.json)";
    }
    if (Root.empty())
    {
        Root = Detect();
        if (Root.empty())
        {
            Why = "no Black Ops III install with the Mod Tools found (Steam: install \"Call of Duty: Black Ops III - Mod Tools\"; "
                  "or name its folder with --mod-tools-root or bo3_mod_tools_root in graywolf.json)";
            return std::string();
        }
        return Root;
    }
    std::string Missing;
    if (!IsRoot(Root, &Missing))
    {
        Why = std::string(From) + " " + Root + " is not a Black Ops III Mod Tools folder (no " + Missing + ")";
        return std::string();
    }
    return Root;
}

std::string RootOf(const DiskGame::Context& C)
{
    if (DiskGame::ModToolsOutput.Of(C.Settings) != "on") return std::string();
    return C.Setting("mod_tools_root");
}

namespace
{
    // Windows' error as a reason a user can act on (the mod tools' programs keep their files open)
    std::string Reason(DWORD Code)
    {
        switch (Code)
        {
        case ERROR_SHARING_VIOLATION:
        case ERROR_LOCK_VIOLATION:
            return "the file is open in another program (Radiant, APE or the Mod Tools launcher): close it and export again";
        case ERROR_ACCESS_DENIED:
            return "access denied (a read-only file or folder, or Windows Security's Controlled folder access blocking GrayWolf)";
        case ERROR_DISK_FULL:
        case ERROR_HANDLE_DISK_FULL:
            return "the disk is full";
        case ERROR_PATH_NOT_FOUND:
            return "its folder could not be made";
        default:
            break;
        }
        return std::system_category().message((int)Code) + " (Windows error " + std::to_string(Code) + ")";
    }
    bool Fail(std::string* Why, const std::string& Step, DWORD Code)
    {
        if (Why) *Why = Step + ": " + Reason(Code);
        return false;
    }
}

bool WriteKept(const fs::path& File, const std::string& Bytes, std::string* Why)
{
    std::error_code Ec;
    fs::create_directories(File.parent_path(), Ec);
    if (fs::exists(File, Ec))
    {
        if (SameBytes(File, Bytes))
        {
            Opened(File);
            return true;
        }
        fs::path Aside;
        for (int N = 1; N < 1000; N++)
        {
            fs::path Candidate = File;
            Candidate += N == 1 ? std::string(".bak") : ".bak" + std::to_string(N);
            if (!fs::exists(Candidate, Ec))
            {
                Aside = Candidate;
                break;
            }
        }
        if (Aside.empty())
        {
            if (Why) *Why = "setting the earlier file aside: .bak to .bak999 are all taken";
            return false;
        }
        if (!MoveFileExW(File.c_str(), Aside.c_str(), 0)) return Fail(Why, "setting the earlier file aside", GetLastError());
        std::lock_guard<std::mutex> L(Lock);
        Run.Kept.emplace_back(Slashed(File), Slashed(Aside));
    }
    {
        const HANDLE H = CreateFileW(File.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (H == INVALID_HANDLE_VALUE) return Fail(Why, "making the file", GetLastError());
        size_t Done = 0;
        DWORD Code = 0;
        while (Done < Bytes.size())
        {
            const DWORD Chunk = (DWORD)std::min<size_t>(Bytes.size() - Done, 1u << 30);
            DWORD Wrote = 0;
            if (!WriteFile(H, Bytes.data() + Done, Chunk, &Wrote, nullptr) || Wrote == 0)
            {
                Code = GetLastError();
                break;
            }
            Done += Wrote;
        }
        CloseHandle(H);
        if (Done < Bytes.size()) return Fail(Why, "writing the file", Code);
    }
    Opened(File);
    return true;
}

bool Place(const fs::path& From, const fs::path& To)
{
    std::error_code Ec;
    fs::create_directories(To.parent_path(), Ec);
    fs::remove(To, Ec);
    Ec.clear();
    fs::create_hard_link(From, To, Ec);
    if (Ec)
    {
        Ec.clear();
        fs::copy_file(From, To, fs::copy_options::overwrite_existing, Ec);
    }
    std::lock_guard<std::mutex> L(Lock);
    (Ec ? Run.NotPlaced : Run.Placed)++;
    return !Ec;
}

std::unordered_map<std::string, std::string> DefinedTypes(const fs::path& Root, const fs::path& Except, bool StockOnly)
{
    std::unordered_map<std::string, std::string> Names;
    std::error_code Ec;
    for (const char* Sub : { "source_data", "texture_assets", "model_export" })
        for (fs::recursive_directory_iterator It(Root / Sub, Ec), End; !Ec && It != End; It.increment(Ec))
        {
            const fs::path& P = It->path();
            const std::string File = ExportNaming::Lower(P.filename().string());
            // GrayWolf's own (model_export/graywolf/, graywolf_*.gdt): no stock asset
            if (StockOnly && File == "graywolf" && It->is_directory(Ec))
            {
                It.disable_recursion_pending();
                continue;
            }
            if (ExportNaming::Lower(P.extension().string()) != ".gdt" || !It->is_regular_file(Ec)) continue;
            if (StockOnly && File.rfind("graywolf_", 0) == 0) continue;
            if (std::error_code Eq; fs::equivalent(P, Except, Eq)) continue;
            std::ifstream In(P, std::ios::binary);
            // an entry's head: "name" ( "type.gdf" ), or "name" [ "parent" ]; a key is "key" "value"
            for (std::string Line; std::getline(In, Line);)
            {
                size_t I = Line.find_first_not_of(" \t");
                if (I == std::string::npos || Line[I] != '"') continue;
                const size_t Close = Line.find('"', I + 1);
                if (Close == std::string::npos) continue;
                const size_t J = Line.find_first_not_of(" \t", Close + 1);
                if (J == std::string::npos || (Line[J] != '(' && Line[J] != '[')) continue;
                // its GDF, "" for an entry derived from another ([ "parent" ])
                std::string Type;
                if (const size_t A = Line.find('"', J), Z = A == std::string::npos ? A : Line.find('"', A + 1); Line[J] == '(' && Z != std::string::npos)
                    Type = ExportNaming::Lower(Line.substr(A + 1, Z - A - 1));
                Names.emplace(ExportNaming::Lower(Line.substr(I + 1, Close - I - 1)), std::move(Type));
            }
        }
    return Names;
}

std::unordered_set<std::string> DefinedAssets(const fs::path& Root, const fs::path& Except)
{
    std::unordered_set<std::string> Names;
    for (const auto& [Name, Type] : DefinedTypes(Root, Except, false)) Names.insert(Name);
    return Names;
}

size_t LeaveOutDefined(const fs::path& Gdt, std::vector<std::string>& Entries)
{
    const auto Defined = DefinedAssets(Gdt.parent_path().parent_path(), Gdt);
    const size_t Was = Entries.size();
    std::erase_if(Entries, [&](const std::string& E) {
        const size_t A = E.find('"'), Z = A == std::string::npos ? A : E.find('"', A + 1);
        if (Z == std::string::npos) return false;
        std::string Name = E.substr(A + 1, Z - A - 1);
        for (char& C : Name) C = (char)std::tolower((unsigned char)C);
        return Defined.count(Name) != 0;
    });
    const size_t Left = Was - Entries.size();
    if (Left) Warn(Gdt.filename().string() + ": " + std::to_string(Left) + " entries left out, other GDTs of the mod tools define them");
    return Left;
}

void Begin(const std::string& Root)
{
    std::lock_guard<std::mutex> L(Lock);
    Run = Record();
    Run.Root = Root;
}

void Warn(const std::string& Text)
{
    std::lock_guard<std::mutex> L(Lock);
    for (const auto& W : Run.Warnings)
        if (W == Text) return;
    Run.Warnings.push_back(Text);
}

Record Take()
{
    std::lock_guard<std::mutex> L(Lock);
    Record R = std::move(Run);
    Run = Record();
    return R;
}

std::vector<std::string> NextSteps(const Record& R)
{
    std::vector<std::string> Out;
    if (R.Root.empty()) return Out;
    const fs::path Root(R.Root);
    const auto Below = [&](const std::string& File) {
        const fs::path Rel = fs::path(File).lexically_relative(Root);
        return Rel.empty() ? File : Slashed(Rel);
    };
    std::vector<std::string> Gdts, Maps, Csvs;
    for (const auto& F : R.Files)
    {
        const std::string Ext = ExportNaming::Lower(fs::path(F).extension().string());
        (Ext == ".gdt" ? Gdts : Ext == ".map" ? Maps : Csvs).push_back(Below(F));
    }
    const auto List = [](const std::vector<std::string>& Files) {
        std::string S;
        for (const auto& F : Files) S += (S.empty() ? "" : ", ") + F;
        return S;
    };
    if (!Gdts.empty())
        Out.push_back("Open APE (Asset Property Editor: the Mod Tools launcher, or bin\\asseteditor_modtools.exe): it lists " + List(Gdts) +
                      " once GdtDB has read source_data (APE and the launcher update it as they start; from a command line: "
                      "gdtdb\\gdtdb.exe /update). APE shows their entries; the launcher's Link (bin\\linker_modtools.exe) "
                      "converts the xmodels, materials and images a map or mod uses when it builds it.");
    if (!Maps.empty())
        Out.push_back("Open " + List(Maps) + " in Radiant (the Mod Tools launcher, or bin\\Radiant_modtools.exe); to build it, save it "
                      "as the .map of a map made by the launcher (map_source\\zm\\ or map_source\\mp\\).");
    if (!Csvs.empty())
        Out.push_back("Sound aliases " + List(Csvs) + ": add each as an ALIAS source to your map's or mod's sound zone config "
                      "(sound\\zoneconfig\\<name>.szc); their files are below sound_assets\\graywolf\\.");
    if (!R.Kept.empty())
        Out.push_back(std::to_string(R.Kept.size()) + (R.Kept.size() == 1 ? " earlier file was" : " earlier files were") +
                      " set aside as .bak, not overwritten (see the report)");
    for (const auto& W : R.Warnings) Out.push_back(W);
    return Out;
}
}
