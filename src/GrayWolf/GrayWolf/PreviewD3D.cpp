#include "stdafx.h"

#include "PreviewD3D.h"

namespace PreviewD3D
{
    bool Texture(ID3D11Device* Dev, int W, int H, DXGI_FORMAT Format, UINT Bind, ComPtr<ID3D11Texture2D>& Out, UINT Samples, UINT Mips,
                 const D3D11_SUBRESOURCE_DATA* Init)
    {
        D3D11_TEXTURE2D_DESC Td{};
        Td.Width = (UINT)W; Td.Height = (UINT)H; Td.MipLevels = Mips; Td.ArraySize = 1; Td.Format = Format;
        Td.SampleDesc.Count = Samples; Td.Usage = Init ? D3D11_USAGE_IMMUTABLE : D3D11_USAGE_DEFAULT; Td.BindFlags = Bind;
        return SUCCEEDED(Dev->CreateTexture2D(&Td, Init, &Out));
    }

    bool Sampled(ID3D11Device* Dev, int W, int H, DXGI_FORMAT Format, ComPtr<ID3D11Texture2D>& Tex, ComPtr<ID3D11ShaderResourceView>& Srv)
    {
        if (Texture(Dev, W, H, Format, D3D11_BIND_SHADER_RESOURCE, Tex) && SUCCEEDED(Dev->CreateShaderResourceView(Tex.Get(), nullptr, &Srv)))
            return true;
        Tex.Reset(); Srv.Reset();
        return false;
    }

    bool Immutable(ID3D11Device* Dev, int W, int H, DXGI_FORMAT Format, const void* Bytes, UINT Pitch, ComPtr<ID3D11ShaderResourceView>& Srv)
    {
        const D3D11_SUBRESOURCE_DATA Init{ Bytes, Pitch, 0 };
        ComPtr<ID3D11Texture2D> Tex;
        return Texture(Dev, W, H, Format, D3D11_BIND_SHADER_RESOURCE, Tex, 1, 1, &Init) &&
               SUCCEEDED(Dev->CreateShaderResourceView(Tex.Get(), nullptr, &Srv));
    }

    bool Target(ID3D11Device* Dev, int W, int H, DXGI_FORMAT Format, ComPtr<ID3D11RenderTargetView>& Rtv, ComPtr<ID3D11ShaderResourceView>& Srv)
    {
        ComPtr<ID3D11Texture2D> Tex;
        return Texture(Dev, W, H, Format, D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE, Tex) &&
               SUCCEEDED(Dev->CreateRenderTargetView(Tex.Get(), nullptr, &Rtv)) && SUCCEEDED(Dev->CreateShaderResourceView(Tex.Get(), nullptr, &Srv));
    }

    bool Storage(ID3D11Device* Dev, int W, int H, DXGI_FORMAT Format, ComPtr<ID3D11UnorderedAccessView>& Uav, ComPtr<ID3D11ShaderResourceView>& Srv)
    {
        ComPtr<ID3D11Texture2D> Tex;
        return Texture(Dev, W, H, Format, D3D11_BIND_UNORDERED_ACCESS | D3D11_BIND_SHADER_RESOURCE, Tex) &&
               SUCCEEDED(Dev->CreateUnorderedAccessView(Tex.Get(), nullptr, &Uav)) && SUCCEEDED(Dev->CreateShaderResourceView(Tex.Get(), nullptr, &Srv));
    }

    bool ConstantBuffer(ID3D11Device* Dev, UINT Bytes, ComPtr<ID3D11Buffer>& Out)
    {
        D3D11_BUFFER_DESC B{};
        B.ByteWidth = (Bytes + 15) & ~15u; B.Usage = D3D11_USAGE_DEFAULT; B.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
        return SUCCEEDED(Dev->CreateBuffer(&B, nullptr, &Out));
    }
}
