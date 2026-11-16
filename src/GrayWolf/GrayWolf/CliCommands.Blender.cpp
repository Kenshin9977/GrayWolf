// open-map <map, model or folder> [--reimport] [--background] [--props fast|objects]: a map export (or a model's, through the
// <model>_import.py beside its .cast or .gltf) opened in Blender
// (BlenderLauncher.h), the command line's face of the GUI's "Open in Blender".

#include "stdafx.h"

#include "BlenderLauncher.h"
#include "CliCommands.h"
#include "MapRadiantPlanes.h"

#include <fstream>
#include <iterator>

#include <CLI/CLI.hpp>
#include "CliSupport.h"

#include <filesystem>

int RunOpenMap(const CliOptions& Options)
{
    namespace fs = std::filesystem;
    std::string Error;
    Blender::Map M;
    std::error_code Ec;
    bool Found = false;
    if (fs::is_directory(fs::path(Options.OpenMap), Ec))   // argv: the ANSI code page
    {
        const std::vector<Blender::Map> Maps = Blender::MapsIn(Options.OpenMap);
        if (Maps.size() == 1) { M = Maps[0]; Found = true; }
        else if (Maps.empty()) Error = "no map in " + Options.OpenMap + " (no *_import.py there or in maps\\)";
        else
        {
            Error = std::to_string(Maps.size()) + " maps in " + Options.OpenMap + ", pass one's .glb:";
            for (const auto& Each : Maps) Error += "\n  " + Each.Glb;
        }
    }
    else Found = Blender::MapOf(Options.OpenMap, M, Error);

    std::string Did;
    Blender::LaunchOptions O;
    // a props choice is an import's: it re-imports a map already saved
    O.Reimport = Options.OpenMapReimport || !Options.OpenMapProps.empty();
    O.Background = Options.OpenMapBackground;
    O.Props = Options.OpenMapProps;
    const bool Ok = Found && Blender::Open(M, O, Did, Error);
    const Blender::Exe B = Blender::Find();
    CliReport(Options, Ok ? "result=ok\ndid=" + Did + "\nblender=" + B.Path + " (" + B.From + ")\n"
                          : "result=error\nreason=" + Error + "\n");
    // dropped on the exe from Explorer: no console to read the error in
    if (!Ok && GetConsoleWindow() == nullptr && Options.Report.empty())
        MessageBoxA(nullptr, Error.c_str(), "GrayWolf: open-map", MB_OK | MB_ICONEXCLAMATION);
    return Ok ? 0 : 1;
}

// map-limits <file.map>: what Black Ops III's cod2map makes of a Radiant .map's brushes
// (MapRadiantPlanes.h), its plane slots against MAX_MAP_PLANES_COMPILE, before a build
int RunMapLimits(const CliOptions& Options)
{
    std::ifstream In(std::filesystem::path(Options.MapLimits), std::ios::binary);
    if (!In)
    {
        CliReport(Options, "result=error\nreason=cannot read " + Options.MapLimits + "\n");
        return 1;
    }
    const std::string Text((std::istreambuf_iterator<char>(In)), std::istreambuf_iterator<char>());
    const Maps::PlaneCount C = Maps::CountPlanes(Text);
    CliReport(Options, "result=ok\nbrushes=" + std::to_string(C.Brushes) + "\nsides=" + std::to_string(C.Sides) +
                           "\nbox_bevels=" + std::to_string(C.BoxBevels) + "\nedge_bevels=" + std::to_string(C.EdgeBevels) +
                           "\nplane_slots=" + std::to_string(C.Slots) + "\nplane_slots_limit=" + std::to_string(0xa0000) +
                           "\nside_planes=" + std::to_string(C.SidePlanes) + "\ncollision_brush_sides=" + std::to_string(C.CollisionSides) + "\n");
    return 0;
}

// ---- the subcommands (CliCommands.h)

void AddBlenderCommands(CliCommandBuilders& B)
{
    B["open-map"] = [](CLI::App& App, CliOptions& Options) {
        auto* Sub = App.add_subcommand("open-map", "Open a map export (or a Cast or glTF model's) in Blender: the first time its "
                                                   "import script runs and <name>.blend is saved beside it, later that .blend opens");
        Sub->add_option("map", Options.OpenMap, "The map's .glb, a model's .cast or .gltf, its _import.py, its .blend, or a folder holding one map")
            ->required()->check(CLI::ExistingPath);
        Sub->add_flag("--reimport", Options.OpenMapReimport, "Run the import script again, even when the .blend exists");
        Sub->add_flag("--background", Options.OpenMapBackground, "Import and save the .blend without opening Blender's window");
        Sub->add_option("--props", Options.OpenMapProps,
                        "A map's props: fast (map cells, Object > GrayWolf: Make selection editable for a region; the default) or "
                        "objects (one object per prop, slow on a big map); re-imports the map")
            ->check(CLI::IsMember({ "fast", "objects" }));
    };
    B["map-limits"] = [](CLI::App& App, CliOptions& Options) {
        auto* Sub = App.add_subcommand("map-limits", "Count what Black Ops III's cod2map makes of a Radiant .map's brushes: plane slots "
                                                     "(MAX_MAP_PLANES_COMPILE, 655360), bevels, brush sides (an estimate, a little high)");
        Sub->add_option("file", Options.MapLimits, "The .map")->required()->check(CLI::ExistingFile);
    };
}
