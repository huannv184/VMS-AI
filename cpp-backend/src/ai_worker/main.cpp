// ============================================================================
// File: ai_worker/main.cpp - JSON PROTOCOL VERSION
// 
// Architecture:
//   - stdout: JSON IPC protocol for Manager process (DO NOT use for logging)
//   - stderr: Diagnostic/status logs (read by Manager's stderr pipe)
//   - spdlog is NOT used here because this is a separate executable
//     spawned by the Manager; cerr usage is intentional and by design.
// ============================================================================

#include <iostream>
#include <thread>
#include <chrono>
#include <csignal>
#include <filesystem>
#include <memory>
#include <nlohmann/json.hpp> 
#include <sqlite3.h>
#include <vector>
#include <fstream>
#include <iomanip>
#include <zmq.hpp>
#include <cctype>
#include <stdexcept>

#include "../ai/ipc/shared_memory_manager.h"
#include "../ai/inference/include/inference/multi_model_infer.h"
#include "../ai/inference/include/inference/tracking.h"



using json = nlohmann::json;

// Global flag
volatile std::sig_atomic_t g_running = 1;

// Cap on the base64 image payload accepted by EXTRACT_EMBEDDING. Mirrors the
// 10MB cap that face_controller.cpp:269 enforces before forwarding; the worker
// adds 2MB headroom to absorb base64 inflation off raw image bytes (≈4/3) and
// still reject anything pathological.
static constexpr size_t kMaxEmbeddingImageBase64Bytes = 12u * 1024u * 1024u;

// Basic Base64 Decoder
std::string base64_decode(const std::string &in) {
    std::string out;
    std::vector<int> T(256, -1);
    for (int i = 0; i < 64; i++) T["ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/"[i]] = i;

    int val = 0, valb = -8;
    for (size_t i = 0; i < in.size(); i++) {
        unsigned char c = static_cast<unsigned char>(in[i]);
        if (std::isspace(c)) continue;
        if (c == '=') break; // End of base64 data
        if (T[c] == -1) throw std::invalid_argument("Invalid character in base64 string");
        val = (val << 6) + T[c];
        valb += 6;
        if (valb >= 0) {
            out.push_back(char((val >> valb) & 0xFF));
            valb -= 8;
        }
    }
    return out;
}

// Forward decl — defined later; needed by zmq_rep_thread for RELOAD_FACE_DB.
void loadFaceDatabase(int camera_id, const std::string& db_path, inference::MultiModelInfer* inferer);

