#include "stdafx.h"
#include "siren.h"

#include <filesystem>
#include <mutex>
#include <vector>

// Oodle compression routine definition, verified for Oodle v5 (x32) (All callbacks are unmarked and aren't needed)
typedef int32_t(*OodleLZ32_CompressDef)(int32_t Format, unsigned char *Buffer, int32_t BufferSize, unsigned char *OutputBuffer, int32_t Level, void *a, void *b, void *c);
// Oodle compression routine definition, verified for Oodle v5 (x64) (All callbacks are unmarked and aren't needed)
typedef int64_t(*OodleLZ64_CompressDef)(int32_t Format, unsigned char *Buffer, int64_t BufferSize, unsigned char *OutputBuffer, int64_t Level, void *a, void *b, void *c);

// Oodle decompression routine definition, verified for Oodle v5 (x32) (All callbacks are unmarked and aren't needed)
typedef int32_t(*OodleLZ32_DecompressDef)(unsigned char *Buffer, int32_t BufferSize, unsigned char *OutputBuffer, int32_t OutputBufferSize, int32_t a, int32_t b, int32_t c, void *d, void *e, void *f, void *g, void *h, void *i, int32_t ThreadModule);
// Oodle decompression routine definition, verified for Oodle v5 (x64) (All callbacks are unmarked and aren't needed)
typedef int64_t(*OodleLZ64_DecompressDef)(unsigned char *Buffer, int64_t BufferSize, unsigned char *OutputBuffer, int64_t OutputBufferSize, int32_t a, int32_t b, int64_t c, void *d, void *e, void *f, void *g, void *h, void *i, int32_t ThreadModule);

HMODULE Siren::OodleHandle = NULL;

uintptr_t Siren::OodleLZ_Compress = NULL;
uintptr_t Siren::OodleLZ_Decompress = NULL;

namespace
{
    // once at a time: zones decompress on several threads (the install index's workers)
    std::mutex Lock;
    std::vector<std::filesystem::path> SearchDirs;

    // DllPath beside the exe (the loader's own search), else in a game's folder or up to three of
    // its parents (an install given as its zone or cod24 folder)
    HMODULE LoadFromSearchDirs(const TCHAR* DllPath)
    {
        if (HMODULE H = LoadLibrary(DllPath)) return H;
        const std::filesystem::path Name(DllPath);
        if (Name.has_parent_path()) return NULL;
        for (const auto& Dir : SearchDirs)
        {
            std::filesystem::path At = Dir;
            for (int Up = 0; Up < 4 && !At.empty(); Up++, At = At.parent_path())
            {
                std::error_code Ec;
                const auto File = At / Name;
                if (std::filesystem::is_regular_file(File, Ec))
                    if (HMODULE H = LoadLibraryExW(File.c_str(), NULL, LOAD_WITH_ALTERED_SEARCH_PATH)) return H;
                if (At == At.parent_path()) break;
            }
        }
        SetLastError(ERROR_MOD_NOT_FOUND);
        return NULL;
    }
}

void Siren::AddSearchDir(const std::wstring& Dir)
{
    if (Dir.empty()) return;
    std::error_code Ec;
    auto Abs = std::filesystem::absolute(Dir, Ec);
    if (Ec) return;
    std::lock_guard<std::mutex> Guard(Lock);
    for (const auto& D : SearchDirs)
        if (D == Abs) return;
    SearchDirs.push_back(std::move(Abs));
}

SirenStatus Siren::Initialize(const TCHAR* DllPath)
{
    std::lock_guard<std::mutex> Guard(Lock);
    if (OodleHandle != NULL && OodleLZ_Compress != NULL && OodleLZ_Decompress != NULL)
        return SirenStatus::Success;

    OodleHandle = NULL;

    if (DllPath == nullptr)
    {
        if (sizeof(uintptr_t) == 4)
        {
            OodleHandle = LoadLibrary(L"oo2core_5_win32.dll");
        }
        else if (sizeof(uintptr_t) == 8)
        {
            OodleHandle = LoadLibrary(L"oo2core_5_win64.dll");
        }
    }
    else
    {
        OodleHandle = LoadFromSearchDirs(DllPath);
    }

    if (OodleHandle == NULL)
    {
        switch (GetLastError())
        {
        case ERROR_MOD_NOT_FOUND:
            return SirenStatus::MissingDll;
        case ERROR_BAD_FORMAT:
            return SirenStatus::InvalidArch;
        }
        return SirenStatus::MissingDll;
    }

    if (sizeof(uintptr_t) == 4)
    {
        OodleLZ_Compress = (uintptr_t)GetProcAddress(OodleHandle, "_OodleLZ_Compress@32");
        OodleLZ_Decompress = (uintptr_t)GetProcAddress(OodleHandle, "_OodleLZ_Decompress@56");

        if (OodleLZ_Compress == NULL || OodleLZ_Decompress == NULL) { return SirenStatus::OutdatedOrInvalid; }
    }
    else if (sizeof(uintptr_t) == 8)
    {
        OodleLZ_Compress = (uintptr_t)GetProcAddress(OodleHandle, "OodleLZ_Compress");
        OodleLZ_Decompress = (uintptr_t)GetProcAddress(OodleHandle, "OodleLZ_Decompress");

        if (OodleLZ_Compress == NULL || OodleLZ_Decompress == NULL) { return SirenStatus::OutdatedOrInvalid; }
    }

    return SirenStatus::Success;
}

void Siren::Shutdown()
{
    if (OodleHandle != NULL)
    {
        FreeLibrary(OodleHandle);
        OodleHandle = NULL;
    }

    OodleLZ_Compress = NULL;
    OodleLZ_Decompress = NULL;
}

uintptr_t Siren::Decompress(const uint8_t* Buffer, const uintptr_t BufferSize, uint8_t* OutputBuffer, const uintptr_t OutputBufferSize)
{
    if (OodleLZ_Decompress != NULL && Buffer != nullptr && BufferSize > 0 && OutputBuffer != nullptr && OutputBufferSize > 0)
    {
        intptr_t Result = 0;

        // Oodle's last argument is its ThreadPhase: 3 (ThreadPhaseAll) makes Oodle use its own internal
        // threads, which races its phase state when several of our worker threads decode at once
        // ("corruption: inconsistency detected during phase 2"). 0 (Unthreaded): our pool supplies the
        // parallelism, which BO4 export needs not to crash.
        const int32_t ThreadPhase = 0;
        if (sizeof(uintptr_t) == 4)
        {
            Result = (intptr_t)((OodleLZ32_DecompressDef)OodleLZ_Decompress)((uint8_t*)Buffer, (int32_t)BufferSize, OutputBuffer, (int32_t)OutputBufferSize, 0, 0, 0, NULL, NULL, NULL, NULL, NULL, NULL, ThreadPhase);
        }
        else if (sizeof(uintptr_t) == 8)
        {
            Result = (intptr_t)((OodleLZ64_DecompressDef)OodleLZ_Decompress)((uint8_t*)Buffer, (int64_t)BufferSize, OutputBuffer, (int64_t)OutputBufferSize, 0, 0, 0, NULL, NULL, NULL, NULL, NULL, NULL, ThreadPhase);
        }

        if (Result > 0) { return Result; }
    }

    return 0;
}
