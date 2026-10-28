#include "stdafx.h"
#include "ByteView.h"

#include "AssetPreview.h"

#include <miniz/miniz.h>
#include "CoDIWITranslator.h"
#include "CoDAssetType.h"   // XImageDDS
#include "Image.h"
#include "Strings.h"
#include "DirectXTex.h"
#include "IWDSoundDecoder.h"
#include "Sound.h"
#include "IPAKCache.h"
#include "AssetIndex.h"

#include <filesystem>
#include <fstream>
#include <functional>
#include <memory>
#include <unordered_set>
#include <cstring>
#include <cmath>

// Pulls one entry out of an IWD into a heap buffer.
static std::unique_ptr<uint8_t[]> ExtractEntry(const std::string& IwdPath,
                                               const std::string& EntryName, uint32_t& OutSize)
{
    OutSize = 0;

    mz_zip_archive Zip;
    std::memset(&Zip, 0, sizeof(Zip));
    if (!mz_zip_reader_init_file(&Zip, IwdPath.c_str(), 0))
        return nullptr;

    std::unique_ptr<uint8_t[]> Buffer;
    const int Index = mz_zip_reader_locate_file(&Zip, EntryName.c_str(), nullptr, 0);
    if (Index >= 0)
    {
        mz_zip_archive_file_stat Info;
        if (mz_zip_reader_file_stat(&Zip, (mz_uint)Index, &Info) && Info.m_uncomp_size > 0)
        {
            Buffer = std::make_unique<uint8_t[]>((size_t)Info.m_uncomp_size);
            if (mz_zip_reader_extract_to_mem(&Zip, (mz_uint)Index, Buffer.get(), (size_t)Info.m_uncomp_size, 0))
                OutSize = (uint32_t)Info.m_uncomp_size;
            else
                Buffer.reset();
        }
    }

    mz_zip_reader_end(&Zip);
    return Buffer;
}

// Walks every .iwd under InstallDir, calling Fn(iwdPath) until Fn returns true (early-out).
static void ForEachIwd(const std::string& InstallDir, const std::function<bool(const std::string&)>& Fn)
{
    namespace fs = std::filesystem;
    std::error_code Ec;
    for (auto It = fs::recursive_directory_iterator(InstallDir, Ec);
         It != fs::recursive_directory_iterator(); It.increment(Ec))
    {
        if (Ec) break;
        if (!It->is_regular_file(Ec)) continue;
        auto Ext = It->path().extension().string();
        for (auto& C : Ext) C = (char)tolower((unsigned char)C);
        if (Ext == ".iwd" && Fn(It->path().string())) return;
    }
}

// Finds + extracts one entry by name from the first .iwd under InstallDir that carries it.
static std::unique_ptr<uint8_t[]> FindIwdEntry(const std::string& InstallDir, const std::string& EntryName, uint32_t& OutSize)
{
    std::unique_ptr<uint8_t[]> Result;
    uint32_t Size = 0;
    ForEachIwd(InstallDir, [&](const std::string& Iwd) {
        auto Data = ExtractEntry(Iwd, EntryName, Size);
        if (Data && Size > 0) { Result = std::move(Data); return true; }
        return false;
    });
    OutSize = Result ? Size : 0;
    return Result;
}

// Turns raw IWD sound bytes (BO1 block MS-ADPCM, or an already-RIFF entry) into a PCM16 WAV on disk.
// Robust: a bad output path (odd chars / over-long) skips instead of throwing. False if undecodable.
static bool WriteIwdSound(ByteView Raw, const std::string& OutPath)
{
    std::vector<uint8_t> Wav;
    const bool IsRiff = Raw.size() >= 12 && std::memcmp(Raw.data(), "RIFF", 4) == 0
                                         && std::memcmp(Raw.data() + 8, "WAVE", 4) == 0;
    ByteView Payload;
    if (DecodeIWDSound(Raw, Wav)) Payload = Wav;
    else if (IsRiff)              Payload = Raw;
    else                          return false;
    try
    {
        namespace fs = std::filesystem;
        std::error_code Ec;
        fs::create_directories(fs::path(OutPath).parent_path(), Ec);
        std::ofstream O(OutPath, std::ios::binary | std::ios::trunc);
        if (!O) return false;
        O.write((const char*)Payload.data(), (std::streamsize)Payload.size());
        return (bool)O;
    }
    catch (const std::exception&) { return false; }
}

