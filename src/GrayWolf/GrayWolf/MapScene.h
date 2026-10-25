#pragma once

// A level, the same for every game: what a game's map reader fills (Exports(Kind::Map, ...)) and
// the one writer (MapGlbWriter.h) turns into a .glb. Taken from C2M's CoDMap, which held 26
// games: world meshes, models placed by static, dynamic and splined instances, lights, materials
// by name and the images they use, the sky, the ambient colour and the entity string. Positions
// stay in the game's space (Z up, inches); triangles keep the game's winding.

#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace Maps
{
    struct Vertex
    {
        float Pos[3] = { 0, 0, 0 };
        float Normal[3] = { 0, 0, 1 };
        float Uv[2] = { 0, 0 };
        uint8_t Color[4] = { 255, 255, 255, 255 };
    };

    // Triangles of one material.
    struct Surface
    {
        std::string Material;
        std::vector<Vertex> Vertices;
        std::vector<uint32_t> Indices;   // three per triangle, into Vertices
        // the UV sets after each vertex's Uv (a blended surface's other layers: WWII), UvSets - 1
        // pairs per vertex in a row; empty when the surface has one set
        uint32_t UvSets = 1;
        std::vector<float> MoreUvs;
        size_t Triangles() const { return Indices.size() / 3; }
    };

    // World geometry (in world space) or a model (in its own space, LOD 0).
    struct Mesh
    {
        std::string Name;
        std::vector<Surface> Surfaces;
    };

    // A model placed in the world: Axis rows are its local X, Y and Z in world space.
    struct Instance
    {
        std::string Name;    // the node's name ("prop_12")
        std::string Model;   // a key of MapScene::Models
        float Axis[9] = { 1, 0, 0, 0, 1, 0, 0, 0, 1 };
        float Origin[3] = { 0, 0, 0 };
        float Scale = 1.0f;
        std::string Destroyed;   // a destructible's other model, when it has one
    };

    struct Light
    {
        enum class Type { Sun, Spot, Point };
        Type Kind = Type::Point;
        float Origin[3] = { 0, 0, 0 };
        float Dir[3] = { 0, 0, -1 };
        float Color[3] = { 1, 1, 1 };   // as the game gives it (HDR: its brightness is its intensity)
        float Radius = 0.0f;
        float CosHalfFovOuter = 0.0f;
    };

    // A texture a material uses: the game's semantic hash (0 when it has none) and the image.
    struct Texture
    {
        uint32_t Semantic = 0;
        std::string Image;   // a key of MapScene::Images
    };

    struct Material
    {
        std::string Name;
        std::string Techset;
        std::string ColorMap;   // the image drawn as the base colour ("" when none)
        std::vector<Texture> Textures;
        std::vector<std::vector<uint8_t>> Constants;   // the game's constant blocks (the adapter's tint, layers)
        int SortKey = -1;                               // the game's sort key, -1 when unknown
    };

    // An image's pixels: PNG bytes embedded in the .glb, or files (absolute paths, which may be
    // written later in the run: a dependency). File is the image kind's (what the materials sidecar
    // names); Gltf a PNG of it for glTF tools (the map_png setting). The .glb names Gltf, else File
    // (a DDS through MSFT_texture_dds).
    struct Image
    {
        std::vector<uint8_t> Png;
        std::string File, Gltf;
        bool Empty() const { return Png.empty() && File.empty() && Gltf.empty(); }
    };

    struct Sky
    {
        std::string Model;   // a sky drawn as a model (a key of Models), placed at the origin
        std::string Image;   // a sky drawn as a texture (a key of Images): written beside the .glb
        float Sun[3] = { 0, 0, 0 };   // unit vector toward the sun, game space (zero: unknown)
        bool Atmosphere = false;      // the game draws the sky by scattering (no texture): a physical sky
    };

    // A brush of the clip map: the convex volume its sides bound. Each side is a plane whose normal
    // points out of the brush (a point p is inside when Normal . p <= Dist for every side), with
    // the clip map's material name when the game keeps one per side ("" otherwise) and the flags
    // the game gives the side. Contents are the brush's content flags (Quake's lineage: 1 solid,
    // 0x10000 player clip, 0x20000 monster clip, ...). The axis-aligned sides the game keeps as a
    // bounding box are sides here like the others; a side may bound nothing (a bevel).
    struct BrushSide
    {
        float Normal[3] = { 0, 0, 1 };
        float Dist = 0.0f;
        std::string Material;
        uint32_t Contents = 0, Surface = 0;
    };

    struct Brush
    {
        uint32_t Contents = 0;
        std::vector<BrushSide> Sides;
    };

    // The level's collision, from its clip map: the world's brushes, and each brush model's, in
    // the model's own space (Models[N] is "*N" in the entity string; Models[0], the world, empty).
    struct Collision
    {
        std::vector<Brush> Brushes;
        std::vector<std::vector<Brush>> Models;
        // the entity string's trigger volumes (MapTriggers: "model" "?N" is element N), in its entity's space
        std::vector<std::vector<Brush>> Triggers;
        size_t ClipBrushes = 0;   // the clip map's brush count (world and models): what was read from
        // the clip map's materials (name, content flags): what names the tool materials of this game
        std::vector<std::pair<std::string, uint32_t>> Materials;
        // the world's collision triangles (patches, terrain): three floats per vertex, three
        // vertex numbers per triangle (a, b, c), in world space, each facing out where
        // (b - a) x (c - a) points (a game storing them the other way round is turned by its reader)
        std::vector<float> Vertices;
        std::vector<uint32_t> Triangles;
        bool Empty() const { return Brushes.empty() && Models.empty() && Triggers.empty(); }
    };

    struct MapScene
    {
        std::string Name;
        std::string Game;                           // the game id ("bo4"): the materials sidecar's
        std::vector<Mesh> Objects;                  // world geometry
        std::map<std::string, Mesh> Models;         // what the instances place, by name
        std::vector<Instance> StaticInstances, DynamicInstances, SplinedInstances;
        std::vector<Light> Lights;
        std::map<std::string, Material> Materials;  // by name
        std::map<std::string, Image> Images;        // by name
        float AmbientColor[3] = { 0, 0, 0 };
        Maps::Sky Sky;
        std::string MapEnts;                        // the entity string, as the game keeps it
        std::map<std::string, std::string> EntityKeys;   // a key the entity string numbers (MW3, Ghosts on), by number
        Maps::Collision Collision;                  // the clip map's brushes, when the game reads them
    };
}
