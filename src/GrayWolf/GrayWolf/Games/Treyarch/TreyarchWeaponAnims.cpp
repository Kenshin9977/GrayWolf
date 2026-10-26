#include "stdafx.h"

// The weapons' models and xanims for the anim owners (TreyarchLevel.h's WeaponDocuments), read
// from the relocated zone at the places the title gives (WeaponLayout).

#include "Games/Treyarch/GscAnimOwners.h"
#include "Games/Treyarch/TreyarchLevel.h"

#include <set>

void TreyarchLevelZone::WeaponDocuments(std::vector<std::pair<std::string, std::string>>& Out, const WeaponLayout& L) const
{
    const auto& Img = Image();
    const int64_t Ptr = Rules().PointerSize;
    // a list of Count string pointers at the pointer field Field
    const auto Names = [&](int64_t Field, int64_t Count) {
        std::vector<std::string> List;
        const int64_t At = Off(PointerAt(Field));
        if (At < 0 || At + Count * Ptr > (int64_t)Img.size()) return List;
        for (int64_t K = 0; K < Count; K++) List.push_back(StringAt(PointerAt(At + K * Ptr)));
        return List;
    };
    // (an attachment unique is loaded where a weapon names it first: inside that weapon)
    std::set<std::string> Seen;
    for (const auto& I : Walk().Instances)
    {
        const bool Unique = L.UniqueAnims >= 0 && OwnerIs(I, "attachmentunique");
        if (!Unique && !OwnerIs(I, "weapon")) continue;
        const std::string Name = StringAt(PointerAt(I.Pos));
        if (Name.empty() || Name[0] == ',' || !Seen.insert(Name).second) continue;
        std::string Gun, Hands;
        if (!Unique)
            if (const int64_t Def = Off(PointerAt(I.Pos + L.Def)); Def >= 0)
            {
                // the first gun model the weapon has (one per camo/attachment slot)
                if (const int64_t Guns = Off(PointerAt(Def + L.Guns)); Guns >= 0 && Guns + L.GunCount * Ptr <= (int64_t)Img.size())
                    for (int64_t K = 0; K < L.GunCount && Gun.empty(); K++)
                        if (const uint64_t A = PointerAt(Guns + K * Ptr)) Gun = ModelAt(A);
                if (const uint64_t A = PointerAt(Def + L.Hands)) Hands = ModelAt(A);
            }
        const auto Anims = Unique ? Names(I.Pos + L.UniqueAnims, L.UniqueAnimCount) : Names(I.Pos + L.Anims, L.AnimCount);
        Out.push_back(GscAnimOwners::WeaponDocument(Name, Unique, Gun, Hands, Anims));
    }
}