// ZMQ Thread Function
void zmq_rep_thread(int camera_id, inference::MultiModelInfer* inferer,
                    std::mutex* infer_mutex, std::string db_path) {
    try {
        zmq::context_t context(1);
        zmq::socket_t socket(context, zmq::socket_type::rep);
        
        // PORT CONFLICT FIX: Enable socket reuse and proper cleanup
        int linger_ms = 0; // Immediately close socket on exit
        socket.set(zmq::sockopt::linger, linger_ms);
        
        int port = 5560 + camera_id;
        // BUG-AIW-NETBIND-01 (audit 2026-05-08): pre-fix bound "tcp://*",
        // exposing the EXTRACT_EMBEDDING / RELOAD_FACE_DB ZMQ REP socket on
        // every network interface. Anyone reachable on the LAN could submit
        // images for face-embedding extraction (computational DoS, plus the
        // returned embeddings leaked information about whatever the model
        // produced for that crop), or trigger DB reload churn. Same shape
        // class as MediaMTX `tcp_address: :8889` → `127.0.0.1:8889` fix
        // 2026-05-02. The legitimate caller (face_controller) connects to
        // `tcp://127.0.0.1:port`; loopback-only is sufficient and removes
        // the entire LAN-exposed surface. Override via VMS_AI_ZMQ_BIND if
        // a multi-host deployment ever becomes a real requirement.
        std::string bind_host = "127.0.0.1";
        if (const char* e = std::getenv("VMS_AI_ZMQ_BIND"); e && *e) {
            bind_host = e;
        }
        std::string endpoint = "tcp://" + bind_host + ":" + std::to_string(port);
        
        // RETRY LOGIC: Try to bind with exponential backoff
        const int max_retries = 5;
        const int base_delay_ms = 500;
        bool bound = false;
        
        for (int retry = 0; retry < max_retries && !bound; ++retry) {
            try {
                socket.bind(endpoint);
                bound = true;
                std::cerr << "[AI-Worker-" << camera_id << "] ZMQ REP listening on " << endpoint << std::endl;
            } catch (const zmq::error_t& e) {
                if (retry < max_retries - 1) {
                    int delay = base_delay_ms * (1 << retry); // Exponential backoff
                    std::cerr << "[AI-Worker-" << camera_id << "] ZMQ bind failed (attempt " 
                              << (retry + 1) << "/" << max_retries << "): " << e.what() 
                              << ". Retrying in " << delay << "ms..." << std::endl;
                    std::this_thread::sleep_for(std::chrono::milliseconds(delay));
                } else {
                    throw; // Re-throw on final retry
                }
            }
        }
        
        if (!bound) {
            std::cerr << "[AI-Worker-" << camera_id << "] Failed to bind ZMQ socket after " 
                      << max_retries << " attempts" << std::endl;
            return;
        }
        
        // Timeout for recv to allow checking g_running
        const int timeout = 1000;
        socket.set(zmq::sockopt::rcvtimeo, timeout);
        
        while (g_running) {
            zmq::message_t request;
            auto res = socket.recv(request, zmq::recv_flags::none);
            if (!res.has_value()) continue; // Timeout
            
            std::string req_str(static_cast<char*>(request.data()), request.size());
            std::string reply_str = "{}";
            
            try {
                auto req_json = json::parse(req_str);
                std::string command = req_json.value("command", "");
                
                if (command == "EXTRACT_EMBEDDING") {
                    std::string img_base64 = req_json.value("image", "");
                    if (img_base64.empty()) {
                        reply_str = "{\"status\": \"error\", \"message\": \"Empty image data\"}";
                    } else if (img_base64.size() > kMaxEmbeddingImageBase64Bytes) {
                        // BUG-AIW-INPUT-01 (audit 2026-05-08): pre-fix the worker
                        // had no upper bound on the ZMQ image payload. A 100 MB
                        // base64 string would happily decode to ~75 MB and run
                        // through cv::imdecode — single bad caller could OOM
                        // the worker or stall the TensorRT mutex for tens of
                        // seconds. The legitimate caller (face_controller) caps
                        // at 10 MB on its own; we mirror that limit here so a
                        // direct ZMQ client can't bypass it.
                        reply_str = "{\"status\": \"error\", \"message\": \"image payload exceeds 12MB cap\"}";
                        std::cerr << "[AI-Worker-" << camera_id
                                  << "] EXTRACT_EMBEDDING rejected: payload "
                                  << img_base64.size() << " bytes" << std::endl;
                    } else {
                        std::string decoded = base64_decode(img_base64);
                        std::vector<uchar> data(decoded.begin(), decoded.end());
                        cv::Mat img = cv::imdecode(data, cv::IMREAD_COLOR);

                        if (!img.empty()) {
                            // THREAD SAFETY FIX: Lock mutex before using TensorRT
                            std::lock_guard<std::mutex> lock(*infer_mutex);
                            std::cerr << "[AI-Worker-" << camera_id << "] EXTRACT_EMBEDDING: img "
                                      << img.cols << "x" << img.rows << std::endl;

                            auto emb = inferer->embedImage(img);

                            json j_resp;
                            if (emb.empty()) {
                                j_resp["status"] = "error";
                                j_resp["message"] = "No face detected";
                                std::cerr << "[AI-Worker-" << camera_id << "] embedImage returned empty" << std::endl;
                            } else {
                                j_resp["status"] = "success";
                                j_resp["embedding"] = emb;
                                std::cerr << "[AI-Worker-" << camera_id << "] Embedding extracted (dim="
                                          << emb.size() << ")" << std::endl;
                            }
                            reply_str = j_resp.dump();
                        } else {
                            reply_str = "{\"status\": \"error\", \"message\": \"Failed to decode image\"}";
                        }
                    }
                } else if (command == "RELOAD_FACE_DB") {
                    // BUG-AIW-FACEDB-01 (audit 2026-05-08): face_database_ in
                    // the inference engine is populated only by loadFaceDatabase
                    // at boot. After boot, when face_controller inserts/updates/
                    // deletes a person via REST, the live inference path never
                    // re-reads the DB → newly registered faces never get
                    // matched, deleted faces still match. Half-dead pipeline,
                    // same shape as the BUG-ANPR-PIPELINE family. Now the
                    // controller fans out RELOAD_FACE_DB after every CUD op,
                    // workers reload + replace their in-memory gallery atomically.
                    std::lock_guard<std::mutex> lock(*infer_mutex);
                    if (inferer) {
                        inferer->clearDatabase();
                        loadFaceDatabase(camera_id, db_path, inferer);
                        reply_str = "{\"status\": \"success\", \"message\": \"face DB reloaded\"}";
                    } else {
                        reply_str = "{\"status\": \"error\", \"message\": \"inferer not available\"}";
                    }
                } else {
                    reply_str = "{\"status\": \"error\", \"message\": \"Unknown command\"}";
                }
            } catch (const std::exception& e) {
                reply_str = "{\"status\": \"error\", \"message\": \"" + std::string(e.what()) + "\"}";
            }
            
            zmq::message_t reply(reply_str.data(), reply_str.size());
            socket.send(reply, zmq::send_flags::none);
        }
    } catch (const std::exception& e) {
        std::cerr << "[AI-Worker-" << camera_id << "] ZMQ Thread Error: " << e.what() << std::endl;
    }
}



void signal_handler(int) {
    g_running = 0;
}

