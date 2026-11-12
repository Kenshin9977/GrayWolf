#include "stdafx.h"

// DiskGame::LevelNames: an install's level zones by their display names, read from the game's
// own localized strings (IDiskGame::LevelNames says where). One text file per game and install
// beside the install index: a header with the key, then a "<zone>\t<name>" line per named zone.

#include "DiskGame.h"
#include "ExportNaming.h"
#include "ExportWriters.h"
#include "Strings.h"

#include <filesystem>
#include <fstream>
#include <set>

namespace fs = std::filesystem;

namespace DiskGame
{
namespace
{
    // bumped when what Lookup finds changes for the same rules and strings
    constexpr int Format = 2;

    using ExportNaming::Lower;

    std::string Upper(std::string S)
    {
        for (auto& C : S) C = (char)std::toupper((unsigned char)C);
        return S;
    }

    // Name matched against Pattern ('*' any run, the shortest first, '?' one character),
    // case-insensitively; Stars is what each '*' matched
    bool Match(const char* P, const char* N, std::vector<std::string>* Stars)
    {
        if (*P == 0) return *N == 0;
        if (*P == '*')
        {
            const size_t Mark = Stars != nullptr ? Stars->size() : 0;
            for (const char* At = N;; At++)
            {
                if (Stars != nullptr) Stars->emplace_back(N, At);
                if (Match(P + 1, At, Stars)) return true;
                if (Stars != nullptr) Stars->resize(Mark);
                if (*At == 0) return false;
            }
        }
        if (*N == 0) return false;
        if (*P != '?' && std::tolower((unsigned char)*P) != std::tolower((unsigned char)*N)) return false;
        return Match(P + 1, N + 1, Stars);
    }

    std::string Stem(const std::string& Zone)
    {
        return Lower(fs::path(Zone).stem().string());
    }


    // the names a zone's own is tried off: a language, localized_ or patch_ prefix, a _patch or
    // _load suffix, a part number (angola_2 is Angola's)
    std::vector<std::string> Reduced(const std::string& Stem)
    {
        static const char* Languages[] = { "en", "eng", "fr", "fra", "de", "ger", "it", "ita", "es", "spa", "ru", "rus",
                                           "pl", "pol", "ja", "jpn", "ko", "kor", "pt", "por", "ar", "ara", "zh", "chs",
                                           "cht", "tc", "sc", "ea", "ge", "po" };
        std::vector<std::string> Out;
        const auto Strip = [&](const std::string& S) {
            std::vector<std::string> R;
            const size_t U = S.find('_');
            if (U != std::string::npos)
                for (const char* L : Languages)
                    if (S.compare(0, U, L) == 0) R.push_back(S.substr(U + 1));
            for (const char* P : { "localized_", "patch_" })
                if (S.rfind(P, 0) == 0) R.push_back(S.substr(strlen(P)));
            for (const char* P : { "_patch", "_load" })
                if (S.size() > strlen(P) && S.compare(S.size() - strlen(P), strlen(P), P) == 0) R.push_back(S.substr(0, S.size() - strlen(P)));
            const size_t Last = S.rfind('_');
            if (Last != std::string::npos && Last + 1 < S.size() && Last > 0 &&
                S.find_first_not_of("0123456789", Last + 1) == std::string::npos)
                R.push_back(S.substr(0, Last));
            return R;
        };
        std::set<std::string> Seen{ Stem };
        std::vector<std::string> Round{ Stem };
        for (int Depth = 0; Depth < 2; Depth++)
        {
            std::vector<std::string> Next;
            for (const auto& S : Round)
                for (auto& R : Strip(S))
                    if (!R.empty() && Seen.insert(R).second) Next.push_back(R), Out.push_back(R);
            Round = std::move(Next);
        }
        return Out;
    }

