#include "stdafx.h"

#include "ZonekitZone.h"
#include "DiskGameSizes.h"
#include "ResumeIndex.h"
#include "MapSceneExport.h"

#include "CoDAssets.h"
#include "ExportNaming.h"
#include "FastFile.h"
#include "MapSceneWriter.h"
#include "MapGlbWriter.h"
#include "MapLive.h"
#include "MaterialDescription.h"
#include "Gdt.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <format>
#include <fstream>
#include <map>
#include <mutex>
#include <set>
#include <unordered_map>
#include <unordered_set>

namespace fs = std::filesystem;

ZonekitZone::ZonekitZone(std::string FastFilePath)
    : Path(std::move(FastFilePath)), ZoneName(fs::path(Path).stem().string())
{
    using namespace DiskGame;
    Exports(Kind::Model, [this](const Context& C, Totals& T) { ExportLive(Kind::Model, ModelType(), C, T); });
    Exports(Kind::Anim, [this](const Context& C, Totals& T) { ExportLive(Kind::Anim, AnimType(), C, T); });
    Exports(Kind::Localize, [this](const Context& C, Totals& T) { ExportLocalize(C, T); });
}

ZonekitZone* ZonekitZone::Serving = nullptr;

ZonekitZone::~ZonekitZone()
{
    if (Serving == this) Serving = nullptr;
}

std::vector<std::string> ZonekitZone::InlineStrings(ByteView Z, int64_t First, uint64_t Count, int SlotSize,
                                                    uint64_t Marker)
{
    std::vector<std::string> Out((size_t)Count);
    int64_t Text = First + (int64_t)Count * SlotSize;
    for (uint64_t K = 0; K < Count; K++)
    {
        const int64_t Slot = First + (int64_t)K * SlotSize;
        if ((SlotSize == 4 ? At<uint32_t>(Z, Slot) : At<uint64_t>(Z, Slot)) != Marker) continue;
        std::string S;
        while (Text < (int64_t)Z.size() && Z[(size_t)Text]) S.push_back((char)Z[(size_t)Text++]);
        Text++;
        Out[(size_t)K] = std::move(S);
    }
    return Out;
}

namespace
{
    class Shared final : public CoDPackageCache
    {
    public:
        explicit Shared(std::shared_ptr<CoDPackageCache> P) : Inner(std::move(P)) { SetLoadedState(); }
        std::unique_ptr<uint8_t[]> ExtractPackageObject(uint64_t CacheID, uint32_t& ResultSize) override
        {
            return Inner->ExtractPackageObject(CacheID, ResultSize);
        }
        uint64_t HashPackageID(const std::string& Value) override { return Inner->HashPackageID(Value); }

    private:
        std::shared_ptr<CoDPackageCache> Inner;
    };
}

std::unique_ptr<CoDPackageCache> ZonekitZone::SharedPackages(std::shared_ptr<CoDPackageCache> Packages)
{
    return std::make_unique<Shared>(std::move(Packages));
}

void ZonekitZone::ServePackages(bool On, const std::function<std::unique_ptr<CoDPackageCache>()>& Make)
{
    if (On == PackagesServed) return;
    if (On)
    {
        SavedPackages = std::move(CoDAssets::GamePackageCache);
        CoDAssets::GamePackageCache = Make();
    }
    else
        CoDAssets::GamePackageCache = std::move(SavedPackages);
    PackagesServed = On;
}

std::string ZonekitZone::SlotString(uint64_t Index)
{
    return VirtualGameSession::SlotString(Serving ? &Serving->Slots : nullptr, Index);
}

void ZonekitZone::ExportNamedImages(const DiskGame::Context& C, DiskGame::Totals& T, const NamedImageDecoder& Decode, const char* Unread,
                                    bool ReferencesByName)
{
    using DiskGame::Kind;
    auto& Kt = T.Kinds[Kind::Image];
    std::map<std::string, int64_t> Names;   // name -> the image's position
    std::set<std::string> ReferencedHere;
    for (const auto& I : R.Instances)
    {
        if (!OwnerIs(I, "image")) continue;
        std::string N = AssetName(Kind::Image, I.Pos);
        const bool Reference = IsReference(Kind::Image, N, I.Pos);
        if (Reference) N = Referenced(Kind::Image, N);
        if (N.empty() || !C.Wants(Kind::Image, N)) continue;
        if (Reference) ReferencedHere.insert(N);
        else Names.emplace(N, I.Pos);
    }
    for (const auto& N : ReferencedHere)
        if (!Names.count(N))
        {
            if (ReferencesByName)
            {
                Names.emplace(N, -1);   // read by name below, else needed
                continue;
            }
            Kt.References++;
            C.Needs(Kind::Image, N);
        }
    ExportWriters::ImageSink Sink;
    const fs::path Dir = C.KindDir(Kind::Image);
    const std::vector<std::pair<std::string, int64_t>> Order(Names.begin(), Names.end());
    for (size_t N = 0; N < Order.size(); N++)
    {
        const auto& [ImageName, Pos] = Order[N];
        if (!C.Advance(N, Order.size(), ImageName)) break;
        auto Dds = Decode(ImageName, Pos);
        if (!Dds && Pos < 0)
        {
            Kt.References++;
            C.Needs(Kind::Image, ImageName);
            continue;
        }
        Kt.Seen++;
        if (Pos < 0) Counter("images_referenced_read_by_name")++;
        if (!Dds)
        {
            Counter("images_unread")++;
            T.Fail(Name(), Kind::Image, ImageName, Unread);
            continue;
        }
        Sink.Write(std::move(Dds), (Dir / (ExportNaming::SafeFileName(ImageName) + Sink.Extension())).string());
    }
    size_t Missing = 0;
    const auto W = Sink.Finish([&](size_t Tag) { return Decode(Order[Tag].first, Order[Tag].second); }, Missing);
    Kt.Written += W.Written;
    Kt.Linked += W.Linked;
    if (W.Failed + Missing) T.Fail(Name(), Kind::Image, "", std::to_string(W.Failed + Missing) + " not written");
}

void ZonekitZone::ExportImageList(const DiskGame::Context& C, DiskGame::Totals& T, const ImageList& L)
{
    using DiskGame::Kind;
    auto& Kt = T.Kinds[Kind::Image];
    ExportWriters::ImageSink Sink;
    const fs::path Dir = C.KindDir(Kind::Image);
    std::set<std::string> ReferencedHere, DefinedHere;
    for (size_t N = 0; N < L.Order.size(); N++)
    {
        const int64_t Pos = L.Order[N];
        std::string ImageName = AssetName(Kind::Image, Pos + L.NameAt);
        if (!C.Advance(N, L.Order.size(), ImageName)) break;
        const bool Reference = IsReference(Kind::Image, ImageName, Pos);
        if (Reference) ImageName = Referenced(Kind::Image, ImageName);
        if (L.NamesChecked && ImageName.empty()) continue;
        if (!C.Wants(Kind::Image, ImageName)) continue;
        // a name read through a misplaced back reference (the layout off in a zone)
        if (L.NamesChecked && std::any_of(ImageName.begin(), ImageName.end(), [](char Ch) { return Ch < 0x20 || Ch > 0x7e; }))
        {
            T.Fail(Name(), Kind::Image, "", "an image name that does not read");
            continue;
        }
        if (Reference || (L.Held && !L.Held(Pos)))
        {
            Kt.References++;
            ReferencedHere.insert(ImageName);
            continue;
        }
        if (!DefinedHere.insert(ImageName).second && L.OnePerName) continue;
        Kt.Seen++;
        const std::string Path = (Dir / (ExportNaming::SafeFileName(ImageName) + Sink.Extension())).string();
        if (Sink.LinkIfKnown(L.KeyOf(Pos, Sink.Extension()), Path, (size_t)Pos))
        {
            Kt.Known++;
            continue;
        }
        auto Dds = L.Decode(Pos, ImageName);
        if (!Dds)
        {
            T.Fail(Name(), Kind::Image, ImageName, L.FailWhy ? L.FailWhy(Pos) : std::string());
            continue;
        }
        Sink.Write(std::move(Dds), Path);
    }
    size_t Missing = 0;
    const auto W = Sink.Finish([&](size_t Pos) { return L.Decode((int64_t)Pos, std::string()); }, Missing,
                               [&](size_t Pos) { return L.KeyOf((int64_t)Pos, Sink.Extension()); });
    Kt.Written += W.Written;
    Kt.Linked += W.Linked;
    if (W.Failed + Missing) T.Fail(Name(), Kind::Image, "", std::to_string(W.Failed + Missing) + " not written");
    // a reference to an image this zone also holds is no dependency
    for (const auto& N : ReferencedHere)
        if (!N.empty() && !DefinedHere.count(N)) C.Needs(Kind::Image, N);
}

void ZonekitZone::DefineImages(std::vector<std::pair<DiskGame::AssetId, int64_t>>& Out) const
{
    for (const auto& I : R.Instances)
        if (OwnerIs(I, "image"))
        {
            const std::string N = AssetName(DiskGame::Kind::Image, I.Pos);
            if (!N.empty() && !IsReference(DiskGame::Kind::Image, N, I.Pos)) Out.push_back({ { DiskGame::Kind::Image, N }, I.Pos });
        }
}

bool ZonekitZone::LoadBytes(ZoneBytes& Out, std::string& Error)
{
    return FastFile::Decompress(Path, Out, Error);
}

const ZoneTables::StreamRules& ZonekitZone::Rules() const
{
    return Loaded->Profile.Rules;
}

bool ZonekitZone::OwnerIs(const ZoneTables::ZoneResult::Instance& I, const char* Type) const
{
    return I.Owner < R.OwnerNames.size() && R.OwnerNames[I.Owner] == Type;
}

std::vector<int64_t> ZonekitZone::AllOf(const char* Type) const
{
    if (Partial)
    {
        const auto It = WholeRanked.find(Type);
        return It == WholeRanked.end() ? std::vector<int64_t>() : It->second;
    }
    std::vector<int64_t> Out;
    for (const auto& I : R.Instances)
        if (OwnerIs(I, Type)) Out.push_back(I.Pos);
    return Out;
}

std::string ZonekitZone::StringAt(uint64_t Address) const
{
    if (!Session || !Session->Contains(Address)) return std::string();
    std::string S;
    for (int64_t Off = (int64_t)(Address - Session->Origin()); Off >= 0 && Off < (int64_t)Bytes.size() && Bytes[(size_t)Off]; Off++)
        S.push_back((char)Bytes[(size_t)Off]);
    return S;
}

namespace
{
    // A back reference's block and offset in the stream memory, by the profile's encoding
    bool DecodeBackRef(const ZoneTables::StreamRules& Rs, uint64_t Value, int& Block, int64_t& Off)
    {
        if (Rs.Iw6Refs)
        {
            Block = (int)((Value >> 32) & 0xF);
            Off = (int64_t)(Value & 0xFFFFFFFFull) - 1;
            return true;
        }
        if (Rs.RefShift || Rs.PointerSize == 4 || Rs.Block32) return Rs.DecodeRef(Value, Block, Off);
        Block = (int)(Value >> 60);
        Off = (int64_t)(Value & 0x0FFFFFFFFFFFFFFFull) - 1;
        return true;
    }
}

