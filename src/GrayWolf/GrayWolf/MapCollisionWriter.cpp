#include "stdafx.h"

#include "MapCollisionWriter.h"

#include "MapSceneWriter.h"

#include <cstdio>
#include <format>
#include <fstream>
#include <string>

namespace Maps
{
namespace
{
    void Text(std::string& Out, const std::string& S)
    {
        Out += '"';
        for (const unsigned char C : S)
        {
            if (C == '"' || C == '\\') Out += '\\', Out += (char)C;
            else if (C < 0x20) Out += std::format("\\u{:04x}", C);
            else Out += (char)C;
        }
        Out += '"';
    }

    // a float as the shortest text that reads back the same
    void Number(std::string& Out, float V) { Out += std::format("{}", V); }

    void WriteBrush(std::string& Out, const Brush& B)
    {
        Out += std::format("{{\"contents\":{},\"sides\":[", B.Contents);
        for (size_t K = 0; K < B.Sides.size(); K++)
        {
            const BrushSide& S = B.Sides[K];
            if (K) Out += ',';
            Out += '[';
            for (int J = 0; J < 3; J++) Number(Out, S.Normal[J]), Out += ',';
            Number(Out, S.Dist);
            Out += ',';
            Text(Out, S.Material);
            Out += std::format(",{},{}]", S.Contents, S.Surface);
        }
        Out += "]}";
    }

    void WriteBrushLists(std::string& Out, const char* Key, const std::vector<std::vector<Brush>>& Lists)
    {
        Out += std::format(",\n\"{}\":[", Key);
        for (size_t K = 0; K < Lists.size(); K++)
        {
            Out += K ? ",\n[" : "\n[";
            for (size_t B = 0; B < Lists[K].size(); B++)
            {
                if (B) Out += ',';
                WriteBrush(Out, Lists[K][B]);
            }
            Out += ']';
        }
        Out += "]";
    }

    class CollisionSceneWriter final : public ISceneWriter
    {
    public:
        const char* Extension() const override { return "_collision.json"; }
        bool Write(const MapScene& Scene, const std::string& Path, const Materials::IAdapter&, SceneWriteResult& Result,
                   std::string& Error, const WriteProgress&) const override
        {
            // a level whose game reads no clip map has nothing to write: not a failure
            if (Scene.Collision.Empty() && Scene.Collision.Triangles.empty())
            {
                Result.Counters = { { "map_collision_empty", 1 } };
                return true;
            }
            if (!WriteCollisionJson(Scene, Path, Error))
            {
                Error = "collision json: " + Error;
                return false;
            }
            size_t ModelBrushes = 0;
            for (const auto& M : Scene.Collision.Models) ModelBrushes += M.size();
            Result.Counters = { { "map_collision_maps", 1 },
                                { "map_collision_brushes", Scene.Collision.Brushes.size() },
                                { "map_collision_model_brushes", ModelBrushes },
                                { "map_collision_triangles", Scene.Collision.Triangles.size() / 3 } };
            return true;
        }
    };
}

bool WriteCollisionJson(const MapScene& Scene, const std::string& Path, std::string& Error)
{
    const Collision& C = Scene.Collision;
    std::string Out = "{\"format\":\"graywolf-collision\",\"version\":1,\n\"map\":";
    Text(Out, Scene.Name);
    Out += std::format(",\"clip_brushes\":{},\n\"materials\":[", C.ClipBrushes);
    for (size_t K = 0; K < C.Materials.size(); K++)
    {
        if (K) Out += ',';
        Out += '[';
        Text(Out, C.Materials[K].first);
        Out += std::format(",{}]", C.Materials[K].second);
    }
    Out += "],\n\"brushes\":[";
    for (size_t K = 0; K < C.Brushes.size(); K++)
    {
        Out += K ? ",\n" : "\n";
        WriteBrush(Out, C.Brushes[K]);
    }
    Out += "]";
    WriteBrushLists(Out, "models", C.Models);
    WriteBrushLists(Out, "triggers", C.Triggers);
    Out += ",\n\"vertices\":[";
    for (size_t K = 0; K < C.Vertices.size(); K++)
    {
        if (K) Out += ',';
        Number(Out, C.Vertices[K]);
    }
    Out += "],\n\"triangles\":[";
    for (size_t K = 0; K < C.Triangles.size(); K++)
    {
        if (K) Out += ',';
        Out += std::to_string(C.Triangles[K]);
    }
    Out += "]}\n";

    std::ofstream File(Path, std::ios::binary);
    if (!File)
    {
        Error = "cannot create " + Path;
        return false;
    }
    File << Out;
    return File.good();
}

const ISceneWriter& CollisionWriter()
{
    static const CollisionSceneWriter Writer;
    return Writer;
}
}
