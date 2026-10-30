#include "stdafx.h"

#include "ResumeIndex.h"
#include "ExportNaming.h"
#include "FileSystems.h"

#define XXH_INLINE_ALL   // header-only: no xxhash.lib to link
#include <xxhash.h>

#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <fstream>

namespace fs = std::filesystem;

namespace
{
    constexpr char Magic[4] = { 'G', 'W', 'R', 'S' };
    constexpr uint32_t Version = 7;   // 2: Black Ops 7's keys and hashes; 3: Ranked; 4: Size; 7: its heads beside it
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

    // the bytes written: unsigned varints, zigzag for signed differences, raw u64 for hashes
    struct Writer
    {
        std::string B;
        void Var(uint64_t V)
        {
            while (V >= 0x80)
            {
                B.push_back((char)(V | 0x80));
                V >>= 7;
            }
            B.push_back((char)V);
        }
        void Zig(int64_t V) { Var(((uint64_t)V << 1) ^ (0 - ((uint64_t)V >> 63))); }
        void U64(uint64_t V) { B.append(reinterpret_cast<const char*>(&V), 8); }
        void Text(const std::string& S)
        {
            Var(S.size());
            B += S;
        }
    };

    struct Reader
    {
        const uint8_t* P;
        const uint8_t* End;
        bool Ok = true;
        uint64_t Var()
        {
            uint64_t V = 0;
            for (int Shift = 0; Shift < 64; Shift += 7)
            {
                if (P >= End) { Ok = false; return 0; }
                const uint8_t C = *P++;
                V |= (uint64_t)(C & 0x7f) << Shift;
                if (!(C & 0x80)) return V;
            }
            Ok = false;
            return 0;
        }
        int64_t Zig()
        {
            const uint64_t V = Var();
            return (int64_t)(V >> 1) ^ -(int64_t)(V & 1);
        }
        uint64_t U64()
        {
            if (End - P < 8) { Ok = false; return 0; }
            uint64_t V = 0;
            memcpy(&V, P, 8);
            P += 8;
            return V;
        }
        std::string Text()
        {
            const uint64_t N = Var();
            if (!Ok || (uint64_t)(End - P) < N) { Ok = false; return {}; }
            std::string S(reinterpret_cast<const char*>(P), (size_t)N);
            P += N;
            return S;
        }
        // a count no larger than what the bytes left could hold
        size_t Count()
        {
            const uint64_t N = Var();
            if (N > (uint64_t)(End - P)) Ok = false;
            return Ok ? (size_t)N : 0;
        }
    };

    using ExportNaming::Hex16;
}

size_t ZoneResume::Find(DiskGame::Kind K, const std::string& Name) const
{
    const std::tuple<uint8_t, uint64_t, uint32_t> Key{ (uint8_t)K, ResumeIndex::NameHash(K, Name), 0 };
    auto It = std::lower_bound(Names.begin(), Names.end(), Key);
    if (It == Names.end() || std::get<0>(*It) != std::get<0>(Key) || std::get<1>(*It) != std::get<1>(Key)) return SIZE_MAX;
    return std::get<2>(*It);
}

namespace ResumeIndex
{
    uint64_t NameHash(DiskGame::Kind K, const std::string& Name)
    {
        const std::string Lower = ExportNaming::Lower(Name);
        return XXH3_64bits_withSeed(Lower.data(), Lower.size(), (uint64_t)K);
    }

    std::string FileFor(const std::string& ZonePath, const std::string& TablesKey, const std::vector<std::string>& Sources)
    {
        std::string Key = TablesKey + "|files";
        std::error_code Ec;
        for (const std::string& S : Sources)
        {
            const fs::path P(S);
            const uintmax_t Size = fs::file_size(P, Ec);
            if (Ec) return std::string();
            const auto Written = fs::last_write_time(P, Ec);
            if (Ec) return std::string();
            Key += "|" + fs::absolute(P, Ec).string() + "|" + std::to_string(Size) + "|" + std::to_string(Written.time_since_epoch().count());
        }
        const fs::path Dir = fs::temp_directory_path(Ec) / "greyhound_resume";
        return (Dir / (fs::path(ZonePath).stem().string() + "_" + Hex16(XXH3_64bits(Key.data(), Key.size())) + ".gwr")).string();
    }