std::string ZonekitZone::TextAt(int64_t Field) const
{
    const uint64_t Value = PointerAt(Field);
    if (Session && Session->Contains(Value)) return StringAt(Value);
    int Block = 0;
    int64_t Off = 0, Pos = 0;
    if (Value == 0 || Value == Rules().Marker || !DecodeBackRef(Rules(), Value, Block, Off)) return std::string();
    if (!Memory) Memory = std::make_unique<ZoneTables::BlockMap>(R);
    if (!Memory->ZonePos(Block, Off, Pos)) return std::string();
    std::string S;
    for (; Pos < (int64_t)Bytes.size() && Bytes[(size_t)Pos]; Pos++) S.push_back((char)Bytes[(size_t)Pos]);
    return S;
}

ExportWriters::ModelTextures ZonekitZone::Textures(const DiskGame::Context& C) const
{
    // Model textures go to the run's images/ folder, each written once (skipprevimg), named from
    // the model's folder models/<name>/ (a copy per model would make most of an install-wide export's
    // files).
    return { C.KindDir(DiskGame::Kind::Image), "..\\\\..\\\\images\\\\",
             ExportWriters::ImageExtension(ExportWriters::ImageFormatSetting()) };
}

bool ZonekitZone::Open(std::string& Error)
{
    auto Start = std::chrono::steady_clock::now();
    const auto Lap = [&](const char* Step) {
        const auto Now = std::chrono::steady_clock::now();
        Steps.emplace_back(Step, std::chrono::duration<double>(Now - Start).count());
        Start = Now;
    };
    // a run asking only for images the zone's resume index holds whole: served from it, the zone
    // not read (a model's textures in another zone)
    if (const DiskGame::Selection* Want = DiskGame::PartialScope::Wanted(); Want != nullptr && OpensPartially() && Heads().Size > 0)
        if (const std::vector<std::string> Sources = SourceFiles(); !Sources.empty())
            if ((Loaded = ExportWriters::ZoneProfile(ProfileName.empty() ? Profile() : ProfileName, Error)) != nullptr &&
                OpenFromHeads(*Want, ResumeIndex::FileFor(Path, ProfileKey(*Loaded), Sources), Lap))
                return true;
    if (!LoadBytes(Bytes, Error)) return false;
    Lap("load");
    Loaded = ExportWriters::ZoneProfile(ProfileName.empty() ? Profile() : ProfileName, Error);
    if (Loaded == nullptr) return false;
    Lap("profile");
    // a few assets asked: only them, from the zone's resume index, when it has one
    std::string ResumeFile;
    if (OpensPartially())
    {
        const std::vector<std::string> Sources = SourceFiles();
        if (!Sources.empty()) ResumeFile = ResumeIndex::FileFor(Path, ProfileKey(*Loaded), Sources);
        if (ResumeFile.empty()) ResumeFile = ResumeIndex::FileFor(Path, ProfileKey(*Loaded), Bytes);
        Lap("resume_name");
        if (const DiskGame::Selection* Want = DiskGame::PartialScope::Wanted())
        {
            if (OpenPartially(*Want, ResumeFile, Lap)) return true;
            Lap("partial_fell_back");
        }
    }
    {
        ZoneTables::StreamRules Rs = Loaded->Profile.Rules;
        Rs.StartOffset = LayoutOffset();
        R = ZoneTables::WalkZone(Bytes, Loaded->Tables, Loaded->Profile.TypeNames, Rs, true);
    }
    if (R.ParseFailed)
    {
        Error = R.Why;
        return false;
    }
    Walked = R.Why == "walked to the end";
    Lap("walk");
    // the links resolved before the session (the walk's alone); from here on, the process-wide
    // session lock is held (the game's handlers and fixups reach process-wide state)
    ResolvedLinks L = ResolveLinks(R, Rules(), DerefAssetPointers(), Bytes);
    auto Taken = VirtualGameSession::Acquire();
    Lap("links");
    const VirtualGameSession::Handlers H = Handlers();
    Session = std::make_unique<VirtualGameSession>(std::move(Taken), LiveGame(), ZoneMemory::Copy(Bytes), H, Rules().PointerSize);
    Serving = this;
    Relocate(L);
    Lap("relocate");
    Fixups();
    Lap("fixups");
    // the resume index a later partial open reads (one of an older format written again)
    if (!ResumeFile.empty() && !R.Resume.empty() && !ResumeIndex::IsCurrent(ResumeFile))
    {
        WriteResume(ResumeFile);
        Lap("resume_index");
    }
    return true;
}

bool ZonekitZone::OpenPartially(const DiskGame::Selection& Want, const std::string& File, const std::function<void(const char*)>& Lap)
{
    ZoneResume Z;
    if (!ResumeIndex::Load(File, Z)) return false;
    Lap("resume_read");
    // an index without a type the game counts is from before it did: read whole
    for (const std::string& T : Ranked())
        if (!Z.Ranked.count(T)) return false;
    std::vector<size_t> Targets;
    for (const auto& A : Want)
        if (const size_t P = Z.Find(A.K, A.Name); P != SIZE_MAX) Targets.push_back(P);
        else Counters["partial_not_indexed"]++;
    if (Targets.empty()) return false;
    ZoneTables::StreamRules Rs = Loaded->Profile.Rules;
    Rs.StartOffset = LayoutOffset();
    ResolveHints H;
    H.Points = &Z.Points;
    H.Keys = &Z.Keys;
    H.ByHash = &Z.ByHash;
    // the asked assets, then every asset their links reach (a cap: a composite reaching most of
    // the zone is read whole)
    constexpr size_t AtMost = 4000;
    bool Ok = true;
    ZoneTables::ZoneResult M;
    ZoneTables::WalkOnLargeStack([&] {
        ZoneTables::PartialWalk PW(Bytes, Loaded->Tables, Loaded->Profile.TypeNames, Rs, Z.Points);
        Ok = PW.Ok();
        if (!Ok) return;
        PW.UseKeys(H.Keyed());
        PW.AddAssetHashes(Z.ByHash);
        std::set<size_t> Seen;
        Ok = WalkReached(PW, Targets, Rs, H, Seen, Counters["partial_rounds"], AtMost);
        if (Ok)
        {
            M = PW.Merged();
            Listed = PW.ListedTypes();
        }
    });
    if (!Ok) return false;
    R = std::move(M);
    Walked = true;
    Partial = true;
    Counters["partial_assets_walked"] += R.Instances.empty() ? 0 : (size_t)std::count_if(R.Instances.begin(), R.Instances.end(), [](const auto& I) { return I.Parent < 0; });
    Lap("walk");
    ResolvedLinks L = ResolveLinks(R, Rs, DerefAssetPointers(), Bytes, &H);
    Whole.Keys = std::move(Z.Keys);
    Whole.ByHash = std::move(Z.ByHash);
    WholeRanked = std::move(Z.Ranked);
    auto Taken = VirtualGameSession::Acquire();
    Lap("links");
    const VirtualGameSession::Handlers Hd = Handlers();
    const ICoDGame& Game = LiveGame();
    Lap("live_game");
    ZoneMemory::Copy Served(Bytes);
    Lap("copy");
    Session = std::make_unique<VirtualGameSession>(std::move(Taken), Game, std::move(Served), Hd, Rules().PointerSize);
    Lap("session");
    Serving = this;
    Relocate(L);
    Lap("relocate");
    Fixups();
    Lap("fixups");
    return true;
}

void ZonekitZone::WriteResume(const std::string& File) const
{
    ZoneResume Z;
    Z.Size = Bytes.size();
    Z.Points = R.Resume;
    std::vector<int64_t> Starts;
    Starts.reserve(Z.Points.size());
    for (const auto& P : Z.Points) Starts.push_back(P.Pos);
    // each asset a run may ask for -> the resume point whose walk holds it
    std::vector<std::pair<DiskGame::AssetId, int64_t>> Named;
    DefinesAt(Named);
    for (const auto& [A, Pos] : Named)
    {
        auto It = std::upper_bound(Starts.begin(), Starts.end(), Pos);
        if (It == Starts.begin()) continue;
        Z.Names.emplace_back((uint8_t)A.K, ResumeIndex::NameHash(A.K, A.Name), (uint32_t)(It - Starts.begin() - 1));
    }
    std::sort(Z.Names.begin(), Z.Names.end());
    Z.Names.erase(std::unique(Z.Names.begin(), Z.Names.end()), Z.Names.end());
    // keyed references and assets by hash (Black Ops 6/7: keyed_refs or rewind_prepare): what a
    // partial walk cannot learn
    if (Rules().KeyMask != 0)
    {
        Z.Keys.assign(R.Keys.begin(), R.Keys.end());
        for (const auto& I : R.Instances)
            if (I.Pos >= 0 && I.Pos + 8 <= (int64_t)Bytes.size())
            {
                const uint64_t H = ExportNaming::At<uint64_t>(Bytes, I.Pos) & Rules().AssetHashMask;
                Z.ByHash.try_emplace({ R.OwnerNames[I.Owner], H }, I.Pos);
                Z.ByHash.try_emplace({ std::string(), H }, I.Pos);
            }
    }
    for (const std::string& T : Ranked()) Z.Ranked[T] = AllOf(T.c_str());
    ResumeIndex::Save(File, Z);
    // the images an export reads in their record alone (and the data it points at), as served
    if (const HeadSpec Spec = Heads(); Spec.Size > 0)
    {
        std::map<int64_t, std::string> Records;
        const ByteView Img = Image();
        for (const auto& I : R.Instances)
        {
            if (!OwnerIs(I, Spec.Type) || I.Pos + Spec.Size > (int64_t)Img.size()) continue;
            const ByteView Head = Img.subspan((size_t)I.Pos, (size_t)Spec.Size);
            std::string Record(reinterpret_cast<const char*>(Head.data()), Head.size());
            bool Whole = true;
            for (const auto& D : Spec.Data)
            {
                const uint64_t Raw = At<uint64_t>(Img, I.Pos + D.At);
                const int64_t From = Off(PointerAt(I.Pos + D.At));
                const uint64_t N = D.Bytes(Head);
                uint32_t Length = ~0u;
                if (From >= 0 && N <= D.Max && From + (int64_t)N <= (int64_t)Img.size())
                    Length = (uint32_t)N;
                else if (Raw != 0)
                    Whole = false;
                Record.append(reinterpret_cast<const char*>(&Length), 4);
                if (Length != ~0u) Record.append(reinterpret_cast<const char*>(Img.data()) + From, Length);
            }
            if (Whole) Records.emplace(I.Pos, std::move(Record));
        }
        std::map<uint64_t, std::pair<std::string, std::string>> Kept;
        for (const auto& [A, Pos] : Named)
            if (const auto It = Records.find(Pos); A.K == DiskGame::Kind::Image && It != Records.end())
                Kept.try_emplace(ResumeIndex::NameHash(A.K, A.Name), A.Name, It->second);
        ResumeIndex::SaveHeads(ResumeIndex::HeadsFile(File), Kept);
    }
}

ZonekitZone::HeadSpec ZonekitZone::ModernIwHeads()
{
    HeadSpec S;
    S.Type = "image";
    S.Size = 0x40;
    S.Data = { { 0x38, [](ByteView R) { return (uint64_t)ExportNaming::At<uint32_t>(R, 0x18); }, 256 << 10 },
               { 0x30, [](ByteView R) { return (uint64_t)ExportNaming::At<uint8_t>(R, 0x2e) * 24; } } };
    return S;
}

