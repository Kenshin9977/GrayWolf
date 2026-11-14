#pragma once

// <model>_import.py, the Blender script written beside a Cast or glTF model (the
// export_blender_script setting): it imports the model (a .cast read by the script itself, no add-on
// needed; a .glb/.gltf through Blender's importer), armature and shape keys kept, and builds every
// material from the model's .materials.json, each map wired (colour, normal, roughness or gloss,
// metalness or specular, occlusion, emission, alpha, layers and tints). Its body is
// tools/mapimport/model_import.py with the materials code it shares with the map script, embedded
// by tools/mapimport/embed.py; this fills its parameters.

#include <string>
#include <vector>

namespace Models
{
    struct ImportScriptParams
    {
        std::vector<std::string> Models;   // the model files' names beside the script, the first one there imported
        std::string Materials;             // the materials sidecar's file name
        float Scale = 0.01f;               // a Cast file's unit in metres (centimetres; inches 0.0254)
    };

    std::string ImportScript(const ImportScriptParams& P);

    // Writes <Base>_import.py for the model files named Base + .cast/.glb/.gltf (those Formats has
    // on) when the setting asks for it; a file already holding the text is left alone.
    void WriteImportScript(const std::string& Base, bool Cast, bool Glb, bool Gltf);
}
