#pragma once

#include "streaming/stream_pipeline.h"
#include "recording/buffer_manager.h"
#include "core/ffmpeg_process.h"
#include <QByteArray>
#include <vector>
#include <string>
#include <memory>
#include <mutex>
#include <atomic>
#include <thread>
#include <fstream>
#include <unordered_set>

namespace vms {
namespace recording {

class BufferPipeline : public streaming::StreamPipeline {
public:
    BufferPipeline(int camera_id, const streaming::StreamProfile& profile);
    ~BufferPipeline();

    bool start() override;
    void stop() override;

    // Feed raw H264 data from NativeReader into the muxer
    void writeRawData(const uint8_t* data, int size);


    // Trigger an event recording
    // duration_sec: How long to record AFTER the trigger (post-record)
    // pre_record_sec: How much RAM buffer to include (pre-record)
    std::string triggerEvent(const std::string& event_id, int duration_sec = 20, int pre_record_sec = 10);

protected:
    std::string buildFFmpegCommand() override;

private:
    void handleFFmpegData(const QByteArray& data); 
    void remuxToMP4(const std::string& ts_filename); // Helper for post-processing

    // Note: ffmpeg_ and worker_thread_ are inherited from StreamPipeline
    
    std::unique_ptr<BufferManager> buffer_manager_;
    
    // std::atomic<bool> should_stop_{false}; // Inherited from StreamPipeline too!
    
    // Active Recording State (Thread-Safe)
    struct ActiveRecording {
        std::string event_id;
        std::string filename;
        long long start_time;
        long long end_time; // When to stop post-recording
        
        std::atomic<bool> header_written{false};
        std::atomic<bool> finished{false};
        
        // File access - must be synchronized
        std::unique_ptr<std::ofstream> file_stream;
        std::mutex file_mutex; 
        
        // Helper to safely write data
        bool safeWrite(const char* data, size_t size) {
            if (finished.load()) return false; // Optimization check
            std::lock_guard<std::mutex> lock(file_mutex);
            if (!file_stream || !file_stream->is_open()) return false;
            try {
                file_stream->write(data, size);
                return true;
            } catch (...) { return false; }
        }

        // Helper to safely close
        void safeClose() {
            finished.store(true);
            std::lock_guard<std::mutex> lock(file_mutex);
            if (file_stream && file_stream->is_open()) {
                file_stream->flush();
                file_stream->close();
            }
        }
    };

    std::mutex recording_mutex_;
    std::vector<std::shared_ptr<ActiveRecording>> active_recordings_;
    
    // Fix Race Condition in Remux
    std::mutex remux_mutex_;
    std::unordered_set<std::string> pending_remux_;
    
    // ARCH-008 FIX: Track remux threads for safe joining on shutdown
    std::vector<std::thread> remux_threads_;
};

} // namespace recording
} // namespace vms
