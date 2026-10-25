// NOMINMAX before <windows.h>: std::max/std::min below would be mangled by its macros.
#define NOMINMAX

#include <cmath>
#include <memory>
#include <filesystem>
#include <fstream>
#include <algorithm>
#include <vector>
#include <iterator>
#include <string>
#include <format>

#include "CliOptions.h"
#include "CliCommands.h"

#include <CLI/CLI.hpp>
#include "CliSupport.h"

#include "WraithX.h"
#include "CoDAssets.h"

#include "AudioPlayer.h"
#include "SABSupport.h"
#include "IWDSoundDecoder.h"
#include "Sound.h"

// Decodes an exported Opus file back to PCM to prove the encoder produced something
// a real decoder accepts, rather than just a plausible looking header
int RunVerifyOpus(const CliOptions& Options)
{
    uint32_t Channels = 0;
    uint64_t Samples = 0;
    const auto Valid = Sound::ValidateOpusFile(Options.VerifyOpus, Channels, Samples);
    // --out <wav>: also the samples a player gets (pre-skip and end trimming applied), as a WAV
    const bool Decoded = Valid && !Options.Out.empty() && Sound::DecodeOpusToWav(Options.VerifyOpus, Options.Out);

    CliReport(Options, std::format(
        "result={}\nfile={}\nchannels={}\nsamples={}\n{}",
        Valid ? "ok" : "error",
        Options.VerifyOpus.c_str(),
        Channels,
        (unsigned long long)Samples,
        Options.Out.empty() ? std::string() : "wav=" + (Decoded ? Options.Out : std::string("not written")) + "\n"));

    return Valid ? 0 : 5;
}


// Diagnostic: decode an audio file through the same AudioPlayer the GUI uses and print
// what it recovered (duration / sample rate / channels / waveform peak count). Proves the
// miniaudio decode path end-to-end without needing the interactive shell.
int RunAudioProbe(const CliOptions& Options)
{
    std::string Ext;
    { auto d = Options.AudioProbe.find_last_of('.');
      if (d != std::string::npos) { Ext = Options.AudioProbe.substr(d + 1);
          for (char& c : Ext) c = (char)::tolower((unsigned char)c); } }
    AudioPlayer Ap;
    const bool Ok = Ap.Load(Options.AudioProbe, Ext);
    if (!Ok)
    {
        CliReport(Options, "result=error\nreason=AudioPlayer could not decode this file\n");
        return 5;
    }
    // A rough non-silence measure so we know the PCM is real, not zeros.
    float Peak = 0.0f;
    for (size_t k = 0; k < Ap.PeaksMax().size(); ++k)
        Peak = std::max(Peak, std::max(Ap.PeaksMax()[k], -Ap.PeaksMin()[k]));
    CliReport(Options, std::format(
        "result=ok\nformat={}\nduration_s={:.3f}\nsample_rate={}\nchannels={}\npeak_columns={}\nmax_amplitude={:.3f}\n",
        Ext.c_str(), Ap.Duration(), Ap.SampleRate(), Ap.Channels(), Ap.PeaksMax().size(), Peak));
    return 0;
}


// Decodes one Black Ops 1 IWD sound (custom block MS-ADPCM) to a playable PCM16 WAV.
int RunDecodeIwd(const CliOptions& Options)
{
    std::ifstream In(Options.DecodeIwd, std::ios::binary);
    if (!In)
    {
        CliReport(Options, "result=error\nreason=cannot open input\n");
        return 5;
    }
    std::vector<uint8_t> Raw((std::istreambuf_iterator<char>(In)), std::istreambuf_iterator<char>());

    std::vector<uint8_t> Wav;
    if (!DecodeIWDSound(Raw, Wav))
    {
        CliReport(Options, "result=error\nreason=not a recognisable IWD sound\n");
        return 5;
    }

    const bool Opus = (Options.AudioFormat == "opus");
    const std::string OutPath = !Options.Out.empty() ? Options.Out
                                                      : (Options.DecodeIwd + (Opus ? ".opus" : ".wav"));
    if (Opus)
    {
        const bool Ok = Sound::ConvertSoundMemory((int8_t*)Wav.data(), (uint64_t)Wav.size(),
            SoundFormat::WAV_WithHeader, OutPath, SoundFormat::Standard_OPUS);
        CliReport(Options, std::format("result={}\nout={}\nformat=opus\n", Ok ? "ok" : "error", OutPath.c_str()));
        return Ok ? 0 : 5;
    }
    std::ofstream OutF(OutPath, std::ios::binary | std::ios::trunc);
    OutF.write((const char*)Wav.data(), (std::streamsize)Wav.size());

    CliReport(Options, std::format("result=ok\nout={}\nwav_bytes={}\n", OutPath.c_str(), Wav.size()));
    return 0;
}


