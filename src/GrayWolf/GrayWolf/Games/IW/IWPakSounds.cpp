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
#include <utility>

namespace fs = std::filesystem;

namespace IWPakSounds
{

    std::string Extension()
    {
        return ExportWriters::SoundExtension(ExportWriters::SoundFormatSetting());
    }

    void Write(std::vector<CoDAsset_t*>& Found, std::shared_ptr<CoDPackageCache> Packages, const DiskGame::Context& C, DiskGame::Totals& T,
               const std::string& Zone, const Counters& Counter, const WriteOptions& Options)
    {
        auto& Kt = T.Kinds[DiskGame::Kind::Sound];
        const CoDAssets::Scoped Using(CoDAssets::GamePackageCache, std::move(Packages));
        const CoDAssets::Scoped Language(GameWorldWar2::SoundLanguage, Options.Language);
        const std::string Extension = IWPakSounds::Extension();
        const std::string Dir = C.KindDir(DiskGame::Kind::Sound);
        const auto Success = static_cast<int>(ExportGameResult::Success);
        std::set<std::string> Names;
        ExportWriters::SoundFiles Files;
        // read here (the swapped globals are this thread's), decoded and written by the sink's pool
        ExportWriters::SoundSink Sink;
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
                Files.Write(Sink, CoDAssets::Game, *Sound, Dir, Extension,
                            [&Kt, &T, &Zone, Empty = Options.EmptyFileFails, Asset = Sound->AssetName, Streamed = Sound->IsFileEntry,
                             Package = (unsigned)Sound->PackageIndex, Pointer = (unsigned long long)Sound->AssetPointer,
                             Size = (unsigned long long)Sound->AssetSize, Localized = Sound->IsLocalized ? 1 : 0](
                                int Code, const std::string& Crash, const fs::path& File) {
                                std::error_code Ec;
                                if (Code == Success && Empty && fs::exists(File, Ec) && fs::file_size(File, Ec) == 0)
                                    T.Fail(Zone, DiskGame::Kind::Sound, Asset, std::format("empty file ({}, package {}, offset {:x}, size {})",
                                        Streamed ? "pak" : "zone", Package, Pointer, Size));
                                else if (Code == Success) Kt.Written++;
                                else T.Fail(Zone, DiskGame::Kind::Sound, Asset, std::format("code {} {} ({}, package {}, offset {:x}, size {}, localized {})",
                                    Code, Crash.c_str(), Streamed ? "streamed" : "loaded", Package, Pointer, Size, Localized));
                            });
            }
            delete Sound;
        }
        Found.clear();
        Sink.Finish();
        if (Files.Renamed) Counter("sounds_renamed") += Files.Renamed;
        if (Files.Copies) Counter("sounds_same_file") += Files.Copies;
    }
}
