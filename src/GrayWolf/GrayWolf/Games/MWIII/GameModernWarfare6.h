#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "CoDXModelTranslator.h"

class GameModernWarfare6
{
public:
    static std::unique_ptr<XAnim_t> ReadXAnim(const CoDAnim_t* Animation);
    static std::unique_ptr<XModel_t> ReadXModel(const CoDModel_t* Model);
    static std::unique_ptr<XImageDDS> ReadXImage(const CoDImage_t* Image);
    static std::unique_ptr<XSound> ReadXSound(const CoDSound_t* Sound);
    static const XMaterial_t ReadXMaterial(uint64_t MaterialPointer);
    // A material another zone defines (the multiplayer's header: no slots, no techset), by its name
    // hash in hex: its images as (semantic, image name), when the caller knows them. Read by name
    // only: ReadXMaterial gives them the ExportWriters::ImageElsewhere pointer.
    static thread_local std::function<bool(const std::string& Hash, std::vector<std::pair<uint32_t, std::string>>& Images)> ImagesElsewhere;

    static std::unique_ptr<XImageDDS> LoadXImage(const XImage_t& Image);
    static void LoadXModel(const std::unique_ptr<XModel_t>& Model, const XModelLod_t& ModelLOD, const std::unique_ptr<WraithModel>& ResultModel);
    static void LoadXAnim(const std::unique_ptr<XAnim_t>& Anim, std::unique_ptr<WraithAnim>& ResultAnim);

    static void PerformInitialSetup();
};