void ZonekitZone::WriteSoundFile(const DiskGame::Context& C, DiskGame::Totals& T, const std::string& File, const uint8_t* Data, size_t Size,
                                 const char* Written)
{
    const std::string Format = ExportWriters::SoundFormatSetting(), Out = ExportWriters::SoundFileName(File, Format);
    const fs::path Path = (C.KindDir(DiskGame::Kind::Sound) / fs::path(std::u8string(Out.begin(), Out.end()))).make_preferred();
    std::string Why;
    if (ExportWriters::WriteSound(ExportWriters::SoundSource::OfFile(Data, Size, File), Path, Format, &Why))
    {
        T.Kinds[DiskGame::Kind::Sound].Written++;
        Counter(Written)++;
    }
    else
        T.Fail(Name(), DiskGame::Kind::Sound, File, Why);
}

DiskGame::Weight ZonekitZone::ModernIwImageWeight(int64_t Pos, uint32_t (*Dxgi)(uint8_t Format)) const
{
    constexpr int64_t Loaded = 0x18, Width = 0x20, Height = 0x22, Format = 0x2a, MipCount = 0x2e, Mips = 0x30, Pixels = 0x38, MipSize = 24, Packed = 16;
    const ByteView Img = Image();
    if (Pos < 0 || Pos + 0x40 > (int64_t)Img.size()) return {};
    const uint32_t W = At<uint16_t>(Img, Pos + Width), H = At<uint16_t>(Img, Pos + Height);
    if (At<uint64_t>(Img, Pos + Pixels) != 0) return DiskGame::Sizes::ImageWeight(At<uint32_t>(Img, Pos + Loaded), W, H);
    const int64_t Table = Off(PointerAt(Pos + Mips));
    const uint32_t Count = At<uint8_t>(Img, Pos + MipCount) & 0xf;
    if (Table < 0 || Count == 0 || Table + Count * MipSize > (int64_t)Img.size()) return {};
    const auto Running = [&](uint32_t K) { return (At<uint64_t>(Img, Table + K * MipSize + Packed) >> 30) & 0x1FFFFFFF; };
    for (uint32_t K = Count; K-- > 0;)
    {
        const uint64_t Key = At<uint64_t>(Img, Table + K * MipSize);
        if (CoDAssets::GamePackageCache == nullptr || !CoDAssets::GamePackageCache->Exists(Key)) continue;
        const uint32_t Shift = Count - 1 - K, LevelW = std::max(1u, W >> Shift), LevelH = std::max(1u, H >> Shift);
        if (Dxgi != nullptr) return DiskGame::Sizes::ImageWeight(LevelW, LevelH, Dxgi(At<uint8_t>(Img, Pos + Format)), 1, 1);
        return DiskGame::Sizes::ImageWeight(Running(K) - (K ? Running(K - 1) : 0), LevelW, LevelH);
    }
    return {};
}

bool ZonekitZone::OpenFromHeads(const DiskGame::Selection& Want, const std::string& File, const std::function<void(const char*)>& Lap)
{
    const HeadSpec Spec = Heads();
    if (File.empty() || Want.empty() || !ResumeIndex::IsCurrent(File) ||
        std::any_of(Want.begin(), Want.end(), [](const DiskGame::AssetId& A) { return A.K != DiskGame::Kind::Image; }))
        return false;
    ResumeIndex::Heads Kept;
    if (!Kept.Open(ResumeIndex::HeadsFile(File))) return false;
    std::vector<std::pair<std::string_view, std::string_view>> Use;
    for (const auto& A : Want)
    {
        std::string_view Name, Record;
        if (!Kept.Find(ResumeIndex::NameHash(A.K, A.Name), Name, Record) || (int64_t)Record.size() < Spec.Size) return false;
        Use.emplace_back(Name, Record);
    }
    Lap("resume_read");
    // the records one after the other (16-aligned), each one's pixels (when kept) and name after it,
    // its pointers linked to them: the zone a walk of those images alone would have given
    Bytes.clear();
    R = ZoneTables::ZoneResult();
    R.OwnerNames = { Spec.Type };
    ResolvedLinks L;
    for (const auto& [Name, Record] : Use)
    {
        const int64_t Pos = ((int64_t)Bytes.size() + 15) & ~(int64_t)15;
        Bytes.resize((size_t)Pos);
        Bytes.insert(Bytes.end(), Record.begin(), Record.begin() + Spec.Size);
        size_t At = (size_t)Spec.Size;
        for (const auto& D : Spec.Data)
        {
            uint32_t Length = ~0u;
            if (At + 4 > Record.size()) return false;
            std::memcpy(&Length, Record.data() + At, 4);
            At += 4;
            if (Length == ~0u) continue;
            if (At + Length > Record.size()) return false;
            const int64_t Data = ((int64_t)Bytes.size() + 15) & ~(int64_t)15;
            Bytes.resize((size_t)Data);
            Bytes.insert(Bytes.end(), Record.begin() + At, Record.begin() + At + Length);
            At += Length;
            L.Links.emplace_back(Pos + D.At, Data);
        }
        if (Spec.NameAt >= 0)
        {
            L.Links.emplace_back(Pos + Spec.NameAt, (int64_t)Bytes.size());
            Bytes.insert(Bytes.end(), Name.begin(), Name.end());
            Bytes.push_back(0);
        }
        ZoneTables::ZoneResult::Instance I;
        I.Pos = Pos;
        I.End = Pos + Spec.Size;
        R.Instances.push_back(I);
    }
    Walked = true;
    Partial = true;
    Counters["partial_from_heads"] += Use.size();
    Lap("walk");
    auto Taken = VirtualGameSession::Acquire();
    const VirtualGameSession::Handlers Hd = Handlers();
    const ICoDGame& Game = LiveGame();
    Lap("live_game");
    Session = std::make_unique<VirtualGameSession>(std::move(Taken), Game, ZoneMemory::Copy(Bytes), Hd, Rules().PointerSize);
    Serving = this;
    Relocate(L);
    Fixups();
    Lap("fixups");
    return true;
}

bool WalkReached(ZoneTables::PartialWalk& PW, const std::vector<size_t>& Targets, const ZoneTables::StreamRules& Rs,
                 const ResolveHints& H, std::set<size_t>& Seen, size_t& Rounds, size_t AtMost)
{
    bool Ok = true;
    for (const size_t P : Targets) Ok = Ok && PW.Walk(P) != nullptr;
    // each round looks only at the walks the rounds before have not (an owner walked on demand
    // included): what those reached is walked already
    while (Ok)
    {
        std::set<size_t> New;
        for (const auto& [P, Result] : PW.Walked())
            if (Seen.insert(P).second) New.insert(P);
        if (New.empty()) break;
        const std::set<size_t> More = MissingOwners(PW.Merged(&New), Rs, PW, H);
        Rounds++;
        if (More.empty()) break;
        for (const size_t P : More) Ok = Ok && PW.Walk(P) != nullptr;
        Ok = Ok && PW.Walked().size() <= AtMost;
    }
    return Ok;
}

namespace
{
    // A resident pointer (the marker_resident value) reserves a slot in the resident block
    // (resident_slot) holding where its data went, which is read elsewhere (often a block that
    // does not grow). A later back reference to that slot means that data: BO3 view models share
    // their LODs this way. The slot is where the block's cursor stood when the pointer was met,
    // right before its data's first read: the end of the block's last placed read, aligned.
    // Reserved, when given, gets each slot as {the read it comes before, its offset}.
    std::unordered_map<int64_t, int64_t> ResidentSlots(const ZoneTables::ZoneResult& R, const ZoneTables::StreamRules& Rs,
                                                       ByteView Zone,
                                                       std::vector<std::pair<size_t, int64_t>>* Reserved = nullptr)
    {
        std::unordered_map<int64_t, int64_t> Slots;
        if (Zone.empty() || Rs.ResidentSize <= 0 || Rs.ResidentAlign <= 0) return Slots;
        const int64_t Ptr = Rs.PointerSize == 4 ? 4 : 8;   // a pointer field's width
        // the resident pointers' data (a few per zone), then where each is first read
        std::unordered_map<int64_t, size_t> FirstRead;
        for (const auto& [Field, Target] : R.Links)
        {
            uint64_t V = 0;
            if (Field < 0 || Field + Ptr > (int64_t)Zone.size()) continue;
            std::memcpy(&V, Zone.data() + Field, (size_t)Ptr);
            if (V == Rs.SlotMarker) FirstRead.emplace(Target, SIZE_MAX);
        }
        for (size_t K = 0; K < R.ReadPos.size() && !FirstRead.empty(); K++)
            if (const auto It = FirstRead.find(R.ReadPos[K]); It != FirstRead.end() && It->second == SIZE_MAX) It->second = K;
        std::vector<std::pair<size_t, int64_t>> Events;   // the data's first read, the data
        for (const auto& [Field, Target] : R.Links)
        {
            uint64_t V = 0;
            if (Field < 0 || Field + Ptr > (int64_t)Zone.size()) continue;
            std::memcpy(&V, Zone.data() + Field, (size_t)Ptr);
            if (V != Rs.SlotMarker) continue;
            const auto It = FirstRead.find(Target);
            if (It->second != SIZE_MAX) Events.emplace_back(It->second, Target);
        }
        std::sort(Events.begin(), Events.end());
        int64_t Cursor = 0;
        size_t E = 0;
        for (size_t K = 0; K < R.Reads.size() && E < Events.size(); K++)
        {
            for (; E < Events.size() && Events[E].first == K; E++)
            {
                Cursor = (Cursor + Rs.ResidentAlign - 1) / Rs.ResidentAlign * Rs.ResidentAlign;
                Slots.emplace(Cursor, Events[E].second);
                if (Reserved != nullptr) Reserved->emplace_back(K, Cursor);
                Cursor += Rs.ResidentSize;
            }
            if (K < R.ReadMem.size() && R.ReadMem[K] >= 0 && (int)(R.ReadMem[K] >> 40) == Rs.ResidentBlock)
                Cursor = (R.ReadMem[K] & 0xFFFFFFFFFFll) + (R.Reads[K] < 0 ? -R.Reads[K] : R.Reads[K]);
        }
        return Slots;
    }
}

