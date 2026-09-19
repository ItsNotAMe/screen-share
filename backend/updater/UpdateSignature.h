#pragma once

#include <cstddef>
#include <span>

namespace screenshare {

// Verify an ECDSA-P256 (prime256v1) signature over `message` against a pinned
// public key, used to authenticate the auto-update manifest before anything is
// downloaded or installed.
//
// - publicKeyXy: the raw 64-byte uncompressed public key (X||Y, 32 bytes each),
//   i.e. an X9.62 point without the leading 0x04 byte.
// - signature:   the raw 64-byte signature (R||S, 32 bytes each). This is the
//   IEEE-P1363 form, not ASN.1/DER.
// - message:     the exact bytes that were signed.
//
// The message is hashed with SHA-256 internally (matching `openssl dgst
// -sha256`). Returns true ONLY for a valid signature; any malformed input,
// wrong-size buffer, or crypto error returns false (fail closed).
[[nodiscard]] bool VerifyUpdateManifestSignatureEcdsaP256(
    std::span<const std::byte> publicKeyXy,
    std::span<const std::byte> message,
    std::span<const std::byte> signature);

} // namespace screenshare
