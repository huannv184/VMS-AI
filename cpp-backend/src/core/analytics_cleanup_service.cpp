#include "../../include/core/analytics_cleanup_service.h"
#include "../../include/database/traffic_repository.h"
#include "../../include/database/event_repository.h"
#include "../../include/database/segment_repository.h"
#include "../../include/database/db_manager.h"
#include "../../include/utils/logger.h"
#include <thread>
#include <chrono>
#include <atomic>
#include <string>
#include <memory>
#include <utility>

namespace vms {
namespace core {

class AnalyticsCleanupService::Impl {
public:
    std::atomic<bool> running_{false};
    std::thread worker_;

    void start() {
        if (running_) return;
        running_ = true;
        worker_ = std::thread([this]() {
            LOG_INFO("Analytics Cleanup Service started");
            while (running_) {
                try {
                    performCleanup();
                } catch (const std::exception& e) {
                    LOG_ERROR("Cleanup error: {}", e.what());
                }
                
                // Sleep for 1 hour, check running_ frequently
                for (int i = 0; i < 3600 && running_; ++i) {
                    std::this_thread::sleep_for(std::chrono::seconds(1));
                }
            }
            LOG_INFO("Analytics Cleanup Service stopped");
        });
    }

    void stop() {
        running_ = false;
        if (worker_.joinable()) worker_.join();
    }

    void performCleanup() {
        auto& db = vms::database::DbManager::getInstance();
        std::string days_str = db.getSetting("analytics_retention_days", "30");
        int days = std::stoi(days_str);

        if (days <= 0) return;

        std::time_t now = std::time(nullptr);
        std::time_t threshold = now - (days * 24 * 3600);

        vms::database::TrafficRepository repo;
        if (repo.pruneOld(threshold)) {
            LOG_INFO("Successfully pruned analytics data older than {} days (threshold: {})", days, threshold);
        }

        vms::database::EventRepository eventRepo;
        if (eventRepo.pruneOld(threshold)) {
            LOG_INFO("Successfully pruned old events (threshold: {})", threshold);
        }

        vms::database::SegmentRepository segmentRepo;
        if (segmentRepo.deleteOldSegments(threshold)) {
            LOG_INFO("Successfully pruned old recording segments (threshold: {})", threshold);
        }
    }
};

AnalyticsCleanupService::AnalyticsCleanupService() : impl_(std::make_unique<Impl>()) {}
AnalyticsCleanupService::~AnalyticsCleanupService() { stop(); }

void AnalyticsCleanupService::start() { impl_->start(); }
void AnalyticsCleanupService::stop() { impl_->stop(); }

AnalyticsCleanupService& AnalyticsCleanupService::getInstance() {
    static AnalyticsCleanupService instance;
    return instance;
}

} // namespace core
} // namespace vms
