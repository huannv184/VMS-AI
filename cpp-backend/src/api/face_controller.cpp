#include "api/face_controller.h"
#include "core/camera_manager.h"
#include "database/person_repository.h"
#include "utils/api_utils.h"
#include "database/json_serialization.h"
#include "utils/logger.h"
#include "middleware/auth_middleware.h"
#include <zmq.hpp>
#include <filesystem>
#include <fstream>
#include <vector>

using json = nlohmann::json;

#ifdef DELETE
#undef DELETE
#endif

// Single application-level ZMQ context for face embedding extraction.
// zmq::context_t is thread-safe; sockets are created per-request.
static zmq::context_t& getZmqContext() {
    static zmq::context_t ctx(1);
    return ctx;
}

// ===========================================================================
// Helper: Extract face embedding from AI Worker via ZMQ
// Tries up to MAX_WORKER_ATTEMPTS active cameras' AI workers and returns the
// first successful embedding. Bounded so an HTTP request thread cannot stall
// for more than ~MAX_WORKER_ATTEMPTS × (sndtimeo + rcvtimeo) when every
// reachable worker is dead. Sequential is fine here — the success rate of the
// first worker is high in normal operation; this code path matters only on
// the failure side, which we're trimming.
// ===========================================================================
static bool extractEmbeddingFromAI(const std::string& image_base64, std::string& out_embedding_json) {
    out_embedding_json = "[]";

    // Pre-fix this loop iterated over EVERY active camera with 5s aggregate
    // timeout each. With 10 active cameras and all workers down (e.g. inference
    // service crashed), the calling Crow handler thread blocked for 50+ seconds
    // on a single /api/faces/persons POST — long enough that other API
    // requests on the same handler thread time out. Cap at 3 attempts and
    // tighten per-attempt timeouts so the worst case is ~6s.
    constexpr int MAX_WORKER_ATTEMPTS = 3;
    constexpr int RECV_TIMEOUT_MS = 1500;
    constexpr int SEND_TIMEOUT_MS = 500;

    try {
        auto& cam_mgr = vms::core::CameraManager::getInstance();
        auto active = cam_mgr.getAllCameras();
        auto& context = getZmqContext();

        int attempts = 0;
        for (const auto& cam : active) {
            if (!cam.is_active) continue;
            if (attempts >= MAX_WORKER_ATTEMPTS) break;
            ++attempts;

            int worker_port = 5560 + cam.id;
            zmq::socket_t socket(context, zmq::socket_type::req);
            socket.set(zmq::sockopt::rcvtimeo, RECV_TIMEOUT_MS);
            socket.set(zmq::sockopt::sndtimeo, SEND_TIMEOUT_MS);
            socket.set(zmq::sockopt::linger, 0);

            try {
                socket.connect("tcp://127.0.0.1:" + std::to_string(worker_port));

                json zmq_req;
                zmq_req["command"] = "EXTRACT_EMBEDDING";
                zmq_req["image"] = image_base64;

                std::string req_str = zmq_req.dump();
                socket.send(zmq::message_t(req_str.data(), req_str.size()), zmq::send_flags::none);

                zmq::message_t reply;
                auto res = socket.recv(reply, zmq::recv_flags::none);

                if (res.has_value()) {
                    std::string reply_str(static_cast<char*>(reply.data()), reply.size());
                    auto zmq_resp = json::parse(reply_str);

                    if (zmq_resp.value("status", "") == "success" && zmq_resp.contains("embedding")) {
                        out_embedding_json = zmq_resp["embedding"].dump();
                        LOG_INFO("Embedding extracted via Camera {} worker (dim={})",
                                 cam.id, zmq_resp["embedding"].size());
                        return true;
                    }
                }
            } catch (const std::exception& e) {
                LOG_DEBUG("AI Worker on port {} failed: {}", worker_port, e.what());
                continue;
            }
        }

        if (attempts == 0) {
            LOG_WARN("No active cameras available; cannot extract face embedding");
        }
    } catch (const std::exception& e) {
        LOG_ERROR("Error accessing camera manager: {}", e.what());
    }

    LOG_WARN("No active AI Worker could extract embedding (after up to {} attempts)",
             MAX_WORKER_ATTEMPTS);
    return false;
}

