#include "core/snapshot_manager.h"
#include <QByteArray>
#include "core/camera_pipeline_manager.h"
#include "utils/logger.h"
#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <ctime>
#include <nlohmann/json.hpp>

using json = nlohmann::json;

namespace vms {
namespace core {

SnapshotManager& SnapshotManager::getInstance() {
    static SnapshotManager instance;
    return instance;
}

std::string SnapshotManager::takeSnapshot(int camera_id) {
    auto frame_opt = CameraPipelineManager::getInstance().getLatestFrame(camera_id);
    if (!frame_opt) {
        LOG_WARN("[Snapshot] No cached frame for camera {}", camera_id);
        return "";
    }
    const auto& data = frame_opt.value();
    return saveSnapshot(camera_id,
                        reinterpret_cast<const unsigned char*>(data.data()),
                        data.size());
}

std::string SnapshotManager::saveSnapshot(int camera_id, const unsigned char* jpeg_data, size_t size, long long timestamp) {
    std::lock_guard<std::mutex> lock(mutex_);

    // Use millisecond-resolution timestamp + monotonic counter so two snapshots
    // taken in the same second on the same camera don't collide on id/filename.
    // The previous "snap_<cam>_<sec>" format was the BUG-DB-01 pattern: same id
    // → index lookups + delete only see one entry.
    long long ts_sec = (timestamp != 0)
        ? timestamp
        : static_cast<long long>(std::time(nullptr));
    long long ts_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    static std::atomic<uint64_t> snap_counter{0};
    uint64_t seq = snap_counter.fetch_add(1, std::memory_order_relaxed);

    std::filesystem::create_directories(storage_path_);
    std::string filename = "snap_" + std::to_string(camera_id) + "_"
                         + std::to_string(ts_ms) + "_"
                         + std::to_string(seq) + ".jpg";
    std::string full_path = storage_path_ + filename;

    std::ofstream out(full_path, std::ios::out | std::ios::binary | std::ios::trunc);
    if (!out.is_open()) {
        LOG_ERROR("Failed to save snapshot file: {}", full_path);
        return "";
    }

    out.write(reinterpret_cast<const char*>(jpeg_data), size);
    out.close();

    Snapshot snap;
    snap.id = "snap_" + std::to_string(camera_id) + "_"
            + std::to_string(ts_ms) + "_" + std::to_string(seq);
    snap.camera_id = camera_id;
    snap.trigger = "manual";
    snap.filepath = filename;
    snap.timestamp = ts_sec;
    snap.timestamp_str = std::to_string(ts_sec);
    snap.metadata_json = "{}";

    updateIndex(snap);

    LOG_INFO("Snapshot saved: {} for camera {}", full_path, camera_id);
    return filename;
}

std::vector<Snapshot> SnapshotManager::getRecentSnapshots(int camera_id, int limit) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (snapshots_.empty()) {
        loadIndex();
    }

    std::vector<Snapshot> result;
    for (auto it = snapshots_.rbegin(); it != snapshots_.rend(); ++it) {
        if (camera_id != -1 && it->camera_id != camera_id) continue;
        result.push_back(*it);
        if ((int)result.size() >= limit) break;
    }
    return result;
}

bool SnapshotManager::deleteSnapshot(const std::string& snapshot_id) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (snapshots_.empty()) loadIndex();

    // Prevent basic path traversal from invalid IDs
    if (snapshot_id.find("..") != std::string::npos || snapshot_id.find('/') != std::string::npos || snapshot_id.find('\\') != std::string::npos) {
        LOG_WARN("Invalid snapshot ID format (path traversal attempt?): {}", snapshot_id);
        return false;
    }

    auto it = std::find_if(snapshots_.begin(), snapshots_.end(), [&](const Snapshot& s) {
        return s.id == snapshot_id;
    });

    if (it != snapshots_.end()) {
        std::filesystem::remove(std::filesystem::path(storage_path_) / it->filepath);
        snapshots_.erase(it);

        // Authoritative source after a delete: rewrite snapshots.jsonl from in-memory.
        // Cheaper than rewriting per-save (delete is rare, save is hot path).
        std::ofstream out(storage_path_ + "snapshots.jsonl", std::ios::out | std::ios::trunc);
        if (out.is_open()) {
            for (const auto& s : snapshots_) {
                json line = {
                    {"id", s.id},
                    {"camera_id", s.camera_id},
                    {"trigger", s.trigger},
                    {"filepath", s.filepath},
                    {"timestamp", s.timestamp_str},
                    {"metadata", json::parse(s.metadata_json)}
                };
                out << line.dump() << '\n';
            }
        }
        // Drop legacy index.json once we've migrated content into the .jsonl
        std::error_code ec;
        std::filesystem::remove(storage_path_ + "index.json", ec);
        return true;
    }
    return false;
}

std::string SnapshotManager::generateFilename(int camera_id, long long timestamp) {
    return "snap_" + std::to_string(camera_id) + "_" + std::to_string(timestamp) + ".jpg";
}

void SnapshotManager::updateIndex(const Snapshot& snap) {
    snapshots_.push_back(snap);

    // BUG-18 FIX: Was O(N²) — full index.json rewrite on every snapshot.
    // After 10k snapshots that's 50M writes total just for the index file.
    // Switched to line-delimited JSON (snapshots.jsonl): one append per save,
    // O(1) per snapshot. loadIndex() reads both formats for back-compat with
    // existing index.json files; deleteSnapshot() rewrites the .jsonl as the
    // authoritative source.
    json line = {
        {"id", snap.id},
        {"camera_id", snap.camera_id},
        {"trigger", snap.trigger},
        {"filepath", snap.filepath},
        {"timestamp", snap.timestamp_str},
        {"metadata", json::parse(snap.metadata_json)}
    };

    std::ofstream out(storage_path_ + "snapshots.jsonl", std::ios::out | std::ios::app);
    if (out.is_open()) {
        out << line.dump() << '\n';
    }
}

void SnapshotManager::loadIndex() {
    snapshots_.clear();

    auto load_item = [&](const json& item) {
        Snapshot s;
        s.id = item.value("id", "");
        s.camera_id = item.value("camera_id", -1);
        s.trigger = item.value("trigger", "");
        s.filepath = item.value("filepath", "");
        s.timestamp_str = item.value("timestamp", "0");
        try { s.timestamp = std::stoll(s.timestamp_str); } catch (...) { s.timestamp = 0; }
        s.metadata_json = item.value("metadata", json::object()).dump();
        snapshots_.push_back(s);
    };

    // Legacy index.json (single JSON array — kept for back-compat with old installs)
    std::string legacy_path = storage_path_ + "index.json";
    if (std::filesystem::exists(legacy_path)) {
        try {
            std::ifstream in(legacy_path);
            json index;
            in >> index;
            for (const auto& item : index) load_item(item);
        } catch (...) {
            LOG_ERROR("Failed to load legacy snapshot index.json");
        }
    }

    // Append-only snapshots.jsonl — current format
    std::string jsonl_path = storage_path_ + "snapshots.jsonl";
    if (std::filesystem::exists(jsonl_path)) {
        try {
            std::ifstream in(jsonl_path);
            std::string line;
            while (std::getline(in, line)) {
                if (line.empty()) continue;
                try { load_item(json::parse(line)); } catch (...) {}
            }
        } catch (...) {
            LOG_ERROR("Failed to load snapshots.jsonl");
        }
    }
}

} // namespace core
} // namespace vms
