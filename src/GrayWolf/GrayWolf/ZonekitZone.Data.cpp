#include "stdafx.h"

// ZonekitZone's rawfile kind (ZonekitZone.h): what every game whose zones hold rawfiles or Lua
// files writes them with; each game says where its fields are.

#include "ZonekitZone.h"

#include "CoDAssets.h"

#include "ExportNaming.h"
#include "ExportWriters.h"
#include "Techsets.h"

#include <miniz/miniz.h>

#include <algorithm>
#include <filesystem>
#include <format>
#include <set>
#include <string_view>
#include <tuple>

namespace fs = std::filesystem;

namespace
{
    // <Dir>/<Name>, the name's folders kept, each part a safe file name (no "." or ".." part)
    fs::path UnderFolder(const fs::path& Dir, const std::string& Name)
    {
        fs::path File = Dir;
        std::string Part;
        for (char Ch : Name + "/")
            if (Ch == '/' || Ch == '\\')
            {
                if (!Part.empty() && Part != "." && Part != "..") File /= ExportNaming::SafeFileName(Part);
                Part.clear();
            }
            else Part += Ch;
        return File;
    }
}

namespace
{
    // A rawfile's file. A zone holds an empty rawfile named after itself ("ui"), which can be named
    // like another rawfile's folder ("ui/lui/..."): an empty file in the way of a folder gives way
    // to it, and an empty file whose place is a folder is left out (neither holds anything)
    bool WriteRawfile(const fs::path& Dir, const fs::path& File, const std::vector<uint8_t>& Bytes)
    {
        if (ExportWriters::WriteBytes(File, Bytes.data(), Bytes.size())) return true;
        std::error_code Ec;
        if (Bytes.empty() && fs::is_directory(File, Ec)) return true;
        bool Cleared = false;
        for (fs::path P = File.parent_path(); !P.empty() && P != Dir && P != P.parent_path(); P = P.parent_path())
            if (fs::is_regular_file(P, Ec) && fs::file_size(P, Ec) == 0) Cleared = fs::remove(P, Ec) || Cleared;
        return Cleared && ExportWriters::WriteBytes(File, Bytes.data(), Bytes.size());
    }
}

void ZonekitZone::ExportRawfiles(const DiskGame::Context& C, DiskGame::Totals& T, const std::vector<std::pair<const char*, RawfileReader>>& Readers)
{
    using DiskGame::Kind;
    auto& Kt = T.Kinds[Kind::Rawfile];
    std::vector<std::tuple<int64_t, const char*, const RawfileReader*>> Found;
    for (const auto& I : Walk().Instances)
        if (I.Parent < 0)
            for (const auto& [Type, Read] : Readers)
                if (OwnerIs(I, Type)) Found.emplace_back(I.Pos, Type, &Read);
    const fs::path Dir = C.KindDir(Kind::Rawfile);
    std::set<std::string> Written;
    for (size_t N = 0; N < Found.size(); N++)
    {
        const auto [Pos, Type, Read] = Found[N];
        std::string Name = AssetName(Kind::Rawfile, Pos);
        // a hashed name no index knows, rawfile_<hash>: named after its own type (luafile_<hash>)
        if (std::string_view(Type) != "rawfile" && Name.rfind("rawfile_", 0) == 0) Name = Type + Name.substr(7);
        if (!C.Advance(N, Found.size(), Name)) break;
        if (IsReference(Kind::Rawfile, Name, Pos))
        {
            Kt.References++;
            continue;
        }
        if (!C.Wants(Kind::Rawfile, Name)) continue;
        // a name the zone does not hold as text (a back reference the walk left elsewhere: a few of
        // Infinite Warfare's) is not one to write under
        if (!NameInPlace(Kind::Rawfile, Pos) ||
            std::any_of(Name.begin(), Name.end(), [](char Ch) { return (unsigned char)Ch < 0x20 || (unsigned char)Ch == 0xff; }))
        {
            Kt.Seen++;
            T.Fail(ZoneName, Kind::Rawfile, std::format("at {:#x}", Pos), "name not read");
            continue;
        }
        std::vector<uint8_t> Bytes;
        const std::string Asked = Name;
        const bool Ok = (*Read)(Pos, Name, Bytes);
        if (!Written.insert(ExportNaming::Lower(Name)).second) continue;
        Kt.Seen++;
        if (!Ok)
        {
            T.Fail(ZoneName, Kind::Rawfile, Asked, "payload not read");
            continue;
        }
        if (WriteRawfile(Dir, UnderFolder(Dir, Name), Bytes)) Kt.Written++;
        else T.Fail(ZoneName, Kind::Rawfile, Asked, "write failed");
    }
    C.Advance(Found.size(), Found.size());
}

