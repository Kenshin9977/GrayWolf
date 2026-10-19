#include "stdafx.h"

#include "IwctxReaders.h"

#include "CoDAssets.h"
#include "CoDRawImageTranslator.h"
#include "CoDXAnimReader.h"
#include "ExportNaming.h"
#include "ExportWriters.h"
#include "Games/IW/IwEntityKeys.h"
#include "FileSystems.h"
#include "MapLive.h"
#include "SABSupport.h"
#include "SettingsManager.h"
#include "Strings.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <format>
#include <mutex>
#include <set>
#include <unordered_map>

#include "Games/Common/StringCipher.h"

#define XXH_INLINE_ALL
#include <xxhash.h>

namespace
{
    template <class T>
    T Read(uint64_t Address)
    {
        return CoDAssets::GameInstance->Read<T>(Address);
    }

    // a pointer the served zone holds: a reference it does not hold keeps its raw value (a hash or
    // a key, bit 63 or high bits set), which no read reaches
    bool Mapped(uint64_t Address)
    {
        uint8_t B = 0;
        return Address != 0 && (Address >> 48) == 0 && CoDAssets::GameInstance->Read(&B, Address, 1) == 1;
    }
}

void IwctxReaders::UnpackNormalQuat(uint32_t Packed, float (&Normal)[3])
{
    // the quaternion's three smallest components (the dropped one's index in the top two bits),
    // then up (0, 0, 1) rotated by it
    const float A = ((((Packed >> 0) & 0x3FF) / 511.5f) - 1.0f) / 1.4142135f;
    const float B = ((((Packed >> 10) & 0x3FF) / 511.5f) - 1.0f) / 1.4142135f;
    const float C = ((((Packed >> 20) & 0x1FF) / 255.5f) - 1.0f) / 1.4142135f;
    const float D = std::sqrt(std::max(0.0f, 1.0f - A * A - B * B - C * C));
    float X, Y, Z, W;
    switch (Packed >> 30)
    {
    case 0: X = D, Y = A, Z = B, W = C; break;
    case 1: X = A, Y = D, Z = B, W = C; break;
    case 2: X = A, Y = B, Z = D, W = C; break;
    default: X = A, Y = B, Z = C, W = D; break;
    }
    // up rotated: v + 2 * q.xyz x (q.xyz x v + w v), v = (0, 0, 1)
    const float Ax = Y, Ay = -X, Az = W;
    const float Bx = Y * Az - Z * Ay, By = Z * Ax - X * Az, Bz = X * Ay - Y * Ax;
    Normal[0] = Bx + Bx, Normal[1] = By + By, Normal[2] = 1.0f + Bz + Bz;
}

std::unique_ptr<XImageDDS> IwctxReaders::LoadXImage(const XImage_t& Image, const uint8_t* Formats, uint32_t FormatCount)
{
    constexpr int64_t InZoneSize = 0x18, Width = 0x20, Height = 0x22, Format = 0x2a, MipCount = 0x2e, Mips = 0x30, Pixels = 0x38;
    const uint64_t At = Image.ImagePtr;
    const uint8_t Fmt = Read<uint8_t>(At + Format);
    const uint32_t Dxgi = Fmt < FormatCount ? Formats[Fmt] : 0;
    if (Dxgi == 0) return nullptr;
    const uint16_t W = Read<uint16_t>(At + Width), H = Read<uint16_t>(At + Height);
    const uint64_t InZone = Read<uint64_t>(At + Pixels);
    const uint32_t Size = Read<uint32_t>(At + InZoneSize);
    if (InZone != 0 && Size != 0)
    {
        auto Data = std::make_unique<uint8_t[]>(Size);
        if (CoDAssets::GameInstance->Read(Data.get(), InZone, Size) != Size) return nullptr;
        return CoDRawImageTranslator::TranslateBC(Data, Size, W, H, Dxgi);
    }
    const uint64_t MipsAt = Read<uint64_t>(At + Mips);
    const uint32_t Count = Read<uint8_t>(At + MipCount) & 0xF;
    if (MipsAt == 0 || Count == 0 || CoDAssets::GamePackageCache == nullptr) return nullptr;
    if (FILE* Dbg = Maps::DebugFile())
        for (uint32_t K = 0; K < Count; K++)
        {
            const uint64_t Key = Read<uint64_t>(MipsAt + K * 24);
            std::fprintf(Dbg, "[image] %llx %ux%u fmt %u mip %u key %016llx packed %016llx exists %d\n", (unsigned long long)Read<uint64_t>(At), W, H, Fmt, K,
                         (unsigned long long)Key, (unsigned long long)Read<uint64_t>(MipsAt + K * 24 + 16), (int)CoDAssets::GamePackageCache->Exists(Key));
        }
    // the largest level the packages hold (the last record is the full size; a package object
    // holds its level alone)
    for (uint32_t K = Count; K-- > 0;)
    {
        const uint64_t Key = Read<uint64_t>(MipsAt + K * 24);
        if (!CoDAssets::GamePackageCache->Exists(Key)) continue;
        uint32_t Got = 0;
        auto Data = CoDAssets::GamePackageCache->ExtractPackageObject(Key, Got);
        if (Data == nullptr || Got == 0) continue;
        const uint32_t Shift = Count - 1 - K;
        return CoDRawImageTranslator::TranslateBC(Data, Got, std::max(1u, (uint32_t)W >> Shift), std::max(1u, (uint32_t)H >> Shift), Dxgi);
    }
    return nullptr;
}

