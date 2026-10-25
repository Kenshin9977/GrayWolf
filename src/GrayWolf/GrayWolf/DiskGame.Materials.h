#pragma once

// Context::MaterialsElsewhere as Export gives it (DiskGame.Materials.cpp): a zone's name-only
// material headers filled from the books of the zones the install index says define them.

#include "DiskGame.h"

#include <memory>
#include <set>
#include <string>

namespace DiskGame
{
    class MaterialLookup
    {
    public:
        // ReaderKey: the run's (the first zone's IndexKey), read when the first lookup is made;
        // T: the run's totals, its material_headers_* and material_books_* counters
        MaterialLookup(const IDiskGame& Game, const ExportOptions& Options, const std::string& Install, const std::string& ReaderKey,
                       Totals& T);
        void operator()(IDiskZone& Asking, const std::set<std::string>& Names, MaterialBook& Out) const;

    private:
        struct State;
        std::shared_ptr<State> S;
    };
}
