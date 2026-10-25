#include "stdafx.h"

// The Assets tab's rows: the folders' walks, their merge into one row per asset, the header reads.
#include "ImGuiShell.Internal.h"
#include "AppLog.h"           // timed steps
#include "CastReader.h"        // ReadCastAnim, CastFile::ModelCounts / FileKind
#include "UiAsync.h"           // the folder walks off the frame

#include <nlohmann/json.hpp>   // a .glb's JSON chunk

#include <algorithm>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <fstream>

namespace ShellUi
{
    namespace
    {
        // Reads just the pixel dimensions from an image header (per format), cheaply, during the scan.
        void ReadImageDims(const std::string& Path, const std::string& Ext, int& W, int& H)
        {
            W = 0; H = 0;
            std::ifstream In(Path, std::ios::binary);
            if (!In) return;
            uint8_t B[32] = {};
            In.read((char*)B, sizeof(B));
            if (In.gcount() < 24) return;
            auto BE32 = [&](int o) { return (uint32_t)((B[o] << 24) | (B[o+1] << 16) | (B[o+2] << 8) | B[o+3]); };
            auto LE32 = [&](int o) { return (uint32_t)(B[o] | (B[o+1] << 8) | (B[o+2] << 16) | ((uint32_t)B[o+3] << 24)); };
            auto LE16 = [&](int o) { return (uint16_t)(B[o] | (B[o+1] << 8)); };
            if (Ext == "png")      { W = (int)BE32(16); H = (int)BE32(20); }
            else if (Ext == "dds") { H = (int)LE32(12); W = (int)LE32(16); }
            else if (Ext == "iwi") { W = (int)LE16(6);  H = (int)LE16(8); }
            else if (Ext == "tga") { W = (int)LE16(12); H = (int)LE16(14); }
        }

        // Reads a map .glb's mesh/vertex/tri counts cheaply: a GLB is a 12-byte header, a length-prefixed
        // JSON chunk, then the (often huge) binary chunk; only the JSON chunk is parsed.
        bool ReadGlbMeta(const std::string& Path, uint32_t& Meshes, uint32_t& Verts, uint32_t& Tris)
        {
            Meshes = Verts = Tris = 0;
            std::ifstream In(Path, std::ios::binary);
            if (!In) return false;
            uint8_t Hdr[12];
            if (!In.read((char*)Hdr, sizeof(Hdr)) || std::memcmp(Hdr, "glTF", 4) != 0) return false;
            uint32_t ChunkLen = 0, ChunkType = 0;
            if (!In.read((char*)&ChunkLen, 4) || !In.read((char*)&ChunkType, 4)) return false;
            if (ChunkType != 0x4E4F534Au) return false; // "JSON"
            std::string Json((size_t)ChunkLen, '\0');
            if (!In.read(Json.data(), (std::streamsize)ChunkLen)) return false;

            try
            {
                nlohmann::json J = nlohmann::json::parse(Json);
                if (!J.contains("meshes") || !J.contains("accessors")) return false;
                Meshes = (uint32_t)J["meshes"].size();
                const auto& Accessors = J["accessors"];
                for (const auto& Mesh : J["meshes"])
                    for (const auto& Prim : Mesh.value("primitives", nlohmann::json::array()))
                    {
                        const auto Attrs = Prim.value("attributes", nlohmann::json::object());
                        if (Attrs.contains("POSITION"))
                        {
                            const int Ai = Attrs["POSITION"].get<int>();
                            if (Ai >= 0 && Ai < (int)Accessors.size()) Verts += Accessors[Ai].value("count", 0u);
                        }
                        if (Prim.contains("indices"))
                        {
                            const int Ai = Prim["indices"].get<int>();
                            if (Ai >= 0 && Ai < (int)Accessors.size()) Tris += Accessors[Ai].value("count", 0u) / 3;
                        }
                    }
                return true;
            }
            catch (...) { return false; }
        }

        std::string AssetKey(const Item& M)
        {
            std::string Key = M.Root + '|' + std::to_string(M.Kind) + '|' + M.Format + '|' + M.Name;
            for (auto& c : Key) c = (char)tolower((unsigned char)c);
            return Key;
        }
    }

