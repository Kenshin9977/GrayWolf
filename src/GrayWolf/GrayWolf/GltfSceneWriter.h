#pragma once

#include <array>
#include <cstdint>
#include <set>
#include <string>
#include <vector>

// Minimal glTF 2.0 GLB *scene* writer: geometry + instanced nodes + PBR materials + embedded PNG
// textures + punctual lights (KHR_lights_punctual). Self-contained (only nlohmann/json), no OAT/engine
// types - callers feed plain vertex/index/transform data. This is the core of the Blender map export:
// OAT's XModel/GfxWorld geometry is decoded elsewhere and assembled here into one drag-and-drop .glb.
//
// glTF axis note: glTF is right-handed, +Y up, metres. CoD is +Z up, inches. Callers convert before
// feeding us (positions already in the target space); we do not transform.
class GltfSceneWriter
{
public:
    struct Vertex
    {
        float pos[3];
        float normal[3];
        float uv[2];
        uint8_t color[4]; // RGBA, normalized on read
    };

    enum class LightType
    {
        Point,
        Spot,
        Directional,
    };

    // A 4x4 node transform, column-major (glTF convention). Identity by default.
    static std::array<float, 16> Identity();
    // Builds a column-major TRS-ish matrix from a CoD placement (row-major 3x3 axis, origin, scale).
    // Applies no coordinate conversion - caller passes values already in glTF space.
    static std::array<float, 16> MatrixFromAxis(const float axis[9], const float origin[3], float scale);
    // Builds a node matrix at `origin` whose local -Z points along `dir` (glTF spot/directional lights
    // shine down local -Z). Values stay in the caller's space (the scene root converts).
    static std::array<float, 16> MatrixLookDir(const float origin[3], const float dir[3]);

    // Registry adds - each returns an index used to reference it.
    int AddImagePng(const std::string& name, const std::vector<uint8_t>& png); // -1 on empty
    // An image the .glb refers to by a path relative to itself, not embedded (-1 on empty).
    int AddImageUri(const std::string& name, const std::string& uri);
    // How the base colour's alpha is used (glTF alphaMode): OPAQUE, MASK (cut out at the cutoff), BLEND.
    enum class Alpha
    {
        Opaque,
        Mask,
        Blend,
    };
    int AddMaterial(const std::string& name, int baseColorImage, float r = 1, float g = 1, float b = 1, float a = 1,
                    Alpha alpha = Alpha::Opaque, float alphaCutoff = 0.5f);
    // Adds a mesh with one primitive (one material). Returns the mesh index, or -1 if empty.
    // moreUvs: moreSets UV pairs per vertex in a row, written as TEXCOORD_1.. (none when null).
    int AddMesh(const std::string& name, const std::vector<Vertex>& verts,
                const std::vector<uint32_t>& indices, int material,
                const std::vector<float>* moreUvs = nullptr, uint32_t moreSets = 0);
    // Adds a mesh of several primitives (a model: one per material); empty parts are left out.
    struct Part
    {
        const std::vector<Vertex>* verts;
        const std::vector<uint32_t>* indices;
        int material;
        const std::vector<float>* moreUvs = nullptr;
        uint32_t moreSets = 0;
    };
    int AddMesh(const std::string& name, const std::vector<Part>& parts);

    // Scene nodes. AddNode places a mesh instance; a mesh may be referenced by many nodes (instancing).
    int AddNode(const std::string& name, int mesh, const std::array<float, 16>& matrix);
    int AddLightNode(const std::string& name, LightType type, float r, float g, float b,
                     float intensityCandelaOrLux, float range, const std::array<float, 16>& matrix,
                     float outerConeAngle = 0.7853982f);

    // Wraps every node under one root carrying `matrix` (e.g. a CoD->glTF axis+scale convert), so all
    // content can stay in the source coordinate space and instancing is preserved. Optional.
    void SetSceneRoot(const std::array<float, 16>& matrix, const std::string& name = "cod_to_gltf");

    [[nodiscard]] size_t MeshCount() const { return m_meshes.size(); }
    [[nodiscard]] size_t NodeCount() const { return m_nodes.size(); }

    // Serializes everything to a binary .glb at Path. Returns false on I/O failure.
    bool WriteGlb(const std::string& Path);

private:
    struct BufferView { size_t offset, length; int target; }; // target: 0=none,34962 array,34963 element
    struct Accessor { int bufferView, componentType, count; std::string type; bool normalized;
                      std::vector<double> min, max; bool hasBounds; };
    struct Primitive { int posAcc, normAcc, uvAcc, colAcc, idxAcc, material; std::vector<int> moreUvAcc; };
    struct Mesh { std::string name; std::vector<Primitive> prims; };
    struct Material { std::string name; int baseColorImage; float rgba[4]; Alpha alpha; float cutoff; };
    struct Image { std::string name; int bufferView; std::string uri; };
    struct Node { std::string name; int mesh; int light; std::array<float, 16> matrix; };
    struct Light { std::string name; LightType type; float color[3]; float intensity; float range; float outerCone; };

    // Appends bytes to the binary blob (4-byte aligned start), returns a bufferView index.
    int PushView(const void* data, size_t bytes, int target);
    Primitive AddPrimitive(const std::vector<Vertex>& verts, const std::vector<uint32_t>& indices, int material);

    std::vector<uint8_t> m_bin;
    std::vector<BufferView> m_views;
    std::vector<Accessor> m_accessors;
    std::vector<Mesh> m_meshes;
    std::vector<Material> m_materials;
    std::vector<Image> m_images;
    std::vector<Node> m_nodes;
    std::vector<Light> m_lights;
    std::set<std::string> m_extensions;   // extensionsUsed
    bool m_hasRoot = false;
    std::array<float, 16> m_rootMatrix{};
    std::string m_rootName;
};
