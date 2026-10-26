#include "stdafx.h"

#include "IwDiskGame.h"

#include "EfxEffects.h"
#include "IwAssetFiles.h"
#include "IwBsp.h"
#include "IwShaders.h"
#include "IwInstall.h"

#include "CoDAssets.h"
#include "CoDAssetType.h"
#include "CoDIWIDecoder.h"
#include "ExportNaming.h"
#include "ExportWriters.h"
#include "Games/Common/WeaponFiles.h"
#include "Games/IwClassic/IwClassicAnimOwners.h"
#include "MapSceneExport.h"
#include "MaterialDescription.h"
#include "ScriptExport.h"
#include "SettingsManager.h"
#include "WraithAnim.h"
#include "WraithModel.h"

#include "DirectXTex.h"

#include <nlohmann/json.hpp>

#include <array>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <mutex>
#include <set>
#include <sstream>
#include <tuple>

namespace fs = std::filesystem;

namespace IW12
{
namespace
{
    using DiskGame::Context;
    using DiskGame::Kind;
    using DiskGame::Totals;

    using V3 = std::array<float, 3>;
    using Q4 = std::array<float, 4>;   // x y z w

    Q4 Mul(const Q4& A, const Q4& B)
    {
        return { A[3] * B[0] + A[0] * B[3] + A[1] * B[2] - A[2] * B[1],
                 A[3] * B[1] - A[0] * B[2] + A[1] * B[3] + A[2] * B[0],
                 A[3] * B[2] + A[0] * B[1] - A[1] * B[0] + A[2] * B[3],
                 A[3] * B[3] - A[0] * B[0] - A[1] * B[1] - A[2] * B[2] };
    }
    V3 Rotate(const Q4& Q, const V3& V)
    {
        const Q4 P = Mul(Mul(Q, { V[0], V[1], V[2], 0 }), { -Q[0], -Q[1], -Q[2], Q[3] });
        return { P[0], P[1], P[2] };
    }
    V3 Add(const V3& A, const V3& B) { return { A[0] + B[0], A[1] + B[1], A[2] + B[2] }; }
    float Dist(const V3& A, const V3& B)
    {
        const float X = A[0] - B[0], Y = A[1] - B[1], Z = A[2] - B[2];
        return std::sqrt(X * X + Y * Y + Z * Z);
    }

    std::string Extension(const std::string& Path)
    {
        const size_t Slash = Path.rfind('/'), Dot = Path.rfind('.');
        return Dot == std::string::npos || (Slash != std::string::npos && Dot < Slash) ? std::string() : Path.substr(Dot);
    }
    std::string WithoutExtension(const std::string& Path)
    {
        const std::string E = Extension(Path);
        return Path.substr(0, Path.size() - E.size());
    }
    bool IsImage(const std::string& Ext) { return Ext == ".iwi" || Ext == ".dds" || Ext == ".tga" || Ext == ".jpg"; }

    // The asset a file is, by kind and name (paths are lower case): models and animations by file
    // name, images by path without extension (images/ dropped: CoD2's folder of .iwi), maps by
    // file name (maps/mp/mp_harbor.bsp: mp_harbor, as the console's map command names it; no two
    // maps of either install share one), sounds by path under sound/ without extension.
    bool AssetOf(const std::string& Path, Kind& K, std::string& Name)
    {
        const std::string Ext = Extension(Path);
        const auto Directly = [&](const char* Folder) {
            const size_t N = std::strlen(Folder);
            return Path.size() > N && Path.compare(0, N, Folder) == 0 && Path.find('/', N) == std::string::npos;
        };
        if (Directly("xmodel/")) { K = Kind::Model; Name = Path.substr(7); return true; }
        if (Directly("xanim/")) { K = Kind::Anim; Name = Path.substr(6); return true; }
        // weapons/mp/<name>, weapons/sp/<name>: the weapon files (no two share a name)
        if (Path.compare(0, 8, "weapons/") == 0 && Ext.empty())
            if (const size_t Slash = Path.find('/', 8); Slash != std::string::npos && Slash + 1 < Path.size() &&
                                                         Path.find('/', Slash + 1) == std::string::npos)
            {
                K = Kind::Weapons;
                Name = Path.substr(Slash + 1);
                return true;
            }
        // CoD2's materials/ holds material files, some named after CoD1's textures (x.dds, x.tga)
        const bool MaterialFolder = Path.compare(0, 10, "materials/") == 0 || Path.compare(0, 14, "materials_dx7/") == 0;
        if (IsImage(Ext) && !MaterialFolder)
        {
            K = Kind::Image;
            Name = WithoutExtension(Path.compare(0, 7, "images/") == 0 ? Path.substr(7) : Path);
            return true;
        }
        if ((Ext == ".bsp" || Ext == ".d3dbsp") && Path.compare(0, 5, "maps/") == 0)
        {
            K = Kind::Map;
            Name = WithoutExtension(fs::path(Path).filename().string());
            return true;
        }
        if ((Ext == ".wav" || Ext == ".mp3") && Path.compare(0, 6, "sound/") == 0)
        {
            K = Kind::Sound;
            Name = WithoutExtension(Path.substr(6));
            return true;
        }
        // effects: fx/<name>.efx, by the name a runner gives (explosions/grenade_flash)
        if (Ext == ".efx" && Path.compare(0, 3, "fx/") == 0)
        {
            K = Kind::Fx;
            Name = Efx::EffectName(Path);
            return true;
        }
        // scripts: source, maps/ and the game's script folders
        if (Scripts::IsSourceName(Path))
        {
            K = Kind::Script;
            Name = Path;
            return true;
        }
        return false;
    }

    // A TGA DirectXTex refuses (CoD2's run-length fonts): true colour or grey, raw or run-length,
    // as RGBA rows from the top.
    bool DecodeTga(const std::vector<uint8_t>& B, DirectX::ScratchImage& Img, DirectX::TexMetadata& Meta)
    {
        if (B.size() < 18) return false;
        const uint8_t Type = B[2], Bpp = B[16], Desc = B[17];
        const size_t W = B[12] | (B[13] << 8), H = B[14] | (B[15] << 8);
        const bool Grey = Type == 3 || Type == 11, Rle = Type == 10 || Type == 11;
        if ((Type != 2 && Type != 3 && Type != 10 && Type != 11) || B[1] != 0 || W == 0 || H == 0) return false;
        const size_t Px = Bpp / 8;
        if (Grey ? Px != 1 : (Px != 3 && Px != 4)) return false;
        if (FAILED(Img.Initialize2D(DXGI_FORMAT_R8G8B8A8_UNORM, W, H, 1, 1))) return false;
        Meta = Img.GetMetadata();
        uint8_t* Out = Img.GetImages()[0].pixels;
        const size_t Pitch = Img.GetImages()[0].rowPitch;
        size_t At = 18 + B[0];
        std::vector<uint8_t> Pixels;
        Pixels.reserve(W * H * Px);
        while (Pixels.size() < W * H * Px)
        {
            size_t Run = 1;
            bool Repeat = false;
            if (Rle)
            {
                if (At >= B.size()) return false;
                Repeat = (B[At] & 0x80) != 0;
                Run = (B[At++] & 0x7F) + 1;
            }
            const size_t Bytes = Repeat ? Px : Run * Px;
            if (At + Bytes > B.size()) return false;
            for (size_t K = 0; K < Run; K++)
                Pixels.insert(Pixels.end(), B.begin() + At + (Repeat ? 0 : K * Px), B.begin() + At + (Repeat ? 0 : K * Px) + Px);
            At += Bytes;
        }
        const bool TopFirst = (Desc & 0x20) != 0;
        for (size_t Y = 0; Y < H; Y++)
        {
            const uint8_t* Row = Pixels.data() + (TopFirst ? Y : H - 1 - Y) * W * Px;
            uint8_t* Dst = Out + Y * Pitch;
            for (size_t X = 0; X < W; X++, Row += Px, Dst += 4)
            {
                if (Grey) Dst[0] = Dst[1] = Dst[2] = Row[0];
                else
                {
                    Dst[0] = Row[2];
                    Dst[1] = Row[1];
                    Dst[2] = Row[0];
                }
                Dst[3] = Px == 4 ? Row[3] : 255;
            }
        }
        return true;
    }

    // An image file as a DDS in memory: IWI through CoDIWIDecoder (CoD2's wavelet formats too), DDS
    // as it is, TGA and JPG decoded and stored uncompressed.
    std::unique_ptr<XImageDDS> DdsOf(const std::string& Ext, const std::vector<uint8_t>& Bytes, std::string& Error)
    {
        std::string Dds;
        if (Ext == ".iwi")
        {
            if (Bytes.size() >= 16)
            {
                uint32_t FileSize = 0;
                std::memcpy(&FileSize, Bytes.data() + 12, 4);
                if (Bytes[3] == 5 && FileSize != Bytes.size())
                {
                    Error = "header file size " + std::to_string(FileSize) + " of " + std::to_string(Bytes.size());
                    return nullptr;
                }
            }
            if (!CoDIWIDecoder::ToDds(Bytes.data(), Bytes.size(), Dds, Error)) return nullptr;
        }
        else if (Ext == ".dds")
        {
            if (Bytes.size() < 128 || std::memcmp(Bytes.data(), "DDS ", 4) != 0)
            {
                Error = "not a DDS";
                return nullptr;
            }
            Dds.assign(reinterpret_cast<const char*>(Bytes.data()), Bytes.size());
        }
        else
        {
            static thread_local bool Com = SUCCEEDED(CoInitializeEx(nullptr, COINIT_MULTITHREADED));
            (void)Com;
            DirectX::ScratchImage Img;
            DirectX::TexMetadata Meta;
            HRESULT Hr = Ext == ".tga" ? DirectX::LoadFromTGAMemory(Bytes.data(), Bytes.size(), &Meta, Img)
                                       : DirectX::LoadFromWICMemory(Bytes.data(), Bytes.size(), DirectX::WIC_FLAGS_NONE, &Meta, Img);
            if (FAILED(Hr) && Ext == ".tga" && DecodeTga(Bytes, Img, Meta)) Hr = S_OK;
            DirectX::Blob Blob;
            if (FAILED(Hr) || FAILED(DirectX::SaveToDDSMemory(Img.GetImages(), Img.GetImageCount(), Meta, DirectX::DDS_FLAGS_NONE, Blob)))
            {
                Error = "does not decode";
                return nullptr;
            }
            Dds.assign(reinterpret_cast<const char*>(Blob.GetBufferPointer()), Blob.GetBufferSize());
        }
        auto Out = std::make_unique<XImageDDS>();
        Out->DataBuffer = new int8_t[Dds.size()];
        std::memcpy(Out->DataBuffer, Dds.data(), Dds.size());
        Out->DataSize = (uint32_t)Dds.size();
        return Out;
    }

