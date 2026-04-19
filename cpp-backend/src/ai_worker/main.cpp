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

// ZMQ Thread Function
void zmq_rep_thread(int camera_id, inference::MultiModelInfer* inferer, std::mutex* infer_mutex) {
    try {
        zmq::context_t context(1);
        zmq::socket_t socket(context, zmq::socket_type::rep);
        
        // PORT CONFLICT FIX: Enable socket reuse and proper cleanup
        int linger_ms = 0; // Immediately close socket on exit
        socket.set(zmq::sockopt::linger, linger_ms);
        
        int port = 5560 + camera_id;
        std::string endpoint = "tcp://*:" + std::to_string(port);
        
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
                    if (!img_base64.empty()) {
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
                    } else {
                        reply_str = "{\"status\": \"error\", \"message\": \"Empty image data\"}";
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
    ai_config.yolo_conf_threshold = 0.30f;
    ai_config.yolo_nms_threshold = 0.45f;
    
    ai_config.enable_face_detection = true; // SCRFD
    ai_config.enable_face_recognition = true; // ArcFace
    
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
    std::thread zmq_thread(zmq_rep_thread, camera_id, inferer.get(), &infer_mutex);
    
    std::cerr << "[AI-Worker-" << camera_id << "] Ready processing loop." << std::endl;
    
    // ========================================
    // 5. Main Processing Loop
    // ========================================
    cv::Mat frame;
    uint64_t last_frame_id = 0;
    uint64_t timestamp = 0;
    uint64_t frames_processed = 0;
    
    auto loop_start = std::chrono::steady_clock::now();
    
    // Instantiate Face Tracker for smoothing and ID persistence
    inference::FaceTracker faceTracker(0.3f, 30, 2); 
    
    
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
                
                // Objects
                for (const auto& obj : result.objects) {
                    // ✅ OPTIMIZATION: Filter to only person, car, motorcycle, bicycle
                    // class_id (COCO): 0=person, 1=bicycle, 2=car, 3=motorcycle
                    if (obj.class_id != 0 && obj.class_id != 1 && obj.class_id != 2 && obj.class_id != 3) {
                        continue;  // Skip all other classes
                    }
                    
                    inference::TrackedObject to;
                    to.bbox = obj;
                    to.confidence = obj.score;
                    to.label = getClassName(obj.class_id);
                    to.track_id = -1; 
                    tracked_objects.push_back(to);
                    
                    // JSON
                    j_out["objects"].push_back({
                        {"label", to.label},
                        {"confidence", to.confidence},
                        {"track_id", to.track_id},
                        {"box", {to.bbox.x1, to.bbox.y1, to.bbox.x2, to.bbox.y2}}
                    });
                }

                // Faces - Process through tracker for smoothing and stabilization
                auto tracked_faces = faceTracker.update(result.faces);
                
                for (const auto& face : tracked_faces) {
                    inference::TrackedObject to;
                    to.bbox.x1 = face.x1; to.bbox.y1 = face.y1; to.bbox.x2 = face.x2; to.bbox.y2 = face.y2;
                    to.confidence = face.confidence;
                    to.bbox.score = face.confidence;    // SHM relies on this
                    to.bbox.class_id = 100;             // Distinct class ID for Face
                    to.label = (!face.name.empty() && face.name != "Unknown") ? face.name : "Face";
                    to.track_id = face.track_id;
                    tracked_objects.push_back(to);
                    
                    // JSON
                    j_out["objects"].push_back({
                        {"label", to.label},
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
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
    }
    
    // BUG-003 FIX: Join ZMQ thread before inferer is destroyed
    g_running = 0; // Ensure ZMQ thread exits its loop
    if (zmq_thread.joinable()) {
        zmq_thread.join();
    }
    
    std::cerr << "[AI-Worker-" << camera_id << "] Stopped." << std::endl;
    return 0;
}