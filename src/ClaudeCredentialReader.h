#pragma once

#include <optional>
#include <string>
#include <vector>

struct ClaudeToken {
    std::string accessToken;
    double expiresAt = 0;  // Unix seconds
    std::string clientId;
    bool isV2 = false;
};

// Read the best available Claude Desktop token (read-only, no refresh/write)
std::optional<ClaudeToken> GetDesktopToken();

// Read Claude CLI credentials and refresh if needed
std::optional<std::string> GetCliAccessToken();

// Read raw CLI credentials JSON
std::optional<std::string> ReadCliCredentials();

// Refresh CLI token, returns new access token or nullopt
std::optional<std::string> RefreshCliToken(const std::string& refreshToken);

// Persist refreshed CLI credentials
void PersistCliCredentials(const std::string& accessToken, const std::string& refreshToken, double expiresAt);
