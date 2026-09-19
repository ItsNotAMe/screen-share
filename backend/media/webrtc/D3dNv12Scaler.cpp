#include "D3dNv12Scaler.h"
#include "D3dVideoFrameBuffer.h"
#include <d3d10.h>
#include <d3dcompiler.h>
#include <cstring>
#include <stdexcept>

namespace screenshare::media {
namespace {
void Check(HRESULT hr) { if (FAILED(hr)) throw std::runtime_error("GPU NV12 scaling unavailable"); }
constexpr char shader[] = R"(
struct Vertex { float4 position : SV_Position; float2 uv : TEXCOORD0; };
Vertex vs_main(uint id : SV_VertexID) {
    float2 p[3] = { float2(-1,-1), float2(-1,3), float2(3,-1) };
    Vertex result; result.position = float4(p[id],0,1);
    result.uv = float2((p[id].x+1)*0.5, (1-p[id].y)*0.5); return result;
}
Texture2D<float2> plane : register(t0);
SamplerState linearClamp : register(s0);
float4 ps_main(Vertex input) : SV_Target {
    return float4(plane.SampleLevel(linearClamp, input.uv, 0), 0, 1);
}
)";
Microsoft::WRL::ComPtr<ID3DBlob> Compile(const char* entry, const char* target) {
    Microsoft::WRL::ComPtr<ID3DBlob> code, errors;
    Check(D3DCompile(shader, std::strlen(shader), nullptr, nullptr, nullptr, entry, target,
        D3DCOMPILE_ENABLE_STRICTNESS | D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &code, &errors));
    return code;
}
}
D3dNv12Scaler::D3dNv12Scaler(ID3D11Device* device) {
    auto vertex = Compile("vs_main", "vs_4_0"), pixel = Compile("ps_main", "ps_4_0");
    Check(device->CreateVertexShader(vertex->GetBufferPointer(), vertex->GetBufferSize(), nullptr, &vertex_));
    Check(device->CreatePixelShader(pixel->GetBufferPointer(), pixel->GetBufferSize(), nullptr, &pixel_));
    D3D11_SAMPLER_DESC sampler{};
    sampler.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    sampler.AddressU = sampler.AddressV = sampler.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    sampler.MaxLOD = D3D11_FLOAT32_MAX; sampler.ComparisonFunc = D3D11_COMPARISON_NEVER;
    Check(device->CreateSamplerState(&sampler, &sampler_));
}
Microsoft::WRL::ComPtr<ID3D11Texture2D> D3dNv12Scaler::Scale(ID3D11Device* device, ID3D11DeviceContext* context,
    ID3D11Texture2D* input, int width, int height, int left, int top, int imageWidth, int imageHeight) {
    const bool ready = pending_.Ready([&](const auto& query) {
        BOOL complete = FALSE;
        const auto result = context->GetData(query.Get(), &complete, sizeof(complete), D3D11_ASYNC_GETDATA_DONOTFLUSH);
        Check(result);
        return result == S_OK && complete;
    });
    // The latest-frame CPU handoff does not bound already submitted GPU work.
    // Four outstanding conversions cover the normal viewer budget without an
    // unbounded GPU command/resource backlog when the driver falls behind.
    if (!ready) throw GpuScalingBusy{};
    D3D11_QUERY_DESC queryDescription{D3D11_QUERY_EVENT, 0};
    Microsoft::WRL::ComPtr<ID3D11Query> completed;
    Check(device->CreateQuery(&queryDescription, &completed));
    D3D11_TEXTURE2D_DESC description{};
    description.Width = width; description.Height = height;
    description.MipLevels = description.ArraySize = description.SampleDesc.Count = 1;
    description.Format = DXGI_FORMAT_NV12; description.Usage = D3D11_USAGE_DEFAULT;
    description.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
    Microsoft::WRL::ComPtr<ID3D11Texture2D> output;
    Check(device->CreateTexture2D(&description, nullptr, &output));
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> sources[2];
    Microsoft::WRL::ComPtr<ID3D11RenderTargetView> targets[2];
    for (int i = 0; i < 2; ++i) {
        const auto format = i ? DXGI_FORMAT_R8G8_UNORM : DXGI_FORMAT_R8_UNORM;
        D3D11_SHADER_RESOURCE_VIEW_DESC source{};
        source.Format = format; source.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D; source.Texture2D.MipLevels = 1;
        Check(device->CreateShaderResourceView(input, &source, &sources[i]));
        D3D11_RENDER_TARGET_VIEW_DESC target{};
        target.Format = format; target.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2D;
        Check(device->CreateRenderTargetView(output.Get(), &target, &targets[i]));
    }
    Microsoft::WRL::ComPtr<ID3D10Multithread> protection;
    Check(device->QueryInterface(IID_PPV_ARGS(&protection)));
    protection->Enter();
    struct Unlock { ID3D10Multithread* value; ~Unlock() { value->Leave(); } } unlock{protection.Get()};
    // Capture and MF share this immediate context. Keep the complete draw sequence
    // protected and reset the state we use so another pipeline cannot affect it.
    context->IASetInputLayout(nullptr);
    context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    context->VSSetShader(vertex_.Get(), nullptr, 0);
    context->GSSetShader(nullptr, nullptr, 0);
    context->HSSetShader(nullptr, nullptr, 0);
    context->DSSetShader(nullptr, nullptr, 0);
    context->PSSetShader(pixel_.Get(), nullptr, 0);
    auto* sampler = sampler_.Get(); context->PSSetSamplers(0, 1, &sampler);
    context->RSSetState(nullptr);
    context->OMSetBlendState(nullptr, nullptr, 0xffffffff);
    context->OMSetDepthStencilState(nullptr, 0);
    for (int i = 0; i < 2; ++i) {
        const float black[4] = {i ? 128.f / 255 : 16.f / 255, 128.f / 255, 0, 1};
        context->ClearRenderTargetView(targets[i].Get(), black);
        const int divisor = i ? 2 : 1;
        D3D11_VIEWPORT viewport{float(left / divisor), float(top / divisor),
            float(imageWidth / divisor), float(imageHeight / divisor), 0, 1};
        auto* target = targets[i].Get(); auto* source = sources[i].Get();
        context->RSSetViewports(1, &viewport);
        context->OMSetRenderTargets(1, &target, nullptr);
        context->PSSetShaderResources(0, 1, &source);
        context->Draw(3, 0);
    }
    ID3D11ShaderResourceView* noSource = nullptr;
    context->PSSetShaderResources(0, 1, &noSource);
    context->OMSetRenderTargets(0, nullptr, nullptr);
    context->End(completed.Get());
    pending_.Submitted(std::move(completed));
    // Submit before another device-manager worker consumes this owned texture.
    // No CPU staging map or GPU completion wait is introduced.
    context->Flush();
    Check(device->GetDeviceRemovedReason());
    return output;
}
}
