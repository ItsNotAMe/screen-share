#include "DxgiCursor.h"
#include "DesktopCapturer.h"
#include "CaptureBackendPolicy.h"
#include <d3dcompiler.h>
#include <cstring>

namespace screenshare {
namespace {
void Check(HRESULT hr) {
    if (hr == DXGI_ERROR_DEVICE_REMOVED || hr == DXGI_ERROR_DEVICE_RESET || hr == DXGI_ERROR_DEVICE_HUNG || hr == DXGI_ERROR_ACCESS_LOST)
        throw CaptureDeviceLostError(hr);
    if (FAILED(hr)) throw CaptureBackendError(hr, "DXGI cursor composition failed");
}
constexpr size_t MaxBytes = 4 * 1024 * 1024;
constexpr char Shader[] = R"(
Texture2D<float4> desktop : register(t0);
Texture2D<float4> cursor : register(t1);
cbuffer Geometry : register(b0) {
    float2 sourceSize; float2 outputSize;
    int2 pointerPosition; uint2 pointerSize;
    uint pointerType; uint3 padding;
};
float4 VS(uint id : SV_VertexID) : SV_Position {
    return float4(id == 2 ? 3 : -1, id == 1 ? 3 : -1, 0, 1);
}
float4 PS(float4 position : SV_Position) : SV_Target {
    float4 bg = desktop.Load(int3(int2(position.xy), 0));
    int2 p = int2(floor(position.xy * sourceSize / outputSize)) - pointerPosition;
    if (any(p < 0) || any(p >= int2(pointerSize))) return bg;
    float4 fg = cursor.Load(int3(p, 0));
    if (pointerType == 2) return float4(lerp(bg.rgb, fg.rgb, fg.a), 1);
    uint3 b = uint3(round(saturate(bg.rgb) * 255));
    uint3 f = uint3(round(fg.rgb * 255));
    if (pointerType == 1) b = (b & (fg.a > 0.5 ? 255u : 0u)) ^ f;
    else b = fg.a > 0.5 ? b ^ f : f;
    return float4(float3(b) / 255, 1);
}
)";
}
void DxgiCursor::Shape(const DXGI_OUTDUPL_POINTER_SHAPE_INFO& info, std::span<const std::byte> bytes) {
    const bool mono = info.Type == DXGI_OUTDUPL_POINTER_SHAPE_TYPE_MONOCHROME;
    const UINT height = mono ? info.Height / 2 : info.Height;
    if ((!mono && info.Type != DXGI_OUTDUPL_POINTER_SHAPE_TYPE_COLOR &&
         info.Type != DXGI_OUTDUPL_POINTER_SHAPE_TYPE_MASKED_COLOR) ||
        !info.Width || !height || info.Width > 1024 || height > 1024 ||
        (mono && info.Height % 2) || info.Pitch < (mono ? (info.Width + 7) / 8 : info.Width * 4) ||
        uint64_t(info.Pitch) * info.Height > bytes.size() || bytes.size() > MaxBytes)
        throw std::runtime_error("Invalid DXGI cursor shape");
    std::vector<std::byte> pixels(size_t(info.Width) * height * 4);
    for (UINT y = 0; y < height; ++y) {
        if (!mono) {
            std::memcpy(pixels.data() + size_t(y) * info.Width * 4, bytes.data() + size_t(y) * info.Pitch, info.Width * 4);
            continue;
        }
        for (UINT x = 0; x < info.Width; ++x) {
            const auto mask = 0x80u >> (x % 8);
            const auto andBit = std::to_integer<unsigned>(bytes[size_t(y) * info.Pitch + x / 8]) & mask;
            const auto xorBit = std::to_integer<unsigned>(bytes[size_t(y + height) * info.Pitch + x / 8]) & mask;
            auto* pixel = pixels.data() + (size_t(y) * info.Width + x) * 4;
            pixel[0] = pixel[1] = pixel[2] = xorBit ? std::byte{255} : std::byte{0};
            pixel[3] = andBit ? std::byte{255} : std::byte{0};
        }
    }
    pixels_ = std::move(pixels); width_ = info.Width; height_ = height; type_ = info.Type; dirty_ = true;
}
void DxgiCursor::Update(IDXGIOutputDuplication* duplication, const DXGI_OUTDUPL_FRAME_INFO& frame) {
    if (frame.LastMouseUpdateTime.QuadPart) Position(frame.PointerPosition.Position, frame.PointerPosition.Visible != FALSE);
    if (!frame.PointerShapeBufferSize) return;
    if (frame.PointerShapeBufferSize > MaxBytes) throw std::runtime_error("DXGI cursor exceeds shape budget");
    std::vector<std::byte> bytes(frame.PointerShapeBufferSize);
    UINT required = 0;
    DXGI_OUTDUPL_POINTER_SHAPE_INFO info{};
    Check(duplication->GetFramePointerShape(static_cast<UINT>(bytes.size()), bytes.data(), &required, &info));
    if (required > bytes.size()) throw std::runtime_error("Invalid DXGI cursor byte count");
    Shape(info, std::span(bytes.data(), required));
}
ID3D11Texture2D* DxgiCursor::Composite(ID3D11Device* device, ID3D11DeviceContext* context,
    ID3D11Texture2D* desktop, UINT sourceWidth, UINT sourceHeight) {
    if (!visible_ || pixels_.empty()) return desktop;
    D3D11_TEXTURE2D_DESC desc{}; desktop->GetDesc(&desc);
    if (!sourceWidth || !sourceHeight || desc.Format != DXGI_FORMAT_B8G8R8A8_UNORM || desc.SampleDesc.Count != 1)
        throw std::runtime_error("DXGI cursor requires an SDR BGRA desktop");
    if (!vertex_) {
        ComPtr<ID3DBlob> vs, ps, errors;
        Check(D3DCompile(Shader, sizeof(Shader), nullptr, nullptr, nullptr, "VS", "vs_5_0", 0, 0, &vs, &errors));
        Check(D3DCompile(Shader, sizeof(Shader), nullptr, nullptr, nullptr, "PS", "ps_5_0", 0, 0, &ps, &errors));
        Check(device->CreateVertexShader(vs->GetBufferPointer(), vs->GetBufferSize(), nullptr, &vertex_));
        Check(device->CreatePixelShader(ps->GetBufferPointer(), ps->GetBufferSize(), nullptr, &pixel_));
        D3D11_BUFFER_DESC cb{}; cb.ByteWidth = 48; cb.Usage = D3D11_USAGE_DEFAULT; cb.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
        Check(device->CreateBuffer(&cb, nullptr, &constants_));
        D3D11_RASTERIZER_DESC rs{}; rs.FillMode = D3D11_FILL_SOLID; rs.CullMode = D3D11_CULL_NONE; rs.DepthClipEnable = TRUE;
        Check(device->CreateRasterizerState(&rs, &raster_));
    }
    if (dirty_) {
        cursorView_.Reset(); cursor_.Reset();
        D3D11_TEXTURE2D_DESC cd{}; cd.Width = width_; cd.Height = height_; cd.MipLevels = cd.ArraySize = 1;
        cd.Format = DXGI_FORMAT_B8G8R8A8_UNORM; cd.SampleDesc.Count = 1; cd.Usage = D3D11_USAGE_IMMUTABLE; cd.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        D3D11_SUBRESOURCE_DATA data{pixels_.data(), width_ * 4, 0};
        Check(device->CreateTexture2D(&cd, &data, &cursor_));
        Check(device->CreateShaderResourceView(cursor_.Get(), nullptr, &cursorView_)); dirty_ = false;
    }
    D3D11_TEXTURE2D_DESC existing{}; if (output_) output_->GetDesc(&existing);
    if (!output_ || existing.Width != desc.Width || existing.Height != desc.Height) {
        target_.Reset(); output_.Reset(); backgroundView_.Reset(); background_.Reset();
        auto cd = desc; cd.Usage = D3D11_USAGE_DEFAULT; cd.CPUAccessFlags = cd.MiscFlags = 0;
        cd.MipLevels = cd.ArraySize = 1; cd.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        Check(device->CreateTexture2D(&cd, nullptr, &background_));
        Check(device->CreateShaderResourceView(background_.Get(), nullptr, &backgroundView_));
        cd.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
        Check(device->CreateTexture2D(&cd, nullptr, &output_));
        Check(device->CreateRenderTargetView(output_.Get(), nullptr, &target_));
    }
    context->CopyResource(background_.Get(), desktop);
    struct Constants { float source[2], output[2]; int position[2]; UINT size[2], type, padding[3]; };
    const Constants cb{{float(sourceWidth), float(sourceHeight)}, {float(desc.Width), float(desc.Height)},
        {position_.x, position_.y}, {width_, height_}, type_, {}};
    static_assert(sizeof(cb) == 48);
    context->UpdateSubresource(constants_.Get(), 0, nullptr, &cb, 0, 0);
    D3D11_VIEWPORT viewport{0, 0, float(desc.Width), float(desc.Height), 0, 1};
    context->RSSetViewports(1, &viewport); context->RSSetState(raster_.Get());
    context->OMSetBlendState(nullptr, nullptr, 0xffffffff); context->OMSetDepthStencilState(nullptr, 0);
    auto* target = target_.Get(); context->OMSetRenderTargets(1, &target, nullptr);
    context->IASetInputLayout(nullptr); context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    context->VSSetShader(vertex_.Get(), nullptr, 0); context->GSSetShader(nullptr, nullptr, 0);
    context->PSSetShader(pixel_.Get(), nullptr, 0);
    auto* constant = constants_.Get(); context->PSSetConstantBuffers(0, 1, &constant);
    ID3D11ShaderResourceView* views[]{backgroundView_.Get(), cursorView_.Get()}; context->PSSetShaderResources(0, 2, views);
    context->Draw(3, 0);
    ID3D11ShaderResourceView* empty[]{nullptr, nullptr}; context->PSSetShaderResources(0, 2, empty);
    context->OMSetRenderTargets(0, nullptr, nullptr);
    return output_.Get();
}
}
