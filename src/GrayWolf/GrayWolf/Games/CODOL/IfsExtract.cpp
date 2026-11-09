#include "stdafx.h"

#include <CLI/CLI.hpp>

#include "CliOptions.h"
#include "CliSupport.h"
#include "GameCommand.h"
#include <wolff/Codol.h>

#include <filesystem>
#include <fstream>
#include <regex>

namespace Ifs = wolff::Ifs;

// `ifs-extract <install>`: the files of an install's .ifs archives, each at its latest version and
// unwrapped (FileCrypt.h: images, sounds, model data), as loose files under --out (the archives' own paths: client_shipretail/chinese/<zone>.ff).
namespace
{
    std::string FilesPattern = "*.ff";

    int Run(const CliOptions& Options, const std::string& Input)
    {
        std::string Report;
        Ifs::Install Install;
        std::string Error;
        if (Options.Out.empty())
            Error = "--out is required";
        else if (!Install.Open(Input, Error))
            Error = "install: " + Error;
        if (!Error.empty())
        {
            CliReport(Options, "result=fail\nerror=" + Error + "\n");
            return 1;
        }

        const auto Pattern = Ifs::GlobRegex(FilesPattern);
        const auto Needles = SplitNameFilter(Options.NameFilter);
        size_t Written = 0, Failed = 0;
        uint64_t Bytes = 0;
        for (const auto& [Key, Located] : Install.Files())
        {
            const auto Base = Ifs::Utf8(Ifs::Utf8Path(Key).filename());
            if (!std::regex_match(Base, Pattern) || !MatchesNameFilter(Key, Needles))
                continue;
            std::vector<uint8_t> Data;
            std::string ReadError;
            const auto Dst = Ifs::Utf8Path(Options.Out) / Ifs::Utf8Path(Key);
            std::error_code Ec;
            std::filesystem::create_directories(Dst.parent_path(), Ec);
            if (!Install.Read(Located, Data, ReadError))
            {
                Report += "failed\t" + Key + "\t" + ReadError + "\n";
                Failed++;
                continue;
            }
            wolff::Codol::UnwrapFile(Key, Data, wolff::DefaultContext(), Ifs::Utf8Path(Install.Folder()));
            std::ofstream F(Dst, std::ios::binary);
            F.write(reinterpret_cast<const char*>(Data.data()), (std::streamsize)Data.size());
            if (!F)
            {
                Report += "failed\t" + Key + "\tcould not write " + Ifs::Utf8(Dst) + "\n";
                Failed++;
                continue;
            }
            Report += Key + "\tsize=" + std::to_string(Data.size()) + "\n";
            Written++;
            Bytes += Data.size();
        }
        Report += "archives=" + std::to_string(Install.ArchiveCount()) + "\nfiles=" + std::to_string(Install.Files().size()) +
                  "\nwritten=" + std::to_string(Written) + "\nbytes=" + std::to_string(Bytes) +
                  "\nfailed=" + std::to_string(Failed) + "\nresult=" + (Failed == 0 ? "ok" : "fail") + "\n";
        CliReport(Options, Report);
        return Failed == 0 ? 0 : 1;
    }

    void AddOptions(CLI::App& Sub, CliOptions&)
    {
        Sub.add_option("--files", FilesPattern, "file names to extract, a glob (default *.ff; *.iwi, *.mp3, * ...)");
    }

    const GameCommand Command{
        "ifs-extract",
        "Call of Duty Online: the files of the install's Tencent .ifs archives, at their latest version, as loose files under --out "
        "(fast files land in client_shipretail/chinese/)",
        "The CODOL install (or its IIPS\\IIPSDownload folder)",
        Run,
        AddOptions,
    };
    const GameCommandRegistration Registered(Command);
}