// Locates an image's IWD and decodes its IWi to an in-memory DDS. Nullptr on failure.
static std::unique_ptr<XImageDDS> DecodeEntry(const AssetEntry& Entry, const std::string& InstallDir)
{
    if (Entry.Kind != AssetKind::Image)
        return nullptr;

    namespace fs = std::filesystem;
    std::error_code Ec;

    fs::path IwdPath = fs::path(InstallDir) / "main" / Entry.Source;
    if (!fs::exists(IwdPath, Ec))
    {
        for (auto It = fs::recursive_directory_iterator(InstallDir, Ec);
             It != fs::recursive_directory_iterator(); It.increment(Ec))
        {
            if (Ec) break;
            if (It->is_regular_file(Ec) && It->path().filename() == Entry.Source)
            {
                IwdPath = It->path();
                break;
            }
        }
    }

    uint32_t Size = 0;
    auto Iwi = ExtractEntry(IwdPath.string(), Entry.Name + ".iwi", Size);
    if (!Iwi || Size == 0)
        return nullptr;

    return CoDIWITranslator::TranslateIWI(Iwi, Size);
}

bool IsNormalMapName(const std::string& Name)
{
    // CoD colour/normal/spec maps end in _c / _n / _s; treat _n, _nml, _normal as normal maps.
    auto Ends = [&](const char* Suffix) {
        const size_t L = std::strlen(Suffix);
        return Name.size() >= L && Name.compare(Name.size() - L, L, Suffix) == 0;
    };
    return Ends("_n") || Ends("_nml") || Ends("_normal") || Name.find("_nml") != std::string::npos;
}

bool FlipNormalMapGreen(const std::string& PngPath)
{
    DirectX::TexMetadata Meta;
    DirectX::ScratchImage Loaded;
    if (FAILED(DirectX::LoadFromWICFile(Strings::ToUnicodeString(PngPath).c_str(),
                                        DirectX::WIC_FLAGS_NONE, &Meta, Loaded)))
        return false;

    // Work on straight RGBA8.
    DirectX::ScratchImage Rgba;
    DirectX::ScratchImage* Target = &Loaded;
    if (Meta.format != DXGI_FORMAT_R8G8B8A8_UNORM)
    {
        if (FAILED(DirectX::Convert(Loaded.GetImages(), Loaded.GetImageCount(), Meta,
                                    DXGI_FORMAT_R8G8B8A8_UNORM, DirectX::TEX_FILTER_DEFAULT,
                                    DirectX::TEX_THRESHOLD_DEFAULT, Rgba)))
            return false;
        Target = &Rgba;
    }

    const DirectX::Image* Img = Target->GetImage(0, 0, 0);
    for (size_t y = 0; y < Img->height; y++)
    {
        uint8_t* Row = Img->pixels + y * Img->rowPitch;
        for (size_t x = 0; x < Img->width; x++)
            Row[x * 4 + 1] = (uint8_t)(255 - Row[x * 4 + 1]); // invert green
    }

    return SUCCEEDED(DirectX::SaveToWICFile(*Img, DirectX::WIC_FLAGS_NONE,
                                            DirectX::GetWICCodec(DirectX::WIC_CODEC_PNG),
                                            Strings::ToUnicodeString(PngPath).c_str()));
}

bool RenderImageToPNG(const AssetEntry& Entry, const std::string& InstallDir, const std::string& OutPngPath)
{
    auto Dds = DecodeEntry(Entry, InstallDir);
    if (!Dds || Dds->DataBuffer == nullptr || Dds->DataSize == 0)
        return false;

    std::error_code Ec;
    std::filesystem::create_directories(std::filesystem::path(OutPngPath).parent_path(), Ec);

    return Image::ConvertImageMemory(Dds->DataBuffer, Dds->DataSize, ImageFormat::DDS_WithHeader,
                                     OutPngPath, ImageFormat::Standard_PNG, Dds->ImagePatchType);
}

