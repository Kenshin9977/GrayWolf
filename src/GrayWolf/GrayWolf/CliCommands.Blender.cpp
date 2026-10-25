// open-map <map or folder> [--reimport] [--background]: a map export opened in Blender
// (BlenderLauncher.h), the command line's face of the GUI's "Open in Blender".

#include "stdafx.h"

#include "BlenderLauncher.h"
#include "CliCommands.h"

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
    const bool Ok = Found && Blender::Open(M, { Options.OpenMapReimport, Options.OpenMapBackground }, Did, Error);
    const Blender::Exe B = Blender::Find();
    CliReport(Options, Ok ? "result=ok\ndid=" + Did + "\nblender=" + B.Path + " (" + B.From + ")\n"
                          : "result=error\nreason=" + Error + "\n");
    // dropped on the exe from Explorer: no console to read the error in
    if (!Ok && GetConsoleWindow() == nullptr && Options.Report.empty())
        MessageBoxA(nullptr, Error.c_str(), "GrayWolf: open-map", MB_OK | MB_ICONEXCLAMATION);
    return Ok ? 0 : 1;
}

// ---- the subcommands (CliCommands.h)

void AddBlenderCommands(CliCommandBuilders& B)
{
    B["open-map"] = [](CLI::App& App, CliOptions& Options) {
        auto* Sub = App.add_subcommand("open-map", "Open a map export in Blender: the first time its import script runs and "
                                                   "<map>.blend is saved beside it, later that .blend opens");
        Sub->add_option("map", Options.OpenMap, "The map's .glb, its _import.py, its .blend, or a folder holding one map")
            ->required()->check(CLI::ExistingPath);
        Sub->add_flag("--reimport", Options.OpenMapReimport, "Run the import script again, even when the .blend exists");
        Sub->add_flag("--background", Options.OpenMapBackground, "Import and save the .blend without opening Blender's window");
    };
}
