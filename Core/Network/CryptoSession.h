#pragma once

#include <windows.h>
#include <bcrypt.h>
#include <atomic>
#include <cstdint>
#include <mutex>
#include <vector>

namespace gupt {
namespace core {
namespace network {

// Ephemeral ECDH P-256 + AES-256-GCM authenticated encryption session using Windows CNG.
// Provides Forward Secrecy per session and tamper-evident authenticated encryption for
// P2P UDP and LAN TCP streams with zero external dependencies.
class CryptoSession {
public:
    CryptoSession() = default;
    ~CryptoSession();

    CryptoSession(const CryptoSession&) = delete;
    CryptoSession& operator=(const CryptoSession&) = delete;

    // Generates a fresh ephemeral ECDH P-256 keypair and random nonce prefix.
    bool InitializeLocalKeyPair();

    // Exports our local public key in ANSI X9.63 uncompressed format (0x04 || X || Y, 65 bytes).
    std::vector<uint8_t> GetLocalPublicKey() const;

    // Imports peer's 65-byte ANSI X9.63 P-256 public key and derives the shared AES-256-GCM session key.
    bool DeriveSharedSecret(const uint8_t* peerPublicKey, size_t peerPublicKeyLen);

    bool IsEstablished() const { return m_Established.load(); }

    // Encrypts plaintext into [12-byte nonce][16-byte GCM tag][ciphertext].
    bool Encrypt(const uint8_t* plaintext, size_t len, std::vector<uint8_t>& outPacket);

    // Verifies GCM authentication tag and decrypts [12-byte nonce][16-byte GCM tag][ciphertext].
    bool Decrypt(const uint8_t* packet, size_t len, std::vector<uint8_t>& outPlaintext);

    // Resets established symmetric key state while preserving or regenerating local keys.
    void Reset();

    // Fills buffer with CSPRNG bytes via Windows CNG BCRYPT_USE_SYSTEM_PREFERRED_RNG.
    static bool GenerateRandomBytes(uint8_t* outBuf, size_t len);

private:
    mutable std::mutex m_Mutex;
    std::atomic<bool> m_Established{false};

    BCRYPT_ALG_HANDLE m_EcdhAlg = NULL;
    BCRYPT_KEY_HANDLE m_LocalEcdhKey = NULL;
    BCRYPT_ALG_HANDLE m_AesAlg = NULL;
    BCRYPT_KEY_HANDLE m_AesKey = NULL;

    std::vector<uint8_t> m_LocalX963PublicKey;
    std::vector<uint8_t> m_KeyObject;
    uint32_t m_NoncePrefix = 0;
    uint64_t m_EncryptCounter = 0;

    void CleanupSymmetricLocked();
    void CleanupEcdhLocked();
};

} // namespace network
} // namespace core
} // namespace gupt
