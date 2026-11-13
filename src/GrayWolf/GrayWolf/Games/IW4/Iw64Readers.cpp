#include "stdafx.h"

#include "Games/IW4/Iw64Readers.h"
#include "Games/IW4/Iw4Readers.h"
#include "Games/IW3/LoadDefImage.h"

#include "CoDAssets.h"
#include "Image.h"

#include <algorithm>
#include <cstring>
#include <vector>

// Every offset is a field the game's loader reads: profiles/iw4, iw5 (the rebuild), iw4x86 (the
// original) tables (xmodel, xmodelsurfs, the XSurface, xanim, material, image, loaded_sound, sound
// readers).
namespace Iw64
{
    namespace
    {
        // XModel 0x198 / 0x1a0: bone names u16 script strings, lodInfo[4] of 0x38 (the LOD's own
        // surfs pointer at +0x28 is the game's load-time copy of modelSurfs->surfs); XSurface 0x58
        // with a 32-byte GfxPackedVertex and a 16-byte XRigidVertList; MaterialTextureDef 0x10;
        // XAnimParts 0x88: numframes 0x12, flags 0x14 (bit 0: looping), counts 0x15, framerate
        // 0x2c, the pointers from 0x38, the delta part 0x80
        Iw4Family::Layout Shapes64()
        {
            Iw4Family::Layout L;
            L.PointerSize = 8;
            L.ModelBones = 0x8, L.ModelRootBones = 0x9, L.ModelBoneNames = 0x28, L.ModelParents = 0x30, L.ModelQuats = 0x38;
            L.ModelTrans = 0x40, L.ModelBaseMat = 0x50, L.ModelMaterials = 0x58, L.ModelLods = 0x60, L.LodSize = 0x38;
            L.ModelLodCount = 0x141, L.ModelSurfsSurfs = 0x8;
            L.SurfSize = 0x58, L.SurfVerts = 0x2, L.SurfTris = 0x4, L.SurfTriIndices = 0x10, L.SurfBlendCounts = 0x18;
            L.SurfBlend = 0x20, L.SurfVerts0 = 0x28, L.SurfVertListCount = 0x30, L.SurfVertList = 0x38;
            L.TextureDefSize = 0x10;
            L.AnimFrames = 0x12, L.AnimFlags = 0x14, L.LoopMask = 1, L.AnimCounts = 0x15, L.AnimRate = 0x2c;
            L.AnimPointers = 0x38, L.AnimDelta = 0x80;
            return L;
        }

        // snd_alias_t, OAT's IW4/IW5 headers with 8-byte pointers (the iw4/iw5 tables agree on its
        // strings, SoundFile 0x28 and curve): MW2 0x88 (volumes 0x34/0x38, pitches 0x3c/0x40,
        // distances 0x44/0x48, flags 0x50, probability 0x58, center 0x60, start delay 0x64, envelope
        // 0x70/0x74/0x78); MW3 0x98 (volModIndex after the volumes: +4; a master priority after the
        // flags: +0xc; the curve 0x78: +0x10). Flags: looping 0, type 7-8; 2D/3D is not a flag here
        // (the 6th, spatialize, is set on 2 of 279 variants of so_ied_berlin): left empty.
        IWSoundAliases::Layout Aliases64(bool Mw3)
        {
            IWSoundAliases::Layout L;
            const int64_t A = Mw3 ? 4 : 0, B = Mw3 ? 0xc : 0, E = Mw3 ? 0x10 : 0;
            L.ListCount = 0x10, L.ListCountBytes = 4, L.Size = Mw3 ? 0x98 : 0x88;
            L.Subtitle = 8, L.Secondary = 0x10, L.SoundFile = 0x28, L.VolMin = 0x34, L.VolMax = 0x38;
            L.PitchMin = 0x3c + A, L.PitchMax = 0x40 + A, L.DistMin = 0x44 + A, L.DistMax = 0x48 + A, L.Flags = 0x50 + A;
            L.Probability = 0x58 + B, L.CenterPercentage = 0x60 + B, L.StartDelay = 0x64 + B;
            L.EnvelopMin = 0x70 + E, L.EnvelopMax = 0x74 + E, L.EnvelopPercentage = 0x78 + E;
            return L;
        }