bool ZonekitZone::RawBytes(int64_t Pos, const RawLayout& L, std::vector<uint8_t>& Out) const
{
    const uint32_t Size = At<uint32_t>(Image(), Pos + L.SizeAt);
    const uint32_t Packed = L.PackedAt >= 0 ? At<uint32_t>(Image(), Pos + L.PackedAt) : 0;
    Out.clear();
    if (Size == 0 && Packed == 0) return true;   // an empty file
    std::vector<uint8_t> Stored;
    if (!BytesAt(PointerAt(Pos + L.DataAt), Packed ? Packed : Size, Stored)) return false;
    if (!Packed)
    {
        Out = std::move(Stored);
        return true;
    }
    // zlib to Size bytes (a little more room: some games count the NUL in Size, some not)
    Out.resize((size_t)Size + 1);
    mz_ulong Got = (mz_ulong)Out.size();
    if (mz_uncompress(Out.data(), &Got, Stored.data(), (mz_ulong)Stored.size()) != MZ_OK) return false;
    Out.resize(std::min<size_t>(Got, Size));
    return true;
}

void ZonekitZone::NameLua(std::string& Name, const std::vector<uint8_t>& Bytes)
{
    // the name's own .lua or .luac off, then the one the bytes are
    const std::string L = ExportNaming::Lower(Name);
    if (L.size() > 5 && L.compare(L.size() - 5, 5, ".luac") == 0) Name.resize(Name.size() - 5);
    else if (L.size() > 4 && L.compare(L.size() - 4, 4, ".lua") == 0) Name.resize(Name.size() - 4);
    Name += !Bytes.empty() && Bytes[0] == 0x1b ? ".luac" : ".lua";
}

namespace
{
    // A rawfile's extension by the hash its asset keeps (FNV-1a 32 of the lowered extension), for a
    // name no index knows; ".raw" for one not listed
    std::string RawfileExtension(uint32_t Hash)
    {
        for (const char* Ext : { ".json", ".cfg", ".tif", ".bin", ".png", ".ttf", ".txt", ".news", ".graph", ".csv", ".lua", ".xml",
                                 ".gsc", ".csc", ".vision", ".rmb", ".shock", ".arena", ".atr", ".recipe", ".def", ".dds", ".wav" })
        {
            uint32_t H = 0x811C9DC5u;
            for (const char* C = Ext; *C; C++) H = (H ^ (uint8_t)*C) * 0x1000193u;
            if (H == Hash) return Ext;
        }
        return ".raw";
    }
}

void ZonekitZone::ExportsHashedRawfiles(int64_t LuaSize, int64_t LuaData)
{
    using DiskGame::Kind;
    Exports(Kind::Rawfile, [this, LuaSize, LuaData](const DiskGame::Context& C, DiskGame::Totals& T) {
        ExportRawfiles(C, T,
                       { { "rawfile", [this](int64_t Pos, std::string& Name, std::vector<uint8_t>& Bytes) {
                              if (Name.rfind("rawfile_", 0) == 0) Name += RawfileExtension(At<uint32_t>(Image(), Pos + 8));
                              return RawBytes(Pos, { 0x10, 0x18, 0xc }, Bytes);
                          } },
                         { "luafile", [this, LuaSize, LuaData](int64_t Pos, std::string& Name, std::vector<uint8_t>& Bytes) {
                              if (!RawBytes(Pos, { LuaSize, LuaData }, Bytes)) return false;
                              NameLua(Name, Bytes);
                              return true;
                          } } });
    });
}

