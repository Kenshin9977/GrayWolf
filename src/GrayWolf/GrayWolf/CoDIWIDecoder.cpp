#include "stdafx.h"

#include "CoDIWIDecoder.h"

#include <algorithm>
#include <array>
#include <cstring>
#include <limits>
#include <vector>

// The wavelet codec as Infinity Ward's decoder implements it (three 12-bit Huffman lookups, a
// Haar-like 2x2 reconstruction from the next smaller mip), first written down by OpenAssetTools
// (IwiWaveletDecoder.cpp, its pull request #980), ported here.
namespace
{
    constexpr unsigned LookupBits = 12;
    constexpr int Escape = std::numeric_limits<int16_t>::min();

    struct Codeword
    {
        uint16_t Code;
        uint8_t Bits;
        int16_t Value;
    };
    struct Decode
    {
        int16_t Value = 0;
        uint8_t Bits = 0;
    };
    using Lookup = std::array<Decode, 1u << LookupBits>;

    // the unique LSB-first codewords of the game's three 4096-entry lookups
    const Codeword BlueCodewords[] = {
        {0x001, 3, 0}, {0x004, 5, 4}, {0x005, 5, 2}, {0x007, 5, 1}, {0x00A, 5, 3}, {0x014, 5, -4}, {0x015, 5, -2}, {0x017, 5, -1},
        {0x01A, 5, -3}, {0x000, 6, 12}, {0x002, 6, 10}, {0x003, 6, 7}, {0x006, 6, 9}, {0x00B, 6, 6}, {0x018, 6, 11}, {0x01E, 6, 8},
        {0x01F, 6, 5}, {0x020, 6, -12}, {0x022, 6, -10}, {0x023, 6, -7}, {0x026, 6, -9}, {0x02B, 6, -6}, {0x038, 6, -11},
        {0x03C, 6, Escape}, {0x03E, 6, -8}, {0x03F, 6, -5}, {0x00F, 7, 13}, {0x012, 7, 19}, {0x016, 7, 18}, {0x01B, 7, 14},
        {0x028, 7, 21}, {0x02C, 7, 20}, {0x02D, 7, 16}, {0x02E, 7, 17}, {0x030, 7, 22}, {0x03D, 7, 15}, {0x04F, 7, -13},
        {0x052, 7, -19}, {0x056, 7, -18}, {0x05B, 7, -14}, {0x068, 7, -21}, {0x06C, 7, -20}, {0x06D, 7, -16}, {0x06E, 7, -17},
        {0x070, 7, -22}, {0x07D, 7, -15}, {0x008, 8, 34}, {0x00D, 8, 28}, {0x00E, 8, 29}, {0x013, 8, 26}, {0x01D, 8, 27},
        {0x02F, 8, 23}, {0x033, 8, 25}, {0x03B, 8, 24}, {0x048, 8, 33}, {0x04C, 8, 32}, {0x05C, 8, 31}, {0x072, 8, 30},
        {0x088, 8, -34}, {0x08D, 8, -28}, {0x08E, 8, -29}, {0x093, 8, -26}, {0x09D, 8, -27}, {0x0AF, 8, -23}, {0x0B3, 8, -25},
        {0x0BB, 8, -24}, {0x0C8, 8, -33}, {0x0CC, 8, -32}, {0x0DC, 8, -31}, {0x0F2, 8, -30}, {0x00C, 9, 47}, {0x01C, 9, 46},
        {0x032, 9, 45}, {0x036, 9, 44}, {0x050, 9, 48}, {0x076, 9, 43}, {0x07B, 9, 37}, {0x090, 9, 49}, {0x0CD, 9, 40},
        {0x0CE, 9, 41}, {0x0D3, 9, 38}, {0x0DD, 9, 39}, {0x0EF, 9, 35}, {0x0F6, 9, 42}, {0x0FB, 9, 36}, {0x10C, 9, -47},
        {0x11C, 9, -46}, {0x132, 9, -45}, {0x136, 9, -44}, {0x150, 9, -48}, {0x176, 9, -43}, {0x17B, 9, -37}, {0x190, 9, -49},
        {0x1CD, 9, -40}, {0x1CE, 9, -41}, {0x1D3, 9, -38}, {0x1DD, 9, -39}, {0x1EF, 9, -35}, {0x1F6, 9, -42}, {0x1FB, 9, -36},
        {0x010, 10, 65}, {0x04D, 10, 56}, {0x04E, 10, 57}, {0x05D, 10, 55}, {0x08C, 10, 62}, {0x09C, 10, 61}, {0x110, 10, 64},
        {0x153, 10, 53}, {0x15D, 10, 54}, {0x16F, 10, 50}, {0x173, 10, 52}, {0x19C, 10, 60}, {0x1B2, 10, 59}, {0x1B6, 10, 58},
        {0x1D0, 10, 63}, {0x1F3, 10, 51}, {0x210, 10, -65}, {0x24D, 10, -56}, {0x24E, 10, -57}, {0x25D, 10, -55},
        {0x28C, 10, -62}, {0x29C, 10, -61}, {0x310, 10, -64}, {0x353, 10, -53}, {0x35D, 10, -54}, {0x36F, 10, -50},
        {0x373, 10, -52}, {0x39C, 10, -60}, {0x3B2, 10, -59}, {0x3B6, 10, -58}, {0x3D0, 10, -63}, {0x3F3, 10, -51},
        {0x053, 11, 70}, {0x06F, 11, 66}, {0x073, 11, 69}, {0x0B2, 11, 77}, {0x0B6, 11, 75}, {0x0D0, 11, 81}, {0x14E, 11, 73},
        {0x18C, 11, 79}, {0x273, 11, 68}, {0x2B2, 11, 76}, {0x2B6, 11, 74}, {0x2D0, 11, 80}, {0x2F3, 11, 67}, {0x34D, 11, 71},
        {0x34E, 11, 72}, {0x38C, 11, 78}, {0x453, 11, -70}, {0x46F, 11, -66}, {0x473, 11, -69}, {0x4B2, 11, -77},
        {0x4B6, 11, -75}, {0x4D0, 11, -81}, {0x54E, 11, -73}, {0x58C, 11, -79}, {0x673, 11, -68}, {0x6B2, 11, -76},
        {0x6B6, 11, -74}, {0x6D0, 11, -80}, {0x6F3, 11, -67}, {0x74D, 11, -71}, {0x74E, 11, -72}, {0x78C, 11, -78},
        {0x0F3, 12, 85}, {0x14D, 12, 89}, {0x253, 12, 87}, {0x26F, 12, 83}, {0x4F3, 12, 84}, {0x54D, 12, 88}, {0x653, 12, 86},
        {0x66F, 12, 82}, {0x8F3, 12, -85}, {0x94D, 12, -89}, {0xA53, 12, -87}, {0xA6F, 12, -83}, {0xCF3, 12, -84},
        {0xD4D, 12, -88}, {0xE53, 12, -86}, {0xE6F, 12, -82},
    };
    const Codeword RedGreenCodewords[] = {
        {0x003, 2, 0}, {0x002, 3, 1}, {0x006, 3, -1}, {0x001, 4, 2}, {0x009, 4, -2}, {0x004, 5, 4}, {0x00D, 5, 3}, {0x014, 5, -4},
        {0x01D, 5, -3}, {0x00C, 6, 6}, {0x010, 6, 7}, {0x015, 6, 5}, {0x02C, 6, -6}, {0x030, 6, -7}, {0x035, 6, -5}, {0x018, 7, 10},
        {0x01C, 7, 9}, {0x020, 7, 11}, {0x025, 7, 8}, {0x058, 7, -10}, {0x05C, 7, -9}, {0x060, 7, -11}, {0x065, 7, -8},
        {0x068, 7, Escape}, {0x038, 8, 14}, {0x040, 8, 16}, {0x045, 8, 12}, {0x048, 8, 15}, {0x07C, 8, 13}, {0x0B8, 8, -14},
        {0x0C0, 8, -16}, {0x0C5, 8, -12}, {0x0C8, 8, -15}, {0x0FC, 8, -13}, {0x080, 9, 22}, {0x085, 9, 17}, {0x088, 9, 21},
        {0x0A8, 9, 20}, {0x0BC, 9, 18}, {0x0F8, 9, 19}, {0x180, 9, -22}, {0x185, 9, -17}, {0x188, 9, -21}, {0x1A8, 9, -20},
        {0x1BC, 9, -18}, {0x1F8, 9, -19}, {0x000, 10, 30}, {0x03C, 10, 25}, {0x078, 10, 26}, {0x100, 10, 29}, {0x105, 10, 23},
        {0x108, 10, 28}, {0x128, 10, 27}, {0x13C, 10, 24}, {0x200, 10, -30}, {0x23C, 10, -25}, {0x278, 10, -26},
        {0x300, 10, -29}, {0x305, 10, -23}, {0x308, 10, -28}, {0x328, 10, -27}, {0x33C, 10, -24}, {0x005, 11, 31},
        {0x008, 11, 37}, {0x028, 11, 35}, {0x178, 11, 33}, {0x208, 11, 36}, {0x228, 11, 34}, {0x378, 11, 32}, {0x405, 11, -31},
        {0x408, 11, -37}, {0x428, 11, -35}, {0x578, 11, -33}, {0x608, 11, -36}, {0x628, 11, -34}, {0x778, 11, -32},
        {0x205, 12, 39}, {0x605, 12, 38}, {0xA05, 12, -39}, {0xE05, 12, -38},
    };
    const Codeword AlphaCodewords[] = {
        {0x001, 1, 0}, {0x000, 4, Escape}, {0x002, 4, 1}, {0x00A, 4, -1}, {0x00C, 5, 2}, {0x01C, 5, -2}, {0x016, 6, 3},
        {0x018, 6, 4}, {0x036, 6, -3}, {0x038, 6, -4}, {0x004, 7, 7}, {0x02E, 7, 5}, {0x034, 7, 6}, {0x044, 7, -7},
        {0x06E, 7, -5}, {0x074, 7, -6}, {0x006, 8, 11}, {0x008, 8, 14}, {0x014, 8, 12}, {0x01E, 8, 9}, {0x048, 8, 15},
        {0x066, 8, 10}, {0x068, 8, 13}, {0x07E, 8, 8}, {0x086, 8, -11}, {0x088, 8, -14}, {0x094, 8, -12}, {0x09E, 8, -9},
        {0x0C8, 8, -15}, {0x0E6, 8, -10}, {0x0E8, 8, -13}, {0x0FE, 8, -8}, {0x028, 9, 23}, {0x046, 9, 19}, {0x054, 9, 20},
        {0x08E, 9, 17}, {0x0A4, 9, 22}, {0x0A8, 9, 24}, {0x0C6, 9, 18}, {0x0DE, 9, 16}, {0x0E4, 9, 21}, {0x128, 9, -23},
        {0x146, 9, -19}, {0x154, 9, -20}, {0x18E, 9, -17}, {0x1A4, 9, -22}, {0x1A8, 9, -24}, {0x1C6, 9, -18}, {0x1DE, 9, -16},
        {0x1E4, 9, -21}, {0x00E, 10, 29}, {0x024, 10, 37}, {0x026, 10, 31}, {0x04E, 10, 28}, {0x064, 10, 35}, {0x0BE, 10, 32},
        {0x0D4, 10, 33}, {0x124, 10, 36}, {0x126, 10, 30}, {0x13E, 10, 25}, {0x15E, 10, 26}, {0x164, 10, 34}, {0x1A6, 10, 127},
        {0x1CE, 10, 27}, {0x1D4, 10, 128}, {0x20E, 10, -29}, {0x224, 10, -37}, {0x226, 10, -31}, {0x24E, 10, -28},
        {0x264, 10, -35}, {0x2BE, 10, -32}, {0x2D4, 10, -33}, {0x324, 10, -36}, {0x326, 10, -30}, {0x33E, 10, -25},
        {0x35E, 10, -26}, {0x364, 10, -34}, {0x3A6, 10, -127}, {0x3CE, 10, -27}, {0x3D4, 10, -128}, {0x03E, 11, 41},
        {0x05E, 11, 43}, {0x0A6, 11, 50}, {0x0CE, 11, 48}, {0x10E, 11, 49}, {0x14E, 11, 64}, {0x1BE, 11, 39}, {0x23E, 11, 40},
        {0x25E, 11, 42}, {0x2A6, 11, 47}, {0x2CE, 11, 44}, {0x30E, 11, 46}, {0x34E, 11, 45}, {0x3BE, 11, 38}, {0x43E, 11, -41},
        {0x45E, 11, -43}, {0x4A6, 11, -50}, {0x4CE, 11, -48}, {0x50E, 11, -49}, {0x54E, 11, -64}, {0x5BE, 11, -39},
        {0x63E, 11, -40}, {0x65E, 11, -42}, {0x6A6, 11, -47}, {0x6CE, 11, -44}, {0x70E, 11, -46}, {0x74E, 11, -45},
        {0x7BE, 11, -38},
    };