    std::string FileFor(const std::string& ZonePath, const std::string& TablesKey, ByteView Bytes)
    {
        const uint64_t Content = XXH3_64bits(Bytes.data(), Bytes.size());
        const std::string Key = TablesKey + "|" + std::to_string(Bytes.size()) + "|" + Hex16(Content);
        std::error_code Ec;
        const fs::path Dir = fs::temp_directory_path(Ec) / "greyhound_resume";
        return (Dir / (fs::path(ZonePath).stem().string() + "_" + Hex16(XXH3_64bits(Key.data(), Key.size())) + ".gwr")).string();
    }

    bool Save(const std::string& File, const ZoneResume& Z)
    {
        Writer W;
        W.B.append(Magic, 4);
        W.B.append(reinterpret_cast<const char*>(&Version), 4);
        W.Var(Z.Size);
        // each point as differences from the one before: list slot, position, cursors (by block),
        // then where each block's last read ended, as a difference from that block's cursor
        W.Var(Z.Points.size());
        uint32_t PrevList = 0;
        int64_t PrevPos = 0;
        std::map<int, int64_t> PrevCursor;
        for (const auto& P : Z.Points)
        {
            W.Var(P.ListIndex - PrevList);
            W.Zig(P.Pos - PrevPos);
            W.Var(P.Cursors.size());
            std::map<int, int64_t> Here;
            for (const auto& [B, C] : P.Cursors)
            {
                W.Var((uint64_t)B);
                W.Zig(C - PrevCursor[B]);
                Here[B] = C;
            }
            W.Var(P.Starts.size());
            for (const auto& [B, C] : P.Starts)
            {
                W.Var((uint64_t)B);
                const auto It = Here.find(B);
                W.Zig(C - (It == Here.end() ? 0 : It->second));
            }
            PrevList = P.ListIndex;
            PrevPos = P.Pos;
            PrevCursor = std::move(Here);
        }
        W.Var(Z.Names.size());
        for (const auto& [K, H, P] : Z.Names)
        {
            W.Var(K);
            W.U64(H);
            W.Var(P);
        }
        W.Var(Z.Keys.size());
        int64_t PrevKeyPos = 0;
        for (const auto& [Key, Pos] : Z.Keys)
        {
            W.U64(Key);
            W.Zig(Pos - PrevKeyPos);
            PrevKeyPos = Pos;
        }
        std::map<std::string, uint64_t> Types;
        for (const auto& [K, Pos] : Z.ByHash) Types.emplace(K.first, 0);
        W.Var(Types.size());
        uint64_t N = 0;
        for (auto& [Name, Id] : Types)
        {
            Id = N++;
            W.Text(Name);
        }
        W.Var(Z.ByHash.size());
        for (const auto& [K, Pos] : Z.ByHash)
        {
            W.Var(Types[K.first]);
            W.U64(K.second);
            W.Zig(Pos);
        }
        W.Var(Z.Ranked.size());
        for (const auto& [Type, All] : Z.Ranked)
        {
            W.Text(Type);
            W.Var(All.size());
            int64_t Prev = 0;
            for (const int64_t Pos : All)
            {
                W.Zig(Pos - Prev);
                Prev = Pos;
            }
        }
        W.U64(XXH3_64bits(W.B.data(), W.B.size()));

        std::error_code Ec;
        fs::create_directories(fs::path(File).parent_path(), Ec);
        const std::string Temp = File + ".tmp";
        {
            std::ofstream Out(Temp, std::ios::binary | std::ios::trunc);
            if (!Out.write(W.B.data(), (std::streamsize)W.B.size())) return false;
        }
        fs::rename(Temp, File, Ec);
        return !Ec;
    }

