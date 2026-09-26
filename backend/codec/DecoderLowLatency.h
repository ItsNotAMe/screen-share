#pragma once
#include "core/MediaOperationError.h"
#include <Windows.h>
#include <codecapi.h>
#include <vfwmsgs.h>

namespace screenshare {
// Low latency is optional on older decoders. A supported property that fails
// to apply is still an error; only a missing interface/property is compatible.
inline bool DecoderLowLatencyUnavailable(HRESULT result) {
    return result == E_NOINTERFACE || result == E_NOTIMPL || result == E_PROP_ID_UNSUPPORTED;
}
template<class Supported, class Set, class Get>
bool ConfigureDecoderLowLatencyProperty(HRESULT queried, Supported supported, Set set, Get get) {
    if (DecoderLowLatencyUnavailable(queried)) return false;
    if (FAILED(queried)) throw MediaOperationError("decoder-codec-api", queried, "Decoder codec API failed");
    const HRESULT support = supported();
    if (support == S_FALSE || DecoderLowLatencyUnavailable(support)) return false;
    if (FAILED(support)) throw MediaOperationError("decoder-low-latency-support", support, "Decoder low-latency query failed");
    const HRESULT applied = set();
    if (FAILED(applied)) throw MediaOperationError("decoder-low-latency-set", applied, "Decoder low-latency request failed");
    VARIANT actual{};
    const HRESULT read = get(&actual);
    const bool enabled = SUCCEEDED(read) && actual.vt == VT_UI4 && actual.ulVal != 0;
    VariantClear(&actual);
    if (!enabled) throw MediaOperationError("decoder-low-latency-readback", FAILED(read) ? read : E_FAIL, "Decoder low-latency readback failed");
    return true;
}
}
