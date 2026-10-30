#include "stdafx.h"

#include "ExportWriters.h"

#include "CoDAssetType.h"
#include "CoDXModelTranslator.h"
#include "ExportNaming.h"
#include "ICoDGame.h"
#include "Image.h"
#include "SettingsManager.h"
#include "Strings.h"

#include <DbgHelp.h>
#include <d3d11.h>
#include <wrl/client.h>
#include "DirectXTex.h"

#pragma comment(lib, "d3d11.lib")

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <deque>
#include <filesystem>
#include <format>
#include <fstream>
#include <iterator>
#include <mutex>
#include <set>
#include <thread>
#include <tuple>
#include <unordered_map>
#include <unordered_set>
#include <cwctype>

namespace ExportWriters
{
    namespace
    {
        // A folder this run made or wrote into, by its path as given (normal form, lower case).
        // Created: this run made it, so it was empty then and holds since only what this run's
        // writers put there, each file named first (Claim): a name never claimed is not there.
        struct Folder
        {
            bool Made = false, Created = false;
            std::unordered_set<std::wstring> Claimed;   // file names, or bases an extension follows (lower case)
        };
        std::mutex DirsLock;
        std::unordered_map<std::wstring, Folder>& Folders()
        {
            static std::unordered_map<std::wstring, Folder> All;
            return All;
        }

        std::filesystem::path NormalDir(const std::filesystem::path& Dir)
        {
            std::filesystem::path N = Dir.lexically_normal();
            if (!N.has_filename() && N.has_relative_path()) N = N.parent_path();   // "a\b\" is "a\b"
            return N;
        }

        std::wstring LowerKey(std::wstring S)
        {
            for (wchar_t& C : S) C = (wchar_t)towlower(C);
            return S;
        }

        // MayExist's answer, the lock held
        bool MayExistLocked(const std::wstring& DirKey, const std::wstring& Name)
        {
            const auto It = Folders().find(DirKey);
            if (It == Folders().end() || !It->second.Created) return true;
            const auto& Claimed = It->second.Claimed;
            if (Claimed.count(Name)) return true;
            // a base claimed for every file named after it ("x_LOD0" for "x_LOD0.semodel", ".materials.json")
            for (size_t Dot = Name.find(L'.'); Dot != std::wstring::npos; Dot = Name.find(L'.', Dot + 1))
                if (Claimed.count(Name.substr(0, Dot))) return true;
            return false;
        }
    }

    bool MakeDirs(const std::filesystem::path& Dir)
    {
        if (Dir.empty()) return true;
        const std::filesystem::path Normal = NormalDir(Dir);
        const std::wstring Key = LowerKey(Normal.native());
        {
            std::lock_guard<std::mutex> G(DirsLock);
            if (const auto It = Folders().find(Key); It != Folders().end() && It->second.Made) return true;
        }
        // the parent first, through this same set: create_directories would ask about every folder of
        // the path again for each new folder (one CreateDirectoryW each, through the file system
        // filters). Its failure is not ours: as create_directories, the folder itself decides.
        const std::filesystem::path Parent = Normal.parent_path();
        if (Parent.has_relative_path() && Parent != Normal) MakeDirs(Parent);
        std::error_code Ec;
        const bool Created = std::filesystem::create_directory(Normal, Ec);
        if (!Created && !std::filesystem::is_directory(Normal, Ec)) return false;
        std::lock_guard<std::mutex> G(DirsLock);
        Folder& F = Folders()[Key];
        if (!F.Made) F.Made = true, F.Created = Created;   // a racing thread that found it there: not ours to vouch for
        return true;
    }

    void ForgetDirs()
    {
        std::lock_guard<std::mutex> G(DirsLock);
        Folders().clear();
    }

    bool MayExist(const std::filesystem::path& File)
    {
        const std::wstring DirKey = LowerKey(NormalDir(File.parent_path()).native()), Name = LowerKey(File.filename().native());
        std::lock_guard<std::mutex> G(DirsLock);
        return MayExistLocked(DirKey, Name);
    }

    bool Claim(const std::filesystem::path& File)
    {
        const std::wstring DirKey = LowerKey(NormalDir(File.parent_path()).native()), Name = LowerKey(File.filename().native());
        std::lock_guard<std::mutex> G(DirsLock);
        const bool Was = MayExistLocked(DirKey, Name);
        // kept even for a folder not made yet: the thread making it may find it Created after this
        Folders()[DirKey].Claimed.insert(Name);
        return Was;
    }

    bool HasFile(const std::filesystem::path& File)
    {
        std::error_code Ec;
        return MayExist(File) && std::filesystem::is_regular_file(File, Ec);
    }

    bool WriteBytes(const std::filesystem::path& P, const uint8_t* Data, size_t Size)
    {
        Claim(P);   // before the file exists: a skip check (HasFile) then asks the disk
        MakeDirs(P.parent_path());
        std::ofstream O(P, std::ios::binary);
        if (Size) O.write(reinterpret_cast<const char*>(Data), (std::streamsize)Size);
        return (bool)O;
    }

    bool ReadBytes(const std::filesystem::path& P, std::vector<uint8_t>& Out)
    {
        std::ifstream F(P, std::ios::binary);
        if (!F) return false;
        Out.assign(std::istreambuf_iterator<char>(F), std::istreambuf_iterator<char>());
        return true;
    }

namespace
{
    bool EnvSet(const char* Name, const char* Legacy)
    {
        return GetEnvironmentVariableA(Name, nullptr, 0) != 0 || GetEnvironmentVariableA(Legacy, nullptr, 0) != 0;
    }

    bool EnvValue(const char* Name, const char* Legacy, char* Out, DWORD Size)
    {
        return GetEnvironmentVariableA(Name, Out, Size) != 0 || GetEnvironmentVariableA(Legacy, Out, Size) != 0;
    }

    // Content key -> the file already holding those bytes, for the whole process (every zone).
    std::mutex DedupLock;
    std::unordered_map<uint64_t, std::string>& Written()
    {
        static std::unordered_map<uint64_t, std::string> Map;
        return Map;
    }

    uint64_t Micros(std::chrono::steady_clock::time_point Since)
    {
        return (uint64_t)std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - Since).count();
    }

