#include "stdafx.h"

#include "GltfSceneWriter.h"
#include "GLTFExport.h"

#include <nlohmann/json.hpp>

#include <cmath>
#include <cstring>
#include <fstream>
#include <limits>

using nlohmann::json;

namespace
{
    constexpr int GL_FLOAT = 5126;
    constexpr int GL_UNSIGNED_BYTE = 5121;
    constexpr int GL_UNSIGNED_INT = 5125;
    constexpr int TARGET_ARRAY = 34962;
    constexpr int TARGET_ELEMENT = 34963;

    void Pad4(std::vector<uint8_t>& v, uint8_t fill)
    {
        while (v.size() % 4 != 0)
            v.push_back(fill);
    }
}

std::array<float, 16> GltfSceneWriter::Identity()
{
    return {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
}

std::array<float, 16> GltfSceneWriter::MatrixFromAxis(const float axis[9], const float origin[3], float scale)
{
    // CoD placement: row-major 3x3 axis (rows = local X/Y/Z in world), origin, uniform scale.
    // glTF wants a column-major 4x4. Column c = axis row c * scale; translation = origin.
    std::array<float, 16> m = Identity();
    for (int c = 0; c < 3; c++)
    {
        m[c * 4 + 0] = axis[c * 3 + 0] * scale;
        m[c * 4 + 1] = axis[c * 3 + 1] * scale;
        m[c * 4 + 2] = axis[c * 3 + 2] * scale;
        m[c * 4 + 3] = 0.0f;
    }
    m[12] = origin[0];
    m[13] = origin[1];
    m[14] = origin[2];
    m[15] = 1.0f;
    return m;
}

std::array<float, 16> GltfSceneWriter::MatrixLookDir(const float origin[3], const float dir[3])
{
    auto norm = [](float v[3]) {
        const float l = std::sqrt(v[0]*v[0] + v[1]*v[1] + v[2]*v[2]);
        if (l > 1e-8f) { v[0]/=l; v[1]/=l; v[2]/=l; }
    };
    auto cross = [](const float a[3], const float b[3], float o[3]) {
        o[0] = a[1]*b[2] - a[2]*b[1]; o[1] = a[2]*b[0] - a[0]*b[2]; o[2] = a[0]*b[1] - a[1]*b[0];
    };
    float f[3] = {dir[0], dir[1], dir[2]};
    norm(f);
    float z[3] = {-f[0], -f[1], -f[2]}; // node +Z = -forward (so local -Z = forward = dir)
    float up[3] = {0, 0, 1};            // CoD up
    if (std::fabs(z[0]*up[0] + z[1]*up[1] + z[2]*up[2]) > 0.99f) { up[0]=0; up[1]=1; up[2]=0; }
    float x[3]; cross(up, z, x); norm(x);
    float y[3]; cross(z, x, y);
    return {x[0], x[1], x[2], 0,
            y[0], y[1], y[2], 0,
            z[0], z[1], z[2], 0,
            origin[0], origin[1], origin[2], 1};
}

int GltfSceneWriter::PushView(const void* data, size_t bytes, int target)
{
    Pad4(m_bin, 0);
    const size_t offset = m_bin.size();
    const auto* p = static_cast<const uint8_t*>(data);
    m_bin.insert(m_bin.end(), p, p + bytes);
    m_views.push_back({offset, bytes, target});
    return (int)m_views.size() - 1;
}

int GltfSceneWriter::AddImagePng(const std::string& name, const std::vector<uint8_t>& png)
{
    if (png.empty())
        return -1;
    const int view = PushView(png.data(), png.size(), 0);
    m_images.push_back({name, view, std::string()});
    return (int)m_images.size() - 1;
}

int GltfSceneWriter::AddImageUri(const std::string& name, const std::string& uri)
{
    if (uri.empty())
        return -1;
    m_images.push_back({name, -1, GLTF::ImageUri(uri)});   // a URI: %-encoded
    return (int)m_images.size() - 1;
}

int GltfSceneWriter::AddMaterial(const std::string& name, int baseColorImage, float r, float g, float b, float a,
                                 Alpha alpha, float alphaCutoff)
{
    m_materials.push_back({name, baseColorImage, {r, g, b, a}, alpha, alphaCutoff});
    return (int)m_materials.size() - 1;
}

int GltfSceneWriter::AddMesh(const std::string& name, const std::vector<Vertex>& verts,
                             const std::vector<uint32_t>& indices, int material,
                             const std::vector<float>* moreUvs, uint32_t moreSets)
{
    return AddMesh(name, std::vector<Part>{ {&verts, &indices, material, moreUvs, moreSets} });
}

int GltfSceneWriter::AddMesh(const std::string& name, const std::vector<Part>& parts)
{
    Mesh m{name, {}};
    for (const Part& p : parts)
        if (!p.verts->empty() && !p.indices->empty())
        {
            m.prims.push_back(AddPrimitive(*p.verts, *p.indices, p.material));
            // TEXCOORD_1..: set k of every vertex, out of the moreSets pairs a vertex has in a row
            if (p.moreUvs && p.moreSets && p.moreUvs->size() == p.verts->size() * p.moreSets * 2)
                for (uint32_t k = 0; k < p.moreSets; k++)
                {
                    std::vector<float> uv;
                    uv.reserve(p.verts->size() * 2);
                    for (size_t v = 0; v < p.verts->size(); v++)
                    {
                        uv.push_back((*p.moreUvs)[(v * p.moreSets + k) * 2]);
                        uv.push_back((*p.moreUvs)[(v * p.moreSets + k) * 2 + 1]);
                    }
                    const int view = PushView(uv.data(), uv.size() * sizeof(float), TARGET_ARRAY);
                    m_accessors.push_back({view, GL_FLOAT, (int)p.verts->size(), "VEC2", false, {}, {}, false});
                    m.prims.back().moreUvAcc.push_back((int)m_accessors.size() - 1);
                }
        }
    if (m.prims.empty())
        return -1;
    m_meshes.push_back(std::move(m));
    return (int)m_meshes.size() - 1;
}

GltfSceneWriter::Primitive GltfSceneWriter::AddPrimitive(const std::vector<Vertex>& verts,
                                                         const std::vector<uint32_t>& indices, int material)
{

    // De-interleave into tightly-packed attribute arrays (one bufferView + accessor each).
    std::vector<float> pos, norm, uv;
    std::vector<uint8_t> col;
    pos.reserve(verts.size() * 3);
    norm.reserve(verts.size() * 3);
    uv.reserve(verts.size() * 2);
    col.reserve(verts.size() * 4);

    double mn[3] = {std::numeric_limits<double>::max(), std::numeric_limits<double>::max(), std::numeric_limits<double>::max()};
    double mx[3] = {std::numeric_limits<double>::lowest(), std::numeric_limits<double>::lowest(), std::numeric_limits<double>::lowest()};
    for (const auto& v : verts)
    {
        for (int k = 0; k < 3; k++)
        {
            pos.push_back(v.pos[k]);
            norm.push_back(v.normal[k]);
            mn[k] = v.pos[k] < mn[k] ? v.pos[k] : mn[k];
            mx[k] = v.pos[k] > mx[k] ? v.pos[k] : mx[k];
        }
        uv.push_back(v.uv[0]);
        uv.push_back(v.uv[1]);
        col.push_back(v.color[0]); col.push_back(v.color[1]);
        col.push_back(v.color[2]); col.push_back(v.color[3]);
    }

    auto addAcc = [&](int view, int compType, const char* type, int count, bool normalized) {
        m_accessors.push_back({view, compType, count, type, normalized, {}, {}, false});
        return (int)m_accessors.size() - 1;
    };

    const int posView = PushView(pos.data(), pos.size() * sizeof(float), TARGET_ARRAY);
    const int posAcc = addAcc(posView, GL_FLOAT, "VEC3", (int)verts.size(), false);
    m_accessors[posAcc].hasBounds = true;
    m_accessors[posAcc].min = {mn[0], mn[1], mn[2]};
    m_accessors[posAcc].max = {mx[0], mx[1], mx[2]};

    const int normView = PushView(norm.data(), norm.size() * sizeof(float), TARGET_ARRAY);
    const int normAcc = addAcc(normView, GL_FLOAT, "VEC3", (int)verts.size(), false);

    const int uvView = PushView(uv.data(), uv.size() * sizeof(float), TARGET_ARRAY);
    const int uvAcc = addAcc(uvView, GL_FLOAT, "VEC2", (int)verts.size(), false);

    const int colView = PushView(col.data(), col.size(), TARGET_ARRAY);
    const int colAcc = addAcc(colView, GL_UNSIGNED_BYTE, "VEC4", (int)verts.size(), true);

    const int idxView = PushView(indices.data(), indices.size() * sizeof(uint32_t), TARGET_ELEMENT);
    const int idxAcc = addAcc(idxView, GL_UNSIGNED_INT, "SCALAR", (int)indices.size(), false);

    return {posAcc, normAcc, uvAcc, colAcc, idxAcc, material};
}

int GltfSceneWriter::AddNode(const std::string& name, int mesh, const std::array<float, 16>& matrix)
{
    m_nodes.push_back({name, mesh, -1, matrix});
    return (int)m_nodes.size() - 1;
}

int GltfSceneWriter::AddLightNode(const std::string& name, LightType type, float r, float g, float b,
                                  float intensity, float range, const std::array<float, 16>& matrix,
                                  float outerConeAngle)
{
    m_lights.push_back({name, type, {r, g, b}, intensity, range, outerConeAngle});
    const int lightIndex = (int)m_lights.size() - 1;
    m_nodes.push_back({name, -1, lightIndex, matrix});
    return (int)m_nodes.size() - 1;
}

void GltfSceneWriter::SetSceneRoot(const std::array<float, 16>& matrix, const std::string& name)
{
    m_hasRoot = true;
    m_rootMatrix = matrix;
    m_rootName = name;
}

bool GltfSceneWriter::WriteGlb(const std::string& Path)
{
    json doc;
    doc["asset"] = {{"version", "2.0"}, {"generator", "Greyhoundx GltfSceneWriter"}};

    doc["buffers"] = json::array();
    doc["buffers"].push_back({{"byteLength", (uint64_t)m_bin.size()}});

    doc["bufferViews"] = json::array();
    for (const auto& v : m_views)
    {
        json jv = {{"buffer", 0}, {"byteOffset", (uint64_t)v.offset}, {"byteLength", (uint64_t)v.length}};
        if (v.target != 0)
            jv["target"] = v.target;
        doc["bufferViews"].push_back(jv);
    }

    doc["accessors"] = json::array();
    for (const auto& a : m_accessors)
    {
        json ja = {{"bufferView", a.bufferView}, {"componentType", a.componentType},
                   {"count", a.count}, {"type", a.type}};
        if (a.normalized)
            ja["normalized"] = true;
        if (a.hasBounds)
        {
            ja["min"] = a.min;
            ja["max"] = a.max;
        }
        doc["accessors"].push_back(ja);
    }

    if (!m_images.empty())
    {
        doc["images"] = json::array();
        doc["textures"] = json::array();
        doc["samplers"] = json::array();
        doc["samplers"].push_back({{"magFilter", 9729}, {"minFilter", 9987}, {"wrapS", 10497}, {"wrapT", 10497}});
        for (const auto& img : m_images)
        {
            const int source = (int)doc["images"].size();
            if (img.bufferView >= 0)
                doc["images"].push_back({{"name", img.name}, {"bufferView", img.bufferView}, {"mimeType", "image/png"}});
            else
                doc["images"].push_back({{"name", img.name}, {"uri", img.uri}});
            // a DDS is no core glTF image: named through MSFT_texture_dds, without a fallback
            // (a tool without the extension shows the material untextured)
            const bool dds = img.uri.size() > 4 && _stricmp(img.uri.c_str() + img.uri.size() - 4, ".dds") == 0;
            if (dds)
            {
                doc["images"].back()["mimeType"] = "image/vnd-ms.dds";
                doc["textures"].push_back({{"extensions", {{"MSFT_texture_dds", {{"source", source}}}}}, {"sampler", 0}});
                m_extensions.insert("MSFT_texture_dds");
            }
            else
                doc["textures"].push_back({{"source", source}, {"sampler", 0}});
        }
    }

    if (!m_materials.empty())
    {
        doc["materials"] = json::array();
        for (const auto& m : m_materials)
        {
            json pbr = {{"baseColorFactor", {m.rgba[0], m.rgba[1], m.rgba[2], m.rgba[3]}},
                        {"metallicFactor", 0.0}, {"roughnessFactor", 1.0}};
            if (m.baseColorImage >= 0)
                pbr["baseColorTexture"] = {{"index", m.baseColorImage}};
            json jm = {{"name", m.name}, {"pbrMetallicRoughness", pbr}, {"doubleSided", true}};
            if (m.alpha == Alpha::Mask)
            {
                jm["alphaMode"] = "MASK";
                jm["alphaCutoff"] = m.cutoff;
            }
            else if (m.alpha == Alpha::Blend)
                jm["alphaMode"] = "BLEND";
            doc["materials"].push_back(jm);
        }
    }

    doc["meshes"] = json::array();
    for (const auto& m : m_meshes)
    {
        json prims = json::array();
        for (const Primitive& p : m.prims)
        {
            json attrs = {{"POSITION", p.posAcc}, {"NORMAL", p.normAcc}, {"TEXCOORD_0", p.uvAcc}, {"COLOR_0", p.colAcc}};
            for (size_t k = 0; k < p.moreUvAcc.size(); k++)
                attrs["TEXCOORD_" + std::to_string(k + 1)] = p.moreUvAcc[k];
            json prim = {{"attributes", attrs}, {"indices", p.idxAcc}, {"mode", 4}};
            if (p.material >= 0)
                prim["material"] = p.material;
            prims.push_back(prim);
        }
        doc["meshes"].push_back({{"name", m.name}, {"primitives", prims}});
    }

    if (!m_lights.empty())
    {
        json lights = json::array();
        for (const auto& l : m_lights)
        {
            const char* t = l.type == LightType::Point ? "point" : (l.type == LightType::Spot ? "spot" : "directional");
            json jl = {{"name", l.name}, {"type", t}, {"color", {l.color[0], l.color[1], l.color[2]}},
                       {"intensity", l.intensity}};
            if (l.type != LightType::Directional && l.range > 0.0f)
                jl["range"] = l.range;
            if (l.type == LightType::Spot)
                jl["spot"] = {{"innerConeAngle", 0.0}, {"outerConeAngle", (double)l.outerCone}};
            lights.push_back(jl);
        }
        doc["extensions"]["KHR_lights_punctual"] = {{"lights", lights}};
        m_extensions.insert("KHR_lights_punctual");
    }
    if (!m_extensions.empty())
        doc["extensionsUsed"] = m_extensions;

    doc["nodes"] = json::array();
    for (const auto& n : m_nodes)
    {
        json jn;
        jn["name"] = n.name;
        jn["matrix"] = json::array();
        for (float f : n.matrix)
            jn["matrix"].push_back(f);
        if (n.mesh >= 0)
            jn["mesh"] = n.mesh;
        if (n.light >= 0)
            jn["extensions"]["KHR_lights_punctual"] = {{"light", n.light}};
        doc["nodes"].push_back(jn);
    }

    json roots = json::array();
    if (m_hasRoot && !m_nodes.empty())
    {
        // Wrap all content nodes under one root that carries the coordinate transform.
        json rootNode;
        rootNode["name"] = m_rootName;
        rootNode["matrix"] = json::array();
        for (float f : m_rootMatrix)
            rootNode["matrix"].push_back(f);
        json children = json::array();
        for (size_t i = 0; i < m_nodes.size(); i++)
            children.push_back((int)i);
        rootNode["children"] = children;
        doc["nodes"].push_back(rootNode);
        roots.push_back((int)m_nodes.size()); // the root is the last-appended node
    }
    else
    {
        for (size_t i = 0; i < m_nodes.size(); i++)
            roots.push_back((int)i);
    }
    doc["scenes"] = json::array({{{"nodes", roots}}});
    doc["scene"] = 0;

    std::string jsonStr = doc.dump();
    std::vector<uint8_t> jsonChunk(jsonStr.begin(), jsonStr.end());
    Pad4(jsonChunk, ' ');
    std::vector<uint8_t> binChunk = m_bin;
    Pad4(binChunk, 0);

    const uint32_t total = 12 + 8 + (uint32_t)jsonChunk.size() + 8 + (uint32_t)binChunk.size();

    std::ofstream out(Path, std::ios::binary | std::ios::trunc);
    if (!out)
        return false;

    auto u32 = [&](uint32_t v) { out.write(reinterpret_cast<const char*>(&v), 4); };
    u32(0x46546C67); // "glTF"
    u32(2);          // version
    u32(total);
    u32((uint32_t)jsonChunk.size());
    u32(0x4E4F534A); // "JSON"
    out.write(reinterpret_cast<const char*>(jsonChunk.data()), (std::streamsize)jsonChunk.size());
    u32((uint32_t)binChunk.size());
    u32(0x004E4942); // "BIN\0"
    out.write(reinterpret_cast<const char*>(binChunk.data()), (std::streamsize)binChunk.size());

    return (bool)out;
}
