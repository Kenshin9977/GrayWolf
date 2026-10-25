// CliCommands.ZoneWalk.cpp - `zone-walk` (alias `bo4-walk`): walk zones with a zonekit profile's
// reader tables (ZoneTableWalker), in parallel, and write every read to a binary file for the
// zonekit tools.
#define NOMINMAX

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <fstream>
#include <condition_variable>
#include <filesystem>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <cstring>
#include <unordered_map>
#include <vector>

#include "CliCommands.h"

#include <CLI/CLI.hpp>
#include "CliSupport.h"
#include "MemoryBudget.h"
#include "ZoneTableWalker.h"

namespace
{
    std::vector<std::string> ReadLines(const std::string& Path)
    {
        std::vector<std::string> Out;
        std::ifstream In(Path);
        std::string Line;
        while (std::getline(In, Line))
        {
            while (!Line.empty() && (Line.back() == '\r' || Line.back() == ' ')) Line.pop_back();
            if (!Line.empty()) Out.push_back(Line);
        }
        return Out;
    }

    void PutU32(std::ofstream& O, uint32_t V) { O.write(reinterpret_cast<const char*>(&V), 4); }

    // one read call for the whole zone: istreambuf_iterator went byte by byte over GBs of zones
    bool ReadFile(const std::string& Path, std::vector<uint8_t>& Buf)
    {
        std::ifstream In(Path, std::ios::binary | std::ios::ate);
        if (!In) return false;
        const std::streamsize Size = In.tellg();
        In.seekg(0);
        Buf.resize(static_cast<size_t>(Size));
        return static_cast<bool>(In.read(reinterpret_cast<char*>(Buf.data()), Size));
    }
    void PutStr(std::ofstream& O, const std::string& S)
    {
        PutU32(O, (uint32_t)S.size());
        O.write(S.data(), (std::streamsize)S.size());
    }

    template <typename T> T At(const std::vector<uint8_t>& Buf, int64_t Pos)
    {
        T V{};
        if (Pos >= 0 && (uint64_t)Pos + sizeof(T) <= Buf.size()) std::memcpy(&V, Buf.data() + Pos, sizeof(T));
        return V;
    }

    // --assets-out: one TSV per zone, for accounting what a zone is made of.
    //   zone  <zone size>  <why the walk ended>
    //   asset <index> <type/table> <parent> <start> <end> <u64 at start, hex>
    //   image <index> <in-zone pixel bytes> <format> <width> <height> <mip count> <mip key:size;...>
    // `image` lines follow Cold War's GfxImage (the t9 `image` table, 0xd0 bytes; the same fields
    // GameBlackOpsCW::LoadXImage reads): pixels in the zone when (u32@0x90 >> 4) & 1 == 0, u32@0x98
    // bytes; DXGI format u32@0x9c, size u16@0xa0/0xa2; u8@0xb8 mip records of 32 bytes at the
    // pointer +0x30 (key u64@0, pixel bytes (u64@0x18 >> 4) & 0x1fffffff, as the t9 image table).
    void WriteAssets(const std::string& Path, const std::string& Profile, const std::vector<uint8_t>& Buf,
                     const ZoneTables::ZoneResult& R)
    {
        std::ofstream O(Path, std::ios::binary);
        std::string Why = R.Why;
        std::replace(Why.begin(), Why.end(), '\t', ' ');
        std::replace(Why.begin(), Why.end(), '\n', ' ');
        O << "zone\t" << Buf.size() << "\t" << Why << "\n";
        std::unordered_map<int64_t, int64_t> Links(R.Links.begin(), R.Links.end());
        const bool T9 = Profile == "t9";
        for (size_t I = 0; I < R.Instances.size(); I++)
        {
            const auto& In = R.Instances[I];
            const std::string& Type = In.Owner < R.OwnerNames.size() ? R.OwnerNames[In.Owner] : std::string("?");
            O << "asset\t" << I << "\t" << Type << "\t" << In.Parent << "\t" << In.Pos << "\t" << In.End << "\t"
              << std::hex << At<uint64_t>(Buf, In.Pos) << std::dec << "\n";
            if (!T9 || (Type != "image" && Type != "f1bcb9b0")) continue;
            const int64_t P = In.Pos;
            const bool InZone = ((At<uint32_t>(Buf, P + 0x90) >> 4) & 1) == 0;
            const uint32_t Mips = At<uint8_t>(Buf, P + 0xb8);
            O << "image\t" << I << "\t" << (InZone ? At<uint32_t>(Buf, P + 0x98) : 0) << "\t" << At<uint32_t>(Buf, P + 0x9c)
              << "\t" << At<uint16_t>(Buf, P + 0xa0) << "\t" << At<uint16_t>(Buf, P + 0xa2) << "\t" << Mips << "\t";
            const auto L = Links.find(P + 0x30);
            if (L == Links.end()) { O << (Mips ? "unresolved" : "") << "\n"; continue; }
            for (uint32_t K = 0; K < Mips; K++)
            {
                const int64_t M = L->second + 32 * (int64_t)K;
                O << (K ? ";" : "") << std::hex << At<uint64_t>(Buf, M) << std::dec << ":" << ((At<uint64_t>(Buf, M + 0x18) >> 4) & 0x1fffffff);
            }
            O << "\n";
        }
    }
}

