#include "recording/continuous_recorder.h"
#include "core/ffmpeg_process.h"
#include "database/segment_repository.h"
#include "utils/logger.h"

#include <sstream>
#include <filesystem>
#include <chrono>
#include <regex>
#include <fstream>

namespace fs = std::filesystem;

namespace vms {
namespace recording {

ContinuousRecorder::ContinuousRecorder(int camera_id, const std::string& rtsp_url,
                                       const std::string& output_dir,
                                       int segment_sec, int retention_days)
    : camera_id_(camera_id)
    , rtsp_url_(rtsp_url)
    , output_dir_(output_dir)
    , segment_sec_(segment_sec)
    , retention_days_(retention_days) {
}

ContinuousRecorder::~ContinuousRecorder() {
    stop();
}

std::string ContinuousRecorder::buildSegmentDir() const {
    return output_dir_ + "/cam_" + std::to_string(camera_id_);
}

bool ContinuousRecorder::start() {
    if (running_.load()) {
        LOG_WARN("[ContinuousRecorder-{}] Already running", camera_id_);
        return false;
    }

    // Create output directory
    std::string seg_dir = buildSegmentDir();
    try {
        fs::create_directories(seg_dir);
    } catch (const std::exception& e) {
        LOG_ERROR("[ContinuousRecorder-{}] Failed to create dir {}: {}", camera_id_, seg_dir, e.what());
        return false;
    }

    should_stop_ = false;
    running_ = true;
    recorder_thread_ = std::thread(&ContinuousRecorder::recorderLoop, this);

    LOG_INFO("[ContinuousRecorder-{}] Started → {}", camera_id_, seg_dir);
    return true;
}

void ContinuousRecorder::stop() {
    if (!running_.load()) return;

    should_stop_ = true;
    
    if (ffmpeg_) {
        ffmpeg_->stop();
    }

    if (recorder_thread_.joinable()) {
        recorder_thread_.join();
    }

    running_ = false;
    LOG_INFO("[ContinuousRecorder-{}] Stopped", camera_id_);
}

void ContinuousRecorder::kill() {
    should_stop_ = true;
    if (ffmpeg_) ffmpeg_->kill();
}

void ContinuousRecorder::writeRawData(const uint8_t* data, int size) {
    if (ffmpeg_ && ffmpeg_->isRunning()) {
        ffmpeg_->writeStdin(reinterpret_cast<const char*>(data), size);
    }
}

void ContinuousRecorder::recorderLoop() {
    const int MAX_RESTARTS = 10;
    const int RESTART_DELAY_SEC = 5;
    int restart_count = 0;

    while (!should_stop_.load()) {
        // Build FFmpeg command using segment muxer
        std::string seg_dir = buildSegmentDir();

        // Output pattern: seg_YYYYMMDD_HHMMSS.mp4
        // %epoch is not supported by strftime, use %%04d for segment index instead
        std::string output_pattern = seg_dir + "/seg_%04d.mp4";

        // Segment list file for tracking completed segments
        std::string segment_list = seg_dir + "/segment_list.csv";

        // Resolve FFmpeg path using the same locator as FFmpegProcess
        std::string ffmpeg_path = core::getFfmpegPath();
        LOG_INFO("[ContinuousRecorder-{}] Resolved FFmpeg: {}", camera_id_, ffmpeg_path);

        std::stringstream cmd;
        cmd << "\"" << ffmpeg_path << "\" -y -nostdin -loglevel warning "
            << "-rtsp_transport tcp -i \"" << rtsp_url_ << "\" "
            << "-c copy "
            << "-f segment "
            << "-segment_time " << segment_sec_ << " "
            << "-segment_format mp4 "
            << "-segment_list \"" << segment_list << "\" "
            << "-segment_list_type csv "
            << "-reset_timestamps 1 "
            << "-movflags +faststart "
            << "\"" << output_pattern << "\"";

        LOG_INFO("[ContinuousRecorder-{}] Starting FFmpeg: segment_time={}s", camera_id_, segment_sec_);

        ffmpeg_ = std::make_unique<core::FFmpegProcess>();
        
        QObject::connect(ffmpeg_.get(), &core::FFmpegProcess::stderrReady, [this](const QByteArray& data) {
            std::string err_str = data.toStdString();
            // Optional: trim newline if needed
            if (!err_str.empty() && err_str.back() == '\n') err_str.pop_back();
            if (!err_str.empty() && err_str.back() == '\r') err_str.pop_back();
            LOG_WARN("[ContinuousRecorder-{}] FFmpeg stderr: {}", camera_id_, err_str);
        });

        if (!ffmpeg_->start(cmd.str())) {
            LOG_ERROR("[ContinuousRecorder-{}] Failed to start FFmpeg", camera_id_);
            restart_count++;
            if (restart_count >= MAX_RESTARTS) {
                LOG_ERROR("[ContinuousRecorder-{}] Max restarts reached, giving up", camera_id_);
                break;
            }
            std::this_thread::sleep_for(std::chrono::seconds(RESTART_DELAY_SEC));
            continue;
        }

        restart_count = 0;

        // Monitor FFmpeg process
        while (!should_stop_.load() && ffmpeg_ && ffmpeg_->isRunning()) {
            // Every 30 seconds, scan for new completed segments and register them
            std::this_thread::sleep_for(std::chrono::seconds(30));
            
            if (!should_stop_.load()) {
                scanAndRegisterSegments();
                pruneOldSegments();
            }
        }

        if (!should_stop_.load()) {
            LOG_WARN("[ContinuousRecorder-{}] FFmpeg exited unexpectedly, restarting in {}s...", 
                     camera_id_, RESTART_DELAY_SEC);
            
            // Do a final scan before restart
            scanAndRegisterSegments();
            
            std::this_thread::sleep_for(std::chrono::seconds(RESTART_DELAY_SEC));
        }
    }

    // Final scan on exit
    scanAndRegisterSegments();
    running_ = false;
}

void ContinuousRecorder::scanAndRegisterSegments() {
    std::string seg_dir = buildSegmentDir();
    std::string segment_list_path = seg_dir + "/segment_list.csv";

    // Read segment list CSV produced by FFmpeg
    // Format: filename,start_time,end_time
    if (!fs::exists(segment_list_path)) return;

    try {
        std::ifstream csvFile(segment_list_path);
        if (!csvFile.is_open()) return;

        database::SegmentRepository repo;
        std::string line;

        while (std::getline(csvFile, line)) {
            if (line.empty()) continue;
            
            // Parse CSV: filename,start_seconds,end_seconds
            std::stringstream ss(line);
            std::string filename, start_str, end_str;
            
            std::getline(ss, filename, ',');
            std::getline(ss, start_str, ',');
            std::getline(ss, end_str, ',');

            if (filename.empty()) continue;

            // Build full path
            std::string full_path = seg_dir + "/" + filename;
            if (!fs::exists(full_path)) continue;

            // Calculate timestamps
            // FFmpeg segment_list gives relative seconds from start
            // We need to figure out absolute timestamps from file modification time
            auto file_mod_time = fs::last_write_time(full_path);
            auto sctp = std::chrono::time_point_cast<std::chrono::system_clock::duration>(
                file_mod_time - fs::file_time_type::clock::now() + std::chrono::system_clock::now()
            );
            time_t end_time = std::chrono::system_clock::to_time_t(sctp);
            time_t start_time = end_time - segment_sec_;

            size_t file_size = fs::file_size(full_path);

            // Check if already registered (avoid duplicates)
            auto existing = repo.getSegments(camera_id_, start_time - 10, start_time + 10);
            bool already_exists = false;
            for (const auto& seg : existing) {
                if (seg.filename == full_path) {
                    already_exists = true;
                    break;
                }
            }

            if (!already_exists) {
                repo.insertSegment(camera_id_, full_path, start_time, end_time, file_size, "completed");
            }
        }
    } catch (const std::exception& e) {
        LOG_ERROR("[ContinuousRecorder-{}] Error scanning segments: {}", camera_id_, e.what());
    }
}

void ContinuousRecorder::pruneOldSegments() {
    // Only run every ~10 minutes (called every 30s from loop)
    const auto now = std::chrono::steady_clock::now();
    if (std::chrono::duration_cast<std::chrono::minutes>(now - last_prune_time_).count() < 10) {
        return;
    }
    last_prune_time_ = now;

    try {
        time_t cutoff = std::time(nullptr) - (retention_days_ * 24 * 3600);
        
        database::SegmentRepository repo;
        auto old_segments = repo.getSegments(camera_id_, 0, cutoff);

        for (const auto& seg : old_segments) {
            // Delete file from disk
            try {
                if (fs::exists(seg.filename)) {
                    fs::remove(seg.filename);
                    LOG_DEBUG("[ContinuousRecorder-{}] Deleted old segment: {}", camera_id_, seg.filename);
                }
            } catch (const std::exception& e) {
                LOG_WARN("[ContinuousRecorder-{}] Failed to delete file {}: {}", camera_id_, seg.filename, e.what());
            }
        }

        // Delete records from DB
        repo.deleteOldSegments(cutoff);

    } catch (const std::exception& e) {
        LOG_ERROR("[ContinuousRecorder-{}] Error pruning segments: {}", camera_id_, e.what());
    }
}

} // namespace recording
} // namespace vms
