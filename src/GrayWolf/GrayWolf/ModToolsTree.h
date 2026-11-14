#pragma once

// The Black Ops III Mod Tools tree the mod tools files go into (the mod_tools setting): a BO3
// install with the Mod Tools (Steam app 455130, installed in the game's folder). Every file goes in
// a GrayWolf name (map_source/graywolf/, model_export/graywolf/, source_data/graywolf_*.gdt,
// share/raw/sound/aliases/graywolf_*.csv, sound_assets/graywolf/). A file a user edits there (the
// .map, a GDT, an alias CSV) is never overwritten: one with other bytes is renamed <file>.bak
// first; the model, image and audio files are the export's own, replaced.

#include <cstddef>
#include <filesystem>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace DiskGame { struct Context; }

namespace ModToolsTree
{
    // A BO3 folder with the mod tools in it: bin/asseteditor_modtools.exe (APE),
    // bin/linker_modtools.exe and the folders source_data/, model_export/ and map_source/. Missing:
    // the first marker not there.
    bool IsRoot(const std::filesystem::path& Folder, std::string* Missing = nullptr);
    // The first Black Ops III install GameInstalls finds with the mod tools in it; "" when none
    std::string Detect();
    // The tree a run writes into: Given (--mod-tools-root) when set, else graywolf.json's
    // bo3_mod_tools_root, else Detect(); "" and Why when none is, or the folder is no mod tools tree
    std::string Resolve(const std::string& Given, std::string& Why);
    // The run's tree (its "mod_tools_root" setting, which Export sets with mod_tools on); "" when off
    std::string RootOf(const DiskGame::Context& C);

    // Writes Bytes as File in the tree, a file a user edits in the mod tools: one already there with
    // other bytes renamed <file>.bak first (the first free of .bak, .bak2, ...); false when it
    // cannot be set aside or the file is not written
    bool WriteKept(const std::filesystem::path& File, const std::string& Bytes);
    // A file the export wrote, hard-linked (else copied) as To in the tree, replacing what is there
    bool Place(const std::filesystem::path& From, const std::filesystem::path& To);
    // The asset names (lower case) the GDTs already in the tree define (source_data/, texture_assets/,
    // model_export/, the stock ones and those a user or another export added), but Except: an entry
    // of the same name in a GDT GrayWolf writes is a duplicate APE and the linker refuse
    std::unordered_set<std::string> DefinedAssets(const std::filesystem::path& Root, const std::filesystem::path& Except);
    // The same names with their entries' GDF (lower case: "xmodel.gdf", "material.gdf"; "" for an
    // entry derived from another). StockOnly: the tree's own GDTs alone, GrayWolf's left out
    // (source_data/graywolf_*.gdt, model_export/graywolf/): what the mod tools ship or a user made
    std::unordered_map<std::string, std::string> DefinedTypes(const std::filesystem::path& Root, const std::filesystem::path& Except,
                                                              bool StockOnly);
    // Gdt, a GDT GrayWolf writes in the tree (<Root>/source_data/<file>): the entries (Gdt::Entry
    // texts) another GDT of the tree defines removed, said once in the run's warnings; how many
    size_t LeaveOutDefined(const std::filesystem::path& Gdt, std::vector<std::string>& Entries);

    // What a run put in the tree: Begin at its start, Take at its end (one run at a time)
    struct Record
    {
        std::string Root;
        std::vector<std::string> Files;                          // the files a user opens (.map, .gdt, .csv)
        std::vector<std::pair<std::string, std::string>> Kept;   // an earlier file -> where it was set aside
        size_t Placed = 0, NotPlaced = 0;                        // model, image and audio files
        std::vector<std::string> Warnings;
    };
    void Begin(const std::string& Root);
    void Warn(const std::string& Text);
    Record Take();
    // What is left to do in the mod tools, one line each, for the report and the Load Game panel
    std::vector<std::string> NextSteps(const Record& R);
}