    AssetCatalog::AssetCatalog()
    {
        // a few readers: a header read is one small file open, bound by the disk's latency, not its rate
        for (int i = 0; i < 4; i++) ScanWorkers.emplace_back([this] { ScanLoop(); });
    }

    AssetCatalog::~AssetCatalog() { Stop(); }

    void AssetCatalog::Stop()
    {
        { std::lock_guard<std::mutex> Lk(ScanMx); ScanStop = true; }
        ScanCv.notify_all();
        for (auto& W : ScanWorkers) if (W.joinable()) W.join();
        Lists->Generation++;   // a folder walk still running stops
    }

    void AssetCatalog::ScanLoop()
    {
        for (;;)
        {
            ScanJob Job;
            {
                std::unique_lock<std::mutex> Lk(ScanMx);
                ScanCv.wait(Lk, [&] { return ScanStop || !ScanQueue.empty() || !ScanImages.empty(); });
                if (ScanStop) return; // shutting down - the app is closing, no need to drain the rest
                auto& From = !ScanQueue.empty() ? ScanQueue : ScanImages;
                if (From.empty()) continue; // spurious wake
                Job = std::move(From.front());
                From.pop_front();
            }
            ScanResult R; R.Id = Job.Id; R.Path = Job.Path;
            if (Job.Format == "semodel")
                ReadSEModelHeaderCounts(Job.Path, R.Bones, R.Meshes, R.Mats);
            else if (Job.Format == "cast" && Job.Kind == 0)
                CastFile::ModelCounts(Job.Path, R.Bones, R.Meshes, R.Mats);
            else if (Job.Format == "cast" && Job.Kind == 3)
            {
                SeAnimClip Clip;   // Cast keeps no header counts: the curves are the anim's length
                if (ReadCastAnim(Job.Path, Clip))
                {
                    R.Looping = Clip.Looping;
                    R.FrameRate = Clip.FrameRate;
                    R.Frames = Clip.FrameCount;
                    R.Bones = (uint32_t)Clip.Bones.size();
                }
            }
            else if (Job.Format == "xmodel_export")
            {
                // Text format: NUMBONES sits at the top (cheap head read); NUMOBJECTS/NUMMATERIALS
                // sit right before the material list, near EOF - a tail read finds them without
                // scanning the whole (often multi-MB) per-vertex/per-face body.
                std::ifstream In(Job.Path, std::ios::binary);
                if (In)
                {
                    char Head[4096] = {};
                    In.read(Head, sizeof(Head) - 1);
                    std::string HeadStr(Head, (size_t)In.gcount());
                    if (auto Pos = HeadStr.find("NUMBONES "); Pos != std::string::npos)
                        R.Bones = (uint32_t)std::atoi(HeadStr.c_str() + Pos + 9);

                    const long long TailLen = std::min<long long>(Job.SizeBytes, 65536);
                    if (TailLen > 0)
                    {
                        std::string Tail((size_t)TailLen, '\0');
                        In.seekg(Job.SizeBytes - TailLen, std::ios::beg);
                        In.read(Tail.data(), TailLen);
                        if (auto Pos = Tail.find("NUMOBJECTS "); Pos != std::string::npos)
                            R.Meshes = (uint32_t)std::atoi(Tail.c_str() + Pos + 11);
                        if (auto Pos = Tail.find("NUMMATERIALS "); Pos != std::string::npos)
                            R.Mats = (uint32_t)std::atoi(Tail.c_str() + Pos + 13);
                    }
                }
            }
            else if (Job.Kind == 1) ReadImageDims(Job.Path, Job.Format, R.ImgW, R.ImgH);
            else if (Job.Format == "seanim")
            {
                // Fixed binary header (see SEAnimExport.cpp): "SEAnim"(6) version(2) headerSize(2)
                // animType(1) flags(1, bit0=looping) dataFlags(1) dataPropFlags(1) reserved(2)
                // frameRate(4) frameCount(4) boneCount(4) ...
                std::ifstream In(Job.Path, std::ios::binary); uint8_t H[28];
                if (In.read((char*)H, sizeof(H)) && std::memcmp(H, "SEAnim", 6) == 0)
                {
                    R.Looping = (H[11] & 1) != 0;
                    std::memcpy(&R.FrameRate, H+16, 4);
                    std::memcpy(&R.Frames,   H+20, 4);
                    std::memcpy(&R.Bones,    H+24, 4);
                }
            }
            else if (Job.Format == "glb") ReadGlbMeta(Job.Path, R.Meshes, R.Verts, R.Tris);

            std::lock_guard<std::mutex> Lk(ScanMx);
            ScanDone.push_back(std::move(R));
        }
    }

