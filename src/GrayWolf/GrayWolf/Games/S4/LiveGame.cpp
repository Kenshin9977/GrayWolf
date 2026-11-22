#include "stdafx.h"

// Vanguard's live readers, as the common code reaches them (ICoDGame.h), over the served zone. The
// sound banks are SAB version 17 (0x11) with 56-byte entries, their Opus packets after each entry's
// seek table (a bank of mp_shipment_s4: 97 entries at 0x760000, 2 s at 48 kHz in 14 163 bytes, not
// PCM). Models, materials and animations: the records of S4Assets.h, the facts behind them in
// README.md beside this file.

#include "CoDAssets.h"
#include "DBGameFiles.h"
#include "GameReaders.h"
#include "Games/Common/MeshDecode.h"
#include "Games/S4/S4Assets.h"
#include "Games/S4/S4Readers.h"
#include "ICoDGame.h"
#include "SABSupport.h"
#include "HalfFloats.h"
#include "SettingsManager.h"

#include <cmath>

thread_local GameReaders::ImagesElsewhereFn S4Readers::ImagesElsewhere;

// an image's name pointer first in it (GfxImage 0); a slot's semantic 0 the colour map
const XMaterial_t S4Readers::ReadXMaterial(uint64_t MaterialPointer)
{
    return GameReaders::ReadNamedMaterial<S4XMaterial, S4XMaterialImage>(
        MaterialPointer, 0, ImagesElsewhere, [](uint32_t Semantic) { return Semantic == 0 ? ImageUsageType::DiffuseMap : ImageUsageType::Unknown; });
}

namespace
{
    class GameVanguardLive final : public ICoDGame
    {
    public:
        const char* Engine() const override { return "S4"; }
        std::vector<PackageMark> Packages() const override { return { { PackageMark::Container::Sab, 0x11 } }; }
        const char* GetExportFolderName() const override { return "vanguard"; }

        // 4-byte bone names; additive when its type is 6; its blend shape weights (names 0x98,
        // weights 0xa0, count u16 0x90)
        std::unique_ptr<XAnim_t> ReadXAnim(const CoDAnim_t* Animation) const override
        {
            return GameReaders::ReadAnim<S4XAnim, S4XAnimDeltaParts>(Animation, 4, {}, [](XAnim_t& Anim, const S4XAnim& Data) {
                if (Data.AssetType == 0x6) Anim.AdditiveAnimation = true;
                Anim.BlendShapeWeightCount = Data.BlendShapeCount;
                Anim.BlendShapeNamesPtr = Data.BlendShapeNamesPtr;
                Anim.BlendShapeWeightsPtr = Data.BlendShapeWeightsPtr;
            });
        }

        // The skeleton (u16 parents), then each LOD record (0xc8, 0x40 each): its xmodelsurfs and
        // the surfaces it holds (the record's own copy is made at load time: empty in the zone),
        // one material handle per surface, LOD after LOD. A LOD whose xmodelsurfs another zone
        // defines (a name-only header, no surfaces) is left out, its handles skipped.
        std::unique_ptr<XModel_t> ReadXModel(const CoDModel_t* Model) const override
        {
            if (CoDAssets::GameInstance == nullptr || !CoDAssets::GameInstance->IsRunning()) return nullptr;
            auto* G = CoDAssets::GameInstance.get();
            const auto Data = G->Read<S4XModel>(Model->AssetPointer);
            auto Asset = std::make_unique<XModel_t>(Data.NumLods);
            GameReaders::CopyStreamedBones(*Asset, Model->AssetName, Data, 2);
            uint64_t Handles = Data.MaterialHandlesPtr;
            for (uint32_t i = 0; i < Data.NumLods && Data.ModelLodsPtr != 0; i++)
            {
                const auto Lod = G->Read<S4XModelLod>(Data.ModelLodsPtr + i * sizeof(S4XModelLod));
                const uint64_t Surfs = Lod.MeshPtr == 0 ? 0 : Lod.SurfsPtr != 0 ? Lod.SurfsPtr : G->Read<S4XModelMesh>(Lod.MeshPtr).SurfsPtr;
                if (Surfs == 0)
                {
                    Handles += (uint64_t)Lod.NumSurfs * sizeof(uint64_t);
                    continue;
                }
                Asset->ModelLods.emplace_back(Lod.NumSurfs);
                auto& L = Asset->ModelLods.back();
                L.LodDistance = Lod.LodDistance;
                L.LODStreamInfoPtr = Lod.MeshPtr;
                GameReaders::ReadSurfaces<S4XModelSurface, uint64_t>(L, Surfs, Lod.NumSurfs, Handles, S4Readers::ReadXMaterial,
                                                                     [](XModelSubmesh_t& Submesh, const S4XModelSurface& Info) {
                                                                         Submesh.RigidWeightsPtr = Info.RigidWeightsPtr;
                                                                         Submesh.VertListcount = Info.VertListCount;
                                                                         Submesh.VertexCount = Info.VertexCount;
                                                                         Submesh.FaceCount = Info.FacesCount;
                                                                         Submesh.VertexPtr = Info.Offsets[0];
                                                                         Submesh.VertexUVsPtr = Info.Offsets[1];
                                                                         Submesh.VertexNormalsPtr = Info.Offsets[2];
                                                                         Submesh.FacesPtr = Info.Offsets[3];
                                                                         Submesh.VertexColorPtr = Info.Offsets[6];
                                                                         // a scale of its own: the positions about the origin
                                                                         const bool Own = Info.NewScale != -1;
                                                                         Submesh.Scale = Own ? Info.NewScale : fmaxf(fmaxf(Info.Min, Info.Scale), Info.Max);
                                                                         Submesh.XOffset = Own ? 0 : Info.XOffset;
                                                                         Submesh.YOffset = Own ? 0 : Info.YOffset;
                                                                         Submesh.ZOffset = Own ? 0 : Info.ZOffset;
                                                                         for (int W = 0; W < 8; W++) Submesh.WeightCounts[W] = Info.WeightCounts[W];
                                                                         Submesh.WeightsPtr = Info.WeightsPtr;
                                                                         Submesh.BlendShapesPtr = Info.BlendShapesPtr;
                                                                     });
            }
            Asset->BlendShapeNamesPtr = G->Read<uint64_t>(Model->AssetPointer + ModelBlendShapes);
            return Asset;
        }

