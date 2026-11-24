#include "stdafx.h"

#include "ModelImages.h"

#include "DiskGame.h"
#include "ExportNaming.h"     // Lower
#include "ExportWriters.h"    // MakeDirs
#include "MaterialDescription.h"
#include "MaterialSplit.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <nlohmann/json.hpp>

#include <filesystem>
#include <fstream>
#include <map>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <vector>

namespace fs = std::filesystem;

namespace ModelImages
{
namespace
{
    // a model's own folder, as its files name it, and the shared one from there (sidecar paths)
    constexpr const char* Own = "images/";
    constexpr const char* Shared = "../../images/";

    std::string Generic(std::string P)
    {
        std::replace(P.begin(), P.end(), '\\', '/');
        return P;
    }
    // the part of a sidecar path below the model's own images/ folder; empty when it names another
    std::string BelowOwn(const std::string& Path)
    {
        const std::string P = Generic(Path);
        const size_t N = std::char_traits<char>::length(Own);
        // (never a path that climbs out of it: a link is made, and a stray removed, below images/ only)
        if (P.size() <= N || ExportNaming::Lower(P.substr(0, N)) != Own || ("/" + P + "/").find("/../") != std::string::npos) return std::string();
        return P.substr(N);
    }

    // What the file system says a file is: the same file under two names shares Volume and Index
    struct FileId
    {
        bool Exists = false;
        DWORD Volume = 0, Links = 0;
        uint64_t Index = 0, Size = 0;
        FILETIME Written{};
        bool Same(const FileId& O) const { return Exists && O.Exists && Volume == O.Volume && Index == O.Index; }
        bool SameContentAs(const FileId& O) const
        {
            return Exists && O.Exists && Size == O.Size && CompareFileTime(&Written, &O.Written) == 0;
        }
    };
    FileId IdOf(const fs::path& P)
    {
        FileId Id;
        const HANDLE H = CreateFileW(P.c_str(), FILE_READ_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                                     OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr);
        if (H == INVALID_HANDLE_VALUE) return Id;
        BY_HANDLE_FILE_INFORMATION I{};
        if (GetFileInformationByHandle(H, &I) && !(I.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY))
        {
            Id.Exists = true;
            Id.Volume = I.dwVolumeSerialNumber;
            Id.Links = I.nNumberOfLinks;
            Id.Index = ((uint64_t)I.nFileIndexHigh << 32) | I.nFileIndexLow;
            Id.Size = ((uint64_t)I.nFileSizeHigh << 32) | I.nFileSizeLow;
            Id.Written = I.ftLastWriteTime;
        }
        CloseHandle(H);
        return Id;
    }
    bool Remove(const fs::path& P)
    {
        if (DeleteFileW(P.c_str())) return true;
        // a read-only copy (a texture copied from a read-only source keeps the attribute)
        SetFileAttributesW(P.c_str(), FILE_ATTRIBUTE_NORMAL);
        return DeleteFileW(P.c_str()) != 0;
    }

    struct Counts
    {
        std::atomic<size_t> Linked{ 0 }, Relinked{ 0 }, Kept{ 0 }, Copied{ 0 }, Missing{ 0 }, Failed{ 0 }, Pruned{ 0 }, Unread{ 0 };
    };

    // The shared files' ids, asked once per run (most textures are read by many models)
    class Sources
    {
    public:
        FileId Of(const fs::path& P)
        {
            const std::wstring Key = P.native();
            {
                std::lock_guard<std::mutex> G(Lock);
                if (const auto It = Known.find(Key); It != Known.end()) return It->second;
            }
            const FileId Id = IdOf(P);
            std::lock_guard<std::mutex> G(Lock);
            return Known.emplace(Key, Id).first->second;
        }

    private:
        std::mutex Lock;
        std::unordered_map<std::wstring, FileId> Known;
    };

