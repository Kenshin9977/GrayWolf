#pragma once

// The languages an export speaks of: one short code per language ("en", "fr", "de"...), each
// game's own spelling of it mapped here once (Black Ops 4's "ge", the IW games' "fra_" and
// "french" folders, Black Ops 6's "spanishna", Battle.net's "esES", Steam's "schinese"), the
// language an install is played in as its launcher says, and what a run keeps.
//
// A run keeps its languages' content and the content of no language (Choice::Wants): "auto" is
// the install's language (all of them when its launcher says none), "all" every language.

#include <set>
#include <string>
#include <string_view>
#include <vector>

namespace Languages
{
    struct Language
    {
        const char* Code;    // "fr"
        const char* Name;    // "French"
        // every spelling a game, a launcher or a file name gives it, lower case
        std::vector<const char*> Spellings;
    };
    // the languages Call of Duty ships, English first
    const std::vector<Language>& All();
    // A spelling's code ("french", "fra", "frFR", "ge", "koreana"... without case), "" when it is none
    std::string CodeOf(std::string_view Spelling);
    // The code's display name ("French"); the code itself when it is none
    std::string NameOf(const std::string& Code);

    // The language a zone's file belongs to: a language folder it is in (zone/french/x.ff,
    // french/fra_x.ff) or its name's first word (fra_x.ff, en_core_frontend.bin, eng_x.ff); ""
    // for a zone of no language
    std::string OfZone(const std::string& Path);
    // The language an exported asset's name is of: its first folder when that is one
    // (es\vox\x, a Black Ops 4 or III bank's sound); "" for none ("all\x", "vox\x")
    std::string OfAssetName(const std::string& Name);

    // What an install's launcher says of its languages, read from its files (never the launcher):
    // Battle.net's <install>\.product.db (the text and speech languages picked, the ones installed),
    // Steam's steamapps\appmanifest_<app>.acf ("language" in UserConfig). The install may be given
    // as a folder inside it (its zone folder) or a zone file in it.
    struct Detected
    {
        std::string Speech, Text;             // codes; "" when not known
        std::vector<std::string> Installed;   // codes of the languages installed, when the launcher lists them
        std::string Source;                   // "Battle.net", "Steam"; "" when nothing was found
        std::string Raw;                      // as the launcher spells it ("esES", "french")
        // the language the game is played in: its speech, else its text
        const std::string& Main() const { return Speech.empty() ? Text : Speech; }
    };
    Detected OfInstall(const std::string& Folder);
    // "French (Steam)", "Spanish voices, French text (Battle.net)", "not known"
    std::string Describe(const Detected& D);

    // The languages a run keeps
    struct Choice
    {
        bool Every = true;                 // every language
        std::set<std::string> Codes;       // else these
        // content in Code ("" for content of no language: always kept)
        bool Wants(const std::string& Code) const { return Every || Code.empty() || Codes.count(Code) != 0; }
        // a spelling of a language (CodeOf), kept or of no language
        bool WantsSpelling(std::string_view Spelling) const;
        std::string Text() const;          // "all", "fr", "en,fr"
    };
    // A run's languages from its option: "auto" (the install's language; every language when it is
    // not known), "all", or a comma list of languages in any spelling CodeOf takes, "auto" among
    // them ("auto,en"). False and Error for a word that names no language.
    bool Resolve(const std::string& Option, const Detected& Install, Choice& Out, std::string& Error);
    // The option a run takes when none is given: "auto" in the GUI's setting, "all" on the command
    // line (a command's output depends on its arguments, not on what a launcher is set to)
    inline constexpr const char* SettingKey = "export_languages";
    inline constexpr const char* GuiDefault = "auto";
    inline constexpr const char* CliDefault = "all";

    // The run's languages while it exports, on the thread that exports (DiskGame::Export sets it):
    // for a reader the run's Context does not reach (Black Ops 4's sound banks). Every language when
    // no run set it (the install index, a preview).
    class Scope
    {
    public:
        explicit Scope(const Choice& Run);
        ~Scope();
        Scope(const Scope&) = delete;
        Scope& operator=(const Scope&) = delete;
        static const Choice& Current();

    private:
        const Choice* Previous;
    };
}