    // The translations every animation of an install gives each bone, for skeletons stored without
    // positions (first-person hands and weapons: the game takes them from the animations).
    class BonePositions
    {
    public:
        static const BonePositions& Of(const Install& I)
        {
            static std::mutex Lock;
            static std::map<const Install*, std::unique_ptr<BonePositions>> All;
            std::lock_guard<std::mutex> Guard(Lock);
            auto& P = All[&I];
            if (!P) P.reset(new BonePositions(I));
            return *P;
        }
        // For bones Names: the translation most keys give, over the animations that translate at
        // least 90% of them. Found[i] false when none does.
        void Find(const std::vector<std::string>& Names, std::vector<V3>& Out, std::vector<bool>& Found, size_t& Used) const
        {
            using Key = std::tuple<int, int, int>;
            std::vector<std::map<Key, std::pair<size_t, V3>>> Votes(Names.size());
            Used = 0;
            for (const auto& A : Anims)
            {
                size_t Have = 0;
                for (const auto& N : Names) Have += A.count(N);
                if (Names.empty() || Have * 10 < Names.size() * 9) continue;
                Used++;
                for (size_t B = 0; B < Names.size(); B++)
                {
                    const auto It = A.find(Names[B]);
                    if (It == A.end()) continue;
                    for (const V3& T : It->second)
                    {
                        auto& Vote = Votes[B][{ (int)std::lround(T[0] * 1000), (int)std::lround(T[1] * 1000), (int)std::lround(T[2] * 1000) }];
                        if (Vote.first++ == 0) Vote.second = T;
                    }
                }
            }
            Out.assign(Names.size(), { 0, 0, 0 });
            Found.assign(Names.size(), false);
            for (size_t B = 0; B < Names.size(); B++)
            {
                size_t Best = 0;
                for (const auto& [K, V] : Votes[B])
                    if (V.first > Best)
                    {
                        Best = V.first;
                        Out[B] = V.second;
                        Found[B] = true;
                    }
            }
        }

    private:
        explicit BonePositions(const Install& I)
        {
            std::vector<uint8_t> Bytes;
            for (const auto& [Path, A] : I.Files())
            {
                Kind K;
                std::string Name;
                if (!AssetOf(Path, K, Name) || K != Kind::Anim || !I.Read(Path, Bytes)) continue;
                XAnimFile F;
                std::string Error;
                if (!ParseXAnim(Bytes.data(), Bytes.size(), F, Error)) continue;
                std::map<std::string, std::vector<V3>> Bones;
                for (const auto& P : F.Parts)
                {
                    if (P.Translations.Keys.empty()) continue;
                    auto& Keys = Bones[ExportNaming::Lower(P.Name)];
                    for (const auto& T : P.Translations.Keys) Keys.push_back({ T[0], T[1], T[2] });
                }
                Anims.push_back(std::move(Bones));
            }
            // every skeleton stored without positions, placed: what the animations aimed at them hold
            for (const auto& [Path, A] : I.Files())
            {
                if (Path.compare(0, 12, "xmodelparts/") != 0 || !I.Read(Path, Bytes)) continue;
                XModelPartsFile F;
                std::string Error;
                if (!ParseXModelParts(Bytes.data(), Bytes.size(), F, Error) || !Positionless(F)) continue;
                std::vector<std::string> Names;
                for (size_t B = F.Roots; B < F.Bones.size(); B++) Names.push_back(ExportNaming::Lower(F.Bones[B].Name));
                std::vector<V3> Pos;
                std::vector<bool> Found;
                size_t Used = 0;
                Find(Names, Pos, Found, Used);
                for (size_t B = 0; B < Names.size(); B++)
                    if (Found[B] && std::sqrt(Pos[B][0] * Pos[B][0] + Pos[B][1] * Pos[B][1] + Pos[B][2] * Pos[B][2]) > 0.1f)
                        Placed[Names[B]].push_back(Pos[B]);
            }
        }
        std::vector<std::map<std::string, std::vector<V3>>> Anims;
        std::map<std::string, std::vector<V3>> Placed;   // bone -> the positions it was given

    public:
        // A skeleton whose bones (two or more besides the roots) are all at 0.
        static bool Positionless(const XModelPartsFile& Parts)
        {
            if (Parts.Bones.size() < Parts.Roots + 2u) return false;
            for (size_t B = Parts.Roots; B < Parts.Bones.size(); B++)
                for (float C : Parts.Bones[B].Position)
                    if (C != 0) return false;
            return true;
        }
        // An animation aimed at such a skeleton: its translations are the bones' positions, where the
        // others' are offsets from their skeleton's (frame 0 equal to a placed position for at least
        // 75% of its translated bones that were placed away from 0).
        bool Absolute(const XAnimFile& F) const
        {
            size_t Placeable = 0, Equal = 0;
            for (const auto& P : F.Parts)
            {
                if (P.Translations.Keys.empty()) continue;
                const auto It = Placed.find(ExportNaming::Lower(P.Name));
                if (It == Placed.end()) continue;
                Placeable++;
                const auto& K = P.Translations.Keys[0];
                for (const V3& At : It->second)
                    if (Dist(At, { K[0], K[1], K[2] }) < 0.01f)
                    {
                        Equal++;
                        break;
                    }
            }
            return Placeable > 0 && Equal * 4 >= Placeable * 3;
        }
    };

    // CoD1's shader scripts, read once per install (every scripts/<x>.shader the game sees, in
    // name order: the first definition of a shader wins), then the effects' (fxshaders/<x>.shader:
    // the shaders the .efx files name, gfx/effects/...) for the names the first left undefined.
    const ShaderScripts& ShadersOf(const Install& I)
    {
        static std::mutex Lock;
        static std::map<const Install*, std::unique_ptr<ShaderScripts>> All;
        std::lock_guard<std::mutex> Guard(Lock);
        auto& S = All[&I];
        if (!S)
        {
            S = std::make_unique<ShaderScripts>();
            std::vector<uint8_t> Bytes;
            for (const char* Folder : { "scripts/", "fxshaders/" })
                for (const auto& [Path, A] : I.Files())
                    if (Path.rfind(Folder, 0) == 0 && Extension(Path) == ".shader" && I.Read(Path, Bytes))
                    {
                        bool Ok;
                        S->Parse(std::string(Bytes.begin(), Bytes.end()), Ok);
                    }
        }
        return *S;
    }

    // CoD2's technique set names say how a material draws ("<lighting>_<blend>_..."): replace is
    // opaque, alphatest cut out, blend, add and multiply blended; without one (CoD1, or a model
    // without a material file), the colour map's alpha decides (auto), or the name says cut out.
    Materials::AlphaMode AlphaOf(const std::string& Techset, const std::string& Name)
    {
        const auto Has = [&](const char* Word) { return Techset.find(std::string("_") + Word) != std::string::npos; };
        if (Has("replace")) return Materials::AlphaMode::Opaque;
        if (Has("alphatest")) return Materials::AlphaMode::Cutout;
        if (Has("blend") || Has("add") || Has("multiply")) return Materials::AlphaMode::Blend;
        return Materials::LooksAlphaTested(Name) ? Materials::AlphaMode::Cutout : Materials::AlphaMode::Auto;
    }

    // CoD1/CoD2 materials as the one description (MaterialDescription.h): the roles the game gives
    // its images (CoD2's samplers; CoD1's one colour map), never the file names.
    class Adapter final : public Materials::IAdapter
    {
    public:
        const char* Name() const override { return "iw12"; }
        Materials::Description Describe(const Materials::Source& M) const override
        {
            Materials::Description D;
            D.Name = M.Name;
            for (const auto& I : M.Images)
                // a map's textures carry their role as their semantic (MapSemantic), its colour map
                // also as the hint
                switch (I.Hint != Materials::Role::Unknown || I.Semantic > (uint32_t)Materials::Role::Emissive
                            ? I.Hint : static_cast<Materials::Role>(I.Semantic))
                {
                case Materials::Role::Albedo: if (D.Albedo.Empty()) D.Albedo.Path = I.Path; break;
                case Materials::Role::Normal: if (D.Normal.Empty()) D.Normal.Path = I.Path; break;
                case Materials::Role::Specular:
                    if (!D.Specular.Empty()) break;
                    D.Specular.Path = I.Path;
                    // CoD2's converter names a packed image "~<rgb source>-rgb&<alpha source>-l...":
                    // a specular map's alpha is then its cosine power map, a gloss
                    if (I.Path.find("-rgb&") != std::string::npos) D.Roughness = { I.Path, 3, true };
                    break;
                default: break;
                }
            D.NormalEnc = Materials::NormalEncoding::AG;
            D.Alpha = AlphaOf(M.Techset, M.Name);
            return D;
        }
    };

    class ArchiveZone final : public DiskGame::IDiskZone
    {
    public:
        ArchiveZone(const GameRow& R, std::shared_ptr<Install> I, int A, const std::string& Language = "english")
            : Row(R), Files(std::move(I)), Archive(A), Lang(Language)
        {
            if (Files) ZoneName = fs::path(Files->ArchivePaths()[A]).filename().string();
            Exports(Kind::Model, [this](const Context& C, Totals& T) { ExportModels(C, T); });
            Exports(Kind::Anim, [this](const Context& C, Totals& T) { ExportAnims(C, T); });
            Exports(Kind::Image, [this](const Context& C, Totals& T) { ExportImages(C, T); });
            Exports(Kind::Sound, [this](const Context& C, Totals& T) { ExportSounds(C, T); });
            Exports(Kind::Map, [this](const Context& C, Totals& T) { ExportMaps(C, T); });
            Exports(Kind::Script, [this](const Context& C, Totals& T) { ExportScripts(C, T); });
            Exports(Kind::Localize, [this](const Context& C, Totals& T) { ExportLocalize(C, T); });
            Exports(Kind::Weapons, [this](const Context& C, Totals& T) { ExportWeapons(C, T); });
            Exports(Kind::WeaponStats, [this](const Context& C, Totals& T) { ExportWeaponStats(C, T); });
            Exports(Kind::Fx, [this](const Context& C, Totals& T) { ExportEffects(C, T); });
        }

