#pragma once

#include <filesystem>
#include <functional>
#include <ostream>
#include <string>

// File operations and path joins: std::filesystem. What is left here splits names, asset names as
// much as paths: plain string work where '\', '/' and ':' all separate, that never converts
// through the code page nor throws (std::filesystem::path does both, and reads "x:" as a drive).
namespace FileSystems
{
    std::string GetFileName(const std::string& Path);
    std::string GetFileNameWithoutExtension(const std::string& Path);
    // The name up to its first '.', "a.b.c" -> "a"
    std::string GetFileNamePurgeExtensions(const std::string& Path);
    // ".ext", or "" when the name has none or ends with the '.'
    std::string GetExtension(const std::string& Path);
    // The path before its last separator ("C:\" kept whole)
    std::string GetDirectoryName(const std::string& Path);

    // The folder GrayWolf.exe is in
    std::string GetApplicationPath();

    // Path written whole by Write (into a file beside it, its folder made) then renamed over it: a
    // reader (another GrayWolf) never sees it half written. False when the writing or the rename
    // failed (the file beside it removed).
    bool WriteReplacing(const std::filesystem::path& Path, const std::function<void(std::ostream&)>& Write);
}
