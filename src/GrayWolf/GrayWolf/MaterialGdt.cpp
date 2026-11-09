#include "stdafx.h"

#include "MaterialGdt.h"

#include "AlphaRule.h"
#include "DiskGame.h"
#include "ExportNaming.h"
#include "ExportWriters.h"
#include "GdtWriter.h"
#include "ModToolsTree.h"

#include <nlohmann/json.hpp>
#include <stb_image.h>
#include <stb_image_write.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <regex>
#include <set>
#include <tuple>

namespace fs = std::filesystem;

namespace MaterialGdt
{
namespace
{
    std::atomic<bool> UseAssetNames{ false };

    // APE writes an entry's keys sorted, case aside
    bool KeyLess(const std::string& A, const std::string& B)
    {
        return std::lexicographical_compare(A.begin(), A.end(), B.begin(), B.end(), [](char X, char Y) {
            return std::tolower((unsigned char)X) < std::tolower((unsigned char)Y);
        });
    }

    std::string Floats3(const float C[3])
    {
        char B[96];
        std::snprintf(B, sizeof(B), "%g %g %g 1", C[0], C[1], C[2]);
        return B;
    }

    // the short tag an image's name takes when its file serves two semantics
    std::string Tag(const std::string& Semantic)
    {
        static const std::map<std::string, std::string> Tags = { { "diffuseMap", "c" }, { "normalMap", "n" }, { "glossMap", "g" },
                                                                 { "specularMap", "s" }, { "specularMask", "sm" }, { "occlusionMap", "o" },
                                                                 { "thicknessMap", "t" } };
        const auto It = Tags.find(Semantic);
        return It == Tags.end() ? Semantic : It->second;
    }

    // The code images BO3's own materials name ("$normal"): what the stock GDTs of its mod tools put
    // in a material's image slots
    const char* const StockCodeImages[] = { "$black_color", "$black_gloss", "$black_reveal", "$black_thickness",
                                            "$blacktransparent_color", "$color_black_40", "$gloss", "$gray", "$gray_color",
                                            "$identitynormalmap", "$normal", "$occlusion", "$spec_simple", "$specular_mask",
                                            "$white_ao", "$white_diffuse", "$white_gloss", "$white_multimask", "$white_reveal",
                                            "$white_specular", "$white_thickness" };

    bool TiffOrPng(const fs::path& File)
    {
        const std::string E = ExportNaming::Lower(File.extension().string());
        return E == ".tif" || E == ".tiff" || E == ".png";
    }

    // Whether a colour map's alpha masks something (AlphaTally), the file read with stb (the
    // preview's reader goes through WIC, which wants COM on the thread)
    bool AlphaMasks(const fs::path& File)
    {
        int W = 0, H = 0, N = 0;
        stbi_uc* Px = stbi_load(File.string().c_str(), &W, &H, &N, 4);
        if (Px == nullptr) return false;
        AlphaTally T;
        for (size_t I = 0; I < (size_t)W * H; I++) T.Add(Px[I * 4 + 3]);
        stbi_image_free(Px);
        return N == 4 && T.Masks();
    }

    // An image under 4 texels a side (MW2019's 1x1 constant colours) as a 4x4 one beside it
    // (<stem>_4x4.png, kept when newer): BO3's linker calls a smaller one missing. False: the
    // image as it is.
    bool Grown(const fs::path& From, fs::path& To)
    {
        int W = 0, H = 0, N = 0;
        if (ExportNaming::Lower(From.extension().string()) != ".png" || !stbi_info(From.string().c_str(), &W, &H, &N) || (W >= 4 && H >= 4))
            return false;
        To = fs::path(From).replace_filename(From.stem().string() + "_4x4.png");
        std::error_code Ec, Ec2;
        if (fs::is_regular_file(To, Ec) && fs::last_write_time(To, Ec) >= fs::last_write_time(From, Ec2) && !Ec && !Ec2) return true;
        stbi_uc* Px = stbi_load(From.string().c_str(), &W, &H, &N, 4);
        if (Px == nullptr) return false;
        std::vector<stbi_uc> Out(4 * 4 * 4);
        for (int Y = 0; Y < 4; Y++)
            for (int X = 0; X < 4; X++)
                std::memcpy(&Out[(Y * 4 + X) * 4], Px + ((size_t)(Y * H / 4) * W + (size_t)(X * W / 4)) * 4, 4);
        stbi_image_free(Px);
        return stbi_write_png(To.string().c_str(), 4, 4, 4, Out.data(), 4 * 4) != 0;
    }

