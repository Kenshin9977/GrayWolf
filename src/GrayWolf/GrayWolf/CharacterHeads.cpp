#include "stdafx.h"

#include "CharacterHeads.h"

#include "DiskGame.h"
#include "ExportNaming.h"

#include <algorithm>
#include <map>
#include <mutex>

namespace fs = std::filesystem;

namespace CharacterHeads
{
    void Add(Pairs& Out, const std::string& Body, const std::vector<std::string>& Heads)
    {
        const std::string B = ExportNaming::Lower(Body);
        if (B.empty() || Heads.empty()) return;
        auto It = std::find_if(Out.begin(), Out.end(), [&](const auto& P) { return P.first == B; });
        if (It == Out.end()) It = Out.insert(Out.end(), { B, {} });
        auto& List = It->second;
        for (const auto& H : Heads)
            if (const std::string L = ExportNaming::Lower(H); !L.empty() && L != B && std::find(List.begin(), List.end(), L) == List.end())
                List.push_back(L);
        if (List.empty()) Out.erase(It);
    }

    std::vector<std::shared_ptr<const Documents>> DocumentsOf(DiskGame::IDiskZone& Asking, const std::vector<fs::path>& Paths,
                                                              const Opener& Open)
    {
        struct Read
        {
            uintmax_t Size = 0;
            fs::file_time_type When;
            std::shared_ptr<const Documents> Docs;
        };
        static std::mutex Lock;
        static std::map<std::wstring, Read> Kept;
        std::vector<std::shared_ptr<const Documents>> Out;
        bool Parked = false;
        for (const auto& P : Paths)
        {
            std::error_code Ec, Ec2;
            const uintmax_t Size = fs::file_size(P, Ec);
            const auto When = fs::last_write_time(P, Ec2);
            if (Ec || Ec2) continue;
            std::lock_guard<std::mutex> G(Lock);
            if (const auto It = Kept.find(P.wstring()); It != Kept.end() && It->second.Size == Size && It->second.When == When)
            {
                Out.push_back(It->second.Docs);
                continue;
            }
            if (!Parked && !(Parked = Asking.Park())) break;
            auto Docs = std::make_shared<Documents>();
            if (auto Z = Open(P.string())) Z->Documents(*Docs);
            Kept[P.wstring()] = { Size, When, Docs };
            Out.push_back(std::move(Docs));
        }
        if (Parked) Asking.Resume();
        return Out;
    }
}
