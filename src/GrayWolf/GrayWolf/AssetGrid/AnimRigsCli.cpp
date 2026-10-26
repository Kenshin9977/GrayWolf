#include "stdafx.h"

// anim-rigs <dir>: the rig the browser picks for each anim of a folder (AnimRigs.h), with its
// score, and how many anims have one: what the Assets grid's anim cells play on, without a window.
#include "AnimRigs.h"
#include "CastReader.h"   // CastFile::FileKind
#include "CliCommands.h"
#include "CliSupport.h"   // CliReport

#include <algorithm>
#include <array>
#include <chrono>
#include <filesystem>
#include <format>
#include <map>
#include <sstream>

namespace fs = std::filesystem;

int RunAnimRigs(const CliOptions& Options)
{
    const auto Start = std::chrono::steady_clock::now();
    std::vector<AnimRigs::ModelRef> Models;
    std::vector<std::string> Anims;
    // --name-filter: only the anims holding one of its words are picked (every model still counts)
    std::vector<std::string> Filter;
    {
        std::stringstream Ss(Options.NameFilter);
        std::string W;
        while (std::getline(Ss, W, ','))
            if (!W.empty())
            {
                for (auto& c : W) c = (char)tolower((unsigned char)c);
                Filter.push_back(W);
            }
    }
    const auto Wanted = [&](std::string Name) {
        if (Filter.empty()) return true;
        for (auto& c : Name) c = (char)tolower((unsigned char)c);
        for (const auto& W : Filter) if (Name.find(W) != std::string::npos) return true;
        return false;
    };
    std::error_code Ec;
    for (auto It = fs::recursive_directory_iterator(Options.AnimRigs, Ec); It != fs::recursive_directory_iterator(); It.increment(Ec))
    {
        if (Ec) break;
        if (!It->is_regular_file(Ec)) continue;
        std::string Ext = It->path().extension().string();
        for (auto& c : Ext) c = (char)tolower((unsigned char)c);
        const std::string Path = It->path().string();
        const bool Cast = Ext == ".cast";
        const CastFile::Kind K = Cast ? CastFile::FileKind(Path) : CastFile::Kind::None;
        if (Ext == ".semodel" || Ext == ".xmodel_export" || K == CastFile::Kind::Model) Models.push_back({ Path, It->path().stem().string() });
        else if ((Ext == ".seanim" || K == CastFile::Kind::Animation) && Wanted(It->path().stem().string())) Anims.push_back(Path);
    }
    std::sort(Anims.begin(), Anims.end());

    AnimRigs::Index Rigs;
    Rigs.SetFetchReferences(!Options.AnimRigsNoFetch);
    Rigs.SetModels(std::move(Models), 1);
    // asked until no pick moves: a game's references exported midway have its anims picked again
    std::vector<AnimRigs::Pick> Picks(Anims.size());
    for (int Round = 0; Round < 50; Round++)
    {
        const uint64_t Gen = Rigs.Generation();
        size_t Known = 0;
        for (size_t i = 0; i < Anims.size(); i++) Known += Rigs.PickFor(Anims[i], Picks[i]);
        Rigs.WaitIdle();
        if (Known == Anims.size() && Rigs.Generation() == Gen) break;
    }

    std::map<std::string, std::array<size_t, 4>> PerGame;   // anims, with a rig, scoring 0.8 or more, whose own model is absent
    std::string Lines;
    size_t Rigged = 0, Good = 0, Absent = 0;
    for (size_t i = 0; i < Anims.size(); i++)
    {
        const AnimRigs::Pick& P = Picks[i];
        auto& G = PerGame[AnimRigs::GameOf(Anims[i])];
        G[0]++;
        G[1] += !P.Paths.empty();
        G[2] += !P.Paths.empty() && P.Score >= 0.8f;
        G[3] += !P.Paths.empty() && P.OwnModelAbsent;
        Rigged += !P.Paths.empty();
        Good += !P.Paths.empty() && P.Score >= 0.8f;
        Absent += !P.Paths.empty() && P.OwnModelAbsent;
        std::string Paths;
        for (const auto& Pth : P.Paths) Paths += (Paths.empty() ? "" : "|") + Pth;
        // the next best, when only a few anims are asked for
        std::vector<AnimRigs::Candidate> Ranked;
        if (!Filter.empty() && Rigs.Ranked(Anims[i], Ranked))
            for (size_t k = 0; k < Ranked.size() && k < 4; k++) Paths += std::format("\t{:.2f} {}", Ranked[k].Score, Ranked[k].Path);
        // last: the share of its bones no model has, and its own model absent
        Lines += std::format("anim={}\t{:.2f}\t{}{}\t{}\t{}\tunmodelled={:.2f}{}\n", fs::path(Anims[i]).stem().string(), P.Score,
                             P.Paths.empty() ? std::string("-") : P.Label, P.Reference ? "\t(reference)" : "", Anims[i], Paths, P.Unmodelled,
                             P.OwnModelAbsent ? "\town_model_absent" : "");
    }
    std::string Report = std::format("result=ok\nanims={}\nrigged={}\nscore_0_8={}\nown_model_absent={}\nrate_0_8={:.3f}\nseconds={:.1f}\n", Anims.size(),
                                     Rigged, Good, Absent, Anims.empty() ? 0.0 : (double)Good / (double)Anims.size(),
                                     std::chrono::duration<double>(std::chrono::steady_clock::now() - Start).count());
    for (const auto& [Game, N] : PerGame)
        Report += std::format("game={}\tanims={}\trigged={}\tscore_0_8={}\town_model_absent={}\n", Game.empty() ? "-" : Game, N[0], N[1], N[2], N[3]);
    Report += "stats=" + Rigs.Stats() + "\n" + Lines;
    CliReport(Options, Report);
    return 0;
}
