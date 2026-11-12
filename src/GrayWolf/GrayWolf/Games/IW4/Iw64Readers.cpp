#include "stdafx.h"

#include "Games/IW4/Iw64Readers.h"
#include "Games/IW4/Iw4Readers.h"
#include "Games/IW3/LoadDefImage.h"

#include "CoDAssets.h"
#include "Image.h"

#include <algorithm>
#include <cstring>
#include <vector>

// Every offset is a field the game's loader reads: profiles/iw4 and profiles/iw5 tables
// (xmodel, xmodelsurfs, the XSurface, xanim, material, image readers).
namespace Iw64
{
    // Map: gfx_map's table (draw at 0x78: vd and indices; models 60 bytes; dpvs), com_map's light
    // records (0x48 / 0x58), col_map_sp's dynamic entity lists (0x70 / 0x78)
    const Layout Mw2{ "Modern Warfare 2", 0x50, 0x60, 0x88,
                      { 0x78 + 0x50, 0x78 + 0x58, 0x78 + 0x80, 0x78 + 0x88, 0x160, 0x168, 0x2a0,
                        0x48, 28, 40, 44, 0x148, 0x150, 0x70 } };
    const Layout Mw3{ "Modern Warfare 3", 0x56, 0x68, 0x98,
                      { 0x78 + 0x68, 0x78 + 0x70, 0x78 + 0x98, 0x78 + 0xa0, 0x178, 0x180, 0x2b8,
                        0x58, 40, 52, 56, 0x188, 0x190, 0x78 } };

    thread_local std::function<uint64_t(const std::string& Name)> MaterialDefinedAt;
    thread_local std::function<bool(const std::string& Name, std::vector<std::pair<uint32_t, std::string>>& Images)> MaterialImagesElsewhere;

    namespace
    {
        template <class T>
        T Get(uint64_t Address) { return CoDAssets::GameInstance->Read<T>(Address); }

        // XModel 0x198 / 0x1a0: bone names u16 script strings, lodInfo[4] of 0x38 (the LOD's own
        // surfs pointer at +0x28 is the game's load-time copy of modelSurfs->surfs); XSurface 0x58
        // with a 32-byte GfxPackedVertex and a 16-byte XRigidVertList; MaterialTextureDef 0x10;
        // XAnimParts 0x88: numframes 0x12, flags 0x14 (bit 0: looping), counts 0x15, framerate
        // 0x2c, the pointers from 0x38, the delta part 0x80
        Iw4Family::Layout Readers(const Layout& G)
        {
            Iw4Family::Layout L;
            L.PointerSize = 8;
            L.ModelBones = 0x8, L.ModelRootBones = 0x9, L.ModelBoneNames = 0x28, L.ModelParents = 0x30, L.ModelQuats = 0x38;
            L.ModelTrans = 0x40, L.ModelBaseMat = 0x50, L.ModelMaterials = 0x58, L.ModelLods = 0x60, L.LodSize = 0x38;
            L.ModelLodCount = 0x141, L.ModelSurfsSurfs = 0x8;
            L.SurfSize = 0x58, L.SurfVerts = 0x2, L.SurfTris = 0x4, L.SurfTriIndices = 0x10, L.SurfBlendCounts = 0x18;
            L.SurfBlend = 0x20, L.SurfVerts0 = 0x28, L.SurfVertListCount = 0x30, L.SurfVertList = 0x38;
            L.MaterialName = 0, L.MaterialTextureCount = G.MaterialTextureCount, L.MaterialTextureTable = G.MaterialTextureTable;
            L.TextureDefSize = 0x10, L.ImageName = ImageName;
            L.MaterialByName = MaterialDefinedAt, L.MaterialImagesByName = MaterialImagesElsewhere;
            L.AnimFrames = 0x12, L.AnimFlags = 0x14, L.LoopMask = 1, L.AnimCounts = 0x15, L.AnimRate = 0x2c;
            L.AnimPointers = 0x38, L.AnimDelta = 0x80;
            return L;
        }
    }

