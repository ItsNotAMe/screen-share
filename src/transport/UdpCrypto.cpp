#include "transport/UdpCrypto.h"

#include <windows.h>
#include <bcrypt.h>

#include <algorithm>
#include <cstring>
#include <limits>
#include <stdexcept>
#include <string>
#include <cwchar>

namespace screenshare {
namespace {

constexpr std::array<std::byte, 41> KeyDerivationSalt{
    std::byte{'S'}, std::byte{'c'}, std::byte{'r'}, std::byte{'e'},
    std::byte{'e'}, std::byte{'n'}, std::byte{'S'}, std::byte{'h'},
    std::byte{'a'}, std::byte{'r'}, std::byte{'e'}, std::byte{' '},
    std::byte{'l'}, std::byte{'o'}, std::byte{'c'}, std::byte{'a'},
    std::byte{'l'}, std::byte{' '}, std::byte{'U'}, std::byte{'D'},
    std::byte{'P'}, std::byte{' '}, std::byte{'e'}, std::byte{'n'},
    std::byte{'c'}, std::byte{'r'}, std::byte{'y'}, std::byte{'p'},
    std::byte{'t'}, std::byte{'i'}, std::byte{'o'}, std::byte{'n'},
    std::byte{' '}, std::byte{'v'}, std::byte{'1'}, std::byte{':'},
    std::byte{'k'}, std::byte{'d'}, std::byte{'f'}, std::byte{':'},
    std::byte{'1'},
};
constexpr ULONGLONG KeyDerivationIterations = 100'000;

bool BcryptOk(NTSTATUS status) noexcept
{
    return status >= 0;
}

std::string BcryptErrorMessage(const char* operation, NTSTATUS status)
{
    return std::string(operation) + " failed with NTSTATUS 0x" + std::to_string(static_cast<unsigned long>(status));
}

void ThrowIfBcryptFailed(const char* operation, NTSTATUS status)
{
    if (!BcryptOk(status)) {
        throw std::runtime_error(BcryptErrorMessage(operation, status));
    }
}

ULONG CheckedUlongSize(size_t value, const char* name)
{
    if (value > std::numeric_limits<ULONG>::max()) {
        throw std::runtime_error(std::string(name) + " is too large for Windows CNG");
    }
    return static_cast<ULONG>(value);
}

void WriteBigEndian16(std::span<std::byte> bytes, size_t offset, uint16_t value)
{
    bytes[offset] = static_cast<std::byte>((value >> 8) & 0xFFU);
    bytes[offset + 1] = static_cast<std::byte>(value & 0xFFU);
}

void WriteBigEndian32(std::span<std::byte> bytes, size_t offset, uint32_t value)
{
    bytes[offset] = static_cast<std::byte>((value >> 24) & 0xFFU);
    bytes[offset + 1] = static_cast<std::byte>((value >> 16) & 0xFFU);
    bytes[offset + 2] = static_cast<std::byte>((value >> 8) & 0xFFU);
    bytes[offset + 3] = static_cast<std::byte>(value & 0xFFU);
}

void WriteBigEndian48(std::span<std::byte> bytes, size_t offset, uint64_t value)
{
    bytes[offset] = static_cast<std::byte>((value >> 40) & 0xFFU);
    bytes[offset + 1] = static_cast<std::byte>((value >> 32) & 0xFFU);
    bytes[offset + 2] = static_cast<std::byte>((value >> 24) & 0xFFU);
    bytes[offset + 3] = static_cast<std::byte>((value >> 16) & 0xFFU);
    bytes[offset + 4] = static_cast<std::byte>((value >> 8) & 0xFFU);
    bytes[offset + 5] = static_cast<std::byte>(value & 0xFFU);
}

std::array<std::byte, 32> HmacSha256(
    std::span<const std::byte> key,
    std::span<const std::byte> data)
{
    BCRYPT_ALG_HANDLE algorithm = nullptr;
    ThrowIfBcryptFailed(
        "BCryptOpenAlgorithmProvider(SHA256/HMAC)",
        BCryptOpenAlgorithmProvider(
            &algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, BCRYPT_ALG_HANDLE_HMAC_FLAG));

    BCRYPT_HASH_HANDLE hash = nullptr;
    NTSTATUS status = BCryptCreateHash(
        algorithm,
        &hash,
        nullptr,
        0,
        reinterpret_cast<PUCHAR>(const_cast<std::byte*>(key.data())),
        CheckedUlongSize(key.size(), "HMAC key"),
        0);
    if (!BcryptOk(status)) {
        BCryptCloseAlgorithmProvider(algorithm, 0);
        ThrowIfBcryptFailed("BCryptCreateHash(HMAC-SHA256)", status);
    }

    std::array<std::byte, 32> output{};
    status = BCryptHashData(
        hash,
        reinterpret_cast<PUCHAR>(const_cast<std::byte*>(data.data())),
        CheckedUlongSize(data.size(), "HMAC data"),
        0);
    if (BcryptOk(status)) {
        status = BCryptFinishHash(
            hash,
            reinterpret_cast<PUCHAR>(output.data()),
            CheckedUlongSize(output.size(), "HMAC output"),
            0);
    }

    BCryptDestroyHash(hash);
    BCryptCloseAlgorithmProvider(algorithm, 0);
    ThrowIfBcryptFailed("BCryptHashData/FinishHash(HMAC-SHA256)", status);
    return output;
}

} // namespace

bool UdpCryptoKey::valid() const noexcept
{
    return std::any_of(bytes.begin(), bytes.end(), [](std::byte value) {
        return value != std::byte{0};
    });
}

uint64_t UdpAccessCodeFingerprint(std::string_view accessCode)
{
    uint64_t hash = 14695981039346656037ULL;
    const auto mix = [&hash](unsigned char value) {
        hash ^= value;
        hash *= 1099511628211ULL;
    };

    constexpr std::string_view prefix = "screenshare-access-code-v1:";
    for (const char ch : prefix) {
        mix(static_cast<unsigned char>(ch));
    }
    for (const char ch : accessCode) {
        mix(static_cast<unsigned char>(ch));
    }
    return hash == 0 ? 1 : hash;
}

UdpCryptoKey DeriveUdpCryptoKey(std::string_view accessCode)
{
    if (accessCode.empty()) {
        throw std::invalid_argument("Access code is required for UDP encryption");
    }

    BCRYPT_ALG_HANDLE prfAlgorithm = nullptr;
    ThrowIfBcryptFailed(
        "BCryptOpenAlgorithmProvider(SHA256/HMAC)",
        BCryptOpenAlgorithmProvider(
            &prfAlgorithm,
            BCRYPT_SHA256_ALGORITHM,
            nullptr,
            BCRYPT_ALG_HANDLE_HMAC_FLAG));

    UdpCryptoKey key;
    const NTSTATUS deriveStatus = BCryptDeriveKeyPBKDF2(
        prfAlgorithm,
        reinterpret_cast<PUCHAR>(const_cast<char*>(accessCode.data())),
        CheckedUlongSize(accessCode.size(), "Access code"),
        reinterpret_cast<PUCHAR>(const_cast<std::byte*>(KeyDerivationSalt.data())),
        CheckedUlongSize(KeyDerivationSalt.size(), "UDP crypto salt"),
        KeyDerivationIterations,
        reinterpret_cast<PUCHAR>(key.bytes.data()),
        CheckedUlongSize(key.bytes.size(), "UDP crypto key"),
        0);

    BCryptCloseAlgorithmProvider(prfAlgorithm, 0);
    ThrowIfBcryptFailed("BCryptDeriveKeyPBKDF2", deriveStatus);
    return key;
}

UdpCryptoKey DeriveUdpSessionKey(
    const UdpCryptoKey& master,
    const UdpCryptoSessionSalt& salt)
{
    if (!master.valid()) {
        throw std::invalid_argument("UDP session key derivation requires a valid master key");
    }

    // HKDF-SHA256 (RFC 5869). Extract with the per-session salt, then a single
    // expand block (output is 32 bytes = one HMAC block). The sub-streams
    // (video/audio/feedback/control) share this key but occupy disjoint nonce
    // spaces via the role byte in the nonce, so no fixed info label is needed.
    const std::array<std::byte, 32> prk = HmacSha256(
        std::span<const std::byte>(salt.data(), salt.size()),
        std::span<const std::byte>(master.bytes.data(), master.bytes.size()));

    constexpr std::string_view info = "screenshare-udp-session-v2\x01";
    std::array<std::byte, 27> infoBytes{};
    for (size_t index = 0; index < info.size(); ++index) {
        infoBytes[index] = static_cast<std::byte>(static_cast<unsigned char>(info[index]));
    }

    const std::array<std::byte, 32> okm = HmacSha256(
        std::span<const std::byte>(prk.data(), prk.size()),
        std::span<const std::byte>(infoBytes.data(), infoBytes.size()));

    UdpCryptoKey key;
    std::copy(okm.begin(), okm.end(), key.bytes.begin());
    return key;
}

UdpCryptoSessionSalt GenerateUdpCryptoSessionSalt()
{
    UdpCryptoSessionSalt salt{};
    ThrowIfBcryptFailed(
        "BCryptGenRandom",
        BCryptGenRandom(
            nullptr,
            reinterpret_cast<PUCHAR>(salt.data()),
            CheckedUlongSize(salt.size(), "UDP crypto session salt"),
            BCRYPT_USE_SYSTEM_PREFERRED_RNG));
    return salt;
}

std::array<std::byte, UdpCryptoNonceBytes> GenerateUdpCryptoNonce()
{
    std::array<std::byte, UdpCryptoNonceBytes> nonce{};
    ThrowIfBcryptFailed(
        "BCryptGenRandom",
        BCryptGenRandom(
            nullptr,
            reinterpret_cast<PUCHAR>(nonce.data()),
            CheckedUlongSize(nonce.size(), "UDP crypto nonce"),
            BCRYPT_USE_SYSTEM_PREFERRED_RNG));
    return nonce;
}

std::string GenerateUdpAccessCode()
{
    constexpr std::array<char, 32> Alphabet{
        'A', 'B', 'C', 'D', 'E', 'F', 'G', 'H',
        'J', 'K', 'L', 'M', 'N', 'P', 'Q', 'R',
        'S', 'T', 'U', 'V', 'W', 'X', 'Y', 'Z',
        '2', '3', '4', '5', '6', '7', '8', '9',
    };
    constexpr size_t AccessCodeCharacters = 20;
    constexpr size_t AccessCodeGroupSize = 5;

    std::array<unsigned char, AccessCodeCharacters> randomBytes{};
    ThrowIfBcryptFailed(
        "BCryptGenRandom",
        BCryptGenRandom(
            nullptr,
            randomBytes.data(),
            CheckedUlongSize(randomBytes.size(), "UDP access code random bytes"),
            BCRYPT_USE_SYSTEM_PREFERRED_RNG));

    std::string code;
    code.reserve(AccessCodeCharacters + (AccessCodeCharacters / AccessCodeGroupSize));
    for (size_t index = 0; index < randomBytes.size(); ++index) {
        if (index > 0 && index % AccessCodeGroupSize == 0) {
            code.push_back('-');
        }
        code.push_back(Alphabet[randomBytes[index] & 0x1FU]);
    }
    return code;
}

void WriteUdpCryptoNonce(
    std::span<std::byte, UdpCryptoNonceBytes> nonce,
    UdpCryptoRole role,
    uint64_t mediaId,
    uint16_t fragmentIndex)
{
    // [role:1][zero:3][mediaId:6][fragmentIndex:2]. The role byte gives each
    // sub-stream a disjoint nonce space under the shared per-session key;
    // uniqueness within a stream comes from the monotonic id + fragment index,
    // not a random prefix.
    WriteBigEndian32(nonce, 0, static_cast<uint32_t>(static_cast<uint8_t>(role)) << 24);
    WriteBigEndian48(nonce, 4, mediaId & 0x0000FFFFFFFFFFFFULL);
    WriteBigEndian16(nonce, 10, fragmentIndex);
}

UdpAesGcm::UdpAesGcm(const UdpCryptoKey& key)
{
    if (!key.valid()) {
        throw std::invalid_argument("UDP encryption key is empty");
    }

    BCRYPT_ALG_HANDLE algorithm = nullptr;
    ThrowIfBcryptFailed(
        "BCryptOpenAlgorithmProvider(AES)",
        BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_AES_ALGORITHM, nullptr, 0));

