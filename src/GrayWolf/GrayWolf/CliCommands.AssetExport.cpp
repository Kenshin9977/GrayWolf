// NOMINMAX before <windows.h>: std::max/std::min below would be mangled by its macros.
#define NOMINMAX

#include <stdio.h>
#include <map>
#include <filesystem>
#include <algorithm>
#include <vector>
#include <string>
#include <format>

#include "CliOptions.h"
#include "CliCommands.h"

#include <CLI/CLI.hpp>
#include "CliSupport.h"

#include "WraithX.h"
#include "CoDAssets.h"
#include "PackageFileMode.h"
#include "ICoDGame.h"
#include "ExportNaming.h"

#include "AssetIndex.h"
#include "AssetFilter.h"
#include "AssetPreview.h"
#include "ModelPreview.h"

// Shared tail of both modes: summarise the pool, optionally export it, report, clean up
static int RunExport(const CliOptions& Options, int64_t LoadMs)
{
    const auto Loaded = (CoDAssets::GameAssets != nullptr)
        ? CoDAssets::GameAssets->LoadedAssets.size()
        : (size_t)0;

    // Break the pool down per type so a caller can tell what it is actually measuring
    size_t Images = 0, Models = 0, Anims = 0, Sounds = 0, Materials = 0, RawFiles = 0, Others = 0;
    if (CoDAssets::GameAssets != nullptr)
    {
        for (auto& Asset : CoDAssets::GameAssets->LoadedAssets)
        {
            switch (Asset->AssetType)
            {
            case WraithAssetType::Image:     Images++;    break;
            case WraithAssetType::Model:     Models++;    break;
            case WraithAssetType::Animation: Anims++;     break;
            case WraithAssetType::Sound:     Sounds++;    break;
            case WraithAssetType::Material:  Materials++; break;
            case WraithAssetType::RawFile:   RawFiles++;  break;
            default:                         Others++;    break;
            }
        }
    }

    // Resolve --type and --limit into the exact set we are going to export
    WraithAssetType WantedType = WraithAssetType::Image;
    const bool FilterByType = !Options.Type.empty();

    if (FilterByType && !ParseAssetType(Options.Type, WantedType))
    {
        CliReport(Options, std::format("result=error\nreason=unknown --type {}\n", Options.Type.c_str()));
        CoDAssets::CleanUpGame();
        WraithX::ShutdownAPI();
        return 2;
    }

    const auto Needles = SplitNameFilter(Options.NameFilter);

    std::vector<uint32_t> Selection;
    const bool UseSelection = FilterByType || Options.Limit > 0 || !Needles.empty();

    if (UseSelection && CoDAssets::GameAssets != nullptr)
    {
        for (uint32_t Index = 0; Index < (uint32_t)Loaded; Index++)
        {
            if (FilterByType && CoDAssets::GameAssets->LoadedAssets[Index]->AssetType != WantedType)
            {
                continue;
            }

            if (!MatchesNameFilter(CoDAssets::GameAssets->LoadedAssets[Index]->AssetName, Needles))
            {
                continue;
            }

            Selection.push_back(Index);

            if (Options.Limit > 0 && Selection.size() >= Options.Limit)
            {
                break;
            }
        }
    }

    // The export loop skips placeholders but still counts them, so track them separately
    // rather than letting the caller read "exported" as "written"
    size_t Placeholders = 0;
    if (CoDAssets::GameAssets != nullptr)
    {
        if (UseSelection)
        {
            for (const auto Index : Selection)
            {
                if (CoDAssets::GameAssets->LoadedAssets[Index]->AssetStatus == WraithAssetStatus::Placeholder) Placeholders++;
            }
        }
        else
        {
            for (auto& Asset : CoDAssets::GameAssets->LoadedAssets)
            {
                if (Asset->AssetStatus == WraithAssetStatus::Placeholder) Placeholders++;
            }
        }
    }

    int64_t ExportMs = 0;
    uint32_t Exported = 0;
    size_t Failed = 0;
    size_t Requested = UseSelection ? Selection.size() : Loaded;

    if (Options.ExportAll && Requested > 0)
    {
        const auto ExportStart = CliClock::now();

        Exported = PackageFileMode::Export(UseSelection ? &Selection : nullptr, Options.Threads);

        ExportMs = CliElapsedMs(ExportStart, CliClock::now());

        // Anything the export loop could not write is flagged on the asset itself
        for (auto& Asset : CoDAssets::GameAssets->LoadedAssets)
        {
            if (Asset->AssetStatus == WraithAssetStatus::Error)
            {
                Failed++;
            }
        }
    }

    CliReport(Options, std::format(
        "result=ok\n"
        "source={}\n"
        "game={}\n"
        "assets_loaded={}\n"
        "load_ms={}\n"
        "images={}\nmodels={}\nanims={}\nsounds={}\nmaterials={}\nrawfiles={}\nothers={}\n"
        "requested={}\n"
        "placeholders={}\n"
        "exported={}\n"
        "failed={}\n"
        "export_ms={}\n"
        "threads={}\n"
        "export_path={}\n",
        Options.File.c_str(),
        CoDAssets::Game != nullptr ? CoDAssets::Game->Engine() : "none",
        (unsigned long long)Loaded,
        (long long)LoadMs,
        (unsigned long long)Images, (unsigned long long)Models, (unsigned long long)Anims,
        (unsigned long long)Sounds, (unsigned long long)Materials, (unsigned long long)RawFiles,
        (unsigned long long)Others,
        (unsigned long long)Requested,
        (unsigned long long)Placeholders,
        Exported,
        (unsigned long long)Failed,
        (long long)ExportMs,
        Options.Threads,
        PackageFileMode::LatestExportPath().c_str()));

    CoDAssets::CleanUpGame();
    WraithX::ShutdownAPI();
    return 0;
}