    void AssetCatalog::RebuildIdIndex()
    {
        IdToIndex.clear(); for (size_t i = 0; i < Models.size(); i++) IdToIndex[Models[i].Id] = i;
    }

    void AssetCatalog::DrainScanResults()
    {
        std::vector<ScanResult> Batch;
        { std::lock_guard<std::mutex> Lk(ScanMx); if (ScanDone.empty()) return; Batch.swap(ScanDone); }
        HeaderVersion++;
        for (auto& R : Batch)
        {
            auto It = IdToIndex.find(R.Id);
            if (It == IdToIndex.end() || It->second >= Models.size()) continue;
            Item& M = Models[It->second];
            if (M.Path != R.Path) continue; // a copy the row no longer shows (a newer one replaced it)
            M.Bones = R.Bones; M.Meshes = R.Meshes; M.Mats = R.Mats;
            M.ImgW = R.ImgW; M.ImgH = R.ImgH;
            M.Frames = R.Frames; M.FrameRate = R.FrameRate; M.Looping = R.Looping;
            M.Verts = R.Verts; M.Tris = R.Tris;
        }
    }

    void AssetCatalog::ListFolder(const std::string& Dir)
    {
        const uint64_t Gen = Lists->Generation.load();
        { std::lock_guard<std::mutex> Lk(Lists->M); Lists->Dir = Dir; }
        Lists->Walks++;
        UiAsync::Run("list folder " + Dir, [Lists = Lists, Dir, Gen] {
            struct Ended { std::atomic<int>& N; ~Ended() { N--; } } Ending{ Lists->Walks };
            AppLog::Timer Scan("scan folder " + Dir, spdlog::level::info, 1e9);
            size_t Seen = 0, Other = 0, ByKind[6] = {};
            std::vector<Item> Batch;
            auto LastFlush = std::chrono::steady_clock::now();
            const auto Flush = [&] {
                std::lock_guard<std::mutex> Lk(Lists->M);
                if (Lists->Generation.load() == Gen) for (auto& Md : Batch) Lists->Ready.push_back(std::move(Md));
                Batch.clear();
                LastFlush = std::chrono::steady_clock::now();
            };
            std::error_code Ec;
            if (std::filesystem::is_directory(Dir, Ec))
                for (auto It = std::filesystem::recursive_directory_iterator(Dir, Ec); It != std::filesystem::recursive_directory_iterator(); It.increment(Ec))
                {
                    if (Ec || Lists->Generation.load() != Gen) break;
                    if (!It->is_regular_file(Ec)) continue;
                    Seen++;
                    Lists->Seen++;
                    auto P = It->path();
                    std::string PathText, Stem, E;
                    // a name the ANSI code page cannot spell (path::string() throws) is skipped
                    try { PathText = P.string(); Stem = P.stem().string(); E = P.extension().string(); }
                    catch (const std::exception&) { Other++; continue; }
                    for (auto& c:E) c=(char)tolower((unsigned char)c);

                    Item Md; Md.Path = PathText; Md.Name = Stem;
                    // the folder and its first level under it (a game's folder when the output
                    // folder holding several games is listed): two games' assets of one name stay two rows
                    {
                        const size_t From = PathText.find_first_not_of("\\/", std::min(Dir.size(), PathText.size()));
                        const size_t To = From == std::string::npos ? From : PathText.find_first_of("\\/", From);
                        Md.Root = Dir + '|' + (To == std::string::npos ? std::string() : PathText.substr(From, To - From));
                    }
                    Md.SizeBytes = (long long)It->file_size(Ec); // from the entry the iterator already has, no extra stat
                    Md.Written = (long long)It->last_write_time(Ec).time_since_epoch().count(); // so is this
                    Md.Format = E.empty() ? "" : E.substr(1);
                    { std::string Lp = Md.Path; for (auto& c:Lp) c=(char)tolower((unsigned char)c); Md.ModelTexture = (Lp.find("_images") != std::string::npos); }

                    if (E == ".semodel")            { Md.Kind = 0; Md.Format = "semodel"; }
                    else if (E == ".xmodel_export") { Md.Kind = 0; Md.Format = "xmodel_export"; }
                    else if (E == ".png" || E == ".dds" || E == ".tga" || E == ".iwi") Md.Kind = 1;
                    else if (E == ".wav" || E == ".flac" || E == ".opus") Md.Kind = 2;
                    else if (E == ".seanim")        { Md.Kind = 3; Md.Format = "seanim"; }
                    else if (E == ".cast")          // a model or an anim: its first node says which
                    {
                        const CastFile::Kind K = CastFile::FileKind(Md.Path);
                        if (K == CastFile::Kind::None) { Other++; continue; }
                        Md.Kind = (K == CastFile::Kind::Model) ? 0 : 3; Md.Format = "cast";
                    }
                    else if (E == ".glb")           { Md.Kind = 4; Md.Format = "glb"; }
                    else if (E == ".json" && FxPreviewPanel::IsEffectFile(PathText)) { Md.Kind = 5; Md.Format = "json"; }
                    else { Other++; continue; }
                    ByKind[Md.Kind]++;
                    Batch.push_back(std::move(Md));
                    if (std::chrono::steady_clock::now() - LastFlush > std::chrono::milliseconds(250)) Flush();
                }
            Flush();
            Scan.Detail(": " + std::to_string(Seen) + " files seen, models " + std::to_string(ByKind[0]) + ", images " +
                        std::to_string(ByKind[1]) + ", audio " + std::to_string(ByKind[2]) + ", anims " + std::to_string(ByKind[3]) +
                        ", maps " + std::to_string(ByKind[4]) + ", fx " + std::to_string(ByKind[5]) + ", other " + std::to_string(Other) + " (on a worker)");
        });
    }

