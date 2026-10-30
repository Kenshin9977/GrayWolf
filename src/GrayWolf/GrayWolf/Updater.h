#pragma once

#include <cstdint>
#include <optional>
#include <string>

// Updates from the project's GitHub releases: the hash name packages (package_index\) are swapped
// in place, the program is downloaded and replaced by a helper run of this exe once it has exited.
//
// The latest release carries manifest.json (written by CI on a v* tag):
//   { "schema": 1,
//     "app":      { "version": "1.47.0",        "file": "GrayWolf-1.47.0-x64.zip",          "sha256": "..." },
//     "packages": { "version": "2026.10.04.17", "file": "hash-packages-2026.10.04.17.zip", "sha256": "..." } }
// Each file is an asset of the same release. Only the GUI start and the `update` subcommand call
// this: an export or a regress run never touches the network.
namespace Updater
{
    struct Config
    {
        std::string Repo;        // owner/name (setting update_repo)
        std::string ApiBase;     // https://api.github.com, or GRAYWOLF_UPDATE_API (tests only)
        bool UseGhToken = false; // setting update_use_gh_token: ask `gh auth token` when no env token
        std::string AppDir;      // the exe's folder
        std::string PackageDir;  // <AppDir>\package_index
        bool Force = false;      // packages: install even when not newer, unmanaged or linked
    };

    // From the settings and the environment.
    Config FromSettings();

    enum class Status
    {
        UpToDate,     // nothing newer
        Available,    // a newer version exists (check only)
        Updated,      // installed / staged
        Unavailable,  // no release, no manifest, or a private repository without a token
        NetworkError, // the request did not complete, or an HTTP error
        VerifyFailed, // sha256 mismatch or a bad archive: nothing installed
        Refused,      // package_index not managed by updates (hand-built or linked): left alone
        Failed,       // a local file error: the previous set is kept
    };
    const char* StatusName(Status S);

    struct Asset
    {
        std::string Version, File, Sha256, Url; // Url: the API asset URL
    };

    struct Release
    {
        std::string Tag;
        std::optional<Asset> App, Packages;
    };

    struct CheckResult
    {
        Status Result = Status::NetworkError;
        std::string Message;
        Release Latest;
        std::string LocalApp, LocalPackages; // LocalPackages empty: none installed by updates
        bool AppNewer = false, PackagesNewer = false;
    };

    // GET the latest release and its manifest.
    CheckResult Check(const Config& C);

    // Downloads the packages zip, verifies it, swaps its files into PackageDir (the previous files
    // are kept aside until every file is in place, and put back on any failure), then stamps
    // PackageDir\hash-packages.json.
    Status UpdatePackages(const Config& C, const Asset& Packages, std::string& Message);

    // Downloads the app zip, verifies it and extracts it to <AppDir>\.update\stage-<version>.
    Status StageApp(const Config& C, const Asset& App, std::string& StageDir, std::string& Message);

    // Starts a copy of this exe (`apply-update`) that waits for this process to exit, copies
    // StageDir over AppDir and, with Relaunch, starts the new GrayWolf.exe. Refused (Message says
    // which) while another GrayWolf.exe runs from AppDir, e.g. an `mcp` server.
    bool LaunchApply(const Config& C, const std::string& StageDir, bool Relaunch, std::string& Message);

    // The `apply-update` subcommand: the helper's side of LaunchApply. Returns a process exit code.
    int ApplyStaged(const std::string& StageDir, const std::string& TargetDir, uint32_t WaitPid, bool Relaunch,
                    std::string& Report);

    // This exe's version (its VERSIONINFO), "0" when unreadable.
    std::string AppVersion();
    // The version stamped in PackageDir\hash-packages.json, empty when none.
    std::string LocalPackagesVersion(const std::string& PackageDir);
    // Dotted numeric compare ("v" prefix ignored): <0, 0, >0.
    int CompareVersions(const std::string& A, const std::string& B);

    // Removes what a finished update leaves in <AppDir>\.update (the helper copy, backups).
    void CleanupLeftovers(const std::string& AppDir);

    // The GUI's side: one check at startup on a worker thread, polled by the frame.
    namespace Background
    {
        struct View
        {
            bool Busy = false;
            std::string Line;          // the last outcome, for the menu
            std::string AppVersion;    // non-empty: a newer app was found
            bool AppStaged = false;    // downloaded and verified, ready to apply
            std::string Error;         // a failed download of the app, for the banner
        };
        // Starts the startup check (packages applied when newer, app only reported).
        void Start(bool Startup);
        // Downloads and stages the newer app found by the last check.
        void DownloadApp();
        // Starts the helper on the staged app; the caller then quits.
        bool ApplyAndRestart(std::string& Message);
        View Get();
    }
}
