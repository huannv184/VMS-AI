#pragma once
#include <memory>
#include <string>

namespace vms {
namespace core {

/**
 * @brief Background service to prune old analytics and counting data
 */
class AnalyticsCleanupService {
public:
    static AnalyticsCleanupService& getInstance();
    
    void start();
    void stop();

private:
    AnalyticsCleanupService();
    ~AnalyticsCleanupService();

    class Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace core
} // namespace vms
