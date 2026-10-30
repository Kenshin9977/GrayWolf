#pragma once

// The Direct3D 11 resources the previews make again and again (their textures, targets and
// constant buffers), one shape each. A false return leaves nothing the caller can use.

#include <d3d11.h>
#include <wrl/client.h>

namespace PreviewD3D
{
    using Microsoft::WRL::ComPtr;

    // A W x H texture, one slice: DEFAULT usage, or IMMUTABLE when Init gives its bytes
    bool Texture(ID3D11Device* Dev, int W, int H, DXGI_FORMAT Format, UINT Bind, ComPtr<ID3D11Texture2D>& Out, UINT Samples = 1,
                 UINT Mips = 1, const D3D11_SUBRESOURCE_DATA* Init = nullptr);

    // A W x H texture something copies into and a shader samples: the texture and its view (both
    // reset on a failure)
    bool Sampled(ID3D11Device* Dev, int W, int H, DXGI_FORMAT Format, ComPtr<ID3D11Texture2D>& Tex, ComPtr<ID3D11ShaderResourceView>& Srv);

    // A W x H table of fixed bytes (Pitch bytes a row) a shader samples
    bool Immutable(ID3D11Device* Dev, int W, int H, DXGI_FORMAT Format, const void* Bytes, UINT Pitch, ComPtr<ID3D11ShaderResourceView>& Srv);

    // A W x H target sampled afterwards: its render target and shader views
    bool Target(ID3D11Device* Dev, int W, int H, DXGI_FORMAT Format, ComPtr<ID3D11RenderTargetView>& Rtv, ComPtr<ID3D11ShaderResourceView>& Srv);

    // A W x H texture a compute pass writes and a later one reads
    bool Storage(ID3D11Device* Dev, int W, int H, DXGI_FORMAT Format, ComPtr<ID3D11UnorderedAccessView>& Uav, ComPtr<ID3D11ShaderResourceView>& Srv);

    // A constant buffer of Bytes rounded up to 16 (what D3D11 requires), DEFAULT usage (UpdateSubresource)
    bool ConstantBuffer(ID3D11Device* Dev, UINT Bytes, ComPtr<ID3D11Buffer>& Out);
}
