#pragma once

// "ADM3" sound objects: Crankcase Audio REV engine models (the ".mod" files REV.Tool builds), the
// recordings of a vehicle's engine a granular synthesizer plays by RPM and load. Cold War's
// sound_asset format 11 (v6), MWIII's sndasset format 2 and MW4's .xsub objects (v7). The layout
// field by field: graywolf-tools docs/games/T9.md, "ADM3 engine models".

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <map>
#include <string>
#include <vector>

namespace Adm3
{
    // Where the "ADM3" tag is in Data (at most 0x200 bytes in), -1 when none
    int64_t Find(const uint8_t* Data, size_t Size);

    // a recording (SMP2): a loop at one RPM, or a sweep the grains are cut from
    struct Clip
    {
        uint32_t At = 0;
        float Freq = 0;            // its firing frequency, Hz (0: a sweep)
        uint32_t Channels = 0, RpmBucket = 0, Rate = 0, Frames = 0, Bytes = 0, Offset = 0;
        uint16_t Version = 0;
        std::string Name;          // its file's stem
        bool Decodable = false;    // its bytes are the blocks its frames need
    };
    // an RMP1 (on or off load) with its SMB1 (loops, grain markers) and GRN1 (grains)
    struct Ramp
    {
        uint32_t At = 0, Smb = 0, Grn = 0;
        float From = 0, To = 0;
        std::vector<uint32_t> Loops;          // SMP2 offsets
        uint32_t Sweep = 0;                   // its SMP2 offset (0 none)
        uint32_t GrainCount = 0, GrainFlag = 0, Rate = 0, MarkersAt = 0;
        float FreqMin = 0, FreqMax = 0, GrainValue = 0;
        std::vector<uint32_t> GrainWords;     // the GRN1's {1, 0x24} record
        struct Marker { uint16_t Index, Period; float Freq, Position; };
        std::vector<Marker> Markers;
    };
    struct Model
    {
        uint16_t Version = 0;
        uint32_t DataOffset = 0;
        std::vector<uint32_t> HeaderWords;    // 0x14 .. 0xf0: the engine's parameters
        std::vector<uint32_t> Top;            // the header's refs
        std::vector<Clip> Clips;
        std::vector<Ramp> Ramps;
    };
    // The object's tables (Data from its tag); false when it is no ADM3 this reads
    bool Parse(const uint8_t* Data, size_t Size, Model& Out);
    // A clip's frames, interleaved 16-bit (IMA ADPCM); empty when its blocks are not there
    std::vector<int16_t> Decode(const uint8_t* Data, size_t Size, uint16_t Version, const Clip& C);

    // Data (from its tag or before it) written under Dir: each clip as <Stem>/<clip><ext> in the
    // audio format setting, the tables as <Stem>.adm3.json; an object not read is written as it is,
    // <Stem>.adm3.bin. Counted in Counters (sounds_adm3, sounds_adm3_clips, sounds_adm3_raw).
    bool Write(const uint8_t* Data, size_t Size, const std::filesystem::path& Dir, const std::string& Stem, std::map<std::string, size_t>& Counters);
}
