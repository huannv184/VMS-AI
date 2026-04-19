#include "middleware/ldap_connector.h"
#include "utils/logger.h"
#include <string>
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <winldap.h>

namespace vms {
namespace middleware {

LdapConnector::LdapConnector(const std::string& host, int port, const std::string& domain)
    : host_(host), port_(port), domain_(domain) {
}

LdapConnector::~LdapConnector() {
}

std::string LdapConnector::formatPrincipal(const std::string& username) {
    if (username.find("@") != std::string::npos || domain_.empty()) {
        return username; // Already a UPN or no domain configured
    }
    return username + "@" + domain_;
}

bool LdapConnector::authenticate(const std::string& username, const std::string& password) {
    if (username.empty() || password.empty()) return false;

    // Convert std::string to wide string for Unicode API
    std::wstring wHostname;
    if (!host_.empty()) {
        int len = MultiByteToWideChar(CP_UTF8, 0, host_.c_str(), -1, NULL, 0);
        wHostname.resize(len);
        MultiByteToWideChar(CP_UTF8, 0, host_.c_str(), -1, &wHostname[0], len);
        // Remove null terminator if included by resize
        if (!wHostname.empty() && wHostname.back() == 0) wHostname.pop_back();
    }

    // Initialize LDAP connection
    LDAP* ld = ldap_initW(wHostname.empty() ? NULL : const_cast<wchar_t*>(wHostname.c_str()), port_);
    if (ld == NULL) {
        LOG_ERROR("ldap_init failed with error: {}", LdapGetLastError());
        return false;
    }

    // Set LDAP version to 3
    ULONG version = LDAP_VERSION3;
    ULONG ret = ldap_set_option(ld, LDAP_OPT_PROTOCOL_VERSION, (void*)&version);
    if (ret != LDAP_SUCCESS) {
        LOG_WARN("ldap_set_option(LDAP_VERSION3) failed: {}", ret);
    }

    // Connect to server (optional, bind handles it, but good for diagnostics)
    ret = ldap_connect(ld, NULL);
    if (ret != LDAP_SUCCESS) {
        LOG_ERROR("ldap_connect failed with error: 0x{:X}", ret);
        ldap_unbind(ld);
        return false;
    }

    // Format UPN (user@domain.com) for binding
    std::string principal = formatPrincipal(username);
    
    // Convert Principal to Wide String
    std::wstring wPrincipal;
    int lenP = MultiByteToWideChar(CP_UTF8, 0, principal.c_str(), -1, NULL, 0);
    wPrincipal.resize(lenP);
    MultiByteToWideChar(CP_UTF8, 0, principal.c_str(), -1, &wPrincipal[0], lenP);
    if (!wPrincipal.empty() && wPrincipal.back() == 0) wPrincipal.pop_back();

    // Convert Password to Wide String
    std::wstring wPassword;
    int lenPass = MultiByteToWideChar(CP_UTF8, 0, password.c_str(), -1, NULL, 0);
    wPassword.resize(lenPass);
    MultiByteToWideChar(CP_UTF8, 0, password.c_str(), -1, &wPassword[0], lenPass);
    if (!wPassword.empty() && wPassword.back() == 0) wPassword.pop_back();

    // Perform Simple Bind (Authentication)
    // Note: Plaintext password over non-SSL LDAP is insecure but standard for internal networks
    // Ideally use ldap_bind_s with SEC_WINNT_AUTH_IDENTITY or SSL
    ret = ldap_simple_bind_sW(ld, const_cast<wchar_t*>(wPrincipal.c_str()), const_cast<wchar_t*>(wPassword.c_str()));

    bool success = (ret == LDAP_SUCCESS);

    if (success) {
        LOG_INFO("LDAP Authentication successful for user: {}", principal);
    } else {
        LOG_WARN("LDAP Authentication failed for user: {} (Error: 0x{:X})", principal, ret);
    }

    // Unbind and free resources
    ldap_unbind(ld);

    return success;
}

} // namespace middleware
} // namespace vms
