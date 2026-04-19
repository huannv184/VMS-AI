#pragma once

#include <nlohmann/json.hpp>
#include "database/models.h"

namespace vms {

NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(Camera, id, name, rtsp_url, sub_stream_url, description, is_active, zmq_port, ai_config, advanced_config, created_at, updated_at)
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(ROI, id, camera_id, name, roi_type, points_json, enabled, created_at)
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(Event, id, camera_id, event_type, description, snapshot_path, metadata_json, timestamp)
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(Detection, object_id, track_id, class_id, class_name, confidence, x1, y1, x2, y2, x, y, width, height)
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(Person, id, name, description, face_image_path, embedding_json, created_at, updated_at)
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(LicensePlate, id, plate_number, vehicle_type, color, image_path, camera_id, confidence, detected_at)

} // namespace vms
