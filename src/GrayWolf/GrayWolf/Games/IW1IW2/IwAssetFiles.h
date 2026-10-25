#pragma once

// The asset files of Call of Duty (IW1, 2003) and Call of Duty 2 (IW2), as they sit in the .pk3/.iwd
// zip archives: one file per asset, no fast file. One parser per file kind, with a version switch
// (CoD1 writes everything at version 14; CoD2 its models at 20 and its animations still at 14).
// Every parser reads its file to the last byte or refuses it, naming the offset: a file that parses
// with bytes left over is a layout not understood, never a success.

#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace IW12
{
    // xmodel/<name>: the LOD list (each LOD names an xmodelsurfs file, LOD 0 also the xmodelparts
    // file), the collision surfaces (skipped) and each LOD's materials, one per surface.
    struct XModelFile
    {
        struct Lod
        {
            std::string Name;
            float Distance = 0;
            std::vector<std::string> Materials;
        };
        uint16_t Version = 0;
        std::vector<Lod> Lods;      // the named ones, in order
        int32_t CollisionLod = -1;
        // Version 20 ends with each bone's bounds (two vec3, 24 bytes), the bone count being the
        // xmodelparts file's: the bytes after the materials, which Complete() checks.
        size_t Trailing = 0;
        bool Complete(size_t BoneCount) const { return Version == 14 ? Trailing == 0 : Trailing == 24 * BoneCount; }
    };

    // xmodelparts/<lod0 name>: the skeleton. Roots first (no transform of their own: tag_origin),
    // then the other bones with their parent (an absolute index, always lower) and local transform;
    // then every bone's name (version 14 follows each name with 24 bytes of bounds), then a part
    // classification byte per bone.
    struct XModelPartsFile
    {
        struct Bone
        {
            std::string Name;
            int Parent = -1;               // absolute index, -1 for a root
            float Position[3] = { 0, 0, 0 };
            int16_t Rotation[3] = { 0, 0, 0 };   // quaternion x, y, z / 32767; w = +sqrt(1 - |xyz|^2)
        };
        uint16_t Version = 0;
        uint16_t Roots = 0;
        std::vector<Bone> Bones;
    };

    // xmodelsurfs/<lod name>: one surface per material. A vertex's position is its offset in its
    // bone's space; a weighted vertex carries one more (bone, offset, weight) per extra influence,
    // the first bone keeping the rest of the weight.
    struct XModelSurfsFile
    {
        struct Influence
        {
            uint16_t Bone = 0;
            float Offset[3] = { 0, 0, 0 };
            float Weight = 1;
        };
        struct Vertex
        {
            float Normal[3] = { 0, 0, 0 };   // in the first influence's bone space
            float UV[2] = { 0, 0 };
            uint8_t Color[4] = { 255, 255, 255, 255 };
            std::vector<Influence> Influences;   // the first is the vertex's own bone
        };
        struct Surface
        {
            std::vector<Vertex> Vertices;
            std::vector<std::array<uint16_t, 3>> Faces;   // the game's order (clockwise, as D3D)
            uint16_t HeaderFaces = 0;                       // the face count the header gives
        };
        uint16_t Version = 0;
        std::vector<Surface> Surfaces;
    };

    // xanim/<name>: per part (bone, by name) its rotation and translation keys. A key list holds
    // one key (frame 0), every frame, or the frames it names. The rotations of a part whose flip bit
    // is set have a negative w; a "simple" part stores only z (a rotation about z). A delta
    // animation carries tag_origin's movement first.
    struct XAnimFile
    {
        struct Track
        {
            std::vector<uint16_t> Frames;            // one per key
            std::vector<std::array<float, 4>> Keys;   // rotation x y z w, or translation x y z
        };
        struct Part
        {
            std::string Name;
            bool Flip = false, Simple = false;
            Track Rotations, Translations;
        };
        uint16_t Version = 0;
        uint16_t FrameCount = 0;   // as stored; a looping animation has one more frame
        uint16_t FrameRate = 0;
        uint8_t Flags = 0;         // 1 looping, 2 delta
        bool Looping() const { return (Flags & 1) != 0; }
        bool Delta() const { return (Flags & 2) != 0; }
        uint32_t Frames() const { return FrameCount + (Looping() ? 1u : 0u); }
        Part DeltaPart;            // tag_origin, when Delta()
        std::vector<Part> Parts;
        std::vector<std::pair<std::string, uint16_t>> Notes;   // name, frame
    };

    // materials/<name> (CoD2): the technique set and the images by sampler ("colorMap", "normalMap",
    // "detailMap", "specularMap"...). Offsets point into a string pool that ends the file. An effect
    // material's images can be an atlas: its rows and columns at 0xe, 0xf (gfx_smokepuff_atlas 4 x 4;
    // every other material 1 x 1).
    struct MaterialFile
    {
        std::string Name, Techset;
        uint8_t AtlasRows = 1, AtlasCols = 1;
        std::vector<std::pair<std::string, std::string>> Textures;   // sampler, image name
    };

    // Each returns false with Error ("<what> at <offset>") when the file does not read to its end.
    bool ParseXModel(const uint8_t* Data, size_t Size, XModelFile& Out, std::string& Error);
    bool ParseXModelParts(const uint8_t* Data, size_t Size, XModelPartsFile& Out, std::string& Error);
    bool ParseXModelSurfs(const uint8_t* Data, size_t Size, XModelSurfsFile& Out, std::string& Error);
    bool ParseXAnim(const uint8_t* Data, size_t Size, XAnimFile& Out, std::string& Error);
    bool ParseMaterial(const uint8_t* Data, size_t Size, MaterialFile& Out, std::string& Error);

    // A stored quaternion's x, y, z (int16 / 32767) with w rebuilt, negative when Flip.
    std::array<float, 4> Quat(int16_t X, int16_t Y, int16_t Z, bool Flip = false);
}
