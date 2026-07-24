#include "DpapiHelper.h"
#include "CodexUsageCore.h"

#include <Windows.h>
#include <wincrypt.h>
#include <bcrypt.h>

#pragma comment(lib, "crypt32.lib")
#pragma comment(lib, "bcrypt.lib")

#include <fstream>
#include <sstream>

namespace {

std::optional<std::wstring> GetDesktopDir() {
    // MSIX (Microsoft Store) version
    wchar_t localAppData[MAX_PATH]{};
    if (GetEnvironmentVariableW(L"LOCALAPPDATA", localAppData, MAX_PATH) > 0) {
        std::wstring packagesDir = std::wstring(localAppData) + L"\\Packages";
        WIN32_FIND_DATAW findData{};
        HANDLE hFind = FindFirstFileW((packagesDir + L"\\Claude_*").c_str(), &findData);
        if (hFind != INVALID_HANDLE_VALUE) {
            do {
                std::wstring candidate = packagesDir + L"\\" + findData.cFileName +
                    L"\\LocalCache\\Roaming\\Claude\\config.json";
                DWORD attrs = GetFileAttributesW(candidate.c_str());
                if (attrs != INVALID_FILE_ATTRIBUTES) {
                    FindClose(hFind);
                    return std::wstring(packagesDir + L"\\" + findData.cFileName +
                        L"\\LocalCache\\Roaming\\Claude");
                }
            } while (FindNextFileW(hFind, &findData));
            FindClose(hFind);
        }
    }
    // Classic install
    wchar_t appData[MAX_PATH]{};
    if (GetEnvironmentVariableW(L"APPDATA", appData, MAX_PATH) > 0) {
        return std::wstring(appData) + L"\\Claude";
    }
    return std::nullopt;
}

std::optional<std::string> ReadFileUtf8(const std::wstring& path) {
    HANDLE file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ,
        nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return std::nullopt;

    LARGE_INTEGER size{};
    if (!GetFileSizeEx(file, &size) || size.QuadPart <= 0 || size.QuadPart > 16 * 1024 * 1024) {
        CloseHandle(file);
        return std::nullopt;
    }

    std::string contents(static_cast<size_t>(size.QuadPart), '\0');
    DWORD read = 0;
    BOOL ok = ReadFile(file, contents.data(), static_cast<DWORD>(contents.size()), &read, nullptr);
    CloseHandle(file);
    if (!ok || read != contents.size()) return std::nullopt;
    return contents;
}

// Extract JSON string value (simple parser for known keys)
std::optional<std::string> ExtractJsonString(const std::string& json, const std::string& key) {
    std::string search = "\"" + key + "\"";
    size_t pos = json.find(search);
    if (pos == std::string::npos) return std::nullopt;
    pos = json.find(':', pos + search.size());
    if (pos == std::string::npos) return std::nullopt;
    pos++;
    while (pos < json.size() && (json[pos] == ' ' || json[pos] == '\t' || json[pos] == '\n' || json[pos] == '\r')) pos++;
    if (pos >= json.size() || json[pos] != '"') return std::nullopt;
    pos++;
    size_t end = json.find('"', pos);
    if (end == std::string::npos) return std::nullopt;
    return json.substr(pos, end - pos);
}

// Extract JSON object (returns the raw substring including braces)
std::optional<std::string> ExtractJsonObject(const std::string& json, const std::string& key) {
    std::string search = "\"" + key + "\"";
    size_t pos = json.find(search);
    if (pos == std::string::npos) return std::nullopt;
    pos = json.find(':', pos + search.size());
    if (pos == std::string::npos) return std::nullopt;
    pos++;
    while (pos < json.size() && (json[pos] == ' ' || json[pos] == '\t' || json[pos] == '\n' || json[pos] == '\r')) pos++;
    if (pos >= json.size() || json[pos] != '{') return std::nullopt;
    int depth = 0;
    bool inString = false;
    bool escape = false;
    size_t start = pos;
    for (size_t i = pos; i < json.size(); i++) {
        char ch = json[i];
        if (inString) {
            if (escape) { escape = false; continue; }
            if (ch == '\\') { escape = true; continue; }
            if (ch == '"') inString = false;
            continue;
        }
        if (ch == '"') { inString = true; continue; }
        if (ch == '{') depth++;
        else if (ch == '}') { depth--; if (depth == 0) return json.substr(start, i - start + 1); }
    }
    return std::nullopt;
}

} // namespace

std::optional<std::vector<unsigned char>> DpapiUnprotect(const unsigned char* data, size_t size) {
    DATA_BLOB inBlob{}, outBlob{};
    inBlob.cbData = static_cast<DWORD>(size);
    inBlob.pbData = const_cast<BYTE*>(data);
    if (!CryptUnprotectData(&inBlob, nullptr, nullptr, nullptr, nullptr, 0, &outBlob)) {
        return std::nullopt;
    }
    std::vector<unsigned char> result(outBlob.pbData, outBlob.pbData + outBlob.cbData);
    LocalFree(outBlob.pbData);
    return result;
}