int RunZoneWalk(const CliOptions& Options)
{
    // the console does not reach a caller's pipe: every failure also goes to --report
    auto Fail = [&](const std::string& Why) {
        fprintf(stderr, "zone-walk: %s\n", Why.c_str());
        CliReport(Options, "result=error\nreason=" + Why + "\n");
        return 1;
    };
    const auto Start = std::chrono::steady_clock::now();
    if (Options.Out.empty())
    {
        return Fail("needs --out");
    }
    ZoneTables::Profile Profile;
    std::string Error;
    if (!ZoneTables::LoadProfile(Options.ZoneWalkProfile.empty() ? "bo4" : Options.ZoneWalkProfile,
                               Profile, Error))
    {
        return Fail(Error);
    }
    std::vector<std::string> TypeNames = Profile.TypeNames;
    if (!Options.ZoneWalkTypes.empty())
    {
        TypeNames.assign(256, "");
        for (const std::string& Line : ReadLines(Options.ZoneWalkTypes))
        {
            std::istringstream S(Line);
            int Tid = -1;
            std::string Name;
            if (S >> Tid >> Name && Tid >= 0 && Tid < 256) TypeNames[(size_t)Tid] = Name;
        }
    }
    const std::vector<std::string>& TablePaths =
        Options.ZoneWalkTables.empty() ? Profile.TablePaths : Options.ZoneWalkTables;
    ZoneTables::Tables* Tables = nullptr;
    try
    {
        Tables = ZoneTables::LoadTables(TablePaths);
    }
    catch (const std::exception& E)
    {
        return Fail(E.what());
    }
    const ZoneTables::StreamRules Rules = Profile.Rules;
    const std::vector<std::string> Zones = ReadLines(Options.ZoneWalkList);
    // Each zone is written as soon as it and every zone before it are walked, then freed.
    std::ofstream O(Options.Out, std::ios::binary);
    O.write("BO4W", 4);
    PutU32(O, Options.ZoneWalkLayout ? 2 : 1);
    PutU32(O, (uint32_t)Zones.size());
    std::vector<ZoneTables::ZoneResult> Results(Zones.size());
    std::vector<bool> Done(Zones.size(), false);
    size_t NextWrite = 0;
    uint64_t Total = 0;
    std::mutex Lock;
    auto Write = [&](size_t I) {
        const auto& R = Results[I];
        PutStr(O, Zones[I]);
        PutStr(O, R.Why);
        O.put(R.ParseFailed ? 1 : 0);
        PutU32(O, (uint32_t)R.OwnerNames.size());
        for (const auto& N : R.OwnerNames) PutStr(O, N);
        const uint64_t N = R.Reads.size();
        O.write(reinterpret_cast<const char*>(&N), 8);
        O.write(reinterpret_cast<const char*>(R.Reads.data()), static_cast<std::streamsize>(N * 8));
        O.write(reinterpret_cast<const char*>(R.Owners.data()), static_cast<std::streamsize>(N * 2));
        if (Options.ZoneWalkLayout)   // version 2: block << 40 | offset per read (-1 elsewhere), zone position per read
        {
            O.write(reinterpret_cast<const char*>(R.ReadMem.data()), static_cast<std::streamsize>(N * 8));
            O.write(reinterpret_cast<const char*>(R.ReadPos.data()), static_cast<std::streamsize>(N * 8));
        }
        Total += N;
        Results[I] = ZoneTables::ZoneResult{};
    };
    // The zones in memory at once stay within the free memory above MemoryBudget::Reserve() (one
    // zone always passes). A zone costs about its own size, so the largest one says up front
    // whether the run fits.
    uint64_t Largest = 0;
    for (const auto& Z : Zones)
    {
        std::error_code Ec;
        Largest = std::max<uint64_t>(Largest, std::filesystem::file_size(Z, Ec));
    }
    const std::string Short = MemoryBudget::CheckFits(Largest + Largest / 10, "the largest zone");
    if (!Short.empty())
    {
        return Fail(Short);
    }
    const uint64_t Budget = MemoryBudget::For(1.0, Largest);
    uint64_t InFlight = 0;
    std::mutex BudgetLock;
    std::condition_variable BudgetFreed;
    std::atomic<size_t> Next{0};
    const unsigned Threads = Options.Threads ? Options.Threads : 8;
    std::vector<std::thread> Pool;
    Pool.reserve(Threads);
    for (unsigned K = 0; K < Threads; K++)
        Pool.emplace_back([&]() {
            std::vector<uint8_t> Buf;
            for (size_t I = Next++; I < Zones.size(); I = Next++)
            {
                std::error_code Ec;
                const uint64_t Size = std::filesystem::file_size(Zones[I], Ec);
                {
                    std::unique_lock<std::mutex> G(BudgetLock);
                    BudgetFreed.wait(G, [&] { return InFlight == 0 || InFlight + Size <= Budget; });
                    InFlight += Size;
                }
                ZoneTables::ZoneResult R;
                if (!ReadFile(Zones[I], Buf))
                {
                    R.ParseFailed = true;
                    R.Why = "raised: cannot open the zone";
                }
                else
                {
                    R = ZoneTables::WalkZone(Buf, Tables, TypeNames, Rules,
                                             Options.ZoneWalkLayout || !Options.ZoneWalkAssetsOut.empty());
                    if (!Options.ZoneWalkAssetsOut.empty())
                    {
                        std::filesystem::create_directories(Options.ZoneWalkAssetsOut, Ec);
                        WriteAssets((std::filesystem::path(Options.ZoneWalkAssetsOut) /
                                     (std::filesystem::path(Zones[I]).stem().string() + ".assets.tsv")).string(),
                                    Profile.Name, Buf, R);
                    }
                }
                if (Buf.capacity() > (64u << 20)) std::vector<uint8_t>().swap(Buf);
                {
                    std::lock_guard<std::mutex> G(BudgetLock);
                    InFlight -= Size;
                }
                BudgetFreed.notify_all();
                std::lock_guard<std::mutex> G(Lock);
                Results[I] = std::move(R);
                Done[I] = true;
                for (; NextWrite < Zones.size() && Done[NextWrite]; NextWrite++) Write(NextWrite);
            }
        });
    for (auto& T : Pool) T.join();
    ZoneTables::FreeTables(Tables);
    const double Secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - Start).count();
    printf("zone-walk (%s): %zu zones, %llu reads, %.1fs, memory budget %s\n", Profile.Name.c_str(),
           Zones.size(), (unsigned long long)Total, Secs, MemoryBudget::Gb(Budget).c_str());
    return 0;
}

