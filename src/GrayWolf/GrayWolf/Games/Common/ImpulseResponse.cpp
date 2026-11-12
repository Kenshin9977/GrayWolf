#include "stdafx.h"

#include "ImpulseResponse.h"

#include "CoDAssetType.h"
#include "Sound.h"

#include <algorithm>
#include <cmath>
#include <complex>
#include <cstring>
#include <vector>

namespace
{
    // the header: u32 log2 of the first stage's partition, u32 log2 of the second's, u32 rate, u32
    // channels, u16 0, u16 first-stage blocks, u16 0, u16 second-stage partitions
    struct Header
    {
        uint32_t SmallLog, BigLog, Rate, Channels;
        uint16_t Pad0, SmallBlocks, Pad1, BigCount;
    };
    static_assert(sizeof(Header) == 0x18, "the IR header's fields (0x20 bytes with its padding)");

    bool HeaderOf(const uint8_t* Data, size_t Size, Header& H)
    {
        if (Size < 0x20) return false;
        std::memcpy(&H, Data, sizeof(H));
        if (H.SmallLog < 4 || H.BigLog > 16 || H.SmallLog >= H.BigLog || H.Rate < 8000 || H.Rate > 192000 || H.Channels == 0 || H.Channels > 16) return false;
        if (H.SmallBlocks + (uint32_t)H.BigCount == 0) return false;
        // each block: a channel's 2^(big+1) floats
        const uint64_t Floats = (uint64_t)(H.SmallBlocks + (uint64_t)H.BigCount) * H.Channels * (2ull << H.BigLog);
        return 0x20 + Floats * 4 == Size;
    }

    // in-place radix-2 complex FFT (Sign -1 forward, +1 inverse, unscaled)
    void Fft(std::vector<std::complex<double>>& A, int Sign)
    {
        const size_t N = A.size();
        for (size_t I = 1, J = 0; I < N; I++)
        {
            size_t Bit = N >> 1;
            for (; J & Bit; Bit >>= 1) J ^= Bit;
            J ^= Bit;
            if (I < J) std::swap(A[I], A[J]);
        }
        for (size_t Len = 2; Len <= N; Len <<= 1)
        {
            const double Ang = Sign * 2 * 3.14159265358979323846 / (double)Len;
            const std::complex<double> W(std::cos(Ang), std::sin(Ang));
            for (size_t I = 0; I < N; I += Len)
            {
                std::complex<double> Wk(1, 0);
                for (size_t K = 0; K < Len / 2; K++, Wk *= W)
                {
                    const auto U = A[I + K], V = A[I + K + Len / 2] * Wk;
                    A[I + K] = U + V, A[I + K + Len / 2] = U - V;
                }
            }
        }
    }

    // One partition: N floats = N/2 real parts then N/2 imaginary parts of an N-point real FFT
    // (the Nyquist bin's real part in the imaginary slot 0); its first N/2 samples are the IR's,
    // the second half the zero padding
    void Partition(const float* V, size_t N, double* Out)
    {
        const size_t H = N / 2;
        std::vector<std::complex<double>> A(N);
        A[0] = V[0];
        A[H] = V[H];
        for (size_t K = 1; K < H; K++)
        {
            A[K] = std::complex<double>(V[K], V[H + K]);
            A[N - K] = std::conj(A[K]);
        }
        Fft(A, +1);
        for (size_t K = 0; K < H; K++) Out[K] = A[K].real() / (double)N;
    }
}

bool ImpulseResponse::Is(const uint8_t* Data, size_t Size)
{
    Header H;
    return Data != nullptr && HeaderOf(Data, Size, H);
}

std::unique_ptr<XSound> ImpulseResponse::Read(const uint8_t* Data, size_t Size, double* Gain)
{
    Header H;
    if (Data == nullptr || !HeaderOf(Data, Size, H)) return nullptr;
    const size_t Small = (size_t)1 << H.SmallLog, Big = (size_t)1 << H.BigLog, Per = 2 * Big;
    const size_t Frames = (size_t)H.SmallBlocks * Big + (size_t)H.BigCount * Big;
    std::vector<double> Ir((size_t)H.Channels * Frames);
    std::vector<float> Floats((Size - 0x20) / 4);
    std::memcpy(Floats.data(), Data + 0x20, Floats.size() * 4);
    // the first stage's blocks, each holding every channel (small partitions in turn), then each
    // channel's second-stage partitions
    size_t Unit = 0;
    for (uint32_t B = 0; B < H.SmallBlocks; B++)
        for (uint32_t C = 0; C < H.Channels; C++, Unit++)
            for (size_t K = 0; K < Per; K += 2 * Small)
                Partition(Floats.data() + Unit * Per + K, 2 * Small, Ir.data() + C * Frames + B * Big + K / 2);
    for (uint32_t C = 0; C < H.Channels; C++)
        for (uint32_t P = 0; P < H.BigCount; P++, Unit++)
            Partition(Floats.data() + Unit * Per, Per, Ir.data() + C * Frames + ((size_t)H.SmallBlocks + P) * Big);

    double Peak = 0;
    for (double V : Ir) Peak = std::max(Peak, std::abs(V));
    const double Scale = Peak > 0 ? 0.98 * 32767.0 / Peak : 0;
    if (Gain) *Gain = Scale > 0 ? 32767.0 / Scale : 0;
    const size_t PcmSize = Frames * H.Channels * sizeof(int16_t);
    auto Result = std::make_unique<XSound>();
    Result->DataBuffer = new int8_t[PcmSize + (size_t)Sound::GetMaximumWAVHeaderSize()];
    Result->DataType = SoundDataTypes::WAV_WithHeader;
    Result->DataSize = (uint32_t)(PcmSize + Sound::GetMaximumWAVHeaderSize());
    Result->FrameRate = H.Rate, Result->Channels = H.Channels, Result->FrameCount = (uint32_t)Frames;
    Sound::WriteWAVHeaderToStream(Result->DataBuffer, H.Rate, H.Channels, (uint32_t)PcmSize);
    auto* Pcm = reinterpret_cast<int16_t*>(Result->DataBuffer + Sound::GetMaximumWAVHeaderSize());
    for (size_t F = 0; F < Frames; F++)
        for (uint32_t C = 0; C < H.Channels; C++)
            Pcm[F * H.Channels + C] = (int16_t)std::clamp(std::lround(Ir[C * Frames + F] * Scale), -32767L, 32767L);
    return Result;
}
