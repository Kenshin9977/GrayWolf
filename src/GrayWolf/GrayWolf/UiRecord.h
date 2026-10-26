#pragma once

#include <string>

// ui-record: the shell hidden on a folder, driven by a JSON script (steps: ImGuiShell.Record.cpp),
// one fixed time step a frame, written as <OutDir>/frame_NNNNN.png + audio.wav (what the player
// played) + timeline.txt (the frame each labelled step starts at). Frames are held while a
// selection loads.
struct UiRecordOptions
{
    std::string Script;
    std::string OutDir;
    int Width = 1920, Height = 1080;
    int Fps = 30;
    double HoldTimeout = 180;   // seconds a selection may load before recording goes on anyway
};
