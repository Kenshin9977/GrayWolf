#include "stdafx.h"
#include "ByteView.h"

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
    std::vector<std::string> IndexFiles(Paths.size());
    // each zone's resume index: written by an eager read, read by a lazy one
    if (Loaded.Profile.Rules.KeyMask != 0)
        for (size_t K = 0; K < Paths.size(); K++)
            IndexFiles[K] = ResumeIndex::FileFor(Paths[K], ProfileKey(Loaded), std::vector<std::string>{ Paths[K] });
    // a zone read and walked whole
    const auto Eager = [&](size_t K) {
        auto Z = std::make_unique<Zone>();
        Z->Name = std::filesystem::path(Paths[K]).stem().string();
        Z->Before = Before;
        std::string Error;
        if (!FastFile::Decompress(Paths[K], Z->Bytes, Error) || Z->Bytes.empty()) return;
        Z->R = ZoneTables::WalkZone(Z->Bytes, Loaded.Tables, Loaded.Profile.TypeNames, Loaded.Profile.Rules, true);
        if (Z->R.ParseFailed) return;
        Z->Size = Z->Bytes.size();
        // its resume index, for the partial opens to come (read before its own links are written)
        if (!IndexFiles[K].empty() && !Z->R.Resume.empty())
        {
            ZoneResume Ix;
            Ix.Size = Z->Bytes.size();
            Ix.Points = Z->R.Resume;
            Ix.Keys.assign(Z->R.Keys.begin(), Z->R.Keys.end());
            const uint64_t Mask = Loaded.Profile.Rules.AssetHashMask;
            for (const auto& I : Z->R.Instances)
                if (I.Pos >= 0 && (uint64_t)I.Pos + 8 <= Z->Bytes.size() && I.Owner < Z->R.OwnerNames.size())
                {
                    const uint64_t H = ExportNaming::At<uint64_t>(Z->Bytes, I.Pos) & Mask;
                    Ix.ByHash.try_emplace({ Z->R.OwnerNames[I.Owner], H }, I.Pos);
                    Ix.ByHash.try_emplace({ std::string(), H }, I.Pos);
                }
            ResumeIndex::Save(IndexFiles[K], Ix);
        }
        Read[K] = std::move(Z);
    };
    // a lazy zone: placed from its resume index, read when a reference reaches it (Reach)
    std::atomic<size_t> NextPath{ 0 };
    const auto Work = [&] {
        for (size_t K; (K = NextPath++) < Paths.size();)
        {
            auto Ix = std::make_unique<ZoneResume>();
            if (!Lazy || IndexFiles[K].empty() || !ResumeIndex::Load(IndexFiles[K], *Ix) || Ix->Size == 0 || Ix->Points.empty())
            {
                Eager(K);
                continue;
            }
            auto Z = std::make_unique<Zone>();
            Z->Name = std::filesystem::path(Paths[K]).stem().string();
            Z->Before = Before;
            Z->Path = Paths[K];
            Z->Size = Ix->Size;
            Z->Loaded = &Loaded;
            Z->Index = std::move(Ix);
            Read[K] = std::move(Z);
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
        Z->Base = Next;
        Next = (Next + Z->Size + Align) & ~Align;
        if (Z->Index != nullptr)
        {
            Counters["beside_zones_lazy"]++;
            Zones.push_back(std::move(Z));
            Done++;
            continue;
        }
        if (Z->R.Why != "walked to the end") Counters["beside_zones_not_to_end"]++;
        // its own pointers: inline, and (stream.keyed_refs) the keys and asset hashes of its own
        // instances; the rewind table's own keys the walker linked already
        const auto Own = Loaded.Profile.Rules.RewindRefs ? Z->R.Links : ResolveLinks(Z->R, Loaded.Profile.Rules, false, Z->Bytes).Links;
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

void Set::Link(const ZoneTables::ZoneResult& Main, uint64_t MainBase, ByteView MainBytes,
               const ZoneTables::StreamRules& Rules, const std::function<void(uint64_t Field, uint64_t Value)>& PutMain,
               std::map<std::string, size_t>& Counters, const Unresolved& OnUnresolved,
               const std::vector<std::pair<uint64_t, int64_t>>* MainKeys, const std::map<std::pair<std::string, uint64_t>, int64_t>* MainHashes)
{
    if (Zones.empty() || Rules.KeyMask == 0) return;
    // a key's data, or the asset of a type and hash: Main's first (it loaded first), then each zone's,
    // the Before zones' last, the first that has it (the game's tables keep the first filing). Looked
    // up where each one keeps them (an index's tables, millions of entries over a map's zones, are
    // not merged); a walk's own built here
    struct Filed
    {
        std::unordered_map<uint64_t, int64_t> Keys;
        std::map<std::pair<std::string, uint64_t>, int64_t> Hashes;
    };
    const auto Build = [&](const ZoneTables::ZoneResult& R, ByteView Bytes) {
        Filed F;
        for (const auto& [Key, Pos] : R.Keys) F.Keys.try_emplace(Key, Pos);
        for (const auto& I : R.Instances)
            if (I.Pos >= 0 && (uint64_t)I.Pos + 8 <= Bytes.size() && I.Owner < R.OwnerNames.size())
            {
                uint64_t H = 0;
                std::memcpy(&H, Bytes.data() + I.Pos, 8);
                F.Hashes.try_emplace({ R.OwnerNames[I.Owner], H & Rules.AssetHashMask }, I.Pos);
            }
        return F;
    };
    const bool MainWhole = MainKeys != nullptr && MainHashes != nullptr;
    const Filed MainWalked = MainWhole ? Filed() : Build(Main, MainBytes);
    if (MainWhole && MainKeysOf != MainKeys)
    {
        // (Main's keys by key, kept for the next call)
        MainKeyed.clear();
        MainKeyed.reserve(MainKeys->size());
        for (const auto& [Key, Pos] : *MainKeys) MainKeyed.try_emplace(Key, Pos);
        MainKeysOf = MainKeys;
    }
    std::map<const Zone*, Filed> ZonesWalked;   // the zones read whole (no index)
    std::vector<const Zone*> Order;
    for (const bool B : { false, true })
        for (const auto& Z : Zones)
            if (Z->Before == B)
            {
                Order.push_back(Z.get());
                if (Z->Index == nullptr) ZonesWalked.emplace(Z.get(), Build(Z->R, Z->Bytes));
                else if (Z->Hints.Keys == nullptr)
                    Z->Hints.Keys = &Z->Index->Keys, Z->Hints.ByHash = &Z->Index->ByHash;
            }
    const auto KeyTarget = [&](uint64_t Key) -> uint64_t {
        const auto& M = MainWhole ? MainKeyed : MainWalked.Keys;
        if (const auto It = M.find(Key); It != M.end()) return MainBase + (uint64_t)It->second;
        for (const Zone* Z : Order)
        {
            const auto& K = Z->Index != nullptr ? Z->Hints.Keyed() : ZonesWalked.at(Z).Keys;
            if (const auto It = K.find(Key); It != K.end()) return Z->Base + (uint64_t)It->second;
        }
        return 0;
    };
    const auto HashTarget = [&](const std::string& Type, uint64_t Hash) -> uint64_t {
        const std::pair<std::string, uint64_t> Key{ Type, Hash };
        const auto& M = MainWhole ? *MainHashes : MainWalked.Hashes;
        if (const auto It = M.find(Key); It != M.end()) return MainBase + (uint64_t)It->second;
        for (const Zone* Z : Order)
        {
            const auto& H = Z->Index != nullptr ? Z->Index->ByHash : ZonesWalked.at(Z).Hashes;
            if (const auto It = H.find(Key); It != H.end()) return Z->Base + (uint64_t)It->second;
        }
        return 0;
    };
    // a target in a lazy zone where it has not walked: wanted there
    std::map<Zone*, std::vector<int64_t>> Need;
    const auto Reached = [&](uint64_t Target) {
        for (const auto& Z : Zones)
            if (Z->Index != nullptr && !Z->Failed && Target >= Z->Base && Target < Z->Base + Z->Size)
            {
                const int64_t Pos = (int64_t)(Target - Z->Base);
                const size_t P = Z->Walker ? Z->Walker->OwnerOf(Pos) : SIZE_MAX;
                if (!Z->Walker || (P != SIZE_MAX && !Z->Walker->Walked().count(P))) Need[Z.get()].push_back(Pos);
                return;
            }
    };
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
                if (Typed) Target = HashTarget(R.OwnerNames[Type->second], Value & Rules.AssetHashMask);
            }
            else
                Target = KeyTarget(Value & Rules.KeyMask);
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
            Reached(Target);
        }
    };
    const auto PutBeside = [this](uint64_t Field, uint64_t Value) { Put(Field, Value); };
    Resolve(Main, MainBase, MainBytes.size(), "", false, PutMain);
    for (const auto& Z : Zones) Resolve(Z->R, Z->Base, Z->Size, Z->Name, Z->Before, PutBeside);
    // the lazy zones' assets the references reach, walked round after round, their own references
    // resolved in turn
    while (!Need.empty())
    {
        std::map<Zone*, std::vector<int64_t>> Round;
        Round.swap(Need);
        for (auto& [Z, Positions] : Round)
        {
            std::set<size_t> New;
            if (!Reach(*Z, Positions, Counters, New) || New.empty()) continue;
            Resolve(Z->Walker->Merged(&New), Z->Base, Z->Size, Z->Name, Z->Before, PutBeside);
        }
    }
    Counters["links_across_zones"] += Linked;
    Counters["refs_unresolved_across_zones"] += Unlinked;
    Counters["refs_unresolved_across_zones_assets"] += UnlinkedAssets;
    if (UnlinkedBefore != 0) Counters["refs_unresolved_in_zones_before"] += UnlinkedBefore;
}