void ZonekitZone::ExportsMaterialLinks(const MaterialLinkLayout& L)
{
    using DiskGame::Kind;
    Exports(Kind::MaterialLinks, [this, L](const DiskGame::Context& C, DiskGame::Totals& T) {
        auto& Kt = T.Kinds[Kind::MaterialLinks];
        constexpr uint64_t IdBits = 0x7FFFFFFFFFFFFFFFull;
        // the engine's fill-ins for a slot the material leaves empty: not the material's own
        static const uint64_t Defaults[] = { 0x4a882744bc523875 /* $black */, 0x3c29eeff15212c37 /* $white */,
                                             0x5c2d1c3e952cb190 /* $identitypackednog */ };
        const auto& Img = Image();
        std::set<int64_t> ImagesHere;
        for (const auto& I : R.Instances)
            if (OwnerIs(I, ImageType())) ImagesHere.insert(I.Pos);
        std::string Rows;
        std::set<uint64_t> Done;
        std::string Info;
        for (const auto& I : R.Instances)
        {
            if (!OwnerIs(I, "material")) continue;
            const uint64_t Id = At<uint64_t>(Img, I.Pos) & IdBits;
            const bool Records = L.Slots < 0;
            const int64_t Slots = Records ? 0 : Off(PointerAt(I.Pos + L.Slots)), Images = Off(PointerAt(I.Pos + L.Images));
            const uint32_t SlotCount = At<uint8_t>(Img, I.Pos + L.SlotCount), ImageCount = Records ? SlotCount : At<uint8_t>(Img, I.Pos + L.ImageCount);
            // a material the zone only names has no slots; one listed twice is written once
            if (Slots < 0 || Images < 0 || SlotCount == 0)
            {
                Kt.References++;
                continue;
            }
            if (Done.count(Id) != 0) continue;
            Kt.Seen++;
            std::string Own;
            bool Read = true;
            for (uint32_t K = 0; K < SlotCount && Read; K++)
            {
                const int64_t Record = Images + (int64_t)K * L.RecordSize;
                const uint32_t Semantic = Records ? At<uint32_t>(Img, Record + L.RecordSemantic) : At<uint8_t>(Img, Slots + K * 2);
                const uint32_t Index = Records ? K : At<uint8_t>(Img, Slots + K * 2 + 1);
                if (Index >= ImageCount) continue;
                // the image: in this zone, in a zone served beside it, or (bit 63) its hash alone
                const uint64_t Field = PointerAt(Records ? Record + L.RecordImage : Images + (int64_t)Index * 8);
                const int64_t At0 = Off(Field);
                uint64_t Hash = 0;
                uint32_t Width = 0, Height = 0, Format = 0;
                const char* Where = "hash";
                if (At0 >= 0)
                {
                    // (a table that does not hold image pointers: the material is not read)
                    if (ImagesHere.count(At0) == 0) Read = false;
                    Hash = At<uint64_t>(Img, At0 + L.ImageHash) & IdBits, Width = At<uint16_t>(Img, At0 + L.ImageWidth), Height = At<uint16_t>(Img, At0 + L.ImageHeight);
                    Format = L.WideFormat ? At<uint32_t>(Img, At0 + L.ImageFormat) : At<uint8_t>(Img, At0 + L.ImageFormat);
                    Where = Width != 0 ? "zone" : "named";
                }
                else if (Field >> 63) Hash = Field & IdBits;
                else if (Field != 0)
                {
                    auto& Game = *CoDAssets::GameInstance;
                    Hash = Game.Read<uint64_t>(Field + L.ImageHash) & IdBits, Width = Game.Read<uint16_t>(Field + L.ImageWidth),
                    Height = Game.Read<uint16_t>(Field + L.ImageHeight);
                    Format = L.WideFormat ? Game.Read<uint32_t>(Field + L.ImageFormat) : Game.Read<uint8_t>(Field + L.ImageFormat);
                    Where = "beside";
                    // an address of no zone served here (another zone's image), or not an address
                    if (Hash == 0) (Field >> 44) == 0 ? Counter("matlinks_unresolved") += 1 : Read = false;
                }
                if (Hash == 0 || std::find(std::begin(Defaults), std::end(Defaults), Hash) != std::end(Defaults)) continue;
                Own += std::format("{:016x},{},{},{:016x},{},{},{},{}\n", Id, K, Semantic, Hash, Where, Width, Height, Format);
            }
            if (!Read)
            {
                Counter("matlinks_unread") += 1;
                continue;
            }
            Done.insert(Id);
            Rows += Own;
            if (L.Techset >= 0)
            {
                // the techset: in this zone, or (bit 63) its hash alone
                const uint64_t Field = PointerAt(I.Pos + L.Techset);
                const int64_t At0 = Off(Field);
                // the constant table as one number (FNV-1a 64 of its bytes), 0 for none
                const int64_t Consts = Off(PointerAt(I.Pos + L.Constants)), ConstBytes = (int64_t)At<uint8_t>(Img, I.Pos + L.ConstantCount) * 16;
                uint64_t Sum = 0;
                if (Consts >= 0 && ConstBytes != 0)
                {
                    Sum = 0xcbf29ce484222325ull;
                    for (int64_t B = 0; B < ConstBytes; B++) Sum = (Sum ^ At<uint8_t>(Img, Consts + B)) * 0x100000001b3ull;
                }
                Info += std::format("{:016x},{:016x},{:08x},{},{},{:016x}\n", Id, (At0 >= 0 ? At<uint64_t>(Img, At0) : (Field >> 63) ? Field : 0) & IdBits,
                                    At<uint32_t>(Img, I.Pos + L.Flags), At<uint8_t>(Img, I.Pos + L.Layers), ConstBytes / 16, Sum);
            }
            Kt.Written++;
        }
        // the models: their materials in surface order (a model the zone only names has none)
        std::string Models;
        for (const auto& I : R.Instances)
        {
            if (L.ModelMaterials < 0 || !OwnerIs(I, "xmodel")) continue;
            const int64_t Handles = Off(PointerAt(I.Pos + L.ModelMaterials));
            const uint32_t Count = Handles >= 0 ? At<uint16_t>(Img, I.Pos + 0x10) : 0;
            const std::string Name = TextAt(I.Pos + 8);
            for (uint32_t K = 0; K < Count; K++)
            {
                const uint64_t Field = PointerAt(Handles + (int64_t)K * 8);
                const int64_t At0 = Off(Field);
                Models += std::format("{:016x},{},{},{:016x}\n", At<uint64_t>(Img, I.Pos) & IdBits, Name, K, (At0 >= 0 ? At<uint64_t>(Img, At0) : (Field >> 63) ? Field : 0) & IdBits);
            }
        }
        if (!Models.empty())
        {
            Models.insert(0, "model_id,model,surface,material_id\n");
            const fs::path File = fs::path(C.KindDir(Kind::MaterialLinks)) / "models" / (ExportNaming::SafeFileName(ZoneName) + ".csv");
            if (!ExportWriters::WriteBytes(File, (const uint8_t*)Models.data(), Models.size())) T.Fail(ZoneName, Kind::MaterialLinks, ZoneName, "write failed");
        }
        if (!Info.empty())
        {
            Info.insert(0, "material_id,techset_id,flags,layers,constants,constants_id\n");
            const fs::path File = fs::path(C.KindDir(Kind::MaterialLinks)) / "materials" / (ExportNaming::SafeFileName(ZoneName) + ".csv");
            if (!ExportWriters::WriteBytes(File, (const uint8_t*)Info.data(), Info.size())) T.Fail(ZoneName, Kind::MaterialLinks, ZoneName, "write failed");
        }
        if (Rows.empty()) return;
        Rows.insert(0, "material_id,slot,semantic,image_id,image,width,height,format\n");
        const fs::path File = fs::path(C.KindDir(Kind::MaterialLinks)) / (ExportNaming::SafeFileName(ZoneName) + ".csv");
        if (!ExportWriters::WriteBytes(File, (const uint8_t*)Rows.data(), Rows.size())) T.Fail(ZoneName, Kind::MaterialLinks, ZoneName, "write failed");
    });
}

