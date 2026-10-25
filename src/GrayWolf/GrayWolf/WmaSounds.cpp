#include "stdafx.h"

#include "WmaSounds.h"

#include "ExportWriters.h"
#include "OatBridge.h"
#include "SettingsManager.h"

#include <algorithm>
#include <cstring>
#include <filesystem>
#include <mutex>

namespace fs = std::filesystem;

namespace WmaSounds
{
namespace
{
    std::mutex Lock;
    bool Resolved = false;
    std::string Exe;

    std::string Resolve()
    {
        std::lock_guard<std::mutex> G(Lock);
        if (!Resolved) Exe = FfmpegFindExe(SettingsManager::GetSetting("ffmpeg_exe", "")), Resolved = true;
        return Exe;
    }

    // The fmt chunk's body (its size in Len), nullptr when there is none
    uint8_t* FmtOf(uint8_t* Data, size_t Size, uint32_t& Len)
    {
        for (size_t At = 12; At + 8 <= Size;)
        {
            std::memcpy(&Len, Data + At + 4, 4);
            if (std::memcmp(Data + At, "fmt ", 4) == 0) return Len >= 16 && Size - (At + 8) >= 16 ? Data + At + 8 : nullptr;
            At += 8 + (size_t)Len + (Len & 1);
        }
        return nullptr;
    }

    // A chunk's body and size, nullptr when there is none
    const uint8_t* ChunkOf(const std::vector<uint8_t>& F, const char* Id, uint32_t& Len)
    {
        for (size_t At = 12; At + 8 <= F.size();)
        {
            std::memcpy(&Len, F.data() + At + 4, 4);
            if (std::memcmp(F.data() + At, Id, 4) == 0) return Len <= F.size() - (At + 8) ? F.data() + At + 8 : nullptr;
            At += 8 + (size_t)Len + (Len & 1);
        }
        return nullptr;
    }

    // ffmpeg's decode of In: true when it exited 0, said no error and, when In has a dpds, decoded
    // the stream to its last packet (its PCM bytes at least dpds' last count less the largest packet's;
    // more are cut to that count)
    bool RunOnce(const std::string& Ffmpeg, const std::vector<uint8_t>& In, std::vector<uint8_t>& Wav, uint32_t& Short, std::string& Why)
    {
        const fs::path Base = ToolScratchPath("gh_wma");
        const fs::path InPath = fs::path(Base).concat(".xwma"), OutPath = fs::path(Base).concat(".wav");
        std::error_code Ec;
        bool Ok = ExportWriters::WriteBytes(InPath, In.data(), In.size());
        std::string Log;
        Ok = Ok && FfmpegDecodeToWav(Ffmpeg, InPath.string(), OutPath.string(), Log);
        if (Ok)
        {
            ExportWriters::ReadBytes(OutPath, Wav);
            uint32_t DataLen = 0, DpdsLen = 0;
            Ok = ChunkOf(Wav, "data", DataLen) != nullptr && DataLen > 0 && Log.find_first_not_of(" \r\n") == std::string::npos;
            if (const uint8_t* Dpds = ChunkOf(In, "dpds", DpdsLen); Ok && Dpds != nullptr && DpdsLen >= 4)
            {
                uint32_t Prev = 0, Last = 0, Step = 0;
                for (uint32_t I = 0; I + 4 <= DpdsLen; I += 4)
                {
                    std::memcpy(&Last, Dpds + I, 4);
                    Step = std::max(Step, Last - std::min(Last, Prev)), Prev = Last;
                }
                Short = Last > DataLen ? Last - DataLen : 0;
                if ((uint64_t)DataLen + Step < Last)
                {
                    Ok = false;
                    Log = "decoded " + std::to_string(DataLen) + " of " + std::to_string(Last) + " bytes";
                }
                else if (DataLen > Last)
                {
                    // the samples past the sound's end (a flushed stream's last frame): cut
                    const size_t Body = (size_t)(ChunkOf(Wav, "data", DataLen) - Wav.data());
                    Wav.erase(Wav.begin() + (std::ptrdiff_t)(Body + Last), Wav.begin() + (std::ptrdiff_t)(Body + DataLen));
                    const uint32_t Riff = (uint32_t)Wav.size() - 8;
                    std::memcpy(Wav.data() + Body - 4, &Last, 4);
                    std::memcpy(Wav.data() + 4, &Riff, 4);
                }
            }
        }
        fs::remove(InPath, Ec);
        fs::remove(OutPath, Ec);
        if (!Ok)
        {
            const size_t Eol = Log.find_first_of("\r\n");
            Why = "ffmpeg: " + (Log.empty() ? std::string("no output") : Log.substr(0, Eol));
        }
        return Ok;
    }

