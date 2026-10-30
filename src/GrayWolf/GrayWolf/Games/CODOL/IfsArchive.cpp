#include "stdafx.h"
#include "ByteView.h"

#include "Games/CODOL/IfsArchive.h"
#include "ZipInflate.h"

#include <miniz/miniz.h>

#include <algorithm>
#include <cstring>
#include <filesystem>
#include <memory>
#include <regex>

namespace
{
    constexpr uint32_t FlagExists = 0x80000000u;
    constexpr uint32_t FlagCompress = 0x200u;
    constexpr uint32_t FlagEncrypted = 0x10000u;
    constexpr uint32_t FlagFixKey = 0x20000u;
    constexpr uint32_t FlagsHandled = FlagExists | FlagCompress | FlagEncrypted | FlagFixKey;
    constexpr uint8_t SectorZlib = 2;

    const uint32_t* CryptTable()
    {
        static uint32_t Table[0x500];
        static const bool Built = []
        {
            uint32_t Seed = 0x100001;
            for (uint32_t I = 0; I < 0x100; I++)
            {
                for (uint32_t J = 0; J < 5; J++)
                {
                    Seed = (Seed * 125 + 3) % 0x2AAAAB;
                    const uint32_t High = (Seed & 0xFFFF) << 16;
                    Seed = (Seed * 125 + 3) % 0x2AAAAB;
                    Table[0x100 * J + I] = High | (Seed & 0xFFFF);
                }
            }
            return true;
        }();
        (void)Built;
        return Table;
    }

    uint32_t HashString(const std::string& S, uint32_t Offset)
    {
        const auto* T = CryptTable();
        uint32_t Hash = 0x7FED7FED, Seed = 0xEEEEEEEE;
        for (const char Ch : S)
        {
            auto B = static_cast<uint8_t>(Ch);
            if (B >= 0x80) B = '?';
            if (B > 0x60 && B < 0x7B) B -= 0x20;
            Hash = T[Offset + B] ^ (Hash + Seed);
            Seed = Seed + Hash + (Seed << 5) + B + 3;
        }
        return Hash;
    }

    // MPQ's block cipher, over the whole dwords only: a trailing 1-3 bytes stay as they are
    void DecryptBlock(uint8_t* Data, size_t Length, uint32_t Key)
    {
        const auto* T = CryptTable();
        uint32_t Temp = 0xEEEEEEEE;
        for (size_t I = 0; I + 4 <= Length; I += 4)
        {
            uint32_t V;
            std::memcpy(&V, Data + I, 4);
            Temp += T[0x400 + (Key & 0xFF)];
            V ^= Key + Temp;
            Temp += V + (Temp << 5) + 3;
            std::memcpy(Data + I, &V, 4);
            Key = (Key >> 11) | (0x11111111 + ((Key ^ 0x7FF) << 21));
        }
    }

    uint32_t Rot(uint32_t X, int K) { return (X << K) | (X >> (32 - K)); }

    // Bob Jenkins' lookup3 hashlittle2
    void HashLittle2(const uint8_t* K, size_t Length, uint32_t& Pc, uint32_t& Pb)
    {
        uint32_t A, B, C;
        A = B = C = 0xDEADBEEF + static_cast<uint32_t>(Length) + Pc;
        C += Pb;
        auto Word = [&](size_t At)
        {
            uint32_t V;
            std::memcpy(&V, K + At, 4);
            return V;
        };
        size_t At = 0;
        while (Length > 12)
        {
            A += Word(At); B += Word(At + 4); C += Word(At + 8);
            A -= C; A ^= Rot(C, 4); C += B;
            B -= A; B ^= Rot(A, 6); A += C;
            C -= B; C ^= Rot(B, 8); B += A;
            A -= C; A ^= Rot(C, 16); C += B;
            B -= A; B ^= Rot(A, 19); A += C;
            C -= B; C ^= Rot(B, 4); B += A;
            Length -= 12;
            At += 12;
        }
        if (Length == 0)
        {
            Pc = C;
            Pb = B;
            return;
        }
        uint8_t Tail[12] = {};
        std::memcpy(Tail, K + At, Length);
        uint32_t W[3];
        std::memcpy(W, Tail, 12);
        A += W[0]; B += W[1]; C += W[2];
        C ^= B; C -= Rot(B, 14);
        A ^= C; A -= Rot(C, 11);
        B ^= A; B -= Rot(A, 25);
        C ^= B; C -= Rot(B, 16);
        A ^= C; A -= Rot(C, 4);
        B ^= A; B -= Rot(A, 14);
        C ^= B; C -= Rot(B, 24);
        Pc = C;
        Pb = B;
    }

