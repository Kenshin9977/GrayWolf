#include "stdafx.h"

#include "Games/IW/IWZone.h"

#include "CoDAssets.h"
#include "ExportNaming.h"
#include "ExportWriters.h"
#include "FileSystems.h"
#include "MapLive.h"
#include "SABSupport.h"
#include "SettingsManager.h"
#include "Strings.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <format>
#include <fstream>
#include <tuple>
#include <unordered_map>
#include <unordered_set>

namespace fs = std::filesystem;

namespace
{
    std::vector<IWZone::PakEntry> ReadStreamTable(const std::string& FastFilePath)
    {
        std::vector<IWZone::PakEntry> Out;
        std::ifstream F(FastFilePath, std::ios::binary);
        uint32_t Count = 0;
        F.seekg(24);
        F.read(reinterpret_cast<char*>(&Count), 4);
        if (!F || Count > (1u << 20)) return Out;
        Out.resize(Count);
        static_assert(sizeof(IWZone::PakEntry) == 24, "the .ff's stream entry");
        F.read(reinterpret_cast<char*>(Out.data()), (std::streamsize)Count * sizeof(IWZone::PakEntry));
        if (!F) Out.clear();
        return Out;
    }
}

size_t IWZone::LinkLodSurfaces(const LodLayout& L, const std::function<uint64_t(int64_t ModelSurfs)>& Elsewhere)
{
    size_t Unlinked = 0;
    for (const auto& I : Walk().Instances)
    {
        if (!OwnerIs(I, L.Owner)) continue;
        const int64_t N = std::min<int64_t>(At<uint8_t>(Image(), I.Pos + L.LodCount), L.MaxLods);
        for (int64_t Lod = 0; Lod < N; Lod++)
        {
            const int64_t Base = I.Pos + L.Lods + Lod * L.LodSize;
            const uint64_t ModelSurfs = At<uint64_t>(Image(), Base + L.LodModelSurfs);
            if (!Live().Contains(ModelSurfs)) continue;
            uint64_t Surfs = At<uint64_t>(Image(), Off(ModelSurfs) + L.ModelSurfsSurfs);
            if (!Live().Contains(Surfs)) Surfs = Elsewhere ? Elsewhere(Off(ModelSurfs)) : 0;
            if (Surfs == 0)
            {
                Unlinked++;
                continue;
            }
            Live().Link((uint64_t)(Base + L.LodSurfs), (uint64_t)Off(Surfs));
        }
    }
    return Unlinked;
}

void IWZone::ReadStreams(int64_t StreamedAt)
{
    std::vector<int64_t> Streamed;
    for (const auto& I : Walk().Instances)
        if (OwnerIs(I, "image") && At<uint8_t>(Zone(), I.Pos + StreamedAt) > 0) Streamed.push_back(I.Pos);
    const std::vector<PakEntry> Table = ReadStreamTable(FastFilePath());
    Counter("images_streamed") += Streamed.size();
    if (Table.size() == Streamed.size() * 4)
    {
        for (size_t K = 0; K < Streamed.size(); K++)
            Streams[Streamed[K]] = { Table[4 * K], Table[4 * K + 1], Table[4 * K + 2], Table[4 * K + 3] };
    }
    else
        Counter("image_tables_mismatched")++;
}

std::vector<int64_t> IWZone::StreamOrder() const
{
    std::vector<int64_t> Order;
    for (const auto& Entry : Streams) Order.push_back(Entry.first);
    std::sort(Order.begin(), Order.end(), [&](int64_t A, int64_t B) {
        const PakEntry& X = Streams.at(A)[0];
        const PakEntry& Y = Streams.at(B)[0];
        return std::make_tuple(PakOf(X), X.Start, A) < std::make_tuple(PakOf(Y), Y.Start, B);
    });
    return Order;
}

void IWZone::MixStreams(int64_t Pos, ExportWriters::ContentKey& Key) const
{
    for (const PakEntry& E : Streams.at(Pos)) Key.Mix(PakOf(E)).Mix(E.Start).Mix(E.End);
}

