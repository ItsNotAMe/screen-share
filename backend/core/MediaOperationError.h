#pragma once
#include <cstdint>
#include <stdexcept>
#include <string>

namespace screenshare {
// The operation is an internal constant, not a device name or remote payload.
class MediaOperationError : public std::runtime_error {
public:
    MediaOperationError(const char* operation, int32_t code, std::string message)
        : std::runtime_error(std::move(message)), operation(operation), code(code) {}
    const char* operation;
    int32_t code;
};
}