    // Target the shared file Source: kept when it already is (or is a copy of its current content)
    void Place(const fs::path& Target, const fs::path& Source, Sources& Known, Counts& N)
    {
        const FileId From = Known.Of(Source);
        if (!From.Exists)
        {
            N.Missing++;
            return;
        }
        const FileId To = IdOf(Target);
        bool Stale = false;
        if (To.Exists)
        {
            if (To.Same(From) || (To.Links == 1 && To.SameContentAs(From)))
            {
                N.Kept++;
                return;
            }
            // a link to a file the shared name no longer is (replaced since: deleted and written
            // again, or made a link to another copy), or an old copy: its name freed first, so the
            // file it was (another model's link may share it) is never written through
            if (!Remove(Target))
            {
                N.Failed++;
                return;
            }
            Stale = true;
        }
        else
            ExportWriters::MakeDirs(Target.parent_path());
        if (CreateHardLinkW(Target.c_str(), Source.c_str(), nullptr))
        {
            N.Linked++;
            if (Stale) N.Relinked++;
            return;
        }
        // another volume, FAT/exFAT, NTFS's 1023 links of one file: a copy (its write time kept)
        if (CopyFileW(Source.c_str(), Target.c_str(), FALSE)) N.Copied++;
        else N.Failed++;
    }

    // A sidecar's materials' "sources" paths, as written (MaterialDescription.cpp's SidecarText)
    std::vector<std::string> SourcePaths(const fs::path& File)
    {
        std::vector<std::string> Out;
        std::ifstream In(File, std::ios::binary);
        try
        {
            const nlohmann::json Root = nlohmann::json::parse(In);
            for (const auto& M : Root.value("materials", nlohmann::json::array()))
                for (const auto& I : M.value("sources", nlohmann::json::array()))
                    if (I.is_object() && I.contains("path") && I["path"].is_string()) Out.push_back(I["path"].get<std::string>());
        }
        catch (const std::exception&) {}
        return Out;
    }