        // ---- blend shapes (the tables' xmodel 0x110 -> 0x6c7d550, a surface's 0xc0 -> 0x6c728f0)
        // The model's (0x18): shape count u16 0, name count u16 2, map count u16 4, the names (u32
        // script strings) at 8, the map at 0x10 (8 bytes each: {u16 name, u16 shape, f32}).
        // A surface's (0x20): its vertex records and deltas at 0 (u32 0x10 bytes), the vertices with
        // extended deltas u16 0x18 and the others u16 0x1a: a record per vertex (8 bytes: {u16 vertex,
        // u16 shape count, u32 offset}), the deltas at offset x 16 (extended) or x 8: {u16 shape,
        // 3 half floats} (+ 8 bytes extended).
        static constexpr uint64_t ModelBlendShapes = 0x110;

        static void ReadBlendShapes(const XModel_t& Model, const XModelLod_t& Lod, WraithModel& Out, size_t FirstSubmesh)
        {
            auto* G = CoDAssets::GameInstance.get();
            if (Model.BlendShapeNamesPtr == 0) return;
            const uint16_t ShapeCount = G->Read<uint16_t>(Model.BlendShapeNamesPtr), NameCount = G->Read<uint16_t>(Model.BlendShapeNamesPtr + 2);
            const uint16_t MapCount = G->Read<uint16_t>(Model.BlendShapeNamesPtr + 4);
            const uint64_t Names = G->Read<uint64_t>(Model.BlendShapeNamesPtr + 8), Map = G->Read<uint64_t>(Model.BlendShapeNamesPtr + 0x10);
            if (NameCount == 0 || Names == 0) return;
            Out.BlendShapes.clear();
            for (uint32_t i = 0; i < NameCount; i++) Out.BlendShapes.push_back(CoDAssets::GameStringHandler(G->Read<uint32_t>(Names + i * 4ull)));
            std::vector<uint32_t> NameOf(std::max<uint32_t>(ShapeCount, 1), 0);
            for (uint32_t i = 0; i < MapCount && Map != 0; i++)
            {
                const uint16_t Name = G->Read<uint16_t>(Map + i * 8ull), Shape = G->Read<uint16_t>(Map + i * 8ull + 2);
                if (Shape >= NameOf.size()) NameOf.resize(Shape + 1, 0);
                NameOf[Shape] = Name;
            }
            for (size_t k = 0; k < Lod.Submeshes.size() && FirstSubmesh + k < Out.Submeshes.size(); k++)
            {
                const uint64_t Info = Lod.Submeshes[k].BlendShapesPtr;
                if (Info == 0) continue;
                auto& Mesh = Out.Submeshes[FirstSubmesh + k];
                const uint64_t Data = G->Read<uint64_t>(Info);
                const uint32_t Size = G->Read<uint32_t>(Info + 0x10);
                const uint32_t Extended = G->Read<uint16_t>(Info + 0x18), Plain = G->Read<uint16_t>(Info + 0x1a);
                if (Data == 0 || Size == 0 || Size > (64u << 20)) continue;
                std::vector<uint8_t> B(Size);
                if (G->Read(B.data(), Data, Size) != Size) continue;
                const auto U16 = [&](size_t At) { return At + 2 <= B.size() ? *(const uint16_t*)(B.data() + At) : (uint16_t)0; };
                const auto U32 = [&](size_t At) { return At + 4 <= B.size() ? *(const uint32_t*)(B.data() + At) : 0u; };
                for (uint32_t i = 0; i < Extended + Plain; i++)
                {
                    const uint16_t Vertex = U16(i * 8ull), Shapes = U16(i * 8ull + 2);
                    const bool Ext = i < Extended;
                    size_t At = (size_t)U32(i * 8ull + 4) * (Ext ? 16 : 8);
                    if (Vertex >= Mesh.Verticies.size()) continue;
                    for (uint32_t j = 0; j < Shapes && At + 8 <= B.size(); j++)
                    {
                        const uint16_t Shape = U16(At);
                        const float X = HalfFloats::ToFloat(U16(At + 2)), Y = HalfFloats::ToFloat(U16(At + 4)), Z = HalfFloats::ToFloat(U16(At + 6));
                        At += Ext ? 16 : 8;
                        if (Shape < NameOf.size()) Mesh.Verticies[Vertex].BlendShapeDeltas.push_back({ NameOf[Shape], Vector3(X, Y, Z) });
                    }
                }
            }
        }