// ===========================================================================
// Helper: Save decoded image to disk
// Returns web-accessible path on success, empty string on failure
// ===========================================================================
static std::string saveImageToDisk(const std::string& decoded_image, const std::string& person_name) {
    std::string storage_dir = "storage/faces";
    if (!std::filesystem::exists(storage_dir)) {
        std::filesystem::create_directories(storage_dir);
    }

    // Generate unique filename
    std::string clean_name = person_name;
    std::replace_if(clean_name.begin(), clean_name.end(),
                    [](char c) { return !std::isalnum(static_cast<unsigned char>(c)); }, '_');
    std::string timestamp = std::to_string(std::time(nullptr));
    std::string filename = clean_name + "_" + timestamp + ".jpg";
    std::string filepath = storage_dir + "/" + filename;

    {
        std::ofstream out_file(filepath, std::ios::binary);
        if (!out_file.is_open()) {
            LOG_ERROR("Cannot open file for writing: {}", filepath);
            return "";
        }
        out_file.write(decoded_image.data(), decoded_image.size());
        if (!out_file.good()) {
            out_file.close();
            std::filesystem::remove(filepath);
            LOG_ERROR("Failed to write image to disk (disk full?)");
            return "";
        }
        out_file.close();
    }

    LOG_INFO("Saved face image to: {}", filepath);
    return "/api/storage/faces/" + filename;
}

// ===========================================================================
// Helper: Delete image file from disk
// ===========================================================================
static void deleteImageFromDisk(const std::string& web_path) {
    if (web_path.empty()) return;

    // Convert web path to local path: /api/storage/faces/xxx.jpg → storage/faces/xxx.jpg
    const std::string prefix = "/api/storage/faces/";
    if (web_path.find(prefix) != 0) return;

    std::string local_path = "storage/faces/" + web_path.substr(prefix.size());
    try {
        if (std::filesystem::exists(local_path)) {
            std::filesystem::remove(local_path);
            LOG_INFO("Deleted face image: {}", local_path);
        }
    } catch (const std::exception& e) {
        LOG_WARN("Failed to delete face image {}: {}", local_path, e.what());
    }
}

