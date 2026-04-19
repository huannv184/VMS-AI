#pragma once

#include <string>
#include <vector>
#include <ctime>

namespace vms {

/**
 * @brief Camera model
 */
struct Camera {
    int id = -1;
    std::string name;
    std::string rtsp_url;
    std::string sub_stream_url;
    std::string description;
    bool is_active = false;
    int zmq_port = 0;
    std::string ai_config; // JSON string: {"yolo":true, "face":false, ...}
    std::string advanced_config; // Raw JSON/XML from manufacturer API
    std::time_t created_at = 0;
    std::time_t updated_at = 0;
    bool motion_detection_enabled = false;
    double motion_sensitivity = 0.02;
    int site_id = -1;  // Association with Site model
};

/**
 * @brief ROI model
 */
struct ROI {
    int id = -1;
    int camera_id = -1;
    std::string name;
    std::string roi_type;  // "zone", "line", "fence"
    std::string points_json;  // JSON array of [[x1,y1], [x2,y2], ...]
    bool enabled = true;
    std::time_t created_at = 0;
};

/**
 * @brief Event model
 */
struct Event {
    std::string id;  // UUID
    int camera_id = -1;
    std::string event_type;  // "intrusion", "line_crossing", "loitering", etc.
    std::string description;
    std::string snapshot_path;
    std::string video_path; // Path to recording file
    std::string metadata_json;  // JSON with additional data
    int duration = 0; // Duration in seconds
    std::string severity = "low"; // Event severity: critical, high, medium, low
    std::time_t timestamp = 0;
};

/**
 * @brief Detection model (for real-time data)
 */
struct Detection {
    int object_id = -1;
    int track_id = -1;  // Track ID from object tracker
    int class_id = -1;
    std::string class_name;
    float confidence = 0.0f;
    float x1 = 0.0f, y1 = 0.0f;  // Bounding box
    float x2 = 0.0f, y2 = 0.0f;
    float x = 0.0f, y = 0.0f;      // Center point
    float width = 0.0f, height = 0.0f;  // Box dimensions
};

/**
 * @brief Camera metadata (from ai-server)
 */
struct CameraMetadata {
    int camera_id = -1;
    uint64_t frame_id = 0;
    std::time_t timestamp = 0;
    int fps = 0;
    int width = 0;
    int height = 0;
    std::vector<Detection> detections;
};

/**
 * @brief Person model for face recognition
 */
struct Person {
    int id = -1;
    std::string name;
    std::string description;
    std::string face_image_path;  // Path to the uploaded face image
    std::string embedding_json;  // JSON array of face embeddings
    std::time_t created_at = 0;
    std::time_t updated_at = 0;
};

/**
 * @brief Vehicle model (Registered vehicles)
 */
struct Vehicle {
    int id = -1;
    std::string plate_number;
    std::string owner_name;
    std::string brand;
    std::string color;
    std::string vehicle_type;
    std::string description;
    std::time_t created_at = 0;
    std::time_t updated_at = 0;
};

/**
 * @brief License Plate model (ANPR events)
 */
struct LicensePlate {
    int id = -1;
    std::string plate_number;
    std::string vehicle_type; // car, truck, bike, unknown
    std::string color;
    std::string image_path;
    int camera_id = -1;
    float confidence = 0.0f;
    std::time_t detected_at = 0;
};


/**
 * @brief Traffic Count model (vehicle counting per period)
 */
struct TrafficCount {
    int id = -1;
    int camera_id = -1;
    int roi_id = -1;            // -1 = entire frame
    std::string direction;      // "in", "out", "both"
    std::string vehicle_type;   // "car", "motorcycle", "truck", "all"
    int count = 0;
    std::time_t period_start = 0;
    std::time_t period_end = 0;
    std::time_t created_at = 0;
};

/**
 * @brief Traffic Summary (aggregated stats for dashboard)
 */
struct TrafficSummary {
    int camera_id = -1;
    int total_today = 0;
    int total_in = 0;
    int total_out = 0;
    int peak_hour = -1;    // 0-23
    int peak_count = 0;
};

/**
 * @brief Snapshot model (Generic for manual/auto snapshots)
 */
struct Snapshot {
    std::string id;
    int camera_id = -1;
    std::string trigger; // "manual", "auto", "event"
    std::string filepath;
    std::string timestamp_str;
    long long timestamp = 0;
    std::string metadata_json; 
};

/**
 * @brief Recording Segment model (continuous 24/7 recording)
 */
struct RecordingSegment {
    int id = -1;
    int camera_id = -1;
    std::string filename;
    std::time_t start_time = 0;
    std::time_t end_time = 0;
    size_t file_size = 0;
    std::string status; // "recording", "completed", "corrupted"
    std::time_t created_at = 0;
};

/**
 * @brief Site model (Hierarchical locations)
 */
struct Site {
    int id = -1;
    std::string name;
    std::string address;
    int parent_site_id = -1; // For hierarchy (Building -> Floor)
    std::string description;
    std::time_t created_at = 0;
};

/**
 * @brief User model
 */
struct User {
    int id = -1;
    std::string username;
    std::string password_hash;
    std::string email;
    int role_id = 0; // 1 = Admin, 2 = Operator, 3 = Viewer
    bool two_factor_enabled = false;
    std::string two_factor_secret;
    std::time_t created_at = 0;
};

/**
 * @brief User Permission model
 */
struct UserPermission {
    int user_id = -1;
    std::vector<int> allowed_camera_ids;
    std::vector<int> allowed_site_ids;
    bool can_view_live = true;
    bool can_view_playback = true;
    bool can_manage_settings = false;
    bool can_ptz = true;
};

} // namespace vms