    const NTSTATUS modeStatus = BCryptSetProperty(
        algorithm,
        BCRYPT_CHAINING_MODE,
        reinterpret_cast<PUCHAR>(const_cast<wchar_t*>(BCRYPT_CHAIN_MODE_GCM)),
        static_cast<ULONG>((std::wcslen(BCRYPT_CHAIN_MODE_GCM) + 1) * sizeof(wchar_t)),
        0);
    if (!BcryptOk(modeStatus)) {
        BCryptCloseAlgorithmProvider(algorithm, 0);
        ThrowIfBcryptFailed("BCryptSetProperty(AES-GCM)", modeStatus);
    }

    ULONG objectLength = 0;
    ULONG resultBytes = 0;
    const NTSTATUS lengthStatus = BCryptGetProperty(
        algorithm,
        BCRYPT_OBJECT_LENGTH,
        reinterpret_cast<PUCHAR>(&objectLength),
        sizeof(objectLength),
        &resultBytes,
        0);
    if (!BcryptOk(lengthStatus)) {
        BCryptCloseAlgorithmProvider(algorithm, 0);
        ThrowIfBcryptFailed("BCryptGetProperty(BCRYPT_OBJECT_LENGTH)", lengthStatus);
    }

    keyObject_.assign(objectLength, std::byte{0});
    BCRYPT_KEY_HANDLE keyHandle = nullptr;
    const NTSTATUS keyStatus = BCryptGenerateSymmetricKey(
        algorithm,
        &keyHandle,
        reinterpret_cast<PUCHAR>(keyObject_.data()),
        objectLength,
        reinterpret_cast<PUCHAR>(const_cast<std::byte*>(key.bytes.data())),
        CheckedUlongSize(key.bytes.size(), "UDP AES key"),
        0);
    if (!BcryptOk(keyStatus)) {
        BCryptCloseAlgorithmProvider(algorithm, 0);
        ThrowIfBcryptFailed("BCryptGenerateSymmetricKey", keyStatus);
    }

