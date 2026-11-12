#pragma once

// Reverb impulse responses kept as the partitioned convolution's spectra (MWIII / MW4 sndasset
// format 4): read back to the impulse response's samples. Layout: graywolf-tools docs/games/MW4.md,
// "Codec 4".

#include <cstddef>
#include <cstdint>
#include <memory>

struct XSound;

namespace ImpulseResponse
{
    // Data (the object from its 32-byte IR header) holds an impulse response this reads
    bool Is(const uint8_t* Data, size_t Size);
    // its samples, a 16-bit WAV (every channel, the peak at 0.98 of full scale; Gain the factor
    // from the WAV's samples / 32767 back to the game's values); nullptr when it is none
    std::unique_ptr<XSound> Read(const uint8_t* Data, size_t Size, double* Gain = nullptr);
}
