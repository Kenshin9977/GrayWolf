#pragma once

// Havok's tagfiles (TAG0; SDK 2024.2: Modern Warfare 4, Black Ops 7; 2021.1: Modern Warfare III,
// Black Ops 6) and the hknp collision shapes in them, one reader for every game. Format, offsets and their proof:
// graywolf-tools/docs/havok-tagfile-2024.md. (Infinite Warfare's hk_2014 packfiles: IW7/HavokShapes.h.)

#include "MapScene.h"

#include <cstdint>
#include <functional>
#include <map>
#include <string>
#include <vector>

namespace Havok
{
    struct Member
    {
        std::string Name;
        uint32_t Flags = 0, Offset = 0, Type = 0;
    };

    struct Type
    {
        std::string Name;
        uint32_t Parent = 0, Flags = 0, Size = 0;
        std::vector<Member> Members;
    };

    struct Item
    {
        uint32_t Type = 0, Flags = 0, Offset = 0, Count = 0;
    };

    // One tagfile: its DATA, and its types and items when it describes itself
    class Tagfile
    {
    public:
        // false when Bytes do not start a TAG0 that fits in Size
        bool Open(const uint8_t* Bytes, size_t Size);

        size_t Size = 0;                 // the whole tagfile's
        std::string Sdk;                 // SDKV ("20240200")
        const uint8_t* Data = nullptr;   // DATA's contents
        size_t DataSize = 0;
        std::vector<Type> Types;         // 0: none
        std::vector<Item> Items;         // 0: none

        uint32_t TypeNamed(const std::string& Name) const;                       // 0: none
        int64_t MemberOffset(uint32_t TypeIndex, const std::string& Name) const;  // its parents' too; -1: none
        int64_t ObjectOf(const std::string& Name) const;   // the first object item of that type, -1: none

    private:
        bool ReadTypes(const uint8_t* Begin, const uint8_t* End);
        bool ReadIndex(const uint8_t* Begin, const uint8_t* End);
    };

    // Shapes' geometry in the game's units (a Havok unit is Scale of them)
    struct Geometry
    {
        std::vector<float> Vertices;          // three a vertex
        std::vector<uint32_t> Triangles;      // three a triangle, facing where (b - a) x (c - a) points
        std::vector<uint16_t> TriangleTags;   // each triangle's shape tag (0xffff: none)
        std::vector<Maps::Brush> Convex;      // the convex pieces, their sides the hulls' planes
        std::vector<uint16_t> ConvexTags;
    };

    // Rotation (quaternion x y z w), translation, scale (applied first)
    struct Transform
    {
        float Rotation[4] = { 0, 0, 0, 1 };
        float Translation[3] = { 0, 0, 0 };
        float Scale[3] = { 1, 1, 1 };
    };

    // Where an SDK keeps what the shapes read: hknpShapeType's values, the convex radius, the hull,
    // the mesh's inverse bit scale, tag table and sections (a section's vertex view counting bytes
    // or vertices), the compound's instances
    struct Layout
    {
        uint8_t Convex = 0, Box = 5, Mesh = 7, Compound = 9;
        int64_t Radius = 0x68, Hull = 0x38;
        int64_t MeshInverse = 0x50, MeshTags = 0x60, MeshSections = 0x88;
        bool VertexBytes = true;
        int64_t Instances = 0x38;
    };
    // the layout of the tagfile's SDK (SDKV: 2024.2, 2021.1), nullptr when none is known
    const Layout* LayoutFor(const std::string& Sdk);

    // The shape at At in Data (a tagfile's DATA, its references relative) placed by Place into Out;
    // a compound's instances placed by theirs after it, a piece without a tag of its own taking
    // its instance's (Tag). False when it is no shape read (Counters: havok_*).
    bool ReadShape(const uint8_t* Data, size_t DataSize, size_t At, const Layout& L, const Transform& Place, uint16_t Tag, float Scale,
                   Geometry& Out, std::map<std::string, size_t>& Counters);

    // A HavokPhysicsShapeList's shape tags' collisionFilterInfo (the content flags), from a
    // tagfile that describes itself; false when it holds no such list
    bool ShapeTagContents(const Tagfile& List, std::vector<uint32_t>& Out);

    // A level's collision: each tile tagfile's shape (at DATA's start) into Out, its triangles with
    // their contents' material (TagContents by shape tag), its convex pieces as brushes
    void AddLevelCollision(Maps::Collision& Out, const std::vector<uint32_t>& TagContents, const std::vector<std::vector<uint8_t>>& Tiles,
                           float Scale, std::map<std::string, size_t>& Counters);

    // Where a zone keeps a level's collision: its col_map's shape list tagfile (pointer, u32 size
    // fields) and its collisiontile assets' tagfiles (pointer, u32 size: the mesh, the compound),
    // read through Locate (an address, a size: the bytes, nullptr when not loaded)
    struct LevelSource
    {
        std::function<const uint8_t*(uint64_t, uint64_t)> Locate;
        uint64_t ClipMap = 0;
        int64_t ShapeList = 0, ShapeListSize = 0;
        std::vector<uint64_t> Tiles;
        std::vector<std::pair<int64_t, int64_t>> TileShapes;
    };
    // The level's collision into Out (AddLevelCollision), Counters havok_*
    void ReadLevel(Maps::Collision& Out, const LevelSource& Src, float Scale, std::map<std::string, size_t>& Counters);
}
