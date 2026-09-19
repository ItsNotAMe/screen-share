#pragma once
#include <d3d11.h>
#include <dxgi1_2.h>
#include <wrl/client.h>
#include <span>
#include <cstddef>
#include <vector>

namespace screenshare {
// Capture-owner only. Pointer bytes are bounded; desktop pixels never leave the GPU.
class DxgiCursor {
public:
    void Update(IDXGIOutputDuplication* duplication, const DXGI_OUTDUPL_FRAME_INFO& frame);
    void Shape(const DXGI_OUTDUPL_POINTER_SHAPE_INFO& info, std::span<const std::byte> bytes);
    void Position(POINT position, bool visible) { position_ = position; visible_ = visible; }
    ID3D11Texture2D* Composite(ID3D11Device* device, ID3D11DeviceContext* context,
        ID3D11Texture2D* desktop, UINT sourceWidth, UINT sourceHeight);
private:
    template<class T> using ComPtr = Microsoft::WRL::ComPtr<T>;
    POINT position_{};
    bool visible_ = false, dirty_ = false;
    UINT width_ = 0, height_ = 0, type_ = 0;
    std::vector<std::byte> pixels_;
    ComPtr<ID3D11VertexShader> vertex_;
    ComPtr<ID3D11PixelShader> pixel_;
    ComPtr<ID3D11Buffer> constants_;
    ComPtr<ID3D11RasterizerState> raster_;
    ComPtr<ID3D11Texture2D> cursor_, background_, output_;
    ComPtr<ID3D11ShaderResourceView> cursorView_, backgroundView_;
    ComPtr<ID3D11RenderTargetView> target_;
};
}
