#include "stdafx.h"

#include "MapRadiantLayers.h"

#include "MaterialGdt.h"

#include <cstdio>
#include <tuple>

namespace Maps
{
namespace
{
    // the semantics (IW's R_HashString, lowercased; the same from World at War to WWII) of the
    // base layer's images and of layers 2..4's: colorMap, colorMap1.. (seen on WaW, BO1, BO2,
    // Ghosts, WWII composites)
    constexpr uint32_t BaseColor = 0xA0AB1041, BaseNormal = 0x59D30D0F, BaseSpecular = 0x34ECCCB3;
    constexpr uint32_t LayerColor[3] = { 0xB60D1850, 0xB60D1853, 0xB60D1852 };      // colorMap1..3
    constexpr uint32_t LayerNormal[3] = { 0x9434AEDE, 0x9434AEDD, 0x9434AEDC };     // normalMap1..3
    constexpr uint32_t LayerSpecular[3] = { 0xD2866322, 0xD2866321, 0xD2866320 };   // specularMap1..3
    constexpr uint32_t LayerReveal[3] = { 0xCFE18444, 0xCFE18447, 0xCFE18446 };     // revealMap1..3

    const std::vector<const BlendLayer*> None;

    uint32_t Fnv(const std::string& S)
    {
        uint32_t H = 2166136261u;
        for (const char C : S) H = (H ^ (uint8_t)C) * 16777619u;
        return H;
    }
}

BlendLayers::BlendLayers(const MapScene& Scene)
{
    // one overlay per layer and images, whichever composites share it
    std::map<std::tuple<int, std::string, std::string, std::string, std::string, char>, std::string> Keys;
    for (const auto& [Name, M] : Scene.Materials)
    {
        if (M.Colors != VertexColors::LayerWeights) continue;
        int Count = 1;   // the layers: 1 + the last with a colour image
        for (const Texture& T : M.Textures)
            for (int K = 0; K < 3; K++)
                if (T.Semantic == LayerColor[K] && !T.Image.empty()) Count = (std::max)(Count, K + 2);
        if (Count < 2) continue;
        std::vector<const BlendLayer*>& Out = ByMaterial[Name];
        Out.assign((size_t)Count - 1, nullptr);
        for (int K = 0; K + 2 <= Count; K++)
        {
            Layers++;
            BlendLayer L;
            L.Layer = K + 2;
            std::string Normal, Specular;
            for (const Texture& T : M.Textures)
            {
                if (T.Image.empty()) continue;
                if (T.Semantic == LayerColor[K]) L.Mat.ColorMap = T.Image;
                else if (T.Semantic == LayerNormal[K]) Normal = T.Image;
                else if (T.Semantic == LayerSpecular[K]) Specular = T.Image;
                else if (T.Semantic == LayerReveal[K]) L.Reveal = T.Image;
            }
            if (L.Mat.ColorMap.empty())
            {
                LayersNoColour++;
                continue;
            }
            L.Mat.Techset = M.Techset;
            L.Op = LayerOp(M.Techset, L.Layer);
            L.Mat.Textures.push_back({ BaseColor, L.Mat.ColorMap });
            if (!Normal.empty()) L.Mat.Textures.push_back({ BaseNormal, Normal });
            if (!Specular.empty()) L.Mat.Textures.push_back({ BaseSpecular, Specular });
            const auto Key = std::make_tuple(L.Layer, L.Mat.ColorMap, Normal, Specular, L.Reveal, L.Op);
            auto [It, New] = Keys.emplace(Key, std::string());
            if (New)
            {
                // named by its layer and colour image (as a GDT asset name); another layer of that
                // colour and other images its own
                std::string N = "blend" + std::to_string(L.Layer) + "_" + MaterialGdt::AssetName(L.Mat.ColorMap);
                if (ByName.count(N))
                {
                    char Tail[16];
                    std::snprintf(Tail, sizeof(Tail), "_%08x", Fnv(Normal + "|" + Specular + "|" + L.Reveal + "|" + std::string(1, L.Op)));
                    N += Tail;
                }
                L.Name = L.Mat.Name = N;
                It->second = N;
                ByName.emplace(N, std::move(L));
            }
            Out[(size_t)K] = &ByName.at(It->second);
        }
    }
}

const std::vector<const BlendLayer*>& BlendLayers::Of(const std::string& Material) const
{
    const auto It = ByMaterial.find(Material);
    return It == ByMaterial.end() ? None : It->second;
}

const BlendLayer* BlendLayers::Find(const std::string& Overlay) const
{
    const auto It = ByName.find(Overlay);
    return It == ByName.end() ? nullptr : &It->second;
}

char LayerOp(const std::string& Techset, int Layer)
{
    const char Number = (char)('0' + Layer - 1);
    for (size_t At = 0; At < Techset.size();)
    {
        const size_t End = (std::min)(Techset.find('_', At), Techset.size());
        const std::string T = Techset.substr(At, End - At);
        if (T.size() >= 2 && T[0] >= 'a' && T[0] <= 'z' && T[1] == Number && (T.size() == 2 || (T[2] >= 'a' && T[2] <= 'z'))) return T[0];
        At = End + 1;
    }
    return 0;
}

void LayerUv(const Surface& S, uint32_t I, int Layer, float Uv[2])
{
    Uv[0] = S.Vertices[I].Uv[0], Uv[1] = S.Vertices[I].Uv[1];
    const uint32_t Set = (uint32_t)Layer - 1;   // the layer's set after the base's (1 = the first in MoreUvs)
    if (S.UvSets <= Set) return;
    const size_t At = ((size_t)I * (S.UvSets - 1) + (Set - 1)) * 2;
    if (At + 1 >= S.MoreUvs.size()) return;
    Uv[0] = S.MoreUvs[At], Uv[1] = S.MoreUvs[At + 1];
}

const char* LayerSort(int Layer)
{
    // material.awi's decal layers run top first: Mud above Dirt above Grunge
    return Layer >= 4 ? "Mud" : Layer == 3 ? "Dirt" : "Grunge";
}
}
