#include "codec/H264Bitstream.h"

#include <cstdint>
#include <optional>

namespace screenshare {
namespace {

constexpr uint8_t NalUnitTypeMask = 0x1F;
constexpr uint8_t NalTypeIdrSlice = 5;
constexpr uint8_t NalTypeSps = 7;
constexpr uint8_t NalTypePps = 8;

struct StartCode {
    size_t offset = 0;
    size_t bytes = 0;
};

uint8_t ByteAt(std::span<const std::byte> bytes, size_t offset)
{
    return static_cast<uint8_t>(bytes[offset]);
}

std::optional<StartCode> FindAnnexBStartCode(std::span<const std::byte> bytes, size_t offset)
{
    for (size_t i = offset; i + 2 < bytes.size(); ++i) {
        if (ByteAt(bytes, i) != 0 || ByteAt(bytes, i + 1) != 0) {
            continue;
        }
        if (ByteAt(bytes, i + 2) == 1) {
            return StartCode{i, 3};
        }
        if (i + 3 < bytes.size() && ByteAt(bytes, i + 2) == 0 && ByteAt(bytes, i + 3) == 1) {
            return StartCode{i, 4};
        }
    }

    return std::nullopt;
}

void AddNalUnit(H264AccessUnitInfo& info, uint8_t nalHeader)
{
    switch (nalHeader & NalUnitTypeMask) {
    case NalTypeIdrSlice:
        info.hasIdrSlice = true;
        break;
    case NalTypeSps:
        info.hasSps = true;
        break;
    case NalTypePps:
        info.hasPps = true;
        break;
    default:
        break;
    }
}

bool InspectAnnexB(std::span<const std::byte> bytes, H264AccessUnitInfo& info)
{
    auto startCode = FindAnnexBStartCode(bytes, 0);
    if (!startCode) {
        return false;
    }

    while (startCode) {
        size_t nalStart = startCode->offset + startCode->bytes;
        const auto nextStartCode = FindAnnexBStartCode(bytes, nalStart);
        const size_t nalEnd = nextStartCode ? nextStartCode->offset : bytes.size();

        while (nalStart < nalEnd && ByteAt(bytes, nalStart) == 0) {
            ++nalStart;
        }
        if (nalStart < nalEnd) {
            AddNalUnit(info, ByteAt(bytes, nalStart));
        }

        startCode = nextStartCode;
    }

    return true;
}

uint32_t ReadBigEndianLength(std::span<const std::byte> bytes, size_t offset, size_t lengthBytes)
{
    uint32_t value = 0;
    for (size_t i = 0; i < lengthBytes; ++i) {
        value = (value << 8) | ByteAt(bytes, offset + i);
    }
    return value;
}

bool InspectLengthPrefixed(std::span<const std::byte> bytes, size_t lengthBytes, H264AccessUnitInfo& info)
{
    size_t offset = 0;
    bool sawNalUnit = false;

    while (offset + lengthBytes <= bytes.size()) {
        const uint32_t nalBytes = ReadBigEndianLength(bytes, offset, lengthBytes);
        offset += lengthBytes;

        if (nalBytes == 0 || nalBytes > bytes.size() - offset) {
            return false;
        }

        AddNalUnit(info, ByteAt(bytes, offset));
        sawNalUnit = true;
        offset += nalBytes;
    }

    return sawNalUnit && offset == bytes.size();
}

} // namespace

H264AccessUnitInfo InspectH264AccessUnit(std::span<const std::byte> bytes)
{
    H264AccessUnitInfo info;
    if (bytes.empty()) {
        return info;
    }

    if (InspectAnnexB(bytes, info)) {
        return info;
    }

    H264AccessUnitInfo lengthPrefixed4;
    if (InspectLengthPrefixed(bytes, 4, lengthPrefixed4)) {
        return lengthPrefixed4;
    }

    H264AccessUnitInfo lengthPrefixed2;
    if (InspectLengthPrefixed(bytes, 2, lengthPrefixed2)) {
        return lengthPrefixed2;
    }

    return info;
}

} // namespace screenshare