std::unique_ptr<XSound> IwctxReaders::ReadXSound(const CoDSound_t* Sound, const SoundLayout& L)
{
    const uint64_t At = Sound->AssetPointer;
    const SoundSource S = L.Where(Read<uint8_t>(At + L.Flags));
    const uint32_t Seek = Read<uint32_t>(At + SoundSeek), Loaded = Read<uint32_t>(At + SoundLoaded);
    const uint32_t Rate = Read<uint16_t>(At + L.Rate), Frames = Read<uint32_t>(At + SoundFrames);
    if (Rate == 0) return nullptr;
    std::unique_ptr<uint8_t[]> Data;
    const uint8_t* Opus = nullptr;
    size_t Size = 0, Held = 0;
    // the .xsub object: its header (32 bytes {u64 0, u64 -4, u64 payload} when tagged), the seek
    // table (u32 offsets), then the frames
    if (S.Key >= 0 && CoDAssets::GamePackageCache != nullptr)
    {
        uint32_t Got = 0;
        Data = CoDAssets::GamePackageCache->ExtractPackageObject(Read<uint64_t>(At + S.Key), Got);
        uint64_t Tag = 0;
        if (Data != nullptr && Got >= 16) std::memcpy(&Tag, Data.get() + 8, 8);
        const size_t Skip = (Tag == 0xFFFFFFFFFFFFFFFCull ? 32 : L.Header) + (size_t)Seek;
        if (Data != nullptr && Got >= Skip + 3)
            Opus = Data.get() + Skip, Size = std::min<size_t>(Loaded != 0 ? Loaded : Got - Skip, Got - Skip), Held = Got - Skip;
    }
    // else the frames the zone keeps (primed, or loaded with it)
    if (Opus == nullptr && S.ZoneFrames >= 0 && Mapped(Read<uint64_t>(At + S.ZoneFrames)))
    {
        const uint32_t Got = Read<uint32_t>(At + S.ZoneSize);
        Data = std::make_unique<uint8_t[]>(std::max<size_t>(Got, 1));
        if (CoDAssets::GameInstance->Read(Data.get(), Read<uint64_t>(At + S.ZoneFrames), Got) != Got) return nullptr;
        Opus = Data.get(), Size = L.ZoneCappedByLoaded ? std::min<size_t>(Loaded != 0 ? Loaded : Got, Got) : Got, Held = Got;
    }
    if (Opus == nullptr || Size < 3) return nullptr;
    // packets {u16 size, Opus packet}: the first one's TOC byte says mono or stereo
    const uint32_t Channels = (Opus[2] & 4) ? 2 : 1;
    return SABSupport::OpusPackets(Opus, Held, 0, Size, Rate, Channels, Frames);
}