namespace vms {
namespace api {

void FaceController::registerRoutes(vms::server::VmsApp& app) {

    // =======================================================================
    // GET /api/faces/cameras/<int> — Get detected faces from live camera feed
    // =======================================================================
    CROW_ROUTE(app, "/api/faces/cameras/<int>")
    .methods(crow::HTTPMethod::GET, crow::HTTPMethod::OPTIONS)
    ([&app](const crow::request& req, int camera_id) {
        std::string origin = ApiUtils::resolveCorsOrigin(req);

        if (req.method == crow::HTTPMethod::OPTIONS)
            return ApiUtils::createResponse(json::object(), 204, origin);

        auto& ctx = app.get_context<vms::middleware::AuthMiddleware>(req);
        if (auto err = ApiUtils::requirePermission(ctx, Permission::FACE_READ, origin)) return std::move(*err);

        try {
            auto& camera_mgr = vms::core::CameraManager::getInstance();
            auto camera_opt = camera_mgr.getCamera(camera_id);
            
            if (!camera_opt) {
                return ApiUtils::createErrorResponse("Camera not found", 404, origin);
            }
            
            auto metadata_opt = camera_mgr.getLatestMetadata(camera_id);
            json faces = json::array();
            
            if (metadata_opt) {
                auto metadata = metadata_opt.value();
                for (const auto& det : metadata.detections) {
                    json face = {
                        {"track_id", det.track_id},
                        {"class_id", det.class_id},
                        {"confidence", det.confidence},
                        {"bbox", {{"x", det.x}, {"y", det.y}, {"width", det.width}, {"height", det.height}}},
                        {"person_id", -1},
                        {"person_name", "Unknown"}
                    };
                    faces.push_back(face);
                }
            }
            
            return ApiUtils::createResponse({
                {"camera_id", camera_id},
                {"faces", faces},
                {"count", faces.size()}
            }, 200, origin);
        } catch (const std::exception& e) {
            return ApiUtils::createErrorResponse(e.what(), 500, origin);
        }
    });

    // =======================================================================
    // GET/POST /api/faces/persons — List all persons / Create new person
    // =======================================================================
    CROW_ROUTE(app, "/api/faces/persons")
    .methods(crow::HTTPMethod::GET, crow::HTTPMethod::POST, crow::HTTPMethod::OPTIONS)
    ([&app](const crow::request& req) {
        std::string origin = ApiUtils::resolveCorsOrigin(req);

        if (req.method == crow::HTTPMethod::OPTIONS)
            return ApiUtils::createResponse(json::object(), 204, origin);

        auto& ctx = app.get_context<vms::middleware::AuthMiddleware>(req);
        // GET → FACE_READ; POST → FACE_WRITE. Operator role has READ-only on
        // faces; only admin can register / mutate persons. Previously any
        // authenticated user could spoof identities by inserting persons.
        const Permission needed = (req.method == crow::HTTPMethod::Get)
            ? Permission::FACE_READ : Permission::FACE_WRITE;
        if (auto err = ApiUtils::requirePermission(ctx, needed, origin)) return std::move(*err);

        // --- GET: List all persons ---
        if (req.method == crow::HTTPMethod::Get) {
            try {
                database::PersonRepository repo;
                auto persons = repo.getAllPersons();
                json response = json::array();
                for (const auto& p : persons) response.push_back(p);
                return ApiUtils::createResponse({{"persons", response}}, 200, origin);
            } catch (const std::exception& e) {
                return ApiUtils::createErrorResponse(e.what(), 500, origin);
            }
        }
        
        // --- POST: Create new person ---
        if (req.method == crow::HTTPMethod::Post) {
            LOG_INFO("POST /api/faces/persons received");
            try {
                auto body = json::parse(req.body);

                if (!body.contains("name")) {
                    LOG_ERROR("Missing name in request");
                    return ApiUtils::createErrorResponse("Missing name", 400, origin);
                }
                
                Person person;

                // Validation: Name
                if (!body["name"].is_string())
                    return ApiUtils::createErrorResponse("Name must be a string", 400, origin);
                std::string name = body["name"].get<std::string>();
                if (name.empty() || name.length() > 255)
                    return ApiUtils::createErrorResponse("Name length must be between 1 and 255", 400, origin);
                person.name = name;

                // Validation: Description
                if (body.contains("description")) {
                    if (!body["description"].is_string())
                        return ApiUtils::createErrorResponse("Description must be a string", 400, origin);
                    std::string desc = body["description"].get<std::string>();
                    if (desc.length() > 1000)
                        return ApiUtils::createErrorResponse("Description too long (max 1000 chars)", 400, origin);
                    person.description = desc;
                }

                // Handle Image Upload
                std::string image_base64 = body.value("image", "");
                person.embedding_json = "[]";

                if (!image_base64.empty()) {
                    try {
                        // Size limit check
                        if (image_base64.size() > 10 * 1024 * 1024) {
                            return ApiUtils::createErrorResponse("Image too large (max 10MB)", 400, origin);
                        }

                        std::string decoded_image = ApiUtils::base64_decode(image_base64);
                        if (decoded_image.empty()) {
                            return ApiUtils::createErrorResponse("Failed to decode image", 400, origin);
                        }

                        // Save to disk
                        std::string web_path = saveImageToDisk(decoded_image, person.name);
                        if (web_path.empty()) {
                            return ApiUtils::createErrorResponse("Failed to save image", 500, origin);
                        }
                        person.face_image_path = web_path;
                        LOG_INFO("Face image saved: {}", web_path);

                        // Extract embedding from AI Worker
                        std::string embedding_json;
                        if (extractEmbeddingFromAI(image_base64, embedding_json)) {
                            person.embedding_json = embedding_json;
                        } else {
                            LOG_WARN("Person will be created without embedding (no AI Worker available)");
                        }

                    } catch (const std::exception& e) {
                        LOG_ERROR("Image processing error: {}", e.what());
                        return ApiUtils::createErrorResponse("Image processing failed: " + std::string(e.what()), 500, origin);
                    }
                }

                person.created_at = std::time(nullptr);
                person.updated_at = person.created_at;

                database::PersonRepository repo;
                int new_id = repo.insertPerson(person);
                if (new_id > 0) {
                    LOG_INFO("Person created successfully: {} (ID: {})", person.name, new_id);

                    // Return full person object with ID
                    bool has_embedding = (person.embedding_json != "[]" && !person.embedding_json.empty());
                    json response = {
                        {"person", {
                            {"id", new_id},
                            {"name", person.name},
                            {"description", person.description},
                            {"face_image_path", person.face_image_path},
                            {"has_embedding", has_embedding},
                            {"created_at", person.created_at},
                            {"updated_at", person.updated_at}
                        }}
                    };
                    return ApiUtils::createResponse(response, 201, origin);
                }
                // BUG-H6 FIX: image was saved BEFORE the DB insert. If insert
                // fails we now own a file with no row pointing at it (orphan).
                // Roll the file back so /storage/faces doesn't accumulate junk.
                if (!person.face_image_path.empty()) {
                    deleteImageFromDisk(person.face_image_path);
                }
                LOG_ERROR("Failed to insert person into DB");
                return ApiUtils::createErrorResponse("Failed to create person", 500, origin);
            } catch (const std::exception& e) {
                LOG_ERROR("Exception in POST /api/faces/persons: {}", e.what());
                return ApiUtils::createErrorResponse(e.what(), 500, origin);
            }
        }

        return ApiUtils::createErrorResponse("Method not allowed", 405, origin);
    });
    
    // =======================================================================
    // GET/PUT/DELETE /api/faces/persons/<int> — CRUD single person
    // =======================================================================
    CROW_ROUTE(app, "/api/faces/persons/<int>")
    .methods(crow::HTTPMethod::GET, crow::HTTPMethod::PUT, crow::HTTPMethod::DELETE, crow::HTTPMethod::OPTIONS)
    ([&app](const crow::request& req, int id) {
        std::string origin = ApiUtils::resolveCorsOrigin(req);

        if (req.method == crow::HTTPMethod::Options)
            return ApiUtils::createResponse(json::object(), 204, origin);

        auto& ctx = app.get_context<vms::middleware::AuthMiddleware>(req);
        const Permission needed = (req.method == crow::HTTPMethod::Get)
            ? Permission::FACE_READ : Permission::FACE_WRITE;
        if (auto err = ApiUtils::requirePermission(ctx, needed, origin)) return std::move(*err);

        database::PersonRepository repo;

        // --- GET person by ID ---
        if (req.method == crow::HTTPMethod::Get) {
            try {
                auto p = repo.getPersonById(id);
                if (p) return ApiUtils::createResponse(p.value(), 200, origin);
                return ApiUtils::createErrorResponse("Not found", 404, origin);
            } catch (const std::exception& e) {
                return ApiUtils::createErrorResponse(e.what(), 500, origin);
            }
        }

        // --- PUT: Update person (supports image re-upload) ---
        if (req.method == crow::HTTPMethod::Put) {
            try {
                auto p_opt = repo.getPersonById(id);
                if (!p_opt) return ApiUtils::createErrorResponse("Not found", 404, origin);

                auto body = json::parse(req.body);
                Person p = p_opt.value();
                
                if (body.contains("name")) {
                    if (!body["name"].is_string())
                        return ApiUtils::createErrorResponse("Name must be a string", 400, origin);
                    std::string name = body["name"].get<std::string>();
                    if (name.empty() || name.length() > 255)
                        return ApiUtils::createErrorResponse("Name length must be between 1 and 255", 400, origin);
                    p.name = name;
                }
                if (body.contains("description")) {
                    p.description = body["description"].get<std::string>();
                }

                // Handle image re-upload
                std::string image_base64 = body.value("image", "");
                if (!image_base64.empty()) {
                    try {
                        if (image_base64.size() > 10 * 1024 * 1024) {
                            return ApiUtils::createErrorResponse("Image too large (max 10MB)", 400, origin);
                        }

                        std::string decoded_image = ApiUtils::base64_decode(image_base64);
                        if (decoded_image.empty()) {
                            return ApiUtils::createErrorResponse("Failed to decode image", 400, origin);
                        }

                        // BUG-H6 FIX: save new image FIRST. Only after we know
                        // we have a working replacement on disk do we delete
                        // the previous one. The old order (delete-then-save)
                        // left the person with no image at all if the new save
                        // failed (out of disk, permission error, etc.).
                        std::string previous_path = p.face_image_path;
                        std::string web_path = saveImageToDisk(decoded_image, p.name);
                        if (!web_path.empty()) {
                            p.face_image_path = web_path;
                            if (!previous_path.empty() && previous_path != web_path) {
                                deleteImageFromDisk(previous_path);
                            }
                        }

                        // Re-extract embedding. If the image changed but the
                        // AI worker is unavailable, the OLD embedding must
                        // not stick around — search against it would be a
                        // false-identity result against the new face. Clear
                        // it so search returns "no match" until the operator
                        // re-uploads (or the worker is brought back).
                        std::string embedding_json;
                        if (extractEmbeddingFromAI(image_base64, embedding_json)) {
                            p.embedding_json = embedding_json;
                        } else {
                            LOG_WARN("Person {} image updated but AI worker did not return "
                                     "embedding; clearing stale embedding to avoid "
                                     "false-identity match.", p.id);
                            p.embedding_json = "[]";
                        }
                    } catch (const std::exception& e) {
                        LOG_ERROR("Image update error: {}", e.what());
                        // Continue with other updates even if image fails
                    }
                }

                p.updated_at = std::time(nullptr);
                
                if (repo.updatePerson(p)) {
                    bool has_embedding = (p.embedding_json != "[]" && !p.embedding_json.empty());
                    json response = {
                        {"person", {
                            {"id", p.id},
                            {"name", p.name},
                            {"description", p.description},
                            {"face_image_path", p.face_image_path},
                            {"has_embedding", has_embedding},
                            {"created_at", p.created_at},
                            {"updated_at", p.updated_at}
                        }}
                    };
                    return ApiUtils::createResponse(response, 200, origin);
                }
                return ApiUtils::createErrorResponse("Update failed", 500, origin);
            } catch (const std::exception& e) {
                return ApiUtils::createErrorResponse(e.what(), 500, origin);
            }
        }

        // --- DELETE: Remove person + cleanup image file ---
        if (req.method == crow::HTTPMethod::Delete) {
            try {
                // BUG-FACE-DEL-01 (audit 2026-05-08): pre-fix order deleted
                // the image file BEFORE the DB row. If repo.deletePerson()
                // failed (DB lock, FK constraint), the image was already gone
                // but the row stuck around with face_image_path pointing at
                // a now-missing file — UI shows broken thumbnail. Delete the
                // DB row first; only on success do we drop the image. If the
                // image delete itself then fails it logs a warning and we
                // accept the orphan file (less bad than the inverse).
                auto p_opt = repo.getPersonById(id);

                if (!repo.deletePerson(id))
                    return ApiUtils::createErrorResponse("Delete failed", 500, origin);

                if (p_opt) {
                    deleteImageFromDisk(p_opt.value().face_image_path);
                }
                return ApiUtils::createResponse(json::object(), 200, origin);
            } catch (const std::exception& e) {
                return ApiUtils::createErrorResponse(e.what(), 500, origin);
            }
        }

        return ApiUtils::createErrorResponse("Method not allowed", 405, origin);
    });

    // =======================================================================
    // GET /api/faces/stats — Rich statistics
    // =======================================================================
    CROW_ROUTE(app, "/api/faces/stats")
    .methods(crow::HTTPMethod::GET, crow::HTTPMethod::OPTIONS)
    ([&app](const crow::request& req) {
        std::string origin = ApiUtils::resolveCorsOrigin(req);

        if (req.method == crow::HTTPMethod::Options)
            return ApiUtils::createResponse(json::object(), 204, origin);

        auto& ctx = app.get_context<vms::middleware::AuthMiddleware>(req);
        if (auto err = ApiUtils::requirePermission(ctx, Permission::FACE_READ, origin)) return std::move(*err);

        try {
            database::PersonRepository repo;
            auto stats = repo.getPersonStats();
            return ApiUtils::createResponse({
                {"total_registered_persons", stats.total},
                {"persons_with_embedding", stats.with_embedding},
                {"persons_with_image", stats.with_image},
                {"ai_ready_percentage", stats.total > 0 
                    ? static_cast<int>(100.0 * stats.with_embedding / stats.total) 
                    : 0}
            }, 200, origin);
        } catch (const std::exception& e) {
            return ApiUtils::createErrorResponse(e.what(), 500, origin);
        }
    });

    // =======================================================================
    // POST /api/faces/search — Search by face image
    // =======================================================================
    CROW_ROUTE(app, "/api/faces/search")
    .methods(crow::HTTPMethod::POST, crow::HTTPMethod::OPTIONS)
    ([&app](const crow::request& req) {
        std::string origin = ApiUtils::resolveCorsOrigin(req);

        if (req.method == crow::HTTPMethod::Options)
            return ApiUtils::createResponse(json::object(), 204, origin);

        auto& ctx = app.get_context<vms::middleware::AuthMiddleware>(req);
        // Search is a read-only query against the embedding gallery.
        if (auto err = ApiUtils::requirePermission(ctx, Permission::FACE_READ, origin)) return std::move(*err);

        try {
            auto body = json::parse(req.body);
            std::string image_base64 = body.value("image", "");
            float threshold = body.value("threshold", 0.6f);
            
            if (image_base64.empty()) {
                return ApiUtils::createErrorResponse("Missing image data", 400, origin);
            }
            
            // Extract embedding using shared helper
            std::string embedding_json_str;
            if (!extractEmbeddingFromAI(image_base64, embedding_json_str)) {
                return ApiUtils::createErrorResponse("Failed to generate embedding (No active AI Worker)", 503, origin);
            }

            // Parse embedding to vector<float>
            std::vector<float> embedding;
            try {
                auto emb_json = json::parse(embedding_json_str);
                embedding = emb_json.get<std::vector<float>>();
            } catch (...) {
                return ApiUtils::createErrorResponse("Invalid embedding format from AI Worker", 500, origin);
            }
            
            // Search DB
            database::PersonRepository repo;
            auto matches = repo.searchByEmbedding(embedding, threshold);
            
            json results = json::array();
            for (const auto& match : matches) {
                json item = match.first; // Person to JSON (via NLOHMANN macro)
                item["similarity"] = match.second;
                item["has_embedding"] = true;
                results.push_back(item);
            }
            
            return ApiUtils::createResponse({
                {"matches", results},
                {"count", results.size()},
                {"threshold_used", threshold}
            }, 200, origin);
            
        } catch (const std::exception& e) {
            LOG_ERROR("Search exception: {}", e.what());
            return ApiUtils::createErrorResponse(e.what(), 500, origin);
        }
    });
}

} // namespace api
} // namespace vms
