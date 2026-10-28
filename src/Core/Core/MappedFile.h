#pragma once

// A file read in place: mapped whole and read-only until destroyed (moved, never copied). What the
// tables kept on disk are read through: the packages' tables, the name and install indexes, the
// material books.

#include <Windows.h>

#include <cstddef>
#include <cstdint>
#include <filesystem>

class MappedFile
{
public:
    MappedFile() = default;
    ~MappedFile() { Close(); }
    MappedFile(MappedFile&& O) noexcept : View(O.View), Bytes(O.Bytes)
    {
        O.View = nullptr;
        O.Bytes = 0;
    }
    MappedFile& operator=(MappedFile&& O) noexcept
    {
        if (this != &O)
        {
            Close();
            View = O.View, Bytes = O.Bytes;
            O.View = nullptr, O.Bytes = 0;
        }
        return *this;
    }
    MappedFile(const MappedFile&) = delete;
    MappedFile& operator=(const MappedFile&) = delete;

    // false when it is missing, empty or does not map (another one held before is let go)
    bool Open(const std::filesystem::path& Path)
    {
        Close();
        const HANDLE F = CreateFileW(Path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
                                     FILE_ATTRIBUTE_NORMAL, nullptr);
        if (F == INVALID_HANDLE_VALUE) return false;
        LARGE_INTEGER Size{};
        const HANDLE Map = GetFileSizeEx(F, &Size) && Size.QuadPart > 0 ? CreateFileMappingW(F, nullptr, PAGE_READONLY, 0, 0, nullptr) : nullptr;
        CloseHandle(F);
        if (Map == nullptr) return false;
        // (the view keeps the mapping alive)
        View = MapViewOfFile(Map, FILE_MAP_READ, 0, 0, 0);
        CloseHandle(Map);
        if (View == nullptr) return false;
        Bytes = (size_t)Size.QuadPart;
        return true;
    }
    void Close()
    {
        if (View != nullptr) UnmapViewOfFile(View);
        View = nullptr;
        Bytes = 0;
    }

    const uint8_t* data() const { return static_cast<const uint8_t*>(View); }
    size_t size() const { return Bytes; }
    bool IsOpen() const { return View != nullptr; }

private:
    void* View = nullptr;
    size_t Bytes = 0;
};
