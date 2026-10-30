#include "stdafx.h"

#include "Languages.h"

#include "ExportNaming.h"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <mutex>
#include <optional>

namespace fs = std::filesystem;

namespace Languages
{
namespace
{
    // the folders above an install's own a zone or a picked folder may be (zone\all\x.ff, cod24\x.ff)
    constexpr int Up = 4;

    bool IsWordChar(char C) { return (C >= 'a' && C <= 'z') || (C >= '0' && C <= '9'); }

    // A protobuf message's fields (only varints and length-delimited ones are read; the others are
    // stepped over): Battle.net's product.db, ~30 bytes of format, no library
    struct Proto
    {
        std::string_view Bytes;
        size_t At = 0;
        bool Varint(uint64_t& V)
        {
            V = 0;
            for (int Shift = 0; Shift < 64 && At < Bytes.size(); Shift += 7)
            {
                const uint8_t B = (uint8_t)Bytes[At++];
                V |= (uint64_t)(B & 0x7f) << Shift;
                if (B < 0x80) return true;
            }
            return false;
        }
        // the next field: its number, and its bytes when length-delimited (else Data is empty)
        bool Next(uint32_t& Field, std::string_view& Data)
        {
            uint64_t Key = 0, V = 0;
            if (At >= Bytes.size() || !Varint(Key)) return false;
            Field = (uint32_t)(Key >> 3);
            Data = {};
            switch (Key & 7)
            {
            case 0: return Varint(V);
            case 1: At += 8; return At <= Bytes.size();
            case 5: At += 4; return At <= Bytes.size();
            case 2:
                if (!Varint(V) || V > Bytes.size() - At) return false;
                Data = Bytes.substr(At, (size_t)V);
                At += (size_t)V;
                return true;
            default: return false;
            }
        }
    };

    // ProductInstall { 3: UserSettings { 6: selected text language, 7: selected speech language,
    // 8: languages[] { 1: code, 2: option } } } (bartok765/galaxy_blizzard_plugin's product_db.proto)
    bool ReadProductDb(const fs::path& File, Detected& D)
    {
        std::ifstream In(File, std::ios::binary);
        const std::string Bytes((std::istreambuf_iterator<char>(In)), std::istreambuf_iterator<char>());
        if (Bytes.empty() || Bytes.size() > (1u << 20)) return false;
        Proto Top{ Bytes };
        uint32_t Field = 0;
        std::string_view Data;
        while (Top.Next(Field, Data))
        {
            if (Field != 3 || Data.empty()) continue;
            Proto Settings{ Data };
            std::string_view Value;
            while (Settings.Next(Field, Value))
            {
                if (Field == 6)
                {
                    D.Text = CodeOf(Value);
                    if (D.Raw.empty()) D.Raw = std::string(Value);
                }
                else if (Field == 7)
                {
                    D.Speech = CodeOf(Value);
                    D.Raw = std::string(Value);
                }
                else if (Field == 8)
                {
                    Proto Entry{ Value };
                    std::string_view Code;
                    while (Entry.Next(Field, Code))
                        if (Field == 1)
                        {
                            const std::string C = CodeOf(Code);
                            if (!C.empty() && std::find(D.Installed.begin(), D.Installed.end(), C) == D.Installed.end()) D.Installed.push_back(C);
                        }
                }
            }
            D.Source = "Battle.net";
            return !D.Main().empty() || !D.Installed.empty();
        }
        return false;
    }

    // A steamapps folder's app manifests as (installdir lower case, "language"), each one that has
    // both; read again once a few seconds old (a detection asks for each install of the library)
    std::vector<std::pair<std::string, std::string>> SteamManifests(const fs::path& Apps)
    {
        using Clock = std::chrono::steady_clock;
        struct Kept
        {
            Clock::time_point When;
            std::vector<std::pair<std::string, std::string>> Apps;
        };
        static std::mutex Lock;
        static std::map<std::string, Kept> Cache;
        const std::string Key = ExportNaming::Lower(Apps.string());
        {
            std::lock_guard<std::mutex> Guard(Lock);
            const auto It = Cache.find(Key);
            if (It != Cache.end() && Clock::now() - It->second.When < std::chrono::seconds(10)) return It->second.Apps;
        }
        // "key"  "value" lines: the second quoted word of the line whose first is the key
        const auto Value = [](const std::string& Line, const char* Name) -> std::optional<std::string> {
            const size_t A = Line.find('"');
            if (A == std::string::npos) return std::nullopt;
            const size_t B = Line.find('"', A + 1);
            if (B == std::string::npos || Line.compare(A + 1, B - A - 1, Name) != 0) return std::nullopt;
            const size_t C = Line.find('"', B + 1), E = C == std::string::npos ? C : Line.find('"', C + 1);
            if (E == std::string::npos) return std::nullopt;
            return Line.substr(C + 1, E - C - 1);
        };
        std::vector<std::pair<std::string, std::string>> Out;
        std::error_code Ec;
        for (const auto& E : fs::directory_iterator(Apps, Ec))
        {
            const std::string Name = ExportNaming::Lower(E.path().filename().string());
            if (Name.rfind("appmanifest_", 0) != 0 || E.path().extension() != ".acf") continue;
            std::ifstream In(E.path());
            std::string Line, Dir, Language;
            while (std::getline(In, Line) && (Dir.empty() || Language.empty()))
            {
                if (Dir.empty())
                    if (auto V = Value(Line, "installdir")) Dir = ExportNaming::Lower(*V);
                if (Language.empty())
                    if (auto V = Value(Line, "language")) Language = *V;
            }
            if (!Dir.empty() && !Language.empty()) Out.emplace_back(Dir, Language);
        }
        std::lock_guard<std::mutex> Guard(Lock);
        Cache[Key] = { Clock::now(), Out };
        return Out;
    }