    // A codeword of n bits fills every slot whose low n bits are it; complete and without overlap
    // for these three tables (checked once at first use)
    template <size_t N>
    Lookup Build(const Codeword (&Words)[N], bool& Ok)
    {
        Lookup L{};
        for (const auto& W : Words)
            for (unsigned I = W.Code; I < L.size(); I += 1u << W.Bits)
            {
                if (L[I].Bits) Ok = false;
                L[I] = { W.Value, W.Bits };
            }
        for (const auto& E : L)
            if (!E.Bits) Ok = false;
        return L;
    }
    struct Tables
    {
        bool Ok = true;
        Lookup Blue = Build(BlueCodewords, Ok), RedGreen = Build(RedGreenCodewords, Ok), Alpha = Build(AlphaCodewords, Ok);
    };
    const Tables& Huffman()
    {
        static const Tables T;
        return T;
    }

    // raw bytes first (the smallest mips), then LSB-first bits from where the bytes stopped
    class Bits
    {
    public:
        Bits(const uint8_t* Data, size_t Size) : P(Data), N(Size) {}
        bool Byte(uint8_t& V)
        {
            if (Started || At >= N) return false;
            V = P[At++];
            return true;
        }
        unsigned Peek(unsigned Count)
        {
            Start();
            unsigned R = 0;
            for (unsigned I = 0; I < Count && Bit + I < N * 8; I++)
                R |= ((P[(Bit + I) / 8] >> ((Bit + I) % 8)) & 1u) << I;
            return R;
        }
        bool Read(unsigned Count, unsigned& V)
        {
            Start();
            if (Bit + Count > N * 8) return false;
            V = Peek(Count);
            Bit += Count;
            return true;
        }