    // True when the file at Path holds exactly Size bytes equal to Data: its size, then its bytes,
    // read (cheaper than a write) and compared a chunk at a time.
    bool Holds(const std::string& Path, const uint8_t* Data, size_t Size)
    {
        const HANDLE F = CreateFileA(Path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
        if (F == INVALID_HANDLE_VALUE) return false;
        LARGE_INTEGER Length{};
        bool Same = GetFileSizeEx(F, &Length) && (uint64_t)Length.QuadPart == Size;
        std::vector<uint8_t> Chunk(Same ? std::min<size_t>(Size, (size_t)1 << 20) : 0);
        for (size_t At = 0; Same && At < Size;)
        {
            const DWORD Want = (DWORD)std::min(Chunk.size(), Size - At);
            DWORD Got = 0;
            Same = ReadFile(F, Chunk.data(), Want, &Got, nullptr) && Got == Want && std::memcmp(Chunk.data(), Data + At, Want) == 0;
            At += Want;
        }
        CloseHandle(F);
        return Same;
    }

    // WriteImage, but a DDS already there with the very same bytes is not rewritten (the other
    // formats are encoded straight to the file, so theirs are not known before writing).
    bool Put(const XImageDDS& Dds, const std::string& Path, const std::string& Format)
    {
        if (Format == "DDS" && Dds.DataBuffer && Dds.DataSize &&
            Holds(Path, reinterpret_cast<const uint8_t*>(Dds.DataBuffer), Dds.DataSize))
        {
            Times().Unchanged++;
            return true;
        }
        return WriteImage(Dds, Path, Format);
    }
}

WriterTimes& Times()
{
    static WriterTimes T;
    return T;
}

bool WriteMinidump(const std::string& Path)
{
    HANDLE F = CreateFileA(Path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (F == INVALID_HANDLE_VALUE) return false;
    const auto Type = (MINIDUMP_TYPE)(MiniDumpNormal | MiniDumpWithThreadInfo | MiniDumpWithUnloadedModules);
    const bool Ok = MiniDumpWriteDump(GetCurrentProcess(), GetCurrentProcessId(), F, Type, nullptr, nullptr, nullptr) != FALSE;
    CloseHandle(F);
    return Ok;
}

std::string ImageFormatSetting()
{
    return SettingsManager::GetSetting("exportimg", "PNG");
}

std::string ImageExtension(const std::string& Format)
{
    if (Format == "DDS") return ".dds";
    if (Format == "TGA") return ".tga";
    if (Format == "TIFF") return ".tiff";
    return ".png";
}

bool WriteImage(const XImageDDS& Dds, const std::string& Path, const std::string& Format)
{
    if (!Dds.DataBuffer || !Dds.DataSize) return false;
    if (Format == "DDS")
    {
        std::ofstream F(Path, std::ios::binary);
        F.write(reinterpret_cast<const char*>(Dds.DataBuffer), Dds.DataSize);
        return F.good();
    }
    // an engine built-in ("$identitynormalmap", "$white"), 1x1 in the zone: written 4x4 of its pixel,
    // the smallest a mod tool's linker takes (BO3's refuses a normal map under 4x4)
    const bool BuiltIn = std::filesystem::path(Path).filename().string().rfind('$', 0) == 0;
    return Image::ConvertImageMemory(Dds.DataBuffer, Dds.DataSize, ImageFormat::DDS_WithHeader, Path,
                                     Format == "TGA" ? ImageFormat::Standard_TGA : ImageFormat::Standard_PNG,
                                     Dds.ImagePatchType, BuiltIn ? 4 : 0);
}

struct WorkerPool::State
{
    std::mutex QueueLock;
    std::condition_variable HasJob, HasRoom;
    std::deque<std::function<void()>> Queue;
    size_t MaxQueued = 64;
    bool Finished = false;
    std::vector<std::thread> Threads;
};

unsigned WorkerPool::DefaultWorkers()
{
    char Env[16] = {};
    if (EnvValue("GH_CONVERT_THREADS", "GH_BO4_CONVERT_THREADS", Env, sizeof(Env))) return (unsigned)atoi(Env);
    // physical cores (SMT siblings count once); half of them, 70% past 12, so the machine stays usable
    unsigned Cores = 0;
    DWORD Bytes = 0;
    GetLogicalProcessorInformationEx(RelationProcessorCore, nullptr, &Bytes);
    std::vector<uint8_t> Info(Bytes);
    if (Bytes && GetLogicalProcessorInformationEx(RelationProcessorCore, reinterpret_cast<PSYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX>(Info.data()), &Bytes))
        for (DWORD At = 0; At < Bytes; At += reinterpret_cast<PSYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX>(Info.data() + At)->Size)
            Cores++;
    if (Cores == 0) Cores = std::max(1u, std::thread::hardware_concurrency() / 2);
    return std::max(1u, Cores > 12 ? Cores * 7 / 10 : Cores / 2);
}

WorkerPool::WorkerPool(unsigned Workers, size_t MaxQueued) : St(std::make_unique<State>())
{
    St->MaxQueued = std::max<size_t>(1, MaxQueued);
    State* S = St.get();
    for (unsigned K = 0; K < Workers; K++)
        S->Threads.emplace_back([S]() {
            // the WIC fallback (a PNG fpng cannot take) needs COM on the thread that calls it. Never
            // CoUninitialize here: tearing the process's MTA down after one pool left DirectXTex's
            // cached WIC factory dangling for the next.
            CoInitializeEx(nullptr, COINIT_MULTITHREADED);
            for (;;)
            {
                std::function<void()> Job;
                {
                    std::unique_lock<std::mutex> L(S->QueueLock);
                    S->HasJob.wait(L, [S]() { return !S->Queue.empty() || S->Finished; });
                    if (S->Queue.empty()) break;
                    Job = std::move(S->Queue.front());
                    S->Queue.pop_front();
                }
                S->HasRoom.notify_one();
                Job();
            }
        });
}

WorkerPool::~WorkerPool()
{
    Drain();
}

unsigned WorkerPool::Workers() const
{
    return (unsigned)St->Threads.size();
}

void WorkerPool::Post(std::function<void()> Job)
{
    if (St->Threads.empty())
    {
        Job();
        return;
    }
    {
        std::unique_lock<std::mutex> L(St->QueueLock);
        const auto Room = [this]() { return St->Queue.size() < St->MaxQueued; };
        if (!Room())
        {
            // timed only when it blocks: the pool is what this thread waits on
            const auto Start = std::chrono::steady_clock::now();
            St->HasRoom.wait(L, Room);
            Times().QueueWaitUs += Micros(Start);
        }
        St->Queue.push_back(std::move(Job));
    }
    St->HasJob.notify_one();
}

void WorkerPool::Drain()
{
    {
        std::lock_guard<std::mutex> L(St->QueueLock);
        St->Finished = true;
    }
    St->HasJob.notify_all();
    for (auto& T : St->Threads)
        if (T.joinable()) T.join();
}

struct ImageSink::State
{
    std::mutex StatsLock;
    std::vector<std::tuple<std::string, std::string, size_t>> Links;   // link, target, tag
    ImageSinkStats Stats;
    WorkerPool Pool;   // last: destroyed (drained) first, while its jobs' stats are still there
};

ImageSink::ImageSink(std::string Format) : St(std::make_unique<State>()), Fmt(std::move(Format))
{
    Ext = ImageExtension(Fmt);
    UseDedup = !EnvSet("GH_NODEDUP", "GH_BO4_NODEDUP");
    Times().Workers = St->Pool.Workers();
}

ImageSink::~ImageSink() = default;

bool ImageSink::LinkIfKnown(uint64_t Key, const std::string& Path, size_t Tag)
{
    if (!UseDedup || Key == 0) return false;
    std::lock_guard<std::mutex> G(DedupLock);
    const auto [It, New] = Written().try_emplace(Key, Path);
    if (New) return false;
    // the same bytes to the same file (a name shared by zones): it is already there
    if (It->second != Path) St->Links.emplace_back(Path, It->second, Tag);
    return true;
}

void ImageSink::Write(std::unique_ptr<XImageDDS> Dds, const std::string& Path)
{
    // a std::function is copyable: the image goes in shared
    St->Pool.Post([S = St.get(), F = Fmt, D = std::shared_ptr<XImageDDS>(std::move(Dds)), Path]() {
        const auto Start = std::chrono::steady_clock::now();
        const bool Ok = D && Put(*D, Path, F);
        Times().ConvertUs += Micros(Start);
        Times().Converted++;
        std::lock_guard<std::mutex> G(S->StatsLock);
        if (Ok) S->Stats.Written++; else S->Stats.Failed++;
    });
}

void ImageSink::Write(std::function<std::unique_ptr<XImageDDS>()> Build, const std::string& Path)
{
    St->Pool.Post([S = St.get(), F = Fmt, B = std::move(Build), Path]() {
        const auto Start = std::chrono::steady_clock::now();
        std::unique_ptr<XImageDDS> D;
        try { D = B(); }
        catch (const std::exception&) {}
        if (!D) return;
        const bool Ok = Put(*D, Path, F);
        Times().ConvertUs += Micros(Start);
        Times().Converted++;
        std::lock_guard<std::mutex> G(S->StatsLock);
        if (Ok) S->Stats.Written++; else S->Stats.Failed++;
    });
}

ImageSinkStats ImageSink::Finish(const std::function<std::unique_ptr<XImageDDS>(size_t)>& Rebuild, size_t& Missing,
                                 const std::function<uint64_t(size_t)>& KeyOf)
{
    const auto Drain = std::chrono::steady_clock::now();
    St->Pool.Drain();
    Times().DrainUs += Micros(Drain);

    // Every file this sink wrote now exists: make the links.
    const auto Linking = std::chrono::steady_clock::now();
    for (const auto& [LinkPath, Target, Tag] : St->Links)
    {
        const std::wstring W = Strings::ToUnicodeString(LinkPath);
        DeleteFileW(W.c_str());
        if (CreateHardLinkW(W.c_str(), Strings::ToUnicodeString(Target).c_str(), nullptr))
        {
            St->Stats.Written++;
            St->Stats.Linked++;
            continue;
        }
        auto Dds = Rebuild ? Rebuild(Tag) : nullptr;
        if (!Dds)
            Missing++;
        else if (Put(*Dds, LinkPath, Fmt))
        {
            St->Stats.Written++;
            std::lock_guard<std::mutex> G(DedupLock);
            if (const uint64_t Key = KeyOf ? KeyOf(Tag) : 0) Written()[Key] = LinkPath;
        }
        else
            St->Stats.Failed++;
    }
    St->Links.clear();
    Times().LinkUs += Micros(Linking);
    return St->Stats;
}

std::string CsvField(const std::string& S)
{
    if (S.find_first_of(",\"\n\r") == std::string::npos) return S;
    std::string Out = "\"";
    for (char C : S)
    {
        Out += C;
        if (C == '"') Out += C;
    }
    Out += "\"";
    return Out;
}

bool WriteStringTable(const std::string& DataDir, const std::string& Name, uint32_t Columns, uint32_t Rows,
                      const std::vector<std::string>& Cells)
{
    // some names already end in ".csv" (named after their own source file, "budgets.csv")
    std::string File = ExportNaming::SafeFileName(Name);
    if (File.size() < 4 || ExportNaming::Lower(File.substr(File.size() - 4)) != ".csv") File += ".csv";
    const std::filesystem::path Dir = std::filesystem::path(DataDir) / "stringtables";
    MakeDirs(Dir);
    std::ofstream Out(Dir / File, std::ios::binary | std::ios::trunc);
    if (!Out) return false;
    for (uint32_t Row = 0; Row < Rows; Row++)
    {
        for (uint32_t Col = 0; Col < Columns; Col++)
        {
            if (Col) Out << ",";
            const size_t Index = (size_t)Row * Columns + Col;
            if (Index < Cells.size()) Out << CsvField(Cells[Index]);
        }
        Out << "\r\n";
    }
    return (bool)Out;
}

std::string Utf8(const std::string& S, unsigned CodePage, bool Always)
{
    bool Valid = !(Always && CodePage != 0);
    for (size_t I = 0; I < S.size() && Valid;)
    {
        const unsigned char C = (unsigned char)S[I];
        const size_t N = C < 0x80 ? 1 : (C >> 5) == 6 ? 2 : (C >> 4) == 14 ? 3 : (C >> 3) == 30 ? 4 : 0;
        if (N == 0 || I + N > S.size()) Valid = false;
        for (size_t K = 1; K < N && Valid; K++) Valid = ((unsigned char)S[I + K] >> 6) == 2;
        I += N == 0 ? 1 : N;
    }
    if (Valid) return S;
    if (CodePage != 0)
    {
        const int Wide = MultiByteToWideChar(CodePage, 0, S.data(), (int)S.size(), nullptr, 0);
        std::wstring W((size_t)std::max(Wide, 0), L'\0');
        if (Wide > 0 && MultiByteToWideChar(CodePage, 0, S.data(), (int)S.size(), W.data(), Wide) == Wide)
        {
            const int Bytes = WideCharToMultiByte(CP_UTF8, 0, W.data(), Wide, nullptr, 0, nullptr, nullptr);
            std::string Out((size_t)std::max(Bytes, 0), '\0');
            if (Bytes > 0 && WideCharToMultiByte(CP_UTF8, 0, W.data(), Wide, Out.data(), Bytes, nullptr, nullptr) == Bytes)
                return Out;
        }
    }
    static const uint16_t High[32] = { 0x20ac, 0x81,   0x201a, 0x192,  0x201e, 0x2026, 0x2020, 0x2021, 0x2c6,  0x2030, 0x160,
                                       0x2039, 0x152,  0x8d,   0x17d,  0x8f,   0x90,   0x2018, 0x2019, 0x201c, 0x201d, 0x2022,
                                       0x2013, 0x2014, 0x2dc,  0x2122, 0x161,  0x203a, 0x153,  0x9d,   0x17e,  0x178 };
    std::string Out;
    for (unsigned char C : S)
    {
        const uint32_t U = C >= 0x80 && C < 0xa0 ? High[C - 0x80] : C;
        if (U < 0x80) Out += (char)U;
        else if (U < 0x800) { Out += (char)(0xc0 | (U >> 6)); Out += (char)(0x80 | (U & 0x3f)); }
        else { Out += (char)(0xe0 | (U >> 12)); Out += (char)(0x80 | ((U >> 6) & 0x3f)); Out += (char)(0x80 | (U & 0x3f)); }
    }
    return Out;
}

unsigned LanguageCodePage(const std::string& PathOrLanguage)
{
    const std::string L = ExportNaming::Lower(PathOrLanguage);
    if (L.find("russian") != std::string::npos) return 1251;
    for (const char* Word : { "polish", "czech" })
        if (L.find(Word) != std::string::npos) return 1250;
    return 0;
}

bool WriteLocalize(const std::string& Path, const std::vector<std::pair<std::string, std::string>>& Rows, unsigned CodePage)
{
    std::string Csv = "key,text\r\n";
    for (const auto& [Key, Text] : Rows) Csv += CsvField(Utf8(Key, CodePage)) + "," + CsvField(Utf8(Text, CodePage)) + "\r\n";
    std::ofstream Out(Path, std::ios::binary | std::ios::trunc);
    return Out && (Out << Csv);
}

namespace
{
    thread_local std::string LastCrash;

    LONG NoteCrash(EXCEPTION_POINTERS* Info)
    {
        const auto* R = Info->ExceptionRecord;
        LastCrash = std::format("seh {:08x} at +{:x}", (unsigned)R->ExceptionCode,
            (unsigned long long)((uint64_t)R->ExceptionAddress - (uint64_t)GetModuleHandleA(nullptr)));
        // MSVC C++ throw: parameters [2] ThrowInfo, [3] image base; name the first catchable type
        if (R->ExceptionCode == 0xE06D7363 && R->NumberParameters >= 4 && R->ExceptionInformation[2])
        {
            const uint64_t Base = R->ExceptionInformation[3];
            const auto* Throw = reinterpret_cast<const int32_t*>(R->ExceptionInformation[2]);
            const auto* Types = reinterpret_cast<const int32_t*>(Base + (uint32_t)Throw[3]);
            const auto* First = reinterpret_cast<const int32_t*>(Base + (uint32_t)Types[1]);
            LastCrash += std::format(" type {}", reinterpret_cast<const char*>(Base + (uint32_t)First[1] + 16));
            // a std::exception (one of its catchable types): its what(), the object at [1]
            for (int32_t I = 0; I < Types[0] && R->ExceptionInformation[1]; I++)
            {
                const auto* Type = reinterpret_cast<const int32_t*>(Base + (uint32_t)Types[1 + I]);
                if (strcmp(reinterpret_cast<const char*>(Base + (uint32_t)Type[1] + 16), ".?AVexception@std@@") != 0) continue;
                LastCrash += std::format(": {}", reinterpret_cast<const std::exception*>(R->ExceptionInformation[1])->what());
                break;
            }
        }
        static volatile LONG Dumped = 0;
        char Path[MAX_PATH] = {};
        if (EnvValue("GH_EXPORT_CRASHDUMP", "GH_BO4_CRASHDUMP", Path, MAX_PATH) && InterlockedExchange(&Dumped, 1) == 0)
        {
            HANDLE F = CreateFileA(Path, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
            if (F != INVALID_HANDLE_VALUE)
            {
                MINIDUMP_EXCEPTION_INFORMATION Mei{};
                Mei.ThreadId = GetCurrentThreadId();
                Mei.ExceptionPointers = Info;
                MiniDumpWriteDump(GetCurrentProcess(), GetCurrentProcessId(), F, MiniDumpWithFullMemory, &Mei, nullptr, nullptr);
                CloseHandle(F);
            }
        }
        return EXCEPTION_EXECUTE_HANDLER;
    }

    // No C++ object may live in a function with __try: these take everything prepared.
    int ModelSeh(const CoDModel_t* M, const std::string* Dir, const ModelTextures* T)
    {
        __try { return static_cast<int>(LiveExport::ExportModel(M, *Dir, T->ImagesDir, T->Relative, T->Extension, T->Take ? &T->Take : nullptr,
                                                                    T->Fill ? &T->Fill : nullptr)); }
        __except (NoteCrash(GetExceptionInformation())) { return -1; }
    }

    struct CompositeCall
    {
        const std::string* Name;
        const std::vector<CoDModel_t>* Parts;
        const std::string* Dir;
        const ModelTextures* T;
        size_t* Read;
    };
    int CompositeSeh(const CompositeCall* C)
    {
        __try
        {
            return static_cast<int>(LiveExport::ExportCompositeModel(*C->Name, *C->Parts, *C->Dir, C->T->ImagesDir, C->T->Relative, C->T->Extension,
                                                                     C->T->Take ? &C->T->Take : nullptr, C->Read,
                                                                     C->T->Fill ? &C->T->Fill : nullptr));
        }
        __except (NoteCrash(GetExceptionInformation())) { return -1; }
    }

    int AnimSeh(const CoDAnim_t* A, const std::string* Dir)
    {
        __try { return static_cast<int>(LiveExport::ExportAnim(A, *Dir)); }
        __except (NoteCrash(GetExceptionInformation())) { return -1; }
    }

    int SoundSeh(const CoDSound_t* S, const std::string* Dir, const std::string* Ext)
    {
        __try { return static_cast<int>(LiveExport::ExportSound(S, *Dir, *Ext)); }
        __except (NoteCrash(GetExceptionInformation())) { return -1; }
    }

    template <class F>
    int Guarded(F Call, std::string& Crash)
    {
        LastCrash.clear();
        int Code = -1;
        try
        {
            Code = Call();
        }
        catch (const std::exception& E)
        {
            LastCrash = std::string("exception ") + E.what();
        }
        Crash = LastCrash;
        return Code;
    }
}

namespace
{
    int ReadModelInto(const CoDModel_t* M, LiveModel* Out)
    {
        Out->Generic = LiveExport::ReadModel(M);
        if (!Out->Generic) return 1;
        Out->Lod = CoDXModelTranslator::CalculateBiggestLodIndex(Out->Generic);
        if (Out->Lod < 0) return 1;
        Out->Mesh = CoDXModelTranslator::TranslateXModel(Out->Generic, (uint32_t)Out->Lod);
        return Out->Mesh ? 0 : 1;
    }

    int ReadModelSeh(const CoDModel_t* M, LiveModel* Out)
    {
        __try { return ReadModelInto(M, Out); }
        __except (NoteCrash(GetExceptionInformation())) { return -1; }
    }
}

LiveModel::LiveModel() = default;
LiveModel::~LiveModel() = default;

int ReadModel(const CoDModel_t& Model, LiveModel& Out, std::string& Crash)
{
    return Guarded([&]() { return ReadModelSeh(&Model, &Out); }, Crash);
}

size_t PngCopies(const std::vector<std::pair<std::string, std::string>>& SourceTarget, size_t& Failed)
{
    namespace fs = std::filesystem;
    ImageSink Sink("PNG");
    size_t Done = 0, Missing = 0;
    std::error_code Ec;
    for (const auto& [Source, Target] : SourceTarget)
    {
        if (fs::exists(Target, Ec))
        {
            Done++;
            continue;
        }
        MakeDirs(fs::path(Target).parent_path());
        if (ExportNaming::Lower(fs::path(Source).extension().string()) == ".png")
        {
            const std::wstring W = Strings::ToUnicodeString(Target);
            if (CreateHardLinkW(W.c_str(), Strings::ToUnicodeString(Source).c_str(), nullptr) || fs::copy_file(Source, Target, Ec)) Done++;
            else Failed++;
            continue;
        }
        if (Sink.LinkIfKnown(ExportNaming::Fnv1a("png:" + ExportNaming::Lower(Source)), Target)) continue;
        std::ifstream In(Source, std::ios::binary | std::ios::ate);
        const std::streamsize Size = In ? (std::streamsize)In.tellg() : 0;
        if (Size <= 0 || ExportNaming::Lower(fs::path(Source).extension().string()) != ".dds")
        {
            Failed++;   // not written (a dependency not found), or a format only its writer reads
            continue;
        }
        auto Dds = std::make_unique<XImageDDS>();
        Dds->DataBuffer = new int8_t[(size_t)Size];
        Dds->DataSize = (uint32_t)Size;
        In.seekg(0);
        In.read(reinterpret_cast<char*>(Dds->DataBuffer), Size);
        Sink.Write(std::move(Dds), Target);
    }
    const ImageSinkStats S = Sink.Finish(nullptr, Missing);
    Failed += S.Failed + Missing;
    return Done + S.Written;
}

size_t UnsignedDdsCopies(const std::vector<std::pair<std::string, std::string>>& SourceTarget, size_t& Failed)
{
    namespace fs = std::filesystem;
    // DXGI signed format -> its UNORM twin, and the bytes to shift: a BC4 half block is its two
    // endpoints then indices (an order a +128 keeps, so the 8/6 value mode stays), a texel a byte
    struct Remap { uint32_t From, To, Block; std::vector<size_t> Bytes; };
    static const std::vector<Remap> Remaps = {
        { 81, 80, 8, { 0, 1 } },            // BC4_SNORM
        { 84, 83, 16, { 0, 1, 8, 9 } },     // BC5_SNORM
        { 51, 49, 2, { 0, 1 } },            // R8G8_SNORM
        { 31, 28, 4, { 0, 1, 2, 3 } },      // R8G8B8A8_SNORM
    };
    constexpr size_t Header = 148;   // "DDS " + DDS_HEADER + DDS_HEADER_DXT10
    size_t Done = 0;
    std::error_code Ec;
    for (const auto& [Source, Target] : SourceTarget)
    {
        std::ifstream In(Source, std::ios::binary);
        std::vector<char> Data((std::istreambuf_iterator<char>(In)), std::istreambuf_iterator<char>());
        if (Data.size() < Header || std::memcmp(Data.data(), "DDS ", 4) != 0 || std::memcmp(Data.data() + 84, "DX10", 4) != 0)
        {
            if (Data.empty()) Failed++;   // not written (a dependency not found)
            continue;
        }
        uint32_t Format = 0;
        std::memcpy(&Format, Data.data() + 128, 4);
        const auto R = std::find_if(Remaps.begin(), Remaps.end(), [&](const Remap& M) { return M.From == Format; });
        if (R == Remaps.end()) continue;
        if (fs::exists(Target, Ec))
        {
            Done++;
            continue;
        }
        std::memcpy(Data.data() + 128, &R->To, 4);
        for (size_t At = Header; At + R->Block <= Data.size(); At += R->Block)
            for (const size_t B : R->Bytes) Data[At + B] = (char)(Data[At + B] ^ 0x80);
        MakeDirs(fs::path(Target).parent_path());
        std::ofstream Out(Target, std::ios::binary | std::ios::trunc);
        Out.write(Data.data(), (std::streamsize)Data.size());
        if (Out) Done++;
        else Failed++;
    }
    return Done;
}

namespace
{
    // One D3D11 device for DirectXTex's DirectCompute BC6H/BC7 encoders, made on first use (null
    // without a DirectCompute-capable GPU: the CPU encoders then).
    ID3D11Device* CompressDevice()
    {
        static std::mutex Lock;
        static Microsoft::WRL::ComPtr<ID3D11Device> Device;
        static bool Tried = false;
        std::lock_guard<std::mutex> Guard(Lock);
        if (!Tried)
        {
            Tried = true;
            const D3D_FEATURE_LEVEL Levels[] = { D3D_FEATURE_LEVEL_11_0 };
            if (FAILED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, Levels, 1, D3D11_SDK_VERSION,
                                         Device.GetAddressOf(), nullptr, nullptr)))
                Device.Reset();
        }
        return Device.Get();
    }
}

size_t ResizedDdsCopies(const std::vector<std::pair<std::string, std::string>>& SourceTarget, uint32_t MaxSize,
                        size_t& Failed, double& Seconds, bool& Gpu)
{
    namespace fs = std::filesystem;
    using namespace DirectX;
    const auto T0 = std::chrono::steady_clock::now();
    size_t Done = 0;
    std::error_code Ec;
    for (const auto& [Source, Target] : SourceTarget)
    {
        const std::wstring From = Strings::ToUnicodeString(fs::exists(Target, Ec) ? Target : Source);
        TexMetadata Meta;
        ScratchImage Src;
        if (FAILED(GetMetadataFromDDSFile(From.c_str(), DDS_FLAGS_NONE, Meta)))
        {
            if (!fs::exists(Source, Ec)) Failed++;   // not written (a dependency not found)
            continue;
        }
        if ((std::max)(Meta.width, Meta.height) <= MaxSize || Meta.dimension != TEX_DIMENSION_TEXTURE2D ||
            Meta.arraySize != 1 || Meta.depth != 1)
            continue;
        const DXGI_FORMAT Format = Meta.format;
        const double Scale = (double)MaxSize / (double)(std::max)(Meta.width, Meta.height);
        const size_t W = (std::max<size_t>)(4, (size_t)(Meta.width * Scale)), H = (std::max<size_t>)(4, (size_t)(Meta.height * Scale));
        ScratchImage Raw, Small, Mipped, Out;
        const bool Ok = SUCCEEDED(LoadFromDDSFile(From.c_str(), DDS_FLAGS_NONE, &Meta, Src)) &&
            (!IsCompressed(Format) ? SUCCEEDED(Raw.InitializeFromImage(*Src.GetImage(0, 0, 0)))
                                   : SUCCEEDED(Decompress(*Src.GetImage(0, 0, 0), DXGI_FORMAT_UNKNOWN, Raw))) &&
            SUCCEEDED(Resize(*Raw.GetImage(0, 0, 0), W, H, TEX_FILTER_FORCE_NON_WIC, Small)) &&
            SUCCEEDED(GenerateMipMaps(*Small.GetImage(0, 0, 0), TEX_FILTER_FORCE_NON_WIC, 0, Mipped));
        if (!Ok)
        {
            Failed++;
            continue;
        }
        const ScratchImage* Final = &Mipped;
        if (IsCompressed(Format))
        {
            const bool Bc67 = Format >= DXGI_FORMAT_BC6H_TYPELESS && Format <= DXGI_FORMAT_BC7_UNORM_SRGB;
            ID3D11Device* Device = Bc67 ? CompressDevice() : nullptr;
            HRESULT Hr = E_FAIL;
            if (Device)
                Hr = Compress(Device, Mipped.GetImages(), Mipped.GetImageCount(), Mipped.GetMetadata(), Format,
                              TEX_COMPRESS_DEFAULT, TEX_ALPHA_WEIGHT_DEFAULT, Out);
            if (FAILED(Hr))
                Hr = Compress(Mipped.GetImages(), Mipped.GetImageCount(), Mipped.GetMetadata(), Format,
                              TEX_COMPRESS_PARALLEL | (Bc67 ? TEX_COMPRESS_BC7_QUICK : TEX_COMPRESS_DEFAULT), TEX_THRESHOLD_DEFAULT, Out);
            else
                Gpu = true;
            if (FAILED(Hr))
            {
                Failed++;
                continue;
            }
            Final = &Out;
        }
        MakeDirs(fs::path(Target).parent_path());
        if (SUCCEEDED(SaveToDDSFile(Final->GetImages(), Final->GetImageCount(), Final->GetMetadata(), DDS_FLAGS_NONE,
                                    Strings::ToUnicodeString(Target).c_str())))
            Done++;
        else
            Failed++;
    }
    Seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - T0).count();
    return Done;
}

