#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <array>
#include <vector>

// In-process reuse of OpenAssetTools' zone loader: OAT's prebuilt static libs linked into the app
// (ZoneLoading::LoadZone reads an old-CoD fastfile IW3-T6), assets converted to our formats. OAT
// needs C++23, so only this file compiles /std:c++latest; the rest of the app stays C++17.
// XModel/geometry go through xmodel::ToCommonConverter and our .semodel writer. GPLv3 (compatible
// w/ GPL-3.0).

// Runs OAT's full dump pipeline in-process - the same code path Unlinker.exe drives (Unlinker.cpp):
// loads the zone + referenced containers, then IObjWriter::DumpZone() dumps every asset type plus an
// optional GDT. Returns false with a reason on failure.
struct OatDumpOptions
{
    // Model format: "xmodel_export" | "xmodel_bin" | "obj" | "gltf" | "glb" (default gltf).
    std::string ModelFormat = "gltf";
    // Image format: "dds" | "iwi" (default dds).
    std::string ImageFormat = "dds";
    bool Gdt = false; // also emit a source_data/<zone>.gdt
    // Native "detail" pass: also extract the game's sounds OAT can't dump (T5/BO1 IWD audio → WAV
    // under <out>/_sounds). Opt-in: it is install-wide and slow, so off by default.
    bool WithSounds = false;
};
bool OatDumpZoneInProcess(const std::string& FastFilePath, const std::string& OutputDir,
                          const OatDumpOptions& Options, std::string& Report);

// What a dump wrote beyond OAT's own dumpers (the native passes), for the reports.
struct OatDumpCounts
{
    size_t Assets = 0;           // assets of the dumped categories the zone defines
    size_t References = 0;       // ... and those it only names (another zone defines them)
    size_t Files = 0;            // files written through OAT's output (its dumpers, our JSON writers)
    size_t Sounds = 0, StreamedImages = 0;
    size_t NewSoundFiles = 0;    // T6: sound files this dump added (Sounds counts the whole folder)
    size_t DestructibleDefs = 0, XGlobals = 0, SndDriverGlobals = 0, Glasses = 0, EmblemSets = 0,
           PackIndex = 0, FxImpactTables = 0, KeyValuePairs = 0, Ddl = 0, SoundPatches = 0,
           GameWorlds = 0, SoundBanks = 0, MenuLists = 0, MenuDefs = 0;
};

// A zone loaded once (with its companion zones and referenced containers), dumped by category:
// "model" (xmodel), "anim" (xanim), "image", "sound" (sound, loadedsound, soundbank), "rawfile"
// (rawfile, scriptfile, script), "localize" and "data" (every other type, plus the native JSON
// writers).
// Each category's native pass runs with it (T6 streamed images, T5 IWD sounds). Dumping every
// category is exactly OatDumpZoneInProcess. Used by the disk template (Games/OAT/DiskGame.cpp).
class OatZoneSession
{
public:
    static std::unique_ptr<OatZoneSession> Open(const std::string& FastFilePath, std::string& Report);
    ~OatZoneSession();
    OatZoneSession(const OatZoneSession&) = delete;
    OatZoneSession& operator=(const OatZoneSession&) = delete;

    std::string GameShortName() const;   // "T5", "IW4", ...
    std::string ZoneName() const;
    const std::string& FastFile() const;
    size_t CompanionCount() const;
    size_t Count(const std::string& OatTypeName) const;   // assets of that type the zone defines
    static const char* Category(const std::string& OatTypeName);

    // its localize assets as (key, text), the text in the game's own bytes; references left out
    void Strings(std::vector<std::pair<std::string, std::string>>& Out) const;
    // zone_source/<zone>.zone, the mod tools' recompile source
    void WriteZoneSource(const std::string& OutputDir, bool Gdt) const;
    bool Dump(const std::string& OutputDir, const std::vector<std::string>& Categories,
              const OatDumpOptions& Options, OatDumpCounts& Counts);
    // The weapon assets (weapon, attachment, attachmentunique, camo) as OAT's dumpers write them,
    // kept in memory: (path in OAT's tree, contents), "weapons/<name>" the mod tools' weapon file
    // (an InfoString: WEAPONFILE\<field>\<value>..., OAT's field names, which are the game's own
    // weapon field table), "accuracy/..." its graphs, "attachment/<name>"... Assets: those the
    // zone defines.
    bool WeaponFiles(std::vector<std::pair<std::string, std::string>>& Out, size_t& Assets);
    // The string tables the zone defines as OAT writes them: (path, CSV text), "mp/statstable.csv"...
    bool StringTables(std::vector<std::pair<std::string, std::string>>& Out);

private:
    OatZoneSession();
    // the assets of these OAT type names the zone defines, dumped by OAT's dumpers into memory
    bool DumpTypes(const std::vector<std::string>& Wanted, std::vector<std::pair<std::string, std::string>>& Out, size_t& Assets);
    struct Impl;
    std::unique_ptr<Impl> P;
};

