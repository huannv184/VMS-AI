#pragma once
#include <string>

namespace inference {

enum class ModelType {
    DUMMY,
    YOLO,
    FACE,
    REID,
    CUSTOM
};

struct InferConfig {
    ModelType model = ModelType::YOLO;
    
    // Model file paths
    std::string model_path;
    std::string engine_path;
    
    // Input dimensions
    int input_width = 640;
    int input_height = 640;
    
    // Detection thresholds
    float conf_threshold = 0.25f;
    float nms_threshold = 0.45f;

    // 2026-05-19 num_classes override. Default 80 = COCO YOLOv8/v11 (what
    // AdvancedInfer was hardcoded to before). PPE engine uses 9; custom
    // models can use any positive value. Parser reads output_dims = 4 +
    // num_classes (Ultralytics YOLOv8 export convention).
    int num_classes = 80;

    // Performance settings
    int batch_size = 1;
    bool use_fp16 = true;
    int device_id = 0;
    
    // Legacy compatibility
    InferConfig() = default;
    explicit InferConfig(ModelType m) : model(m) {}
};

} // namespace inference