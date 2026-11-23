#pragma once

// The layers after the first of a layered world material (MapScene's VertexColors::LayerWeights)
// as Black Ops III blend overlays (graywolf-tools/docs/material-gdt.md): each layer K of 2..4 a
// decal material of its own (the composite's colorMap<K-1>, normalMap<K-1>, specularMap<K-1>,
// revealMap<K-1> under the base semantics) over the same triangles, its vertices white with the
// layer's weight as alpha, as the mod tools' own blend layers are painted.

#include "MapScene.h"

#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace Maps
{
    struct BlendLayer
    {
        std::string Name;     // the overlay material's source name ("blend2_<colour image's asset name>")
        int Layer = 2;        // 2..4: its place over the base
        Material Mat;         // its textures under the base layer's semantics; ColorMap its colour
        std::string Reveal;   // its reveal map (a key of MapScene::Images), "" without one
        // how it draws over the layers below, as its material's technique set names it (LayerOp):
        // 'b' blends, 't' alpha tests, 'm' multiplies; 0 unnamed (blended)
        char Op = 0;
    };

    // Layer's (2..4) op in a layered material's technique set, as the CoD4 lineage names them
    // (Black Ops' "l_sm_r0c0n0s0_b1c1n1s1x1_m2c2": a token "<op><Layer - 1><maps>" per layer, the
    // maps c n s x each with the layer's number), 0 when no token names it
    char LayerOp(const std::string& Techset, int Layer);

    class BlendLayers
    {
    public:
        explicit BlendLayers(const MapScene& Scene);
        // material Name's overlays by layer (index K - 2), null for a layer with no colour image
        // (nothing to draw); empty for a material that is not layered
        const std::vector<const BlendLayer*>& Of(const std::string& Material) const;
        const BlendLayer* Find(const std::string& Overlay) const;
        // the layers after the first the layered materials have, and of them those without a colour image
        size_t Layers = 0, LayersNoColour = 0;

    private:
        std::map<std::string, BlendLayer> ByName;
        std::map<std::string, std::vector<const BlendLayer*>> ByMaterial;
    };

    // Layer's weight at V (G layer 2, B layer 3, A layer 4)
    inline uint8_t LayerWeight(const Vertex& V, int Layer) { return Layer >= 2 && Layer <= 4 ? V.Color[Layer - 1] : 0; }
    // Layer's UV at vertex I of S: its own set when S has one per layer (Surface::MoreUvs), else the base's
    void LayerUv(const Surface& S, uint32_t I, int Layer, float Uv[2]);
    // Layer's Black Ops III decal layer (layerSortDecal, decalLayerSort): each above the one before
    const char* LayerSort(int Layer);

    // The layered materials whose GDT entry (their first layer, MaterialGdt's Layered) another
    // material's is: each to that one. The map compiler makes a composite per base and the
    // decals over it (BO2's mp_hijacked: "*139n_134(<teak trim>:<bloodpool>)", "*139n_38(<teak
    // trim>:<fillet>)", ten of them over one teak trim), each written alone the same entry ten times.
    // The same first layer: its images (the later layers' left out), the base's op in the
    // technique set ("r0", "t0"...: its alpha) and the sort key. Its material: a plain material of
    // the scene with exactly those (the base the composites were made from), else the first
    // composite by name.
    std::map<std::string, std::string> SharedFirstLayers(const MapScene& Scene);
}
