#include "ClaudeCredentialReader.h"
#include "DpapiHelper.h"
#include "JsonLite.h"

#include <Windows.h>

#include <chrono>
#include <fstream>
#include <mutex>

namespace {

constexpr const char* CLAUDE_CLIENT_ID = "9d1c250a-e61b-44d9-88ed-5944d1962f5e";

std::optional<std::wstring> GetDesktopDir() {
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
    wchar_t appData[MAX_PATH]{};
    if (GetEnvironmentVariableW(L"APPDATA", appData, MAX_PATH) > 0) {
        std::wstring path = std::wstring(appData) + L"\\Claude\\config.json";
        DWORD attrs = GetFileAttributesW(path.c_str());
        if (attrs != INVALID_FILE_ATTRIBUTES) {
            return std::wstring(appData) + L"\\Claude";
        }
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

bool WriteFileUtf8(const std::wstring& path, const std::string& content) {
    // Ensure parent directory exists
    size_t sep = path.find_last_of(L"\\/");
    if (sep != std::wstring::npos) {
        CreateDirectoryW(path.substr(0, sep).c_str(), nullptr);
    }
    HANDLE file = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr,
        CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return false;
    DWORD written = 0;
    BOOL ok = WriteFile(file, content.data(), static_cast<DWORD>(content.size()), &written, nullptr);
    CloseHandle(file);
    return ok && written == content.size();
}

} // namespace

std::optional<std::string> ReadCliCredentials() {
    wchar_t configDir[MAX_PATH]{};
    if (GetEnvironmentVariableW(L"CLAUDE_CONFIG_DIR", configDir, MAX_PATH) == 0) {
        wchar_t home[MAX_PATH]{};
        if (GetEnvironmentVariableW(L"USERPROFILE", home, MAX_PATH) == 0) return std::nullopt;
        std::wstring path = std::wstring(home) + L"\\.claude\\.credentials.json";
        return ReadFileUtf8(path);
    }
    std::wstring path = std::wstring(configDir) + L"\\.credentials.json";
    return ReadFileUtf8(path);
}

std::wstring GetCliCredentialsPath() {
    wchar_t configDir[MAX_PATH]{};
    if (GetEnvironmentVariableW(L"CLAUDE_CONFIG_DIR", configDir, MAX_PATH) == 0) {
        wchar_t home[MAX_PATH]{};
        if (GetEnvironmentVariableW(L"USERPROFILE", home, MAX_PATH) == 0) return L"";
        return std::wstring(home) + L"\\.claude\\.credentials.json";
    }
    return std::wstring(configDir) + L"\\.credentials.json";
}

// Parse JWT expiry (just the exp claim, no signature verification)
double ParseJwtExp(const std::string& token) {
    size_t dot1 = token.find('.');
    if (dot1 == std::string::npos) return 0;
    size_t dot2 = token.find('.', dot1 + 1);
    if (dot2 == std::string::npos) return 0;
    std::string payload = token.substr(dot1 + 1, dot2 - dot1 - 1);
    // Pad for base64
    while (payload.size() % 4 != 0) payload.push_back('=');
    auto decoded = Base64Decode(payload);
    if (!decoded) return 0;
    std::string json(decoded->begin(), decoded->end());
    jsonlite::Parser parser(json);
    auto root = parser.Parse();
    if (!root) return 0;
    auto exp = root->Find("exp");
    if (!exp) return 0;
    auto val = exp->AsNumber();
    return val.value_or(0);
}

std::optional<ClaudeToken> GetDesktopToken() {
    static std::optional<std::pair<std::vector<unsigned char>, bool>> s_keyCache;
    static bool s_keyCached = false;
    static std::mutex s_keyMutex;

    auto dir = GetDesktopDir();
    if (!dir) return std::nullopt;

    auto jsonText = ReadFileUtf8(*dir + L"\\config.json");
    if (!jsonText) return std::nullopt;

    // Get safe storage key (cached)
    std::vector<unsigned char> aesKey;
    bool useGcm = false;
    {
        std::lock_guard<std::mutex> lock(s_keyMutex);
        if (!s_keyCached) {
            s_keyCached = true;
            s_keyCache = GetSafeStorageKey();
        }
        if (!s_keyCache) return std::nullopt;
        aesKey = s_keyCache->first;
        useGcm = s_keyCache->second;
    }

    // Parse config.json to find encrypted token caches
    jsonlite::Parser parser(*jsonText);
    auto root = parser.Parse();
    if (!root) return std::nullopt;

    std::vector<ClaudeToken> candidates;

    // Try both tokenCacheV2 and tokenCache
    for (auto cacheKey : {"oauth:tokenCacheV2", "oauth:tokenCache"}) {
        auto cacheVal = root->Find(cacheKey);
        if (!cacheVal || !cacheVal->IsString()) continue;
        auto cacheStr = cacheVal->AsString();
        if (!cacheStr) continue;

        auto decrypted = DecryptChromiumValue(std::string(*cacheStr), aesKey, useGcm);
        if (!decrypted || decrypted->empty()) continue;

        std::string decryptedStr(decrypted->begin(), decrypted->end());
        jsonlite::Parser cacheParser(decryptedStr);
        auto cacheRoot = cacheParser.Parse();
        if (!cacheRoot || !cacheRoot->IsObject()) continue;

        bool isV2 = (std::string(cacheKey).find("V2") != std::string::npos);
        auto obj = cacheRoot->AsObject();
        if (!obj) continue;

        for (const auto& [k, v] : *obj) {
            if (k.find("api.anthropic.com") == std::string::npos) continue;
            if (k.find("user:profile") == std::string::npos) continue;
            if (!v.IsObject()) continue;

            auto* tok = v.Find("token");
            if (!tok) tok = v.Find("accessToken");
            if (!tok || !tok->IsString()) continue;
            auto tokStr = tok->AsString();
            if (!tokStr || tokStr->empty()) continue;

            double expiresAt = 0;
            auto* exp = v.Find("expiresAt");
            if (exp) {
                auto expNum = exp->AsNumber();
                if (expNum) {
                    expiresAt = *expNum;
                    if (expiresAt > 1e12) expiresAt /= 1000.0; // ms to seconds
                }
            }

            ClaudeToken ct;
            ct.accessToken = std::string(*tokStr);
            ct.expiresAt = expiresAt;
            ct.isV2 = isV2;
            // Extract client_id from cache key (format: "client_id:org:aud:scopes")
            size_t colonPos = k.find(':');
            ct.clientId = (colonPos != std::string::npos) ? k.substr(0, colonPos) : k;
            candidates.push_back(std::move(ct));
        }
    }

    if (candidates.empty()) return std::nullopt;

    // Select best: prefer CLAUDE_CLIENT_ID, then V2, then latest expiry
    auto best = std::max_element(candidates.begin(), candidates.end(),
        [](const ClaudeToken& a, const ClaudeToken& b) {
            bool aMatch = (a.clientId == CLAUDE_CLIENT_ID);
            bool bMatch = (b.clientId == CLAUDE_CLIENT_ID);
            if (aMatch != bMatch) return !aMatch;
            if (a.isV2 != b.isV2) return !a.isV2;
            return a.expiresAt < b.expiresAt;
        });
    return *best;
}

std::optional<std::string> GetCliAccessToken() {
    auto jsonText = ReadCliCredentials();
    if (!jsonText) return std::nullopt;

    jsonlite::Parser parser(*jsonText);
    auto root = parser.Parse();
    if (!root) return std::nullopt;

    auto oauth = root->Find("claudeAiOauth");
    if (!oauth) return std::nullopt;

    auto* at = oauth->Find("accessToken");
    if (!at || !at->IsString()) return std::nullopt;
    auto str = at->AsString();
    if (!str || str->empty()) return std::nullopt;
    return std::string(*str);
}

std::optional<std::string> RefreshCliToken(const std::string& refreshToken) {
    // This is called from ClaudeUsageFetcher when token is expired
    // Returns new access token or nullopt
    return std::nullopt; // Implemented in ClaudeUsageFetcher
}

void PersistCliCredentials(const std::string& accessToken, const std::string& refreshToken, double expiresAt) {
    auto jsonText = ReadCliCredentials();
    if (!jsonText) return;

    // Simple approach: rewrite the JSON with updated tokens
    // Since we have JsonLite but no writer, we'll do string replacement
    std::string result = *jsonText;

    // Replace accessToken
    size_t pos = result.find("\"accessToken\"");
    if (pos != std::string::npos) {
        size_t colon = result.find(':', pos);
        if (colon != std::string::npos) {
            size_t start = result.find('"', colon + 1);
            if (start != std::string::npos) {
                size_t end = result.find('"', start + 1);
                if (end != std::string::npos) {
                    result.replace(start + 1, end - start - 1, accessToken);
                }
            }
        }
    }

    // Replace refreshToken if provided
    if (!refreshToken.empty()) {
        pos = result.find("\"refreshToken\"");
        if (pos != std::string::npos) {
            size_t colon = result.find(':', pos);
            if (colon != std::string::npos) {
                size_t start = result.find('"', colon + 1);
                if (start != std::string::npos) {
                    size_t end = result.find('"', start + 1);
                    if (end != std::string::npos) {
                        result.replace(start + 1, end - start - 1, refreshToken);
                    }
                }
            }
        }
    }

    // Replace expiresAt
    pos = result.find("\"expiresAt\"");
    if (pos != std::string::npos) {
        size_t colon = result.find(':', pos);
        if (colon != std::string::npos) {
            size_t start = colon + 1;
            while (start < result.size() && (result[start] == ' ' || result[start] == '\t')) start++;
            size_t end = start;
            while (end < result.size() && (result[end] >= '0' && result[end] <= '9' || result[end] == '.')) end++;
            result.replace(start, end - start, std::to_string(static_cast<long long>(expiresAt * 1000)));
        }
    }

    auto path = GetCliCredentialsPath();
    if (!path.empty()) {
        WriteFileUtf8(path, result);
    }
}
