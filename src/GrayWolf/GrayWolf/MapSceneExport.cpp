#include "stdafx.h"

#include "MapSceneExport.h"

#include "ExportNaming.h"
#include "ExportWriters.h"
#include "MapSceneWriter.h"
#include "MaterialDescription.h"
#include "ModToolsTree.h"

#include <filesystem>
#include <memory>
#include <utility>
#include <vector>

namespace Maps
{
bool WriteMapFiles(MapScene& Scene, const std::set<std::string>& Names,
                   const std::function<std::string(const std::string&)>& FileOf, const std::string& Ext,
                   const std::string& Stem, const Materials::IAdapter& Adapter, const std::string& Zone,
                   const DiskGame::Context& C, DiskGame::Totals& T, std::map<std::string, size_t>& Counters)
{
    using DiskGame::Kind;
    // The sidecar and the .glb name the image kind's files (the import script reads DDS). With the
    // map_png setting, the colour maps that are not PNG are also copied as PNG under maps/textures/
    // for other glTF tools, and the .glb names those.
    const bool Png = DiskGame::MapPng.Of(C.Settings) == "on";
    const std::filesystem::path MapDir = C.KindDir(Kind::Map);
    std::set<std::string> Colors = { Scene.Sky.Image };
    for (const auto& [MatName, Mat] : Scene.Materials)
        if (!Mat.ColorMap.empty()) Colors.insert(Mat.ColorMap);
    std::map<std::string, Maps::Image> Images;
    std::vector<std::pair<std::string, std::string>> Copies, Unsigned;
    for (const auto& Name : Names)
    {
        const std::string Safe = ExportNaming::SafeFileName(Name);
        Maps::Image& I = Images[Name];
        I.File = FileOf(Name);
        // a signed DDS (Ghosts' BC5 normals) gets an unsigned copy the import script reads instead
        if (ExportNaming::Lower(Ext) == ".dds") Unsigned.emplace_back(I.File, (MapDir / "textures" / (Safe + Ext)).string());
        if (!Png || !Colors.count(Name) || ExportNaming::Lower(Ext) == ".png") continue;
        I.Gltf = (MapDir / "textures" / (Safe + ".png")).string();
        Copies.emplace_back(I.File, I.Gltf);
    }
    // the images the reader made (PNG bytes: a terrain's layer masks), written under maps/textures/
    for (auto& [Name, I] : Scene.Images)
    {
        if (I.Png.empty()) continue;
        const std::filesystem::path File = MapDir / "textures" / (ExportNaming::SafeFileName(Name) + ".png");
        std::error_code Ec;
        std::filesystem::create_directories(File.parent_path(), Ec);
        if (FILE* F = nullptr; fopen_s(&F, File.string().c_str(), "wb") == 0 && F != nullptr)
        {
            const bool Ok = std::fwrite(I.Png.data(), 1, I.Png.size(), F) == I.Png.size();
            std::fclose(F);
            if (!Ok) continue;
            Maps::Image& Out = Images[Name];
            Out.File = Out.Gltf = File.string();
            Counters["map_images_made"]++;
        }
    }
    Scene.Images = std::move(Images);
    if (!Unsigned.empty())
        C.After([Unsigned, &T]() {
            size_t Failed = 0;
            if (const size_t N = ExportWriters::UnsignedDdsCopies(Unsigned, Failed)) T.Counters["map_textures_unsigned"] += N;
        });
    // map_textures: the textures above the size, copied smaller with mips (after the unsigned
    // copies, which they start from)
    const std::string Cap = DiskGame::MapTextures.Of(C.Settings);
    if (!Unsigned.empty() && Cap != "full")
        C.After([Unsigned, Max = Cap == "1k" ? 1024u : 2048u, &T]() {
            size_t Failed = 0;
            double Seconds = 0;
            bool Gpu = false;
            T.Counters["map_textures_resized"] += ExportWriters::ResizedDdsCopies(Unsigned, Max, Failed, Seconds, Gpu);
            T.Counters["map_textures_resize_ms"] += (size_t)(Seconds * 1000);
            if (Gpu) T.Counters["map_textures_resize_gpu"] = 1;
            if (Failed) T.Counters["map_textures_resize_failed"] += Failed;
        });
    if (!Copies.empty())
        C.After([Copies, &T]() {
            size_t Failed = 0;
            T.Counters["map_textures_png"] += ExportWriters::PngCopies(Copies, Failed);
            if (Failed) T.Counters["map_textures_missing"] += Failed;
        });

    // The scene in each enabled format beside one another: the .glb always (the map's own file:
    // without it the map failed), the Cast scene with map_cast
    std::vector<const Maps::ISceneWriter*> Writers = { &Maps::GlbWriter() };
    if (DiskGame::MapCast.Of(C.Settings) == "on") Writers.push_back(&Maps::CastWriter());
    std::unique_ptr<Maps::ISceneWriter> Radiant;
    if (DiskGame::MapRadiant.Of(C.Settings) == "on")
    {
        Radiant = Maps::RadiantWriter(DiskGame::GdtMaterials.Of(C.Settings) == "on", ModToolsTree::RootOf(C),
                                      DiskGame::MapSurfaces.Of(C.Settings) == "patches",
                                      std::filesystem::path(C.OutRoot).filename().string());   // <out>/<game id>
        Writers.push_back(Radiant.get());
    }
    if (DiskGame::MapCollision.Of(C.Settings) == "on") Writers.push_back(&Maps::CollisionWriter());
    bool Written = false;
    for (const Maps::ISceneWriter* W : Writers)
    {
        Maps::SceneWriteResult Result;
        std::string Error;
        if (!W->Write(Scene, Stem + W->Extension(), Adapter, Result, Error))
        {
            T.Fail(Zone, Kind::Map, Scene.Name, Error);
            if (W == Writers.front()) return false;
            continue;
        }
        if (W == Writers.front()) Written = true;
        for (const auto& [Key, V] : Result.Counters) Counters[Key] += V;
        if (Result.Later)
            C.After([Later = Result.Later, Key = Result.LaterCounter, &T]() { T.Counters[Key] += Later(); });
        if (Result.LaterCounters) C.After([Later = Result.LaterCounters, &T]() { Later(T.Counters); });
    }
    return Written;
}
}
