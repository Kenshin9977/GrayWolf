#include "stdafx.h"

#include "TreyarchGscOwners.h"

#include "AnimOwners.h"
#include "ExportNaming.h"

#include <algorithm>
#include <map>
#include <memory>
#include <mutex>

namespace fs = std::filesystem;

namespace TreyarchGscOwners
{
    namespace
    {
        using GscAnimOwners::Facts;

        // a shared zone's facts, read once per file (its size and time unchanged)
        std::shared_ptr<const Facts> SharedFacts(const DiskGame::IDiskGame& Game, const DiskGame::ExportOptions& Options,
                                                 const fs::path& Zone, const GscAnimOwners::Rules& R)
        {
            struct Kept
            {
                uintmax_t Size = 0;
                fs::file_time_type When;
                std::shared_ptr<const Facts> F;
            };
            static std::mutex Lock;
            static std::map<std::wstring, Kept> Cache;
            std::error_code Ec, Ec2;
            const uintmax_t Size = fs::file_size(Zone, Ec);
            const auto When = fs::last_write_time(Zone, Ec2);
            if (Ec || Ec2) return nullptr;
            const std::wstring Key = Zone.wstring();
            {
                std::lock_guard<std::mutex> G(Lock);
                if (const auto It = Cache.find(Key); It != Cache.end() && It->second.Size == Size && It->second.When == When) return It->second.F;
            }
            std::string Error;
            std::vector<std::pair<std::string, std::string>> Docs;
            if (auto Z = Game.Open(Zone.string(), Options, Error)) Z->Documents(Docs);
            auto F = std::make_shared<const Facts>(GscAnimOwners::Parse(Docs, R));
            std::lock_guard<std::mutex> G(Lock);
            Cache[Key] = { Size, When, F };
            return F;
        }

        bool Fits(const char* Name, const char* Pattern)
        {
            if (*Pattern == 0) return *Name == 0;
            if (*Pattern == '*') return Fits(Name, Pattern + 1) || (*Name && Fits(Name + 1, Pattern));
            return *Name && tolower((unsigned char)*Name) == tolower((unsigned char)*Pattern) && Fits(Name + 1, Pattern + 1);
        }
    }

    fs::path FirstZone(const fs::path& Dir, const char* Pattern)
    {
        std::vector<fs::path> Found;
        std::error_code Ec;
        for (fs::directory_iterator It(Dir, Ec), End; !Ec && It != End; It.increment(Ec))
            if (ExportNaming::Lower(It->path().extension().string()) == ".ff" && Fits(It->path().stem().string().c_str(), Pattern))
                Found.push_back(It->path());
        if (Found.empty()) return {};
        return *std::min_element(Found.begin(), Found.end(), [](const fs::path& A, const fs::path& B) {
            return ExportNaming::Lower(A.filename().string()) < ExportNaming::Lower(B.filename().string());
        });
    }

    void Fill(const DiskGame::IDiskGame& Game, const DiskGame::ExportOptions& Options,
              const std::vector<DiskGame::IDiskGame::AnimZone>& Zones, AnimOwners::Book& Out, const Setup& S)
    {
        for (const auto& Z : Zones)
        {
            const Facts Own = GscAnimOwners::Parse(Z.Documents, S.Rules);
            const fs::path Path(Z.Zone);
            std::vector<std::shared_ptr<const Facts>> Shared;
            std::error_code Ec;
            std::vector<fs::path> With = S.Shared ? S.Shared(Path) : std::vector<fs::path>{};
            // a level's patch (<level>_patch) loads with its level
            if (const std::string Stem = ExportNaming::Lower(Path.stem().string()); Stem.size() > 6 && Stem.compare(Stem.size() - 6, 6, "_patch") == 0)
                With.insert(With.begin(), Path.parent_path() / (Path.stem().string().substr(0, Stem.size() - 6) + ".ff"));
            for (const auto& P : With)
                if (!P.empty() && fs::exists(P, Ec) && ExportNaming::Lower(P.stem().string()) != ExportNaming::Lower(Path.stem().string()))
                    if (auto F = SharedFacts(Game, Options, P, S.Rules)) Shared.push_back(std::move(F));
            std::vector<const Facts*> Sources{ &Own };
            for (const auto& F : Shared) Sources.push_back(F.get());
            GscAnimOwners::Own(Sources, Z.Anims, Out, S.Options);
        }
    }
}