        const std::string& Name() const override { return ZoneName; }
        bool WalkedToEnd() const override { return true; }
        void Measures(Totals& T) const override
        {
            for (const auto& [N, V] : Counters) T.Counters[N] += V;
        }
        void Defines(std::vector<DiskGame::AssetId>& Out) const override
        {
            for (const auto& [K, Path, Name] : Assets()) Out.push_back({ K, Name });
        }
        // The .str files' entries as level titles (LevelNames): a title quoted in its text left
        // without the quotes
        void Strings(std::vector<std::pair<std::string, std::string>>& Out) const override { Entries(Out, true); }

        // its .arena files, the map list
        void Documents(std::vector<std::pair<std::string, std::string>>& Out) const override
        {
            if (!Files) return;
            for (const auto& Path : Files->FilesOf(Archive))
            {
                std::vector<uint8_t> Bytes;
                if (Extension(Path) == ".arena" && Files->Read(Path, Bytes)) Out.emplace_back(Path, std::string(Bytes.begin(), Bytes.end()));
            }
        }
        // the maps it holds, by file name (mp_carentan)
        void HeldLevels(std::vector<std::string>& Out) const override
        {
            if (!Files) return;
            for (const auto& [K, Path, Name] : Assets())
                if (K == Kind::Map) Out.push_back(Name);
        }

    private:
        // The .str files of its localizedstrings folder: each REFERENCE as <FILE>_<REFERENCE>
        // (introscreen.str's DECOYTOWN_TITLE: INTROSCREEN_DECOYTOWN_TITLE), the text of the
        // language setting's LANG_ line, else LANG_ENGLISH's ("#same": the English text). Titles:
        // a text quoted whole (""Hold the Line"") without its quotes; else the text as the game
        // shows it, its "\n" a line break. Paths: those .str files instead (the whole install's).
        void Entries(std::vector<std::pair<std::string, std::string>>& Out, bool Titles,
                     const std::vector<std::string>* Paths = nullptr) const
        {
            if (!Files) return;
            const auto Upper = [](std::string S) {
                for (auto& C : S) C = (char)std::toupper((unsigned char)C);
                return S;
            };
            const std::string Wanted = "LANG_" + Upper(Lang);
            for (const auto& Path : Paths != nullptr ? *Paths : Files->FilesOf(Archive))
            {
                if (Path.compare(0, 17, "localizedstrings/") != 0 || Extension(Path) != ".str") continue;
                std::vector<uint8_t> Bytes;
                if (!Files->Read(Path, Bytes)) continue;
                const std::string File = Upper(fs::path(WithoutExtension(Path)).filename().string());
                std::string Reference, English, Own;
                const auto Flush = [&] {
                    std::string Text = Own.empty() || Own == "#same" ? English : Own;
                    if (Titles && Text.size() >= 2 && Text.front() == '"' && Text.back() == '"') Text = Text.substr(1, Text.size() - 2);
                    for (size_t At = 0; !Titles && (At = Text.find("\\n", At)) != std::string::npos; At++) Text.replace(At, 2, "\n");
                    if (!Reference.empty() && !Text.empty()) Out.emplace_back(File + "_" + Reference, Text);
                    Reference.clear(), English.clear(), Own.clear();
                };
                std::istringstream In(std::string(Bytes.begin(), Bytes.end()));
                for (std::string Line; std::getline(In, Line);)
                {
                    std::istringstream Words(Line);
                    std::string Word;
                    Words >> Word;
                    if (Word == "REFERENCE")
                    {
                        Flush();
                        Words >> Reference;
                        continue;
                    }
                    const size_t Open = Line.find('"'), Close = Line.rfind('"');
                    if (Word.compare(0, 5, "LANG_") != 0 || Open == std::string::npos || Close <= Open) continue;
                    if (Word == "LANG_ENGLISH") English = Line.substr(Open + 1, Close - Open - 1);
                    if (Word == Wanted) Own = Line.substr(Open + 1, Close - Open - 1);
                }
                Flush();
            }
        }
        // localize/<archive>.csv: its .str entries (Entries), a row per REFERENCE, in UTF-8 (the text
        // read in the code page of the archive's language, else the setting's)
        void ExportLocalize(const Context& C, Totals& T)
        {
            auto& Kt = T.Kinds[Kind::Localize];
            std::vector<std::pair<std::string, std::string>> All, Rows;
            Entries(All, false);
            for (auto& Row : All)
                if (C.Wants(Kind::Localize, Row.first)) Rows.push_back(std::move(Row));
            Kt.Seen += Rows.size();
            if (Rows.empty()) return;
            unsigned CodePage = ExportWriters::LanguageCodePage(ZoneName);
            if (CodePage == 0) CodePage = ExportWriters::LanguageCodePage(Lang);
            const std::string Csv = (fs::path(C.KindDir(Kind::Localize)) / (WithoutExtension(ZoneName) + ".csv")).string();
            if (ExportWriters::WriteLocalize(Csv, Rows, CodePage)) Kt.Written += Rows.size();
            else T.Fail(ZoneName, Kind::Localize, ZoneName, "write failed");
        }

        // weapons/<mp|sp>/<name>: the weapon files (WEAPONFILE\<field>\<value>...), as the archive
        // holds them, under weapons/<mp|sp>/
        void ExportWeapons(const Context& C, Totals& T)
        {
            auto& Kt = T.Kinds[Kind::Weapons];
            const auto List = Assets(Kind::Weapons, C);
            const fs::path Dir = C.KindDir(Kind::Weapons);
            for (size_t N = 0; N < List.size(); N++)
            {
                const auto& A = List[N];
                if (!C.Advance(N, List.size(), A.Name)) return;
                Kt.Seen++;
                std::vector<uint8_t> Bytes;
                if (!Files->Read(A.Path, Bytes)) T.Fail(ZoneName, Kind::Weapons, A.Name, "does not inflate");
                else if (ExportWriters::WriteBytes(Dir / A.Path.substr(8), Bytes.data(), Bytes.size())) Kt.Written++;
                else T.Fail(ZoneName, Kind::Weapons, A.Name, "write failed");
            }
            C.Advance(List.size(), List.size());
        }

        // The weapon files read into the table (Games/Common/WeaponFiles.h), their mode their folder;
        // the display names from the whole install's .str files (the weapon files are in one archive,
        // weapon.str in a localized one), in the language setting
        void ExportWeaponStats(const Context& C, Totals& T)
        {
            std::vector<WeaponFiles::Weapon> Weapons;
            std::set<std::string> Keys;
            for (const auto& A : Assets())
            {
                std::vector<uint8_t> Bytes;
                if (A.K != Kind::Weapons || !Files->Read(A.Path, Bytes)) continue;
                WeaponFiles::Weapon W{ A.Name, std::string(Bytes.begin(), Bytes.end()), A.Path.substr(8, A.Path.find('/', 8) - 8) };
                if (const auto F = WeaponFiles::Parse(W.Text); F.count("displayName")) Keys.insert(ExportNaming::Lower(F.at("displayName")));
                Weapons.push_back(std::move(W));
            }
            if (Weapons.empty()) return;
            std::vector<std::string> Strs;
            for (const auto& [Path, A] : Files->Files())
                if (Path.compare(0, 17, "localizedstrings/") == 0 && Extension(Path) == ".str") Strs.push_back(Path);
            std::vector<std::pair<std::string, std::string>> All, Strings;
            Entries(All, false, &Strs);
            const unsigned CodePage = ExportWriters::LanguageCodePage(Lang);
            for (auto& [K, V] : All)
                if (Keys.count(ExportNaming::Lower(K))) Strings.emplace_back(K, ExportWriters::Utf8(V, CodePage));
            WeaponFiles::Notes N{ Row.Title,
                                  "The display names are the localized strings of the install's .str files (localizedstrings/weapon.str's\n"
                                  "THOMPSON: WEAPON_THOMPSON), in the language setting (--language, default English).\n",
                                  "Each weapon is the game's own weapon file, weapons/mp/<name> or weapons/sp/<name> in its archives (the\n"
                                  "copy the game reads: the last archive by name holding it), WEAPONFILE\\<field>\\<value>...; the\n"
                                  "game parses it by its weapon field table, so each column below is one named field: times are\n"
                                  "seconds in the file (milliseconds here), distances inches.\n" };
            // CoD1 (game_mp_x86.dll, not packed): its field table's time type (7) is (int)(atof(s) x 1000),
            // truncated (0x200423a7: fmul 1000.0, _ftol), and semiAuto (0x2c4) a qboolean (type 5: atoi != 0)
            if (std::string(Row.Id) == "cod1")
            {
                N.TruncateMs = true;
                N.Extra = { { "semi_auto", "semiAuto", WeaponFiles::Unit::Count } };
                N.ExtraNotes =
                    "- Times (CoD1): the game's parser (game_mp_x86.dll 0x200423a7, the field table's time type) takes\n"
                    "  (int)(seconds x 1000), truncated: fireTime 0.0857 is 85 ms (the Thompson's 705.88 rpm). The\n"
                    "  other weapon file games' times are rounded.\n"
                    "- semi_auto: semiAuto (WeaponDef 0x2c4, a qboolean: atoi of the text, not 0 is 1), 1 for a weapon\n"
                    "  that fires once a pull.\n";
            }
            WeaponFiles::ExportStats(Weapons, Strings, ZoneName, C, T, N);
        }

