#include "stdafx.h"

#include "Bo3ZoneImages.h"

#include "DiskGameSizes.h"
#include "ExportNaming.h"
#include "ExportWriters.h"
#include "FastFile.h"
#include "XPAKCache.h"
#include "CoDRawImageTranslator.h"
#include "Image.h"
#include "SettingsManager.h"

#include <filesystem>
#include <map>
#include <sstream>
#include <unordered_map>
#include <vector>

namespace
{
    namespace fs = std::filesystem;

    // GfxImage, as a T7 fastfile writes it.
    constexpr size_t MipStride = 40;      // size(4) + width(2) + height(2) + hash(8) + pad(24)
    constexpr size_t MipCount = 4;
    constexpr size_t LoadedLevelsOffset = 191;  // how many mip levels the inline payload holds
    constexpr size_t LoadedWidthOffset = 192;   // width/height of the inline payload's top mip
    constexpr size_t PixelsPtrOffset = 216;     // the inline payload's pointer: -1 inline, 0 none
    constexpr size_t LoadedSizeOffset = 232;    // its total byte count, across every level
    constexpr size_t FormatOffset = 240;  // the engine's image-format byte
    // The semantic byte: 4 on every image the materials use as their normal map (X in red, Y in
    // green, blue not its Z: 0-60), 2 colour, 5 specular, 7 gloss, 8 occlusion... Measured over
    // mp_freerun_01 (2026-11-12): its 18 are all BC7 UNORM, but the format alone does not tell,
    // BC7 UNORM holds $camo_base_mask (semantic 1) too.
    constexpr size_t SemanticOffset = 161;
    constexpr uint8_t NormalMapSemantic = 4;
    constexpr size_t NamePtrOffset = 248; // where the 0xFF.. marker sits inside the struct
    constexpr size_t NameOffset = 264;    // the string itself, right after the struct

    struct Mip
    {
        uint32_t Size;
        uint16_t Width;
        uint16_t Height;
        uint64_t Hash;
    };

    Mip ReadMip(const uint8_t* At)
    {
        Mip M{};
        std::memcpy(&M.Size, At + 0, 4);
        std::memcpy(&M.Width, At + 4, 2);
        std::memcpy(&M.Height, At + 6, 2);
        std::memcpy(&M.Hash, At + 8, 8);
        return M;
    }

    // A name has to look like one: printable, NUL-terminated, and not absurdly long. Without this
    // the 0xFF marker alone matches every null pointer in the zone, which is most of them.
    bool ReadName(ByteView Zone, size_t At, std::string& Out)
    {
        Out.clear();
        for (size_t i = At; i < Zone.size() && i < At + 160; i++)
        {
            const uint8_t C = Zone[i];
            if (C == 0)
                return Out.size() >= 2;
            if (C < 0x20 || C > 0x7E)
                return false;
            Out.push_back((char)C);
        }
        return false;
    }

    using ExportNaming::SafeFileName;
}

