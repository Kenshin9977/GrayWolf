#include "stdafx.h"

// One asset the live readers read, written in the formats the settings ask for (LiveExport.h).

#include "LiveExport.h"
#include "ExportWriters.h"

#include "CoDAssets.h"
#include "ICoDGame.h"
#include "CoDXAnimTranslator.h"
#include "CoDXModelTranslator.h"
#include "Strings.h"
#include "Image.h"
#include "Sound.h"
#include "BinaryWriter.h"
#include "SettingsManager.h"
#include "WraithAnim.h"

#include "SEAnimExport.h"
#include "SEModelExport.h"
#include "MayaExport.h"
#include "XMEExport.h"
#include "XNALaraExport.h"
#include "ValveSMDExport.h"
#include "OBJExport.h"
#include "XAnimRawExport.h"
#include "XMBExport.h"
#include "GLTFExport.h"
#include "CastExport.h"
#include "MaterialDescription.h"
#include "MaterialGdt.h"
#include "CastMaterials.h"

#include <filesystem>
#include <format>
#include <fstream>
#include <map>
#include <mutex>
#include <set>
#include <unordered_map>

namespace
{
    // Image Hashes from Black Ops 3's Techsets
    std::map<uint32_t, std::string> SemanticHashes =
    {
        { 0xA0AB1041, "colorMap" },
        { 0xB34B914B, "minimapMask" },
        { 0x369DEAC4, "colorMapSampler1" },
        { 0x12B6046F, "aberrationMask" },
        { 0xD90F4DC7, "gridTexture" },
        { 0x120E7A31, "uvOffsetTexture" },
        { 0x738502A8, "warpMap" },
        { 0x9E7BA008, "maskMap" },
        { 0x961141F1, "offsetMap" },
        { 0x4C094D3E, "irisTexture" },
        { 0x8FAE02B , "warpTexture" },
        { 0x1055275D, "lookupTexture" },
        { 0x7EF460A8, "blockNoiseTexture" },
        { 0x34D849D5, "revealMap" },
        { 0x264F0732, "diffuseMap" },
        { 0xE3C58D6C, "highlightMap" },
        { 0x56FF7ACA, "vignetteMap" },
        { 0x59D30D0F, "normalMap" },
        { 0xD78EC38E, "hexagonMap" },
        { 0x996BD23D, "irisMap" },
        { 0x2177C7B4, "lensFlareMap" },
        { 0x5A9A5201, "blurMaskMap" },
        { 0xDE799F3C, "warpMaskMap" },
        { 0xA6F70E30, "durdenMap" },
        { 0xF12B5F40, "pulseTexture" },
        { 0xE8515E3F, "distortionMask" },
        { 0xB1B4E3D7, "distortionMap" },
        { 0x665A1B6D, "scanlineMap" },
        { 0x29B553E1, "widgetMap1" },
        { 0x29B553E2, "widgetMap2" },
        { 0x29B553E3, "widgetMap3" },
        { 0x29B553E4, "widgetMap4" },
        { 0x29B553E5, "widgetMap5" },
        { 0x973D1967, "staticTexture" },
        { 0xA6DBAABD, "blurMask" },
        { 0x892E3B8B, "maskTexture" },
        { 0x50DA1DE9, "specHighlightMap" },
        { 0xCB13F18C, "normalTexture" },
        { 0xD5058185, "octogonMask" },
        { 0x6663034A, "noDamage" },
        { 0x4B87DB55, "lightDamage" },
        { 0xA9714557, "blockNoise" },
        { 0x7EAF2270, "lookup2" },
        { 0x69D4E61B, "spaceTexture" },
        { 0x75CD4B2 , "spaceAnusTexture" },
        { 0x9294363B, "pantherVagTexture" },
        { 0x6FB3CFCF, "lightningTexture" },
        { 0x7C86392D, "sparkleTexture" },
        { 0x3E013F6F, "faceTexture1" },
        { 0x3E013F6C, "faceTexture2" },
        { 0x369DEAC7, "colorMapSampler2" },
        { 0x369DEAC6, "colorMapSampler3" },
        { 0x9CD45BE1, "extraCamSampler" },
        { 0xFE195021, "noiseTexture" },
        { 0x3C19872 , "washTexture" },
        { 0x439044D6, "revealTexture" },
        { 0xEC84E459, "scubaTexture" },
        { 0xCDF5594E, "rivuletWarpTexture" },
        { 0x4E22BBB3, "rivuletRevealTexture" },
        { 0xF92FE655, "uvRevealTexture" },
        { 0x3FA0721A, "twinkleMap" },
        { 0x817A829B, "wormMap" },
        { 0x5BDF2D54, "distortionTexture" },
        { 0xB60D18E9, "colorMask" },
        { 0x77B50536, "bloodMotesMap" },
        { 0x389DD40F, "thermalHeatmap" },
        { 0x7176BF2 , "aoMap" },
        { 0x6D0A6C98, "glossMap" },
        { 0xE9817F0D, "glossMap" },
        { 0xEC443804, "specColorMap" },
        { 0x9A97E4B1, "customizeMask" },
        { 0xEB529B4D, "detailMap" },
        { 0x34614347, "emissiveMap" },
        { 0x95DF2A73, "detailNormal1" },
        { 0x95DF2A70, "detailNormal2" },
        { 0x95DF2A71, "detailNormal3" },
        { 0x95DF2A76, "detailNormal4" },
        { 0xD4A24996, "detailNormalMask" },
        { 0x53A3FCE2, "flickerLookupMap" },
        { 0xC089ABAF, "emissiveMask" },
        { 0x199A03D3, "tintMask" },
        { 0x2335512A, "thicknessMap" },
        { 0x55A604DC, "detailMap1" },
        { 0x55A604DF, "detailMap2" },
        { 0x80951342, "colorMapDetail2" },
        { 0x4429A80 , "mixMap" },
        { 0x4CDA7E01, "tintMask2" },
        { 0x56B697BB, "glossMapDetail2" },
        { 0x4ED66430, "specularMapDetail2" },
        { 0x338272BC, "alphaMaskMap" },
        { 0x6592A6E , "flowMap" },
        { 0x73502222, "noiseMap" },
        { 0xF79C1C0E, "rippleMap" },
        { 0x511B2AE1, "transitionDiffuse" },
        { 0x864EA29C, "transitionNormal" },
        { 0xF5276B0B, "transitionGloss" },
        { 0x5265433E, "sparkleDataMap" },
        { 0xA301628 , "alphaMap" },
        { 0xFB28D077, "image0" },
        { 0xFB28D076, "image1" },
        { 0xFB28D075, "image2" },
        { 0xFB28D074, "image3" },
        { 0xB598DFF4, "revealTextureB" },
        { 0x1CD5B065, "randomTextureA" },
        { 0x1CD5B066, "randomTextureB" },
        { 0x1CD5B067, "randomTextureC" },
        { 0x49F0DA7F, "distortMap" },
        { 0xFAF906C6, "gridTextureA" },
        { 0xFAF906C5, "gridTextureB" },
        { 0xFAF906C4, "gridTextureC" },
        { 0xFAF906C3, "gridTextureD" },
        { 0x1436343A, "heatLookup" },
        { 0xBE8B269C, "sceneLookup" },
        { 0xA60FE460, "uvOffsetDetailTexture" },
        { 0xB7A07713, "colorDetailTexture" },
        { 0xE625304 , "fireMap" },
        { 0xED590D65, "hudMap" },
        { 0x1EFD70AA, "colorRemapMap" },
        { 0xC67D666E, "shadowMaskMap" },
        { 0x81054C3A, "ditherMap" },
        { 0x5C800242, "smaaMap" },
        { 0x6FB66F22, "velveteenMask" },
        { 0xF7C2DB12, "tintBlendMask" },
        { 0x77151208, "camoMaskMap" },
        { 0xC0F0BF5F, "normalBodyMap" },
        { 0x1B768248, "glossBodyMap" },
        { 0xE86F76D9, "specColorMapThick" },
        { 0x6C246337, "underFfuseMap" },
        { 0xE57FFAA , "glossMap2" },
        { 0x1DE8CDAC, "cavityMap" },
        { 0xA41C9B9F, "styleMaskMap" },
        { 0xC80B7D0A, "colorSwatch1Map" },
        { 0xC807C8E9, "colorSwatch2Map" },
        { 0xBAEAD204, "normalSwatch1Map" },
        { 0xBAE92E67, "normalSwatch2Map" },
        { 0x656DD3F2, "specSwatch1Map" },
        { 0x656E3ED1, "specSwatch2Map" },
        { 0xE55690F3, "glossSwatch1Map" },
        { 0xE5583490, "glossSwatch2Map" },
        { 0xC089AC16, "emissiveMap1" },
        { 0xC089AC15, "emissiveMap2" },
        { 0xC089AC14, "emissiveMap3" },
        { 0x77B02241, "colorMap00" },
        { 0x4C596633, "flagRippleDetailMap" },
        { 0xD8BD9CD3, "decalMap" },
        { 0xA96FDB44, "crackMap" },
        { 0x39EC02D7, "crackNormalMap" },
        { 0x2BE9AF4 , "baseColorMap" },
        { 0x4CDA7E02, "tintMask1" },
        { 0x46FE0E6E, "baseTintMap" },
        { 0x55A604DE, "detailMap3" },
        { 0x80951343, "colorMapDetail3" },
        { 0x4E82BD61, "rColorRamp" },
        { 0xCCF4C850, "foamBase" },
        { 0xAE5C4C5 , "treadHeightMap" },
        { 0x50108D4D, "camoDetailMap" },
        { 0x8C22E816, "coneLUTMap" },
        { 0xFC80F332, "bentNormalMap" },
        { 0xA52C26B4, "specOcclusionMap" },
        { 0xC408965C, "stretchNormal" },
        { 0x6DFCC8D5, "compressNormal" },
        { 0x8F9CAD22, "cloudLayer0" },
        { 0xE566B555, "cloudMask0" },
        { 0x8F9CAD23, "cloudLayer1" },
        { 0xE566B554, "cloudMask1" },
        { 0x56B697BA, "glossMapDetail3" },
        { 0x4ED66431, "specularMapDetail3" },
        { 0x8C95EAB1, "mixMap1" },
        { 0xEE97A158, "clearcoatGlossMap" },
        { 0x8AD0A39B, "metalFlakeNormalMap" },
        { 0x2563EA9C, "metalFlakeMaskMap" },
        { 0x9783E4CD, "wobbleMap" },
        { 0xD6B59E75, "emissiveFlowMap" },
        { 0xCBF21CFB, "extracamTexture1" },
        { 0xCBF21CF8, "extracamTexture2" },
        { 0xCBF21CF9, "extracamTexture3" },
        { 0xCBF21CFE, "extracamTexture4" },
        { 0x8875EE6 , "breakUpMap" },
        { 0x74589F70, "diamondMask" },
        { 0x797F4F5C, "outlineMap" },
        { 0x5032DBE0, "alphaMask" },
        { 0x273482B9, "edgeFadeMap" },
        { 0xD28662DB, "specularMask" },
        { 0x64343DB8, "characterColorMap" },
        { 0xA7527356, "characterNormalMap" },
        { 0xE007460C, "characterRevealMap" },
        { 0x70DE0F82, "edgeColorMap" },
        { 0xDE36D64 , "edgeEmissiveMap" },
        { 0xC1EF31A8, "veinMap" },
        { 0x5E5425B9, "FutureMap" },
        { 0xA1712D0D, "ui3dSampler_C1_P0" },
        { 0xD2EEEEB8, "FontTextutre" },
        { 0x2A6C8C86, "OverlayMap" },
        { 0x85511979, "resolvedScene_C4_P0" },
        { 0x6F691B32, "codeTexture0_C3_P0" },
        { 0x6F645F19, "codeTexture0_C8_P0" },
        { 0x6F666E95, "codeTexture0_C4_P0" },
        { 0xCFED92EA, "Reveal_Map" },
        { 0xF039EC2D, "Diffuse_Map" },
        { 0xE98A255D, "AddMap" },
        { 0xB17777AC, "CompassMap" },
        { 0x3D4F14  , "Mask" },
        { 0x48FF11AE, "Diffuse" },
        { 0xF4E39943, "resolvedPostSun_C11_P0" },
        { 0x32F87F8 , "floatZSampler_C63_P0" },
        { 0x740FE37F, "fontCache" },
        { 0x1BB49   , "EKG" },
        { 0x8B4B7DF2, "resolvedPostSun_C1_P0" },
        { 0xBC83639B, "LightDamange" },
        { 0xF12FF2CE, "DpadTexture" },
        { 0x5F77D83E, "Noise_Texture" },
        { 0xF452E202, "Lookup" },
        { 0xE2AA34C0, "DayMap" },
        { 0x3D3E13  , "Mesh" },
        { 0x6519A44A, "heatmapSampler_C9_P0" },
        { 0x77F67259, "Overlay_Map" },
        { 0xAD1D0190, "Grain_Map" },
        { 0xFABDFA1A, "FadeMap" },
        { 0x6520EB80, "heatmapSampler_C3_P0" },
        { 0x9E2B786E, "C004_File" },
        { 0xAE7C5C1A, "HexagonPattern" },
        { 0xD657AB6E, "PlusTile" },
        { 0x44341ED2, "ScrollTexture" },
        { 0x3CEA479A, "sonarColorSampler_C1_P0" },
        { 0x589C508E, "ScrollTextureMap" },
        { 0x9D55EDBB, "GridTextureMap" },
        { 0x83F6B06F, "floatZSampler_C2_P0" },
        { 0x4969081A, "Static_Noise_Map" },
        { 0x8C128212, "LineMap" },
        { 0x855022BF, "resolvedScene_C2_P0" },
        { 0x7CF86BE , "Noise" },
        { 0xAC16CC94, "Wireframe" },
        { 0x79C6367 , "Image" },
        { 0xDF1738FD, "BackgroundGlow" },
        { 0x1E0097C9, "postEffect1_C4_P0" },
        { 0x1D23D48C, "postEffect0_C0_P0" },
        { 0x813B3FF , "Smoke" },
        { 0x942CBFF0, "Normal_Map" },
        { 0x55A04772, "Detail_Map" },
        { 0x8389EAF0, "PieChart" },
        { 0xE700765E, "LookupMap" },
        { 0xF5095EA4, "resolvedPostSun_C34_P0" },
        { 0x1D21496F, "postEffect0_C3_P0" },
        { 0xDE89DA4B, "TickMarkMaterial" },
        { 0x3E2C14  , "Tile" },
        { 0xA172C0E8, "ui3dSampler_C4_P0" },
        { 0x721D7116, "Vignette" },
        { 0x51CF2F85, "BlurredTexture" },
        { 0xC6CAE542, "ColorTexture" },
        { 0x82A61F22, "YUV_Image" },
        { 0x6F634657, "codeTexture0_C6_P0" },
        { 0x4E07583B, "transColorMap" },
        { 0xAB07D475, "transNormalMap" },
        { 0x9A8739AF, "transRevealMap" },
        { 0x1782DE2 , "transGlossMap" },
        { 0x95B48AE5, "meltRevealMap" },
        { 0x1DCB003F, "meltNormalMap" },
        { 0xB607C0FE, "Color_Map" },
        { 0x34ECCCB3, "specularMap" },
        { 0x6001F931, "occlusionMap" }
    };