std::string IWZone::StreamPak(const PakEntry& E) const
{
    const std::string& Ff = FastFilePath();
    return (E.Pak >> 16) == 0 ? Ff : (fs::path(Ff).parent_path() / std::format("imagefile{}.pak", (unsigned long long)(E.Pak >> 16))).string();
}

std::vector<std::string> IWZone::HeaderScriptStrings() const
{
    const auto& Z = Zone();
    const uint64_t Count = At<uint64_t>(Z, 0x48);
    if (At<uint64_t>(Z, 0x50) == Rules().Marker && Count != 0 && Count <= Z.size() / 8) return InlineStrings(Z, 0x70, Count, 8, Rules().Marker);
    return {};
}

void IWZone::DefineStreamed(std::vector<DiskGame::AssetId>& Out, int64_t NameAt, const std::vector<int64_t>& Also) const
{
    const auto Add = [&](int64_t Pos) {
        const std::string N = AssetName(DiskGame::Kind::Image, Pos + NameAt);
        if (!IsReference(DiskGame::Kind::Image, N, Pos)) Out.push_back({ DiskGame::Kind::Image, N });
    };
    for (const auto& [Pos, Entries] : Streams) Add(Pos);
    for (const int64_t Pos : Also) Add(Pos);
}

bool IWZone::ReadLevel(Maps::MapScene& Scene, std::map<std::string, uint64_t>& Models, const MapLayout& M,
                       const std::function<XMaterial_t(uint64_t)>& Material)
{
    constexpr int64_t WorldName = 8, WorldSurfaceCount = 0x1c, WorldSkyCount = 0x20, WorldSkies = 0x28, SkyImage = 0x10;
    constexpr int64_t VertexSize = 44;
    const int64_t World = Root(M.World);
    if (World < 0) return false;
    const auto& Img = Image();
    FILE* Dbg = Maps::DebugFile();
    Scene.Name = StringAt(At<uint64_t>(Img, World + WorldName));
    if (Scene.Name.empty()) Scene.Name = Name();
    // the vertices: records or streams, in GfxWorld or in the asset that holds them
    int64_t Holder = M.VertexAsset ? Root(M.VertexAsset) : World;
    for (size_t K = 0; Holder < 0 && K < Walk().Instances.size(); K++)   // one GfxWorld holds (Infinite Warfare)
        if (OwnerIs(Walk().Instances[K], M.VertexAsset)) Holder = Walk().Instances[K].Pos;
    if (Holder < 0) return false;
    const bool Streams = M.Streams.Positions != 0;
    const uint32_t VertexCount = At<uint32_t>(Img, Holder + M.VertexCount);
    const uint32_t IndexCount = At<uint32_t>(Img, World + M.IndexCount);
    const auto Stream = [&](int64_t Field) { return Off(At<uint64_t>(Img, Holder + Field)); };
    const int64_t Vertices = Stream(Streams ? M.Streams.Positions : M.Vertices);
    const int64_t Colors = Streams ? Stream(M.Streams.Colors) : -1, Uvs = Streams ? Stream(M.Streams.Uvs) : -1;
    const int64_t Normals = Streams ? Stream(M.Streams.Normals) : -1;
    const int64_t Indices = Off(At<uint64_t>(Img, World + M.Indices));
    const uint32_t SurfaceCount = At<uint32_t>(Img, World + WorldSurfaceCount);
    const int64_t Surfaces = Off(At<uint64_t>(Img, World + M.Surfaces));
    const int32_t BrushCount = At<int32_t>(Img, World + M.BrushCount);
    const int64_t Brushes = Off(At<uint64_t>(Img, World + M.Brushes));
    if (Dbg)
        std::fprintf(Dbg, "[map] %s vertices %u at %llx indices %u at %llx surfaces %u at %llx brushes %d at %llx\n", Scene.Name.c_str(),
                     VertexCount, (long long)Vertices, IndexCount, (long long)Indices, SurfaceCount, (long long)Surfaces, BrushCount, (long long)Brushes);
    const auto Normal = [](uint32_t N, float* Out) {
        for (int K = 0; K < 3; K++) Out[K] = (float)((N >> (10 * K)) & 0x3ff) / 1023.0f * 2.0f - 1.0f;
    };
    // A level split over several transient zones (its own and those TransientsBeside gives): each
    // zone's records placed one after the other in one vertex space, a surface's first vertex moved
    // to where its zone starts (VertexBase)
    std::map<int32_t, TransientVertices> Transients;
    std::vector<std::pair<uint32_t, TransientVertices>> Spans;   // where each zone starts, in order
    std::unordered_map<int32_t, uint32_t> SpanOf;
    if (M.SurfaceTransient >= 0 && !Streams && M.VertexAsset)
    {
        for (const auto& I : Walk().Instances)
            if (OwnerIs(I, M.VertexAsset))
                Transients.try_emplace(At<int32_t>(Img, I.Pos + M.TransientNumber),
                                       TransientVertices{ &Img, Off(At<uint64_t>(Img, I.Pos + M.Vertices)), At<uint32_t>(Img, I.Pos + M.VertexCount) });
        TransientsBeside(Transients);
        if (Transients.size() > 1)
        {
            uint32_t Next = 0;
            for (const auto& [Number, T] : Transients)
            {
                SpanOf[Number] = Next;
                Spans.emplace_back(Next, T);
                Next += T.Count;
            }
            Counter("map_transient_zones") += Transients.size();
        }
    }
    // a record: position, binormal sign, colour, uv, then its packed normal at 36
    const auto Record = [&](uint32_t I, Maps::Vertex& V) {
        const std::vector<uint8_t>* Bytes = &Img;
        int64_t From = Vertices;
        uint32_t Count = VertexCount;
        if (!Spans.empty())
        {
            const auto It = std::upper_bound(Spans.begin(), Spans.end(), I, [](uint32_t X, const auto& S) { return X < S.first; });
            if (It == Spans.begin()) return false;
            const auto& [Start, T] = *std::prev(It);
            Bytes = T.Bytes, From = T.Vertices, Count = T.Count, I -= Start;
        }
        if (I >= Count || From < 0) return false;
        const int64_t P = From + (int64_t)I * VertexSize;
        if (P + VertexSize > (int64_t)Bytes->size()) return false;
        for (int K = 0; K < 3; K++) V.Pos[K] = At<float>(*Bytes, P + K * 4);
        const uint32_t Color = At<uint32_t>(*Bytes, P + 16);
        std::memcpy(V.Color, &Color, 4);
        V.Uv[0] = At<float>(*Bytes, P + 20);
        V.Uv[1] = At<float>(*Bytes, P + 24);
        Normal(At<uint32_t>(*Bytes, P + 36), V.Normal);
        return true;
    };
    // the streams, each where its asset has it
    const auto Streamed = [&](uint32_t I, Maps::Vertex& V) {
        if (I >= VertexCount || Vertices < 0) return false;
        for (int K = 0; K < 3; K++) V.Pos[K] = At<float>(Img, Vertices + (int64_t)I * 12 + K * 4);
        if (Colors >= 0)
        {
            const uint32_t Color = At<uint32_t>(Img, Colors + (int64_t)I * 4);
            std::memcpy(V.Color, &Color, 4);
        }
        if (Uvs >= 0)
            for (int K = 0; K < 2; K++) V.Uv[K] = At<float>(Img, Uvs + (int64_t)I * 8 + K * 4);
        if (Normals >= 0) Normal(At<uint32_t>(Img, Normals + (int64_t)I * 4), V.Normal);
        return true;
    };
    const Maps::WorldBuilder::Fetch Fetch = Streams ? Maps::WorldBuilder::Fetch(Streamed) : Maps::WorldBuilder::Fetch(Record);
    Maps::WorldLayout L;
    L.Image = &Img;
    L.Surfaces = Surfaces, L.SurfaceCount = SurfaceCount;
    L.Brushes = Brushes, L.BrushCount = (uint32_t)std::max(BrushCount, 0);
    L.Indices = Indices, L.IndexCount = IndexCount;
    L.SurfaceSize = M.SurfaceSize, L.FirstVertex = M.SurfaceFirstVertex, L.TriCount = M.SurfaceTris, L.BaseIndex = M.SurfaceBaseIndex;
    L.Material = M.SurfaceMaterial;
    L.BrushSize = M.BrushSize, L.FirstSurface = M.BrushFirstSurface, L.SurfaceCountAt = M.BrushSurfaceCount;
    L.MoreUvs = M.MoreUvs;
    if (!Spans.empty())
        L.VertexBase = [&](int64_t Surface) {
            const auto It = SpanOf.find(At<int32_t>(Img, Surface + M.SurfaceTransient));
            if (It != SpanOf.end()) return It->second;
            Counter("map_surfaces_transient_missing")++;
            return 0xF0000000u;   // no zone: its vertices are not read
        };
    const auto MaterialOf = [&](uint64_t Address) { return Off(Address) < 0 ? std::string() : Maps::AddMaterial(Scene, Material(Address)); };
    if (const size_t Bad = Maps::AddWorld(Scene, L, Fetch, MaterialOf)) Counter("map_triangles_unread") += Bad;

    AddStaticModels(Scene, Models, Off(At<uint64_t>(Img, World + M.SModels)), At<uint32_t>(Img, World + M.SModelCount), M.SModelSize, M.SModelModel);
    AddDynamicEntities(Scene, Models, M.Clip, M.DynCounts, M.DynLists, M.DynSize, M.DynDestroyed, M.DynPose);
    AddPrimaryLights(Scene, M.Lights, M.Com);

    // ---- the sky: the first sky's image, for the import script
    if (At<int32_t>(Img, World + WorldSkyCount) > 0)
        if (const int64_t Sky = Off(At<uint64_t>(Img, World + WorldSkies)); Sky >= 0)
            if (const int64_t SkyImg = Off(At<uint64_t>(Img, Sky + SkyImage)); SkyImg >= 0)
            {
                Scene.Sky.Image = AssetName(DiskGame::Kind::Image, SkyImg + M.ImageNameAt);
                Scene.Images[Scene.Sky.Image];
            }

    AddMapEnts(Scene);
    for (const auto& [Number, Key] : M.EntityKeys) Scene.EntityKeys[std::to_string(Number)] = Key;
    if (M.ClipBrushes.BrushSize) AddCollision(Scene, { M.Clip ? M.Clip : "col_map_sp", "col_map_mp" }, M.ClipBrushes);
    if (M.Triggers >= 0) AddTriggers(Scene, M.Triggers, "map_ents", M.TriggerShapes);
    return true;
}

