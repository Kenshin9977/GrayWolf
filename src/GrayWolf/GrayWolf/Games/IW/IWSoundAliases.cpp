#include "stdafx.h"

#include "IWSoundAliases.h"

#include "FileSystems.h"
#include "GameReaders.h"
#include "Games/IW/IWPakSounds.h"

#include <algorithm>
#include <cmath>
#include <cstring>

using SoundAliasCsv::Col;

namespace IWSoundAliases
{
namespace
{
    constexpr const char* Storages[] = { "", "loaded", "streamed", "primed" };
    // World at War's snd_limit_type_t; "softest" has no mod tools value
    constexpr const char* LimitTypes[] = { "none", "oldest", "reject", "priority" };

    std::string Rounded(float V)
    {
        return std::isfinite(V) ? std::to_string(std::lround(V)) : std::string();
    }
}

std::string LiveName(const SoundAliasCsv::ZoneView& Z, const Layout& L, const std::string& Name, uint32_t Index, int64_t File,
                     std::map<int64_t, std::string>& Seen)
{
    if (File < 0) return std::string();
    const int64_t Union = File + Z.PointerSize;
    switch (Z.At<uint8_t>(File))
    {
    case 1: {
        const int64_t Loaded = Z.Follow(Union);
        // a name-only header (",name") stands for the LoadedSound another zone defines
        std::string S = Loaded < 0 ? std::string() : Z.Text(Loaded);
        if (!S.empty() && S[0] == ',') S.erase(0, 1);
        return GameReaders::LoadedSoundStem(S);
    }
    case 2: {
        std::string Lower = Name;
        std::transform(Lower.begin(), Lower.end(), Lower.begin(), [](unsigned char C) { return (char)std::tolower(C); });
        return Seen.emplace(File, Lower + "_" + std::to_string(Index)).first->second;
    }
    case 3: {
        if (L.PrimedLoaded < 0) return std::string();
        const int64_t Loaded = Z.Follow(File + L.PrimedLoaded);
        std::string S = Loaded < 0 ? std::string() : Z.Text(Loaded);
        if (!S.empty() && S[0] == ',') S.erase(0, 1);
        return FileSystems::GetFileName(S);
    }
    default:
        return std::string();
    }
}

void Read(const SoundAliasCsv::ZoneView& Z, int64_t List, const std::string& Name, const Layout& L, const FileOf& File,
          SoundAliasCsv::Rows& Out)
{
    auto& N = Out.Counters;
    const uint32_t Count = L.ListCountBytes == 1 ? Z.At<uint8_t>(List + L.ListCount) : Z.At<uint32_t>(List + L.ListCount);
    const int64_t Head = Z.Follow(List + (L.ListHead >= 0 ? L.ListHead : Z.PointerSize));
    if (Count == 0 || Head < 0)
    {
        N["alias_lists_unread"]++;
        return;
    }
    N["alias_lists"]++;
    const auto F = [&](int64_t E, int64_t At) { return Z.At<float>(E + At); };
    // out of what the mod tools take: clamped, counted
    const auto Pitch = [&](float V) {
        bool Clamped = false;
        std::string S = SoundAliasCsv::Cents(V, &Clamped);
        if (Clamped) N["alias_values_clamped"]++;
        return S;
    };
    const auto Inches = [&](float V) {
        bool Clamped = false;
        std::string S = SoundAliasCsv::Distance(V, &Clamped);
        if (Clamped) N["alias_values_clamped"]++;
        return S;
    };
    for (uint32_t J = 0; J < Count; J++)
    {
        const int64_t E = Head + (int64_t)J * L.Size;
        SoundAliasCsv::Alias R;
        // a list whose name is not in the zone: its variant's own (at 0), else the row is left out
        R[Col::Name] = Name.empty() ? Z.Text(E) : Name;
        if (R[Col::Name].empty())
        {
            N["alias_rows_unnamed"]++;
            continue;
        }
        if (L.Subtitle >= 0) R[Col::Subtitle] = Z.Text(E + L.Subtitle);
        if (L.Secondary >= 0) R[Col::Secondary] = Z.Text(E + L.Secondary);
        const int64_t Spec = L.SoundFile >= 0 ? Z.Follow(E + L.SoundFile) : -1;
        const std::string FileName = File(Name, J, Spec);
        if (!FileName.empty())
        {
            R[Col::FileSpec] = FileName;
            std::replace(R[Col::FileSpec].begin(), R[Col::FileSpec].end(), '/', (char)92);
        }
        else N["alias_files_unnamed"]++;
        if (Spec >= 0) R[Col::Storage] = SoundAliasCsv::Named(Z.At<uint8_t>(Spec), Storages);

        if (L.VolMin >= 0) R[Col::VolMin] = SoundAliasCsv::Volume(F(E, L.VolMin));
        if (L.VolMax >= 0) R[Col::VolMax] = SoundAliasCsv::Volume(F(E, L.VolMax));
        if (L.PitchMin >= 0) R[Col::PitchMin] = Pitch(F(E, L.PitchMin));
        if (L.PitchMax >= 0) R[Col::PitchMax] = Pitch(F(E, L.PitchMax));
        if (L.DistMin >= 0) R[Col::DistMin] = Inches(F(E, L.DistMin));
        if (L.DistMax >= 0) R[Col::DistMaxDry] = Inches(F(E, L.DistMax));
        if (L.DistReverbMax >= 0) R[Col::DistMaxWet] = Inches(F(E, L.DistReverbMax));
        if (L.Flags >= 0)
        {
            const uint32_t Flags = Z.At<uint32_t>(E + L.Flags);
            R[Col::Looping] = ((Flags >> L.LoopingBit) & ((1u << L.LoopingBits) - 1)) ? "looping" : "nonlooping";
            if (L.Is3DBit >= 0) R[Col::PanType] = ((Flags >> L.Is3DBit) & 1) ? "3d" : "2d";
            if (L.FlagsType >= 0)
            {
                constexpr const char* Types[] = { "loaded", "loaded", "streamed", "primed" };
                R[Col::Storage] = Types[(Flags >> L.FlagsType) & 3];
            }
            if (L.ChannelPan != nullptr)
            {
                const uint32_t Channel = (Flags >> L.ChannelShift) & 0xff;
                const char Pan = Channel < std::strlen(L.ChannelPan) ? L.ChannelPan[Channel] : '-';
                if (Pan == '2' || Pan == '3') R[Col::PanType] = Pan == '3' ? "3d" : "2d";
                else N["alias_pan_unknown"]++;
            }
        }
        if (L.Probability >= 0) R[Col::Probability] = SoundAliasCsv::Number(std::clamp(F(E, L.Probability), 0.0f, 1.0f));
        if (L.CenterPercentage >= 0) R[Col::CenterSend] = SoundAliasCsv::Volume(F(E, L.CenterPercentage));
        if (L.StartDelay >= 0) R[Col::StartDelay] = std::to_string(Z.At<int32_t>(E + L.StartDelay));
        if (L.EnvelopMin >= 0) R[Col::EnvelopMin] = Inches(F(E, L.EnvelopMin));
        if (L.EnvelopMax >= 0) R[Col::EnvelopMax] = Inches(F(E, L.EnvelopMax));
        if (L.EnvelopPercentage >= 0) R[Col::EnvelopPercent] = SoundAliasCsv::Volume(F(E, L.EnvelopPercentage));
        if (L.AllowDoppler >= 0) R[Col::Doppler] = SoundAliasCsv::YesNo(Z.At<uint8_t>(E + L.AllowDoppler) != 0);

        if (L.MinPriority >= 0) R[Col::PriorityMin] = Rounded(std::clamp(F(E, L.MinPriority), 0.0f, 255.0f));
        if (L.MaxPriority >= 0) R[Col::PriorityMax] = Rounded(std::clamp(F(E, L.MaxPriority), 0.0f, 255.0f));
        if (L.MinPriorityThreshold >= 0) R[Col::PriorityThresholdMin] = SoundAliasCsv::Number(std::clamp(F(E, L.MinPriorityThreshold), 0.0f, 1.0f));
        if (L.MaxPriorityThreshold >= 0) R[Col::PriorityThresholdMax] = SoundAliasCsv::Number(std::clamp(F(E, L.MaxPriorityThreshold), 0.0f, 1.0f));
        if (L.ReverbSend >= 0) R[Col::ReverbSend] = SoundAliasCsv::Volume(F(E, L.ReverbSend));
        if (L.OcclusionLevel >= 0) R[Col::OcclusionLevel] = SoundAliasCsv::Number(std::clamp(F(E, L.OcclusionLevel), 0.0f, 1.0f));
        if (L.LimitType >= 0)
        {
            R[Col::LimitType] = SoundAliasCsv::Named(Z.At<uint32_t>(E + L.LimitType), LimitTypes);
            if (R[Col::LimitType].empty()) N["alias_enums_unknown"]++;
        }
        if (L.EntityLimitCount >= 0) R[Col::EntityLimitCount] = std::to_string(std::min<uint32_t>(Z.At<uint32_t>(E + L.EntityLimitCount), 255));
        if (L.RandomizeType >= 0)
        {
            // snd_randomize_type_t: entity volume 1, pitch 2, variant 4 (0: per instance)
            const uint32_t Random = Z.At<uint32_t>(E + L.RandomizeType);
            std::string S;
            for (const auto& [Bit, Text] : { std::pair{ 1u, "volume" }, std::pair{ 2u, "pitch" }, std::pair{ 4u, "variant" } })
                if (Random & Bit) S += (S.empty() ? "" : " ") + std::string(Text);
            R[Col::RandomizeType] = S;
        }
        Out.Aliases.push_back(std::move(R));
    }
}

void ReadLive(const SoundAliasCsv::ZoneView& Z, const std::vector<std::pair<int64_t, std::string>>& Lists, const Layout& L,
              SoundAliasCsv::Rows& Out)
{
    std::map<int64_t, std::string> Seen;
    const std::string Extension = IWPakSounds::Extension();
    const FileOf Named = [&](const std::string& Name, uint32_t Index, int64_t File) {
        const std::string S = LiveName(Z, L, Name, Index, File, Seen);
        return S.empty() ? S : ExportNaming::SafeFileName(S) + Extension;
    };
    for (const auto& [Pos, Name] : Lists) Read(Z, Pos, Name, L, Named, Out);
}
}