    // <library>\steamapps\common\<dir>: the manifests in <library>\steamapps whose installdir is
    // <dir> (a game's single-player and multiplayer apps share it)
    bool ReadSteam(const fs::path& Dir, Detected& D)
    {
        const fs::path Common = Dir.parent_path(), Apps = Common.parent_path();
        if (ExportNaming::Lower(Common.filename().string()) != "common" || ExportNaming::Lower(Apps.filename().string()) != "steamapps")
            return false;
        const std::string Want = ExportNaming::Lower(Dir.filename().string());
        for (const auto& [InstallDir, Language] : SteamManifests(Apps))
        {
            if (InstallDir != Want) continue;
            const std::string C = CodeOf(Language);
            if (C.empty()) continue;
            if (D.Speech.empty())
            {
                D.Speech = D.Text = C;
                D.Raw = Language;
            }
            if (std::find(D.Installed.begin(), D.Installed.end(), C) == D.Installed.end()) D.Installed.push_back(C);
        }
        if (D.Speech.empty()) return false;
        D.Source = "Steam";
        return true;
    }

    thread_local const Choice* Running = nullptr;

    // A word of a file name (its first, before _ or a folder separator) as a language: a short code
    // or a language's name (fra, en, english), not a launcher's locale (frfr); "" for none
    std::string CodeOfWord(const std::string& First)
    {
        if (First.size() == 4) return {};
        // two letters only for the codes the games use: br_ is Blackout's, cp_, mp_, zm_ modes
        static const char* const Short[] = { "en", "fr", "de", "ge", "it", "es", "pl", "ru", "ja", "ko", "pt" };
        if (First.size() < 3 && std::none_of(std::begin(Short), std::end(Short), [&](const char* S) { return First == S; })) return {};
        return CodeOf(First);
    }
}

const std::vector<Language>& All()
{
    static const std::vector<Language> Table = {
        { "en", "English", { "english", "eng", "en", "enus", "engb", "ens", "english_safe" } },
        { "fr", "French", { "french", "fra", "fr", "frfr" } },
        { "de", "German", { "german", "deu", "ger", "ge", "de", "dede" } },
        { "it", "Italian", { "italian", "ita", "it", "itit" } },
        { "es", "Spanish", { "spanish", "spa", "es", "eses" } },
        { "es-mx", "Spanish (Latin America)", { "spanishna", "latam", "esmx", "mx", "es-mx" } },
        { "pt", "Portuguese", { "portuguese", "por", "pt", "ptbr", "ptpt", "brazilian", "br" } },
        { "pl", "Polish", { "polish", "pol", "pl", "plpl", "polish_full", "polish_partial" } },
        { "ru", "Russian", { "russian", "rus", "ru", "ruru", "russian_full", "russian_partial" } },
        { "ja", "Japanese", { "japanese", "jpn", "ja", "jp", "jajp", "japanese_full", "japanese_partial", "jpf", "jpp" } },
        { "ko", "Korean", { "korean", "kor", "ko", "kr", "kokr", "koreana", "korean_full", "korean_partial" } },
        { "zh-hans", "Chinese (Simplified)", { "simplified_chinese", "schinese", "sch", "chs", "zhcn", "cn", "sc", "zh-hans", "zh" } },
        { "zh-hant", "Chinese (Traditional)", { "traditional_chinese", "tchinese", "tch", "cht", "zhtw", "tw", "tc", "zh-hant" } },
        { "ar", "Arabic", { "arabic", "ara", "ar", "arsa" } },
        { "cs", "Czech", { "czech", "cze", "cz", "cs", "cscz" } },
        { "tr", "Turkish", { "turkish", "tur", "tr", "trtr" } },
        { "th", "Thai", { "thai", "tha", "th", "thth" } },
    };
    return Table;
}

std::string CodeOf(std::string_view Spelling)
{
    std::string Want(Spelling);
    for (char& C : Want) C = (char)tolower((unsigned char)C);
    for (const Language& L : All())
        for (const char* S : L.Spellings)
            if (Want == S) return L.Code;
    return {};
}

std::string NameOf(const std::string& Code)
{
    for (const Language& L : All())
        if (Code == L.Code) return L.Name;
    return Code;
}

std::string OfZone(const std::string& Path)
{
    const fs::path P(Path);
    // a language folder (zone\french\, french\): a whole word, not a code ("all", "en" are no folder of one)
    fs::path Dir = P.parent_path();
    for (int K = 0; K < 2 && !Dir.empty() && Dir != Dir.parent_path(); K++, Dir = Dir.parent_path())
    {
        const std::string Name = ExportNaming::Lower(Dir.filename().string());
        if (Name.size() >= 4 && Name.find_first_not_of("abcdefghijklmnopqrstuvwxyz_") == std::string::npos)
            if (std::string C = CodeOf(Name); !C.empty()) return C;
    }
    // the name's first word: a short code or a language's name (fra_x, en_x, english_x), not a
    // launcher's locale (frfr)
    const std::string Name = ExportNaming::Lower(P.filename().string());
    size_t End = 0;
    while (End < Name.size() && IsWordChar(Name[End])) End++;
    if (End == 0 || End >= Name.size() || Name[End] != '_') return {};
    return CodeOfWord(Name.substr(0, End));
}

std::string OfAssetName(const std::string& Name)
{
    const size_t End = Name.find_first_of("\\/");
    return End == std::string::npos ? std::string() : CodeOfWord(ExportNaming::Lower(Name.substr(0, End)));
}

Detected OfInstall(const std::string& Folder)
{
    Detected D;
    if (Folder.empty()) return D;
    std::error_code Ec;
    fs::path Dir = fs::absolute(fs::path(Folder), Ec);
    if (Ec) Dir = fs::path(Folder);
    if (!fs::is_directory(Dir, Ec)) Dir = Dir.parent_path();
    for (int K = 0; K < Up && !Dir.empty(); K++, Dir = Dir.parent_path())
    {
        if (fs::is_regular_file(Dir / ".product.db", Ec))
        {
            Detected B;
            if (ReadProductDb(Dir / ".product.db", B)) return B;
        }
        Detected S;
        if (ReadSteam(Dir, S)) return S;
        if (Dir == Dir.parent_path()) break;
    }
    return D;
}

std::string Describe(const Detected& D)
{
    if (D.Main().empty()) return "not known";
    std::string Out = NameOf(D.Main());
    if (!D.Text.empty() && !D.Speech.empty() && D.Text != D.Speech) Out = NameOf(D.Speech) + " voices, " + NameOf(D.Text) + " text";
    return Out + " (" + D.Source + ")";
}

bool Choice::WantsSpelling(std::string_view Spelling) const
{
    if (Every) return true;
    const std::string C = CodeOf(Spelling);
    return C.empty() || Codes.count(C) != 0;
}

std::string Choice::Text() const
{
    if (Every) return "all";
    std::string Out;
    for (const auto& C : Codes) Out += (Out.empty() ? "" : ",") + C;
    return Out;
}

bool Resolve(const std::string& Option, const Detected& Install, Choice& Out, std::string& Error)
{
    Out = Choice();
    Out.Every = false;
    bool Any = false;
    size_t Start = 0;
    const std::string Text = Option.empty() ? std::string(CliDefault) : Option;
    while (Start <= Text.size())
    {
        size_t End = Text.find(',', Start);
        if (End == std::string::npos) End = Text.size();
        std::string Word = ExportNaming::Lower(Text.substr(Start, End - Start));
        Word.erase(0, Word.find_first_not_of(" \t"));
        Word.erase(Word.find_last_not_of(" \t") + 1);
        Start = End + 1;
        if (Word.empty()) continue;
        Any = true;
        if (Word == "all") Out.Every = true;
        else if (Word == "auto")
        {
            if (Install.Main().empty()) Out.Every = true;
            else Out.Codes.insert(Install.Main());
        }
        else if (std::string C = CodeOf(Word); !C.empty()) Out.Codes.insert(C);
        else
        {
            std::string Known;
            for (const Language& L : All()) Known += std::string(Known.empty() ? "" : ", ") + L.Code;
            Error = "unknown language " + Word + ": auto, all or " + Known;
            return false;
        }
    }
    if (!Any || Out.Every) Out = Choice();
    return true;
}

Scope::Scope(const Choice& Run) : Previous(std::exchange(Running, &Run)) {}

Scope::~Scope() { Running = Previous; }

const Choice& Scope::Current()
{
    static const Choice Every;
    return Running ? *Running : Every;
}
}
