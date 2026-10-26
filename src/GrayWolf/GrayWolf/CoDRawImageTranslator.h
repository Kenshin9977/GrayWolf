#pragma once

#include <cstdint>
#include <memory>

struct XImageDDS;

// A class that handles converting BC encoded files to DDS files in memory
class CoDRawImageTranslator
{
public:
    static std::unique_ptr<XImageDDS> TranslateBC(const std::unique_ptr<uint8_t[]>& BCBuffer, uint32_t BCBufferSize, uint32_t Width, uint32_t Height, uint8_t ImageFormat, uint8_t MipLevels = 1, bool isCubemap = false, uint32_t Depth = 1);
};