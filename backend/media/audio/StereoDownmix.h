#pragma once
#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <span>
#include <stdexcept>

namespace screenshare::media {
// PCM16 in WAVE speaker-mask order. Mono is duplicated; stereo is unchanged.
// Center/surround contribute -3 dB, LFE is omitted. Fixed normalization supplies
// headroom for coherent full-scale channels without clipping or pumping gain.
class StereoDownmix {
    unsigned channels_;
    std::array<std::array<double, 2>, 8> weights_{};
public:
    StereoDownmix(unsigned channels, uint32_t mask) : channels_(channels) {
        if (channels == 1 && !mask) mask = 4;
        if (channels == 2 && !mask) mask = 3;
        if (!channels || channels > 8 || std::popcount(mask) != channels || (mask & ~0x7ffu))
            throw std::invalid_argument("Unsupported audio speaker layout");
        unsigned index = 0;
        for (unsigned bit = 0; bit < 11; ++bit) if (mask & (1u << bit)) {
            auto& weight = weights_[index++];
            switch (bit) {
            case 0: weight = {1, 0}; break; case 1: weight = {0, 1}; break;
            case 2: case 8: weight = {0.7071067811865476, 0.7071067811865476}; break;
            case 4: case 6: case 9: weight = {0.7071067811865476, 0}; break;
            case 5: case 7: case 10: weight = {0, 0.7071067811865476}; break;
            case 3: break; // LFE is not a full-range channel.
            }
        }
        if (channels == 1 && mask != 8) weights_[0] = {1, 1};
        double left = 0, right = 0;
        for (const auto& weight : weights_) { left += weight[0]; right += weight[1]; }
        if (!left && !right) throw std::invalid_argument("No full-range audio channels");
        for (auto& weight : weights_) { weight[0] /= std::max(1.0, left); weight[1] /= std::max(1.0, right); }
    }
    void Convert(std::span<const std::byte> input, std::span<int16_t> output) const {
        if (output.size() % 2 || input.size() != output.size() / 2 * channels_ * 2)
            throw std::invalid_argument("Invalid downmix PCM block");
        for (size_t frame = 0; frame < output.size() / 2; ++frame) {
            double left = 0, right = 0;
            for (unsigned channel = 0; channel < channels_; ++channel) {
                int16_t sample;
                std::memcpy(&sample, input.data() + (frame * channels_ + channel) * 2, 2);
                left += sample * weights_[channel][0]; right += sample * weights_[channel][1];
            }
            output[frame * 2] = int16_t(std::clamp(std::lround(left), -32768l, 32767l));
            output[frame * 2 + 1] = int16_t(std::clamp(std::lround(right), -32768l, 32767l));
        }
    }
};
}