        struct Asset
        {
            Kind K;
            std::string Path, Name;
        };
        std::vector<Asset> Assets() const
        {
            std::vector<Asset> Out;
            for (const auto& Path : Files->FilesOf(Archive))
            {
                Asset A{ Kind::Model, Path, {} };
                if (AssetOf(Path, A.K, A.Name)) Out.push_back(std::move(A));
            }
            return Out;
        }
        std::vector<Asset> Assets(Kind K, const Context& C) const
        {
            std::vector<Asset> Out;
            for (auto& A : Assets())
                if (A.K == K && C.Wants(K, A.Name)) Out.push_back(std::move(A));
            return Out;
        }
        size_t& Counter(const std::string& N) { return Counters[N]; }
        std::string Version(const char* What, uint16_t V) { return std::string(What) + "_v" + std::to_string(V); }

        // Images: one writer for the image kind and for models' textures
        struct ImageJob
        {
            std::string Path, Name;
        };
        // Writes Jobs under Dir (written and linked counted in Kt); the number that failed.
        size_t WriteImages(const std::vector<ImageJob>& Jobs, const std::string& Dir, const Context& C, Totals& T,
                           DiskGame::KindTotals& Kt)
        {
            ExportWriters::ImageSink Sink;
            std::vector<uint8_t> Bytes;
            size_t Failed = 0;
            const auto Decode = [&](size_t Tag) -> std::unique_ptr<XImageDDS> {
                std::string Error;
                if (!Files->Read(Jobs[Tag].Path, Bytes)) return nullptr;
                return DdsOf(Extension(Jobs[Tag].Path), Bytes, Error);
            };
            for (size_t N = 0; N < Jobs.size(); N++)
            {
                const auto& J = Jobs[N];
                if (!C.Advance(N, Jobs.size(), J.Name)) break;
                Kt.Seen++;
                const fs::path Out = fs::path(Dir) / (J.Name + Sink.Extension());
                std::error_code Ec;
                fs::create_directories(Out.parent_path(), Ec);
                if (Sink.LinkIfKnown(ExportNaming::Fnv1a(std::string(Row.Id) + ":" + J.Path), Out.string(), N)) continue;
                std::string Error;
                if (!Files->Read(J.Path, Bytes))
                {
                    T.Fail(ZoneName, Kind::Image, J.Name, "does not inflate");
                    Failed++;
                    continue;
                }
                auto Dds = DdsOf(Extension(J.Path), Bytes, Error);
                if (!Dds)
                {
                    T.Fail(ZoneName, Kind::Image, J.Name, Error);
                    Failed++;
                    continue;
                }
                if (Extension(J.Path) == ".iwi") Counter(Version("iwi_exact", Bytes[3]))++;
                Sink.Write(std::move(Dds), Out.string());
            }
            size_t Missing = 0;
            const auto S = Sink.Finish(Decode, Missing);
            Kt.Written += S.Written;
            Kt.Linked += S.Linked;
            if (S.Failed) T.Fail(ZoneName, Kind::Image, "", std::to_string(S.Failed) + " images not written");
            return Failed + S.Failed;
        }

        void ExportImages(const Context& C, Totals& T)
        {
            std::vector<ImageJob> Jobs;
            for (auto& A : Assets(Kind::Image, C)) Jobs.push_back({ A.Path, A.Name });
            WriteImages(Jobs, C.KindDir(Kind::Image), C, T, T.Kinds[Kind::Image]);
            C.Advance(Jobs.size(), Jobs.size());
        }

        // Sounds: the game's own files (PCM WAV, MP3), written as they are
        void ExportSounds(const Context& C, Totals& T)
        {
            auto& Kt = T.Kinds[Kind::Sound];
            const auto List = Assets(Kind::Sound, C);
            const std::string Dir = C.KindDir(Kind::Sound);
            std::vector<uint8_t> Bytes;
            for (size_t N = 0; N < List.size(); N++)
            {
                const auto& A = List[N];
                if (!C.Advance(N, List.size(), A.Name)) return;
                Kt.Seen++;
                if (!Files->Read(A.Path, Bytes))
                {
                    T.Fail(ZoneName, Kind::Sound, A.Name, "does not inflate");
                    continue;
                }
                const std::string Format = ExportWriters::SoundFormatSetting();
                const std::string File = A.Name + Extension(A.Path);
                const fs::path Out = fs::path(Dir) / ExportWriters::SoundFileName(File, Format);
                std::string Why;
                if (ExportWriters::WriteSound(ExportWriters::SoundSource::OfFile(Bytes.data(), Bytes.size(), File), Out, Format, &Why)) Kt.Written++;
                else T.Fail(ZoneName, Kind::Sound, A.Name, Why + " " + Out.string());
            }
            C.Advance(List.size(), List.size());
        }

        void ExportScripts(const Context& C, Totals& T)
        {
            const auto List = Assets(Kind::Script, C);
            Scripts::Sink Out(C, T, ZoneName, {});
            for (size_t N = 0; N < List.size(); N++)
            {
                const auto& A = List[N];
                if (!C.Advance(N, List.size(), A.Name)) return;
                Scripts::Script S{ A.Name };
                if (!Files->Read(A.Path, S.Bytes))
                {
                    T.Kinds[Kind::Script].Seen++;
                    T.Fail(ZoneName, Kind::Script, A.Name, "does not inflate");
                    continue;
                }
                Out.Add(std::move(S));
            }
            C.Advance(List.size(), List.size());
        }

        void ExportAnims(const Context& C, Totals& T)
        {
            auto& Kt = T.Kinds[Kind::Anim];
            const auto List = Assets(Kind::Anim, C);
            const std::string Dir = C.KindDir(Kind::Anim);
            std::vector<uint8_t> Bytes;
            for (size_t N = 0; N < List.size(); N++)
            {
                const auto& A = List[N];
                if (!C.Advance(N, List.size(), A.Name)) return;
                Kt.Seen++;
                XAnimFile F;
                std::string Error;
                if (!Files->Read(A.Path, Bytes) || !ParseXAnim(Bytes.data(), Bytes.size(), F, Error))
                {
                    T.Fail(ZoneName, Kind::Anim, A.Name, Error.empty() ? "does not inflate" : Error);
                    continue;
                }
                Counter(Version("xanim_exact", F.Version))++;
                WraithAnim W;
                W.AssetName = ExportNaming::SafeFileName(A.Name);
                W.FrameRate = F.FrameRate;
                W.Looping = F.Looping();
                // translations are offsets from the skeleton's positions (the game adds them; SEAnim's
                // and Cast's "relative"), except for skeletons stored without positions, whose
                // animations hold the positions themselves: those are written absolute, which keeps
                // them right on the exported skeleton (its positions taken from them, Pose())
                W.AnimType = WraithAnimationType::Relative;
                if (BonePositions::Of(*Files).Absolute(F))
                {
                    W.AnimType = WraithAnimationType::Absolute;
                    Counter("anims_absolute")++;
                }
                const uint32_t Frames = F.Frames();
                size_t Outside = 0;
                const auto AddPart = [&](const XAnimFile::Part& P) {
                    for (size_t K = 0; K < P.Rotations.Keys.size(); K++)
                    {
                        const auto& Q = P.Rotations.Keys[K];
                        Outside += P.Rotations.Frames[K] >= Frames;
                        W.AddRotationKey(P.Name, P.Rotations.Frames[K], Q[0], Q[1], Q[2], Q[3]);
                    }
                    for (size_t K = 0; K < P.Translations.Keys.size(); K++)
                    {
                        const auto& V = P.Translations.Keys[K];
                        Outside += P.Translations.Frames[K] >= Frames;
                        W.AddTranslationKey(P.Name, P.Translations.Frames[K], V[0], V[1], V[2]);
                    }
                };
                if (F.Delta())
                {
                    W.AnimType = WraithAnimationType::Delta;
                    W.DeltaTagName = "tag_origin";
                    AddPart(F.DeltaPart);
                }
                for (const auto& P : F.Parts) AddPart(P);
                for (const auto& [Note, Frame] : F.Notes)
                {
                    Counter(Frame <= Frames ? "notes_in_range" : "notes_out_of_range")++;
                    W.AddNoteTrack(Note, Frame);
                }
                if (Outside)
                {
                    T.Fail(ZoneName, Kind::Anim, A.Name, std::to_string(Outside) + " keys past the last frame");
                    continue;
                }
                try
                {
                    if (LiveExport::ExportWraithAnim(W, Dir) == ExportGameResult::Success) Kt.Written++;
                    else T.Fail(ZoneName, Kind::Anim, A.Name, "not written");
                }
                catch (const std::exception& E)
                {
                    T.Fail(ZoneName, Kind::Anim, A.Name, E.what());
                }
            }
            C.Advance(List.size(), List.size());
        }

        struct Material
        {
            std::string Name;                                    // as the model file names it
            std::string Techset;                                 // CoD2's (its material file); CoD1: "shader_<blend word>"
            bool NoDraw = false, Sky = false;                    // CoD1: what its shader script says
            uint8_t AtlasRows = 1, AtlasCols = 1;                // CoD2: its images' atlas grid
            std::vector<std::pair<Materials::Role, std::string>> Images;   // role, install path
            std::vector<std::string> Frames;                     // CoD1: an animMap's images (install paths)
            float FrameRate = 0;                                 // their rate a second
        };

        // an image a shader script names (the path as written, or with .dds, .tga, .jpg; a leading
        // slash dropped: pj_fx's "map /gfx/effects/explosion/dirt1"), "" when the install has none
        std::string ScriptImage(std::string Written)
        {
            while (!Written.empty() && Written[0] == '/') Written.erase(0, 1);
            const std::string Base = WithoutExtension(Written);
            for (const std::string& P : { Written, Base + ".dds", Base + ".tga", Base + ".jpg" })
                if (Files->Owner(P) >= 0) return P;
            return std::string();
        }

