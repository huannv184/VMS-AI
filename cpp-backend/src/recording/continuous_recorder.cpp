#include "recording/continuous_recorder.h"
#include "core/ffmpeg_process.h"
#include "database/segment_repository.h"
#include "utils/logger.h"

#include <sstream>
#include <filesystem>
#include <chrono>
#include <regex>
#include <fstream>
#include <QThread>

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

    // QThread gives the recorder an event loop so QProcess signals (stderrReady,
    // finished, etc.) are delivered. std::thread has no event loop — QProcess I/O
    // never fires in it, causing silent recording failures.
    recorder_qthread_ = QThread::create([this]() { recorderLoop(); });
    recorder_qthread_->start();

    LOG_INFO("[ContinuousRecorder-{}] Started → {}", camera_id_, seg_dir);
    return true;
}

void ContinuousRecorder::stop() {
    if (!running_.load()) return;

    should_stop_ = true;

    {
        std::lock_guard<std::mutex> lk(ffmpeg_mutex_);
        if (ffmpeg_) ffmpeg_->stop();
    }

    if (recorder_qthread_) {
        recorder_qthread_->quit();
        if (!recorder_qthread_->wait(8000)) {
            recorder_qthread_->terminate();
            recorder_qthread_->wait(2000);
        }
        delete recorder_qthread_;
        recorder_qthread_ = nullptr;
    }

    running_ = false;
    LOG_INFO("[ContinuousRecorder-{}] Stopped", camera_id_);
}

void ContinuousRecorder::kill() {
    should_stop_ = true;
    std::lock_guard<std::mutex> lk(ffmpeg_mutex_);
    if (ffmpeg_) ffmpeg_->kill();
}

void ContinuousRecorder::writeRawData(const uint8_t* data, int size) {
    std::lock_guard<std::mutex> lk(ffmpeg_mutex_);
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

        {
            std::lock_guard<std::mutex> lk(ffmpeg_mutex_);
            ffmpeg_ = std::make_unique<core::FFmpegProcess>();
            QObject::connect(ffmpeg_.get(), &core::FFmpegProcess::stderrReady, [this](const QByteArray& data) {
                std::string err_str = data.toStdString();
                if (!err_str.empty() && err_str.back() == '\n') err_str.pop_back();
                if (!err_str.empty() && err_str.back() == '\r') err_str.pop_back();
                LOG_WARN("[ContinuousRecorder-{}] FFmpeg stderr: {}", camera_id_, err_str);
            });
        }

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

        // Monitor FFmpeg process — scan every 10s so segments appear promptly in DB.
        int scan_tick = 0;
        while (!should_stop_.load() && ffmpeg_ && ffmpeg_->isRunning()) {
            std::this_thread::sleep_for(std::chrono::seconds(5));
            if (!should_stop_.load() && ++scan_tick >= 2) {  // every 10s
                scan_tick = 0;
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
            // A more portable way to get file time
            auto file_mod_time = fs::last_write_time(full_path);
            
            // To ensure C++17 compatibility across MSVC/GCC, we approximate time_t
            // by using the clock differences. std::filesystem::file_time_type in MSVC C++17 
            // can't just be cast to system_clock without this trick.
            auto file_mod_time_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(file_mod_time.time_since_epoch()).count();
            auto file_clock_now_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(fs::file_time_type::clock::now().time_since_epoch()).count();
            auto sys_clock_now_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
            
            // Absolute time in ns = System_Now_ns - (File_Clock_Now_ns - File_Mod_Time_ns)
            auto abs_time_ns = sys_clock_now_ns - (file_clock_now_ns - file_mod_time_ns);
            
            time_t end_time = static_cast<time_t>(abs_time_ns / 1000000000LL);

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
                if (repo.insertSegment(camera_id_, full_path, start_time, end_time, file_size, "completed")) {
                    LOG_INFO("[ContinuousRecorder-{}] Registered segment: {} ({}s)", camera_id_, filename, segment_sec_);
                } else {
                    // Segment is on disk but DB row was not written. The next
                    // retention sweep can mistake this for an orphan and
                    // delete a valid recent recording — surface the failure
                    // loudly instead of leaving the disk/DB drift silent.
                    LOG_ERROR("[ContinuousRecorder-{}] insertSegment returned false for {} "
                              "(file exists on disk but is NOT registered; "
                              "may be pruned as orphan)",
                              camera_id_, full_path);
                }
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