ResolvedLinks ResolveLinks(const ZoneTables::ZoneResult& R, const ZoneTables::StreamRules& Rs, bool DerefAssetPointers,
                           ByteView Zone, const ResolveHints* Hints)
{
    ResolvedLinks Out;
    // counted here, named once at the end: a name looked up per link was most of the time
    size_t Inline = R.Links.size(), Asset = 0, Resident = 0, Data = 0, Derefs = 0, Unresolved = 0;
    Out.Links.reserve(R.Links.size() + R.BackRefs.size());
    Out.Links.assign(R.Links.begin(), R.Links.end());

    // Zone-level instances come in list order, one per inline or resident header.
    std::vector<int64_t> AssetPos(R.AssetHeads.size(), -1);
    if (Hints && Hints->Points)
    {
        // a partial walk's: every listed asset where its resume point says it starts
        for (const auto& P : *Hints->Points)
            if (P.ListIndex < AssetPos.size()) AssetPos[P.ListIndex] = P.Pos;
    }
    else
    {
        std::vector<int64_t> Roots;
        for (const auto& I : R.Instances)
            if (I.Parent < 0) Roots.push_back(I.Pos);
        size_t Next = 0;
        for (size_t K = 0; K < R.AssetHeads.size() && Next < Roots.size(); K++)
            if (R.AssetHeads[K] == Rs.Marker || R.AssetHeads[K] == Rs.Resident) AssetPos[K] = Roots[Next++];
    }

    // A back reference names a block and an offset of the stream memory. The asset list sits in
    // the start block right before where the first asset loads (R.Start4, from the zone format);
    // a reference into its 16-byte slot k names asset k. Anything else is data an earlier read
    // placed, found through the layout (blocks.txt, empties.txt).
    const int64_t Entry = Rs.PointerSize == 4 ? 8 : 16;   // XAsset {type, header}
    const int64_t ListAt = R.Start4 - Entry * (int64_t)R.AssetHeads.size();
    const ZoneTables::BlockMap Memory(R);
    std::vector<std::pair<size_t, int64_t>> Reserved;   // resident slots: the read each precedes, offset
    std::unordered_map<int64_t, int64_t> Slots;
    if (Hints && Hints->Points)
        for (const auto& [Off, Data] : R.Slots) Slots.emplace(Off, Data);   // as the walks reserved them
    else
        Slots = ResidentSlots(R, Rs, Zone, &Reserved);
    // stream.backref_asset_bit (IW8): such a reference counts from where the zone-level asset
    // holding the field began in its block; each root asset's block cursors at its first read.
    // A cursor counts the resident slots reserved up to there too, its own included (MWIII: the
    // -3 images a gfx_map holds take 8 bytes of block 15 each, no read; mp_jup_shipment_iw9's
    // map_ents begins 0x2c past its last read)
    std::vector<std::pair<int64_t, std::vector<int64_t>>> AssetStarts;   // root position -> cursors
    if (Rs.AssetBit > 0 && Hints && Hints->Points)
    {
        // a partial walk's: where each block's last read ended at its resume points
        for (const auto& P : *Hints->Points)
        {
            std::vector<int64_t> Cursor((size_t)std::max(Rs.StreamBlocks, 1), 0);
            for (const auto& [B, C] : P.Starts)
                if (B >= 0 && B < (int)Cursor.size()) Cursor[(size_t)B] = C;
            AssetStarts.emplace_back(P.Pos, std::move(Cursor));
        }
    }
    else if (Rs.AssetBit > 0)
    {
        std::unordered_map<int64_t, size_t> FirstRead;
        for (size_t K = 0; K < R.ReadPos.size(); K++) FirstRead.try_emplace(R.ReadPos[K], K);
        std::vector<std::pair<size_t, int64_t>> Roots;   // first read, position
        for (const auto& I : R.Instances)
            if (I.Parent < 0)
                if (const auto It = FirstRead.find(I.Pos); It != FirstRead.end()) Roots.emplace_back(It->second, I.Pos);
        std::sort(Roots.begin(), Roots.end());
        std::vector<int64_t> Cursor((size_t)std::max(Rs.StreamBlocks, 1), 0);
        if (Rs.StartBlock >= 0 && Rs.StartBlock < (int)Cursor.size()) Cursor[(size_t)Rs.StartBlock] = R.Start4;
        const bool SlotBlock = Rs.ResidentBlock >= 0 && Rs.ResidentBlock < (int)Cursor.size();
        size_t S = 0;   // the next reserved slot
        const auto Reserve = [&](size_t Before) {
            for (; S < Reserved.size() && Reserved[S].first <= Before; S++)
                if (SlotBlock) Cursor[(size_t)Rs.ResidentBlock] = Reserved[S].second + Rs.ResidentSize;
        };
        size_t K = Roots.empty() ? R.Reads.size() : Roots[0].first;
        for (const auto& [First, Pos] : Roots)
        {
            for (; K < First && K < R.ReadMem.size(); K++)
            {
                Reserve(K);
                if (R.ReadMem[K] >= 0 && (size_t)(R.ReadMem[K] >> 40) < Cursor.size())
                    Cursor[(size_t)(R.ReadMem[K] >> 40)] = (R.ReadMem[K] & 0xFFFFFFFFFFll) + (R.Reads[K] < 0 ? -R.Reads[K] : R.Reads[K]);
            }
            Reserve(First);
            AssetStarts.emplace_back(Pos, Cursor);
        }
        std::sort(AssetStarts.begin(), AssetStarts.end(), [](const auto& A, const auto& B) { return A.first < B.first; });
    }
    std::vector<std::pair<int64_t, int64_t>> Deref;   // field, referenced place
    // stream.keyed_refs (Black Ops 6/7): no stream layout; a data reference names the key an inline
    // pointer filed its data under (the first filing wins, as the game's map keeps it), an asset
    // reference the name hash at +0 of an asset instance of the zone
    // (an asset reference names an instance of the type its chain reads inline, R.RefTypes)
    std::unordered_map<uint64_t, int64_t> Keyed;   // the walk's own keys, under the hints' (Hints->Keyed)
    std::map<std::pair<uint16_t, uint64_t>, int64_t> Hashed;
    if (Rs.InlineBit > 0)
    {
        for (const auto& [Key, Pos] : R.Keys) Keyed.try_emplace(Key, Pos);
        if (!Zone.empty() && !Hints)
            for (const auto& I : R.Instances)
                if (I.Pos >= 0 && I.Pos + 8 <= (int64_t)Zone.size())
                    Hashed.try_emplace({ I.Owner, ExportNaming::At<uint64_t>(Zone, I.Pos) & Rs.AssetHashMask }, I.Pos);
    }
    const auto KeyedAt = [&](uint64_t Key) -> int64_t {
        if (Hints)
            if (const auto It = Hints->Keyed().find(Key); It != Hints->Keyed().end()) return It->second;
        const auto It = Keyed.find(Key);
        return It == Keyed.end() ? -1 : It->second;
    };
    for (size_t K = 0; K < R.BackRefs.size(); K++)
    {
        const auto& [Field, Value] = R.BackRefs[K];
        if (Rs.InlineBit > 0)
        {
            const bool IsAsset = K < R.BackRefIsAsset.size() && R.BackRefIsAsset[K];
            int64_t Pos = -1;
            if (IsAsset)
            {
                const auto Type = R.RefTypes.find(Field);
                if (Hints)
                {
                    // the zone's assets by type name and hash, the walks' owner ids being their own
                    if (Type != R.RefTypes.end() && Type->second < R.OwnerNames.size())
                        if (Hints->ByHash != nullptr)
                            if (const auto It = Hints->ByHash->find({ R.OwnerNames[Type->second], Value & Rs.AssetHashMask }); It != Hints->ByHash->end())
                            Pos = It->second;
                }
                else
                {
                    const auto It = Type == R.RefTypes.end() ? Hashed.end() : Hashed.find({ Type->second, Value & Rs.AssetHashMask });
                    if (It != Hashed.end()) Pos = It->second;
                }
            }
            else
                Pos = KeyedAt(Value & Rs.KeyMask);
            if (Pos < 0)
            {
                Unresolved++;
                continue;
            }
            Out.Links.emplace_back(Field, Pos);
            (IsAsset ? Asset : Data)++;
            continue;
        }
        int Block = 0;
        int64_t Off = 0;
        if (!DecodeBackRef(Rs, Value, Block, Off))
        {
            Unresolved++;
            continue;
        }
        if (!Rs.Iw6Refs && (Rs.RefShift || Rs.PointerSize == 4 || Rs.Block32) && Rs.AssetRelative(Value))
        {
            auto It = std::upper_bound(AssetStarts.begin(), AssetStarts.end(), Field,
                                       [](int64_t F, const auto& A) { return F < A.first; });
            if (It == AssetStarts.begin() || (size_t)Block >= (--It)->second.size())
            {
                Unresolved++;
                continue;
            }
            Off += It->second[(size_t)Block];
        }
        if (Block == Rs.StartBlock && Off >= ListAt && Off < R.Start4)
        {
            const int64_t Pos = AssetPos[(size_t)((Off - ListAt) / Entry)];
            if (Pos >= 0)
            {
                Out.Links.emplace_back(Field, Pos);
                Asset++;
            }
            else
                Unresolved++;
            continue;
        }
        int64_t Pos = 0;
        if (!Memory.ZonePos(Block, Off, Pos))
        {
            // a resident pointer's slot: the reference names what that pointer points to
            const auto Slot = Block == Rs.ResidentBlock ? Slots.find(Off) : Slots.end();
            if (Slot == Slots.end())
            {
                Unresolved++;
                continue;
            }
            Out.Links.emplace_back(Field, Slot->second);
            Resident++;
            continue;
        }
        if (DerefAssetPointers && K < R.BackRefIsAsset.size() && R.BackRefIsAsset[K])
        {
            Deref.emplace_back(Field, Pos);
            continue;
        }
        Out.Links.emplace_back(Field, Pos);
        Data++;
    }
    // An asset pointer, where the game resolves it by reading the pointer at the referenced place
    // (IW6 resolve_deref): the asset is where that field points. Chains resolve in turn. A field's
    // target is its first inline link, else its last resolved one.
    if (!Deref.empty())
    {
        std::unordered_map<int64_t, int64_t> FieldTarget;
        FieldTarget.reserve(Out.Links.size());
        for (size_t K = 0; K < Out.Links.size(); K++)
            if (K < R.Links.size()) FieldTarget.try_emplace(Out.Links[K].first, Out.Links[K].second);
            else FieldTarget.insert_or_assign(Out.Links[K].first, Out.Links[K].second);
        for (bool Progress = true; Progress && !Deref.empty();)
        {
            Progress = false;
            std::vector<std::pair<int64_t, int64_t>> Waiting;
            for (const auto& [Field, Place] : Deref)
            {
                auto It = FieldTarget.find(Place);
                if (It == FieldTarget.end())
                {
                    Waiting.emplace_back(Field, Place);
                    continue;
                }
                const int64_t Target = It->second;
                Out.Links.emplace_back(Field, Target);
                FieldTarget.insert_or_assign(Field, Target);
                Derefs++;
                Progress = true;
            }
            Deref.swap(Waiting);
        }
    }
    Unresolved += Deref.size();
    auto& Counters = Out.Counters;
    for (const auto& [Name, N] : { std::pair<const char*, size_t>{ "links_inline", Inline }, { "links_asset", Asset },
                                   { "links_resident", Resident }, { "links_data", Data }, { "links_deref", Derefs } })
        if (N) Counters[Name] += N;
    Counters["refs_unresolved"] += Unresolved;
    return Out;
}

const std::unordered_map<uint64_t, int64_t>& ResolveHints::Keyed() const
{
    if (!KeyedOnce)
    {
        auto M = std::make_shared<std::unordered_map<uint64_t, int64_t>>();
        if (Keys != nullptr)
        {
            M->reserve(Keys->size());
            for (const auto& [Key, Pos] : *Keys) M->try_emplace(Key, Pos);
        }
        KeyedOnce = std::move(M);
    }
    return *KeyedOnce;
}

