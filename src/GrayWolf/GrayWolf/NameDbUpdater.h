#pragma once

#include <string>
#include <vector>

// Downloads and converts echo000/cod-name-db's CSV hash lists (github.com/echo000/cod-name-db)
// into the .wni files this codebase's per-game LoadStringCache/PerformInitialSetup code already
// expects under package_index\ - nothing there changes, this just puts the files in place. The
// repo's own CSV names don't always match the expected .wni names (a "v2" suffix vs. prefix,
// "strings" vs. "string") - see the mapping table in the .cpp, not a filename-matching scheme.
namespace NameDbUpdater
{
    // One CSV -> .wni conversion, and its outcome.
    struct FileResult
    {
        std::string WniName;   // "fnv1a_ximages" (no extension)
        bool Downloaded = false;
        bool Converted = false;
        size_t Entries = 0;
        std::string Error;     // set when Downloaded or Converted is false
    };

    // Downloads every mapped CSV from the repo's default branch, merges each into its .wni file
    // under OutputDir (created if missing; the names the file already holds are kept, the CSV's
    // win for a hash both give), and returns one FileResult per mapped file - callers
    // decide what counts as an acceptable partial failure (a network hiccup on one file
    // shouldn't discard the rest). OutputDir is normally <app dir>\package_index.
    std::vector<FileResult> Update(const std::string& OutputDir);

    // Same "hash,name" CSV -> .wni conversion as Update above, but for a file already on disk -
    // no download. Used to bring in a hash database built outside this codebase (a GPU cracking
    // session's own output, say) the same way a downloaded one is brought in. WniName is the
    // output stem (no extension) under OutputDir.
    FileResult ConvertLocalCsv(const std::string& CsvPath, const std::string& WniName,
                               const std::string& OutputDir);
}
