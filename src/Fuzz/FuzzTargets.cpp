// libFuzzer harnesses for GrayWolf's parsers of user files (Fuzz.vcxproj, run by tools/fuzz.ps1).
// One exe, the target named by GW_FUZZ_TARGET (or a first argument -target=<name>, which sets it so
// libFuzzer's -jobs children inherit it). Parsers that take a path read the input from a file of
// this process's own in %TEMP%\graywolf-fuzz\<pid>\.

#include "FastFile.h"
#include <wolff/Codol.h>
namespace Ifs = wolff::Ifs;
#include "SABSupport.h"
#include "XPAKSupport.h"
#include "IPAKSupport.h"
#include "IWDSupport.h"
#include "CoDIWIDecoder.h"
#include "CoDIWITranslator.h"

#include "SeAnimReader.h"
#include "ModelPreview.h"
#include "MaterialDescription.h"
#include "CoDAssets.h"
#include "CoDAssetType.h"
#include "WraithAnim.h"
#include "Sound.h"
#include "Image.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include <Windows.h>

namespace
{
    std::string Target;
    std::filesystem::path WorkDir;

    // The input as a file with the extension the parser expects
    std::string AsFile(const uint8_t* Data, size_t Size, const char* Name)
    {
        const auto Path = WorkDir / Name;
        std::ofstream Out(Path, std::ios::binary | std::ios::trunc);
        Out.write(reinterpret_cast<const char*>(Data), (std::streamsize)Size);
        return Path.string();
    }

    // A fresh asset pool, as a game load starts with
    void ResetAssets()
    {
        CoDAssets::GameAssets = std::make_unique<AssetPool>();
        CoDAssets::Game = nullptr;
    }

    void FuzzFastFile(const uint8_t* Data, size_t Size)
    {
        const auto Path = AsFile(Data, Size, "zone.ff");
        (void)FastFile::Identify(Path);
        std::vector<uint8_t> Zone;
        std::string Error;
        (void)FastFile::Decompress(Path, Zone, Error);
    }

    void FuzzIfs(const uint8_t* Data, size_t Size)
    {
        const auto Path = AsFile(Data, Size, "archive.ifs");
        Ifs::Archive Archive;
        std::string Error;
        if (!Archive.Open(Path, Error))
            return;
        size_t Count = 0;
        for (const auto& E : Archive.Files())
        {
            if (++Count > 16)
                break;
            std::vector<uint8_t> Out;
            (void)Archive.Read(E, Out, Error);
        }
    }

    void FuzzSab(const uint8_t* Data, size_t Size)
    {
        ResetAssets();
        const auto Path = AsFile(Data, Size, "bank.sabl");
        if (SABSupport::ParseSAB(Path))
        {
            // the sounds as the export reads them back, those whose samples lie within the file
            size_t Count = 0;
            for (auto* Asset : CoDAssets::GameAssets->LoadedAssets)
            {
                auto* Sound = dynamic_cast<CoDSound_t*>(Asset);
                if (Sound == nullptr || Sound->AssetSize > Size || ++Count > 8)
                    continue;
                if (Sound->DataType == SoundDataTypes::WAV_WithHeader || Sound->DataType == SoundDataTypes::WAV_NeedsHeader ||
                    Sound->DataType == SoundDataTypes::FLAC_WithHeader || Sound->DataType == SoundDataTypes::FLAC_NeedsHeader)
                    (void)SABSupport::LoadSound(Sound);
            }
        }
        ResetAssets();
    }

    void FuzzXpak(const uint8_t* Data, size_t Size)
    {
        ResetAssets();
        (void)XPAKSupport::ParseXPAK(AsFile(Data, Size, "images.xpak"));
        ResetAssets();
    }

    void FuzzIpak(const uint8_t* Data, size_t Size)
    {
        ResetAssets();
        (void)IPAKSupport::ParseIPAK(AsFile(Data, Size, "images.ipak"));
        ResetAssets();
    }

    void FuzzIwd(const uint8_t* Data, size_t Size)
    {
        ResetAssets();
        (void)IWDSupport::ParseIWD(AsFile(Data, Size, "images.iwd"));
        ResetAssets();
    }

    void FuzzIwi(const uint8_t* Data, size_t Size)
    {
        std::string Dds, Error;
        (void)CoDIWIDecoder::ToDds(Data, Size, Dds, Error);
        if (Size > 0xFFFFFFFFull)
            return;
        auto Copy = std::make_unique<uint8_t[]>(Size);
        if (Size)
            std::memcpy(Copy.get(), Data, Size);
        (void)CoDIWITranslator::TranslateIWI(Copy, (uint32_t)Size);
    }

    void FuzzSeAnim(const uint8_t* Data, size_t Size)
    {
        SeAnimClip Clip;
        if (ReadSeAnim(AsFile(Data, Size, "anim.seanim"), Clip))
        {
            WraithAnim Anim;
            (void)BuildWraithAnimFromSeAnimClip(Clip, Anim);
        }
    }

    void FuzzSeModel(const uint8_t* Data, size_t Size)
    {
        PreviewGeometry Geometry;
        (void)LoadPreviewGeometry({ AsFile(Data, Size, "model.semodel") }, Geometry);
    }

    void FuzzSidecar(const uint8_t* Data, size_t Size)
    {
        Materials::Sidecar Sidecar;
        (void)Materials::ReadSidecar(AsFile(Data, Size, "model.materials.json"), Sidecar);
    }

    struct TargetInfo { const char* Name; void (*Run)(const uint8_t*, size_t); };
    const TargetInfo Targets[] = {
        { "fastfile", FuzzFastFile }, { "ifs", FuzzIfs }, { "sab", FuzzSab }, { "xpak", FuzzXpak },
        { "ipak", FuzzIpak }, { "iwd", FuzzIwd }, { "iwi", FuzzIwi }, { "seanim", FuzzSeAnim },
        { "semodel", FuzzSeModel }, { "sidecar", FuzzSidecar },
    };
    void (*Run)(const uint8_t*, size_t) = nullptr;
}

extern "C" int LLVMFuzzerInitialize(int* argc, char*** argv)
{
    // -target=<name>: consumed here, kept in the environment for -jobs/-workers children
    for (int i = 1; i < *argc; i++)
    {
        if (std::strncmp((*argv)[i], "-target=", 8) == 0)
        {
            _putenv_s("GW_FUZZ_TARGET", (*argv)[i] + 8);
            for (int j = i; j + 1 < *argc; j++)
                (*argv)[j] = (*argv)[j + 1];
            --*argc;
            break;
        }
    }
    char Name[64] = {};
    GetEnvironmentVariableA("GW_FUZZ_TARGET", Name, sizeof(Name));
    Target = Name;
    for (const auto& T : Targets)
        if (Target == T.Name)
            Run = T.Run;
    if (Run == nullptr)
    {
        std::fprintf(stderr, "GrayWolfFuzz: set GW_FUZZ_TARGET (or pass -target=) to one of:");
        for (const auto& T : Targets)
            std::fprintf(stderr, " %s", T.Name);
        std::fprintf(stderr, "\n");
        std::exit(2);
    }
    WorkDir = std::filesystem::temp_directory_path() / "graywolf-fuzz" / std::to_string(GetCurrentProcessId());
    std::filesystem::create_directories(WorkDir);
    return 0;
}

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* Data, size_t Size)
{
    // A C++ exception is a rejected input: the readers throw on short data (MemoryReader) and the
    // app's callers catch it. Memory errors, hangs and allocation bombs are what is looked for.
    try
    {
        Run(Data, Size);
    }
    catch (...)
    {
    }
    return 0;
}
