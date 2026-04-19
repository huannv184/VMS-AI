#pragma once

#include <string>
#include <string>

namespace vms {
namespace middleware {

class LdapConnector {
public:
    LdapConnector(const std::string& host, int port, const std::string& domain);
    ~LdapConnector();

    // Prevent copying
    LdapConnector(const LdapConnector&) = delete;
    LdapConnector& operator=(const LdapConnector&) = delete;

    /**
     * @brief Authenticate user against Active Directory
     * @param username Username (without domain or with domain)
     * @param password Password
     * @return true if authentication successful
     */
    bool authenticate(const std::string& username, const std::string& password);

private:
    std::string host_;
    int port_;
    std::string domain_;
    
    // Helper to format username (user@domain.com)
    std::string formatPrincipal(const std::string& username);
};

} // namespace middleware
} // namespace vms