// Advanced Warfare's and MWR's clipMap_t brushes (the s1 and h1 tables' col_map_mp): a ClipInfo at 0x10 (0xf8: planes 0/8,
// ClipMaterial 0x10 at 0x10/0x18 (a name, surface 8, contents 0xc); leaf brush nodes 0x20/0x28,
// 0x18 each; brush sides 0x60/0x68, 8 bytes, the plane a u32 number, a u16 material number at 4;
// brushes u16 0x80, records 0x88 (0x30: u16 numsides, sides 8, axial material numbers 0x18),
// Bounds 0x90, contents 0x98; triangles 0xa0/0xa8 and u16 0xb0/0xb8); brush models
// 0x130/0x138, cmodel_t 0x58 (a ClipInfo pointer at 0x20: the world's; leafBrushNode at 0x50).
// MapEnts' MapTriggers at 0x18
Maps::ClipLayout IWZone::AwClipBrushes()
{
    Maps::ClipLayout C;
    C.InfoAt = 0x10;
    C.Info.PlaneCount = 0, C.Info.Planes = 8, C.Info.MaterialCount = 0x10, C.Info.Materials = 0x18;
    C.Info.LeafBrushNodeCount = 0x20, C.Info.LeafBrushNodes = 0x28;
    C.Info.BrushCount = 0x80, C.Info.Brushes = 0x88, C.Info.BrushBounds = 0x90, C.Info.BrushContents = 0x98;
    C.MaterialSize = 0x10, C.MaterialName = 0, C.MaterialSurface = 8, C.MaterialContents = 0xc;
    C.BrushSize = 0x30, C.BrushSideCount = 0, C.BrushSideCountBytes = 2, C.BrushSides = 8, C.AxialMaterial = 0x18;
    C.SideSize = 8, C.SidePlane = 0, C.SidePlaneNumber = true, C.SideMaterial = 4, C.SideMaterialBytes = 2;
    C.ModelCount = 0x130, C.Models = 0x138, C.ModelSize = 0x58, C.ModelInfo = 0x20, C.ModelLeafBrushNode = 0x50;
    C.NodeSize = 0x18;
    C.VertexCount = 0x10 + 0xa0, C.Vertices = 0x10 + 0xa8, C.TriangleCount = 0x10 + 0xb0, C.Triangles = 0x10 + 0xb8;
    return C;
}

