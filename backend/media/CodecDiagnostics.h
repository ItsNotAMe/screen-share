#pragma once
#include "DiagnosticHistory.h"
#include "core/MediaOperationError.h"

namespace screenshare::media {
inline void CodecFailure(const std::shared_ptr<DiagnosticHistory>& diagnostics, const char* stage, const std::exception& error) {
    if (!diagnostics) return;
    DiagnosticRecord record;
    record.labels["event"] = stage;
    if (const auto* native = dynamic_cast<const screenshare::MediaOperationError*>(&error)) {
        record.labels["operation"] = native->operation;
        record.numbers["hresult"] = uint32_t(native->code);
    }
    diagnostics->Add(std::move(record));
}
}