    // Images already written, keyed by file name, so a texture shared by several models is decoded
    // and encoded once and copied afterwards (only while "skipprevimg" is on)
    std::mutex ExportedImageMutex;
    std::unordered_map<std::string, std::string> ExportedImages;

    bool ShouldExportAnim(const std::string& ExportPath)
    {
        std::error_code Ec;
        auto SkipPrevAnims = SettingsManager::GetSetting("skipprevanim") == "true";

        if (!SkipPrevAnims)
            return true;

        bool Result = false;

        if (SettingsManager::GetSetting("export_directxanim") == "true" && !std::filesystem::is_regular_file(ExportPath, Ec))
            Result = true;
        if (SettingsManager::GetSetting("export_seanim") == "true" && !std::filesystem::is_regular_file(ExportPath + ".seanim", Ec))
            Result = true;
        if (SettingsManager::GetSetting("export_castanim") == "true" && !std::filesystem::is_regular_file(ExportPath + ".cast", Ec))
            Result = true;

        return Result;
    }

    // Expected: the model's materials sidecar, when described. Its files all there are not enough to
    // skip it: a sidecar that differs (materials read better since) rewrites the model too, whose
    // .cast carries the same material slots.
    bool ShouldExportModel(const std::string& ExportPath, const Materials::Sidecar* Expected = nullptr)
    {
        std::error_code Ec;
        auto SkipPrevModels = SettingsManager::GetSetting("skipprevmodel") == "true";

        if (!SkipPrevModels)
            return true;

        bool Result = Expected != nullptr && !Materials::SidecarHolds(ExportPath + ".materials.json", *Expected);

        if (SettingsManager::GetSetting("export_xmexport") == "true" && !std::filesystem::is_regular_file(ExportPath + ".XMODEL_EXPORT", Ec))
            Result = true;
        if (SettingsManager::GetSetting("export_smd") == "true" && !std::filesystem::is_regular_file(ExportPath + ".smd", Ec))
            Result = true;
        if (SettingsManager::GetSetting("export_obj") == "true" && !std::filesystem::is_regular_file(ExportPath + ".obj", Ec))
            Result = true;
        if (SettingsManager::GetSetting("export_ma") == "true" && !std::filesystem::is_regular_file(ExportPath + ".ma", Ec))
            Result = true;
        if (SettingsManager::GetSetting("export_xna") == "true" && !std::filesystem::is_regular_file(ExportPath + ".mesh.ascii", Ec))
            Result = true;
        if (SettingsManager::GetSetting("export_semodel") == "true" && !std::filesystem::is_regular_file(ExportPath + ".semodel", Ec))
            Result = true;
        if (SettingsManager::GetSetting("export_gltf") == "true" && !std::filesystem::is_regular_file(ExportPath + ".gltf", Ec))
            Result = true;
        if (SettingsManager::GetSetting("export_glb") == "true" && !std::filesystem::is_regular_file(ExportPath + ".glb", Ec))
            Result = true;
        if (SettingsManager::GetSetting("export_castmdl") == "true" && !std::filesystem::is_regular_file(ExportPath + ".cast", Ec))
            Result = true;
        return Result;
    }