    // A normal map packed with two channels (AG: x in alpha, y in green, DXT5nm, CoD4's; XY: x in
    // red, y in green) as BO3's normalMap reads one, x, y, z in r, g, b: written to To, kept when
    // newer than From. Read as it is, the packed file tilts every normal (z about 0.5), which
    // lights a surface in dark speckles. Y stays as the source game has it (both CoD). The file
    // has the last word over the description: grey in r, g, b with the alpha varying is AG
    // whatever Enc says (CoD4's adapter leaves its normals at the XY default).
    bool UnpackNormal(const fs::path& From, const fs::path& To, Materials::NormalEncoding Enc, bool& AsAg)
    {
        std::error_code Ec, Ec2;
        int W = 0, H = 0, N = 0;
        stbi_uc* Px = stbi_load(From.string().c_str(), &W, &H, &N, 4);
        if (Px == nullptr) return false;
        const size_t Count = (size_t)W * H;
        uint64_t Grey = 0;
        int Low = 255, High = 0;
        for (size_t I = 0; I < Count; I++)
        {
            const stbi_uc* P = Px + I * 4;
            Grey += std::abs(P[0] - P[1]) + std::abs(P[1] - P[2]);
            Low = std::min<int>(Low, P[3]);
            High = std::max<int>(High, P[3]);
        }
        if (Count > 0 && Grey < Count * 4 && High - Low > 8) Enc = Materials::NormalEncoding::AG;
        AsAg = Enc == Materials::NormalEncoding::AG;
        if (fs::is_regular_file(To, Ec) && fs::last_write_time(To, Ec) >= fs::last_write_time(From, Ec2) && !Ec && !Ec2)
        {
            stbi_image_free(Px);
            return true;
        }
        std::vector<stbi_uc> Rgb(Count * 3);
        for (size_t I = 0; I < Count; I++)
        {
            const stbi_uc* P = Px + I * 4;
            const stbi_uc Xb = Enc == Materials::NormalEncoding::AG ? P[3] : P[0];
            const float X = Xb / 127.5f - 1.0f, Y = P[1] / 127.5f - 1.0f;
            const float Z = std::sqrt(std::max(0.0f, 1.0f - X * X - Y * Y));
            Rgb[I * 3] = Xb;
            Rgb[I * 3 + 1] = P[1];
            Rgb[I * 3 + 2] = (stbi_uc)std::clamp(std::lround((Z + 1.0f) * 127.5f), 0L, 255L);
        }
        stbi_image_free(Px);
        return stbi_write_png(To.string().c_str(), W, H, 3, Rgb.data(), W * 3) != 0;
    }
}

std::string AssetName(const std::string& Source)
{
    std::string Out;
    Out.reserve(Source.size());
    for (const char C : Source)
    {
        const char L = (char)std::tolower((unsigned char)C);
        Out += (L >= 'a' && L <= 'z') || (L >= '0' && L <= '9') ? L : '_';
    }
    const size_t First = Out.find_first_not_of('_');
    if (First == std::string::npos) return std::string();
    return Out.substr(First, Out.find_last_not_of('_') - First + 1);
}

std::string PrefixedName(const std::string& Prefix, const std::string& Source)
{
    if (Prefix.empty()) return AssetName(Source);
    const std::string Base = AssetName(Source);
    std::string Name = Prefix + Base;
    constexpr size_t Most = 56;   // room for an image's semantic tag and number ("_sma_2")
    // a source name the asset name loses something of (BO6's "~tm_x", the spline of "tm_x"; a
    // capital, a '/') takes its hash too, or two models would be one GDT entry
    const bool Lossy = Base != Source;
    if (Name.size() <= Most && !Lossy) return Name;
    uint32_t Hash = 2166136261u;   // FNV-1a, of the name as the source has it
    for (const char C : Prefix + Source) Hash = (Hash ^ (uint8_t)C) * 16777619u;
    char Tail[16];
    std::snprintf(Tail, sizeof(Tail), "_%08x", Hash);
    return Name.substr(0, (std::min)(Name.size(), Most - 9)) + Tail;
}

bool Builder::Image::operator<(const Image& O) const
{
    return std::tie(Builtin, File, Semantic, FromAlpha) < std::tie(O.Builtin, O.File, O.Semantic, O.FromAlpha);
}

Builder::Builder(std::function<std::string(const fs::path&)> BaseImage, std::string CounterPrefix, std::string Names)
    : BaseImageOf(std::move(BaseImage)), Prefix(std::move(CounterPrefix)), NamePrefix(std::move(Names))
{
}

bool Builder::Empty() const
{
    return Defined.empty();
}

// One image slot: the map's file, as the BO3 image of Semantic. A single-channel semantic takes the
// file's red channel, or its alpha (fromAlpha): a value in green or blue has no BO3 slot.
bool Builder::Slot(Material& M, const char* Key, const Materials::Map& Map, const fs::path& Dir, const char* Semantic,
                   const char* Core, const char* Compression)
{
    if (Map.Empty()) return false;
    const bool Single = std::string(Core) == "Linear1ch";
    if (Single && (Map.Channel == 1 || Map.Channel == 2))
    {
        Counter(std::string(Key) + "_channel_dropped")++;
        return false;
    }
    Image I;
    I.File = (Dir / fs::path(Map.Path)).lexically_normal();
    I.FromAlpha = Single && Map.Channel == 3;
    // a code image BO3 has too: named as it is, no file
    const std::string Stem = ExportNaming::Lower(I.File.stem().string());
    if (!I.FromAlpha && std::find(std::begin(StockCodeImages), std::end(StockCodeImages), Stem) != std::end(StockCodeImages))
    {
        I.Builtin = Stem;
        M.Slots[Key] = std::move(I);
        Counter(std::string("slot_") + Key)++;
        Counter("slots_stock_images")++;
        return true;
    }
    std::error_code Ec;
    if (!fs::is_regular_file(I.File, Ec))
    {
        Counter("images_missing")++;
        return false;
    }
    if (BaseImageOf(I.File).empty())
    {
        Counter("images_outside")++;
        return false;
    }
    if (!TiffOrPng(I.File)) Counter("slots_not_tiff_png")++;
    if (fs::path Larger; Grown(I.File, Larger))
    {
        I.File = Larger;
        Counter("images_grown_to_4x4")++;
    }
    I.Semantic = Semantic;
    I.Core = Core;
    I.Compression = Compression;
    M.Slots[Key] = std::move(I);
    Counter(std::string("slot_") + Key)++;
    return true;
}

void Builder::Add(const Materials::Description& D, const fs::path& Dir)
{
    const std::string Bare = AssetName(D.Name);
    if (Bare.empty())
    {
        Counter("unnamed")++;
        return;
    }
    const std::string Name = PrefixedName(NamePrefix, D.Name);
    if (const auto It = Defined.find(Name); It != Defined.end())
    {
        if (It->second.Source == D.Name) Counter("shared")++;
        else if (Collisions[Name].insert(D.Name).second) Counter("name_collisions")++;
        return;
    }
    Material M;
    M.Source = D.Name;
    if (Bare != D.Name) Counter("renamed")++;

    // the colour and roughness of a layered material are its layers': the first one's
    Materials::Map Albedo = D.Albedo, Roughness = D.Roughness;
    if (!D.Layers.empty())
    {
        if (Albedo.Empty()) Albedo = D.Layers.front().Albedo;
        if (Roughness.Empty()) Roughness = D.Layers.front().Roughness;
        Counter("layered_first_layer")++;
    }
    if (!D.Tint.Empty() || !D.Color.Empty()) Counter("tint_dropped")++;
    if (!D.AlphaMap.Empty()) Counter("alpha_map_dropped")++;
    if (!D.Metalness.Empty()) Counter("metalness_dropped")++;

    // the technique: emission, then light through the surface, then the alpha, then a colour specular
    const bool Emissive = !D.Emissive.Empty(), Backlit = !D.Transmission.Empty() && D.Transmission.Image.Invert;
    if (!D.Transmission.Empty() && !D.Transmission.Image.Invert) Counter("transmission_not_thickness")++;
    // Auto: cutout when the colour map's alpha masks something (as the preview and the glTF judge
    // it: a chain-link fence, foliage cards), else opaque
    bool AutoCut = false;
    if (D.Alpha == Materials::AlphaMode::Auto && !Albedo.Empty())
    {
        AutoCut = AlphaMasks((Dir / fs::path(Albedo.Path)).lexically_normal());
    }
    const bool Blend = D.Alpha == Materials::AlphaMode::Blend, Cutout = D.Alpha == Materials::AlphaMode::Cutout || AutoCut;
    const bool Spec = !D.Specular.Empty();
    M.Category = "Geometry";
    if (Emissive) M.Type = "lit_emissive";
    else if (Backlit) M.Type = "lit_backlit";
    else if (Blend) M.Type = Spec ? "lit_transparent_advanced" : "lit_transparent";
    else if (Cutout) M.Type = Spec ? "lit_alphatest_advanced" : "lit_alphatest";
    else if (Spec) M.Type = "lit_advanced_fullspec";
    else M.Type = "lit";
    if (M.Type.find("advanced") != std::string::npos) M.Category = "Geometry Advanced";
    // a decal (named so, as the older games name theirs) lies on the surface it marks: BO3 draws
    // one above it only as a lit_decal, else the two fight for the depth and flicker
    const bool Decal = !Emissive && ExportNaming::Lower(D.Name).find("decal") != std::string::npos;
    if (Decal)
    {
        M.Type = "lit_decal";
        M.Category = "Decal";
        M.Usage = "decal";
        M.Values.emplace_back("layerSortDecal", "Grunge");
        Counter("decals")++;
    }
    if ((Emissive || Backlit) && (Blend || Cutout)) Counter("alpha_dropped")++;
    if (D.Alpha == Materials::AlphaMode::Auto) Counter(AutoCut ? "alpha_auto_cutout" : "alpha_auto_opaque")++;

    if (!Slot(M, "colorMap", Albedo, Dir, "diffuseMap", "sRGB3chAlpha", D.Alpha == Materials::AlphaMode::Opaque ? "compressed no alpha" : "compressed high color"))
        Counter("no_color")++;
    // a two-channel normal map as a three-channel one beside it (<stem>_xyz.png)
    Materials::Map Normal = D.Normal;
    if (!Normal.Empty() && D.NormalEnc != Materials::NormalEncoding::XYZ)
    {
        const fs::path From = (Dir / fs::path(Normal.Path)).lexically_normal();
        const fs::path To = fs::path(From).replace_filename(From.stem().string() + "_xyz.png");
        bool AsAg = false;
        if (UnpackNormal(From, To, D.NormalEnc, AsAg))
        {
            Normal.Path = (fs::path(Normal.Path).parent_path() / To.filename()).generic_string();
            Counter(AsAg ? "normals_unpacked_ag" : "normals_unpacked_xy")++;
        }
        else
            Counter("normals_not_unpacked")++;
    }
    if (Slot(M, "normalMap", Normal, Dir, "normalMap", "Normal", "compressed"))
    {
        Counter(D.NormalEnc == Materials::NormalEncoding::AG ? "normal_ag" : D.NormalEnc == Materials::NormalEncoding::XYZ ? "normal_xyz" : "normal_xy")++;
        if (D.NormalFlipY) Counter("normal_flip_y")++;
    }
    // BO3's cosinePowerMap is a gloss map: a roughness map has no slot
    if (!Roughness.Empty() && !Roughness.Invert) Counter("roughness_not_gloss")++;
    else Slot(M, "cosinePowerMap", Roughness, Dir, "glossMap", "Linear1ch", "compressed");
    if (Spec && !Decal)
    {
        // a full colour in the fullspec and emissive techniques, else the intensity mask theirs take
        const bool Colour = M.Type == "lit_advanced_fullspec" || M.Type == "lit_emissive";
        if (Slot(M, "specColorMap", D.Specular, Dir, Colour ? "specularMap" : "specularMask", Colour ? "sRGB3ch" : "Linear1ch", "compressed"))
        {
            if (!Colour) Counter("spec_as_mask")++;
            if (D.LegacySpecular) Counter("spec_legacy")++;
        }
    }
    Slot(M, "occMap", D.Occlusion, Dir, "occlusionMap", "Linear1ch", "compressed");
    if (Emissive) Slot(M, "colorMap00", D.Emissive, Dir, "diffuseMap", "sRGB3chAlpha", "compressed no alpha");
    if (Backlit && Slot(M, "alphaRevealMap", D.Transmission.Image, Dir, "thicknessMap", "Linear1ch", "compressed"))
        M.Values.emplace_back("backlightScatterColor", Floats3(D.Transmission.Color));
    // a gloss map's full range, as the source game's reads it
    if (M.Slots.count("cosinePowerMap")) M.Values.emplace_back("glossSurfaceType", "<full>");
    Counter("type_" + M.Type)++;
    Counter("materials")++;
    Defined.emplace(Name, std::move(M));
}

std::vector<std::string> Builder::Entries(const char* Eol) const
{
    // the images: "i_<file stem>", the semantic's tag added when one stem names several images
    std::map<Image, std::string> ImageNames;
    std::map<std::string, std::vector<Image>> ByStem;
    for (const auto& [Name, M] : Defined)
        for (const auto& [Key, I] : M.Slots)
            if (I.Builtin.empty() && !ImageNames.count(I))
            {
                ImageNames[I];
                // BO3's own convention, "i_<name>" (a name that has it already keeps it), the
                // name prefix after it
                const std::string Stem = AssetName(I.File.stem().string());
                const std::string Bare = Stem.rfind("i_", 0) == 0 ? Stem.substr(2) : Stem;
                ByStem[NamePrefix.empty() ? "i_" + Bare : PrefixedName("i_" + NamePrefix, Bare)].push_back(I);
            }
    for (auto& [Stem, List] : ByStem)
    {
        std::sort(List.begin(), List.end());
        std::map<std::string, size_t> Seen;
        for (const Image& I : List)
        {
            std::string Name = Stem;
            if (List.size() > 1) Name += "_" + Tag(I.Semantic) + (I.FromAlpha ? "a" : "");
            if (const size_t N = Seen[Name]++) Name += "_" + std::to_string(N + 1);
            ImageNames[I] = Name;
        }
    }

    std::vector<std::string> Out;
    for (const auto& [Name, M] : Defined)
    {
        Gdt::Fields F = { { "materialCategory", M.Category }, { "materialType", M.Type }, { "surfaceType", "<none>" },
                          { "template", "material.template" }, { "usage", M.Usage } };
        for (const auto& [Key, I] : M.Slots) F.emplace_back(Key, I.Builtin.empty() ? ImageNames.at(I) : I.Builtin);
        F.insert(F.end(), M.Values.begin(), M.Values.end());
        std::stable_sort(F.begin(), F.end(), [](const auto& A, const auto& B) { return KeyLess(A.first, B.first); });
        Out.push_back(Gdt::Entry(Name, "material.gdf", F, Eol));
    }
    std::map<std::string, const Image*> Images;
    for (const auto& [I, Name] : ImageNames) Images[Name] = &I;
    for (const auto& [Name, I] : Images)
    {
        Gdt::Fields F = { { "baseImage", Gdt::Path(BaseImageOf(I->File)) }, { "compressionMethod", I->Compression },
                          { "coreSemantic", I->Core }, { "imageType", "Texture" }, { "semantic", I->Semantic },
                          { "streamable", "1" }, { "type", "image" } };
        if (I->FromAlpha) F.emplace_back("fromAlpha", "1");
        std::stable_sort(F.begin(), F.end(), [](const auto& A, const auto& B) { return KeyLess(A.first, B.first); });
        Out.push_back(Gdt::Entry(Name, "image.gdf", F, Eol));
    }
    return Out;
}

std::string Builder::NamesJson() const
{
    nlohmann::ordered_json Names = nlohmann::ordered_json::object(), Twice = nlohmann::ordered_json::object();
    for (const auto& [Name, M] : Defined) Names[Name] = M.Source;
    for (const auto& [Name, Sources] : Collisions) Twice[Name] = std::vector<std::string>(Sources.begin(), Sources.end());
    const nlohmann::ordered_json Root{ { "materials", Names }, { "collisions", Twice } };
    return Root.dump(1, '\t', false, nlohmann::ordered_json::error_handler_t::replace) + "\n";
}

namespace
{
    // XMODEL_EXPORT's bone count: one is a rigid model
    bool Rigid(const fs::path& File)
    {
        std::ifstream In(File);
        std::string Line;
        for (int K = 0; K < 64 && std::getline(In, Line); K++)
            if (Line.rfind("NUMBONES ", 0) == 0) return std::atoi(Line.c_str() + 9) <= 1;
        return true;
    }