void IwctxReaders::ExportSounds(const DiskGame::Context& C, DiskGame::Totals& T, const std::string& Zone, const std::vector<SoundEntry>& Sounds)
{
    using DiskGame::Kind;
    auto& Kt = T.Kinds[Kind::Sound];
    const std::string Extension = ExportWriters::SoundExtension(ExportWriters::SoundFormatSetting());
    std::set<std::string> Done;
    for (size_t N = 0; N < Sounds.size(); N++)
    {
        const SoundEntry& E = Sounds[N];
        if (!C.Advance(N, Sounds.size(), E.Full)) break;
        if (!C.Wants(Kind::Sound, E.Full) || !Done.insert(E.Full).second) continue;
        Kt.Seen++;
        CoDSound_t Sound;
        Sound.AssetName = ExportNaming::SafeFileName(std::filesystem::path(E.Full).stem().string());
        Sound.FullPath = std::filesystem::path(E.Full).parent_path().string();
        Sound.AssetPointer = E.Address;
        Sound.FrameRate = E.Rate;
        Sound.FrameCount = E.Frames;
        Sound.IsFileEntry = false;
        Sound.DataType = SoundDataTypes::Opus_Interleaved;
        const std::string Dir = (std::filesystem::path(C.KindDir(Kind::Sound)) / Sound.FullPath).string();
        std::error_code Ec;
        std::filesystem::create_directories(Dir, Ec);
        std::string Crash;
        const int Code = ExportWriters::ExportSound(Sound, Dir, Extension, Crash);
        if (Code == static_cast<int>(ExportGameResult::Success)) Kt.Written++;
        else T.Fail(Zone, Kind::Sound, E.Full, "code " + std::to_string(Code) + " " + Crash);
    }
}

std::vector<std::string> IwctxReaders::ZoneStrings(const std::vector<uint8_t>& Z, const std::string& ProfileDir, size_t ListSize)
{
    std::vector<std::string> Out;
    if (Z.size() < ListSize || ListSize < 0x10) return Out;
    uint32_t Count = 0;
    uint64_t Ptr = 0;
    std::memcpy(&Count, Z.data(), 4);
    std::memcpy(&Ptr, Z.data() + 8, 8);
    if (Ptr == 0) return Out;
    std::string Error;
    const auto* Cipher = StringCipher::Load(ProfileDir, Error);
    size_t Pos = ListSize + (size_t)Count * 8;
    for (uint32_t K = 0; K < Count && ListSize + 8 + K * 8ull <= Z.size(); K++)
    {
        uint64_t Slot = 0;
        std::memcpy(&Slot, Z.data() + ListSize + K * 8ull, 8);
        Out.emplace_back();
        if (Slot == 0 || ((Slot >> 61) & 1) == 0) continue;
        std::string Raw;
        for (; Pos < Z.size() && Z[Pos] != 0; Pos++) Raw.push_back((char)Z[Pos]);
        Pos++;
        if (!StringCipher::Decrypt(Cipher, Raw, Out.back())) Out.back().clear();
    }
    return Out;
}

namespace
{
    std::mutex ScriptStringsLock;
    std::unordered_map<uint64_t, std::string> ScriptStrings;
    std::vector<std::string> ServedSlots;
}

uint64_t IwctxReaders::ScriptStringHash(const std::string& Text)
{
    return XXH3_64bits_withSeed(Text.data(), Text.size(), 0x100000001B3ull);
}

void IwctxReaders::RegisterScriptStrings(const std::vector<std::string>& Texts)
{
    std::lock_guard Guard(ScriptStringsLock);
    for (const auto& T : Texts)
        if (!T.empty()) ScriptStrings.emplace(ScriptStringHash(T), T);
}

std::string IwctxReaders::ScriptString(uint64_t Hash)
{
    {
        std::lock_guard Guard(ScriptStringsLock);
        ScriptStrings.emplace(ScriptStringHash("end"), "end");   // a script constant, in no zone's list
        if (const auto It = ScriptStrings.find(Hash); It != ScriptStrings.end()) return It->second;
    }
    return std::format("{:x}", Hash);
}

void IwctxReaders::ServeSlotStrings(const std::vector<std::string>& Slots)
{
    std::lock_guard Guard(ScriptStringsLock);
    ServedSlots = Slots;
}

std::string IwctxReaders::SlotScriptString(uint64_t Slot)
{
    std::lock_guard Guard(ScriptStringsLock);
    if (Slot < ServedSlots.size() && !ServedSlots[Slot].empty()) return ServedSlots[Slot];
    return std::format("string_{}", Slot);
}

std::string IwctxReaders::HashedName(const char* Type, uint64_t Hash)
{
    constexpr uint64_t Mask = 0x7FFFFFFFFFFFFFFFull;   // 63-bit name hashes (bit 63: a reference)
    auto& Db = CoDAssets::AssetNameCache.NameDatabase;
    for (const uint64_t Key : { Hash & Mask, Hash & 0xFFFFFFFFFFFFFFFull })
        if (const auto It = Db.find(Key); It != Db.end()) return It->second;
    return std::format("{}_{:x}", Type, Hash & Mask);
}