std::set<size_t> MissingOwners(const ZoneTables::ZoneResult& M, const ZoneTables::StreamRules& Rs,
                               const ZoneTables::PartialWalk& Walk, const ResolveHints& Hints)
{
    std::set<size_t> Out;
    const auto& Walked = Walk.Walked();
    const auto Need = [&](size_t Point) {
        if (Point != SIZE_MAX && !Walked.count(Point)) Out.insert(Point);
    };
    const int64_t Entry = Rs.PointerSize == 4 ? 8 : 16;
    const int64_t ListAt = M.Start4 - Entry * (int64_t)M.AssetHeads.size();
    const auto& Keyed = Hints.Keyed();
    for (const auto& [Field, Target] : M.Links) Need(Walk.OwnerOf(Target));
    for (size_t K = 0; K < M.BackRefs.size(); K++)
    {
        const auto& [Field, Value] = M.BackRefs[K];
        if (Rs.InlineBit > 0)
        {
            int64_t Pos = -1;
            if (K < M.BackRefIsAsset.size() && M.BackRefIsAsset[K])
            {
                const auto Type = M.RefTypes.find(Field);
                if (Type != M.RefTypes.end() && Type->second < M.OwnerNames.size())
                    if (Hints.ByHash != nullptr)
                        if (const auto It = Hints.ByHash->find({ M.OwnerNames[Type->second], Value & Rs.AssetHashMask }); It != Hints.ByHash->end())
                        Pos = It->second;
            }
            else if (const auto It = Keyed.find(Value & Rs.KeyMask); It != Keyed.end())
                Pos = It->second;
            if (Pos >= 0) Need(Walk.OwnerOf(Pos));
            continue;
        }
        int Block = 0;
        int64_t Off = 0;
        if (!DecodeBackRef(Rs, Value, Block, Off)) continue;
        if (!Rs.Iw6Refs && (Rs.RefShift || Rs.PointerSize == 4 || Rs.Block32) && Rs.AssetRelative(Value))
        {
            const size_t Holder = Walk.OwnerOf(Field);
            if (Holder == SIZE_MAX) continue;
            for (const auto& [B, C] : Walk.ResumePoints()[Holder].Starts)
                if (B == Block) Off += C;
        }
        if (Block == Rs.StartBlock && Off >= ListAt && Off < M.Start4)
        {
            Need(Walk.PointOfListIndex((uint32_t)((Off - ListAt) / Entry)));
            continue;
        }
        Need(Walk.PlacedBy(Block, Off));
    }
    return Out;
}

void ZonekitZone::Relocate(const ResolvedLinks& L)
{
    for (const auto& [Field, Target] : L.Links) Session->Link((uint64_t)Field, (uint64_t)Target);
    for (const auto& [Name, N] : L.Counters) Counters[Name] += N;
}

bool ZonekitZone::Park()
{
    if (!Session) return false;
    Served(false);
    if (Serving == this) Serving = nullptr;
    ParkedImage = Session->Release();
    Session.reset();
    return true;
}

void ZonekitZone::Resume()
{
    if (Session) return;
    Session = std::make_unique<VirtualGameSession>(VirtualGameSession::Acquire(), LiveGame(), std::move(ParkedImage), Handlers(), Rules().PointerSize);
    Serving = this;
    Served(true);
}

void ZonekitZone::Measures(DiskGame::Totals& T) const
{
    for (const auto& [Name, N] : Counters) T.Counters[Name] += N;
}

void ZonekitZone::ExportLive(DiskGame::Kind K, const char* Type, const DiskGame::Context& C, DiskGame::Totals& T)
{
    auto& Kt = T.Kinds[K];
    // the kind's assets first, for the progress count
    std::vector<std::pair<int64_t, std::string>> Assets;
    std::set<std::string> ReferencedHere, DefinedHere;
    for (const auto& I : R.Instances)
    {
        if (!OwnerIs(I, Type)) continue;
        std::string Name = AssetName(K, I.Pos);
        if (const char* Why = Unwritable(K, I.Pos))
        {
            if (C.Wants(K, Name)) Counter(Why)++;
            continue;
        }
        const bool Reference = IsReference(K, Name, I.Pos);
        if (Reference) Name = Referenced(K, Name);
        if (!C.Wants(K, Name)) continue;
        if (Reference)
        {
            Kt.References++;
            ReferencedHere.insert(Name);
            continue;
        }
        DefinedHere.insert(Name);
        Assets.emplace_back(I.Pos, std::move(Name));
    }
    // what only a reference names here is needed from the zone that defines it
    for (const auto& N : ReferencedHere)
        if (!N.empty() && !DefinedHere.count(N)) C.Needs(K, N);
    // A model's textures are named where the image kind writes them (images/<name>) and written
    // by it once the models are done. A slot without an image, or whose pointer lands on no
    // image the walk read (inside its material: a link the layout gets wrong), is left out.
    std::set<std::string> TextureNames;
    std::unordered_set<int64_t> Images;
    size_t NotImage = 0;
    ExportWriters::ModelTextures Tex = Textures(C);
    if (K == DiskGame::Kind::Model && ExporterOf(DiskGame::Kind::Image) != nullptr)
    {
        // first: another zone may be opened meanwhile, this one parked
        Tex.Fill = HeaderFiller(C);
        for (const auto& I : R.Instances)
            if (OwnerIs(I, ImageType())) Images.insert(I.Pos);
        Tex.Take = [&](XImage_t& I) {
            if (I.ImagePtr == 0 || I.ImageName.empty()) return false;
            if (I.ImagePtr != ExportWriters::ImageElsewhere && !Images.count((int64_t)(I.ImagePtr - Session->Origin())))
            {
                // an image of a zone served beside is taken, but an engine built-in there ($white,
                // $identitynormalmap) is left out, as when no zone held it
                const bool Beside = ImageBeside(I.ImagePtr);
                if (!Beside) NotImage++;
                if (!Beside || I.ImageName[0] == '$') return false;
            }
            I.ImageName = Referenced(DiskGame::Kind::Image, I.ImageName);   // ",name" is name
            TextureNames.insert(I.ImageName);
            I.ImageName = ExportNaming::SafeFileName(I.ImageName);
            return true;
        };
    }
    for (size_t N = 0; N < Assets.size(); N++)
    {
        const auto& [Pos, Name] = Assets[N];
        if (!C.Advance(N, Assets.size(), Name)) return;
        Kt.Seen++;
        std::string Crash;
        int Code = -1;
        if (K == DiskGame::Kind::Model)
        {
            CoDModel_t Model;
            Model.AssetName = ExportNaming::SafeFileName(Name);   // it names the files ("mv/x" is a folder)
            Model.AssetPointer = Live().Addr((uint64_t)Pos);
            const std::string Dir = (fs::path(C.KindDir(K)) / Model.AssetName).string();
            std::error_code Ec;
            fs::create_directories(Dir, Ec);
            Code = ExportWriters::ExportModel(Model, Dir, Tex, Crash);
        }
        else
        {
            CoDAnim_t Anim;
            Anim.AssetName = Name;
            Anim.AssetPointer = Live().Addr((uint64_t)Pos);
            Code = ExportWriters::ExportAnim(Anim, C.KindDir(K), Crash);
        }
        if (Code == static_cast<int>(ExportGameResult::Success)) Kt.Written++;
        else if (K == DiskGame::Kind::Model && Code == static_cast<int>(ExportGameResult::Placeholder))
            Counter("models_without_lods")++;   // the game's own model has no geometry: nothing to write
        else T.Fail(ZoneName, K, Name, "code " + std::to_string(Code) + (Crash.empty() ? "" : " " + Crash));
    }
    C.Advance(Assets.size(), Assets.size());
    if (NotImage) T.Fail(ZoneName, K, "", std::to_string(NotImage) + " texture slots point at no image, left out");
    if (!TextureNames.empty()) ExportTextures(TextureNames, C, T);
}

void ZonekitZone::ExportTextures(const std::set<std::string>& Names, const DiskGame::Context& C, DiskGame::Totals& T)
{
    using DiskGame::Kind;
    auto Only = std::make_shared<DiskGame::Selection>();
    for (const auto& N : Names) Only->insert({ Kind::Image, N });
    DiskGame::Context Tex = C;
    Tex.Only = std::move(Only);
    Tex.Kinds = { Kind::Image };
    DiskGame::Selection Met;
    Tex.OnWanted = [&](const DiskGame::AssetId& A) {
        Met.insert(A);
        if (C.OnWanted) C.OnWanted(A);
    };
    DiskGame::Totals Own;   // counted as textures, not as the run's images
    Export(Kind::Image, Tex, Own);
    const auto& Img = Own.Kinds[Kind::Image];
    Counter("textures") += Names.size();
    Counter("textures_written") += Img.Written + Img.Linked + Img.Known;
    T.Failures.insert(T.Failures.end(), Own.Failures.begin(), Own.Failures.end());
    T.Absent.insert(T.Absent.end(), Own.Absent.begin(), Own.Absent.end());
    // not in this zone at all: the zone that defines it gives it
    for (const auto& N : Names)
        if (!Met.count({ Kind::Image, N })) C.Needs(Kind::Image, N);
}

int64_t ZonekitZone::Root(const char* Type) const
{
    for (const auto& I : R.Instances)
        if (I.Parent < 0 && OwnerIs(I, Type)) return I.Pos;
    return -1;
}

bool ZonekitZone::Lists(const char* Type) const
{
    return Partial ? Listed.count(Type) > 0 : Root(Type) >= 0;
}

std::string ZonekitZone::ModelAt(uint64_t Address) const
{
    if (!Session || !Session->Contains(Address)) return std::string();
    const int64_t Pos = (int64_t)(Address - Session->Origin());
    const std::string Name = AssetName(DiskGame::Kind::Model, Pos);
    return IsReference(DiskGame::Kind::Model, Name, Pos) ? Referenced(DiskGame::Kind::Model, Name) : Name;
}

void ZonekitZone::AddStaticModels(Maps::MapScene& Scene, std::map<std::string, uint64_t>& Models, int64_t Array,
                                  uint32_t Count, int64_t Stride, int64_t ModelField, int64_t Placement)
{
    const auto& Img = Image();
    for (uint32_t K = 0; Array >= 0 && K < Count; K++)
    {
        const int64_t Rec = Array + (int64_t)K * Stride;
        const uint64_t Address = PointerAt(Rec + ModelField);
        const std::string ModelName = ModelAt(Address);
        if (ModelName.empty()) continue;
        Maps::Instance In;
        In.Name = "smodel_" + std::to_string(K);
        In.Model = ModelName;
        for (int J = 0; J < 3; J++) In.Origin[J] = At<float>(Img, Rec + Placement + J * 4);
        for (int J = 0; J < 9; J++) In.Axis[J] = At<float>(Img, Rec + Placement + 12 + J * 4);
        In.Scale = At<float>(Img, Rec + Placement + 48);
        Scene.StaticInstances.push_back(In);
        Models.emplace(ModelName, Address);
    }
}

void ZonekitZone::AddMapEnts(Maps::MapScene& Scene, const char* Type)
{
    // zone-level, else inside another asset (CoD4, World at War: the clipMap_t's)
    int64_t Ents = Root(Type);
    for (size_t K = 0; Ents < 0 && K < R.Instances.size(); K++)
        if (OwnerIs(R.Instances[K], Type)) Ents = R.Instances[K].Pos;
    if (Ents >= 0) Scene.MapEnts = StringAt(PointerAt(Ents + Rules().PointerSize));
}

Maps::ClipSource ZonekitZone::ClipSource() const
{
    Maps::ClipSource Src;
    Src.Image = &Image();
    Src.PointerSize = Rules().PointerSize;
    Src.Off = [this](uint64_t Address) { return Off(Address); };
    Src.String = [this](uint64_t Address) { return StringAt(Address); };
    return Src;
}

