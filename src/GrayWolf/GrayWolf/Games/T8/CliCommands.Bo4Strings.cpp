// Games/T8/CliCommands.Bo4Strings.cpp - Black Ops 4's string commands: bo4-export-localize-disk,
// bo4-export-rawstrings-disk, bo4-decrypt.
// NOMINMAX before <windows.h>: std::max/std::min below would be mangled by its macros.
#define NOMINMAX

#include <stdio.h>
#include <cmath>
#include <memory>
#include <filesystem>
#include <fstream>
#include <algorithm>
#include <vector>
#include <iterator>
#include <string>
#include <format>
#include <functional>

#include "CliOptions.h"
#include "CliCommands.h"

#include <CLI/CLI.hpp>
#include "CliSupport.h"

#include "WraithX.h"
#include "CoDAssets.h"
#include "FastFile.h"

#include "GameBlackOps4.h"
#include "SABSupport.h"
#include "CascFileSystem.h"
#include "CoDFileHandle.h"
#include "DBGameFiles.h"

namespace
{
    // A decompressed zone's bytes (In) through Export(Zone, Report): result=ok|error, then the
    // exporter's report (dump_error= when it throws)
    int RunZoneExport(const CliOptions& Options, const std::string& In,
                      const std::function<bool(const std::vector<uint8_t>& Zone, std::string& Report)>& Export)
    {
        std::ifstream File(In, std::ios::binary);
        if (!File)
        {
            CliReport(Options, "result=error\nreason=could not open input file\n");
            return 5;
        }
        std::vector<uint8_t> Zone((std::istreambuf_iterator<char>(File)), std::istreambuf_iterator<char>());

        std::string Report;
        bool Ok = false;
        try
        {
            Ok = Export(Zone, Report);
        }
        catch (const std::exception& Ex)
        {
            Report += std::format("dump_error={}\n", Ex.what());
        }
        catch (...)
        {
            Report += "dump_error=unhandled exception\n";
        }

        CliReport(Options, std::format("result={}\n{}", Ok ? "ok" : "error", Report.c_str()));
        return Ok ? 0 : 3;
    }

    // One string hex-encoded as [EncryptionID][StringSize][ciphertext][NUL] through
    // Decrypt(Bytes, Report): result=ok|error, source=offline, then its report
    int RunHexDecrypt(const CliOptions& Options, const std::string& Hex,
                      const std::function<bool(std::vector<uint8_t>& Bytes, std::string& Report)>& Decrypt)
    {
        std::string HexReport;
        bool HexOk = false;
        try
        {
            std::vector<uint8_t> Bytes;
            for (size_t i = 0; i + 1 < Hex.size(); i += 2)
            {
                Bytes.push_back((uint8_t)std::strtoul(Hex.substr(i, 2).c_str(), nullptr, 16));
            }
            if (Bytes.size() < 3)
            {
                HexReport += "decrypt_error=need at least [EncryptionID][StringSize][data...]\n";
            }
            else
            {
                HexOk = Decrypt(Bytes, HexReport);
            }
        }
        catch (const std::exception& Ex)
        {
            HexReport += std::format("decrypt_error={}\n", Ex.what());
        }
        catch (...)
        {
            HexReport += "decrypt_error=unhandled exception\n";
        }

        CliReport(Options, std::format(
            "result={}\nsource=offline\n{}",
            HexOk ? "ok" : "error",
            HexReport.c_str()));

        WraithX::ShutdownAPI();
        return HexOk ? 0 : 3;
    }
}

int RunBo4ExportLocalizeDisk(const CliOptions& Options)
{
    return RunZoneExport(Options, Options.Bo4ExportLocalizeDiskIn, [&Options](const std::vector<uint8_t>& Zone, std::string& Report) {
        return GameBlackOps4::ExportDiskLocalize(Zone, Options.Bo4ExportLocalizeDiskOut, Report);
    });
}