    // Key's text, "" for none: as it is, or as the zones name it hashed
    std::string Text(const IDiskGame::LevelNaming& Rules, const std::string& Key, const std::map<std::string, std::string>& Strings)
    {
        if (const auto It = Strings.find(Upper(Key)); It != Strings.end() && !It->second.empty()) return It->second;
        if (Rules.Hashed != nullptr)
            if (const auto It = Strings.find(Upper(Rules.Hashed(Key))); It != Strings.end() && !It->second.empty()) return It->second;
        return std::string();
    }

    // An .arena longname in a level list, marked: the name itself when no string has it as key
    constexpr char LongName = '=';

    // The text of the first of a listed map's keys that has one, "" for none; an .arena longname
    // that is no key and not the map's own file name is the name (CoD2's "Carentan, France")
    std::string FromList(const IDiskGame::LevelNaming& Rules, const std::string& Map, const std::map<std::string, std::string>& Strings,
                         const std::map<std::string, std::vector<std::string>>& Listed)
    {
        const auto It = Listed.find(Map);
        if (It == Listed.end()) return std::string();
        for (const auto& K : It->second)
        {
            const bool Long = !K.empty() && K[0] == LongName;
            const std::string Key = Long ? K.substr(1) : K;
            if (std::string Found = Text(Rules, Key, Strings); !Found.empty()) return Found;
            if (Long && Lower(Key) != Map && Key.find_first_not_of("ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_") != std::string::npos)
                return Key;
        }
        return std::string();
    }

    // Name's level name: by the game's own level list, else by the rules (a pattern's '*' naming
    // a listed map giving that map's name: a mode's addon zone), "" for none
    std::string Direct(const IDiskGame::LevelNaming& Rules, const std::string& Name,
                       const std::map<std::string, std::string>& Strings,
                       const std::map<std::string, std::vector<std::string>>& Listed)
    {
        if (std::string Found = FromList(Rules, Name, Strings, Listed); !Found.empty()) return Found;
        for (const auto& [Pattern, Keys] : Rules.Keys)
        {
            std::vector<std::string> Stars;
            if (!Match(Pattern.c_str(), Name.c_str(), &Stars)) continue;
            Stars.resize(std::max<size_t>(Stars.size(), 2));
            for (std::string Key : Keys)
            {
                for (const auto& [From, To] : { std::pair<const char*, std::string>{ "{NAME}", Upper(Name) },
                                                { "{REST}", Upper(Stars[0]) }, { "{REST2}", Upper(Stars[1]) } })
                    for (size_t At; (At = Key.find(From)) != std::string::npos;) Key.replace(At, strlen(From), To);
                // and "_2020" written "2020" (BO2's PRESENCE_MP_NUKETOWN2020)
                std::string Joined;
                for (size_t I = 0; I < Key.size(); I++)
                    if (!(Key[I] == '_' && I + 1 < Key.size() && std::isdigit((unsigned char)Key[I + 1]))) Joined += Key[I];
                for (const auto& K : { Key, Joined })
                    if (std::string Found = Text(Rules, K, Strings); !Found.empty()) return Found;
            }
            for (const auto& Star : Stars)
                if (std::string Found = FromList(Rules, Star, Strings, Listed); !Found.empty()) return Found;
        }
        return std::string();
    }

