#pragma once
#include <string>
#include <vector>
#include <functional>
#include <memory>
#include <future>
#include <atomic>
#include <mutex>
#include <chrono>
#include "camera_types.h"
#include "utils/logger.h"

namespace CameraDiscovery {

class AutoDetector {
public:
    using ProbeCallback = std::function<void(const std::string&, bool)>;

    static DeviceInfo discover(DiscoveryConfig cfg,
                               ProbeCallback on_probe = nullptr);

private:
    static DeviceInfo discoverKnown(const DiscoveryConfig& cfg);
};

} // namespace CameraDiscovery

#include "brands/core_factory.hpp"

namespace CameraDiscovery {

inline DeviceInfo AutoDetector::discover(DiscoveryConfig cfg, ProbeCallback on_probe)
{
    if (cfg.brand != Brand::Unknown)
        return discoverKnown(cfg);

    std::vector<Brand> tier1_brands = {
        Brand::Hikvision, Brand::Dahua, Brand::Uniview, Brand::Hanwha,
        Brand::Axis, Brand::Bosch, Brand::Pelco, Brand::Milesight, Brand::Reolink
    };

    auto found = std::make_shared<std::atomic<bool>>(false);
    auto is_valid = std::make_shared<std::atomic<bool>>(true); // Protects on_probe after timeout
    auto active_threads = std::make_shared<std::atomic<int>>(static_cast<int>(tier1_brands.size()));
    auto result_mutex = std::make_shared<std::mutex>();
    auto matched_brand = std::make_shared<std::atomic<Brand>>(Brand::Unknown);
    auto successful_port = std::make_shared<std::atomic<int>>(cfg.http_port);

    LOG_INFO("Starting parallel discovery for: {}", cfg.host);

    for (auto b : tier1_brands) {
        std::thread([b, cfg, on_probe, found, result_mutex, matched_brand, successful_port, is_valid, active_threads]() {
            if (found->load()) {
                active_threads->fetch_sub(1);
                return;
            }

            auto core = vms::core::brands::CoreFactory::getCore(b);
            if (!core) {
                active_threads->fetch_sub(1);
                return;
            }

            std::string brand_name = core->getBrandName();
            std::vector<int> ports_to_try = { cfg.http_port };
            for (int p : {80, 8000, 8080, 8899, 443}) {
                if (p != cfg.http_port) ports_to_try.push_back(p);
            }

            bool brand_found = false;
            for (int p : ports_to_try) {
                if (found->load()) break;
                auto trial_cfg = cfg;
                trial_cfg.http_port = p;
                
                try { 
                    if (core->probe(trial_cfg)) {
                        bool trigger_probe = false;
                        {
                            std::lock_guard<std::mutex> lock(*result_mutex);
                            if (!found->load()) {
                                found->store(true);
                                matched_brand->store(b);
                                successful_port->store(p);
                                LOG_INFO(">>> MATCH FOUND: {} at {} (Port {}) <<<", brand_name, cfg.host, p);
                                trigger_probe = true;
                            }
                        }
                        // Release lock before callback to avoid deadlock
                        if (trigger_probe && on_probe && is_valid->load()) {
                            on_probe(brand_name, true);
                        }
                        brand_found = true;
                        break; // Stop trying ports
                    }
                } catch (...) {}
            }
            if (on_probe && !brand_found && !found->load() && is_valid->load()) {
                on_probe(brand_name, false);
            }
            active_threads->fetch_sub(1);
        }).detach();
    }

    auto start_time = std::chrono::steady_clock::now();
    while (active_threads->load() > 0 && !found->load()) {
        if (std::chrono::steady_clock::now() - start_time > std::chrono::seconds(10)) {
            LOG_WARN("Discovery timeout after 10 seconds for {}", cfg.host);
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }

    // Invalidate callback context in case detached threads are still running
    is_valid->store(false);

    if (found->load()) {
        cfg.brand = matched_brand->load();
        cfg.http_port = successful_port->load();
        auto core = vms::core::brands::CoreFactory::getCore(cfg.brand);
        if (core) {
            try { 
                auto result = core->discover(cfg); 
                if (result.error.empty()) return result;
                LOG_WARN("Brand {} discover() error: {}", brandToString(cfg.brand), result.error);
            } catch (const std::exception& e) {
                LOG_ERROR("Discover failed: {}", e.what());
            }
        }
    }

    try {
        auto onvif = vms::core::brands::CoreFactory::getCore(Brand::ONVIF);
        if (onvif && onvif->probe(cfg)) {
            if (on_probe) on_probe("ONVIF", true);
            return onvif->discover(cfg);
        }
    } catch (...) {}

    DeviceInfo unknown;
    unknown.error = "Không thể nhận diện hãng camera. Vui lòng kiểm tra IP, tài khoản, mật khẩu và đảm bảo camera đang online.";
    return unknown;
}

inline DeviceInfo AutoDetector::discoverKnown(const DiscoveryConfig& cfg) {
    auto core = vms::core::brands::CoreFactory::getCore(cfg.brand);
    if (core) return core->discover(cfg);
    DeviceInfo d; d.error = "No core implemented for this brand";
    return d;
}

} // namespace CameraDiscovery
