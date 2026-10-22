#pragma once

#include <string>
#include <cstdint>
#include <memory>

#include "WraithNameIndex.h"

class CoDSound_t;
struct XSound;
class BinaryReader;
struct SABFileHeader;

class SABSupport
{
public:

    static bool ParseSAB(const std::string& FilePath);

    static std::unique_ptr<XSound> LoadSound(const CoDSound_t* SoundAsset);
    // A bank entry's Opus packets (OpusPackets), the game's pre-skip with them
    static std::unique_ptr<XSound> LoadOpusSound(const CoDSound_t* SoundAsset, int32_t PreSkip);

    // The decoded samples before a sound's first, at 48 kHz: the encoder's look-ahead (312) and,
    // in the games that add one, a 20 ms packet ahead of the sound (silence, or a loop's own end).
    // Measured on each game's decodes: abrupt starts and loop seams land there, the frame count
    // ends where the sound does (graywolf-tools docs/games/IW8.md, "Opus pre-skip").
    // OpusPreSkipPacket: Modern Warfare (2019) banks, Cold War, Modern Warfare III, Black Ops 6's
    // campaign; OpusPreSkipLookAhead: Black Ops 6 multiplayer, Black Ops 7; OpusPreSkipUnmeasured:
    // a game not installed here (Modern Warfare II, Vanguard), every packet's samples kept.
    static constexpr int32_t OpusPreSkipPacket = 960, OpusPreSkipLookAhead = 312, OpusPreSkipUnmeasured = -1;

    // The game's Opus packets ({u16 size, packet}, from Offset, Size bytes of them; an empty one is
    // 20 ms concealed) decoded to a 16-bit WAV at FrameRate: FrameCount samples after PreSkip (48 kHz
    // samples; OpusPreSkipUnmeasured or no FrameCount: every packet's samples)
    static std::unique_ptr<XSound> DecodeOpusInterleaved(const uint8_t* OpusBuffer, size_t OpusBufferSize, size_t OpusDataOffset, uint32_t FrameRate,
                                                         uint32_t Channels, uint32_t FrameCount, int32_t PreSkip);
    // The same packets kept as they are (XSound::DataType Opus_Interleaved), for the sound writer to
    // remux or decode (ExportWriters::WriteSound): Available, the bytes Buffer holds (the last packet
    // may end past Offset + Size); PreSkip, the game's (above)
    static std::unique_ptr<XSound> OpusPackets(const uint8_t* Buffer, size_t Available, size_t Offset, size_t Size, uint32_t FrameRate,
                                               uint32_t Channels, uint32_t FrameCount, int32_t PreSkip);

    // Decrypts an encrypted SAB to a side file and reopens Reader on it. Public so a caller doing its
    // own bank scan (Bo3ZoneSounds.cpp: ParseSAB's global CoDAssets state is unsafe on worker threads)
    // reuses the zm_genesis Salsa20 decrypt path.
    static void HandleSABEncryption(BinaryReader& Reader, const std::string& FilePath);

private:

    // Handle reading entries for a v4 SAB file (0x4)
    static void HandleSABv4(BinaryReader& Reader, const SABFileHeader& Header, const std::vector<std::string>& NameList, const WraithNameIndex& NameIndex);
    // Handle reading entries for a v10 SAB file (0xA)
    static void HandleSABv10(BinaryReader& Reader, const SABFileHeader& Header, const std::vector<std::string>& NameList, const WraithNameIndex& NameIndex);
    // Handle reading entries for a v14 SAB file (0xE)
    static void HandleSABv14(BinaryReader& Reader, const SABFileHeader& Header, const std::vector<std::string>& NameList, const WraithNameIndex& NameIndex);
    // Handle reading entries for a v15 SAB file (0xF)
    static void HandleSABv15(BinaryReader& Reader, const SABFileHeader& Header, const std::vector<std::string>& NameList, const WraithNameIndex& NameIndex);
    // Handle reading entries for a v17 SAB file (0x11)
    static void HandleSABv17(BinaryReader& Reader, const SABFileHeader& Header, const std::vector<std::string>& NameList, const WraithNameIndex& NameIndex);
    // Handle reading entries for a v21 SAB file (0x15)
    static void HandleSABv21(BinaryReader& Reader, const SABFileHeader& Header, const std::vector<std::string>& NameList, const WraithNameIndex& NameIndex, const std::string FileName);
    // Handle reading entries for a v21 SAB file (0x15) (Modern Warfare 5)
    static void HandleSABv21v2(BinaryReader& Reader, const SABFileHeader& Header, const std::vector<std::string>& NameList, const WraithNameIndex& NameIndex, const std::string FileName);
};