    void ExportMaterialImageNames(const XMaterial_t& Material, const std::string& ExportPath)
    {
        try
        {
            auto ImageNamesPath = (std::filesystem::path(ExportPath) / (Material.MaterialName + "_images.txt")).string();

            std::ofstream ImageNames(ImageNamesPath);
            ImageNames << "semantic,image_name\n";
            for (auto& Image : Material.Images)
            {
                if (SemanticHashes.find(Image.SemanticHash) != SemanticHashes.end())
                {
                    ImageNames << std::format("{},{}\n", SemanticHashes[Image.SemanticHash].c_str(), Image.ImageName.c_str());
                }
                else
                {
                    ImageNames << std::format("unk_semantic_0x{:X},{}\n", Image.SemanticHash, Image.ImageName.c_str());
                }
            }

            if (Material.Settings.size() > 0)
            {
                auto SettingsPath = (std::filesystem::path(ExportPath) / (Material.MaterialName + "_settings.txt")).string();

                std::ofstream Settings(SettingsPath);

                Settings << std::format("# Material: {}\n", Material.MaterialName.c_str());
                Settings << std::format("# Techset/Type: {}\n", Material.TechsetName.c_str());
                Settings << "name,type,x,y,z,w\n";

                for (auto& Setting : Material.Settings)
                {
                    Settings << std::format("{},{},{:f},{:f},{:f},{:f}",
                        Setting.Name.c_str(),
                        Setting.Type.c_str(),
                        Setting.Data[0],
                        Setting.Data[1],
                        Setting.Data[2],
                        Setting.Data[3]) << '\n';
                }
            }
        }
        catch (...)
        {
        }
    }

