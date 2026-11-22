#include "stdafx.h"

#include "MaterialGdt.h"

#include "AlphaRule.h"
#include "DiskGame.h"
#include "ExportNaming.h"
#include "ExportWriters.h"
#include "GdtWriter.h"
#include "ModelImages.h"
#include "ModToolsTree.h"

#include "DirectXTex.h"
#include <nlohmann/json.hpp>
#include <stb_image.h>
#include <stb_image_write.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <execution>
#include <fstream>
#include <functional>
#include <future>
#include <mutex>
#include <regex>
#include <optional>
#include <set>
#include <tuple>

namespace fs = std::filesystem;

namespace MaterialGdt
{
// The image work Add does (a colour map's alpha read, a map rewritten beside it), each done once
// per file, by the first Builder or thread to ask, the others waiting for it: AddAll does it on
// every core, then adds in order, each image's answer already there.
struct Builder::Work
{
    template <class T>
    class Once
    {
    public:
        template <class Fn>
        T Get(const std::string& Key, Fn&& Make)
        {
            std::promise<T> Mine;
            std::shared_future<T> Result;
            bool First = false;
            {
                const std::lock_guard<std::mutex> G(Lock);
                auto [It, New] = Done.try_emplace(Key);
                if (New)
                {
                    It->second = Mine.get_future().share();
                    First = true;
                }
                Result = It->second;
            }
            if (First)
            {
                try { Mine.set_value(Make()); }
                catch (...) { Mine.set_exception(std::current_exception()); }
            }
            return Result.get();
        }

    private:
        std::mutex Lock;
        std::map<std::string, std::shared_future<T>> Done;
    };
    Once<std::optional<AlphaTally>> Alphas;   // by the colour map read
    Once<int> Files;                          // by the file written beside its source
};

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

    // Whether a map's file is one of BO3's code images (named as it is in a slot)
    bool IsStockCodeImage(const Materials::Map& Map)
    {
        if (Map.Empty()) return false;
        const std::string Stem = ExportNaming::Lower(fs::path(Map.Path).stem().string());
        return std::find(std::begin(StockCodeImages), std::end(StockCodeImages), Stem) != std::end(StockCodeImages);
    }

    // A linear colour as the GDT's colour fields hold one: sRGB, "r g b 1"
    std::string SrgbFloats3(const float Linear[3])
    {
        float C[3];
        for (int K = 0; K < 3; K++)
        {
            const double L = std::clamp((double)Linear[K], 0.0, 1.0);
            C[K] = (float)(L <= 0.0031308 ? L * 12.92 : 1.055 * std::pow(L, 1.0 / 2.4) - 0.055);
            C[K] = std::round(C[K] * 1e6f) / 1e6f;
        }
        return Floats3(C);
    }

    bool TiffOrPng(const fs::path& File)
    {
        const std::string E = ExportNaming::Lower(File.extension().string());
        return E == ".tif" || E == ".tiff" || E == ".png";
    }

    // A colour map's alpha tallied, the file read with stb (the preview's reader goes through WIC,
    // which wants COM on the thread); nothing when it has no alpha channel or does not read
    std::optional<AlphaTally> TallyAlpha(const fs::path& File)
    {
        int W = 0, H = 0, N = 0;
        stbi_uc* Px = stbi_load(File.string().c_str(), &W, &H, &N, 4);
        if (Px == nullptr) return std::nullopt;
        AlphaTally T;
        for (size_t I = 0; I < (size_t)W * H; I++) T.Add(Px[I * 4 + 3]);
        stbi_image_free(Px);
        if (N != 4) return std::nullopt;
        return T;
    }

    // A colour map's tally, read once
    std::optional<AlphaTally> TallyOnce(Builder::Work& W, const fs::path& File)
    {
        return W.Alphas.Get(File.string(), [&] { return TallyAlpha(File); });
    }

    // Whether a colour map's alpha masks something (AlphaTally)
    bool AlphaMasks(Builder::Work& W, const fs::path& File)
    {
        const auto T = TallyOnce(W, File);
        return T && T->Masks();
    }

    // Whether a blend layer's colour alpha shapes it (a stain's soft edge as much as a cutout):
    // false for one all but opaque, or all but clear (an alpha the source's shader did not read
    // as an opacity, which would hide the layer)
    bool AlphaShapes(Builder::Work& W, const fs::path& File)
    {
        const auto T = TallyOnce(W, File);
        return T && T->Seen && T->Solid * 100 < T->Seen * 99 && T->Clear * 100 < T->Seen * 99;
    }

    // Whether a colour map's alpha is all but opaque (99% of its texels): a blend through it is opaque
    bool AlphaSolid(Builder::Work& W, const fs::path& File)
    {
        const auto T = TallyOnce(W, File);
        return T && T->Seen && T->Solid * 100 >= T->Seen * 99;
    }