        // CoD1: a shader script named Name gives the colour map (its file: the path as written, or
        // with .dds, .tga, .jpg), the blending and whether it is drawn; false when none is named so.
        bool FromShader(const std::string& Name, Material& M)
        {
            const auto& All = ShadersOf(*Files).Shaders;
            const auto It = All.find(Name);
            if (It == All.end()) return false;
            const Shader& S = It->second;
            M.Techset = std::string("shader_") + S.BlendWord();
            M.NoDraw = S.NoDraw;
            M.Sky = S.Sky;
            Counter("shader_materials")++;
            if (S.ColorMap.empty()) return true;
            const std::string P = ScriptImage(S.ColorMap);
            if (P.empty())
            {
                Counter("shader_color_maps_missing")++;
                return true;
            }
            M.Images.emplace_back(Materials::Role::Albedo, P);
            // an animated one: every frame in the install, or none
            for (const auto& F : S.Frames)
                if (std::string Frame = ScriptImage(F); !Frame.empty()) M.Frames.push_back(std::move(Frame));
            if (M.Frames.size() == S.Frames.size()) M.FrameRate = S.FrameRate;
            else M.Frames.clear();
            return true;
        }

        // CoD1: the model names its colour map (skins/ or textures/ <name>), or a shader script
        // named after it does; CoD2: a material file.
        bool ResolveMaterial(const std::string& Ref, Material& M, std::string& Error)
        {
            const std::string Lower = ExportNaming::Lower(Ref);
            M = Material();
            if (Row.TexturesByName)
            {
                M.Name = WithoutExtension(fs::path(Lower).filename().string());
                const std::string Stem = WithoutExtension(Lower);
                for (const std::string& Sh : { "skins/" + Stem, "textures/" + Stem, Stem })
                    if (FromShader(Sh, M)) return true;
                for (const std::string& P : { "skins/" + Lower, "textures/" + Lower, Lower })
                    if (Files->Owner(P) >= 0)
                    {
                        M.Images.emplace_back(Materials::Role::Albedo, P);
                        return true;
                    }
                Error = "texture " + Ref + " not in the install";
                return false;
            }
            M.Name = Lower;
            auto It = MaterialCache.find(Lower);
            if (It == MaterialCache.end())
            {
                std::vector<uint8_t> Bytes;
                MaterialFile F;
                std::string Why;
                if (!Files->Read("materials/" + Lower, Bytes)) Why = "material file missing";
                else if (!ParseMaterial(Bytes.data(), Bytes.size(), F, Why)) Why = "material: " + Why;
                else Counter("material_exact")++;
                It = MaterialCache.emplace(Lower, std::make_pair(std::move(F), Why)).first;
            }
            if (!It->second.second.empty())
            {
                Error = It->second.second;
                return false;
            }
            M.Techset = It->second.first.Techset;
            M.AtlasRows = It->second.first.AtlasRows;
            M.AtlasCols = It->second.first.AtlasCols;
            for (const auto& [Sampler, Image] : It->second.first.Textures)
            {
                const Materials::Role R = Sampler == "colorMap" ? Materials::Role::Albedo
                    : Sampler == "normalMap" ? Materials::Role::Normal
                    : Sampler == "specularMap" ? Materials::Role::Specular : Materials::Role::Unknown;
                M.Images.emplace_back(R, "images/" + ExportNaming::Lower(Image) + ".iwi");
            }
            return true;
        }

        // The skeleton's world transforms; bones stored without positions take them from the
        // install's animations.
        struct Skeleton
        {
            std::vector<V3> LocalPos, WorldPos;
            std::vector<Q4> LocalRot, WorldRot;
        };
        Skeleton Pose(const XModelPartsFile& Parts)
        {
            Skeleton S;
            const size_t N = Parts.Bones.size();
            S.LocalPos.resize(N);
            S.LocalRot.resize(N);
            for (size_t B = 0; B < N; B++)
            {
                const auto& Bone = Parts.Bones[B];
                S.LocalPos[B] = { Bone.Position[0], Bone.Position[1], Bone.Position[2] };
                S.LocalRot[B] = B < Parts.Roots ? Q4{ 0, 0, 0, 1 } : Quat(Bone.Rotation[0], Bone.Rotation[1], Bone.Rotation[2]);
            }
            if (BonePositions::Positionless(Parts))
            {
                std::vector<std::string> Names;
                for (size_t B = Parts.Roots; B < N; B++) Names.push_back(ExportNaming::Lower(Parts.Bones[B].Name));
                std::vector<V3> Pos;
                std::vector<bool> Found;
                size_t Used = 0;
                BonePositions::Of(*Files).Find(Names, Pos, Found, Used);
                size_t Placed = 0;
                for (size_t B = 0; B < Names.size(); B++)
                    if (Found[B])
                    {
                        S.LocalPos[Parts.Roots + B] = Pos[B];
                        Placed++;
                    }
                Counter("skeleton_from_anims")++;
                Counter("skeleton_from_anims_bones") += Placed;
                Counter("skeleton_from_anims_bones_left") += Names.size() - Placed;
            }
            S.WorldPos = S.LocalPos;
            S.WorldRot = S.LocalRot;
            for (size_t B = Parts.Roots; B < N; B++)
            {
                const int P = Parts.Bones[B].Parent;
                S.WorldPos[B] = Add(S.WorldPos[P], Rotate(S.WorldRot[P], S.LocalPos[B]));
                S.WorldRot[B] = Mul(S.WorldRot[P], S.LocalRot[B]);
            }
            return S;
        }

        bool ReadFile(const std::string& Path, std::vector<uint8_t>& Bytes, std::string& Error)
        {
            if (Files->Read(Path, Bytes)) return true;
            Error = Path + (Files->Owner(Path) < 0 ? " not in the install" : " does not inflate");
            return false;
        }

        // A model as read: LOD 0's skeleton and one LOD's surfaces (LodIndex, its own xmodelsurfs
        // file, skinned to LOD 0's bones) as a WraithModel whose materials are named, and each
        // material's images (role, install path), parallel to its Materials. LodCount: the LODs the
        // model names.
        struct BuiltModel
        {
            std::unique_ptr<WraithModel> Model;
            std::vector<Material> Images;
        };
        bool BuildModel(const std::string& Path, const std::string& Name, BuiltModel& Out, std::string& Error, size_t LodIndex = 0,
                        size_t* LodCount = nullptr)
        {
            std::vector<uint8_t> Bytes;
            XModelFile Model;
            if (!ReadFile(Path, Bytes, Error) || !ParseXModel(Bytes.data(), Bytes.size(), Model, Error)) return false;
            if (Model.Lods.empty())
            {
                Error = "no LOD";
                return false;
            }
            if (LodCount != nullptr) *LodCount = Model.Lods.size();
            if (LodIndex >= Model.Lods.size())
            {
                Error = "no LOD " + std::to_string(LodIndex);
                return false;
            }
            const auto& Lod = Model.Lods[LodIndex];
            const std::string LodName = ExportNaming::Lower(Lod.Name), PartsName = ExportNaming::Lower(Model.Lods[0].Name);
            XModelPartsFile Parts;
            XModelSurfsFile Surfs;
            if (!ReadFile("xmodelparts/" + PartsName, Bytes, Error) || !ParseXModelParts(Bytes.data(), Bytes.size(), Parts, Error))
            {
                Error = "xmodelparts/" + PartsName + ": " + Error;
                return false;
            }
            if (!Model.Complete(Parts.Bones.size()))
            {
                Error = "xmodel: " + std::to_string(Model.Trailing) + " bytes after the materials for " + std::to_string(Parts.Bones.size()) + " bones";
                return false;
            }
            if (LodIndex == 0) Counter(Version("xmodel_exact", Model.Version))++;
            if (!ReadFile("xmodelsurfs/" + LodName, Bytes, Error) || !ParseXModelSurfs(Bytes.data(), Bytes.size(), Surfs, Error))
            {
                Error = "xmodelsurfs/" + LodName + ": " + Error;
                return false;
            }
            if (Lod.Materials.size() != Surfs.Surfaces.size())
            {
                Error = std::to_string(Lod.Materials.size()) + " materials for " + std::to_string(Surfs.Surfaces.size()) + " surfaces";
                return false;
            }
            const Skeleton S = Pose(Parts);
            const size_t BoneCount = Parts.Bones.size();

            auto W = std::make_unique<WraithModel>();
            W->AssetName = ExportNaming::SafeFileName(Name) + "_LOD" + std::to_string(LodIndex);
            for (size_t B = 0; B < BoneCount; B++)
            {
                auto& Bone = W->AddBone();
                Bone.TagName = Parts.Bones[B].Name;
                Bone.BoneParent = Parts.Bones[B].Parent;
                Bone.LocalPosition = Vector3(S.LocalPos[B][0], S.LocalPos[B][1], S.LocalPos[B][2]);
                Bone.LocalRotation = Quaternion(S.LocalRot[B][0], S.LocalRot[B][1], S.LocalRot[B][2], S.LocalRot[B][3]);
                Bone.GlobalPosition = Vector3(S.WorldPos[B][0], S.WorldPos[B][1], S.WorldPos[B][2]);
                Bone.GlobalRotation = Quaternion(S.WorldRot[B][0], S.WorldRot[B][1], S.WorldRot[B][2], S.WorldRot[B][3]);
            }

            // materials, one per surface (the same one once)
            std::map<std::string, int32_t> MaterialIndex;
            std::vector<int32_t> SurfaceMaterial;
            for (const auto& Ref : Lod.Materials)
            {
                Material M;
                std::string Why;
                if (!ResolveMaterial(Ref, M, Why))
                {
                    Counter("materials_unresolved")++;
                    if (UnresolvedSeen.insert(Ref).second) Unresolved.push_back(Ref + ": " + Why);
                    M.Name = WithoutExtension(fs::path(ExportNaming::Lower(Ref)).filename().string());
                    M.Images.clear();
                }
                std::string Key = ExportNaming::SafeFileName(std::string(M.Name));
                for (auto& Ch : Key) if (Ch == '@') Ch = '_';
                auto It = MaterialIndex.find(Key);
                if (It == MaterialIndex.end())
                {
                    WraithMaterial Wm;
                    Wm.MaterialName = Key;
                    for (auto I = M.Images.begin(); I != M.Images.end();)
                    {
                        if (Files->Owner(I->second) >= 0) ++I;
                        else
                        {
                            Counter("textures_missing")++;
                            I = M.Images.erase(I);
                        }
                    }
                    M.Name = Key;
                    It = MaterialIndex.emplace(Key, (int32_t)W->Materials.size()).first;
                    W->AddMaterial(Wm);
                    Out.Images.push_back(std::move(M));
                }
                SurfaceMaterial.push_back(It->second);
            }

            // surfaces: a vertex where the game's skinning puts it in the bind pose, the weighted sum
            // of its offsets carried by each influencing bone
            for (size_t Si = 0; Si < Surfs.Surfaces.size(); Si++)
            {
                const auto& Surface = Surfs.Surfaces[Si];
                if (Surfs.Version == 14 && Surface.Faces.size() != Surface.HeaderFaces) Counter("surface_strip_count_differs")++;
                auto& Mesh = W->AddSubmesh();
                Mesh.AddMaterial(SurfaceMaterial[Si]);
                for (const auto& V : Surface.Vertices)
                {
                    auto& Vo = Mesh.AddVertex();
                    V3 Pos{ 0, 0, 0 }, First{ 0, 0, 0 };
                    float Total = 0, Spread = 0;
                    for (size_t K = 0; K < V.Influences.size(); K++)
                    {
                        const auto& I = V.Influences[K];
                        if (I.Bone >= BoneCount)
                        {
                            Error = "vertex bone " + std::to_string(I.Bone) + " of " + std::to_string(BoneCount);
                            return false;
                        }
                        const V3 P = Add(S.WorldPos[I.Bone], Rotate(S.WorldRot[I.Bone], { I.Offset[0], I.Offset[1], I.Offset[2] }));
                        if (K == 0) First = P;
                        else Spread = std::max(Spread, Dist(First, P));
                        for (int C = 0; C < 3; C++) Pos[C] += P[C] * I.Weight;
                        Total += I.Weight;
                        bool Merged = false;
                        for (auto& Existing : Vo.Weights)
                            if (Existing.BoneIndex == I.Bone)
                            {
                                Existing.Weight += I.Weight;
                                Merged = true;
                            }
                        if (!Merged) Vo.AddVertexWeight(I.Bone, I.Weight);
                    }
                    if (V.Influences.size() > 1)
                    {
                        Counter("weighted_vertices")++;
                        if (Spread >= 0.1f) Counter("weighted_vertices_apart")++;
                    }
                    if (Total > 0)
                        for (auto& C : Pos) C /= Total;
                    const V3 Normal = Rotate(S.WorldRot[V.Influences[0].Bone], { V.Normal[0], V.Normal[1], V.Normal[2] });
                    Vo.Position = Vector3(Pos[0], Pos[1], Pos[2]);
                    Vo.Normal = Vector3(Normal[0], Normal[1], Normal[2]);
                    Vo.AddUVLayer(V.UV[0], V.UV[1]);
                    std::memcpy(Vo.Color, V.Color, 4);
                }
                for (const auto& F : Surface.Faces) Mesh.AddFace(F[0], F[1], F[2]);
            }
            if (LodIndex == 0)
            {
                Counter(Version("xmodelparts_exact", Parts.Version))++;
                Counter(Version("xmodelsurfs_exact", Surfs.Version))++;
            }
            else
                Counter("model_lods_written")++;
            Out.Model = std::move(W);
            return true;
        }