bool Bo3DescribeZoneImage(ByteView Zone, size_t Start, XPAKCache& Cache, Bo3ZoneImage& Image,
                          const std::string& ZoneStem)
{
    const uint8_t* Base = Zone.data();
    const size_t End = Zone.size();
    if (Start + NameOffset >= End)
        return false;
    std::string Name;
    if (!ReadName(Zone, Start + NameOffset, Name))
        return false;

    const size_t AfterName = Start + NameOffset + Name.size() + 1;

    // The biggest mip the packages actually carry. The zone declares four and an install
    // that never shipped the high-resolution pack has only the small ones.
    Image = Bo3ZoneImage{};
    for (size_t m = 0; m < MipCount; m++)
    {
        const Mip M = ReadMip(Base + Start + m * MipStride);
        if (M.Hash == 0 || M.Size == 0 || !Cache.Exists(M.Hash))
            continue;
        if (M.Size <= Image.Size)
            continue;
        Image.Hash = M.Hash;
        Image.Width = M.Width;
        Image.Height = M.Height;
        Image.Size = M.Size;
    }

    if (Image.Hash == 0)
    {
        // Nothing streamed - fall back to the inline-pixels case.
        uint64_t Loaded = 0;
        std::memcpy(&Loaded, Base + Start + LoadedSizeOffset, 8);
        uint16_t Lw = 0, Lh = 0;
        std::memcpy(&Lw, Base + Start + LoadedWidthOffset, 2);
        std::memcpy(&Lh, Base + Start + LoadedWidthOffset + 2, 2);

        if (Loaded == 0 || Loaded > 64ull * 1024ull * 1024ull || Lw == 0 || Lh == 0
            || Lw > 8192 || Lh > 8192)
            return false;

        Image.Width = Lw;
        Image.Height = Lh;
        Image.Size = (uint32_t)Loaded;
        Image.Levels = Base[Start + LoadedLevelsOffset];
        if (Image.Levels == 0)
            Image.Levels = 1;

        // Inline only when the pixel pointer says so (-1, the stream follows). A map's baked
        // images (its sky, probes) have a null one and no mip keys: the packages' index names
        // them. Reading after the name there took the next assets' bytes (zm_factory's sky).
        uint64_t Pixels = 0;
        std::memcpy(&Pixels, Base + Start + PixelsPtrOffset, 8);
        XPAKCache::NamedObject Named;
        if (Pixels == ~0ull && AfterName + Loaded <= End)
            Image.PixelOffset = AfterName;
        else if (Pixels == 0 && !ZoneStem.empty() && Cache.FindNamed(Name, ZoneStem, Named)
                 && Named.Parts == 1 && Named.Size == Loaded)
            Image.Hash = Named.Key;   // one 2D image, its whole chain in one part
        else
            return false;
    }

    Image.Format = Base[Start + FormatOffset];
    Image.Semantic = Base[Start + SemanticOffset];
    Image.Name = Name;
    return true;
}

namespace
{
    // The index's format names as the zones' image records give the same images (the DXGI format):
    // measured over core_ui, zm_genesis and mp_freerun_01's images the index also names
    uint8_t DxgiOfIndexFormat(const std::string& Format)
    {
        static const std::unordered_map<std::string, uint8_t> Formats = {
            { "BC1", 71 }, { "BC1_SRGB", 72 }, { "BC2", 74 }, { "BC2_SRGB", 75 }, { "BC3", 77 }, { "BC3_SRGB", 78 },
            { "BC4", 80 }, { "BC4_SNORM", 81 }, { "BC5", 83 }, { "BC5_SNORM", 84 }, { "BC6_UH", 95 }, { "BC6_SH", 96 },
            { "BC7", 98 }, { "BC7_SRGB", 99 }, { "R8G8B8A8", 28 }, { "R8G8B8A8_SRGB", 29 }, { "R8_UN", 61 },
            { "R16G16B16A16F", 10 },
        };
        const auto It = Formats.find(Format);
        return It == Formats.end() ? 0 : It->second;
    }
}