    bool Load(const std::string& File, ZoneResume& Z)
    {
        std::ifstream In(File, std::ios::binary | std::ios::ate);
        if (!In) return false;
        std::vector<uint8_t> B((size_t)In.tellg());
        In.seekg(0);
        if (B.size() < 16 || !In.read(reinterpret_cast<char*>(B.data()), (std::streamsize)B.size())) return false;
        uint64_t Sum = 0;
        memcpy(&Sum, B.data() + B.size() - 8, 8);
        if (Sum != XXH3_64bits(B.data(), B.size() - 8)) return false;
        uint32_t V = 0;
        memcpy(&V, B.data() + 4, 4);
        if (memcmp(B.data(), Magic, 4) != 0 || V != Version) return false;
        Reader R{ B.data() + 8, B.data() + B.size() - 8 };
        Z = ZoneResume();
        Z.Size = R.Var();
        Z.Points.resize(R.Count());
        uint32_t PrevList = 0;
        int64_t PrevPos = 0;
        // per block the previous point's cursor, and this point's (blocks are a few small numbers):
        // a block the previous point had no cursor for counts from 0, as Save wrote it
        std::vector<int64_t> PrevCursor, Here;
        std::vector<uint8_t> PrevHas, Has;
        const auto Slot = [](std::vector<int64_t>& V, std::vector<uint8_t>& H, size_t B) {
            if (B >= V.size()) { V.resize(B + 1, 0); H.resize(B + 1, 0); }
        };
        for (auto& P : Z.Points)
        {
            P.ListIndex = PrevList + (uint32_t)R.Var();
            P.Pos = PrevPos + R.Zig();
            std::fill(Has.begin(), Has.end(), 0);
            const size_t NC = R.Count();
            P.Cursors.reserve(NC);
            for (size_t K = 0; K < NC && R.Ok; K++)
            {
                const uint64_t Block = R.Var();
                if (Block > 4096) { R.Ok = false; break; }
                Slot(Here, Has, (size_t)Block);
                const int64_t From = Block < PrevCursor.size() && PrevHas[(size_t)Block] ? PrevCursor[(size_t)Block] : 0;
                const int64_t C = From + R.Zig();
                P.Cursors.emplace_back((int)Block, C);
                Here[(size_t)Block] = C;
                Has[(size_t)Block] = 1;
            }
            const size_t NS = R.Count();
            P.Starts.reserve(NS);
            for (size_t K = 0; K < NS && R.Ok; K++)
            {
                const uint64_t Block = R.Var();
                const int64_t From = Block < Here.size() && Has[(size_t)Block] ? Here[(size_t)Block] : 0;
                P.Starts.emplace_back((int)Block, From + R.Zig());
            }
            PrevList = P.ListIndex;
            PrevPos = P.Pos;
            std::swap(PrevCursor, Here);
            std::swap(PrevHas, Has);
            if (!R.Ok) return false;
        }
        Z.Names.resize(R.Count());
        for (auto& [K, H, P] : Z.Names)
        {
            K = (uint8_t)R.Var();
            H = R.U64();
            P = (uint32_t)R.Var();
        }
        Z.Keys.resize(R.Count());
        int64_t PrevKeyPos = 0;
        for (auto& [Key, Pos] : Z.Keys)
        {
            Key = R.U64();
            Pos = PrevKeyPos + R.Zig();
            PrevKeyPos = Pos;
        }
        std::vector<std::string> Types(R.Count());
        for (auto& T : Types) T = R.Text();
        for (size_t K = 0, N = R.Count(); K < N && R.Ok; K++)
        {
            const uint64_t Type = R.Var();
            const uint64_t Hash = R.U64();
            const int64_t Pos = R.Zig();
            if (Type < Types.size()) Z.ByHash.emplace_hint(Z.ByHash.end(), std::make_pair(Types[(size_t)Type], Hash), Pos);   // written in order
        }
        for (size_t K = 0, N = R.Count(); K < N && R.Ok; K++)
        {
            std::vector<int64_t>& All = Z.Ranked[R.Text()];
            All.resize(R.Count());
            int64_t Prev = 0;
            for (int64_t& Pos : All) Pos = Prev = Prev + R.Zig();
        }
        return R.Ok && R.P == R.End;
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

    bool IsCurrent(const std::string& File)
    {
        std::ifstream In(File, std::ios::binary);
        char Head[8] = {};
        uint32_t V = 0;
        if (!In.read(Head, 8)) return false;
        std::memcpy(&V, Head + 4, 4);
        return std::memcmp(Head, Magic, 4) == 0 && V == Version;
    }
}