size_t ExtractIpakImagesByKey(const std::string& IpakDir,
                              const std::vector<std::pair<uint64_t, std::string>>& Jobs)
{
    namespace fs = std::filesystem;
    std::error_code Ec;
    if (!fs::is_directory(IpakDir, Ec) || Jobs.empty())
        return 0;

    IPAKCache Cache;
    for (auto& E : fs::directory_iterator(IpakDir, Ec))
    {
        if (Ec) break;
        auto Ext = E.path().extension().string();
        for (auto& C : Ext) C = (char)tolower((unsigned char)C);
        if (Ext == ".ipak")
            Cache.LoadPackage(E.path().string());
    }

    size_t Written = 0;
    for (const auto& Job : Jobs)
    {
        uint32_t Size = 0;
        auto Data = Cache.ExtractPackageObject(Job.first, Size);
        if (!Data || Size == 0)
            continue;
        auto Dds = CoDIWITranslator::TranslateIWI(Data, Size);
        if (!Dds || Dds->DataBuffer == nullptr || Dds->DataSize == 0)
            continue;
        fs::create_directories(fs::path(Job.second).parent_path(), Ec);
        if (Image::ConvertImageMemory(Dds->DataBuffer, Dds->DataSize, ImageFormat::DDS_WithHeader,
                                      Job.second, ImageFormat::Standard_PNG, Dds->ImagePatchType))
            Written++;
    }
    return Written;
}

bool ExtractIwiToPng(const std::string& InstallDir, const std::string& ImageName, const std::string& OutPng)
{
    if (ImageName.empty() || ImageName[0] == '$') // "$white", "$identitynormalmap" - engine defaults
        return false;

    namespace fs = std::filesystem;
    std::error_code Ec;

    uint32_t Size = 0;
    auto Iwi = FindIwdEntry(InstallDir, "images/" + ImageName + ".iwi", Size);
    if (!Iwi || Size == 0)
        return false;

    auto Dds = CoDIWITranslator::TranslateIWI(Iwi, Size);
    if (!Dds || Dds->DataBuffer == nullptr || Dds->DataSize == 0)
        return false;

    fs::create_directories(fs::path(OutPng).parent_path(), Ec);
    return Image::ConvertImageMemory(Dds->DataBuffer, Dds->DataSize, ImageFormat::DDS_WithHeader,
                                     OutPng, ImageFormat::Standard_PNG, Dds->ImagePatchType);
}

bool RenderAudioToFile(const AssetEntry& Entry, const std::string& InstallDir, const std::string& OutPath, bool AsOpus)
{
    if (Entry.Kind != AssetKind::Audio)
        return false;

    namespace fs = std::filesystem;
    std::error_code Ec;

    fs::path IwdPath = fs::path(InstallDir) / "main" / Entry.Source;
    if (!fs::exists(IwdPath, Ec))
    {
        for (auto It = fs::recursive_directory_iterator(InstallDir, Ec);
             It != fs::recursive_directory_iterator(); It.increment(Ec))
        {
            if (Ec) break;
            if (It->is_regular_file(Ec) && It->path().filename() == Entry.Source)
            {
                IwdPath = It->path();
                break;
            }
        }
    }

    uint32_t Size = 0;
    auto Raw = ExtractEntry(IwdPath.string(), Entry.Name + ".wav", Size);
    if (!Raw || Size == 0)
        return false;

    std::vector<uint8_t> RawVec(Raw.get(), Raw.get() + Size);
    std::vector<uint8_t> Wav;
    if (!DecodeIWDSound(RawVec, Wav))
        return false;

    fs::create_directories(fs::path(OutPath).parent_path(), Ec);

    if (AsOpus)
    {
        // Reuse the project's encoder: feed the decoded WAV (with header) straight to Opus/Ogg.
        return Sound::ConvertSoundMemory((int8_t*)Wav.data(), (uint64_t)Wav.size(),
                                         SoundFormat::WAV_WithHeader, OutPath, SoundFormat::Standard_OPUS);
    }

    std::ofstream Out(OutPath, std::ios::binary | std::ios::trunc);
    if (!Out)
        return false;
    Out.write((const char*)Wav.data(), (std::streamsize)Wav.size());
    return (bool)Out;
}