    algorithm_ = algorithm;
    key_ = keyHandle;
}

UdpAesGcm::~UdpAesGcm()
{
    if (key_ != nullptr) {
        BCryptDestroyKey(static_cast<BCRYPT_KEY_HANDLE>(key_));
        key_ = nullptr;
    }
    if (algorithm_ != nullptr) {
        BCryptCloseAlgorithmProvider(static_cast<BCRYPT_ALG_HANDLE>(algorithm_), 0);
        algorithm_ = nullptr;
    }
}

std::vector<std::byte> UdpAesGcm::Encrypt(
    std::span<const std::byte> nonce,
    std::span<const std::byte> authenticatedData,
    std::span<const std::byte> plaintext,
    std::span<std::byte, UdpCryptoTagBytes> tag) const
{
    std::vector<std::byte> ciphertext(plaintext.size());

    BCRYPT_AUTHENTICATED_CIPHER_MODE_INFO authInfo;
    BCRYPT_INIT_AUTH_MODE_INFO(authInfo);
    authInfo.pbNonce = reinterpret_cast<PUCHAR>(const_cast<std::byte*>(nonce.data()));
    authInfo.cbNonce = CheckedUlongSize(nonce.size(), "UDP crypto nonce");
    authInfo.pbAuthData = reinterpret_cast<PUCHAR>(const_cast<std::byte*>(authenticatedData.data()));
    authInfo.cbAuthData = CheckedUlongSize(authenticatedData.size(), "UDP crypto authenticated data");
    authInfo.pbTag = reinterpret_cast<PUCHAR>(tag.data());
    authInfo.cbTag = CheckedUlongSize(tag.size(), "UDP crypto tag");

    ULONG outputBytes = 0;
    ThrowIfBcryptFailed(
        "BCryptEncrypt(AES-GCM)",
        BCryptEncrypt(
            static_cast<BCRYPT_KEY_HANDLE>(key_),
            reinterpret_cast<PUCHAR>(const_cast<std::byte*>(plaintext.data())),
            CheckedUlongSize(plaintext.size(), "UDP crypto plaintext"),
            &authInfo,
            nullptr,
            0,
            reinterpret_cast<PUCHAR>(ciphertext.data()),
            CheckedUlongSize(ciphertext.size(), "UDP crypto ciphertext"),
            &outputBytes,
            0));
    if (outputBytes != ciphertext.size()) {
        throw std::runtime_error("BCryptEncrypt(AES-GCM) returned an unexpected ciphertext size");
    }

    return ciphertext;
}

