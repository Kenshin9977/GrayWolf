// NOMINMAX before <windows.h>: std::max/std::min below would be mangled by its macros.
#define NOMINMAX

#include <windows.h>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <fstream>
#include <string>
#include <vector>

#include "CliSupport.h"
#include "SettingsManager.h"
#include "Strings.h"
#include "CliOptions.h"

// Writes the report to the parent console when there is one, and to --report when asked
void CliReport(const CliOptions& Options, const std::string& Report)
{
    if (AttachConsole(ATTACH_PARENT_PROCESS))
    {
        FILE* Out = nullptr;
        if (freopen_s(&Out, "CONOUT$", "w", stdout) == 0 && Out != nullptr)
        {
            fputs(Report.c_str(), stdout);
            fflush(stdout);
        }
    }

    if (!Options.Report.empty())
    {
        std::ofstream Writer(Options.Report);
        if (Writer)
        {
            Writer << Report;
        }
    }
}


// Applies the per-run export CLI flags over the settings the exporters read
// (exportimg/exportsnd/export_<fmt>/exportalllods/unit), for this run only: graywolf.json keeps
// the user's choices (SettingsManager::SetRunSetting).
namespace
{
    struct FormatKey { const char* Name; const char* Key; };

    // A --model-format/--anim-format list ("cast", "semodel,cast", "none"): every format of the
    // family off, then the named ones on, so the run writes exactly those whatever graywolf.json says
    template <size_t N>
    void ApplyFormatList(const std::string& List, const FormatKey (&Formats)[N])
    {
        if (List.empty()) return;
        for (const auto& F : Formats) SettingsManager::SetRunSetting(F.Key, "false");
        for (const auto& Name : SplitNameFilter(List))   // comma separated, lowered
            for (const auto& F : Formats)
                if (Name == F.Name) SettingsManager::SetRunSetting(F.Key, "true");
    }

    const FormatKey ModelFormats[] = {
        { "cast", "export_castmdl" }, { "castmdl", "export_castmdl" }, { "semodel", "export_semodel" },
        { "obj", "export_obj" }, { "ma", "export_ma" }, { "maya", "export_ma" }, { "xna", "export_xna" },
        { "xnalara", "export_xna" }, { "xmodel_export", "export_xmexport" }, { "xmexport", "export_xmexport" },
        { "xmodel_bin", "export_xmbin" }, { "xmbin", "export_xmbin" }, { "smd", "export_smd" },
        { "gltf", "export_gltf" }, { "glb", "export_glb" } };
    const FormatKey AnimFormats[] = {
        { "cast", "export_castanim" }, { "castanim", "export_castanim" }, { "seanim", "export_seanim" },
        { "xanim_export", "export_directxanim" }, { "directxanim", "export_directxanim" } };
}

void ApplyExportOverrides(const CliOptions& Options)
{
    ApplyFormatList(Options.ModelFormat, ModelFormats);
    ApplyFormatList(Options.AnimFormat, AnimFormats);

    if (!Options.ImageFormat.empty()) { const std::string V = Strings::ToUpper(Options.ImageFormat); SettingsManager::SetRunSetting("exportimg", V); }
    if (!Options.AudioFormat.empty()) { const std::string V = Strings::ToUpper(Options.AudioFormat); SettingsManager::SetRunSetting("exportsnd", V); }
    if (Options.Lods)                 SettingsManager::SetRunSetting("exportalllods", "true");
    if (Options.KeepImages)           SettingsManager::SetRunSetting("keep_previous_images", "true");
    if (!Options.Unit.empty())        SettingsManager::SetRunSetting("unit", Options.Unit);
}


// A comma-separated list of case-insensitive substrings, lowered once so matching is cheap.
// Matching ANY of them, not all: a game's 2D interface art is spread over several unrelated
// prefixes (ui_, hud_, menu_, compass, minimap, icon) that aren't a distinct asset type, only a
// naming convention - selecting it needs a set of names, not one.
std::vector<std::string> SplitNameFilter(const std::string& Filter)
{
    std::vector<std::string> Needles;
    std::string Lower = Filter;
    std::transform(Lower.begin(), Lower.end(), Lower.begin(),
                   [](unsigned char C) { return (char)tolower(C); });

    size_t Start = 0;
    while (Start <= Lower.size() && !Lower.empty())
    {
        const size_t Comma = Lower.find(',', Start);
        auto Piece = Lower.substr(Start, Comma == std::string::npos ? std::string::npos : Comma - Start);
        if (!Piece.empty()) Needles.push_back(Piece);
        if (Comma == std::string::npos) break;
        Start = Comma + 1;
    }
    return Needles;
}


bool MatchesNameFilter(const std::string& Name, const std::vector<std::string>& Needles)
{
    if (Needles.empty()) return true;

    std::string Lower = Name;
    std::transform(Lower.begin(), Lower.end(), Lower.begin(),
                   [](unsigned char C) { return (char)tolower(C); });

    for (const auto& N : Needles)
        if (Lower.find(N) != std::string::npos) return true;
    return false;
}


// Maps a --type value onto an asset type, so a run can measure one kind at a time.
bool ParseAssetType(const std::string& Name, WraithAssetType& TypeOut)
{
    if (Name == "image")    { TypeOut = WraithAssetType::Image;     return true; }
    if (Name == "model")    { TypeOut = WraithAssetType::Model;     return true; }
    if (Name == "anim")     { TypeOut = WraithAssetType::Animation; return true; }
    if (Name == "sound")    { TypeOut = WraithAssetType::Sound;     return true; }
    if (Name == "material") { TypeOut = WraithAssetType::Material;  return true; }
    if (Name == "rawfile")  { TypeOut = WraithAssetType::RawFile;   return true; }
    if (Name == "effect")   { TypeOut = WraithAssetType::Effect;    return true; }
    return false;
}


// How long loading took, for the CLI's reports.
using CliClock = std::chrono::steady_clock;

int64_t CliElapsedMs(CliClock::time_point A, CliClock::time_point B)
{
    return std::chrono::duration_cast<std::chrono::milliseconds>(B - A).count();
}