    private:
        void Start()
        {
            if (!Started) Bit = At * 8, Started = true;
        }
        const uint8_t* P;
        size_t N, At = 0, Bit = 0;
        bool Started = false;
    };

    bool Value(const Lookup& L, unsigned EscapeBits, int EscapeBias, Bits& B, int& V)
    {
        const Decode E = L[B.Peek(LookupBits)];
        unsigned Ignored;
        if (!E.Bits || !B.Read(E.Bits, Ignored)) return false;
        V = E.Value;
        if (E.Value == Escape)
        {
            unsigned Raw;
            if (!B.Read(EscapeBits, Raw)) return false;
            V = (int)Raw - EscapeBias;
        }
        return true;
    }
    bool Coefficients(const Lookup& L, unsigned EscapeBits, int EscapeBias, Bits& B, std::array<int, 3>& C)
    {
        for (auto& V : C)
            if (!Value(L, EscapeBits, EscapeBias, B, V)) return false;
        return true;
    }
    void Reconstruct(const uint8_t* Src, uint8_t* Dst, unsigned Bpp, unsigned Stride, unsigned Ch, unsigned Parity, const std::array<int, 3>& C)
    {
        const int Base = 2 * Src[Ch], H = C[0], Vt = C[1], D = C[2];
        Dst[Ch] = (uint8_t)(Parity + ((D + Vt + H + Base) >> 1));
        Dst[Bpp + Ch] = (uint8_t)((H + Base - D - Vt) >> 1);
        Dst[Stride + Ch] = (uint8_t)((Vt - D + Base - H) >> 1);
        Dst[Stride + Bpp + Ch] = (uint8_t)((Base - H - Vt + D) >> 1);
    }

