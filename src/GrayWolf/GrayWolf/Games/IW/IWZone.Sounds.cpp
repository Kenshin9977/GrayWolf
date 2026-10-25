#include "stdafx.h"

// IWZone's sound kind for the titles whose live alias readers find the sounds (Ghosts, Advanced
// Warfare, MWR): the samples from the install's soundfile<N>.pak, then the alias CSV.

#include "Games/IW/IWZone.h"

#include "GameWorldWar2.h"
#include "Games/IW/IWPakSounds.h"
#include "Games/IW/IWSoundAliases.h"
#include "PAKCache.h"

void IWZone::ExportPakSounds(const DiskGame::Context& C, DiskGame::Totals& T, const std::string& Install, bool EmptyFileFails,
                             const IWSoundAliases::Layout& Aliases, const AliasSounds& Add)
{
    std::set<uint64_t> Unique;
    std::vector<CoDAsset_t*> Found;
    std::vector<std::pair<int64_t, std::string>> Lists;
    for (const auto& I : Walk().Instances)
    {
        if (!OwnerIs(I, "sound")) continue;
        const std::string AliasName = AssetName(DiskGame::Kind::Sound, I.Pos);
        if (IsReference(DiskGame::Kind::Sound, AliasName, I.Pos)) continue;
        Add(VirtualGameSession::Address((uint64_t)I.Pos), AliasName, Unique, Found);
        Lists.emplace_back(I.Pos, AliasName);
    }
    PAKCache Packages;
    Packages.LoadPackageCache(Install);
    IWPakSounds::WriteOptions Options;
    Options.EmptyFileFails = EmptyFileFails;
    Options.Language = GameWorldWar2::LanguageOfZone(FastFilePath());
    IWPakSounds::Write(Found, &Packages, C, T, Name(), [this](const std::string& N) -> size_t& { return Counter(N); }, Options);

    if (!SoundAliasCsv::Wanted(C)) return;
    SoundAliasCsv::Rows Rows;
    IWSoundAliases::ReadLive(AliasView(), Lists, Aliases, Rows);
    SoundAliasCsv::Write(C, T, Name(), Rows);
}