void IwctxReaders::LoadNameIndexes()
{
    static std::once_flag Once;
    std::call_once(Once, [] {
        const auto Index = std::filesystem::path(FileSystems::GetApplicationPath()) / "package_index";
        for (const char* F : { "fnv1a_string.wni", "fnv1a_bones.wni" }) CoDAssets::StringCache.LoadIndex((Index / F).string());
        // the games name bones by the FNV-1a 64 of the name (the indexes key them by 32 bits): add those keys
        std::vector<std::pair<uint64_t, std::string>> Wide;
        for (const auto& [Key, Name] : CoDAssets::StringCache.NameDatabase)
        {
            uint64_t H = 0xcbf29ce484222325ull;
            for (const unsigned char Ch : Name) H = (H ^ Ch) * 0x100000001b3ull;
            Wide.emplace_back(H, Name);
        }
        for (auto& [Key, Name] : Wide) CoDAssets::StringCache.NameDatabase.try_emplace(Key, std::move(Name));
        // fnv1a_localize.wni: localize key names, hashed as asset names are
        for (const char* F : { "fnv1a_xmodels.wni", "fnv1a_v2_ximages.wni", "fnv1a_v2_xmaterials.wni", "fnv1a_v2_xanims.wni", "fnv1a_ximages.wni",
                               "fnv1a_xmaterials.wni", "fnv1a_xsounds.wni", "fnv1a_localize.wni" })
            CoDAssets::AssetNameCache.LoadIndex((Index / F).string());
    });
}

// GfxEntityPairs (0x40, 0x28451c0): counts u32 0 (pairs), 4 (keys), 0xc (vectors), 0x10 (string
// pool size); pairs 0x18 ({u16 at, u8 low key bits << 4, u8 type << 4 | high key bits}: type 0 a
// string in the pool, 2 a vector of three floats, 9 a new entity), keys 0x20 (a 64-bit hash each),
// vectors 0x30, string pool 0x38. Black Ops 6's map_game_objects holds one at 8 (its 0x60 struct's
// start), Black Ops 7's at 0x10 (0x3872410's 0x60 struct, the same 0x40 pairs 0x386c0e0);
// scriptbrushmodel one at 0x10 in both.
// A key is named by IwEntityKeys: the hash of its text (the multiplayer's salt after the first
// character, the campaign's after the name)
void IwctxReaders::EntityPairs(const std::vector<uint8_t>& Img, int64_t Pairs, const std::function<int64_t(uint64_t)>& Off,
                               const std::function<uint64_t(int64_t)>& Ptr, std::vector<MapEntity>& Out, IwEntityKeys::Scheme KeyScheme,
                               const PairPacking& Packing)
{
    const auto U32 = [&](int64_t At) { return ExportNaming::At<uint32_t>(Img, At); };
    const uint32_t N = U32(Pairs), Keys = U32(Pairs + 4), Vecs = U32(Pairs + 0xc), PoolSize = U32(Pairs + 0x10);
    const int64_t PairAt = Off(Ptr(Pairs + 0x18)), KeyAt = Off(Ptr(Pairs + 0x20)), VecAt = Off(Ptr(Pairs + 0x30)), Pool = Off(Ptr(Pairs + 0x38));
    if (PairAt < 0 || KeyAt < 0 || N == 0) return;
    bool Open = false;
    for (uint32_t K = 0; K < N; K++)
    {
        const uint32_t Pair = U32(PairAt + K * 4);
        const uint32_t At = Pair & ((1u << Packing.AtBits) - 1), Type = Pair >> 28, Key = (Pair & 0x0FFFFFFFu) >> Packing.KeyShift;
        if (Type == Packing.NewEntity)
        {
            Out.emplace_back();
            Open = true;
            continue;
        }
        if (!Open) Out.emplace_back(), Open = true;
        std::string Name;
        if (Key < Keys)
        {
            const uint64_t Hash = ExportNaming::At<uint64_t>(Img, KeyAt + (int64_t)Key * 8);
            Name = IwEntityKeys::Name(Hash, KeyScheme);
        }
        else
            Name = std::format("key_{}", Key);
        std::string Value;
        if (Type == 0 && Pool >= 0 && At < PoolSize)
            for (int64_t P = Pool + At; P < (int64_t)Img.size() && Img[(size_t)P]; P++) Value.push_back((char)Img[(size_t)P]);
        else if (Type == Packing.Vector && VecAt >= 0 && At < Vecs)
            Value = std::format("{} {} {}", ExportNaming::At<float>(Img, VecAt + At * 12), ExportNaming::At<float>(Img, VecAt + At * 12 + 4),
                                ExportNaming::At<float>(Img, VecAt + At * 12 + 8));
        else
            Value = std::format("type{}:{}", Type, At);
        Out.back().emplace_back(std::move(Name), std::move(Value));
    }
}