// Builds a searchable asset index from a game install's IWD packages - image headers
// (dimensions/format/alpha) and audio names.
int RunIndexAssets(const CliOptions& Options)
{
    AssetIndex Index;
    if (!Options.IndexAssets.empty()) Index.IndexIWDPackages(Options.IndexAssets);
    if (!Options.ModelsDir.empty())   Index.IndexModelDir(Options.ModelsDir);
    const std::string OutPath = Options.Out.empty() ? "assets_index.json" : Options.Out;
    const bool Ok = Index.Save(OutPath);

    size_t Images = 0, Audio = 0, Models = 0, WithAlpha = 0;
    for (const auto& E : Index.Entries)
    {
        if (E.Kind == AssetKind::Image) { Images++; if (E.Image.HasAlpha) WithAlpha++; }
        else if (E.Kind == AssetKind::Audio) Audio++;
        else if (E.Kind == AssetKind::Model) Models++;
    }

    CliReport(Options, std::format(
        "result={}\nindexed={}\nimages={}\nimages_with_alpha={}\naudio={}\nmodels={}\nout={}\n",
        Ok ? "ok" : "error", Index.Count(), Images, WithAlpha, Audio, Models, OutPath.c_str()));
    return Ok ? 0 : 5;
}


// Applies a saved filter to an asset index and lists the matches - the icon-finding query
// (e.g. "small image with transparency" for HUD elements).
int RunQueryAssets(const CliOptions& Options)
{
    AssetIndex Index;
    if (!Index.Load(Options.QueryAssets))
    {
        CliReport(Options, "result=error\nreason=could not load index\n");
        return 5;
    }

    AssetFilter Filter;
    if (!Options.FilterFile.empty() && !LoadFilter(Filter, Options.FilterFile))
    {
        CliReport(Options, "result=error\nreason=could not load filter\n");
        return 5;
    }

    // Optionally decode each matching image to a PNG (the "see the icons" half of the
    // find-then-view workflow). Needs the install (to reach the IWDs) and WIC.
    size_t Previews = 0;
    // Models render straight from their on-disk .semodel path, so previews only need a target
    // dir; images/audio additionally need --install to reach the IWDs (checked per-kind below).
    const bool DoPreview = !Options.PreviewDir.empty();
    const bool DoSheet = !Options.ContactSheet.empty() && !Options.Install.empty();
    if (DoPreview || DoSheet)
    {
        WraithX::InitializeAPI();
    }

    std::vector<const AssetEntry*> ImageMatches;
    std::string List;
    size_t Matched = 0;

    // Model parts are grouped by character base name and assembled into one preview per character:
    // strip _LOD<n>, then a trailing part token (_body/_head/_hat/...). Collected during the scan,
    // rendered in a pass afterwards.
    std::map<std::string, std::vector<std::string>> ModelGroups;

    for (const auto& E : Index.Entries)
    {
        if (!MatchesFilter(Filter, E)) continue;
        Matched++;
        if (E.Kind == AssetKind::Image) ImageMatches.push_back(&E);
        if (E.Kind == AssetKind::Image)
            List += std::format("{}\t{}\t{}x{}\t{}\t{}\n", E.Name.c_str(), E.Source.c_str(),
                E.Image.Width, E.Image.Height, AssetIndex::FormatName(E.Image.Format),
                E.Image.HasAlpha ? "alpha" : "opaque");
        else if (E.Kind == AssetKind::Model)
            List += std::format("{}\t{}\tmodel\tbones={} meshes={} mats={}\n",
                E.Name.c_str(), E.Source.c_str(),
                E.Model.Bones, E.Model.Meshes, E.Model.Materials);
        else
            List += std::format("{}\t{}\t{}\n", E.Name.c_str(), E.Source.c_str(),
                AssetIndex::KindName(E.Kind));

        if (DoPreview && (E.Kind == AssetKind::Image || E.Kind == AssetKind::Audio))
        {
            std::string Safe = E.Name;
            for (auto& C : Safe) if (C == '/' || C == '\\') C = '_';
            const bool Opus = (E.Kind == AssetKind::Audio && Options.AudioFormat == "opus");
            const char* Ext = (E.Kind != AssetKind::Audio) ? ".png" : (Opus ? ".opus" : ".wav");
            const std::string OutFile = (std::filesystem::path(Options.PreviewDir) / (Safe + Ext)).string();

            // Resume-friendly: an already-rendered preview is left untouched and still counted,
            // so a re-run continues where an interrupted one stopped instead of redoing everything.
            std::error_code ExEc;
            if (std::filesystem::exists(OutFile, ExEc)) { Previews++; continue; }

            bool Ok = (E.Kind == AssetKind::Image) ? RenderImageToPNG(E, Options.Install, OutFile)
                                                   : RenderAudioToFile(E, Options.Install, OutFile, Opus);

            // DirectX->OpenGL normal-map flip on request (for Blender and other DCC tools).
            if (Ok && E.Kind == AssetKind::Image && Options.NormalMap == "opengl" && IsNormalMapName(E.Name))
                FlipNormalMapGreen(OutFile);

            if (Ok) Previews++;
        }
        else if (DoPreview && E.Kind == AssetKind::Model && !E.Path.empty())
        {
            // Body-like parts seed the reference skeleton, so they go first in the group.
            const bool BodyLike = (E.Name.find("_body") != std::string::npos ||
                                   E.Name.find("_torso") != std::string::npos ||
                                   E.Name.find("_fullbody") != std::string::npos);
            auto& Grp = ModelGroups[ExportNaming::CharacterBaseName(E.Name)];
            if (BodyLike) Grp.insert(Grp.begin(), E.Path); else Grp.push_back(E.Path);
        }
    }

    // Assemble each character group into one textured preview (single-part groups render directly).
    for (const auto& Kv : ModelGroups)
    {
        std::string Safe = Kv.first;
        for (auto& C : Safe) if (C == '/' || C == '\\') C = '_';
        const std::string OutFile = (std::filesystem::path(Options.PreviewDir) / (Safe + ".png")).string();
        std::error_code ExEc;
        if (std::filesystem::exists(OutFile, ExEc)) { Previews++; continue; }
        const bool Ok = (Kv.second.size() == 1) ? RenderModelToPNG(Kv.second[0], OutFile, 512)
                                                : RenderModelsToPNG(Kv.second, OutFile, 512);
        if (Ok) Previews++;
    }

    if (!Options.Out.empty())
    {
        FILE* MatchFile = nullptr;
        if (fopen_s(&MatchFile, Options.Out.c_str(), "wb") == 0 && MatchFile != nullptr)
        {
            fwrite(List.data(), 1, List.size(), MatchFile);
            fclose(MatchFile);
        }
    }

    bool SheetOk = false;
    if (DoSheet && !ImageMatches.empty())
        SheetOk = BuildContactSheet(ImageMatches, Options.Install, Options.ContactSheet);

    CliReport(Options, std::format(
        "result=ok\nfilter={}\nmatched={}\ntotal={}\npreviews={}\ncontact_sheet={}\n{}",
        Filter.Name.empty() ? "(none)" : Filter.Name.c_str(),
        Matched, Index.Count(), Previews,
        !DoSheet ? "(none)" : (SheetOk ? Options.ContactSheet.c_str() : "failed"),
        Options.Out.empty() ? List.c_str() : ""));
    return 0;
}


