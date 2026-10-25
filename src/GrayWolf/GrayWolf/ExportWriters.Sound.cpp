#include "stdafx.h"

// ExportWriters' sounds: one writer for every game (ExportWriters.h, WriteSound). WAV stays what
// each game wrote before (the same bytes); FLAC and OPUS decode what is not already that format and
// encode it, except the game's own Opus packets, which an .opus file carries as they are.

#include "ExportWriters.h"

#include "CoDAssetType.h"
#include "SABSupport.h"
#include "SettingsManager.h"
#include "Sound.h"
#include "Strings.h"
#include "WmaSounds.h"

#include "miniaudio.h"
#include <opus/opus.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <functional>
#include <mutex>
#include <numeric>

namespace ExportWriters
{
namespace
{
    std::mutex CountersLock;
    std::map<std::string, size_t> Counts;

    void Count(const char* Name)
    {
        std::lock_guard<std::mutex> Lock(CountersLock);
        Counts[Name]++;
    }

    std::string Lower(std::string S)
    {
        return Strings::ToLower(std::move(S));
    }

    // The samples of a 16-bit PCM WAV (format 1 or WAVE_FORMAT_EXTENSIBLE), false for any other
    bool PcmOfWav(const uint8_t* Data, size_t Size, std::vector<int16_t>& Samples, uint32_t& Channels, uint32_t& Rate)
    {
        if (Size < 12 || std::memcmp(Data, "RIFF", 4) != 0 || std::memcmp(Data + 8, "WAVE", 4) != 0) return false;
        uint16_t Tag = 0, Bits = 0;
        Channels = Rate = 0;
        for (size_t At = 12; At + 8 <= Size;)
        {
            uint32_t Len = 0;
            std::memcpy(&Len, Data + At + 4, 4);
            const uint8_t* Body = Data + At + 8;
            const size_t Room = Size - (At + 8);
            if (std::memcmp(Data + At, "fmt ", 4) == 0 && Len >= 16 && Room >= 16)
            {
                uint16_t Ch = 0;
                std::memcpy(&Tag, Body, 2);
                std::memcpy(&Ch, Body + 2, 2);
                std::memcpy(&Rate, Body + 4, 4);
                std::memcpy(&Bits, Body + 14, 2);
                Channels = Ch;
                if (Tag == 0xFFFE && Len >= 26 && Room >= 26) std::memcpy(&Tag, Body + 24, 2);   // the sub-format's
            }
            else if (std::memcmp(Data + At, "data", 4) == 0)
            {
                if (Tag != 1 || Bits != 16 || Channels == 0 || Rate == 0) return false;
                const size_t N = std::min<size_t>(Len, Room) / 2;
                Samples.assign(N, 0);
                if (N) std::memcpy(Samples.data(), Body, N * 2);
                return true;
            }
            At += 8 + (size_t)Len + (Len & 1);
        }
        return false;
    }

    // Any file miniaudio decodes (WAV with ADPCM, 8/24/32-bit or float, FLAC, MP3) as 16-bit PCM
    bool PcmOfFile(const uint8_t* Data, size_t Size, std::vector<int16_t>& Samples, uint32_t& Channels, uint32_t& Rate)
    {
        ma_decoder_config Config = ma_decoder_config_init(ma_format_s16, 0, 0);
        ma_decoder Decoder;
        if (ma_decoder_init_memory(Data, Size, &Config, &Decoder) != MA_SUCCESS) return false;
        Channels = Decoder.outputChannels;
        Rate = Decoder.outputSampleRate;
        Samples.clear();
        std::vector<int16_t> Chunk(4096 * (size_t)std::max<uint32_t>(Channels, 1));
        for (;;)
        {
            ma_uint64 Got = 0;
            const ma_result R = ma_decoder_read_pcm_frames(&Decoder, Chunk.data(), 4096, &Got);
            Samples.insert(Samples.end(), Chunk.begin(), Chunk.begin() + (size_t)Got * Channels);
            if (R != MA_SUCCESS || Got == 0) break;
        }
        ma_decoder_uninit(&Decoder);
        return Channels > 0 && Rate > 0 && !Samples.empty();
    }

