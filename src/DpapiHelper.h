#pragma once

#include <optional>
#include <string>
#include <vector>

// Windows DPAPI decrypt (CryptUnprotectData)
std::optional<std::vector<unsigned char>> DpapiUnprotect(const unsigned char* data, size_t size);

// AES-256-GCM decrypt via Windows BCrypt
std::optional<std::vector<unsigned char>> AesGcmDecrypt(
    const unsigned char* key, size_t keySize,
    const unsigned char* nonce, size_t nonceSize,
    const unsigned char* ciphertext, size_t ctSize,
    const unsigned char* tag, size_t tagSize);

// Get Chromium safe storage key from Claude Desktop's Local State
// Returns (aes_key, true) or nullopt
std::optional<std::pair<std::vector<unsigned char>, bool>> GetSafeStorageKey();

// Decrypt a Chromium-encrypted value (base64 encoded, v10/v11 prefix)
std::optional<std::vector<unsigned char>> DecryptChromiumValue(
    const std::string& base64Value,
    const std::vector<unsigned char>& aesKey,
    bool useGcm);

// Base64 decode
std::optional<std::vector<unsigned char>> Base64Decode(const std::string& input);
