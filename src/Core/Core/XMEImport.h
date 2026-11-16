#pragma once

#include <cstdint>
#include <string>
#include <vector>

// One bone as an XMODEL_EXPORT file records it
struct XMEBone
{
    std::string Name;
    int32_t Parent = -1;
    float Offset[3] = {};
    float Scale[3] = { 1.0f, 1.0f, 1.0f };
    // the X, Y and Z lines: the bone's axes in model space (identity when the file has none)
    float Axes[3][3] = { { 1, 0, 0 }, { 0, 1, 0 }, { 0, 0, 1 } };
};

// What could be read back out of an XMODEL_EXPORT file
struct XMEModel
{
    uint32_t Version = 0;
    std::vector<XMEBone> Bones;
    uint32_t VertexCount = 0;
    uint32_t FaceCount = 0;
    std::vector<std::string> Materials;
};

// Reads the text form of Call of Duty's XMODEL_EXPORT - a port of the reader side of
// Scobalula's CallOfFile (MIT), used to round-trip test the writer. Binary XMODEL_BIN is not
// handled here.
class CodXMEReader
{
public:

    // Reads a file. Returns false and fills Error when the file is not a model we know.
    static bool ReadFile(const std::string& FileName, XMEModel& Model, std::string& Error);

    // Reads from memory, which is what the round trip test uses
    static bool ReadText(const std::string& Text, XMEModel& Model, std::string& Error);
};

// Reads the binary form, XMODEL_BIN: a "*LZ4*" magic + decompressed size + one LZ4 block,
// containing the same model as hash-tagged blocks rather than text. That inner stream isn't
// decoded yet - this just unpacks the container and hands back the raw bytes.
class CodXMBReader
{
public:

    // Unpacks the container. Returns false and fills Error if the magic is wrong or the
    // block does not decompress to the size the header promises.
    static bool ReadFile(const std::string& FileName, std::vector<uint8_t>& Contents, std::string& Error);
};
