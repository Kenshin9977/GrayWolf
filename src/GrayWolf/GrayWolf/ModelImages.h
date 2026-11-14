#pragma once

// Each exported model's textures in its own folder (the model_images setting): models/<model>/images/
// holds the files its materials read, as NTFS hard links to the image kind's shared images/ files
// (no disk space; a plain copy where a link cannot be made). The model files and the
// <model>.materials.json then name images/<file>, beside the model: the folder is whole when moved
// or zipped, and Blender finds the textures there. The links are made once the run has written
// every image (the ones another zone defines, pulled last, included), from the sidecars.

#include <filesystem>
#include <string>

namespace DiskGame { struct Context; struct Totals; }
namespace Materials { struct Sidecar; }

namespace ModelImages
{
    // model_images on for this run
    bool On(const DiskGame::Context& C);
    // How a model file in models/<model>/ names the folder of its textures (doubled backslashes, as
    // ExportWriters::ModelTextures::Relative): images\\ with model_images on, else ..\\..\\images\\.
    std::string Relative(const DiskGame::Context& C);

    // A model sidecar's paths into its own images/ folder named as the shared files they link to
    // (../../images/...): what a pass that writes beside the images reads (MaterialGdt,
    // MaterialSplit), so it works on the shared files once, as without model_images. File: the
    // sidecar's; one that is not a model's (models/<model>/) is left as it is.
    void ToShared(Materials::Sidecar& S, const std::filesystem::path& File);

    // Every model sidecar under the run's models/ folder: each image it names in its images/ folder
    // made a hard link to the shared file (with split_maps, the split files too), else copied. A
    // link or copy already there that no longer is the shared file's content (the file replaced
    // since) is made again; a file of images/ no sidecar of the folder names is removed.
    // Counters: model_images_linked (made), model_images_relinked (of linked: stale ones replaced),
    // model_images_kept (already the file), model_images_copied, model_images_missing (no shared
    // file), model_images_failed, model_images_pruned.
    void LinkForRun(const DiskGame::Context& C, DiskGame::Totals& T);
}
