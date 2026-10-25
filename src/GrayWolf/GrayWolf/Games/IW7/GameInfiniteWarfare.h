#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <functional>
#include <utility>
#include <vector>

class CoDAnim_t;
class CoDModel_t;
struct XMaterial_t;
struct XModel_t;
struct XAnim_t;

class GameInfiniteWarfare
{
public:
    static std::unique_ptr<XAnim_t> ReadXAnim(const CoDAnim_t* Animation);
    static std::unique_ptr<XModel_t> ReadXModel(const CoDModel_t* Model);

    // What a name-only material header (a leading ',') stands for, when the caller knows the
    // zone defining it: its images as (semantic hash, image name), handed on with the
    // ExportWriters::ImageElsewhere pointer.
    static thread_local std::function<bool(const std::string& Name, std::vector<std::pair<uint32_t, std::string>>& Images)> ImagesElsewhere;
    // and its techset's name and sort key (the material adapter's alpha mode)
    static thread_local std::function<bool(const std::string& Name, std::string& Techset, int& SortKey)> DetailsElsewhere;
    // A material by its address (the map reader's surfaces too)
    static const XMaterial_t ReadXMaterial(uint64_t MaterialPointer);
};