void IWZone::AddDynamicEntities(Maps::MapScene& Scene, std::map<std::string, uint64_t>& Models, const char* Clip, int64_t Counts,
                                int64_t Lists, int64_t Size, int64_t Destroyed, const DynPoseLayout& Pose)
{
    const int64_t DynQuat = Pose.Quat, DynOrigin = Pose.Origin, DynModel = Pose.Model;
    const int64_t ClipMap = Root(Clip);
    if (ClipMap < 0) return;
    const auto& Img = Image();
    FILE* Dbg = Maps::DebugFile();
    for (int List = 0; List < 2; List++)
    {
        const uint16_t Count = At<uint16_t>(Img, ClipMap + Counts + List * 2);
        const int64_t Dyn = Off(At<uint64_t>(Img, ClipMap + Lists + List * 8));
        for (uint16_t K = 0; Dyn >= 0 && K < Count; K++)
        {
            const int64_t E = Dyn + (int64_t)K * Size;
            const uint64_t Model = At<uint64_t>(Img, E + DynModel);
            const std::string ModelName = ModelAt(Model);
            if (ModelName.empty()) continue;
            Maps::Instance In;
            In.Name = "dynent_" + std::to_string(List) + "_" + std::to_string(K);
            In.Model = ModelName;
            float Q[4];
            for (int J = 0; J < 4; J++) Q[J] = At<float>(Img, E + DynQuat + J * 4);
            Maps::AxisFromQuat(Q, In.Axis);
            for (int J = 0; J < 3; J++) In.Origin[J] = At<float>(Img, E + DynOrigin + J * 4);
            if (Destroyed >= 0) In.Destroyed = ModelAt(At<uint64_t>(Img, E + Destroyed));
            if (Dbg)
                std::fprintf(Dbg, "[dyn] %s %s quat %g %g %g %g origin %g %g %g\n", In.Name.c_str(), ModelName.c_str(), Q[0], Q[1], Q[2], Q[3],
                             In.Origin[0], In.Origin[1], In.Origin[2]);
            Scene.DynamicInstances.push_back(In);
            Models.emplace(ModelName, Model);
        }
    }
}

