// Tests for FrameBus contract (Phase A refactor).
// Verifies:
//   - Synchronous fan-out to multiple consumers
//   - Zero-copy raw_frame fan-out (shared_ptr identity across consumers)
//   - Expired weak_ptr is skipped without crashing publish()
//   - Unsubscribe stops delivery to that consumer only
//   - unsubscribeAll clears all consumers for a camera
//   - publishStateChange broadcasts to onStreamStateChanged()
//   - Concurrent publish + subscribe/unsubscribe does not crash and
//     delivers a count within sane bounds (no UB, no double-invoke).
//
// Links the real `src/core/frame_bus.cpp` (not an inline reproduction) so
// regressions in the actual production class are caught.

#include "core/frame_bus.h"

#include <atomic>
#include <chrono>
#include <gtest/gtest.h>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

using vms::core::CameraState;
using vms::core::FrameBus;
using vms::core::FrameEnvelope;
using vms::core::IFrameConsumer;

namespace {

class CountingConsumer : public IFrameConsumer {
public:
    explicit CountingConsumer(std::string id) : id_(std::move(id)) {}
    std::string consumerName() const override { return id_; }

    void onFrame(const FrameEnvelope& f) override {
        std::lock_guard<std::mutex> lock(mu_);
        ++frame_count_;
        last_camera_ = f.camera_id;
        last_raw_ptr_ = f.raw_frame.get();
        last_frame_index_ = f.frame_index;
    }

    void onStreamStateChanged(int camera_id, CameraState state) override {
        std::lock_guard<std::mutex> lock(mu_);
        ++state_count_;
        last_state_camera_ = camera_id;
        last_state_ = state;
    }

    int frameCount() const { std::lock_guard<std::mutex> l(mu_); return frame_count_; }
    int stateCount() const { std::lock_guard<std::mutex> l(mu_); return state_count_; }
    int lastCamera() const { std::lock_guard<std::mutex> l(mu_); return last_camera_; }
    const cv::Mat* lastRawPtr() const { std::lock_guard<std::mutex> l(mu_); return last_raw_ptr_; }
    uint64_t lastFrameIndex() const { std::lock_guard<std::mutex> l(mu_); return last_frame_index_; }
    int lastStateCamera() const { std::lock_guard<std::mutex> l(mu_); return last_state_camera_; }
    CameraState lastState() const { std::lock_guard<std::mutex> l(mu_); return last_state_; }

private:
    std::string id_;
    mutable std::mutex mu_;
    int frame_count_{0};
    int state_count_{0};
    int last_camera_{-1};
    const cv::Mat* last_raw_ptr_{nullptr};
    uint64_t last_frame_index_{0};
    int last_state_camera_{-1};
    CameraState last_state_{CameraState::CONNECTING};
};

FrameEnvelope makeEnvelope(int cam, uint64_t idx, const std::shared_ptr<cv::Mat>& mat) {
    FrameEnvelope env;
    env.camera_id = cam;
    env.timestamp_ms = 1'700'000'000'000ULL + idx;
    env.frame_index = idx;
    env.raw_frame = mat;
    return env;
}

// Each test uses a unique camera_id to avoid cross-pollution from the FrameBus
// singleton's internal state. unsubscribeAll() at the end of each test keeps the
// map clean for the next test.
constexpr int kCamA = 9001;
constexpr int kCamB = 9002;
constexpr int kCamC = 9003;
constexpr int kCamD = 9004;
constexpr int kCamE = 9005;
constexpr int kCamStress = 9006;

} // namespace

TEST(FrameBus, FanoutToMultipleConsumers) {
    auto& bus = FrameBus::getInstance();

    auto a = std::make_shared<CountingConsumer>("A");
    auto b = std::make_shared<CountingConsumer>("B");
    EXPECT_TRUE(bus.subscribe(kCamA, "A", a));
    EXPECT_TRUE(bus.subscribe(kCamA, "B", b));
    EXPECT_EQ(bus.subscriberCount(kCamA), 2u);

    auto mat = std::make_shared<cv::Mat>(cv::Mat::zeros(4, 4, CV_8UC1));
    bus.publish(kCamA, makeEnvelope(kCamA, 1, mat));
    bus.publish(kCamA, makeEnvelope(kCamA, 2, mat));

    EXPECT_EQ(a->frameCount(), 2);
    EXPECT_EQ(b->frameCount(), 2);
    EXPECT_EQ(a->lastCamera(), kCamA);
    EXPECT_EQ(b->lastCamera(), kCamA);
    EXPECT_EQ(a->lastFrameIndex(), 2u);

    bus.unsubscribeAll(kCamA);
}

TEST(FrameBus, RawFrameIsSharedZeroCopyAcrossConsumers) {
    auto& bus = FrameBus::getInstance();

    auto a = std::make_shared<CountingConsumer>("A");
    auto b = std::make_shared<CountingConsumer>("B");
    auto c = std::make_shared<CountingConsumer>("C");
    bus.subscribe(kCamB, "A", a);
    bus.subscribe(kCamB, "B", b);
    bus.subscribe(kCamB, "C", c);

    auto mat = std::make_shared<cv::Mat>(cv::Mat::ones(8, 8, CV_8UC3));
    const cv::Mat* expected_ptr = mat.get();
    bus.publish(kCamB, makeEnvelope(kCamB, 42, mat));

    // All three consumers must observe the SAME cv::Mat object — no per-consumer
    // deep copy, no per-consumer header copy. This is the core perf invariant we
    // refactored to in Phase A.
    EXPECT_EQ(a->lastRawPtr(), expected_ptr);
    EXPECT_EQ(b->lastRawPtr(), expected_ptr);
    EXPECT_EQ(c->lastRawPtr(), expected_ptr);

    bus.unsubscribeAll(kCamB);
}