        // The image name of a texture (as the image kind writes it), recorded in Textures.
        std::string ImageName(const std::string& Path, std::map<std::string, std::string>& Textures)
        {
            Kind K;
            std::string Name;
            AssetOf(Path, K, Name);
            Textures[Path] = Name;
            return Name;
        }

        // One model: LOD 0 (every LOD with --lods, each its own file) written with its materials'
        // sidecar; its textures added to Textures.
        bool ExportModel(const Asset& A, const std::string& Dir, std::map<std::string, std::string>& Textures, std::string& Error)
        {
            size_t Lods = 1;
            if (!ExportLod(A, Dir, Textures, Error, 0, &Lods)) return false;
            if (SettingsManager::GetSetting("exportalllods") == "true")
                for (size_t L = 1; L < Lods; L++)
                {
                    std::string Why;
                    if (!ExportLod(A, Dir, Textures, Why, L, nullptr)) Counter("model_lods_unread")++;
                }
            return true;
        }
        bool ExportLod(const Asset& A, const std::string& Dir, std::map<std::string, std::string>& Textures, std::string& Error, size_t LodIndex,
                       size_t* LodCount)
        {
            BuiltModel B;
            if (!BuildModel(A.Path, A.Name, B, Error, LodIndex, LodCount)) return false;
            // a texture is named where the image writer puts it
            const std::string Relative = "..\\\\..\\\\images\\\\";
            const std::string Ext = ExportWriters::ImageExtension(ExportWriters::ImageFormatSetting());
            Materials::Sidecar Side;
            Adapter Describer;
            Side.Game = Row.Id;
            Side.Adapter = Describer.Name();
            for (size_t Mi = 0; Mi < B.Images.size(); Mi++)
            {
                WraithMaterial& Wm = B.Model->Materials[Mi];
                Materials::Source Src;
                Src.Name = Wm.MaterialName;
                Src.Techset = B.Images[Mi].Techset;
                for (const auto& [Role, Path] : B.Images[Mi].Images)
                {
                    std::string Escaped;
                    for (char Ch : ImageName(Path, Textures)) Escaped += Ch == '/' ? std::string("\\\\") : std::string(1, Ch);
                    const std::string File = Relative + Escaped + Ext;
                    if (Role == Materials::Role::Albedo && Wm.DiffuseMapName.empty()) Wm.DiffuseMapName = File;
                    if (Role == Materials::Role::Normal && Wm.NormalMapName.empty()) Wm.NormalMapName = File;
                    if (Role == Materials::Role::Specular && Wm.SpecularMapName.empty()) Wm.SpecularMapName = File;
                    Src.Images.push_back({ File, Role, 0 });
                }
                Side.Materials.push_back(Describer.Describe(Src));
                Side.Sources.push_back(std::move(Src));
            }
            try
            {
                std::error_code Ec;
                fs::create_directories(Dir, Ec);
                if (LiveExport::ExportWraithModel(B.Model, Dir, &Side) != ExportGameResult::Success)
                {
                    Error = "not written";
                    return false;
                }
            }
            catch (const std::exception& E)
            {
                Error = E.what();
                return false;
            }
            return true;
        }

        // Every skeleton, surface and material file of the zone parsed to its end, whether a model's
        // first LOD uses it or not (the other LODs, files no model names): check_* counters.
        void CheckModelFiles(Totals& T)
        {
            std::vector<uint8_t> Bytes;
            for (const auto& Path : Files->FilesOf(Archive))
            {
                const size_t Slash = Path.find('/');
                if (Slash == std::string::npos || Path.find('/', Slash + 1) != std::string::npos) continue;
                const std::string Folder = Path.substr(0, Slash);
                std::string Error, Counted;
                const std::string Ext = Extension(Path);
                const bool Shader = Ext == ".tech" || Ext == ".techset" || Ext == ".hlsl" || Ext == ".sm";
                if (Folder == "xmodelparts" || Folder == "xmodelsurfs" || (Folder == "materials" && !Shader))
                {
                    if (!Files->Read(Path, Bytes)) Error = "does not inflate";
                    else if (Folder == "xmodelparts")
                    {
                        XModelPartsFile F;
                        if (ParseXModelParts(Bytes.data(), Bytes.size(), F, Error)) Counted = Version("check_xmodelparts_exact", F.Version);
                    }
                    else if (Folder == "xmodelsurfs")
                    {
                        XModelSurfsFile F;
                        if (ParseXModelSurfs(Bytes.data(), Bytes.size(), F, Error)) Counted = Version("check_xmodelsurfs_exact", F.Version);
                    }
                    else
                    {
                        MaterialFile F;
                        if (ParseMaterial(Bytes.data(), Bytes.size(), F, Error)) Counted = "check_material_exact";
                    }
                    if (!Counted.empty()) Counter(Counted)++;
                    else T.Fail(ZoneName, Kind::Model, Path, "check: " + Error);
                }
            }
        }

        void ExportModels(const Context& C, Totals& T)
        {
            ShaderMeasures(T);
            auto& Kt = T.Kinds[Kind::Model];
            const auto List = Assets(Kind::Model, C);
            const std::string Dir = C.KindDir(Kind::Model);
            const std::string ImagesDir = C.KindDir(Kind::Image);
            std::map<std::string, std::string> Textures;   // install path -> image name
            for (size_t N = 0; N < List.size(); N++)
            {
                const auto& A = List[N];
                if (!C.Advance(N, List.size(), A.Name)) return;
                Kt.Seen++;
                std::string Error;
                if (ExportModel(A, (fs::path(Dir) / ExportNaming::SafeFileName(A.Name)).string(), Textures, Error)) Kt.Written++;
                else T.Fail(ZoneName, Kind::Model, A.Name, Error);
            }
            C.Advance(List.size(), List.size());
            if (C.NameFilter.empty() && !C.Only) CheckModelFiles(T);
            for (const auto& U : Unresolved) T.Fail(ZoneName, Kind::Model, "", "material " + U);
            Unresolved.clear();
            // the textures, through the image kind's writer, counted apart from the run's images
            std::vector<ImageJob> Jobs;
            for (const auto& [Path, Name] : Textures) Jobs.push_back({ Path, Name });
            DiskGame::KindTotals Own;
            const size_t Failed = WriteImages(Jobs, ImagesDir, C, T, Own);
            Counter("textures") += Jobs.size();
            Counter("textures_written") += Jobs.size() - Failed;   // or written earlier by another zone
        }