    // one mip of one face from the next smaller one (or raw bytes for a 1-pixel-wide or -high one)
    bool WaveletLevel(const uint8_t* Src, uint8_t* Dst, unsigned W, unsigned H, unsigned Channels, unsigned Bpp, Bits& B)
    {
        const Tables& T = Huffman();
        if (W <= 1 || H <= 1)
        {
            for (unsigned P = 0; P < W * H; P++, Dst += Bpp)
            {
                for (unsigned C = 0; C < Channels; C++)
                    if (!B.Byte(Dst[C])) return false;
                for (unsigned C = Channels; C < Bpp; C++) Dst[C] = 0xFF;
            }
            return true;
        }
        if (!Src) return false;
        unsigned Delta;
        if (!B.Read(1, Delta)) return false;
        std::vector<uint8_t> Adjusted;
        if (Delta)
        {
            Adjusted.assign(Src, Src + W * H / 4 * Bpp);
            uint8_t* P = Adjusted.data();
            for (unsigned I = 0; I < W * H / 4; I++, P += Bpp)
                for (unsigned C = 0; C < Channels; C++)
                {
                    int V;
                    if (!Value(T.Alpha, 9, 255, B, V)) return false;
                    P[C] = (uint8_t)(P[C] + V);
                }
            Src = Adjusted.data();
        }
        const unsigned Stride = W * Bpp;
        for (unsigned Y = 0; Y < H; Y += 2)
            for (unsigned X = 0; X < W; X += 2)
            {
                const uint8_t* S = Src + ((Y / 2) * (W / 2) + X / 2) * Bpp;
                uint8_t* D = Dst + (Y * W + X) * Bpp;
                std::array<int, 3> Blue{};
                unsigned Parity;
                if (Channels != 1)
                {
                    if (!B.Read(1, Parity) || !Coefficients(T.Blue, 9, 0xFF, B, Blue)) return false;
                    Reconstruct(S, D, Bpp, Stride, 0, Parity, Blue);
                    if (Channels >= 3)
                        for (unsigned C = 1; C <= 2; C++)
                        {
                            std::array<int, 3> K{};
                            if (!B.Read(1, Parity) || !Coefficients(T.RedGreen, 10, 0x1FE, B, K)) return false;
                            for (size_t I = 0; I < K.size(); I++) K[I] += Blue[I];
                            Reconstruct(S, D, Bpp, Stride, C, Parity, K);
                        }
                }
                if (Channels == 3)
                    D[3] = D[Bpp + 3] = D[Stride + 3] = D[Stride + Bpp + 3] = 0xFF;
                else
                {
                    std::array<int, 3> K{};
                    if (!B.Read(1, Parity) || !Coefficients(T.Alpha, 9, 0xFF, B, K)) return false;
                    Reconstruct(S, D, Bpp, Stride, Channels - 1, Parity, K);
                }
            }
        return true;
    }

