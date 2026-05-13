// Manual smoke test for vms::utils::isInternalUrl. Compile + run from inside
// build/Release/ where ws2_32 is already linked into the runtime.
//
//   cl /std:c++17 /I cpp-backend/include test_url_validator.cpp \
//      cpp-backend/src/utils/url_validator.cpp ws2_32.lib

#include "utils/url_validator.h"
#include <cassert>
#include <cstdio>
#include <string>

static void check(const char* label, const std::string& url, bool want_internal) {
    auto v = vms::utils::isInternalUrl(url);
    const char* mark = (v.internal == want_internal) ? "PASS" : "FAIL";
    std::printf("[%s] %-30s url=%-45s host=%-25s ip=%-20s reason=%s\n",
                mark, label, url.c_str(), v.host.c_str(),
                v.resolved_ip.c_str(), v.reason.c_str());
}

int main() {
    // Internal — should refuse
    check("loopback v4",     "http://127.0.0.1/",        true);
    check("loopback alias",  "http://localhost/",        true);
    check("RFC1918 192",     "http://192.168.1.5/",      true);
    check("RFC1918 172.17",  "http://172.17.0.1/",       true);  // pre-fix MISSED
    check("RFC1918 172.31",  "http://172.31.255.1/",     true);  // pre-fix MISSED
    check("RFC1918 10",      "http://10.0.0.1/",         true);
    check("AWS metadata",    "http://169.254.169.254/",  true);
    check("link-local v4",   "http://169.254.1.1/",      true);  // pre-fix MISSED
    check("0.0.0.0",         "http://0.0.0.0/",          true);  // pre-fix MISSED
    check("CGNAT",           "http://100.64.0.1/",       true);  // pre-fix MISSED
    check("IPv6 loopback",   "http://[::1]/",            true);  // pre-fix MISSED
    check("IPv6 ULA",        "http://[fc00::1]/",        true);  // pre-fix MISSED
    check("IPv6 link-local", "http://[fe80::1]/",        true);  // pre-fix MISSED
    check("mDNS .local",     "http://nas.local/",        true);  // pre-fix MISSED
    check("malformed",       "http://",                  true);

    // Public — should allow (assuming network resolves them)
    check("public dns",      "https://example.com/",     false);
    check("public ip",       "https://1.1.1.1/",         false);

    // buildResolveEntry — TOCTOU pin format. Tests run offline (no DNS).
    // We build a synthetic UrlValidation rather than relying on isInternalUrl
    // for the pin shape so the test is hermetic.
    std::puts("\n--- buildResolveEntry shape tests ---");
    {
        vms::utils::UrlValidation v;
        v.internal = false;
        v.host = "example.com";
        v.resolved_ip = "93.184.216.34";
        v.port = 443;
        v.resolved_ip_is_v6 = false;
        std::string e = vms::utils::buildResolveEntry(v);
        std::printf("[%s] v4 pin                        entry=%s\n",
                    e == "example.com:443:93.184.216.34" ? "PASS" : "FAIL", e.c_str());
    }
    {
        vms::utils::UrlValidation v;
        v.internal = false;
        v.host = "ipv6.example.com";
        v.resolved_ip = "2606:2800:220:1:248:1893:25c8:1946";
        v.port = 80;
        v.resolved_ip_is_v6 = true;
        std::string e = vms::utils::buildResolveEntry(v);
        std::printf("[%s] v6 pin                        entry=%s\n",
                    e == "ipv6.example.com:80:2606:2800:220:1:248:1893:25c8:1946" ? "PASS" : "FAIL",
                    e.c_str());
    }
    {
        vms::utils::UrlValidation v;
        v.internal = true;  // would-be-rejected, MUST return "" so a buggy caller can't pin a private IP
        v.host = "foo.com";
        v.resolved_ip = "10.0.0.5";
        v.port = 80;
        std::string e = vms::utils::buildResolveEntry(v);
        std::printf("[%s] internal -> empty entry       entry='%s'\n",
                    e.empty() ? "PASS" : "FAIL", e.c_str());
    }
    {
        vms::utils::UrlValidation v;
        v.internal = false;
        v.host = "foo.com";
        v.resolved_ip = "";  // resolver failed but somehow caller skipped the internal check
        v.port = 80;
        std::string e = vms::utils::buildResolveEntry(v);
        std::printf("[%s] missing ip -> empty entry     entry='%s'\n",
                    e.empty() ? "PASS" : "FAIL", e.c_str());
    }
    {
        vms::utils::UrlValidation v;
        v.internal = false;
        v.host = "foo.com";
        v.resolved_ip = "1.2.3.4";
        v.port = 0;  // unknown scheme, no explicit port — can't pin
        std::string e = vms::utils::buildResolveEntry(v);
        std::printf("[%s] no port -> empty entry        entry='%s'\n",
                    e.empty() ? "PASS" : "FAIL", e.c_str());
    }
    return 0;
}