std::string IwctxReaders::EntityText(const std::vector<MapEntity>& Entities)
{
    std::string Out;
    for (const auto& E : Entities)
    {
        Out += "{\n";
        for (const auto& [K, V] : E) Out += "\"" + K + "\" \"" + V + "\"\n";
        Out += "}\n";
    }
    return Out;
}

std::string IwctxReaders::EntityText(const std::vector<uint8_t>& Img, int64_t Pairs, const std::function<int64_t(uint64_t)>& Off,
                       const std::function<uint64_t(int64_t)>& Ptr, size_t& Count, IwEntityKeys::Scheme KeyScheme, const PairPacking& Packing)
{
    std::vector<MapEntity> Entities;
    EntityPairs(Img, Pairs, Off, Ptr, Entities, KeyScheme, Packing);
    Count += Entities.size();
    return EntityText(Entities);
}

void IwctxReaders::ExportMapEnts(const DiskGame::Context& C, DiskGame::Totals& T, const std::string& Zone, const std::vector<MapEntity>& Entities)
{
    using DiskGame::Kind;
    auto& Kt = T.Kinds[Kind::MapEnts];
    if (Entities.empty() || !C.Wants(Kind::MapEnts, Zone)) return;
    Kt.Seen++;
    const auto Clean = [](std::string S) {
        for (char& Ch : S)
            if (Ch == '\t' || Ch == '\n' || Ch == '\r') Ch = ' ';
        return S;
    };
    std::string Out;
    size_t Pairs = 0;
    for (size_t E = 0; E < Entities.size(); E++)
        for (const auto& [K, V] : Entities[E])
        {
            Out += std::to_string(E) + "\t" + Clean(K) + "\t" + Clean(V) + "\n";
            Pairs++;
        }
    const std::string Stem = ExportNaming::SafeFileName(Zone);
    const auto File = std::filesystem::path(C.KindDir(Kind::MapEnts)) / (Stem + ".tsv");
    if (!ExportWriters::WriteBytes(File, (const uint8_t*)Out.data(), Out.size()))
    {
        T.Fail(Zone, Kind::MapEnts, Zone, "write failed");
        return;
    }
    Kt.Written++;
    T.Counters["mapents_entities"] += Entities.size();
    T.Counters["mapents_pairs"] += Pairs;
    T.Index(Kind::MapEnts, "", "", "", Zone, Zone, File.string());
}

std::string IwctxReaders::ScriptString32(uint32_t Hash)
{
    std::lock_guard Guard(ScriptStringsLock);
    static std::unordered_map<uint32_t, std::string> Low;
    static size_t From = 0;
    if (From != ScriptStrings.size())
    {
        Low.clear();
        for (const auto& [H, Text] : ScriptStrings) Low.try_emplace((uint32_t)H, Text);
        From = ScriptStrings.size();
    }
    const auto It = Low.find(Hash);
    return It == Low.end() ? std::string() : It->second;
}
// ---- xanim (IwctxReaders.h, XAnimLayout)
namespace
{
    // the header's ten bone counts, Modern Warfare's order (the total last)
    enum XAnimCount { NoneRot, Rot2D, Rot3D, Static2D, Static3D, Trans, TransPrecise, TransStatic, TransNone, Total };
    // the first part's section table: data bytes, shorts, ints; random shorts (3 + part) and bytes
    // (7 + part); bone names; u16 frame indices
    enum XAnimSection { DataBytes = 0, DataShorts = 1, DataInts = 2, RandomShorts = 3, RandomBytes = 7, BoneNames = 13, Indices = 15 };
}

