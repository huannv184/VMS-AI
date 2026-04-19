#include "AIWorker.h"

// Prevent Windows.h macro pollution
#ifndef NOMINMAX
#ifndef NOMINMAX
#define NOMINMAX
#endif
#endif

#include <iostream>
#include <chrono>
#include <iomanip>
#include <sstream>
#include <filesystem>
#include <functional> 
#include <opencv2/opencv.hpp>

// New libraries for ZMQ commands
#include "utils/api_utils.h"
#include <nlohmann/json.hpp>
using json = nlohmann::json;

// Include full definitions
#include "video/video_source.h"
#include "video/video_source_factory.h"
#include "inference/multi_model_infer.h"
#include "inference/tracking.h"
#include "ipc/video_publisher.h"
#include "common/frame.h"

// ============================================================================
// UTILITY FUNCTIONS
// ============================================================================

inline uint64_t get_timestamp_ms() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()
    ).count();
}

inline bool isValidFloat(float value) {
    return std::isfinite(value) && !std::isnan(value);
}

inline bool isValidBoundingBox(float x1, float y1, float x2, float y2, int width, int height) {
    if (!isValidFloat(x1) || !isValidFloat(y1) || !isValidFloat(x2) || !isValidFloat(y2)) {
        return false;
    }
    if (x1 < 0 || y1 < 0 || x2 > width || y2 > height) {
        return false;
    }
    if (x2 <= x1 || y2 <= y1) {
        return false;
    }
    return true;
}

// ============================================================================
// AIWORKER IMPLEMENTATION
// ============================================================================

AIWorker::AIWorker(const Config& config) 
    : m_config(config)
    , m_running(false)
    , m_state(State::STOPPED)
{
    std::cout << "[AIWorker] Created for Camera " << m_config.camera_id << std::endl;
}

AIWorker::~AIWorker() {
    stop();
    std::cout << "[AIWorker] Destroyed for Camera " << m_config.camera_id << std::endl;
}

// ============================================================================
// CONTROL METHODS
// ============================================================================

void AIWorker::start() {
    if (m_running.load()) {
        std::cout << "[AIWorker] Already running, ignoring start request" << std::endl;
        return;
    }
    
    m_running = true;
    m_state = State::STOPPED;
    m_startTime = std::chrono::steady_clock::now();
    
    m_thread = std::thread(&AIWorker::run, this);
    std::cout << "[AIWorker] Thread started for Camera " << m_config.camera_id << std::endl;
}

void AIWorker::stop() {
    if (!m_running.load()) {
        return;
    }
    
    std::cout << "[AIWorker] Stopping worker for Camera " << m_config.camera_id << "..." << std::endl;
    m_running = false;
    
    if (m_thread.joinable()) {
        m_thread.join();
    }
    
    m_state = State::STOPPED;
    std::cout << "[AIWorker] Worker stopped for Camera " << m_config.camera_id << std::endl;
}

// ============================================================================
// THREAD-SAFE GETTERS
// ============================================================================

std::string AIWorker::getLastError() const {
    std::lock_guard<std::mutex> lock(m_errorMutex);
    return m_lastError;
}

AIWorker::Statistics AIWorker::getStatistics() const {
    std::lock_guard<std::mutex> lock(m_statsMutex);
    
    Statistics stats = m_statistics;
    
    // Calculate uptime
    auto now = std::chrono::steady_clock::now();
    auto uptime = std::chrono::duration_cast<std::chrono::seconds>(now - m_startTime);
    stats.uptime_seconds = uptime.count();
    
    return stats;
}

// ============================================================================
// HELPER METHODS
// ============================================================================

void AIWorker::setError(const std::string& error) {
    {
        std::lock_guard<std::mutex> lock(m_errorMutex);
        m_lastError = error;
    }
    
    m_state = State::FAILED;
    std::cerr << "[AIWorker] ERROR (Camera " << m_config.camera_id << "): " << error << std::endl;
}

