#include "PreviewPost.hlsli"

FullscreenOut main(uint Id : SV_VertexID)
{
    return Fullscreen(Id);
}
