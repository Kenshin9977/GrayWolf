#include "stdafx.h"

#include "SoundAliasCsv.h"

#include "ExportNaming.h"
#include "ModToolsTree.h"

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <format>
#include <fstream>
#include <limits>
#include <set>

namespace fs = std::filesystem;

namespace SoundAliasCsv
{
namespace
{
    constexpr const char* Names[] = {
        "Name", "Behavior", "Storage", "FileSpec", "FileSpecSustain", "FileSpecRelease", "Template", "Loadspec",
        "Secondary", "SustainAlias", "ReleaseAlias", "Bus", "VolumeGroup", "DuckGroup", "Duck", "ReverbSend",
        "CenterSend", "VolMin", "VolMax", "DistMin", "DistMaxDry", "DistMaxWet", "DryMinCurve", "DryMaxCurve",
        "WetMinCurve", "WetMaxCurve", "LimitCount", "LimitType", "EntityLimitCount", "EntityLimitType", "PitchMin", "PitchMax",
        "PriorityMin", "PriorityMax", "PriorityThresholdMin", "PriorityThresholdMax", "AmplitudePriority", "PanType", "Pan", "Futz",
        "Looping", "RandomizeType", "Probability", "StartDelay", "EnvelopMin", "EnvelopMax", "EnvelopPercent", "OcclusionLevel",
        "IsBig", "DistanceLpf", "FluxType", "FluxTime", "Subtitle", "Doppler", "ContextType", "ContextValue",
        "ContextType1", "ContextValue1", "ContextType2", "ContextValue2", "ContextType3", "ContextValue3", "Timescale", "IsMusic",
        "IsCinematic", "FadeIn", "FadeOut", "Pauseable", "StopOnEntDeath", "Compression", "StopOnPlay", "DopplerScale",
        "FutzPatch", "VoiceLimit", "IgnoreMaxDist", "NeverPlayTwice", "ContinuousPan", "FileSource", "FileSourceSustain", "FileSourceRelease",
        "FileTarget", "FileTargetSustain", "FileTargetRelease", "Platform", "Language", "OutputDevices", "PlatformMask", "WiiUMono",
        "StopAlias", "DistanceLpfMin", "DistanceLpfMax", "FacialAnimationName", "RestartContextLoops", "SilentInCPZ", "ContextFailsafe", "GPAD",
        "GPADOnly", "MuteVoice", "MuteMusic", "RowSourceFileName", "RowSourceShortName", "RowSourceLineNumber",
    };
    static_assert(std::size(Names) == Columns, "SoundAliasCsv: one name per column");

    double Db(double Linear)
    {
        const double D = 20.0 * std::log10(std::max(Linear, 0.0000152879));
        return D > -95.0 ? std::clamp(D + 100.0, 0.0, 100.0) : 0.0;
    }

    // the fewest decimals (0..4) whose value Back maps to the same stored value as Value did
    template <class F>
    std::string Shortest(double Value, F SameStored)
    {
        std::string Text;
        for (int Places = 0; Places <= 4; Places++)
        {
            Text = std::format("{:.{}f}", Value, Places);
            if (SameStored(std::stod(Text))) break;
        }
        if (Text == "-0") Text = "0";
        return Text;
    }