    void ExportMaterialImages(const XMaterial_t& Material, const std::string& ImagesPath, const std::string& ImageExtension, ImageFormat ImageFormatType)
    {
        for (auto& Image : Material.Images)
        {
            std::error_code Ec;
            auto FullImagePath = (std::filesystem::path(ImagesPath) / (Image.ImageName + ImageExtension)).string();
            auto SkipPrevImages = SettingsManager::GetSetting("skipprevimg") == "true";
            if (!std::filesystem::is_regular_file(FullImagePath, Ec) || !SkipPrevImages)
            {
                // Each model writes textures to its own folder, so the FileExists check above can't
                // catch a texture already decoded for another model - copy the first result instead.
                if (SkipPrevImages)
                {
                    const auto CacheKey = Image.ImageName + ImageExtension;

                    std::string Source;
                    {
                        std::lock_guard<std::mutex> Lock(ExportedImageMutex);
                        auto Found = ExportedImages.find(CacheKey);
                        if (Found != ExportedImages.end())
                        {
                            Source = Found->second;
                        }
                    }

                    if (!Source.empty() && Source != FullImagePath && std::filesystem::is_regular_file(Source, Ec))
                    {
                        std::filesystem::copy_file(Source, FullImagePath, std::filesystem::copy_options::overwrite_existing, Ec);

                        // Falls through to a full conversion if the copy did not take
                        if (std::filesystem::is_regular_file(FullImagePath, Ec))
                        {
                            continue;
                        }
                    }
                }

                std::unique_ptr<XImageDDS> ImageData = CoDAssets::GameXImageHandler(Image);

                if (ImageData != nullptr)
                {
                    if (ImageFormatType == ImageFormat::DDS_WithHeader)
                    {
                        // Since this can throw, wrap it in an exception handler
                        try
                        {
                            auto Writer = BinaryWriter();
                            Writer.Create(FullImagePath);
                            Writer.Write((const int8_t*)ImageData->DataBuffer, ImageData->DataSize);
                        }
                        catch (...)
                        {
                            // Nothing, this means that something is already accessing the image
                        }
                    }
                    else
                    {
                        // Convert it, this method is a nothrow
                        Image::ConvertImageMemory(ImageData->DataBuffer, ImageData->DataSize, ImageFormat::DDS_WithHeader, FullImagePath, ImageFormatType, ImageData->ImagePatchType);
                    }

                    // Remember it so the next model that wants it can copy rather than redo
                    if (std::filesystem::is_regular_file(FullImagePath, Ec))
                    {
                        std::lock_guard<std::mutex> Lock(ExportedImageMutex);
                        ExportedImages.emplace(Image.ImageName + ImageExtension, FullImagePath);
                    }
                }
            }
        }
    }