        // A LOD's mesh buffer: in the zone (its record's bytes) unless the record says streamed
        // (u8 0xc & 1), then the .xsub object the xmodelsurfs' key (0x10) names
        void LoadStreamedXModel(const std::unique_ptr<XModel_t>& Model, const XModelLod_t& ModelLOD, const std::unique_ptr<WraithModel>& ResultModel) const override
        {
            const bool ExportColors = SettingsManager::GetSetting("exportvtxcolor", "true") == "true";
            const auto MeshInfo = CoDAssets::GameInstance->Read<S4XModelMesh>(ModelLOD.LODStreamInfoPtr);
            const auto BufferInfo = CoDAssets::GameInstance->Read<S4XModelMeshBufferInfo>(MeshInfo.MeshBufferPointer);
            const bool Streamed = (BufferInfo.Streamed & 1) != 0 || BufferInfo.BufferPtr == 0;
            if (Streamed && CoDAssets::GamePackageCache == nullptr) return;
            const auto Mesh = !Streamed ? MeshDecode::FromZone(BufferInfo.BufferPtr, BufferInfo.BufferSize)
                                        : MeshDecode::FromPackage(MeshInfo.LODStreamKey, (int32_t)BufferInfo.BufferSize);
            // 21-bit positions, Modern Warfare 2019's packed quaternion normals, half UVs, u16 faces;
            // rigid runs of 8 bytes {u16 bone, u16 vertices, ...}, the skinned weights in the zone
            constexpr MeshDecode::LodFormat Format{ { MeshDecode::PositionCodec::Packed21, MeshDecode::NormalCodec::PackedQuat, MeshDecode::UvCodec::Half2,
                                                      MeshDecode::FaceCodec::U16 },
                                                    { 8, 2 },
                                                    false };
            const size_t First = ResultModel->Submeshes.size();
            if (Mesh.Data != nullptr) MeshDecode::DecodeLod(Mesh.View(), ModelLOD, Format, ExportColors, *ResultModel);
            if (Mesh.Data != nullptr && Model != nullptr) ReadBlendShapes(*Model, ModelLOD, *ResultModel, First);
        }

        // A bank file's entry, or a bank's streamed entry (the soundbank's list at 0x1e8: a 56-byte
        // SABv17Entry in the served zone) whose offset is the .xsub object holding its seek table then
        // its Opus packets
        std::unique_ptr<XSound> ReadXSound(const CoDSound_t* Sound) const override
        {
            if (Sound->IsFileEntry) return SABSupport::LoadOpusSound(Sound, SABSupport::OpusPreSkipUnmeasured);
            if (CoDAssets::GameInstance == nullptr || CoDAssets::GamePackageCache == nullptr) return nullptr;
            const auto Entry = CoDAssets::GameInstance->Read<SABv17Entry>(Sound->AssetPointer);
            uint32_t Got = 0;
            const auto Data = CoDAssets::GamePackageCache->ExtractPackageObject(Entry.Offset, (int32_t)(Entry.Size + Entry.SeekTableLength), Got);
            if (Data == nullptr || Got <= Entry.SeekTableLength) return nullptr;
            return SABSupport::OpusPackets(Data.get(), Got, Entry.SeekTableLength, Entry.Size, Entry.FrameRate, Entry.ChannelCount, Entry.FrameCount,
                                           SABSupport::OpusPreSkipUnmeasured);
        }
        DeltaBitness GetDeltaBitness() const override { return DeltaBitness::Bits64; }
        NotetrackFormat GetNotetrackFormat() const override { return NotetrackFormat::VG; }
        BlendShapeFormat GetBlendShapeFormat() const override { return BlendShapeFormat::MW; }
    };

    const GameVanguardLive Game;
    const LiveGameRegistration Registered(Game);
}
