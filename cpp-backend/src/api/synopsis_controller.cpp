#include "api/synopsis_controller.h"
#include "core/runtime_state.h"
#include "synopsis/SynopsisEngine.h"
#include "utils/api_utils.h"
#include "utils/background_job_runner.h"
#include <nlohmann/json.hpp>
#include "utils/logger.h"
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

std::map<std::string, SynopsisJob> g_jobs;
std::mutex g_jobs_mutex;

vms::utils::BackgroundJobRunner& synopsisJobRunner() {
    static vms::utils::BackgroundJobRunner runner("synopsis-jobs", 1, 32);
    return runner;
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
    CROW_ROUTE(app, "/api/synopsis/create")
    .methods(crow::HTTPMethod::POST, crow::HTTPMethod::OPTIONS)
    ([&auth](const crow::request& req) {
        std::string origin = req.get_header_value("Origin");
        if (req.method == crow::HTTPMethod::OPTIONS) return ApiUtils::createResponse(json::object(), 204, origin);
        if (!auth.validate(req)) return ApiUtils::createErrorResponse("Unauthorized", 401, origin);

        try {
            auto body = json::parse(req.body);
            int cameraId = body.value("cameraId", -1);
            if (cameraId == -1) return ApiUtils::createErrorResponse("Missing cameraId", 400, origin);

            std::string videoPath = body.value("videoPath", "");
            
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

            int targetDuration = body.value("targetDuration", 60);
            std::string jobId = "syn_" + std::to_string(cameraId) + "_" + std::to_string(std::time(nullptr));
            std::string outputPath = "recordings/" + jobId + ".mp4";
            {
                std::lock_guard<std::mutex> lock(g_jobs_mutex);
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

                ai::synopsis::SynopsisEngine engine;
                bool success = engine.generate(config, [](float p) {
                    LOG_INFO("Synopsis Progress: {:.0f}%", p * 100);
                });

                std::lock_guard<std::mutex> lock(g_jobs_mutex);
                auto it = g_jobs.find(jobId);
                if (it == g_jobs.end()) return;

                if (success && std::filesystem::exists(outputPath)) {
                    it->second.status = "done";
                    LOG_INFO("Synopsis created: {}", outputPath);
                } else {
                    it->second.status = "failed";
                    it->second.error = "Synopsis creation failed";
                    LOG_ERROR("Synopsis creation failed");
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
            return ApiUtils::createErrorResponse(e.what(), 500, origin);
        }
    });

    CROW_ROUTE(app, "/api/synopsis/<string>/status")
    .methods(crow::HTTPMethod::GET, crow::HTTPMethod::OPTIONS)
    ([&auth](const crow::request& req, const std::string& jobId) {
        std::string origin = req.get_header_value("Origin");
        if (req.method == crow::HTTPMethod::OPTIONS) return ApiUtils::createResponse(json::object(), 204, origin);
        if (!auth.validate(req)) return ApiUtils::createErrorResponse("Unauthorized", 401, origin);

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
