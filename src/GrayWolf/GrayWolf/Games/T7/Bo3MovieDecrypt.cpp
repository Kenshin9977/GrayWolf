#include "stdafx.h"
#include "ByteView.h"

#include "Bo3MovieDecrypt.h"

#include "OatBridge.h"
#include "Bo3MovieSalsa20.h"
#include "Strings.h"

#include <filesystem>
#include <fstream>
#include <regex>
#include <vector>
#include <algorithm>
#include <cstring>
#include <format>

namespace fs = std::filesystem;

namespace
{
    struct MovieKeyCandidate
    {
        std::string MovieName;
        std::string KeyBase64;
        std::string SourceFile;
    };

    // Matches lui::prime_movie(str_movie, ..., str_key) and lui::play_movie(str_movie, ..., str_key)
    // in decompiled GSC source text: captures the movie name (first quoted arg) and the key (the
    // last quoted arg, only when it looks like a real base64 blob - an empty "" key, as used by
    // every non-encrypted movie call, never matches this pattern at all).
    const std::regex MovieCallPattern(
        R"RX((?:play_movie|prime_movie)\s*\(\s*"([^"]*)"\s*,.*?"([A-Za-z0-9+/=]{16,})"\s*\))RX");

    std::vector<MovieKeyCandidate> FindMovieKeyCandidates(const std::string& ScriptsDir)
    {
        std::vector<MovieKeyCandidate> Out;
        std::error_code Ec;

        for (const auto& Entry : fs::recursive_directory_iterator(ScriptsDir,
             fs::directory_options::skip_permission_denied, Ec))
        {
            if (Ec) break;
            if (!Entry.is_regular_file(Ec)) continue;
            if (Entry.path().extension() != ".gsc") continue;

            std::ifstream In(Entry.path(), std::ios::binary);
            if (!In) continue;
            std::string Source((std::istreambuf_iterator<char>(In)), std::istreambuf_iterator<char>());

            for (auto It = std::sregex_iterator(Source.begin(), Source.end(), MovieCallPattern);
                 It != std::sregex_iterator(); ++It)
            {
                MovieKeyCandidate C;
                C.MovieName = (*It)[1].str();
                C.KeyBase64 = (*It)[2].str();
                C.SourceFile = Entry.path().string();
                Out.push_back(C);
            }
        }
        return Out;
    }

    // EBML/Matroska structural check (magic bytes + DocType), not a byte-count heuristic.
    bool LooksLikeValidMkv(ByteView Data)
    {
        static const uint8_t EbmlMagic[4] = { 0x1A, 0x45, 0xDF, 0xA3 };
        if (Data.size() < 256 || std::memcmp(Data.data(), EbmlMagic, 4) != 0)
            return false;

        static const char DocType[] = "matroska";
        const size_t Window = Data.size() < 4096 ? Data.size() : 4096;
        const auto Begin = Data.begin();
        const auto End = Data.begin() + Window;
        return std::search(Begin, End, DocType, DocType + 8) != End;
    }
}

bool Bo3DecryptMovie(const std::string& InstallDir, const std::string& ZoneName,
                     const std::string& ActsExe, const std::string& ActsHandler,
                     const std::string& OutPath, std::string& Report)
{
    std::error_code Ec;

    if (InstallDir.empty() || ZoneName.empty())
    {
        Report = "Bo3DecryptMovie: --install and a zone name are both required\n";
        return false;
    }

    // Locate the zone's own compiled fastfile - prefer *_patch.ff (where the level's own scripts,
    // including any lui::play_movie key, actually live), fall back to the plain zone .ff.
    const std::string ZoneDir = (std::filesystem::path(InstallDir) / "zone").string();
    std::string FfPath = (std::filesystem::path(ZoneDir) / (ZoneName + "_patch.ff")).string();
    if (!fs::exists(FfPath, Ec))
        FfPath = (std::filesystem::path(ZoneDir) / (ZoneName + ".ff")).string();
    if (!fs::exists(FfPath, Ec))
    {
        Report = std::format("Bo3DecryptMovie: neither {}_patch.ff nor {}.ff exists under {}\n",
                                  ZoneName.c_str(), ZoneName.c_str(), ZoneDir.c_str());
        return false;
    }

    Report = std::format("Zone fastfile: {}\n", FfPath.c_str());

    // Decompile it (scripts included) with acts - a scratch dir per zone, so a re-run naturally
    // reuses a previously-successful decompile instead of re-running acts every time.
    const std::string ScratchDir =
        (fs::temp_directory_path(Ec) / "greyhound_bo3_movie_decrypt" / ZoneName).string();

    ActsExtractOptions Opts;
    Opts.Handler = ActsHandler.empty() ? "bo3" : ActsHandler;
    std::string ActsLog;
    const bool ActsOk = ActsExtractFastFile(ActsExe, FfPath, ScratchDir, Opts, ActsLog);
    Report += std::format("acts decompile: {} (scratch={})\n", ActsOk ? "ok" : "FAILED",
                               ScratchDir.c_str());
    if (!ActsOk)
    {
        Report += "--- acts log ---\n" + ActsLog + "\n";
        return false;
    }

    const auto Candidates = FindMovieKeyCandidates(ScratchDir);
    Report += std::format("Keyed lui::prime_movie/play_movie calls found: {}\n", Candidates.size());
    if (Candidates.empty())
    {
        Report += "No non-empty-key movie call found in this zone's decompiled scripts.\n";
        return false;
    }

    const std::string VideoDir = (std::filesystem::path(InstallDir) / "video").string();

    for (const auto& C : Candidates)
    {
        Report += std::format("- candidate movie=\"{}\" key={}... (from {})\n",
                                   C.MovieName.c_str(), C.KeyBase64.substr(0, 12).c_str(),
                                   C.SourceFile.c_str());

        const auto KeyBytes = Strings::FromBase64String(C.KeyBase64);
        if (KeyBytes.size() != 40)
        {
            Report += std::format("  skip: base64 decodes to {} bytes, expected 40\n", KeyBytes.size());
            continue;
        }

        const std::string CtPath = (std::filesystem::path(VideoDir) / (C.MovieName + ".mkv")).string();
        if (!fs::exists(CtPath, Ec))
        {
            Report += std::format("  skip: no loose ciphertext at {} (XPAK CacheID resolution "
                                       "is not implemented - see cinematic-encryption.md)\n", CtPath.c_str());
            continue;
        }

        std::ifstream In(CtPath, std::ios::binary);
        if (!In)
        {
            Report += std::format("  skip: could not open {}\n", CtPath.c_str());
            continue;
        }
        std::vector<uint8_t> Data((std::istreambuf_iterator<char>(In)), std::istreambuf_iterator<char>());
        In.close();

        Salsa20::CryptInPlace(Data.data(), Data.size(), KeyBytes.data(), KeyBytes.data() + 32);

        const bool Valid = LooksLikeValidMkv(Data);
        const std::string RealOut = OutPath.empty() ? (C.MovieName + "_decrypted.mkv") : OutPath;

        std::ofstream Out(RealOut, std::ios::binary);
        if (!Out)
        {
            Report += std::format("  decrypted {} bytes but could not write {}\n", Data.size(), RealOut.c_str());
            continue;
        }
        Out.write((const char*)Data.data(), (std::streamsize)Data.size());
        Out.close();

        Report += std::format(
            "  ciphertext={} ({} bytes)\n  decrypted -> {}\n  EBML/Matroska validity check: {}\n",
            CtPath.c_str(), Data.size(), RealOut.c_str(), Valid ? "PASS" : "FAIL");

        if (Valid)
            return true;

        Report += "  (wrote the file anyway for inspection, but this is not a confirmed success)\n";
    }

    Report += "No candidate produced a validated decrypt.\n";
    return false;
}
