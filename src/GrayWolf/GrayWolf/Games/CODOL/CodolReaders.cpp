#include "stdafx.h"
#include "ByteView.h"

#include "Games/CODOL/CodolReaders.h"
#include "Games/IW4/Iw4Readers.h"

#include "CoDAssets.h"

#include <map>
#include <mutex>
#include <cstring>

// Every offset is a field the game's loader reads: profiles/codol tables (xmodel f89b20, its LOD
// f89990, xmodelsurfs f89840, the XSurface f870a0, material f88250, xanim f85b40).
namespace Codol
{
    namespace
    {
        // XModel (0x168): bones u8 at 4, root bones 5; u16 bone names, parents, quats, trans,
        // base matrices and material handles from 0x2c; lodInfo[4] of 0x38 at 0x48; numLods u8 at
        // 0x129
        constexpr int64_t ModelBones = 0x4, ModelRootBones = 0x5, ModelBoneNames = 0x2c, ModelParents = 0x30,
                          ModelQuats = 0x34, ModelTrans = 0x38, ModelBaseMat = 0x40, ModelMaterials = 0x44,
                          ModelLods = 0x48, LodSize = 0x38, ModelLodCount = 0x129;
        // XModelLodInfo: the XModelSurfs asset at 8 (its surfs at +4); the LOD's own copy of them at
        // +0x30 is the game's load-time copy (0x4899bc)
        constexpr int64_t ModelSurfsSurfs = 0x4;
        // XSurface (0x54): vertCount u16 4, triCount u16 6, triIndices 0x10, four i16 blend counts
        // 0x14, blend data 0x1c, 32-byte GfxPackedVertex 0x20, 12-byte XRigidVertList count 0x28 and
        // array 0x2c
        constexpr int64_t SurfSize = 0x54, SurfVerts = 0x4, SurfTris = 0x6, SurfTriIndices = 0x10,
                          SurfBlendCounts = 0x14, SurfBlend = 0x1c, SurfVerts0 = 0x20, SurfVertListCount = 0x28,
                          SurfVertList = 0x2c;
        // Material (0x68): the name at 0x10, textureCount u8 0x51, the 12-byte MaterialTextureDef
        // table at 0x5c {nameHash u32, ..., semantic u8 at 7, image at 8}
        constexpr int64_t MaterialName = 0x10, MaterialTextureCount = 0x51, MaterialTextureTable = 0x5c;
        constexpr int64_t TextureDefSize = 0xc;

        // A streamed XModelSurfs file (read by 0x499ec0): a 0x58-byte header {version 1, a
        // build hash, then four sections of 0x14 bytes {size, file offset, ...}}: A (the
        // XModelSurfs struct, its name, its XSurfaces, their blend data and rigid lists: pointers
        // relative to A), D, C (the vertices), B (the triangle indices). Served here as A | C | B
        // at a base below 4 GB, its pointers made absolute.
        constexpr size_t StreamHeader = 0x58;
        struct Streamed
        {
            std::vector<uint8_t> Bytes;
            uint64_t Base = 0;
            uint64_t Surfs = 0;   // the XModelSurfs struct's address
        };
        std::mutex StreamLock;
        StreamedFile Source;
        std::map<std::string, std::unique_ptr<Streamed>> Served;
        uint64_t NextBase = 0x60000000;

        bool Section(ByteView F, int K, uint32_t& Offset, uint32_t& Size)
        {
            std::memcpy(&Size, F.data() + 8 + K * 0x14, 4);
            std::memcpy(&Offset, F.data() + 0xc + K * 0x14, 4);
            return (uint64_t)Offset + Size <= F.size();
        }

        // a section-relative pointer made absolute; offset 0 is the section's start, so whether the
        // pointer exists is said by its count, not by its value
        void Fix(std::vector<uint8_t>& B, uint64_t At, uint64_t Add, bool Present)
        {
            uint32_t V;
            std::memcpy(&V, B.data() + At, 4);
            V = Present ? (uint32_t)(V + Add) : 0;
            std::memcpy(B.data() + At, &V, 4);
        }

        template <class T>
        T Field(ByteView B, uint64_t At)
        {
            T V;
            std::memcpy(&V, B.data() + At, sizeof(T));
            return V;
        }