    // One model folder: its sidecars' images placed, the files of its images/ no sidecar names removed
    void Folder(const fs::path& Dir, const std::vector<fs::path>& Sidecars, bool Split, const std::string& SplitExt, Sources& Known, Counts& N)
    {
        // lower-case path below images/ -> that path as the sidecar names it
        std::map<std::string, std::string> Wanted;
        std::map<std::string, std::string> Optional;   // sources and split files: placed when there
        bool Unread = false;
        for (const fs::path& S : Sidecars)
        {
            Materials::Sidecar Side;
            if (!Materials::ReadSidecar(S.string(), Side))
            {
                N.Unread++;
                Unread = true;
                continue;
            }
            Materials::ForEachPath(Side, [&](std::string& Path) {
                if (const std::string Rest = BelowOwn(Path); !Rest.empty()) Wanted.emplace(ExportNaming::Lower(Rest), Rest);
            });
            // the material's other images (its "sources" ReadSidecar leaves out: a texture no role
            // reads, an engine built-in no zone writes): placed when there
            for (const std::string& Path : SourcePaths(S))
                if (const std::string Rest = BelowOwn(Path); !Rest.empty()) Optional.emplace(ExportNaming::Lower(Rest), Rest);
            if (!Split) continue;
            for (const auto& [Path, Outputs] : MaterialSplit::Plan(Side.Materials))
            {
                const std::string Rest = BelowOwn(Path);
                if (Rest.empty()) continue;
                const fs::path R(Rest);
                for (const MaterialSplit::Output& O : Outputs)
                {
                    const std::string Out = (R.parent_path() / (R.stem().string() + O.Suffix + SplitExt)).generic_string();
                    Optional.emplace(ExportNaming::Lower(Out), Out);
                }
            }
        }
        const fs::path Images = Dir / "images";
        const fs::path SharedDir = (Dir / fs::path(Shared)).lexically_normal();
        for (const auto& [Key, Rest] : Wanted) Place(Images / fs::path(Rest), SharedDir / fs::path(Rest), Known, N);
        for (const auto& [Key, Rest] : Optional)
        {
            if (Wanted.count(Key)) continue;
            if (Known.Of(SharedDir / fs::path(Rest)).Exists) Place(Images / fs::path(Rest), SharedDir / fs::path(Rest), Known, N);
        }
        // what no sidecar names any more (a texture the model no longer reads): removed, never
        // anything outside images/; nothing when a sidecar of the folder could not be read
        std::error_code Ec;
        if (Unread || !fs::is_directory(Images, Ec)) return;
        std::vector<fs::path> Stray;
        for (fs::recursive_directory_iterator It(Images, Ec), End; !Ec && It != End; It.increment(Ec))
        {
            if (!It->is_regular_file(Ec)) continue;
            const std::string Rel = ExportNaming::Lower(It->path().lexically_relative(Images).generic_string());
            if (!Wanted.count(Rel) && !Optional.count(Rel)) Stray.push_back(It->path());
        }
        for (const fs::path& P : Stray)
            if (Remove(P)) N.Pruned++;
    }
}

bool On(const DiskGame::Context& C)
{
    return DiskGame::ModelImagesFolder.Of(C.Settings) == "on";
}

std::string Relative(const DiskGame::Context& C)
{
    return On(C) ? "images\\\\" : "..\\\\..\\\\images\\\\";
}

void ToShared(Materials::Sidecar& S, const fs::path& File)
{
    if (File.parent_path().parent_path().filename() != DiskGame::KindFolder(DiskGame::Kind::Model)) return;
    Materials::ForEachPath(S, [](std::string& Path) {
        if (const std::string Rest = BelowOwn(Path); !Rest.empty()) Path = Shared + Rest;
    });
}

namespace
{
    // the folders Note was given, not linked yet (lower case -> as given)
    std::mutex NotedLock;
    std::map<std::wstring, fs::path> Noted;
}

void Note(const std::string& ModelFileBase)
{
    const fs::path Dir = fs::path(ModelFileBase).parent_path().lexically_normal();
    std::wstring Key = Dir.native();
    std::transform(Key.begin(), Key.end(), Key.begin(), ::towlower);
    std::lock_guard<std::mutex> G(NotedLock);
    Noted.emplace(std::move(Key), Dir);
}

void LinkForRun(const DiskGame::Context& C, DiskGame::Totals& T)
{
    const fs::path Models = (fs::path(C.OutRoot) / DiskGame::KindFolder(DiskGame::Kind::Model)).lexically_normal();
    C.Working("linking the models' images");
    // the run's model folders (below its models/; another export's, a preview's, stay noted)
    std::vector<fs::path> Dirs;
    {
        std::wstring Root = Models.native();
        std::transform(Root.begin(), Root.end(), Root.begin(), ::towlower);
        if (!Root.empty() && Root.back() != L'\\') Root += L'\\';
        std::lock_guard<std::mutex> G(NotedLock);
        for (auto It = Noted.lower_bound(Root); It != Noted.end() && It->first.compare(0, Root.size(), Root) == 0;)
        {
            Dirs.push_back(It->second);
            It = Noted.erase(It);
        }
    }
    std::map<fs::path, std::vector<fs::path>> Folders;
    std::error_code Ec;
    for (const fs::path& Dir : Dirs)
        for (fs::directory_iterator It(Dir, Ec), End; !Ec && It != End; It.increment(Ec))
        {
            const std::string Name = It->path().filename().string();
            if (Name.size() > 15 && Name.compare(Name.size() - 15, 15, ".materials.json") == 0 && It->is_regular_file(Ec))
                Folders[Dir].push_back(It->path());
        }
    if (Folders.empty())
    {
        C.Working("");
        return;
    }

    const std::vector<std::pair<fs::path, std::vector<fs::path>>> List(Folders.begin(), Folders.end());
    const bool Split = DiskGame::SplitMaps.Of(C.Settings) == "on";
    const std::string SplitExt = MaterialSplit::Extension();
    Sources Known;
    Counts N;
    std::atomic<size_t> Next{ 0 }, Done{ 0 };
    const auto Work = [&] {
        for (size_t I; (I = Next++) < List.size() && !C.Cancelled(); Done++) Folder(List[I].first, List[I].second, Split, SplitExt, Known, N);
    };
    const unsigned Threads = std::max(1u, std::min(8u, std::thread::hardware_concurrency()));
    std::vector<std::thread> Pool;
    for (unsigned I = 0; I < Threads; I++) Pool.emplace_back(Work);
    while (Done < List.size() && !C.Cancelled())
    {
        C.Working("linking the models' images", Done, List.size());
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    for (std::thread& Th : Pool) Th.join();
    C.Working("");
    const auto Add = [&](const char* Name, size_t V) {
        if (V) T.Counters[Name] += V;
    };
    Add("model_images_folders", List.size());
    Add("model_images_linked", N.Linked);
    Add("model_images_relinked", N.Relinked);
    Add("model_images_kept", N.Kept);
    Add("model_images_copied", N.Copied);
    Add("model_images_missing", N.Missing);
    Add("model_images_failed", N.Failed);
    Add("model_images_pruned", N.Pruned);
    Add("model_images_sidecars_unread", N.Unread);
}
}
