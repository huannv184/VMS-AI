#pragma once

#include <atomic>

namespace vms {
namespace database {

inline std::atomic<bool> db_ready{false};

// SEC-001: Set to true when the default admin password (SHA256("admin")) is
// still active. Checked at login to force password change. Lock-free, O(1).
inline std::atomic<bool> default_password_active{false};

} // namespace database
} // namespace vms
