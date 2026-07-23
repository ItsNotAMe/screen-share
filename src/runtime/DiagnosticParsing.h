#pragma once

#include <cmath>
#include <optional>
#include <string>

namespace screenshare_runtime_internal {

inline std::optional<double> ParseDiagnosticDouble(const std::string& text)
{
    if (text.empty()) {
        return std::nullopt;
    }
    try {
        size_t parsed = 0;
        const double value = std::stod(text, &parsed);
        if (parsed != text.size() || !std::isfinite(value)) {
            return std::nullopt;
        }
        return value;
    } catch (...) {
        return std::nullopt;
    }
}

} // namespace screenshare_runtime_internal