// Loads a package file straight off disk and exports it (the `file` subcommand).
int RunLoadFileMode(const CliOptions& Options)
{

    // The package parsers throw rather than return on malformed or missing input
    auto LoadResult = LoadGameFileResult::UnknownError;
    const auto LoadStart = CliClock::now();
    try
    {
        LoadResult = PackageFileMode::Load(Options.File);
    }
    catch (const std::exception& Ex)
    {
        CliReport(Options, std::format(
            "result=error\nreason={}\nfile={}\n", Ex.what(), Options.File.c_str()));

        WraithX::ShutdownAPI();
        return 3;
    }
    catch (...)
    {
        CliReport(Options, std::format(
            "result=error\nreason=unhandled exception while loading\nfile={}\n", Options.File.c_str()));

        WraithX::ShutdownAPI();
        return 3;
    }
    const auto LoadEnd = CliClock::now();

    if (LoadResult != LoadGameFileResult::Success)
    {
        CliReport(Options, std::format(
            "result=error\nreason={}\nfile={}\n",
            (LoadResult == LoadGameFileResult::InvalidFile) ? "invalid file" : "unknown error",
            Options.File.c_str()));

        WraithX::ShutdownAPI();
        return 3;
    }

    return RunExport(Options, CliElapsedMs(LoadStart, LoadEnd));
}