    // The game's Opus packets as the WAV path decodes them (SABSupport::DecodeOpusInterleaved: the
    // sound's frames after the game's pre-skip), as a WAV file's bytes
    std::unique_ptr<XSound> WavOfOpus(const SoundSource& S)
    {
        return SABSupport::DecodeOpusInterleaved(S.Data, S.Size, 0, S.Rate, S.Channels, S.Frames, S.PreSkip);
    }

    bool Pcm(const SoundSource& S, std::vector<int16_t>& Samples, uint32_t& Channels, uint32_t& Rate)
    {
        switch (S.Is)
        {
        case SoundSource::Form::Pcm16:
            Samples.assign(S.Size / 2, 0);
            if (!Samples.empty()) std::memcpy(Samples.data(), S.Data, Samples.size() * 2);
            Channels = S.Channels, Rate = S.Rate;
            return Channels > 0 && Rate > 0 && !Samples.empty();
        case SoundSource::Form::Opus:
        {
            const auto Wav = WavOfOpus(S);
            return Wav != nullptr && PcmOfWav(reinterpret_cast<const uint8_t*>(Wav->DataBuffer), Wav->DataSize, Samples, Channels, Rate);
        }
        default:
            break;
        }
        const std::string Ext = Lower(S.Extension);
        if (Ext == ".wav" && PcmOfWav(S.Data, S.Size, Samples, Channels, Rate)) return true;
        // a FLAC stream behind a WAV header that does not describe it (WWII's sounds whose header
        // says PCM: no channels, no rate)
        for (size_t At = 0; Ext == ".wav" && At + 4 <= std::min<size_t>(S.Size, 128); At++)
            if (std::memcmp(S.Data + At, "fLaC", 4) == 0)
                return Sound::DecodeFLACToPCM(reinterpret_cast<int8_t*>(const_cast<uint8_t*>(S.Data + At)), S.Size - At, Samples, Channels, Rate);
        if (Ext == ".flac" && Sound::DecodeFLACToPCM(reinterpret_cast<int8_t*>(const_cast<uint8_t*>(S.Data)), S.Size, Samples, Channels, Rate))
            return true;
        return PcmOfFile(S.Data, S.Size, Samples, Channels, Rate);
    }

    // To 48 kHz, for an Opus encoder that takes 8, 12, 16, 24 or 48 kHz only: windowed sinc (a
    // Blackman window over 32 input samples each side), its cut-off at the lower of the two
    // Nyquist frequencies, centred on each output instant, so the result lines up with the source
    // and lasts as long
    bool Resample48k(std::vector<int16_t>& Samples, uint32_t Channels, uint32_t& Rate)
    {
        constexpr int Half = 32;
        constexpr double Pi = 3.14159265358979323846;
        if (Rate == 0 || Channels == 0) return false;
        const int64_t In = (int64_t)(Samples.size() / Channels);
        const int64_t Out = (In * 48000 + Rate / 2) / Rate;
        const double Cut = std::min(1.0, 48000.0 / (double)Rate);  // of the input's Nyquist; also the gain
        const int Taps = (int)std::ceil(Half / Cut);               // input samples each side
        // output K sits at input K * Rate / 48000 = Base + Phase / Phases: one row of taps per phase
        // (an odd rate whose phases do not repeat soon: 4096 phases, each instant rounded to one)
        const uint32_t G = std::gcd(Rate, 48000u);
        const bool Exact = 48000 / G <= 4096;
        const uint32_t Phases = Exact ? 48000 / G : 4096, Num = Rate / G;
        const int Row = 2 * Taps + 1;
        std::vector<double> Table((size_t)Phases * Row);
        for (uint32_t P = 0; P < Phases; P++)
            for (int T = -Taps; T <= Taps; T++)
            {
                const double X = (double)T - (double)P / Phases;   // input sample minus the instant
                double H = 0.0;
                if (std::abs(X) < Taps)
                {
                    const double U = X * Cut;
                    const double Sinc = std::abs(U) < 1e-12 ? 1.0 : std::sin(Pi * U) / (Pi * U);
                    const double W = 0.42 + 0.5 * std::cos(Pi * X / Taps) + 0.08 * std::cos(2.0 * Pi * X / Taps);
                    H = Cut * Sinc * W;
                }
                Table[(size_t)P * Row + (T + Taps)] = H;
            }
        std::vector<int16_t> Result((size_t)Out * Channels);
        std::vector<double> Acc(Channels);
        for (int64_t K = 0; K < Out; K++)
        {
            const int64_t Pos = Exact ? K * Num : (int64_t)std::llround((double)K * Rate / 48000.0 * Phases);
            const int64_t Base = Pos / Phases;
            const uint32_t P = (uint32_t)(Pos % Phases);
            const double* H = Table.data() + (size_t)P * Row;
            std::fill(Acc.begin(), Acc.end(), 0.0);
            const int64_t From = std::max<int64_t>(-Taps, -Base), To = std::min<int64_t>(Taps, In - 1 - Base);
            for (int64_t T = From; T <= To; T++)
            {
                const double Hk = H[T + Taps];
                const int16_t* Frame = Samples.data() + (size_t)(Base + T) * Channels;
                for (uint32_t C = 0; C < Channels; C++) Acc[C] += Hk * Frame[C];
            }
            for (uint32_t C = 0; C < Channels; C++)
                Result[(size_t)K * Channels + C] = (int16_t)std::clamp(std::lround(Acc[C]), -32768L, 32767L);
        }
        if (Result.empty()) return false;
        Samples.swap(Result);
        Rate = 48000;
        return true;
    }

