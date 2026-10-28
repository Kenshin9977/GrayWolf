#include "stdafx.h"
#include "ByteView.h"

#include "IWDSoundDecoder.h"

#include <cstring>

namespace
{
    // Standard MS-ADPCM adaptation table and the two coefficient sets.
    const int AdaptTable[16] = { 230,230,230,230,307,409,512,614,768,614,512,409,307,230,230,230 };
    const int Coef1[7] = { 256,512,0,192,240,460,392 };
    const int Coef2[7] = { 0,-256,0,64,0,-208,-232 };

    inline int16_t ClampS16(int v)
    {
        if (v > 32767) return 32767;
        if (v < -32768) return -32768;
        return (int16_t)v;
    }

    inline int32_t ReadU32(ByteView D, size_t O)
    {
        return (int32_t)(D[O] | (D[O + 1] << 8) | (D[O + 2] << 16) | ((uint32_t)D[O + 3] << 24));
    }
    inline int16_t ReadS16(ByteView D, size_t O)
    {
        return (int16_t)(D[O] | (D[O + 1] << 8));
    }

    // Per-channel MS-ADPCM predictor state.
    struct Ch
    {
        int Predictor = 0, Delta = 0, S1 = 0, S2 = 0, C1 = 0, C2 = 0;
        void Init(ByteView D, size_t O)
        {
            int bp = D[O]; if (bp > 6) bp = 6;
            Delta = ReadS16(D, O + 1);
            S1 = ReadS16(D, O + 3);
            S2 = ReadS16(D, O + 5);
            C1 = Coef1[bp]; C2 = Coef2[bp];
        }
        int16_t Step(int nibble)
        {
            const int n = nibble >= 8 ? nibble - 16 : nibble;
            int pred = (S1 * C1 + S2 * C2) >> 8;
            pred += n * Delta;
            const int16_t s = ClampS16(pred);
            Delta = (AdaptTable[nibble] * Delta) >> 8;
            if (Delta < 16) Delta = 16;
            S2 = S1; S1 = s;
            return s;
        }
    };

    void AppendWavHeader(std::vector<uint8_t>& Out, uint32_t DataBytes, uint16_t Channels, uint32_t Rate)
    {
        auto u32 = [&](uint32_t v) { Out.push_back(v & 0xFF); Out.push_back((v >> 8) & 0xFF); Out.push_back((v >> 16) & 0xFF); Out.push_back((v >> 24) & 0xFF); };
        auto u16 = [&](uint16_t v) { Out.push_back(v & 0xFF); Out.push_back((v >> 8) & 0xFF); };
        auto tag = [&](const char* t) { Out.insert(Out.end(), t, t + 4); };
        const uint16_t BlockAlign = (uint16_t)(Channels * 2);
        tag("RIFF"); u32(36 + DataBytes); tag("WAVE");
        tag("fmt "); u32(16); u16(1); u16(Channels); u32(Rate); u32(Rate * BlockAlign); u16(BlockAlign); u16(16);
        tag("data"); u32(DataBytes);
    }
}

