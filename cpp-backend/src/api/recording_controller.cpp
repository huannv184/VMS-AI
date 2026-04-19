#include "api/recording_controller.h"
#include "utils/api_utils.h"
#include "utils/camera_name_cache.h"
#include <filesystem>
#include <fstream>
#include <regex>
#include <algorithm>
#include <cctype>
#include <sstream>
#include <iomanip>
#include "database/event_repository.h"
#include "database/segment_repository.h"
#include "utils/storage_manager.h"
#include "utils/config.h"
#include "utils/media_signer.h"

using json = nlohmann::json;

// Validate recording ID: only alphanumeric + hyphens allowed (prevents path traversal)
static bool isValidRecordingId(const std::string& id) {
    if (id.empty() || id.size() > 64) return false;
    for (char c : id) {
        if (!std::isalnum(static_cast<unsigned char>(c)) && c != '-' && c != '_') return false;
    }
    return true;
}

namespace vms {
namespace api {

void RecordingController::registerRoutes(vms::server::VmsApp& app) {
    // GET /api/recordings - List all recordings (events with video files)
    CROW_ROUTE(app, "/api/recordings")
    .methods(crow::HTTPMethod::Get, crow::HTTPMethod::Options)
    ([](const crow::request& req) {
        std::string origin = ApiUtils::resolveCorsOrigin(req);
        if (req.method == crow::HTTPMethod::Options) {
            return ApiUtils::createResponse(json::object(), 204, origin);
        }

        try {
            const auto& media_cfg = vms::Config::getInstance().getMediaSigningConfig();
            int filter_camera_id = -1;
            auto camera_id_str = req.url_params.get("camera_id");
            int limit = 50;
            int offset = 0;
            auto limit_str = req.url_params.get("limit");
            auto offset_str = req.url_params.get("offset");

            try {
                if (camera_id_str) filter_camera_id = std::stoi(camera_id_str);
                if (limit_str) limit = std::stoi(limit_str);
                if (limit < 1 || limit > 1000) limit = 50;
                if (offset_str) offset = std::stoi(offset_str);
                if (offset < 0) offset = 0;
            } catch (const std::exception&) {
                return ApiUtils::createErrorResponse("Invalid query parameter: camera_id, limit, and offset must be integers", 400, origin);
            }

            // Use the new recording-specific query (only events with video_path)
            vms::database::EventRepository event_repo;
            auto events = event_repo.getRecordingEvents(filter_camera_id, limit, offset);
            int total_count = event_repo.getRecordingCount(filter_camera_id);

            auto& name_cache = vms::utils::CameraNameCache::getInstance();
            auto cam_names = name_cache.getNames();

            json recordings = json::array();
            for (const auto& event : events) {
                // Verify video file existence (MinIO first, then local)
                std::string object_key = event.video_path;
                bool exists_on_minio = vms::utils::StorageManager::getInstance().exists(object_key);
                bool exists_locally = std::filesystem::exists("recordings/" + object_key) || std::filesystem::exists(object_key);

                if (!exists_on_minio && !exists_locally) {
                    continue; // Skip if no video found anywhere
                }

                json recording;
                recording["id"] = event.id;
                recording["filename"] = std::filesystem::path(object_key).filename().string();
                
                // For playback, we can either proxy or redirect
                // Here we'll use our proxy endpoint
                vms::utils::MediaAccessScope video_scope;
                video_scope.scope = "recording_video";
                video_scope.camera_id = event.camera_id;
                video_scope.resource_id = event.id;
                recording["videoUrl"] = vms::utils::presignPath(
                    "/api/recordings/" + event.id + "/video",
                    media_cfg.recording_video_ttl_seconds,
                    video_scope
                );
                recording["cameraId"] = event.camera_id;
                
                if (cam_names.find(event.camera_id) != cam_names.end()) {
                    recording["cameraName"] = cam_names[event.camera_id];
                } else {
                    recording["cameraName"] = "Camera " + std::to_string(event.camera_id);
                }
                
                recording["eventType"] = event.event_type;
                recording["description"] = event.description;
                recording["location"] = recording["cameraName"];
                recording["timestamp"] = (uint64_t)event.timestamp * 1000;
                recording["startTime"] = (uint64_t)event.timestamp * 1000;
                recording["duration"] = event.duration > 0 ? event.duration : 20;
                recording["storage"] = exists_on_minio ? "minio" : "local";
                
                // Size estimate
                recording["size"] = exists_locally ? (uint64_t)std::filesystem::file_size(object_key) : 0;
                
                // Thumbnail
                if (!event.snapshot_path.empty()) {
                    vms::utils::MediaAccessScope thumb_scope;
                    thumb_scope.scope = "snapshot";
                    thumb_scope.camera_id = event.camera_id;
                    thumb_scope.resource_id = event.id;
                    recording["thumbnail"] = vms::utils::presignPath(
                        "/api/snapshots/files/" + std::filesystem::path(event.snapshot_path).filename().string(),
                        media_cfg.snapshot_ttl_seconds,
                        thumb_scope
                    );
                }

                recordings.push_back(recording);
            }

            // Return with total count for pagination
            json response;
            response["recordings"] = recordings;
            response["total"] = total_count;
            response["limit"] = limit;
            response["offset"] = offset;

            return ApiUtils::createResponse(response, 200, origin);
        } catch (const std::exception& e) {
            return ApiUtils::createErrorResponse(e.what(), 500, origin);
        }
    });

    // GET /api/recordings/:id/video - Get video file (proxied from MinIO or local)
    CROW_ROUTE(app, "/api/recordings/<string>/video")
    .methods(crow::HTTPMethod::Get, crow::HTTPMethod::Options)
    ([](const crow::request& req, std::string id) {
        std::string origin = req.get_header_value("Origin");
        if (req.method == crow::HTTPMethod::Options) {
            return ApiUtils::createResponse(json::object(), 204, origin);
        }

        if (!isValidRecordingId(id)) {
            return ApiUtils::createErrorResponse("Invalid recording ID", 400, origin);
        }

        try {
            vms::database::EventRepository event_repo;
            auto event_opt = event_repo.getEventById(id);
            if (!event_opt) {
                return ApiUtils::createErrorResponse("Recording not found", 404, origin);
            }

            std::string object_key = event_opt->video_path;
            std::vector<char> video_data;
            std::string content_type = "video/mp4";

            // 1. Try MinIO
            if (vms::utils::StorageManager::getInstance().exists(object_key)) {
                video_data = vms::utils::StorageManager::getInstance().getObject(object_key);
            } 
            // 2. Try Local
            else {
                std::string local_path = "recordings/" + object_key;
                if (!std::filesystem::exists(local_path)) local_path = object_key;
                
                if (std::filesystem::exists(local_path)) {
                    std::ifstream file(local_path, std::ios::binary);
                    video_data = std::vector<char>((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
                }
            }

            if (video_data.empty()) {
                return ApiUtils::createErrorResponse("Recording data source missing", 404, origin);
            }

            crow::response res;
            res.code = 200;
            res.set_header("Content-Type", content_type);
            res.set_header("Content-Length", std::to_string(video_data.size()));
            res.set_header("Accept-Ranges", "bytes");
            res.body = std::string(video_data.begin(), video_data.end());
            
            std::string allowed = origin.empty() ? "*" : origin;
            res.set_header("Access-Control-Allow-Origin", allowed);
            return res;
        } catch (const std::exception& e) {
            return ApiUtils::createErrorResponse(e.what(), 500, origin);
        }
    });

    // DELETE /api/recordings/:id - Delete recording (both video file AND event from DB)
    CROW_ROUTE(app, "/api/recordings/<string>")
    .methods(crow::HTTPMethod::Delete, crow::HTTPMethod::Options)
    ([](const crow::request& req, std::string id) {
        std::string origin = ApiUtils::resolveCorsOrigin(req);
        if (req.method == crow::HTTPMethod::Options) {
            return ApiUtils::createResponse(json::object(), 204, origin);
        }

        if (!isValidRecordingId(id)) {
            return ApiUtils::createErrorResponse("Invalid recording ID", 400, origin);
        }

        try {
            vms::database::EventRepository event_repo;
            
            // Delete video files (both .ts and .mp4)
            std::string ts_path = "recordings/event_" + id + ".ts";
            std::string mp4_path = "recordings/event_" + id + ".mp4";
            
            // Also check DB for actual video_path
            auto event_opt = event_repo.getEventById(id);
            if (event_opt && !event_opt->video_path.empty()) {
                // H11: canonicalize against recordings/ root to prevent path traversal
                std::error_code ec;
                auto rec_root = std::filesystem::canonical("recordings", ec);
                auto vp = std::filesystem::path(event_opt->video_path);
                auto canonical_vp = std::filesystem::weakly_canonical(vp);
                bool under_root = !ec &&
                    canonical_vp.string().rfind(rec_root.string(), 0) == 0;
                if (under_root) {
                    if (std::filesystem::exists(canonical_vp)) {
                        std::filesystem::remove(canonical_vp);
                    }
                    std::string alt_path = canonical_vp.parent_path().string() + "/" +
                        canonical_vp.stem().string() +
                        (canonical_vp.extension() == ".ts" ? ".mp4" : ".ts");
                    if (std::filesystem::exists(alt_path)) {
                        std::filesystem::remove(alt_path);
                    }
                }
            }
            
            // Cleanup standard paths too
            if (std::filesystem::exists(ts_path)) std::filesystem::remove(ts_path);
            if (std::filesystem::exists(mp4_path)) std::filesystem::remove(mp4_path);
            
            // Delete snapshot if exists
            if (event_opt && !event_opt->snapshot_path.empty()) {
                if (std::filesystem::exists(event_opt->snapshot_path)) {
                    std::filesystem::remove(event_opt->snapshot_path);
                }
            }

            // Delete the event from DB entirely
            event_repo.deleteEvent(id);
            
            return ApiUtils::createResponse(json::object(), 200, origin);
        } catch (const std::exception& e) {
            return ApiUtils::createErrorResponse(e.what(), 500, origin);
        }
    });

    // =========================================================
    // CONTINUOUS RECORDING SEGMENTS API
    // =========================================================

    // GET /api/recordings/segments - List recording segments for a camera on a date
    CROW_ROUTE(app, "/api/recordings/segments")
    .methods(crow::HTTPMethod::Get, crow::HTTPMethod::Options)
    ([](const crow::request& req) {
        std::string origin = ApiUtils::resolveCorsOrigin(req);
        if (req.method == crow::HTTPMethod::Options) {
            return ApiUtils::createResponse(json::object(), 204, origin);
        }

        try {
            const auto& media_cfg = vms::Config::getInstance().getMediaSigningConfig();
            auto camera_id_str = req.url_params.get("camera_id");
            auto date_str = req.url_params.get("date"); // YYYY-MM-DD

            if (!camera_id_str) {
                return ApiUtils::createErrorResponse("camera_id is required", 400, origin);
            }

            int camera_id = std::stoi(camera_id_str);

            // Calculate time range from date
            time_t from_ts = 0;
            time_t to_ts = 0;

            if (date_str) {
                // Parse YYYY-MM-DD
                std::tm tm = {};
                std::istringstream ss(date_str);
                ss >> std::get_time(&tm, "%Y-%m-%d");
                if (!ss.fail()) {
                    tm.tm_hour = 0; tm.tm_min = 0; tm.tm_sec = 0;
                    from_ts = mktime(&tm);
                    to_ts = from_ts + 24 * 3600; // End of day
                }
            }

            vms::database::SegmentRepository seg_repo;
            auto segments = seg_repo.getSegments(camera_id, from_ts, to_ts);

            json segments_json = json::array();
            for (const auto& seg : segments) {
                json s;
                s["id"] = seg.id;
                s["camera_id"] = seg.camera_id;
                s["filename"] = seg.filename;
                s["start_time"] = seg.start_time;
                s["end_time"] = seg.end_time;
                s["duration"] = seg.end_time - seg.start_time;
                s["file_size"] = seg.file_size;
                s["status"] = seg.status;
                vms::utils::MediaAccessScope segment_scope;
                segment_scope.scope = "recording_segment_video";
                segment_scope.camera_id = seg.camera_id;
                segment_scope.resource_id = std::to_string(seg.id);
                s["video_url"] = vms::utils::presignPath(
                    "/api/recordings/segments/" + std::to_string(seg.id) + "/video",
                    media_cfg.segment_video_ttl_seconds,
                    segment_scope
                );
                segments_json.push_back(s);
            }

            json response;
            response["segments"] = segments_json;
            response["total"] = static_cast<int>(segments.size());

            return ApiUtils::createResponse(response, 200, origin);
        } catch (const std::exception& e) {
            return ApiUtils::createErrorResponse(e.what(), 500, origin);
        }
    });

    // GET /api/recordings/segments/<id>/video - Stream segment video
    CROW_ROUTE(app, "/api/recordings/segments/<int>/video")
    .methods(crow::HTTPMethod::Get, crow::HTTPMethod::Options)
    ([](const crow::request& req, int segment_id) {
        std::string origin = ApiUtils::resolveCorsOrigin(req);
        if (req.method == crow::HTTPMethod::Options) {
            return ApiUtils::createResponse(json::object(), 204, origin);
        }

        try {
            vms::database::SegmentRepository seg_repo;
            auto seg = seg_repo.getSegmentById(segment_id);

            if (seg.id < 0) {
                return ApiUtils::createErrorResponse("Segment not found", 404, origin);
            }

            std::filesystem::path file_path(seg.filename);
            if (!std::filesystem::exists(file_path)) {
                return ApiUtils::createErrorResponse("Segment file not found on disk", 404, origin);
            }

            // Read file and serve
            size_t file_size = std::filesystem::file_size(file_path);
            std::ifstream file(file_path, std::ios::binary);
            
            if (!file.is_open()) {
                return ApiUtils::createErrorResponse("Cannot open segment file", 500, origin);
            }

            // Check for Range header (partial content support)
            auto range_header = req.get_header_value("Range");
            
            if (!range_header.empty()) {
                // Parse Range: bytes=start-end
                size_t start = 0, end = file_size - 1;
                std::regex range_regex("bytes=(\\d+)-(\\d*)");
                std::smatch match;
                if (std::regex_search(range_header, match, range_regex)) {
                    start = std::stoull(match[1].str());
                    if (match[2].length() > 0) {
                        end = std::stoull(match[2].str());
                    }
                }

                if (start >= file_size) {
                    crow::response resp(416);
                    resp.set_header("Content-Range", "bytes */" + std::to_string(file_size));
                    return resp;
                }

                // Cap range chunk to 2MB to prevent OOM
                const size_t MAX_CHUNK = 2 * 1024 * 1024;
                if (end - start + 1 > MAX_CHUNK) {
                    end = start + MAX_CHUNK - 1;
                }

                end = std::min(end, file_size - 1);
                size_t content_length = end - start + 1;

                file.seekg(start);
                std::string content(content_length, '\0');
                file.read(&content[0], content_length);

                crow::response resp(206);
                resp.body = std::move(content);
                resp.set_header("Content-Type", "video/mp4");
                resp.set_header("Content-Length", std::to_string(content_length));
                resp.set_header("Content-Range", "bytes " + std::to_string(start) + "-" + std::to_string(end) + "/" + std::to_string(file_size));
                resp.set_header("Accept-Ranges", "bytes");
                if (!origin.empty()) {
                    resp.set_header("Access-Control-Allow-Origin", origin);
                }
                return resp;
            }

            // Full content
            std::string content((std::istreambuf_iterator<char>(file)),
                                 std::istreambuf_iterator<char>());

            crow::response resp(200);
            resp.body = std::move(content);
            resp.set_header("Content-Type", "video/mp4");
            resp.set_header("Content-Length", std::to_string(file_size));
            resp.set_header("Accept-Ranges", "bytes");
            if (!origin.empty()) {
                resp.set_header("Access-Control-Allow-Origin", origin);
            }
            return resp;
        } catch (const std::exception& e) {
            return ApiUtils::createErrorResponse(e.what(), 500, origin);
        }
    });
}

} // namespace api
} // namespace vms