std::optional<std::vector<unsigned char>> AesGcmDecrypt(
    const unsigned char* key, size_t keySize,
    const unsigned char* nonce, size_t nonceSize,
    const unsigned char* ciphertext, size_t ctSize,
    const unsigned char* tag, size_t tagSize)
{
    BCRYPT_ALG_HANDLE hAlg = nullptr;
    BCRYPT_KEY_HANDLE hKey = nullptr;
    std::vector<unsigned char> result;

    do {
        if (BCryptOpenAlgorithmProvider(&hAlg, BCRYPT_AES_ALGORITHM, nullptr, 0) != 0) break;
        if (BCryptSetProperty(hAlg, BCRYPT_CHAINING_MODE,
            (PUCHAR)BCRYPT_CHAIN_MODE_GCM, sizeof(BCRYPT_CHAIN_MODE_GCM), 0) != 0) break;

        if (BCryptGenerateSymmetricKey(hAlg, &hKey, nullptr, 0,
            const_cast<PUCHAR>(key), static_cast<ULONG>(keySize), 0) != 0) break;

        BCRYPT_AUTHENTICATED_CIPHER_MODE_INFO authInfo{};
        BCRYPT_INIT_AUTH_MODE_INFO(authInfo);
        authInfo.pbNonce = const_cast<PUCHAR>(nonce);
        authInfo.cbNonce = static_cast<ULONG>(nonceSize);
        authInfo.pbTag = const_cast<PUCHAR>(tag);
        authInfo.cbTag = static_cast<ULONG>(tagSize);

        result.resize(ctSize);
        ULONG bytesWritten = 0;
        if (BCryptDecrypt(hKey, const_cast<PUCHAR>(ciphertext), static_cast<ULONG>(ctSize),
            &authInfo, nullptr, 0, result.data(), static_cast<ULONG>(result.size()),
            &bytesWritten, 0) != 0) {
            result.clear();
            break;
        }
        result.resize(bytesWritten);
    } while (false);

    if (hKey) BCryptDestroyKey(hKey);
    if (hAlg) BCryptCloseAlgorithmProvider(hAlg, 0);
    return result.empty() ? std::nullopt : std::make_optional(std::move(result));
}

std::optional<std::vector<unsigned char>> Base64Decode(const std::string& input) {
    DWORD outLen = 0;
    if (!CryptStringToBinaryA(input.c_str(), static_cast<DWORD>(input.size()),
        CRYPT_STRING_BASE64, nullptr, &outLen, nullptr, nullptr)) {
        return std::nullopt;
    }
    std::vector<unsigned char> output(outLen);
    if (!CryptStringToBinaryA(input.c_str(), static_cast<DWORD>(input.size()),
        CRYPT_STRING_BASE64, output.data(), &outLen, nullptr, nullptr)) {
        return std::nullopt;
    }
    output.resize(outLen);
    return output;
}

std::optional<std::pair<std::vector<unsigned char>, bool>> GetSafeStorageKey() {
    auto dir = GetDesktopDir();
    if (!dir) return std::nullopt;

    auto jsonText = ReadFileUtf8(*dir + L"\\Local State");
    if (!jsonText) return std::nullopt;

    auto encB64 = ExtractJsonString(*jsonText, "encrypted_key");
    if (!encB64) return std::nullopt;

    auto encBytes = Base64Decode(*encB64);
    if (!encBytes || encBytes->size() < 6) return std::nullopt;
    if ((*encBytes)[0] != 'D' || (*encBytes)[1] != 'P' || (*encBytes)[2] != 'A' ||
        (*encBytes)[3] != 'P' || (*encBytes)[4] != 'I') {
        return std::nullopt;
    }

    auto key = DpapiUnprotect(encBytes->data() + 5, encBytes->size() - 5);
    if (!key || key->size() != 32) return std::nullopt;

    return std::make_pair(std::move(*key), true);
}

std::optional<std::vector<unsigned char>> DecryptChromiumValue(
    const std::string& base64Value,
    const std::vector<unsigned char>& aesKey,
    bool useGcm)
{
    auto blob = Base64Decode(base64Value);
    if (!blob) return std::nullopt;

    const unsigned char* data = blob->data();
    size_t dataSize = blob->size();

    // v10 or v11 prefix
    if (dataSize > 3 && data[0] == 'v' && (data[1] == '1' || data[1] == '0') &&
        (data[2] == '0' || data[2] == '1')) {
        data += 3;
        dataSize -= 3;
        if (useGcm) {
            if (dataSize < 12 + 16) return std::nullopt; // nonce + tag minimum
            const unsigned char* nonce = data;
            const unsigned char* ciphertext = data + 12;
            size_t ctSize = dataSize - 12 - 16;
            const unsigned char* tag = data + 12 + ctSize;
            return AesGcmDecrypt(aesKey.data(), aesKey.size(), nonce, 12, ciphertext, ctSize, tag, 16);
        }
        return std::nullopt; // CBC not needed on Windows
    }

    // No version prefix: try DPAPI directly (old format)
    return DpapiUnprotect(data, dataSize);
}