bool ZonekitZone::AddTriggers(Maps::MapScene& Scene, int64_t At, const char* Type, const Maps::TriggerLayout& L)
{
    int64_t Ents = Root(Type);
    for (size_t K = 0; Ents < 0 && K < R.Instances.size(); K++)
        if (OwnerIs(R.Instances[K], Type)) Ents = R.Instances[K].Pos;
    if (Ents < 0) return false;
    return Maps::ReadTriggers(Scene.Collision, ClipSource(), Ents + At, L);
}

bool ZonekitZone::AddCollision(Maps::MapScene& Scene, const std::vector<const char*>& Types, const Maps::ClipLayout& L)
{
    int64_t Clip = -1;
    for (size_t K = 0; Clip < 0 && K < Types.size(); K++) Clip = Root(Types[K]);
    if (Clip < 0) return false;
    Maps::ClipSource Src;
    Src.Image = &Image();
    Src.PointerSize = Rules().PointerSize;
    Src.Off = [this](uint64_t Address) { return Off(Address); };
    // a back reference left as the game stores it: the place an earlier read filled
    std::unique_ptr<ZoneTables::BlockMap> Memory;
    if (L.BackRefs)
    {
        Memory = std::make_unique<ZoneTables::BlockMap>(R);
        Src.Off = [this, &Memory](uint64_t Address) {
            if (const int64_t Pos = Off(Address); Pos >= 0) return Pos;
            int Block = 0;
            int64_t Offset = 0, Pos = -1;
            if (!Rules().DecodeRef(Address, Block, Offset) || !Memory->ZonePos(Block, Offset, Pos)) return (int64_t)-1;
            return Pos;
        };
    }
    Src.String = [this](uint64_t Address) { return StringAt(Address); };
    std::map<std::string, size_t> Unread;
    const bool Read = Maps::ReadCollision(Scene.Collision, Src, Clip, L, Unread);
    for (const auto& [Key, N] : Unread) Counter("map_" + Key) += N;
    return Read;
}

// The level as one .glb (MapGlbWriter): what the game's ReadMap reads, the placed models through
// the live model reader, their textures through the image kind (the zone's own, the rest needed
// from the zones that define them) and, after the run's dependencies, copied as PNG for glTF.
void ZonekitZone::ExportMap(const DiskGame::Context& C, DiskGame::Totals& T)
{
    using DiskGame::Kind;
    auto& Kt = T.Kinds[Kind::Map];
    Maps::MapScene Scene;
    std::map<std::string, uint64_t> Models;
    if (!ReadMap(Scene, Models) || !C.Wants(Kind::Map, Scene.Name)) return;
    if (DiskGame::MapContent.Of(C.Settings) == "terrain") Maps::KeepTerrain(Scene, Models);
    Kt.Seen++;
    const Materials::IAdapter& Adapter = Materials::AdapterFor(LiveGame().Engine());
    Scene.Game = Adapter.Name();

    // each placed model once; one this zone only names is exported from the zone defining it
    size_t N = 0;
    for (const auto& [Name, Address] : Models)
    {
        if (!C.Advance(N++, Models.size(), Name)) return;
        const int64_t Pos = Live().Contains(Address) ? (int64_t)(Address - Live().Origin()) : -1;
        if (Pos < 0 || IsReference(Kind::Model, AssetName(Kind::Model, Pos), Pos))
        {
            C.Needs(Kind::Model, Name);
            Counter("map_models_elsewhere")++;
            continue;
        }
        CoDModel_t Model;
        Model.AssetName = ExportNaming::SafeFileName(Name);
        Model.AssetPointer = Address;
        ExportWriters::LiveModel Read;
        std::string Crash;
        const int Code = ExportWriters::ReadModel(Model, Read, Crash);
        size_t Hidden = 0;
        const bool Added = Code == 0 && Maps::AddModel(Scene, Name, Read, [this](const XMaterial_t& M) { return MaterialDrawn(M); }, &Hidden);
        if (Hidden) Counter("map_model_faces_not_drawn") += Hidden;
        if (Added) continue;
        Counter("map_models_unread")++;
        if (Code < 0) T.Fail(ZoneName, Kind::Map, Name, "model " + Crash);
    }
    C.Advance(Models.size(), Models.size());

    // a material the level names only (",name": no image slot) gets the images of the material it
    // stands for, this zone's or another's, as a model's do (HeaderFiller)
    if (ExporterOf(Kind::Image) != nullptr)
        if (const auto Fill = HeaderFiller(C))
        {
            std::vector<XMaterial_t> Filled;
            for (const auto& [MatName, Mat] : Scene.Materials)
            {
                if (!Mat.Textures.empty()) continue;
                XMaterial_t X(0);
                X.MaterialName = MatName, X.TechsetName = Mat.Techset, X.Constants = Mat.Constants, X.SortKey = Mat.SortKey;
                Fill(X);
                if (!X.Images.empty()) Filled.push_back(std::move(X));
            }
            for (const auto& X : Filled)
            {
                Scene.Materials.erase(X.MaterialName);
                Maps::AddMaterial(Scene, X);
            }
        }

    // the textures as the image kind names them (",name" is name), written by it
    std::map<std::string, std::string> Renamed;
    // (an image the reader made, its PNG bytes at hand, keeps its name: WriteMapFiles writes it)
    for (const auto& [Name, Img] : Scene.Images) Renamed[Name] = Img.Png.empty() ? Referenced(Kind::Image, Name) : Name;
    std::map<std::string, Maps::Image> Images;
    for (auto& [MatName, Mat] : Scene.Materials)
    {
        Mat.ColorMap = Mat.ColorMap.empty() ? Mat.ColorMap : Renamed[Mat.ColorMap];
        for (auto& Tx : Mat.Textures) Tx.Image = Renamed[Tx.Image];
    }
    std::set<std::string> Names;
    for (const auto& [From, To] : Renamed)
        if (!To.empty() && Scene.Images[From].Png.empty()) Names.insert(To);
    if (ExporterOf(Kind::Image) != nullptr && !Names.empty()) ExportTextures(Names, C, T);

    // the files the image kind wrote, then the scene in every enabled format (MapSceneExport.h)
    if (!Scene.Sky.Image.empty()) Scene.Sky.Image = Renamed[Scene.Sky.Image];
    const std::string Ext = Textures(C).Extension;
    const std::filesystem::path ImageDir = C.KindDir(Kind::Image);
    const auto FileOf = [&](const std::string& Name) { return (ImageDir / (ExportNaming::SafeFileName(Name) + Ext)).string(); };
    const std::string Stem = (std::filesystem::path(C.KindDir(Kind::Map)) / ExportNaming::SafeFileName(ZoneName)).string();
    if (Maps::WriteMapFiles(Scene, Names, FileOf, Ext, Stem, Adapter, ZoneName, C, T, Counters)) Kt.Written++;
}

// Localized strings: one key,text CSV per zone (localize/<zone>.csv), a row per entry.
void ZonekitZone::Strings(std::vector<std::pair<std::string, std::string>>& Out) const
{
    for (const auto& I : R.Instances)
    {
        if (I.Parent >= 0 || !OwnerIs(I, LocalizeType())) continue;
        std::string Key, Text;
        LocalizeEntry(I.Pos, Key, Text);
        Out.emplace_back(std::move(Key), std::move(Text));
    }
}

// A stringtable: 64-bit {name, i32 columns 8, i32 rows 0xc, cells 0x10 of 16 bytes (string, hash)},
// 32-bit {name, columns 4, rows 8, cells 0xc of 8 bytes}.
void ZonekitZone::Documents(std::vector<std::pair<std::string, std::string>>& Out) const
{
    const bool Narrow = Rules().PointerSize == 4;
    const int64_t ColsAt = Narrow ? 4 : 8, CellsAt = Narrow ? 0xc : 0x10, Cell = Narrow ? 8 : 16;
    for (const auto& I : R.Instances)
    {
        if (I.Parent >= 0 || !OwnerIs(I, "stringtable")) continue;
        const std::string Name = StringAt(PointerAt(I.Pos));
        if (Name.empty() || Name[0] == ',') continue;
        const int32_t Cols = At<int32_t>(Image(), I.Pos + ColsAt), Rows = At<int32_t>(Image(), I.Pos + ColsAt + 4);
        const int64_t Cells = Off(PointerAt(I.Pos + CellsAt));
        if (Cols <= 0 || Rows <= 0 || Cells < 0 || Cells + (int64_t)Cols * Rows * Cell > (int64_t)Image().size()) continue;
        std::string Csv;
        for (int32_t Row = 0; Row < Rows; Row++)
        {
            for (int32_t K = 0; K < Cols; K++)
                Csv += (K ? "," : "") + StringAt(PointerAt(Cells + ((int64_t)Row * Cols + K) * Cell));
            Csv += '\n';
        }
        Out.emplace_back(Name, std::move(Csv));
    }
}

void ZonekitZone::ExportLocalize(const DiskGame::Context& C, DiskGame::Totals& T)
{
    auto& Kt = T.Kinds[DiskGame::Kind::Localize];
    std::vector<std::pair<std::string, std::string>> All, Rows;
    Strings(All);
    for (auto& [Key, Text] : All)
    {
        if (!C.Wants(DiskGame::Kind::Localize, Key)) continue;
        if (Key.empty())
        {
            Kt.References++;
            continue;
        }
        Kt.Seen++;
        Rows.emplace_back(std::move(Key), std::move(Text));
    }
    if (Rows.empty()) return;
    const fs::path Path = fs::path(C.KindDir(DiskGame::Kind::Localize)) / (ExportNaming::SafeFileName(ZoneName) + ".csv");
    if (ExportWriters::WriteLocalize(Path.string(), Rows, LocalizeCodePage())) Kt.Written += Rows.size();
    else T.Fail(ZoneName, DiskGame::Kind::Localize, ZoneName, "write failed");
}

bool ZonekitZone::BytesAt(uint64_t Address, uint64_t N, std::vector<uint8_t>& Out) const
{
    const int64_t From = Off(Address);
    if (From < 0 || (uint64_t)From + N > Image().size()) return false;
    Out.assign(Image().data() + From, Image().data() + From + N);
    return true;
}

void ZonekitZone::ExportScripts(const DiskGame::Context& C, DiskGame::Totals& T, const Scripts::Vm& Vm,
                                const std::vector<std::pair<const char*, ScriptReader>>& Readers)
{
    using DiskGame::Kind;
    auto& Kt = T.Kinds[Kind::Script];
    std::vector<std::pair<int64_t, const ScriptReader*>> Found;
    for (const auto& I : R.Instances)
        if (I.Parent < 0)
            for (const auto& [Type, Read] : Readers)
                if (OwnerIs(I, Type)) Found.emplace_back(I.Pos, &Read);
    Scripts::Sink Out(C, T, ZoneName, Vm);
    for (size_t N = 0; N < Found.size(); N++)
    {
        const auto [Pos, Read] = Found[N];
        Scripts::Script S;
        S.Name = AssetName(Kind::Script, Pos);
        if (!C.Advance(N, Found.size(), S.Name)) break;
        if (IsReference(Kind::Script, S.Name, Pos))
        {
            Kt.References++;
            continue;
        }
        if (!C.Wants(Kind::Script, S.Name)) continue;
        const std::string Name = S.Name;
        if (!(*Read)(Pos, S))
        {
            Kt.Seen++;
            T.Fail(ZoneName, Kind::Script, Name, "payload not read");
            continue;
        }
        if (!S.Name.empty()) Out.Add(std::move(S));
    }
    C.Advance(Found.size(), Found.size(), std::string("(decompiler)"));
    Out.Finish();
}

