#include "stdafx.h"

#include "XMEImport.h"

#include "Strings.h"
#include "Compression.h"

#include <format>
#include <sstream>

namespace
{
    // XMODEL_EXPORT is line oriented, and every line is a keyword then its arguments.
    // Comments start with // and blank lines carry no meaning.
    bool NextLine(std::istringstream& Stream, std::string& Line)
    {
        while (std::getline(Stream, Line))
        {
            // Strip the carriage return the writer leaves on Windows
            if (!Line.empty() && Line.back() == '\r')
            {
                Line.pop_back();
            }

            const auto Trimmed = Strings::Trim(Line);

            if (Trimmed.empty() || Trimmed.compare(0, 2, "//") == 0)
            {
                continue;
            }

            Line = Trimmed;
            return true;
        }

        return false;
    }

    // Splits a line into its keyword and the rest
    void SplitKeyword(const std::string& Line, std::string& Keyword, std::string& Rest)
    {
        const auto Space = Line.find(' ');

        if (Space == std::string::npos)
        {
            Keyword = Line;
            Rest.clear();
            return;
        }

        Keyword = Line.substr(0, Space);
        Rest = Line.substr(Space + 1);
    }

    // Pulls the text between the first pair of quotes
    bool Quoted(const std::string& Value, std::string& Out)
    {
        const auto Open = Value.find('"');
        if (Open == std::string::npos) return false;

        const auto Close = Value.find('"', Open + 1);
        if (Close == std::string::npos) return false;

        Out = Value.substr(Open + 1, Close - Open - 1);
        return true;
    }

    // Reads up to three floats from a line that separates them with commas
    size_t ReadTriple(const std::string& Value, float* Out)
    {
        auto Copy = Value;
        std::replace(Copy.begin(), Copy.end(), ',', ' ');

        std::istringstream Parts(Copy);
        size_t Read = 0;

        while (Read < 3 && (Parts >> Out[Read]))
        {
            Read++;
        }

        return Read;
    }
}

bool CodXMEReader::ReadText(const std::string& Text, XMEModel& Model, std::string& Error)
{
    Model = {};
    Error.clear();

    std::istringstream Stream(Text);
    std::string Line, Keyword, Rest;

    bool SawModel = false;
    // Which bone the OFFSET and SCALE lines that follow belong to
    int64_t CurrentBone = -1;
    bool BonesDone = false;

    while (NextLine(Stream, Line))
    {
        SplitKeyword(Line, Keyword, Rest);

        if (Keyword == "MODEL")
        {
            SawModel = true;
        }
        else if (Keyword == "VERSION")
        {
            Model.Version = (uint32_t)strtoul(Rest.c_str(), nullptr, 10);
        }
        else if (Keyword == "NUMBONES")
        {
            Model.Bones.resize((size_t)strtoul(Rest.c_str(), nullptr, 10));
        }
        else if (Keyword == "BONE" && !BonesDone)
        {
            // Two spellings share the keyword. The declaration carries a parent and a
            // quoted name, the later block is just an index followed by OFFSET and SCALE.
            std::string Name;

            if (Quoted(Rest, Name))
            {
                std::istringstream Parts(Rest);
                int64_t Index = -1, Parent = -1;
                Parts >> Index >> Parent;

                if (Index < 0 || (size_t)Index >= Model.Bones.size())
                {
                    Error = std::format("bone index {} is outside the declared {} bones",
                        (long long)Index, (unsigned long long)Model.Bones.size());
                    return false;
                }

                Model.Bones[(size_t)Index].Name = Name;
                Model.Bones[(size_t)Index].Parent = (int32_t)Parent;
            }
            else
            {
                CurrentBone = strtoll(Rest.c_str(), nullptr, 10);

                if (CurrentBone < 0 || (size_t)CurrentBone >= Model.Bones.size())
                {
                    Error = std::format("bone block {} is outside the declared {} bones",
                        (long long)CurrentBone, (unsigned long long)Model.Bones.size());
                    return false;
                }
            }
        }
        else if (Keyword == "OFFSET" && CurrentBone >= 0)
        {
            ReadTriple(Rest, Model.Bones[(size_t)CurrentBone].Offset);
        }
        else if (Keyword == "SCALE" && CurrentBone >= 0)
        {
            ReadTriple(Rest, Model.Bones[(size_t)CurrentBone].Scale);
        }
        else if ((Keyword == "X" || Keyword == "Y" || Keyword == "Z") && CurrentBone >= 0)
        {
            ReadTriple(Rest, Model.Bones[(size_t)CurrentBone].Axes[Keyword[0] - 'X']);
        }
        else if (Keyword == "NUMVERTS" || Keyword == "NUMVERTS32")
        {
            Model.VertexCount = (uint32_t)strtoul(Rest.c_str(), nullptr, 10);
            // the vertices' OFFSET and BONE lines are not the bones' any more
            CurrentBone = -1;
            BonesDone = true;
        }
        else if (Keyword == "NUMFACES")
        {
            Model.FaceCount = (uint32_t)strtoul(Rest.c_str(), nullptr, 10);
        }
        else if (Keyword == "MATERIAL")
        {
            std::string Name;
            if (Quoted(Rest, Name))
            {
                Model.Materials.push_back(Name);
            }
        }
    }

    if (!SawModel)
    {
        Error = "no MODEL keyword, this is not an XMODEL_EXPORT";
        return false;
    }

    return true;
}

