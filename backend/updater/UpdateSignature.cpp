#include "updater/UpdateSignature.h"

#include <windows.h>
#include <bcrypt.h>

#include <array>
#include <cstring>
#include <limits>
#include <vector>

namespace screenshare {
namespace {

constexpr size_t EcdsaP256CoordinateBytes = 32;
constexpr size_t EcdsaP256PublicKeyBytes = 2 * EcdsaP256CoordinateBytes; // X||Y
constexpr size_t EcdsaP256SignatureBytes = 2 * EcdsaP256CoordinateBytes; // R||S

bool BcryptOk(NTSTATUS status) noexcept
{
    return status >= 0;
}

// SHA-256 of a message via CNG. Returns false on any error.
bool Sha256(std::span<const std::byte> message, std::array<std::byte, 32>& digest)
{
    if (message.size() > (std::numeric_limits<ULONG>::max)()) {
        return false;
    }

    BCRYPT_ALG_HANDLE algorithm = nullptr;
    if (!BcryptOk(BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0))) {
        return false;
    }

    bool ok = false;
    BCRYPT_HASH_HANDLE hash = nullptr;
    if (BcryptOk(BCryptCreateHash(algorithm, &hash, nullptr, 0, nullptr, 0, 0))) {
        if (BcryptOk(BCryptHashData(
                hash,
                reinterpret_cast<PUCHAR>(const_cast<std::byte*>(message.data())),
                static_cast<ULONG>(message.size()),
                0))) {
            ok = BcryptOk(BCryptFinishHash(
                hash, reinterpret_cast<PUCHAR>(digest.data()), static_cast<ULONG>(digest.size()), 0));
        }
        BCryptDestroyHash(hash);
    }

    BCryptCloseAlgorithmProvider(algorithm, 0);
    return ok;
}

} // namespace

bool VerifyUpdateManifestSignatureEcdsaP256(
    std::span<const std::byte> publicKeyXy,
    std::span<const std::byte> message,
    std::span<const std::byte> signature)
{
    // Fail closed on any shape mismatch (also covers an unconfigured / empty
    // pinned key, which must never be treated as a valid verifier).
    if (publicKeyXy.size() != EcdsaP256PublicKeyBytes ||
        signature.size() != EcdsaP256SignatureBytes ||
        message.empty()) {
        return false;
    }

    std::array<std::byte, 32> digest{};
    if (!Sha256(message, digest)) {
        return false;
    }

    // Build a BCRYPT_ECCPUBLIC_BLOB: header + X + Y.
    std::vector<std::byte> blob(sizeof(BCRYPT_ECCKEY_BLOB) + EcdsaP256PublicKeyBytes);
    auto* header = reinterpret_cast<BCRYPT_ECCKEY_BLOB*>(blob.data());
    header->dwMagic = BCRYPT_ECDSA_PUBLIC_P256_MAGIC;
    header->cbKey = static_cast<ULONG>(EcdsaP256CoordinateBytes);
    std::memcpy(blob.data() + sizeof(BCRYPT_ECCKEY_BLOB), publicKeyXy.data(), EcdsaP256PublicKeyBytes);

    BCRYPT_ALG_HANDLE algorithm = nullptr;
    if (!BcryptOk(BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_ECDSA_P256_ALGORITHM, nullptr, 0))) {
        return false;
    }

    bool verified = false;
    BCRYPT_KEY_HANDLE key = nullptr;
    NTSTATUS status = BCryptImportKeyPair(
        algorithm,
        nullptr,
        BCRYPT_ECCPUBLIC_BLOB,
        &key,
        reinterpret_cast<PUCHAR>(blob.data()),
        static_cast<ULONG>(blob.size()),
        0);
    if (BcryptOk(status)) {
        status = BCryptVerifySignature(
            key,
            nullptr,
            reinterpret_cast<PUCHAR>(digest.data()),
            static_cast<ULONG>(digest.size()),
            reinterpret_cast<PUCHAR>(const_cast<std::byte*>(signature.data())),
            static_cast<ULONG>(signature.size()),
            0);
        verified = BcryptOk(status); // STATUS_INVALID_SIGNATURE is negative -> false
        BCryptDestroyKey(key);
    }

    BCryptCloseAlgorithmProvider(algorithm, 0);
    return verified;
}

} // namespace screenshare