bool Set::Reach(Zone& Z, const std::vector<int64_t>& Positions, std::map<std::string, size_t>& Counters, std::set<size_t>& New)
{
    const ZoneTables::StreamRules& Rules = Z.Loaded->Profile.Rules;
    bool Ok = true;
    if (!Z.Walker)
    {
        std::string Error;
        if (!FastFile::Decompress(Z.Path, Z.Bytes, Error) || Z.Bytes.size() != Z.Size)
        {
            Z.Failed = true;
            Counters["beside_zones_unread"]++;
            return false;
        }
        Counters["beside_zones_read_lazily"]++;
        Counters["beside_zones_mb"] += Z.Bytes.size() >> 20;
        Z.Hints.Keys = &Z.Index->Keys;
        Z.Hints.ByHash = &Z.Index->ByHash;
        ZoneTables::WalkOnLargeStack([&] {
            // (the points moved in: the walk's from here on)
            Z.Walker = std::make_unique<ZoneTables::PartialWalk>(Z.Bytes, Z.Loaded->Tables, Z.Loaded->Profile.TypeNames, Rules, std::move(Z.Index->Points));
            Z.Hints.Points = &Z.Walker->ResumePoints();
            Ok = Z.Walker->Ok();
            if (!Ok) return;
            Z.Walker->UseKeys(Z.Hints.Keyed());
            Z.Walker->AddAssetHashes(Z.Index->ByHash);
        });
        if (!Ok)
        {
            Z.Failed = true;
            Z.Walker.reset();
            Counters["beside_zones_unread"]++;
            return false;
        }
    }
    std::vector<size_t> Targets;
    for (const int64_t Pos : Positions)
        if (const size_t P = Z.Walker->OwnerOf(Pos); P != SIZE_MAX && !Z.Walker->Walked().count(P)) Targets.push_back(P);
    if (Targets.empty()) return true;
    std::set<size_t> Had;
    for (const auto& [P, Result] : Z.Walker->Walked()) Had.insert(P);
    ZoneTables::WalkOnLargeStack([&] { Ok = WalkReached(*Z.Walker, Targets, Rules, Z.Hints, Z.Seen, Counters["beside_partial_rounds"], SIZE_MAX); });
    for (const auto& [P, Result] : Z.Walker->Walked())
        if (!Had.count(P)) New.insert(P);
    Counters["beside_assets_walked"] += New.size();
    Z.R = Z.Walker->Merged();
    // its own pointers, each written once (a field Link wrote since keeps Link's target)
    for (const auto& [Field, Target] : ResolveLinks(Z.R, Rules, false, Z.Bytes, &Z.Hints).Links)
        if (Field >= 0 && (uint64_t)Field + 8 <= Z.Bytes.size() && Z.OwnLinked.insert(Field).second)
        {
            const uint64_t V = Z.Base + (uint64_t)Target;
            std::memcpy(Z.Bytes.data() + Field, &V, 8);
        }
    {
        std::lock_guard<std::mutex> G(HeldLock);
        Held.clear();
    }
    return Ok;
}

void Set::Serve() const
{
    if (CoDAssets::GameInstance == nullptr) return;
    for (const auto& Z : Zones)
        if (!Z->Bytes.empty()) CoDAssets::GameInstance->AddVirtual(Z->Bytes.data(), Z->Bytes.size(), Z->Base);
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