bool DecodeIwdSoundByName(const std::string& InstallDir, const std::string& EntryName, const std::string& OutWavPath)
{
    uint32_t Size = 0;
    auto Raw = FindIwdEntry(InstallDir, EntryName, Size);
    if (!Raw || Size == 0)
        return false;
    return WriteIwdSound(std::vector<uint8_t>(Raw.get(), Raw.get() + Size), OutWavPath);
}

size_t ExtractIwdSounds(const std::string& InstallDir, const std::string& OutDir)
{
    namespace fs = std::filesystem;
    size_t Written = 0;
    std::unordered_set<std::string> Seen; // dedup identical entry names across packages

    ForEachIwd(InstallDir, [&](const std::string& IwdPath) {
        mz_zip_archive Zip;
        std::memset(&Zip, 0, sizeof(Zip));
        if (!mz_zip_reader_init_file(&Zip, IwdPath.c_str(), 0))
            return false;

        const mz_uint Count = (mz_uint)mz_zip_reader_get_num_files(&Zip);
        for (mz_uint i = 0; i < Count; i++)
        {
            mz_zip_archive_file_stat Info;
            if (!mz_zip_reader_file_stat(&Zip, i, &Info) || Info.m_uncomp_size == 0)
                continue;

            std::string Name = Info.m_filename;
            std::string Lower = Name;
            for (auto& C : Lower) C = (char)tolower((unsigned char)C);
            if (Lower.size() < 4 || Lower.compare(Lower.size() - 4, 4, ".wav") != 0)
                continue;
            if (!Seen.insert(Lower).second)
                continue;

            std::vector<uint8_t> Raw((size_t)Info.m_uncomp_size);
            if (!mz_zip_reader_extract_to_mem(&Zip, i, Raw.data(), Raw.size(), 0))
                continue;

            // WriteIwdSound handles the MS-ADPCM decode / RIFF passthrough and the per-entry
            // filesystem guard; preserve the sound/... subpath under OutDir.
            if (WriteIwdSound(Raw, (fs::path(OutDir) / Name).string()))
                Written++;
        }

        mz_zip_reader_end(&Zip);
        return false; // sweep every package
    });

    return Written;
}

// Decodes an entry to a square RGBA thumbnail ScratchImage. False on failure.
static bool DecodeThumb(const AssetEntry& Entry, const std::string& InstallDir, int ThumbSize,
                        DirectX::ScratchImage& Out)
{
    auto Dds = DecodeEntry(Entry, InstallDir);
    if (!Dds || Dds->DataBuffer == nullptr || Dds->DataSize == 0)
        return false;

    DirectX::TexMetadata Meta;
    DirectX::ScratchImage Loaded;
    if (FAILED(DirectX::LoadFromDDSMemory(reinterpret_cast<const uint8_t*>(Dds->DataBuffer), (size_t)Dds->DataSize,
                                          DirectX::DDS_FLAGS_NONE, &Meta, Loaded)))
        return false;

    // Decompress BCn / convert to straight RGBA8; an sRGB source keeps its encoded values (Image.cpp).
    const bool Srgb = DirectX::IsSRGB(Meta.format);
    const DXGI_FORMAT Rgba8 = Srgb ? DXGI_FORMAT_R8G8B8A8_UNORM_SRGB : DXGI_FORMAT_R8G8B8A8_UNORM;
    DirectX::ScratchImage Rgba;
    if (DirectX::IsCompressed(Meta.format))
    {
        if (FAILED(DirectX::Decompress(Loaded.GetImages(), Loaded.GetImageCount(), Meta, Rgba8, Rgba)))
            return false;
    }
    else if (Meta.format != Rgba8)
    {
        if (FAILED(DirectX::Convert(Loaded.GetImages(), Loaded.GetImageCount(), Meta,
                                    Rgba8, DirectX::TEX_FILTER_DEFAULT,
                                    DirectX::TEX_THRESHOLD_DEFAULT, Rgba)))
            return false;
    }
    else
    {
        Rgba = std::move(Loaded);
    }
    if (Srgb) Rgba.OverrideFormat(DXGI_FORMAT_R8G8B8A8_UNORM);

    // Square resize to the thumbnail size (top mip only).
    return SUCCEEDED(DirectX::Resize(*Rgba.GetImage(0, 0, 0), (size_t)ThumbSize, (size_t)ThumbSize,
                                     DirectX::TEX_FILTER_DEFAULT, Out));
}