    // The model in every format the settings ask for; Materials: its materials as Cast slots, for the .cast;
    // Described: its materials as the game's adapter describes them (their colour factor, for glTF)
    void WriteModelFiles(const std::unique_ptr<WraithModel>& Model, const std::string& ExportPath,
                         const std::vector<CastMaterialSlots>* Materials = nullptr,
                         const std::vector<::Materials::Description>* Described = nullptr)
    {
        if (Described != nullptr)
            for (auto& Material : Model->Materials)
                for (const auto& D : *Described)
                    if (D.Name == Material.MaterialName) ::Materials::BaseColorFactor(D, Material.ColorFactor);
        std::ofstream Cosmetics((std::filesystem::path(ExportPath) / (Model->AssetName + "_cosmetics.mel")).string());

        for (auto& Bone : Model->Bones)
        {
            if (Bone.IsCosmetic)
            {
                Cosmetics << std::format("select -add {};\n", Bone.TagName.c_str());
            }
        }

        // Note: the mod tools formats below (XME/XMB) and SMD are always exported in game
        // units (inches), the "unit" setting only applies to the DCC formats further down

        // with material_gdt, they name their materials as the material GDT defines them
        std::vector<std::string> SourceNames;
        if (MaterialGdt::ModelFilesUseAssetNames())
            for (auto& Material : Model->Materials)
            {
                SourceNames.push_back(Material.MaterialName);
                Material.MaterialName = MaterialGdt::AssetName(Material.MaterialName);
            }
        if (SettingsManager::GetSetting("export_xmexport") == "true")
        {
            CodXME::ExportXME(*Model.get(), (std::filesystem::path(ExportPath) / (Model->AssetName + ".XMODEL_EXPORT")).string());
        }
        if (SettingsManager::GetSetting("export_xmbin") == "true")
        {
            CodXMB::ExportXMB(*Model.get(), (std::filesystem::path(ExportPath) / (Model->AssetName + ".XMODEL_BIN")).string());
        }
        for (size_t i = 0; i < SourceNames.size(); i++) Model->Materials[i].MaterialName = SourceNames[i];
        if (SettingsManager::GetSetting("export_smd") == "true")
        {
            ValveSMD::ExportSMD(*Model.get(), (std::filesystem::path(ExportPath) / (Model->AssetName + ".smd")).string());
        }

        // The following formats are scaled, unless the user asked for game units (inches)
        if (SettingsManager::GetSetting("unit", "Centimeter") != "Inch")
        {
            Model->ScaleModel(2.54f);
        }

        if (SettingsManager::GetSetting("export_obj") == "true")
        {
            WavefrontOBJ::ExportOBJ(*Model.get(), (std::filesystem::path(ExportPath) / (Model->AssetName + ".obj")).string());
        }
        if (SettingsManager::GetSetting("export_ma") == "true")
        {
            Maya::ExportMaya(*Model.get(), (std::filesystem::path(ExportPath) / (Model->AssetName + ".ma")).string());
        }
        if (SettingsManager::GetSetting("export_xna") == "true")
        {
            XNALara::ExportXNA(*Model.get(), (std::filesystem::path(ExportPath) / (Model->AssetName + ".mesh.ascii")).string());
        }
        // glTF is in metres: its scene root scales the model's unit (and turns Z up into Y up)
        const float GltfMetres = SettingsManager::GetSetting("unit", "Centimeter") == "Inch" ? 0.0254f : 0.01f;
        if (SettingsManager::GetSetting("export_gltf") == "true")
        {
            GLTF::ExportGLTF(*Model.get(), (std::filesystem::path(ExportPath) / (Model->AssetName + ".gltf")).string(), false, false, GltfMetres);
        }
        if (SettingsManager::GetSetting("export_glb") == "true")
        {
            GLTF::ExportGLTF(*Model.get(), (std::filesystem::path(ExportPath) / (Model->AssetName + ".glb")).string(), false, true, GltfMetres);
        }
        if (SettingsManager::GetSetting("export_semodel") == "true")
        {
            SEModel::ExportSEModel(*Model.get(), (std::filesystem::path(ExportPath) / (Model->AssetName + ".semodel")).string());
        }
        if (SettingsManager::GetSetting("export_castmdl") == "true")
        {
            Cast::ExportCastModel(*Model.get(), (std::filesystem::path(ExportPath) / (Model->AssetName + ".cast")).string(), false, Materials);
        }
    }

    std::unique_ptr<XAnim_t> ReadAnim(const CoDAnim_t* Animation)
    {
        if (auto* Adapter = CoDAssets::Game)
        {
            return Adapter->ReadXAnim(Animation);
        }

        return nullptr;
    }

    // The LOD's materials as the game's adapter describes them: written as <model file>.materials.json
    // so a preview or another tool reads them without knowing the game (MaterialDescription.h), and
    // handed to the .cast writer as its material slots.
    Materials::Sidecar DescribeMaterials(const XModelLod_t& Lod)
    {
        const Materials::IAdapter& Adapter = Materials::AdapterFor(CoDAssets::Game != nullptr ? CoDAssets::Game->Engine() : "");
        Materials::Sidecar S;
        S.Game = S.Adapter = Adapter.Name();
        std::set<std::string> Seen;
        for (const auto& Mat : Lod.Materials)
        {
            Materials::Source Src;
            Src.Name = Strings::Replace(Mat.MaterialName, "@", "_");   // as the model file names it
            if (!Seen.insert(Src.Name).second) continue;
            Src.Techset = Mat.TechsetName;
            Src.Constants = Mat.Constants;
            Src.SortKey = Mat.SortKey;
            for (const auto& Image : Mat.Images)
            {
                Materials::Role Hint = Materials::Role::Unknown;
                switch (Image.ImageUsage)
                {
                case ImageUsageType::DiffuseMap:  Hint = Materials::Role::Albedo; break;
                case ImageUsageType::NormalMap:   Hint = Materials::Role::Normal; break;
                case ImageUsageType::SpecularMap: Hint = Materials::Role::Specular; break;
                case ImageUsageType::GlossMap:    Hint = Materials::Role::Gloss; break;
                default: break;
                }
                Src.Images.push_back({ Image.ImageName, Hint, Image.SemanticHash });
            }
            S.Materials.push_back(Adapter.Describe(Src));
            S.Sources.push_back(std::move(Src));
        }
        return S;
    }
    // A LOD's described materials, for its files and its sidecar: a description that fails never
    // costs the model its export (Slots stays empty, the .cast keeps its colour/normal/specular)
    struct DescribedLod
    {
        Materials::Sidecar Sidecar;
        std::vector<CastMaterialSlots> Slots;
        bool Ok = false;
        const std::vector<CastMaterialSlots>* CastSlots() const { return Ok ? &Slots : nullptr; }
    };
    DescribedLod DescribeLod(const XModelLod_t& Lod)
    {
        DescribedLod D;
        try
        {
            D.Sidecar = DescribeMaterials(Lod);
            D.Slots = Materials::CastSlots(D.Sidecar.Materials);
            D.Ok = true;
        }
        catch (const std::exception&) {}
        return D;
    }
    void WriteSidecar(const DescribedLod& D, const std::string& ModelFileBase)
    {
        if (!D.Ok) return;
        try { Materials::WriteSidecar(ModelFileBase + ".materials.json", D.Sidecar); }
        catch (const std::exception&) {}
    }
}

void LiveExport::ForgetWrittenImages()
{
    std::lock_guard<std::mutex> Lock(ExportedImageMutex);
    ExportedImages.clear();
}

std::unique_ptr<XModel_t> LiveExport::ReadModel(const CoDModel_t* Model)
{
    if (auto* Adapter = CoDAssets::Game)
    {
        return Adapter->ReadXModel(Model);
    }

    return nullptr;
}