    // In with one more packet (BlockAlign bytes: superframe header {index 0, 1 frame}, bit offset 0,
    // zeros) after its data: ffmpeg's WMA decoder keeps the stream's last frame, whose bits a
    // superframe ends with, until the next packet completes it, and at the end of the stream gives
    // instead the previous frame's overlap alone; this packet makes it decode that frame. Without
    // it the sound's last frame (2048 samples, more than a twentieth of a second) is lost or, when the
    // stream ends 512 samples into a frame (dpds), the sound is decoded short (mp_nuked: 509 of 552
    // sounds end on such a frame, 38 short). Empty when In has no dpds (no count to cut the decode
    // to) or its data is not whole packets.
    std::vector<uint8_t> Flushed(const std::vector<uint8_t>& In)
    {
        uint32_t FmtLen = 0, DataLen = 0, DpdsLen = 0;
        const uint8_t* Fmt = ChunkOf(In, "fmt ", FmtLen);
        const uint8_t* Data = ChunkOf(In, "data", DataLen);
        uint16_t Align = 0;
        if (Fmt != nullptr && FmtLen >= 16) std::memcpy(&Align, Fmt + 12, 2);
        if (ChunkOf(In, "dpds", DpdsLen) == nullptr || DpdsLen < 4 || Data == nullptr || Align < 2 || DataLen == 0 || DataLen % Align != 0) return {};
        const size_t End = (size_t)(Data - In.data()) + DataLen;
        std::vector<uint8_t> Out(In.begin(), In.begin() + (std::ptrdiff_t)End);
        Out.push_back(0x01);
        Out.insert(Out.end(), (size_t)Align - 1, 0);
        // the rest after the data (its pad byte dropped: the data stays even), sizes fixed
        const size_t Next = End + (DataLen & 1);
        if (Next < In.size()) Out.insert(Out.end(), In.begin() + (std::ptrdiff_t)Next, In.end());
        const uint32_t NewLen = DataLen + Align, Riff = (uint32_t)Out.size() - 8;
        std::memcpy(Out.data() + (Data - In.data()) - 4, &NewLen, 4);
        std::memcpy(Out.data() + 4, &Riff, 4);
        return Out;
    }

    // RunOnce of In flushed (Flushed), else of In as it is (a stream ffmpeg fails to flush: Black
    // Ops' whose last packet decodes to nothing)
    bool Run(const std::string& Ffmpeg, const std::vector<uint8_t>& In, std::vector<uint8_t>& Wav, uint32_t& Short, std::string& Why)
    {
        if (const std::vector<uint8_t> F = Flushed(In); !F.empty() && RunOnce(Ffmpeg, F, Wav, Short, Why)) return true;
        return RunOnce(Ffmpeg, In, Wav, Short, Why);
    }

