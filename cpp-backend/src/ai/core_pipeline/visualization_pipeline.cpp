#include "visualization_pipeline.h"
#include "tracking_pipeline.h"
#include <sstream>
#include <iomanip>

namespace pipeline {

VisualizationPipeline::VisualizationPipeline(const VisualizationConfig& config)
    : config_(config)
{
}

void VisualizationPipeline::drawTracks(
    cv::Mat& frame,
    const std::vector<Track>& tracks)
{
    for (const auto& track : tracks) {
        drawSingleTrack(frame, track);
    }
}

void VisualizationPipeline::drawSingleTrack(
    cv::Mat& frame,
    const Track& track)
{
    cv::Scalar box_color = track.has_face ? 
        config_.person_with_face_color : 
        config_.person_color;
    
    int thickness = track.stable_frames > config_.stable_track_threshold ? 
        config_.stable_thickness : 
        config_.normal_thickness;
    
    cv::rectangle(frame, track.bbox, box_color, thickness);
    
    std::ostringstream label_stream;
    
    if (config_.draw_class_names) {
        label_stream << track.class_name;
    }
    
    if (config_.draw_track_ids) {
        if (!label_stream.str().empty()) label_stream << " ";
        label_stream << "#" << track.track_id;
    }
    
    if (config_.draw_confidence) {
        if (!label_stream.str().empty()) label_stream << " ";
        label_stream << static_cast<int>(track.confidence * 100) << "%";
    }
    
    if (track.has_face) {
        label_stream << " [FACE]";
    }
    
    std::string label = label_stream.str();
    
    if (!label.empty()) {
        int baseline = 0;
        cv::Size text_size = cv::getTextSize(
            label,
            config_.font_face,
            config_.font_scale,
            config_.font_thickness,
            &baseline
        );
        
        cv::rectangle(
            frame,
            cv::Point(track.bbox.x, track.bbox.y - text_size.height - 5),
            cv::Point(track.bbox.x + text_size.width, track.bbox.y),
            box_color,
            -1
        );
        
        cv::putText(
            frame,
            label,
            cv::Point(track.bbox.x, track.bbox.y - 5),
            config_.font_face,
            config_.font_scale,
            cv::Scalar(0, 0, 0),
            config_.font_thickness
        );
    }
    
    if (config_.draw_velocity_vectors && 
        (track.velocity.x != 0 || track.velocity.y != 0)) {
        cv::Point center(
            track.bbox.x + track.bbox.width / 2,
            track.bbox.y + track.bbox.height / 2
        );
        cv::Point end(
            center.x + static_cast<int>(track.velocity.x * 2),
            center.y + static_cast<int>(track.velocity.y * 2)
        );
        cv::arrowedLine(frame, center, end, cv::Scalar(255, 255, 0), 2);
    }
}

void VisualizationPipeline::drawFaces(
    cv::Mat& frame,
    const std::vector<inference::BoundingBox>& faces,
    const std::vector<Track>& tracks)
{
    if (!config_.draw_face_boxes) {
        return;
    }
    
    for (const auto& face : faces) {
        bool inside_track = false;
        for (const auto& track : tracks) {
            if (isFaceInsideTrack(face, track)) {
                inside_track = true;
                break;
            }
        }
        
        if (inside_track) {
            cv::rectangle(
                frame,
                cv::Point(static_cast<int>(face.x1), static_cast<int>(face.y1)),
                cv::Point(static_cast<int>(face.x2), static_cast<int>(face.y2)),
                config_.face_color,
                config_.face_thickness
            );
        }
    }
}

bool VisualizationPipeline::isFaceInsideTrack(
    const inference::BoundingBox& face,
    const Track& track)
{
    cv::Rect face_rect(
        static_cast<int>(face.x1),
        static_cast<int>(face.y1),
        static_cast<int>(face.x2 - face.x1),
        static_cast<int>(face.y2 - face.y1)
    );
    
    cv::Rect intersection = face_rect & track.bbox;
    
    if (intersection.area() == 0) {
        return false;
    }
    
    float overlap_ratio = static_cast<float>(intersection.area()) / face_rect.area();
    return overlap_ratio >= config_.face_overlap_threshold;
}

void VisualizationPipeline::drawInfoOverlay(
    cv::Mat& frame,
    double fps,
    uint32_t frame_count,
    size_t object_count,
    size_t face_count,
    float object_confidence_threshold,
    float face_confidence_threshold,
    double latency_ms)
{
    if (!config_.draw_info_overlay) {
        return;
    }
    
    int y_offset = 30;
    int line_spacing = 30;
    cv::Point start_pos(10, y_offset);
    
    std::ostringstream fps_text;
    fps_text << "FPS: " << std::fixed << std::setprecision(1) << fps;
    drawTextWithBackground(frame, fps_text.str(), start_pos, 
                          cv::Scalar(0, 255, 0), cv::Scalar(0, 0, 0));
    
    start_pos.y += line_spacing;
    std::ostringstream frame_text;
    frame_text << "Frame: " << frame_count;
    drawTextWithBackground(frame, frame_text.str(), start_pos,
                          cv::Scalar(255, 255, 255), cv::Scalar(0, 0, 0));
    
    start_pos.y += line_spacing;
    std::ostringstream obj_text;
    obj_text << "Objects: " << object_count;
    drawTextWithBackground(frame, obj_text.str(), start_pos,
                          cv::Scalar(0, 255, 255), cv::Scalar(0, 0, 0));
    
    start_pos.y += line_spacing;
    std::ostringstream face_text;
    face_text << "Faces: " << face_count;
    drawTextWithBackground(frame, face_text.str(), start_pos,
                          cv::Scalar(255, 0, 0), cv::Scalar(0, 0, 0));
    
    if (latency_ms > 0) {
        start_pos.y += line_spacing;
        std::ostringstream latency_text;
        latency_text << "Latency: " << std::fixed << std::setprecision(1) 
                    << latency_ms << " ms";
        drawTextWithBackground(frame, latency_text.str(), start_pos,
                              cv::Scalar(255, 255, 0), cv::Scalar(0, 0, 0));
    }
    
    cv::Point threshold_pos(10, frame.rows - 60);
    std::ostringstream thresh_text;
    thresh_text << "Obj Conf: " << std::fixed << std::setprecision(2) 
                << object_confidence_threshold;
    drawTextWithBackground(frame, thresh_text.str(), threshold_pos,
                          cv::Scalar(200, 200, 200), cv::Scalar(0, 0, 0));
    
    threshold_pos.y += line_spacing;
    std::ostringstream face_thresh_text;
    face_thresh_text << "Face Conf: " << std::fixed << std::setprecision(2) 
                     << face_confidence_threshold;
    drawTextWithBackground(frame, face_thresh_text.str(), threshold_pos,
                          cv::Scalar(200, 200, 200), cv::Scalar(0, 0, 0));
}

void VisualizationPipeline::drawTextWithBackground(
    cv::Mat& frame,
    const std::string& text,
    cv::Point position,
    cv::Scalar text_color,
    cv::Scalar bg_color)
{
    int baseline = 0;
    cv::Size text_size = cv::getTextSize(
        text,
        config_.font_face,
        config_.font_scale,
        config_.font_thickness,
        &baseline
    );
    
    cv::rectangle(
        frame,
        cv::Point(position.x - 2, position.y - text_size.height - 2),
        cv::Point(position.x + text_size.width + 2, position.y + 2),
        bg_color,
        -1
    );
    
    cv::putText(
        frame,
        text,
        position,
        config_.font_face,
        config_.font_scale,
        text_color,
        config_.font_thickness
    );
}

cv::Mat VisualizationPipeline::visualize(
    const cv::Mat& frame,
    const std::vector<Track>& tracks,
    const std::vector<inference::BoundingBox>& faces,
    double fps,
    uint32_t frame_count,
    double latency_ms)
{
    cv::Mat display_frame = frame.clone();
    
    drawTracks(display_frame, tracks);
    drawFaces(display_frame, faces, tracks);
    
    drawInfoOverlay(
        display_frame,
        fps,
        frame_count,
        tracks.size(),
        faces.size(),
        0.0f,
        0.0f,
        latency_ms
    );
    
    return display_frame;
}

cv::Scalar getColorFromTrackId(int track_id) {
    static const cv::Scalar colors[] = {
        cv::Scalar(255, 0, 0),
        cv::Scalar(0, 255, 0),
        cv::Scalar(0, 0, 255),
        cv::Scalar(255, 255, 0),
        cv::Scalar(255, 0, 255),
        cv::Scalar(0, 255, 255),
        cv::Scalar(128, 0, 128),
        cv::Scalar(255, 128, 0),
        cv::Scalar(0, 128, 255),
        cv::Scalar(128, 255, 0),
    };
    
    return colors[track_id % 10];
}

void drawFPSCounter(cv::Mat& frame, double fps, cv::Point position) {
    std::ostringstream text;
    text << "FPS: " << std::fixed << std::setprecision(1) << fps;
    
    cv::putText(
        frame,
        text.str(),
        position,
        cv::FONT_HERSHEY_SIMPLEX,
        0.7,
        cv::Scalar(0, 255, 0),
        2
    );
}

void drawLatencyInfo(cv::Mat& frame, double latency_ms, cv::Point position) {
    std::ostringstream text;
    text << "Latency: " << std::fixed << std::setprecision(1) << latency_ms << " ms";
    
    cv::putText(
        frame,
        text.str(),
        position,
        cv::FONT_HERSHEY_SIMPLEX,
        0.6,
        cv::Scalar(255, 255, 0),
        2
    );
}

void drawDetectionCount(
    cv::Mat& frame,
    size_t object_count,
    size_t face_count,
    cv::Point position)
{
    std::ostringstream text;
    text << "Objects: " << object_count << " | Faces: " << face_count;
    
    cv::putText(
        frame,
        text.str(),
        position,
        cv::FONT_HERSHEY_SIMPLEX,
        0.6,
        cv::Scalar(255, 255, 255),
        2
    );
}

void createOverlay(
    cv::Mat& frame,
    cv::Rect region,
    cv::Scalar color,
    double alpha)
{
    cv::Mat overlay = frame.clone();
    cv::rectangle(overlay, region, color, -1);
    cv::addWeighted(overlay, alpha, frame, 1 - alpha, 0, frame);
}

} // namespace pipeline