int ExportModel(const CoDModel_t& Model, const std::string& Dir, const ModelTextures& Textures, std::string& Crash)
{
    return Guarded([&]() { return ModelSeh(&Model, &Dir, &Textures); }, Crash);
}

namespace
{
    int ModelFilesSeh(const std::function<void()>* Job)
    {
        __try
        {
            (*Job)();
            return 0;
        }
        __except (NoteCrash(GetExceptionInformation())) { return -1; }
    }
}

struct ModelFilesOnPool::State
{
    std::mutex Lock;
    std::condition_variable Done;
    std::set<std::string> Busy;   // the Keys being written
    std::vector<std::pair<size_t, std::string>> Failed;
    size_t Tag = 0;
    LiveExport::ModelFileJobs Post;
    const LiveExport::ModelFileJobs* Was = nullptr;   // the thread's before this one
    bool Installed = false;
    // a few models queued: one may weigh hundreds of MB
    WorkerPool Pool{ WorkerPool::DefaultWorkers(), 8 };
};

ModelFilesOnPool::ModelFilesOnPool() : St(std::make_unique<State>())
{
    State* S = St.get();
    S->Post = [S](const std::string& Key, std::function<void()> Job) {
        {
            // the same files written twice in a run (one name twice in a zone): one after the other,
            // the last as it would be written now
            std::unique_lock<std::mutex> L(S->Lock);
            S->Done.wait(L, [&]() { return !S->Busy.count(Key); });
            S->Busy.insert(Key);
        }
        S->Pool.Post([S, Key, Tag = S->Tag, Job = std::move(Job)]() {
            std::string Crash;
            const int Code = Guarded([&]() { return ModelFilesSeh(&Job); }, Crash);
            {
                std::lock_guard<std::mutex> L(S->Lock);
                if (Code != 0) S->Failed.emplace_back(Tag, "code " + std::to_string(Code) + (Crash.empty() ? "" : " " + Crash));
                S->Busy.erase(Key);
            }
            S->Done.notify_all();
        });
    };
    S->Was = LiveExport::WriteModelFilesWith(&S->Post);
    S->Installed = true;
}