bool DecodeIWDSound(ByteView Raw, std::vector<uint8_t>& OutWav)
{
    if (Raw.size() < 32) return false;

    const uint32_t Rate = (uint32_t)ReadU32(Raw, 8);
    const uint32_t Channels = (uint32_t)ReadU32(Raw, 12);
    const uint32_t DataOffset = (uint32_t)ReadU32(Raw, 16);
    const uint32_t BlockSize = (uint32_t)ReadU32(Raw, 20);

    // Sanity gate on the reverse-engineered fields.
    if (Rate < 8000 || Rate > 96000) return false;
    if (Channels != 1 && Channels != 2) return false;
    if (DataOffset < 16 || DataOffset >= Raw.size()) return false;
    if (BlockSize < 64 || BlockSize > 1u * 1024 * 1024) return false;

    const uint32_t FrameCount = (uint32_t)ReadU32(Raw, 4);
    const size_t PreamblePerBlock = (size_t)7 * Channels;

    // The header's BlockSize (@20, 67072) is the streaming CHUNK, not one ADPCM block: it holds many
    // standard 512-sample MS-ADPCM blocks (128 stereo / 256 mono blocks per chunk). A 512-sample
    // block is 262 bytes/channel (7 preamble + 255 nibble bytes -> 510 nibble frames + 2 preamble
    // samples). The predictor MUST reset from each real block's preamble; running it across the whole
    // 67072 chunk lets Delta drift -> muffled + crackle. (BlockSize is still gated for sanity above.)
    (void)BlockSize;
    const size_t RealBlock = (size_t)262 * Channels;

    std::vector<int16_t> Samples; // interleaved
    Samples.reserve((Raw.size() - DataOffset) * 2);

    // Every block, including a final partial one (short sounds fit in less than a full block).
    // The exact length is trimmed afterwards from the header's frame count.
    for (size_t b = DataOffset; b + PreamblePerBlock <= Raw.size(); b += RealBlock)
    {
        const size_t End = (b + RealBlock <= Raw.size()) ? (b + RealBlock) : Raw.size();

        if (Channels == 2)
        {
            // Canonical MS-ADPCM stereo preamble is PLANAR, not two contiguous 7-byte mono preambles:
            // bp[L],bp[R], idelta[L],idelta[R], sample1[L],sample1[R], sample2[L],sample2[R] (14 bytes).
            // Reading it contiguously mis-parses idelta (e.g. 6657 instead of 26) -> noise.
            Ch L, R;
            int bpL = Raw[b]; if (bpL > 6) bpL = 6;
            int bpR = Raw[b + 1]; if (bpR > 6) bpR = 6;
            L.C1 = Coef1[bpL]; L.C2 = Coef2[bpL];
            R.C1 = Coef1[bpR]; R.C2 = Coef2[bpR];
            L.Delta = ReadS16(Raw, b + 2);  R.Delta = ReadS16(Raw, b + 4);
            L.S1 = ReadS16(Raw, b + 6);     R.S1 = ReadS16(Raw, b + 8);
            L.S2 = ReadS16(Raw, b + 10);    R.S2 = ReadS16(Raw, b + 12);
            // Canonical MS-ADPCM emits sample2 (the older sample) first, then sample1 - matching
            // ffmpeg/NAudio.
            Samples.push_back((int16_t)L.S2); Samples.push_back((int16_t)R.S2);
            Samples.push_back((int16_t)L.S1); Samples.push_back((int16_t)R.S1);
            for (size_t i = b + 14; i < End; i++)
            {
                Samples.push_back(L.Step(Raw[i] >> 4));
                Samples.push_back(R.Step(Raw[i] & 0xF));
            }
        }
        else
        {
            Ch M;
            M.Init(Raw, b);
            Samples.push_back((int16_t)M.S2);
            Samples.push_back((int16_t)M.S1);
            for (size_t i = b + 7; i < End; i++)
            {
                Samples.push_back(M.Step(Raw[i] >> 4));
                Samples.push_back(M.Step(Raw[i] & 0xF));
            }
        }
    }

    if (Samples.empty()) return false;

    // Trim to the exact length: the header's frame count drops trailing block padding (and, for
    // a short sound, the unused tail of its single partial block).
    const size_t TargetSamples = (size_t)FrameCount * Channels;
    if (FrameCount > 0 && TargetSamples <= Samples.size())
        Samples.resize(TargetSamples);

    const uint32_t DataBytes = (uint32_t)(Samples.size() * sizeof(int16_t));
    OutWav.clear();
    OutWav.reserve(44 + DataBytes);
    AppendWavHeader(OutWav, DataBytes, (uint16_t)Channels, Rate);
    const size_t Base = OutWav.size();
    OutWav.resize(Base + DataBytes);
    std::memcpy(OutWav.data() + Base, Samples.data(), DataBytes);
    return true;
}