    // The xWMA In as a RIFF WAVE whose fmt carries WMA's extra data (Flags: its encode options) and
    // Avg bytes a second, its dpds kept for Run's check; empty when In has no fmt or data
    std::vector<uint8_t> WaveOf(const std::vector<uint8_t>& In, uint32_t Avg, uint16_t Flags)
    {
        uint32_t FmtLen = 0, DpdsLen = 0, DataLen = 0;
        const uint8_t* Fmt = ChunkOf(In, "fmt ", FmtLen);
        const uint8_t* Dpds = ChunkOf(In, "dpds", DpdsLen);
        const uint8_t* Data = ChunkOf(In, "data", DataLen);
        if (Fmt == nullptr || FmtLen < 16 || Data == nullptr) return {};
        std::vector<uint8_t> Out;
        const auto U16 = [&](uint32_t V) { Out.push_back((uint8_t)V), Out.push_back((uint8_t)(V >> 8)); };
        const auto U32 = [&](uint32_t V) { U16(V & 0xffff), U16(V >> 16); };
        const auto Tag = [&](const char* T4) { Out.insert(Out.end(), T4, T4 + 4); };
        const auto Chunk = [&](const char* T4, const uint8_t* Body, uint32_t Len) {
            Tag(T4), U32(Len), Out.insert(Out.end(), Body, Body + Len);
            if (Len & 1) Out.push_back(0);
        };
        Tag("RIFF"), U32(0), Tag("WAVE");
        // tag, channels, rate as the xWMA's; WMAv2's 10 bytes: samples a block, encode options, super block align
        Tag("fmt "), U32(28);
        Out.insert(Out.end(), Fmt, Fmt + 8), U32(Avg), Out.insert(Out.end(), Fmt + 12, Fmt + 16);
        U16(10), U32(0), U16(Flags), U32(0);
        if (Dpds != nullptr) Chunk("dpds", Dpds, DpdsLen);
        Chunk("data", Data, DataLen);
        const uint32_t Riff = (uint32_t)Out.size() - 8;
        std::memcpy(Out.data() + 4, &Riff, 4);
        return Out;
    }
}

void UseFfmpeg(const std::string& Hint)
{
    std::lock_guard<std::mutex> G(Lock);
    Exe = FfmpegFindExe(Hint.empty() ? SettingsManager::GetSetting("ffmpeg_exe", "") : Hint);
    Resolved = true;
}

bool Available()
{
    return !Resolve().empty();
}

std::string NoFfmpegMessage(size_t Sounds)
{
    return std::to_string(Sounds) +
           " WMA sounds not decoded: no ffmpeg found. Install one (winget install Gyan.FFmpeg, choco install ffmpeg, "
           "scoop install ffmpeg, or ffmpeg.exe beside GrayWolf.exe), or name it: ffmpeg_exe in graywolf.json, --ffmpeg-exe";
}

bool IsWma(const uint8_t* Data, size_t Size)
{
    if (Size < 12 || std::memcmp(Data, "RIFF", 4) != 0) return false;
    if (std::memcmp(Data + 8, "XWMA", 4) == 0) return true;
    if (std::memcmp(Data + 8, "WAVE", 4) != 0) return false;
    uint32_t Len = 0;
    const uint8_t* Fmt = FmtOf(const_cast<uint8_t*>(Data), Size, Len);
    uint16_t Tag = 0;
    if (Fmt) std::memcpy(&Tag, Fmt, 2);
    return Tag >= 0x160 && Tag <= 0x163;
}

bool Decode(const uint8_t* Data, size_t Size, std::vector<uint8_t>& Wav, bool& Retried, uint32_t& Short, std::string& Why)
{
    Retried = false;
    Short = 0;
    const std::string Ffmpeg = Resolve();
    if (Ffmpeg.empty())
    {
        Why = "no ffmpeg";
        return false;
    }
    std::vector<uint8_t> In(Data, Data + Size);
    if (Run(Ffmpeg, In, Wav, Short, Why)) return true;
    uint32_t Len = 0;
    uint8_t* Fmt = FmtOf(In.data(), In.size(), Len);
    if (Fmt == nullptr) return false;
    uint32_t Own = 0;
    std::memcpy(&Own, Fmt + 8, 4);
    // xWMA's average bytes a second (XAudio2's xWMA list)
    const uint32_t Rates[] = { 12000u, 20000u, 24000u, 6000u, 8000u, 4000u, 2500u };
    for (const uint32_t Avg : Rates)
    {
        if (Avg == Own) continue;
        std::memcpy(Fmt + 8, &Avg, 4);
        std::string Again;
        if (Run(Ffmpeg, In, Wav, Short, Again)) return Retried = true;
    }
    // ffmpeg's xWMA reader sets WMA's encode options to 0x1f (five block sizes at 32 kHz). World at
    // War's 32 kHz mono streams are 20 kbps ones (2500 bytes a second, their fmt says 12000) encoded
    // with 0x17 (one block size fewer): passed as a WAVE whose fmt carries them, every rate tried
    // with 2500 first (ber1: 14 of 14 decode at 2500 only, to the dpds' count; no other options do)
    if (std::memcmp(In.data() + 8, "XWMA", 4) != 0) return false;
    for (const uint32_t Avg : { 2500u, 12000u, 20000u, 24000u, 6000u, 8000u, 4000u })
    {
        const std::vector<uint8_t> Wave = WaveOf(In, Avg, 0x17);
        std::string Again;
        if (!Wave.empty() && Run(Ffmpeg, Wave, Wav, Short, Again)) return Retried = true;
    }
    return false;
}

std::vector<uint8_t> Xwma(uint16_t Channels, uint32_t Rate, uint32_t AvgBytes, uint16_t BlockAlign, const uint32_t* Dpds,
                          size_t DpdsCount, const uint8_t* Packets, size_t Size)
{
    std::vector<uint8_t> Out;
    const auto U16 = [&](uint32_t V) { Out.push_back((uint8_t)V), Out.push_back((uint8_t)(V >> 8)); };
    const auto U32 = [&](uint32_t V) { U16(V & 0xffff), U16(V >> 16); };
    const auto Tag = [&](const char* T4) { Out.insert(Out.end(), T4, T4 + 4); };
    Tag("RIFF"), U32(0), Tag("XWMA");
    Tag("fmt "), U32(18);
    U16(0x161), U16(Channels), U32(Rate), U32(AvgBytes), U16(BlockAlign), U16(16), U16(0);
    Tag("dpds"), U32((uint32_t)(DpdsCount * 4));
    for (size_t I = 0; I < DpdsCount; I++) U32(Dpds[I]);
    Tag("data"), U32((uint32_t)Size);
    Out.insert(Out.end(), Packets, Packets + Size);
    if (Size & 1) Out.push_back(0);
    const uint32_t Riff = (uint32_t)Out.size() - 8;
    std::memcpy(Out.data() + 4, &Riff, 4);
    return Out;
}
}