void AIWorker::updateStatistics(double fps, double infer_fps, int tracks) {
    std::lock_guard<std::mutex> lock(m_statsMutex);
    
    m_statistics.current_fps = fps;
    m_statistics.inference_fps = infer_fps;
    m_statistics.active_tracks = tracks;
}

std::string AIWorker::resolveModelPath(const std::string& path, const std::string& filename) {
    // Use explicit std::filesystem instead of namespace alias for MSVC compatibility
    
    std::cout << "[AIWorker] Resolving model path: " << path << std::endl;
    std::cout << "[AIWorker] Current working directory: " << std::filesystem::current_path() << std::endl;
    
    // If absolute path and exists, use it directly
    std::filesystem::path model_path(path);
    if (model_path.is_absolute()) {
        if (std::filesystem::exists(model_path)) {
            std::cout << "[AIWorker] ✓ Found model (absolute): " << model_path << std::endl;
            return std::filesystem::absolute(model_path).string();
        } else {
            std::cerr << "[AIWorker] ✗ Model not found (absolute): " << model_path << std::endl;
        }
    }
    
    // Try multiple search paths for relative paths
    std::vector<std::filesystem::path> search_paths = {
        // 1. Exact path as provided (relative to current dir)
        std::filesystem::current_path() / path,
        
        // 2. In models/ subdirectory (most common)
        std::filesystem::current_path() / "models" / filename,
        
        // 3. In src/ai/inference/models/ (development structure)
        std::filesystem::current_path() / "src/ai/inference/models" / filename,
        
        // 4. One level up (if running from build/)
        std::filesystem::current_path().parent_path() / path,
        std::filesystem::current_path().parent_path() / "models" / filename,
        std::filesystem::current_path().parent_path() / "src/ai/inference/models" / filename,
        
        // 5. Two levels up (if running from build/Debug or build/Release)
        std::filesystem::current_path().parent_path().parent_path() / path,
        std::filesystem::current_path().parent_path().parent_path() / "models" / filename,
        std::filesystem::current_path().parent_path().parent_path() / "src/ai/inference/models" / filename,
        
        // 6. Just the filename in current directory
        std::filesystem::current_path() / filename
    };
    
    std::cout << "[AIWorker] Searching for '" << filename << "' in " << search_paths.size() << " locations..." << std::endl;
    
    for (size_t i = 0; i < search_paths.size(); ++i) {
        const auto& p = search_paths[i];
        if (std::filesystem::exists(p)) {
            auto absolute = std::filesystem::absolute(p);
            std::cout << "[AIWorker] ✓ Found model at [" << (i+1) << "/" << search_paths.size() << "]: " << absolute << std::endl;
            return absolute.string();
        }
    }
    
    // Not found - print all searched paths
    std::cerr << "[AIWorker] ✗ Model '" << filename << "' NOT found in any location. Searched:" << std::endl;
    for (size_t i = 0; i < search_paths.size(); ++i) {
        std::cerr << "    [" << (i+1) << "] " << search_paths[i] << std::endl;
    }
    
    return ""; // Return empty to signal failure
}

void AIWorker::handleCommands() {
    if (!m_commandSocket) return;

    try {
        zmq::message_t request;
        auto res = m_commandSocket->recv(request, zmq::recv_flags::dontwait);
        
        if (!res.has_value()) return; // No message
        
        std::string req_str(static_cast<char*>(request.data()), request.size());
        json response;
        
        try {
            auto req_json = json::parse(req_str);
            std::string command = req_json.value("command", "");
            
            if (command == "EXTRACT_EMBEDDING") {
                std::string image_base64 = req_json.value("image", "");
                if (image_base64.empty()) {
                    response["status"] = "error";
                    response["message"] = "Empty image data";
                } else {
                    std::string decoded = vms::api::ApiUtils::base64_decode(image_base64);
                    // Convert string to vector<uchar>
                    std::vector<uchar> data(decoded.begin(), decoded.end());
                    cv::Mat img = cv::imdecode(data, cv::IMREAD_COLOR);
                    
                    if (img.empty()) {
                        response["status"] = "error";
                        response["message"] = "Failed to decode image";
                    } else {
                        // Use MultiModelInfer to get embedding
                        if (m_multiInfer) {
                            auto embedding = m_multiInfer->embedImage(img);
                            if (embedding.empty()) {
                                response["status"] = "error";
                                response["message"] = "No face detected or embedding failed";
                            } else {
                                response["status"] = "success";
                                response["embedding"] = embedding;
                            }
                        } else {
                            response["status"] = "error";
                            response["message"] = "Inference engine not ready";
                        }
                    }
                }
            } else {
                response["status"] = "error";
                response["message"] = "Unknown command";
            }
        } catch (const std::exception& e) {
            response["status"] = "error";
            response["message"] = std::string("JSON error: ") + e.what();
        }
        
        std::string resp_str = response.dump();
        m_commandSocket->send(zmq::message_t(resp_str.data(), resp_str.size()), zmq::send_flags::none);
        
    } catch (const std::exception& e) {
        std::cerr << "[AIWorker] Command handling error: " << e.what() << std::endl;
    }
}

