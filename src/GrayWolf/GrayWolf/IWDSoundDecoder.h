#pragma once

#include "ByteView.h"

#include <cstdint>
#include <vector>

// Decodes a Black Ops 1 IWD "wav" - a custom container that is really block MS-ADPCM - into a
// standard PCM16 RIFF WAV in memory. Format:
//   header 2096 bytes: {u32 fmt@0, u32 sampleCount@4, u32 sampleRate@8, u32 channels@12,
//                       u32 dataOffset@16 (=2096), u32 blockSize@20 (=67072), ...}
//   then MS-ADPCM blocks of blockSize bytes; per-channel 7-byte block preamble
//   (bpredictor + idelta + sample1 + sample2), interleaved 4-bit nibbles (high=L, low=R).
//
// Returns false if the buffer is not a recognisable IWD sound. On success OutWav holds a
// complete little-endian RIFF/WAVE PCM16 file.
bool DecodeIWDSound(ByteView Raw, std::vector<uint8_t>& OutWav);

// What DecodeIWDSound makes of a file of RawSize bytes whose first bytes are Head (its 24-byte
// header suffices), without decoding: the 44-byte header of the WAV it writes (WavHeader) and the
// bytes of its samples. False exactly where DecodeIWDSound would give false.
bool IWDSoundDecodedHeader(ByteView Head, size_t RawSize, std::vector<uint8_t>& WavHeader, uint32_t& DataBytes);