    uint64_t NameHash(const std::string& Name)
    {
        std::string S = Name;
        for (auto& Ch : S)
        {
            Ch = static_cast<char>(std::tolower(static_cast<unsigned char>(Ch)));
            if (Ch == '/') Ch = '\\';
        }
        uint32_t Pc = 2, Pb = 1;
        HashLittle2(reinterpret_cast<const uint8_t*>(S.data()), S.size(), Pc, Pb);
        return Pc | (static_cast<uint64_t>(Pb) << 32);
    }

    uint64_t Bits(ByteView Buf, uint64_t Bit, uint32_t Count)
    {
        uint64_t V = 0;
        for (uint32_t J = 0; J < Count; J++, Bit++)
        {
            if ((Bit >> 3) < Buf.size() && ((Buf[(size_t)(Bit >> 3)] >> (Bit & 7)) & 1) != 0)
                V |= 1ull << J;
        }
        return V;
    }

    using FileHandle = std::unique_ptr<FILE, decltype(&fclose)>;

    FileHandle OpenRead(const std::string& Path)
    {
        FILE* Raw = nullptr;
        if (fopen_s(&Raw, Path.c_str(), "rb") != 0)
            Raw = nullptr;
        return FileHandle(Raw, fclose);
    }

    bool ReadAt(FILE* F, uint64_t Pos, void* Out, size_t Length)
    {
        return _fseeki64(F, static_cast<int64_t>(Pos), SEEK_SET) == 0 && fread(Out, 1, Length, F) == Length;
    }

    // a HET or BET table: 12 bytes (magic, version, size) then its data, encrypted
    bool ReadTable(FILE* F, uint64_t Pos, const char* KeyName, std::vector<uint8_t>& Out)
    {
        uint32_t Head[3] = {};
        if (!ReadAt(F, Pos, Head, sizeof(Head)) || Head[2] > (64u << 20))
            return false;
        Out.resize(Head[2]);
        if (fread(Out.data(), 1, Out.size(), F) != Out.size())
            return false;
        DecryptBlock(Out.data(), Out.size(), HashString(KeyName, 0x300));
        return true;
    }

    std::vector<std::string> Lines(ByteView Text)
    {
        std::vector<std::string> Out;
        std::string Line;
        for (const uint8_t Ch : Text)
        {
            if (Ch == '\n' || Ch == '\r')
            {
                if (!Line.empty()) Out.push_back(Line);
                Line.clear();
            }
            else
            {
                Line.push_back(static_cast<char>(Ch));
            }
        }
        if (!Line.empty()) Out.push_back(Line);
        return Out;
    }

    // "_V17.1.ifs" -> {17, 1}; an archive with no version is {0}
    std::vector<uint32_t> ArchiveVersion(const std::string& FileName)
    {
        static const std::regex Pattern(R"(_V([0-9.]+)\.ifs$)", std::regex::icase);
        std::smatch M;
        std::vector<uint32_t> V;
        if (std::regex_search(FileName, M, Pattern))
        {
            const std::string Digits = M[1].str();
            size_t Start = 0;
            while (Start <= Digits.size())
            {
                const auto Dot = Digits.find('.', Start);
                const auto Part = Digits.substr(Start, Dot == std::string::npos ? std::string::npos : Dot - Start);
                if (!Part.empty()) V.push_back(static_cast<uint32_t>(std::stoul(Part)));
                if (Dot == std::string::npos) break;
                Start = Dot + 1;
            }
        }
        if (V.empty()) V.push_back(0);
        return V;
    }
}