bool CodXMBReader::ReadFile(const std::string& FileName, std::vector<uint8_t>& Contents, std::string& Error)
{
    Contents.clear();
    Error.clear();

    // "*LZ4*"
    static const uint8_t Magic[5] = { 0x2A, 0x4C, 0x5A, 0x34, 0x2A };
    constexpr size_t HeaderSize = sizeof(Magic) + sizeof(uint32_t);

    FILE* Raw = nullptr;
    if (fopen_s(&Raw, FileName.c_str(), "rb") != 0 || Raw == nullptr)
    {
        Error = "could not open the file";
        return false;
    }

    auto Owned = std::unique_ptr<FILE, decltype(&fclose)>(Raw, fclose);

    if (_fseeki64(Owned.get(), 0, SEEK_END) != 0)
    {
        Error = "could not measure the file";
        return false;
    }

    const auto Length = _ftelli64(Owned.get());

    if (Length <= (int64_t)HeaderSize)
    {
        Error = "file is shorter than its header";
        return false;
    }

    std::vector<uint8_t> File((size_t)Length);
    _fseeki64(Owned.get(), 0, SEEK_SET);

    if (fread(File.data(), 1, File.size(), Owned.get()) != File.size())
    {
        Error = "could not read the whole file";
        return false;
    }

    if (std::memcmp(File.data(), Magic, sizeof(Magic)) != 0)
    {
        Error = "not an XMODEL_BIN, the magic does not match";
        return false;
    }

    uint32_t Decompressed = 0;
    std::memcpy(&Decompressed, File.data() + sizeof(Magic), sizeof(Decompressed));

    if (Decompressed == 0)
    {
        Error = "the header declares an empty payload";
        return false;
    }

    Contents.resize(Decompressed);

    const auto Produced = Compression::DecompressLZ4Block(
        (const int8_t*)(File.data() + HeaderSize),
        (int8_t*)Contents.data(),
        (int32_t)(File.size() - HeaderSize),
        (int32_t)Decompressed);

    // Getting a different size back means the block was not what the header said it was,
    // which is worth failing on rather than handing back a half filled buffer
    if (Produced != Decompressed)
    {
        Error = std::format("payload decompressed to {} bytes, the header declares {}",
            Produced, Decompressed);
        Contents.clear();
        return false;
    }

    return true;
}

bool CodXMEReader::ReadFile(const std::string& FileName, XMEModel& Model, std::string& Error)
{
    FILE* Raw = nullptr;
    if (fopen_s(&Raw, FileName.c_str(), "rb") != 0 || Raw == nullptr)
    {
        Error = "could not open the file";
        return false;
    }

    auto Owned = std::unique_ptr<FILE, decltype(&fclose)>(Raw, fclose);

    if (_fseeki64(Owned.get(), 0, SEEK_END) != 0)
    {
        Error = "could not measure the file";
        return false;
    }

    const auto Length = _ftelli64(Owned.get());
    if (Length <= 0)
    {
        Error = "the file is empty";
        return false;
    }

    std::string Text;
    Text.resize((size_t)Length);
    _fseeki64(Owned.get(), 0, SEEK_SET);

    if (fread(&Text[0], 1, Text.size(), Owned.get()) != Text.size())
    {
        Error = "could not read the whole file";
        return false;
    }

    return ReadText(Text, Model, Error);
}
