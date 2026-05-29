#include "api/export_controller.h"
#include "core/ffmpeg_process.h"
#include "core/runtime_state.h"
#include "database/audit_repository.h"
#include "middleware/auth_middleware.h"
#include "utils/api_utils.h"
#include "utils/background_job_runner.h"
#include "utils/logger.h"
#include <nlohmann/json.hpp>
#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <mutex>
#include <map>
#include <ctime>
#include <random>

using json = nlohmann::json;

namespace vms {
namespace api {

namespace {

// Export jobs are bounded in memory and pruned after completion so stale job
// metadata and output files do not accumulate indefinitely.
constexpr size_t kMaxExportJobs = 200;
constexpr int    kMaxExportJobAgeSeconds = 24 * 3600;  // 24h

static std::map<std::string, ExportJob> exportJobs;
static std::mutex jobsMutex;

vms::utils::BackgroundJobRunner& exportJobRunner() {
    static vms::utils::BackgroundJobRunner runner("export-jobs", 2, 64);
    return runner;
}

// Caller must hold jobsMutex. Mirrors pruneOldJobsLocked() in synopsis_controller.
// Pending/processing jobs are NEVER evicted because the worker thread reads
// exportJobs[jobId] to write status updates.
void pruneOldExportJobsLocked() {
    auto now = std::time(nullptr);
    for (auto it = exportJobs.begin(); it != exportJobs.end();) {
        const auto& job = it->second;
        bool finished = (job.status == "done" || job.status == "failed");
        // ExportJob has no createdAt yet; we approximate with the absence of
        // an in-flight worker. Skip pending/processing aggressively. For
        // finished jobs, evict ones whose output file no longer exists or
        // is older than the cap.
        if (finished) {
            std::error_code ec;
            auto last_write = std::filesystem::last_write_time(job.outputPath, ec);
            if (ec) {
                // Output gone (manual cleanup, retention sweep) → safe to drop.
                it = exportJobs.erase(it);
                continue;
            }
            auto sctp = std::chrono::time_point_cast<std::chrono::system_clock::duration>(
                last_write - std::filesystem::file_time_type::clock::now() +
                std::chrono::system_clock::now());
            auto file_time = std::chrono::system_clock::to_time_t(sctp);
            if ((now - file_time) > kMaxExportJobAgeSeconds) {
                it = exportJobs.erase(it);
                continue;
            }
        }
        ++it;
    }
    // If still over the cap, drop oldest finished jobs by output file mtime.
    while (exportJobs.size() >= kMaxExportJobs) {
        auto oldest = exportJobs.end();
        std::filesystem::file_time_type oldest_time{};
        for (auto it = exportJobs.begin(); it != exportJobs.end(); ++it) {
            if (it->second.status != "done" && it->second.status != "failed") continue;
            std::error_code ec;
            auto t = std::filesystem::last_write_time(it->second.outputPath, ec);
            if (ec) { oldest = it; break; }
            if (oldest == exportJobs.end() || t < oldest_time) {
                oldest = it; oldest_time = t;
            }
        }
        if (oldest == exportJobs.end()) break;  // all entries are pending/processing
        exportJobs.erase(oldest);
    }
}

void markJobFailed(const std::string& jobId, const std::string& message) {
    std::lock_guard<std::mutex> lock(jobsMutex);
    auto it = exportJobs.find(jobId);
    if (it == exportJobs.end()) {
        return;
    }

    it->second.status = "failed";
    it->second.errorMessage = message;
}
} // namespace

std::string ExportController::generateJobId() {
    static std::random_device rd;
    static std::mt19937 gen(rd());
    static std::uniform_int_distribution<> dis(0, 15);
    
    const char* hex_chars = "0123456789abcdef";
    std::string uuid;
    for (int i = 0; i < 32; i++) {
        uuid += hex_chars[dis(gen)];
    }
    return "export_" + uuid;
}

std::string ExportController::buildFFmpegCommand(
    const std::string& inputPath,
    const std::string& outputPath,
    const std::vector<MaskRegion>& masks
) {
    std::ostringstream cmd;
    cmd << "ffmpeg -y -i \"" << inputPath << "\"";
    
    if (!masks.empty()) {
        cmd << " -filter_complex \"";
        
        // Build filter chain for each mask
        for (size_t i = 0; i < masks.size(); i++) {
            const auto& mask = masks[i];
            
            if (mask.type == "blur") {
                // Blur filter: crop region, apply boxblur, overlay back
                cmd << "[0:v]crop=" << mask.width << ":" << mask.height 
                    << ":" << mask.x << ":" << mask.y 
                    << ",boxblur=" << mask.strength << "[blur" << i << "];";
            } else if (mask.type == "pixelate") {
                // Pixelate: crop, scale down and up
                int pixelSize = std::max(1, mask.strength);
                cmd << "[0:v]crop=" << mask.width << ":" << mask.height 
                    << ":" << mask.x << ":" << mask.y
                    << ",scale=iw/" << pixelSize << ":ih/" << pixelSize
                    << ":flags=neighbor,scale=" << mask.width << ":" << mask.height
                    << ":flags=neighbor[pixelate" << i << "];";
            }
        }
        
        // Overlay all masks back onto original video
        cmd << "[0:v]";
        for (size_t i = 0; i < masks.size(); i++) {
            const auto& mask = masks[i];
            std::string maskLabel = (mask.type == "blur") ? "blur" : "pixelate";
            cmd << "[" << maskLabel << i << "]";
            cmd << "overlay=" << mask.x << ":" << mask.y;
            if (i < masks.size() - 1) {
                cmd << "[tmp" << i << "];[tmp" << i << "]";
            }
        }
        cmd << "\" -c:a copy";
    } else {
        cmd << " -c copy";  // No masks, just remux
    }
    
    cmd << " \"" << outputPath << "\"";
    return cmd.str();
}

void ExportController::processExportJob(const ExportJob& job) {
    std::string inputPath = "recordings/event_" + job.recordingId + ".ts";
    std::string outputPath = "exports/" + job.jobId + "." + job.format;
    
    // Create exports directory if not exists
    std::filesystem::create_directories("exports");
    
    // Update job status
    {
        std::lock_guard<std::mutex> lock(jobsMutex);
        if (exportJobs.find(job.jobId) != exportJobs.end()) {
            exportJobs[job.jobId].status = "processing";
        }
    }
    
    try {
        // Run FFmpeg through the managed wrapper so shutdown can terminate it.
        std::string cmd = buildFFmpegCommand(inputPath, outputPath, job.masks);
        LOG_INFO("Executing FFmpeg: {}", cmd);

        vms::core::FFmpegProcess process;
        if (!process.start(cmd)) {
            markJobFailed(job.jobId, "FFmpeg failed to start");
            LOG_ERROR("Export job {} failed to start FFmpeg", job.jobId);
            return;
        }

        int result = process.wait(-1);

        std::lock_guard<std::mutex> lock(jobsMutex);
        if (exportJobs.find(job.jobId) != exportJobs.end()) {
            if (result == 0 && std::filesystem::exists(outputPath)) {
                exportJobs[job.jobId].status = "done";
                exportJobs[job.jobId].outputPath = outputPath;
                LOG_INFO("Export job {} completed successfully", job.jobId);
            } else {
                exportJobs[job.jobId].status = "failed";
                exportJobs[job.jobId].errorMessage = "FFmpeg processing failed";
                LOG_ERROR("Export job {} failed with code {}", job.jobId, result);
            }
        }
    } catch (const std::exception& e) {
        // P0 #2 fix: errorMessage is exposed via /api/export/{id}/status
        // (line ~341) — never put raw e.what() in there. Log full stack
        // server-side, store a generic phrase that's safe to ship to the
        // client. Specific FFmpeg errors are already surfaced from the
        // earlier `errorMessage = "FFmpeg processing failed"` site.
        LOG_ERROR("Export job {} exception: {}", job.jobId, e.what());
        std::lock_guard<std::mutex> lock(jobsMutex);
        if (exportJobs.find(job.jobId) != exportJobs.end()) {
            exportJobs[job.jobId].status = "failed";
            exportJobs[job.jobId].errorMessage = "Export failed (see server log)";
        }
    }
}

void ExportController::registerRoutes(vms::server::VmsApp& app, vms::middleware::AuthMiddleware& auth) {
    (void)auth;  // legacy parameter — auth now goes via app.get_context
    LOG_INFO("Registering export routes...");

    // Export creation/download is permission-gated on RECORDING_READ and
    // remains auditable because jobs can consume CPU and expose recording data.

    // POST /api/export/create - Create export job
    CROW_ROUTE(app, "/api/export/create")
    .methods(crow::HTTPMethod::Post)
    ([&app](const crow::request& req) {
        std::string origin = ApiUtils::resolveCorsOrigin(req);
        auto& ctx = app.get_context<vms::middleware::AuthMiddleware>(req);
        if (auto err = ApiUtils::requirePermission(ctx, Permission::RECORDING_READ, origin)) return std::move(*err);

        try {
            auto body = json::parse(req.body);

            if (!body.contains("recordingId")) {
                return ApiUtils::createErrorResponse("Missing recordingId", 400);
            }

            ExportJob job;
            job.jobId = generateJobId();

            std::string recId = body["recordingId"].get<std::string>();
            if (!std::all_of(recId.begin(), recId.end(), [](unsigned char c) { return std::isalnum(c) || c == '-' || c == '_'; })) {
                return ApiUtils::createErrorResponse("Invalid recordingId format", 400);
            }
            job.recordingId = recId;
            job.format = body.value("format", "mp4");

            // SECURITY FIX: Prevent Command Injection by strict whitelisting.
            // Pre-fix silently normalised invalid formats to "mp4". That hid
            // operator typos. Now: reject explicitly with 400.
            if (job.format != "mp4" && job.format != "avi" && job.format != "mkv") {
                return ApiUtils::createErrorResponse("Invalid format (allowed: mp4, avi, mkv)", 400);
            }

            job.status = "queued";

            // Parse masks
            if (body.contains("masks") && body["masks"].is_array()) {
                for (const auto& maskJson : body["masks"]) {
                    MaskRegion mask;
                    mask.x = maskJson.value("x", 0);
                    mask.y = maskJson.value("y", 0);
                    mask.width = maskJson.value("width", 100);
                    mask.height = maskJson.value("height", 100);
                    mask.type = maskJson.value("type", "blur");
                    mask.strength = maskJson.value("strength", 10);
                    job.masks.push_back(mask);
                }
            }

            // Store job + cap eviction
            {
                std::lock_guard<std::mutex> lock(jobsMutex);
                pruneOldExportJobsLocked();
                if (exportJobs.size() >= kMaxExportJobs) {
                    return ApiUtils::createErrorResponse(
                        "Export job queue full; retry shortly", 429);
                }
                exportJobs[job.jobId] = job;
            }

            if (vms::core::shutting_down.load(std::memory_order_acquire) ||
                !exportJobRunner().submit([job]() { processExportJob(job); })) {
                markJobFailed(job.jobId, "Export queue unavailable");
                return ApiUtils::createErrorResponse("Export queue unavailable", 503);
            }

            // Audit-log: exports are evidence/PII extraction; record who
            // initiated each one. Helps forensics trace where a leaked clip
            // originated.
            if (ctx.user) {
                vms::database::AuditRepository audit;
                audit.insertLog(ctx.user->id, "CREATE_EXPORT",
                                "Export job " + job.jobId + " for recording " + job.recordingId);
            }

            json response = {
                {"jobId", job.jobId},
                {"status", "queued"}
            };

            return ApiUtils::createResponse(response, 201);
        } catch (const json::exception& e) {
            return ApiUtils::createErrorResponse(std::string("Invalid JSON: ") + e.what(), 400);
        } catch (const std::exception& e) {
            return ApiUtils::createSafeError(e);
        }
    });

    // GET /api/export/:jobId/status - Get export job status
    CROW_ROUTE(app, "/api/export/<string>/status")
    .methods(crow::HTTPMethod::Get)
    ([&app](const crow::request& req, std::string jobId) {
        std::string origin = ApiUtils::resolveCorsOrigin(req);
        auto& ctx = app.get_context<vms::middleware::AuthMiddleware>(req);
        if (auto err = ApiUtils::requirePermission(ctx, Permission::RECORDING_READ, origin)) return std::move(*err);
        
        std::lock_guard<std::mutex> lock(jobsMutex);
        auto it = exportJobs.find(jobId);
        
        if (it == exportJobs.end()) {
            return ApiUtils::createErrorResponse("Export job not found", 404);
        }
        
        json response = {
            {"jobId", it->second.jobId},
            {"status", it->second.status}
        };
        
        if (it->second.status == "done") {
            response["downloadUrl"] = "/api/export/" + jobId + "/download";
        } else if (it->second.status == "failed") {
            response["error"] = it->second.errorMessage;
        }
        
        return ApiUtils::createResponse(response);
    });
    
    // GET /api/export/:jobId/download - Download exported video
    CROW_ROUTE(app, "/api/export/<string>/download")
    .methods(crow::HTTPMethod::Get)
    ([&app](const crow::request& req, std::string jobId) {
        std::string origin = ApiUtils::resolveCorsOrigin(req);
        auto& ctx = app.get_context<vms::middleware::AuthMiddleware>(req);
        if (auto err = ApiUtils::requirePermission(ctx, Permission::RECORDING_READ, origin)) return std::move(*err);

        std::string outputPath;
        {
            std::lock_guard<std::mutex> lock(jobsMutex);
            auto it = exportJobs.find(jobId);

            if (it == exportJobs.end()) {
                return ApiUtils::createErrorResponse("Export job not found", 404);
            }

            if (it->second.status != "done") {
                return ApiUtils::createErrorResponse("Export not ready", 400);
            }
            outputPath = it->second.outputPath;
        }

        if (!std::filesystem::exists(outputPath)) {
            return ApiUtils::createErrorResponse("Export file not found", 404);
        }

        // Audit-log download — pairs with the CREATE_EXPORT entry so
        // forensics can see "user X created this export and user Y
        // downloaded it on date Z."
        if (ctx.user) {
            vms::database::AuditRepository audit;
            audit.insertLog(ctx.user->id, "DOWNLOAD_EXPORT", "Export " + jobId);
        }

        // Use static file info to prevent OOM
        crow::response res;
        res.code = 200;
        res.set_static_file_info(outputPath);
        res.set_header("Content-Type", "video/mp4");
        res.set_header("Content-Disposition", "attachment; filename=\"" + jobId + ".mp4\"");
        ApiUtils::applyCors(res, origin);

        return res;
    });
    
    LOG_INFO("Export routes registered");
}

void ExportController::shutdown() {
    exportJobRunner().shutdown();
}

} // namespace api
} // namespace vms