ModelFilesOnPool::~ModelFilesOnPool()
{
    Finish();
}

void ModelFilesOnPool::Owner(size_t Tag)
{
    St->Tag = Tag;
}

std::vector<std::pair<size_t, std::string>> ModelFilesOnPool::Finish()
{
    if (St->Installed) LiveExport::WriteModelFilesWith(St->Was);
    St->Installed = false;
    St->Pool.Drain();
    std::vector<std::pair<size_t, std::string>> Out;
    std::lock_guard<std::mutex> L(St->Lock);
    Out.swap(St->Failed);
    std::sort(Out.begin(), Out.end());
    return Out;
}

int ExportComposite(const std::string& Name, const std::vector<CoDModel_t>& Parts, const std::string& Dir, const ModelTextures& Textures,
                    size_t& PartsRead, std::string& Crash)
{
    const CompositeCall Call{ &Name, &Parts, &Dir, &Textures, &PartsRead };
    return Guarded([&]() { return CompositeSeh(&Call); }, Crash);
}

int ExportAnim(const CoDAnim_t& Anim, const std::string& Dir, std::string& Crash)
{
    return Guarded([&]() { return AnimSeh(&Anim, &Dir); }, Crash);
}

int ExportSound(const CoDSound_t& Sound, const std::string& Dir, const std::string& Extension, std::string& Crash)
{
    return Guarded([&]() { return SoundSeh(&Sound, &Dir, &Extension); }, Crash);
}

