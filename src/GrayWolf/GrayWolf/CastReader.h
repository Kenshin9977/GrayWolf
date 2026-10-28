#pragma once

#include "ByteView.h"

#include <cstdint>
#include <map>
#include <string>
#include <vector>

struct SeAnimClip;

// Reads Cast files (github.com/dtzxporter/cast: the format CastExport.cpp writes, and Blender's or
// Maya's Cast plugins too): the node tree, each property a view into the file's bytes. The preview
// (ModelPreview.cpp), the asset browser and the asset index read GrayWolf's exports through it.
namespace CastFile
{
    // Node ids (CastNodeId in CastExport.h)
    enum : uint32_t
    {
        IdRoot = 0x746F6F72, IdModel = 0x6C646F6D, IdMesh = 0x6873656D, IdBlendShape = 0x68736C62,
        IdSkeleton = 0x6C656B73, IdBone = 0x656E6F62, IdAnimation = 0x6D696E61, IdCurve = 0x76727563,
        IdCurveModeOverride = 0x564F4D43, IdNotificationTrack = 0x6669746E, IdMaterial = 0x6C74616D,
        IdFile = 0x656C6966,
    };

    struct Property
    {
        uint16_t Type = 0;            // 'b' 'h' 'i' 'l' 'f' 'd' 's' 'v2' 'v3' 'v4' (CastPropertyId)
        uint32_t Count = 0;           // elements (a vector is one)
        const uint8_t* Data = nullptr;
        size_t Bytes = 0;

        // Element I of an integer property (b, h, i, l), 0 past its end
        uint64_t Integer(size_t I = 0) const;
        // The I-th float of a float or vector property (a v3's element E is floats 3E..3E+2), 0 past its end
        float Float(size_t I) const;
        size_t Floats() const;
        std::string String() const;
    };

    struct Node
    {
        uint32_t Id = 0;
        uint64_t Hash = 0;
        std::map<std::string, Property> Properties;
        std::vector<Node> Children;

        const Property* Find(const std::string& Name) const;
        std::string String(const std::string& Name) const;   // "" when absent
        uint64_t Integer(const std::string& Name, uint64_t Default = 0) const;
    };

    // The file's root nodes; false on a bad magic or anything past the end of Bytes
    bool Parse(ByteView Bytes, std::vector<Node>& Roots);

    enum class Kind { None, Model, Animation };
    // What a file holds (its first Model or Animation node), read from the node headers only
    Kind FileKind(const std::string& Path);
    // A model file's bone, mesh and material counts, from the node headers only
    bool ModelCounts(const std::string& Path, uint32_t& Bones, uint32_t& Meshes, uint32_t& Materials);
}

// A .cast animation as the preview's clip: per bone its rotation (rq), translation (tx/ty/tz) and
// scale (sx/sy/sz) keys (channels keyed apart are merged, a missing one interpolated), the mode of
// its translation curves or of a CurveModeOverride naming it, the notetracks. False when the file
// holds no animation.
bool ReadCastAnim(const std::string& Path, SeAnimClip& Out);

// A .cast or a .seanim, told apart by its magic
bool ReadAnimClip(const std::string& Path, SeAnimClip& Out);
