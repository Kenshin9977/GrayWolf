#include "stdafx.h"

#include "Adm3.h"

#include "ExportWriters.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <format>
#include <fstream>
#include <set>

#include <nlohmann/json.hpp>

namespace
{
    constexpr int Steps[89] = { 7, 8, 9, 10, 11, 12, 13, 14, 16, 17, 19, 21, 23, 25, 28, 31, 34, 37, 41, 45, 50, 55, 60, 66, 73, 80, 88, 97, 107, 118, 130, 143,
                                157, 173, 190, 209, 230, 253, 279, 307, 337, 371, 408, 449, 494, 544, 598, 658, 724, 796, 876, 963, 1060, 1166, 1282, 1411, 1552,
                                1707, 1878, 2066, 2272, 2499, 2749, 3024, 3327, 3660, 4026, 4428, 4871, 5358, 5894, 6484, 7132, 7845, 8630, 9493, 10442, 11487,
                                12635, 13899, 15289, 16818, 18500, 20350, 22385, 24623, 27086, 29794, 32767 };
    constexpr int IndexStep[16] = { -1, -1, -1, -1, 2, 4, 6, 8, -1, -1, -1, -1, 2, 4, 6, 8 };
    constexpr uint32_t FramesPerBlock = 64;

    template <typename T> T At(const uint8_t* D, size_t Size, size_t Pos)
    {
        T V{};
        if (Pos + sizeof(T) <= Size) std::memcpy(&V, D + Pos, sizeof(T));
        return V;
    }
    float F32(const uint8_t* D, size_t Size, size_t Pos) { return At<float>(D, Size, Pos); }
    uint32_t U32(const uint8_t* D, size_t Size, size_t Pos) { return At<uint32_t>(D, Size, Pos); }

    // one channel's packet: v6 Apple IMA4's {big-endian u16 predictor & 0xff80 | index}, v7 a
    // big-endian i16 predictor and a u8 index; then 64 nibbles, low first
    void Packet(const uint8_t* P, uint16_t Version, int16_t* Out, size_t Stride)
    {
        int Pred, Index;
        size_t Hdr;
        if (Version >= 7)
            Pred = (int16_t)((P[0] << 8) | P[1]), Index = P[2], Hdr = 3;
        else
            Pred = (int16_t)(((P[0] << 8) | P[1]) & 0xff80), Index = P[1] & 0x7f, Hdr = 2;
        Index = std::min(Index, 88);
        for (uint32_t N = 0; N < FramesPerBlock; N++)
        {
            const int Code = (P[Hdr + N / 2] >> ((N & 1) * 4)) & 15;
            const int Step = Steps[Index];
            int Diff = Step >> 3;
            if (Code & 1) Diff += Step >> 2;
            if (Code & 2) Diff += Step >> 1;
            if (Code & 4) Diff += Step;
            Pred = std::clamp(Code & 8 ? Pred - Diff : Pred + Diff, -32768, 32767);
            Index = std::clamp(Index + IndexStep[Code], 0, 88);
            Out[N * Stride] = (int16_t)Pred;
        }
    }
    size_t PacketSize(uint16_t Version) { return Version >= 7 ? 35 : 34; }

    // a 4-byte word as JSON: small values as integers, the rest as the float they are
    nlohmann::json Word(uint32_t W)
    {
        if (W < 0x10000 || W >= 0xffff0000u) return (int32_t)W;
        float F;
        std::memcpy(&F, &W, 4);
        return std::isfinite(F) ? nlohmann::json(std::round(F * 1e6) / 1e6) : nlohmann::json(W);
    }
    std::string Hex(uint32_t V) { return std::format("0x{:x}", V); }
}

int64_t Adm3::Find(const uint8_t* Data, size_t Size)
{
    if (Data == nullptr) return -1;
    for (size_t Pos = 0; Pos + 4 <= Size && Pos <= 0x200; Pos += 4)
        if (std::memcmp(Data + Pos, "ADM3", 4) == 0) return (int64_t)Pos;
    return -1;
}