ExportGameResult LiveExport::ExportAnim(const CoDAnim_t* Animation, const std::string& ExportPath)
{
    if (!ShouldExportAnim((std::filesystem::path(ExportPath) / Animation->AssetName).string()))
        return ExportGameResult::Success;

    std::unique_ptr<XAnim_t> GenericAnimation = ReadAnim(Animation);

    if (GenericAnimation != nullptr)
    {
        auto Result = CoDXAnimTranslator::TranslateXAnim(GenericAnimation);

        if (Result != nullptr)
        {
            ExportWraithAnim(*Result, ExportPath);
        }
        else
        {
            return ExportGameResult::UnknownError;
        }
    }
    else
    {
        return ExportGameResult::UnknownError;
    }

    return ExportGameResult::Success;
}

ExportGameResult LiveExport::ExportWraithAnim(WraithAnim& Anim, const std::string& ExportPath)
{
    // Note: DirectXAnim below is always exported in game units (inches), the
    // "unit" setting only applies to the DCC formats further down

    if (SettingsManager::GetSetting("export_directxanim") == "true")
    {
        auto XAnimVer = (SettingsManager::GetSetting("directxanim_ver") == "17") ? XAnimRawVersion::WorldAtWar : XAnimRawVersion::BlackOps;
        XAnimRaw::ExportXAnimRaw(Anim, (std::filesystem::path(ExportPath) / Anim.AssetName).string(), XAnimVer);
    }

    // The following formats are scaled, unless the user asked for game units (inches)
    if (SettingsManager::GetSetting("unit", "Centimeter") != "Inch")
    {
        Anim.ScaleAnimation(2.54f);
    }

    if (SettingsManager::GetSetting("export_seanim") == "true")
    {
        SEAnim::ExportSEAnim(Anim, (std::filesystem::path(ExportPath) / (Anim.AssetName + ".seanim")).string());
    }
    if (SettingsManager::GetSetting("export_castanim") == "true")
    {
        Cast::ExportCastAnim(Anim, (std::filesystem::path(ExportPath) / (Anim.AssetName + ".cast")).string());
    }
    return ExportGameResult::Success;
}

ExportGameResult LiveExport::ExportWraithModel(std::unique_ptr<WraithModel>& Model, const std::string& ExportPath,
                                                const Materials::Sidecar* Sidecar)
{
    if (Model == nullptr) return ExportGameResult::UnknownError;
    std::vector<CastMaterialSlots> Slots;
    bool Described = false;
    if (Sidecar != nullptr)
    {
        try
        {
            Slots = Materials::CastSlots(Sidecar->Materials);
            Described = true;
        }
        catch (const std::exception&) {}
    }
    if (!ShouldExportModel((std::filesystem::path(ExportPath) / Model->AssetName).string(), Described ? Sidecar : nullptr))
        return ExportGameResult::Success;
    WriteModelFiles(Model, ExportPath, Described ? &Slots : nullptr, Described ? &Sidecar->Materials : nullptr);
    if (Described)
    {
        try { Materials::WriteSidecar((std::filesystem::path(ExportPath) / Model->AssetName).string() + ".materials.json", *Sidecar); }
        catch (const std::exception&) {}
    }
    return ExportGameResult::Success;
}

namespace
{
    // The textures of every LOD's materials written (or handed to TakeImages) and named as the
    // model files refer to them
    void PrepareTextures(XModel_t& GenericModel, const std::string& ExportPath, const std::string& ImagesPath,
                         const std::string& ImageRelativePath, const std::string& ImageExtension, const LiveExport::ImageTaker* TakeImages,
                         const LiveExport::MaterialFiller* FillMaterials);
}

ExportGameResult LiveExport::ExportModel(const CoDModel_t* Model, const std::string& ExportPath, const std::string& ImagesPath, const std::string& ImageRelativePath, const std::string& ImageExtension,
                                             const ImageTaker* TakeImages, const MaterialFiller* FillMaterials)
{
    std::unique_ptr<XModel_t> GenericModel = ReadModel(Model);
    if (GenericModel != nullptr) PrepareTextures(*GenericModel, ExportPath, ImagesPath, ImageRelativePath, ImageExtension, TakeImages, FillMaterials);
    if (GenericModel != nullptr)
    {
        if (SettingsManager::GetSetting("exportalllods") == "true")
        {
            auto LodCount = (uint32_t)GenericModel->ModelLods.size();

            for (uint32_t i = 0; i < LodCount; i++)
            {
                const DescribedLod Described = DescribeLod(GenericModel->ModelLods[i]);
                if (ShouldExportModel((std::filesystem::path(ExportPath) / (Model->AssetName + std::format("_LOD{}", i))).string(),
                                      Described.Ok ? &Described.Sidecar : nullptr))
                {
                    auto Result = CoDXModelTranslator::TranslateXModel(GenericModel, i);

                    Result->AssetName += std::format("_LOD{}", i);

                    if (Result != nullptr)
                    {
                        WriteModelFiles(Result, ExportPath, Described.CastSlots(), Described.Ok ? &Described.Sidecar.Materials : nullptr);
                        WriteSidecar(Described, (std::filesystem::path(ExportPath) / Result->AssetName).string());
                    }
                    else
                    {
                        return ExportGameResult::UnknownError;
                    }
                }
            }
        }
        else
        {
            const auto BiggestLodIndex = CoDXModelTranslator::CalculateBiggestLodIndex(GenericModel);

            if (BiggestLodIndex > -1)
            {
                std::string LodIndexSuffix = "_LOD0";

                // If we are using the "match game lod index" setting
                if (SettingsManager::GetSetting("match_game_lod_index", "false") == "true")
                {
                    LodIndexSuffix = std::format("_LOD{}", BiggestLodIndex);
                }

                const DescribedLod Described = DescribeLod(GenericModel->ModelLods[BiggestLodIndex]);
                if (ShouldExportModel((std::filesystem::path(ExportPath) / (Model->AssetName + LodIndexSuffix)).string(),
                                      Described.Ok ? &Described.Sidecar : nullptr))
                {
                    const auto Result = CoDXModelTranslator::TranslateXModel(GenericModel, BiggestLodIndex);

                    Result->AssetName += LodIndexSuffix;

                    if (Result != nullptr)
                    {
                        WriteModelFiles(Result, ExportPath, Described.CastSlots(), Described.Ok ? &Described.Sidecar.Materials : nullptr);
                        WriteSidecar(Described, (std::filesystem::path(ExportPath) / Result->AssetName).string());
                    }
                    else
                    {
                        return ExportGameResult::UnknownError;
                    }
                }
            }
            else
            {
                // no LOD: the model holds no geometry to write (AW's vm_sn6_nocamo: no LOD, no bone)
                return ExportGameResult::Placeholder;
            }
        }

        if (SettingsManager::GetSetting("exporthitbox") == "true")
        {
            std::unique_ptr<WraithModel> Result = nullptr;
            if (auto* Adapter = CoDAssets::Game)
            {
                Result = Adapter->TranslateHitboxModel(GenericModel);
            }

            if (Result != nullptr)
            {
                Result->AssetName += "_HITBOX";
                WriteModelFiles(Result, ExportPath);
            }
        }
    }
    else
    {
        return ExportGameResult::UnknownError;
    }

    return ExportGameResult::Success;
}