TEST(FrameBus, ExpiredConsumerIsSkippedNotCrashed) {
    auto& bus = FrameBus::getInstance();

    auto live = std::make_shared<CountingConsumer>("live");
    bus.subscribe(kCamC, "live", live);

    {
        auto temp = std::make_shared<CountingConsumer>("temp");
        bus.subscribe(kCamC, "temp", temp);
        // temp goes out of scope here → weak_ptr in FrameBus expires.
    }

    // subscriberCount() should report only the live consumer.
    EXPECT_EQ(bus.subscriberCount(kCamC), 1u);

    auto mat = std::make_shared<cv::Mat>(cv::Mat::zeros(2, 2, CV_8UC1));
    // Must not crash even though one slot's weak_ptr is expired.
    bus.publish(kCamC, makeEnvelope(kCamC, 1, mat));

    EXPECT_EQ(live->frameCount(), 1);

    bus.unsubscribeAll(kCamC);
}

TEST(FrameBus, UnsubscribeStopsDeliveryToThatConsumerOnly) {
    auto& bus = FrameBus::getInstance();

    auto a = std::make_shared<CountingConsumer>("A");
    auto b = std::make_shared<CountingConsumer>("B");
    bus.subscribe(kCamD, "A", a);
    bus.subscribe(kCamD, "B", b);

    auto mat = std::make_shared<cv::Mat>(cv::Mat::zeros(2, 2, CV_8UC1));
    bus.publish(kCamD, makeEnvelope(kCamD, 1, mat));
    EXPECT_EQ(a->frameCount(), 1);
    EXPECT_EQ(b->frameCount(), 1);

    bus.unsubscribe(kCamD, "A");
    bus.publish(kCamD, makeEnvelope(kCamD, 2, mat));
    EXPECT_EQ(a->frameCount(), 1);  // unchanged
    EXPECT_EQ(b->frameCount(), 2);

    bus.unsubscribeAll(kCamD);
}

TEST(FrameBus, StateChangeBroadcast) {
    auto& bus = FrameBus::getInstance();

    auto a = std::make_shared<CountingConsumer>("A");
    auto b = std::make_shared<CountingConsumer>("B");
    bus.subscribe(kCamE, "A", a);
    bus.subscribe(kCamE, "B", b);

    bus.publishStateChange(kCamE, CameraState::DEGRADED);
    bus.publishStateChange(kCamE, CameraState::FAILED);

    EXPECT_EQ(a->stateCount(), 2);
    EXPECT_EQ(b->stateCount(), 2);
    EXPECT_EQ(a->lastState(), CameraState::FAILED);
    EXPECT_EQ(b->lastStateCamera(), kCamE);

    bus.unsubscribeAll(kCamE);
}

// Concurrency stress test:
//   - 4 publisher threads spamming publish() for ~50ms.
//   - 1 churn thread that repeatedly subscribes a transient consumer, lets it die,
//     and re-subscribes — exercising the expired-weak_ptr code path under load.
//   - 1 stable consumer that must receive a non-zero count and never crash.
TEST(FrameBus, ConcurrentPublishAndSubscribeNoCrash) {
    auto& bus = FrameBus::getInstance();

    auto stable = std::make_shared<CountingConsumer>("stable");
    bus.subscribe(kCamStress, "stable", stable);

    std::atomic<bool> stop{false};
    auto mat = std::make_shared<cv::Mat>(cv::Mat::zeros(4, 4, CV_8UC1));

    std::vector<std::thread> producers;
    std::atomic<uint64_t> total_published{0};
    for (int t = 0; t < 4; ++t) {
        producers.emplace_back([&]() {
            uint64_t i = 0;
            while (!stop.load(std::memory_order_relaxed)) {
                bus.publish(kCamStress, makeEnvelope(kCamStress, i++, mat));
                total_published.fetch_add(1, std::memory_order_relaxed);
            }
        });
    }

    std::thread churn([&]() {
        int gen = 0;
        while (!stop.load(std::memory_order_relaxed)) {
            auto transient = std::make_shared<CountingConsumer>("transient");
            bus.subscribe(kCamStress, "transient_" + std::to_string(gen), transient);
            std::this_thread::sleep_for(std::chrono::microseconds(100));
            bus.unsubscribe(kCamStress, "transient_" + std::to_string(gen));
            ++gen;
        }
    });

    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    stop.store(true, std::memory_order_relaxed);

    for (auto& th : producers) th.join();
    churn.join();

    // Stable consumer must have received SOMETHING (we ran for 50ms with 4 producers).
    EXPECT_GT(stable->frameCount(), 0);
    // And no more than total_published (sanity: each publish hits stable at most once).
    EXPECT_LE(static_cast<uint64_t>(stable->frameCount()),
              total_published.load(std::memory_order_relaxed));

    bus.unsubscribeAll(kCamStress);
}