bool Adm3::Parse(const uint8_t* D, size_t Size, Model& Out)
{
    Out = Model();
    if (Size < 0x120 || std::memcmp(D, "ADM3", 4) != 0) return false;
    Out.Version = At<uint16_t>(D, Size, 0xa);
    Out.DataOffset = U32(D, Size, 0x10);
    if (Out.Version < 6 || Out.Version > 7 || Out.DataOffset < 0x120 || Out.DataOffset > Size) return false;
    for (size_t Pos = 0x14; Pos < 0xf0; Pos += 4) Out.HeaderWords.push_back(U32(D, Size, Pos));
    const uint32_t TopCount = U32(D, Size, 0xf0);
    for (uint32_t K = 0; K < TopCount && K < 16; K++) Out.Top.push_back(U32(D, Size, 0xf8 + K * 8));

    std::map<uint32_t, size_t> ClipAt;
    const auto Tag = [&](size_t Pos, const char* T) { return std::memcmp(D + Pos, T, 4) == 0; };
    for (size_t Pos = 0x120; Pos + 0x30 <= Out.DataOffset; Pos += 4)
    {
        if (Tag(Pos, "SMP2"))
        {
            Clip C;
            C.At = (uint32_t)Pos;
            C.Version = At<uint16_t>(D, Size, Pos + 0xa);
            C.Channels = U32(D, Size, Pos + 0xc);
            C.Freq = F32(D, Size, Pos + 0x10);
            C.RpmBucket = U32(D, Size, Pos + 0x14);
            C.Rate = U32(D, Size, Pos + 0x1c), C.Frames = U32(D, Size, Pos + 0x20), C.Bytes = U32(D, Size, Pos + 0x24), C.Offset = U32(D, Size, Pos + 0x2c);
            const size_t Block = PacketSize(Out.Version) * C.Channels;
            C.Decodable = C.Channels >= 1 && C.Channels <= 8 && C.Rate >= 4000 && C.Rate <= 192000 && C.Offset >= Out.DataOffset && (uint64_t)C.Offset + C.Bytes <= Size &&
                          (uint64_t)C.Bytes == (uint64_t)(C.Frames + FramesPerBlock - 1) / FramesPerBlock * Block;
            ClipAt[C.At] = Out.Clips.size();
            Out.Clips.push_back(C);
        }
        else if (Tag(Pos, "RMP1"))
        {
            Ramp R;
            R.At = (uint32_t)Pos;
            R.From = F32(D, Size, Pos + 0x10), R.To = F32(D, Size, Pos + 0x14);
            R.Smb = U32(D, Size, Pos + 0x1c), R.Grn = U32(D, Size, Pos + 0x24);
            Out.Ramps.push_back(R);
        }
    }
    for (auto& R : Out.Ramps)
    {
        if (R.Smb + 0x20 <= Out.DataOffset && Tag(R.Smb, "SMB1"))
        {
            const uint32_t Count = U32(D, Size, R.Smb + 0x10), Refs = U32(D, Size, R.Smb + 0x18);
            for (uint32_t K = 0; K < Count && Refs + K * 8ull + 8 <= Out.DataOffset; K++) R.Loops.push_back(U32(D, Size, Refs + K * 8ull + 4));
        }
        if (R.Grn + 0x60 <= Out.DataOffset && Tag(R.Grn, "GRN1"))
        {
            R.GrainCount = U32(D, Size, R.Grn + 0x14), R.GrainFlag = U32(D, Size, R.Grn + 0x18);
            R.FreqMin = F32(D, Size, R.Grn + 0x1c), R.FreqMax = F32(D, Size, R.Grn + 0x20), R.GrainValue = F32(D, Size, R.Grn + 0x24);
            R.Rate = (uint32_t)F32(D, Size, R.Grn + 0x28);
            for (size_t Pos = R.Grn + 0x30; Pos < R.Grn + 0x4c; Pos += 4) R.GrainWords.push_back(U32(D, Size, Pos));
            R.MarkersAt = U32(D, Size, R.Grn + 0x54), R.Sweep = U32(D, Size, R.Grn + 0x5c);
            // the markers, in order (index 0, 1 ...), a grain each and the end of the last
            for (uint32_t K = 0; K <= R.GrainCount && R.MarkersAt + K * 12ull + 12 <= Out.DataOffset; K++)
            {
                const size_t Pos = R.MarkersAt + K * 12ull;
                if (At<uint16_t>(D, Size, Pos) != K) break;
                R.Markers.push_back({ (uint16_t)K, At<uint16_t>(D, Size, Pos + 2), F32(D, Size, Pos + 4), F32(D, Size, Pos + 8) });
            }
        }
    }
    // names: ramp<r>_loop<k>_<rpm>rpm, ramp<r>_sweep, else clip_<offset>
    std::set<std::string> Taken;
    const auto Name = [&](uint32_t ClipOffset, const std::string& N) {
        const auto I = ClipAt.find(ClipOffset);
        if (I == ClipAt.end() || !Out.Clips[I->second].Name.empty()) return;
        std::string Unique = N;
        for (int K = 2; !Taken.insert(Unique).second; K++) Unique = N + "_" + std::to_string(K);
        Out.Clips[I->second].Name = Unique;
    };
    std::set<uint32_t> Sets;
    for (const auto& Rp : Out.Ramps) Sets.insert(Rp.GrainFlag);
    const bool BySet = Sets.size() == Out.Ramps.size();
    for (size_t Ri = 0; Ri < Out.Ramps.size(); Ri++)
    {
        const auto& Rp = Out.Ramps[Ri];
        const size_t R = BySet ? Rp.GrainFlag : Ri;
        for (size_t K = 0; K < Rp.Loops.size(); K++)
        {
            const auto I = ClipAt.find(Rp.Loops[K]);
            const float F = I == ClipAt.end() ? 0.0f : Out.Clips[I->second].Freq;
            Name(Rp.Loops[K], std::format("ramp{}_loop{:02}_{}rpm", R, K, (int)std::lround(F * 60)));
        }
        Name(Rp.Sweep, std::format("ramp{}_sweep", R));
    }
    for (auto& C : Out.Clips)
        if (C.Name.empty()) Name(C.At, std::format("clip_{:x}", C.At));
    return !Out.Clips.empty();
}