ExportGameResult LiveExport::ExportCompositeModel(const std::string& Name, const std::vector<CoDModel_t>& Parts, const std::string& ExportPath,
                                                  const std::string& ImagesPath, const std::string& ImageRelativePath,
                                                  const std::string& ImageExtension, const ImageTaker* TakeImages, size_t* PartsRead,
                                                  const MaterialFiller* FillMaterials)
{
    // each part's largest LOD, its bones joined by name (a bone of two parts is one bone: the part's
    // own skeleton is the composite's, its rig on the same tags), its materials by name
    auto Merged = std::make_unique<WraithModel>();
    Merged->AssetName = Name + "_LOD0";
    Materials::Sidecar Sidecar;
    std::vector<CastMaterialSlots> Slots;
    bool Described = true;
    std::unordered_map<std::string, uint32_t> BoneOf, MaterialOf;
    size_t Read = 0;
    for (const CoDModel_t& Part : Parts)
    {
        std::unique_ptr<XModel_t> Generic = ReadModel(&Part);
        if (Generic == nullptr) continue;
        PrepareTextures(*Generic, ExportPath, ImagesPath, ImageRelativePath, ImageExtension, TakeImages, FillMaterials);
        const int32_t Lod = CoDXModelTranslator::CalculateBiggestLodIndex(Generic);
        if (Lod < 0) continue;
        const auto Mesh = CoDXModelTranslator::TranslateXModel(Generic, (uint32_t)Lod);
        if (Mesh == nullptr) continue;
        Read++;
        const DescribedLod D = DescribeLod(Generic->ModelLods[(size_t)Lod]);
        Described = Described && D.Ok && D.Slots.size() == Mesh->Materials.size() && D.Sidecar.Materials.size() == Mesh->Materials.size();
        std::vector<uint32_t> Bones(Mesh->Bones.size());
        for (size_t B = 0; B < Mesh->Bones.size(); B++)
        {
            const WraithBone& Bone = Mesh->Bones[B];
            if (const auto It = BoneOf.find(Bone.TagName); It != BoneOf.end())
            {
                Bones[B] = It->second;
                continue;
            }
            Bones[B] = (uint32_t)Merged->Bones.size();
            BoneOf.emplace(Bone.TagName, Bones[B]);
            WraithBone Copy = Bone;
            Copy.BoneParent = Bone.BoneParent >= 0 && (size_t)Bone.BoneParent < B ? (int32_t)Bones[(size_t)Bone.BoneParent] : -1;
            Merged->Bones.push_back(Copy);
        }
        std::vector<int32_t> Materials(Mesh->Materials.size());
        for (size_t M = 0; M < Mesh->Materials.size(); M++)
        {
            const auto [It, New] = MaterialOf.try_emplace(Mesh->Materials[M].MaterialName, (uint32_t)Merged->Materials.size());
            Materials[M] = (int32_t)It->second;
            if (!New) continue;
            Merged->Materials.push_back(Mesh->Materials[M]);
            if (Described && M < D.Slots.size() && M < D.Sidecar.Materials.size())
            {
                Slots.push_back(D.Slots[M]);
                Sidecar.Materials.push_back(D.Sidecar.Materials[M]);
                if (M < D.Sidecar.Sources.size()) Sidecar.Sources.push_back(D.Sidecar.Sources[M]);
            }
            if (Sidecar.Game.empty()) Sidecar.Game = D.Sidecar.Game, Sidecar.Adapter = D.Sidecar.Adapter;
        }
        for (WraithSubmesh& Sub : Mesh->Submeshes)
        {
            for (WraithVertex& V : Sub.Verticies)
                for (WraithVertexWeight& W : V.Weights)
                    if (W.BoneIndex < Bones.size()) W.BoneIndex = Bones[W.BoneIndex];
            for (int32_t& M : Sub.MaterialIndicies)
                if (M >= 0 && (size_t)M < Materials.size()) M = Materials[(size_t)M];
            Merged->Submeshes.push_back(std::move(Sub));
        }
    }
    if (PartsRead != nullptr) *PartsRead = Read;
    if (Read == 0) return ExportGameResult::UnknownError;
    if (!ShouldExportModel((std::filesystem::path(ExportPath) / Merged->AssetName).string(), Described ? &Sidecar : nullptr))
        return ExportGameResult::Success;
    WriteModelFiles(Merged, ExportPath, Described ? &Slots : nullptr);
    if (Described)
    {
        try { Materials::WriteSidecar((std::filesystem::path(ExportPath) / Merged->AssetName).string() + ".materials.json", Sidecar); }
        catch (const std::exception&) {}
    }
    return ExportGameResult::Success;
}