    // The DDS pixel layouts the IWI formats come out as.
    enum class Out { BGRA, BGRX, LA, L, A, BC1, BC2, BC3, BC5 };
    struct Layout
    {
        Out Format;
        unsigned Bits;    // per pixel, or per 4x4 block
        bool Block;
    };
    Layout LayoutOf(Out F)
    {
        switch (F)
        {
        case Out::BGRA: case Out::BGRX: return { F, 32, false };
        case Out::LA: return { F, 16, false };
        case Out::L: case Out::A: return { F, 8, false };
        case Out::BC1: return { F, 64, true };
        default: return { F, 128, true };
        }
    }
    size_t MipSize(const Layout& L, unsigned W, unsigned H, unsigned D, unsigned Mip)
    {
        const unsigned Mw = std::max(1u, W >> Mip), Mh = std::max(1u, H >> Mip), Md = std::max(1u, D >> Mip);
        if (L.Block) return (size_t)((Mw + 3) / 4) * ((Mh + 3) / 4) * Md * L.Bits / 8;
        return (size_t)Mw * Mh * Md * L.Bits / 8;
    }

    void Put32(std::string& S, uint32_t V) { S.append(reinterpret_cast<const char*>(&V), 4); }

    std::string Header(const Layout& L, unsigned W, unsigned H, unsigned Depth, unsigned Mips, bool HasMips, bool Cube)
    {
        constexpr uint32_t Caps = 0x1, Height = 0x2, Width = 0x4, Pitch = 0x8, PixelFormat = 0x1000, MipCount = 0x20000,
                           LinearSize = 0x80000, DepthFlag = 0x800000;
        uint32_t Flags = Caps | Height | Width | PixelFormat | (HasMips ? MipCount : 0) | (L.Block ? LinearSize : Pitch) | (Depth > 1 ? DepthFlag : 0);
        const uint32_t PitchOrSize = L.Block ? ((W + 3) / 4) * L.Bits / 8 : W * L.Bits / 8;
        std::string S = "DDS ";
        Put32(S, 124);
        Put32(S, Flags);
        Put32(S, H);
        Put32(S, W);
        Put32(S, PitchOrSize);
        Put32(S, Depth);
        Put32(S, HasMips ? Mips : 1);
        for (int I = 0; I < 11; I++) Put32(S, 0);
        // pixel format: size, flags, fourcc, bit count, r g b a masks
        uint32_t PfFlags = 0, FourCc = 0, Count = 0, R = 0, G = 0, B = 0, A = 0;
        const auto Cc = [](const char* T) { return (uint32_t)T[0] | (uint32_t)T[1] << 8 | (uint32_t)T[2] << 16 | (uint32_t)T[3] << 24; };
        switch (L.Format)
        {
        case Out::BGRA: PfFlags = 0x1 | 0x40; Count = 32; R = 0xFF0000; G = 0xFF00; B = 0xFF; A = 0xFF000000; break;
        case Out::BGRX: PfFlags = 0x40; Count = 32; R = 0xFF0000; G = 0xFF00; B = 0xFF; break;
        case Out::LA: PfFlags = 0x1 | 0x20000; Count = 16; R = 0xFF; A = 0xFF00; break;
        case Out::L: PfFlags = 0x20000; Count = 8; R = 0xFF; break;
        case Out::A: PfFlags = 0x2; Count = 8; A = 0xFF; break;
        case Out::BC1: PfFlags = 0x4; FourCc = Cc("DXT1"); break;
        case Out::BC2: PfFlags = 0x4; FourCc = Cc("DXT3"); break;
        case Out::BC3: PfFlags = 0x4; FourCc = Cc("DXT5"); break;
        case Out::BC5: PfFlags = 0x4; FourCc = Cc("DX10"); break;
        }
        for (uint32_t V : { 32u, PfFlags, FourCc, Count, R, G, B, A }) Put32(S, V);
        uint32_t Caps1 = 0x1000;
        if (HasMips) Caps1 |= 0x8 | 0x400000;
        if (Cube) Caps1 |= 0x8;
        Put32(S, Caps1);
        Put32(S, Cube ? 0x200u | 0xFC00u : (Depth > 1 ? 0x200000u : 0u));
        for (int I = 0; I < 3; I++) Put32(S, 0);
        if (L.Format == Out::BC5)   // DXGI_FORMAT_BC5_UNORM, a 2D texture (6 for a cube)
            for (uint32_t V : { 83u, 3u, Cube ? 4u : 0u, Cube ? 6u : 1u, 0u }) Put32(S, V);
        return S;
    }
}