    void AssetCatalog::AddFolder(const std::string& Dir)
    {
        if (std::find(ShownFolders.begin(), ShownFolders.end(), Dir) == ShownFolders.end()) ShownFolders.push_back(Dir);
        ListFolder(Dir);
    }

    void AssetCatalog::Clear()
    {
        { std::lock_guard<std::mutex> Lk(Lists->M); Lists->Generation++; Lists->Ready.clear(); Lists->Seen = 0; }
        Models.clear(); KnownPaths.clear(); AssetRows.clear(); ShownFolders.clear(); ModelsVersion++;
        { std::lock_guard<std::mutex> Lk(ScanMx); ScanQueue.clear(); ScanImages.clear(); ScanDone.clear(); }
        IdToIndex.clear();
    }

    std::string AssetCatalog::WalkedDir() const
    {
        std::lock_guard<std::mutex> Lk(Lists->M);
        return Lists->Dir;
    }

    bool AssetCatalog::TakeListed(std::vector<Item>& Batch)
    {
        std::lock_guard<std::mutex> Lk(Lists->M);
        if (Lists->Ready.empty()) return false;
        Batch.swap(Lists->Ready);
        return true;
    }

    size_t AssetCatalog::Merge(std::vector<Item>& Batch)
    {
        const size_t Sorted = Models.size();
        std::vector<ScanJob> NewJobs;
        for (auto& Md : Batch)
        {
            if (!KnownPaths.insert(Md.Path).second) continue;
            const auto [Row, Added] = AssetRows.try_emplace(AssetKey(Md), NextItemId);
            if (!Added)
            {
                // another copy of an asset listed already: the row shows the newer one
                const auto It = IdToIndex.find(Row->second);
                if (It == IdToIndex.end() || It->second >= Models.size()) continue;
                Item& Shown = Models[It->second];
                Shown.ModelTexture = Shown.ModelTexture && Md.ModelTexture;
                if (Md.Written < Shown.Written || (Md.Written == Shown.Written && Md.Path >= Shown.Path)) continue;
                Md.Id = Shown.Id; Md.ModelTexture = Shown.ModelTexture;
                Shown = std::move(Md);
                if (Shown.Kind != 2) NewJobs.push_back(ScanJob{ Shown.Id, Shown.Path, Shown.Kind, Shown.Format, Shown.SizeBytes });
                continue;
            }
            Md.Id = NextItemId++;
            if (Md.Kind != 2) NewJobs.push_back(ScanJob{ Md.Id, Md.Path, Md.Kind, Md.Format, Md.SizeBytes });
            IdToIndex[Md.Id] = Models.size();
            Models.push_back(std::move(Md));
        }
        if (!NewJobs.empty())
        {
            std::lock_guard<std::mutex> Lk(ScanMx);
            for (auto& J : NewJobs) (J.Kind == 1 ? ScanImages : ScanQueue).push_back(std::move(J));
            ScanCv.notify_all();
        }
        return Sorted;
    }

