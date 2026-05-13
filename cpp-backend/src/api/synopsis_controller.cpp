#include "api/synopsis_controller.h"
#include "core/runtime_state.h"
#include "middleware/auth_middleware.h"
#include "synopsis/SynopsisEngine.h"
#include "utils/api_utils.h"
#include "utils/background_job_runner.h"
#include <nlohmann/json.hpp>
#include "utils/logger.h"
#include <algorithm>
#include <filesystem>
#include <map>
#include <mutex>
#include "database/event_repository.h"

using json = nlohmann::json;

namespace vms {
namespace api {

namespace {
struct SynopsisJob {
    std::string jobId;
    int cameraId{0};
    std::string status{"queued"};
    std::string outputPath;
    std::string error;
    std::time_t createdAt{0};
};

// BUG-SYN-LEAK-01 (audit 2026-05-08): pre-fix g_jobs grew without bound —
// every accepted /api/synopsis/create request added a permanent entry, no
// cleanup on success or failure. With even a moderate operator workload
// (10 jobs/day for 6 months) it leaks 1800+ entries plus their associated
// recordings/<jobId>.mp4 files on disk. Cap the in-memory map; rely on
// pruneOldJobsLocked() to evict at insert time.
constexpr size_t kMaxJobs = 200;
constexpr int    kMaxJobAgeSeconds = 24 * 3600;  // 24h

std::map<std::string, SynopsisJob> g_jobs;
std::mutex g_jobs_mutex;

vms::utils::BackgroundJobRunner& synopsisJobRunner() {
    static vms::utils::BackgroundJobRunner runner("synopsis-jobs", 1, 32);
    return runner;
}

// Caller must hold g_jobs_mutex. Evicts entries that are (a) completed/failed
// AND older than kMaxJobAgeSeconds, then if still over kMaxJobs evicts the
// oldest done/failed entries. Pending/processing jobs are never evicted —
// otherwise the job worker thread would lose its status anchor mid-run.
void pruneOldJobsLocked() {
    auto now = std::time(nullptr);
    for (auto it = g_jobs.begin(); it != g_jobs.end();) {
        const auto& job = it->second;
        bool finished = (job.status == "done" || job.status == "failed");
        if (finished && (now - job.createdAt) > kMaxJobAgeSeconds) {
            it = g_jobs.erase(it);
        } else {
            ++it;
        }
    }
    while (g_jobs.size() >= kMaxJobs) {
        auto oldest = g_jobs.end();
        for (auto it = g_jobs.begin(); it != g_jobs.end(); ++it) {
            if (it->second.status != "done" && it->second.status != "failed") continue;
            if (oldest == g_jobs.end() || it->second.createdAt < oldest->second.createdAt) {
                oldest = it;
            }
        }
        if (oldest == g_jobs.end()) break;  // all entries are pending/processing
        g_jobs.erase(oldest);
    }
}

// Resolve a user-supplied recording video path, rejecting anything that
// escapes the "recordings/" root (BUG-SYN-PATH-01). Empty string → empty
// (caller will fall through to DB lookup). Returns empty string on rejection
// after logging the attempt.
std::string sanitizeVideoPath(const std::string& raw) {
    if (raw.empty()) return "";
    if (raw.find("..") != std::string::npos) {
        LOG_WARN("Synopsis: rejected videoPath with traversal token: {}", raw);
        return "";
    }
    namespace fs = std::filesystem;
    std::error_code ec;
    fs::path root = fs::weakly_canonical(fs::path("recordings"), ec);
    if (ec) return "";
    fs::path target = fs::weakly_canonical(fs::path(raw), ec);
    if (ec) return "";
    auto root_str   = root.string();
    auto target_str = target.string();
    if (target_str.rfind(root_str, 0) != 0) {
        LOG_WARN("Synopsis: rejected videoPath outside recordings root: {}", raw);
        return "";
    }
    return target_str;
}

void failJob(const std::string& jobId, const std::string& error) {
    std::lock_guard<std::mutex> lock(g_jobs_mutex);
    auto it = g_jobs.find(jobId);
    if (it == g_jobs.end()) {
        return;
    }

    it->second.status = "failed";
    it->second.error = error;
}
} // namespace

void SynopsisController::registerRoutes(vms::server::VmsApp& app, vms::middleware::AuthMiddleware& auth) {
    (void)auth;  // legacy parameter — auth now goes via app.get_context

    CROW_ROUTE(app, "/api/synopsis/create")
    .methods(crow::HTTPMethod::POST, crow::HTTPMethod::OPTIONS)
    ([&app](const crow::request& req) {
        std::string origin = ApiUtils::resolveCorsOrigin(req);
        if (req.method == crow::HTTPMethod::OPTIONS) return ApiUtils::createResponse(json::object(), 204, origin);

        // BUG-SYN-RBAC-01 (audit 2026-05-08): pre-fix gate was just
        // `auth.validate(req)` — every logged-in user (including viewer)
        // could spawn synopsis jobs that CPU-saturate a worker for minutes
        // and read recording paths off the DB. Same shape as SEC-005
        // (face/reid/videowall "is logged in" gates from 2026-05-02).
        // Synopsis is an analytics feature → ANALYTICS_READ matches the
        // attendance/counter/reid surface.
        auto& ctx = app.get_context<vms::middleware::AuthMiddleware>(req);
        if (auto err = ApiUtils::requirePermission(ctx, Permission::ANALYTICS_READ, origin)) return std::move(*err);

        try {
            auto body = json::parse(req.body);

            // BUG-SYN-CAMID-01: pre-fix accepted any int. Reject negatives /
            // pathological values so a bad lookup can't probe for events
            // across the entire camera id space.
            int cameraId = body.value("cameraId", -1);
            if (cameraId < 0 || cameraId > 1000000) {
                return ApiUtils::createErrorResponse("Missing or invalid cameraId", 400, origin);
            }

            // BUG-SYN-PATH-01 (audit 2026-05-08): pre-fix the user-supplied
            // videoPath was passed straight to cv::VideoCapture. Anyone who
            // could reach the API could probe arbitrary paths on disk via
            // success/failure timing; with the right combination of args the
            // engine could even render content from a non-recording video
            // file into the publicly served recordings/<jobId>.mp4 output.
            // sanitizeVideoPath() canonicalises and rejects anything outside
            // recordings/.
            std::string raw_path = body.value("videoPath", "");
            std::string videoPath = sanitizeVideoPath(raw_path);
            if (!raw_path.empty() && videoPath.empty()) {
                return ApiUtils::createErrorResponse("videoPath outside recordings root", 400, origin);
            }

            // DYNAMIC LOOKUP FIX: If videoPath is empty, find the best match in DB
            if (videoPath.empty()) {
                std::time_t startTime = 0;
                if (body.contains("startTime")) {
                    // Convert ms to seconds if necessary (frontend might send ms)
                    long long st = body["startTime"].get<long long>();
                    if (st > 1000000000000LL) st /= 1000; // ms to sec
                    startTime = static_cast<std::time_t>(st);
                } else {
                    startTime = std::time(nullptr) - 3600; // default 1h ago
                }

                database::EventRepository repo;
                auto events = repo.getEvents(cameraId, 100, 0);
                
                std::string bestPath = "";
                long long minDiff = -1;

                for (const auto& ev : events) {
                    if (ev.video_path.empty()) continue;
                    
                    long long diff = std::abs(static_cast<long long>(ev.timestamp) - static_cast<long long>(startTime));
                    if (minDiff == -1 || diff < minDiff) {
                        minDiff = diff;
                        bestPath = ev.video_path;
                    }
                }

                if (bestPath.empty()) {
                    return ApiUtils::createErrorResponse("No recordings found for this camera/time", 404, origin);
                }
                videoPath = bestPath;
                LOG_INFO("Synopsis: Resolved cameraId {}/time {} to {}", cameraId, startTime, videoPath);
            }

            // BUG-SYN-DURATION-01 (audit 2026-05-08): pre-fix this was
            // unbounded. A client could request a 24-hour synopsis (≈2.6M
            // output frames at 30 fps) → render loop is O(output_frames ×
            // tubes × frames_per_tube) and would saturate the synopsis
            // worker for hours. Clamp to a sensible UI range.
            int targetDuration = body.value("targetDuration", 60);
            if (targetDuration < 5)    targetDuration = 5;
            if (targetDuration > 600)  targetDuration = 600;

            std::string jobId = "syn_" + std::to_string(cameraId) + "_" + std::to_string(std::time(nullptr));
            std::string outputPath = "recordings/" + jobId + ".mp4";
            {
                std::lock_guard<std::mutex> lock(g_jobs_mutex);
                pruneOldJobsLocked();
                if (g_jobs.size() >= kMaxJobs) {
                    // All slots are pending/processing — refuse rather than
                    // silently overwriting a running job's status.
                    return ApiUtils::createErrorResponse(
                        "Synopsis job queue full; retry shortly", 429, origin);
                }
                g_jobs[jobId] = SynopsisJob{jobId, cameraId, "queued", outputPath, "", std::time(nullptr)};
            }

            if (vms::core::shutting_down.load(std::memory_order_acquire) ||
                !synopsisJobRunner().submit([videoPath, targetDuration, cameraId, jobId, outputPath]() {
                ai::synopsis::SynopsisConfig config;
                config.inputVideoPath = videoPath;
                std::filesystem::create_directories("recordings");
                config.outputVideoPath = outputPath;
                config.targetDurationSec = targetDuration;

                {
                    std::lock_guard<std::mutex> lock(g_jobs_mutex);
                    auto it = g_jobs.find(jobId);
                    if (it != g_jobs.end()) it->second.status = "processing";
                }

                bool success = false;
                std::string failure_msg = "Synopsis creation failed";
                try {
                    ai::synopsis::SynopsisEngine engine;
                    success = engine.generate(config, [](float p) {
                        LOG_INFO("Synopsis Progress: {:.0f}%", p * 100);
                    });
                } catch (const std::exception& e) {
                    // OpenCV throws on size-mismatched copyTo, codec failure,
                    // disk-full on the writer, etc. Without this catch, the
                    // background-job worker thread would log "uncaught
                    // exception" and the job would be stuck forever in
                    // "processing." Catch + record so the REST status path
                    // surfaces the real reason.
                    failure_msg = std::string("engine threw: ") + e.what();
                    LOG_ERROR("Synopsis engine exception: {}", e.what());
                }

                std::lock_guard<std::mutex> lock(g_jobs_mutex);
                auto it = g_jobs.find(jobId);
                if (it == g_jobs.end()) return;

                if (success && std::filesystem::exists(outputPath)) {
                    it->second.status = "done";
                    LOG_INFO("Synopsis created: {}", outputPath);
                } else {
                    it->second.status = "failed";
                    it->second.error = failure_msg;
                    LOG_ERROR("Synopsis creation failed: {}", failure_msg);
                }
            })) {
                failJob(jobId, "Synopsis queue unavailable");
                return ApiUtils::createErrorResponse("Synopsis queue unavailable", 503, origin);
            }

            return ApiUtils::createResponse({
                {"jobId", jobId}
            }, 200, origin);
        } catch (const std::exception& e) {
            LOG_ERROR("Error in /api/synopsis/create: {}", e.what());
            return ApiUtils::createSafeError(e, 500, origin);
        }
    });

    CROW_ROUTE(app, "/api/synopsis/<string>/status")
    .methods(crow::HTTPMethod::GET, crow::HTTPMethod::OPTIONS)
    ([&app](const crow::request& req, const std::string& jobId) {
        std::string origin = ApiUtils::resolveCorsOrigin(req);
        if (req.method == crow::HTTPMethod::OPTIONS) return ApiUtils::createResponse(json::object(), 204, origin);

        auto& ctx = app.get_context<vms::middleware::AuthMiddleware>(req);
        if (auto err = ApiUtils::requirePermission(ctx, Permission::ANALYTICS_READ, origin)) return std::move(*err);

        std::lock_guard<std::mutex> lock(g_jobs_mutex);
        auto it = g_jobs.find(jobId);
        if (it == g_jobs.end()) return ApiUtils::createErrorResponse("Job not found", 404, origin);

        json resp = {
            {"jobId", it->second.jobId},
            {"cameraId", it->second.cameraId},
            {"status", it->second.status}
        };
        if (it->second.status == "done") {
            std::filesystem::path p(it->second.outputPath);
            resp["videoUrl"] = "/recordings/" + p.filename().string();
        } else if (it->second.status == "failed") {
            resp["error"] = it->second.error.empty() ? "Unknown error" : it->second.error;
        }

        return ApiUtils::createResponse(resp, 200, origin);
    });
}

void SynopsisController::shutdown() {
    synopsisJobRunner().shutdown();
}

} // namespace api
} // namespace vms