namespace
{
    // One sound for a SoundSink's pool: its source, the bytes it points into (Keep), where it goes
    struct SoundJob
    {
        SoundSource Source;
        std::shared_ptr<const void> Keep;
        std::filesystem::path Path;
        std::string Format;
        bool AsRead = false;   // the bytes as they are (ICoDGame::SoundsAsRead, an empty sound)
        bool Ok = false;
        std::string Why;
    };

    void WriteSoundJob(SoundJob* J)
    {
        J->Ok = J->AsRead ? WriteBytes(J->Path, J->Source.Data, J->Source.Size) : WriteSound(J->Source, J->Path, J->Format, &J->Why);
    }

    int WriteSoundJobSeh(SoundJob* J)
    {
        __try { WriteSoundJob(J); return 0; }
        __except (NoteCrash(GetExceptionInformation())) { return -1; }
    }

    void ReadSoundInto(const ICoDGame* Game, const CoDSound_t* Sound, std::unique_ptr<XSound>* Out)
    {
        *Out = Game->ReadXSound(Sound);
    }

    int ReadSoundSeh(const ICoDGame* Game, const CoDSound_t* Sound, std::unique_ptr<XSound>* Out)
    {
        __try { ReadSoundInto(Game, Sound, Out); return 0; }
        __except (NoteCrash(GetExceptionInformation())) { return -1; }
    }
}