    void AssetCatalog::SortByName(size_t SortedPrefix)
    {
        const auto ByName = [](const Item& a, const Item& b){ return a.Name < b.Name; };
        SortedPrefix = std::min(SortedPrefix, Models.size());
        std::sort(Models.begin() + SortedPrefix, Models.end(), ByName);
        if (SortedPrefix > 0) std::inplace_merge(Models.begin(), Models.begin() + SortedPrefix, Models.end(), ByName);
        RebuildIdIndex();
        ModelsVersion++;
    }

    void SortModels(ShellState& S, size_t SortedPrefix)
    {
        auto& Models = S.Catalog.Models;
        auto& V = S.Viewport;
        const auto PathOf = [&](int i) { return i >= 0 && i < (int)Models.size() ? Models[i].Path : std::string(); };
        const std::string Sel = PathOf(S.Selected), AnimModel = PathOf(V.AnimModelPick);
        std::set<std::string> Picked;
        for (int i : S.MultiSel) Picked.insert(PathOf(i));
        S.Catalog.SortByName(SortedPrefix);
        S.Selected = V.AnimModelPick = -1;
        S.MultiSel.clear();
        for (int i = 0; i < (int)Models.size(); i++)
        {
            if (!Sel.empty() && Models[i].Path == Sel) S.Selected = i;
            if (!AnimModel.empty() && Models[i].Path == AnimModel) V.AnimModelPick = i;
            if (Picked.count(Models[i].Path)) S.MultiSel.insert(i);
        }
    }

    void OpenFolder(ShellState& S, const std::string& Dir, bool Replace)
    {
        if (Replace)
        {
            auto& V = S.Viewport;
            S.Catalog.Clear();
            S.Selected = -1; S.MultiSel.clear(); S.ScrollToSelected = false;
            V.AnimClipOk = false; V.AnimModelPick = -1; V.AnimBindOk = false; V.AnimPlaying = false; V.AnimFrame = 0.0f;
            V.Loader.Forget(); V.Mesh = GpuMesh{}; V.ImageSRV.Reset(); V.ImageW = V.ImageH = 0;
            V.Audio.Unload(); V.FxPanel.Unload();
        }
        S.Catalog.AddFolder(Dir);
    }

    // Once per frame: the listed entries handed back so far join Models (a path already listed is
    // skipped), their header reads queued, the list sorted again.
    static void MergeListed(ShellState& S)
    {
        std::vector<Item> Batch;
        if (!S.Catalog.TakeListed(Batch)) return;
        AppLog::Timer Merge("merge " + std::to_string(Batch.size()) + " listed assets");
        SortModels(S, S.Catalog.Merge(Batch));
    }

    void PollListing(ShellState& S)
    {
        const bool Walking = S.Catalog.Walking();   // read first: a walk that ends now has flushed
        MergeListed(S);
        SyncAnimRigs(S, Walking);
        if (!S.SelectFirstModel || Walking) return;
        S.SelectFirstModel = false;
        const auto& Models = S.Catalog.Models;
        if (Models.empty() || S.Selected >= 0) return;
        int First = 0;
        const int FirstKind = S.ShotMode && S.Assets.TypeFilter > 0 ? S.Assets.TypeFilter - 1 : 0;   // ui-grid-shot: the first of its tab
        for (int k = 0; k < (int)Models.size(); k++) if (Models[k].Kind == FirstKind) { First = k; break; }
        LoadSelection(S, First);
    }
}
