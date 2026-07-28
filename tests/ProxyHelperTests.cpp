#include "ProxyHelper.h"

#include <iostream>
#include <iterator>

namespace {

int failures = 0;

void Expect(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        ++failures;
    }
}

} // namespace

int main() {
    {
        ProxyConfig config;
        config.requireProxy = true;

        std::wstring error;
        HINTERNET session = OpenHttpSession(
            config, L"ProxyHelperTests", nullptr, &error);
        Expect(session == nullptr, "RequireProxy must fail closed without a proxy");
        Expect(error.find(L"BLOCKED") != std::wstring::npos,
            "RequireProxy failure must report a blocked policy");
        if (session) WinHttpCloseHandle(session);
    }

    {
        ProxyConfig config;
        config.allowedExitIps = {L"203.0.113.10"};
        config.exitIpCheckUrl = L"http://example.com/";

        std::wstring observed;
        std::wstring error;
        Expect(!VerifyExitIp(config, nullptr, &observed, &error),
            "Exit IP checks must reject non-HTTPS URLs");
        Expect(observed.empty(), "Rejected checks must not report an observed IP");
    }

    {
        const ProxyConfig config = DetectProxy(
            L"", true, L"203.0.113.10; 2001:db8::10,198.51.100.2",
            L"https://api.ipify.org/");
        Expect(config.requireProxy, "RequireProxy must be retained");
        Expect(config.allowedExitIps.size() == 3,
            "AllowedExitIPs must parse comma, semicolon, and whitespace separators");
        Expect(config.statusMessage.find(L"exit IP lock") != std::wstring::npos,
            "TUN/VPN lock status must be visible");
    }

    {
        const std::wstring trace =
            L"fl=abc\r\nh=chatgpt.com\r\n"
            L"ip=2001:db8::443\r\nloc=ZZ\r\n";
        Expect(ExtractExitIp(trace) == L"2001:db8::443",
            "Cloudflare trace parser must extract the ip value");
        Expect(ExtractExitIp(L"203.0.113.10\n") == L"203.0.113.10",
            "Plain IP responses must remain supported");
        Expect(ExtractExitIp(L"ip=not-an-ip").empty(),
            "Invalid trace IP values must fail closed");
    }

    if (GetEnvironmentVariableW(
            L"AIUSAGE_RUN_LIVE_PROXY_TEST", nullptr, 0) > 0) {
        wchar_t proxyBuffer[1024]{};
        wchar_t exitIpBuffer[128]{};
        GetEnvironmentVariableW(
            L"AIUSAGE_TEST_PROXY", proxyBuffer,
            static_cast<DWORD>(std::size(proxyBuffer)));
        GetEnvironmentVariableW(
            L"AIUSAGE_EXPECTED_EXIT_IP", exitIpBuffer,
            static_cast<DWORD>(std::size(exitIpBuffer)));
        Expect(proxyBuffer[0] != L'\0',
            "AIUSAGE_TEST_PROXY is required for the live test");
        Expect(exitIpBuffer[0] != L'\0',
            "AIUSAGE_EXPECTED_EXIT_IP is required for the live test");
        if (proxyBuffer[0] == L'\0' || exitIpBuffer[0] == L'\0') {
            return 1;
        }

        ProxyConfig config;
        config.explicitProxy = proxyBuffer;
        config.proxyActive = true;
        config.requireProxy = true;
        config.allowedExitIps = {exitIpBuffer};
        config.exitIpCheckUrl = L"https://api.ipify.org/";
        config.verifyTargetHost = true;

        for (const wchar_t* host : {
                L"chatgpt.com",
                L"api.anthropic.com",
                L"platform.claude.com",
                L"console.anthropic.com"}) {
            std::wstring observed;
            std::wstring error;
            const bool allowed = VerifyExitIp(
                config, host, &observed, &error);
            Expect(allowed, "Live target-host exit IP check must pass");
            Expect(observed == exitIpBuffer,
                "Live target-host check must observe the configured exit IP");
        }
    }

    if (failures == 0) {
        std::cout << "All ProxyHelper tests passed\n";
    }
    return failures == 0 ? 0 : 1;
}