void ZonekitZone::ExportsTechsets(std::shared_ptr<const Techsets::Format> F)
{
    using DiskGame::Kind;
    Exports(Kind::Techsets, [this, F](const DiskGame::Context& C, DiskGame::Totals& T) {
        Techsets::ZoneView Z;
        Z.Name = ZoneName;
        Z.Image = &Image();
        Z.Ptr = [this](int64_t Field) { return Off(PointerAt(Field)); };
        for (const auto& I : R.Instances)
            if (OwnerIs(I, "techset") || OwnerIs(I, "material")) Z.Assets.emplace_back(R.OwnerNames[I.Owner], I.Pos);
        Z.NameOf = [this](uint64_t Hash) { return KnownHash(Hash); };
        Z.Package = [](uint64_t Key, std::vector<uint8_t>& Out) {
            if (CoDAssets::GamePackageCache == nullptr || !CoDAssets::GamePackageCache->Exists(Key)) return false;
            uint32_t Size = 0;
            const auto Bytes = CoDAssets::GamePackageCache->ExtractPackageObject(Key, Size);
            if (Bytes == nullptr || Size == 0) return false;
            Out.assign(Bytes.get(), Bytes.get() + Size);
            return true;
        };
        Z.Folder = fs::path(Path).parent_path().string();
        Z.Profile = ProfileUsed();
        Techsets::Export(Z, *F, C, T);
    });
}