// Feasibility probe for adding sound support to a game: run the EXISTING SAB parser against a
// bank straight off disk, with no running game. If the entries come out named and a sound
// decodes, the format work is already done and only the per-game wiring is missing.
int RunSabProbe(const CliOptions& Options)
{
    WraithX::InitializeAPI();
    CoDAssets::GameAssets = std::make_unique<AssetPool>();

    // SABSupport::LoadSound reads the bank back through GamePackageCache->GetPackagesPath().
    // Without a cache pointed at the file it dereferences null.
    // No WaitForPackageCacheLoad here: the base cache only reaches the loaded state for a
    // DIRECTORY of packages, so waiting on a single bank file never returns. LoadSound only
    // needs the path, which LoadPackageCacheAsync records before its worker does anything.
    CoDAssets::GamePackageCache = std::make_unique<CoDPackageCache>();
    CoDAssets::GamePackageCache->LoadPackageCacheAsync(Options.SabProbe);

    // A directory parses EVERY bank in it. That is the case the per-entry BankFile exists for:
    // decoding an entry from the first bank afterwards only works if each sound remembers its
    // own file, since the package cache still points at whichever bank was parsed last.
    std::vector<std::string> Banks;
    {
        std::error_code Ec;
        if (std::filesystem::is_directory(Options.SabProbe, Ec))
        {
            for (const auto& E : std::filesystem::directory_iterator(Options.SabProbe, Ec))
                if (E.is_regular_file(Ec) && E.path().extension() == ".sabl")
                    Banks.push_back(E.path().string());
            std::sort(Banks.begin(), Banks.end());
        }
        else Banks.push_back(Options.SabProbe);
    }

    const auto Start = CliClock::now();
    bool Parsed = false;
    for (const auto& Bank : Banks)
    {
        CoDAssets::GamePackageCache->LoadPackageCacheAsync(Bank);
        if (SABSupport::ParseSAB(Bank)) Parsed = true;
    }
    const auto Ms = CliElapsedMs(Start, CliClock::now());

    std::string Report = std::format("file={}\nbanks={}\nparsed={}\nparse_ms={}\nentries={}\n",
        Options.SabProbe.c_str(), Banks.size(), Parsed ? "yes" : "no", (long long)Ms,
        CoDAssets::GameAssets->LoadedAssets.size());

    const uint32_t Show = Options.Limit > 0 ? Options.Limit : 5;
    uint32_t Shown = 0;
    const CoDSound_t* First = nullptr;
    for (const auto* A : CoDAssets::GameAssets->LoadedAssets)
    {
        const auto* S = dynamic_cast<const CoDSound_t*>(A);
        if (S == nullptr) continue;
        if (First == nullptr) First = S;
        if (Shown++ >= Show) continue;
        Report += std::format("sound name={} rate={} ch={} frames={}\n",
            S->AssetName.c_str(), S->FrameRate, (unsigned)S->ChannelsCount, S->FrameCount);
    }

    // Decoding is gated behind --out: it needs more of the app standing up than parsing does
    // (package cache, game id), so keeping it opt-in tells us WHICH half fails.
    if (First != nullptr && !Options.Out.empty())
    {
        auto Decoded = SABSupport::LoadSound(First);
        Report += std::format("decode={} bytes={}\n",
            Decoded != nullptr ? "ok" : "failed", Decoded != nullptr ? Decoded->DataSize : 0u);

        if (Decoded != nullptr && Decoded->DataBuffer != nullptr)
        {
            std::ofstream Wav(Options.Out, std::ios::binary | std::ios::trunc);
            Wav.write((const char*)Decoded->DataBuffer, (std::streamsize)Decoded->DataSize);
            Report += std::format("wrote={}\n", Options.Out.c_str());
        }
    }

    Report += (Parsed && First != nullptr) ? "result=ok\n" : "result=error\n";
    CliReport(Options, Report);
    WraithX::ShutdownAPI();
    return (Parsed && First != nullptr) ? 0 : 5;
}

// ---- the subcommands (CliCommands.h)

void AddAudioCommands(CliCommandBuilders& B)
{
    B["verify-opus"] = [](CLI::App& App, CliOptions& Options) {
        auto* Sub = App.add_subcommand("verify-opus", "Decode an Opus file and report duration/rate/peaks");
        Sub->add_option("file", Options.VerifyOpus, "Opus file to decode")->required()->check(CLI::ExistingFile);
    };
    B["audio-probe"] = [](CLI::App& App, CliOptions& Options) {
        auto* Sub = App.add_subcommand("audio-probe", "Decode via AudioPlayer and print duration/rate/peaks");
        Sub->add_option("file", Options.AudioProbe, "Audio file to decode")->required()->check(CLI::ExistingFile);
    };
    B["decode-iwd"] = [](CLI::App& App, CliOptions& Options) {
        auto* Sub = App.add_subcommand("decode-iwd", "BO1 IWD sound -> PCM WAV (--audio-format opus for opus)");
        Sub->add_option("file", Options.DecodeIwd, "IWD sound entry to decode")->required()->check(CLI::ExistingFile);
    };
    B["sab-probe"] = [](CLI::App& App, CliOptions& Options) {
        auto* Sub = App.add_subcommand("sab-probe", "Run SABSupport on a sound bank");
        Sub->add_option("file", Options.SabProbe, "Sound bank (.sab) file, or a folder of .sabl banks")->required()->check(CLI::ExistingPath);
    };
}