namespace Ifs
{
    bool Archive::Open(const std::string& FilePath, std::string& Error)
    {
        File = FilePath;
        auto F = OpenRead(FilePath);
        if (!F)
        {
            Error = "could not open " + FilePath;
            return false;
        }
        uint8_t Head[0x60] = {};
        if (!ReadAt(F.get(), 0, Head, sizeof(Head)) || std::memcmp(Head, "nifs", 4) != 0)
        {
            Error = FilePath + ": not an ifs archive";
            return false;
        }
        uint16_t SectorShift = 0;
        uint64_t BetPos = 0, HetPos = 0;
        std::memcpy(&HeaderSize, Head + 4, 4);
        std::memcpy(&SectorShift, Head + 0xA, 2);
        std::memcpy(&BetPos, Head + 0x14, 8);
        std::memcpy(&HetPos, Head + 0x1C, 8);
        SectorSize = 0x200u << SectorShift;

        std::vector<uint8_t> Het, Bet;
        if (!ReadTable(F.get(), HetPos, "(hash table)", Het) || !ReadTable(F.get(), BetPos, "(block table)", Bet) ||
            Het.size() < 32 || Bet.size() < 21 * 4)
        {
            Error = FilePath + ": unreadable hash or block table";
            return false;
        }
        uint32_t HetFields[8], BetFields[21];
        std::memcpy(HetFields, Het.data(), sizeof(HetFields));
        std::memcpy(BetFields, Bet.data(), sizeof(BetFields));
        const uint32_t HashBits = HetFields[3];
        AndMask = HashBits >= 64 ? ~0ull : (1ull << HashBits) - 1;
        OrMask = 1ull << (HashBits - 1);

        const uint32_t Count = BetFields[1], EntryBits = BetFields[2];
        const uint32_t BitsPos = BetFields[9], BitsSize = BetFields[10], BitsStored = BetFields[11];
        const uint32_t BitsFlags = BetFields[12], BitsHash = BetFields[13];
        const uint32_t HashTotal = BetFields[15], HashArray = BetFields[20];
        // each field is a number of at most 64 bits within its entry: other widths are a corrupt table
        if (BitsPos > 64 || BitsSize > 64 || BitsStored > 64 || BitsFlags > 64 || HashTotal > 64 || HashBits == 0 || HashBits > 64 ||
            (uint64_t)BitsPos + BitsSize + BitsStored + BitsFlags > EntryBits)
        {
            Error = FilePath + ": block table with impossible field widths";
            return false;
        }
        const size_t TableBytes = ((size_t)EntryBits * Count + 7) / 8;
        const size_t HashBytes = ((size_t)HashTotal * Count + 7) / 8;
        if (21 * 4 + TableBytes + HashBytes > Bet.size())
        {
            Error = FilePath + ": block table shorter than its entries";
            return false;
        }
        const std::vector<uint8_t> Table(Bet.begin() + 21 * 4, Bet.begin() + 21 * 4 + TableBytes);
        const std::vector<uint8_t> Hashes(Bet.begin() + 21 * 4 + TableBytes, Bet.begin() + 21 * 4 + TableBytes + HashBytes);
        uint64_t Bit = 0, HashBit = 0;
        for (uint32_t I = 0; I < Count; I++)
        {
            Entry E;
            E.Pos = Bits(Table, Bit, BitsPos); Bit += BitsPos;
            E.Size = Bits(Table, Bit, BitsSize); Bit += BitsSize;
            E.StoredSize = Bits(Table, Bit, BitsStored); Bit += BitsStored;
            E.Flags = static_cast<uint32_t>(Bits(Table, Bit, BitsFlags)); Bit += BitsFlags;
            Bit += BitsHash + HashArray;
            const uint64_t Hash = Bits(Hashes, HashBit, HashTotal); HashBit += HashTotal;
            if ((E.Flags & FlagExists) != 0)
                ByHash[Hash] = E;
        }

        // a base archive also stores a plain "<archive>.lst" first; the MPQ listfile is in every one
        Entry List;
        if (!Find("(listfile)", List))
            return true;
        List.Name = "(listfile)";
        std::vector<uint8_t> Text;
        if (!Read(List, Text, Error))
            return false;
        for (const auto& Line : Lines(Text))
        {
            Entry E;
            if (Find(Line, E) && E.Size > 0)
            {
                E.Name = Line;
                Named.push_back(E);
            }
        }
        return true;
    }

