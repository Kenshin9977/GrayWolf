#include "stdafx.h"

#include "Bo3SoundNames.h"

#include "ExportNaming.h"

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <map>
#include <mutex>
#include <set>

namespace fs = std::filesystem;

namespace Bo3SoundNames
{
namespace
{
    // bumped when what a zone gives the table changes (the reader's tables are in the key already)
    constexpr int Format = 2;
    const std::string Magic = "greyhound-bo3-sound-names ";

    using ExportNaming::Lower;

    // A zone as the file records it: its name, size and time; a zone whose record differs is read again
    std::string Stamp(const std::string& Zone)
    {
        std::error_code Ec;
        const auto Size = fs::file_size(Zone, Ec);
        const auto Time = fs::last_write_time(Zone, Ec).time_since_epoch().count();
        return Lower(fs::path(Zone).filename().string()) + "\t" + std::to_string(Size) + "\t" + std::to_string(Time);
    }

    using Parts = std::map<std::string, Table>;   // a zone's stamp -> the names it gives

    // The complete zone records of File when its key is Key (a record without its "end", the torn
    // last one of a build that stopped, is left out)
    bool Load(const std::string& File, const std::string& Key, Parts& Out)
    {
        std::ifstream In(File, std::ios::binary);
        std::string Line;
        if (!In || !std::getline(In, Line) || Line != Magic + std::to_string(Format) || !std::getline(In, Line) || Line != "key " + Key)
            return false;
        std::string Zone;
        Table Names;
        bool InZone = false;
        while (std::getline(In, Line))
        {
            if (Line.rfind("zone\t", 0) == 0)
            {
                Zone = Line.substr(5);
                Names.clear();
                InZone = true;
            }
            else if (Line == "end")
            {
                if (InZone) Out[Zone] = std::move(Names);
                Names = Table();
                InZone = false;
            }
            else if (const size_t Tab = Line.find('\t'); InZone && Tab != std::string::npos)
                Names.emplace((uint32_t)std::strtoul(Line.c_str(), nullptr, 16), Line.substr(Tab + 1));
        }
        return true;
    }

    void WriteZone(std::ofstream& Out, const std::string& Zone, const Table& Names)
    {
        // sorted, so a build gives the same file whatever the hash order
        const std::map<uint32_t, std::string> Sorted(Names.begin(), Names.end());
        Out << "zone\t" << Zone << "\n";
        for (const auto& [Hash, Name] : Sorted) Out << std::hex << Hash << std::dec << "\t" << Name << "\n";
        Out << "end\n";
        Out.flush();
    }
}

std::vector<std::string> Zones(const std::string& ZoneDir, bool Languages)
{
    std::set<std::string> Prefixes;
    std::error_code Ec;
    for (const auto& Sub : fs::directory_iterator(fs::path(ZoneDir) / "snd", Ec))
    {
        const std::string Locale = Lower(Sub.path().filename().string());
        if (Sub.is_directory(Ec) && Locale != "all") Prefixes.insert(Locale + "_");
    }
    std::vector<std::string> Out;
    for (const auto& Entry : fs::directory_iterator(ZoneDir, Ec))
    {
        if (!Entry.is_regular_file(Ec) || Lower(Entry.path().extension().string()) != ".ff" || Entry.file_size(Ec) == 0) continue;
        const std::string Name = Lower(Entry.path().filename().string());
        const bool Language = std::any_of(Prefixes.begin(), Prefixes.end(), [&](const std::string& P) { return Name.rfind(P, 0) == 0; });
        if (Language == Languages) Out.push_back(Entry.path().string());
    }
    std::sort(Out.begin(), Out.end());
    return Out;
}

Kept For(const std::vector<std::string>& Zones, const std::string& File, const std::string& ReaderKey,
         const std::function<bool(const std::string& Zone, Table& Out)>& Read, const std::function<void()>& Before,
         const std::function<void()>& After, const std::function<bool(size_t, size_t, const std::string&)>& Step)
{
    // one table per kept file for the process: every zone of a run asks
    static std::mutex Lock;
    static std::map<std::string, std::pair<std::string, std::shared_ptr<const Table>>> Loaded;

    const auto Start = std::chrono::steady_clock::now();
    std::vector<std::string> Stamps;
    std::string Whole = ReaderKey + "\n";
    for (const auto& Z : Zones) Whole += (Stamps.emplace_back(Stamp(Z))) + "\n";
    // the file's key: the format and the reader's tables (each zone's record carries its own stamp)
    const std::string Key = ExportNaming::Hex16(ExportNaming::Fnv1a("format " + std::to_string(Format) + "\n" + ReaderKey + "\n"));
    const std::string Current = ExportNaming::Hex16(ExportNaming::Fnv1a(Whole));
    Kept K;
    K.Zones = Zones.size();
    const auto Done = [&] {
        K.Seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - Start).count();
        return K;
    };

    std::lock_guard<std::mutex> G(Lock);
    if (const auto It = Loaded.find(File); It != Loaded.end() && It->second.first == Current)
    {
        K.Names = It->second.second;
        return Done();
    }
    Parts Read_;
    Load(File, Key, Read_);
    std::vector<size_t> Todo;
    for (size_t Z = 0; Z < Zones.size(); Z++)
        if (!Read_.count(Stamps[Z])) Todo.push_back(Z);
    bool Stopped = false;
    if (!Todo.empty())
    {
        K.Built = true;
        K.Resumed = Zones.size() - Todo.size();
        // the file rewritten with the records still current, then each zone read appended as it is
        // done: a build that stops resumes from there
        std::ofstream Out(File + ".tmp", std::ios::binary | std::ios::trunc);
        Out << Magic << Format << "\nkey " << Key << "\n";
        for (size_t Z = 0; Z < Zones.size(); Z++)
            if (const auto It = Read_.find(Stamps[Z]); It != Read_.end()) WriteZone(Out, Stamps[Z], It->second);
        Out.close();
        std::error_code Ec;
        fs::rename(File + ".tmp", File, Ec);
        Out.open(File, std::ios::binary | std::ios::app);
        if (Before) Before();
        for (size_t N = 0; N < Todo.size(); N++)
        {
            const size_t Z = Todo[N];
            if (Step && !Step(K.Resumed + N, Zones.size(), fs::path(Zones[Z]).filename().string()))
            {
                Stopped = true;
                break;
            }
            Table Names;
            if (!Read(Zones[Z], Names)) K.ZonesFailed++;
            K.ZonesRead++;
            WriteZone(Out, Stamps[Z], Names);
            Read_[Stamps[Z]] = std::move(Names);
        }
        if (After) After();
    }
    // the zones' names in zone order, an earlier zone's name kept: the same table whatever order
    // the zones were read in
    auto Names = std::make_shared<Table>();
    for (size_t Z = 0; Z < Zones.size(); Z++)
        if (const auto It = Read_.find(Stamps[Z]); It != Read_.end()) Names->insert(It->second.begin(), It->second.end());
    if (!Stopped) Loaded[File] = { Current, Names };
    K.Names = std::move(Names);
    return Done();
}
}
