#include "utils/url_validator.h"

#ifdef _WIN32
  #ifndef WIN32_LEAN_AND_MEAN
  #define WIN32_LEAN_AND_MEAN
  #endif
  #include <winsock2.h>
  #include <ws2tcpip.h>
  #pragma comment(lib, "ws2_32.lib")
#else
  #include <sys/socket.h>
  #include <netdb.h>
  #include <arpa/inet.h>
  #include <netinet/in.h>
#endif

#include <cstdint>
#include <cstring>
#include <sstream>

namespace vms {
namespace utils {

namespace {

// Strip scheme + trailing path/query/fragment, return host segment (with
// optional :port). Brackets stripped from IPv6 literals.
std::string extractHost(const std::string& url, std::string* port_out) {
    auto scheme_end = url.find("://");
    std::string remainder = (scheme_end == std::string::npos)
                            ? url : url.substr(scheme_end + 3);
    // Drop userinfo if present (`user:pass@host`)
    auto at_pos = remainder.find('@');
    if (at_pos != std::string::npos) remainder = remainder.substr(at_pos + 1);

    // Stop at the first /, ?, or # — those mark path/query/fragment.
    size_t end = remainder.size();
    for (size_t i = 0; i < remainder.size(); ++i) {
        char c = remainder[i];
        if (c == '/' || c == '?' || c == '#') { end = i; break; }
    }
    std::string host_port = remainder.substr(0, end);

    // Bracketed IPv6 literal: `[::1]` or `[fe80::1]:8080`
    if (!host_port.empty() && host_port.front() == '[') {
        auto rb = host_port.find(']');
        if (rb == std::string::npos) return {};  // malformed
        std::string h = host_port.substr(1, rb - 1);
        if (port_out && rb + 1 < host_port.size() && host_port[rb + 1] == ':') {
            *port_out = host_port.substr(rb + 2);
        }
        return h;
    }
    // host[:port]
    auto colon = host_port.rfind(':');
    if (colon != std::string::npos) {
        if (port_out) *port_out = host_port.substr(colon + 1);
        return host_port.substr(0, colon);
    }
    return host_port;
}

bool isPrivateIPv4(uint32_t addr_host_order, std::string* reason) {
    // RFC1918 + RFC6598 (CGNAT) + loopback + link-local + 0.0.0.0/8 +
    // documentation + cloud metadata + multicast + broadcast.
    auto octet = [&](int i) -> uint8_t {
        return static_cast<uint8_t>((addr_host_order >> (8 * (3 - i))) & 0xFF);
    };
    uint8_t a = octet(0), b = octet(1);
    if (a == 0)   { *reason = "0.0.0.0/8 (this network)"; return true; }
    if (a == 10)  { *reason = "10.0.0.0/8 (RFC1918)";     return true; }
    if (a == 127) { *reason = "127.0.0.0/8 (loopback)";   return true; }
    if (a == 169 && b == 254) {
        // Cloud metadata endpoints all live in 169.254.169.254/32 + relatives;
        // catch the whole link-local block.
        *reason = "169.254.0.0/16 (link-local + cloud metadata)";
        return true;
    }
    if (a == 172 && b >= 16 && b <= 31) { *reason = "172.16.0.0/12 (RFC1918)"; return true; }
    if (a == 192 && b == 168)           { *reason = "192.168.0.0/16 (RFC1918)"; return true; }
    if (a == 192 && b == 0)             { *reason = "192.0.0.0/24 (IETF)"; return true; }
    if (a == 198 && (b == 18 || b == 19)) { *reason = "198.18.0.0/15 (benchmarking)"; return true; }
    if (a == 100 && b >= 64 && b <= 127){ *reason = "100.64.0.0/10 (RFC6598 CGNAT)"; return true; }
    if (a == 224)                       { *reason = "224.0.0.0/4 (multicast)"; return true; }
    if (a >= 240)                       { *reason = "240.0.0.0/4 (reserved/broadcast)"; return true; }
    return false;
}

bool isPrivateIPv6(const struct in6_addr& a, std::string* reason) {
    // ::1 — loopback
    static const uint8_t loopback[16] = {0,0,0,0, 0,0,0,0, 0,0,0,0, 0,0,0,1};
    if (std::memcmp(&a, loopback, 16) == 0) { *reason = "::1 (IPv6 loopback)"; return true; }
    // :: — unspecified
    static const uint8_t any[16] = {0};
    if (std::memcmp(&a, any, 16) == 0) { *reason = ":: (IPv6 unspecified)"; return true; }
    uint8_t b0 = a.s6_addr[0], b1 = a.s6_addr[1];
    // fc00::/7 — Unique Local Addresses (RFC4193)
    if ((b0 & 0xFE) == 0xFC) { *reason = "fc00::/7 (IPv6 ULA)"; return true; }
    // fe80::/10 — link-local
    if (b0 == 0xFE && (b1 & 0xC0) == 0x80) { *reason = "fe80::/10 (IPv6 link-local)"; return true; }
    // ff00::/8 — multicast
    if (b0 == 0xFF) { *reason = "ff00::/8 (IPv6 multicast)"; return true; }
    // ::ffff:0:0/96 — IPv4-mapped — fall through to v4 check via caller
    if (a.s6_addr[0]==0 && a.s6_addr[1]==0 && a.s6_addr[2]==0 && a.s6_addr[3]==0 &&
        a.s6_addr[4]==0 && a.s6_addr[5]==0 && a.s6_addr[6]==0 && a.s6_addr[7]==0 &&
        a.s6_addr[8]==0 && a.s6_addr[9]==0 && a.s6_addr[10]==0xFF && a.s6_addr[11]==0xFF) {
        uint32_t v4 = (uint32_t(a.s6_addr[12]) << 24) | (uint32_t(a.s6_addr[13]) << 16) |
                      (uint32_t(a.s6_addr[14]) << 8)  |  uint32_t(a.s6_addr[15]);
        return isPrivateIPv4(v4, reason);
    }
    return false;
}

#ifdef _WIN32
struct WsaOnce {
    WsaOnce()  { WSADATA d; WSAStartup(MAKEWORD(2, 2), &d); }
    ~WsaOnce() { WSACleanup(); }
};
static WsaOnce s_wsa_once;
#endif

} // namespace

UrlValidation isInternalUrl(const std::string& url) {
    UrlValidation v;
    std::string port_str;
    v.host = extractHost(url, &port_str);
    if (v.host.empty()) {
        v.internal = true;
        v.reason = "URL parse failed (missing or malformed host)";
        return v;
    }

    // Populate v.port for CURLOPT_RESOLVE consumers. Prefer the explicit port
    // from the URL; otherwise default by scheme. Unknown schemes leave port=0,
    // which makes buildResolveEntry return "" and forces cURL to fall back to
    // its normal DNS path (still gated by v.internal upstream).
    if (!port_str.empty()) {
        try { v.port = std::stoi(port_str); } catch (...) { v.port = 0; }
    } else {
        if (url.rfind("https://", 0) == 0)      v.port = 443;
        else if (url.rfind("http://",  0) == 0) v.port = 80;
        else                                    v.port = 0;
    }

    // *.local mDNS — hostname-class private even before resolving (some
    // resolvers refuse, others return random LAN IPs). Refuse outright.
    if (v.host.size() >= 6) {
        const auto suffix = v.host.substr(v.host.size() - 6);
        if ((suffix == ".local" || suffix == ".LOCAL")
            && (v.host.size() == 6 || v.host[v.host.size() - 7] == '.')) {
            v.internal = true;
            v.reason = "*.local mDNS hostname";
            return v;
        }
    }
    // Bare "localhost" alias
    if (v.host == "localhost" || v.host == "LOCALHOST") {
        v.internal = true;
        v.reason = "localhost alias";
        return v;
    }

    struct addrinfo hints{};
    hints.ai_family = AF_UNSPEC;       // both A and AAAA
    hints.ai_socktype = SOCK_STREAM;
    struct addrinfo* res = nullptr;
    int rc = ::getaddrinfo(v.host.c_str(), port_str.empty() ? nullptr : port_str.c_str(),
                           &hints, &res);
    if (rc != 0 || res == nullptr) {
        v.internal = true;
        // gai_strerror returns WCHAR* on Win32 (yuck). Just include the
        // numeric code — operators can grep for it in net docs.
        std::ostringstream os;
        os << "DNS resolution failed (fail-closed): rc=" << rc;
        v.reason = os.str();
        return v;
    }

    bool any_internal = false;
    std::string first_internal_reason;
    for (struct addrinfo* p = res; p != nullptr; p = p->ai_next) {
        std::string r;
        bool internal = false;
        if (p->ai_family == AF_INET) {
            auto* sa4 = reinterpret_cast<struct sockaddr_in*>(p->ai_addr);
            uint32_t addr = ntohl(sa4->sin_addr.s_addr);
            internal = isPrivateIPv4(addr, &r);
            if (v.resolved_ip.empty()) {
                char buf[INET_ADDRSTRLEN] = {0};
                ::inet_ntop(AF_INET, &sa4->sin_addr, buf, sizeof(buf));
                v.resolved_ip = buf;
                v.resolved_ip_is_v6 = false;
            }
        } else if (p->ai_family == AF_INET6) {
            auto* sa6 = reinterpret_cast<struct sockaddr_in6*>(p->ai_addr);
            internal = isPrivateIPv6(sa6->sin6_addr, &r);
            if (v.resolved_ip.empty()) {
                char buf[INET6_ADDRSTRLEN] = {0};
                ::inet_ntop(AF_INET6, &sa6->sin6_addr, buf, sizeof(buf));
                v.resolved_ip = buf;
                v.resolved_ip_is_v6 = true;
            }
        }
        if (internal && !any_internal) {
            any_internal = true;
            first_internal_reason = r;
        }
    }
    ::freeaddrinfo(res);

    if (any_internal) {
        v.internal = true;
        v.reason = first_internal_reason;
    } else {
        v.internal = false;
        v.reason = "public destination";
    }
    return v;
}

std::string buildResolveEntry(const UrlValidation& v) {
    if (v.internal || v.host.empty() || v.resolved_ip.empty() || v.port == 0) {
        return {};
    }
    // CURLOPT_RESOLVE format: "HOST:PORT:ADDRESS[,ADDRESS2,...]"
    // For IPv6, the address segment is NOT bracketed; libcurl accepts the
    // bare ::1 / fc00::1 form. (Bracket-wrapping breaks the parser.)
    std::ostringstream os;
    os << v.host << ":" << v.port << ":" << v.resolved_ip;
    return os.str();
}

} // namespace utils
} // namespace vms