std::vector<Bo3ZoneImage> Bo3DescribePackageImages(XPAKCache& Cache, const std::string& Package, size_t& Unread)
{
    // per name, its largest record (a name has one per streamed mip, the smallest ones chained)
    std::map<std::string, XPAKCache::NamedObject> Largest;
    for (auto& [Name, Object] : Cache.NamedObjects(Package))
    {
        if (Object.Type != "image" || Object.Parts != 1) continue;
        auto& L = Largest[Name];
        if (Object.Size > L.Size) L = std::move(Object);
    }
    std::vector<Bo3ZoneImage> Out;
    for (const auto& [Name, Object] : Largest)
    {
        Bo3ZoneImage Image;
        Image.Name = Name;
        Image.Hash = Object.Key;
        Image.Width = Object.Height;
        Image.Height = Object.Width;
        Image.Format = DxgiOfIndexFormat(Object.Format);
        Image.Semantic = Object.Semantic == "normalMap" ? NormalMapSemantic : 0;
        // the levels whose chain is its size (its "levels" says fewer than some hold)
        Image.Levels = 0;
        for (uint8_t L = 1; Image.Format != 0 && L <= 16 && Image.Levels == 0; L++)
            if (DiskGame::Sizes::ImageWeight(Image.Width, Image.Height, Image.Format, L, 1).Bytes == Object.Size) Image.Levels = L;
        if (Image.Levels == 0 || Image.Width == 0 || Image.Height == 0 || Object.Size > 0xFFFFFFFFull)
        {
            Unread++;
            continue;
        }
        Image.Size = (uint32_t)Object.Size;
        Out.push_back(std::move(Image));
    }
    return Out;
}

std::vector<Bo3ZoneImage> Bo3ScanZoneImages(const std::string& FastFilePath,
                                            const std::string& XpakDir,
                                            std::string& FailureReason,
                                            ZoneBytes* ZoneOut)
{
    std::vector<Bo3ZoneImage> Out;

    ZoneBytes Local;
    ZoneBytes& Zone = ZoneOut ? *ZoneOut : Local;
    if (!FastFile::Decompress(FastFilePath, Zone, FailureReason))
        return Out;
    if (Zone.size() < NameOffset + 16)
    {
        FailureReason = "zone too small";
        return Out;
    }

    XPAKCache Cache;
    const std::string Dir = XpakDir.empty()
        ? fs::path(FastFilePath).parent_path().string()
        : XpakDir;
    Cache.LoadPackagesIn(Dir);

    // The cache is the oracle. A mip hash that no package holds is not a mip hash, which is what
    // makes scanning for a marker byte pattern safe rather than a guess.
    const uint8_t* Base = Zone.data();
    const size_t End = Zone.size();

    // Byte by byte, not every four: records are unaligned; the marker's first-byte test keeps
    // this cheap.
    for (size_t At = NamePtrOffset; At + 16 < End; At++)
    {
        if (Base[At] != 0xFF)
            continue;

        // The name pointer, as a fastfile writes an unresolved pointer.
        uint64_t Marker = 0;
        std::memcpy(&Marker, Base + At, 8);
        if (Marker != 0xFFFFFFFFFFFFFFFFull)
            continue;

        const size_t Start = At - NamePtrOffset;
        if (Start + NameOffset >= End)
            continue;

        Bo3ZoneImage Image;
        if (!Bo3DescribeZoneImage(Zone, Start, Cache, Image, fs::path(FastFilePath).stem().string()))
            continue;
        Out.push_back(Image);
    }

    return Out;
}

bool Bo3ListZoneImages(const std::string& FastFilePath, const std::string& XpakDir,
                       std::string& Report)
{
    std::string Failure;
    const auto Images = Bo3ScanZoneImages(FastFilePath, XpakDir, Failure);

    std::ostringstream Os;
    if (Images.empty())
    {
        Os << "result=error\nreason=" << (Failure.empty() ? "no images found" : Failure) << "\n";
        Report = Os.str();
        return false;
    }

    Os << "result=ok\nimages_in_zone=" << Images.size() << "\n--- content ---\n";
    for (const auto& Image : Images)
        Os << Image.Name << "\t" << Image.Width << "x" << Image.Height
           << "\tformat " << (unsigned)Image.Format << "\tsize " << Image.Size
           << (Image.PixelOffset ? "\tinline" : "\tstreamed") << "\n";
    Report = Os.str();
    return true;
}