    bool OpusRate(uint32_t Rate)
    {
        return Rate == 8000 || Rate == 12000 || Rate == 16000 || Rate == 24000 || Rate == 48000;
    }

    // The game's packets in an Ogg Opus file, as they are, so that it plays what the WAV path
    // writes, sample for sample: the game's pre-skip in the header (SABSupport::OpusPreSkip...) and
    // the last page's granule at the sound's end (pre-skip + frames, at 48 kHz), the packets wholly
    // past it left out; a game whose pre-skip is not measured keeps pre-skip 0 and every packet's
    // samples. The zero-size packets after the last one (the package's padding) are left out.
    bool RemuxOpus(const SoundSource& S, const std::string& Path, std::string* Why)
    {
        const size_t Held = std::max(S.Available, S.Size);
        const bool Trim = S.PreSkip >= 0 && S.Frames > 0 && S.Rate > 0;
        const uint64_t End = Trim ? (uint64_t)S.PreSkip + (uint64_t)S.Frames * 48000 / S.Rate : 0;
        std::vector<std::pair<const uint8_t*, uint32_t>> Packets;
        uint64_t Samples = 0;
        for (size_t At = 0; At < S.Size && At + 2 <= Held && (!Trim || Samples < End);)
        {
            uint16_t Len = 0;
            std::memcpy(&Len, S.Data + At, 2);
            if (Len == 0 || At + 2 + Len > Held) break;
            const uint8_t* P = S.Data + At + 2;
            const int N = opus_packet_get_nb_samples(P, Len, 48000);
            if (N <= 0) break;
            Packets.emplace_back(P, Len);
            Samples += (uint64_t)N;
            At += 2 + (size_t)Len;
        }
        if (Packets.empty())
        {
            if (Why) *Why = "no Opus packet";
            return false;
        }
        // the decoder's channels: the asset's, else the first packet's (its TOC's stereo bit)
        const uint32_t Channels = (S.Channels == 1 || S.Channels == 2) ? S.Channels : ((Packets[0].first[0] & 4) ? 2 : 1);
        if (!Sound::WriteOggOpus(Packets, Channels, S.Rate, Trim ? (uint16_t)S.PreSkip : 0, End, Path))
        {
            if (Why) *Why = "Ogg Opus not written";
            return false;
        }
        Count("sounds_opus_remuxed");
        return true;
    }