        // Map: gfx_map's table (draw at 0x78: vd and indices; models 60 bytes; dpvs), com_map's light
        // records (0x48 / 0x58), col_map_sp's dynamic entity lists (0x70 / 0x78)
        Layout Rebuild(bool Mw3)
        {
            Layout L = Mw3 ? Layout{ "Modern Warfare 3", 0x56, 0x68, 0x98,
                                     { 0x78 + 0x68, 0x78 + 0x70, 0x78 + 0x98, 0x78 + 0xa0, 0x178, 0x180, 0x2b8,
                                       0x58, 40, 52, 56, 0x188, 0x190, 0x78 } }
                           : Layout{ "Modern Warfare 2", 0x50, 0x60, 0x88,
                                     { 0x78 + 0x50, 0x78 + 0x58, 0x78 + 0x80, 0x78 + 0x88, 0x160, 0x168, 0x2a0,
                                       0x48, 28, 40, 44, 0x148, 0x150, 0x70 } };
            L.Aliases = Aliases64(Mw3);
            L.Shapes = Shapes64();
            L.XFileSize = Mw3 ? 0x2c : 0x28;
            return L;
        }

        // The original 32-bit MW2 (iw4x86 tables, iw4mp.exe): XModel 0x130 (bones u8 4, root bones
        // 5; u16 bone names 0x24, parents 0x28, quats 0x2c, trans 0x30, base matrices 0x38, material
        // handles 0x3c; lodInfo[4] of 0x2c at 0x40, numLods u8 0xf1; XModelSurfs' surfaces at 4);
        // XSurface 0x40 (triIndices 0xc, blend counts 0x10, blend data 0x18, vertices 0x1c, rigid
        // lists 0x20/0x24); MaterialTextureDef 0xc; XAnimParts 0x58 (Call of Duty Online's)
        Iw4Family::Layout ShapesMw2x86()
        {
            Iw4Family::Layout L;
            L.PointerSize = 4;
            L.ModelBones = 0x4, L.ModelRootBones = 0x5, L.ModelBoneNames = 0x24, L.ModelParents = 0x28, L.ModelQuats = 0x2c;
            L.ModelTrans = 0x30, L.ModelBaseMat = 0x38, L.ModelMaterials = 0x3c, L.ModelLods = 0x40, L.LodSize = 0x2c;
            L.ModelLodCount = 0xf1, L.ModelSurfsSurfs = 0x4;
            L.SurfSize = 0x40, L.SurfVerts = 0x2, L.SurfTris = 0x4, L.SurfTriIndices = 0xc, L.SurfBlendCounts = 0x10;
            L.SurfBlend = 0x18, L.SurfVerts0 = 0x1c, L.SurfVertListCount = 0x20, L.SurfVertList = 0x24;
            L.TextureDefSize = 0xc;
            L.AnimFrames = 0xe, L.AnimFlags = 0x10, L.AnimCounts = 0x11, L.AnimRate = 0x28, L.AnimPointers = 0x30, L.AnimDelta = 0x54;
            return L;
        }