    bool Archive::Find(const std::string& Name, Entry& Out) const
    {
        const uint64_t Key = ((NameHash(Name) & AndMask) | OrMask) & (AndMask >> 8);
        const auto It = ByHash.find(Key);
        if (It == ByHash.end())
            return false;
        Out = It->second;
        return true;
    }

    bool Archive::Read(const Entry& E, std::vector<uint8_t>& Out, std::string& Error) const
    {
        Out.clear();
        if ((E.Flags & ~FlagsHandled) != 0)
        {
            Error = E.Name + ": archive flags not handled";
            return false;
        }
        auto F = OpenRead(File);
        std::vector<uint8_t> Raw((size_t)E.StoredSize);
        if (!F || !ReadAt(F.get(), E.Pos, Raw.data(), Raw.size()))
        {
            Error = E.Name + ": could not read " + File;
            return false;
        }
        if ((E.Flags & (FlagCompress | FlagEncrypted)) == 0)
        {
            Raw.resize((size_t)std::min<uint64_t>(E.Size, Raw.size()));
            Out = std::move(Raw);
            return true;
        }

        uint32_t Key = 0;
        if ((E.Flags & FlagEncrypted) != 0)
        {
            auto Base = E.Name;
            const auto Slash = Base.find_last_of("/\\");
            if (Slash != std::string::npos) Base = Base.substr(Slash + 1);
            Key = HashString(Base, 0x300);
            if ((E.Flags & FlagFixKey) != 0)
                Key = (Key + static_cast<uint32_t>(E.Pos)) ^ static_cast<uint32_t>(E.Size);
        }
        const size_t Sectors = (size_t)((E.Size + SectorSize - 1) / SectorSize);
        if ((E.Flags & FlagCompress) == 0)
        {
            for (size_t I = 0; I < Sectors; I++)
            {
                const size_t At = I * SectorSize;
                if (At >= Raw.size()) break;
                DecryptBlock(Raw.data() + At, std::min<size_t>(SectorSize, Raw.size() - At), Key + (uint32_t)I);
            }
            Raw.resize((size_t)std::min<uint64_t>(E.Size, Raw.size()));
            Out = std::move(Raw);
            return true;
        }

        std::vector<uint32_t> Offsets(Sectors + 1);
        if (Raw.size() < Offsets.size() * 4)
        {
            Error = E.Name + ": sector table past the file";
            return false;
        }
        std::memcpy(Offsets.data(), Raw.data(), Offsets.size() * 4);
        if ((E.Flags & FlagEncrypted) != 0)
            DecryptBlock(reinterpret_cast<uint8_t*>(Offsets.data()), Offsets.size() * 4, Key - 1);
        Out.reserve((size_t)E.Size);
        for (size_t I = 0; I < Sectors; I++)
        {
            if (Offsets[I] > Offsets[I + 1] || Offsets[I + 1] > Raw.size())
            {
                Error = E.Name + ": sector past the file";
                return false;
            }
            std::vector<uint8_t> Sector(Raw.begin() + Offsets[I], Raw.begin() + Offsets[I + 1]);
            if ((E.Flags & FlagEncrypted) != 0)
                DecryptBlock(Sector.data(), Sector.size(), Key + (uint32_t)I);
            const size_t Want = (size_t)std::min<uint64_t>(SectorSize, E.Size - (uint64_t)I * SectorSize);
            if (Sector.size() < Want)
            {
                if (Sector.empty() || Sector[0] != SectorZlib)
                {
                    Error = E.Name + ": sector compression not handled";
                    return false;
                }
                std::vector<uint8_t> Plain(Want);
                size_t Got = 0;
                if (!ZipInflate::Zlib(Sector.data() + 1, Sector.size() - 1, Plain.data(), Want, Got))
                {
                    Error = E.Name + ": a sector does not inflate";
                    return false;
                }
                Plain.resize(Got);
                Sector = std::move(Plain);
            }
            Out.insert(Out.end(), Sector.begin(), Sector.end());
        }
        return true;
    }

