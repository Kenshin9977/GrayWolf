#pragma once

// WMA sounds (World at War's RIFF XWMA files, Black Ops' WMA loaded sounds) decoded to 16-bit PCM
// WAV by an external ffmpeg, like the script decompilers (OatBridge.h): nothing in GrayWolf decodes
// WMA (Windows' own WMA decoder gave wrong samples for these). Without an ffmpeg, a WMA sound is
// kept as the game holds it (ExportWriters::WriteSound).

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace WmaSounds
{
    // The ffmpeg used from now on: Hint if that file exists (--ffmpeg-exe, the ffmpeg_exe setting),
    // else one installed the usual ways (FfmpegFindExe: beside the exe, PATH, WinGet, Chocolatey,
    // Scoop). Before any call, the setting of graywolf.json is read.
    void UseFfmpeg(const std::string& Hint);
    // An ffmpeg found
    bool Available();
    // The run's counter of WMA sounds met without an ffmpeg (kept as the game holds them, or left
    // out), and what the report and the log say of them
    constexpr const char* NoFfmpegCounter = "sounds_wma_no_ffmpeg";
    std::string NoFfmpegMessage(size_t Sounds);

    // A RIFF XWMA, or a RIFF WAVE whose fmt is WMA (0x160 to 0x163)
    bool IsWma(const uint8_t* Data, size_t Size);

    // The sound decoded by ffmpeg to a 16-bit PCM WAV. xWMA leaves out WMA's extra data, which
    // ffmpeg fills from the fmt's average bytes a second (the bit rate) and needs right: a stream
    // it does not decode with its own is tried with the other xWMA rates, then (an xWMA) as a WAVE
    // with WMA's encode options 0x17 instead of ffmpeg's 0x1f (Retried set when one does).
    // Short: the PCM bytes decoded short of the dpds' count (0 when none or all).
    bool Decode(const uint8_t* Data, size_t Size, std::vector<uint8_t>& Wav, bool& Retried, uint32_t& Short, std::string& Why);

    // A RIFF XWMA file of WMAv2 packets (BlockAlign bytes each): fmt 0x161, dpds (Dpds: the decoded
    // byte count at the end of each packet), data
    std::vector<uint8_t> Xwma(uint16_t Channels, uint32_t Rate, uint32_t AvgBytes, uint16_t BlockAlign, const uint32_t* Dpds,
                              size_t DpdsCount, const uint8_t* Packets, size_t Size);
}