        // Its level, images, sounds (the iw4x86 tables): docs/games/IW4.md, "The original 32-bit
        // build", lists each struct; every number below is one of those tables' offsets.
        Layout Mw2Original()
        {
            Layout L{ "Modern Warfare 2 (32-bit)", 0x48, 0x54, 0x64,
                      { 0x50 + 0x28, 0x50 + 0x2c, 0x50 + 0x40, 0x50 + 0x44, 0xd0, 0xd4, 0x1c8, 0x44, 28, 40, 44, 0xa8, 0xac, 0x5c } };
            auto& M = L.Map;
            M.WorldBaseName = 4, M.WorldSurfaceCount = 0x10, M.WorldSkyCount = 0x14, M.WorldSkies = 0x18;
            M.SkySize = 0x10, M.SkySurfs = 4, M.SkyImage = 8;
            M.DpvsSurfaces = 0x54, M.DpvsSModels = 0x5c, M.SurfaceSize = 0x18, M.SurfaceMaterial = 0x10, M.SModelSize = 0x4c, M.SModelModel = 0x34;
            M.LightCount = 8, M.Lights = 0xc, M.Triggers = 0xc;
            L.PointerSize = 4;
            L.Image = { 0x0, 0x4, 0x5, 0x14, 0x16, 0x1c };
            L.Loaded = { 0x4, 0x18, 0x10, 0x14, 0xc, 0x28 };
            auto& A = L.Aliases;
            A.ListCount = 8, A.ListCountBytes = 4, A.Size = 0x64;
            A.Subtitle = 4, A.Secondary = 8, A.SoundFile = 0x14, A.VolMin = 0x1c, A.VolMax = 0x20, A.PitchMin = 0x24, A.PitchMax = 0x28;
            A.DistMin = 0x2c, A.DistMax = 0x30, A.Flags = 0x38, A.Probability = 0x40, A.CenterPercentage = 0x48, A.StartDelay = 0x4c;
            A.EnvelopMin = 0x54, A.EnvelopMax = 0x58, A.EnvelopPercentage = 0x5c;
            L.Shapes = ShapesMw2x86();
            L.XFileSize = 0x28;
            L.Rebuild = false;
            return L;
        }

        // The original 32-bit MW3 (iw5x86 tables, iw5mp.exe): MW2's original shapes but XSurface
        // 0x44 (triIndices 0x10, blend counts 0x14, blend data 0x1c, vertices 0x20, rigid lists
        // 0x24/0x28); Material 0x68 (texture count 0x4e, table 0x58); snd_alias_t 0x70
        // (volModIndex after the volumes, a master priority and percentage after the flags, the
        // curve 0x5c); GfxWorld 0x280 (GfxWorldDraw 0x54 at 0x50, brush models 0xdc/0xe0, dpvs at
        // 0x1d4); ComPrimaryLight 0x50; clipMap_t's dynamic entities u16 0xc4, lists 0xc8, 0x60 each.
        Layout Mw3Original()
        {
            Layout L = Mw2Original();
            L.Title = "Modern Warfare 3 (32-bit)";
            L.MaterialTextureCount = 0x4e, L.MaterialTextureTable = 0x58, L.AliasSize = 0x70;
            auto& M = L.Map;
            M.VertexCount = 0x50 + 0x34, M.Vertices = 0x50 + 0x38, M.IndexCount = 0x50 + 0x4c, M.Indices = 0x50 + 0x50;
            M.BrushCount = 0xdc, M.Brushes = 0xe0, M.Dpvs = 0x1d4;
            M.LightSize = 0x50, M.LightOrigin = 40, M.LightRadius = 52, M.LightCosOuter = 56;
            M.DynCount = 0xc4, M.Dyns = 0xc8, M.DynSize = 0x60;
            auto& A = L.Aliases;
            A.Size = 0x70;
            A.PitchMin = 0x28, A.PitchMax = 0x2c, A.DistMin = 0x30, A.DistMax = 0x34, A.Flags = 0x3c;
            A.Probability = 0x4c, A.CenterPercentage = 0x54, A.StartDelay = 0x58;
            A.EnvelopMin = 0x60, A.EnvelopMax = 0x64, A.EnvelopPercentage = 0x68;
            auto& S = L.Shapes;
            S.SurfSize = 0x44, S.SurfTriIndices = 0x10, S.SurfBlendCounts = 0x14, S.SurfBlend = 0x1c, S.SurfVerts0 = 0x20;
            S.SurfVertListCount = 0x24, S.SurfVertList = 0x28;
            L.XFileSize = 0x2c;
            return L;
        }
    }