        // fx/<name>.json: each .efx as the fx kind's JSON (EfxEffects.h), a shader's visual with its
        // techset, colour map and atlas or frames (the level's materials: CoD1 a shader script or an
        // image, CoD2 a material file); with the image kind in the run, those images written too
        void ExportEffects(const Context& C, Totals& T)
        {
            auto& Kt = T.Kinds[Kind::Fx];
            const auto List = Assets(Kind::Fx, C);
            const fs::path Dir = C.KindDir(Kind::Fx);
            std::map<std::string, std::string> Textures;   // install path -> image name
            const auto Fields = [&](const std::string& Shader, nlohmann::ordered_json& V) {
                Material M;
                std::string Why;
                // a shader named as its image file (CoD1's textures/sfx/spark.tga): its name without it
                const std::string Ext = Extension(ExportNaming::Lower(Shader));
                if (!ResolveLevelMaterial(IsImage(Ext) ? WithoutExtension(Shader) : Shader, M, Why))
                {
                    Counter("fx_shaders_unresolved")++;
                    return;
                }
                V["techset"] = M.Techset;
                auto Images = nlohmann::ordered_json::array();
                for (const auto& [Role, Path] : M.Images)
                {
                    if (Files->Owner(Path) < 0) continue;
                    const char* Usage = Role == Materials::Role::Albedo ? "diffuse" : Role == Materials::Role::Normal ? "normal"
                                      : Role == Materials::Role::Specular ? "specular" : "unknown";
                    Images.push_back({ { "usage", Usage }, { "semantic", 0 }, { "name", ImageName(Path, Textures) } });
                }
                V["images"] = std::move(Images);
                if (M.AtlasRows * M.AtlasCols > 1) V["atlas"] = { M.AtlasRows, M.AtlasCols };
                // CoD1's animated shader: its frames' images in order, their rate a second
                if (!M.Frames.empty())
                {
                    auto Frames = nlohmann::ordered_json::array();
                    for (const auto& Path : M.Frames) Frames.push_back(ImageName(Path, Textures));
                    V["frames"] = std::move(Frames);
                    V["frameRate"] = M.FrameRate;
                }
            };
            for (size_t N = 0; N < List.size(); N++)
            {
                const auto& A = List[N];
                if (!C.Advance(N, List.size(), A.Name)) return;
                Kt.Seen++;
                std::vector<uint8_t> Bytes;
                std::string Json, Error;
                if (!Files->Read(A.Path, Bytes)) T.Fail(ZoneName, Kind::Fx, A.Name, "does not inflate");
                else if (!Efx::ToJson(std::string(Bytes.begin(), Bytes.end()), A.Name, Row.Id, Fields, Json, Error))
                    T.Fail(ZoneName, Kind::Fx, A.Name, Error);
                else if (ExportWriters::WriteBytes(Dir / (A.Name + ".json"), reinterpret_cast<const uint8_t*>(Json.data()), Json.size()))
                    Kt.Written++;
                else T.Fail(ZoneName, Kind::Fx, A.Name, "write failed");
            }
            C.Advance(List.size(), List.size());
            if (!C.Kinds.count(Kind::Image)) return;
            std::vector<ImageJob> Jobs;
            for (const auto& [Path, Name] : Textures) Jobs.push_back({ Path, Name });
            DiskGame::KindTotals Own;
            const size_t Failed = WriteImages(Jobs, C.KindDir(Kind::Image), C, T, Own);
            Counter("fx_textures") += Jobs.size();
            Counter("fx_textures_written") += Jobs.size() - Failed;
        }

        // A level material (CoD1: an image path without extension; CoD2: a material file) and its
        // images, added to the scene once, with each image recorded for the image kind.
        void AddMapMaterial(Maps::MapScene& Scene, const std::string& Name, const Material& M,
                            std::map<std::string, std::string>& Textures)
        {
            if (Scene.Materials.count(Name)) return;
            Maps::Material& Out = Scene.Materials[Name];
            Out.Name = Name;
            Out.Techset = M.Techset;
            for (const auto& [Role, Path] : M.Images)
            {
                const std::string Image = ImageName(Path, Textures);
                if (Role == Materials::Role::Albedo && Out.ColorMap.empty()) Out.ColorMap = Image;
                Out.Textures.push_back({ static_cast<uint32_t>(Role), Image });
            }
        }
        bool ResolveLevelMaterial(const std::string& Name, Material& M, std::string& Error)
        {
            M = Material();
            M.Name = Name;
            if (!Row.TexturesByName) return ResolveMaterial(Name, M, Error);
            const std::string Lower = ExportNaming::Lower(Name);
            if (FromShader(Lower, M)) return true;
            for (const char* Ext : { ".dds", ".tga", ".jpg" })
                if (Files->Owner(Lower + Ext) >= 0)
                {
                    M.Images.emplace_back(Materials::Role::Albedo, Lower + Ext);
                    return true;
                }
            Error = "no image " + Name;
            return false;
        }

        // One level: its drawn surfaces as the world mesh, its collision (brushes, brush models,
        // curved and terrain triangles), the models its entities place (misc_*,
        // script_*), its lights and sun, the entity string; written by the map writers.
        bool ExportMap(const Asset& A, const Context& C, Totals& T, std::string& Error)
        {
            std::vector<uint8_t> Bytes;
            BspFile Bsp;
            if (!ReadFile(A.Path, Bytes, Error) || !ParseBsp(Bytes.data(), Bytes.size(), Bsp, Error)) return false;
            Counter("bsp_exact_v" + std::to_string(Bsp.Version))++;
            std::vector<Entity> Ents;
            if (!ParseEntities(Bsp.Entities, Ents))
            {
                Error = "entity string does not parse";
                return false;
            }
            Maps::MapScene Scene;
            Scene.Name = A.Name;
            Scene.Game = Row.Id;
            Scene.MapEnts = Bsp.Entities;
            BspCollision(Bsp, Scene.Collision, Counters);
            std::map<std::string, std::string> Textures;   // install path -> image name

            // world: the soups grouped by material, each vertex once per group
            Maps::Mesh World{ "world", {} };
            std::map<std::string, size_t> Group;
            std::vector<std::map<uint32_t, uint32_t>> Remap;
            // what each material is: drawn, a tool's (clip, caulk...: never drawn) or the sky's
            enum class Use { Draw, Tool, Sky };
            std::vector<Use> Uses(Bsp.Materials.size(), Use::Draw);
            std::vector<Material> Resolved(Bsp.Materials.size());
            for (size_t I = 0; I < Bsp.Materials.size(); I++)
            {
                const std::string Name = ExportNaming::Lower(Bsp.Materials[I].Name);
                std::string Why;
                if (!ResolveLevelMaterial(Bsp.Materials[I].Name, Resolved[I], Why)) Counter("map_materials_unresolved")++;
                const std::string& Tech = Resolved[I].Techset;
                const bool Shaded = Tech.compare(0, 7, "shader_") == 0;
                if (Resolved[I].NoDraw || Tech.compare(0, 5, "tools") == 0 || (!Shaded && Name.compare(0, 16, "textures/common/") == 0))
                    Uses[I] = Use::Tool;
                else if (Resolved[I].Sky || Tech == "sky" || (!Shaded && Name.compare(0, 13, "textures/sky/") == 0)) Uses[I] = Use::Sky;
            }
            for (const auto& S : Bsp.Soups)
            {
                const std::string& MatName = Bsp.Materials[S.Material].Name;
                if (Uses[S.Material] != Use::Draw)
                {
                    Counter(Uses[S.Material] == Use::Tool ? "map_tool_triangles_skipped" : "map_sky_triangles_skipped") += S.IndexCount / 3;
                    continue;
                }
                auto G = Group.find(MatName);
                if (G == Group.end())
                {
                    G = Group.emplace(MatName, World.Surfaces.size()).first;
                    World.Surfaces.push_back({ MatName, {}, {} });
                    Remap.emplace_back();
                    AddMapMaterial(Scene, MatName, Resolved[S.Material], Textures);
                }
                auto& Surface = World.Surfaces[G->second];
                auto& Map = Remap[G->second];
                for (uint32_t K = 0; K < S.IndexCount; K++)
                {
                    const uint32_t V = S.FirstVertex + Bsp.Indices[S.FirstIndex + K];
                    auto [It, New] = Map.emplace(V, (uint32_t)Surface.Vertices.size());
                    if (New) Surface.Vertices.push_back(Bsp.Vertices[V]);
                    Surface.Indices.push_back(It->second);
                }
            }
            Scene.Objects.push_back(std::move(World));

            // entities: placed models, lights, the sun
            std::set<std::string> ModelNames;
            for (const auto& E : Ents)
            {
                const auto Get = [&](const char* K) { const auto It = E.find(K); return It == E.end() ? std::string() : It->second; };
                const std::string Class = Get("classname"), Model = ExportNaming::Lower(Get("model"));
                if (Class == "worldspawn")
                {
                    float Dir[3], Color[3] = { 1, 1, 1 }, Light = 1;
                    Maps::EntityFloats(E, "suncolor", Color, 3);
                    Maps::EntityFloats(E, "sunlight", &Light, 1);
                    if (Maps::EntityFloats(E, "sundirection", Dir, 3))
                    {
                        float Axis[9];
                        Maps::AnglesToAxis(Dir, Axis);
                        Maps::Light Sun;
                        Sun.Kind = Maps::Light::Type::Sun;
                        for (int I = 0; I < 3; I++)
                        {
                            Scene.Sky.Sun[I] = Axis[I];   // toward the sun
                            Sun.Dir[I] = -Axis[I];
                            Sun.Color[I] = Color[I] * Light;
                        }
                        Scene.Lights.push_back(Sun);
                    }
                    float Ambient = 0, Tint[3] = { 1, 1, 1 };
                    if (Maps::EntityFloats(E, "ambient", &Ambient, 1))
                    {
                        Maps::EntityFloats(E, "_color", Tint, 3);
                        for (int I = 0; I < 3; I++) Scene.AmbientColor[I] = Tint[I] * Ambient;
                    }
                    continue;
                }
                if (Class == "light")
                {
                    Maps::Light L;
                    float Intensity = 1, Value = 0;
                    Maps::EntityFloats(E, "origin", L.Origin, 3);
                    Maps::EntityFloats(E, "_color", L.Color, 3);
                    Maps::EntityFloats(E, "intensity", &Intensity, 1);
                    Maps::EntityFloats(E, "light", &Value, 1);
                    if (!Maps::EntityFloats(E, "radius", &L.Radius, 1)) L.Radius = Value;
                    for (auto& Ch : L.Color) Ch *= Intensity;
                    Scene.Lights.push_back(L);
                    continue;
                }
                if (Model.compare(0, 7, "xmodel/") != 0 || (Class.compare(0, 5, "misc_") != 0 && Class.compare(0, 7, "script_") != 0))
                    continue;
                const std::string Name = Model.substr(7);
                if (Files->Owner("xmodel/" + Name) < 0)
                {
                    Counter("map_placements_no_model")++;   // xmodel/fx and the like: script markers
                    continue;
                }
                Maps::Instance I;
                I.Model = Name;
                I.Name = Name + "_" + std::to_string(Scene.StaticInstances.size() + Scene.DynamicInstances.size());
                Maps::EntityFloats(E, "origin", I.Origin, 3);
                float Angles[3] = { 0, 0, 0 };
                Maps::EntityFloats(E, "angles", Angles, 3);
                Maps::AnglesToAxis(Angles, I.Axis);
                Maps::EntityFloats(E, "modelscale", &I.Scale, 1);
                (Class == "misc_model" ? Scene.StaticInstances : Scene.DynamicInstances).push_back(I);
                ModelNames.insert(Name);
            }

            // each placed model once, in its own space
            size_t N = 0;
            for (const auto& Name : ModelNames)
            {
                C.Advance(N++, ModelNames.size(), Name);
                BuiltModel B;
                std::string Why;
                if (!BuildModel("xmodel/" + Name, Name, B, Why))
                {
                    Counter("map_models_unread")++;
                    T.Fail(ZoneName, Kind::Map, A.Name, "model " + Name + ": " + Why);
                    continue;
                }
                Maps::Mesh Mesh{ Name, {} };
                for (const auto& Sub : B.Model->Submeshes)
                {
                    const int32_t Mi = Sub.MaterialIndicies.empty() ? -1 : Sub.MaterialIndicies[0];
                    Maps::Surface S;
                    if (Mi >= 0)
                    {
                        S.Material = B.Model->Materials[Mi].MaterialName;
                        AddMapMaterial(Scene, S.Material, B.Images[Mi], Textures);
                    }
                    for (const auto& V : Sub.Verticies)
                    {
                        Maps::Vertex O;
                        O.Pos[0] = V.Position.X, O.Pos[1] = V.Position.Y, O.Pos[2] = V.Position.Z;
                        O.Normal[0] = V.Normal.X, O.Normal[1] = V.Normal.Y, O.Normal[2] = V.Normal.Z;
                        if (!V.UVLayers.empty()) O.Uv[0] = V.UVLayers[0].X, O.Uv[1] = V.UVLayers[0].Y;
                        std::memcpy(O.Color, V.Color, 4);
                        S.Vertices.push_back(O);
                    }
                    for (const auto& F : Sub.Faces)
                        for (uint32_t Ix : { F.Index1, F.Index2, F.Index3 }) S.Indices.push_back(Ix);
                    Mesh.Surfaces.push_back(std::move(S));
                }
                Scene.Models[Name] = std::move(Mesh);
            }
            Counter("map_lights") += Scene.Lights.size();

            // the textures through the image kind's writer, then the scene's files
            std::vector<ImageJob> Jobs;
            std::set<std::string> Names;
            for (const auto& [Path, Name] : Textures)
            {
                Jobs.push_back({ Path, Name });
                Names.insert(Name);
            }
            DiskGame::KindTotals Own;
            const std::string ImagesDir = C.KindDir(Kind::Image);
            WriteImages(Jobs, ImagesDir, C, T, Own);
            Counter("map_textures") += Jobs.size();
            const std::string Ext = ExportWriters::ImageExtension(ExportWriters::ImageFormatSetting());
            const auto FileOf = [&](const std::string& Name) { return (fs::path(ImagesDir) / (Name + Ext)).string(); };
            const std::string Stem = (fs::path(C.KindDir(Kind::Map)) / ExportNaming::SafeFileName(A.Name)).string();
            Adapter Describer;
            if (!Maps::WriteMapFiles(Scene, Names, FileOf, Ext, Stem, Describer, ZoneName, C, T, Counters))
            {
                Error = "not written";
                return false;
            }
            return true;
        }