// ---- the subcommands (CliCommands.h)

void AddZoneWalkCommands(CliCommandBuilders& B)
{
    // bo4-walk: the same command, profile bo4 by default
    B["zone-walk"] = [](CLI::App& App, CliOptions& Options) {
        for (const char* Name : {"zone-walk", "bo4-walk"})
        {
            auto* Sub = App.add_subcommand(Name,
                "Walk decompressed zones with a zonekit profile's reader tables and write every read");
            Sub->add_option("zone-list", Options.ZoneWalkList, "Text file: one decompressed zone path per line")->required()->check(CLI::ExistingFile);
            Sub->add_option("--profile", Options.ZoneWalkProfile, "zonekit profile: a name (bo4) or a profile directory");
            Sub->add_option("--tables", Options.ZoneWalkTables, "Reader table file(s) instead of the profile's; later ones override");
            Sub->add_option("--types", Options.ZoneWalkTypes, "Text file: `<type id> <table name>` per line, instead of the profile's")->check(CLI::ExistingFile);
            Sub->add_flag("--layout", Options.ZoneWalkLayout, "Also write each read's block and offset in the stream memory (output version 2)");
            Sub->add_option("--assets-out", Options.ZoneWalkAssetsOut,
                "Also write <dir>/<zone>.assets.tsv: every asset instance walked (type, parent, start, end, u64 at start) and each image's mip records (layout walk)");
        }
    };
}