// ============================================================================
// COCO CLASS NAMES (YOLO classes)
// ============================================================================
static const std::unordered_map<int, std::string> COCO_CLASSES = {
    {0, "person"}, {1, "bicycle"}, {2, "car"}, {3, "motorcycle"}, 
    {4, "airplane"}, {5, "bus"}, {6, "train"}, {7, "truck"}, 
    {8, "boat"}, {9, "traffic light"}, {10, "fire hydrant"}, 
    {11, "stop sign"}, {12, "parking meter"}, {13, "bench"}, 
    {14, "bird"}, {15, "cat"}, {16, "dog"}, {17, "horse"}, 
    {18, "sheep"}, {19, "cow"}, {20, "elephant"}, {21, "bear"}, 
    {22, "zebra"}, {23, "giraffe"}, {24, "backpack"}, {25, "umbrella"},
    {26, "handbag"}, {27, "tie"}, {28, "suitcase"}, {29, "frisbee"},
    {30, "skis"}, {31, "snowboard"}, {32, "sports ball"}, {33, "kite"},
    {34, "baseball bat"}, {35, "baseball glove"}, {36, "skateboard"},
    {37, "surfboard"}, {38, "tennis racket"}, {39, "bottle"},
    {40, "wine glass"}, {41, "cup"}, {42, "fork"}, {43, "knife"},
    {44, "spoon"}, {45, "bowl"}, {46, "banana"}, {47, "apple"},
    {48, "sandwich"}, {49, "orange"}, {50, "broccoli"}, {51, "carrot"},
    {52, "hot dog"}, {53, "pizza"}, {54, "donut"}, {55, "cake"},
    {56, "chair"}, {57, "couch"}, {58, "potted plant"}, {59, "bed"},
    {60, "dining table"}, {61, "toilet"}, {62, "tv"}, {63, "laptop"},
    {64, "mouse"}, {65, "remote"}, {66, "keyboard"}, {67, "cell phone"},
    {68, "microwave"}, {69, "oven"}, {70, "toaster"}, {71, "sink"},
    {72, "refrigerator"}, {73, "book"}, {74, "clock"}, {75, "vase"},
    {76, "scissors"}, {77, "teddy bear"}, {78, "hair drier"}, {79, "toothbrush"}
};

std::string getClassName(int class_id) {
    auto it = COCO_CLASSES.find(class_id);
    if (it != COCO_CLASSES.end()) {
        return it->second;
    }
    return "object_" + std::to_string(class_id);
}

// ============================================================================
// FACE DATABASE LOADING
// ============================================================================
void loadFaceDatabase(int camera_id, const std::string& db_path, inference::MultiModelInfer* inferer) {
    std::cerr << "[AI-Worker-" << camera_id << "] Loading face database from: " << db_path << std::endl;
    
    sqlite3* db;
    int rc = sqlite3_open(db_path.c_str(), &db);
    if (rc != SQLITE_OK) {
        std::cerr << "[AI-Worker-" << camera_id << "] Error: Cannot open database for faces: " << sqlite3_errmsg(db) << std::endl;
        sqlite3_close(db);
        return;
    }
    
    const char* sql = "SELECT id, name, embedding_json FROM persons";
    sqlite3_stmt* stmt;
    rc = sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr);
    
    if (rc != SQLITE_OK) {
        std::cerr << "[AI-Worker-" << camera_id << "] Error: Failed to prepare face query: " << sqlite3_errmsg(db) << std::endl;
        sqlite3_close(db);
        return;
    }
    
    int count = 0;
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        int id = sqlite3_column_int(stmt, 0);
        const char* name_ptr = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 1));
        const char* json_ptr = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 2));
        
        if (!name_ptr || !json_ptr) continue;
        
        std::string name(name_ptr);
        std::string embedding_json(json_ptr);
        
        try {
            auto j = nlohmann::json::parse(embedding_json);
            if (j.is_array() && j.size() == 512) {
                std::vector<float> embedding = j.get<std::vector<float>>();
                if (inferer->addPerson(id, name, embedding)) {
                    count++;
                }
            } else {
                // If it's a nested array (e.g., list of embeddings for one person)
                if (j.is_array() && j.size() > 0 && j[0].is_array() && j[0].size() == 512) {
                     std::vector<float> embedding = j[0].get<std::vector<float>>();
                     if (inferer->addPerson(id, name, embedding)) {
                         count++;
                     }
                }
            }
        } catch (...) {
            std::cerr << "[AI-Worker-" << camera_id << "] Failed to parse embedding for: " << name << std::endl;
        }
    }
    
    sqlite3_finalize(stmt);
    sqlite3_close(db);
    
    std::cerr << "[AI-Worker-" << camera_id << "] ✅ Loaded " << count << " persons for recognition" << std::endl;
}

// Helper to find models robustly
std::string resolveModelPath(const std::string& filename, const std::string& exe_path) {
    namespace fs = std::filesystem;
    
    // 1. Check if already absolute and exists
    if (fs::path(filename).is_absolute() && fs::exists(filename)) {
        return filename;
    }
    
    // 2. Check relative to current working directory
    if (fs::exists(filename)) {
        return fs::absolute(filename).string();
    }
    
    // 3. Check in 'models' subfolder of CWD
    fs::path in_models = fs::path("models") / filename;
    if (fs::exists(in_models)) {
        return fs::absolute(in_models).string();
    }
    
    // 4. Check relative to executable directory
    fs::path exe_dir = fs::path(exe_path).parent_path();
    fs::path rel_to_exe = exe_dir / filename;
    if (fs::exists(rel_to_exe)) {
        return fs::absolute(rel_to_exe).string();
    }
    
    fs::path models_rel_to_exe = exe_dir / "models" / filename;
    if (fs::exists(models_rel_to_exe)) {
        return fs::absolute(models_rel_to_exe).string();
    }

    // Return original and let the engine fail with a clear log if not found
    return filename;
}