bool BuildContactSheet(const std::vector<const AssetEntry*>& Images, const std::string& InstallDir,
                       const std::string& OutPngPath, int ThumbSize, int MaxImages)
{
    if (Images.empty() || ThumbSize <= 0)
        return false;

    const int Count = (int)std::min<size_t>(Images.size(), (size_t)MaxImages);
    const int Pad = 4;
    const int Cols = std::min(Count, (int)std::ceil(std::sqrt((double)Count) * 1.3));
    const int UseCols = Cols < 1 ? 1 : Cols;
    const int Rows = (Count + UseCols - 1) / UseCols;

    const size_t SheetW = (size_t)(UseCols * (ThumbSize + Pad) + Pad);
    const size_t SheetH = (size_t)(Rows * (ThumbSize + Pad) + Pad);

    DirectX::ScratchImage Sheet;
    if (FAILED(Sheet.Initialize2D(DXGI_FORMAT_R8G8B8A8_UNORM, SheetW, SheetH, 1, 1)))
        return false;

    const DirectX::Image* Canvas = Sheet.GetImage(0, 0, 0);
    // Dark, fully-opaque background so transparent icons read on it.
    for (size_t y = 0; y < SheetH; y++)
    {
        uint8_t* Row = Canvas->pixels + y * Canvas->rowPitch;
        for (size_t x = 0; x < SheetW; x++)
        {
            Row[x * 4 + 0] = 32; Row[x * 4 + 1] = 32; Row[x * 4 + 2] = 32; Row[x * 4 + 3] = 255;
        }
    }

    int Placed = 0;
    for (int i = 0; i < Count; i++)
    {
        DirectX::ScratchImage Thumb;
        if (!DecodeThumb(*Images[i], InstallDir, ThumbSize, Thumb))
            continue;

        const DirectX::Image* T = Thumb.GetImage(0, 0, 0);
        const int Col = Placed % UseCols;
        const int Row = Placed / UseCols;
        const int DestX = Pad + Col * (ThumbSize + Pad);
        const int DestY = Pad + Row * (ThumbSize + Pad);
        Placed++;

        // Alpha-over composite the thumbnail onto the dark canvas.
        const int H = (int)std::min<size_t>((size_t)ThumbSize, T->height);
        const int W = (int)std::min<size_t>((size_t)ThumbSize, T->width);
        for (int y = 0; y < H; y++)
        {
            const uint8_t* Src = T->pixels + (size_t)y * T->rowPitch;
            uint8_t* Dst = Canvas->pixels + (size_t)(DestY + y) * Canvas->rowPitch + (size_t)DestX * 4;
            for (int x = 0; x < W; x++)
            {
                const int A = Src[x * 4 + 3];
                for (int c = 0; c < 3; c++)
                    Dst[x * 4 + c] = (uint8_t)((Src[x * 4 + c] * A + Dst[x * 4 + c] * (255 - A)) / 255);
                Dst[x * 4 + 3] = 255;
            }
        }
    }

    if (Placed == 0)
        return false;

    std::error_code Ec;
    std::filesystem::create_directories(std::filesystem::path(OutPngPath).parent_path(), Ec);

    return SUCCEEDED(DirectX::SaveToWICFile(*Canvas, DirectX::WIC_FLAGS_NONE,
                                            DirectX::GetWICCodec(DirectX::WIC_CODEC_PNG),
                                            Strings::ToUnicodeString(OutPngPath).c_str()));
}
