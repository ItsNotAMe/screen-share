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
constexpr size_t UdpCryptoSessionSaltBytes = 16;

struct UdpCryptoKey {
    std::array<std::byte, UdpCryptoKeyBytes> bytes{};

    [[nodiscard]] bool valid() const noexcept;
};

// A per-session random salt. The endpoint that encrypts generates one at Open
// and puts it in every packet header it sends; the decrypting endpoint reads it
// from the header and derives the matching per-session key. This makes the AES
// key unique per session (and per endpoint), so the AES-GCM nonce only has to
// be unique within one session -- which a strictly increasing counter
// guarantees -- instead of relying on the old 32-bit random nonce prefix over a
// globally deterministic key.
using UdpCryptoSessionSalt = std::array<std::byte, UdpCryptoSessionSaltBytes>;

// The four sub-streams multiplexed over a session. Each occupies a disjoint
// nonce space (via the leading nonce byte), so their independent counters can
// never collide on a nonce under the shared per-session key.
enum class UdpCryptoRole : uint8_t {
    Video = 0,
    Audio = 1,
    Feedback = 2,
    Control = 3,
};

// PBKDF2 of the access code -> the long-term master secret shared by both ends.
UdpCryptoKey DeriveUdpCryptoKey(std::string_view accessCode);
// HKDF-SHA256(master, per-session salt) -> the AES key for one session. Unique
// per session (and per encrypting endpoint) because the salt is random per
// Open, which is what lets a plain counter nonce stay unique.
UdpCryptoKey DeriveUdpSessionKey(
    const UdpCryptoKey& master,
    const UdpCryptoSessionSalt& salt);
uint64_t UdpAccessCodeFingerprint(std::string_view accessCode);
std::string GenerateUdpAccessCode();
UdpCryptoSessionSalt GenerateUdpCryptoSessionSalt();
// A full 96-bit random nonce for one-shot AEAD uses (e.g. encrypting a NAT
// invite blob), where there is no session/counter to derive a nonce from.
std::array<std::byte, UdpCryptoNonceBytes> GenerateUdpCryptoNonce();
// Nonce = 8-bit role || 24 zero bits || 48-bit media/sequence id || 16-bit
// fragment index. Unique within a session key: the role byte separates the
// sub-streams, the id increases monotonically, and the fragment index is
// bounded well under 2^16.
void WriteUdpCryptoNonce(
    std::span<std::byte, UdpCryptoNonceBytes> nonce,
    UdpCryptoRole role,
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