std::vector<std::vector<uint8_t>> IwctxReaders::XAnimParts(uint64_t Anim, const XAnimLayout& L)
{
    std::vector<std::vector<uint8_t>> Out;
    const uint8_t Count = Read<uint8_t>(Anim + L.PartCount);
    const uint64_t List = Read<uint64_t>(Anim + L.Parts);
    for (uint32_t K = 0; K < Count && List != 0 && (List >> 48) == 0; K++)
    {
        const uint64_t E = List + (uint64_t)K * 0x18;
        std::vector<uint8_t> Buf;
        if (const uint64_t Data = Read<uint64_t>(E); Data != 0 && (Data >> 48) == 0)
        {
            Buf.resize(Read<uint32_t>(E + 8));
            if (CoDAssets::GameInstance->Read(Buf.data(), Data, Buf.size()) != Buf.size()) Buf.clear();
        }
        else if (CoDAssets::GamePackageCache != nullptr)
        {
            uint32_t Got = 0;
            auto Object = CoDAssets::GamePackageCache->ExtractPackageObject(Read<uint64_t>(E + 8), Got);
            if (Object != nullptr) Buf.assign(Object.get(), Object.get() + Got);
        }
        if (Buf.empty() && K == 0) return {};
        Out.push_back(std::move(Buf));
    }
    return Out;
}

std::unique_ptr<XAnim_t> IwctxReaders::ReadXAnim(const CoDAnim_t* Animation, const XAnimLayout& L, std::string (*BoneName)(uint64_t))
{
    const uint64_t At = Animation->AssetPointer;
    const auto Parts = XAnimParts(At, L);
    if (Parts.empty() || Parts[0].size() < 0xa0) return nullptr;
    auto Anim = std::make_unique<XAnim_t>();
    Anim->AnimationName = Animation->AssetName;
    Anim->FrameCount = Read<uint16_t>(At + L.Frames);
    Anim->FrameRate = Read<float>(At + L.Rate);
    Anim->LoopingAnimation = (Read<uint8_t>(At + L.Flags) & 1) != 0;
    Anim->AdditiveAnimation = Read<uint8_t>(At + L.Type) == 6;
    Anim->TotalBoneCount = Read<uint16_t>(At + L.Counts + Total * 2);
    Anim->ReaderInformationPointer = At;
    Anim->Reader = std::make_unique<CoDXAnimReader>();
    const auto& B = Parts[0];
    int32_t Names = -1;
    std::memcpy(&Names, B.data() + BoneNames * 4, 4);
    for (uint32_t K = 0; K < Anim->TotalBoneCount; K++)
    {
        uint64_t Hash = 0;
        if (Names >= 0 && (size_t)Names + (K + 1) * 8ull <= B.size()) std::memcpy(&Hash, B.data() + Names + K * 8, 8);
        Anim->Reader->BoneNames.push_back(BoneName(Hash));
    }
    if (L.NoteCount >= 0 && L.NoteName != nullptr)
    {
        // each part's records (parts 0 and 1), then merged by time as the game's iterator yields them
        uint32_t Counts = 0;
        std::memcpy(&Counts, B.data() + 22 * 4, 4);
        std::vector<std::pair<uint16_t, float>> Heads[2];
        for (size_t K = 0; K < 2 && K < Parts.size(); K++)
        {
            uint64_t Off = 0;
            std::memcpy(&Off, B.data() + (16 + 2 * K) * 4, 8);
            if (Off == 0xFFFFFFFFull || Parts[K].empty()) continue;
            for (uint32_t J = 0; J < ((Counts >> (8 * K)) & 0xFF); J++)
            {
                const uint64_t Rec = Off + J * 16ull;
                if (Rec + 8 > Parts[K].size()) break;
                uint16_t Index = 0;
                float Time = 0;
                std::memcpy(&Index, Parts[K].data() + Rec, 2);
                std::memcpy(&Time, Parts[K].data() + Rec + 4, 4);
                Heads[K].emplace_back(Index, Time);
            }
        }
        const uint16_t NameCount = Read<uint16_t>(At + L.NoteNameCount);
        const uint64_t NameList = Read<uint64_t>(At + L.NoteNames);
        const auto Name = [&](uint16_t Index) {
            if (Index >= NameCount || NameList == 0) return std::format("note_{}", Index);
            if (L.NoteNameSlots) return L.NoteName(Read<uint32_t>(NameList + Index * 4ull));
            const uint64_t Hash = (uint64_t)Read<uint32_t>(NameList + NameCount * 4ull + Index * 4ull) << 32 | Read<uint32_t>(NameList + Index * 4ull);
            return L.NoteName(Hash);
        };
        for (size_t I = 0, J = 0; I < Heads[0].size() || J < Heads[1].size();)
        {
            const bool Second = J < Heads[1].size() && (I >= Heads[0].size() || Heads[1][J].second < Heads[0][I].second);
            const auto& [Index, Time] = Second ? Heads[1][J++] : Heads[0][I++];
            // a notetrack at FLT_MAX (Black Ops 6 has some) is on no frame: the game's iterator
            // yields it last, at no time the animation reaches; it is left out
            if (!(Time >= 0.0f && Time <= 1.0f)) continue;
            Anim->Reader->Notetracks.emplace_back(Name(Index), (size_t)std::lround(Time * (float)Anim->FrameCount));
        }
    }
    const XAnimLayout* Layout = &L;
    Anim->ReaderFunction = [Layout](const std::unique_ptr<XAnim_t>& A, std::unique_ptr<WraithAnim>& R) { LoadXAnim(A, R, *Layout); };
    return Anim;
}