struct SoundSink::State
{
    std::mutex DoneLock;   // the callbacks, one at a time
    // the files being written (lower case): a second sound to the same file waits for the first,
    // so that the last one met is the file left, as when they were written one after the other
    std::mutex PathsLock;
    std::condition_variable PathDone;
    std::unordered_set<std::wstring> InFlight;
    WorkerPool Pool;       // last: destroyed (drained) first, while the locks are still there

    // J written on the pool (inline under a DrySounds); Then(code of the guarded write, Crash)
    // under DoneLock
    static std::wstring KeyOf(const std::filesystem::path& Path)
    {
        std::wstring Key = Path.lexically_normal().native();
        for (wchar_t& C : Key) C = (wchar_t)towlower(C);
        return Key;
    }

    void Run(std::shared_ptr<SoundJob> J, std::function<void(int Code, const std::string& Crash)> Then, bool Inline = false)
    {
        std::wstring Key = KeyOf(J->Path);
        {
            std::unique_lock<std::mutex> L(PathsLock);
            PathDone.wait(L, [&]() { return !InFlight.count(Key); });
            InFlight.insert(Key);
        }
        auto Job = [this, J = std::move(J), Then = std::move(Then), Key = std::move(Key)]() {
            std::string Crash;
            const int Code = Guarded([&]() { return WriteSoundJobSeh(J.get()); }, Crash);
            if (Code < 0) J->Ok = false;
            // the bytes freed before waiting on a lock
            J->Keep.reset();
            {
                std::lock_guard<std::mutex> L(PathsLock);
                InFlight.erase(Key);
            }
            PathDone.notify_all();
            if (!Then) return;
            std::lock_guard<std::mutex> G(DoneLock);
            Then(Code, Crash);
        };
        if (Inline || DrySoundsActive()) Job();
        else Pool.Post(std::move(Job));
    }
};

