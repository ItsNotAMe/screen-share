#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace screenshare {

constexpr size_t UdpCryptoKeyBytes = 32;
constexpr size_t UdpCryptoNonceBytes = 12;
constexpr size_t UdpCryptoTagBytes = 16;

struct UdpCryptoKey {
    std::array<std::byte, UdpCryptoKeyBytes> bytes{};

    [[nodiscard]] bool valid() const noexcept;
};

UdpCryptoKey DeriveUdpCryptoKey(std::string_view accessCode);
uint64_t UdpAccessCodeFingerprint(std::string_view accessCode);
std::string GenerateUdpAccessCode();
uint32_t GenerateUdpCryptoNoncePrefix();
void WriteUdpCryptoNonce(
    std::span<std::byte, UdpCryptoNonceBytes> nonce,
    uint32_t prefix,
    uint64_t mediaId,
    uint16_t fragmentIndex);

class UdpAesGcm {
public:
    explicit UdpAesGcm(const UdpCryptoKey& key);
    ~UdpAesGcm();

    UdpAesGcm(const UdpAesGcm&) = delete;
    UdpAesGcm& operator=(const UdpAesGcm&) = delete;

    [[nodiscard]] std::vector<std::byte> Encrypt(
        std::span<const std::byte> nonce,
        std::span<const std::byte> authenticatedData,
        std::span<const std::byte> plaintext,
        std::span<std::byte, UdpCryptoTagBytes> tag) const;

    [[nodiscard]] std::optional<std::vector<std::byte>> Decrypt(
        std::span<const std::byte> nonce,
        std::span<const std::byte> authenticatedData,
        std::span<const std::byte> ciphertext,
        std::span<const std::byte, UdpCryptoTagBytes> tag) const;

private:
    void* algorithm_ = nullptr;
    void* key_ = nullptr;
    std::vector<std::byte> keyObject_;
};

} // namespace screenshare
