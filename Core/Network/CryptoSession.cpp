#include "CryptoSession.h"
#include <algorithm>
#include <cstring>

#ifndef NT_SUCCESS
#define NT_SUCCESS(Status) (((NTSTATUS)(Status)) >= 0)
#endif

namespace gupt {
namespace core {
namespace network {

namespace {

constexpr size_t kP256CoordSize = 32;
constexpr size_t kP256X963Size = 1 + (2 * kP256CoordSize); // 0x04 + X(32) + Y(32)
constexpr size_t kAes256KeySize = 32;
constexpr size_t kGcmNonceSize = 12;
constexpr size_t kGcmTagSize = 16;

struct ScopedBCryptKey {
    BCRYPT_KEY_HANDLE handle = NULL;
    ~ScopedBCryptKey() {
        if (handle) BCryptDestroyKey(handle);
    }
};

struct ScopedBCryptSecret {
    BCRYPT_SECRET_HANDLE handle = NULL;
    ~ScopedBCryptSecret() {
        if (handle) BCryptDestroySecret(handle);
    }
};

struct ScopedBCryptAlg {
    BCRYPT_ALG_HANDLE handle = NULL;
    ~ScopedBCryptAlg() {
        if (handle) BCryptCloseAlgorithmProvider(handle, 0);
    }
};

// Converts CNG BCRYPT_ECCPUBLIC_BLOB (8-byte header + X + Y) into 65-byte ANSI X9.63 point.
bool EccBlobToX963(const std::vector<uint8_t>& blob, std::vector<uint8_t>& outX963) {
    if (blob.size() < sizeof(BCRYPT_ECCKEY_BLOB) + (2 * kP256CoordSize)) return false;
    const auto* hdr = reinterpret_cast<const BCRYPT_ECCKEY_BLOB*>(blob.data());
    if (hdr->dwMagic != BCRYPT_ECDH_PUBLIC_P256_MAGIC || hdr->cbKey != kP256CoordSize) return false;

    outX963.resize(kP256X963Size);
    outX963[0] = 0x04;
    std::memcpy(outX963.data() + 1, blob.data() + sizeof(BCRYPT_ECCKEY_BLOB), 2 * kP256CoordSize);
    return true;
}

// Converts 65-byte ANSI X9.63 uncompressed point into CNG BCRYPT_ECCPUBLIC_BLOB.
bool X963ToEccBlob(const uint8_t* x963, size_t len, std::vector<uint8_t>& outBlob) {
    if (!x963 || len != kP256X963Size || x963[0] != 0x04) return false;
    outBlob.resize(sizeof(BCRYPT_ECCKEY_BLOB) + (2 * kP256CoordSize));
    auto* hdr = reinterpret_cast<BCRYPT_ECCKEY_BLOB*>(outBlob.data());
    hdr->dwMagic = BCRYPT_ECDH_PUBLIC_P256_MAGIC;
    hdr->cbKey = static_cast<ULONG>(kP256CoordSize);
    std::memcpy(outBlob.data() + sizeof(BCRYPT_ECCKEY_BLOB), x963 + 1, 2 * kP256CoordSize);
    return true;
}

} // namespace

CryptoSession::~CryptoSession() {
    std::lock_guard<std::mutex> lk(m_Mutex);
    CleanupSymmetricLocked();
    CleanupEcdhLocked();
}

void CryptoSession::CleanupSymmetricLocked() {
    m_Established = false;
    if (m_AesKey) {
        BCryptDestroyKey(m_AesKey);
        m_AesKey = NULL;
    }
    if (m_AesAlg) {
        BCryptCloseAlgorithmProvider(m_AesAlg, 0);
        m_AesAlg = NULL;
    }
    if (!m_KeyObject.empty()) {
        SecureZeroMemory(m_KeyObject.data(), m_KeyObject.size());
        m_KeyObject.clear();
    }
    m_EncryptCounter = 0;
}

void CryptoSession::CleanupEcdhLocked() {
    if (m_LocalEcdhKey) {
        BCryptDestroyKey(m_LocalEcdhKey);
        m_LocalEcdhKey = NULL;
    }
    if (m_EcdhAlg) {
        BCryptCloseAlgorithmProvider(m_EcdhAlg, 0);
        m_EcdhAlg = NULL;
    }
    m_LocalX963PublicKey.clear();
}

bool CryptoSession::GenerateRandomBytes(uint8_t* outBuf, size_t len) {
    if (!outBuf || len == 0) return false;
    return NT_SUCCESS(BCryptGenRandom(NULL, outBuf, static_cast<ULONG>(len), BCRYPT_USE_SYSTEM_PREFERRED_RNG));
}

bool CryptoSession::InitializeLocalKeyPair() {
    std::lock_guard<std::mutex> lk(m_Mutex);
    CleanupSymmetricLocked();
    CleanupEcdhLocked();

    NTSTATUS st = BCryptOpenAlgorithmProvider(&m_EcdhAlg, BCRYPT_ECDH_P256_ALGORITHM, NULL, 0);
    if (!NT_SUCCESS(st)) {
        CleanupEcdhLocked();
        return false;
    }

    st = BCryptGenerateKeyPair(m_EcdhAlg, &m_LocalEcdhKey, 256, 0);
    if (!NT_SUCCESS(st)) {
        CleanupEcdhLocked();
        return false;
    }

    st = BCryptFinalizeKeyPair(m_LocalEcdhKey, 0);
    if (!NT_SUCCESS(st)) {
        CleanupEcdhLocked();
        return false;
    }

    ULONG pubSize = 0;
    st = BCryptExportKey(m_LocalEcdhKey, NULL, BCRYPT_ECCPUBLIC_BLOB, NULL, 0, &pubSize, 0);
    if (!NT_SUCCESS(st) || pubSize == 0) {
        CleanupEcdhLocked();
        return false;
    }

    std::vector<uint8_t> eccBlob(pubSize);
    st = BCryptExportKey(m_LocalEcdhKey, NULL, BCRYPT_ECCPUBLIC_BLOB, eccBlob.data(), pubSize, &pubSize, 0);
    if (!NT_SUCCESS(st) || !EccBlobToX963(eccBlob, m_LocalX963PublicKey)) {
        CleanupEcdhLocked();
        return false;
    }

    GenerateRandomBytes(reinterpret_cast<uint8_t*>(&m_NoncePrefix), sizeof(m_NoncePrefix));
    return true;
}

std::vector<uint8_t> CryptoSession::GetLocalPublicKey() const {
    std::lock_guard<std::mutex> lk(m_Mutex);
    return m_LocalX963PublicKey;
}

bool CryptoSession::DeriveSharedSecret(const uint8_t* peerPublicKey, size_t peerPublicKeyLen) {
    std::lock_guard<std::mutex> lk(m_Mutex);
    if (!m_EcdhAlg || !m_LocalEcdhKey) return false;

    std::vector<uint8_t> peerBlob;
    if (!X963ToEccBlob(peerPublicKey, peerPublicKeyLen, peerBlob)) {
        return false;
    }

    ScopedBCryptKey peerKey;
    NTSTATUS st = BCryptImportKeyPair(
        m_EcdhAlg, NULL, BCRYPT_ECCPUBLIC_BLOB, &peerKey.handle,
        peerBlob.data(), static_cast<ULONG>(peerBlob.size()), 0);
    if (!NT_SUCCESS(st)) return false;

    ScopedBCryptSecret secret;
    st = BCryptSecretAgreement(m_LocalEcdhKey, peerKey.handle, &secret.handle, 0);
    if (!NT_SUCCESS(st)) return false;

    // Derive deterministic 256-bit key material via SHA-256 KDF
    BCryptBuffer kdfParams[1]{};
    kdfParams[0].BufferType = KDF_HASH_ALGORITHM;
    kdfParams[0].cbBuffer = static_cast<ULONG>((wcslen(BCRYPT_SHA256_ALGORITHM) + 1) * sizeof(wchar_t));
    kdfParams[0].pvBuffer = const_cast<wchar_t*>(BCRYPT_SHA256_ALGORITHM);

    BCryptBufferDesc kdfDesc{};
    kdfDesc.ulVersion = BCRYPTBUFFER_VERSION;
    kdfDesc.cBuffers = 1;
    kdfDesc.pBuffers = kdfParams;

    std::vector<uint8_t> derivedKey(kAes256KeySize);
    ULONG derivedLen = 0;
    st = BCryptDeriveKey(
        secret.handle, BCRYPT_KDF_HASH, &kdfDesc,
        derivedKey.data(), static_cast<ULONG>(derivedKey.size()), &derivedLen, 0);
    if (!NT_SUCCESS(st) || derivedLen < kAes256KeySize) {
        SecureZeroMemory(derivedKey.data(), derivedKey.size());
        return false;
    }

    CleanupSymmetricLocked();

    st = BCryptOpenAlgorithmProvider(&m_AesAlg, BCRYPT_AES_ALGORITHM, NULL, 0);
    if (!NT_SUCCESS(st)) {
        SecureZeroMemory(derivedKey.data(), derivedKey.size());
        CleanupSymmetricLocked();
        return false;
    }

    st = BCryptSetProperty(
        m_AesAlg, BCRYPT_CHAINING_MODE,
        reinterpret_cast<PUCHAR>(const_cast<wchar_t*>(BCRYPT_CHAIN_MODE_GCM)),
        static_cast<ULONG>((wcslen(BCRYPT_CHAIN_MODE_GCM) + 1) * sizeof(wchar_t)), 0);
    if (!NT_SUCCESS(st)) {
        SecureZeroMemory(derivedKey.data(), derivedKey.size());
        CleanupSymmetricLocked();
        return false;
    }

    DWORD keyObjSize = 0;
    DWORD cbResult = 0;
    st = BCryptGetProperty(
        m_AesAlg, BCRYPT_OBJECT_LENGTH,
        reinterpret_cast<PUCHAR>(&keyObjSize), sizeof(DWORD), &cbResult, 0);
    if (!NT_SUCCESS(st) || keyObjSize == 0) {
        SecureZeroMemory(derivedKey.data(), derivedKey.size());
        CleanupSymmetricLocked();
        return false;
    }

    m_KeyObject.resize(keyObjSize);
    st = BCryptGenerateSymmetricKey(
        m_AesAlg, &m_AesKey, m_KeyObject.data(), keyObjSize,
        derivedKey.data(), kAes256KeySize, 0);

    SecureZeroMemory(derivedKey.data(), derivedKey.size());
    if (!NT_SUCCESS(st)) {
        CleanupSymmetricLocked();
        return false;
    }

    m_EncryptCounter = 1;
    m_Established = true;
    return true;
}

bool CryptoSession::Encrypt(const uint8_t* plaintext, size_t len, std::vector<uint8_t>& outPacket) {
    if (!plaintext || len == 0) return false;
    std::lock_guard<std::mutex> lk(m_Mutex);
    if (!m_Established.load() || !m_AesKey) return false;

    uint8_t nonce[kGcmNonceSize]{};
    uint64_t ctr = m_EncryptCounter++;
    std::memcpy(nonce, &m_NoncePrefix, sizeof(m_NoncePrefix));
    std::memcpy(nonce + sizeof(m_NoncePrefix), &ctr, sizeof(ctr));

    outPacket.resize(kGcmNonceSize + kGcmTagSize + len);
    std::memcpy(outPacket.data(), nonce, kGcmNonceSize);

    BCRYPT_AUTHENTICATED_CIPHER_MODE_INFO authInfo;
    BCRYPT_INIT_AUTH_MODE_INFO(authInfo);
    authInfo.pbNonce = nonce;
    authInfo.cbNonce = static_cast<ULONG>(kGcmNonceSize);
    authInfo.pbTag = outPacket.data() + kGcmNonceSize;
    authInfo.cbTag = static_cast<ULONG>(kGcmTagSize);

    ULONG cipherLen = 0;
    NTSTATUS st = BCryptEncrypt(
        m_AesKey,
        const_cast<PUCHAR>(plaintext), static_cast<ULONG>(len),
        &authInfo,
        NULL, 0,
        outPacket.data() + kGcmNonceSize + kGcmTagSize, static_cast<ULONG>(len),
        &cipherLen, 0);

    if (!NT_SUCCESS(st)) {
        outPacket.clear();
        return false;
    }

    outPacket.resize(kGcmNonceSize + kGcmTagSize + cipherLen);
    return true;
}

bool CryptoSession::Decrypt(const uint8_t* packet, size_t len, std::vector<uint8_t>& outPlaintext) {
    if (!packet || len <= (kGcmNonceSize + kGcmTagSize)) return false;
    std::lock_guard<std::mutex> lk(m_Mutex);
    if (!m_Established.load() || !m_AesKey) return false;

    const uint8_t* nonce = packet;
    const uint8_t* tag = packet + kGcmNonceSize;
    const uint8_t* cipher = packet + kGcmNonceSize + kGcmTagSize;
    size_t cipherLen = len - kGcmNonceSize - kGcmTagSize;

    outPlaintext.resize(cipherLen);

    BCRYPT_AUTHENTICATED_CIPHER_MODE_INFO authInfo;
    BCRYPT_INIT_AUTH_MODE_INFO(authInfo);
    authInfo.pbNonce = const_cast<PUCHAR>(nonce);
    authInfo.cbNonce = static_cast<ULONG>(kGcmNonceSize);
    authInfo.pbTag = const_cast<PUCHAR>(tag);
    authInfo.cbTag = static_cast<ULONG>(kGcmTagSize);

    ULONG plainLen = 0;
    NTSTATUS st = BCryptDecrypt(
        m_AesKey,
        const_cast<PUCHAR>(cipher), static_cast<ULONG>(cipherLen),
        &authInfo,
        NULL, 0,
        outPlaintext.data(), static_cast<ULONG>(cipherLen),
        &plainLen, 0);

    if (!NT_SUCCESS(st)) {
        outPlaintext.clear();
        return false;
    }

    outPlaintext.resize(plainLen);
    return true;
}

void CryptoSession::Reset() {
    InitializeLocalKeyPair();
}

} // namespace network
} // namespace core
} // namespace gupt