// ---- the subcommands (CliCommands.h)

void AddAssetExportCommands(CliCommandBuilders& B)
{
    B["index-assets"] = [](CLI::App& App, CliOptions& Options) {
        auto* Sub = App.add_subcommand("index-assets", "Build a searchable asset index (assets_index.json)");
        Sub->add_option("iwd-dir", Options.IndexAssets, "IWD package directory to index");
        Sub->add_option("--models-dir", Options.ModelsDir, "Model directory to index");
    };
    B["query-assets"] = [](CLI::App& App, CliOptions& Options) {
        auto* Sub = App.add_subcommand("query-assets", "Apply a saved filter to an asset index and list the matches");
        Sub->add_option("query", Options.QueryAssets, "Asset index (.json) to query")->required();
        Sub->add_option("--filter", Options.FilterFile, "Saved filter file to apply");
        Sub->add_option("--contact-sheet", Options.ContactSheet, "Also render matches into one contact sheet image");
        Sub->add_option("--preview-dir", Options.PreviewDir, "Also render each match to a PNG/WAV in this directory");
    };
    B["file"] = [](CLI::App& App, CliOptions& Options) {
        auto* Sub = App.add_subcommand("file", "Load a package file straight off disk and export it");
        Sub->add_option("path", Options.File, "Package/fast file to load")->required()->check(CLI::ExistingFile);
    };
}