    std::string Install::ArchiveFolder(const std::string& InstallDir)
    {
        const std::filesystem::path Dir = Ifs::Utf8Path(InstallDir);
        const auto Nested = Dir / "IIPS" / "IIPSDownload";
        std::error_code Ec;
        return std::filesystem::is_directory(Nested, Ec) ? Utf8(Nested) : Utf8(Dir);
    }

    std::string Install::Key(const std::string& Name)
    {
        std::string K = Name;
        for (auto& Ch : K)
        {
            Ch = static_cast<char>(std::tolower(static_cast<unsigned char>(Ch)));
            if (Ch == '\\') Ch = '/';
        }
        return K;
    }

    bool Install::Open(const std::string& InstallDir, std::string& Error)
    {
        Archives.clear();
        Latest.clear();
        std::vector<std::filesystem::path> Paths;
        std::error_code Ec;
        for (const auto& It : std::filesystem::directory_iterator(Ifs::Utf8Path(ArchiveFolder(InstallDir)), Ec))
        {
            auto Ext = Utf8(It.path().extension());
            std::transform(Ext.begin(), Ext.end(), Ext.begin(), [](unsigned char C) { return (char)std::tolower(C); });
            if (It.is_regular_file() && Ext == ".ifs")
                Paths.push_back(It.path());
        }
        std::sort(Paths.begin(), Paths.end());
        if (Paths.empty())
        {
            Error = "no .ifs archive under " + ArchiveFolder(InstallDir);
            return false;
        }
        for (const auto& P : Paths)
        {
            Archive A;
            if (!A.Open(Utf8(P), Error))
                return false;
            const auto Version = ArchiveVersion(Utf8(P.filename()));
            const size_t Index = Archives.size();
            for (const auto& E : A.Files())
            {
                auto& Slot = Latest[Key(E.Name)];
                if (Slot.Version.empty() || Version > Slot.Version)
                    Slot = { Index, E, Version };
            }
            Archives.push_back(std::move(A));
        }
        return true;
    }

    const Install::Located* Install::Find(const std::string& Name) const
    {
        const auto It = Latest.find(Key(Name));
        return It == Latest.end() ? nullptr : &It->second;
    }

    bool Install::Read(const Located& L, std::vector<uint8_t>& Out, std::string& Error) const
    {
        return Archives[L.Archive].Read(L.File, Out, Error);
    }

    std::regex GlobRegex(const std::string& Glob)
    {
        std::string R;
        for (const char Ch : Glob)
        {
            if (Ch == '*') R += ".*";
            else if (Ch == '?') R += '.';
            else if (std::string("\\^$.|+()[]{}").find(Ch) != std::string::npos) R += std::string("\\") + Ch;
            else R += Ch;
        }
        return std::regex(R, std::regex::icase);
    }
}