void ZonekitZone::ExportsSoundLinks(const SoundLinkLayout& L)
{
    using DiskGame::Kind;
    Exports(Kind::SoundLinks, [this, L](const DiskGame::Context& C, DiskGame::Totals& T) {
        auto& Kt = T.Kinds[Kind::SoundLinks];
        constexpr uint64_t IdBits = 0x7FFFFFFFFFFFFFFFull;
        const auto& Img = Image();
        std::string Rows;
        std::set<std::pair<uint64_t, uint64_t>> Done;
        for (const auto& I : R.Instances)
        {
            if (L.Bank != nullptr ? !OwnerIs(I, L.Bank) : !OwnerIs(I, "soundbank") && !OwnerIs(I, "soundbanktransient")) continue;
            const int64_t Aliases = Off(PointerAt(I.Pos + L.Aliases));
            const uint32_t AliasCount = At<uint32_t>(Img, I.Pos + L.AliasCount);
            // a bank the zone only names has no aliases
            if (Aliases < 0 || AliasCount == 0)
            {
                Kt.References++;
                continue;
            }
            Kt.Seen++;
            for (uint32_t A = 0; A < AliasCount; A++)
            {
                const int64_t Alias = Aliases + (int64_t)A * L.AliasSize;
                const uint64_t AliasId = At<uint64_t>(Img, Alias + L.AliasHash) & IdBits;
                const int64_t Entries = Off(PointerAt(Alias + L.Entries));
                const uint32_t Count = Entries >= 0 ? At<uint16_t>(Img, Alias + L.EntryCount) : 0;
                for (uint32_t K = 0; K < Count; K++)
                {
                    // the file: in this zone, or (bit 63) its hash alone
                    const int64_t Entry = Entries + (int64_t)K * L.EntrySize + L.EntrySound;
                    const uint64_t Field = L.SoundIsHash ? 0 : PointerAt(Entry);
                    const int64_t At0 = L.SoundIsHash ? -1 : Off(Field);
                    const uint64_t File = (L.SoundIsHash ? At<uint64_t>(Img, Entry) : At0 >= 0 ? At<uint64_t>(Img, At0) : (Field >> 63) ? Field : 0) & IdBits;
                    if (File == 0)
                    {
                        Counter("sndlinks_no_file") += 1;
                        continue;
                    }
                    if (!Done.insert({ AliasId, File }).second) continue;
                    Rows += std::format("{:x}\t{:x}\t{}\n", AliasId, File, K);
                    Kt.Written++;
                }
            }
        }
        if (Rows.empty()) return;
        const fs::path File = fs::path(C.KindDir(Kind::SoundLinks)) / (ExportNaming::SafeFileName(ZoneName) + ".tsv");
        if (!ExportWriters::WriteBytes(File, (const uint8_t*)Rows.data(), Rows.size())) T.Fail(ZoneName, Kind::SoundLinks, ZoneName, "write failed");
    });
}

