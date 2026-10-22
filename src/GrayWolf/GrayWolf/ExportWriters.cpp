#include "stdafx.h"

#include "ExportWriters.h"

#include "CoDXModelTranslator.h"
#include "ExportNaming.h"
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
#include <thread>
#include <tuple>
#include <unordered_map>

namespace ExportWriters
{
    bool WriteBytes(const std::filesystem::path& P, const uint8_t* Data, size_t Size)
    {
        std::error_code Ec;
        std::filesystem::create_directories(P.parent_path(), Ec);
        std::ofstream O(P, std::ios::binary);
        if (Size) O.write(reinterpret_cast<const char*>(Data), (std::streamsize)Size);
        return (bool)O;
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
    return Image::ConvertImageMemory(Dds.DataBuffer, Dds.DataSize, ImageFormat::DDS_WithHeader, Path,
                                     Format == "TGA" ? ImageFormat::Standard_TGA : ImageFormat::Standard_PNG,
                                     Dds.ImagePatchType);
}

struct ImageSink::State
{
    struct Job
    {
        std::unique_ptr<XImageDDS> Dds;
        std::string Path;
    };
    unsigned Workers = 0;
    std::mutex QueueLock, StatsLock;
    std::condition_variable HasJob, HasRoom;
    std::deque<Job> Queue;
    bool Finished = false;
    static constexpr size_t MaxQueued = 64;
    std::vector<std::thread> Pool;
    std::vector<std::tuple<std::string, std::string, size_t>> Links;   // link, target, tag
    ImageSinkStats Stats;
};

ImageSink::ImageSink(std::string Format) : St(std::make_unique<State>()), Fmt(std::move(Format))
{
    Ext = ImageExtension(Fmt);
    UseDedup = !EnvSet("GH_NODEDUP", "GH_BO4_NODEDUP");
    St->Workers = std::max(1u, std::min(8u, std::thread::hardware_concurrency() / 2));
    char Env[16] = {};
    if (EnvValue("GH_CONVERT_THREADS", "GH_BO4_CONVERT_THREADS", Env, sizeof(Env))) St->Workers = (unsigned)atoi(Env);
    Times().Workers = St->Workers;
    State* S = St.get();
    const std::string F = Fmt;
    for (unsigned K = 0; K < S->Workers; K++)
        S->Pool.emplace_back([S, F]() {
            // the WIC fallback (a PNG fpng cannot take) needs COM on the thread that calls it. Never
            // CoUninitialize here: tearing the process's MTA down after one sink left DirectXTex's
            // cached WIC factory dangling for the next.
            CoInitializeEx(nullptr, COINIT_MULTITHREADED);
            for (;;)
            {
                State::Job J;
                {
                    std::unique_lock<std::mutex> L(S->QueueLock);
                    S->HasJob.wait(L, [S]() { return !S->Queue.empty() || S->Finished; });
                    if (S->Queue.empty()) break;
                    J = std::move(S->Queue.front());
                    S->Queue.pop_front();
                }
                S->HasRoom.notify_one();
                const auto Start = std::chrono::steady_clock::now();
                const bool Ok = J.Dds && Put(*J.Dds, J.Path, F);
                Times().ConvertUs += Micros(Start);
                Times().Converted++;
                std::lock_guard<std::mutex> G(S->StatsLock);
                if (Ok) S->Stats.Written++; else S->Stats.Failed++;
            }
        });
}

ImageSink::~ImageSink()
{
    {
        std::lock_guard<std::mutex> L(St->QueueLock);
        St->Finished = true;
    }
    St->HasJob.notify_all();
    for (auto& T : St->Pool)
        if (T.joinable()) T.join();
}

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
    State::Job J{std::move(Dds), Path};
    if (St->Workers == 0)
    {
        const auto Start = std::chrono::steady_clock::now();
        const bool Ok = J.Dds && Put(*J.Dds, J.Path, Fmt);
        Times().ConvertUs += Micros(Start);
        Times().Converted++;
        if (Ok) St->Stats.Written++; else St->Stats.Failed++;
        return;
    }
    {
        std::unique_lock<std::mutex> L(St->QueueLock);
        const auto Room = [this]() { return St->Queue.size() < State::MaxQueued; };
        if (!Room())
        {
            // timed only when it blocks: the pool is what this thread waits on
            const auto Start = std::chrono::steady_clock::now();
            St->HasRoom.wait(L, Room);
            Times().QueueWaitUs += Micros(Start);
        }
        St->Queue.push_back(std::move(J));
    }
    St->HasJob.notify_one();
}

ImageSinkStats ImageSink::Finish(const std::function<std::unique_ptr<XImageDDS>(size_t)>& Rebuild, size_t& Missing,
                                 const std::function<uint64_t(size_t)>& KeyOf)
{
    {
        std::lock_guard<std::mutex> L(St->QueueLock);
        St->Finished = true;
    }
    St->HasJob.notify_all();
    const auto Drain = std::chrono::steady_clock::now();
    for (auto& T : St->Pool)
        if (T.joinable()) T.join();
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
    std::error_code Ec;
    std::filesystem::create_directories(Dir, Ec);
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

std::string Utf8(const std::string& S, unsigned CodePage)
{
    bool Valid = true;
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
        __try { return static_cast<int>(LiveExport::ExportModel(M, *Dir, T->ImagesDir, T->Relative, T->Extension, T->Take ? &T->Take : nullptr)); }
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
        fs::create_directories(fs::path(Target).parent_path(), Ec);
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
        fs::create_directories(fs::path(Target).parent_path(), Ec);
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
        fs::create_directories(fs::path(Target).parent_path(), Ec);
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
    bool SameBytes(const std::filesystem::path& A, const std::filesystem::path& B)
    {
        std::error_code Ec;
        const auto Size = std::filesystem::file_size(A, Ec);
        if (Ec || Size != std::filesystem::file_size(B, Ec) || Ec) return false;
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
    std::filesystem::create_directories(Dir, Ec);
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