        // the XModelSurfs a LOD streams, served beside the zone: its address, 0 when not found
        uint64_t StreamedSurfs(const std::string& Name)
        {
            std::lock_guard<std::mutex> G(StreamLock);
            auto& S = Served[Name];
            if (!S)
            {
                std::vector<uint8_t> F;
                uint32_t AO, AS, CO, CS, BO, BS;
                if (!Source || !Source(Name, F) || F.size() < StreamHeader || !Section(F, 0, AO, AS) ||
                    !Section(F, 2, CO, CS) || !Section(F, 1, BO, BS) || AS < 0x30)
                    return 0;
                S = std::make_unique<Streamed>();
                auto& B = S->Bytes;
                B.insert(B.end(), F.begin() + AO, F.begin() + AO + AS);
                B.insert(B.end(), F.begin() + CO, F.begin() + CO + CS);
                B.insert(B.end(), F.begin() + BO, F.begin() + BO + BS);
                S->Base = NextBase;
                NextBase += (B.size() + 0xffff) & ~0xffffull;
                const uint64_t A = S->Base, C = S->Base + AS, Bx = S->Base + AS + CS;
                uint32_t SurfsAt = 0;
                uint16_t Count = 0;
                std::memcpy(&SurfsAt, B.data() + 4, 4);
                std::memcpy(&Count, B.data() + 8, 2);
                Fix(B, 0, A, true);
                Fix(B, 4, A, Count > 0);
                for (uint16_t K = 0; K < Count && SurfsAt + (K + 1) * SurfSize <= AS; K++)
                {
                    const uint64_t P = SurfsAt + K * SurfSize;
                    bool Blend = false;
                    for (int W = 0; W < 4; W++) Blend |= Field<int16_t>(B, P + SurfBlendCounts + W * 2) != 0;
                    Fix(B, P + SurfTriIndices, Bx, Field<uint16_t>(B, P + SurfTris) > 0);
                    Fix(B, P + SurfBlend, A, Blend);
                    Fix(B, P + SurfVerts0, C, Field<uint16_t>(B, P + SurfVerts) > 0);
                    Fix(B, P + SurfVertList, A, Field<uint32_t>(B, P + SurfVertListCount) > 0);
                }
                S->Surfs = A;
            }
            if (S->Surfs) CoDAssets::GameInstance->AddVirtual(S->Bytes.data(), S->Bytes.size(), S->Base);
            return S->Surfs;
        }
    }

    void ServeStreamedSurfaces(StreamedFile From)
    {
        std::lock_guard<std::mutex> G(StreamLock);
        Source = std::move(From);
        Served.clear();
        NextBase = 0x60000000;
    }

    namespace
    {
        // MW2's shapes with CODOL's sizes, 32-bit; XAnimParts (0x58): counts from 4, numframes u16
        // 0xe, flags 0x10, boneCount[10] and notifyCount from 0x11, framerate 0x28, the pointers
        // from 0x30, the delta part at 0x54
        Iw4Family::Layout Readers()
        {
            Iw4Family::Layout L;
            L.PointerSize = 4;
            L.ModelBones = ModelBones, L.ModelRootBones = ModelRootBones, L.ModelBoneNames = ModelBoneNames;
            L.ModelParents = ModelParents, L.ModelQuats = ModelQuats, L.ModelTrans = ModelTrans, L.ModelBaseMat = ModelBaseMat;
            L.ModelMaterials = ModelMaterials, L.ModelLods = ModelLods, L.LodSize = LodSize, L.ModelLodCount = ModelLodCount;
            L.ModelSurfsSurfs = ModelSurfsSurfs;
            L.SurfSize = SurfSize, L.SurfVerts = SurfVerts, L.SurfTris = SurfTris, L.SurfTriIndices = SurfTriIndices;
            L.SurfBlendCounts = SurfBlendCounts, L.SurfBlend = SurfBlend, L.SurfVerts0 = SurfVerts0;
            L.SurfVertListCount = SurfVertListCount, L.SurfVertList = SurfVertList;
            L.MaterialName = MaterialName, L.MaterialTextureCount = MaterialTextureCount, L.MaterialTextureTable = MaterialTextureTable;
            L.TextureDefSize = TextureDefSize, L.ImageName = ImageName;
            L.AnimFrames = 0xe, L.AnimFlags = 0x10, L.AnimCounts = 0x11, L.AnimRate = 0x28, L.AnimPointers = 0x30, L.AnimDelta = 0x54;
            L.StreamedSurfs = StreamedSurfs;   // from main/models
            return L;
        }
    }

    std::unique_ptr<XModel_t> ReadXModel(const CoDModel_t* Model) { return Iw4Family::ReadXModel(Model, Readers()); }
    std::unique_ptr<XAnim_t> ReadXAnim(const CoDAnim_t* Animation) { return Iw4Family::ReadXAnim(Animation, Readers()); }
    XMaterial_t ReadXMaterial(uint64_t MaterialPointer) { return Iw4Family::ReadXMaterial(MaterialPointer, Readers()); }
}