namespace
{
    // An IWI decoded: its mips, largest first, each holding all its faces, in the layout F
    struct Decoded
    {
        Layout L{};
        unsigned W = 0, H = 0, Depth = 1, MipCount = 0;
        bool HasMips = false, Cube = false;
        std::vector<std::vector<uint8_t>> Mips;
    };

    // The header of each IWI version this decoder reads (OpenAssetTools' IwiTypes.h):
    // 5, 6 (CoD2, CoD4, WaW): u8 format 4, u8 flags 5 (no mips 0x2, cube 0x4), u16 dims 6, u32 sizes[4] 12; 28 bytes
    // 8, 9 (MW2, MW3, CoD Online): u32 flags 4 (no mips 0x2, cube 1 << 16), u8 format 8, u16 dims 10, u32 sizes[4] 16; 32
    // 13 (Black Ops): as 6, then f32 gamma 12, u32 sizes[8] 16; 48
    bool Parse(const uint8_t* Data, size_t Size, Decoded& D, uint8_t& Format, size_t& HeaderSize, const uint32_t*& Sizes, unsigned& SizeCount,
               std::string& Error)
    {
        if (Size < 4 || Data[0] != 'I' || Data[1] != 'W' || Data[2] != 'i')
        {
            Error = "not an IWI";
            return false;
        }
        const uint8_t Version = Data[3];
        size_t DimsAt = 6;
        uint32_t Flags = 0;
        switch (Version)
        {
        case 5: case 6: HeaderSize = 28, SizeCount = 4; break;
        case 8: case 9: HeaderSize = 32, SizeCount = 4, DimsAt = 10; break;
        case 13: HeaderSize = 48, SizeCount = 8; break;
        default:
            Error = "IWI version " + std::to_string(Version);
            return false;
        }
        if (Size < HeaderSize)
        {
            Error = "IWI header past the end";
            return false;
        }
        if (Version == 8 || Version == 9)
        {
            std::memcpy(&Flags, Data + 4, 4);
            Format = Data[8];
            D.HasMips = !(Flags & 0x2), D.Cube = (Flags & 0x30000) == 0x10000;
        }
        else
        {
            Format = Data[4];
            D.HasMips = !(Data[5] & 0x2), D.Cube = (Data[5] & 0x4) != 0;
        }
        uint16_t Dim[3];
        std::memcpy(Dim, Data + DimsAt, 6);
        Sizes = reinterpret_cast<const uint32_t*>(Data + HeaderSize - 4 * SizeCount);
        D.W = Dim[0], D.H = Dim[1], D.Depth = std::max<unsigned>(1, Dim[2]);
        return true;
    }