bool ZonekitZone::ReadScriptFile(int64_t Pos, Scripts::Script& Out) const
{
    const uint32_t Packed = At<uint32_t>(Image(), Pos + 8), Len = At<uint32_t>(Image(), Pos + 0xc),
                   CodeLen = At<uint32_t>(Image(), Pos + 0x10);
    std::vector<uint8_t> Stack, Code;
    if (Packed && !BytesAt(PointerAt(Pos + 0x18), Packed, Stack)) return false;
    if (CodeLen && !BytesAt(PointerAt(Pos + 0x20), CodeLen, Code)) return false;
    Out.Bytes = Scripts::GscBin(Packed, Len, Stack, Code);
    Out.Compiled = true;
    return true;
}

void ZonekitZone::ExportsScriptFiles(const Scripts::Vm& Vm)
{
    Exports(DiskGame::Kind::Script, [this, Vm](const DiskGame::Context& C, DiskGame::Totals& T) {
        ExportScripts(C, T, Vm, { { "scriptfile", [this](int64_t Pos, Scripts::Script& S) { return ReadScriptFile(Pos, S); } } });
    });
}

std::string ZonekitZone::NamedAsset(const std::string& Type, int64_t Pos) const
{
    using DiskGame::Kind;
    const std::string T = ExportNaming::Lower(Type);
    for (const auto& [K, Own] : { std::pair<Kind, const char*>{ Kind::Model, ModelType() }, { Kind::Anim, AnimType() } })
        if (T == Own)
        {
            const std::string Name = AssetName(K, Pos);
            return IsReference(K, Name, Pos) ? Referenced(K, Name) : Name;
        }
    const uint64_t H = At<uint64_t>(Image(), Pos) & HashBits();
    if (H == 0) return std::string();
    std::string N = HashName(H);
    if (N.empty()) N = "hash_" + std::format("{:x}", (unsigned long long)H);
    return N;
}

void ZonekitZone::ExportsGdt()
{
    Exports(DiskGame::Kind::Gdt, [this](const DiskGame::Context& C, DiskGame::Totals& T) { ExportGdt(C, T); });
}

std::vector<std::shared_ptr<const Gdt::Type>> ZonekitZone::GdtTypes(std::string& Error) const
{
    // a profile's gdt types, read once: every <profile>/gdt/<type>/ holding a type.jsonc
    using Types = std::vector<std::shared_ptr<const Gdt::Type>>;
    static std::mutex Lock;
    static std::map<std::string, std::pair<Types, std::string>> Loads;
    std::lock_guard<std::mutex> Guard(Lock);
    const std::string& Dir = Loaded->Profile.Dir;
    auto It = Loads.find(Dir);
    if (It == Loads.end())
    {
        std::string Failed;
        Types Read;
        std::vector<fs::path> Folders;
        std::error_code Ec;
        for (const auto& E : fs::directory_iterator(fs::path(Dir) / "gdt", Ec))
            if (E.is_directory() && fs::exists(E.path() / "type.jsonc")) Folders.push_back(E.path());
        std::sort(Folders.begin(), Folders.end());
        for (const auto& F : Folders)
        {
            auto One = Gdt::Type::Load(F.string(), fs::path(Dir).parent_path().string(), Loaded->Profile.TypeNames, Failed);
            if (!One) break;
            Read.push_back(std::move(One));
        }
        if (Read.empty() && Failed.empty()) Failed = "no gdt/<type>/type.jsonc in " + Dir;
        if (!Failed.empty()) Read.clear();
        It = Loads.emplace(Dir, std::make_pair(std::move(Read), Failed)).first;
    }
    Error = It->second.second;
    return It->second.first;
}

Gdt::Memory ZonekitZone::GdtMemory() const
{
    Gdt::Memory M;
    M.Image = &Image();
    M.Off = [this](uint64_t A) { return Off(A); };
    M.AssetName = [this](const std::string& K, int64_t Pos) { return NamedAsset(K, Pos); };
    M.HashName = [this](uint64_t H) { return HashName(H); };
    M.String = [this](uint64_t A) { return StringAt(A); };
    M.ScriptString = [this](uint32_t I) { return I < Slots.size() ? Slots[I] : std::string(); };
    M.AliasName = [this](uint32_t H) { return AliasName(H); };
    M.HashMask = HashBits();
    return M;
}

bool ZonekitZone::ForEachGdtAsset(const std::string& Id, const std::function<void(const Gdt::Type&, const Gdt::Memory&, int64_t)>& Visit,
                                  std::string& Error) const
{
    const auto Types = GdtTypes(Error);
    const auto It = std::find_if(Types.begin(), Types.end(), [&](const auto& G) { return G->Id() == Id; });
    if (It == Types.end())
    {
        if (Error.empty()) Error = "no gdt type " + Id;
        return false;
    }
    const Gdt::Memory M = GdtMemory();
    for (const auto& I : R.Instances)
        if (I.Parent < 0 && OwnerIs(I, (*It)->ZoneType().c_str())) Visit(**It, M, I.Pos);
    return true;
}

void ZonekitZone::ExportGdt(const DiskGame::Context& C, DiskGame::Totals& T)
{
    using DiskGame::Kind;
    std::string Error;
    const auto Own = GdtTypes(Error);
    if (Own.empty())
    {
        T.Fail(ZoneName, Kind::Gdt, "", Error);
        return;
    }
    const Gdt::Memory M = GdtMemory();

    auto& Kt = T.Kinds[Kind::Gdt];
    Gdt::Stats S;
    std::vector<std::string> Blocks;
    std::vector<std::pair<const Gdt::Type*, int64_t>> Found;
    for (const auto& Ty : Own)
    {
        // the zone-level assets of its type, those loaded inline in another (Nested), and those
        // another type's assets hold in a pointer array (FoundIn: BO3's attachment uniques in a weapon)
        for (const auto& I : R.Instances)
            if ((I.Parent < 0 || Ty->Nested()) && OwnerIs(I, Ty->ZoneType().c_str())) Found.emplace_back(Ty.get(), I.Pos);
        for (const auto& In : Ty->FoundIn())
            for (const auto& I : R.Instances)
            {
                if (I.Parent >= 0 || !OwnerIs(I, In.ZoneType.c_str())) continue;
                const int64_t List = Off(PointerAt(I.Pos + In.Pointers));
                for (int K = 0; List >= 0 && K < In.Count; K++)
                    if (const int64_t A = Off(PointerAt(List + 8 * (int64_t)K)); A >= 0) Found.emplace_back(Ty.get(), A);
            }
    }
    std::set<std::pair<std::string, std::string>> Names;   // (type, name): an asset once
    for (size_t N = 0; N < Found.size(); N++)
    {
        const auto& [Ty, Pos] = Found[N];
        const std::string Name = Ty->Name(M, Pos);
        if (!C.Advance(N, Found.size(), Name)) break;
        if (Name.empty())
        {
            Counter("gdt_" + Ty->Id() + "_unnamed")++;
            continue;
        }
        if (Name[0] == ',')
        {
            Kt.References++;   // a name-only header: defined in another zone
            continue;
        }
        if (!C.Wants(Kind::Gdt, Name)) continue;
        if (!Names.emplace(Ty->Id(), ExportNaming::Lower(Name)).second)
        {
            Counter("gdt_" + Ty->Id() + "_twice")++;
            continue;
        }
        std::string Block = Ty->Convert(M, Pos, Name, S);
        if (Block.empty() && S.Files.empty())
        {
            Kt.References++;   // a name-only header: not followed into the zone defining it
            continue;
        }
        Kt.Seen++;
        Counter("gdt_" + Ty->Id())++;
        if (!Block.empty()) Blocks.push_back(std::move(Block));
        // the files the converter writes beside the .gdt, under the gdt folder (never outside it)
        for (const auto& [Path, Bytes] : S.Files)
        {
            const fs::path Rel = fs::path(Path).lexically_normal();
            const bool Inside = !Path.empty() && !Rel.has_root_path() && *Rel.begin() != "..";
            if (Inside && ExportWriters::WriteBytes(fs::path(C.KindDir(Kind::Gdt)) / Rel, reinterpret_cast<const uint8_t*>(Bytes.data()), Bytes.size()))
                Kt.Written++;
            else T.Fail(ZoneName, Kind::Gdt, Path, Inside ? "write failed" : "path outside the gdt folder");
        }
        S.Files.clear();
    }
    C.Advance(Found.size(), Found.size());
    Counter("gdt_entries_filled") += S.Entries;
    Counter("gdt_entries_animations") += S.Animations;
    for (const auto& [E, N] : S.Clamped) Counter("gdt_clamped_" + E) += N;
    for (const auto& [E, N] : S.NotAChoice) Counter("gdt_not_a_choice_" + E) += N;
    for (const auto& [E, N] : S.Counters) Counter("gdt_" + E) += N;
    Counter("gdt_hashes") += S.Hashes;
    Counter("gdt_hashes_named") += S.HashesNamed;
    if (Blocks.empty()) return;
    const std::string Text = Gdt::Document(Blocks);
    const fs::path File = fs::path(C.KindDir(Kind::Gdt)) / (ExportNaming::SafeFileName(ZoneName) + ".gdt");
    if (ExportWriters::WriteBytes(File, reinterpret_cast<const uint8_t*>(Text.data()), Text.size())) Kt.Written += Blocks.size();
    else T.Fail(ZoneName, Kind::Gdt, ZoneName, "write failed");
}

void ZonekitZone::ExportEffects(const DiskGame::Context& C, DiskGame::Totals& T, const char* Type, const EffectReader& Read)
{
    using DiskGame::Kind;
    auto& Kt = T.Kinds[Kind::Fx];
    std::vector<int64_t> Found;
    for (const auto& I : R.Instances)
        if (I.Parent < 0 && OwnerIs(I, Type)) Found.push_back(I.Pos);
    const fs::path Dir = C.KindDir(Kind::Fx);
    std::set<std::string> Written;
    for (size_t N = 0; N < Found.size(); N++)
    {
        const std::string Name = AssetName(Kind::Fx, Found[N]);
        if (!C.Advance(N, Found.size(), Name)) break;
        if (IsReference(Kind::Fx, Name, Found[N]))
        {
            Kt.References++;
            continue;
        }
        if (!C.Wants(Kind::Fx, Name) || !Written.insert(ExportNaming::Lower(Name)).second) continue;
        Kt.Seen++;
        std::string Json;
        if (!Read(Found[N], Name, Json))
        {
            T.Fail(ZoneName, Kind::Fx, Name, "elements not read");
            continue;
        }
        // fx/explosions/fx_exp_grenade: its folders kept, each part a safe file name
        fs::path File = Dir;
        std::string Part;
        for (char Ch : Name + "/")
            if (Ch == '/' || Ch == '\\')
            {
                if (!Part.empty() && Part != "." && Part != "..") File /= ExportNaming::SafeFileName(Part);
                Part.clear();
            }
            else Part += Ch;
        File += ".json";
        if (ExportWriters::WriteBytes(File, reinterpret_cast<const uint8_t*>(Json.data()), Json.size())) Kt.Written++;
        else T.Fail(ZoneName, Kind::Fx, Name, "write failed");
    }
    C.Advance(Found.size(), Found.size());
}

