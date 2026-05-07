#include "core/frame_bus.h"

#include "utils/logger.h"

#include <algorithm>

namespace vms::core {

FrameBus& FrameBus::getInstance() {
    static FrameBus instance;
    return instance;
}

bool FrameBus::subscribe(int camera_id, const std::string& consumer_id,
                         const std::shared_ptr<IFrameConsumer>& consumer) {
    if (!consumer || consumer_id.empty()) {
        return false;
    }

    std::lock_guard<std::mutex> lock(mutex_);
    compactExpiredLocked(camera_id);
    auto& slots = subscribers_[camera_id];

    auto it = std::find_if(slots.begin(), slots.end(),
                           [&](const Subscription& sub) {
                               return sub.consumer_id == consumer_id;
                           });
    if (it != slots.end()) {
        it->consumer = consumer;
        return true;
    }

    slots.push_back(Subscription{consumer_id, consumer});
    LOG_INFO("[FrameBus] Camera {} subscribed consumer '{}'", camera_id, consumer_id);
    return true;
}

void FrameBus::unsubscribe(int camera_id, const std::string& consumer_id) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = subscribers_.find(camera_id);
    if (it == subscribers_.end()) {
        return;
    }

    auto& slots = it->second;
    slots.erase(std::remove_if(slots.begin(), slots.end(),
                               [&](const Subscription& sub) {
                                   return sub.consumer_id == consumer_id;
                               }),
                slots.end());
    if (slots.empty()) {
        subscribers_.erase(it);
    }
}

void FrameBus::unsubscribeAll(int camera_id) {
    std::lock_guard<std::mutex> lock(mutex_);
    subscribers_.erase(camera_id);
}

void FrameBus::publish(int camera_id, const FrameEnvelope& frame) {
    // Hot path: do NOT compact expired entries here. Expired weak_ptrs are filtered
    // implicitly by lock() returning null below; explicit compaction happens on the
    // rare subscribe/unsubscribe paths to keep the per-publish lock window small.
    std::vector<std::shared_ptr<IFrameConsumer>> consumers;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = subscribers_.find(camera_id);
        if (it == subscribers_.end()) {
            return;
        }

        consumers.reserve(it->second.size());
        for (const auto& slot : it->second) {
            if (auto consumer = slot.consumer.lock()) {
                consumers.push_back(std::move(consumer));
            }
        }
    }

    // Invoke consumers OUTSIDE the lock so a slow consumer cannot stall publish() for
    // any other camera, and so consumers may legally call back into FrameBus.
    for (const auto& consumer : consumers) {
        consumer->onFrame(frame);
    }
}

void FrameBus::publishStateChange(int camera_id, CameraState state) {
    std::vector<std::shared_ptr<IFrameConsumer>> consumers;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = subscribers_.find(camera_id);
        if (it == subscribers_.end()) {
            return;
        }

        consumers.reserve(it->second.size());
        for (const auto& slot : it->second) {
            if (auto consumer = slot.consumer.lock()) {
                consumers.push_back(std::move(consumer));
            }
        }
    }

    for (const auto& consumer : consumers) {
        consumer->onStreamStateChanged(camera_id, state);
    }
}

size_t FrameBus::subscriberCount(int camera_id) const {
    // No compaction here either — kept lock window minimal so the hot-path producer
    // can call this as a cheap precondition check before building a FrameEnvelope.
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = subscribers_.find(camera_id);
    if (it == subscribers_.end()) {
        return 0;
    }

    return std::count_if(it->second.begin(), it->second.end(),
                         [](const Subscription& sub) {
                             return !sub.consumer.expired();
                         });
}

void FrameBus::compactExpiredLocked(int camera_id) {
    auto it = subscribers_.find(camera_id);
    if (it == subscribers_.end()) {
        return;
    }

    auto& slots = it->second;
    slots.erase(std::remove_if(slots.begin(), slots.end(),
                               [](const Subscription& sub) {
                                   return sub.consumer.expired();
                               }),
                slots.end());
    if (slots.empty()) {
        subscribers_.erase(it);
    }
}

} // namespace vms::core
