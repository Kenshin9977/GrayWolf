#pragma once

// Black Ops III Mod Tools material GDT entries for any game's materials (the material_gdt
// setting): each Materials::Description the export already computed (the <model>.materials.json
// sidecars, a map's scene materials) as a `material.gdf` entry whose image slots name `image.gdf`
// entries for the exported image files, so a ported model or Radiant map links in the mod tools
// without its materials made by hand in APE. Only the description is read: a game supports it by
// having a material adapter.

#include "MaterialDescription.h"

#include <cstddef>
#include <filesystem>
#include <functional>
#include <map>
#include <set>
#include <string>
#include <vector>

namespace DiskGame { struct Context; struct Totals; }

namespace MaterialGdt
{
    // A source material's BO3 asset name: lowercased, every character outside [a-z0-9_] as '_',
    // leading and trailing '_' dropped ("mc/mtl_Foo" -> "mc_mtl_foo", "~mtl_x-1" -> "mtl_x_1").
    // A function of the name alone, so a model file written on its own names what the GDT defines.
    std::string AssetName(const std::string& Source);
    // Prefix + AssetName(Source), at most 56 characters (BO3's tools keep 64, cod2map asserting on
    // a cut name; an image's tag may follow): a longer one cut, an 8-digit hash of the source name
    // after it so it stays its own; so too one AssetName changes ("~tm_x" and "tm_x" two names).
    // Without a prefix, AssetName(Source) as it is.
    std::string PrefixedName(const std::string& Prefix, const std::string& Source);

    class Builder
    {
    public:
        // BaseImage: an image file (absolute) as the GDT's baseImage, a path below the BO3 root
        // with '/' separators ("" leaves the slot out). Prefix: the counters' ("material_gdt_").
        // NamePrefix: before every material's asset name and after the "i_" of every image's
        // (a map's own, so two maps in one mod tools tree never define the same asset).
        Builder(std::function<std::string(const std::filesystem::path& File)> BaseImage, std::string Prefix, std::string NamePrefix = {});

        // A material, its image paths relative to Dir. The first description of an asset name
        // wins: the same material from another model is counted shared, another source name
        // that gives the same asset name is counted a collision (callers add in a sorted order).
        // Layered: a world material the map compiler merged from layered patches (MapScene's
        // VertexColors::LayerWeights), drawn opaque by its first layer: never a decal, whatever its
        // layers' names say ("*104n_14(brick_wall:decal_grunge...)" is a brick wall).
        // BlendLayerSort: a later layer of such a material drawn over it (MapRadiantLayers.h), a
        // decal whose opacity is its vertices' alpha (times its colour's alpha, or its reveal map,
        // D.AlphaMap: lit_decal_reveal), in that decal layer (layerSortDecal).
        void Add(const Materials::Description& D, const std::filesystem::path& Dir, bool Layered = false, const char* BlendLayerSort = nullptr);

        bool Empty() const;
        // the material entries then the image entries, each by name
        std::vector<std::string> Entries(const char* Eol) const;
        // { "materials": { asset: source }, "collisions": { asset: [other sources] } }
        std::string NamesJson() const;
        // what was written and what had no BO3 slot: <Prefix><what> -> count
        const std::map<std::string, size_t>& Counters() const { return Count; }

        struct Image
        {
            std::filesystem::path File;
            std::string Semantic, Core, Compression;
            bool FromAlpha = false;
            std::string Builtin;   // a BO3 code image ("$normal") named as it is: no entry, no file
            bool operator<(const Image& O) const;
        };
        struct Material
        {
            std::string Source, Category, Type, Usage = "<not in editor>";
            std::map<std::string, Image> Slots;                 // colorMap -> image
            std::vector<std::pair<std::string, std::string>> Values;   // other fields set
        };

    private:
        std::function<std::string(const std::filesystem::path&)> BaseImageOf;
        std::string Prefix, NamePrefix;
        std::map<std::string, Material> Defined;               // by asset name
        std::map<std::string, std::set<std::string>> Collisions;
        std::map<std::string, size_t> Count;
        size_t& Counter(const std::string& What) { return Count[Prefix + What]; }
        bool Slot(Material& M, const char* Key, const Materials::Map& Map, const std::filesystem::path& Dir, const char* Semantic,
                  const char* Core, const char* Compression);
    };

    // The model kind's GDT: every <model>.materials.json under the run's models folder (this run's
    // and earlier ones'), its materials, their images and the models' XMODEL_EXPORT files, as
    // <out>/source_data/graywolf_<out folder>.gdt. Paths assume <out> is copied to
    // <BO3>/model_export/graywolf/<out folder>/. With mod_tools (ModToolsTree.h), the GDT goes into
    // the tree's source_data and the files it names there. Counters into T.
    void WriteForModels(const DiskGame::Context& C, DiskGame::Totals& T);

    // While a run with material_gdt on exports, the mod tools' model files (XMODEL_EXPORT, _BIN)
    // name their materials by AssetName (LiveExport reads it); off again when the guard ends.
    bool ModelFilesUseAssetNames();
    class AssetNamesInModelFiles
    {
    public:
        explicit AssetNamesInModelFiles(bool On);
        ~AssetNamesInModelFiles();
        AssetNamesInModelFiles(const AssetNamesInModelFiles&) = delete;
        AssetNamesInModelFiles& operator=(const AssetNamesInModelFiles&) = delete;

    private:
        bool Was;
    };
}
