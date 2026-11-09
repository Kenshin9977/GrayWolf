#include "stdafx.h"

#include "ResumeIndex.h"
#include "FileSystems.h"

#include <algorithm>
#include <cstring>
#include <filesystem>

namespace fs = std::filesystem;

namespace
{
    constexpr char HeadsMagic[8] = { 'G', 'W', 'H', 'E', 'A', 'D', '1', '\n' };
#pragma pack(push, 1)
    struct HeadRow
    {
        uint64_t Hash;
        uint64_t At;   // its name then its record, from the file's start
        uint32_t NameLength;
        uint32_t RecordLength;
    };
#pragma pack(pop)
}

namespace ResumeIndex
{
    fs::path Dir()
    {
        std::error_code Ec;
        return fs::temp_directory_path(Ec) / "greyhound_resume";
    }

    std::string FileFor(const std::string& ZonePath, const std::string& TablesKey, const std::vector<std::string>& Sources)
    {
        return wolff::ResumeIndex::FileFor(Dir(), ZonePath, TablesKey, Sources);
    }

    std::string FileFor(const std::string& ZonePath, const std::string& TablesKey, ByteView Bytes)
    {
        return wolff::ResumeIndex::FileFor(Dir(), ZonePath, TablesKey, Bytes);
    }

    std::string HeadsFile(const std::string& ResumeFile)
    {
        return fs::path(ResumeFile).replace_extension(".gwh").string();
    }

    // the magic, u64 count, the rows by hash, then each image's name and record
    bool SaveHeads(const std::string& File, const std::map<uint64_t, std::pair<std::string, std::string>>& Heads)
    {
        return FileSystems::WriteReplacing(File, [&](std::ostream& Out) {
            Out.write(HeadsMagic, sizeof(HeadsMagic));
            const uint64_t Count = Heads.size();
            Out.write(reinterpret_cast<const char*>(&Count), 8);
            uint64_t At = sizeof(HeadsMagic) + 8 + Count * sizeof(HeadRow);
            for (const auto& [Hash, Head] : Heads)
            {
                const HeadRow Row{ Hash, At, (uint32_t)Head.first.size(), (uint32_t)Head.second.size() };
                Out.write(reinterpret_cast<const char*>(&Row), sizeof(Row));
                At += Head.first.size() + Head.second.size();
            }
            for (const auto& [Hash, Head] : Heads)
            {
                Out.write(Head.first.data(), (std::streamsize)Head.first.size());
                Out.write(Head.second.data(), (std::streamsize)Head.second.size());
            }
        });
    }

    bool Heads::Open(const std::string& File)
    {
        if (!Map.Open(File) || Map.size() < sizeof(HeadsMagic) + 8 || std::memcmp(Map.data(), HeadsMagic, sizeof(HeadsMagic)) != 0) return false;
        std::memcpy(&Count, Map.data() + sizeof(HeadsMagic), 8);
        return Count <= (Map.size() - sizeof(HeadsMagic) - 8) / sizeof(HeadRow);
    }

    bool Heads::Find(uint64_t Hash, std::string_view& Name, std::string_view& Record) const
    {
        const HeadRow* Rows = reinterpret_cast<const HeadRow*>(Map.data() + sizeof(HeadsMagic) + 8);
        const HeadRow* End = Rows + Count;
        const HeadRow* It = std::lower_bound(Rows, End, Hash, [](const HeadRow& R, uint64_t H) { return R.Hash < H; });
        if (It == End || It->Hash != Hash || It->At + It->NameLength + It->RecordLength > Map.size()) return false;
        const char* At = reinterpret_cast<const char*>(Map.data() + It->At);
        Name = std::string_view(At, It->NameLength);
        Record = std::string_view(At + It->NameLength, It->RecordLength);
        return true;
    }
}