std::optional<std::vector<std::byte>> UdpAesGcm::Decrypt(
    std::span<const std::byte> nonce,
    std::span<const std::byte> authenticatedData,
    std::span<const std::byte> ciphertext,
    std::span<const std::byte, UdpCryptoTagBytes> tag) const
{
    std::vector<std::byte> plaintext(ciphertext.size());

    BCRYPT_AUTHENTICATED_CIPHER_MODE_INFO authInfo;
    BCRYPT_INIT_AUTH_MODE_INFO(authInfo);
    authInfo.pbNonce = reinterpret_cast<PUCHAR>(const_cast<std::byte*>(nonce.data()));
    authInfo.cbNonce = CheckedUlongSize(nonce.size(), "UDP crypto nonce");
    authInfo.pbAuthData = reinterpret_cast<PUCHAR>(const_cast<std::byte*>(authenticatedData.data()));
    authInfo.cbAuthData = CheckedUlongSize(authenticatedData.size(), "UDP crypto authenticated data");
    authInfo.pbTag = reinterpret_cast<PUCHAR>(const_cast<std::byte*>(tag.data()));
    authInfo.cbTag = CheckedUlongSize(tag.size(), "UDP crypto tag");

    ULONG outputBytes = 0;
    const NTSTATUS status = BCryptDecrypt(
        static_cast<BCRYPT_KEY_HANDLE>(key_),
        reinterpret_cast<PUCHAR>(const_cast<std::byte*>(ciphertext.data())),
        CheckedUlongSize(ciphertext.size(), "UDP crypto ciphertext"),
        &authInfo,
        nullptr,
        0,
        reinterpret_cast<PUCHAR>(plaintext.data()),
        CheckedUlongSize(plaintext.size(), "UDP crypto plaintext"),
        &outputBytes,
        0);
    if (!BcryptOk(status)) {
        return std::nullopt;
    }
    if (outputBytes != plaintext.size()) {
        return std::nullopt;
    }

    return plaintext;
}

} // namespace screenshare
