#pragma once

#include <atomic>

namespace vms {
namespace core {

inline std::atomic<bool> shutdown_requested{false};
inline std::atomic<bool> shutting_down{false};

} // namespace core
} // namespace vms
