#pragma once

#include "ByteView.h"

// ResumeIndex - a zone's resume index (graywolf-tools docs/resume-index.md): what opening a few
// assets of a zone takes instead of walking all of it. Each listed asset's resume point (where its
// walk starts, the stream memory's cursors there), the assets a run may ask for by kind and name ->
// the resume point holding them, and for the games that resolve by key or hash (Black Ops 6/7) the
// zone's keys and assets by hash. One file per zone, its tables and its bytes, under
// %TEMP%/greyhound_resume: written by a full open (ZonekitZone), read by a partial one.

#include "DiskGame.h"
#include "MappedFile.h"
#include "ZoneTableWalker.h"

#include <cstdint>
#include <map>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>
#include <vector>

struct ZoneResume
{
    uint64_t Size = 0;   // the zone's bytes (a zone beside is placed before they are read)
    std::vector<ZoneTables::ZoneResult::ResumePoint> Points;
    // (kind, NameHash) -> the resume point whose walk holds that asset, sorted
    std::vector<std::tuple<uint8_t, uint64_t, uint32_t>> Names;
    std::vector<std::pair<uint64_t, int64_t>> Keys;                  // keyed references, in stream order
    std::map<std::pair<std::string, uint64_t>, int64_t> ByHash;      // (type name, "" any; hash) -> asset
    // a type whose instances a game counts over the whole zone (ZonekitZone::Ranked) -> each one's
    // position, in walk order
    std::map<std::string, std::vector<int64_t>> Ranked;

    // the resume point of an asset by kind and name, SIZE_MAX when the zone lists none
    size_t Find(DiskGame::Kind K, const std::string& Name) const;
};

namespace ResumeIndex
{
    // an asset's key: its kind and its name in lower case, hashed (XXH3 64)
    uint64_t NameHash(DiskGame::Kind K, const std::string& Name);
    // the file of a zone: its name, the tables it was walked with and the files it is read from
    // (each one's path, size and time written), "" when one of them is not there
    std::string FileFor(const std::string& ZonePath, const std::string& TablesKey, const std::vector<std::string>& Sources);
    // the same, from the bytes walked (a zone read from inside an archive): their XXH3
    std::string FileFor(const std::string& ZonePath, const std::string& TablesKey, ByteView Bytes);
    bool Save(const std::string& File, const ZoneResume& Z);
    // false when missing, from another format or damaged (its checksum)
    bool Load(const std::string& File, ZoneResume& Z);
    // the file is there and of this format (its head alone read): else a full open writes it again
    // (with its heads beside it)
    bool IsCurrent(const std::string& File);

    // A zone's image records beside its resume index (<index>.gwh, ZonekitZone::Heads), sorted by
    // NameHash and read in place: each image's name and record as served
    std::string HeadsFile(const std::string& ResumeFile);
    bool SaveHeads(const std::string& File, const std::map<uint64_t, std::pair<std::string, std::string>>& Heads);
    class Heads
    {
    public:
        bool Open(const std::string& File);
        // the image of NameHash Hash: its name and its record; false when the zone keeps none
        bool Find(uint64_t Hash, std::string_view& Name, std::string_view& Record) const;

    private:
        MappedFile Map;
        uint64_t Count = 0;
    };
}