    // The encoders open their file by a narrow path: a path the code page cannot spell (CODOL's
    // Chinese names) is written under a temporary name, then moved to its own
    template <class F>
    bool ViaNarrowPath(const std::filesystem::path& Path, F Write)
    {
        const std::string Narrow = Path.string();
        if (std::filesystem::path(Narrow) == Path) return Write(Narrow);
        std::error_code Ec;
        const std::filesystem::path Tmp = std::filesystem::temp_directory_path(Ec) /
            ("graywolf_sound_" + std::to_string(std::hash<std::wstring>()(Path.filename().wstring())) + Path.extension().string());
        if (!Write(Tmp.string())) return false;
        std::filesystem::copy_file(Tmp, Path, std::filesystem::copy_options::overwrite_existing, Ec);
        std::error_code Ignored;
        std::filesystem::remove(Tmp, Ignored);
        return !Ec;
    }
}

std::string SoundFormatSetting()
{
    const std::string F = Strings::ToUpper(SettingsManager::GetSetting("exportsnd", "WAV"));
    return F == "FLAC" || F == "OPUS" ? F : "WAV";
}

std::string SoundExtension(const std::string& Format, const std::string& SourceExtension, bool KeptUnderWav)
{
    const std::string F = Strings::ToUpper(Format);
    if (F == "FLAC") return ".flac";
    if (F == "OPUS") return ".opus";
    const std::string Ext = Lower(SourceExtension);
    return (KeptUnderWav || (!Ext.empty() && Ext != ".flac")) ? SourceExtension : ".wav";
}

std::string SoundFileName(const std::string& Name, const std::string& Format, bool KeptUnderWav)
{
    // the extension by its bytes (a name in a game's own code page is not converted)
    const size_t Dot = Name.rfind('.'), Slash = Name.find_last_of("/\\");
    const std::string Ext = (Dot == std::string::npos || (Slash != std::string::npos && Dot < Slash) || Dot + 1 == Name.size())
        ? std::string() : Name.substr(Dot);
    const std::string To = SoundExtension(Format, Ext.empty() ? ".wav" : Ext, KeptUnderWav);
    // a name kept as it is: in its own format, or without an extension under WAV (a WAV)
    if (Lower(To) == Lower(Ext) || (Ext.empty() && To == ".wav")) return Name;
    return Ext.empty() ? Name + To : Name.substr(0, Name.size() - Ext.size()) + To;
}

SoundSource SourceOf(const XSound& X)
{
    SoundSource S;
    S.Data = reinterpret_cast<const uint8_t*>(X.DataBuffer);
    S.Size = S.Available = X.DataSize;
    if (X.DataType == SoundDataTypes::Opus_Interleaved)
    {
        S.Is = SoundSource::Form::Opus;
        S.Rate = X.FrameRate, S.Channels = X.Channels, S.Frames = X.FrameCount, S.PreSkip = X.OpusPreSkip;
    }
    else
        S.Extension = X.DataType == SoundDataTypes::FLAC_WithHeader ? ".flac" : ".wav";
    return S;
}

namespace
{
    thread_local DrySounds::Sink DryRun;
}

DrySounds::DrySounds(Sink Run) : Previous(std::move(DryRun))
{
    DryRun = std::move(Run);
}

DrySounds::~DrySounds()
{
    DryRun = std::move(Previous);
}

bool WriteSound(const SoundSource& S, const std::filesystem::path& Path, const std::string& Format, std::string* Why)
{
    if (DryRun)
    {
        DryRun(S, Path);
        return true;
    }
    const std::string F = Strings::ToUpper(Format);
    const std::string Ext = Lower(S.Extension);
    const bool IsFile = S.Is == SoundSource::Form::File;
    const auto AsItIs = [&](const uint8_t* Data, size_t Size) {
        if (WriteBytes(Path, Data, Size)) return true;
        if (Why) *Why = "write failed";
        return false;
    };
    if (IsFile && S.Size == 0) return AsItIs(S.Data, 0);   // an empty file stays one
    if (S.Data == nullptr || S.Size == 0)
    {
        if (Why) *Why = "no data";
        return false;
    }
    std::error_code Ec;
    std::filesystem::create_directories(Path.parent_path(), Ec);

    // WMA, with an ffmpeg (WmaSounds.h): decoded to a PCM WAV, then written as one (a .wav under WAV)
    if (IsFile && WmaSounds::IsWma(S.Data, S.Size) && WmaSounds::Available())
    {
        std::vector<uint8_t> Wav;
        bool Retried = false;
        uint32_t Short = 0;
        std::string Failed;
        if (WmaSounds::Decode(S.Data, S.Size, Wav, Retried, Short, Failed))
        {
            Count("sounds_wma_decoded");
            if (Retried) Count("sounds_wma_bitrate_retried");
            if (Short != 0) Count("sounds_wma_decoded_short");
            std::filesystem::path Out = Path;
            if (F != "FLAC" && F != "OPUS") Out.replace_extension(".wav");
            return WriteSound(SoundSource::OfFile(Wav.data(), Wav.size(), ".wav"), Out, Format, Why);
        }
        Count("sounds_wma_decode_failed");
    }
    else if (IsFile && WmaSounds::IsWma(S.Data, S.Size))
        Count(WmaSounds::NoFfmpegCounter);

    if (F != "FLAC" && F != "OPUS")
    {
        // WAV: a file as the game keeps it, unless a FLAC to decode; the samples behind a header
        if (IsFile && (S.KeptUnderWav || Ext != ".flac")) return AsItIs(S.Data, S.Size);
        if (IsFile)
        {
            // the FLAC decoder's own WAV writer (the bytes every game wrote before)
            if (ViaNarrowPath(Path, [&](const std::string& P) {
                    return Sound::ConvertSoundMemory(reinterpret_cast<int8_t*>(const_cast<uint8_t*>(S.Data)), S.Size,
                                                     SoundFormat::FLAC_WithHeader, P, SoundFormat::Standard_WAV);
                }))
                return true;
            if (Why) *Why = "FLAC not decoded";
            return false;
        }
        if (S.Is == SoundSource::Form::Opus)
        {
            const auto Wav = WavOfOpus(S);
            if (Wav == nullptr)
            {
                if (Why) *Why = "Opus not decoded";
                return false;
            }
            return AsItIs(reinterpret_cast<const uint8_t*>(Wav->DataBuffer), Wav->DataSize);
        }
        uint32_t Size = 0;
        const auto Header = Sound::BuildWAVHeader(S.Rate, S.Channels, (uint32_t)S.Size, Size);
        std::vector<uint8_t> Wav(Size + S.Size);
        std::memcpy(Wav.data(), Header.get(), Size);
        std::memcpy(Wav.data() + Size, S.Data, S.Size);
        return AsItIs(Wav.data(), Wav.size());
    }

    if (F == "FLAC" && IsFile && Ext == ".flac") return AsItIs(S.Data, S.Size);
    if (F == "OPUS" && S.Is == SoundSource::Form::Opus)
        return ViaNarrowPath(Path, [&](const std::string& P) { return RemuxOpus(S, P, Why); });

    std::vector<int16_t> Samples;
    uint32_t Channels = 0, Rate = 0;
    if (!Pcm(S, Samples, Channels, Rate))
    {
        if (IsFile)
        {
            // a file no decoder here reads (World at War's and Black Ops' xWMA): kept as it is,
            // under its own extension, rather than left out
            std::filesystem::path Kept = Path;
            Kept.replace_extension(S.Extension);
            if (WriteBytes(Kept, S.Data, S.Size))
            {
                Count("sounds_kept_undecoded");
                return true;
            }
        }
        if (Why) *Why = "not decoded (" + (IsFile ? S.Extension : std::string("samples")) + ")";
        return false;
    }
    if (F == "FLAC")
    {
        if (ViaNarrowPath(Path, [&](const std::string& P) { return Sound::EncodePCMToFLAC(Samples, Channels, Rate, P); }))
        {
            Count("sounds_flac_encoded");
            return true;
        }
        if (Why) *Why = "FLAC not encoded (" + std::to_string(Channels) + " channels)";
        return false;
    }
    if (!OpusRate(Rate))
    {
        if (!Resample48k(Samples, Channels, Rate))
        {
            if (Why) *Why = "not resampled to 48 kHz";
            return false;
        }
        Count("sounds_opus_resampled");
    }
    if (ViaNarrowPath(Path, [&](const std::string& P) { return Sound::EncodePCMToOpus(Samples, Channels, Rate, P); }))
    {
        Count("sounds_opus_encoded");
        return true;
    }
    if (Why) *Why = "Opus not encoded (" + std::to_string(Channels) + " channels)";
    return false;
}

std::map<std::string, size_t> SoundCounters()
{
    std::lock_guard<std::mutex> Lock(CountersLock);
    return Counts;
}
}