void IWZone::AddPrimaryLights(Maps::MapScene& Scene, const LightLayout& L, const char* ComType)
{
    const int64_t Com = Root(ComType);
    if (Com < 0) return;
    const auto& Img = Image();
    FILE* Dbg = Maps::DebugFile();
    const uint32_t Count = At<uint32_t>(Img, Com + L.Count);
    const int64_t Lights = Off(At<uint64_t>(Img, Com + L.Array));
    for (uint32_t K = 0; Lights >= 0 && K < Count; K++)
    {
        const int64_t P = Lights + (int64_t)K * L.Size;
        const uint8_t Type = At<uint8_t>(Img, P + L.TypeAt);
        if (Type < 1 || Type > 3) continue;
        Maps::Light Out;
        Out.Kind = Type == 1 ? Maps::Light::Type::Sun : Type == 2 ? Maps::Light::Type::Spot : Maps::Light::Type::Point;
        for (int J = 0; J < 3; J++)
        {
            Out.Color[J] = At<float>(Img, P + L.Color + J * 4);
            Out.Dir[J] = At<float>(Img, P + L.Dir + J * 4);
            Out.Origin[J] = At<float>(Img, P + L.Origin + J * 4);
        }
        Out.Radius = At<float>(Img, P + L.Radius);
        Out.CosHalfFovOuter = At<float>(Img, P + L.CosHalfFov);
        if (Dbg)
            std::fprintf(Dbg, "[light] %u type %u color %g %g %g dir %g %g %g origin %g %g %g radius %g cos %g def '%s'\n", K, Type,
                         Out.Color[0], Out.Color[1], Out.Color[2], Out.Dir[0], Out.Dir[1], Out.Dir[2], Out.Origin[0], Out.Origin[1],
                         Out.Origin[2], Out.Radius, Out.CosHalfFovOuter, L.Def >= 0 ? StringAt(At<uint64_t>(Img, P + L.Def)).c_str() : "");
        Scene.Lights.push_back(Out);
    }
}