void IwctxReaders::LoadXAnim(const std::unique_ptr<XAnim_t>& Anim, std::unique_ptr<WraithAnim>& Result, const XAnimLayout& L)
{
    const uint64_t At = Anim->ReaderInformationPointer;
    const auto Bufs = XAnimParts(At, L);
    if (Bufs.empty()) return;
    const auto& B0 = Bufs[0];
    const size_t N = Bufs.size();
    uint16_t C[10] = {};
    for (int K = 0; K < 10; K++) C[K] = Read<uint16_t>(At + L.Counts + K * 2);
    const auto Sec = [&](int K) {
        int32_t V = -1;
        std::memcpy(&V, B0.data() + K * 4, 4);
        return V;
    };
    // each part's random data: shorts at section 3 + part; bytes at 7 + part (Black Ops 6's
    // reading: the first part's at 7, the others' at 0)
    std::vector<size_t> Rs(N), Rb(N, 0);
    for (size_t K = 0; K < N; K++)
    {
        Rs[K] = (size_t)Sec(RandomShorts + (int)K);
        if (L.GameRules || K == 0) Rb[K] = (size_t)Sec(RandomBytes + (int)K);
    }
    size_t Pb = (size_t)Sec(DataBytes), Ps = (size_t)Sec(DataShorts), Pi = (size_t)Sec(DataInts), Px = (size_t)Sec(Indices);
    const auto Get = [&](const std::vector<uint8_t>& Buf, size_t& P, auto Zero) {
        decltype(Zero) V = Zero;
        if (P + sizeof(V) <= Buf.size()) std::memcpy(&V, Buf.data() + P, sizeof(V));
        P += sizeof(V);
        return V;
    };
    const bool ByteFrames = Anim->FrameCount < 0x100;
    const auto& Names = Anim->Reader->BoneNames;
    // a table: its last key's index T, then (more than one part) the keys each part holds, then
    // every key's frame: bytes under 0x100 frames, else u16 inline (T under 0x40) or in the indices
    // section (after a coarse lookup table of ((T - 1) >> 8) + 2 shorts)
    std::vector<uint32_t> Frames;
    std::vector<uint32_t> PerBuffer(N);
    const auto Table = [&]() {
        const int16_t T = Get(B0, Ps, (int16_t)0);
        std::fill(PerBuffer.begin(), PerBuffer.end(), 0);
        if (L.GameRules)
        {
            if (N > 1 && T + 1 >= 4)
                for (size_t K = 0; K < N; K++) PerBuffer[K] = (uint32_t)Get(B0, Ps, (int16_t)0);
            else
                PerBuffer[0] = (uint32_t)T + 1;
            if (T >= 0x40 && !ByteFrames && N == 1) Ps += (size_t)(((T - 1) >> 8) + 2) * 2;
        }
        else
        {
            if (T >= 0x40 && !ByteFrames) Ps += (size_t)(((T - 1) >> 8) + 2) * 2;
            if (N > 1)
                for (size_t K = 0; K < N; K++) PerBuffer[K] = (uint32_t)Get(B0, Ps, (int16_t)0);
            else
                PerBuffer[0] = (uint32_t)T + 1;
        }
        Frames.clear();
        for (int J = 0; J <= T; J++)
            Frames.push_back(ByteFrames ? Get(B0, Pb, (uint8_t)0) : T >= 0x40 ? Get(B0, Px, (uint16_t)0) : (uint16_t)Get(B0, Ps, (int16_t)0));
    };
    // translations relative to the bone's rest position (SEAnim "relative"); an additive anim
    // (asset type 6) stays additive (Black Ops 6's reading: always relative)
    if (!L.GameRules || !Anim->AdditiveAnimation) Result->AnimType = WraithAnimationType::Relative;
    constexpr float Q = 0.000030518509f;   // 1 / 32767
    size_t Bone = 0;
    for (; Bone < C[NoneRot] && Bone < Names.size(); Bone++) Result->AddRotationKey(Names[Bone], 0, 0, 0, 0, 1.0f);
    for (int Kind = 0; Kind < 2; Kind++)
    {
        const size_t W = Kind == 0 ? 2 : 4;
        for (size_t E = Bone + (Kind == 0 ? C[Rot2D] : C[Rot3D]); Bone < E && Bone < Names.size(); Bone++)
        {
            Table();
            size_t F = 0;
            for (size_t K = 0; K < N; K++)
                for (uint32_t J = 0; J < PerBuffer[K] && F < Frames.size(); J++, F++)
                {
                    if (Bufs[K].empty()) continue;
                    int16_t V[4] = {};
                    for (size_t X = 0; X < W; X++) V[X] = Get(Bufs[K], Rs[K], (int16_t)0);
                    if (W == 2) Result->AddRotationKey(Names[Bone], Frames[F], 0, 0, V[0] * Q, V[1] * Q);
                    else Result->AddRotationKey(Names[Bone], Frames[F], V[0] * Q, V[1] * Q, V[2] * Q, V[3] * Q);
                }
        }
    }
    for (size_t E = Bone + C[Static2D]; Bone < E && Bone < Names.size(); Bone++)
    {
        const float Z = Get(B0, Ps, (int16_t)0) * Q, Wv = Get(B0, Ps, (int16_t)0) * Q;
        Result->AddRotationKey(Names[Bone], 0, 0, 0, Z, Wv);
    }
    for (size_t E = Bone + C[Static3D]; Bone < E && Bone < Names.size(); Bone++)
    {
        float V[4];
        for (float& X : V) X = Get(B0, Ps, (int16_t)0) * Q;
        Result->AddRotationKey(Names[Bone], 0, V[0], V[1], V[2], V[3]);
    }
    // translations: the bone's index, its table, the minimum and the scale (floats), then per key
    // three bytes (Trans) or u16 (TransPrecise): scale * key + minimum, in float as the game
    for (int Precise = 0; Precise < 2; Precise++)
        for (uint32_t K0 = 0; K0 < (Precise ? C[TransPrecise] : C[Trans]); K0++)
        {
            const uint16_t Index = (uint16_t)Get(B0, Ps, (int16_t)0);
            Table();
            float Min[3], Frame[3];
            for (float& X : Min) X = Get(B0, Pi, 0.0f);
            for (float& X : Frame) X = Get(B0, Pi, 0.0f);
            size_t F = 0;
            for (size_t K = 0; K < N; K++)
                for (uint32_t J = 0; J < PerBuffer[K] && F < Frames.size(); J++, F++)
                {
                    if (Bufs[K].empty()) continue;
                    float V[3];
                    for (int X = 0; X < 3; X++)
                        V[X] = Frame[X] * (Precise ? (float)Get(Bufs[K], Rs[K], (uint16_t)0) : (float)Get(Bufs[K], Rb[K], (uint8_t)0)) + Min[X];
                    if (Index < Names.size()) Result->AddTranslationKey(Names[Index], Frames[F], V[0], V[1], V[2]);
                }
        }
    for (uint32_t K = 0; K < C[TransStatic]; K++)
    {
        float V[3];
        for (float& X : V) X = Get(B0, Pi, 0.0f);
        const uint16_t Index = (uint16_t)Get(B0, Ps, (int16_t)0);
        if (Index < Names.size()) Result->AddTranslationKey(Names[Index], 0, V[0], V[1], V[2]);
    }
    // a bone's keys come part after part: in frame order, as the game looks them up
    if (L.GameRules)
    {
        for (auto& Keys : Result->AnimationRotationKeys)
            std::stable_sort(Keys.second.begin(), Keys.second.end(), [](const auto& A, const auto& B) { return A.Frame < B.Frame; });
        for (auto& Keys : Result->AnimationPositionKeys)
            std::stable_sort(Keys.second.begin(), Keys.second.end(), [](const auto& A, const auto& B) { return A.Frame < B.Frame; });
    }
}