SoundSink::SoundSink(std::string Format) : St(std::make_unique<State>()), Fmt(std::move(Format))
{
    SkipPrevious = SettingsManager::GetSetting("skipprevsound") == "true";
}

SoundSink::~SoundSink()
{
    Finish();
}

void SoundSink::Write(const SoundSource& S, std::filesystem::path Path, WriteDone Done, std::shared_ptr<const void> Keep)
{
    auto J = std::make_shared<SoundJob>();
    J->Source = S;
    J->Path = std::move(Path);
    J->Format = Fmt;
    // a copy of what the source points at (an Opus source may read past Size, up to Available)
    if (Keep) J->Keep = std::move(Keep);
    else if (S.Data != nullptr)
    {
        auto Copy = std::make_shared<std::vector<uint8_t>>(S.Data, S.Data + std::max(S.Size, S.Available));
        J->Source.Data = Copy->data();
        J->Keep = std::move(Copy);
    }
    SoundJob* Raw = J.get();
    St->Run(std::move(J), [Raw, Done = std::move(Done)](int Code, const std::string& Crash) {
        if (Done) Done(Code >= 0 && Raw->Ok, Raw->Why, Crash);
    });
}

void SoundSink::Export(const ICoDGame* Game, const CoDSound_t& Sound, const std::string& Dir, const std::string& Extension, ExportDone Done)
{
    ExportOn(Game, Sound, Dir, Extension, std::move(Done), false);
}

int SoundSink::ExportNow(const ICoDGame* Game, const CoDSound_t& Sound, const std::string& Dir, const std::string& Extension, std::string& Crash)
{
    int Code = -1;
    ExportOn(Game, Sound, Dir, Extension, [&](int C, const std::string& Why) { Code = C, Crash = Why; }, true);
    return Code;
}

void SoundSink::WaitFor(const std::filesystem::path& Path)
{
    const std::wstring Key = State::KeyOf(Path);
    std::unique_lock<std::mutex> L(St->PathsLock);
    St->PathDone.wait(L, [&]() { return !St->InFlight.count(Key); });
}

void SoundSink::ExportOn(const ICoDGame* Game, const CoDSound_t& Sound, const std::string& Dir, const std::string& Extension, ExportDone Done,
                         bool Inline)
{
    const auto Report = [&](int Code, const std::string& Crash) {
        if (!Done) return;
        std::lock_guard<std::mutex> G(St->DoneLock);
        Done(Code, Crash);
    };
    // LiveExport::ExportSound's path, built the same way
    std::string Full;
    std::string Crash;
    std::unique_ptr<XSound> Read;
    const int ReadCode = Guarded([&]() {
        Full = (std::filesystem::path(Dir) / (Sound.AssetName + Extension)).string();
        if (SkipPrevious && HasFile(Full)) return 1;
        return Game == nullptr ? 0 : ReadSoundSeh(Game, &Sound, &Read);
    }, Crash);
    if (ReadCode < 0) return Report(-1, Crash);
    if (ReadCode == 1) return Report(static_cast<int>(ExportGameResult::Success), "");
    // nothing read: a failure the run reports (not a sound counted written)
    if (Read == nullptr) return Report(static_cast<int>(ExportGameResult::UnknownError), "");

    auto J = std::make_shared<SoundJob>();
    J->AsRead = Game->SoundsAsRead() || Read->DataSize == 0;
    J->Source = SourceOf(*Read);
    J->Keep = std::shared_ptr<const XSound>(std::move(Read));
    J->Path = Full;
    J->Format = Fmt;
    SoundJob* Raw = J.get();
    St->Run(std::move(J), [Raw, Done = std::move(Done)](int Code, const std::string& Crash) {
        if (!Done) return;
        if (Code < 0) Done(-1, Crash);
        else Done(static_cast<int>(Raw->Ok ? ExportGameResult::Success : ExportGameResult::UnknownError), "");
    }, Inline);
}