void ZonekitZone::ExportsAssetRefs(std::function<std::string(const std::string&)> Decrypt)
{
    using DiskGame::Kind;
    Exports(Kind::AssetRefs, [this, Decrypt](const DiskGame::Context& C, DiskGame::Totals& T) {
        auto& Kt = T.Kinds[Kind::AssetRefs];
        constexpr uint64_t IdBits = 0x7FFFFFFFFFFFFFFFull;
        static const std::set<std::string, std::less<>> Bulk = { "image", "material", "xmodel", "xmodelsurfs", "xanim", "sndasset", "techset",
                                                                  "technique", "computeshader", "vertexshader", "pixelshader", "hullshader",
                                                                  "domainshader", "gfx_map", "gfx_map_trzone", "clipmap", "fx_map", "fx_map_trzone",
                                                                  "map_ents_trzone", "streamtreeasset", "physicsasset", "bonesync" };
        const auto& Img = Image();
        const auto Type = [this](const auto& I) -> const std::string& {
            static const std::string None = "?";
            return I.Owner < R.OwnerNames.size() ? R.OwnerNames[I.Owner] : None;
        };
        // the zone-level assets by position, for a pointer to one
        std::unordered_map<int64_t, size_t> Top;
        for (size_t K = 0; K < R.Instances.size(); K++)
            if (R.Instances[K].Parent < 0) Top.emplace(R.Instances[K].Pos, K);
        std::string Rows;
        for (const auto& I : R.Instances)
        {
            if (I.Parent >= 0 || Bulk.count(Type(I)) != 0) continue;
            Kt.Seen++;
            // its hash, or for a type that keeps none in front (a pointer, 0), @ and its position
            const uint64_t Head = At<uint64_t>(Img, I.Pos), Id = Head & IdBits;
            const std::string IdText = Head != 0 && Off(Head) < 0 ? std::format("{:016x}", Id) : std::format("@{:x}", I.Pos);
            std::set<std::pair<int, uint64_t>> Seen;
            std::string Own;
            const auto Put = [&](const char* What, const std::string& Value) {
                Own += std::format("{},{},{},{}\n", Type(I), IdText, What, Value);
            };
            for (uint32_t K = I.FirstRead; K < I.EndRead && K < R.Reads.size() && K < R.ReadPos.size() && Seen.size() < 20000; K++)
            {
                const int64_t Pos = R.ReadPos[K], Size = R.Reads[K];
                if (Pos < 0) continue;
                if (Size < 0)
                {
                    // a string: its text (the NUL left out), decrypted when the game encrypts them
                    std::string Text;
                    for (int64_t B = 0; B < -Size && Pos + B < (int64_t)Img.size() && Img[(size_t)(Pos + B)] != 0; B++) Text += (char)Img[(size_t)(Pos + B)];
                    if (Decrypt && !Text.empty()) Text = Decrypt(Text);
                    const bool Printable = !Text.empty() && std::all_of(Text.begin(), Text.end(), [](char Ch) { return Ch >= 0x20 && Ch < 0x7f && Ch != ','; });
                    if (Printable && Seen.insert({ 2, std::hash<std::string>{}(Text) }).second) Put("str", Text);
                    continue;
                }
                for (int64_t B = 0; B + 8 <= Size && Pos + B + 8 <= (int64_t)Img.size(); B += 8)
                {
                    const uint64_t V = At<uint64_t>(Img, Pos + B);
                    if (V == 0 || V == ~0ull) continue;
                    const int64_t To = Off(V);
                    if (To >= 0)
                    {
                        const auto It = Top.find(To);
                        if (It == Top.end() || It->second == (size_t)(&I - R.Instances.data())) continue;
                        const uint64_t Target = At<uint64_t>(Img, To) & IdBits;
                        if (Seen.insert({ 1, Target }).second) Put("ptr", std::format("{}:{:016x}", Type(R.Instances[It->second]), Target));
                        continue;
                    }
                    const uint64_t H = V & IdBits;
                    if ((V >> 63) == 0 && V < (1ull << 40)) continue;
                    if (H != Id && Seen.insert({ 0, H }).second) Put("hash", std::format("{:016x}", H));
                }
            }
            Rows += Own;
            Kt.Written++;
        }
        if (Rows.empty()) return;
        Rows.insert(0, "asset_type,asset_id,what,value\n");
        const fs::path File = fs::path(C.KindDir(Kind::AssetRefs)) / (ExportNaming::SafeFileName(ZoneName) + ".csv");
        if (!ExportWriters::WriteBytes(File, (const uint8_t*)Rows.data(), Rows.size())) T.Fail(ZoneName, Kind::AssetRefs, ZoneName, "write failed");
    });
}
