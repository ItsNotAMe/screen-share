#pragma once
#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <optional>
#include <span>
#include <stdexcept>

namespace proof {
// Fixture-only visual timestamp association. Pixels cross the real codec/link;
// the timestamp table stays local. Never use this as a cross-machine clock.
class FrameAgeMarker {
public:
    using Clock = std::chrono::steady_clock;
    static constexpr int Width = 256, Height = 16;
    void Stamp(std::span<uint8_t> luma, int stride, Clock::time_point captured) {
        if (stride < Width || luma.size() < size_t(stride) * Height || next_ == 65535)
            throw std::runtime_error("Invalid/exhausted frame-age marker");
        const uint16_t id = ++next_;
        const uint32_t word = uint32_t(id) | (uint32_t(Checksum(id)) << 16);
        for (int y = 0; y < Height; ++y) for (int x = 0; x < Width; ++x)
            luma[size_t(y) * stride + x] = (((word >> (x / 8)) & 1) != (y >= 8)) ? 230 : 25;
        times_[id].store(std::chrono::duration_cast<std::chrono::nanoseconds>(captured.time_since_epoch()).count(),
            std::memory_order_release);
    }
    std::optional<Clock::time_point> Read(std::span<const uint8_t> luma, int stride) const {
        if (stride < Width || luma.size() < size_t(stride) * Height) return {};
        uint32_t word = 0;
        for (int bit = 0; bit < 32; ++bit) {
            unsigned upper = 0, lower = 0;
            for (int y = 2; y < 6; ++y) for (int x = 2; x < 6; ++x) {
                upper += luma[size_t(y) * stride + bit * 8 + x];
                lower += luma[size_t(y + 8) * stride + bit * 8 + x];
            }
            if (upper > 16 * 170 && lower < 16 * 85) word |= uint32_t(1) << bit;
            else if (!(upper < 16 * 85 && lower > 16 * 170)) return {};
        }
        const uint16_t id = uint16_t(word);
        if (!id || Checksum(id) != uint16_t(word >> 16)) return {};
        const auto ns = times_[id].load(std::memory_order_acquire);
        if (!ns) return {};
        return Clock::time_point(std::chrono::duration_cast<Clock::duration>(std::chrono::nanoseconds(ns)));
    }
private:
    static uint16_t Checksum(uint16_t id) {
        uint16_t crc = 0xffff;
        for (unsigned byte : {unsigned(id >> 8), unsigned(id & 255)}) {
            crc ^= uint16_t(byte << 8);
            for (int i = 0; i < 8; ++i) crc = uint16_t((crc << 1) ^ ((crc & 0x8000) ? 0x1021 : 0));
        }
        return crc;
    }
    uint16_t next_ = 0; // One capture owner writes; readers use the atomic table.
    std::array<std::atomic<int64_t>, 65536> times_{};
};
}