    std::string Generic(const fs::path& P) { return P.generic_string(); }
}

void WriteForModels(const DiskGame::Context& C, DiskGame::Totals& T)
{
    const fs::path Out(C.OutRoot);
    const fs::path Models = Out / DiskGame::KindFolder(DiskGame::Kind::Model);
    const std::string Folder = Out.filename().string();
    // <out> copied to <BO3>/model_export/graywolf/<folder>/: its files below the BO3 root
    const fs::path Root = fs::path("model_export") / "graywolf" / Folder;
    const auto Below = [&](const fs::path& File) {
        std::error_code Ec;
        const fs::path R = fs::relative(File, Out, Ec);
        return Ec || R.empty() || *R.begin() == ".." ? std::string() : Generic(R);
    };
    // the files the GDT names, below <out>: what a mod tools tree gets (ModToolsTree.h)
    std::set<std::string> Named;
    Builder B([&](const fs::path& File) {
        const std::string R = Below(File);
        if (!R.empty()) Named.insert(R);
        return R.empty() ? std::string() : Generic(Root / R);
    }, "material_gdt_");

    std::vector<fs::path> Sidecars;
    std::error_code Ec;
    for (fs::recursive_directory_iterator It(Models, Ec), End; !Ec && It != End; It.increment(Ec))
    {
        const std::string Name = It->path().filename().string();
        if (It->is_regular_file(Ec) && Name.size() > 15 && Name.compare(Name.size() - 15, 15, ".materials.json") == 0) Sidecars.push_back(It->path());
    }
    std::sort(Sidecars.begin(), Sidecars.end());
    // the models' XMODEL_EXPORT files by model folder, LOD by LOD
    std::map<fs::path, std::vector<fs::path>> Xmodels;
    const std::regex Lod(R"(_lod(\d+)$)", std::regex::icase);
    for (const fs::path& S : Sidecars)
    {
        Materials::Sidecar Side;
        if (!Materials::ReadSidecar(S.string(), Side))
        {
            T.Counters["material_gdt_sidecars_unread"]++;
            continue;
        }
        T.Counters["material_gdt_sidecars"]++;
        for (const auto& D : Side.Materials) B.Add(D, S.parent_path());
        const std::string Stem = S.filename().string().substr(0, S.filename().string().size() - 15);
        const fs::path X = S.parent_path() / (Stem + ".XMODEL_EXPORT");
        if (std::regex_search(Stem, Lod) && fs::is_regular_file(X, Ec)) Xmodels[S.parent_path()].push_back(X);
    }
    for (const auto& [K, N] : B.Counters()) T.Counters[K] += N;
    if (B.Empty()) return;

    std::vector<std::string> Entries = B.Entries("\r\n");
    // an xmodel per model folder: its LODs (the first four) by number
    static const char* LodKeys[] = { "filename", "mediumLod", "lowLod", "lowestLod" };
    std::vector<std::string> ModelEntries;
    for (auto& [Dir, Files] : Xmodels)
    {
        std::sort(Files.begin(), Files.end(), [&](const fs::path& A, const fs::path& Bp) {
            std::smatch Ma, Mb;
            const std::string Sa = A.stem().string(), Sb = Bp.stem().string();
            std::regex_search(Sa, Ma, Lod);
            std::regex_search(Sb, Mb, Lod);
            return std::stoi(Ma[1]) < std::stoi(Mb[1]);
        });
        const std::string Name = AssetName(Dir.filename().string());
        if (Name.empty()) continue;
        Gdt::Fields F;
        for (size_t K = 0; K < Files.size() && K < 4; K++)
        {
            const std::string R = Below(Files[K]);
            if (R.empty()) continue;
            F.emplace_back(LodKeys[K], Gdt::Path(Generic(fs::path("graywolf") / Folder / R)));
            Named.insert(R);
        }
        if (F.empty()) continue;
        F.emplace_back("type", Rigid(Files.front()) ? "rigid" : "animated");
        ModelEntries.push_back(Gdt::Entry(Name, "xmodel.gdf", F));
        T.Counters["material_gdt_xmodels"]++;
    }
    Entries.insert(Entries.begin(), ModelEntries.begin(), ModelEntries.end());

    // mod_tools: the GDT in the tree's source_data, the files it names where it says (<out> as
    // model_export/graywolf/<folder>/)
    const std::string Tree = ModToolsTree::RootOf(C);
    const fs::path Dir = Tree.empty() ? Out / "source_data" : fs::path(Tree) / "source_data";
    fs::create_directories(Dir, Ec);
    const fs::path File = Dir / ("graywolf_" + AssetName(Folder) + ".gdt");
    const std::string Text = Gdt::Document(Entries), Names = B.NamesJson();
    if (!Tree.empty())
    {
        for (const std::string& R : Named) ModToolsTree::Place(Out / fs::path(R), fs::path(Tree) / Root / fs::path(R));
        if (!ModToolsTree::WriteKept(File, Text))
        {
            T.Fail("", DiskGame::Kind::Model, File.filename().string(), "not written in the mod tools tree (nor the one there set aside)");
            return;
        }
        if (!ExportWriters::WriteBytes(fs::path(File).replace_extension(".names.json"), reinterpret_cast<const uint8_t*>(Names.data()), Names.size()))
            T.Fail("", DiskGame::Kind::Model, File.filename().string(), "names write failed");
        else
            T.Counters["material_gdt_files"]++;
        return;
    }
    if (!ExportWriters::WriteBytes(File, reinterpret_cast<const uint8_t*>(Text.data()), Text.size()) ||
        !ExportWriters::WriteBytes(fs::path(File).replace_extension(".names.json"), reinterpret_cast<const uint8_t*>(Names.data()), Names.size()))
        T.Fail("", DiskGame::Kind::Model, File.filename().string(), "write failed");
    else
        T.Counters["material_gdt_files"]++;
}

bool ModelFilesUseAssetNames()
{
    return UseAssetNames.load();
}

AssetNamesInModelFiles::AssetNamesInModelFiles(bool On) : Was(UseAssetNames.exchange(On))
{
}

AssetNamesInModelFiles::~AssetNamesInModelFiles()
{
    UseAssetNames.store(Was);
}
}