size_t Bo3ExportZoneImages(const std::string& FastFilePath, const std::string& XpakDir,
                           const std::string& OutDir, const std::string& NameFilter,
                           std::string& Report)
{
    std::string Failure;
    ZoneBytes Zone;
    const auto Images = Bo3ScanZoneImages(FastFilePath, XpakDir, Failure, &Zone);

    std::ostringstream Os;
    if (Images.empty())
    {
        Os << "result=error\nreason=" << (Failure.empty() ? "no images found" : Failure) << "\n";
        Report = Os.str();
        return 0;
    }

    XPAKCache Cache;
    Cache.LoadPackagesIn(XpakDir.empty()
        ? fs::path(FastFilePath).parent_path().string()
        : XpakDir);
    return Bo3WriteZoneImages(Zone, Images, Cache, OutDir, NameFilter, Report);
}

size_t Bo3WriteZoneImages(ByteView Zone, const std::vector<Bo3ZoneImage>& Images,
                          XPAKCache& Cache, const std::string& OutDir, const std::string& NameFilter,
                          std::string& Report, const std::function<bool(size_t, size_t)>& Step)
{
    std::ostringstream Os;
    std::error_code Ec;
    fs::create_directories(OutDir, Ec);

    // Comma-separated, matched as "any of". A zone is decompressed once per call, so asking for
    // one substring at a time means unpacking 271 MB again for every family you want.
    const ExportNaming::NameFilter Filter(NameFilter);

    size_t Matched = 0, Written = 0, NoPayload = 0, NoDds = 0, NoWrite = 0, NormalsExpanded = 0;
    // the chosen format (exportimg), converted on the shared sink's pool
    ExportWriters::ImageSink Sink;

    for (size_t N = 0; N < Images.size(); N++)
    {
        if (Step && !Step(N, Images.size())) break;
        const auto& Image = Images[N];
        if (!Filter.Matches(Image.Name)) continue;
        Matched++;

        uint32_t ResultSize = 0;
        std::unique_ptr<uint8_t[]> Inline;
        if (Image.PixelOffset != 0)
        {
            Inline = std::make_unique<uint8_t[]>(Image.Size);
            std::memcpy(Inline.get(), Zone.data() + Image.PixelOffset, Image.Size);
            ResultSize = Image.Size;
        }

        // Size must come from the zone, not -1 - the XPAK index only stores the compressed size.
        auto Data = Image.PixelOffset != 0
            ? std::move(Inline)
            : Cache.ExtractPackageObject(Image.Hash, (int32_t)Image.Size, ResultSize);
        if (!Data || ResultSize == 0)
        {
            NoPayload++;
            continue;
        }

        // Raw block-compressed pixels, not an IWi - DDS header synthesized.
        auto Dds = CoDRawImageTranslator::TranslateBC(Data, ResultSize, Image.Width, Image.Height,
                                                      Image.Format, Image.Levels);
        if (!Dds || Dds->DataBuffer == nullptr || Dds->DataSize == 0)
        {
            NoDds++;
            continue;
        }
        // a normal map's Z rebuilt from its X and Y when "patchnormals" is on
        if (Image.Semantic == NormalMapSemantic && SettingsManager::GetSetting("patchnormals", "true") == "true")
        {
            Dds->ImagePatchType = ImagePatch::Normal_Expand;
            NormalsExpanded++;
        }

        Sink.Write(std::move(Dds), (fs::path(OutDir) / (SafeFileName(Image.Name) + Sink.Extension())).string());
    }
    size_t Missing = 0;
    const auto Done = Sink.Finish(nullptr, Missing);
    Written = Done.Written;
    NoWrite = Done.Failed;

    Os << "result=ok\n";
    Os << "images_in_zone=" << Images.size() << "\n";
    Os << "matched=" << Matched << "\n";
    Os << "written=" << Written << "\n";
    Os << "normals_expanded=" << NormalsExpanded << "\n";
    Os << "no_payload=" << NoPayload << "\nno_dds=" << NoDds << "\nno_write=" << NoWrite << "\n";
    Os << "out=" << OutDir << "\n";
    Report = Os.str();
    return Written;
}