namespace
{
void PrepareTextures(XModel_t& GenericModel, const std::string& ExportPath, const std::string& ImagesPath,
                     const std::string& ImageRelativePath, const std::string& ImageExtension, const LiveExport::ImageTaker* TakeImages,
                     const LiveExport::MaterialFiller* FillMaterials)
{
    XModel_t* const Generic = &GenericModel;
    auto ImageFormatType = ImageFormat::Standard_PNG;
    auto ImageSetting = SettingsManager::GetSetting("exportimg", "PNG");
    auto ExportImages = SettingsManager::GetSetting("exportmodelimg") == "true";
    auto ExportImageNames = SettingsManager::GetSetting("exportimgnames") == "true";
    auto ExportMaterialFolders = SettingsManager::GetSetting("mdlmtlfolders") == "true" && TakeImages == nullptr;

    if (ExportImages)
    {
        std::error_code Ec;
        std::filesystem::create_directories(ImagesPath, Ec);
    }

    if (ImageSetting == "DDS")
    {
        ImageFormatType = ImageFormat::DDS_WithHeader;
    }
    else if (ImageSetting == "TGA")
    {
        ImageFormatType = ImageFormat::Standard_TGA;
    }
    else if (ImageSetting == "TIFF")
    {
        ImageFormatType = ImageFormat::Standard_TIFF;
    }

    {
        for (auto& LOD : Generic->ModelLods)
        {
            for (auto& Material : LOD.Materials)
            {
                if (FillMaterials != nullptr && Material.Images.empty()) (*FillMaterials)(Material);
                auto CompleteImagesPath = ImagesPath;
                auto CompleteImageRelativePath = ImageRelativePath;

                if (ExportMaterialFolders && ExportImages)
                {
                    std::error_code Ec;
                    CompleteImagesPath = (std::filesystem::path(ImagesPath) / Material.MaterialName).string();
                    CompleteImageRelativePath = (std::filesystem::path(ImageRelativePath) / Material.MaterialName).string() + "\\\\";
                    std::filesystem::create_directories(CompleteImagesPath, Ec);
                }

                if (ExportImageNames)
                {
                    ExportMaterialImageNames(Material, ExportPath);
                }
                if (ExportImages && TakeImages != nullptr)
                {
                    // The caller writes the textures: it may rename one, or drop it
                    auto& Images = Material.Images;
                    Images.erase(std::remove_if(Images.begin(), Images.end(), [&](XImage_t& Image) { return !(*TakeImages)(Image); }), Images.end());
                }
                else if (ExportImages)
                {
                    ExportMaterialImages(Material, CompleteImagesPath, ImageExtension, ImageFormatType);
                }

                for (auto& Image : Material.Images)
                {
                    // Append the relative path and image extension here, since we are done with these images
                    Image.ImageName = CompleteImageRelativePath + Image.ImageName + ImageExtension;
                }
            }
        }

    }
}
}

ExportGameResult LiveExport::ExportImage(const CoDImage_t* Image, const std::string& ExportPath, const std::string& ImageExtension)
{
    std::error_code Ec;
    auto FullImagePath = (std::filesystem::path(ExportPath) / (Image->AssetName + ImageExtension)).string();
    auto SkipPrevImages = SettingsManager::GetSetting("skipprevimg") == "true";

    if (!std::filesystem::is_regular_file(FullImagePath, Ec) || !SkipPrevImages)
    {
        std::unique_ptr<XImageDDS> ImageData = nullptr;

        if (Image->IsFileEntry)
        {
            if (auto* Adapter = CoDAssets::Game)
            {
                ImageData = Adapter->ReadFileImage(Image);
            }
        }
        else
        {
            if (auto* Adapter = CoDAssets::Game)
            {
                ImageData = Adapter->ReadXImage(Image);
            }
        }

        auto ImageFormatType = ImageFormat::Standard_PNG;
        auto ImageSetting = SettingsManager::GetSetting("exportimg", "PNG");

        if (ImageSetting == "DDS")
        {
            ImageFormatType = ImageFormat::DDS_WithHeader;
        }
        else if (ImageSetting == "TGA")
        {
            ImageFormatType = ImageFormat::Standard_TGA;
        }
        else if (ImageSetting == "TIFF")
        {
            ImageFormatType = ImageFormat::Standard_TIFF;
        }

        if (ImageData != nullptr)
        {
            if (ImageFormatType == ImageFormat::DDS_WithHeader)
            {
                // Since this can throw, wrap it in an exception handler
                try
                {
                    auto Writer = BinaryWriter();
                    Writer.Create(FullImagePath);
                    Writer.Write((const int8_t*)ImageData->DataBuffer, ImageData->DataSize);
                }
                catch (...)
                {
                    // Nothing, this means that something is already accessing the image
                }
            }
            else
            {
                // Convert it, this method is a nothrow
                Image::ConvertImageMemory(ImageData->DataBuffer, ImageData->DataSize, ImageFormat::DDS_WithHeader, FullImagePath, ImageFormatType, ImageData->ImagePatchType);
            }
        }
    }

    return ExportGameResult::Success;
}

ExportGameResult LiveExport::ExportSound(const CoDSound_t* Sound, const std::string& ExportPath, const std::string& SoundExtension)
{
    std::error_code Ec;
    auto FullSoundPath = (std::filesystem::path(ExportPath) / (Sound->AssetName + SoundExtension)).string();
    auto SkipPrevSound = SettingsManager::GetSetting("skipprevsound") == "true";

    if (!std::filesystem::is_regular_file(FullSoundPath, Ec) || !SkipPrevSound)
    {
        std::unique_ptr<XSound> SoundData = nullptr;

        if (auto* Adapter = CoDAssets::Game)
        {
            SoundData = Adapter->ReadXSound(Sound);
        }

        // nothing read, or nothing written: a failure the run reports (not a sound counted written)
        if (SoundData == nullptr) return ExportGameResult::UnknownError;
        const bool AsRead = (CoDAssets::Game != nullptr && CoDAssets::Game->SoundsAsRead()) || SoundData->DataSize == 0;
        const bool Written = AsRead
            // the game's bytes as they are (World at War's; an empty sound an empty file, which a
            // caller may count as a failure)
            ? ExportWriters::WriteBytes(FullSoundPath, reinterpret_cast<const uint8_t*>(SoundData->DataBuffer), SoundData->DataSize)
            : ExportWriters::WriteSound(ExportWriters::SourceOf(*SoundData), FullSoundPath, ExportWriters::SoundFormatSetting());
        if (!Written) return ExportGameResult::UnknownError;
    }

    return ExportGameResult::Success;
}
