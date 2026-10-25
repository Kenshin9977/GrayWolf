// Games/T7/CliCommands.BO3.cpp - the Black Ops III commands: bo3-images, bo3-dump-xpak-entries,
// salsa20-selftest, bo3-decrypt-movie.
#include <string>

#include "CliOptions.h"
#include "CliCommands.h"

#include <CLI/CLI.hpp>
#include "CliSupport.h"

#include "WraithX.h"
#include "Strings.h"

#include "Bo3ZoneImages.h"
#include "Bo3XpakDump.h"
#include "Bo3MovieDecrypt.h"
#include "Bo3MovieSalsa20.h"


// BO3 images, offline. OAT has no T7 zone loader, so this reads the fastfile itself and
// pulls the pixels out of the install's .xpak packages - see Bo3ZoneImages.h.
int RunBo3Images(const CliOptions& Options)
{
    // No --out means "tell me what is in there". Which families a game keeps its 2D icons
    // under is not guessable.
    if (Options.Out.empty())
    {
        std::string Report;
        const bool Ok = Bo3ListZoneImages(Options.Bo3Images, Options.Bo3Xpaks, Report);
        CliReport(Options, Report);
        return Ok ? 0 : 5;
    }

    WraithX::InitializeAPI();   // WIC, for the DDS -> PNG write
    const std::string OutDir = Options.Out;
    std::string Report;
    const size_t Written = Bo3ExportZoneImages(Options.Bo3Images, Options.Bo3Xpaks, OutDir,
                                               Options.Type, Report);
    CliReport(Options, Report);
    return Written ? 0 : 5;
}


// Dumps large .xpak entries (see Bo3XpakDump.h). --out sets the dump directory.
int RunBo3DumpXpakEntries(const CliOptions& Options)
{
    const auto XpakFiles = Strings::SplitString(Options.Bo3DumpXpakEntries, ',', true);
    const std::string OutDir = Options.Out.empty() ? "bo3_xpak_dump" : Options.Out;
    std::string Report;
    const size_t Written = Bo3DumpXpakEntries(XpakFiles, OutDir,
        (uint64_t)Options.Bo3XpakMinMB * 1024ull * 1024ull, Report);
    CliReport(Options, Report);
    return Written ? 0 : 5;
}


// Verifies the hand-written Salsa20 against a test vector independent of any BO3 file.
int RunSalsa20SelfTest(const CliOptions& Options)
{
    const bool Ok = Salsa20::SelfTest();
    CliReport(Options, std::format("result={}\n", Ok ? "ok" : "FAIL"));
    return Ok ? 0 : 5;
}


// BO3 cinematic decryption - see Bo3MovieDecrypt.h. The key is extracted fresh from the zone's
// own compiled GSC every run. --install sets the game install root, --out the output file.
int RunBo3DecryptMovie(const CliOptions& Options)
{
    std::string Report;
    const bool Ok = Bo3DecryptMovie(Options.Install, Options.Bo3DecryptMovie, Options.ActsExe,
                                    Options.ActsHandler, Options.Out, Report);
    CliReport(Options, Report);
    return Ok ? 0 : 5;
}

// ---- the subcommands (CliCommands.h)

void AddBo3Commands(CliCommandBuilders& B)
{
    B["bo3-images"] = [](CLI::App& App, CliOptions& Options) {
        auto* Sub = App.add_subcommand("bo3-images", "BO3 zone images -> PNG, pixels sourced from the .xpaks");
        Sub->add_option("file", Options.Bo3Images, "Fast file (.ff) to read image assets from")->required()->check(CLI::ExistingFile);
        Sub->add_option("--bo3-xpaks", Options.Bo3Xpaks, "Where the packages are (default: beside the ff)");
    };
    B["bo3-dump-xpak-entries"] = [](CLI::App& App, CliOptions& Options) {
        auto* Sub = App.add_subcommand("bo3-dump-xpak-entries",
            "Diagnostic: dump every entry >= --bo3-xpak-min-mb to --out (cinematic-encryption brute-force search)");
        Sub->add_option("files", Options.Bo3DumpXpakEntries, "Comma separated .xpak file list")->required();
        Sub->add_option("--bo3-xpak-min-mb", Options.Bo3XpakMinMB, "Size floor in MB (default 2)");
    };
    B["salsa20-selftest"] = [](CLI::App& App, CliOptions& Options) {
        App.add_subcommand("salsa20-selftest", "Verify the hand-written Salsa20 cipher vs a test vector")
            ->callback([&Options] { Options.Salsa20SelfTest = true; });
    };
    B["bo3-decrypt-movie"] = [](CLI::App& App, CliOptions& Options) {
        auto* Sub = App.add_subcommand("bo3-decrypt-movie",
            "Decrypt a BO3 cinematic (Salsa20, key comes from the zone's own GSC, never hardcoded)");
        Sub->add_option("zone", Options.Bo3DecryptMovie, "Zone name whose cinematic to decrypt")->required();
    };
}