    // An image under 4 texels a side (MW2019's 1x1 constant colours) as a 4x4 one beside it
    // (<stem>_4x4.png, kept when newer): BO3's linker calls a smaller one missing. False: the
    // image as it is.
    bool GrownAs(const fs::path& From, const fs::path& To)
    {
        int W = 0, H = 0, N = 0;
        if (ExportNaming::Lower(From.extension().string()) != ".png" || !stbi_info(From.string().c_str(), &W, &H, &N) || (W >= 4 && H >= 4))
            return false;
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
    bool Grown(Builder::Work& Once, const fs::path& From, fs::path& To)
    {
        To = fs::path(From).replace_filename(From.stem().string() + "_4x4.png");
        return Once.Files.Get(To.string(), [&] { return GrownAs(From, To) ? 1 : 0; }) != 0;
    }

    // A normal map packed with two channels (AG: x in alpha, y in green, DXT5nm, CoD4's; XY: x in
    // red, y in green) as BO3's normalMap reads one, x, y, z in r, g, b: written to To, kept when
    // newer than From. Read as it is, the packed file tilts every normal (z about 0.5), which
    // lights a surface in dark speckles. Y stays as the source game has it (both CoD). The file
    // has the last word over the description: grey in r, g, b with the alpha varying is AG
    // whatever Enc says (CoD4's adapter leaves its normals at the XY default).
    bool UnpackNormalAs(const fs::path& From, const fs::path& To, Materials::NormalEncoding Enc, bool& AsAg)
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
    // once per file written (1: unpacked, 2: as AG whatever Enc said, the file grey with an alpha)
    bool UnpackNormal(Builder::Work& Once, const fs::path& From, const fs::path& To, Materials::NormalEncoding Enc, bool& AsAg)
    {
        const int Done = Once.Files.Get(To.string(), [&] {
            bool Ag = false;
            if (!UnpackNormalAs(From, To, Enc, Ag)) return 0;
            return Ag && Enc != Materials::NormalEncoding::AG ? 2 : 1;
        });
        AsAg = Done == 2 || Enc == Materials::NormalEncoding::AG;
        return Done != 0;
    }

    // From rewritten texel by texel into To (Channels of each, kept when newer): Texel gets the
    // source's rgba and writes the new texel. False: From unreadable or To unwritten.
    bool Rewritten(const fs::path& From, const fs::path& To, int Channels, const std::function<void(const stbi_uc*, stbi_uc*)>& Texel)
    {
        std::error_code Ec, Ec2;
        if (fs::is_regular_file(To, Ec) && fs::last_write_time(To, Ec) >= fs::last_write_time(From, Ec2) && !Ec && !Ec2) return true;
        int W = 0, H = 0, N = 0;
        stbi_uc* Px = stbi_load(From.string().c_str(), &W, &H, &N, 4);
        if (Px == nullptr) return false;
        std::vector<stbi_uc> Out((size_t)W * H * Channels);
        for (size_t I = 0; I < (size_t)W * H; I++) Texel(Px + I * 4, Out.data() + I * Channels);
        stbi_image_free(Px);
        return stbi_write_png(To.string().c_str(), W, H, Channels, Out.data(), W * Channels) != 0;
    }

    // An image the BO3 converter does not read, as a PNG beside it (<stem>.png, kept when newer).
    // The linker reads TIFF, PNG and DDS with the old header (the mod tools' stock GDTs name 2362
    // .tif, 782 .dds, 66 .png), not the DX10-header DDS GrayWolf writes (BC7, BC4, the sRGB
    // formats: "format not supported", then "image ... is missing": 695 of 976 on a Nacht
    // usermap). The top level decoded as stored: an sRGB format's bytes as they are (the GDT's
    // semantic says sRGB), a signed one (BC5_SNORM normals) moved to 0..255, one channel as grey.
    // TGA and the rest through stb. The helpers below read the PNG too (stb reads no DDS).
    bool DecodeDds(const fs::path& From, int& W, int& H, std::vector<uint8_t>& Px)
    {
        namespace Dx = DirectX;
        Dx::TexMetadata M;
        Dx::ScratchImage Img, Plain;
        if (FAILED(Dx::LoadFromDDSFile(From.wstring().c_str(), Dx::DDS_FLAGS_NONE, &M, Img)) || Img.GetImage(0, 0, 0) == nullptr) return false;
        const Dx::Image* Top = Img.GetImage(0, 0, 0);
        const DXGI_FORMAT F = M.format;
        const bool Signed = F == DXGI_FORMAT_BC4_SNORM || F == DXGI_FORMAT_BC5_SNORM || F == DXGI_FORMAT_R8G8_SNORM ||
                            F == DXGI_FORMAT_R8_SNORM || F == DXGI_FORMAT_R8G8B8A8_SNORM || F == DXGI_FORMAT_R16G16_SNORM ||
                            F == DXGI_FORMAT_R16_SNORM || F == DXGI_FORMAT_R16G16B16A16_SNORM;
        const bool Grey = F == DXGI_FORMAT_BC4_UNORM || F == DXGI_FORMAT_BC4_SNORM || F == DXGI_FORMAT_BC4_TYPELESS ||
                          F == DXGI_FORMAT_R8_UNORM || F == DXGI_FORMAT_R8_SNORM || F == DXGI_FORMAT_R16_UNORM ||
                          F == DXGI_FORMAT_R16_SNORM || F == DXGI_FORMAT_R16_FLOAT || F == DXGI_FORMAT_R32_FLOAT;
        // the bytes as stored: an sRGB format to 8-bit sRGB (no gamma applied between the two)
        const DXGI_FORMAT Want = Signed ? DXGI_FORMAT_R8G8B8A8_SNORM : Dx::IsSRGB(F) ? DXGI_FORMAT_R8G8B8A8_UNORM_SRGB : DXGI_FORMAT_R8G8B8A8_UNORM;
        const HRESULT Hr = Dx::IsCompressed(F) ? Dx::Decompress(*Top, Want, Plain)
                         : F == Want           ? Plain.InitializeFromImage(*Top)
                                               : Dx::Convert(*Top, Want, Dx::TEX_FILTER_DEFAULT, Dx::TEX_THRESHOLD_DEFAULT, Plain);
        if (FAILED(Hr) || Plain.GetImage(0, 0, 0) == nullptr) return false;
        const Dx::Image& P = *Plain.GetImage(0, 0, 0);
        W = (int)P.width;
        H = (int)P.height;
        Px.resize((size_t)W * H * 4);
        for (int Y = 0; Y < H; Y++)
        {
            const uint8_t* Row = P.pixels + (size_t)Y * P.rowPitch;
            uint8_t* Out = Px.data() + (size_t)Y * W * 4;
            for (int X = 0; X < W * 4; X++)
                Out[X] = Signed && (X & 3) != 3 ? (uint8_t)std::clamp(std::lround(((int8_t)Row[X] / 127.0 * 0.5 + 0.5) * 255.0), 0L, 255L) : Row[X];
            if (Signed)   // the alpha of a signed format: 1 where it has none, else its value
                for (int X = 3; X < W * 4; X += 4) Out[X] = (uint8_t)std::clamp(std::lround(std::max((int8_t)Row[X] / 127.0, 0.0) * 255.0), 0L, 255L);
            if (Grey)
                for (int X = 0; X < W * 4; X += 4) Out[X + 1] = Out[X + 2] = Out[X];
        }
        return true;
    }
    bool ConverterReadableAs(const fs::path& From, const fs::path& To)
    {
        std::error_code Ec, Ec2;
        if (fs::is_regular_file(To, Ec) && fs::last_write_time(To, Ec) >= fs::last_write_time(From, Ec2) && !Ec && !Ec2) return true;
        int W = 0, H = 0;
        std::vector<uint8_t> Px;
        if (ExportNaming::Lower(From.extension().string()) == ".dds")
        {
            if (!DecodeDds(From, W, H, Px)) return false;
        }
        else
        {
            int N = 0;
            stbi_uc* P = stbi_load(From.string().c_str(), &W, &H, &N, 4);
            if (P == nullptr) return false;
            Px.assign(P, P + (size_t)W * H * 4);
            stbi_image_free(P);
        }
        // three channels when the alpha is all opaque (half the file, and the converter's "no alpha")
        bool Opaque = true;
        for (size_t I = 3; I < Px.size() && Opaque; I += 4) Opaque = Px[I] == 255;
        if (Opaque)
        {
            for (size_t I = 0, O = 0; I < Px.size(); I += 4, O += 3) Px[O] = Px[I], Px[O + 1] = Px[I + 1], Px[O + 2] = Px[I + 2];
            Px.resize((size_t)W * H * 3);
        }
        const int C = Opaque ? 3 : 4;
        return stbi_write_png(To.string().c_str(), W, H, C, Px.data(), W * C) != 0;
    }
    // The map's file as one the converter reads: Map.Path redirected to the PNG; false when it could
    // not be written (the map left as it is)
    bool ConverterReadable(Builder::Work& Once, const fs::path& Dir, Materials::Map& Map)
    {
        if (Map.Empty() || IsStockCodeImage(Map)) return true;
        const fs::path From = (Dir / fs::path(Map.Path)).lexically_normal();
        if (TiffOrPng(From)) return true;
        std::error_code Ec;
        if (!fs::is_regular_file(From, Ec)) return true;   // counted missing by Slot
        const fs::path To = fs::path(From).replace_extension(".png");
        if (Once.Files.Get(To.string(), [&] { return ConverterReadableAs(From, To) ? 1 : 0; }) == 0) return false;
        Map.Path = (fs::path(Map.Path).parent_path() / To.filename()).generic_string();
        return true;
    }

    // A roughness map's channel as the gloss BO3's cosinePowerMap reads, 255 - value, grey beside
    // it (<stem>_gloss<r|g|b|a>.png). Black Ops' specular alpha is such a roughness: the BO3 gloss
    // of the same textures in Treyarch's ports is 255 - alpha (r = -0.95 on Zombies Chronicles).
    bool GlossFromRoughness(Builder::Work& Once, const fs::path& From, int Channel, fs::path& To)
    {
        To = fs::path(From).replace_filename(From.stem().string() + "_gloss" + "rgba"[Channel & 3] + ".png");
        return Once.Files.Get(To.string(), [&] {
            return Rewritten(From, To, 3, [Channel](const stbi_uc* In, stbi_uc* Out) { Out[0] = Out[1] = Out[2] = (stbi_uc)(255 - In[Channel & 3]); }) ? 1 : 0;
        }) != 0;
    }

    // A gamma 2.0 colour map (Black Ops, BO2) as sRGB beside it (<stem>_srgb.png), alpha kept:
    // srgb((c / 255)^2), as Treyarch's BO3 ports of the same textures (3.8 levels off per texel
    // against 9.1 for the file as it is, Zombies Chronicles)
    bool SrgbFromGamma2(Builder::Work& Once, const fs::path& From, fs::path& To)
    {
        To = fs::path(From).replace_filename(From.stem().string() + "_srgb.png");
        static const std::array<stbi_uc, 256> Table = [] {
            std::array<stbi_uc, 256> T{};
            for (int V = 0; V < 256; V++)
            {
                const double L = (V / 255.0) * (V / 255.0);
                const double S = L <= 0.0031308 ? L * 12.92 : 1.055 * std::pow(L, 1.0 / 2.4) - 0.055;
                T[V] = (stbi_uc)std::clamp(std::lround(S * 255.0), 0L, 255L);
            }
            return T;
        }();
        return Once.Files.Get(To.string(), [&] {
            return Rewritten(From, To, 4, [](const stbi_uc* In, stbi_uc* Out) {
                for (int K = 0; K < 3; K++) Out[K] = Table[In[K]];
                Out[3] = In[3];
            }) ? 1 : 0;
        }) != 0;
    }

    // A metal workflow's colour and metalness as the specular workflow BO3 has (lit_advanced_fullspec:
    // a diffuse colour, an F0 colour), both sRGB, beside the colour map: <stem>_m<hash>_d.png (the
    // diffuse, colour * (1 - metal), alpha kept) and <stem>_m<hash>_s.png (F0 = lerp(dielectric,
    // colour, metal)), the hash of the metalness map and channel (a colour map two materials share
    // with different metalness). The metalness is sampled at the colour map's texel (nearest).
    // Linear: metal = value, dielectric 0.04; Reflectance (WWII): Description::MetalEncoding.
    bool SpecularFromMetal(Builder::Work& Once, const fs::path& Colour, bool Gamma2, const fs::path& Metal, int Channel,
                           Materials::Description::MetalEncoding Enc, fs::path& Diffuse, fs::path& F0)
    {
        uint32_t Hash = 2166136261u;
        for (const char C : Metal.generic_string() + "#" + "rgba"[Channel & 3] + (Enc == Materials::Description::MetalEncoding::Reflectance ? "r" : "l"))
            Hash = (Hash ^ (uint8_t)C) * 16777619u;
        char Tag[16];
        std::snprintf(Tag, sizeof(Tag), "_m%08x", Hash);
        Diffuse = fs::path(Colour).replace_filename(Colour.stem().string() + Tag + "_d.png");
        F0 = fs::path(Colour).replace_filename(Colour.stem().string() + Tag + "_s.png");
        return Once.Files.Get(F0.string(), [&] {
            std::error_code Ec, Ec2, Ec3;
            const auto Newer = [&](const fs::path& P) {
                return fs::is_regular_file(P, Ec) && fs::last_write_time(P, Ec) >= fs::last_write_time(Colour, Ec2) &&
                       fs::last_write_time(P, Ec) >= fs::last_write_time(Metal, Ec3) && !Ec && !Ec2 && !Ec3;
            };
            if (Newer(Diffuse) && Newer(F0)) return 1;
            int W = 0, H = 0, N = 0, MW = 0, MH = 0, MN = 0;
            stbi_uc* C = stbi_load(Colour.string().c_str(), &W, &H, &N, 4);
            stbi_uc* Mt = stbi_load(Metal.string().c_str(), &MW, &MH, &MN, 4);
            if (C == nullptr || Mt == nullptr)
            {
                if (C) stbi_image_free(C);
                if (Mt) stbi_image_free(Mt);
                return 0;
            }
            static const auto Tables = [] {
                std::array<std::array<double, 256>, 2> T{};
                for (int V = 0; V < 256; V++)
                {
                    const double S = V / 255.0;
                    T[0][V] = S <= 0.04045 ? S / 12.92 : std::pow((S + 0.055) / 1.055, 2.4);
                    T[1][V] = S * S;
                }
                return T;
            }();
            const auto& ToLinear = Tables[Gamma2 ? 1 : 0];
            const auto ToSrgb = [](double L) {
                L = std::clamp(L, 0.0, 1.0);
                const double S = L <= 0.0031308 ? L * 12.92 : 1.055 * std::pow(L, 1.0 / 2.4) - 0.055;
                return (stbi_uc)std::clamp(std::lround(S * 255.0), 0L, 255L);
            };
            std::vector<stbi_uc> D((size_t)W * H * 4), S((size_t)W * H * 3);
            for (int Y = 0; Y < H; Y++)
                for (int X = 0; X < W; X++)
                {
                    const size_t I = (size_t)Y * W + X;
                    const size_t J = (size_t)(Y * MH / H) * MW + (size_t)(X * MW / W);
                    const double V = Mt[J * 4 + (Channel & 3)] / 255.0;
                    double Metalness = V, Dielectric = 0.04;
                    if (Enc == Materials::Description::MetalEncoding::Reflectance)
                    {
                        const double R = V * V;
                        Metalness = std::clamp((R - 0.1) / 0.9, 0.0, 1.0);
                        Dielectric = std::min(R, 0.1);
                    }
                    for (int K = 0; K < 3; K++)
                    {
                        const double L = ToLinear[C[I * 4 + K]];
                        D[I * 4 + K] = ToSrgb(L * (1.0 - Metalness));
                        S[I * 3 + K] = ToSrgb(Enc == Materials::Description::MetalEncoding::Reflectance ? Metalness * L + Dielectric
                                                                                                     : Dielectric + (L - Dielectric) * Metalness);
                    }
                    D[I * 4 + 3] = C[I * 4 + 3];
                }
            stbi_image_free(C);
            stbi_image_free(Mt);
            return stbi_write_png(Diffuse.string().c_str(), W, H, 4, D.data(), W * 4) != 0 &&
                   stbi_write_png(F0.string().c_str(), W, H, 3, S.data(), W * 3) != 0 ? 1 : 0;
        }) != 0;
    }

    // An F0 colour map (sRGB) as the mask BO3's advanced techniques lerp from 0.04 to a white tint
    // (<stem>_sm.png, grey, linear): mask = (luminance(F0) - 0.04) / 0.96, the F0's colour lost
    bool SpecMaskFromColour(Builder::Work& Once, const fs::path& From, fs::path& To)
    {
        To = fs::path(From).replace_filename(From.stem().string() + "_sm.png");
        static const std::array<double, 256> Linear = [] {
            std::array<double, 256> T{};
            for (int V = 0; V < 256; V++)
            {
                const double S = V / 255.0;
                T[V] = S <= 0.04045 ? S / 12.92 : std::pow((S + 0.055) / 1.055, 2.4);
            }
            return T;
        }();
        return Once.Files.Get(To.string(), [&] {
            return Rewritten(From, To, 3, [](const stbi_uc* In, stbi_uc* Out) {
                const double Y = 0.2126 * Linear[In[0]] + 0.7152 * Linear[In[1]] + 0.0722 * Linear[In[2]];
                Out[0] = Out[1] = Out[2] = (stbi_uc)std::clamp(std::lround((Y - 0.04) / 0.96 * 255.0), 0L, 255L);
            }) ? 1 : 0;
        }) != 0;
    }

    // A multiply colour map as the decal BO3 draws in its place (<stem>_mul.png): black, its alpha
    // 1 - the colour's linear luminance. Over a colour c, lerp(c, 0, 1 - m) = c * m, exact for a
    // grey m (BO3's decals blend into the G-buffer and have no multiply). Gamma2: the colour is
    // gamma 2.0 (MaterialDescription's AlbedoGamma), else sRGB.
    bool DarkeningFromMultiply(Builder::Work& Once, const fs::path& From, bool Gamma2, fs::path& To)
    {
        To = fs::path(From).replace_filename(From.stem().string() + "_mul.png");
        static const auto Linear = [](bool G2) {
            std::array<double, 256> T{};
            for (int V = 0; V < 256; V++)
            {
                const double S = V / 255.0;
                T[V] = G2 ? S * S : S <= 0.04045 ? S / 12.92 : std::pow((S + 0.055) / 1.055, 2.4);
            }
            return T;
        };
        static const std::array<double, 256> Tables[2] = { Linear(false), Linear(true) };
        const std::array<double, 256>& T = Tables[Gamma2 ? 1 : 0];
        return Once.Files.Get(To.string(), [&] {
            return Rewritten(From, To, 4, [&T](const stbi_uc* In, stbi_uc* Out) {
                const double Y = 0.2126 * T[In[0]] + 0.7152 * T[In[1]] + 0.0722 * T[In[2]];
                Out[0] = Out[1] = Out[2] = 0;
                Out[3] = (stbi_uc)std::clamp(std::lround((1.0 - Y) * 255.0), 0L, 255L);
            }) ? 1 : 0;
        }) != 0;
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
    : BaseImageOf(std::move(BaseImage)), Prefix(std::move(CounterPrefix)), NamePrefix(std::move(Names)), Done(std::make_shared<Work>())
{
}

void Builder::AddAll(const std::vector<Item>& Items)
{
    // the items Add would describe (the first of each asset name, Add's own test), their image
    // work done on every core by scratch builders sharing this one's Work: the same files, each
    // written once, whatever the order the threads reach them in
    std::vector<size_t> Fresh;
    std::set<std::string> Seen;
    for (const auto& [Name, M] : Defined) Seen.insert(Name);
    for (size_t I = 0; I < Items.size(); I++)
        if (!AssetName(Items[I].D->Name).empty() && Seen.insert(PrefixedName(NamePrefix, Items[I].D->Name)).second) Fresh.push_back(I);
    std::for_each(std::execution::par, Fresh.begin(), Fresh.end(), [&](size_t I) {
        Builder Scratch(BaseImageOf, Prefix, NamePrefix);
        Scratch.Done = Done;
        Scratch.Scratch = true;
        Scratch.Add(*Items[I].D, Items[I].Dir, Items[I].Layered, Items[I].BlendLayerSort);
    });
    for (const Item& I : Items) Add(*I.D, I.Dir, I.Layered, I.BlendLayerSort);
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
    Materials::Map Use = Map;
    // a value in green or blue (WWII's gloss in its specular map's b) as a grey image beside it
    // (<stem>_<g|b>.png): BO3 reads a single channel in red or alpha (fromAlpha)
    if (Single && (Map.Channel == 1 || Map.Channel == 2))
    {
        const fs::path From = (Dir / fs::path(Map.Path)).lexically_normal();
        const fs::path To = fs::path(From).replace_filename(From.stem().string() + "_" + "rgba"[Map.Channel] + ".png");
        std::error_code Ec;
        const bool Extracted = fs::is_regular_file(From, Ec) && Done->Files.Get(To.string(), [&] {
            return Rewritten(From, To, 3, [C = Map.Channel](const stbi_uc* In, stbi_uc* Out) { Out[0] = Out[1] = Out[2] = In[C]; }) ? 1 : 0;
        }) != 0;
        if (!Extracted)
        {
            Counter(std::string(Key) + "_channel_dropped")++;
            return false;
        }
        Use = { (fs::path(Map.Path).parent_path() / To.filename()).generic_string(), 0, Map.Invert };
        Counter(std::string(Key) + "_channel_extracted")++;
    }
    Image I;
    I.File = (Dir / fs::path(Use.Path)).lexically_normal();
    I.FromAlpha = Single && Use.Channel == 3;
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
    if (Scratch) return true;   // AddAll's first pass: the image work only, no image named
    if (BaseImageOf(I.File).empty())
    {
        Counter("images_outside")++;
        return false;
    }
    if (!TiffOrPng(I.File)) Counter("slots_not_tiff_png")++;
    if (fs::path Larger; Grown(*Done, I.File, Larger))
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

void Builder::Add(const Materials::Description& Source, const fs::path& Dir, bool Layered, const char* BlendLayerSort)
{
    const std::string Bare = AssetName(Source.Name);
    if (Bare.empty())
    {
        Counter("unnamed")++;
        return;
    }
    const std::string Name = PrefixedName(NamePrefix, Source.Name);
    if (const auto It = Defined.find(Name); It != Defined.end())
    {
        if (It->second.Source == Source.Name) Counter("shared")++;
        else if (Collisions[Name].insert(Source.Name).second) Counter("name_collisions")++;
        return;
    }
    // the maps a slot may name as files the converter reads (a DDS, a TGA as a PNG beside it), before
    // the work below reads them
    Materials::Description D = Source;
    {
        Materials::Map* Maps[] = { &D.Albedo, &D.Normal, &D.Roughness, &D.Metalness, &D.Specular, &D.Occlusion, &D.Emissive,
                                   &D.Transmission.Image, &D.AlphaMap.Image, D.Layers.empty() ? nullptr : &D.Layers.front().Albedo,
                                   D.Layers.empty() ? nullptr : &D.Layers.front().Roughness };
        for (Materials::Map* Map : Maps)
        {
            if (Map == nullptr || Map->Empty()) continue;
            const std::string Was = Map->Path;
            if (!ConverterReadable(*Done, Dir, *Map)) Counter("images_not_converted")++;
            else if (Map->Path != Was) Counter("images_converted_png")++;
        }
    }
    Material M;
    M.Source = D.Name;
    if (Bare != D.Name) Counter("renamed")++;

    // the colour and roughness of a layered material are its layers': the first one's
    Materials::Map Albedo = D.Albedo, Roughness = D.Roughness;
    // a multiply layer or decal has a colour only (BO1's "m1c1", "unlitdecalblend_multiply")
    if (D.Multiply) Roughness = {};
    if (!D.Layers.empty())
    {
        if (Albedo.Empty()) Albedo = D.Layers.front().Albedo;
        if (Roughness.Empty()) Roughness = D.Layers.front().Roughness;
        Counter("layered_first_layer")++;
    }
    if (!D.Tint.Empty() || !D.Color.Empty()) Counter("tint_dropped")++;
    const bool Overlay = BlendLayerSort != nullptr;   // a layer drawn over a layered material
    if (!D.AlphaMap.Empty() && !Overlay) Counter("alpha_map_dropped")++;
    // a metal workflow (BO3 has none) as its specular one: the colour's metal part moved to an F0
    // colour map, the technique then lit_advanced_fullspec. Kept dielectric, a gold or gun metal
    // would be its colour as a plastic's
    if (!D.Metalness.Empty() && D.Specular.Empty() && !Albedo.Empty() && !D.Multiply && BlendLayerSort == nullptr)
    {
        fs::path Diffuse, F0;
        if (SpecularFromMetal(*Done, (Dir / fs::path(Albedo.Path)).lexically_normal(), D.AlbedoGamma == 2.0f,
                              (Dir / fs::path(D.Metalness.Path)).lexically_normal(), D.Metalness.Channel, D.MetalEnc, Diffuse, F0))
        {
            const fs::path Parent = fs::path(Albedo.Path).parent_path();
            Albedo.Path = (Parent / Diffuse.filename()).generic_string();
            D.AlbedoGamma = 0.0f;   // written sRGB
            D.Specular = { (Parent / F0.filename()).generic_string(), 0, false };
            D.LegacySpecular = D.SpecularMask = false;
            Counter("metalness_as_specular")++;
        }
        else
            Counter("metalness_dropped")++;
    }
    else if (!D.Metalness.Empty())
        Counter("metalness_dropped")++;

    // the technique: emission, then light through the surface, then the alpha, then a colour specular
    const bool Emissive = !D.Emissive.Empty(), Backlit = !D.Transmission.Empty() && D.Transmission.Image.Invert;
    if (!D.Transmission.Empty() && !D.Transmission.Image.Invert) Counter("transmission_not_thickness")++;
    // Auto: cutout when the colour map's alpha masks something (as the preview and the glTF judge
    // it: a chain-link fence, foliage cards), else opaque
    bool AutoCut = false;
    if (D.Alpha == Materials::AlphaMode::Auto && !Albedo.Empty())
    {
        AutoCut = AlphaMasks(*Done, (Dir / fs::path(Albedo.Path)).lexically_normal());
    }
    // a blend through an opaque colour is opaque (BO1's outer ground, "b0" at sort 43, its colour's
    // alpha full), drawn transparent it would write no depth and sort with the effects
    bool Blend = D.Alpha == Materials::AlphaMode::Blend;
    if (Blend && !D.Decal && !Albedo.Empty() && AlphaSolid(*Done, (Dir / fs::path(Albedo.Path)).lexically_normal()))
    {
        Blend = false;
        Counter("blend_alpha_solid")++;
    }
    const bool Cutout = D.Alpha == Materials::AlphaMode::Cutout || AutoCut;
    // a pre-PBR specular map has no BO3 slot: Treyarch ports such materials as plain lit (BO3's
    // fixed dielectric specular), 21 of 23 of BO1's in Zombies Chronicles and 92 of 94 of their
    // t6_legacy GDTs; in the advanced techniques the map would be a metal mask (Materials.pdf)
    const bool Spec = !D.Specular.Empty() && !D.LegacySpecular;
    if (!D.Specular.Empty() && D.LegacySpecular) Counter("spec_legacy_dropped")++;
    // a specular mask (F0 = tint * map: BO3's advanced techniques) goes in the technique that reads
    // one, lit_advanced, with its tint; so does a code image of BO3's in the slot ($white_specular,
    // $specular_mask: specularMask images, which the fullspec technique refuses: the linker then
    // drops the material, "no techset for surface")
    const bool SpecMask = Spec && (D.SpecularMask || IsStockCodeImage(D.Specular));
    M.Category = "Geometry";
    if (Emissive) M.Type = "lit_emissive";
    else if (Backlit) M.Type = "lit_backlit";
    else if (Blend) M.Type = Spec ? "lit_transparent_advanced" : "lit_transparent";
    else if (Cutout) M.Type = Spec ? "lit_alphatest_advanced" : "lit_alphatest";
    else if (Spec) M.Type = SpecMask ? "lit_advanced" : "lit_advanced_fullspec";
    else M.Type = "lit";
    if (M.Type.find("advanced") != std::string::npos) M.Category = "Geometry Advanced";
    // a decal (named so, as the older games name theirs) lies on the surface it marks: BO3 draws
    // one above it only as a lit_decal, else the two fight for the depth and flicker
    const bool Decal = !Emissive && !Layered && !Overlay && (D.Decal || D.Multiply || ExportNaming::Lower(D.Name).find("decal") != std::string::npos);
    if (Layered) Counter("layered_world")++;
    if (Decal)
    {
        M.Type = "lit_decal";
        M.Category = "Decal";
        M.Usage = "decal";
        M.Values.emplace_back("layerSortDecal", "Grunge");
        Counter("decals")++;
    }
    // a blend layer as the mod tools' own (t6_legacy core.gdt's core_grass_med_blend...): a
    // lit_decal, usage terrain, its opacity its vertices' alpha times its colour's alpha (an
    // opaque colour: the weight alone), or the reveal map's in place of the colour's (material.awi:
    // "the Reveal Map will override the Color Map Alpha"), lit_decal_reveal
    bool OverlayAlpha = false;
    if (Overlay)
    {
        const bool Reveal = !D.AlphaMap.Empty();
        M.Type = Reveal ? "lit_decal_reveal" : "lit_decal";
        M.Category = "Decal";
        M.Usage = "terrain";
        M.Values.emplace_back("layerSortDecal", BlendLayerSort);
        OverlayAlpha = !Reveal && !Albedo.Empty() && (D.Multiply || AlphaShapes(*Done, (Dir / fs::path(Albedo.Path)).lexically_normal()));
        Counter("blend_layers")++;
        if (OverlayAlpha) Counter("blend_layers_colour_alpha")++;
    }
    if ((Emissive || Backlit) && (Blend || Cutout) && !Overlay) Counter("alpha_dropped")++;
    if (D.Alpha == Materials::AlphaMode::Auto && !Overlay) Counter(AutoCut ? "alpha_auto_cutout" : "alpha_auto_opaque")++;

    const bool ColourAlpha = Overlay ? OverlayAlpha : D.Multiply || D.Alpha != Materials::AlphaMode::Opaque;
    if (fs::path Mul; !Albedo.Empty() && D.Multiply)
    {
        if (DarkeningFromMultiply(*Done, (Dir / fs::path(Albedo.Path)).lexically_normal(), D.AlbedoGamma == 2.0f, Mul))
        {
            Albedo.Path = (fs::path(Albedo.Path).parent_path() / Mul.filename()).generic_string();
            Counter("multiply_as_darkening")++;
        }
        else
            Counter("multiply_not_rewritten")++;
    }
    else if (fs::path Srgb; !Albedo.Empty() && D.AlbedoGamma == 2.0f && SrgbFromGamma2(*Done, (Dir / fs::path(Albedo.Path)).lexically_normal(), Srgb))
    {
        Albedo.Path = (fs::path(Albedo.Path).parent_path() / Srgb.filename()).generic_string();
        Counter("colour_gamma2_as_srgb")++;
    }
    if (!Slot(M, "colorMap", Albedo, Dir, "diffuseMap", "sRGB3chAlpha", ColourAlpha ? "compressed high color" : "compressed no alpha"))
        Counter("no_color")++;
    if (Overlay && !D.AlphaMap.Empty()) Slot(M, "alphaRevealMap", D.AlphaMap.Image, Dir, "revealMap", "Linear1ch", "compressed");
    // a two-channel normal map as a three-channel one beside it (<stem>_xyz.png)
    Materials::Map Normal = D.Multiply ? Materials::Map() : D.Normal;
    if (!Normal.Empty() && D.NormalEnc != Materials::NormalEncoding::XYZ)
    {
        const fs::path From = (Dir / fs::path(Normal.Path)).lexically_normal();
        const fs::path To = fs::path(From).replace_filename(From.stem().string() + "_xyz.png");
        bool AsAg = false;
        if (UnpackNormal(*Done, From, To, D.NormalEnc, AsAg))
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
    // BO3's cosinePowerMap is a gloss map: a roughness map as its inverse beside it
    if (fs::path Gloss; !Roughness.Empty() && !Roughness.Invert)
    {
        if (GlossFromRoughness(*Done, (Dir / fs::path(Roughness.Path)).lexically_normal(), Roughness.Channel, Gloss))
        {
            Roughness = { (fs::path(Roughness.Path).parent_path() / Gloss.filename()).generic_string(), 0, true };
            Counter("gloss_from_roughness")++;
        }
        else
            Counter("roughness_not_gloss")++;
    }
    if (Roughness.Empty() || Roughness.Invert) Slot(M, "cosinePowerMap", Roughness, Dir, "glossMap", "Linear1ch", "compressed");
    if (Spec && !Decal && !Overlay)
    {
        // a full colour in the fullspec and emissive techniques, else the intensity mask theirs take
        const bool Colour = !SpecMask && (M.Type == "lit_advanced_fullspec" || M.Type == "lit_emissive");
        // an F0 colour where the technique takes a mask (the alpha-tested and transparent advanced
        // ones: F0 = lerp(0.04, tint, mask)) as the mask of a white tint giving its luminance: read
        // as it is, a dielectric's sRGB 0.2 (F0 0.04) would be a mask of 0.2, F0 0.23
        Materials::Map Specular = D.Specular;
        bool WhiteTint = false;
        if (fs::path Mask; !Colour && !SpecMask && SpecMaskFromColour(*Done, (Dir / fs::path(Specular.Path)).lexically_normal(), Mask))
        {
            Specular = { (fs::path(Specular.Path).parent_path() / Mask.filename()).generic_string(), 0, false };
            WhiteTint = true;
        }
        if (Slot(M, "specColorMap", Specular, Dir, Colour ? "specularMap" : "specularMask", Colour ? "sRGB3ch" : "Linear1ch", "compressed"))
        {
            if (!Colour) Counter(D.SpecularMask ? "spec_mask" : WhiteTint ? "spec_colour_as_mask" : "spec_as_mask")++;
            if (D.LegacySpecular) Counter("spec_legacy")++;
            // the mask's colour, as the GDT writes a colour (sRGB)
            if (D.SpecularMask) M.Values.emplace_back("specColorTint", SrgbFloats3(D.SpecularTint));
            else if (WhiteTint) M.Values.emplace_back("specColorTint", "1 1 1 1");
        }
    }
    Slot(M, "occMap", D.Occlusion, Dir, "occlusionMap", "Linear1ch", "compressed");
    if (Emissive) Slot(M, "colorMap00", D.Emissive, Dir, "diffuseMap", "sRGB3chAlpha", "compressed no alpha");
    if (Backlit && Slot(M, "alphaRevealMap", D.Transmission.Image, Dir, "thicknessMap", "Linear1ch", "compressed"))
        M.Values.emplace_back("backlightScatterColor", Floats3(D.Transmission.Color));
    // a gloss map's range: the source's own when it has one (BO3's glossRange, log2 of the specular
    // power: <full> spans 0..17, which made a concrete of 0..4 or a worn iron of 0..6 a mirror; the
    // other games' from the power their shaders take of the gloss, MaterialDescription.h), a pre-PBR
    // description without one (an older sidecar) as Treyarch's ports (<custom> 0..13: 223 of 259 of
    // the t6_legacy GDTs), else the full range
    if (M.Slots.count("cosinePowerMap") && D.GlossRange[0] >= 0.0f)
    {
        char B[32];
        M.Values.emplace_back("glossSurfaceType", "<custom>");
        std::snprintf(B, sizeof(B), "%g", D.GlossRange[0]);
        M.Values.emplace_back("glossRangeMin", B);
        std::snprintf(B, sizeof(B), "%g", D.GlossRange[1]);
        M.Values.emplace_back("glossRangeMax", B);
        Counter("gloss_range_source")++;
    }
    else if (M.Slots.count("cosinePowerMap") && D.LegacySpecular)
    {
        M.Values.emplace_back("glossSurfaceType", "<custom>");
        M.Values.emplace_back("glossRangeMin", "0");
        M.Values.emplace_back("glossRangeMax", "13");
    }
    else if (M.Slots.count("cosinePowerMap"))
        M.Values.emplace_back("glossSurfaceType", "<full>");
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
    // an image two slots name (one entry): with its alpha when one of them reads it (a blend
    // layer's colour, the base's colour without)
    std::map<Image, std::string> Compression;
    for (const auto& [Name, M] : Defined)
        for (const auto& [Key, I] : M.Slots)
        {
            auto [It, New] = Compression.emplace(I, I.Compression);
            if (I.Compression == "compressed high color") It->second = I.Compression;
        }
    for (const auto& [Name, I] : Images)
    {
        Gdt::Fields F = { { "baseImage", Gdt::Path(BaseImageOf(I->File)) }, { "compressionMethod", Compression.at(*I) },
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
    // every sidecar's materials, added in the sidecars' order (AddAll: their images' work on every core)
    std::vector<std::pair<Materials::Sidecar, fs::path>> Sides;
    for (const fs::path& S : Sidecars)
    {
        Materials::Sidecar Side;
        if (!Materials::ReadSidecar(S.string(), Side))
        {
            T.Counters["material_gdt_sidecars_unread"]++;
            continue;
        }
        ModelImages::ToShared(Side, S);   // a model's images/ links: the shared files, as without them
        T.Counters["material_gdt_sidecars"]++;
        Sides.emplace_back(std::move(Side), S.parent_path());
        const std::string Stem = S.filename().string().substr(0, S.filename().string().size() - 15);
        const fs::path X = S.parent_path() / (Stem + ".XMODEL_EXPORT");
        if (std::regex_search(Stem, Lod) && fs::is_regular_file(X, Ec)) Xmodels[S.parent_path()].push_back(X);
    }
    std::vector<Builder::Item> Items;
    for (const auto& [Side, Dir] : Sides)
        for (const auto& D : Side.Materials) Items.push_back({ &D, Dir });
    B.AddAll(Items);
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
    // an asset the tree's GDTs already define (the stock ones, another export's) is left to them:
    // APE and the linker refuse a second entry of the same name
    if (!Tree.empty()) T.Counters["material_gdt_already_in_tree"] += ModToolsTree::LeaveOutDefined(File, Entries);
    const std::string Text = Gdt::Document(Entries), Names = B.NamesJson();
    if (!Tree.empty())
    {
        for (const std::string& R : Named) ModToolsTree::Place(Out / fs::path(R), fs::path(Tree) / Root / fs::path(R));
        if (std::string Why; !ModToolsTree::WriteKept(File, Text, &Why))
        {
            T.Fail("", DiskGame::Kind::Model, File.filename().string(), "not written in the mod tools tree, " + Why);
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