    std::unique_ptr<XModel_t> ReadXModel(const CoDModel_t* Model, const Layout& L) { return Iw4Family::ReadXModel(Model, Readers(L)); }
    // the anim layout is the same in both games
    std::unique_ptr<XAnim_t> ReadXAnim(const CoDAnim_t* Animation) { return Iw4Family::ReadXAnim(Animation, Readers(Mw2)); }
    XMaterial_t ReadXMaterial(uint64_t MaterialPointer, const Layout& L) { return Iw4Family::ReadXMaterial(MaterialPointer, Readers(L)); }

    namespace
    {
        bool Copy(uint8_t* To, uint64_t From, uint64_t N)
        {
            uintptr_t Read = 0;
            const auto Pixels = CoDAssets::GameInstance->Read(From, N, Read);
            const bool Ok = Pixels != nullptr && Read == N;
            if (Ok) std::memcpy(To, Pixels, N);
            delete[] Pixels;
            return Ok;
        }
    }

    std::unique_ptr<XImageDDS> ZoneImage(uint64_t ImagePointer)
    {
        const uint64_t Def = Get<uint64_t>(ImagePointer + ImageLoadDef);
        if (Def == 0) return nullptr;
        const uint32_t Size = Get<uint32_t>(Def + LoadDefSize);
        if (Size == 0) return nullptr;
        const uint32_t W = Get<uint16_t>(ImagePointer + ImageWidth), H = Get<uint16_t>(ImagePointer + ImageHeight);
        if (W == 0 || H == 0) return nullptr;
        // the D3D9 formats the rebuild's GfxImageLoadDef carries (FourCC or D3DFMT), as the 32-bit
        // zones keep them; the largest level only: it comes first (of each face, for a cube map)
        ImageFormat Format;
        uint64_t Level = 0;
        LoadDefImage::Convert How = LoadDefImage::Convert::None;
        if (!LoadDefImage::D3dFormat(Get<uint32_t>(Def + LoadDefFormat), W, H, Format, Level, How)) return nullptr;
        const bool Cube = Get<uint8_t>(ImagePointer + ImageMapType) == 5;   // MAPTYPE_CUBE
        const uint64_t Faces = Cube ? 6 : 1;
        if (Level * Faces > Size) return nullptr;
        const uint64_t Chain = Cube ? Size / 6 : 0;
        std::vector<uint8_t> Pixels(Level * Faces);
        for (uint64_t F = 0; F < Faces; F++)
            if (!Copy(Pixels.data() + F * Level, Def + LoadDefData + F * Chain, Level)) return nullptr;
        return LoadDefImage::Dds(W, H, Format, Pixels.data(), Level, How, Faces, Level);
    }

    bool LoadedSound(uint64_t P, CoDSound_t& Sound)
    {
        // AILSOUNDINFO at +8: a WAVEFORMATEX, the payload length at +0x18, the payload at +0x30
        const uint64_t Info = P + 8;
        const uint16_t Format = Get<uint16_t>(Info);
        const uint16_t Channels = Get<uint16_t>(Info + 2);
        const uint32_t Rate = Get<uint32_t>(Info + 4);
        const uint16_t Bits = Get<uint16_t>(Info + 14);
        const uint32_t Bytes = Get<uint32_t>(Info + 0x18);
        const uint64_t Data = Get<uint64_t>(Info + 0x30);
        if (Format != 1 || Bytes == 0 || Data == 0 || Channels == 0 || Rate == 0 || Bits < 8) return false;
        Sound.AssetPointer = Data;
        Sound.AssetSize = Bytes;
        Sound.FrameRate = Rate;
        Sound.ChannelsCount = Channels;
        Sound.FrameCount = Bytes / (Channels * (Bits / 8));
        Sound.IsFileEntry = false;
        Sound.DataType = SoundDataTypes::WAV_NeedsHeader;
        Sound.Length = (uint32_t)(1000.0 * Sound.FrameCount / Rate);
        return true;
    }
}