void SoundSink::Locked(const std::function<void()>& F)
{
    std::lock_guard<std::mutex> G(St->DoneLock);
    F();
}

void SoundSink::Finish()
{
    St->Pool.Drain();
}

namespace
{
    // Two Ogg files whose pages are the same but for the stream's serial (bytes 14-17 of each page
    // header) and so the page's CRC (22-25): an Opus encode's serial comes from its file's name,
    // the same sound under <name>_2 differs only there
    bool SameOggPages(const std::filesystem::path& A, const std::filesystem::path& B)
    {
        std::vector<uint8_t> Da, Db;
        if (!ReadBytes(A, Da) || !ReadBytes(B, Db) || Da.size() != Db.size()) return false;
        for (size_t At = 0; At < Da.size();)
        {
            if (At + 27 > Da.size() || std::memcmp(Da.data() + At, "OggS", 4) != 0) return false;
            const size_t Segments = Da[At + 26];
            if (At + 27 + Segments > Da.size()) return false;
            size_t Body = 0;
            for (size_t K = 0; K < Segments; K++) Body += Da[At + 27 + K];
            const size_t End = At + 27 + Segments + Body;
            if (End > Da.size()) return false;
            for (size_t I = At; I < End; I++)
            {
                const size_t In = I - At;
                if ((In >= 14 && In < 18) || (In >= 22 && In < 26)) continue;
                if (Da[I] != Db[I]) return false;
            }
            At = End;
        }
        return true;
    }

    bool SameBytes(const std::filesystem::path& A, const std::filesystem::path& B)
    {
        std::error_code Ec;
        const auto Size = std::filesystem::file_size(A, Ec);
        if (Ec || Size != std::filesystem::file_size(B, Ec) || Ec) return false;
        const std::string Ext = ExportNaming::Lower(A.extension().string());
        if (Ext == ".opus" || Ext == ".ogg") return SameOggPages(A, B);
        std::ifstream Fa(A, std::ios::binary), Fb(B, std::ios::binary);
        std::vector<char> Ba(1 << 16), Bb(1 << 16);
        while (Fa && Fb)
        {
            Fa.read(Ba.data(), (std::streamsize)Ba.size());
            Fb.read(Bb.data(), (std::streamsize)Bb.size());
            if (Fa.gcount() != Fb.gcount() || std::memcmp(Ba.data(), Bb.data(), (size_t)Fa.gcount()) != 0) return false;
        }
        return Fa.eof() && Fb.eof();
    }
}

void SoundFiles::Write(SoundSink& Sink, const ICoDGame* Game, const CoDSound_t& Sound, const std::filesystem::path& Dir,
                       const std::string& Extension, std::function<void(int Code, const std::string& Crash, const std::filesystem::path& File)> Done)
{
    const auto Key = [&](const std::string& Stem) { return ExportNaming::Lower((Dir / (Stem + Extension)).string()); };
    if (!Taken.contains(Key(Sound.AssetName)))
    {
        // the name's first file: on the pool
        Taken.insert(Key(Sound.AssetName));
        MakeDirs(Dir);
        Sink.Export(Game, Sound, Dir.string(), Extension,
                    [Done = std::move(Done), File = Dir / (Sound.AssetName + Extension)](int Code, const std::string& Crash) {
                        if (Done) Done(Code, Crash, File);
                    });
        return;
    }
    // a second sound under that name: written here, once the name's first file is (the copy is
    // compared with it), as Write does
    std::string Stem = Sound.AssetName;
    for (size_t N = 2; Taken.contains(Key(Stem)); N++) Stem = std::format("{}_{}", Sound.AssetName, N);
    Taken.insert(Key(Stem));
    CoDSound_t Named = Sound;
    Named.AssetName = Stem;
    MakeDirs(Dir);
    Sink.WaitFor((std::filesystem::path(Dir.string()) / (Sound.AssetName + Extension)).string());
    std::string Crash;
    const int Code = Sink.ExportNow(Game, Named, Dir.string(), Extension, Crash);
    std::filesystem::path Written = Dir / (Stem + Extension);
    if (Code == static_cast<int>(ExportGameResult::Success))
    {
        const std::filesystem::path First = Dir / (Sound.AssetName + Extension);
        std::error_code Ec;
        if (SameBytes(First, Written))
        {
            std::filesystem::remove(Written, Ec);
            Taken.erase(Key(Stem));
            Written = First;
            Copies++;
        }
        else Renamed++;
    }
    if (Done) Sink.Locked([&] { Done(Code, Crash, Written); });
}

int SoundFiles::Write(const CoDSound_t& Sound, const std::filesystem::path& Dir, const std::string& Extension, std::string& Crash,
                      std::filesystem::path* File)
{
    const auto Key = [&](const std::string& Stem) { return ExportNaming::Lower((Dir / (Stem + Extension)).string()); };
    std::string Stem = Sound.AssetName;
    for (size_t N = 2; Taken.contains(Key(Stem)); N++) Stem = std::format("{}_{}", Sound.AssetName, N);
    Taken.insert(Key(Stem));
    CoDSound_t Named = Sound;
    Named.AssetName = Stem;
    std::error_code Ec;
    MakeDirs(Dir);
    const int Code = ExportSound(Named, Dir.string(), Extension, Crash);
    std::filesystem::path Written = Dir / (Stem + Extension);
    if (Code == static_cast<int>(ExportGameResult::Success) && Stem != Sound.AssetName)
    {
        const std::filesystem::path First = Dir / (Sound.AssetName + Extension);
        if (SameBytes(First, Written))
        {
            std::filesystem::remove(Written, Ec);
            Taken.erase(Key(Stem));
            Written = First;
            Copies++;
        }
        else Renamed++;
    }
    if (File) *File = Written;
    return Code;
}

const LoadedProfile* ZoneProfile(const std::string& Name, std::string& Error)
{
    static std::mutex Lock;
    static std::unordered_map<std::string, std::unique_ptr<LoadedProfile>> Cache;
    std::lock_guard<std::mutex> Guard(Lock);
    auto It = Cache.find(Name);
    if (It != Cache.end()) return It->second.get();
    auto L = std::make_unique<LoadedProfile>();
    if (!ZoneTables::LoadProfile(Name, L->Profile, Error)) return nullptr;
    try
    {
        L->Tables = ZoneTables::LoadTables(L->Profile.TablePaths);
    }
    catch (const std::exception& E)
    {
        Error = E.what();
        return nullptr;
    }
    return (Cache[Name] = std::move(L)).get();
}
}