    bool Decode(const uint8_t* Data, size_t Size, Decoded& Result, std::string& Error)
    {
        uint8_t Format = 0;
        size_t HeaderSize = 0;
        const uint32_t* SizesAt = nullptr;
        unsigned SizeCount = 0;
        if (!Parse(Data, Size, Result, Format, HeaderSize, SizesAt, SizeCount, Error)) return false;
        std::vector<uint32_t> PicmipSize(SizeCount);
        std::memcpy(PicmipSize.data(), SizesAt, 4 * SizeCount);
        const unsigned W = Result.W, H = Result.H, Depth = Result.Depth;
        const bool HasMips = Result.HasMips, Cube = Result.Cube;
        const unsigned Faces = Cube ? 6 : 1;
        if (!W || !H)
        {
            Error = "empty image";
            return false;
        }
        unsigned MipCount = 0;
        for (unsigned M = std::max({ W, H, Depth }); M; M >>= 1) MipCount++;
        if (!HasMips) MipCount = 1;

        const bool Wavelet = Format >= 6 && Format <= 10;
        Out F;
        switch (Format)
        {
        case 1: case 6: F = Out::BGRA; break;
        case 2: case 7: F = Out::BGRX; break;
        case 3: case 8: F = Out::LA; break;
        case 4: case 9: F = Out::L; break;
        case 5: case 10: F = Out::A; break;
        case 11: F = Out::BC1; break;
        case 12: F = Out::BC2; break;
        case 13: F = Out::BC3; break;
        case 14: F = Out::BC5; break;
        default:
            Error = "format " + std::to_string(Format);
            return false;
        }
        const Layout L = LayoutOf(F);
        // Sized from the header before anything is decoded: stored mips must fit in the file, and a
        // wavelet image (1024 texels at most in CoD2, CoD4 and WaW) is refused past 4096, not given gigabytes
        if (Wavelet && (W > 4096 || H > 4096 || Depth > 1))
        {
            Error = "wavelet image of " + std::to_string(W) + "x" + std::to_string(H) + "x" + std::to_string(Depth);
            return false;
        }
        if (!Wavelet)
        {
            const Layout Stored = F == Out::BGRX ? Layout{ F, 24, false } : L;
            uint64_t Total = 0;
            for (unsigned M = 0; M < MipCount; M++) Total += (uint64_t)MipSize(Stored, W, H, Depth, M) * Faces;
            if (Total > Size - HeaderSize)
            {
                Error = "mips past the end";
                return false;
            }
        }
        // the decoded mips, largest first, each with all its faces
        std::vector<std::vector<uint8_t>>& Mips = Result.Mips;
        Mips.assign(MipCount, {});
        for (unsigned M = 0; M < MipCount; M++) Mips[M].assign(MipSize(L, W, H, Depth, M) * Faces, 0);
        Result.L = L, Result.MipCount = MipCount;

        if (Wavelet)
        {
            if (!Huffman().Ok)
            {
                Error = "wavelet tables";
                return false;
            }
            if (Depth > 1 || (W & (W - 1)) || (H & (H - 1)) || (!HasMips && W > 1 && H > 1))
            {
                Error = "wavelet image not a power of two with mips";
                return false;
            }
            const unsigned Channels = Format == 6 ? 4 : Format == 7 ? 3 : Format == 8 ? 2 : 1;
            const unsigned Bpp = L.Bits / 8;
            Bits B(Data + HeaderSize, Size - HeaderSize);
            for (int M = (int)MipCount - 1; M >= 0; M--)
            {
                const size_t FaceSize = MipSize(L, W, H, 1, M);
                for (unsigned Face = 0; Face < Faces; Face++)
                {
                    const uint8_t* Src = M + 1 < (int)MipCount ? Mips[M + 1].data() + MipSize(L, W, H, 1, M + 1) * Face : nullptr;
                    if (!WaveletLevel(Src, Mips[M].data() + FaceSize * Face, std::max(1u, W >> M), std::max(1u, H >> M), Channels, Bpp, B))
                    {
                        Error = "wavelet data ends in mip " + std::to_string(M);
                        return false;
                    }
                }
            }
        }
        else
        {
            // stored smallest first; RGB comes as 3 bytes a pixel and is widened (the fourth byte 0)
            const Layout Stored = F == Out::BGRX ? Layout{ F, 24, false } : L;
            size_t At = HeaderSize;
            for (int M = (int)MipCount - 1; M >= 0; M--)
            {
                const size_t Bytes = MipSize(Stored, W, H, Depth, M) * Faces;
                if (At + Bytes > Size)
                {
                    Error = "mip " + std::to_string(M) + " past the end";
                    return false;
                }
                if (F == Out::BGRX)
                    for (size_t P = 0; P < Bytes / 3; P++) std::memcpy(Mips[M].data() + P * 4, Data + At + P * 3, 3);
                else
                    std::memcpy(Mips[M].data(), Data + At, Bytes);
                At += Bytes;
                if (M < (int)SizeCount && At != PicmipSize[M])
                {
                    Error = "file size for picmip " + std::to_string(M) + ": " + std::to_string(PicmipSize[M]) + ", mips end at " + std::to_string(At);
                    return false;
                }
            }
        }
        return true;
    }
}