    // a cell in CSV: quoted when it holds a comma, a quote or a line break
    std::string Cell(const std::string& S)
    {
        if (S.find_first_of(",\"\r\n") == std::string::npos) return S;
        std::string Out = "\"";
        for (char Ch : S)
        {
            if (Ch == '"') Out += '"';
            Out += Ch;
        }
        return Out + "\"";
    }
}

const char* ColumnName(Col C)
{
    return Names[static_cast<size_t>(C)];
}

std::string Volume(double Linear)
{
    // the stored float, recomputed by the game's own float math, is within a float's precision
    const double V = Db(Linear);
    return Shortest(V, [&](double Back) { return std::fabs(Back - V) < 0.0001; });
}

std::string Volume16(uint16_t Value)
{
    const double Linear = Value / 65535.0;
    return Shortest(Db(Linear), [&](double Back) {
        return static_cast<uint16_t>(std::pow(10.0, (Back - 100.0) / 20.0) * 65535.0 + 0.5) == Value || (Value == 0 && Back == 0);
    });
}

std::string Cents(double Ratio, bool* Clamped)
{
    if (!(Ratio > 0)) return std::string();
    const double Raw = 1200.0 * std::log2(Ratio), V = std::clamp(Raw, -2400.0, 1200.0);
    if (Clamped) *Clamped = V != Raw;
    return Shortest(V, [&](double Back) { return std::fabs(Back - V) < 0.0001; });
}

std::string Distance(double Inches, bool* Clamped)
{
    if (!std::isfinite(Inches)) return std::string();
    const double V = std::clamp(std::round(Inches), 0.0, 131070.0);
    if (Clamped) *Clamped = V != std::round(Inches);
    return std::to_string((long long)V);
}

std::string CentsU16(uint16_t Value, double Scale)
{
    if (Value == 0) return std::string();
    const double C = 1200.0 * std::log2(Value / Scale);
    return Shortest(C, [&](double Back) { return static_cast<uint16_t>(std::pow(2.0, Back / 1200.0) * Scale + 0.5) == Value; });
}

std::string Fraction(uint8_t Value)
{
    return Shortest(Value / 255.0, [&](double Back) { return static_cast<uint8_t>(Back * 255.0 + 0.5) == Value; });
}

std::string Number(double Value)
{
    if (!std::isfinite(Value)) return std::string();
    std::string S = std::format("{:.4f}", Value);
    while (!S.empty() && S.back() == '0') S.pop_back();
    if (!S.empty() && S.back() == '.') S.pop_back();
    if (S == "-0") S = "0";
    return S;
}

std::string Text(const std::vector<Alias>& Aliases)
{
    std::string Out;
    for (size_t I = 0; I < Columns; I++) Out += (I ? "," : "") + std::string(Names[I]);
    Out += "\r\n";
    for (const Alias& A : Aliases)
    {
        for (size_t I = 0; I < Columns; I++)
        {
            if (I) Out += ',';
            Out += Cell(A.Cells[I]);
        }
        Out += "\r\n";
    }
    return Out;
}

std::string ZoneView::Chars(int64_t Pos, size_t N) const
{
    std::string S;
    for (size_t I = 0; I < N; I++)
    {
        const char Ch = At<char>(Pos + (int64_t)I);
        if (Ch == 0) break;
        S.push_back(Ch);
    }
    return S;
}

bool Wanted(const DiskGame::Context& C)
{
    return DiskGame::SoundAliases.Of(C.Settings) == "on";
}

void Write(const DiskGame::Context& C, DiskGame::Totals& T, const std::string& Zone, const Rows& R)
{
    for (const auto& [Name, N] : R.Counters) T.Counters[Name] += N;
    const fs::path Dir = C.KindDir(DiskGame::Kind::Sound);
    std::error_code Ec;
    const auto Held = [&](const std::string& Spec) {
        std::string Rel = Spec;
        std::replace(Rel.begin(), Rel.end(), '\\', '/');
        return !Rel.empty() && fs::is_regular_file(Dir / fs::path(Rel), Ec);
    };
    // a run limited to some assets (a dependency pulled from this zone, a pick): the rows playing a
    // file it wrote
    std::vector<Alias> Kept;
    if (C.Only)
        for (const Alias& A : R.Aliases)
            if (Held(A[Col::FileSpec])) Kept.push_back(A);
    const std::vector<Alias>& Rows = C.Only ? Kept : R.Aliases;
    if (Rows.empty()) return;
    const std::string Tree = ModToolsTree::RootOf(C);
    if (!Tree.empty())
    {
        // mod_tools: the CSV in the tree's aliases, its files in sound_assets/graywolf/<out folder>/,
        // the FileSpecs naming them there
        const std::string Folder = fs::path(C.OutRoot).filename().string();
        const std::string Prefix = "graywolf\\" + Folder + "\\";
        const fs::path Assets = fs::path(Tree) / "sound_assets" / "graywolf" / Folder;
        std::vector<Alias> There = Rows;
        for (Alias& A : There)
            for (const Col Spec : { Col::FileSpec, Col::FileSpecSustain, Col::FileSpecRelease })
            {
                std::string& Cell = A[Spec];
                if (Cell.empty()) continue;
                if (Held(Cell))
                {
                    std::string Rel = Cell;
                    std::replace(Rel.begin(), Rel.end(), '\\', '/');
                    ModToolsTree::Place(Dir / fs::path(Rel), Assets / fs::path(Rel));
                    if (ExportNaming::Lower(fs::path(Rel).extension().string()) != ".wav")
                        ModToolsTree::Warn("The sounds are " + fs::path(Rel).extension().string() +
                                           " files; the mod tools' sound assets are WAV: export them with the wav audio format");
                }
                Cell = Prefix + Cell;
            }
        const fs::path File = fs::path(Tree) / "share" / "raw" / "sound" / "aliases" / ("graywolf_" + ExportNaming::SafeFileName(Zone) + ".csv");
        if (!ModToolsTree::WriteKept(File, Text(There)))
        {
            T.Fail(Zone, DiskGame::Kind::Sound, File.filename().string(), "alias CSV not written in the mod tools tree (nor the one there set aside)");
            return;
        }
    }
    else
    {
        const fs::path File = Dir / (ExportNaming::SafeFileName(Zone) + ".csv");
        std::ofstream Out(File, std::ios::binary | std::ios::trunc);
        const std::string Csv = Text(Rows);
        Out.write(Csv.data(), (std::streamsize)Csv.size());
        if (!Out.good())
        {
            T.Fail(Zone, DiskGame::Kind::Sound, File.filename().string(), "alias CSV not written");
            return;
        }
    }
    std::set<std::string> Names;
    size_t Cells = 0, Files = 0, Elsewhere = 0;
    for (const Alias& A : Rows)
    {
        Names.insert(A[Col::Name]);
        for (const auto& S : A.Cells) Cells += !S.empty();
        if (A[Col::FileSpec].empty()) continue;
        if (Held(A[Col::FileSpec])) Files++;
        else Elsewhere++;
    }
    T.Counters["alias_csv_files"]++;
    T.Counters["alias_rows"] += Rows.size();
    T.Counters["alias_names"] += Names.size();
    T.Counters["alias_cells"] += Cells;
    T.Counters["alias_files"] += Files;
    T.Counters["alias_files_elsewhere"] += Elsewhere;
}
}
