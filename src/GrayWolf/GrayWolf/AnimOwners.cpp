#include "stdafx.h"

#include "AnimOwners.h"

#include "ExportNaming.h"
#include "Strings.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <set>

namespace fs = std::filesystem;

namespace AnimOwners
{
    namespace
    {
        constexpr const char* Format = "graywolf-anim-owners";
        constexpr int Version = 1;
    }

    void Book::Own(const std::string& Anim, const std::string& Id)
    {
        auto& Ids = Anims[Anim];
        if (std::find(Ids.begin(), Ids.end(), Id) == Ids.end()) Ids.push_back(Id);
    }

    void Book::KeepOnly(const std::vector<std::string>& Keep)
    {
        std::set<std::string> Names;
        for (const auto& N : Keep) Names.insert(ExportNaming::Lower(N));
        for (auto It = Anims.begin(); It != Anims.end();)
            It = Names.count(ExportNaming::Lower(It->first)) ? std::next(It) : Anims.erase(It);
        std::set<std::string> Named;
        for (const auto& [Anim, Ids] : Anims) Named.insert(Ids.begin(), Ids.end());
        for (auto It = Owners.begin(); It != Owners.end();)
            It = Named.count(It->first) ? std::next(It) : Owners.erase(It);
    }

    std::string FileIn(const std::string& AnimsDir)
    {
        return Strings::ToNormalString((fs::path(Strings::ToUnicodeString(AnimsDir)) / "anim_owners.json").wstring());
    }

    bool Read(const std::string& File, Book& Out, std::string* Game)
    {
        Out = Book{};
        try
        {
            std::ifstream In(fs::path(Strings::ToUnicodeString(File)), std::ios::binary);
            if (!In) return false;
            const nlohmann::json J = nlohmann::json::parse(In);
            if (J.value("format", std::string()) != Format || J.value("version", 0) > Version) return false;
            if (Game) *Game = J.value("game", std::string());
            // read in place (value() copies: a 3.5 MB file's tree twice)
            if (const auto Owners = J.find("owners"); Owners != J.end() && Owners->is_object())
                for (auto It = Owners->begin(); It != Owners->end(); ++It)
                {
                    Owner& W = Out.Owners.try_emplace(Out.Owners.end(), It.key())->second;
                    if (const auto M = It->find("models"); M != It->end() && M->is_array())
                    {
                        W.Models.reserve(M->size());
                        for (const auto& Name : *M) W.Models.push_back(Name.get<std::string>());
                    }
                    if (const auto S = It->find("source"); S != It->end() && S->is_string()) W.Source = S->get<std::string>();
                }
            if (const auto Anims = J.find("anims"); Anims != J.end() && Anims->is_object())
                for (auto It = Anims->begin(); It != Anims->end(); ++It)
                {
                    auto& Ids = Out.Anims.try_emplace(Out.Anims.end(), It.key())->second;
                    Ids.reserve(It->size());
                    for (const auto& Id : *It) Ids.push_back(Id.get<std::string>());
                }
            return true;
        }
        catch (const std::exception&)
        {
            Out = Book{};
            return false;
        }
    }

    bool Merge(const std::string& AnimsDir, const std::string& Game, const Book& New, std::string* Why)
    {
        const std::string Name = FileIn(AnimsDir);
        const fs::path File(Strings::ToUnicodeString(Name));
        Book All;
        std::error_code Ec;
        if (fs::exists(File, Ec) && !Read(Name, All))
            All = Book{};   // not ours or broken: written anew
        // an anim's entry replaced whole, under the name an earlier run gave it when only the case differs
        std::map<std::string, std::string> ByLower;
        for (const auto& [Anim, Ids] : All.Anims) ByLower[ExportNaming::Lower(Anim)] = Anim;
        for (const auto& [Anim, Ids] : New.Anims)
        {
            if (const auto It = ByLower.find(ExportNaming::Lower(Anim)); It != ByLower.end() && It->second != Anim) All.Anims.erase(It->second);
            All.Anims[Anim] = Ids;
        }
        // an owner given again: its source replaced, its models the earlier ones and the new ones
        // not among them (an animtree's bodies and heads met in several zones)
        for (const auto& [Id, O] : New.Owners)
        {
            Owner& W = All.Owners[Id];
            W.Source = O.Source;
            for (const auto& M : O.Models)
                if (std::none_of(W.Models.begin(), W.Models.end(), [&](const std::string& Had) { return _stricmp(Had.c_str(), M.c_str()) == 0; }))
                    W.Models.push_back(M);
        }
        std::set<std::string> Named;
        for (const auto& [Anim, Ids] : All.Anims) Named.insert(Ids.begin(), Ids.end());
        for (auto It = All.Owners.begin(); It != All.Owners.end();)
            It = Named.count(It->first) ? std::next(It) : All.Owners.erase(It);

        nlohmann::ordered_json J;
        J["format"] = Format;
        J["version"] = Version;
        J["game"] = Game;
        nlohmann::ordered_json Owners = nlohmann::ordered_json::object(), Anims = nlohmann::ordered_json::object();
        for (const auto& [Id, O] : All.Owners) Owners[Id] = { { "models", O.Models }, { "source", O.Source } };
        for (const auto& [Anim, Ids] : All.Anims) Anims[Anim] = Ids;
        J["owners"] = std::move(Owners);
        J["anims"] = std::move(Anims);
        std::string Text;
        try
        {
            Text = J.dump(1, '\t', false, nlohmann::json::error_handler_t::replace) + "\n";
        }
        catch (const std::exception& E)
        {
            if (Why) *Why = E.what();
            return false;
        }
        fs::create_directories(File.parent_path(), Ec);
        const fs::path Temp = File.wstring() + L"." + std::to_wstring(GetCurrentProcessId()) + L".tmp";
        {
            std::ofstream O(Temp, std::ios::binary);
            O.write(Text.data(), (std::streamsize)Text.size());
            if (!O)
            {
                if (Why) *Why = "cannot write " + Strings::ToNormalString(Temp.wstring());
                O.close();
                fs::remove(Temp, Ec);
                return false;
            }
        }
        if (!MoveFileExW(Temp.c_str(), File.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
        {
            if (Why) *Why = "cannot replace " + Name + " (" + std::to_string(GetLastError()) + ")";
            fs::remove(Temp, Ec);
            return false;
        }
        return true;
    }
}