namespace CoDIWIDecoder
{
bool ToDds(const uint8_t* Data, size_t Size, std::string& Dds, std::string& Error)
{
    if (Size < 4 || (Data[3] != 5 && Data[3] != 6))
    {
        Error = "not an IWI of version 5 or 6";
        return false;
    }
    Decoded D;
    if (!Decode(Data, Size, D, Error)) return false;
    Dds = Header(D.L, D.W, D.H, D.Depth, D.MipCount, D.HasMips, D.Cube);
    if (!D.Cube)
        for (const auto& M : D.Mips) Dds.append(reinterpret_cast<const char*>(M.data()), M.size());
    else
        for (unsigned Face = 0; Face < 6; Face++)   // DDS: a face with all its mips, then the next
            for (unsigned M = 0; M < D.MipCount; M++)
            {
                const size_t FaceSize = D.Mips[M].size() / 6;
                Dds.append(reinterpret_cast<const char*>(D.Mips[M].data() + FaceSize * Face), FaceSize);
            }
    return true;
}

bool LargestRgba(const uint8_t* Data, size_t Size, Rgba& Image, std::string& Error)
{
    Decoded D;
    if (!Decode(Data, Size, D, Error)) return false;
    if (D.L.Block)
    {
        Error = "a block-compressed format";
        return false;
    }
    Image.Width = D.W, Image.Height = D.H, Image.Cube = D.Cube;
    const std::vector<uint8_t>& Top = D.Mips[0];
    const size_t Bpp = D.L.Bits / 8, Pixels = Top.size() / Bpp;
    Image.Pixels.assign(Pixels * 4, 0);
    for (size_t P = 0; P < Pixels; P++)
    {
        const uint8_t* S = Top.data() + P * Bpp;
        uint8_t* T = Image.Pixels.data() + P * 4;
        switch (D.L.Format)
        {
        case Out::BGRA: T[0] = S[2], T[1] = S[1], T[2] = S[0], T[3] = S[3]; break;
        case Out::BGRX: T[0] = S[2], T[1] = S[1], T[2] = S[0], T[3] = 0xFF; break;
        case Out::LA: T[0] = T[1] = T[2] = S[0], T[3] = S[1]; break;
        case Out::L: T[0] = T[1] = T[2] = S[0], T[3] = 0xFF; break;
        default: T[3] = S[0]; break;   // alpha alone: black, as an A8 image converts
        }
    }
    return true;
}
}