    // A level list's entries: each map it names (lower case) and the keys its name may be. An
    // .arena rawfile's blocks ({ map "mp_brecourt" longname "MPUI_WASTELAND" ... }: the longname,
    // marked), else a CSV's rows (a row's map its first cell naming a zone, the other cells).
    void ReadLevelList(const std::string& Name, const std::string& Text, const std::set<std::string>& Zones,
                       std::map<std::string, std::vector<std::string>>& Out)
    {
        const bool Arena = Name.size() > 6 && Lower(Name).compare(Name.size() - 6, 6, ".arena") == 0;
        std::vector<std::vector<std::string>> Entries(1);
        if (Arena)
        {
            // each { } block's field "value" pairs
            std::string Map, Long, Field;
            for (size_t I = 0; I < Text.size(); I++)
            {
                if (Text[I] == '}')
                {
                    if (!Map.empty() && !Long.empty()) Out[Lower(Map)].push_back(LongName + Long);
                    Map.clear(), Long.clear();
                }
                if (std::isalpha((unsigned char)Text[I]))
                {
                    const size_t End = Text.find_first_of(" \t\r\n\"", I);
                    Field = Lower(Text.substr(I, End == std::string::npos ? std::string::npos : End - I));
                    if (End == std::string::npos) break;
                    I = End - 1;
                    continue;
                }
                if (Text[I] != '"') continue;
                const size_t End = Text.find('"', I + 1);
                if (End == std::string::npos) break;
                (Field == "map" ? Map : Field == "longname" ? Long : Field) = Text.substr(I + 1, End - I - 1);
                I = End;
            }
            return;
        }
        else
        {
            std::string Cell;
            for (const char C : Text)
            {
                if (C == ',' || C == '\n') Entries.back().push_back(std::move(Cell)), Cell.clear();
                else if (C != '\r') Cell += C;
                if (C == '\n') Entries.emplace_back();
            }
        }
        for (const auto& E : Entries)
            for (size_t K = 0; K < E.size(); K++)
            {
                const std::string Map = Lower(E[K]);
                if (Zones.count(Map) == 0) continue;
                auto& Keys = Out[Map];
                for (size_t J = 0; J < E.size(); J++)
                    if (J != K && !E[J].empty() && E[J].find(' ') == std::string::npos) Keys.push_back(E[J]);
                break;
            }
    }
}

std::string LevelNames::Lookup(const IDiskGame::LevelNaming& Rules, const std::string& Zone,
                               const std::map<std::string, std::string>& Strings,
                               const std::map<std::string, std::vector<std::string>>& Listed)
{
    const std::string Name = Lower(Zone);
    if (std::string Found = Direct(Rules, Name, Strings, Listed); !Found.empty()) return Found;
    for (const auto& R : Reduced(Name))
        if (std::string Found = Direct(Rules, R, Strings, Listed); !Found.empty()) return Found;
    return std::string();
}

std::string LevelNames::Of(const std::string& Zone) const
{
    const auto It = Names.find(Zone);
    return It == Names.end() ? std::string() : It->second;
}

std::unique_ptr<LevelNames> LevelNames::For(const IDiskGame& Game, const ExportOptions& Options, const std::string& Install,
                                            const std::function<void(size_t, size_t)>& Step, std::string& Error)
{
    const IDiskGame::LevelNaming* Rules = Game.LevelNames();
    if (Rules == nullptr) return nullptr;
    std::vector<std::string> Zones;
    if (!Game.ListZones(Install, Options, Zones, Error)) return nullptr;

    std::string KeyText = "format " + std::to_string(Format) + "\n" + Game.Id() + "\n" + Lower(fs::absolute(Install).string()) + "\n";
    for (const auto& F : Rules->From) KeyText += "from " + F + "\n";
    for (const auto& L : Rules->Lists) KeyText += "list " + L + "\n";
    KeyText += "codepage " + std::to_string(Rules->CodePage) + (Rules->Hashed != nullptr ? "\nhashed" : "") + "\n";
    for (const auto& [P, Keys] : Rules->Keys)
        for (const auto& K : Keys) KeyText += "key " + P + " " + K + "\n";
    if (Rules->ByLevels) KeyText += "by levels\n";
    // the game's own settings (CoD1/CoD2's language)
    for (const Setting& S : Game.Settings()) KeyText += std::string("setting ") + S.Key + "=" + S.Of(Options) + "\n";
    for (const auto& Z : Zones) KeyText += Z + "\n";
    const std::string Key = ExportNaming::Hex16(ExportNaming::Fnv1a(KeyText));
    const std::string File = KeptFile(Game, Options, Install, ".names");

    auto Out = std::unique_ptr<LevelNames>(new LevelNames());
    {
        std::ifstream In(File, std::ios::binary);
        std::string Line;
        if (In && std::getline(In, Line) && Line == "greyhound-level-names " + std::to_string(Format) && std::getline(In, Line) &&
            Line == "key " + Key)
        {
            while (std::getline(In, Line))
                if (const size_t Tab = Line.find('\t'); Tab != std::string::npos) Out->Names[Line.substr(0, Tab)] = Line.substr(Tab + 1);
            return Out;
        }
    }

    // the zones holding the strings, in the order From names them, each once
    std::vector<std::string> Sources;
    std::set<std::string> Taken;
    for (const auto& Pattern : Rules->From)
        for (const auto& Z : Zones)
            if (Match(Pattern.c_str(), Stem(Z).c_str(), nullptr) && Taken.insert(Stem(Z)).second) Sources.push_back(Z);

    std::map<std::string, std::string> Strings;
    std::map<std::string, std::vector<std::string>> Listed;   // a level list's map -> its keys
    std::set<std::string> Stems;
    for (const auto& Z : Zones) Stems.insert(Stem(Z));
    for (size_t I = 0; I < Sources.size(); I++)
    {
        if (Step) Step(I, Sources.size());
        std::vector<std::string> Paths;
        std::string Why;
        if (!Game.ResolveZones(Install, { Sources[I] }, Options, Paths, Why) || Paths.empty()) continue;
        const auto Zone = Game.Open(Paths.front(), Options, Why);
        if (!Zone) continue;
        std::vector<std::pair<std::string, std::string>> Rows;
        Zone->Strings(Rows);
        std::vector<std::pair<std::string, std::string>> Docs;
        if (!Rules->Lists.empty()) Zone->Documents(Docs);
        for (const auto& [Doc, Text] : Docs)
            for (const auto& L : Rules->Lists)
                if (Lower(Doc) == Lower(L)) ReadLevelList(Doc, Text, Stems, Listed);
        for (auto& [K, Text] : Rows)
        {
            std::string T = Strings::Trim(ExportWriters::Utf8(Text, Rules->CodePage));
            for (auto& C : T)
                if (C == '\t' || C == '\n' || C == '\r') C = ' ';
            if (!K.empty() && !T.empty()) Strings[Upper(K)] = std::move(T);
        }
    }
    if (Step) Step(Sources.size(), Sources.size() + (Rules->ByLevels ? Zones.size() : 0));

    for (size_t I = 0; I < Zones.size(); I++)
    {
        const std::string& Z = Zones[I];
        if (!Rules->ByLevels)
        {
            if (std::string Name = Lookup(*Rules, Stem(Z), Strings, Listed); !Name.empty()) Out->Names[Z] = std::move(Name);
            continue;
        }
        // a container: the names of the levels it holds, those named first (the others by their
        // file name), the first three and how many more
        if (Step) Step(Sources.size() + I, Sources.size() + Zones.size());
        std::vector<std::string> Paths, Levels, Named, Unnamed;
        std::string Why;
        if (!Game.ResolveZones(Install, { Z }, Options, Paths, Why) || Paths.empty()) continue;
        if (const auto Zone = Game.Open(Paths.front(), Options, Why)) Zone->HeldLevels(Levels);
        for (const auto& L : Levels)
        {
            std::string Found = Lookup(*Rules, Lower(L), Strings, Listed);
            if (Found.empty()) Unnamed.push_back(L);
            else Named.push_back(std::move(Found));
        }
        Named.insert(Named.end(), Unnamed.begin(), Unnamed.end());
        std::string Name;
        for (size_t L = 0; L < Named.size() && L < 3; L++) Name += (L ? " / " : "") + Named[L];
        if (Named.size() > 3) Name += " +" + std::to_string(Named.size() - 3);
        if (!Name.empty()) Out->Names[Z] = std::move(Name);
    }
    Out->Built = true;
    if (Strings.empty()) return Out;   // no string read: tried again next time

    const std::string Temp = File + ".tmp";
    {
        std::ofstream Os(Temp, std::ios::binary | std::ios::trunc);
        Os << "greyhound-level-names " << Format << "\nkey " << Key << "\n";
        for (const auto& [Z, N] : Out->Names) Os << Z << "\t" << N << "\n";
    }
    std::error_code Ec;
    fs::rename(Temp, File, Ec);
    return Out;
}
}
