#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <functional>
#include <set>
#include <vector>

#include "CoDAssetType.h"

class WraithModel;

class GameWorldWar2
{
public:
    static std::unique_ptr<XAnim_t> ReadXAnim(const CoDAnim_t* Animation);
    static std::unique_ptr<XModel_t> ReadXModel(const CoDModel_t* Model);
    static std::unique_ptr<XSound> ReadXSound(const CoDSound_t* Sound);
    // The first N bytes of a sound's data as ReadXSound reads them (the zone's, or its package's),
    // before any header it adds: a stored FLAC file's STREAMINFO, read without its samples
    static std::vector<uint8_t> ReadSoundHead(const CoDSound_t* Sound, size_t N);
    static std::string SoundPackagePath(const CoDSound_t* Sound);
    // The language folder (french) a localized sound's package is read from while a zone's
    // sounds are written: <folder>/<prefix>soundfile<N>.pak under the package cache's path. ""
    // takes the first language folder the install has.
    static thread_local std::string SoundLanguage;
    // A zone's language folder: the one its .ff is in (french/fra_x.ff), else the one its name's
    // prefix names (fra_x.ff); "" for a zone of no language
    static std::string LanguageOfZone(const std::string& FastFilePath);
    // The install a zone belongs to: the folder of its .ff, or the one above when that is a
    // language folder
    static std::string InstallOfZone(const std::string& FastFilePath);
    static const XMaterial_t ReadXMaterial(uint64_t MaterialPointer);

    static std::unique_ptr<XImageDDS> LoadResidentXImage(const XImage_t& Image);
    static void LoadXModel(const std::unique_ptr<XModel_t>& Model, const XModelLod_t& ModelLOD, const std::unique_ptr<WraithModel>& ResultModel);

    // An alias's sound files (loaded, primed, streamed), each once by its file spec (Unique);
    // Sources, when given, gets each one's loaded sound (loaded) or file spec (the others)
    static void AddAliasSounds(const WWIISoundAlias& Alias, std::set<uint64_t>& Unique, std::vector<CoDAsset_t*>& Out,
                               std::vector<uint64_t>* Sources = nullptr);
    // One level of a streamed image, the LZ4 chunks at Offset in an imagefile<N>.pak
    static std::unique_ptr<XImageDDS> DecodePakImage(const std::string& PakPath, uint64_t Offset, uint32_t Width, uint32_t Height,
                                                     uint8_t Format, bool Cube, ImageUsageType Usage);

    // A material another zone defines (a name-only header without images, its name without the
    // leading ','): its images as (semantic hash, image name), when the caller knows them.
    // ReadXMaterial gives them the ExportWriters::ImageElsewhere pointer.
    static thread_local std::function<bool(const std::string& Name, std::vector<std::pair<uint32_t, std::string>>& Images)> ImagesElsewhere;
};