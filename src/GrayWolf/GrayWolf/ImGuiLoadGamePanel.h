#pragma once

#include <atomic>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "GameInstalls.h"
#include "DiskGame.h"
#include "DiskGameSizes.h"
#include "UiAsync.h"

struct ID3D11Device;

// The shell's "Load Game" window: pick an install (or a folder) and the disk reader it is read
// with, then export its zones through DiskGame's use case, the same as disk-export on the command
// line (ImGuiLoadGamePanel.Disk.cpp). No game is named here: a game on the template appears by
// itself, with its kinds (DiskGame::Kinds) and its settings (IDiskGame::Settings).
class LoadGamePanel
{
public:
    LoadGamePanel();
    ~LoadGamePanel();

    // Draws the whole window. Call once per frame between ImGui::NewFrame and Render. Dev draws the
    // launcher icons of the detected installs.
    void Draw(ID3D11Device* Dev);

    // True exactly once, on the frame an export that wrote something finishes: the caller rescans
    // ExportOutputDir() (<output folder>/<game id>) into the Assets browser.
    bool ConsumeExportJustFinished() { return DiskJustFinished.exchange(false); }
    std::string ExportOutputDir() const { return DiskExportedTo; }

private:
    // Long operations run on a worker so the UI keeps drawing; Busy gates every control.
    void StartWork(const char* Label, void (LoadGamePanel::*Fn)());
    void JoinIfDone();

    // Writes the current choices to the shared settings file so the next session starts where this
    // one left off.
    void Persist();

    // Scans for installed games (registry + common folders) and fills Installs.
    void DoScan();
    // Applies a detected install: its folder, and the disk reader it names.
    void ApplyInstall(const GameInstalls::Install& I);

    // The disk game for GameBuf: the saved "Read with" choice, else detected from its zones, else
    // the picked install's reader; nullptr when none reads it.
    void ChooseDiskGame();
    // The "Read with" combo: one of DiskGame::Games().
    void DrawReadWith();

    // The picked game's output folder, kinds, settings, zone list and Export button.
    void DrawDiskSection();
    // The running export's progress bar and Cancel, drawn outside the busy-disabled block.
    void DrawDiskProgress();
    void DoListDiskZones();
    void DoDiskExport();
    // Worker: the install index built (every zone opened once), for the zones' asset counts
    void DoIndexDiskZones();
    // Worker: the asset counts of each listed zone from the kept install index, if any
    void ReadZoneCounts(const DiskGame::IDiskGame& Game, const DiskGame::ExportOptions& Options, const std::string& Install);
    // The zone list's table: a zone's tick, its level name, its asset count; sortable, filtered
    void DrawZoneTable(std::vector<uint32_t>& Visible);
    // Worker, after the counts: a sample of each kind exported and measured (DiskGame::Sizes)
    void MeasureSizes(const DiskGame::IDiskGame& Game, const DiskGame::ExportOptions& Options, const std::string& Install,
                      const DiskGame::ZoneIndex& Ix);
    // The export's estimated size for the kinds, zones and formats picked, and the free space of
    // the output folder's drive (Target): computed from the counts and the samples, every frame
    void DrawEstimate(const std::string& Target);

    // Guards Log, Status, Installs and the export's progress against the worker thread.
    std::mutex Mutex;
    std::vector<std::string> Log;
    std::string Status;

    std::thread Worker;
    std::atomic<bool> Busy{false};
    std::atomic<bool> Done{false};
    std::string BusyLabel;

    std::vector<GameInstalls::Install> Installs;
    int InstallIndex = -1;
    bool ScannedOnce = false;
    bool AutoScanStarted = false;

    // The disk game GameBuf is read with (see ChooseDiskGame) and the GameBuf it was chosen for.
    const DiskGame::IDiskGame* Disk = nullptr;
    std::string DiskChosenFor;
    // DiskGame::Detect walks the folder for its first fast file: on a worker, keyed by the folder
    UiAsync::Cached<const DiskGame::IDiskGame*> Detected;
    bool Detecting = false;
    bool GameEditing = false;      // the game folder's field has the keyboard
    // the output folder exists, its export log too: looked up on a worker, never by the frame
    UiAsync::Cached<std::pair<bool, bool>> OutputState;
    std::string InstallDiskId;     // the disk reader the detected install names (GameInstalls)
    std::string InstallPath;       // that install's folder: its reader holds while the folder is unchanged

    char GameBuf[512]{};           // ImGui needs writable char buffers
    char DiskOutBuf[512]{};
    std::vector<uint8_t> DiskKindFlags;          // parallel to DiskGame::Kinds
    // The zone list, a flag per zone, and the "<game>|<folder>|<settings>" it was read for: another
    // game, folder or setting reads it again.
    std::vector<std::string> DiskZoneNames;
    std::vector<uint8_t> DiskZoneFlags;
    // parallel to DiskZoneNames: its level name (DiskGame::LevelNames, "" for none) and what the
    // install index lists in it per kind (DiskZoneCounted: an index was found or built)
    std::vector<std::string> DiskZoneLevels;
    std::vector<std::map<DiskGame::Kind, size_t>> DiskZoneCounts;
    bool DiskZoneCounted = false;
    std::vector<uint64_t> DiskZoneFootprints;    // parallel to DiskZoneNames, from the install index
    DiskGame::Sizes::Calibration DiskSizes;      // the size samples (kept beside the index), under Mutex
    UiAsync::Cached<uint64_t> FreeSpace;         // the output drive's free bytes, looked up on a worker
    int DiskZoneSortColumn = 0;                  // 0 zone, 1 level, 2 assets
    bool DiskZoneSortDescending = false;
    std::string DiskZonesFor;
    char DiskZoneFilterBuf[128]{};
    char DiskNameFilterBuf[256]{};               // Context::NameFilter, saved
    std::vector<std::string> DiskZonesToExport;  // snapshotted for the worker
    std::string DiskExportedTo;                  // <out>/<game id> of the last export
    std::atomic<bool> DiskCancel{false};
    std::atomic<bool> DiskJustFinished{false};
    float DiskFraction = 0.0f;                   // the running export's progress, under Mutex
    std::string DiskStep;
    std::string DiskStall;                       // the run watchdog's warning, under Mutex
    double DiskStartedAt = 0;                    // ImGui::GetTime() when the export started
    DiskGame::Eta DiskEta;                       // this run's ETA, fed under Mutex too
    std::string DiskEtaText = "estimating...";
};
