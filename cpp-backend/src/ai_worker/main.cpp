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

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <dbghelp.h>
#endif

#include "../ai/ipc/shared_memory_manager.h"
#include "../ai/inference/include/inference/multi_model_infer.h"
#include "../ai/inference/include/inference/tracking.h"



using json = nlohmann::json;

// Global flag
volatile std::sig_atomic_t g_running = 1;

// ============================================================================
// CRASH HANDLER — writes a full minidump on AV / heap corruption / SEH so the
// caller (process_manager / camera_pipeline_manager) can post-mortem the
// process instead of seeing only the bare exit code (-1073741819 = 0xC0000005
// ACCESS_VIOLATION). Belt-and-suspenders with WER LocalDumps (set up by
// scripts/setup_wer_aiworker_dumps.ps1): WER needs admin + registry; this
// path needs neither and runs even on dev machines without HKLM access.
// Output: logs/ai_worker_cam<id>_pid<pid>_<ts>.dmp next to vms_backend.exe.
// ============================================================================
#ifdef _WIN32
static int g_crash_camera_id = 0;

static LONG WINAPI ai_worker_crash_handler(EXCEPTION_POINTERS* eptr) {
    DWORD code = eptr ? eptr->ExceptionRecord->ExceptionCode : 0;
    void* addr  = eptr ? eptr->ExceptionRecord->ExceptionAddress : nullptr;

    // STATUS_BREAKPOINT / STATUS_SINGLE_STEP — usually a debugger artifact or
    // a CRT debug assertion. Terminating the worker on those would mask the
    // real bug; mirror what backend main.cpp does and continue execution.
    if (code == 0x80000003 || code == 0x80000004) {
        std::cerr << "[AI-Worker-" << g_crash_camera_id
                  << "] Breakpoint/single-step at 0x" << std::hex
                  << reinterpret_cast<uintptr_t>(addr) << std::dec
                  << " — continuing" << std::endl;
        return EXCEPTION_CONTINUE_EXECUTION;
    }

    // Best-effort dump path: <cwd>/logs/ai_worker_cam<id>_pid<pid>_<ts>.dmp.
    // The worker's CWD is set by the spawning manager to the directory holding
    // vms_backend.exe (build/Release/) so dumps land alongside cpp_backend.log.
    CreateDirectoryA("logs", nullptr);
    CreateDirectoryA("logs\\crashdumps", nullptr);

    SYSTEMTIME st;
    GetLocalTime(&st);
    char dump_path[MAX_PATH];
    _snprintf_s(dump_path, sizeof(dump_path), _TRUNCATE,
                "logs\\crashdumps\\ai_worker_cam%d_pid%lu_%04d%02d%02d_%02d%02d%02d.dmp",
                g_crash_camera_id,
                static_cast<unsigned long>(GetCurrentProcessId()),
                st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);

    HANDLE hFile = CreateFileA(dump_path, GENERIC_WRITE, 0, nullptr,
                               CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    bool wrote = false;
    if (hFile != INVALID_HANDLE_VALUE) {
        MINIDUMP_EXCEPTION_INFORMATION mei{};
        mei.ThreadId          = GetCurrentThreadId();
        mei.ExceptionPointers = eptr;
        mei.ClientPointers    = FALSE;

        // Full snapshot: data segments, threads, handles, unloaded modules.
        // Heap is ~hundreds of MB at runtime (TRT engines hold it), but for
        // bisecting a real AV that is exactly what we need; DumpCount in the
        // WER reg gate at 10 keeps disk usage bounded over time.
        const MINIDUMP_TYPE type = static_cast<MINIDUMP_TYPE>(
            MiniDumpWithDataSegs | MiniDumpWithThreadInfo |
            MiniDumpWithProcessThreadData | MiniDumpWithHandleData |
            MiniDumpWithUnloadedModules);
        wrote = MiniDumpWriteDump(GetCurrentProcess(), GetCurrentProcessId(),
                                  hFile, type, &mei, nullptr, nullptr) != 0;
        CloseHandle(hFile);
    }

    std::cerr << "[AI-Worker-" << g_crash_camera_id
              << "] CRASH code=0x" << std::hex << code
              << " addr=0x" << reinterpret_cast<uintptr_t>(addr)
              << std::dec
              << " minidump=" << (wrote ? dump_path : "FAILED")
              << std::endl;

    return EXCEPTION_EXECUTE_HANDLER; // process terminates after we return
}

static void install_ai_worker_crash_handler(int camera_id) {
    g_crash_camera_id = camera_id;
    SetUnhandledExceptionFilter(ai_worker_crash_handler);
}
#else
static void install_ai_worker_crash_handler(int) {}
#endif

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

    // Install crash handler IMMEDIATELY after we know camera_id — before any
    // model load, before SHM init. Anything that AVs from here on writes a
    // minidump tagged with this camera id.
    install_ai_worker_crash_handler(camera_id);

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
    //   the worst false-positive flood.
    // 2026-05-08 (Fix-B²): operator reports 0.30 STILL too noisy ("yolo phải
    //   nâng cao thêm nữa"). Bumped to 0.45 — comfortably above the
    //   "vaguely person-shaped object scores 0.30-0.40" band that COCO
    //   YOLO produces for trash cans / posts / vertical shadows. Recall on
    //   confident, near-camera persons stays > 90% in our test scenes;
    //   distant/partial persons (< 60-70 px tall) lose ~35% recall — that
    //   long tail should already be handled by VMS_MIN_PERSON_HEIGHT_PX
    //   (default 40) so the recall hit is not on the operationally useful
    //   range. If a deployment really needs lower, set
    //   VMS_YOLO_CONF_THRESHOLD explicitly per camera.
    {
        const char* env = std::getenv("VMS_YOLO_CONF_THRESHOLD");
        ai_config.yolo_conf_threshold = (env && *env) ? std::strtof(env, nullptr) : 0.45f;
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

    // 2026-05-19 PPE — disabled by default; operator opts in via cmdline
    // JSON {"ppe":true} or env VMS_AI_ENABLE_PPE=1. Engine ~55 MB; class
    // mapping per PPEClass enum (see header comment in MultiModelInfer).
    // Conf threshold env-tunable: PPE-YOLOv8m calibration differs from
    // COCO YOLO; 0.45 may be too strict for some training configs.
    // Operator dials VMS_PPE_CONF_THRESHOLD=0.20 etc. without rebuild.
    ai_config.enable_ppe = false;
    ai_config.ppe_model_path = resolveModelPath("PPE-YOLOv8m.engine", exe_path);
    {
        const char* env = std::getenv("VMS_PPE_CONF_THRESHOLD");
        if (env && *env) {
            float v = std::strtof(env, nullptr);
            if (v > 0.0f && v < 1.0f) {
                ai_config.ppe_conf_threshold = v;
                std::cerr << "[AI-Worker-" << camera_id
                          << "] VMS_PPE_CONF_THRESHOLD=" << v << std::endl;
            }
        }
    }

    // LPR - Disabled for optimization
    ai_config.enable_lpr = false;
    ai_config.plate_detect_model_path = resolveModelPath("yolov8n_plate.onnx", exe_path);
    ai_config.lpr_model_path = resolveModelPath("lprnet.onnx", exe_path);
    
    // Parse Config from Arguments (Index 4)
    // j_config is lifted out of the try{} so per-camera Path-1 filter
    // overrides (resolved further below) can read it.
    nlohmann::json j_config = nlohmann::json::object();
    if (argc > 4) {
        std::string config_str = argv[4];
        try {
            std::cerr << "[AI-Worker-" << camera_id << "] Parsing config: " << config_str << std::endl;
            j_config = json::parse(config_str);

            if (j_config.contains("yolo")) ai_config.enable_yolo = j_config["yolo"];
            if (j_config.contains("face")) {
                ai_config.enable_face_detection = j_config["face"];
                ai_config.enable_face_recognition = j_config["face"];
            }
            if (j_config.contains("lpr")) ai_config.enable_lpr = j_config["lpr"];
            if (j_config.contains("fire")) ai_config.enable_fire_detection = j_config["fire"];
            if (j_config.contains("ppe")) ai_config.enable_ppe = j_config["ppe"];
            if (j_config.contains("face_match_threshold")) {
                ai_config.face_match_threshold = j_config["face_match_threshold"].get<float>();
                std::cerr << "[AI-Worker-" << camera_id << "] Custom face_match_threshold: "
                          << ai_config.face_match_threshold << std::endl;
            }
        } catch (const std::exception& e) {
             std::cerr << "[AI-Worker-" << camera_id << "] Error parsing config: " << e.what() << std::endl;
             j_config = nlohmann::json::object();
        }
    }

    // 2026-05-22 Path-1 per-camera filter overrides.
    // Precedence: per-camera ai_config JSON > env var > hardcoded default.
    // Operator complaint loop: global env defaults (aspect 1.5, height 20,
    // face 50) were appropriate for doorway/frontal cams but dropped every
    // detection on angled/overhead cams where persons appear with h/w<1.5.
    // Per-camera override lets operator dial each camera's filters via UI
    // without touching env or rebuild. Filter values live in the same
    // ai_config JSON column that already carries yolo/face/lpr/fire/ppe
    // toggles, set by CameraConfigModal.
    auto resolveFloatFilter = [&](const char* json_key, const char* env_name, float def) -> float {
        if (j_config.contains(json_key) && j_config[json_key].is_number()) {
            return j_config[json_key].get<float>();
        }
        const char* e = std::getenv(env_name);
        if (e && *e) return std::strtof(e, nullptr);
        return def;
    };
    const float min_person_height_px    = resolveFloatFilter("min_person_height_px",    "VMS_MIN_PERSON_HEIGHT_PX",   20.0f);
    const float min_person_aspect_ratio = resolveFloatFilter("min_person_aspect_ratio", "VMS_MIN_PERSON_ASPECT_RATIO", 1.5f);
    const float min_face_side_px        = resolveFloatFilter("min_face_size_px",        "VMS_MIN_FACE_SIZE_PX",        50.0f);
    std::cerr << "[AI-Worker-" << camera_id << "] Path-1 filters: "
              << "min_person_height_px=" << min_person_height_px
              << " min_person_aspect_ratio=" << min_person_aspect_ratio
              << " min_face_size_px=" << min_face_side_px << std::endl;

    // Env-var bisect levers — applied AFTER JSON config so they always win.
    // Use case: cmdline JSON escape is broken (the {"yolo":...} arg arrives as
    // {\yolo\:...}) so JSON parse falls through to defaults; or operator wants
    // to disable a specific model without touching the cameras.ai_config DB
    // column. Each *_DISABLE flag forces the feature OFF unconditionally.
    auto env_truthy = [](const char* name) {
        const char* v = std::getenv(name);
        return v && *v && std::atoi(v) != 0;
    };
    if (env_truthy("VMS_AI_DISABLE_YOLO")) {
        ai_config.enable_yolo = false;
        std::cerr << "[AI-Worker-" << camera_id << "] VMS_AI_DISABLE_YOLO=1 — yolo OFF" << std::endl;
    }
    if (env_truthy("VMS_AI_DISABLE_FACE")) {
        ai_config.enable_face_detection   = false;
        ai_config.enable_face_recognition = false;
        std::cerr << "[AI-Worker-" << camera_id << "] VMS_AI_DISABLE_FACE=1 — face detect+recog OFF" << std::endl;
    }
    if (env_truthy("VMS_AI_DISABLE_LPR")) {
        ai_config.enable_lpr = false;
        std::cerr << "[AI-Worker-" << camera_id << "] VMS_AI_DISABLE_LPR=1 — lpr OFF" << std::endl;
    }
    if (env_truthy("VMS_AI_DISABLE_FIRE")) {
        ai_config.enable_fire_detection = false;
        std::cerr << "[AI-Worker-" << camera_id << "] VMS_AI_DISABLE_FIRE=1 — fire OFF" << std::endl;
    }
    // 2026-05-19 PPE env override (parallel to other DISABLE/ENABLE knobs).
    // VMS_AI_ENABLE_PPE=1 force-on; VMS_AI_DISABLE_PPE=1 force-off.
    if (env_truthy("VMS_AI_ENABLE_PPE")) {
        ai_config.enable_ppe = true;
        std::cerr << "[AI-Worker-" << camera_id << "] VMS_AI_ENABLE_PPE=1 — ppe ON" << std::endl;
    }
    if (env_truthy("VMS_AI_DISABLE_PPE")) {
        ai_config.enable_ppe = false;
        std::cerr << "[AI-Worker-" << camera_id << "] VMS_AI_DISABLE_PPE=1 — ppe OFF" << std::endl;
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

                    // Person bbox geometric filters (min_person_height_px,
                    // min_person_aspect_ratio) resolved once at startup —
                    // see Path-1 per-camera filter block above for precedence
                    // (per-camera JSON > env > default).
                    std::vector<inference::BBox> det_bboxes;
                    size_t dropped_small_persons = 0;
                    size_t dropped_low_aspect_persons = 0;
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
                        // Person-class aspect-ratio check (h/w must exceed
                        // threshold, default 1.5). Geometric prior; gates
                        // wide-blob FPs that no class-score tuning fixes.
                        if (obj.class_id == 0 && min_person_aspect_ratio > 0.0f) {
                            const float w = obj.x2 - obj.x1;
                            const float h = obj.y2 - obj.y1;
                            if (w > 1.0f && (h / w) < min_person_aspect_ratio) {
                                ++dropped_low_aspect_persons;
                                continue;
                            }
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
                    if (dropped_low_aspect_persons > 0) {
                        static thread_local int diag_aspect = 0;
                        if ((diag_aspect++ % 30) == 0) {
                            std::cerr << "[AI-Worker-" << camera_id
                                      << "] dropped " << dropped_low_aspect_persons
                                      << " low-aspect persons (h/w < "
                                      << min_person_aspect_ratio << ")" << std::endl;
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

                // Face min-side filter (min_face_side_px) resolved once at
                // startup — see Path-1 per-camera filter block above for
                // precedence (per-camera JSON > env > default).
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

                // BUG-AIW-FACETRACKER-CATCH (debug 2026-05-10): pre-fix this
                // call had no exception guard, so a std::exception thrown from
                // FaceTracker (e.g. on malformed result.faces with NaN coords)
                // unwound straight through the loop's catch and lost the per-
                // frame context. The objectTracker block above already has the
                // same guard for the same reason. AV is still caught by the
                // SetUnhandledExceptionFilter installed in main().
                std::vector<inference::TrackedFace> tracked_faces;
                try {
                    tracked_faces = faceTracker.update(result.faces);
                } catch (const std::exception& e) {
                    std::cerr << "[AI-Worker-" << camera_id
                              << "] FaceTracker.update EXCEPTION: " << e.what()
                              << " — skipping face tracking this frame" << std::endl;
                    tracked_faces.clear();
                } catch (...) {
                    std::cerr << "[AI-Worker-" << camera_id
                              << "] FaceTracker.update UNKNOWN exception"
                              << " — skipping face tracking this frame" << std::endl;
                    tracked_faces.clear();
                }

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
                
                // 2026-05-19 PPE detections — engine classes confirmed via
                // best.pt model.names: ['helmet','vest','person','gloves',
                // 'mask','boots']. This is a DETECTION-only model (no
                // NoHelmet/NoVest violation classes). Violation must be
                // DERIVED — for each person bbox, find overlapping helmet
                // and vest bboxes; if missing → that person is violating.
                //
                // Per-frame pairing happens in this loop because we have
                // the full result.ppe_objects vector here. We build two
                // synthetic event class_ids on the AI-worker side:
                //   406 = NoHelmet violation (person bbox without overlap)
                //   407 = NoVest violation   (person bbox without overlap)
                // And forward the raw detections too with 300-offset
                // namespace (for visibility / debug overlays).
                //
                // Class id remap (raw items, 300-offset to avoid COCO):
                //   raw class N → 300 + N
                // Synthetic violation ids (derived, 400-offset):
                //   406 NoHelmet, 407 NoVest
                //
                // 2026-05-20 labels + class indices come from
                // VMS_AI_PPE_CONFIG_JSON (set by parent vms_backend from
                // backend.yaml `ai.ppe.*` block). Cached once per worker
                // — env reads at first use; subsequent calls hit the
                // static struct. Operator retraining the engine with a
                // different class order edits yaml + restarts backend;
                // no source change / rebuild needed.
                struct PpeRuntimeConfig {
                    std::vector<std::string> labels;
                    int person_class;
                    int helmet_class;
                    int vest_class;
                    int gloves_class;   // 2026-05-21 added (operator request B)
                    int mask_class;
                    int boots_class;
                };
                static const PpeRuntimeConfig kPpeRuntime = []() {
                    PpeRuntimeConfig cfg;
                    // Defaults match the 2026-05-19 PPE-YOLOv8m engine — kept
                    // identical to the pre-2026-05-20 hardcoded values so a
                    // deployment without yaml's `ai.ppe.*` block sees zero
                    // behaviour change for the helmet/vest channels. The
                    // gloves/mask/boots channels are new in 2026-05-21 and
                    // are enabled by default per operator's "B" choice.
                    cfg.labels       = { "Helmet", "Vest", "PPE_Person",
                                         "Gloves", "Mask", "Boots" };
                    cfg.person_class = 2;
                    cfg.helmet_class = 0;
                    cfg.vest_class   = 1;
                    cfg.gloves_class = 3;
                    cfg.mask_class   = 4;
                    cfg.boots_class  = 5;
                    const char* env = std::getenv("VMS_AI_PPE_CONFIG_JSON");
                    if (!env || !*env) return cfg;
                    try {
                        auto j = json::parse(env);
                        if (j.contains("labels") && j["labels"].is_array()) {
                            std::vector<std::string> parsed;
                            parsed.reserve(j["labels"].size());
                            for (const auto& s : j["labels"]) parsed.push_back(s.get<std::string>());
                            if (!parsed.empty()) cfg.labels = std::move(parsed);
                        }
                        if (j.contains("person_class")) cfg.person_class = j["person_class"].get<int>();
                        if (j.contains("helmet_class")) cfg.helmet_class = j["helmet_class"].get<int>();
                        if (j.contains("vest_class"))   cfg.vest_class   = j["vest_class"].get<int>();
                        if (j.contains("gloves_class")) cfg.gloves_class = j["gloves_class"].get<int>();
                        if (j.contains("mask_class"))   cfg.mask_class   = j["mask_class"].get<int>();
                        if (j.contains("boots_class"))  cfg.boots_class  = j["boots_class"].get<int>();
                        std::cerr << "[AI-Worker] PPE config: labels=" << cfg.labels.size()
                                  << " person=" << cfg.person_class
                                  << " helmet=" << cfg.helmet_class
                                  << " vest="   << cfg.vest_class
                                  << " gloves=" << cfg.gloves_class
                                  << " mask="   << cfg.mask_class
                                  << " boots="  << cfg.boots_class
                                  << " (from VMS_AI_PPE_CONFIG_JSON)" << std::endl;
                    } catch (const std::exception& e) {
                        std::cerr << "[AI-Worker] WARN: VMS_AI_PPE_CONFIG_JSON parse failed: "
                                  << e.what() << " — using hardcoded defaults" << std::endl;
                    }
                    return cfg;
                }();
                auto label_for = [&](int src_cls) -> const char* {
                    if (src_cls >= 0 && src_cls < (int)kPpeRuntime.labels.size())
                        return kPpeRuntime.labels[src_cls].c_str();
                    return "PPE_Unknown";
                };

                // Collect person + all 5 wearable types for pairing.
                // *_class == -1 → channel disabled (skip pairing).
                // 2026-05-21 expanded from {helmet, vest} → {helmet, vest,
                // gloves, mask, boots}. Raw detections still flow through
                // for visibility regardless of pairing channel state.
                std::vector<const inference::BBox*> ppe_persons;
                std::vector<const inference::BBox*> ppe_helmets;
                std::vector<const inference::BBox*> ppe_vests;
                std::vector<const inference::BBox*> ppe_gloves;
                std::vector<const inference::BBox*> ppe_masks;
                std::vector<const inference::BBox*> ppe_boots;
                for (const auto& ppe : result.ppe_objects) {
                    const int src_cls = ppe.class_id;
                    if (src_cls < 0) continue;
                    if (kPpeRuntime.person_class >= 0 &&
                        src_cls == kPpeRuntime.person_class)      ppe_persons.push_back(&ppe);
                    else if (kPpeRuntime.helmet_class >= 0 &&
                             src_cls == kPpeRuntime.helmet_class) ppe_helmets.push_back(&ppe);
                    else if (kPpeRuntime.vest_class >= 0 &&
                             src_cls == kPpeRuntime.vest_class)   ppe_vests.push_back(&ppe);
                    else if (kPpeRuntime.gloves_class >= 0 &&
                             src_cls == kPpeRuntime.gloves_class) ppe_gloves.push_back(&ppe);
                    else if (kPpeRuntime.mask_class >= 0 &&
                             src_cls == kPpeRuntime.mask_class)   ppe_masks.push_back(&ppe);
                    else if (kPpeRuntime.boots_class >= 0 &&
                             src_cls == kPpeRuntime.boots_class)  ppe_boots.push_back(&ppe);
                }

                // Push raw detections into metadata channel.
                for (const auto& ppe : result.ppe_objects) {
                    const int src_cls = ppe.class_id;
                    if (src_cls < 0) continue;
                    inference::TrackedObject to;
                    to.bbox.x1 = ppe.x1; to.bbox.y1 = ppe.y1;
                    to.bbox.x2 = ppe.x2; to.bbox.y2 = ppe.y2;
                    to.confidence = ppe.score;
                    to.bbox.score = ppe.score;
                    to.bbox.class_id = 300 + src_cls;
                    to.label = label_for(src_cls);
                    to.track_id = -1;
                    tracked_objects.push_back(to);

                    j_out["objects"].push_back({
                        {"label", to.label},
                        {"class_id", to.bbox.class_id},
                        {"confidence", to.confidence},
                        {"track_id", to.track_id},
                        {"box", {to.bbox.x1, to.bbox.y1, to.bbox.x2, to.bbox.y2}}
                    });
                }

                // 2026-05-19 PPE violation pairing — for each person bbox,
                // check whether ANY helmet/vest bbox overlaps (IoU >= 0.10
                // because PPE items typically sit on/inside the person
                // bbox so a person-with-helmet has helmet bbox CONTAINED
                // inside their body bbox; geometric containment fits the
                // physical reality better than tight IoU). If no overlap
                // is found, emit synthetic NoHelmet (406) / NoVest (407)
                // detection with the person's bbox.
                //
                // Why "min overlap" not tight IoU:
                //   helmet_bbox is small relative to person_bbox, so even
                //   when the person IS wearing a helmet IoU ≈ 0.05-0.15.
                //   The right check is "PPE item bbox CONTAINED in person
                //   bbox" — use intersection / smaller_area as the metric.
                // 2026-05-21 made threshold parametric. helmet/vest/boots
                // are well-contained items (50% works). gloves often partly
                // outside the person bbox because hands extend; mask is
                // small but central — lower thresholds 0.30 / 0.40 reduce
                // false NoGloves / NoMask in real footage.
                auto contained_in = [](const inference::BBox& small,
                                       const inference::BBox& big,
                                       float min_overlap) -> bool {
                    const float ix1 = std::max(small.x1, big.x1);
                    const float iy1 = std::max(small.y1, big.y1);
                    const float ix2 = std::min(small.x2, big.x2);
                    const float iy2 = std::min(small.y2, big.y2);
                    if (ix2 <= ix1 || iy2 <= iy1) return false;
                    const float inter = (ix2 - ix1) * (iy2 - iy1);
                    const float small_area =
                        std::max(1.0f, (small.x2 - small.x1) * (small.y2 - small.y1));
                    return (inter / small_area) >= min_overlap;
                };
                // 2026-05-20 compliance telemetry: count fully-compliant
                // (has_helmet AND has_vest) vs violating PPE-persons in this
                // frame. Emitted in j_out["ppe_summary"] for the consumer to
                // roll into a per-camera rolling window. Compliant persons
                // don't fire individual events (would flood the events
                // table); the summary is the only signal the operator's
                // dashboard has for "denominator" — what fraction of
                // observed persons were OK vs violating.
                int compliant_count = 0;
                int violating_count = 0;
                for (const auto* person : ppe_persons) {
                    bool has_helmet = false, has_vest = false;
                    bool has_gloves = false, has_mask = false, has_boots = false;
                    for (const auto* h : ppe_helmets) if (contained_in(*h, *person, 0.50f)) { has_helmet = true; break; }
                    for (const auto* v : ppe_vests)   if (contained_in(*v, *person, 0.50f)) { has_vest   = true; break; }
                    for (const auto* g : ppe_gloves)  if (contained_in(*g, *person, 0.30f)) { has_gloves = true; break; }
                    for (const auto* m : ppe_masks)   if (contained_in(*m, *person, 0.40f)) { has_mask   = true; break; }
                    for (const auto* b : ppe_boots)   if (contained_in(*b, *person, 0.50f)) { has_boots  = true; break; }
                    // Compliance ignores any check the operator disabled
                    // (*_class == -1). A deployment that only enforces
                    // hard hats sees vest/gloves/mask/boots absence treated
                    // as "not measured", not as "violating".
                    const bool helmet_ok = (kPpeRuntime.helmet_class < 0) || has_helmet;
                    const bool vest_ok   = (kPpeRuntime.vest_class   < 0) || has_vest;
                    const bool gloves_ok = (kPpeRuntime.gloves_class < 0) || has_gloves;
                    const bool mask_ok   = (kPpeRuntime.mask_class   < 0) || has_mask;
                    const bool boots_ok  = (kPpeRuntime.boots_class  < 0) || has_boots;
                    if (helmet_ok && vest_ok && gloves_ok && mask_ok && boots_ok) ++compliant_count;
                    else                                                          ++violating_count;

                    // 2026-05-20 PPE-person ↔ COCO-tracked-person IoU pairing.
                    // PPE engine runs as a parallel AdvancedInfer, NOT through
                    // the ObjectTracker, so PPE persons carry track_id=-1.
                    // Match each PPE-person bbox to the best COCO-tracked
                    // person bbox via IoU and adopt that track_id so the
                    // consumer-side cooldown can bucket per stable identity
                    // instead of a 50px center grid. Threshold 0.40: two
                    // models seeing the same physical person typically agree
                    // at IoU 0.6-0.8; 0.40 catches shape disagreements
                    // (PPE tighter / COCO more generous) without collapsing
                    // across two distinct adjacent persons (typ. IoU ≤ 0.2).
                    // No match → track_id stays -1 and the consumer falls
                    // back to the 50px bucket (preserves prior behaviour
                    // when COCO YOLO is disabled OR PPE / COCO disagree on
                    // person presence).
                    int adopted_track_id = -1;
                    {
                        float best_iou = 0.40f;
                        for (const auto& t : tracked_objects) {
                            if (t.bbox.class_id != 0) continue; // COCO person only
                            if (t.track_id < 0) continue;       // no stable id
                            const auto& a = t.bbox;
                            const float ix1 = std::max(a.x1, person->x1);
                            const float iy1 = std::max(a.y1, person->y1);
                            const float ix2 = std::min(a.x2, person->x2);
                            const float iy2 = std::min(a.y2, person->y2);
                            if (ix2 <= ix1 || iy2 <= iy1) continue;
                            const float inter = (ix2 - ix1) * (iy2 - iy1);
                            const float a_area = std::max(1.0f,
                                (a.x2 - a.x1) * (a.y2 - a.y1));
                            const float p_area = std::max(1.0f,
                                (person->x2 - person->x1) *
                                (person->y2 - person->y1));
                            const float iou = inter / (a_area + p_area - inter);
                            if (iou > best_iou) {
                                best_iou = iou;
                                adopted_track_id = t.track_id;
                            }
                        }
                    }

                    auto emit_violation = [&](int class_id, const char* label) {
                        inference::TrackedObject to;
                        to.bbox.x1 = person->x1; to.bbox.y1 = person->y1;
                        to.bbox.x2 = person->x2; to.bbox.y2 = person->y2;
                        to.confidence = person->score;
                        to.bbox.score = person->score;
                        to.bbox.class_id = class_id;
                        to.label = label;
                        to.track_id = adopted_track_id;
                        tracked_objects.push_back(to);
                        j_out["objects"].push_back({
                            {"label", to.label},
                            {"class_id", to.bbox.class_id},
                            {"confidence", to.confidence},
                            {"track_id", to.track_id},
                            {"box", {to.bbox.x1, to.bbox.y1, to.bbox.x2, to.bbox.y2}}
                        });
                    };
                    // *_class == -1 → operator opted out of that compliance
                    // check; do NOT emit a violation just because we
                    // collected zero detections (we were never looking
                    // for them). 2026-05-21 added gloves/mask/boots
                    // (synthetic class_ids 408/409/410, see ai_event_
                    // processor::processPPEViolation dispatch).
                    if (kPpeRuntime.helmet_class >= 0 && !has_helmet) emit_violation(406, "NoHelmet");
                    if (kPpeRuntime.vest_class   >= 0 && !has_vest)   emit_violation(407, "NoVest");
                    if (kPpeRuntime.gloves_class >= 0 && !has_gloves) emit_violation(408, "NoGloves");
                    if (kPpeRuntime.mask_class   >= 0 && !has_mask)   emit_violation(409, "NoMask");
                    if (kPpeRuntime.boots_class  >= 0 && !has_boots)  emit_violation(410, "NoBoots");
                }
                if (compliant_count + violating_count > 0) {
                    j_out["ppe_summary"] = {
                        {"compliant", compliant_count},
                        {"violating", violating_count}
                    };
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