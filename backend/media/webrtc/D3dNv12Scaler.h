#pragma once
#include <d3d11.h>
#include <wrl/client.h>
#include "media/GpuSubmissionQueue.h"

namespace screenshare::media {
// Used only on D3dVideoDevice's owner. Source/output pixels stay on the GPU;
// every output is a new owned resource, never a mutable shared scratch texture.
class D3dNv12Scaler {
public:
    explicit D3dNv12Scaler(ID3D11Device* device);
    Microsoft::WRL::ComPtr<ID3D11Texture2D> Scale(ID3D11Device* device, ID3D11DeviceContext* context,
        ID3D11Texture2D* input, int width, int height, int left, int top, int imageWidth, int imageHeight);
    unsigned pending() const { return unsigned(pending_.size()); }
private:
    Microsoft::WRL::ComPtr<ID3D11VertexShader> vertex_;
    Microsoft::WRL::ComPtr<ID3D11PixelShader> pixel_;
    Microsoft::WRL::ComPtr<ID3D11SamplerState> sampler_;
    GpuSubmissionQueue<Microsoft::WRL::ComPtr<ID3D11Query>> pending_;
};
}
