#include "stdafx.h"

#include "IwBesideZones.h"

#include "CoDAssets.h"
#include "FastFile.h"
#include "ZonekitZone.h"

#include <algorithm>
#include <atomic>
#include <cstring>
#include <filesystem>
#include <thread>
#include <unordered_map>

namespace IwBeside
{
size_t Set::Load(const std::vector<std::string>& Paths, const ExportWriters::LoadedProfile& Loaded, uint64_t Above,
                 std::map<std::string, size_t>& Counters, bool Before)
{
    constexpr uint64_t Align = 0xFFFFFF;
    if (Next == 0) Next = (Above + Align) & ~Align;
    // read and walked on a few threads (a global zone is 100-170 MB), placed in the order given
    std::vector<std::unique_ptr<Zone>> Read(Paths.size());
    std::atomic<size_t> NextPath{ 0 };
    const auto Work = [&] {
        for (size_t K; (K = NextPath++) < Paths.size();)
        {
            auto Z = std::make_unique<Zone>();
            Z->Name = std::filesystem::path(Paths[K]).stem().string();
            Z->Before = Before;
            std::string Error;
            if (!FastFile::Decompress(Paths[K], Z->Bytes, Error) || Z->Bytes.empty()) continue;
            Z->R = ZoneTables::WalkZone(Z->Bytes, Loaded.Tables, Loaded.Profile.TypeNames, Loaded.Profile.Rules, true);
            if (!Z->R.ParseFailed) Read[K] = std::move(Z);
        }
    };
    std::vector<std::thread> Threads;
    const size_t Count = std::min<size_t>(Paths.size(), std::clamp<size_t>(std::thread::hardware_concurrency() / 4, 1, 4));
    for (size_t T = 0; T < Count; T++) Threads.emplace_back(Work);
    for (auto& T : Threads) T.join();

    size_t Done = 0;
    for (auto& Z : Read)
    {
        if (Z == nullptr)
        {
            Counters["beside_zones_unread"]++;
            continue;
        }
        if (Z->R.Why != "walked to the end") Counters["beside_zones_not_to_end"]++;
        Z->Base = Next;
        Next = (Next + Z->Bytes.size() + Align) & ~Align;
        // its own pointers: inline, and (stream.keyed_refs) the keys and asset hashes of its own
        // instances; the rewind table's own keys the walker linked already
        const auto Own = Loaded.Profile.Rules.RewindRefs ? Z->R.Links : ResolveLinks(Z->R, Loaded.Profile.Rules, false, &Z->Bytes).Links;
        for (const auto& [Field, Target] : Own)
        {
            const uint64_t V = Z->Base + (uint64_t)Target;
            if (Field >= 0 && (uint64_t)Field + 8 <= Z->Bytes.size()) std::memcpy(Z->Bytes.data() + Field, &V, 8);
        }
        Counters["beside_zones"]++;
        Counters["beside_zones_mb"] += Z->Bytes.size() >> 20;
        Zones.push_back(std::move(Z));
        Done++;
    }
    return Done;
}

void Set::Link(const ZoneTables::ZoneResult& Main, uint64_t MainBase, const std::vector<uint8_t>& MainBytes,
               const ZoneTables::StreamRules& Rules, const std::function<void(uint64_t Field, uint64_t Value)>& PutMain,
               std::map<std::string, size_t>& Counters, const Unresolved& OnUnresolved)
{
    if (Zones.empty() || Rules.KeyMask == 0) return;
    // every zone's keys and asset hashes, Main first, the Before zones last: the first filing wins
    std::unordered_map<uint64_t, uint64_t> Keys;
    std::map<std::pair<std::string, uint64_t>, uint64_t> Hashes;
    const auto Index = [&](const ZoneTables::ZoneResult& R, uint64_t Base, const std::vector<uint8_t>& Bytes) {
        for (const auto& [Key, Pos] : R.Keys) Keys.try_emplace(Key, Base + (uint64_t)Pos);
        for (const auto& I : R.Instances)
            if (I.Pos >= 0 && (uint64_t)I.Pos + 8 <= Bytes.size() && I.Owner < R.OwnerNames.size())
            {
                uint64_t H = 0;
                std::memcpy(&H, Bytes.data() + I.Pos, 8);
                Hashes.try_emplace({ R.OwnerNames[I.Owner], H & Rules.AssetHashMask }, Base + (uint64_t)I.Pos);
            }
    };
    Index(Main, MainBase, MainBytes);
    for (const bool B : { false, true })
        for (const auto& Z : Zones)
            if (Z->Before == B) Index(Z->R, Z->Base, Z->Bytes);
    // every reference written (one its own zone resolved gets the same target again, unless
    // another zone filed its key first); counted when its target is in another zone
    size_t Linked = 0, Unlinked = 0, UnlinkedAssets = 0, UnlinkedBefore = 0;
    const auto Resolve = [&](const ZoneTables::ZoneResult& R, uint64_t Base, uint64_t Size, const std::string& Name, bool Before,
                             const std::function<void(uint64_t, uint64_t)>& Put) {
        for (size_t K = 0; K < R.BackRefs.size(); K++)
        {
            const auto& [Field, Value] = R.BackRefs[K];
            const bool Asset = K < R.BackRefIsAsset.size() && R.BackRefIsAsset[K];
            const auto Type = Asset ? R.RefTypes.find(Field) : R.RefTypes.end();
            const bool Typed = Type != R.RefTypes.end() && Type->second < R.OwnerNames.size();
            uint64_t Target = 0;
            if (Asset)
            {
                if (Typed)
                    if (const auto It = Hashes.find({ R.OwnerNames[Type->second], Value & Rules.AssetHashMask }); It != Hashes.end())
                        Target = It->second;
            }
            else if (const auto It = Keys.find(Value & Rules.KeyMask); It != Keys.end())
                Target = It->second;
            if (Target == 0)
            {
                if (Before)
                {
                    UnlinkedBefore++;
                    continue;
                }
                Unlinked++;
                UnlinkedAssets += Asset;
                if (OnUnresolved) OnUnresolved(Name, (uint64_t)Field, Value, Typed ? R.OwnerNames[Type->second] : Asset ? "asset" : "key");
                continue;
            }
            Put(Base + (uint64_t)Field, Target);
            if (Target < Base || Target >= Base + Size) Linked++;
        }
    };
    Resolve(Main, MainBase, MainBytes.size(), "", false, PutMain);
    for (const auto& Z : Zones) Resolve(Z->R, Z->Base, Z->Bytes.size(), Z->Name, Z->Before, [this](uint64_t Field, uint64_t Value) { Put(Field, Value); });
    Counters["links_across_zones"] += Linked;
    Counters["refs_unresolved_across_zones"] += Unlinked;
    Counters["refs_unresolved_across_zones_assets"] += UnlinkedAssets;
    if (UnlinkedBefore != 0) Counters["refs_unresolved_in_zones_before"] += UnlinkedBefore;
}

void Set::Serve() const
{
    if (CoDAssets::GameInstance == nullptr) return;
    for (const auto& Z : Zones) CoDAssets::GameInstance->AddVirtual(Z->Bytes.data(), Z->Bytes.size(), Z->Base);
}

const uint8_t* Set::Locate(uint64_t Address, uint64_t N) const
{
    for (const auto& Z : Zones)
        if (Address >= Z->Base && Address - Z->Base + N <= Z->Bytes.size()) return Z->Bytes.data() + (Address - Z->Base);
    return nullptr;
}

bool Set::Holds(const std::string& Type, uint64_t Address) const
{
    std::lock_guard<std::mutex> G(HeldLock);
    auto& [Seen, Addresses] = Held[Type];
    for (; Seen < Zones.size(); Seen++)
    {
        const Zone& Z = *Zones[Seen];
        for (const auto& I : Z.R.Instances)
            if (I.Owner < Z.R.OwnerNames.size() && Z.R.OwnerNames[I.Owner] == Type) Addresses.insert(Z.Base + (uint64_t)I.Pos);
    }
    return Addresses.count(Address) != 0;
}

bool Set::Put(uint64_t Field, uint64_t Value)
{
    for (const auto& Z : Zones)
        if (Field >= Z->Base && Field - Z->Base + 8 <= Z->Bytes.size())
        {
            std::memcpy(Z->Bytes.data() + (Field - Z->Base), &Value, 8);
            return true;
        }
    return false;
}
}
