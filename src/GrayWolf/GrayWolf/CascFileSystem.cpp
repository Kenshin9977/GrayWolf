#include "stdafx.h"
#include "CascFileSystem.h"
#include "Strings.h"

#include "CascLib.h"

CascFileSystem::CascFileSystem(const std::string& directory)
{
    StorageHandle = NULL;

    // Wide, matching the library (CASCLIB_UNICODE), or the path is silently truncated.
    auto asWStr = Strings::ToUnicodeString(directory);

    if (CascOpenStorage(asWStr.c_str(), CASC_LOCALE_ALL, &StorageHandle))
    {
        LastErrorCode = 0;
        return;
    }

    LastErrorCode = GetCascError();
}

CascFileSystem::~CascFileSystem()
{
    for (auto openHandle : OpenHandles)
    {
        CascCloseFile(openHandle);
    }

    OpenHandles.clear();
    CascCloseStorage(StorageHandle);
}

HANDLE CascFileSystem::OpenFile(const std::string& fileName, const std::string& mode)
{
    // Casc only supports reading
    if (mode != "r")
    {
        LastErrorCode = 0x345300;
        return NULL;
    }

    HANDLE result = NULL;

    // CASC_LOCALE_ALL, not NONE: a localised entry is skipped otherwise, and both tools known to
    if (!CascOpenFile(StorageHandle, fileName.c_str(), CASC_LOCALE_ALL, CASC_OPEN_BY_NAME, &result))
    {
        LastErrorCode = GetCascError();
        result = NULL;
    }
    else
    {
        LastErrorCode = 0;
        OpenHandles.push_back(result);
    }

    return result;
}

void CascFileSystem::CloseHandle(HANDLE handle)
{
    // no longer the destructor's to close: closed twice, a freed handle crashed the exit
    std::erase(OpenHandles, handle);
    CascCloseFile(handle);
}

bool CascFileSystem::Exists(const std::string& fileName)
{
    CASC_FIND_DATA findData{};
    HANDLE fileHandle = CascFindFirstFile(StorageHandle, fileName.c_str(), &findData, NULL);
    bool result = fileHandle != INVALID_HANDLE_VALUE && findData.bFileAvailable > 0;
    CascFindClose(fileHandle);
    return result;
}

size_t CascFileSystem::Read(HANDLE handle, uint8_t* buffer, const size_t offset, const size_t size)
{
    DWORD sizeRead = 0;

    if (!CascReadFile(handle, buffer + offset, size, &sizeRead))
    {
        LastErrorCode = 0x345301;
    }
    else
    {
        LastErrorCode = 0;
    }

    return sizeRead;
}

size_t CascFileSystem::Write(HANDLE handle, const uint8_t* buffer, const size_t offset, const size_t size)
{
    LastErrorCode = 0x345302;
    return 0;
}

size_t CascFileSystem::Tell(HANDLE handle)
{
    ULONGLONG result = 0;

    if (CascSetFilePointer64(handle, 0, &result, FILE_CURRENT))
    {
        LastErrorCode = 0x345302;
    }
    else
    {
        LastErrorCode = 0;
    }

    return result;
}

size_t CascFileSystem::Seek(HANDLE handle, size_t position, size_t direction)
{
    ULONGLONG result = 0;

    if (CascSetFilePointer64(handle, position, &result, direction))
    {
        LastErrorCode = 0x345303;
    }
    else
    {
        LastErrorCode = 0;
    }

    return result;
}

size_t CascFileSystem::Size(HANDLE handle)
{
    if (handle == NULL)
    {
        LastErrorCode = 0x345303;
        return 0;
    }

    ULONGLONG result = 0;

    if (!CascGetFileSize64(handle, &result))
    {
        LastErrorCode = 0x345303;
        result = 0;
    }
    else
    {
        LastErrorCode = 0;
    }

    return result;
}

void CascFileSystem::EnumerateFilesWithKey(const std::string& pattern, std::function<void(const std::string&, const size_t, const std::string&)> onFileFound)
{
    CASC_FIND_DATA findData;
    HANDLE fileHandle = CascFindFirstFile(StorageHandle, pattern.c_str(), &findData, NULL);
    if (fileHandle == INVALID_HANDLE_VALUE) return;
    do
    {
        if (!findData.bFileAvailable) continue;
        char Key[2 * MD5_HASH_SIZE + 1] = {};
        for (int i = 0; i < MD5_HASH_SIZE; i++) snprintf(Key + 2 * i, 3, "%02x", findData.CKey[i]);
        onFileFound(findData.szFileName, (size_t)findData.FileSize, Key);
    } while (CascFindNextFile(fileHandle, &findData));
    CascFindClose(fileHandle);
}

bool CascFileSystem::StorageInfo(const std::string& name, uint64_t& contentSize, uint64_t& encodedSize, std::string& spanKeys)
{
    HANDLE File = NULL;
    if (!CascOpenFile(StorageHandle, name.c_str(), 0, CASC_OPEN_BY_NAME, &File)) return false;
    CASC_FILE_FULL_INFO Full{};
    bool Ok = CascGetFileInfo(File, CascFileFullInfo, &Full, sizeof(Full), nullptr);
    if (Ok)
    {
        contentSize = Full.ContentSize;
        encodedSize = Full.EncodedSize;
        std::vector<CASC_FILE_SPAN_INFO> Spans(Full.SpanCount ? Full.SpanCount : 1);
        spanKeys.clear();
        if (CascGetFileInfo(File, CascFileSpanInfo, Spans.data(), Spans.size() * sizeof(CASC_FILE_SPAN_INFO), nullptr))
            for (const auto& S : Spans)
            {
                char Key[2 * MD5_HASH_SIZE + 1] = {};
                for (int i = 0; i < MD5_HASH_SIZE; i++) snprintf(Key + 2 * i, 3, "%02x", S.EKey[i]);
                if (!spanKeys.empty()) spanKeys += "+";
                spanKeys += Key;
                // where the span lies: archive index and offset in it
                char Where[48] = {};
                snprintf(Where, sizeof(Where), "@%u:%u", S.ArchiveIndex, S.ArchiveOffs);
                spanKeys += Where;
            }
    }
    CascCloseFile(File);
    return Ok;
}

size_t CascFileSystem::EnumerateFiles(const std::string& pattern, std::function<void(const std::string&, const size_t)> onFileFound)
{
    size_t entriesConsumed = 0;
    HANDLE fileHandle;
    CASC_FIND_DATA findData;

    fileHandle = CascFindFirstFile(StorageHandle, pattern.c_str(), &findData, NULL);

    if (fileHandle != INVALID_HANDLE_VALUE)
    {
        do
        {
            if (findData.bFileAvailable)
            {
                onFileFound(findData.szFileName, findData.FileSize);
            }
        } while (CascFindNextFile(fileHandle, &findData));
    }

    CascFindClose(fileHandle);

    return false;
}
