#include "recording/buffer_pipeline.h"
#include <QByteArray>
#include "core/ffmpeg_process.h"
#include "utils/logger.h"
#include <sstream>
#include "database/event_repository.h"
#include "utils/storage_manager.h"
#include <filesystem>
#include <chrono>

namespace vms {
namespace recording {

static std::string sanitizeEventIdForFilename(const std::string& in) {
    // Allow only a conservative set to prevent path traversal and shell escaping issues.
    // Keep length bounded to avoid filesystem/path issues.
    std::string out;
    out.reserve(std::min<size_t>(in.size(), 80));
    for (char ch : in) {
        const unsigned char c = static_cast<unsigned char>(ch);
        if ((c >= 'a' && c <= 'z') ||
            (c >= 'A' && c <= 'Z') ||
            (c >= '0' && c <= '9') ||
            ch == '-' || ch == '_' ) {
            out.push_back(ch);
        } else {
            out.push_back('_');
        }
        if (out.size() >= 80) break;
    }
    if (out.empty()) out = "event";
    return out;
}

BufferPipeline::BufferPipeline(int camera_id, const streaming::StreamProfile& profile)
    : StreamPipeline(camera_id, profile) {
    buffer_manager_ = std::make_unique<BufferManager>(30000); // 30s buffer
}

BufferPipeline::~BufferPipeline() {
    stop();
}

bool BufferPipeline::start() {
    std::lock_guard<std::mutex> lock(state_mutex_);
    if (isRunning()) return false;

    should_stop_ = false;
    
    ffmpeg_ = std::make_unique<core::FFmpegProcess>();
    
    QObject::connect(ffmpeg_.get(), &core::FFmpegProcess::stdoutReady,
                     [this](const QByteArray& data) {
                         this->handleFFmpegData(data);
                     });
                     
    std::string cmd = buildFFmpegCommand();
    if (!ffmpeg_->start(cmd)) {
        LOG_ERROR("[BufferPipeline-{}] Failed to start FFmpeg", camera_id_);
        return false;
    }

    setState(vms::core::PipelineState::RUNNING);
    LOG_INFO("[BufferPipeline-{}] Started RAM buffering", camera_id_);
    return true;
}

void BufferPipeline::stop() {
    should_stop_ = true;
    
    if (ffmpeg_) ffmpeg_->stop();
    
    
    // ARCH-008 FIX: Cleanly join all remux threads before destroying object
    {
        std::lock_guard<std::mutex> lock(remux_mutex_);
        for (auto& t : remux_threads_) {
            if (t.joinable()) t.join();
        }
        remux_threads_.clear();
    }
    
    buffer_manager_->clear();
    setState(vms::core::PipelineState::STOPPED);
}

std::string BufferPipeline::triggerEvent(const std::string& event_id, int duration_sec, int pre_record_sec) {
    std::lock_guard<std::mutex> lock(recording_mutex_);
    
    auto now = std::chrono::system_clock::now();
    long long now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count();
    
    // Create filename
    const auto safe_event_id = sanitizeEventIdForFilename(event_id);
    const std::filesystem::path recordings_dir = "recordings";
    std::filesystem::create_directories(recordings_dir);
    const std::filesystem::path filepath = recordings_dir / ("event_" + safe_event_id + ".ts");
    std::string filename = filepath.lexically_normal().string();

    auto recording = std::make_shared<ActiveRecording>();
    recording->event_id = event_id;
    recording->filename = filename;
    recording->start_time = now_ms;
    recording->end_time = now_ms + (duration_sec * 1000);
    recording->header_written = false;
    
    // Use unique_ptr for file stream (thread-safe struct handles locking)
    recording->file_stream = std::make_unique<std::ofstream>(filename, std::ios::binary);

    if (!recording->file_stream->is_open()) {
        LOG_ERROR("Failed to open file for recording: {}", filename);
        return "";
    }

    // DUMP PRE-RECORD BUFFER
    // Use try-catch for safety during buffer copy
    try {
        auto snapshot = buffer_manager_->getSnapshot();
        if (snapshot.empty()) {
            LOG_WARN("[BufferPipeline-{}] Buffer is EMPTY! Pre-record will be missing.", camera_id_);
        } else {
            LOG_INFO("[BufferPipeline-{}] Buffer snapshot has {} packets.", camera_id_, snapshot.size());
        }

        for (const auto& packet : snapshot) {
            // Use safeWrite (though technically we are exclusive here before push_back)
            if (!recording->safeWrite(packet.data.data(), packet.data.size())) {
                LOG_ERROR("Failed to write pre-record packet");
            }
        }
        
        LOG_INFO("[BufferPipeline-{}] Event Triggered! Dumped {} packets (Pre-record). Recording for {}s more.", 
                 camera_id_, snapshot.size(), duration_sec);
  
        active_recordings_.push_back(recording);
        return filename;

    } catch (const std::exception& e) {
        LOG_ERROR("[BufferPipeline-{}] Error dumping buffer: {}", camera_id_, e.what());
        return "";
    }
}

std::string BufferPipeline::buildFFmpegCommand() {
    std::stringstream cmd;
    // Start an FFmpeg muxer that reads RAW H264 from STDIN and outputs MPEG-TS to STDOUT
    cmd << "ffmpeg -y -nostdin -loglevel warning "
        << "-f h264 -i pipe:0 "
        << "-c copy -f mpegts -"; 
    return cmd.str();
}

void BufferPipeline::writeRawData(const uint8_t* data, int size) {
    if (ffmpeg_ && ffmpeg_->isRunning()) {
        ffmpeg_->writeStdin(reinterpret_cast<const char*>(data), size);
    }
}

void BufferPipeline::handleFFmpegData(const QByteArray& data) {
    if (data.isEmpty()) return;
    
    int n = data.size();
    static thread_local int64_t total_bytes_read = 0;
    
    total_bytes_read += n;
    if (total_bytes_read % (1024 * 1024) < n) {
        LOG_DEBUG("[BufferPipeline-{}] Receiving data... (Total: {} bytes)", camera_id_, total_bytes_read);
    }

    long long now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count();

    std::vector<char> packet_data(data.constData(), data.constData() + n);
    buffer_manager_->push(packet_data, now_ms);
    
    {
        std::lock_guard<std::mutex> lock(recording_mutex_);
        for (auto& rec : active_recordings_) {
            if (rec->finished.load()) continue;
            if (now_ms >= rec->end_time) {
                LOG_INFO("[BufferPipeline] Event recording finished: {}", rec->event_id);
                rec->safeClose();
                try {
                    vms::database::EventRepository event_repo;
                    int duration = (int)((rec->end_time - rec->start_time) / 1000);
                    if (!event_repo.updateEventVideo(rec->event_id, rec->filename, duration)) {
                        // DB write failed but the .ts is on disk and the
                        // remux job below still proceeds. Log so operators
                        // know the playback API will return video_path=NULL
                        // for this event_id even though the file exists.
                        LOG_ERROR("[BufferPipeline] updateEventVideo returned false for event {} "
                                  "(file {} on disk but DB row not updated)",
                                  rec->event_id, rec->filename);
                    }
                } catch (const std::exception& e) {
                    LOG_ERROR("[BufferPipeline] updateEventVideo threw for event {}: {}",
                              rec->event_id, e.what());
                } catch (...) {
                    LOG_ERROR("[BufferPipeline] updateEventVideo threw unknown exception for event {}",
                              rec->event_id);
                }
                remuxToMP4(rec->filename);
                rec->finished.store(true);
            } else {
                rec->safeWrite(data.constData(), n);
            }
        }
        active_recordings_.erase(
            std::remove_if(active_recordings_.begin(), active_recordings_.end(), [](const std::shared_ptr<ActiveRecording>& rec) { return rec->finished.load(); }),
            active_recordings_.end()
        );
    }
}

void BufferPipeline::remuxToMP4(const std::string& ts_filename) {
    if (ts_filename.empty()) return;

    // Fix: Prevent duplicate remux jobs for the same file
    {
        std::lock_guard<std::mutex> lock(remux_mutex_);
        if (pending_remux_.count(ts_filename)) {
            return; // Already being remuxed
        }
        pending_remux_.insert(ts_filename);
    }

    // Clean up finished threads occasionally to prevent vector from growing indefinitely
    {
        // We defer cleanup to avoid deadlocking if called frequently
        // but since this is called rarely, a quick reap is fine if we check joinability carefully.
        // For simplicity and safety, we just let them accumulate until stop()
        // unless it becomes a long-lived object with thousands of events.
    }

    // ARCH-008 FIX: Push joinable thread to member vector instead of detaching
    remux_threads_.emplace_back([this, ts_filename]() {
        try {
            std::filesystem::path ts_path(ts_filename);
            std::filesystem::path mp4_path = ts_path;
            mp4_path.replace_extension(".mp4");
            
            LOG_INFO("Starting remux: {} -> {}", ts_path.string(), mp4_path.string());
            
            // Avoid invoking a shell. Use QProcess wrapper (FFmpegProcess) instead.
            core::FFmpegProcess remux_proc;
            std::stringstream cmd;
            cmd << "ffmpeg -y -nostdin -loglevel error -i \"" << ts_path.string() << "\" "
                << "-c copy -movflags +faststart \"" << mp4_path.string() << "\"";
            const bool started = remux_proc.start(cmd.str());
            int ret = -1;
            if (started) {
                ret = remux_proc.wait(5 * 60 * 1000); // up to 5 minutes
            }
            
            if (ret == 0) {
                 LOG_INFO("Remux success: {}", mp4_path.string());

                 // Upload to MinIO. On failure we keep the local mp4 on disk
                 // and leave the DB pointing at the original .ts path (set at
                 // event-finished time above) — that path is still playable
                 // via the local-recordings fallback. Surface the failure
                 // explicitly so a misconfigured/full MinIO doesn't go
                 // unnoticed.
                 std::string object_key = "recordings/" + mp4_path.filename().string();
                 if (vms::utils::StorageManager::getInstance().uploadFile(mp4_path.string(), object_key)) {
                     LOG_INFO("Uploaded video to MinIO: {}", object_key);

                     try {
                         vms::database::EventRepository event_repo;
                         if (!event_repo.updateEventVideo(ts_filename, object_key, -1)) {
                             LOG_ERROR("[BufferPipeline] updateEventVideo returned false for ts {} -> {} "
                                       "(MinIO upload OK but DB still points at .ts)",
                                       ts_filename, object_key);
                         }
                     } catch (const std::exception& e) {
                         LOG_ERROR("[BufferPipeline] updateEventVideo threw for ts {}: {}", ts_filename, e.what());
                     } catch (...) {
                         LOG_ERROR("[BufferPipeline] updateEventVideo threw unknown for ts {}", ts_filename);
                     }
                 } else {
                     LOG_ERROR("[BufferPipeline] MinIO upload FAILED for {} (key {}). "
                               "Local mp4 retained; DB row keeps the .ts path "
                               "for fallback playback.",
                               mp4_path.string(), object_key);
                 }

                 // Optionally delete local files after upload?
                 // For now keep them as backup unless requested otherwise.
            } else {
                 LOG_ERROR("Remux failed for {}. Return code: {}", ts_path.string(), ret);
            }
        } catch (const std::exception& e) {
            LOG_ERROR("Remux thread exception: {}", e.what());
        }

        // Cleanup pending state
        try {
            std::lock_guard<std::mutex> lock(this->remux_mutex_);
            this->pending_remux_.erase(ts_filename);
        } catch (...) {}
    });
}

} // namespace recording
} // namespace vms
