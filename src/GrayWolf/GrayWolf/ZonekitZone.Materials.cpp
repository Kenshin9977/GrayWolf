#include "stdafx.h"

// A model's name-only material headers (",name": no image slot), given the images of the material
// they stand for: this zone's own, or another zone's (Context::MaterialsElsewhere, the install
// index's), read by the game's live material reader (ZonekitZone::ReadsMaterials). The game swaps
// a header for that material once loaded; the readers, reading the header, see no image.

#include "ZonekitZone.h"

#include "CoDAssetType.h"
#include "ExportNaming.h"
#include "ExportWriters.h"

#include <exception>
#include <unordered_map>

namespace
{
    // a material as the readers name it (",mc/mtl_x" and "mtl_x" alike): no ',', no folder, lower case
    std::string Key(std::string N)
    {
        if (!N.empty() && N[0] == ',') N.erase(0, 1);
        if (const auto Slash = N.find_last_of("/\\"); Slash != std::string::npos) N.erase(0, Slash + 1);
        if (!N.empty() && N[0] == ',') N.erase(0, 1);
        return ExportNaming::Lower(N);
    }
}

bool ZonekitZone::MaterialImages(DiskGame::MaterialBook& Out)
{
    using DiskGame::Kind;
    if (!MaterialOf || MaterialType == nullptr || !Session) return false;
    for (const auto& I : R.Instances)
    {
        if (!OwnerIs(I, MaterialType)) continue;
        const std::string N = AssetName(Kind::Material, I.Pos);
        if (N.empty() || IsReference(Kind::Material, N, I.Pos) || Out.count(N)) continue;
        XMaterial_t M(0);
        try
        {
            M = MaterialOf(Live().Addr((uint64_t)I.Pos));
        }
        catch (const std::exception&)
        {
            continue;
        }
        auto& Images = Out[N];
        for (const XImage_t& Image : M.Images)
        {
            std::string Name = Image.ImageName;
            if (!Name.empty() && Name[0] == ',') Name.erase(0, 1);
            if (!Name.empty()) Images.push_back({ (uint8_t)Image.ImageUsage, Image.SemanticHash, std::move(Name) });
        }
    }
    return true;
}

std::function<void(XMaterial_t& Material)> ZonekitZone::HeaderFiller(const DiskGame::Context& C)
{
    using DiskGame::Kind;
    if (!MaterialOf || MaterialType == nullptr) return nullptr;
    std::set<std::string> Defined, Headers;
    for (const auto& I : R.Instances)
    {
        if (!OwnerIs(I, MaterialType)) continue;
        const std::string N = AssetName(Kind::Material, I.Pos);
        if (!IsReference(Kind::Material, N, I.Pos)) Defined.insert(ExportNaming::Lower(N));
        else if (std::string H = Referenced(Kind::Material, N); !H.empty()) Headers.insert(std::move(H));
    }
    std::set<std::string> Here, Elsewhere;
    for (const auto& H : Headers) (Defined.count(ExportNaming::Lower(H)) ? Here : Elsewhere).insert(H);
    if (Here.empty() && Elsewhere.empty()) return nullptr;
    DiskGame::MaterialBook Book;
    if (!Here.empty())
    {
        DiskGame::MaterialBook Own;
        MaterialImages(Own);
        for (auto& [N, Images] : Own)
            if (Here.count(N)) Book[N] = std::move(Images);
    }
    // (another zone may be opened meanwhile, this one parked and resumed)
    if (!Elsewhere.empty() && C.MaterialsElsewhere) C.MaterialsElsewhere(*this, Elsewhere, Book);
    auto ByKey = std::make_shared<std::unordered_map<std::string, std::vector<DiskGame::MaterialImage>>>();
    for (auto& [N, Images] : Book)
        if (!Images.empty()) ByKey->try_emplace(Key(N), std::move(Images));
    Counter("material_headers") += Headers.size();
    if (ByKey->empty()) return nullptr;
    return [this, ByKey](XMaterial_t& M) {
        const auto It = ByKey->find(Key(M.MaterialName));
        if (It == ByKey->end()) return;
        for (const auto& I : It->second) M.Images.emplace_back((ImageUsageType)I.Usage, I.Hash, ExportWriters::ImageElsewhere, I.Image);
        Counter("model_materials_from_headers")++;
    };
}