// Lists a fastfile's assets in-process ("<assetType>, <assetName>" per line, like Unlinker --list).
// Returns false with a reason on load failure.
bool OatListZoneInProcess(const std::string& FastFilePath, std::string& Report);

// Describes every FxEffectDef in a T5 (BO1) zone: per-element type, plus material + blend mode
// (read off GfxStateBits, same interpretation as GameBlackOps::DescribeEffect() but against OAT's
// already-relocated in-process zone - works from a bare fastfile, no game running). Returns false
// with a reason if the zone isn't T5.
bool OatDescribeFxInProcess(const std::string& FastFilePath, std::string& Report);

// Real per-effect export (not a text report): writes one JSON file per FxEffectDef whose name
// contains NameFilter (case-insensitive substring, empty = every effect) under OutDir. Returns false
// with a reason if the zone isn't T5.
bool OatExportFxInProcess(const std::string& FastFilePath, const std::string& NameFilter,
                           const std::string& OutDir, std::string& Report);

// Lists every xmodel in a T5 (BO1) zone with its per-surface material name + blend mode (same
// DescribeT5Material() read as OatDescribeFxInProcess, walking XModel::materialHandles instead of
// FxElemDef) - finds a material confirmed rendering on a REAL static model, not just through the
// FX/decal pipeline. Returns false with a reason if the zone isn't T5.
bool OatListXModelMaterialsInProcess(const std::string& FastFilePath, std::string& Report);

// Dumps the compiled (post-inheritance) splash/explosion-falloff WeaponDef fields for every T5
// (BO1) weapon whose name contains NameFilter (case-insensitive substring, empty = every weapon).
// Returns false with a reason if the zone isn't T5.
bool OatDescribeWeaponInProcess(const std::string& FastFilePath, const std::string& NameFilter, std::string& Report);

// Dumps every T5 (BO1) rawfile (.gsc/.csc script source, plain text - not compiled bytecode) whose
// name contains NameFilter (case-insensitive substring, empty = every rawfile) to OutDir, preserving
// its path. Reads straight from the compiled zone on disk via OAT. Returns false with a reason
// if the zone isn't T5.
bool OatDumpRawfilesInProcess(const std::string& FastFilePath, const std::string& NameFilter,
                               const std::string& OutDir, std::string& Report);

// Exports a map fastfile's GfxWorld to a single drag-and-drop Blender .glb through the one map writer
// (Maps::MapScene, MapGlbWriter.h): OAT has no GfxWorld dumper for any game. T5 and T6: world mesh,
// props, textures, lights, skybox.
// Returns false with a reason if the zone isn't T5/T6 or has no GfxWorld.
bool OatExportMapGlb(const std::string& FastFilePath, const std::string& OutGlbPath, std::string& Report,
                     bool EmbedTextures = true);

// Per-type coverage for one fastfile on disk: what the zone holds, and which mechanism can get each
// type out (OAT's own writers, one of ours, or nothing). The shared reader (ZoneSource) builds on
// this and OatZoneCoverage formats it. References are kept and flagged: coverage skips them, a
// listing shows them.
struct OatZoneListing
{
    std::string Game;
    std::string Zone;

    struct Entry
    {
        uint32_t    TypeId = 0;
        std::string TypeName;
        std::string Name;
        bool        IsReference = false;
    };

    std::vector<Entry> Assets;
    // Asset type ids this game's OAT writers can dump, asked of the writers themselves.
    std::vector<uint32_t> ExtractableTypes;
};

bool OatDescribeZone(const std::string& FastFilePath, OatZoneListing& Out, std::string& Error);

bool OatZoneCoverage(const std::string& FastFilePath, std::string& Report);
