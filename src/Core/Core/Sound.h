#pragma once

#include <cstdint>
#include <memory>
#include <vector>

#include "BinaryWriter.h"

enum class SoundFormat
{
    Standard_WAV,
    Standard_FLAC,
    Standard_OGG,
    // Opus in an Ogg container, lossy, roughly a fifth of the size of FLAC
    Standard_OPUS,

    // This is used only for InFormat, which will read the WAV format from the header in the stream
    WAV_WithHeader,
    // This is used only for InFormat, which will read the FLAC format from the header in the stream
    FLAC_WithHeader
};

class Sound
{
public:

    static bool ConvertSoundMemory(int8_t* SoundBuffer, uint64_t SoundSize, SoundFormat InFormat, const std::string& OutputFile, SoundFormat OutFormat);

    static void WriteFLACHeaderToStream(int8_t* Buffer, uint32_t FrameRate, uint32_t ChannelsCount, uint32_t FrameCount);

    static void WriteWAVHeaderToStream(int8_t* Buffer, uint32_t FrameRate, uint32_t ChannelsCount, uint32_t TotalSize);

    static const uint32_t GetMaximumFLACHeaderSize();
    static const uint32_t GetMaximumWAVHeaderSize();

private:

    // Transcodes a FLAC file (With header) to a compatible WAV output file
    static bool TranscodeFLACToWav(int8_t* SoundBuffer, uint64_t SoundSize, const std::string& OutputFile);
    // Transcodes a WAV file (With header) to a compatible FLAC output file
    static bool TranscodeWAVToFlac(int8_t* SoundBuffer, uint64_t SoundSize, const std::string& OutputFile);

    static bool TranscodeFLACToOpus(int8_t* SoundBuffer, uint64_t SoundSize, const std::string& OutputFile);
    static bool TranscodeWAVToOpus(int8_t* SoundBuffer, uint64_t SoundSize, const std::string& OutputFile);

public:

    // Encodes interleaved 16bit PCM as Opus inside an Ogg container: 8, 12, 16, 24 or 48 kHz (no
    // resampling here), 1 to 8 channels (more than 2: RFC 7845 channel mapping family 1)
    static bool EncodePCMToOpus(const std::vector<int16_t>& Samples, uint32_t ChannelsCount, uint32_t SampleRate, const std::string& OutputFile);

    // Encodes interleaved 16bit PCM as FLAC (any channel count FLAC takes, 1 to 8)
    static bool EncodePCMToFLAC(const std::vector<int16_t>& Samples, uint32_t ChannelsCount, uint32_t SampleRate, const std::string& OutputFile);

    // Decodes a FLAC file (with its header) to the bytes of a 16bit PCM WAV file (the header as
    // its STREAMINFO says), in memory; Wav holds what was decoded even when it returns false
    static bool DecodeFLACToWav(int8_t* SoundBuffer, uint64_t SoundSize, std::vector<uint8_t>& Wav);

    // Decodes a FLAC file (with its header) to interleaved 16bit PCM
    static bool DecodeFLACToPCM(int8_t* SoundBuffer, uint64_t SoundSize, std::vector<int16_t>& Samples, uint32_t& ChannelsCount, uint32_t& SampleRate);

    // Opus packets as they are, in an Ogg Opus file (RFC 7845), not re-encoded: OpusHead with the
    // channels (1 or 2, mapping family 0), PreSkip and the input rate (informational), granule
    // positions counted at 48 kHz from each packet's own duration; the last page's granule is
    // EndGranule when it is smaller than the packets' total (0: the total). False for a packet that
    // is not one Opus packet.
    static bool WriteOggOpus(const std::vector<std::pair<const uint8_t*, uint32_t>>& Packets, uint32_t ChannelsCount, uint32_t InputRate,
                             uint16_t PreSkip, uint64_t EndGranule, const std::string& OutputFile);


    // Decodes an Ogg Opus file back to PCM purely to prove it is well formed. Returns
    // false if the container, the headers or any packet fails to decode, and reports how
    // many samples per channel came back out.
    static bool ValidateOpusFile(const std::string& InputFile, uint32_t& ChannelsOut, uint64_t& SamplesOut);

    // Decodes an Ogg Opus file to a 48 kHz PCM16 WAV on disk (for playback / re-use), as a player
    // does: OpusHead's pre-skip dropped, the last granule position's end trimming, channel mapping
    // families 0 and 1. Returns false if it cannot be opened or decoded.
    static bool DecodeOpusToWav(const std::string& InputFile, const std::string& OutputWav);

    static std::unique_ptr<int8_t[]> BuildFLACHeader(uint32_t FrameRate, uint32_t ChannelsCount, uint32_t FrameCount, uint32_t& ResultSize);
    static std::unique_ptr<int8_t[]> BuildWAVHeader(uint32_t FrameRate, uint32_t ChannelsCount, uint32_t TotalSize, uint32_t& ResultSize);
};