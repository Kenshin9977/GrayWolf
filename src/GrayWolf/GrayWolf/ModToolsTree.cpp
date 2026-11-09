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

bool WriteKept(const fs::path& File, const std::string& Bytes)
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
        if (Aside.empty()) return false;
        fs::rename(File, Aside, Ec);
        if (Ec) return false;
        std::lock_guard<std::mutex> L(Lock);
        Run.Kept.emplace_back(Slashed(File), Slashed(Aside));
    }
    {
        std::ofstream Out(File, std::ios::binary | std::ios::trunc);
        Out.write(Bytes.data(), (std::streamsize)Bytes.size());
        if (!Out.good()) return false;
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
