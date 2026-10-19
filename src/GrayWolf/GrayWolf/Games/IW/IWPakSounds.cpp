#include "stdafx.h"

#include "Games/IW/IWPakSounds.h"

#include "CoDAssets.h"
#include "ExportNaming.h"
#include "ExportWriters.h"
#include "SettingsManager.h"
#include "DiskGameSizes.h"
#include "GameWorldWar2.h"
#include "Sound.h"
#include "Strings.h"

#include <filesystem>
#include <format>
#include <set>

namespace fs = std::filesystem;

namespace IWPakSounds
{
    PackagesInUse::PackagesInUse(CoDPackageCache* Packages) : Saved(std::move(CoDAssets::GamePackageCache))
    {
        CoDAssets::GamePackageCache = std::unique_ptr<CoDPackageCache>(Packages);
    }

    PackagesInUse::~PackagesInUse()
    {
        CoDAssets::GamePackageCache.release();
        CoDAssets::GamePackageCache = std::move(Saved);
    }

    std::string Extension()
    {
        return ExportWriters::SoundExtension(ExportWriters::SoundFormatSetting());
    }

    void Write(std::vector<CoDAsset_t*>& Found, CoDPackageCache* Packages, const DiskGame::Context& C, DiskGame::Totals& T,
               const std::string& Zone, const Counters& Counter, const WriteOptions& Options)
    {
        auto& Kt = T.Kinds[DiskGame::Kind::Sound];
        const PackagesInUse Using(Packages);
        const std::string Extension = IWPakSounds::Extension();
        const std::string Dir = C.KindDir(DiskGame::Kind::Sound);
        const auto Success = static_cast<int>(ExportGameResult::Success);
        std::set<std::string> Names;
        if (C.OnSoundHeader) T.Counters["sound_headers"] = 1;
        bool Going = true;   // until cancelled; the rest is only freed
        for (size_t N = 0; N < Found.size(); N++)
        {
            auto* Sound = static_cast<CoDSound_t*>(Found[N]);
            Going = Going && C.Advance(N, Found.size(), Sound->AssetName);
            Sound->AssetName = ExportNaming::SafeFileName(Sound->AssetName);
            if (!Going) {}
            else if (!Sound->IsFileEntry && Sound->AssetSize == 0) Kt.References++;
            else if (C.Wants(DiskGame::Kind::Sound, Sound->AssetName) && (!Options.OnePerName || Names.insert(Sound->AssetName = Strings::ToLower(Sound->AssetName)).second))
            {
                Kt.Seen++;
                if (C.OnSoundHeader)
                {
                    // a dry run: what GameWorldWar2::ReadXSound would give WriteSound - PCM behind
                    // a WAV header it adds, FLAC behind one it adds, or a whole FLAC file
                    DiskGame::SoundHeader H;
                    H.Name = Sound->AssetName;
                    H.Rate = Sound->FrameRate, H.Channels = Sound->ChannelsCount, H.Frames = Sound->FrameCount;
                    if (Sound->DataType == SoundDataTypes::WAV_NeedsHeader)
                    {
                        H.Is = DiskGame::SoundHeader::Form::Wav;
                        H.Stored = Sound->AssetSize + Sound::GetMaximumWAVHeaderSize();
                        if (H.Channels) H.Frames = Sound->AssetSize / (2ull * H.Channels);
                    }
                    else
                    {
                        H.Is = DiskGame::SoundHeader::Form::Flac;
                        H.Stored = Sound->AssetSize + (Sound->DataType == SoundDataTypes::FLAC_NeedsHeader ? Sound::GetMaximumFLACHeaderSize() : 0);
                        // a whole FLAC file (streamed, primed): its length in its STREAMINFO,
                        // the first bytes of its data, not in the zone's headers
                        if (Sound->DataType != SoundDataTypes::FLAC_NeedsHeader)
                        {
                            const auto Head = GameWorldWar2::ReadSoundHead(Sound, 42);
                            uint32_t Rate = 0, Channels = 0, Bits = 0;
                            uint64_t Frames = 0;
                            if (DiskGame::Sizes::FlacStreamInfo(Head.data(), Head.size(), Rate, Channels, Bits, Frames))
                                H.Rate = Rate, H.Channels = Channels, H.Bits = Bits, H.Frames = Frames;
                            else
                                H.Frames = 0;   // the zone's frame count is not the file's
                        }
                    }
                    C.OnSoundHeader(H);
                    delete Sound;
                    continue;
                }
                Counter(Sound->IsFileEntry ? "sounds_streamed" : "sounds_loaded")++;
                std::string Crash;
                const int Code = ExportWriters::ExportSound(*Sound, Dir, Extension, Crash);
                std::error_code Ec;
                const fs::path File = fs::path(Dir) / (Sound->AssetName + Extension);
                if (Code == Success && Options.EmptyFileFails && fs::exists(File, Ec) && fs::file_size(File, Ec) == 0)
                    T.Fail(Zone, DiskGame::Kind::Sound, Sound->AssetName, std::format("empty file ({}, package {}, offset {:x}, size {})",
                        Sound->IsFileEntry ? "pak" : "zone", (unsigned)Sound->PackageIndex, (unsigned long long)Sound->AssetPointer, (unsigned long long)Sound->AssetSize));
                else if (Code == Success) Kt.Written++;
                else T.Fail(Zone, DiskGame::Kind::Sound, Sound->AssetName, std::format("code {} {} ({}, package {}, offset {:x}, size {}, localized {})",
                    Code, Crash.c_str(), Sound->IsFileEntry ? "streamed" : "loaded", (unsigned)Sound->PackageIndex,
                    (unsigned long long)Sound->AssetPointer, (unsigned long long)Sound->AssetSize, Sound->IsLocalized ? 1 : 0));
            }
            delete Sound;
        }
        Found.clear();
    }
}
