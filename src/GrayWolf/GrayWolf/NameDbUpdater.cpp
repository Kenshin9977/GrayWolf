#include "stdafx.h"

#include "NameDbUpdater.h"

#include "WebClient.h"
#include "WraithNameIndex.h"

#include <cstdlib>
#include <filesystem>
#include <format>
#include <fstream>
#include <vector>

namespace NameDbUpdater
{
    struct Mapping
    {
        const char* CsvName; // file stem under csv/ in the repo, no extension
        const char* WniName; // file stem this codebase's LoadIndex calls already expect
    };

    // Only the CSVs cross-checked against a WraithNameIndex::LoadIndex call: the names follow no
    // pattern ("_v2" suffix vs. "v2_" prefix, "fnv1a_string" vs. "fnv1a_strings").
    static const Mapping Mappings[] =
    {
        { "fnv1a_xanims",           "fnv1a_xanims" },
        { "fnv1a_ximages",          "fnv1a_ximages" },
        { "fnv1a_xmaterials",       "fnv1a_xmaterials" },
        { "fnv1a_xmodels",          "fnv1a_xmodels" },
        { "fnv1a_xsounds",          "fnv1a_xsounds" },
        { "fnv1a_bones",            "fnv1a_bones" },
        { "fnv1a_strings",          "fnv1a_string" },
        { "fnv1a_xanims_v2",        "fnv1a_v2_xanims" },
        { "fnv1a_ximages_v2",       "fnv1a_v2_ximages" },
        { "fnv1a_xmaterials_v2",    "fnv1a_v2_xmaterials" },
        { "bo2_ipak",               "bo2_ipak" },
        { "bo2_sab",                "bo2_sab" },
        { "bo3_sab",                "bo3_sab" },
    };

    // "d4f6818e,diffuseFilter0\n..." - one entry per line, no header row, hash in hex without a
    // leading "0x".
    static size_t ParseCsvInto(const uint8_t* Data, size_t Size, WraithNameIndex& Index)
    {
        size_t Entries = 0;
        size_t LineStart = 0;

        for (size_t i = 0; i <= Size; i++)
        {
            if (i == Size || Data[i] == '\n')
            {
                size_t LineEnd = i;
                // Trim a trailing \r (the repo's CSVs may be CRLF)
                if (LineEnd > LineStart && Data[LineEnd - 1] == '\r')
                    LineEnd--;

                if (LineEnd > LineStart)
                {
                    size_t Comma = std::string::npos;
                    for (size_t j = LineStart; j < LineEnd; j++)
                    {
                        if (Data[j] == ',') { Comma = j; break; }
                    }

                    if (Comma != std::string::npos && Comma + 1 < LineEnd)
                    {
                        std::string HashHex((const char*)Data + LineStart, Comma - LineStart);
                        std::string Name((const char*)Data + Comma + 1, LineEnd - Comma - 1);

                        auto Hash = std::strtoull(HashHex.c_str(), nullptr, 16);
                        if (Hash != 0 && !Name.empty())
                        {
                            Index.NameDatabase[Hash] = Name;
                            Entries++;
                        }
                    }
                }

                LineStart = i + 1;
            }
        }

        return Entries;
    }

    // The names a .wni already holds, whole 64-bit keys: a CSV is merged over them (its name wins
    // for a hash both give), so the names of a hand-made base package are kept
    static WraithNameIndex Existing(const std::string& WniPath)
    {
        WraithNameIndex Index;
        Index.LoadIndex(WniPath, ~0ull);
        return Index;
    }

    FileResult ConvertLocalCsv(const std::string& CsvPath, const std::string& WniName,
                              const std::string& OutputDir)
    {
        std::error_code Ec;
        FileResult Result;
        Result.WniName = WniName;

        if (!std::filesystem::is_directory(OutputDir, Ec))
        {
            std::filesystem::create_directories(OutputDir, Ec);
        }

        std::ifstream In(CsvPath, std::ios::binary | std::ios::ate);
        if (!In)
        {
            Result.Error = "could not open " + CsvPath;
            return Result;
        }
        const auto Size = (size_t)In.tellg();
        In.seekg(0);
        std::vector<uint8_t> Data(Size);
        In.read((char*)Data.data(), (std::streamsize)Size);
        if (!In)
        {
            Result.Error = "short read on " + CsvPath;
            return Result;
        }
        Result.Downloaded = true;  // "acquired", same meaning Update's own field carries

        const auto WniPath = (std::filesystem::path(OutputDir) / (WniName + std::string(".wni"))).string();
        WraithNameIndex Index = Existing(WniPath);
        Result.Entries = ParseCsvInto(Data.data(), Data.size(), Index);
        if (Result.Entries == 0)
        {
            Result.Error = "no entries parsed";
            return Result;
        }

        Index.SaveIndex(WniPath);
        Result.Converted = std::filesystem::is_regular_file(WniPath, Ec);
        if (!Result.Converted)
        {
            Result.Error = "failed to write .wni";
        }

        return Result;
    }

    std::vector<FileResult> Update(const std::string& OutputDir)
    {
        std::error_code Ec;
        std::vector<FileResult> Results;

        if (!std::filesystem::is_directory(OutputDir, Ec))
        {
            std::filesystem::create_directories(OutputDir, Ec);
        }

        WebClient Client;
        Client.SetTimeout(60000);

        for (const auto& Map : Mappings)
        {
            FileResult Result;
            Result.WniName = Map.WniName;

            const auto Url = std::format(
                "https://raw.githubusercontent.com/echo000/cod-name-db/main/csv/{}.csv", Map.CsvName);

            auto Data = Client.DownloadData(Url);
            if (!Data || Data->empty())
            {
                Result.Error = "download failed";
                Results.push_back(Result);
                continue;
            }
            Result.Downloaded = true;

            const auto WniPath = (std::filesystem::path(OutputDir) / (Map.WniName + std::string(".wni"))).string();
            WraithNameIndex Index = Existing(WniPath);
            Result.Entries = ParseCsvInto(Data->data(), Data->size(), Index);

            if (Result.Entries == 0)
            {
                Result.Error = "no entries parsed";
                Results.push_back(Result);
                continue;
            }

            Index.SaveIndex(WniPath);
            Result.Converted = std::filesystem::is_regular_file(WniPath, Ec);
            if (!Result.Converted)
            {
                Result.Error = "failed to write .wni";
            }

            Results.push_back(Result);
        }

        return Results;
    }
}
