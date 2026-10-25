#pragma once

// The effects preview's geometry (FxPreviewRender.cpp uploads and draws it): a frame's particles
// and trails as triangles of coloured, textured vertices in one array, cut in items (a quad, a
// ribbon, a cone) sorted back to front, and the ground grid's lines. CPU only: the images come
// from the renderer (Images).

#include "FxPreviewRender.h"

#include <DirectXMath.h>

#include <functional>
#include <vector>

namespace FxPreview::Geometry
{
    struct Vertex { float Pos[3]; float Uv[2]; float Col[4]; };

    // the sort key's and the batches' unit: a quad or a ribbon, its texture and blending
    struct Item
    {
        float Depth = 0;
        ID3D11ShaderResourceView* Srv = nullptr;
        int Mode = 0;           // Blend
        float Bright = 1;       // EmissiveBoost
        bool Wrap = false;      // its texture repeated (a trail's along it)
        uint32_t First = 0, Count = 0;   // vertices in Verts
    };

    // the camera the quads face
    struct View
    {
        DirectX::XMVECTOR Eye, Forward, Right, Up;
        float Fov = 0;
    };

    // the images the geometry draws with
    struct Images
    {
        ID3D11ShaderResourceView* Dot = nullptr;     // the soft dot: no image found, lights
        ID3D11ShaderResourceView* White = nullptr;
        std::function<ID3D11ShaderResourceView*(const Material*)> Of;   // a material's (the dot when none)
    };

    float ToLinear(float C);

    struct Batch
    {
        std::vector<Vertex> Verts;
        std::vector<Item> Items;
        size_t Quads = 0;
        // what the next quads draw with: brightness, lit (set per particle, back to 1 and unlit after)
        float NextBright = 1;
        bool NextLit = false;

        void Clear();
        // the particles as quads; the model elements apart (Models), drawn as meshes first
        void AddParticles(const Frame& F, const View& V, const DrawOptions& O, const Images& Img, std::vector<const Particle*>& Models);
        // the trails as ribbons facing the camera, or their profile swept along them
        void AddTrails(const Frame& F, const View& V, const Images& Img);
        // back to front; the draw order of equal depths kept
        void SortBackToFront();

    private:
        void Quad(const DirectX::XMVECTOR& C, const DirectX::XMVECTOR& Right, const DirectX::XMVECTOR& Up, const float* Uv,
                  const float* Col, const View& V, ID3D11ShaderResourceView* Srv, int Mode);
        // what a sprite draws with
        struct Look
        {
            const float* Uv;
            const float* Col;
            ID3D11ShaderResourceView* Srv;
            int Mode;
        };
        void AddLight(const Particle& Pt, const DirectX::XMVECTOR& C, float* Col, const View& V, const DrawOptions& O, const Images& Img);
        void AddLensFlare(const DirectX::XMVECTOR& C, const float* Col, float Strength, const View& V, const Images& Img);
        void AddSpotCone(const Particle& Pt, const DirectX::XMVECTOR& C, const float* Col, float Reach, float Strength, const View& V,
                         const Images& Img);
        void AddBeam(const Particle& Pt, const DirectX::XMVECTOR& C, const float* Col, const View& V, const Images& Img);
        void AddSprite(const Particle& Pt, const DirectX::XMVECTOR& C, float* Col, const View& V, const DrawOptions& O, const Images& Img);
        void AddDecal(const Particle& Pt, const Look& L, const View& V, const DrawOptions& O);
        void AddTail(const Particle& Pt, const DirectX::XMVECTOR& C, const Look& L, const View& V);
        void AddOriented(const Particle& Pt, const DirectX::XMVECTOR& C, const Look& L, const View& V);
        void AddCloud(const Particle& Pt, const DirectX::XMVECTOR& C, const Look& L, const View& V);
        void AddCylinder(const Particle& Pt, const DirectX::XMVECTOR& C, const Look& L, const View& V);
        void AddBillboard(const Particle& Pt, const DirectX::XMVECTOR& C, const Look& L, const View& V);
        void AddProfiledTrail(const TrailStrip& S, Item It, const View& V);
        void AddRibbon(const TrailStrip& S, Item It, const View& V);
    };

    // the grid: lines on the ground under the effect, every O.GridStep units (0: from the camera's
    // distance to what it looks at, Dist)
    std::vector<Vertex> GridLines(const DrawOptions& O, float Dist);
}