void ZonekitZone::Defines(std::vector<DiskGame::AssetId>& Out) const
{
    std::vector<std::pair<DiskGame::AssetId, int64_t>> At;
    DefinesAt(At);
    Out.reserve(Out.size() + At.size());
    for (auto& [Id, Pos] : At) Out.push_back(std::move(Id));
}

void ZonekitZone::Weighs(std::vector<std::pair<DiskGame::AssetId, DiskGame::Weight>>& Out) const
{
    std::vector<std::pair<DiskGame::AssetId, int64_t>> At;
    DefinesAt(At);
    // the instance at each position, and the types of the assets listed
    std::unordered_map<int64_t, size_t> InstanceAt;
    for (size_t I = 0; I < R.Instances.size(); I++) InstanceAt.try_emplace(R.Instances[I].Pos, I);
    std::vector<bool> Listed(R.Instances.size()), KindType(R.OwnerNames.size());
    for (const auto& [Id, Pos] : At)
        if (const auto It = InstanceAt.find(Pos); It != InstanceAt.end())
        {
            Listed[It->second] = true;
            KindType[R.Instances[It->second].Owner] = true;
        }
    const auto ReadBytes = [&](const ZoneTables::ZoneResult::Instance& I) {
        uint64_t N = 0;
        for (uint32_t K = I.FirstRead; K < I.EndRead && K < R.Reads.size(); K++) N += (uint64_t)std::llabs(R.Reads[K]);
        return N;
    };
    std::vector<uint64_t> Bytes(R.Instances.size());
    for (size_t I = 0; I < R.Instances.size(); I++)
        if (Listed[I]) Bytes[I] = ReadBytes(R.Instances[I]);
    // an instance of a listed type inside another: its bytes are its own (weighed, or another
    // zone's asset), not the one holding it; taken off the nearest such instance above it, when
    // that one is listed (one deeper is inside the nearer one's already)
    for (size_t J = 0; J < R.Instances.size(); J++)
    {
        const auto& Inner = R.Instances[J];
        if (Inner.Parent < 0 || !KindType[Inner.Owner]) continue;
        int32_t Up = Inner.Parent;
        while (Up >= 0 && !KindType[R.Instances[Up].Owner]) Up = R.Instances[Up].Parent;
        if (Up >= 0 && Listed[Up]) Bytes[Up] -= std::min(Bytes[Up], ReadBytes(Inner));
    }
    Out.reserve(Out.size() + At.size());
    for (auto& [Id, Pos] : At)
    {
        DiskGame::Weight W;
        if (const auto It = InstanceAt.find(Pos); It != InstanceAt.end()) W.Bytes = Bytes[It->second];
        Weigh(Id.K, Pos, W);
        Out.push_back({ std::move(Id), W });
    }
}

void ZonekitZone::DefinesAt(std::vector<std::pair<DiskGame::AssetId, int64_t>>& Out) const
{
    using DiskGame::Kind;
    for (const auto& I : R.Instances)
        for (const auto& [K, Type] : { std::pair<Kind, const char*>{ Kind::Model, ModelType() }, { Kind::Anim, AnimType() } })
        {
            if (!OwnerIs(I, Type)) continue;
            std::string Name = AssetName(K, I.Pos);
            if (!IsReference(K, Name, I.Pos) && !Unwritable(K, I.Pos)) Out.push_back({ { K, std::move(Name) }, I.Pos });
        }
    // the materials it defines, for a game that exports them (each name once)
    std::set<std::string> Materials;
    if (MaterialType)
        for (const auto& I : R.Instances)
        {
            if (!OwnerIs(I, MaterialType)) continue;   // inline in an effect or a model too
            std::string Name = AssetName(Kind::Material, I.Pos);
            if (!Name.empty() && !IsReference(Kind::Material, Name, I.Pos) && Materials.insert(ExportNaming::Lower(Name)).second)
                Out.push_back({ { Kind::Material, std::move(Name) }, I.Pos });
        }
}

std::string ZonekitZone::IndexKey() const
{
    // an index made before the zones listed their materials is made again
    return ProfileKey(*Loaded) + (MaterialType ? "+materials2" : "");
}

void ZonekitZone::ExportMaterialImages(std::set<std::string> Images, const DiskGame::Context& C, DiskGame::Totals& T)
{
    for (auto It = Images.begin(); It != Images.end();)
        It = It->empty() || (*It)[0] == '$' ? Images.erase(It) : std::next(It);
    if (!Images.empty() && ExporterOf(DiskGame::Kind::Image) != nullptr) ExportTextures(Images, C, T);
}

void ZonekitZone::ExportsMaterials(const char* Type, MaterialReader Read, bool Indexed)
{
    if (Indexed) MaterialType = Type;
    Exports(DiskGame::Kind::Material, [this, Type, Read = std::move(Read)](const DiskGame::Context& C, DiskGame::Totals& T) {
        using DiskGame::Kind;
        auto& Kt = T.Kinds[Kind::Material];
        std::vector<std::pair<int64_t, std::string>> Found;
        std::set<std::string> Seen, ReferencedHere;
        for (const auto& I : R.Instances)
        {
            if (!OwnerIs(I, Type)) continue;   // inline in an effect or a model too
            std::string Name = AssetName(Kind::Material, I.Pos);
            if (IsReference(Kind::Material, Name, I.Pos))
            {
                Name = Referenced(Kind::Material, Name);
                if (!Name.empty() && C.Wants(Kind::Material, Name)) ReferencedHere.insert(Name);
                continue;
            }
            if (Name.empty() || !C.Wants(Kind::Material, Name) || !Seen.insert(ExportNaming::Lower(Name)).second) continue;
            Found.emplace_back(I.Pos, std::move(Name));
        }
        for (const auto& N : ReferencedHere)
            if (!Seen.count(ExportNaming::Lower(N)))
            {
                Kt.References++;
                C.Needs(Kind::Material, N);
            }
        const fs::path Dir = C.KindDir(Kind::Material);
        std::set<std::string> Images;
        for (size_t N = 0; N < Found.size(); N++)
        {
            const auto& [Pos, Name] = Found[N];
            if (!C.Advance(N, Found.size(), Name)) break;
            Kt.Seen++;
            std::string Json;
            if (!Read(Name, Live().Addr((uint64_t)Pos), Json, Images))
            {
                T.Fail(ZoneName, Kind::Material, Name, "not read");
                continue;
            }
            // ei/gfx_fire: its folders kept, each part a safe file name
            fs::path File = Dir;
            std::string Part;
            for (char Ch : Name + "/")
                if (Ch == '/' || Ch == '\\')
                {
                    if (!Part.empty() && Part != "." && Part != "..") File /= ExportNaming::SafeFileName(Part);
                    Part.clear();
                }
                else Part += Ch;
            File += ".json";
            if (ExportWriters::WriteBytes(File, reinterpret_cast<const uint8_t*>(Json.data()), Json.size())) Kt.Written++;
            else T.Fail(ZoneName, Kind::Material, Name, "write failed");
        }
        C.Advance(Found.size(), Found.size());
        // their images, as a model's textures: this zone's, the others needed from where they are
        ExportMaterialImages(std::move(Images), C, T);
    });
}

std::string ProfileKey(const ExportWriters::LoadedProfile& Loaded)
{
    // once per profile loaded (its tables read and hashed whole: Black Ops 7's zones beside asked it
    // 90 times, 2.2 s)
    static std::mutex Lock;
    static std::map<const ExportWriters::LoadedProfile*, std::string> Known;
    {
        std::lock_guard<std::mutex> G(Lock);
        if (const auto It = Known.find(&Loaded); It != Known.end()) return It->second;
    }
    // what the walk and so the index depend on: the profile, its tables and its stream layout
    uint64_t H = 0xcbf29ce484222325ull;
    const auto Mix = [&H](const std::string& S) { H = ExportNaming::Fnv1a(S, H); };
    const auto& P = Loaded.Profile;
    Mix(P.Name);
    std::vector<std::string> Files = P.TablePaths;
    for (const char* F : { "profile.json", "types.json", "blocks.txt", "empties.txt" }) Files.push_back((fs::path(P.Dir) / F).string());
    for (const auto& F : Files)
    {
        std::ifstream In(F, std::ios::binary);
        Mix(fs::path(F).filename().string() + "\n" + std::string(std::istreambuf_iterator<char>(In), std::istreambuf_iterator<char>()));
    }
    char B[40];
    snprintf(B, sizeof(B), "%s_%016llx", P.Name.c_str(), (unsigned long long)H);
    std::lock_guard<std::mutex> G(Lock);
    return Known.emplace(&Loaded, B).first->second;
}

std::string ProfileWalk::String(ByteView Zone, int64_t Field) const
{
    const auto It = Links.find(Field);
    if (It == Links.end()) return std::string();
    std::string S;
    for (int64_t Off = It->second; Off >= 0 && Off < (int64_t)Zone.size() && Zone[(size_t)Off]; Off++)
        S.push_back((char)Zone[(size_t)Off]);
    return S;
}

bool WalkWithProfile(const std::string& FastFilePath, ByteView Zone, ProfileWalk& Out,
                     std::string& Error)
{
    Out = ProfileWalk();
    const DiskGame::IDiskGame* Game = DiskGame::Detect(FastFilePath);
    const auto Unopened = Game ? Game->Unopened() : nullptr;
    const auto* Zk = dynamic_cast<const ZonekitZone*>(Unopened.get());
    if (Zk == nullptr)
    {
        Error = Game ? std::string(Game->Id()) + " is not read by zonekit tables" : "no game reads this fast file";
        return false;
    }
    Out.Profile = Zk->ProfileUsed();
    const ExportWriters::LoadedProfile* Loaded = ExportWriters::ZoneProfile(Out.Profile, Error);
    if (Loaded == nullptr) return false;
    Out.PointerSize = Loaded->Profile.Rules.PointerSize;
    Out.R = ZoneTables::WalkZone(Zone, Loaded->Tables, Loaded->Profile.TypeNames, Loaded->Profile.Rules, true);
    if (Out.R.ParseFailed)
    {
        Error = Out.R.Why;
        return false;
    }
    for (const auto& I : Out.R.Instances)
        if (I.Parent < 0) Out.Reached++;
    // the walk stops before an asset it has no table for, or after the end of the last one; any
    // other ending is an asset that raised, entered but not read whole
    Out.Complete = Out.R.Why == "walked to the end";
    const bool Raised = !Out.Complete && Out.R.Why.rfind("stopped ", 0) != 0 && Out.R.Why.rfind("no table for ", 0) != 0;
    Out.Walked = Out.Reached - (Raised && Out.Reached ? 1 : 0);
    Out.Links.reserve(Out.R.Links.size());
    for (const auto& [Field, Data] : Out.R.Links) Out.Links.emplace(Field, Data);
    return true;
}

uint64_t ZonekitZone::Footprint() const
{
    // the zone, its served copy and the walk
    return Bytes.size() * 2 + R.Instances.size() * sizeof(ZoneTables::ZoneResult::Instance) + R.Reads.size() * 16 +
           (R.Links.size() + R.BackRefs.size()) * 16;
}