    const Layout Mw2 = Rebuild(false), Mw3 = Rebuild(true), Mw2x86 = Mw2Original(), Mw3x86 = Mw3Original();

    thread_local std::function<uint64_t(const std::string& Name)> MaterialDefinedAt;
    thread_local std::function<bool(const std::string& Name, std::vector<std::pair<uint32_t, std::string>>& Images)> MaterialImagesElsewhere;

    namespace
    {
        template <class T>
        T Get(uint64_t Address) { return CoDAssets::GameInstance->Read<T>(Address); }

        uint64_t Pointer(uint64_t Address, const Layout& L)
        {
            return L.PointerSize == 4 ? (uint64_t)Get<uint32_t>(Address) : Get<uint64_t>(Address);
        }

        // the build's shapes, with its material fields and the served zone's material lookups
        Iw4Family::Layout Readers(const Layout& G)
        {
            Iw4Family::Layout L = G.Shapes;
            L.MaterialName = 0, L.MaterialTextureCount = G.MaterialTextureCount, L.MaterialTextureTable = G.MaterialTextureTable;
            L.ImageName = G.Image.Name;
            L.MaterialByName = MaterialDefinedAt, L.MaterialImagesByName = MaterialImagesElsewhere;
            return L;
        }
    }

    std::unique_ptr<XModel_t> ReadXModel(const CoDModel_t* Model, const Layout& L) { return Iw4Family::ReadXModel(Model, Readers(L)); }
    std::unique_ptr<XAnim_t> ReadXAnim(const CoDAnim_t* Animation, const Layout& L) { return Iw4Family::ReadXAnim(Animation, Readers(L)); }
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

    std::unique_ptr<XImageDDS> ZoneImage(uint64_t ImagePointer, const Layout& L)
    {
        const uint64_t Def = Pointer(ImagePointer + L.Image.LoadDef, L);
        if (Def == 0) return nullptr;
        const uint32_t Size = Get<uint32_t>(Def + LoadDefSize);
        if (Size == 0) return nullptr;
        const uint32_t W = Get<uint16_t>(ImagePointer + L.Image.Width), H = Get<uint16_t>(ImagePointer + L.Image.Height);
        if (W == 0 || H == 0) return nullptr;
        // the D3D9 formats GfxImageLoadDef carries (FourCC or D3DFMT); the largest level only: it
        // comes first (of each face, for a cube map)
        ImageFormat Format;
        uint64_t Level = 0;
        LoadDefImage::Convert How = LoadDefImage::Convert::None;
        if (!LoadDefImage::D3dFormat(Get<uint32_t>(Def + LoadDefFormat), W, H, Format, Level, How)) return nullptr;
        const bool Cube = Get<uint8_t>(ImagePointer + L.Image.MapType) == 5;   // MAPTYPE_CUBE
        const uint64_t Faces = Cube ? 6 : 1;
        if (Level * Faces > Size) return nullptr;
        const uint64_t Chain = Cube ? Size / 6 : 0;
        std::vector<uint8_t> Pixels(Level * Faces);
        for (uint64_t F = 0; F < Faces; F++)
            if (!Copy(Pixels.data() + F * Level, Def + LoadDefData + F * Chain, Level)) return nullptr;
        return LoadDefImage::Dds(W, H, Format, Pixels.data(), Level, How, Faces, Level);
    }

    bool LoadedSound(uint64_t P, CoDSound_t& Sound, const Layout& L)
    {
        const uint16_t Format = Get<uint16_t>(P + L.Loaded.Format);
        const uint16_t Channels = Get<uint16_t>(P + L.Loaded.Channels);
        const uint32_t Rate = Get<uint32_t>(P + L.Loaded.Rate);
        const uint16_t Bits = Get<uint16_t>(P + L.Loaded.Bits);
        const uint32_t Bytes = Get<uint32_t>(P + L.Loaded.Bytes);
        const uint64_t Data = Pointer(P + L.Loaded.Data, L);
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