namespace
{
    // A bank sound's folder (cp\dlc4\p6\i) as a relative path, each part made safe
    fs::path SoundBankFolder(const std::string& Folder)
    {
        fs::path Out;
        for (const auto& Part : fs::path(Strings::Replace(Folder, "\\", "/")))
            if (Part != "/" && Part != "." && Part != "..") Out /= ExportNaming::SafeFileName(Part.string());
        return Out;
    }
}

std::string IWZone::SoundBankFile(const std::string& Name, const std::string& Extension)
{
    const fs::path File = SoundBankFolder(FileSystems::GetDirectoryName(Name)) /
                          (ExportNaming::SafeFileName(FileSystems::GetFileNamePurgeExtensions(Name)) + Extension);
    return File.generic_string();
}

void IWZone::ExportSoundBanks(const std::vector<std::string>& Banks, const DiskGame::Context& C, DiskGame::Totals& T,
                              const std::vector<std::string>& Patches)
{
    auto& Kt = T.Kinds[DiskGame::Kind::Sound];
    if (Banks.empty() && Patches.empty()) return;
    auto SavedPool = std::move(CoDAssets::GameAssets);
    auto SavedCache = std::move(CoDAssets::GamePackageCache);
    CoDAssets::GameAssets = std::make_unique<AssetPool>();
    CoDAssets::GamePackageCache = std::make_unique<CoDPackageCache>();
    size_t FromPatches = 0;   // the loaded assets from here on are the patch banks'
    for (const auto* List : { &Banks, &Patches })
    {
        if (List == &Patches) FromPatches = CoDAssets::GameAssets->LoadedAssets.size();
        for (const auto& Bank : *List)
        {
            CoDAssets::GamePackageCache->LoadPackageCacheAsync(Bank);
            SABSupport::ParseSAB(Bank);
        }
    }
    const std::string Extension = ExportWriters::SoundExtension(ExportWriters::SoundFormatSetting());
    const std::string Dir = C.KindDir(DiskGame::Kind::Sound);
    const auto& Loaded = CoDAssets::GameAssets->LoadedAssets;
    // a patch bank's entry replaces the banks' entry of the same key
    std::unordered_set<uint64_t> Patched;
    for (size_t N = FromPatches; N < Loaded.size(); N++)
        if (const auto* Sound = dynamic_cast<const CoDSound_t*>(Loaded[N]); Sound != nullptr && Sound->BankKey != 0)
            Patched.insert(Sound->BankKey);
    const auto Replaced = [&](size_t N, const CoDSound_t& S) { return N < FromPatches && S.BankKey != 0 && Patched.count(S.BankKey) != 0; };
    // a sound in both banks (a primed one: its start in the .sabl, the whole of it in the .sabs)
    // is written once, from the bank holding more of it (a patch bank's first)
    const auto Same = [](const CoDSound_t& S) {
        return std::format("{}/{}|{}|{}|{}", ExportNaming::Lower(S.FullPath), ExportNaming::Lower(S.AssetName), S.FrameRate, S.FrameCount,
                           (unsigned)S.ChannelsCount);
    };
    std::unordered_map<std::string, std::pair<const CoDSound_t*, bool>> Fullest;
    for (size_t N = 0; N < Loaded.size(); N++)
        if (const auto* Sound = dynamic_cast<const CoDSound_t*>(Loaded[N]); Sound != nullptr && !Replaced(N, *Sound))
        {
            auto& [Kept, KeptPatch] = Fullest[Same(*Sound)];
            const bool Patch = N >= FromPatches;
            if (Kept == nullptr || (Patch && !KeptPatch) || (Patch == KeptPatch && Sound->AssetSize > Kept->AssetSize)) Kept = Sound, KeptPatch = Patch;
        }
    ExportWriters::SoundFiles Files;
    for (size_t N = 0; N < Loaded.size(); N++)
    {
        const auto* Sound = dynamic_cast<const CoDSound_t*>(Loaded[N]);
        if (Sound == nullptr) continue;
        if (!C.Advance(N, Loaded.size(), Sound->AssetName)) break;
        if (!C.Wants(DiskGame::Kind::Sound, Sound->AssetName)) continue;
        if (Replaced(N, *Sound))
        {
            Counter("sounds_replaced_by_patch")++;
            continue;
        }
        if (Fullest[Same(*Sound)].first != Sound)
        {
            Counter("sounds_in_both_banks")++;
            continue;
        }
        if (N >= FromPatches) Counter("sounds_from_patch")++;
        Kt.Seen++;
        CoDSound_t Named = *Sound;
        Named.AssetName = ExportNaming::SafeFileName(Sound->AssetName);
        std::string Crash;
        const int Code = Files.Write(Named, fs::path(Dir) / SoundBankFolder(Sound->FullPath), Extension, Crash);
        if (Code == static_cast<int>(ExportGameResult::Success)) Kt.Written++;
        else T.Fail(Name(), DiskGame::Kind::Sound, Sound->AssetName, std::format("code {} {}", Code, Crash.c_str()));
    }
    if (Files.Renamed) Counter("sounds_renamed") += Files.Renamed;
    if (Files.Copies) Counter("sounds_same_file") += Files.Copies;
    CoDAssets::GameAssets = std::move(SavedPool);
    CoDAssets::GamePackageCache = std::move(SavedCache);
}

std::vector<std::string> IWZone::InlineScriptStrings(const std::vector<uint8_t>& Z)
{
    std::vector<std::string> Out;
    if (Z.size() < 0x20) return Out;
    const uint32_t Count = ExportNaming::At<uint32_t>(Z, 0);
    const uint64_t List = ExportNaming::At<uint64_t>(Z, 8);
    const auto Inline = [](uint64_t V) { return V == ~0ull || V == ~1ull; };
    if (!Inline(List) || Count == 0 || 0x20 + (uint64_t)Count * 8 > Z.size()) return Out;
    Out.resize(Count);
    size_t Text = 0x20 + (size_t)Count * 8;
    for (uint32_t K = 0; K < Count && Text < Z.size(); K++)
    {
        if (!Inline(ExportNaming::At<uint64_t>(Z, 0x20 + (int64_t)K * 8))) continue;
        const auto End = std::find(Z.begin() + (std::ptrdiff_t)Text, Z.end(), 0);
        Out[K].assign(Z.begin() + (std::ptrdiff_t)Text, End);
        Text = (size_t)(End - Z.begin()) + 1;
    }
    return Out;
}