int RunBo4ExportRawStringsDisk(const CliOptions& Options)
{
    return RunZoneExport(Options, Options.Bo4ExportRawStringsDiskIn, [&Options](const std::vector<uint8_t>& Zone, std::string& Report) {
        return GameBlackOps4::ExportDiskRawStrings(Zone, Options.Bo4ExportRawStringsDiskOut, Report);
    });
}


// bo4-decrypt --hex|--disk-hex <bytes>: one string, hex-encoded as
// [EncryptionID][StringSize][ciphertext][NUL], through BO4's string cipher.
int RunBo4Decrypt(const CliOptions& Options)
{
    if (!Options.Bo4DecryptHex.empty())
    {
        return RunHexDecrypt(Options, Options.Bo4DecryptHex, [](std::vector<uint8_t>& Bytes, std::string& HexReport) {
            const uint8_t EncryptionID = Bytes[0];
            const uint8_t StringSize = Bytes[1];
            auto Text = GameBlackOps4::DecryptString(Bytes.data() + 2, StringSize, EncryptionID, 0);
            HexReport += std::format(
                "encryption_id={} (case {})\nstring_size={}\ntext={}\n",
                EncryptionID, (uint8_t)(EncryptionID + 128), StringSize, Text.c_str());
            return true;
        });
    }

    // The on-disk cipher (GameBlackOps4::DecryptDiskString).
    if (!Options.Bo4DecryptDiskHex.empty())
    {
        return RunHexDecrypt(Options, Options.Bo4DecryptDiskHex, [](std::vector<uint8_t>& Bytes, std::string& HexReport) {
            const uint8_t EncryptionID = Bytes[0];
            const uint8_t StringSize = Bytes[1];
            std::string Text;
            if (!GameBlackOps4::DecryptDiskString(Bytes.data() + 2, (uint8_t)(StringSize - 1), EncryptionID, Text))
            {
                HexReport += std::format("decrypt_error=unknown encryption_id {}\n", EncryptionID);
                return false;
            }
            HexReport += std::format("encryption_id={}\nstring_size={}\ntext={}\n",
                EncryptionID, StringSize, Text.c_str());
            return true;
        });
    }

    CliReport(Options, "result=error\nreason=bo4-decrypt needs --hex or --disk-hex\n");
    return 2;
}

// ---- the subcommands (CliCommands.h)

void AddBo4StringCommands(CliCommandBuilders& B)
{
    B["bo4-export-localize-disk"] = [](CLI::App& App, CliOptions& Options) {
        auto* Sub = App.add_subcommand("bo4-export-localize-disk",
            "Fully offline BO4 Localize export from an already-decompressed zone (zone-decompress output)");
        Sub->add_option("in", Options.Bo4ExportLocalizeDiskIn, "Decompressed zone bytes (from zone-decompress)")->required()->check(CLI::ExistingFile);
        Sub->add_option("outfile", Options.Bo4ExportLocalizeDiskOut, "Where to write the JSONL output")->required();
    };
    B["bo4-export-rawstrings-disk"] = [](CLI::App& App, CliOptions& Options) {
        auto* Sub = App.add_subcommand("bo4-export-rawstrings-disk",
            "Fully offline BO4 dialogue/rawstring export (Value+Hash+Pad header shape) from a decompressed zone");
        Sub->add_option("in", Options.Bo4ExportRawStringsDiskIn, "Decompressed zone bytes (from zone-decompress)")->required()->check(CLI::ExistingFile);
        Sub->add_option("outfile", Options.Bo4ExportRawStringsDiskOut, "Where to write the JSONL output")->required();
    };
    B["bo4-decrypt"] = [](CLI::App& App, CliOptions& Options) {
        auto* Sub = App.add_subcommand("bo4-decrypt", "Decrypt one BO4 string given as hex bytes [EncryptionID][StringSize][ciphertext][NUL]");
        Sub->add_option("--hex", Options.Bo4DecryptHex, "Through the zone's string cipher (DecryptString)");
        Sub->add_option("--disk-hex", Options.Bo4DecryptDiskHex, "Through the on-disk Localize cipher");
    };
}
