#pragma once

#include <string>

class WraithModel;
class WraithAnim;

class GLTF
{
public:
    // MetresPerUnit: the model's unit in metres (0.01: centimetres, GrayWolf's default; 0.0254: inches)
    static void ExportGLTF(const WraithModel& Model, const std::string& FileName, bool SupportsScale = false, bool writeBinary = false, float MetresPerUnit = 0.01f);
    // A file path (relative to the .gltf/.glb) as a glTF image URI: forward slashes, a run of
    // separators as one (the model files' escaped backslashes), every byte but the unreserved ones
    // and '/' percent-encoded (a '#', a space, UTF-8)
    static std::string ImageUri(const std::string& Path);
    static void ExportGLTF(const WraithAnim& Model, const std::string& FileName, bool SupportsScale = false);
};