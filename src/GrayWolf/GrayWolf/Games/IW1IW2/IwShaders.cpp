#include "stdafx.h"

#include "IwShaders.h"

#include "ExportNaming.h"

#include <cctype>
#include <cstdlib>

namespace IW12
{
namespace
{
    // Tokens of a Quake 3 script: words, braces, line breaks (a keyword's arguments end at one);
    // // and /* */ comments dropped.
    struct Token
    {
        std::string Text;
        bool NewLine = false;   // a line break came before it
    };
    std::vector<Token> Tokenize(const std::string& S)
    {
        std::vector<Token> Out;
        bool Line = true;
        for (size_t I = 0; I < S.size();)
        {
            const char C = S[I];
            if (C == '\n')
            {
                Line = true;
                I++;
            }
            else if (std::isspace((unsigned char)C)) I++;
            else if (C == '/' && I + 1 < S.size() && S[I + 1] == '/')
            {
                while (I < S.size() && S[I] != '\n') I++;
            }
            else if (C == '/' && I + 1 < S.size() && S[I + 1] == '*')
            {
                const size_t End = S.find("*/", I + 2);
                for (size_t K = I; K < (End == std::string::npos ? S.size() : End); K++)
                    if (S[K] == '\n') Line = true;
                I = End == std::string::npos ? S.size() : End + 2;
            }
            else if (C == '{' || C == '}')
            {
                Out.push_back({ std::string(1, C), Line });
                Line = false;
                I++;
            }
            else
            {
                size_t J = I;
                if (C == '"')
                {
                    J = S.find('"', I + 1);
                    if (J == std::string::npos) J = S.size();
                    Out.push_back({ S.substr(I + 1, J - I - 1), Line });
                    I = J + 1;
                }
                else
                {
                    while (J < S.size() && !std::isspace((unsigned char)S[J]) && S[J] != '{' && S[J] != '}') J++;
                    Out.push_back({ S.substr(I, J - I), Line });
                    I = J;
                }
                Line = false;
            }
        }
        return Out;
    }
}

const char* Shader::BlendWord() const
{
    switch (Mode)
    {
    case Blend::Cutout: return "alphatest";
    case Blend::Blend: return "blend";
    case Blend::Add: return "add";
    case Blend::Multiply: return "multiply";
    default: return "replace";
    }
}

void ShaderScripts::Parse(const std::string& Text, bool& Ok)
{
    Files++;
    Ok = true;
    const std::vector<Token> T = Tokenize(Text);
    size_t I = 0;
    // the rest of a keyword's line
    const auto Args = [&](size_t& At) {
        std::vector<std::string> A;
        while (At < T.size() && !T[At].NewLine && T[At].Text != "{" && T[At].Text != "}") A.push_back(ExportNaming::Lower(T[At++].Text));
        return A;
    };
    while (I < T.size())
    {
        const std::string Name = ExportNaming::Lower(T[I++].Text);
        if (I >= T.size() || T[I].Text != "{")
        {
            Ok = false;
            break;
        }
        I++;
        Shader S;
        bool ColorFound = false;
        int Depth = 1;
        // the stage being read: its first bundle's map, environment mapping, blending
        std::string StageMap;
        bool StageEnv = false, StageBundle0 = true, StageAlphaTest = false;
        // its animMap: the frames (on its line, or the map lines after it: AnimByLines), their rate
        std::vector<std::string> StageFrames;
        float StageRate = 0;
        bool AnimByLines = false;
        std::string StageBlend;
        while (I < T.size() && Depth > 0)
        {
            const Token& K = T[I++];
            if (K.Text == "{")
            {
                Depth++;
                StageMap.clear();
                StageEnv = StageAlphaTest = false;
                StageBundle0 = true;
                StageBlend.clear();
                StageFrames.clear();
                StageRate = 0;
                AnimByLines = false;
                continue;
            }
            if (K.Text == "}")
            {
                if (Depth == 2 && !ColorFound && !StageMap.empty() && !StageEnv && StageMap[0] != '$')
                {
                    ColorFound = true;
                    S.ColorMap = StageMap;
                    if (StageFrames.size() > 1 && StageRate > 0)
                    {
                        S.Frames = StageFrames;
                        S.FrameRate = StageRate;
                    }
                    if (StageAlphaTest) S.Mode = Shader::Blend::Cutout;
                    else if (StageBlend == "blend" || StageBlend == "gl_src_alpha gl_one_minus_src_alpha") S.Mode = Shader::Blend::Blend;
                    else if (StageBlend == "add" || StageBlend == "gl_one gl_one") S.Mode = Shader::Blend::Add;
                    else if (StageBlend == "filter" || StageBlend == "gl_dst_color gl_zero" || StageBlend == "gl_zero gl_src_color") S.Mode = Shader::Blend::Multiply;
                    else if (!StageBlend.empty() && StageBlend != "gl_one gl_zero") S.Mode = Shader::Blend::Blend;
                }
                Depth--;
                continue;
            }
            const std::string Key = ExportNaming::Lower(K.Text);
            std::vector<std::string> A = Args(I);
            if (Depth == 1)
            {
                if (Key == "surfaceparm" && !A.empty())
                {
                    if (A[0] == "nodraw") S.NoDraw = true;
                    if (A[0] == "sky") S.Sky = true;
                }
                else if (Key == "skyparms") S.Sky = true;
                else if (Key == "cull" && !A.empty() && (A[0] == "none" || A[0] == "disable" || A[0] == "twosided")) S.TwoSided = true;
            }
            else if (Depth == 2)
            {
                if (Key == "nextbundle") StageBundle0 = false;
                else if (!StageBundle0) continue;
                else if ((Key == "map" || Key == "clampmap") && !A.empty())
                {
                    // "map clamp <image>": the image after the word
                    const std::string& Image = A[0] == "clamp" && A.size() >= 2 ? A[1] : A[0];
                    if (AnimByLines) StageFrames.push_back(Image);
                    StageMap = AnimByLines ? StageFrames[0] : Image;
                }
                else if (Key == "animmap" && !A.empty())
                {
                    StageRate = std::strtof(A[0].c_str(), nullptr);
                    StageFrames.assign(A.begin() + 1, A.end());
                    AnimByLines = StageFrames.empty();
                    if (!StageFrames.empty()) StageMap = StageFrames[0];
                }
                else if (Key == "tcgen" && !A.empty() && A[0] == "environment") StageEnv = true;
                else if (Key == "alphafunc") StageAlphaTest = true;
                else if (Key == "blendfunc")
                {
                    StageBlend.clear();
                    for (const auto& W : A) StageBlend += (StageBlend.empty() ? "" : " ") + W;
                }
            }
        }
        if (Depth != 0)
        {
            Ok = false;
            break;
        }
        Shaders.emplace(Name, std::move(S));   // the first definition wins
    }
    if (!Ok) Failed++;
}
}
