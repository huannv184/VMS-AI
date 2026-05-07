#pragma once

#include "core/camera_runtime_types.h"
#include "inference/tracking.h"

#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include <nlohmann/json.hpp>
#include <opencv2/core.hpp>

namespace vms::core {

struct FrameEnvelope {
    int camera_id{0};
    uint64_t timestamp_ms{0};
    uint64_t frame_index{0};
    // Raw frame is shared across consumers (zero deep-copy fanout). Null if not available.
    // cv::Mat is internally reference-counted, but std::shared_ptr makes ownership across
    // async consumers explicit and avoids relying on cv::Mat's mutable refcount.
    std::shared_ptr<const cv::Mat> raw_frame;
    std::vector<uchar> jpeg_preview;
    std::vector<inference::TrackedObject> objects;
    nlohmann::json metadata;
    std::string source_stream;
    bool is_backup_stream{false};
};

class IFrameConsumer {
public:
    virtual ~IFrameConsumer() = default;
    virtual std::string consumerName() const = 0;
    virtual void onFrame(const FrameEnvelope& frame) = 0;
    virtual void onStreamStateChanged(int camera_id, CameraState state) {
        (void)camera_id;
        (void)state;
    }
};

class FrameBus {
public:
    static FrameBus& getInstance();

    bool subscribe(int camera_id, const std::string& consumer_id,
                   const std::shared_ptr<IFrameConsumer>& consumer);
    void unsubscribe(int camera_id, const std::string& consumer_id);
    void unsubscribeAll(int camera_id);

    void publish(int camera_id, const FrameEnvelope& frame);
    void publishStateChange(int camera_id, CameraState state);

    size_t subscriberCount(int camera_id) const;

private:
    struct Subscription {
        std::string consumer_id;
        std::weak_ptr<IFrameConsumer> consumer;
    };

    void compactExpiredLocked(int camera_id);

    mutable std::mutex mutex_;
    std::unordered_map<int, std::vector<Subscription>> subscribers_;
};

} // namespace vms::core