        // the install's shader scripts as the run's measures (set, not summed: one table per install)
        void ShaderMeasures(Totals& T)
        {
            if (!Row.TexturesByName) return;
            const ShaderScripts& S = ShadersOf(*Files);
            T.Counters["shader_scripts"] = S.Files;
            T.Counters["shader_scripts_unparsed"] = S.Failed;
            T.Counters["shaders"] = S.Shaders.size();
        }

        void ExportMaps(const Context& C, Totals& T)
        {
            ShaderMeasures(T);
            auto& Kt = T.Kinds[Kind::Map];
            for (const auto& A : Assets(Kind::Map, C))
            {
                Kt.Seen++;
                std::string Error;
                if (ExportMap(A, C, T, Error)) Kt.Written++;
                else if (Error != "not written") T.Fail(ZoneName, Kind::Map, A.Name, Error);
            }
        }


        GameRow Row;
        std::shared_ptr<Install> Files;
        int Archive = 0;
        std::string Lang;   // the language setting: the strings' LANG_ line
        std::string ZoneName;
        std::map<std::string, size_t> Counters;
        std::map<std::string, std::pair<MaterialFile, std::string>> MaterialCache;
        std::set<std::string> UnresolvedSeen;
        std::vector<std::string> Unresolved;
    };

    // CoD1 and CoD2 shipped in these languages; an install holds the base (English) and at most one
    // localization (localization.txt names it)
    const DiskGame::Setting Language{ "language", "Language",
        { { "english", "English" }, { "french", "French" }, { "german", "German" }, { "italian", "Italian" },
          { "spanish", "Spanish" }, { "russian", "Russian" }, { "polish", "Polish" } },
        "english", { Kind::Model, Kind::Anim, Kind::Image, Kind::Sound, Kind::Map, Kind::Localize, Kind::WeaponStats } };

    // The install of a zone: the one the run names, else the folder the archive is in.
    std::shared_ptr<Install> InstallOf(const GameRow& Row, const std::string& Zone, const DiskGame::ExportOptions& O, std::string& Error)
    {
        const std::string Folder = !O.Install.empty() ? O.Install : fs::path(Zone).parent_path().string();
        return Install::Open(Folder, Row.Extension, Language.Of(O), Error);
    }
}

const std::vector<DiskGame::Setting>& ArchiveGame::Settings() const
{
    static const std::vector<DiskGame::Setting> All = { Language };
    return All;
}

std::unique_ptr<DiskGame::IDiskZone> ArchiveGame::Unopened() const
{
    return std::make_unique<ArchiveZone>(Row, nullptr, 0);
}

bool ArchiveGame::Zones(const std::string& Input, const DiskGame::ExportOptions& Options, std::vector<std::string>& Out,
                        std::string& Error) const
{
    std::error_code Ec;
    if (fs::is_directory(Input, Ec)) Out = Install::Archives(Input, Row.Extension, Language.Of(Options));
    else if (fs::exists(Input, Ec)) Out.push_back(Input);
    else if (!Options.Install.empty())
    {
        // an archive's name within the install
        for (const auto& P : Install::Archives(Options.Install, Row.Extension, Language.Of(Options)))
            if (ExportNaming::Lower(fs::path(P).filename().string()) == ExportNaming::Lower(Input)) Out.push_back(P);
    }
    if (Out.empty()) Error = std::string("no ") + Row.Extension + " archive in " + Input;
    return !Out.empty();
}

bool ArchiveGame::ListZones(const std::string& InstallFolder, const DiskGame::ExportOptions& Options, std::vector<std::string>& Names,
                            std::string& Error) const
{
    std::error_code Ec;
    for (const auto& P : Install::Archives(InstallFolder, Row.Extension, Language.Of(Options)))
        Names.push_back(fs::relative(P, InstallFolder, Ec).string());
    if (Names.empty()) Error = std::string("no ") + Row.Extension + " archive in " + InstallFolder;
    return !Names.empty();
}

std::unique_ptr<DiskGame::IDiskZone> ArchiveGame::Open(const std::string& Zone, const DiskGame::ExportOptions& Options,
                                                       std::string& Error) const
{
    auto I = InstallOf(Row, Zone, Options, Error);
    if (!I) return nullptr;
    const int A = I->ArchiveOf(Zone);
    if (A < 0)
    {
        Error = Zone + " is not an archive of the install " + I->Folder();
        return nullptr;
    }
    return std::make_unique<ArchiveZone>(Row, std::move(I), A, Language.Of(Options));
}
}

// The install's files the game reads (each path's winning copy) that say an anim's owner: the weapon
// files, the AI and player types, characters, xmodelaliases and animtrees; read once per install
void IW12::ArchiveGame::AnimOwners(const DiskGame::ExportOptions& Options, const std::vector<AnimZone>& Zones, ::AnimOwners::Book& Out) const
{
    static std::mutex Lock;
    static std::map<std::string, std::shared_ptr<const IwClassicAnimOwners::Facts>> Read;   // by install folder and language
    const auto Wanted = [](const std::string& Path) {
        Kind K;
        std::string Name;
        if (AssetOf(Path, K, Name) && K == Kind::Weapons) return true;
        const std::string Ext = Extension(Path);
        if (Path.compare(0, 10, "animtrees/") == 0) return Ext == ".atr";
        for (const char* Folder : { "aitype/", "character/", "mptype/", "xmodelalias/" })
            if (Path.compare(0, std::strlen(Folder), Folder) == 0) return Ext == ".gsc";
        return false;
    };
    for (const auto& Z : Zones)
    {
        std::string Error;
        const auto I = InstallOf(Row, Z.Zone, Options, Error);
        if (!I) continue;
        const std::string Key = I->Folder() + "|" + Language.Of(Options);
        std::shared_ptr<const IwClassicAnimOwners::Facts> F;
        {
            std::lock_guard<std::mutex> G(Lock);
            if (const auto It = Read.find(Key); It != Read.end()) F = It->second;
        }
        if (!F)
        {
            std::vector<std::pair<std::string, std::string>> Docs;
            for (const auto& [Path, A] : I->Files())
            {
                std::vector<uint8_t> Bytes;
                if (Wanted(Path) && I->Read(Path, Bytes)) Docs.emplace_back(Path, std::string(Bytes.begin(), Bytes.end()));
            }
            F = std::make_shared<const IwClassicAnimOwners::Facts>(IwClassicAnimOwners::Parse(Docs));
            std::lock_guard<std::mutex> G(Lock);
            Read[Key] = F;
        }
        IwClassicAnimOwners::Own({ F.get() }, Z.Anims, Out);
    }
}
