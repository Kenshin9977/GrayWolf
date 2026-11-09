#pragma once

#include "ByteView.h"

// ResumeIndex - a zone's resume index (graywolf-tools docs/resume-index.md) is Wolff's
// (wolff/ResumeIndex.h): GrayWolf keeps one per zone under %TEMP%/greyhound_resume, written by a
// full open (ZonekitZone), read by a partial one, its types the zone's (ZonekitZone::ResumeTypes).
// The image heads beside it (<index>.gwh) are GrayWolf's own.

#include "DiskGame.h"
#include "MappedFile.h"

#include <wolff/ResumeIndex.h>

#include <cstdint>
#include <filesystem>
#include <map>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

using wolff::ZoneResume;

namespace ResumeIndex
{
    using wolff::ResumeIndex::FromWalk;
    using wolff::ResumeIndex::IsCurrent;
    using wolff::ResumeIndex::Load;
    using wolff::ResumeIndex::Named;
    using wolff::ResumeIndex::Save;

    // where GrayWolf keeps them: %TEMP%/greyhound_resume
    std::filesystem::path Dir();
    // the file of a zone there (wolff::ResumeIndex::FileFor)
    std::string FileFor(const std::string& ZonePath, const std::string& TablesKey, const std::vector<std::string>& Sources);
    std::string FileFor(const std::string& ZonePath, const std::string& TablesKey, ByteView Bytes);

    // an image's key in the heads: its kind and its name in lower case, hashed (XXH3 64), as the
    // resume index hashes a name of type id K
    inline uint64_t NameHash(DiskGame::Kind K, const std::string& Name) { return wolff::ResumeIndex::NameHash((uint32_t)K, Name); }

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
