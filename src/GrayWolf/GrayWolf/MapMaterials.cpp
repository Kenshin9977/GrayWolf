#include "stdafx.h"

#include "MapMaterials.h"

namespace fs = std::filesystem;

namespace Maps
{
std::string RelativeTo(const std::string& File, const fs::path& Dir)
{
    std::error_code Ec;
    const fs::path R = fs::relative(File, Dir, Ec);
    return (Ec || R.empty() ? fs::path(File) : R).generic_string();
}

std::pair<Materials::Source, Materials::Description> DescribeMaterial(const MapScene& Scene, const std::string& Name,
                                                                      const Materials::IAdapter& Adapter, const fs::path& Dir)
{
    const auto It = Scene.Materials.find(Name);
    Material M = It != Scene.Materials.end() ? It->second : Material();
    M.Name = Name;
    return DescribeMaterial(Scene, M, Adapter, Dir);
}

std::pair<Materials::Source, Materials::Description> DescribeMaterial(const MapScene& Scene, const Material& M,
                                                                      const Materials::IAdapter& Adapter, const fs::path& Dir)
{
    Materials::Source Src;
    Src.Name = M.Name;
    Src.Techset = M.Techset;
    Src.Constants = M.Constants;
    Src.SortKey = M.SortKey;
    for (const Texture& T : M.Textures)
    {
        const auto Img = Scene.Images.find(T.Image);
        if (Img == Scene.Images.end() || Img->second.File.empty()) continue;   // embedded: no file
        Materials::Source::Image SI;
        SI.Path = RelativeTo(Img->second.File, Dir);
        SI.Semantic = T.Semantic;
        SI.Hint = T.Image == M.ColorMap ? Materials::Role::Albedo : Materials::Role::Unknown;
        Src.Images.push_back(std::move(SI));
    }
    Materials::Description D = Adapter.Describe(Src);
    return { std::move(Src), std::move(D) };
}
}