// ============================================================================
// MAIN PROCESSING LOOP
// ============================================================================

void AIWorker::run() {
    std::cout << "\n";
    std::cout << "========================================" << std::endl;
    std::cout << "  AIWorker Initialization" << std::endl;
    std::cout << "  Camera ID: " << m_config.camera_id << std::endl;
    std::cout << "========================================" << std::endl;
    
    m_state = State::INITIALIZING;
    
    try {
        // ====================================================================
        // STEP 1: Shared Memory (SKIPPED/REMOVED)
        // ====================================================================

        // ====================================================================
        // STEP 2: Resolve Model Paths
        // ====================================================================
        std::cout << "\n[Step 2/7] Resolving AI Model Paths..." << std::endl;
        
        // Extract filename from path
        std::string yolo_filename = std::filesystem::path(m_config.yolo_path).filename().string();
        std::string resolved_yolo_path = resolveModelPath(m_config.yolo_path, yolo_filename);
        
        if (resolved_yolo_path.empty()) {
            std::ostringstream oss;
            oss << "YOLO model not found: " << m_config.yolo_path << "\n";
            oss << "Please ensure the model file exists in one of the search paths.\n";
            setError(oss.str());
            m_running = false;
            return;
        }
        
        // Resolve SCRFD path if enabled
        std::string resolved_scrfd_path;
        if (m_config.scrfd_enabled) {
            std::string scrfd_filename = std::filesystem::path(m_config.scrfd_path).filename().string();
            resolved_scrfd_path = resolveModelPath(m_config.scrfd_path, scrfd_filename);
            
            if (resolved_scrfd_path.empty()) {
                std::cout << "⚠ SCRFD model not found: " << m_config.scrfd_path << std::endl;
                std::cout << "⚠ Face detection will be DISABLED" << std::endl;
                m_config.scrfd_enabled = false;
            } else {
                std::cout << "✓ SCRFD model resolved" << std::endl;
            }
        }
        
        std::cout << "✓ Model paths resolved successfully" << std::endl;

        // ====================================================================
        // STEP 3: Initialize Inference Engine
        // ====================================================================
        std::cout << "\n[Step 3/7] Initializing AI Inference Engine..." << std::endl;
        
        inference::MultiModelInfer::Config infer_config;
        infer_config.yolo_model_path = resolved_yolo_path;
        infer_config.yolo_input_size = m_config.yolo_input_size;
        infer_config.yolo_conf_threshold = m_config.yolo_conf;
        infer_config.yolo_nms_threshold = m_config.yolo_nms;
        infer_config.enable_face_detection = m_config.scrfd_enabled;
        
        if (m_config.scrfd_enabled) {
            infer_config.scrfd_model_path = resolved_scrfd_path;
        }
        
        std::cout << "  YOLO Model: " << resolved_yolo_path << std::endl;
        
        m_multiInfer = std::make_unique<inference::MultiModelInfer>(infer_config);
        if (!m_multiInfer->init()) {
            setError("Failed to initialize inference engine.");
            m_running = false;
            return;
        }
        
        std::cout << "✓ Inference Engine initialized successfully" << std::endl;

        // ====================================================================
        // STEP 4: Initialize Object Tracking
        // ====================================================================
        std::cout << "\n[Step 4/7] Initializing Object Tracking..." << std::endl;
        
        if (m_config.tracking_enabled) {
            inference::TrackerConfig tracker_config;
            tracker_config.iou_threshold = m_config.tracking_iou_threshold;
            
            m_tracker = std::make_unique<inference::ObjectTracker>(tracker_config);
            
            if (m_config.scrfd_enabled) {
                m_faceTracker = std::make_unique<inference::FaceTracker>(0.3f, 30, 3);
            }
            std::cout << "✓ Tracking initialized successfully" << std::endl;
        } else {
            std::cout << "  Tracking: Disabled (as configured)" << std::endl;
        }

        // ====================================================================
        // STEP 5: Initialize Video Source
        // ====================================================================
        std::cout << "\n[Step 5/7] Connecting to Video Source..." << std::endl;
        std::cout << "  RTSP URL: " << m_config.rtsp_url << std::endl;
        
        video::SourceConfig src_config;
        src_config.type = video::SourceType::RTSP;
        src_config.uri = m_config.rtsp_url;
        
        m_videoSource = video::VideoSourceFactory::create(src_config);
        if (!m_videoSource || !m_videoSource->open()) {
            setError("Failed to connect to video source");
            m_running = false;
            return;
        }
        
        std::cout << "✓ Video source connected successfully" << std::endl;

        // ====================================================================
        // STEP 6: Initialize Video Publisher (Optional)
        // ====================================================================
        std::cout << "\n[Step 6/7] Initializing Video Publisher..." << std::endl;
        
        if (m_config.video_stream_enabled) {
            std::cout << "  ZMQ Endpoint: " << m_config.video_stream_endpoint << std::endl;
            
            m_videoPublisher = std::make_unique<ipc::VideoPublisher>(
                m_config.camera_id, 
                m_config.video_stream_endpoint, 
                30
            );
            
            std::cout << "✓ Video publisher ready" << std::endl;
        } else {
            std::cout << "  Video streaming: Disabled" << std::endl;
        }

        // ====================================================================
        // STEP 7: Initialize Command Socket (ZMQ REP)
        // ====================================================================
        std::cout << "\n[Step 7/7] Initializing Command Listener..." << std::endl;
        
        m_zmqContext = std::make_unique<zmq::context_t>(1);
        m_commandSocket = std::make_unique<zmq::socket_t>(*m_zmqContext, zmq::socket_type::rep);
        
        int port = 5560 + m_config.camera_id;
        std::string endpoint = "tcp://*:" + std::to_string(port);
        
        try {
            m_commandSocket->bind(endpoint);
            std::cout << "✓ Command listener bound to " << endpoint << std::endl;
        } catch (const zmq::error_t& e) {
            std::cerr << "✗ Failed to bind command socket: " << e.what() << std::endl;
        }

        std::cout << "\n========================================" << std::endl;
        std::cout << "  Initialization Complete!" << std::endl;
        std::cout << "========================================\n" << std::endl;

    } catch (const std::exception& e) {
        std::ostringstream oss;
        oss << "Exception during initialization: " << e.what();
        setError(oss.str());
        m_running = false;
        return;
    }

    // ========================================================================
    // MAIN PROCESSING LOOP
    // ========================================================================
    
    m_state = State::RUNNING;
    
    video::Frame frame;
    uint32_t frame_id = 0;
    
    // FPS tracking
    double display_fps = 0.0;
    double inference_fps = 0.0;
    auto fps_start = std::chrono::steady_clock::now();
    int fps_counter = 0;
    int inference_counter = 0;
    
    int consecutive_failures = 0;
    
    // Inference throttling
    const auto MIN_INFER_INTERVAL = std::chrono::microseconds(
        static_cast<int>(1000000.0 / m_config.target_fps)
    );
    auto last_infer_time = std::chrono::high_resolution_clock::now() - MIN_INFER_INTERVAL;
    
    inference::MultiModelResult last_result;
    std::vector<inference::TrackedObject> last_tracked_objects;

    std::cout << "[AIWorker] Processing loop started" << std::endl;
    
    while (m_running.load()) {
        try {
            // HANDLE COMMANDS
            handleCommands();

            // READ FRAME
            if (!m_videoSource->read(frame)) {
                consecutive_failures++;
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
                
                if (consecutive_failures > m_config.max_reconnect_attempts) {
                    std::cerr << "[AIWorker] Too many failures (" << consecutive_failures 
                              << "), attempting reconnection..." << std::endl;
                    
                    m_state = State::RECONNECTING;
                    
                    m_videoSource.reset();
                    std::this_thread::sleep_for(std::chrono::seconds(m_config.reconnect_delay_sec));
                    
                    video::SourceConfig src_config;
                    src_config.type = video::SourceType::RTSP;
                    src_config.uri = m_config.rtsp_url;
                    
                    m_videoSource = video::VideoSourceFactory::create(src_config);
                    if (m_videoSource && m_videoSource->open()) {
                        consecutive_failures = 0;
                        m_state = State::RUNNING;
                        std::cout << "[AIWorker] Reconnection successful" << std::endl;
                    } else {
                        std::cerr << "[AIWorker] Reconnection failed, will retry..." << std::endl;
                    }
                }
                continue;
            }
            
            consecutive_failures = 0;

            if (frame.image.empty()) {
                continue;
            }
            
            frame_id++;
            
            // RUN INFERENCE (THROTTLED)
            auto now = std::chrono::high_resolution_clock::now();
            auto elapsed = now - last_infer_time;

            if (elapsed >= MIN_INFER_INTERVAL) {
                // Inference
                last_result = m_multiInfer->infer(frame.image, frame_id);
                
                // Filtering
                std::vector<inference::BoundingBox> valid_objects;
                for (const auto& obj : last_result.objects) {
                    if (obj.class_id == 0 && obj.score >= m_config.yolo_conf) {
                        valid_objects.push_back(obj);
                    }
                }

                // Tracking
                if (m_tracker) {
                    last_tracked_objects = m_tracker->update(valid_objects);
                }
                
                if (m_faceTracker && !last_result.faces.empty()) {
                     m_faceTracker->update(last_result.faces);
                }

                // Callback
                {
                    std::lock_guard<std::mutex> lock(m_callbackMutex);
                    if (m_frameCallback) {
                       m_frameCallback(frame.image, last_result, last_tracked_objects);
                    }
                }

                // Publish
                if (m_videoPublisher && m_videoPublisher->is_connected()) {
                    m_videoPublisher->publish_frame(frame.image, frame_id, get_timestamp_ms());
                    if (frame_id % 25 == 0) {
                        m_videoPublisher->send_heartbeat(frame_id, static_cast<float>(display_fps));
                    }
                }

                last_infer_time = now;
                inference_counter++;
            }

            // UPDATE FPS
            fps_counter++;
            auto fps_now = std::chrono::steady_clock::now();
            if (std::chrono::duration_cast<std::chrono::milliseconds>(fps_now - fps_start).count() >= 1000) {
                display_fps = fps_counter;
                inference_fps = inference_counter;
                updateStatistics(display_fps, inference_fps, static_cast<int>(last_tracked_objects.size()));
                
                fps_counter = 0;
                inference_counter = 0;
                fps_start = fps_now;
                
                static int log = 0;
                if (++log >= 5) {
                    log = 0;
                    std::cout << "[AIWorker] Cam " << m_config.camera_id << " | FPS: " << display_fps << std::endl;
                }
            }

        } catch (const std::exception& e) {
            std::cerr << "[AIWorker] Loop error: " << e.what() << std::endl;
            std::this_thread::sleep_for(std::chrono::seconds(1));
        }
    }
    
    std::cout << "[AIWorker] Loop ended" << std::endl;
    m_state = State::STOPPED;
}