// ============================================================================
// MAIN
// ============================================================================
int main(int argc, char** argv) {
    // Disable sync with stdio for performance
    std::ios_base::sync_with_stdio(false);

    if (argc < 3) {
        std::cerr << "Usage: " << argv[0] << " <camera_id> <model_path> [db_path]" << std::endl;
        return 1;
    }
    
    int camera_id = std::stoi(argv[1]);
    std::string model_path = argv[2];
    std::string db_path = (argc > 3) ? argv[3] : "data/events.db";
    
    std::cerr << "[AI-Worker-" << camera_id << "] Starting..." << std::endl;
    
    // ========================================
    // 1. Initialize Shared Memory (Keep for compatibility/future)
    // ========================================
    std::unique_ptr<ipc::SharedMemoryManager> shm;
    
    try {
        shm = std::make_unique<ipc::SharedMemoryManager>(camera_id);
        
        if (!shm->initialize()) {
            std::cerr << "[AI-Worker-" << camera_id << "] Failed to init Shared Memory" << std::endl;
            // return 1; // Don't fail, maybe we just want JSON
        } else {
            std::cerr << "[AI-Worker-" << camera_id << "] ✅ Shared Memory initialized" << std::endl;
        }
        
    } catch (const std::exception& e) {
        std::cerr << "[AI-Worker-" << camera_id << "] SHM Exception: " << e.what() << std::endl;
    }
    
    // ========================================
    // 2. Initialize AI Config
    // ========================================
    inference::MultiModelInfer::Config ai_config;
    ai_config.yolo_model_path = model_path;
    ai_config.yolo_input_size = 640;
    // Confidence threshold.
    // 2026-04-26: lowered to 0.10 to compensate for a TRT engine whose output
    //   probabilities peaked at 0.6-0.9 for confident objects and dropped
    //   below 0.25 for distant/partial ones. That setting let surveillance
    //   scenes detect anything at all — but it also hallucinated "person"
    //   on trash cans, vertical posts, and shadowy textures.
    // 2026-05-08 (Fix-B): raised default to 0.30 after operator-reported
    //   regressions ("thùng rác cũng ra người"). 0.30 catches confident
    //   detections (>= 60% of recall vs 0.10 in our test scenes) without
    //   the false-positive flood. Combine with VMS_MIN_PERSON_HEIGHT_PX
    //   to filter the long-tail of small/distant noise. If a deployment
    //   really needs 0.10, set VMS_YOLO_CONF_THRESHOLD=0.10 explicitly.
    {
        const char* env = std::getenv("VMS_YOLO_CONF_THRESHOLD");
        ai_config.yolo_conf_threshold = (env && *env) ? std::strtof(env, nullptr) : 0.30f;
    }
    ai_config.yolo_nms_threshold = 0.45f;
    
    ai_config.enable_face_detection = true; // SCRFD
    ai_config.enable_face_recognition = true; // ArcFace
    {
        const char* env = std::getenv("VMS_SCRFD_CONF");
        if (env && *env) ai_config.scrfd_conf_threshold = std::strtof(env, nullptr);
    }
    
    std::string exe_path = argv[0];
    ai_config.scrfd_model_path = resolveModelPath("scrfd_2.5g_bnkps.trt", exe_path);
    ai_config.arcface_model_path = resolveModelPath("arcfaceresnet100-8.trt", exe_path);

    // Fire/Smoke - Disabled for optimization
    ai_config.enable_fire_detection = false;
    ai_config.fire_model_path = resolveModelPath("fire_smoke_best.onnx", exe_path); 
    
    // LPR - Disabled for optimization
    ai_config.enable_lpr = false;
    ai_config.plate_detect_model_path = resolveModelPath("yolov8n_plate.onnx", exe_path);
    ai_config.lpr_model_path = resolveModelPath("lprnet.onnx", exe_path);
    
    // Parse Config from Arguments (Index 4)
    if (argc > 4) {
        std::string config_str = argv[4];
        try {
            std::cerr << "[AI-Worker-" << camera_id << "] Parsing config: " << config_str << std::endl;
            auto j_config = json::parse(config_str);
            
            if (j_config.contains("yolo")) ai_config.enable_yolo = j_config["yolo"];
            if (j_config.contains("face")) {
                ai_config.enable_face_detection = j_config["face"];
                ai_config.enable_face_recognition = j_config["face"];
            }
            if (j_config.contains("lpr")) ai_config.enable_lpr = j_config["lpr"];
            if (j_config.contains("fire")) ai_config.enable_fire_detection = j_config["fire"];
            if (j_config.contains("face_match_threshold")) {
                ai_config.face_match_threshold = j_config["face_match_threshold"].get<float>();
                std::cerr << "[AI-Worker-" << camera_id << "] Custom face_match_threshold: " 
                          << ai_config.face_match_threshold << std::endl;
            }
        } catch (const std::exception& e) {
             std::cerr << "[AI-Worker-" << camera_id << "] Error parsing config: " << e.what() << std::endl;
        }
    }
    
    // ========================================
    // 3. Init AI Engine
    // ========================================
    std::cerr << "[AI-Worker-" << camera_id << "] Loading AI Models..." << std::endl;
    
    std::unique_ptr<inference::MultiModelInfer> inferer;
    
    try {
        inferer = std::make_unique<inference::MultiModelInfer>(ai_config);
        
        if (!inferer->init()) {
            std::cerr << "[AI-Worker-" << camera_id << "] ⚠ Warning: AI Init failed" << std::endl;
        } else {
            std::cerr << "[AI-Worker-" << camera_id << "] ✅ AI Models loaded" << std::endl;
            
            // Load face database immediately after init
            if (ai_config.enable_face_recognition) {
                loadFaceDatabase(camera_id, db_path, inferer.get());
            }
        }
        
    } catch (const std::exception& e) {
        std::cerr << "[AI-Worker-" << camera_id << "] AI Init Exception: " << e.what() << std::endl;
        return 1;
    }
    
    // ========================================
    // 4. Setup Signal Handlers
    // ========================================
    std::signal(SIGINT, signal_handler);
    std::signal(SIGTERM, signal_handler);
    
    // Mutex to serialize TensorRT access between main loop and ZMQ thread
    std::mutex infer_mutex;
    
    // Start ZMQ Thread — BUG-003 FIX: join() instead of detach()
    // Detaching caused dangling reference to `inferer` after main() returns
    std::thread zmq_thread(zmq_rep_thread, camera_id, inferer.get(), &infer_mutex, db_path);
    
    std::cerr << "[AI-Worker-" << camera_id << "] Ready processing loop." << std::endl;
    
    // ========================================
    // 5. Main Processing Loop
    // ========================================
    cv::Mat frame;
    uint64_t last_frame_id = 0;
    uint64_t timestamp = 0;
    uint64_t frames_processed = 0;
    // BUG-AIW-LOOP-01 (audit 2026-05-08): pre-fix the catch-all in the loop
    // logged at WARN and slept 100ms forever, even if inference threw on
    // every single frame (e.g. corrupted TRT engine, OOM on input). Combined
    // with stderr piped to the manager process, that produced 10 lines/sec
    // of noise indefinitely with no recovery path. Add a consecutive-failure
    // counter; bail after kMaxConsecutiveFailures so the manager can restart
    // the worker rather than tail-spin forever.
    int consecutive_failures = 0;
    constexpr int kMaxConsecutiveFailures = 50;

    auto loop_start = std::chrono::steady_clock::now();
    
    // Face tracker: smoothing + ID persistence across frames
    inference::FaceTracker faceTracker(0.3f, 30, 2);
    // Object tracker: gives persons/vehicles stable track_ids instead of -1,
    // preventing cooldown key collisions in AiEventProcessor::processIntrusion.
    //
    // BBOX-LATENCY FIX: default min_hits=3 means a detection had to appear in 3
    // IoU-matched consecutive frames before bbox shows up — at 5–10 fps that's
    // 300–600ms invisible, plus any IoU jitter resets the count. Drop to 1 so
    // bbox shows on first detection (track still gets stable ID for cooldown).
    inference::TrackerConfig tracker_cfg;
    tracker_cfg.min_hits = 1;
    tracker_cfg.iou_threshold = 0.2f;  // looser → easier to maintain track on shaky bbox
    inference::ObjectTracker objectTracker(tracker_cfg);
    
    
    while (g_running) {
        try {
            if (!shm) {
                 std::this_thread::sleep_for(std::chrono::milliseconds(100));
                 continue;
            }

            // Read frame from SHM
            uint64_t current_frame_id = 0;
            
            if (shm->readLatestVideoFrame(frame, current_frame_id, timestamp)) {
                if (current_frame_id == last_frame_id) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(5));
                    continue;
                }
                
                last_frame_id = current_frame_id;

                if (frame.empty() || frame.cols == 0 || frame.rows == 0) {
                    continue;
                }

                // DIAG: dump every 200th frame to disk so operator can verify the
                // SHM image is actually a valid camera frame (not all-black or noise).
                // Enable with VMS_AI_DUMP_FRAMES=1.
                {
                    static const bool dump_frames = []() {
                        const char* e = std::getenv("VMS_AI_DUMP_FRAMES");
                        return e && *e && std::atoi(e) != 0;
                    }();
                    // BUG-AIW-DIAG-01 (audit 2026-05-08): pre-fix dumped a JPG
                    // every 200 frames forever when the env was set. At 15 fps
                    // that's one file every ~13 seconds → ~6500 files/day per
                    // camera. With multi-camera deployments and someone
                    // forgetting the env var was on, the diag dir filled the
                    // disk in days. Cap total dumps per process.
                    static thread_local int diag_dumps_done = 0;
                    constexpr int kMaxDiagDumps = 50;
                    if (dump_frames && diag_dumps_done < kMaxDiagDumps
                        && (frames_processed % 200 == 0)) {
                        cv::Scalar mean = cv::mean(frame);
                        std::cerr << "[AI-Worker-" << camera_id << "] frame mean BGR=("
                                  << mean[0] << "," << mean[1] << "," << mean[2]
                                  << ") size=" << frame.cols << "x" << frame.rows
                                  << " type=" << frame.type() << std::endl;
                        std::string fname = "diag_cam" + std::to_string(camera_id)
                                          + "_frame" + std::to_string(frames_processed) + ".jpg";
                        cv::imwrite(fname, frame);
                        ++diag_dumps_done;
                        if (diag_dumps_done == kMaxDiagDumps) {
                            std::cerr << "[AI-Worker-" << camera_id
                                      << "] diag dump cap reached (" << kMaxDiagDumps
                                      << " files); set VMS_AI_DUMP_FRAMES=0 + restart to clear" << std::endl;
                        }
                    }
                }

                // Run Inference (locked to prevent race with ZMQ embedImage)
                std::unique_lock<std::mutex> lock(infer_mutex);
                auto result = inferer->infer(frame, static_cast<uint32_t>(current_frame_id));
                lock.unlock();
                
                // Prepare JSON Output
                json j_out;
                j_out["frame_id"] = current_frame_id;
                j_out["objects"] = json::array();

                std::vector<inference::TrackedObject> tracked_objects;
                tracked_objects.reserve(result.objects.size());

                // Objects — collect filtered detections then track in one pass.
                //
                // BBOX-VISIBILITY FIX: previous filter only kept person/bicycle/car/motorcycle
                // (COCO 0/1/2/3). For surveillance cameras pointed at a desk, parking lot,
                // shop floor, etc., YOLO11 detects backpack/cup/laptop/chair/etc. → ALL of
                // them got dropped → no bbox at all even when AI was running fine.
                //
                // We now allow ALL COCO classes by default. Set VMS_AI_CLASS_FILTER to a
                // comma-separated list (e.g. "0,1,2,3,5,7") to restrict to specific classes.
                {
                    // Default: chỉ detect người + phương tiện (COCO 0/1/2/3/5/7).
                    // Set VMS_AI_CLASS_FILTER="" để cho phép tất cả 80 COCO classes,
                    // hoặc danh sách "0,2,5" để giới hạn cụ thể hơn.
                    static const std::vector<int> class_filter = []() {
                        std::vector<int> filter;
                        const char* env = std::getenv("VMS_AI_CLASS_FILTER");
                        if (env) {
                            // Env present (kể cả rỗng) → user override hoàn toàn
                            std::string s(env);
                            size_t pos = 0;
                            while (pos < s.size()) {
                                size_t comma = s.find(',', pos);
                                std::string tok = s.substr(pos, comma == std::string::npos ? std::string::npos : comma - pos);
                                try { filter.push_back(std::stoi(tok)); } catch (...) {}
                                if (comma == std::string::npos) break;
                                pos = comma + 1;
                            }
                        } else {
                            // Default: person, bicycle, car, motorcycle, bus, truck
                            filter = {0, 1, 2, 3, 5, 7};
                        }
                        return filter;
                    }();
                    // 2026-05-08 (Fix-B): flipped default OFF (use tracker).
                    //   Originally default ON because ObjectTracker.update() was seen
                    //   to silently crash on the first detection frame. The try/catch
                    //   below catches that crash and falls back to bypass_fill() for
                    //   the failing frame, so re-enabling the tracker is now safe
                    //   even if the underlying instability is still present. With
                    //   bypass=true every transient false-positive surfaced (track_id=
                    //   -1, no temporal filter); turning the tracker on gives
                    //   short-lived hallucinations a chance to die before publishing.
                    //   Set VMS_AI_BYPASS_TRACKER=1 to opt back into the legacy
                    //   bypass behaviour for diagnostics.
                    static const bool bypass_tracker = []() {
                        const char* e = std::getenv("VMS_AI_BYPASS_TRACKER");
                        if (!e || !*e) return false;
                        return std::atoi(e) != 0;
                    }();

                    // 2026-05-08 (Fix-B): minimum person bbox height filter.
                    //   YOLO at any conf threshold below ~0.6 produces a long tail
                    //   of small false-positive person detections on background
                    //   texture (vegetation, fence noise, distant shadow). Cameras
                    //   in this deployment are mounted at human-scale; a real
                    //   person fewer than ~40 px tall is either too far for face
                    //   recognition to be useful or noise. Per-camera override via
                    //   VMS_MIN_PERSON_HEIGHT_PX.
                    static const float min_person_height_px = []() {
                        const char* e = std::getenv("VMS_MIN_PERSON_HEIGHT_PX");
                        return (e && *e) ? std::strtof(e, nullptr) : 40.0f;
                    }();

                    std::vector<inference::BBox> det_bboxes;
                    size_t dropped_small_persons = 0;
                    for (const auto& obj : result.objects) {
                        if (!class_filter.empty()) {
                            bool allowed = false;
                            for (int c : class_filter) if (obj.class_id == c) { allowed = true; break; }
                            if (!allowed) continue;
                        }
                        // Person-class minimum height check (class_id 0 in COCO).
                        if (obj.class_id == 0 && (obj.y2 - obj.y1) < min_person_height_px) {
                            ++dropped_small_persons;
                            continue;
                        }
                        det_bboxes.push_back(obj);
                    }
                    if (dropped_small_persons > 0) {
                        static thread_local int diag_small = 0;
                        if ((diag_small++ % 30) == 0) {
                            std::cerr << "[AI-Worker-" << camera_id
                                      << "] dropped " << dropped_small_persons
                                      << " sub-" << min_person_height_px << "px person detections" << std::endl;
                        }
                    }

                    auto bypass_fill = [&](std::vector<inference::TrackedObject>& out) {
                        out.reserve(det_bboxes.size());
                        for (const auto& bb : det_bboxes) {
                            inference::TrackedObject t;
                            t.bbox = bb;
                            t.confidence = bb.score;
                            t.track_id = -1;
                            t.label = bb.label.empty() ? getClassName(bb.class_id) : bb.label;
                            t.class_id = bb.class_id;
                            out.push_back(t);
                        }
                    };

                    std::vector<inference::TrackedObject> tracked;
                    if (bypass_tracker) {
                        bypass_fill(tracked);
                    } else {
                        try {
                            tracked = objectTracker.update(det_bboxes);
                        } catch (const std::exception& e) {
                            std::cerr << "[AI-Worker-" << camera_id
                                      << "] ObjectTracker.update EXCEPTION: " << e.what()
                                      << " — falling back to bypass for this frame" << std::endl;
                            tracked.clear();
                            bypass_fill(tracked);
                        } catch (...) {
                            std::cerr << "[AI-Worker-" << camera_id
                                      << "] ObjectTracker.update UNKNOWN exception"
                                      << " — falling back to bypass for this frame" << std::endl;
                            tracked.clear();
                            bypass_fill(tracked);
                        }
                    }

                    // Diagnostic: log every 30 inferences det_bboxes vs tracked
                    // (was 200 → too rare to debug missing-bbox issues).
                    {
                        static thread_local int diag_n = 0;
                        if ((diag_n++ % 30) == 0) {
                            std::cerr << "[AI-Worker-" << camera_id
                                      << "] result.objects=" << result.objects.size()
                                      << " det_bboxes=" << det_bboxes.size()
                                      << " tracked=" << tracked.size()
                                      << " bypass=" << (bypass_tracker ? 1 : 0) << std::endl;
                        }
                    }

                    for (auto& to : tracked) {
                        to.confidence = to.bbox.score;
                        to.label = getClassName(to.bbox.class_id);
                        tracked_objects.push_back(to);

                        j_out["objects"].push_back({
                            {"label", to.label},
                            {"confidence", to.confidence},
                            {"track_id", to.track_id},
                            {"box", {to.bbox.x1, to.bbox.y1, to.bbox.x2, to.bbox.y2}}
                        });
                    }
                }

                // 2026-05-08 (Fix-B): minimum face side filter — drop SCRFD
                //   detections whose shorter side is below min_face_side_px.
                //   Tiny "faces" on road texture / wall pattern are the
                //   primary source of identity-match false positives; ArcFace
                //   embeddings produced from < 30×30 px crops are not
                //   meaningful even when the patch happens to be a real face.
                //   Per-camera override via VMS_MIN_FACE_SIZE_PX.
                static const float min_face_side_px = []() {
                    const char* e = std::getenv("VMS_MIN_FACE_SIZE_PX");
                    return (e && *e) ? std::strtof(e, nullptr) : 30.0f;
                }();

                if (!result.faces.empty()) {
                    size_t before_min = result.faces.size();
                    result.faces.erase(
                        std::remove_if(result.faces.begin(), result.faces.end(),
                                       [&](const inference::FaceDetectionResult& f) {
                                           const float w = f.x2 - f.x1;
                                           const float h = f.y2 - f.y1;
                                           return std::min(w, h) < min_face_side_px;
                                       }),
                        result.faces.end());
                    size_t after_min = result.faces.size();
                    if (before_min != after_min) {
                        static thread_local int diag_size = 0;
                        if ((diag_size++ % 30) == 0) {
                            std::cerr << "[AI-Worker-" << camera_id
                                      << "] face min-size: dropped " << (before_min - after_min)
                                      << "/" << before_min << " (< " << min_face_side_px << "px)" << std::endl;
                        }
                    }
                }

                // Faces — POST-FILTER: chỉ giữ face nằm trong person bbox.
                // SCRFD threshold 0.55 vẫn có thể detect nhầm trên texture đường/tường
                // (logo, cửa sổ, vạch sơn...). Vì face PHẢI thuộc một người, ta chỉ
                // giữ face nếu center hoặc >50% diện tích face nằm trong vùng
                // người đã được YOLO confirm. Set VMS_SCRFD_REQUIRE_PERSON=0 nếu
                // muốn detect face trong scene không có YOLO person (vd snapshot
                // tài liệu, ảnh thẻ).
                static const bool require_person_overlap = []() {
                    const char* e = std::getenv("VMS_SCRFD_REQUIRE_PERSON");
                    if (!e || !*e) return true;
                    return std::atoi(e) != 0;
                }();

                if (require_person_overlap && !result.faces.empty()) {
                    // Collect person bboxes (class 0) from result.objects (pre-tracker)
                    std::vector<std::tuple<float,float,float,float>> person_boxes;
                    for (const auto& obj : result.objects) {
                        if (obj.class_id != 0) continue; // chỉ lấy "person"
                        person_boxes.emplace_back(obj.x1, obj.y1, obj.x2, obj.y2);
                    }

                    auto inside_any_person = [&](const inference::FaceDetectionResult& f) -> bool {
                        if (person_boxes.empty()) return false;
                        const float fcx = (f.x1 + f.x2) * 0.5f;
                        const float fcy = (f.y1 + f.y2) * 0.5f;
                        const float farea = std::max(1e-3f, (f.x2 - f.x1) * (f.y2 - f.y1));
                        for (const auto& [px1, py1, px2, py2] : person_boxes) {
                            // Center test (bắt face đầu nhô lên trên đầu person bbox)
                            // OR overlap >= 50% diện tích face
                            const bool center_in = (fcx >= px1 && fcx <= px2 && fcy >= py1 && fcy <= py2);
                            const float ix1 = std::max(f.x1, px1), iy1 = std::max(f.y1, py1);
                            const float ix2 = std::min(f.x2, px2), iy2 = std::min(f.y2, py2);
                            const float inter = std::max(0.0f, ix2 - ix1) * std::max(0.0f, iy2 - iy1);
                            if (center_in || inter / farea >= 0.5f) return true;
                        }
                        return false;
                    };

                    size_t before = result.faces.size();
                    result.faces.erase(
                        std::remove_if(result.faces.begin(), result.faces.end(),
                                       [&](const inference::FaceDetectionResult& f) {
                                           return !inside_any_person(f);
                                       }),
                        result.faces.end());
                    size_t after = result.faces.size();
                    if (before != after) {
                        static thread_local int diag_drop = 0;
                        if ((diag_drop++ % 30) == 0) {
                            std::cerr << "[AI-Worker-" << camera_id
                                      << "] face post-filter: dropped " << (before - after)
                                      << "/" << before << " (no person overlap)" << std::endl;
                        }
                    }
                }

                auto tracked_faces = faceTracker.update(result.faces);

                // Back-propagate stable identity into result.faces before SHM write.
                // Per-frame recognition fluctuates; stable_person_id is accumulated over
                // multiple frames by FaceTracker and is more reliable than the raw match.
                for (const auto& tf : tracked_faces) {
                    if (tf.stable_person_id == -1) continue;
                    float best_iou = 0.3f; // minimum threshold
                    int best_idx = -1;
                    for (int fi = 0; fi < (int)result.faces.size(); fi++) {
                        const auto& rf = result.faces[fi];
                        float ix1 = std::max(tf.x1, rf.x1), iy1 = std::max(tf.y1, rf.y1);
                        float ix2 = std::min(tf.x2, rf.x2), iy2 = std::min(tf.y2, rf.y2);
                        if (ix2 <= ix1 || iy2 <= iy1) continue;
                        float inter = (ix2 - ix1) * (iy2 - iy1);
                        float a = (tf.x2 - tf.x1) * (tf.y2 - tf.y1);
                        float b = (rf.x2 - rf.x1) * (rf.y2 - rf.y1);
                        float iou = inter / (a + b - inter);
                        if (iou > best_iou) { best_iou = iou; best_idx = fi; }
                    }
                    if (best_idx >= 0) {
                        result.faces[best_idx].person_id = tf.stable_person_id;
                        result.faces[best_idx].name = tf.stable_name;
                    }
                }

                for (const auto& face : tracked_faces) {
                    inference::TrackedObject to;
                    to.bbox.x1 = face.x1; to.bbox.y1 = face.y1;
                    to.bbox.x2 = face.x2; to.bbox.y2 = face.y2;
                    to.confidence = face.confidence;
                    to.bbox.score = face.confidence;
                    to.bbox.class_id = 100;
                    to.label = (!face.stable_name.empty() && face.stable_name != "Face")
                                   ? face.stable_name
                                   : ((!face.name.empty() && face.name != "Unknown") ? face.name : "Face");
                    to.track_id = face.track_id;
                    tracked_objects.push_back(to);

                    int pid = (face.stable_person_id != -1) ? face.stable_person_id : face.person_id;
                    j_out["objects"].push_back({
                        {"label", to.label},
                        {"person_id", pid},
                        {"confidence", to.confidence},
                        {"track_id", to.track_id},
                        {"box", {to.bbox.x1, to.bbox.y1, to.bbox.x2, to.bbox.y2}}
                    });
                }
                
                // License Plates
                for (const auto& plate : result.plates) {
                    inference::TrackedObject to;
                    to.bbox.x1 = plate.x1; to.bbox.y1 = plate.y1; to.bbox.x2 = plate.x2; to.bbox.y2 = plate.y2;
                    to.confidence = plate.confidence;
                    to.bbox.score = plate.confidence;   // SHM relies on this
                    to.bbox.class_id = 200;             // Distinct class ID for LPR
                    to.label = "LicensePlate";
                    to.track_id = -1;
                    tracked_objects.push_back(to);
                    
                    j_out["objects"].push_back({
                        {"label", to.label},
                        {"confidence", to.confidence},
                        {"text", plate.text}, // Custom field for LPR
                        {"track_id", to.track_id},
                        {"box", {to.bbox.x1, to.bbox.y1, to.bbox.x2, to.bbox.y2}}
                    });
                }
                
                // OUTPUT JSON TO STDOUT
                std::cout << j_out.dump() << std::endl;

                // Sync to SHM (keep existing logic)
                shm->writeMetadata(result, tracked_objects, camera_id, frame.cols, frame.rows);
                
                frames_processed++;
                
                // FPS Log to STDERR
                if (frames_processed % 100 == 0) {
                    auto now = std::chrono::steady_clock::now();
                    auto duration = std::chrono::duration_cast<std::chrono::seconds>(now - loop_start).count();
                    double fps = (duration > 0) ? static_cast<double>(frames_processed) / duration : 0.0;
                    std::cerr << "[AI-Worker-" << camera_id << "] FPS: " << std::fixed << std::setprecision(1) << fps << std::endl;
                }
                
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
                
            } else {
                std::this_thread::sleep_for(std::chrono::milliseconds(20));
            }
            
        } catch (const std::exception& e) {
            std::cerr << "[AI-Worker-" << camera_id << "] Loop Exception: " << e.what() << std::endl;
            ++consecutive_failures;
            if (consecutive_failures >= kMaxConsecutiveFailures) {
                std::cerr << "[AI-Worker-" << camera_id
                          << "] FATAL: " << consecutive_failures
                          << " consecutive inference exceptions — exiting so the manager can restart us"
                          << std::endl;
                g_running = 0;
                break;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            continue;
        }
        // Reset on any successful loop iteration. Placed at the END of the
        // try-block path so we only credit "real" frames — frames where shm
        // had no data fall through the else-branch above without resetting,
        // which is fine: the failure counter only matters when we actually
        // tried to do work and it threw.
        consecutive_failures = 0;
    }

    // BUG-003 FIX: Join ZMQ thread before inferer is destroyed
    g_running = 0; // Ensure ZMQ thread exits its loop
    if (zmq_thread.joinable()) {
        zmq_thread.join();
    }
    
    std::cerr << "[AI-Worker-" << camera_id << "] Stopped." << std::endl;
    return 0;
}