#include "stdafx.h"

// What a zone names only and other zones of the install hold (IWZone.h, ZonesHolding): the zones
// the install index says hold it, and their material books.

#include "Games/IW/IWZone.h"

#include "ExportNaming.h"
#include "ExportWriters.h"
#include "Games/Common/StringCipher.h"
#include "Games/IW/IwBesideZones.h"

#include <algorithm>
#include <filesystem>
#include <map>
#include <set>

namespace fs = std::filesystem;

bool IWZone::ZonesHolding(size_t Count, const std::function<std::vector<size_t>(const DiskGame::ZoneIndex& Index, size_t K)>& Find,
                          bool First, std::vector<std::string>& Paths, size_t& NotIndexed)
{
    if (Owner == nullptr || Install.empty()) return false;
    std::string Error;
    Park();
    const auto Index = DiskGame::ZoneIndex::For(*Owner, Options, Install, IndexKey(), nullptr, nullptr, Error);
    Resume();
    if (!Index) return false;
    const std::string Self = ExportNaming::Lower(Name());
    const auto Other = [&](size_t Z) { return ExportNaming::Lower(fs::path(Index->Zones()[Z]).stem().string()) != Self; };
    std::vector<size_t> Zones;
    if (First)
        for (size_t K = 0; K < Count; K++)
        {
            const auto In = Find(*Index, K);
            const auto It = std::find_if(In.begin(), In.end(), Other);
            if (It == In.end()) NotIndexed++;
            else if (std::find(Zones.begin(), Zones.end(), *It) == Zones.end()) Zones.push_back(*It);
        }
    else
    {
        std::map<size_t, std::set<size_t>> Gives;
        std::set<size_t> Left;
        for (size_t K = 0; K < Count; K++)
        {
            Left.insert(K);
            for (const size_t Z : Find(*Index, K))
                if (Other(Z)) Gives[Z].insert(K);
        }
        Zones = IwBeside::Cover(std::move(Gives), Left);
        NotIndexed += Left.size();
    }
    for (const size_t Z : Zones)
    {
        std::vector<std::string> Found;
        std::string Why;
        if (Owner->ResolveZones(Install, { Index->Zones()[Z] }, Options, Found, Why) && !Found.empty()) Paths.push_back(Found.front());
    }
    return true;
}

std::unique_ptr<IWMaterialBooks::Lookup> IWZone::MaterialBooks(const IWMaterialBooks::Layout& L, std::vector<std::string> Paths) const
{
    const fs::path Cache = !Options.Cache.empty() ? fs::path(Options.Cache) : fs::temp_directory_path() / "greyhound_books";
    std::vector<fs::path> Files(Paths.begin(), Paths.end());
    return std::make_unique<IWMaterialBooks::Lookup>(L, (Cache / (std::string(L.Profile) + "_materials")).string(), [Files] { return Files; });
}

std::vector<std::string> IWZone::MaterialHeaders() const
{
    std::set<std::string> Defined;
    std::map<std::string, std::string> Headers;   // lower-case -> as named
    for (const auto& I : Walk().Instances)
        if (OwnerIs(I, "material"))
        {
            const std::string N = AssetName(DiskGame::Kind::Material, I.Pos);
            if (!IsReference(DiskGame::Kind::Material, N, I.Pos)) Defined.insert(ExportNaming::Lower(N));
            else if (const std::string R = Referenced(DiskGame::Kind::Material, N); !R.empty()) Headers.emplace(ExportNaming::Lower(R), R);
        }
    std::vector<std::string> Out;
    for (const auto& [Lower, Name] : Headers)
        if (!Defined.count(Lower)) Out.push_back(Name);
    return Out;
}

void IWZone::BookMaterialsElsewhere(size_t Count, const std::function<std::vector<size_t>(const DiskGame::ZoneIndex& Index, size_t K)>& Find,
                                    bool First, const IWMaterialBooks::Layout& L)
{
    if (Count == 0) return;
    std::vector<std::string> Paths;
    if (!ZonesHolding(Count, Find, First, Paths, MaterialsNotIndexed))
    {
        Counter("materials_elsewhere_no_index") += Count;
        return;
    }
    MaterialsElsewhere = MaterialBooks(L, std::move(Paths));
}

void IWZone::Measures(DiskGame::Totals& T) const
{
    ZonekitZone::Measures(T);
    if (MaterialsElsewhere) MaterialsElsewhere->Measure(T, "materials_from_other_zones", "materials_in_no_other_zone");
    if (MaterialsNotIndexed) T.Counters["materials_not_indexed"] += MaterialsNotIndexed;
}

bool IWZone::HashedImagesElsewhere(const std::string& Hash, IWMaterialBooks::ImageList& Out, const std::function<std::string(uint64_t)>& Named) const
{
    if (MaterialsElsewhere == nullptr || !MaterialsElsewhere->Find(Hash, Out)) return false;
    for (auto& [Semantic, Name] : Out) Name = Named(std::stoull(Name, nullptr, 16));
    std::erase_if(Out, [](const auto& I) { return I.second.starts_with('$'); });
    return true;
}

bool IWZone::CipherDecrypt(const std::string& Raw, std::string& Text) const
{
    std::string Error;
    const auto* Tables = ExportWriters::ZoneProfile(Profile(), Error);
    const auto* Cipher = Tables != nullptr ? StringCipher::Load(Tables->Profile.Dir, Error) : nullptr;
    return StringCipher::Decrypt(Cipher, Raw, Text);
}
