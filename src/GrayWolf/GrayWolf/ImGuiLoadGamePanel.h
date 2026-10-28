#pragma once

#include <atomic>
#include <map>
#include <set>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "GameInstalls.h"
#include "DiskGame.h"
#include "DiskGameSizes.h"
#include "UiAsync.h"

struct ID3D11Device;

// The shell's "Load Game" window: the game (a detected install or a folder, and the disk reader it
// is read with) at the top, then what to export, picked by asset from the install index
// (ImGuiLoadGamePanel.Assets.cpp) or by zone (ImGuiLoadGamePanel.Disk.cpp), and a bar at the
// bottom that exports it through DiskGame's use case, the same as disk-export on the command line.
// No game is named here: a game on the template appears by itself, with its kinds
// (DiskGame::Kinds) and its settings (IDiskGame::Settings).
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
    // True exactly once, when an asset's preview export is written: the caller lists Dir and shows
    // the asset whose file names Name
    bool ConsumePreview(std::string& Dir, std::string& Name);
    // "Show in Assets": true once after the button, the caller brings its Assets window forward
    bool ConsumeShowAssets() { return ShowAssets.exchange(false); }
    // ui-record's "loadgame" step (JSON): tab ("asset"/"zone"), query, focus (an asset's name), pick
    // (names), zones (zone names ticked), preview (a name), export, index, options (the popup
    // opened), dismiss (each true)
    void Script(const std::string& StepJson);
    // A worker runs or is about to (the zone list not read yet): ui-record holds its frames meanwhile
    bool Working();
    // A spinner or a progress bar was drawn this frame: ui-record times the waits without one
    bool ShowedProgress() const { return DrewIndicator; }

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

    // The game's card: its title, launcher and folder, "Change game" (the setup in a popup, inline
    // while no game is read) and the install index's state.
    void DrawGameCard(ID3D11Device* Dev);
    void DrawSetup(ID3D11Device* Dev);
    // The two ways to pick: by asset (the install index searched) and by zone or map
    void DrawAssetTab();
    void DrawZoneTab();
    // The bar at the bottom: what is picked, where it goes, "Formats & options", Export (and why
    // not); the last export's result above it
    void DrawExportBar();
    void DrawOptions();
    void DrawLastRun();
    // The index and its rows for search: kept by ReadZoneCounts, under Mutex
    void KeepIndex(std::unique_ptr<DiskGame::ZoneIndex> Ix);
    // The search's rows for the query and kinds now, rebuilt when either changes
    void Search();
    // Worker: one asset exported alone to the preview folder (PreviewWanted)
    void DoPreview();
    void StartPreview(const DiskGame::AssetId& A);
    // The export of what the tab shown picks, on the worker
    void StartExport();
    // The picked game's zone list (read when missing), shared by both tabs.
    bool ZonesListed();
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
                      const DiskGame::ZoneIndex& Ix, const std::set<DiskGame::Kind>* Only = nullptr);
    // Worker: MeasureSizes for the kinds ticked that have no sample yet (MeasureMissing), the
    // others kept; started once per such set by DrawEstimate
    void DoMeasureMissing();
    std::set<DiskGame::Kind> MeasureMissing;
    std::string MeasureTried;   // the game and kinds last started, not started again
    // the install index's weights of what is picked (Sizes::WeighPicks), and the picks, kinds and
    // formats they are for: worked out again when one of those changes
    std::map<DiskGame::Kind, DiskGame::Sizes::Weighed> PickWeights;
    std::string PickWeightsFor;
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
    UiAsync::Cached<std::string> GameExeOf;      // the game's exe in GameBuf, for its icon (GameInstalls::GameExe)
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
    // parallel to DiskZoneNames: its group in the list, 1 a level's own zone (the shortest name of
    // the zones that level names), 2 shared content, 3 a language or a variant of a level's zone
    std::vector<int> DiskZoneGroups;
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
    std::atomic<bool> ShowAssets{false};

    // By asset: the install index kept in memory, one row per asset (the first zone defining it)
    // with its name in lower case for the search, under Mutex
    struct AssetRows
    {
        std::vector<std::pair<uint32_t, uint32_t>> Rows;   // (zone, its index in ZoneIndex::Defined(zone))
        std::vector<std::string> Lower;
    };
    std::shared_ptr<const DiskGame::ZoneIndex> DiskIndex;
    std::shared_ptr<const AssetRows> DiskAssets;
    std::string DiskIndexDate;                   // the kept index's file date, "Oct 26"
    char AssetQueryBuf[128]{};
    std::set<DiskGame::Kind> AssetKindsOff;      // kind chips switched off
    bool AssetPickedOnly = false;
    std::vector<uint32_t> AssetHits;             // indices into DiskAssets->Rows
    std::map<DiskGame::Kind, size_t> AssetHitKinds;
    size_t AssetHitsTotal = 0;                   // before the cap
    size_t AssetHitZones = 0;                    // the zones the hits are in
    std::string AssetSearchedFor;                // the query and chips AssetHits answer
    const AssetRows* AssetSearchedIn = nullptr;
    DiskGame::Selection AssetPicks;              // what the asset tab exports
    int AssetFocus = -1;                         // the row shown in the side pane (index into Rows)
    bool ExportByAsset = true;                   // the tab shown: what the bar exports
    int WantTab = -1;                            // a tab to select on the next frame (0 asset, 1 zone)
    int ZoneGroup = 0;                           // 0 all, 1 maps, 2 shared, 3 languages
    float BarHeight = 120.0f;                    // the export bar's height, last frame's
    bool OpenOptions = false;
    bool DrewIndicator = false;                  // a spinner or progress bar this frame (ShowedProgress)                    // "Formats & options" opened on the next frame (ui-record)
    std::string AssetPicksFor;                   // the zone list (DiskZonesFor) AssetPicks were made in
    // what the asset tab's export runs with, snapshotted for the worker
    std::shared_ptr<const DiskGame::Selection> ExportOnly;
    std::set<DiskGame::Kind> ExportKinds;
    // the preview: what to export, and the folder and file name handed to the shell once written
    DiskGame::AssetId PreviewWanted;
    std::string PreviewDir, PreviewName;
    std::atomic<bool> PreviewReady{false};
    // the last export's result, for the banners above the bar (under Mutex)
    struct LastRunInfo
    {
        bool Has = false, Cancelled = false;
        std::string Headline, Detail, OutDir, RunLog;
        std::vector<std::string> ZoneErrors;
        size_t Failures = 0, Written = 0;
    } LastRun;
    float DiskFraction = 0.0f;                   // the running export's progress, under Mutex
    std::string DiskStep;
    std::string DiskStall;                       // the run watchdog's warning, under Mutex
    double DiskStartedAt = 0;                    // ImGui::GetTime() when the export started
    DiskGame::Eta DiskEta;                       // this run's ETA, fed under Mutex too
    std::string DiskEtaText = "estimating...";
};
