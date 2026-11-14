#pragma once

// The char_heads setting (DiskGame::CharacterHeads): which head(s) each body wears, as a game's
// character data says, for its zones' ZonekitZone::CharacterHeads; the model kind then writes each
// body joined with its head (LiveExport::ExportCompositeModel, WraithModel::Join). The engine never
// names a game: each game reads its own data (Games/<ENGINE>/) into Pairs with these helpers.

#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace DiskGame { class IDiskZone; }

namespace CharacterHeads
{
    // body -> its heads, the default first; lower case, as the model kind names them
    using Pairs = std::vector<std::pair<std::string, std::vector<std::string>>>;
    using Documents = std::vector<std::pair<std::string, std::string>>;   // (name, text): IDiskZone::Documents

    // Body wears Heads: added to Body's entry (made at the end when new), a head it has already
    // given, an empty one or the body itself left out; lower-cased
    void Add(Pairs& Out, const std::string& Body, const std::vector<std::string>& Heads);

    // The documents of the zones Paths (IDiskZone::Documents), each read once per process (while its
    // file's size and time are unchanged), opened by Open with Asking parked meanwhile (one session
    // at a time), in Paths' order; a zone missing or not opened gives none
    using Opener = std::function<std::unique_ptr<DiskGame::IDiskZone>(const std::string& Path)>;
    std::vector<std::shared_ptr<const Documents>> DocumentsOf(DiskGame::IDiskZone& Asking, const std::vector<std::filesystem::path>& Paths,
                                                              const Opener& Open);
}