std::vector<int16_t> Adm3::Decode(const uint8_t* Data, size_t Size, uint16_t Version, const Clip& C)
{
    std::vector<int16_t> Out;
    if (!C.Decodable) return Out;
    const uint32_t Channels = C.Channels;
    const size_t P = PacketSize(Version), Blocks = C.Bytes / (P * Channels);
    if ((uint64_t)C.Offset + Blocks * P * Channels > Size) return Out;
    Out.resize(Blocks * FramesPerBlock * Channels);
    for (size_t B = 0; B < Blocks; B++)
        for (uint32_t Ch = 0; Ch < Channels; Ch++)
            Packet(Data + C.Offset + (B * Channels + Ch) * P, Version, Out.data() + B * FramesPerBlock * Channels + Ch, Channels);
    Out.resize((size_t)C.Frames * Channels);
    return Out;
}

bool Adm3::Write(const uint8_t* Data, size_t Size, const std::filesystem::path& Dir, const std::string& Stem, std::map<std::string, size_t>& Counters)
{
    const int64_t Tag = Find(Data, Size);
    if (Tag < 0) return false;
    Data += Tag, Size -= (size_t)Tag;
    // a size estimate's dry run (DrySounds): the clips handed over, nothing else written
    const bool Dry = ExportWriters::DrySoundsActive();
    std::error_code Ec;
    if (!Dry) std::filesystem::create_directories(Dir, Ec);
    const auto Raw = [&]() {
        if (Dry) return true;
        if (!ExportWriters::WriteBytes(Dir / (Stem + ".adm3.bin"), Data, Size)) return false;
        Counters["sounds_adm3_raw"]++;
        return true;
    };
    Model M;
    if (!Parse(Data, Size, M)) return Raw();

    const std::string Format = ExportWriters::SoundFormatSetting(), Extension = ExportWriters::SoundExtension(Format);
    const std::filesystem::path ClipDir = Dir / Stem;
    if (!Dry) std::filesystem::create_directories(ClipDir, Ec);
    nlohmann::json J;
    J["format"] = "ADM3 (Crankcase Audio REV engine model)";
    J["version"] = M.Version;
    J["data_offset"] = Hex(M.DataOffset);
    J["header_words"] = nlohmann::json::array();
    for (uint32_t W : M.HeaderWords) J["header_words"].push_back(Word(W));
    J["header_refs"] = nlohmann::json::array();
    for (uint32_t T : M.Top) J["header_refs"].push_back(Hex(T));
    bool AllWritten = true;
    std::map<uint32_t, std::string> FileOf;
    J["clips"] = nlohmann::json::array();
    for (const auto& C : M.Clips)
    {
        nlohmann::json Cj = { { "chunk", Hex(C.At) }, { "freq_hz", std::round(C.Freq * 1e4) / 1e4 }, { "rpm", (int)std::lround(C.Freq * 60) },
                              { "rpm_bucket", C.RpmBucket }, { "rate", C.Rate }, { "frames", C.Frames }, { "channels", C.Channels }, { "bytes", C.Bytes },
                              { "chunk_version", C.Version } };
        const auto Pcm = Decode(Data, Size, M.Version, C);
        std::string Why;
        ExportWriters::SoundSource S;
        S.Is = ExportWriters::SoundSource::Form::Pcm16;
        S.Data = reinterpret_cast<const uint8_t*>(Pcm.data());
        S.Size = S.Available = Pcm.size() * sizeof(int16_t);
        S.Rate = C.Rate, S.Channels = C.Channels, S.Frames = C.Frames;
        const std::string File = C.Name + Extension;
        if (!Pcm.empty() && ExportWriters::WriteSound(S, ClipDir / File, Format, &Why))
        {
            Cj["file"] = Stem + "/" + File;
            FileOf[C.At] = Cj["file"];
            Counters["sounds_adm3_clips"]++;
        }
        else
        {
            Cj["file"] = nullptr;
            Cj["not_written"] = Pcm.empty() ? "its blocks are not the frames it says" : Why;
            AllWritten = false;
        }
        J["clips"].push_back(Cj);
    }
    J["ramps"] = nlohmann::json::array();
    for (const auto& R : M.Ramps)
    {
        nlohmann::json Rj = { { "chunk", Hex(R.At) }, { "range", { R.From, R.To } }, { "grain_set", R.GrainFlag }, { "grain_count", R.GrainCount },
                              { "grain_freq_min", R.FreqMin }, { "grain_freq_max", R.FreqMax }, { "grain_value", R.GrainValue }, { "grain_rate", R.Rate } };
        Rj["grain_words"] = nlohmann::json::array();
        for (uint32_t W : R.GrainWords) Rj["grain_words"].push_back(Word(W));
        Rj["sweep"] = FileOf.count(R.Sweep) ? nlohmann::json(FileOf[R.Sweep]) : nlohmann::json(nullptr);
        Rj["loops"] = nlohmann::json::array();
        for (uint32_t L : R.Loops) Rj["loops"].push_back(FileOf.count(L) ? nlohmann::json(FileOf[L]) : nlohmann::json(Hex(L)));
        // each grain: [index, period (frames), frequency (Hz), position in the sweep (frames)]
        Rj["markers"] = nlohmann::json::array();
        for (const auto& K : R.Markers) Rj["markers"].push_back({ K.Index, K.Period, std::round(K.Freq * 1e4) / 1e4, K.Position });
        J["ramps"].push_back(Rj);
    }
    Counters["sounds_adm3"]++;
    if (Dry) return true;
    std::ofstream F(Dir / (Stem + ".adm3.json"), std::ios::binary);
    F << J.dump(1);
    if (!F) return false;
    // a clip not decoded: the object kept as it is beside
    if (!AllWritten) Raw();
    return true;
}
